// HldsMonitoring — engine-free host/kernel telemetry core (Stage 6).
//
// See host_metrics.h for the contract. Implementation notes:
//  * every parser maps field NAMES found in the file header to positions and
//    then reads the values at those positions — a missing or renamed field
//    simply keeps its present bit clear (QAS-H1);
//  * /proc/stat has no header line; its columns are documented by proc_stat(5)
//    and steal is a trailing column that older kernels omit (=> absent);
//  * all I/O is bounded, retries EINTR and never allocates (INV-2).

#include "host_metrics.h"

#include <cerrno>
#include <cstring>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace hldsmon {
namespace {

bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

// Next whitespace-separated token on the current line; false at end of line.
bool next_token(const char*& p, const char* end, const char*& tb,
                const char*& te) noexcept {
    while (p < end && is_space(*p)) {
        ++p;
    }
    if (p >= end || *p == '\n') {
        return false;
    }
    tb = p;
    while (p < end && !is_space(*p) && *p != '\n') {
        ++p;
    }
    te = p;
    return true;
}

// Line slice [*line, *line_end) starting at `p`; advances to the next line.
const char* next_line(const char* p, const char* end,
                      const char** line_end) noexcept {
    const char* e = p;
    while (e < end && *e != '\n') {
        ++e;
    }
    *line_end = e;
    return p;
}

bool token_eq(const char* b, const char* e, const char* name) noexcept {
    const size_t n = std::strlen(name);
    return static_cast<size_t>(e - b) == n && std::memcmp(b, name, n) == 0;
}

bool parse_u64_token(const char* b, const char* e, uint64_t* out) noexcept {
    while (b < e && is_space(*b)) {
        ++b;
    }
    if (b >= e) {
        return false;
    }
    uint64_t v = 0;
    bool any = false;
    while (b < e && *b >= '0' && *b <= '9') {
        v = v * 10u + static_cast<uint64_t>(*b - '0');
        ++b;
        any = true;
    }
    if (!any) {
        return false;
    }
    *out = v;
    return true;
}

// Raw-byte substring search (used on /proc files that may contain NUL bytes,
// e.g. /proc/1/environ). No allocation.
bool buf_contains(const char* buf, size_t len, const char* needle) noexcept {
    if (buf == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }
    const size_t n = std::strlen(needle);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i + n <= len; ++i) {
        if (std::memcmp(buf + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

// Counter fields shared by HostSnapshot (prev/cur) and HostRecord (out).
struct DeltaField {
    uint16_t bit;
    uint64_t HostSnapshot::*snap;
    uint64_t HostRecord::*out;
};

const DeltaField kDeltaFields[] = {
    {std::to_underlying(HostPresent::kHostUdpRcvbufErrors), &HostSnapshot::udp_rcvbuf_errors,
     &HostRecord::udp_rcvbuf_errors},
    {std::to_underlying(HostPresent::kHostUdpInErrors), &HostSnapshot::udp_in_errors, &HostRecord::udp_in_errors},
    {std::to_underlying(HostPresent::kHostUdpInCsumErrors), &HostSnapshot::udp_in_csum_errors,
     &HostRecord::udp_in_csum_errors},
    {std::to_underlying(HostPresent::kHostNetRxDropped), &HostSnapshot::net_rx_dropped,
     &HostRecord::net_rx_dropped},
    {std::to_underlying(HostPresent::kHostNetRxMissed), &HostSnapshot::net_rx_missed,
     &HostRecord::net_rx_missed},
    {std::to_underlying(HostPresent::kHostNetRxOverruns), &HostSnapshot::net_rx_overruns,
     &HostRecord::net_rx_overruns},
    {std::to_underlying(HostPresent::kHostIoReadBytes), &HostSnapshot::io_read_bytes, &HostRecord::io_read_bytes},
    {std::to_underlying(HostPresent::kHostIoWriteBytes), &HostSnapshot::io_write_bytes,
     &HostRecord::io_write_bytes},
};

} // namespace

// ---- parsers -------------------------------------------------------------

void parse_snmp_udp(const char* buf, HostSnapshot& out) noexcept {
    if (buf == nullptr) {
        return;
    }
    const char* end = buf + std::strlen(buf);
    const char* p = buf;
    int idx_in_errors = -1;
    int idx_rcvbuf = -1;
    int idx_csum = -1;
    bool got_header = false;

    while (p < end) {
        const char* le = nullptr;
        const char* line = next_line(p, end, &le);
        p = (le < end) ? le + 1 : end;

        const char* q = line;
        const char* tb = nullptr;
        const char* te = nullptr;
        if (!next_token(q, le, tb, te)) {
            continue;  // blank line
        }
        if (!token_eq(tb, te, "Udp:")) {
            continue;  // Ip:/Icmp:/UdpLite: — not the UDP rows
        }
        if (!got_header) {
            // Header row: map wanted names to their token positions.
            int index = 0;
            while (next_token(q, le, tb, te)) {
                if (token_eq(tb, te, "InErrors")) {
                    idx_in_errors = index;
                } else if (token_eq(tb, te, "RcvbufErrors")) {
                    idx_rcvbuf = index;
                } else if (token_eq(tb, te, "InCsumErrors")) {
                    idx_csum = index;
                }
                ++index;
            }
            got_header = true;
        } else {
            // Values row immediately after the header.
            int index = 0;
            uint64_t v = 0;
            while (next_token(q, le, tb, te)) {
                if (index == idx_in_errors && parse_u64_token(tb, te, &v)) {
                    out.udp_in_errors = v;
                    out.present |= std::to_underlying(HostPresent::kHostUdpInErrors);
                } else if (index == idx_rcvbuf && parse_u64_token(tb, te, &v)) {
                    out.udp_rcvbuf_errors = v;
                    out.present |= std::to_underlying(HostPresent::kHostUdpRcvbufErrors);
                } else if (index == idx_csum && parse_u64_token(tb, te, &v)) {
                    out.udp_in_csum_errors = v;
                    out.present |= std::to_underlying(HostPresent::kHostUdpInCsumErrors);
                }
                ++index;
            }
            break;
        }
    }
}

void parse_net_dev(const char* buf, HostSnapshot& out) noexcept {
    if (buf == nullptr) {
        return;
    }
    const char* end = buf + std::strlen(buf);
    const char* p = buf;
    int idx_drop = -1;
    int idx_missed = -1;
    int idx_ovr = -1;
    bool got_header = false;

    while (p < end) {
        const char* le = nullptr;
        const char* line = next_line(p, end, &le);
        p = (le < end) ? le + 1 : end;

        const char* q = line;
        const char* tb = nullptr;
        const char* te = nullptr;
        if (!next_token(q, le, tb, te)) {
            continue;
        }

        if (!got_header) {
            if (!token_eq(tb, te, "face")) {
                continue;  // first line is "Inter-| Receive ..."
            }
            // Tokenize after "face" treating '|' as a separator. The receive
            // half is the first 8 columns of the Linux /proc/net/dev layout;
            // only names inside it are considered (M2: rx counters only).
            int col = 0;
            const char* c = q;
            while (c < le) {
                while (c < le && (is_space(*c) || *c == '|')) {
                    ++c;
                }
                const char* begin = c;
                while (c < le && !is_space(*c) && *c != '|') {
                    ++c;
                }
                if (begin == c) {
                    continue;
                }
                if (col < 8) {
                    if (token_eq(begin, c, "drop")) {
                        idx_drop = col;
                    } else if (token_eq(begin, c, "missed") ||
                               token_eq(begin, c, "missed_errors")) {
                        idx_missed = col;
                    } else if (token_eq(begin, c, "overruns") ||
                               token_eq(begin, c, "overrun")) {
                        idx_ovr = col;
                    }
                }
                ++col;
            }
            got_header = col >= 8;
            continue;
        }

        // Data row: "name: v0 v1 ..."; v(i) is the column i of the header
        // (the label token was consumed above, so the next tokens are v0...).
        int vi = 0;
        uint64_t v = 0;
        while (next_token(q, le, tb, te)) {
            if (parse_u64_token(tb, te, &v)) {
                if (vi == idx_drop) {
                    out.net_rx_dropped += v;
                    out.present |= std::to_underlying(HostPresent::kHostNetRxDropped);
                } else if (vi == idx_missed) {
                    out.net_rx_missed += v;
                    out.present |= std::to_underlying(HostPresent::kHostNetRxMissed);
                } else if (vi == idx_ovr) {
                    out.net_rx_overruns += v;
                    out.present |= std::to_underlying(HostPresent::kHostNetRxOverruns);
                }
            }
            ++vi;
        }
    }
}

void parse_stat(const char* buf, HostSnapshot& out) noexcept {
    if (buf == nullptr) {
        return;
    }
    const char* end = buf + std::strlen(buf);
    const char* p = buf;

    while (p < end) {
        const char* le = nullptr;
        const char* line = next_line(p, end, &le);
        p = (le < end) ? le + 1 : end;

        const char* q = line;
        const char* tb = nullptr;
        const char* te = nullptr;
        if (!next_token(q, le, tb, te)) {
            continue;
        }
        if (!token_eq(tb, te, "cpu")) {
            continue;  // "cpu0"..."cpuN" and other rows are not the aggregate
        }
        // Documented order (proc_stat(5)): user nice system idle iowait irq
        // softirq steal [guest guest_nice]. A kernel without steal (< 2.6.11)
        // yields fewer values and the field stays absent.
        uint64_t vals[8];
        int n = 0;
        uint64_t v = 0;
        while (n < 8 && next_token(q, le, tb, te)) {
            if (!parse_u64_token(tb, te, &v)) {
                break;
            }
            vals[n++] = v;
        }
        if (n >= 8) {
            uint64_t total = 0;
            for (int k = 0; k < 8; ++k) {
                total += vals[k];
            }
            out.stat_steal_jiffies = vals[7];
            out.stat_total_jiffies = total;
            out.present |= std::to_underlying(HostPresent::kHostStealPct);
        }
        break;
    }
}

void parse_self_io(const char* buf, HostSnapshot& out) noexcept {
    if (buf == nullptr) {
        return;
    }
    const char* end = buf + std::strlen(buf);
    const char* p = buf;

    while (p < end) {
        const char* le = nullptr;
        const char* line = next_line(p, end, &le);
        p = (le < end) ? le + 1 : end;

        const char* q = line;
        const char* tb = nullptr;
        const char* te = nullptr;
        if (!next_token(q, le, tb, te)) {
            continue;
        }
        uint16_t bit = 0;
        uint64_t HostSnapshot::*member = nullptr;
        if (token_eq(tb, te, "read_bytes:")) {
            bit = std::to_underlying(HostPresent::kHostIoReadBytes);
            member = &HostSnapshot::io_read_bytes;
        } else if (token_eq(tb, te, "write_bytes:")) {
            bit = std::to_underlying(HostPresent::kHostIoWriteBytes);
            member = &HostSnapshot::io_write_bytes;
        } else {
            continue;
        }
        uint64_t v = 0;
        if (next_token(q, le, tb, te) && parse_u64_token(tb, te, &v)) {
            out.*member = v;
            out.present |= bit;
        }
    }
}

// ---- delta / environment / composition ----------------------------------

bool delta_u64(uint64_t prev, uint64_t cur, uint64_t* out) noexcept {
    if (cur < prev) {
        return false;  // reset (netns/process restart) — never a modulo guess
    }
    if (out != nullptr) {
        *out = cur - prev;
    }
    return true;
}

EnvVerdict detect_env(const EnvInputs& in) noexcept {
    EnvVerdict v;
    v.caveat = 0;
    const bool dedicated = (in.container == Tri::kNo);
    const bool private_confirmed = (in.netns_private == Tri::kYes);
    // Dedicated host/KVM: /proc/net/* is authoritative. Container: only with a
    // positively confirmed private netns; anything else is caveated (H1).
    v.netns_authoritative =
        dedicated || (in.container == Tri::kYes && private_confirmed);
    if (!v.netns_authoritative) {
        v.caveat |= std::to_underlying(HostCaveat::kCaveatUdpNetns);
    }
    v.caveat |= std::to_underlying(HostCaveat::kCaveatNetDev);  // M2: driver-dependent zeroes
    if (!dedicated) {
        v.caveat |= std::to_underlying(HostCaveat::kCaveatSteal);  // lxcfs/OpenVZ zero the field; unknown => caveat
    }
    v.caveat |= std::to_underlying(HostCaveat::kCaveatIo);
    v.caveat |= std::to_underlying(HostCaveat::kCaveatDisk);
    return v;
}

EnvInputs probe_env() noexcept {
    EnvInputs in;
    in.container = Tri::kNo;
    in.netns_private = Tri::kUnknown;

    struct stat st;
    if (::stat("/.dockerenv", &st) == 0) {
        in.container = Tri::kYes;
    }

    char buf[kHostReadCap];
    size_t len = 0;
    bool truncated = false;

    if (read_file("/proc/mounts", buf, &len, &truncated)) {
        if (buf_contains(buf, len, "lxcfs")) {
            in.container = Tri::kYes;
        }
    } else {
        in.container = Tri::kUnknown;  // could not check an auxiliary sign
    }

    if (read_file("/proc/self/cgroup", buf, &len, &truncated)) {
        static const char* const kMarkers[] = {"docker", "lxc", "kubepods",
                                               "containerd", "libpod"};
        for (const char* marker : kMarkers) {
            if (buf_contains(buf, len, marker)) {
                in.container = Tri::kYes;
                break;
            }
        }
    } else {
        in.container = Tri::kUnknown;
    }

    // Best-effort: PID1's environment may be unreadable (ptrace/EACCES); that
    // is absence of evidence, not evidence of a container.
    if (read_file("/proc/1/environ", buf, &len, &truncated)) {
        if (buf_contains(buf, len, "container=")) {
            in.container = Tri::kYes;
        }
    }

    // Positive netns separation: our netns inode differs from PID1's. Equality
    // does NOT prove sharing (H1 — the unsound rule is not used); EACCES/ENOENT
    // yields kUnknown and thus a caveat (QAS-H8).
    struct stat self_ns;
    struct stat pid1_ns;
    if (::stat("/proc/self/ns/net", &self_ns) != 0 ||
        ::stat("/proc/1/ns/net", &pid1_ns) != 0) {
        in.netns_private = Tri::kUnknown;
    } else if (self_ns.st_ino == pid1_ns.st_ino &&
               self_ns.st_dev == pid1_ns.st_dev) {
        in.netns_private = Tri::kNo;
    } else {
        in.netns_private = Tri::kYes;
    }
    return in;
}

// ---- bounded I/O ---------------------------------------------------------

bool read_file(const char* path, std::span<char> buf, size_t* out_len,
               bool* out_truncated) noexcept {
    if (out_len != nullptr) {
        *out_len = 0;
    }
    if (out_truncated != nullptr) {
        *out_truncated = false;
    }
    if (path == nullptr || buf.empty() || out_len == nullptr) {
        return false;
    }
    const size_t cap = buf.size();

    int fd = -1;
    do {
        fd = ::open(path, O_RDONLY);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return false;
    }

    size_t total = 0;
    bool truncated = false;
    for (;;) {
        if (total + 1 >= cap) {
            // Buffer full: probe one byte to distinguish "exactly cap-1" from a
            // genuinely truncated read (L3).
            char probe = 0;
            ssize_t r = -1;
            do {
                r = ::read(fd, &probe, 1);
            } while (r < 0 && errno == EINTR);
            if (r < 0) {
                ::close(fd);
                return false;
            }
            truncated = r > 0;
            break;
        }
        ssize_t r = -1;
        do {
            r = ::read(fd, buf.data() + total, cap - 1 - total);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            ::close(fd);
            return false;
        }
        if (r == 0) {
            break;
        }
        total += static_cast<size_t>(r);
    }
    ::close(fd);
    buf[total] = '\0';
    *out_len = total;
    if (out_truncated != nullptr) {
        *out_truncated = truncated;
    }
    return true;
}

bool statvfs_bavail(const char* dir, uint64_t* out_bavail_mb) noexcept {
    if (dir == nullptr || out_bavail_mb == nullptr) {
        return false;
    }
    struct statvfs vfs;
    if (::statvfs(dir, &vfs) != 0) {
        return false;
    }
    const uint64_t frsize =
        vfs.f_frsize != 0 ? static_cast<uint64_t>(vfs.f_frsize) : 512u;
    const double mb = static_cast<double>(vfs.f_bavail) *
                      static_cast<double>(frsize) / (1024.0 * 1024.0);
    if (!(mb >= 0.0)) {
        return false;
    }
    // 2^64 as double; anything at/above saturates instead of wrapping.
    *out_bavail_mb = mb >= 18446744073709551616.0
                         ? UINT64_MAX
                         : static_cast<uint64_t>(mb);
    return true;
}

// ---- record composition --------------------------------------------------

void build_host_record(const HostSnapshot& prev, bool has_prev,
                       const HostSnapshot& cur, uint16_t caveat_mask,
                       bool truncated, double t, HostRecord& out) noexcept {
    std::memset(&out, 0, sizeof(out));
    out.t = t;
    out.caveat_mask = caveat_mask;
    if (truncated) {
        out.caveat_mask |= std::to_underlying(HostCaveat::kCaveatTruncated);
    }

    // Instantaneous fields do not need a baseline.
    if ((cur.present & std::to_underlying(HostPresent::kHostDiskBavailMb)) != 0) {
        out.disk_bavail_mb = cur.disk_bavail_mb;
        out.present_mask |= std::to_underlying(HostPresent::kHostDiskBavailMb);
    }

    if (!has_prev) {
        return;  // first sample: every counter delta is unknown => null
    }

    for (const DeltaField& f : kDeltaFields) {
        if (((prev.present & cur.present) & f.bit) == 0) {
            continue;  // absent in either snapshot => null
        }
        uint64_t d = 0;
        if (!delta_u64(prev.*f.snap, cur.*f.snap, &d)) {
            continue;  // reset/wrap: re-baseline, emit null (HM-1)
        }
        out.*f.out = d;
        out.present_mask |= f.bit;
    }

    // steal as a percentage of the interval's total CPU time.
    const uint16_t steal_bit = std::to_underlying(HostPresent::kHostStealPct);
    if (((prev.present & cur.present) & steal_bit) != 0) {
        uint64_t dsteal = 0;
        uint64_t dtotal = 0;
        if (delta_u64(prev.stat_steal_jiffies, cur.stat_steal_jiffies,
                      &dsteal) &&
            delta_u64(prev.stat_total_jiffies, cur.stat_total_jiffies,
                      &dtotal) &&
            dtotal > 0) {
            out.steal_pct = static_cast<float>(
                static_cast<double>(dsteal) / static_cast<double>(dtotal) *
                100.0);
            out.present_mask |= steal_bit;
        }
    }
}

} // namespace hldsmon
