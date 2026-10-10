// HldsMonitoring — host-testable POD metric records (plan Phase 1, design §6, Appendix P1).
//
// Pure POD, fixed-width fields, 1-byte packing. No engine/metamod headers, no
// allocation, no std::string. Field sets and JSON key order follow plan
// Appendix P1 (authoritative schema); nested slices follow design §6.
//
// Budgets: sizeof(ClientSample) < 256 bytes (QAS-4). The identity block (name/
// authid/steamid64/ip/port/... ) drives this bound; it was raised from 160 with
// the per-client identity work. DropRecord carries the bounded drop window
// inline (hldsmon_drop_window <= HLDSMON_DROP_WINDOW_MAX).

#ifndef HLDSMON_METRICS_H
#define HLDSMON_METRICS_H

#include <cstddef>
#include <cstdint>

namespace hldsmon {

// Maximum number of preceding client samples embedded in a drop-record window.
// Kept as a compile-time bound so DropRecord stays a trivially-copyable POD.
constexpr size_t HLDSMON_DROP_WINDOW_MAX = 64;

#pragma pack(push, 1)

// Per-client sample — design §6.2 / P1 `client`.
struct ClientSample {
    double   t;                  // wall-clock epoch seconds
    uint8_t  id;                 // player slot
    uint32_t gen;                // generation tag (connect-time / SteamID)
    uint16_t ping;               // latency, ms
    uint32_t loss;               // raw cumulative packet loss
    uint32_t dloss;              // cumulative -> delta
    uint32_t choke;              // raw cumulative choke
    uint32_t dchoke;             // cumulative -> delta
    int32_t  delta_seq;          // netchan delta sequence
    uint32_t in_seq;             // incoming_sequence
    uint32_t in_ack;             // incoming_acknowledged
    uint32_t out_seq;            // outgoing_sequence
    uint32_t rel_seq;            // reliable_sequence
    uint16_t rel_fill;           // reliable_length / MAX_MSGLEN * 100
    uint32_t rate;               // bytes/sec
    float    cleartime;          // netchan cleartime
    uint16_t updaterate;         // cl_updaterate (userinfo)
    uint16_t cmdrate;            // cl_cmdrate (userinfo)
    uint8_t  state;              // bitmask: active|spawned|fully_connected|connected
    uint8_t  fragbuf_total;      // sum of fragbufcount[]
    uint32_t incomingready_mask; // bitmask of incomingready[]
    float    flow_in_kbps;       // flow[k].kbytespersec (in)
    float    flow_out_kbps;      // flow[k].kbytespersec (out)
    float    flow_in_avg;        // flow[k].avgkbytespersec (in)
    float    flow_out_avg;      // flow[k].avgkbytespersec (out)
    uint16_t msg_flags;          // message sizebuf flags
    uint32_t idle_ms;            // ms since last received packet (0 = unknown)

    // ---- per-client identity (P1 identity block; read ~1 Hz / at events, never
    //      on the per-packet path). Empty strings / -1 / 0 when unknown. ----
    char     name[32];           // player name (UTF-8, cut at a char boundary)
    char     authid[64];         // Steam2 auth id ("STEAM_x:y:z", "BOT", ...)
    char     steamid64[20];      // SteamID64 decimal string ("" when unknown)
    char     ip[16];             // dotted IPv4
    int32_t  port;               // UDP source port, host order (-1 unknown)
    int32_t  userid;             // engine userid (-1 unknown)
    int32_t  slot;               // 1..maxclients
    double   conn_s;             // seconds connected (0 when unknown)
    bool     fake;               // bot / fake client
    bool     proxy;              // HLTV / proxy
};

// Per-server 1 Hz aggregate — design §6.1 / P1 `server`.
struct ServerSample {
    double   t;              // wall-clock epoch seconds
    float    frame_ms_min;
    float    frame_ms_avg;
    float    frame_ms_max;
    uint32_t hitch_gt50;     // frames > 50 ms this interval
    uint32_t hitch_gt100;    // frames > 100 ms this interval
    float    cpu_pct;
    uint32_t rss_kb;
    uint64_t minflt;         // cumulative (Delta computed by caller)
    uint64_t majflt;         // cumulative
    uint16_t players;        // all
    uint16_t real;           // real players
    uint16_t proxy;
    uint16_t fake;
    float    in_kbps;        // sum of flow.avgkbytespersec (in)
    float    out_kbps;       // sum of flow.avgkbytespersec (out)
    uint32_t edicts;         // num_edicts
    char     map[64];        // current map name
    uint32_t uptime_s;
    float    fps;            // measured FPS
    uint8_t  maxclients;     // runtime maxclients
};

// client_t slice at drop — P1 drop.client.
struct ClientSnapshot {
    uint8_t  active;
    uint8_t  spawned;
    uint8_t  fully_connected;
    uint8_t  connected;
    uint8_t  proxy;
    uint8_t  fake;
    uint32_t userid;
    float    connecttime;
    float    cmdtime;
    float    connection_started;
};

// netchan_t slice at drop — P1 drop.netchan.
struct NetchanSnapshot {
    uint32_t in_seq;         // incoming_sequence
    uint32_t in_ack;         // incoming_acknowledged
    uint32_t in_rel_ack;     // incoming_reliable_acknowledged
    uint32_t in_rel_seq;     // incoming_reliable_sequence
    uint32_t out_seq;        // outgoing_sequence
    uint32_t rel_seq;        // reliable_sequence
    uint32_t last_rel_seq;   // last_reliable_sequence
    uint16_t rel_len;        // reliable_length
    uint16_t rel_fill;       // reliable_length / MAX_MSGLEN * 100
    uint32_t rate;
    float    cleartime;
    uint16_t msg_flags;
    float    last_received;
    float    connect_time;
    float    now_real;       // engine realtime at drop
    uint32_t idle_ms;        // ms since last_received (round, clamped >=0)
    float    sv_timeout;     // sv_timeout cvar value at drop (0 = unavailable)
};

// Neighbouring-client context at drop — P1 drop.neighbors.
struct NeighborsInfo {
    uint16_t count;
    float    avg_loss;
    float    avg_ping;
    float    max_loss;
};

// Drop record — design §6.3 / P1 `drop`.
struct DropRecord {
    double         t;
    uint8_t        id;
    uint32_t       gen;
    char           reason[128];       // engine-formatted reason (raw)
    char           category[32];      // timeout|drop|kicked|overflow|error
    uint8_t        crash;
    char           observed_via[32];  // sv_dropclient|fallback
    // ---- identity block (P1; read live at drop with the cache as fallback) ----
    char           name[32];
    char           authid[64];
    char           steamid64[20];
    char           ip[16];
    int32_t        port;
    int32_t        userid;
    int32_t        slot;
    double         conn_s;
    bool           fake;
    bool           proxy;
    ClientSnapshot client;
    NetchanSnapshot netchan;
    ClientSample   window[HLDSMON_DROP_WINDOW_MAX];
    uint16_t       window_count;      // valid entries in window
    ServerSample   server;
    NeighborsInfo  neighbors;
};

// Rejected connection — P1 `reject`.
struct RejectRecord {
    double t;
    char   ip[64];
    char   steamid[32];
    char   stage[64];    // which validator rejected
    char   detail[128];
};

// Successful connect — P1 `connect`.
struct ConnectRecord {
    double  t;
    uint8_t id;
    char    ip[64];
    char    steamid[32];
    char    name[64];
};

// Net command histogram entry — P1 `netcmd`.
struct NetCmdRecord {
    double   t;
    uint8_t  id;
    uint8_t  cmd;
    uint32_t count;
};

// Mass-disconnect flag — P1 `massdrop`.
struct MassDropRecord {
    double   t;
    uint32_t window_s;
    uint16_t count;
    char     reason[128];
    char     category[32];
};

// Packet-size histogram — P1 `packet`.
struct PacketSizes {
    uint32_t lt128;
    uint32_t lt512;
    uint32_t lt1024;
    uint32_t ge1024;
};

struct PacketStats {
    double      t;
    uint32_t    pps;
    uint64_t    bytes;
    PacketSizes sizes;
    uint32_t    unique_ips;
    char        top_ip[64];
};

// Frame hitch — P1 `hitch`.
struct HitchRecord {
    double   t;
    uint32_t duration_ms;
};

// Map change marker — P1 `mapchange`.
struct MapChangeRecord {
    double t;
    char   map[64];
};

// Voice activity — P1 `voice`.
struct VoiceRecord {
    double  t;
    uint8_t id;
    float   last_voice_time;
};

// Graceful shutdown marker — P1 `shutdown`.
struct ShutdownRecord {
    double t;
    char   reason[16];   // "graceful"
};

// Fatal-signal marker (best-effort, async-signal-safe) — P1 `crash`.
struct CrashRecord {
    double  t;
    int32_t signal;
};

// Per-`.so` network attribution (modnet stage) — P1 `modnet` extension.
// Emitted (delta over the interval) only for target objects with activity.
struct ModnetRecord {
    double   t;                  // wall-clock epoch seconds (emission moment)
    char     module[64];         // short module label (e.g. easy_http_amxx_i386)
    char     so[64];             // basename of the loaded object
    uint64_t conns;              // Δ successful blocking connect() + accept()
    uint64_t conn_fail;          // Δ connect() failures (errno ∉ {EINPROGRESS,EALREADY})
    uint64_t connect_attempts;   // Δ all connect() calls (incl. pending)
    uint64_t dns_calls;          // Δ getaddrinfo() calls (no bytes)
    uint64_t in_bytes;           // Δ received bytes
    uint64_t out_bytes;          // Δ sent bytes
    float    in_kbps;            // in_bytes / Δt / 1024
    float    out_kbps;           // out_bytes / Δt / 1024
};

#pragma pack(pop)

// Zeroes the instantaneous per-client flow fields when the client has been
// silent for at least `threshold_ms`. The engine's netchan.flow[].kbytespersec
// freezes at its last value once packets stop arriving, so a client awaiting a
// timeout would otherwise keep emitting a flat, stale non-zero rate. Header-only
// and engine-free so this decision stays host-testable; call it after `idle_ms`
// has been filled.
inline void gate_idle_flow(ClientSample& s, uint32_t threshold_ms) noexcept {
    if (s.idle_ms >= threshold_ms) {
        s.flow_in_kbps = 0.0f;
        s.flow_out_kbps = 0.0f;
        s.flow_in_avg = 0.0f;
        s.flow_out_avg = 0.0f;
    }
}

// ---- size/ABI assertions (packed layout, no incidental padding) ----
static_assert(sizeof(ClientSample) == 247, "ClientSample layout drift");
static_assert(sizeof(ClientSample) < 256, "ClientSample exceeds budget (QAS-4)");
static_assert(sizeof(ClientSnapshot) == 22, "ClientSnapshot layout drift");
static_assert(sizeof(NetchanSnapshot) == 62, "NetchanSnapshot layout drift");
static_assert(sizeof(NeighborsInfo) == 14, "NeighborsInfo layout drift");
static_assert(sizeof(ServerSample) == 145, "ServerSample layout drift");
static_assert(sizeof(DropRecord) == 16413, "DropRecord layout drift");
static_assert(sizeof(RejectRecord) == 296, "RejectRecord layout drift");
static_assert(sizeof(ConnectRecord) == 169, "ConnectRecord layout drift");
static_assert(sizeof(NetCmdRecord) == 14, "NetCmdRecord layout drift");
static_assert(sizeof(MassDropRecord) == 174, "MassDropRecord layout drift");
static_assert(sizeof(PacketSizes) == 16, "PacketSizes layout drift");
static_assert(sizeof(PacketStats) == 104, "PacketStats layout drift");
static_assert(sizeof(HitchRecord) == 12, "HitchRecord layout drift");
static_assert(sizeof(MapChangeRecord) == 72, "MapChangeRecord layout drift");
static_assert(sizeof(VoiceRecord) == 13, "VoiceRecord layout drift");
static_assert(sizeof(ShutdownRecord) == 24, "ShutdownRecord layout drift");
static_assert(sizeof(CrashRecord) == 12, "CrashRecord layout drift");
static_assert(sizeof(ModnetRecord) == 192, "ModnetRecord layout drift");

} // namespace hldsmon

#endif // HLDSMON_METRICS_H
