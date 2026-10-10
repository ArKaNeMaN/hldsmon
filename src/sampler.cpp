// HldsMonitoring — sampling and aggregation (plan Phase 3, design §6).
//
// SV_Frame feeds frame timing here; once per second we emit one server record
// and (staggered) client records, plus per-interval packet/netcmd aggregates.
// All buffers are preallocated; no allocation or snprintf on this path (INV-2).

#include "plugin.h"
#include "sampler.h"

#include "delta.h"
#include "events.h"

namespace hldsmon {
namespace {

constexpr uint32_t kNetCmdPerTick = 16;

inline uint16_t rel_fill_pct(int reliable_length) noexcept {
    if (reliable_length <= 0) {
        return 0;
    }
    const uint32_t pct = static_cast<uint32_t>(reliable_length) * 100u /
                         static_cast<uint32_t>(MAX_MSGLEN);
    return pct > 0xFFFFu ? static_cast<uint16_t>(0xFFFFu)
                         : static_cast<uint16_t>(pct);
}

// Milliseconds the client has been silent: engine realtime minus the netchan's
// last_received, rounded and clamped to [0, UINT32_MAX]. 0 when the clock is
// unavailable or last_received is at/ahead of `now_real`.
inline uint32_t idle_ms_since(double now_real, double last_received) noexcept {
    if (now_real <= 0.0 || last_received >= now_real) {
        return 0;
    }
    const double ms = (now_real - last_received) * 1000.0;
    if (ms >= 4294967295.0) {
        return 0xFFFFFFFFu;
    }
    return static_cast<uint32_t>(ms + 0.5);
}

inline bool parse_uint(const char* s, uint32_t* out) noexcept {
    if (s == nullptr) {
        return false;
    }
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    if (*s < '0' || *s > '9') {
        return false;
    }
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + static_cast<uint32_t>(*s - '0');
        ++s;
    }
    *out = v;
    return true;
}

inline uint8_t state_bitmask(const client_t* ct) noexcept {
    uint8_t m = 0;
    if (ct->active)          m |= 0x01;
    if (ct->spawned)         m |= 0x02;
    if (ct->fully_connected) m |= 0x04;
    if (ct->connected)       m |= 0x08;
    return m;
}

// Parses userinfo keys into preallocated storage (no heap, no snprintf).
void parse_userinfo(client_t* ct, ClientRuntime& cr, ClientSample& cs) noexcept {
    uint32_t v = 0;
    char* info = ct->userinfo;
    if (parse_uint(g_engfuncs.pfnInfoKeyValue(info, "rate"), &v)) {
        cr.user_rate = v;
    }
    if (parse_uint(g_engfuncs.pfnInfoKeyValue(info, "cl_updaterate"), &v)) {
        cs.updaterate = static_cast<uint16_t>(v > 0xFFFFu ? 0xFFFFu : v);
    }
    if (parse_uint(g_engfuncs.pfnInfoKeyValue(info, "cl_cmdrate"), &v)) {
        cs.cmdrate = static_cast<uint16_t>(v > 0xFFFFu ? 0xFFFFu : v);
    }
    copy_str(cr.name, sizeof(cr.name), g_engfuncs.pfnInfoKeyValue(info, "name"));
}

bool build_client_sample(int id, double now_epoch, ClientSample& cs) noexcept {
    PluginContext& c = ctx();
    IGameClient* gc = c.svs->GetClient(id);
    if (gc == nullptr || !gc->IsActive()) {
        return false;
    }
    client_t* ct = client_by_id(c.svs, id);
    if (ct == nullptr) {
        return false;
    }

    std::memset(&cs, 0, sizeof(cs));
    cs.t = now_epoch;
    cs.id = static_cast<uint8_t>(id);
    cs.gen = c.clients[id].gen;
    cs.ping = static_cast<uint16_t>(ct->latency);
    cs.loss = static_cast<uint32_t>(ct->packet_loss);
    cs.choke = static_cast<uint32_t>(ct->chokecount);

    ClientRuntime& cr = c.clients[id];
    if (cr.has_prev) {
        cs.dloss = CounterDelta::compute_delta(cr.prev_loss, cs.loss);
        cs.dchoke = CounterDelta::compute_delta(cr.prev_choke, cs.choke);
    }
    cr.prev_loss = cs.loss;
    cr.prev_choke = cs.choke;
    cr.has_prev = true;

    cs.delta_seq = ct->delta_sequence;
    cs.in_seq = static_cast<uint32_t>(ct->netchan.incoming_sequence);
    cs.in_ack = static_cast<uint32_t>(ct->netchan.incoming_acknowledged);
    cs.out_seq = static_cast<uint32_t>(ct->netchan.outgoing_sequence);
    cs.rel_seq = static_cast<uint32_t>(ct->netchan.reliable_sequence);
    cs.rel_fill = rel_fill_pct(ct->netchan.reliable_length);
    cs.rate = static_cast<uint32_t>(ct->netchan.rate);
    cs.cleartime = static_cast<float>(ct->netchan.cleartime);
    cs.state = state_bitmask(ct);

    uint8_t frag = 0;
    uint32_t ready = 0;
    for (int s = 0; s < MAX_STREAMS; ++s) {
        const int n = ct->netchan.fragbufcount[s];
        if (n > 0 && frag <= 0xFF - n) {
            frag = static_cast<uint8_t>(frag + n);
        }
        if (ct->netchan.incomingready[s]) {
            ready |= (1u << s);
        }
    }
    cs.fragbuf_total = frag;
    cs.incomingready_mask = ready;

    cs.flow_in_kbps = ct->netchan.flow[FLOW_INCOMING].kbytespersec;
    cs.flow_out_kbps = ct->netchan.flow[FLOW_OUTGOING].kbytespersec;
    cs.flow_in_avg = ct->netchan.flow[FLOW_INCOMING].avgkbytespersec;
    cs.flow_out_avg = ct->netchan.flow[FLOW_OUTGOING].avgkbytespersec;
    cs.msg_flags = ct->netchan.message.flags;
    if (c.funcs != nullptr) {
        cs.idle_ms = idle_ms_since(c.funcs->GetRealTime(), ct->netchan.last_received);
    }
    gate_idle_flow(cs, kIdleZeroFlowMs);

    parse_userinfo(ct, cr, cs);
    return true;
}

void emit_packet_interval(double now_epoch) noexcept {
    PluginContext& c = ctx();
    if (c.pkt_count > 0) {
        PacketStats ps;
        std::memset(&ps, 0, sizeof(ps));
        ps.t = now_epoch;
        ps.pps = c.pkt_count;
        ps.bytes = c.pkt_bytes;
        ps.sizes = c.pkt_sizes;
        ps.unique_ips = c.ip_unique;

        uint32_t best = 0;
        uint32_t best_ip = 0;
        for (uint32_t i = 0; i < c.ip_table_size; ++i) {
            if (c.ip_table[i].count > best) {
                best = c.ip_table[i].count;
                best_ip = c.ip_table[i].ip;
            }
        }
        if (best > 0 && best_ip != 0) {
            netadr_t a;
            std::memset(&a, 0, sizeof(a));
            a.type = NA_IP;
            a.ip[0] = static_cast<unsigned char>((best_ip >> 24) & 0xFFu);
            a.ip[1] = static_cast<unsigned char>((best_ip >> 16) & 0xFFu);
            a.ip[2] = static_cast<unsigned char>((best_ip >> 8) & 0xFFu);
            a.ip[3] = static_cast<unsigned char>(best_ip & 0xFFu);
            format_ip(a, ps.top_ip, sizeof(ps.top_ip));
        }
        events::write_packet(ps);
        ++c.packet_records;
    }

    if (c.ip_table != nullptr && c.ip_table_size > 0) {
        std::memset(c.ip_table, 0, static_cast<size_t>(c.ip_table_size) * sizeof(IpEntry));
    }
    c.ip_unique = 0;
    c.ip_overflow = 0;
    c.pkt_count = 0;
    c.pkt_bytes = 0;
    std::memset(&c.pkt_sizes, 0, sizeof(c.pkt_sizes));
}

void emit_netcmd_interval(double now_epoch) noexcept {
    PluginContext& c = ctx();
    uint32_t emitted = 0;
    for (int i = 0; i < c.maxclients; ++i) {
        for (int cmd = 0; cmd < 256; ++cmd) {
            const uint32_t cnt = c.netcmd_counts[i][cmd];
            if (cnt == 0) {
                continue;
            }
            c.netcmd_counts[i][cmd] = 0;
            if (emitted < kNetCmdPerTick) {
                NetCmdRecord r;
                std::memset(&r, 0, sizeof(r));
                r.t = now_epoch;
                r.id = static_cast<uint8_t>(i);
                r.cmd = static_cast<uint8_t>(cmd);
                r.count = cnt;
                events::write_netcmd(r);
                ++emitted;
            }
        }
    }
}

void modnet_emit_interval(double now_epoch, double now_real) noexcept {
    PluginContext& c = ctx();
    if (!c.modnet.enabled) {
        return;
    }
    static ModnetEmitItem items[HLDSMON_MODNET_MAX_OBJECTS];
    const size_t n = modnet_got_collect_emit(items, HLDSMON_MODNET_MAX_OBJECTS, now_real);
    for (size_t k = 0; k < n; ++k) {
        ModnetRecord r;
        std::memset(&r, 0, sizeof(r));
        r.t = now_epoch;
        copy_str(r.module, sizeof(r.module), items[k].module);
        copy_str(r.so, sizeof(r.so), items[k].so);
        modnet_diff(items[k].prev, items[k].cur, items[k].dt, r);
        if (!modnet_has_activity(r)) {
            continue;
        }
        events::write_modnet(r);
    }
}

void do_server_tick(double now_real, double now_epoch) noexcept {
    PluginContext& c = ctx();
    ServerSample s;
    std::memset(&s, 0, sizeof(s));
    s.t = now_epoch;
    s.frame_ms_min = c.frame_min;
    s.frame_ms_avg = c.frame_count != 0
                         ? static_cast<float>(c.frame_sum / static_cast<double>(c.frame_count))
                         : 0.0f;
    s.frame_ms_max = c.frame_max;
    s.hitch_gt50 = c.hitch_gt50;
    s.hitch_gt100 = c.hitch_gt100;

    PlatformRusage ru;
    platform_read_rusage(&ru);
    if (c.has_rusage) {
        const double wall = now_real - c.prev_rusage_t;
        if (wall > 0.0) {
            s.cpu_pct = static_cast<float>(
                (ru.cpu_seconds - c.prev_rusage.cpu_seconds) / wall * 100.0);
        }
    }
    s.rss_kb = ru.maxrss_kb;
    s.minflt = ru.minflt;
    s.majflt = ru.majflt;
    c.prev_rusage = ru;
    c.prev_rusage_t = now_real;
    c.has_rusage = true;

    uint16_t players = 0;
    uint16_t real = 0;
    uint16_t proxy = 0;
    uint16_t fake = 0;
    float in_k = 0.0f;
    float out_k = 0.0f;
    for (int i = 0; i < c.maxclients; ++i) {
        client_t* ct = client_by_id(c.svs, i);
        if (ct == nullptr || !ct->active) {
            continue;
        }
        ++players;
        if (ct->fakeclient) {
            ++fake;
        } else if (ct->proxy) {
            ++proxy;
        } else {
            ++real;
        }
        in_k += ct->netchan.flow[FLOW_INCOMING].avgkbytespersec;
        out_k += ct->netchan.flow[FLOW_OUTGOING].avgkbytespersec;
    }
    s.players = players;
    s.real = real;
    s.proxy = proxy;
    s.fake = fake;
    s.in_kbps = in_k;
    s.out_kbps = out_k;
    s.edicts = static_cast<uint32_t>(g_engfuncs.pfnNumberOfEntities());
    copy_str(s.map, sizeof(s.map), current_map());
    s.uptime_s = gpGlobals != nullptr ? static_cast<uint32_t>(gpGlobals->time) : 0;
    s.fps = static_cast<float>(c.frame_count);
    s.maxclients = static_cast<uint8_t>(c.maxclients > 0 ? c.maxclients : 0);

    c.server_ring.push(s);
    events::write_server(s);

    emit_packet_interval(now_epoch);
    emit_netcmd_interval(now_epoch);
    modnet_emit_interval(now_epoch, now_real);

    c.frame_count = 0;
    c.frame_min = 0.0f;
    c.frame_max = 0.0f;
    c.frame_sum = 0.0;
    c.hitch_gt50 = 0;
    c.hitch_gt100 = 0;

    if (c.log != nullptr) {
        c.log->set_retention_days(static_cast<int>(cvar_f(c.cvars.retention_days, 3.0f)));
        c.log->maybe_rotate(now_epoch);
        const double flush_sec = cvar_f(c.cvars.flush_sec, 1.0f);
        if (flush_sec > 0.0 && now_real - c.last_flush_t >= flush_sec) {
            c.log->flush();
            c.last_flush_t = now_real;
        }
    }
}

void sample_clients(double now_epoch) noexcept {
    PluginContext& c = ctx();
    if (c.maxclients <= 0) {
        return;
    }
    const int batch = kClientsPerTick < c.maxclients ? kClientsPerTick : c.maxclients;
    for (int k = 0; k < batch; ++k) {
        const int id = static_cast<int>(c.client_cursor);
        c.client_cursor = (c.client_cursor + 1u) % static_cast<uint32_t>(c.maxclients);
        ClientSample cs;
        if (build_client_sample(id, now_epoch, cs)) {
            c.clients[id].ring.push(cs);
        }
    }
}

void write_clients(double now_epoch) noexcept {
    PluginContext& c = ctx();
    if (c.maxclients <= 0) {
        return;
    }
    const int batch = kClientsPerTick < c.maxclients ? kClientsPerTick : c.maxclients;
    for (int k = 0; k < batch; ++k) {
        const int id = static_cast<int>(c.write_cursor);
        c.write_cursor = (c.write_cursor + 1u) % static_cast<uint32_t>(c.maxclients);
        ClientRuntime& cr = c.clients[id];
        if (cr.ring.size() == 0) {
            continue;
        }
        ClientSample cs = cr.ring.at(cr.ring.size() - 1);
        cs.t = now_epoch;
        IGameClient* gc = c.svs->GetClient(id);
        fill_identity(cs, gc, id, now_epoch);
        events::write_client(cs);

        if (gc != nullptr) {
            const float vt = static_cast<float>(gc->GetLastVoiceTime());
            if (vt > 0.0f && vt != cr.last_voice_t) {
                cr.last_voice_t = vt;
                VoiceRecord vr;
                std::memset(&vr, 0, sizeof(vr));
                vr.t = now_epoch;
                vr.id = static_cast<uint8_t>(id);
                vr.last_voice_time = vt;
                events::write_voice(vr);
            }
        }
    }
}

} // namespace

const char* current_map() noexcept {
    if (gpGlobals != nullptr) {
        const char* m = g_engfuncs.pfnSzFromIndex(static_cast<int>(gpGlobals->mapname));
        if (m != nullptr && m[0] != '\0') {
            return m;
        }
    }
    if (ctx().data != nullptr) {
        const char* m = ctx().data->GetModelName();
        if (m != nullptr && m[0] != '\0') {
            return m;
        }
    }
    return "";
}

void sampler_on_frame(double frame_ms, double now_real) noexcept {
    PluginContext& c = ctx();
    if (!c.attached) {
        return;
    }

    // modnet master-switch sync (MN-2): live enable/disable on cvar change,
    // independent of hldsmon_enable so disabling never leaves GOT slots patched.
    const bool modnet_want = cvar_f(c.cvars.modnet, 0.0f) > 0.5f;
    if (modnet_want != c.modnet.enabled) {
        if (modnet_want) {
            modnet_enable();
        } else {
            modnet_disable();
        }
        c.modnet = modnet_got_status();
        c.modnet.next_rescan_t = 0.0;
    }

    if (!plugin_enabled()) {
        return;
    }

    // modnet rescan cadence (Q1) + immediate request from SV_Spawn_f.
    if (c.modnet.enabled) {
        const double rescan_sec = cvar_f(c.cvars.modnet_rescan_sec, 5.0f);
        const double period = rescan_sec > 1.0 ? rescan_sec : 1.0;
        if (c.modnet.next_rescan_t <= 0.0) {
            c.modnet.next_rescan_t = now_real + period;
        }
        if (modnet_rescan_requested() || now_real >= c.modnet.next_rescan_t) {
            modnet_rescan();
            c.modnet = modnet_got_status();
            c.modnet.next_rescan_t = now_real + period;
        }
    }

    if (frame_ms > 0.0) {
        if (c.frame_count == 0) {
            c.frame_min = static_cast<float>(frame_ms);
            c.frame_max = static_cast<float>(frame_ms);
        } else {
            if (frame_ms < static_cast<double>(c.frame_min)) {
                c.frame_min = static_cast<float>(frame_ms);
            }
            if (frame_ms > static_cast<double>(c.frame_max)) {
                c.frame_max = static_cast<float>(frame_ms);
            }
        }
        c.frame_sum += frame_ms;
        ++c.frame_count;
        if (frame_ms > 50.0) {
            ++c.hitch_gt50;
        }
        if (frame_ms > kHitchMs) {
            ++c.hitch_gt100;
            HitchRecord hr;
            std::memset(&hr, 0, sizeof(hr));
            hr.t = platform_wall_seconds();
            hr.duration_ms = static_cast<uint32_t>(frame_ms);
            events::write_hitch(hr);
            ++c.hitch_count;
        }
    }

    const double now_epoch = platform_wall_seconds();

    if (now_real >= c.next_server_tick_t) {
        do_server_tick(now_real, now_epoch);
        c.next_server_tick_t = now_real + 1.0;
    }

    const int batches = (c.maxclients + kClientsPerTick - 1) / kClientsPerTick;
    const int div = batches > 0 ? batches : 1;

    const double client_hz = cvar_f(c.cvars.client_hz, 1.0f);
    if (client_hz > 0.0 && now_real >= c.next_sample_t) {
        sample_clients(now_epoch);
        c.next_sample_t = now_real + 1.0 / client_hz / static_cast<double>(div);
    }

    const double disk_hz = cvar_f(c.cvars.disk_hz, 1.0f);
    if (disk_hz > 0.0 && now_real >= c.next_write_t) {
        write_clients(now_epoch);
        c.next_write_t = now_real + 1.0 / disk_hz / static_cast<double>(div);
    }
}

} // namespace hldsmon
