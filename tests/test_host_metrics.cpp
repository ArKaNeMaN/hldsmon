// HldsMonitoring — host test for the engine-free host/kernel core (Stage 6).
//
// Covers QAS-H1 (parse-by-name), QAS-H2 (shared netns => caveated), QAS-H3
// (missing source => flag, not zero-health), QAS-H7 (first sample Δ = null,
// steal as %), QAS-H8 (EACCES/uncertainty => caveated, no crash) and HM-1
// (counter reset/wrap re-baselines to null).

#include "host_metrics.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

using namespace hldsmon;

static HostSnapshot empty_snapshot() {
    HostSnapshot s;
    std::memset(&s, 0, sizeof s);
    return s;
}

// ---- QAS-H1: parse by header name --------------------------------------

static void test_parse_snmp_udp_old_kernel() {
    // CentOS 7-era header: no InCsumErrors, no MemErrors/IgnoredMulti.
    const char* kFile =
        "Ip: Forwarding DefaultTTL InReceives\n"
        "Ip: 1 64 100\n"
        "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors IgnoredMulti\n"
        "Udp: 10 0 5 20 3 1\n"
        "UdpLite: InDatagrams NoPorts InErrors OutDatagrams\n"
        "UdpLite: 99 0 77 88\n";
    HostSnapshot s = empty_snapshot();
    parse_snmp_udp(kFile, s);
    CHECK(s.udp_in_errors == 5);
    CHECK(s.udp_rcvbuf_errors == 3);
    CHECK((s.present & kHostUdpInErrors) != 0);
    CHECK((s.present & kHostUdpRcvbufErrors) != 0);
    CHECK((s.present & kHostUdpInCsumErrors) == 0);  // absent => flag, not 0
}

static void test_parse_snmp_udp_modern_and_reordered() {
    const char* kFile =
        "Udp: NoPorts InErrors OutDatagrams RcvbufErrors InCsumErrors InDatagrams\n"
        "Udp: 0 42 7 9 4 1000\n";
    HostSnapshot s = empty_snapshot();
    parse_snmp_udp(kFile, s);
    CHECK(s.udp_in_errors == 42);
    CHECK(s.udp_rcvbuf_errors == 9);
    CHECK(s.udp_in_csum_errors == 4);
    CHECK((s.present & (kHostUdpInErrors | kHostUdpRcvbufErrors |
                        kHostUdpInCsumErrors)) ==
          (kHostUdpInErrors | kHostUdpRcvbufErrors | kHostUdpInCsumErrors));
}

static void test_parse_snmp_udp_no_section() {
    HostSnapshot s = empty_snapshot();
    parse_snmp_udp("Ip: Forwarding\nIp: 1\n", s);
    CHECK(s.present == 0);
    parse_snmp_udp("", s);
    CHECK(s.present == 0);
    parse_snmp_udp(nullptr, s);
    CHECK(s.present == 0);
}

// ---- QAS-H1/H3: /proc/net/dev ------------------------------------------

static void test_parse_net_dev_real_header() {
    // Real Linux layout: the rx "drop" column is printed, rx missed/overruns
    // are not -> they stay absent instead of being reported as healthy zeroes.
    const char* kFile =
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo: 67923409   45422    0    0    0     0          0         0 67923409   45422    0    0    0     0       0          0\n"
        "  eth0: 2163850881 2373861    0    4    0     0          0     13819 1652587300 1557225    0    0    0     0       0          0\n";
    HostSnapshot s = empty_snapshot();
    parse_net_dev(kFile, s);
    CHECK(s.net_rx_dropped == 4);  // summed over interfaces
    CHECK((s.present & kHostNetRxDropped) != 0);
    CHECK((s.present & kHostNetRxMissed) == 0);
    CHECK((s.present & kHostNetRxOverruns) == 0);
}

static void test_parse_net_dev_extended_header() {
    // A kernel/report that does print the extra rx columns: map by name.
    const char* kFile =
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop missed overruns fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "  eth0: 100 10 0 2 3 4 0 0 0 0 100 10 0 0 0 0 0 0\n"
        "  eth1: 200 20 0 5 6 7 0 0 0 0 200 20 0 0 0 0 0 0\n";
    HostSnapshot s = empty_snapshot();
    parse_net_dev(kFile, s);
    CHECK(s.net_rx_dropped == 7);
    CHECK(s.net_rx_missed == 9);
    CHECK(s.net_rx_overruns == 11);
    CHECK((s.present & (kHostNetRxDropped | kHostNetRxMissed |
                        kHostNetRxOverruns)) ==
          (kHostNetRxDropped | kHostNetRxMissed | kHostNetRxOverruns));
}

static void test_parse_net_dev_without_header() {
    HostSnapshot s = empty_snapshot();
    parse_net_dev("eth0: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n", s);
    CHECK(s.present == 0);
}

// ---- /proc/stat and /proc/self/io ---------------------------------------

static void test_parse_stat() {
    const char* kFile =
        "cpu  100 2 50 1000 10 0 5 7 0 0\n"
        "cpu0 50 1 25 500 5 0 2 3 0 0\n"
        "intr 123 0\n";
    HostSnapshot s = empty_snapshot();
    parse_stat(kFile, s);
    CHECK((s.present & kHostStealPct) != 0);
    CHECK(s.stat_steal_jiffies == 7);
    CHECK(s.stat_total_jiffies == 100 + 2 + 50 + 1000 + 10 + 0 + 5 + 7);
}

static void test_parse_stat_without_steal() {
    HostSnapshot s = empty_snapshot();
    parse_stat("cpu 100 2 50 1000 10 0 5\n", s);
    CHECK((s.present & kHostStealPct) == 0);
}

static void test_parse_self_io() {
    const char* kFile =
        "rchar: 7204\n"
        "wchar: 0\n"
        "read_bytes: 4096\n"
        "write_bytes: 8192\n"
        "cancelled_write_bytes: 0\n";
    HostSnapshot s = empty_snapshot();
    parse_self_io(kFile, s);
    CHECK(s.io_read_bytes == 4096);
    CHECK(s.io_write_bytes == 8192);
    CHECK((s.present & (kHostIoReadBytes | kHostIoWriteBytes)) ==
          (kHostIoReadBytes | kHostIoWriteBytes));

    HostSnapshot s2 = empty_snapshot();
    parse_self_io("rchar: 1\nwchar: 2\n", s2);
    CHECK((s2.present & (kHostIoReadBytes | kHostIoWriteBytes)) == 0);
}

// ---- HM-1: reset/wrap-safe delta ----------------------------------------

static void test_delta_u64_reset_and_wrap() {
    uint64_t d = 777;
    CHECK(delta_u64(100, 250, &d));
    CHECK(d == 150);
    CHECK(delta_u64(5, 5, &d));  // unchanged counter is a valid zero delta
    CHECK(d == 0);

    d = 777;
    CHECK(!delta_u64(250, 100, &d));  // reset: rejected, never a modulo guess
    CHECK(d == 777);                  // output untouched

    CHECK(delta_u64(UINT64_MAX - 5, UINT64_MAX, &d));
    CHECK(d == 5);
    CHECK(!delta_u64(UINT64_MAX, 1, &d));  // wrap beyond sane horizon => null
    CHECK(delta_u64(0, UINT64_MAX, &d));
    CHECK(d == UINT64_MAX);
}

// ---- QAS-H2/H8: environment detector ------------------------------------

static void test_detect_env_matrix() {
    // Dedicated host/KVM: authoritative, no netns/steal caveats.
    EnvInputs host;
    host.container = Tri::kNo;
    host.netns_private = Tri::kUnknown;
    EnvVerdict v = detect_env(host);
    CHECK(v.netns_authoritative);
    CHECK((v.caveat & kCaveatUdpNetns) == 0);
    CHECK((v.caveat & kCaveatSteal) == 0);
    CHECK((v.caveat & kCaveatNetDev) != 0);  // M2: always
    CHECK((v.caveat & kCaveatIo) != 0);
    CHECK((v.caveat & kCaveatDisk) != 0);

    // Container with a positively confirmed private netns: authoritative netns,
    // but steal stays caveated (lxcfs/OpenVZ zero it).
    EnvInputs container_private;
    container_private.container = Tri::kYes;
    container_private.netns_private = Tri::kYes;
    v = detect_env(container_private);
    CHECK(v.netns_authoritative);
    CHECK((v.caveat & kCaveatUdpNetns) == 0);
    CHECK((v.caveat & kCaveatSteal) != 0);

    // Container confirmed shared netns => caveated (QAS-H2).
    EnvInputs container_shared;
    container_shared.container = Tri::kYes;
    container_shared.netns_private = Tri::kNo;
    v = detect_env(container_shared);
    CHECK(!v.netns_authoritative);
    CHECK((v.caveat & kCaveatUdpNetns) != 0);
    CHECK((v.caveat & kCaveatSteal) != 0);

    // Unreadable /proc/1/ns/net (EACCES) => unknown => caveated (QAS-H8).
    EnvInputs container_unknown;
    container_unknown.container = Tri::kYes;
    container_unknown.netns_private = Tri::kUnknown;
    v = detect_env(container_unknown);
    CHECK(!v.netns_authoritative);
    CHECK((v.caveat & kCaveatUdpNetns) != 0);

    // Unknown containerization is never treated as health.
    EnvInputs all_unknown;
    all_unknown.container = Tri::kUnknown;
    all_unknown.netns_private = Tri::kUnknown;
    v = detect_env(all_unknown);
    CHECK(!v.netns_authoritative);
    CHECK((v.caveat & kCaveatUdpNetns) != 0);
    CHECK((v.caveat & kCaveatSteal) != 0);
}

static void test_probe_env_smoke() {
    // Must never crash; the verdict invariants hold for any environment.
    const EnvInputs in = probe_env();
    CHECK(in.container == Tri::kNo || in.container == Tri::kYes ||
          in.container == Tri::kUnknown);
    const EnvVerdict v = detect_env(in);
    CHECK((v.caveat & kCaveatNetDev) != 0);
    CHECK((v.caveat & kCaveatIo) != 0);
    CHECK((v.caveat & kCaveatDisk) != 0);
}

// ---- QAS-H3: bounded reader and statvfs --------------------------------

static void test_read_file() {
    char buf[512];
    size_t len = 123;
    bool truncated = true;
    CHECK(!read_file("/nonexistent/hldsmon/file", buf, sizeof buf, &len,
                     &truncated));
    CHECK(len == 0);
    CHECK(!truncated);

    len = 0;
    truncated = true;
    CHECK(read_file("/proc/self/io", buf, sizeof buf, &len, &truncated));
    CHECK(len > 0);
    CHECK(!truncated);
    CHECK(buf[len] == '\0');

    // Small buffer => prefix is returned and the read is marked truncated.
    char tiny[8];
    len = 0;
    truncated = false;
    CHECK(read_file("/proc/self/io", tiny, sizeof tiny, &len, &truncated));
    CHECK(len == sizeof tiny - 1);
    CHECK(truncated);
    CHECK(tiny[len] == '\0');

    // A directory opens but read() fails => unavailable, not a crash.
    len = 99;
    CHECK(!read_file(".", buf, sizeof buf, &len, &truncated));
    CHECK(len == 0);
}

static void test_statvfs_bavail() {
    uint64_t mb = 0;
    CHECK(statvfs_bavail(".", &mb));
    CHECK(!statvfs_bavail("/nonexistent/hldsmon/dir", &mb));
    CHECK(!statvfs_bavail(nullptr, &mb));
}

// ---- QAS-H7/HM-2: record composition ------------------------------------

static void test_build_record_first_sample_is_null() {
    HostSnapshot cur = empty_snapshot();
    cur.present = kHostUdpRcvbufErrors | kHostUdpInErrors | kHostStealPct |
                  kHostDiskBavailMb;
    cur.udp_rcvbuf_errors = 10;
    cur.udp_in_errors = 100;
    cur.stat_steal_jiffies = 1;
    cur.stat_total_jiffies = 100;
    cur.disk_bavail_mb = 12345;

    const HostSnapshot prev = empty_snapshot();
    HostRecord r;
    build_host_record(prev, /*has_prev=*/false, cur, kCaveatUdpNetns, false,
                      1755000000.0, r);
    CHECK(r.t == 1755000000.0);
    // First sample: every Δ is unknown => absent (null), not zero.
    CHECK(r.present_mask == kHostDiskBavailMb);
    CHECK(r.udp_rcvbuf_errors == 0);
    CHECK(r.udp_in_errors == 0);
    CHECK((r.present_mask & kHostUdpRcvbufErrors) == 0);
    CHECK((r.present_mask & kHostStealPct) == 0);
    CHECK((r.present_mask & kHostDiskBavailMb) != 0);
    CHECK(r.disk_bavail_mb == 12345);
    CHECK(r.caveat_mask == kCaveatUdpNetns);
}

static void test_build_record_deltas_and_steal_pct() {
    HostSnapshot prev = empty_snapshot();
    prev.present = kHostUdpRcvbufErrors | kHostUdpInErrors | kHostNetRxDropped |
                   kHostIoReadBytes | kHostIoWriteBytes | kHostStealPct |
                   kHostDiskBavailMb;
    prev.udp_rcvbuf_errors = 10;
    prev.udp_in_errors = 100;
    prev.net_rx_dropped = 0;
    prev.io_read_bytes = 1000;
    prev.io_write_bytes = 2000;
    prev.stat_steal_jiffies = 10;
    prev.stat_total_jiffies = 1000;
    prev.disk_bavail_mb = 500;

    HostSnapshot cur = prev;
    cur.udp_rcvbuf_errors = 25;
    cur.udp_in_errors = 140;
    cur.net_rx_dropped = 7;
    cur.io_read_bytes = 3000;
    cur.io_write_bytes = 2500;
    cur.stat_steal_jiffies = 15;
    cur.stat_total_jiffies = 1500;
    cur.disk_bavail_mb = 499;

    HostRecord r;
    build_host_record(prev, true, cur, kCaveatNetDev, false, 2.0, r);
    CHECK(r.present_mask == (kHostUdpRcvbufErrors | kHostUdpInErrors |
                             kHostNetRxDropped | kHostIoReadBytes |
                             kHostIoWriteBytes | kHostStealPct |
                             kHostDiskBavailMb));
    CHECK(r.udp_rcvbuf_errors == 15);
    CHECK(r.udp_in_errors == 40);
    CHECK(r.net_rx_dropped == 7);
    CHECK(r.io_read_bytes == 2000);
    CHECK(r.io_write_bytes == 500);
    CHECK(r.disk_bavail_mb == 499);
    // steal: 5/500 => exactly 1%.
    CHECK(r.steal_pct > 0.999f && r.steal_pct < 1.001f);
    CHECK(r.caveat_mask == kCaveatNetDev);
}

static void test_build_record_reset_rebaselines() {
    HostSnapshot prev = empty_snapshot();
    prev.present = kHostUdpRcvbufErrors | kHostUdpInErrors | kHostStealPct;
    prev.udp_rcvbuf_errors = 50;  // will be "reset" (netns recreation)
    prev.udp_in_errors = 200;
    prev.stat_steal_jiffies = 30;
    prev.stat_total_jiffies = 1000;

    HostSnapshot cur = empty_snapshot();
    cur.present = prev.present;
    cur.udp_rcvbuf_errors = 3;  // reset: cur < prev => null for this field only
    cur.udp_in_errors = 260;
    cur.stat_steal_jiffies = 5;  // reset => steal_pct null
    cur.stat_total_jiffies = 2000;

    HostRecord r;
    build_host_record(prev, true, cur, 0, false, 3.0, r);
    CHECK((r.present_mask & kHostUdpRcvbufErrors) == 0);
    CHECK((r.present_mask & kHostUdpInErrors) != 0);
    CHECK(r.udp_in_errors == 60);
    CHECK((r.present_mask & kHostStealPct) == 0);
}

static void test_build_record_truncation_caveat() {
    HostSnapshot s = empty_snapshot();
    HostRecord r;
    build_host_record(s, true, s, kCaveatIo, /*truncated=*/true, 1.0, r);
    CHECK((r.caveat_mask & kCaveatTruncated) != 0);
    CHECK((r.caveat_mask & kCaveatIo) != 0);
    CHECK(r.present_mask == 0);
}

// ---- parse-by-name composition through a full tick fixture --------------

static void test_full_snapshot_from_fixtures() {
    HostSnapshot s = empty_snapshot();
    parse_snmp_udp(
        "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors\n"
        "Udp: 10 0 5 20 3 2 7\n",
        s);
    parse_net_dev(
        "Inter-| Receive | Transmit\n"
        " face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n"
        "  eth0: 1 2 0 4 0 0 0 0 1 2 0 5 0 0 0 0\n",
        s);
    parse_stat("cpu 10 1 2 100 1 0 0 3 0 0\n", s);
    parse_self_io("read_bytes: 8\nwrite_bytes: 9\n", s);
    s.disk_bavail_mb = 42;
    s.present |= kHostDiskBavailMb;

    // A second tick with +1 on every counter yields Δ = 1 for each field.
    HostSnapshot next = s;
    next.udp_in_errors += 1;
    next.udp_rcvbuf_errors += 1;
    next.udp_in_csum_errors += 1;
    next.net_rx_dropped += 1;
    next.stat_steal_jiffies += 1;
    next.stat_total_jiffies += 100;
    next.io_read_bytes += 1;
    next.io_write_bytes += 1;
    next.disk_bavail_mb = 41;

    HostRecord r;
    build_host_record(s, true, next, 0, false, 4.0, r);
    CHECK(r.udp_in_errors == 1);
    CHECK(r.udp_rcvbuf_errors == 1);
    CHECK(r.udp_in_csum_errors == 1);
    CHECK(r.net_rx_dropped == 1);
    CHECK(r.io_read_bytes == 1);
    CHECK(r.io_write_bytes == 1);
    CHECK(r.disk_bavail_mb == 41);
    CHECK(r.steal_pct > 0.9f && r.steal_pct < 1.1f);  // 1/100 = 1%
    CHECK(r.present_mask == (kHostUdpRcvbufErrors | kHostUdpInErrors |
                             kHostUdpInCsumErrors | kHostNetRxDropped |
                             kHostStealPct | kHostIoReadBytes |
                             kHostIoWriteBytes | kHostDiskBavailMb));
}

int main() {
    test_parse_snmp_udp_old_kernel();
    test_parse_snmp_udp_modern_and_reordered();
    test_parse_snmp_udp_no_section();
    test_parse_net_dev_real_header();
    test_parse_net_dev_extended_header();
    test_parse_net_dev_without_header();
    test_parse_stat();
    test_parse_stat_without_steal();
    test_parse_self_io();
    test_delta_u64_reset_and_wrap();
    test_detect_env_matrix();
    test_probe_env_smoke();
    test_read_file();
    test_statvfs_bavail();
    test_build_record_first_sample_is_null();
    test_build_record_deltas_and_steal_pct();
    test_build_record_reset_rebaselines();
    test_build_record_truncation_caveat();
    test_full_snapshot_from_fixtures();

    if (g_fail == 0) std::printf("test_host_metrics: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
