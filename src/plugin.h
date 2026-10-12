// HldsMonitoring — shared plugin context and engine include stack (Phase 3/4).
//
// Engine translation units include this header; host tests never do. It pulls
// in the vendored HLSDK/Metamod/ReHLDS headers, the concrete client_t/netchan_t
// definitions, and the frozen host-testable core headers.

#ifndef HLDSMON_PLUGIN_H
#define HLDSMON_PLUGIN_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <extdll.h>
#include <eiface.h>
#include <pm_defs.h>
#include <pm_movevars.h>
#include <com_progdefs.h>
#include <meta_api.h>
#include <rehlds_api.h>
#include <interface.h>

// com_client.h needs MAX_INFO_STRING, which ReAPI obtains from amxmodx; define
// the canonical GoldSrc value before pulling the struct in.
#ifndef MAX_INFO_STRING
#define MAX_INFO_STRING 256
#endif

// com_client.h renders clientOfEdict()/clientOfIndex() in terms of these; we
// never call them (ABI access goes through GetClient_t), but they must resolve
// at parse time. Keep the definitions local to the plugin.
extern edict_t* g_pEdicts;
class IRehldsServerStatic;
extern IRehldsServerStatic* g_RehldsSvs;
inline size_t indexOfEdict(const edict_t* ed) { return static_cast<size_t>(ed - g_pEdicts); }
inline edict_t* edictByIndex(int index) { return g_pEdicts + index; }

#include <com_client.h>

#include "engine_layout.h"
#include "host_metrics.h"
#include "log_writer.h"
#include "metrics.h"
#include "modnet_got.h"
#include "platform_linux.h"
#include "runtime_ring.h"
#include "serialization.h"

// ---- engine/metamod globals (defined in bootstrap.cpp) ----
extern enginefuncs_t      g_engfuncs;
extern globalvars_t*      gpGlobals;
extern meta_globals_t*    gpMetaGlobals;
extern gamedll_funcs_t*   gpGamedllFuncs;
extern mutil_funcs_t*     gpMetaUtilFuncs;
extern plugin_info_t      Plugin_info;

namespace hldsmon {

// ---- compile-time bounds / sizes ----
constexpr size_t kLineBufSize    = 65536; // largest line: drop record + window
constexpr int    kClientsPerTick = 8;     // staggered client poll per tick
constexpr double kHitchMs        = 100.0; // hitch threshold (design §6.3)
constexpr double kMassDropWindowS = 5.0;  // burst window (design §5.3)
constexpr uint32_t kMassDropCount = 5;    // drops per window => massdrop flag
constexpr int    kMaxSlots        = 32;   // client identity cache bound (1..maxclients)
constexpr uint32_t kIdleZeroFlowMs = 1500; // no inbound packet for >this => instantaneous flow is 0

static_assert(MAX_CLIENTS == kMaxSlots, "identity cache bound must match MAX_CLIENTS");

// Cached cvar pointers (registered in bootstrap.cpp).
struct CvarRefs {
    cvar_t* enable      = nullptr;
    cvar_t* client_hz   = nullptr;
    cvar_t* disk_hz     = nullptr;
    cvar_t* flush_sec   = nullptr;
    cvar_t* client_ring = nullptr;
    cvar_t* server_ring = nullptr;
    cvar_t* drop_window = nullptr;
    cvar_t* dir         = nullptr;
    cvar_t* max_mb      = nullptr;
    cvar_t* ip_table    = nullptr;
    cvar_t* retention_days = nullptr;
    cvar_t* modnet           = nullptr;
    cvar_t* modnet_targets   = nullptr;
    cvar_t* modnet_rescan_sec = nullptr;
    cvar_t* host             = nullptr;
    // Tracked engine cvars (config-context stage, design §22.3): cached once at
    // bootstrap, read-only, nullptr when not registered (=> JSON null).
    cvar_t* sys_ticrate      = nullptr;
    cvar_t* host_framerate   = nullptr;
    cvar_t* sv_timeout       = nullptr;
    cvar_t* sv_maxrate       = nullptr;
    cvar_t* sv_minrate       = nullptr;
    cvar_t* sv_maxupdaterate = nullptr;
    cvar_t* sv_minupdaterate = nullptr;
    cvar_t* sv_maxcmdrate    = nullptr;
    cvar_t* sv_mincmdrate    = nullptr;
};

// Mass-drop burst tracker.
struct MassDropState {
    double   last_t    = 0.0;
    uint32_t count     = 0;
    bool     emitted   = false;
    char     category[32] = {0};
};

// Single in-flight connect context (design §25.3, CT-1). SV_ConnectClient has
// no arguments, so t0 cannot be tied to a slot: one fixed context tracks "a
// connect is in progress"; a new SV_ConnectClient overwrites it and `active`
// gates correlation. Missing markers leave the timing fields JSON null (CT-6).
struct ConnectCtx {
    double  t0          = 0.0;  // GetRealTime at SV_ConnectClient entry
    double  steam_enter = 0.0;  // measured Steam_NotifyClientConnect stage in
                                // seconds (>0) for the in-flight connect;
                                // 0 = not measured => steam_ms null (CT-6)
    uint8_t active      = 0;    // 1 while a connect is in progress
};

// Delayed-emission mapchange context (design §25.3 п.2, CT-2): SV_Spawn_f only
// records the spawn time and the pending map; the 1 Hz server tick emits the
// record when the world is ready (or after the tick window), so load_ms can be
// measured. The massdrop reset and the same-map gate stay in SV_Spawn_f.
struct MapCtx {
    double   t_spawn         = 0.0;  // GetRealTime at SV_Spawn_f
    char     pending_map[64] = {0};
    uint8_t  pending         = 0;
    uint16_t ticks           = 0;    // 1 Hz server ticks since spawn
};

// Per-client in-memory state (ring + delta baselines + identity).
struct ClientRuntime {
    RuntimeRing<ClientSample> ring; // window of recent samples
    uint32_t gen          = 0;      // generation tag (connect-time/SteamID)
    bool     has_prev     = false;
    uint32_t prev_loss    = 0;
    uint32_t prev_choke   = 0;
    uint32_t user_rate    = 0;      // userinfo "rate" (parsed, no alloc)
    float    last_voice_t = 0.0f;
    double   last_drop_t  = 0.0;    // engine real time of last drop record
    // packet-quality match + jitter accumulators (design §24.3 п.1/п.2); the
    // accumulator is drained by sample_clients, the packed match is (re)stamped
    // at ClientConnected and cleared when the client leaves.
    double   prev_pkt_real = 0.0;   // engine realtime of the last matched packet
    bool     pkt_has_prev  = false;
    uint32_t ip_packed     = 0;     // packed source IPv4 (0 = no match)
    uint16_t port_packed   = 0;     // raw netadr_t port (network order)
    uint32_t jit_min       = 0;
    uint32_t jit_sum       = 0;
    uint32_t jit_max       = 0;
    uint32_t jit_count     = 0;
    char     ip[64]       = {0};
    char     steamid[32]  = {0};
    char     name[64]     = {0};
};

// Full reset of the per-client packet-quality state (packed IP+port match,
// previous-packet clock, jitter accumulators). Called at ClientConnected and
// when the client leaves; sample_clients clears only the accumulators.
inline void reset_client_pkt_state(ClientRuntime& cr) noexcept {
    cr.prev_pkt_real = 0.0;
    cr.pkt_has_prev = false;
    cr.ip_packed = 0;
    cr.port_packed = 0;
    jitter_reset(cr.jit_min, cr.jit_sum, cr.jit_max, cr.jit_count);
}

// Preallocated per-client identity cache (plan work item B). Stamped at
// ClientConnected, refreshed live at emit time; indexed by slot 1..maxclients.
// No allocation: all fields are plain arrays/scalars read at ~1 Hz / events.
struct ClientIdentity {
    bool    valid      = false;
    char    name[32]   = {0};
    char    authid[64] = {0};
    char    steamid64[20] = {0};
    char    ip[16]     = {0};
    int32_t port       = -1;   // host order (-1 unknown)
    int32_t userid     = -1;
    int32_t slot       = 0;
    double  connect_at = 0.0;  // wall-clock epoch at ClientConnected
    bool    fake       = false;
    bool    proxy      = false;
};

// The single server-thread plugin context. Everything is preallocated at
// Meta_Attach; the hot path only reads/writes these fields (INV-2).
struct PluginContext {
    // engine handles
    IRehldsApi*          api        = nullptr;
    const RehldsFuncs_t* funcs      = nullptr;
    IRehldsHookchains*   hookchains = nullptr;
    IRehldsServerStatic* svs        = nullptr;
    IRehldsServerData*   data       = nullptr;
    int                  maxclients = 0;

    LogWriter* log = nullptr;
    CvarRefs   cvars;

    bool attached        = false;
    bool layout_ok       = false;
    bool hooks_degraded  = false;
    int  layout_checked  = 0;

    // in-memory rings
    RuntimeRing<ServerSample> server_ring;
    ClientRuntime             clients[MAX_CLIENTS];

    // preallocated per-client identity cache (index by slot 1..maxclients)
    ClientIdentity            ident[kMaxSlots + 1];

    // frame accumulators (reset each 1 Hz interval)
    uint32_t frame_count = 0;
    float    frame_min   = 0.0f;
    float    frame_max   = 0.0f;
    double   frame_sum   = 0.0;
    uint32_t hitch_gt50  = 0;
    uint32_t hitch_gt100 = 0;

    // packet counters (reset each 1 Hz interval)
    uint32_t    pkt_count = 0;
    uint64_t    pkt_bytes = 0;
    PacketSizes pkt_sizes{};
    IpEntry*    ip_table      = nullptr;
    uint32_t    ip_table_size = 0; // power of two
    uint32_t    ip_unique     = 0;
    uint32_t    ip_overflow   = 0;

    // net-command histogram per client (reset each interval)
    uint32_t netcmd_counts[MAX_CLIENTS][256];
    uint32_t netcmd_disconnect_candidates = 0;
    uint32_t challenge_count = 0;
    uint32_t connect_attempts = 0;

    // status counters (cumulative)
    uint32_t drop_count          = 0;
    uint32_t fallback_drop_count = 0;
    uint32_t reject_count        = 0;
    uint32_t massdrop_count      = 0;
    uint32_t hitch_count         = 0;
    uint32_t packet_records      = 0;
    uint32_t serialize_failures  = 0;

    // sampler scheduling (engine real time seconds)
    double   next_server_tick_t = 0.0;
    double   next_sample_t = 0.0;
    double   next_write_t  = 0.0;
    double   last_flush_t  = 0.0;
    uint32_t client_cursor = 0;
    uint32_t write_cursor  = 0;

    // prev rusage for CPU%/deltas
    PlatformRusage prev_rusage{};
    bool           has_rusage   = false;
    double         prev_rusage_t = 0.0;

    // mass-drop + last seen map
    MassDropState massdrop;
    char          last_map[64] = {0};

    // connect-timings (design §25): in-flight connect + delayed mapchange
    ConnectCtx connect;
    MapCtx     map_ctx;

    // config-context (design §22.3): last emitted tracked-config snapshot and
    // its validity; the first enabled server-tick baselines even without a
    // change (CFG-4).
    ConfigSnapshot last_config;
    bool           last_config_valid = false;

    // Stage 9 (`event-enrich`) baselines pulled on the 1 Hz tick: LogWriter
    // health counters (EN-P1), the console transition guard, and the edict
    // churn baseline (EN-P0/EN-5; reset on mapchange via SV_Spawn_f).
    uint64_t prev_error_count  = 0;
    uint32_t prev_prune_count  = 0;
    bool     prev_log_enabled  = false;
    int32_t  prev_edicts       = 0;
    bool     edicts_has_prev   = false;

    // host-metrics (design §23.3): environment caveat mask detected once at
    // bootstrap plus the previous cumulative snapshot used as the Δ baseline.
    // The first tick has no baseline and emits nulls (HM-2/QAS-H7).
    HostSnapshot host_prev;
    bool         host_has_prev    = false;
    uint16_t     host_caveat_mask = 0;

    // modnet adapter state (mirrored from the header-free GOT adapter for
    // control output; synced on enable/disable/rescan). Server thread only.
    ModnetState   modnet;

    // preallocated scratch buffers
    char line_buf[kLineBufSize];
};

// Closes the in-flight connect context when an attempt finishes (reject or
// ClientConnected), so a stale marker cannot be attributed to a later connect
// (CT-7). Server thread only.
inline void reset_connect_ctx(PluginContext& c) noexcept {
    c.connect.t0 = 0.0;
    c.connect.steam_enter = 0.0;
    c.connect.active = 0;
}

// Single process-wide context (server thread only, design §10).
PluginContext& ctx() noexcept;

// ---- cvar helpers ----
inline float cvar_f(const cvar_t* c, float def) noexcept {
    return c != nullptr ? c->value : def;
}
inline bool plugin_enabled() noexcept {
    return cvar_f(ctx().cvars.enable, 1.0f) > 0.5f;
}

// ---- allocation-free text helpers (hot-path safe) ----

// Bounded NUL-terminating copy (never writes past dst.size()).
inline void copy_str(std::span<char> dst, const char* src) noexcept {
    if (dst.empty()) {
        return;
    }
    size_t i = 0;
    if (src != nullptr) {
        for (; src[i] != '\0' && i + 1 < dst.size(); ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

// UTF-8-safe copy: like copy_str, but when truncation would split a multibyte
// sequence it backs off to the last complete character boundary.
inline void copy_str_utf8(std::span<char> dst, const char* src) noexcept {
    if (dst.empty()) {
        return;
    }
    size_t i = 0;
    if (src != nullptr) {
        for (; src[i] != '\0' && i + 1 < dst.size(); ++i) {
            dst[i] = src[i];
        }
    }
    if (src != nullptr && src[i] != '\0') {
        size_t k = i;
        while (k > 0 && (static_cast<unsigned char>(dst[k - 1]) & 0xC0u) == 0x80u) {
            --k;
        }
        if (k == 0) {
            i = 0;
        } else {
            const unsigned char lead = static_cast<unsigned char>(dst[k - 1]);
            size_t need = 1;
            if ((lead & 0xE0u) == 0xC0u) {
                need = 2;
            } else if ((lead & 0xF0u) == 0xE0u) {
                need = 3;
            } else if ((lead & 0xF8u) == 0xF0u) {
                need = 4;
            }
            if (i - (k - 1) < need) {
                i = k - 1;
            }
        }
    }
    dst[i] = '\0';
}

inline void append_u32_dec(std::span<char> out, size_t& pos, uint32_t v) noexcept {
    char tmp[10];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0) {
            tmp[n++] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        }
    }
    while (n > 0 && pos + 1 < out.size()) {
        out[pos++] = tmp[--n];
    }
}

// Pack a 4-byte IPv4 address into a host-order uint32 (0.0.0.0 => 0).
inline uint32_t pack_ip4(const unsigned char ip[4]) noexcept {
    return (static_cast<uint32_t>(ip[0]) << 24) |
           (static_cast<uint32_t>(ip[1]) << 16) |
           (static_cast<uint32_t>(ip[2]) << 8) |
           static_cast<uint32_t>(ip[3]);
}

// "a.b.c.d" into out (NUL-terminated).
inline void format_ip(const netadr_t& a, std::span<char> out) noexcept {
    if (out.empty()) {
        return;
    }
    size_t pos = 0;
    for (int k = 0; k < 4; ++k) {
        append_u32_dec(out, pos, static_cast<uint32_t>(a.ip[k]));
        if (k != 3 && pos + 1 < out.size()) {
            out[pos++] = '.';
        }
    }
    out[pos < out.size() ? pos : out.size() - 1] = '\0';
}

// Decimal string of an unsigned 64-bit value (NUL-terminated).
inline void format_u64_dec(uint64_t v, std::span<char> out) noexcept {
    if (out.empty()) {
        return;
    }
    char tmp[20];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0) {
            tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10u));
            v /= 10u;
        }
    }
    size_t pos = 0;
    while (n > 0 && pos + 1 < out.size()) {
        out[pos++] = tmp[--n];
    }
    out[pos < out.size() ? pos : out.size() - 1] = '\0';
}

// "STEAM_x:y:z" from a SteamID64, or "STEAM_ID_PENDING"/"NOSTEAM".
inline void format_steamid(uint64_t sid, char* out, size_t cap) noexcept {
    if (cap == 0) {
        return;
    }
    std::span<char> dst(out, cap);
    if (sid == 0) {
        copy_str(dst, "STEAM_ID_PENDING");
        return;
    }
    const uint64_t kBase = 76561197960265728ULL;
    if (sid < kBase) {
        copy_str(dst, "NOSTEAM");
        return;
    }
    const uint64_t acc = sid - kBase;
    const uint32_t y = static_cast<uint32_t>(acc & 1ULL);
    const uint32_t z = static_cast<uint32_t>(acc >> 1);
    size_t pos = 0;
    const char* prefix = "STEAM_0:";
    for (const char* p = prefix; *p != '\0' && pos + 1 < cap; ++p) {
        out[pos++] = *p;
    }
    append_u32_dec(dst, pos, y);
    if (pos + 1 < cap) {
        out[pos++] = ':';
    }
    append_u32_dec(dst, pos, z);
    out[pos < cap ? pos : cap - 1] = '\0';
}

// Map a raw engine drop reason to a coarse category.
inline void classify_reason(const char* reason, char* out, size_t cap) noexcept {
    const char* cat = "drop";
    if (reason != nullptr) {
        if (std::strstr(reason, "overflow") != nullptr || std::strstr(reason, "Overflow") != nullptr) {
            cat = "overflow";
        } else if (std::strstr(reason, "timed out") != nullptr || std::strstr(reason, "timeout") != nullptr ||
                   std::strstr(reason, "Timed out") != nullptr) {
            cat = "timeout";
        } else if (std::strstr(reason, "kicked") != nullptr || std::strstr(reason, "Kicked") != nullptr) {
            cat = "kicked";
        } else if (std::strstr(reason, "error") != nullptr || std::strstr(reason, "Error") != nullptr) {
            cat = "error";
        }
    }
    copy_str(std::span<char>(out, cap), cat);
}

// Fills the identity block shared by ClientSample and DropRecord (same member
// names). Reads the live IGameClient when present (name/ip/port/userid/fake/
// proxy) and the sticky per-slot cache for authid/steamid64/connect time.
// `out` must already be zero-initialised. Passive: reads engine state only.
template <class Rec>
inline void fill_identity(Rec& out, IGameClient* live, int id, double now_epoch) noexcept {
    PluginContext& c = ctx();
    out.port = -1;
    out.userid = -1;
    out.slot = id + 1;
    out.conn_s = 0.0;
    out.fake = false;
    out.proxy = false;

    if (live != nullptr) {
        copy_str_utf8(out.name, live->GetName());
        INetChan* nc = live->GetNetChan();
        if (nc != nullptr) {
            const netadr_t* a = nc->GetRemoteAdr();
            if (a != nullptr) {
                format_ip(*a, out.ip);
                if (a->type == NA_IP) {
                    const uint16_t p = a->port;
                    out.port = static_cast<int32_t>(
                        static_cast<uint16_t>((p >> 8) | (p << 8)));
                }
            }
        }
        client_t* ct = client_by_id(c.svs, id);
        if (ct != nullptr) {
            out.userid = ct->userid;
            out.fake = ct->fakeclient ? true : false;
            out.proxy = ct->proxy ? true : false;
        }
    }

    const int slot = id + 1;
    const ClientIdentity* ci =
        (slot >= 1 && slot <= kMaxSlots) ? &c.ident[slot] : nullptr;
    if (ci != nullptr && ci->valid) {
        if (out.name[0] == '\0') {
            copy_str_utf8(out.name, ci->name);
        }
        if (out.ip[0] == '\0') {
            copy_str_utf8(out.ip, ci->ip);
        }
        if (out.port < 0) {
            out.port = ci->port;
        }
        if (out.userid < 0) {
            out.userid = ci->userid;
        }
        out.fake = out.fake || ci->fake;
        out.proxy = out.proxy || ci->proxy;
        copy_str_utf8(out.authid, ci->authid);
        copy_str_utf8(out.steamid64, ci->steamid64);
        if (ci->connect_at > 0.0 && now_epoch >= ci->connect_at) {
            out.conn_s = now_epoch - ci->connect_at;
        }
    }
}

} // namespace hldsmon

#endif // HLDSMON_PLUGIN_H
