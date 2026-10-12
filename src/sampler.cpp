// HldsMonitoring — sampling and aggregation (plan Phase 3, design §6).
//
// SV_Frame feeds frame timing here; once per second we emit one server record
// and (staggered) client records, plus per-interval packet/netcmd aggregates.
// All buffers are preallocated; no allocation or snprintf on this path (INV-2).

#include "plugin.h"
#include "sampler.h"

#include "delta.h"
#include "events.h"

#include <utility>

namespace hldsmon {
namespace {

constexpr uint32_t kNetCmdPerTick = 16;
constexpr uint16_t kMapReadyTicks = 30;  // readiness window, 1 Hz server ticks

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
    copy_str(cr.name, g_engfuncs.pfnInfoKeyValue(info, "name"));
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
    cs.ping = ping_ms_from_latency(ct->latency);
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

    // Jitter accumulated since this client's previous sample (design §24.3
    // п.1); count==0 => all-zero "no data".
    jitter_finalize(cr.jit_min, cr.jit_sum, cr.jit_max, cr.jit_count,
                    cs.jit_min_ms, cs.jit_avg_ms, cs.jit_max_ms);

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

        // One linear pass selects the top-3 (EN-4); `top_ip` keeps its old
        // meaning (the count leader) for schema backward compatibility.
        IpCount top[HLDSMON_TOP_IPS];
        std::memset(top, 0, sizeof(top));
        const size_t ntop = select_top_ips(c.ip_table, c.ip_table_size, top);
        for (size_t k = 0; k < ntop; ++k) {
            ps.top_ips[k] = top[k];
        }
        if (ntop > 0) {
            copy_str(ps.top_ip, top[0].ip);
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

// Delayed `mapchange` (design §25.3 п.2, CT-2): SV_Spawn_f records only the
// spawn marker, so the record is emitted here once the world is available
// (non-empty current_map(), Q1) or the readiness window expires; not ready
// within the window => load_ms present bit clear (JSON null).
void emit_pending_mapchange(double now_real, double now_epoch) noexcept {
    PluginContext& c = ctx();
    if (c.map_ctx.pending == 0) {
        return;
    }
    ++c.map_ctx.ticks;
    const bool ready = current_map()[0] != '\0';
    if (!ready && c.map_ctx.ticks < kMapReadyTicks) {
        return;
    }
    MapChangeRecord rec;
    std::memset(&rec, 0, sizeof(rec));
    rec.t = now_epoch;
    copy_str(rec.map, c.map_ctx.pending_map);
    if (ready && c.map_ctx.t_spawn > 0.0) {
        uint32_t load_ms = 0;
        if (timing_delta_ms(now_real - c.map_ctx.t_spawn, load_ms)) {
            rec.load_ms = load_ms;
            rec.load_present = 1;
        }
    }
    events::write_mapchange(rec);
    c.map_ctx.pending = 0;
    c.map_ctx.ticks = 0;
}

void modnet_emit_interval(double now_epoch, double now_real) noexcept {
    PluginContext& c = ctx();
    if (!c.modnet.enabled) {
        return;
    }
    static ModnetEmitItem items[HLDSMON_MODNET_MAX_OBJECTS];
    const size_t n = modnet_got_collect_emit(items, now_real);
    for (size_t k = 0; k < n; ++k) {
        ModnetRecord r;
        std::memset(&r, 0, sizeof(r));
        r.t = now_epoch;
        copy_str(r.module, items[k].module);
        copy_str(r.so, items[k].so);
        modnet_diff(items[k].prev, items[k].cur, items[k].dt, r);
        if (!modnet_has_activity(r)) {
            continue;
        }
        events::write_modnet(r);
    }
}

// statvfs must target the directory the log actually lives in (M5), not "/";
// resolution mirrors LogWriter paths (game dir + relative cvar) without snprintf.
void resolve_host_dir(char* out, size_t cap) noexcept {
    PluginContext& c = ctx();
    const char* dir = "addons/hldsmon/logs";
    if (c.cvars.dir != nullptr && c.cvars.dir->string != nullptr &&
        c.cvars.dir->string[0] != '\0') {
        dir = c.cvars.dir->string;
    }
    if (dir[0] == '/' || cap == 0) {
        copy_str(std::span<char>(out, cap), dir);
        return;
    }
    char game_dir[512];
    game_dir[0] = '\0';
    if (g_engfuncs.pfnGetGameDir != nullptr) {
        g_engfuncs.pfnGetGameDir(game_dir);
    }
    if (game_dir[0] == '\0') {
        copy_str(std::span<char>(out, cap), dir);
        return;
    }
    size_t pos = 0;
    while (game_dir[pos] != '\0' && pos + 1 < cap) {
        out[pos] = game_dir[pos];
        ++pos;
    }
    if (pos + 1 < cap) {
        out[pos] = '/';
        ++pos;
    }
    copy_str(std::span<char>(out + pos, cap - pos), dir);
}

// 1 Hz host/kernel telemetry (design §23.3): gather -> Δ -> emit `host`.
// OFF => not a single read (QAS-H5). Unavailable sources leave their fields
// null; the env caveat mask is computed once at bootstrap.
void host_emit_interval(double now_epoch) noexcept {
    PluginContext& c = ctx();
    if (cvar_f(c.cvars.host, 0.0f) <= 0.5f) {
        return;
    }

    HostSnapshot cur;
    std::memset(&cur, 0, sizeof(cur));
    char buf[kHostReadCap];
    size_t len = 0;
    bool truncated = false;
    bool any_truncated = false;

    if (read_file("/proc/net/snmp", buf, &len, &truncated)) {
        parse_snmp_udp(buf, cur);
    }
    any_truncated = any_truncated || truncated;

    if (read_file("/proc/net/dev", buf, &len, &truncated)) {
        parse_net_dev(buf, cur);
    }
    any_truncated = any_truncated || truncated;

    if (read_file("/proc/stat", buf, &len, &truncated)) {
        parse_stat(buf, cur);
    }
    any_truncated = any_truncated || truncated;

    if (read_file("/proc/self/io", buf, &len, &truncated)) {
        parse_self_io(buf, cur);
    }
    any_truncated = any_truncated || truncated;

    char dir[1024];
    resolve_host_dir(dir, sizeof(dir));
    uint64_t avail_mb = 0;
    if (statvfs_bavail(dir, &avail_mb)) {
        cur.disk_bavail_mb = avail_mb;
        cur.present |= std::to_underlying(HostPresent::kHostDiskBavailMb);
    }

    HostRecord r;
    build_host_record(c.host_prev, c.host_has_prev, cur, c.host_caveat_mask,
                      any_truncated, now_epoch, r);
    c.host_prev = cur;
    c.host_has_prev = true;
    events::write_host(r);
}

// Pull-based degradation visibility (design §26.3 п.4, EN-2/EN-7): LogWriter
// stays an engine-free sink; the 1 Hz tick reads its counters and emits
// `degraded`/`retention` while writing is enabled. A hard write failure or the
// size cap cannot be reported into the JSONL it just disabled, so it goes to
// the console only (no `write_error` type); `hldsmon_status` shows the flags.
void emit_log_health(double now_epoch) noexcept {
    PluginContext& c = ctx();
    if (c.log == nullptr) {
        return;
    }
    const bool enabled = c.log->enabled();

    const uint64_t ec = c.log->error_count();
    if (ec > c.prev_error_count) {
        const uint64_t delta = ec - c.prev_error_count;
        if (enabled) {
            DegradedRecord r;
            std::memset(&r, 0, sizeof(r));
            r.t = now_epoch;
            r.error_count = delta;
            events::write_degraded(r);
        } else if (g_engfuncs.pfnServerPrint != nullptr) {
            g_engfuncs.pfnServerPrint(
                "hldsmon: log write errors; JSONL writing disabled (see hldsmon_status)\n");
        }
    }
    c.prev_error_count = ec;

    const uint32_t pc = c.log->last_prune_count();
    if (enabled && pc > c.prev_prune_count) {
        RetentionRecord r;
        std::memset(&r, 0, sizeof(r));
        r.t = now_epoch;
        r.removed = pc - c.prev_prune_count;
        events::write_retention(r);
    }
    c.prev_prune_count = pc;

    if (c.prev_log_enabled && !enabled && c.log->size_capped() &&
        g_engfuncs.pfnServerPrint != nullptr) {
        g_engfuncs.pfnServerPrint(
            "hldsmon: log size cap reached; writing disabled until next rotation\n");
    }
    c.prev_log_enabled = enabled;
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
    const int32_t edicts_now = static_cast<int32_t>(g_engfuncs.pfnNumberOfEntities());
    s.edicts = static_cast<uint32_t>(edicts_now);
    // Signed churn over the interval (EN-5); no baseline yet (startup, first
    // tick after mapchange) leaves the present bit clear => JSON null.
    if (c.edicts_has_prev) {
        int64_t churn = static_cast<int64_t>(edicts_now) - static_cast<int64_t>(c.prev_edicts);
        if (churn > 2147483647LL) churn = 2147483647LL;
        if (churn < -2147483648LL) churn = -2147483648LL;
        s.edicts_churn = static_cast<int32_t>(churn);
        s.edicts_churn_present = 1;
    }
    c.prev_edicts = edicts_now;
    c.edicts_has_prev = true;
    copy_str(s.map, current_map());
    s.uptime_s = gpGlobals != nullptr ? static_cast<uint32_t>(gpGlobals->time) : 0;
    s.fps = static_cast<float>(c.frame_count);
    s.maxclients = static_cast<uint8_t>(c.maxclients > 0 ? c.maxclients : 0);

    c.server_ring.push(s);
    events::write_server(s);

    // Tracked config: first enabled tick emits the baseline, later ticks emit
    // only on change (design §22.3 п.1, CFG-4; caller gates on plugin_enabled).
    ConfigSnapshot cfg;
    build_config_snapshot(cfg);
    if (!c.last_config_valid || config_changed(c.last_config, cfg)) {
        ConfigRecord cr;
        std::memset(&cr, 0, sizeof(cr));
        cr.t = now_epoch;
        if (c.api != nullptr) {
            cr.rehlds_major = c.api->GetMajorVersion();
            cr.rehlds_minor = c.api->GetMinorVersion();
        }
        copy_str(cr.map, current_map());
        cr.maxclients = static_cast<uint8_t>(c.maxclients > 0 ? c.maxclients : 0);
        cr.config = cfg;
        events::write_config(cr);
        c.last_config = cfg;
        c.last_config_valid = true;
    }

    emit_packet_interval(now_epoch);
    emit_netcmd_interval(now_epoch);
    emit_pending_mapchange(now_real, now_epoch);
    modnet_emit_interval(now_epoch, now_real);
    host_emit_interval(now_epoch);

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
    emit_log_health(now_epoch);
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
            // Reset right after this client's sample: sampling is staggered
            // (M3) and this function only runs under client_hz>0 (CQ-4), so the
            // accumulators are always drained by the matching sample.
            ClientRuntime& cr = c.clients[id];
            jitter_reset(cr.jit_min, cr.jit_sum, cr.jit_max, cr.jit_count);
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

void build_config_snapshot(ConfigSnapshot& out) noexcept {
    PluginContext& c = ctx();
    std::memset(&out, 0, sizeof(out));
    const CvarRefs& cv = c.cvars;
    if (cv.sys_ticrate != nullptr) {
        out.present |= std::to_underlying(ConfigPresent::kCfgSysTicrate);
        out.sys_ticrate = cv.sys_ticrate->value;
    }
    if (cv.host_framerate != nullptr) {
        out.present |= std::to_underlying(ConfigPresent::kCfgHostFramerate);
        out.host_framerate = cv.host_framerate->value;
    }
    if (cv.sv_timeout != nullptr) {
        out.present |= std::to_underlying(ConfigPresent::kCfgSvTimeout);
        out.sv_timeout = cv.sv_timeout->value;
    }
    if (cv.sv_maxrate != nullptr) {
        out.present |= std::to_underlying(ConfigPresent::kCfgSvMaxrate);
        out.sv_maxrate = cv.sv_maxrate->value;
    }
    if (cv.sv_minrate != nullptr) {
        out.present |= std::to_underlying(ConfigPresent::kCfgSvMinrate);
        out.sv_minrate = cv.sv_minrate->value;
    }
    if (cv.sv_maxupdaterate != nullptr) {
        out.present |= std::to_underlying(ConfigPresent::kCfgSvMaxupdaterate);
        out.sv_maxupdaterate = cv.sv_maxupdaterate->value;
    }
    if (cv.sv_minupdaterate != nullptr) {
        out.present |= std::to_underlying(ConfigPresent::kCfgSvMinupdaterate);
        out.sv_minupdaterate = cv.sv_minupdaterate->value;
    }
    if (cv.sv_maxcmdrate != nullptr) {
        out.present |= std::to_underlying(ConfigPresent::kCfgSvMaxcmdrate);
        out.sv_maxcmdrate = cv.sv_maxcmdrate->value;
    }
    if (cv.sv_mincmdrate != nullptr) {
        out.present |= std::to_underlying(ConfigPresent::kCfgSvMincmdrate);
        out.sv_mincmdrate = cv.sv_mincmdrate->value;
    }
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
