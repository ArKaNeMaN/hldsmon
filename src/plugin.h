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
#include "log_writer.h"
#include "metrics.h"
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
};

// Bounded per-IP packet table entry (open addressing).
struct IpEntry {
    uint32_t ip;
    uint32_t count;
};

// Mass-drop burst tracker.
struct MassDropState {
    double   last_t    = 0.0;
    uint32_t count     = 0;
    bool     emitted   = false;
    char     category[32] = {0};
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
    char     ip[64]       = {0};
    char     steamid[32]  = {0};
    char     name[64]     = {0};
};

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

    // preallocated scratch buffers
    char line_buf[kLineBufSize];
};

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

// Bounded NUL-terminating copy (never writes past cap).
inline void copy_str(char* dst, size_t cap, const char* src) noexcept {
    if (cap == 0) {
        return;
    }
    size_t i = 0;
    if (src != nullptr) {
        for (; src[i] != '\0' && i + 1 < cap; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

// UTF-8-safe copy: like copy_str, but when truncation would split a multibyte
// sequence it backs off to the last complete character boundary.
inline void copy_str_utf8(char* dst, size_t cap, const char* src) noexcept {
    if (cap == 0) {
        return;
    }
    size_t i = 0;
    if (src != nullptr) {
        for (; src[i] != '\0' && i + 1 < cap; ++i) {
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

inline void append_u32_dec(char* out, size_t cap, size_t& pos, uint32_t v) noexcept {
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
    while (n > 0 && pos + 1 < cap) {
        out[pos++] = tmp[--n];
    }
}

// "a.b.c.d" into out (NUL-terminated).
inline void format_ip(const netadr_t& a, char* out, size_t cap) noexcept {
    if (cap == 0) {
        return;
    }
    size_t pos = 0;
    for (int k = 0; k < 4; ++k) {
        append_u32_dec(out, cap, pos, static_cast<uint32_t>(a.ip[k]));
        if (k != 3 && pos + 1 < cap) {
            out[pos++] = '.';
        }
    }
    out[pos < cap ? pos : cap - 1] = '\0';
}

// Decimal string of an unsigned 64-bit value (NUL-terminated).
inline void format_u64_dec(uint64_t v, char* out, size_t cap) noexcept {
    if (cap == 0) {
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
    while (n > 0 && pos + 1 < cap) {
        out[pos++] = tmp[--n];
    }
    out[pos < cap ? pos : cap - 1] = '\0';
}

// "STEAM_x:y:z" from a SteamID64, or "STEAM_ID_PENDING"/"NOSTEAM".
inline void format_steamid(uint64_t sid, char* out, size_t cap) noexcept {
    if (cap == 0) {
        return;
    }
    if (sid == 0) {
        copy_str(out, cap, "STEAM_ID_PENDING");
        return;
    }
    const uint64_t kBase = 76561197960265728ULL;
    if (sid < kBase) {
        copy_str(out, cap, "NOSTEAM");
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
    append_u32_dec(out, cap, pos, y);
    if (pos + 1 < cap) {
        out[pos++] = ':';
    }
    append_u32_dec(out, cap, pos, z);
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
    copy_str(out, cap, cat);
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
        copy_str_utf8(out.name, sizeof out.name, live->GetName());
        INetChan* nc = live->GetNetChan();
        if (nc != nullptr) {
            const netadr_t* a = nc->GetRemoteAdr();
            if (a != nullptr) {
                format_ip(*a, out.ip, sizeof out.ip);
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
            copy_str_utf8(out.name, sizeof out.name, ci->name);
        }
        if (out.ip[0] == '\0') {
            copy_str_utf8(out.ip, sizeof out.ip, ci->ip);
        }
        if (out.port < 0) {
            out.port = ci->port;
        }
        if (out.userid < 0) {
            out.userid = ci->userid;
        }
        out.fake = out.fake || ci->fake;
        out.proxy = out.proxy || ci->proxy;
        copy_str_utf8(out.authid, sizeof out.authid, ci->authid);
        copy_str_utf8(out.steamid64, sizeof out.steamid64, ci->steamid64);
        if (ci->connect_at > 0.0 && now_epoch >= ci->connect_at) {
            out.conn_s = now_epoch - ci->connect_at;
        }
    }
}

} // namespace hldsmon

#endif // HLDSMON_PLUGIN_H
