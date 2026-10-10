// HldsMonitoring — ReHLDS hook handlers (plan Phase 3, design §5).
//
// Every handler forwards to chain->callNext and returns its result (INV-1).
// SV_DropClient snapshots the client BEFORE forwarding. PreprocessPacket only
// maintains O(1) counters and returns the chain result (never false).

#include "plugin.h"
#include "hook_manager.h"
#include "sampler.h"

#include "events.h"

namespace hldsmon {
namespace {

inline uint32_t pack_ip4(const unsigned char ip[4]) noexcept {
    return (static_cast<uint32_t>(ip[0]) << 24) |
           (static_cast<uint32_t>(ip[1]) << 16) |
           (static_cast<uint32_t>(ip[2]) << 8) |
           static_cast<uint32_t>(ip[3]);
}

inline uint16_t rel_fill16(int reliable_length) noexcept {
    if (reliable_length <= 0) {
        return 0;
    }
    const uint32_t pct = static_cast<uint32_t>(reliable_length) * 100u /
                         static_cast<uint32_t>(MAX_MSGLEN);
    return pct > 0xFFFFu ? static_cast<uint16_t>(0xFFFFu)
                         : static_cast<uint16_t>(pct);
}

// Milliseconds the client had been silent at drop: engine realtime minus the
// netchan's last_received, rounded and clamped to [0, UINT32_MAX]. 0 when the
// clock is unavailable or last_received is at/ahead of `now_real`.
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

// Bounded per-IP table: open addressing, at most 8 probes, no allocation.
void ip_insert(uint32_t ip) noexcept {
    PluginContext& c = ctx();
    if (c.ip_table == nullptr || c.ip_table_size == 0) {
        return;
    }
    const uint32_t mask = c.ip_table_size - 1u;
    const uint32_t h = (ip * 2654435761u) & mask;
    for (uint32_t probe = 0; probe < 8u; ++probe) {
        IpEntry& e = c.ip_table[(h + probe) & mask];
        if (e.ip == ip) {
            ++e.count;
            return;
        }
        if (e.ip == 0u) {
            e.ip = ip;
            e.count = 1;
            ++c.ip_unique;
            return;
        }
    }
    ++c.ip_overflow;
}

void emit_reject(const netadr_t* adr, const char* stage, const char* detail,
                 const char* steamid) noexcept {
    if (!plugin_enabled()) {
        return;
    }
    RejectRecord r;
    std::memset(&r, 0, sizeof(r));
    r.t = platform_wall_seconds();
    if (adr != nullptr) {
        format_ip(*adr, r.ip, sizeof(r.ip));
    }
    copy_str(r.steamid, sizeof(r.steamid), steamid != nullptr ? steamid : "");
    copy_str(r.stage, sizeof(r.stage), stage != nullptr ? stage : "");
    copy_str(r.detail, sizeof(r.detail), detail != nullptr ? detail : "");
    events::write_reject(r);
    ++ctx().reject_count;
}

void update_massdrop(const char* category, double now_real) noexcept {
    PluginContext& c = ctx();
    MassDropState& m = c.massdrop;
    const bool same = (m.category[0] != '\0') &&
                      (std::strcmp(m.category, category) == 0) &&
                      (now_real - m.last_t <= kMassDropWindowS);
    if (!same) {
        copy_str(m.category, sizeof(m.category), category);
        m.count = 1;
        m.emitted = false;
    } else if (m.count != 0xFFFFFFFFu) {
        ++m.count;
    }
    m.last_t = now_real;

    if (m.count == kMassDropCount && !m.emitted) {
        m.emitted = true;
        if (plugin_enabled()) {
            MassDropRecord r;
            std::memset(&r, 0, sizeof(r));
            r.t = platform_wall_seconds();
            r.window_s = static_cast<uint32_t>(kMassDropWindowS);
            r.count = static_cast<uint16_t>(m.count);
            copy_str(r.reason, sizeof(r.reason), category);
            copy_str(r.category, sizeof(r.category), category);
            events::write_massdrop(r);
            ++c.massdrop_count;
        }
    }
}

// Stamp the per-slot identity cache at ClientConnected (engine pointers are
// live here; authid is a rotating static buffer so it is copied immediately).
void stamp_identity(IGameClient* cl, int id) noexcept {
    PluginContext& c = ctx();
    const int slot = id + 1;
    if (slot < 1 || slot > kMaxSlots) {
        return;
    }
    ClientIdentity& ci = c.ident[slot];
    ci = ClientIdentity{};
    ci.slot = slot;
    if (cl == nullptr) {
        return;
    }

    copy_str_utf8(ci.name, sizeof(ci.name), cl->GetName());

    edict_t* ed = cl->GetEdict();
    if (ed != nullptr && g_engfuncs.pfnGetPlayerAuthId != nullptr) {
        const char* aid = g_engfuncs.pfnGetPlayerAuthId(ed);
        if (aid != nullptr) {
            copy_str_utf8(ci.authid, sizeof(ci.authid), aid);
        }
    }

    USERID_t* uid = cl->GetNetworkUserID();
    if (uid != nullptr && uid->m_SteamID != 0) {
        format_u64_dec(uid->m_SteamID, ci.steamid64, sizeof(ci.steamid64));
    }

    INetChan* nc = cl->GetNetChan();
    if (nc != nullptr) {
        const netadr_t* a = nc->GetRemoteAdr();
        if (a != nullptr) {
            format_ip(*a, ci.ip, sizeof(ci.ip));
            if (a->type == NA_IP) {
                const uint16_t p = a->port;
                ci.port = static_cast<int32_t>(static_cast<uint16_t>((p >> 8) | (p << 8)));
            }
        }
    }

    client_t* ct = client_by_id(c.svs, id);
    if (ct != nullptr) {
        ci.userid = ct->userid;
        ci.fake = ct->fakeclient ? true : false;
        ci.proxy = ct->proxy ? true : false;
    }

    ci.connect_at = platform_wall_seconds();
    ci.valid = true;
}

void build_drop_record(int id, IGameClient* live, bool crash, const char* reason,
                       const char* observed_via, DropRecord& rec) noexcept {
    PluginContext& c = ctx();
    std::memset(&rec, 0, sizeof(rec));
    rec.t = platform_wall_seconds();
    rec.id = static_cast<uint8_t>(id);
    rec.gen = (id >= 0 && id < c.maxclients) ? c.clients[id].gen : 0;
    copy_str(rec.reason, sizeof(rec.reason), reason != nullptr ? reason : "");
    classify_reason(reason, rec.category, sizeof(rec.category));
    rec.crash = crash ? 1 : 0;
    copy_str(rec.observed_via, sizeof(rec.observed_via),
             observed_via != nullptr ? observed_via : "unknown");
    fill_identity(rec, live, id, rec.t);

    const double now_real = c.funcs != nullptr ? c.funcs->GetRealTime() : 0.0;
    rec.netchan.now_real = static_cast<float>(now_real);
    if (g_engfuncs.pfnCVarGetPointer != nullptr) {
        const cvar_t* tmo = g_engfuncs.pfnCVarGetPointer("sv_timeout");
        if (tmo != nullptr) {
            rec.netchan.sv_timeout = tmo->value;
        }
    }

    client_t* ct = client_by_id(c.svs, id);
    if (ct != nullptr) {
        rec.client.active = ct->active ? 1 : 0;
        rec.client.spawned = ct->spawned ? 1 : 0;
        rec.client.fully_connected = ct->fully_connected ? 1 : 0;
        rec.client.connected = ct->connected ? 1 : 0;
        rec.client.proxy = ct->proxy ? 1 : 0;
        rec.client.fake = ct->fakeclient ? 1 : 0;
        rec.client.userid = static_cast<uint32_t>(ct->userid);
        rec.client.connecttime = static_cast<float>(ct->connecttime);
        rec.client.cmdtime = static_cast<float>(ct->cmdtime);
        rec.client.connection_started = static_cast<float>(ct->connection_started);

        rec.netchan.in_seq = static_cast<uint32_t>(ct->netchan.incoming_sequence);
        rec.netchan.in_ack = static_cast<uint32_t>(ct->netchan.incoming_acknowledged);
        rec.netchan.in_rel_ack = static_cast<uint32_t>(ct->netchan.incoming_reliable_acknowledged);
        rec.netchan.in_rel_seq = static_cast<uint32_t>(ct->netchan.incoming_reliable_sequence);
        rec.netchan.out_seq = static_cast<uint32_t>(ct->netchan.outgoing_sequence);
        rec.netchan.rel_seq = static_cast<uint32_t>(ct->netchan.reliable_sequence);
        rec.netchan.last_rel_seq = static_cast<uint32_t>(ct->netchan.last_reliable_sequence);
        const int rl = ct->netchan.reliable_length;
        rec.netchan.rel_len = rl > 0
                                  ? static_cast<uint16_t>(rl > 0xFFFF ? 0xFFFF : rl)
                                  : 0;
        rec.netchan.rel_fill = rel_fill16(rl);
        rec.netchan.rate = static_cast<uint32_t>(ct->netchan.rate);
        rec.netchan.cleartime = static_cast<float>(ct->netchan.cleartime);
        rec.netchan.msg_flags = ct->netchan.message.flags;
        rec.netchan.last_received = ct->netchan.last_received;
        rec.netchan.connect_time = ct->netchan.connect_time;
        rec.netchan.idle_ms = idle_ms_since(now_real, ct->netchan.last_received);
    }

    double dw = cvar_f(c.cvars.drop_window, 60.0f);
    if (dw < 0.0) {
        dw = 0.0;
    }
    size_t n = static_cast<size_t>(dw);
    if (n > HLDSMON_DROP_WINDOW_MAX) {
        n = HLDSMON_DROP_WINDOW_MAX;
    }
    if (id >= 0 && id < c.maxclients && n > 0) {
        rec.window_count = static_cast<uint16_t>(c.clients[id].ring.last(n, rec.window));
    }

    if (c.server_ring.size() > 0) {
        rec.server = c.server_ring.at(c.server_ring.size() - 1);
    }

    uint32_t count = 0;
    double sum_loss = 0.0;
    double sum_ping = 0.0;
    float max_loss = 0.0f;
    for (int i = 0; i < c.maxclients; ++i) {
        if (i == id) {
            continue;
        }
        client_t* o = client_by_id(c.svs, i);
        if (o == nullptr || !o->active) {
            continue;
        }
        ++count;
        const float l = o->packet_loss;
        sum_loss += static_cast<double>(l);
        sum_ping += static_cast<double>(o->latency);
        if (l > max_loss) {
            max_loss = l;
        }
    }
    rec.neighbors.count = static_cast<uint16_t>(count);
    if (count > 0) {
        rec.neighbors.avg_loss = static_cast<float>(sum_loss / static_cast<double>(count));
        rec.neighbors.avg_ping = static_cast<float>(sum_ping / static_cast<double>(count));
        rec.neighbors.max_loss = max_loss;
    }
}

} // namespace

void h_SV_DropClient(IRehldsHook_SV_DropClient* chain, IGameClient* cl,
                     bool crash, const char* reason) {
    PluginContext& c = ctx();
    if (c.attached && plugin_enabled() && cl != nullptr) {
        const int id = cl->GetId();
        if (id >= 0 && id < c.maxclients) {
            const double now_real = c.funcs != nullptr ? c.funcs->GetRealTime() : 0.0;
            DropRecord rec;
            build_drop_record(id, cl, crash, reason, "sv_dropclient", rec);
            events::write_drop(rec);
            c.clients[id].last_drop_t = now_real;
            ++c.drop_count;
            update_massdrop(rec.category, now_real);
        }
    }
    chain->callNext(cl, crash, reason);
}

void h_Steam_NotifyClientDisconnect(IRehldsHook_Steam_NotifyClientDisconnect* chain,
                                    IGameClient* cl) {
    PluginContext& c = ctx();
    if (c.attached && cl != nullptr) {
        const int id = cl->GetId();
        if (id >= 0 && id < c.maxclients) {
            const double now_real = c.funcs != nullptr ? c.funcs->GetRealTime() : 0.0;
            if (plugin_enabled() &&
                (c.clients[id].last_drop_t == 0.0 ||
                 now_real - c.clients[id].last_drop_t > 2.0)) {
                DropRecord rec;
                build_drop_record(id, cl, false, "Steam_NotifyClientDisconnect", "fallback", rec);
                events::write_drop(rec);
                ++c.fallback_drop_count;
                update_massdrop(rec.category, now_real);
            }
            c.clients[id].ring.clear();
            c.clients[id].has_prev = false;
        }
    }
    chain->callNext(cl);
}

void h_ClientConnected(IRehldsHook_ClientConnected* chain, IGameClient* cl) {
    PluginContext& c = ctx();
    if (c.attached && cl != nullptr) {
        const int id = cl->GetId();
        if (id >= 0 && id < c.maxclients) {
            ClientRuntime& cr = c.clients[id];
            cr.ring.clear();
            cr.has_prev = false;
            cr.prev_loss = 0;
            cr.prev_choke = 0;
            cr.user_rate = 0;
            cr.last_voice_t = 0.0f;
            cr.last_drop_t = 0.0;

            uint64_t sid = 0;
            USERID_t* uid = cl->GetNetworkUserID();
            if (uid != nullptr) {
                sid = uid->m_SteamID;
            }
            const double now_real = c.funcs != nullptr ? c.funcs->GetRealTime() : 0.0;
            cr.gen = sid != 0 ? static_cast<uint32_t>(sid & 0xFFFFFFFFull)
                              : static_cast<uint32_t>(now_real);

            INetChan* nc = cl->GetNetChan();
            if (nc != nullptr) {
                const netadr_t* a = nc->GetRemoteAdr();
                if (a != nullptr) {
                    format_ip(*a, cr.ip, sizeof(cr.ip));
                }
            }
            format_steamid(sid, cr.steamid, sizeof(cr.steamid));
            copy_str(cr.name, sizeof(cr.name), cl->GetName());
            stamp_identity(cl, id);

            if (plugin_enabled()) {
                ConnectRecord rec;
                std::memset(&rec, 0, sizeof(rec));
                rec.t = platform_wall_seconds();
                rec.id = static_cast<uint8_t>(id);
                copy_str(rec.ip, sizeof(rec.ip), cr.ip);
                copy_str(rec.steamid, sizeof(rec.steamid), cr.steamid);
                copy_str(rec.name, sizeof(rec.name), cr.name);
                events::write_connect(rec);
            }
        }
    }
    chain->callNext(cl);
}

void h_SV_ConnectClient(IRehldsHook_SV_ConnectClient* chain) {
    if (ctx().attached) {
        ++ctx().connect_attempts;
    }
    chain->callNext();
}

int h_SV_CheckProtocol(IRehldsHook_SV_CheckProtocol* chain, netadr_t* adr, int protocol) {
    const int r = chain->callNext(adr, protocol);
    if (r == 0) {
        emit_reject(adr, "SV_CheckProtocol", "", "");
    }
    return r;
}

int h_SV_CheckIPRestrictions(IRehldsHook_SV_CheckIPRestrictions* chain, netadr_t* adr, int type) {
    const int r = chain->callNext(adr, type);
    if (r == 0) {
        emit_reject(adr, "SV_CheckIPRestrictions", "", "");
    }
    return r;
}

int h_SV_FinishCertificateCheck(IRehldsHook_SV_FinishCertificateCheck* chain, netadr_t* adr,
                                 int type, char* key, char* val) {
    const int r = chain->callNext(adr, type, key, val);
    if (r == 0) {
        emit_reject(adr, "SV_FinishCertificateCheck", val, "");
    }
    return r;
}

int h_SV_CheckKeyInfo(IRehldsHook_SV_CheckKeyInfo* chain, netadr_t* adr, char* key,
                       uint16* pkc, int* pkg, char* steamid, char* val) {
    const int r = chain->callNext(adr, key, pkc, pkg, steamid, val);
    if (r == 0) {
        emit_reject(adr, "SV_CheckKeyInfo", val, steamid);
    }
    return r;
}

int h_SV_CheckUserInfo(IRehldsHook_SV_CheckUserInfo* chain, netadr_t* adr, char* userinfo,
                        qboolean reconnect, int slot, char* name) {
    const int r = chain->callNext(adr, userinfo, reconnect, slot, name);
    if (r == 0) {
        emit_reject(adr, "SV_CheckUserInfo", userinfo, "");
    }
    return r;
}

qboolean h_Steam_NotifyClientConnect(IRehldsHook_Steam_NotifyClientConnect* chain,
                                     IGameClient* cl, const void* key, unsigned int len) {
    const qboolean r = chain->callNext(cl, key, len);
    if (r == 0) {
        char sid[32];
        std::memset(sid, 0, sizeof(sid));
        if (cl != nullptr) {
            USERID_t* uid = cl->GetNetworkUserID();
            if (uid != nullptr) {
                format_steamid(uid->m_SteamID, sid, sizeof(sid));
            }
        }
        emit_reject(nullptr, "Steam_NotifyClientConnect", "", sid);
    }
    return r;
}

void h_SVC_GetChallenge_mod(IRehldsHook_SVC_GetChallenge_mod* chain, char* buffer, int len) {
    if (ctx().attached) {
        ++ctx().challenge_count;
    }
    chain->callNext(buffer, len);
}

bool h_PreprocessPacket(IRehldsHook_PreprocessPacket* chain, uint8* data,
                        unsigned int len, const netadr_t& adr) {
    PluginContext& c = ctx();
    if (c.attached && plugin_enabled()) {
        ++c.pkt_count;
        c.pkt_bytes += static_cast<uint64_t>(len);
        if (len < 128u) {
            ++c.pkt_sizes.lt128;
        } else if (len < 512u) {
            ++c.pkt_sizes.lt512;
        } else if (len < 1024u) {
            ++c.pkt_sizes.lt1024;
        } else {
            ++c.pkt_sizes.ge1024;
        }
        ip_insert(pack_ip4(adr.ip));
    }
    return chain->callNext(data, len, adr);
}

void h_HandleNetCommand(IRehldsHook_HandleNetCommand* chain, IGameClient* cl, uint8 cmd) {
    PluginContext& c = ctx();
    if (c.attached && plugin_enabled() && cl != nullptr) {
        const int id = cl->GetId();
        if (id >= 0 && id < c.maxclients) {
            ++c.netcmd_counts[id][cmd];
        }
        // ReHLDS's clc enum has no clc_disconnect; clc_bad (0) marks an aborted
        // client message and is counted as a disconnect candidate.
        if (cmd == 0) {
            ++c.netcmd_disconnect_candidates;
        }
    }
    chain->callNext(cl, cmd);
}

void h_SV_Frame(IRehldsHook_SV_Frame* chain) {
    PluginContext& c = ctx();
    double frame_ms = 0.0;
    if (gpGlobals != nullptr) {
        frame_ms = static_cast<double>(gpGlobals->frametime) * 1000.0;
    }
    if (frame_ms <= 0.0 && c.funcs != nullptr) {
        frame_ms = c.funcs->GetHostFrameTime() * 1000.0;
    }
    if (c.attached) {
        const double now_real = c.funcs != nullptr ? c.funcs->GetRealTime() : 0.0;
        sampler_on_frame(frame_ms, now_real);
    }
    chain->callNext();
}

void h_SV_Spawn_f(IRehldsHook_SV_Spawn_f* chain) {
    PluginContext& c = ctx();
    if (c.attached) {
        const char* m = current_map();
        if (m != nullptr && std::strcmp(m, c.last_map) != 0) {
            copy_str(c.last_map, sizeof(c.last_map), m);
            c.massdrop.category[0] = '\0';
            c.massdrop.count = 0;
            c.massdrop.emitted = false;
            if (plugin_enabled()) {
                MapChangeRecord rec;
                std::memset(&rec, 0, sizeof(rec));
                rec.t = platform_wall_seconds();
                copy_str(rec.map, sizeof(rec.map), m);
                events::write_mapchange(rec);
            }
        }
        modnet_request_rescan();
    }
    chain->callNext();
}

} // namespace hldsmon
