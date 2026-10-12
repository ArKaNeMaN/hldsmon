// HldsMonitoring — engine-free host/kernel telemetry core (Stage 6, design §23).
//
// POSIX only — no engine/metamod headers — so the parsers, the pure environment
// detector and the bounded file reader link into the host tests through
// CORE_SRCS (design §23.6 M1, like modnet_got.cpp / retention.{h,cpp}).
//
// Passive and best-effort (INV-1): reads /proc files and statvfs(2); never
// writes, never allocates (INV-2), server thread only (INV-3). A missing or
// unreadable source clears the field's `present` bit (JSON null), it is never
// reported as a zero ("flag, don't attribute", N11/QAS-H3). A value that is
// present but not attributable to this server keeps its value and sets a
// `caveat` bit (design M4: caveated != null).

#ifndef HLDSMON_HOST_METRICS_H
#define HLDSMON_HOST_METRICS_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "metrics.h"

namespace hldsmon {

// Fixed buffer for one bounded /proc read: the reader drains to EOF or to a
// full buffer, and a full buffer marks the read truncated (L3).
constexpr size_t kHostReadCap = 16384;

// ---- `present` mask bits — one per emitted `host` field (HM-3) ----
// Scoped bitmask: combine the bits with the constexpr operators below and
// convert to the uint16_t storage fields with std::to_underlying at the
// integer boundary.
enum class HostPresent : uint16_t {
    kHostUdpRcvbufErrors = 1u << 0,  // /proc/net/snmp  Udp RcvbufErrors
    kHostUdpInErrors     = 1u << 1,  // /proc/net/snmp  Udp InErrors
    kHostUdpInCsumErrors = 1u << 2,  // /proc/net/snmp  Udp InCsumErrors (HM-6)
    kHostNetRxDropped    = 1u << 3,  // /proc/net/dev   rx drop
    kHostNetRxMissed     = 1u << 4,  // /proc/net/dev   rx missed (if reported)
    kHostNetRxOverruns   = 1u << 5,  // /proc/net/dev   rx overruns (if reported)
    kHostStealPct        = 1u << 6,  // /proc/stat      cpu steal, as %
    kHostIoReadBytes     = 1u << 7,  // /proc/self/io   read_bytes
    kHostIoWriteBytes    = 1u << 8,  // /proc/self/io   write_bytes
    kHostDiskBavailMb    = 1u << 9,  // statvfs         f_bavail, MiB
};

constexpr HostPresent operator|(HostPresent a, HostPresent b) noexcept {
    return static_cast<HostPresent>(std::to_underlying(a) |
                                    std::to_underlying(b));
}

constexpr HostPresent operator&(HostPresent a, HostPresent b) noexcept {
    return static_cast<HostPresent>(std::to_underlying(a) &
                                    std::to_underlying(b));
}

constexpr HostPresent& operator|=(HostPresent& a, HostPresent b) noexcept {
    a = a | b;
    return a;
}

// ---- `caveat` mask bits — per source / uncertainty (HM-3, M4) ----
enum class HostCaveat : uint16_t {
    kCaveatUdpNetns  = 1u << 0,  // /proc/net/*: netns may be shared (host/neighbour data)
    kCaveatNetDev    = 1u << 1,  // NIC counters are driver-dependent: 0 = "not reported" (M2)
    kCaveatSteal     = 1u << 2,  // steal zeroed by lxcfs/OpenVZ, not authoritative
    kCaveatIo        = 1u << 3,  // /proc/self/io: task-io-accounting/semantics/non-atomic
    kCaveatDisk      = 1u << 4,  // statvfs: thin provisioning / quotas overestimate
    kCaveatTruncated = 1u << 5,  // a source read was truncated this tick (L3)
};

constexpr HostCaveat operator|(HostCaveat a, HostCaveat b) noexcept {
    return static_cast<HostCaveat>(std::to_underlying(a) |
                                   std::to_underlying(b));
}

constexpr HostCaveat operator&(HostCaveat a, HostCaveat b) noexcept {
    return static_cast<HostCaveat>(std::to_underlying(a) &
                                   std::to_underlying(b));
}

constexpr HostCaveat& operator|=(HostCaveat& a, HostCaveat b) noexcept {
    a = a | b;
    return a;
}

// One tick's cumulative snapshot: raw counters (zero when a field is absent —
// the `present` bit says which were actually found) plus the internal
// `/proc/stat` totals used for the steal fraction. Parsers set kHost* bits by
// field name only (QAS-H1); unknown or missing names leave the bit clear.
struct HostSnapshot {
    uint16_t present;                // kHost* bits of parsed fields
    uint64_t udp_rcvbuf_errors;
    uint64_t udp_in_errors;
    uint64_t udp_in_csum_errors;
    uint64_t net_rx_dropped;
    uint64_t net_rx_missed;
    uint64_t net_rx_overruns;
    uint64_t stat_steal_jiffies;     // internal (numerator)
    uint64_t stat_total_jiffies;     // internal (denominator)
    uint64_t io_read_bytes;
    uint64_t io_write_bytes;
    uint64_t disk_bavail_mb;         // instantaneous, filled by the caller
};

// ---- parsers (pure; parse by header name, never by fixed position) ----
void parse_snmp_udp(const char* buf, HostSnapshot& out) noexcept;
void parse_net_dev(const char* buf, HostSnapshot& out) noexcept;
void parse_stat(const char* buf, HostSnapshot& out) noexcept;
void parse_self_io(const char* buf, HostSnapshot& out) noexcept;

// ---- wrap/reset-safe delta (HM-1) ----
// True with *out = cur - prev when cur >= prev. When cur < prev the counter
// reset (netns recreation / process restart) or wrapped beyond a sane horizon:
// false, *out untouched — the caller re-baselines and emits null instead of a
// giant unsigned delta.
[[nodiscard]] bool delta_u64(uint64_t prev, uint64_t cur, uint64_t* out) noexcept;

// ---- environment detection (design §23.3 п.2, H1) ----
// Tri-state probe result. kUnknown means "could not determine" and must be
// treated conservatively (caveat), never as health (QAS-H8).
enum class Tri : uint8_t { kNo = 0, kYes = 1, kUnknown = 2 };

// Pure inputs of the detector; filled by probe_env() or hand-built in tests.
struct EnvInputs {
    Tri container;       // containerization signs (cgroup/lxcfs/.dockerenv/container=)
    Tri netns_private;   // self netns inode differs from PID1's (positive separation)
};

struct EnvVerdict {
    uint16_t caveat;                 // kCaveat* bits for netns/netdev/steal
    bool     netns_authoritative;    // /proc/net/* may be attributed to this server
};

// Pure: a dedicated host (bare metal/KVM) is authoritative; a container is
// authoritative only with a positively confirmed private netns; an unknown or
// unconfirmed environment is caveated (safe direction). The unsound
// "self == PID1 inode => shared" rule is NOT used (H1).
EnvVerdict detect_env(const EnvInputs& in) noexcept;

// POSIX probe collecting EnvInputs: cheap containerization signs plus a
// /proc/self/ns/net vs /proc/1/ns/net inode comparison. EACCES/ENOENT yields
// kUnknown, never a crash (QAS-H8).
EnvInputs probe_env() noexcept;

// ---- bounded POSIX I/O ----
// Reads up to buf.size()-1 bytes of `path` into `buf` (NUL-terminated),
// retrying EINTR and draining to EOF or a full buffer. True on success with
// *out_len set (bytes stored, excluding NUL) and *out_truncated = more data
// remained (partial read => caveated, L3). False on open/read error (metric
// unavailable, QAS-H3) with *out_len = 0.
bool read_file(const char* path, std::span<char> buf, size_t* out_len,
               bool* out_truncated) noexcept;

// statvfs(3): f_bavail * f_frsize in MiB for `dir` (M5 — the log directory,
// not "/"; f_bavail, not f_bfree). False when the call fails (=> null).
bool statvfs_bavail(const char* dir, uint64_t* out_bavail_mb) noexcept;

// ---- record composition (design §23.3 п.4, HM-1/HM-2) ----
// Composes one `host` record from the previous and current snapshots:
//   - has_prev == false (first sample) => every counter and `steal_pct` is
//     absent (present bit clear => JSON null), never 0 (QAS-H7);
//   - cur < prev (reset/wrap) => that counter alone re-baselines to null;
//   - steal_pct = Δsteal / Δtotal * 100, present only when Δtotal > 0;
//   - instantaneous fields (disk_bavail_mb) are copied whenever present in
//     `cur` (available on the first sample too).
// `caveat_mask` comes from detect_env(); `truncated` adds kCaveatTruncated.
// Pure, no allocation.
void build_host_record(const HostSnapshot& prev, bool has_prev,
                       const HostSnapshot& cur, uint16_t caveat_mask,
                       bool truncated, double t, HostRecord& out) noexcept;

} // namespace hldsmon

#endif // HLDSMON_HOST_METRICS_H
