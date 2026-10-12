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
#include <cstring>
#include <span>
#include <utility>

namespace hldsmon {

// Maximum number of preceding client samples embedded in a drop-record window.
// Kept as a compile-time bound so DropRecord stays a trivially-copyable POD.
constexpr size_t HLDSMON_DROP_WINDOW_MAX = 64;

// Number of entries in P1 `packet.top_ips` (design §26.3 п.3, EN-4): bounded,
// compile-time constant so PacketStats stays a trivially-copyable POD.
constexpr size_t HLDSMON_TOP_IPS = 3;

#pragma pack(push, 1)

// Bounded per-IP packet table entry (open addressing; `ip == 0` marks a free
// slot). Lives here with the other PODs so the top-N selection below stays
// host-testable (design §26.3 п.3, EN-4).
struct IpEntry {
    uint32_t ip;
    uint32_t count;
};

// One entry of P1 `packet.top_ips` (EV-5): dotted IPv4 + occurrence count.
struct IpCount {
    char     ip[64];
    uint32_t count;
};

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
    // ---- inter-packet jitter over the sampling interval (design §24.3 п.1).
    //      min/avg/max inter-arrival in ms, integer-clamped; all three are 0
    //      when no delta was observed (first packet since connect / no data). ----
    uint16_t jit_min_ms;
    uint16_t jit_avg_ms;
    uint16_t jit_max_ms;

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
    // ---- edict churn (design §26.3 п.5, EN-5): signed delta of
    //      pfnNumberOfEntities() over the 1 Hz interval; the first interval
    //      after attach/mapchange clears the present bit (JSON null). ----
    int32_t  edicts_churn;
    uint8_t  edicts_churn_present;
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

// Tracked numeric server config (design §22, config-context stage). `present`
// marks which cvar pointers resolved at bootstrap (CFG-2): an absent cvar is
// emitted as the JSON literal `null` and its value slot is left zero, so the
// whole POD can be memset before filling (CFG-5) and compared bitwise.
// Scoped bitmask: combine with the constexpr operators below; convert to the
// uint16_t storage fields with std::to_underlying at the integer boundary.
enum class ConfigPresent : uint16_t {
    kCfgSysTicrate      = 1u << 0,
    kCfgHostFramerate   = 1u << 1,
    kCfgSvTimeout       = 1u << 2,
    kCfgSvMaxrate       = 1u << 3,
    kCfgSvMinrate       = 1u << 4,
    kCfgSvMaxupdaterate = 1u << 5,
    kCfgSvMinupdaterate = 1u << 6,
    kCfgSvMaxcmdrate    = 1u << 7,
    kCfgSvMincmdrate    = 1u << 8,
};

constexpr ConfigPresent operator|(ConfigPresent a, ConfigPresent b) noexcept {
    return static_cast<ConfigPresent>(std::to_underlying(a) |
                                      std::to_underlying(b));
}

constexpr ConfigPresent operator&(ConfigPresent a, ConfigPresent b) noexcept {
    return static_cast<ConfigPresent>(std::to_underlying(a) &
                                      std::to_underlying(b));
}

constexpr ConfigPresent& operator|=(ConfigPresent& a, ConfigPresent b) noexcept {
    a = a | b;
    return a;
}

constexpr ConfigPresent operator~(ConfigPresent a) noexcept {
    return static_cast<ConfigPresent>(~std::to_underlying(a));
}

struct ConfigSnapshot {
    uint16_t present;          // bitmask of kCfg* presence bits
    float    sys_ticrate;
    float    host_framerate;
    float    sv_timeout;
    float    sv_maxrate;
    float    sv_minrate;
    float    sv_maxupdaterate;
    float    sv_minupdaterate;
    float    sv_maxcmdrate;
    float    sv_mincmdrate;
};

// Boot/change configuration context — P1 `config` (design §22.3).
struct ConfigRecord {
    double         t;
    int32_t        rehlds_major;
    int32_t        rehlds_minor;
    char           map[64];
    uint8_t        maxclients;
    ConfigSnapshot config;
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
    char           detector[32];      // rehlds_security detector or "" (EN-1/EN-3)
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
    ConfigSnapshot config;            // live rate/timeout/tickrate snapshot (§22.3 п.3)
    ClientSample   window[HLDSMON_DROP_WINDOW_MAX];
    uint16_t       window_count;      // valid entries in window
    ServerSample   server;
    NeighborsInfo  neighbors;
};

// Presence bits for the connect/load timing fields (stage connect-timings,
// design §25.3; CTP-1/CT-4). uint32 ms cannot express "not measured", so each
// record carries a presence mask and a clear bit serializes as JSON `null`.
// Scoped bitmask: combine with the constexpr operators below; convert to the
// uint8_t storage fields with std::to_underlying at the integer boundary.
enum class TimingPresent : uint8_t {
    kTimingStageMs = 1u << 0,  // reject.stage_ms — validator duration
    kTimingTotalMs = 1u << 1,  // reject.total_ms — SV_ConnectClient -> reject
    kTimingAuthMs  = 1u << 2,  // connect.auth_ms — SV_ConnectClient -> success
    kTimingSteamMs = 1u << 3,  // connect.steam_ms — Steam stage duration
    kTimingLoadMs  = 1u << 4,  // mapchange.load_ms — SV_Spawn_f -> ready
};

constexpr TimingPresent operator|(TimingPresent a, TimingPresent b) noexcept {
    return static_cast<TimingPresent>(std::to_underlying(a) |
                                      std::to_underlying(b));
}

constexpr TimingPresent operator&(TimingPresent a, TimingPresent b) noexcept {
    return static_cast<TimingPresent>(std::to_underlying(a) &
                                      std::to_underlying(b));
}

constexpr TimingPresent& operator|=(TimingPresent& a, TimingPresent b) noexcept {
    a = a | b;
    return a;
}

// Rejected connection — P1 `reject` (+ connect-timings extension).
struct RejectRecord {
    double t;
    char   ip[64];
    char   steamid[32];
    char   stage[64];    // which validator rejected
    char   detail[128];
    uint32_t stage_ms;      // duration around the rejecting callNext (kTimingStageMs)
    uint32_t total_ms;      // SV_ConnectClient entry -> reject (kTimingTotalMs)
    uint8_t  timing_present; // bitmask of kTiming* presence bits
};

// Successful connect — P1 `connect` (+ connect-timings extension).
struct ConnectRecord {
    double  t;
    uint8_t id;
    char    ip[64];
    char    steamid[32];
    char    name[64];
    uint32_t auth_ms;        // SV_ConnectClient entry -> ClientConnected (kTimingAuthMs)
    uint32_t steam_ms;       // measured Steam_NotifyClientConnect stage (kTimingSteamMs)
    uint8_t  timing_present; // bitmask of kTiming* presence bits
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
    char        top_ip[64];               // top-1, kept for schema compatibility
    IpCount     top_ips[HLDSMON_TOP_IPS]; // top-N (EN-4); zero count = empty slot
};

// Frame hitch — P1 `hitch`.
struct HitchRecord {
    double   t;
    uint32_t duration_ms;
};

// Map change marker — P1 `mapchange` (+ connect-timings extension).
struct MapChangeRecord {
    double   t;
    char     map[64];
    uint32_t load_ms;       // SV_Spawn_f -> world ready (kTimingLoadMs)
    uint8_t  load_present;  // 1 when load_ms is measured, 0 => JSON null
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

// Collection-degradation event (Stage 9, design §26.3 п.4, EN-P1) — P1
// `degraded`. Emitted by the 1 Hz sampler only while the log is enabled; the
// value is the Δ of LogWriter::error_count() over the interval.
struct DegradedRecord {
    double   t;
    uint64_t error_count;
};

// Retention event (Stage 9, design §26.3 п.4) — P1 `retention`. Emitted by the
// 1 Hz sampler when prune() removed files; `removed` is the Δ over the interval.
struct RetentionRecord {
    double   t;
    uint32_t removed;
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

// Host/kernel best-effort sample (Stage 6, design §23 / P1 extension `host`).
// Cumulative sources are emitted as Δ over the interval; a field whose
// `present` bit is clear is JSON null (unavailable OR no baseline yet — the
// first sample, HM-2). A field with its `present` bit set but a `caveat` bit
// is a present value not attributable to this server (caveated != null, M4).
// Mask bits are defined in host_metrics.h (kHost* / kCaveat*).
struct HostRecord {
    double   t;                  // wall-clock epoch seconds
    uint64_t udp_rcvbuf_errors;  // Δ /proc/net/snmp Udp RcvbufErrors
    uint64_t udp_in_errors;      // Δ /proc/net/snmp Udp InErrors
    uint64_t udp_in_csum_errors; // Δ /proc/net/snmp Udp InCsumErrors
    uint64_t net_rx_dropped;     // Δ /proc/net/dev rx drop (Σ interfaces)
    uint64_t net_rx_missed;      // Δ /proc/net/dev rx missed (if reported)
    uint64_t net_rx_overruns;    // Δ /proc/net/dev rx overruns (if reported)
    float    steal_pct;          // Δsteal/Δtotal*100 (percent)
    uint64_t io_read_bytes;      // Δ /proc/self/io read_bytes
    uint64_t io_write_bytes;     // Δ /proc/self/io write_bytes
    uint64_t disk_bavail_mb;     // statvfs f_bavail, MiB (instantaneous)
    uint16_t present_mask;       // kHost* bits of available fields
    uint16_t caveat_mask;        // kCaveat* bits (per source)
};

#pragma pack(pop)

// Zeroes the instantaneous per-client flow fields when the client has been
// silent for at least `threshold_ms`. The engine's netchan.flow[].kbytespersec
// freezes at its last value once packets stop arriving, so a client awaiting a
// timeout would otherwise keep emitting a flat, stale non-zero rate. Header-only
// and engine-free so this decision stays host-testable; call it after `idle_ms`
// has been filled.
inline constexpr void gate_idle_flow(ClientSample& s, uint32_t threshold_ms) noexcept {
    if (s.idle_ms >= threshold_ms) {
        s.flow_in_kbps = 0.0f;
        s.flow_out_kbps = 0.0f;
        s.flow_in_avg = 0.0f;
        s.flow_out_avg = 0.0f;
    }
}

// ReHLDS client_t::latency is a float in SECONDS (design §21.2). Convert it to
// the rounded, clamped millisecond value carried by the uint16 ping fields.
// Non-positive / non-finite latency (<=0, NaN, -inf) yields 0 ("not measured");
// values at or above the uint16 ceiling saturate at 0xFFFF. Header-only and
// engine-free so the conversion stays host-testable.
[[nodiscard]] inline constexpr uint16_t ping_ms_from_latency(float latency_s) noexcept {
    const double ms = static_cast<double>(latency_s) * 1000.0;
    if (!(ms > 0.0)) {         // catches <=0, -inf and NaN without <cmath>
        return 0;
    }
    const double rounded = ms + 0.5;  // round half up
    if (rounded >= 65535.0) {         // clamp the ROUNDED value, not `ms`
        return 0xFFFFu;
    }
    return static_cast<uint16_t>(rounded);
}

// Convert a stage duration in seconds (an engine GetRealTime delta) to the
// rounded uint32 millisecond value carried by the connect-timing fields
// (design §25.3 п.1, CTP-4). Returns false for a non-positive or non-finite
// duration — a missing marker, a short-circuited hook, or a clock jump — so
// the caller clears the presence bit (JSON `null`, CT-6/CT-8) instead of
// emitting garbage; otherwise stores the rounded, uint32-clamped value and
// returns true. Header-only and engine-free so the guard stays host-testable.
[[nodiscard]] inline constexpr bool timing_delta_ms(double delta_s, uint32_t& out_ms) noexcept {
    if (!(delta_s > 0.0)) {    // catches <=0, -inf and NaN without <cmath>
        return false;
    }
    const double ms = delta_s * 1000.0;
    if (ms >= 4294967295.0) {
        out_ms = 0xFFFFFFFFu;
        return true;
    }
    out_ms = static_cast<uint32_t>(ms + 0.5);  // round half up
    return true;
}

// ---- inter-packet jitter accumulator (design §24.3 п.1, stage 7) ----
// The per-packet path accumulates one client's inter-arrival deltas for the
// current sampling interval; sample_clients finalizes and then resets them.
// Integer-only, O(1), no allocation. The emitted uint16 triple is min/avg/max
// in ms; count==0 (no delta observed, e.g. the client's first packet since
// connect) finalizes to all zeros meaning "no data" (L1).

inline constexpr void jitter_reset(uint32_t& jit_min, uint32_t& jit_sum, uint32_t& jit_max,
                                   uint32_t& jit_count) noexcept {
    jit_min = 0;
    jit_sum = 0;
    jit_max = 0;
    jit_count = 0;
}

// Convert an inter-arrival clock delta (engine GetRealTime seconds) to whole
// milliseconds. Returns false for a non-positive or non-finite delta, which
// guards clock jumps and the client's first tracked packet (M4); otherwise
// stores the rounded, uint16-clamped value and returns true.
[[nodiscard]] inline constexpr bool jitter_delta_ms(double now_real, double prev_real, uint32_t& out_ms) noexcept {
    if (!(now_real > prev_real)) {   // catches <=0 and NaN without <cmath>
        return false;
    }
    const double ms = (now_real - prev_real) * 1000.0;
    out_ms = ms >= 65535.0 ? 65535u : static_cast<uint32_t>(ms + 0.5);
    return true;
}

inline constexpr void jitter_add_ms(uint32_t& jit_min, uint32_t& jit_sum, uint32_t& jit_max,
                                    uint32_t& jit_count, uint32_t delta_ms) noexcept {
    if (jit_count == 0 || delta_ms < jit_min) {
        jit_min = delta_ms;
    }
    if (delta_ms > jit_max) {
        jit_max = delta_ms;
    }
    const uint32_t room = 0xFFFFFFFFu - jit_sum;
    jit_sum = delta_ms > room ? 0xFFFFFFFFu : jit_sum + delta_ms;
    ++jit_count;
}

// min / avg (= sum/count) / max in ms, each clamped to uint16. count==0
// finalizes to all zeros ("no data").
inline constexpr void jitter_finalize(uint32_t jit_min, uint32_t jit_sum, uint32_t jit_max,
                                      uint32_t jit_count, uint16_t& out_min, uint16_t& out_avg,
                                      uint16_t& out_max) noexcept {
    if (jit_count == 0) {
        out_min = 0;
        out_avg = 0;
        out_max = 0;
        return;
    }
    const uint32_t avg = jit_sum / jit_count;
    out_min = jit_min > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(jit_min);
    out_avg = avg > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(avg);
    out_max = jit_max > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(jit_max);
}

// POD comparison of two config snapshots (design §22.3 п.4, CFG-5): bitwise
// over the packed struct, no allocation. Callers memset a snapshot before
// filling it, so absent fields hold stable zeroes and cannot cause a false
// "changed". Header-only and engine-free for host tests.
[[nodiscard]] inline bool config_changed(const ConfigSnapshot& a, const ConfigSnapshot& b) noexcept {
    return std::memcmp(&a, &b, sizeof(ConfigSnapshot)) != 0;
}

// ---- Stage 9 (`event-enrich`) host-testable helpers ----

// Best-effort `drop.detector` classification (design §26.3 п.2, EN-1/EN-3/EV-1):
// substring match against the rehlds_security drop reasons (captured from
// ReHLDS rehlds/rehlds_security.cpp: "Kicked/Banned for move commands flooding
// (burst|Avg)", "... string commands flooding ...", "Kicked/Banned for dlfile
// flooding", "Kicked/Banned for speedhack"). A miss yields "" and never affects
// the drop. Not covered by design: slowmo (time-deceleration abuse) and
// detectors that throttle/ignore without dropping. Engine-free for host tests.
[[nodiscard]] inline const char* detector_name(const char* reason) noexcept {
    if (reason == nullptr) {
        return "";
    }
    if (std::strstr(reason, "move commands flooding") != nullptr) {
        return "movecmd";
    }
    if (std::strstr(reason, "string commands flooding") != nullptr) {
        return "stringcmd";
    }
    if (std::strstr(reason, "dlfile flooding") != nullptr) {
        return "dlfile";
    }
    if (std::strstr(reason, "speedhack") != nullptr) {
        return "speedhack";
    }
    return "";
}

// Fills `out` with `detector_name(reason)`, bounded and NUL-terminated.
inline void classify_detector(const char* reason, std::span<char> out) noexcept {
    if (out.empty()) {
        return;
    }
    const char* det = detector_name(reason);
    size_t i = 0;
    for (; det[i] != '\0' && i + 1 < out.size(); ++i) {
        out[i] = det[i];
    }
    out[i] = '\0';
}

// Dotted-quad rendering of a packed host-order IPv4 (the engine-path format_ip
// in plugin.h needs netadr_t; this mirror keeps metrics.h engine-free).
inline constexpr void format_ip4_packed(uint32_t ip, std::span<char> out) noexcept {
    if (out.empty()) {
        return;
    }
    size_t pos = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint32_t v = (ip >> shift) & 0xFFu;
        char tmp[3];
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
        if (shift != 0 && pos + 1 < out.size()) {
            out[pos++] = '.';
        }
    }
    out[pos < out.size() ? pos : out.size() - 1] = '\0';
}

// One linear pass over the bounded per-IP table (`entries[0..n)`, `ip == 0` or
// `count == 0` marks an empty slot): selects the top HLDSMON_TOP_IPS entries by
// count into `out`, descending, ties keeping the earlier table position.
// O(n * HLDSMON_TOP_IPS) worst case, no allocation; runs off the hot path in the
// 1 Hz packet aggregation (design §26.3 п.3). Returns the number valid (0..3).
[[nodiscard]] inline constexpr size_t select_top_ips(const IpEntry* entries, uint32_t n,
                                                     IpCount out[HLDSMON_TOP_IPS]) noexcept {
    size_t used = 0;
    if (entries == nullptr) {
        return 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t packed = entries[i].ip;
        const uint32_t count = entries[i].count;
        if (packed == 0u || count == 0u) {
            continue;
        }
        if (used == HLDSMON_TOP_IPS && count <= out[HLDSMON_TOP_IPS - 1].count) {
            continue;
        }
        size_t pos = used < HLDSMON_TOP_IPS ? used : HLDSMON_TOP_IPS - 1;
        while (pos > 0 && out[pos - 1].count < count) {
            out[pos] = out[pos - 1];
            --pos;
        }
        format_ip4_packed(packed, out[pos].ip);
        out[pos].count = count;
        if (used < HLDSMON_TOP_IPS) {
            ++used;
        }
    }
    return used;
}

// ---- size/ABI assertions (packed layout, no incidental padding) ----
static_assert(sizeof(ConfigSnapshot) == 38, "ConfigSnapshot layout drift");
static_assert(sizeof(ConfigRecord) == 119, "ConfigRecord layout drift");
static_assert(sizeof(ClientSample) == 253, "ClientSample layout drift");
static_assert(sizeof(ClientSample) < 256, "ClientSample exceeds budget (QAS-4)");
static_assert(sizeof(ClientSnapshot) == 22, "ClientSnapshot layout drift");
static_assert(sizeof(NetchanSnapshot) == 62, "NetchanSnapshot layout drift");
static_assert(sizeof(NeighborsInfo) == 14, "NeighborsInfo layout drift");
static_assert(sizeof(ServerSample) == 150, "ServerSample layout drift");
static_assert(sizeof(DropRecord) == 16872, "DropRecord layout drift");
static_assert(sizeof(RejectRecord) == 305, "RejectRecord layout drift");
static_assert(sizeof(ConnectRecord) == 178, "ConnectRecord layout drift");
static_assert(sizeof(NetCmdRecord) == 14, "NetCmdRecord layout drift");
static_assert(sizeof(MassDropRecord) == 174, "MassDropRecord layout drift");
static_assert(sizeof(PacketSizes) == 16, "PacketSizes layout drift");
static_assert(sizeof(PacketStats) == 308, "PacketStats layout drift");
static_assert(sizeof(IpCount) == 68, "IpCount layout drift");
static_assert(sizeof(HitchRecord) == 12, "HitchRecord layout drift");
static_assert(sizeof(MapChangeRecord) == 77, "MapChangeRecord layout drift");
static_assert(sizeof(VoiceRecord) == 13, "VoiceRecord layout drift");
static_assert(sizeof(ShutdownRecord) == 24, "ShutdownRecord layout drift");
static_assert(sizeof(CrashRecord) == 12, "CrashRecord layout drift");
static_assert(sizeof(DegradedRecord) == 16, "DegradedRecord layout drift");
static_assert(sizeof(RetentionRecord) == 12, "RetentionRecord layout drift");
static_assert(sizeof(ModnetRecord) == 192, "ModnetRecord layout drift");
static_assert(sizeof(HostRecord) == 88, "HostRecord layout drift");

} // namespace hldsmon

#endif // HLDSMON_METRICS_H
