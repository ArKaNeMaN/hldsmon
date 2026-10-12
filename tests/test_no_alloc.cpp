// HldsMonitoring — INV-2 proof: the serialization write path allocates nothing.
//
// Global operator new/new[] are replaced with a counting allocator. After a
// deliberate probe (to prove the counter works), the counter is reset and a
// full DropRecord (with a non-trivial window) plus client/server lines and
// json_escape are serialized into caller-provided stack buffers. The counter
// must stay at zero. std::string is deliberately absent from the measured path.

#include "serialization.h"
#include "host_metrics.h"
#include "modnet.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

static size_t g_allocs = 0;

void* operator new(std::size_t n) {
    ++g_allocs;
    return std::malloc(n != 0 ? n : 1);
}
void* operator new[](std::size_t n) {
    ++g_allocs;
    return std::malloc(n != 0 ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

using namespace hldsmon;

static ClientSample make_client(uint8_t id) {
    ClientSample c;
    std::memset(&c, 0, sizeof c);
    c.t = 100.0 + static_cast<double>(id);
    c.id = id;
    c.gen = 1712000000u;
    c.ping = 40;
    c.loss = 3;
    c.dloss = 1;
    c.in_seq = 1000 + id;
    c.out_seq = 900 + id;
    c.rel_fill = 5;
    c.rate = 20000;
    c.updaterate = 101;
    c.cmdrate = 101;
    c.state = 7;
    c.flow_in_kbps = 12.3f;
    c.flow_out_kbps = 18.1f;
    c.flow_in_avg = 12.0f;
    c.flow_out_avg = 18.0f;
    c.idle_ms = 1200u + id;
    c.jit_min_ms = 1;
    c.jit_avg_ms = 7;
    c.jit_max_ms = 40 + id;
    return c;
}

int main() {
    // Prove the counting allocator works (so a zero result is meaningful).
    g_allocs = 0;
    int* probe = new int(5);
    CHECK(g_allocs == 1);
    delete probe;

    // Build a DropRecord with a non-trivial window. Construction uses no heap.
    DropRecord d;
    std::memset(&d, 0, sizeof d);
    d.t = 1712345679.0;
    d.id = 7;
    d.gen = 1712000000u;
    std::strcpy(d.reason, "Reliable channel overflowed");
    std::strcpy(d.category, "overflow");
    d.crash = 0;
    std::strcpy(d.observed_via, "sv_dropclient");
    d.client.active = 1;
    d.client.spawned = 1;
    d.client.userid = 7;
    d.netchan.in_seq = 12345;
    d.netchan.rate = 20000;
    d.netchan.last_received = 1000.0f;
    d.netchan.connect_time = 900.0f;
    // Full drop window (worst-case line size for kLineBufSize headroom).
    d.window_count = 64;
    for (uint8_t i = 0; i < 64; ++i) {
        d.window[i] = make_client(i);
    }
    d.server.t = 3.0;
    d.server.frame_ms_avg = 4.0f;
    d.server.cpu_pct = 41.2f;
    d.server.players = 17;
    std::strcpy(d.server.map, "de_dust2");
    d.server.maxclients = 32;
    d.neighbors.count = 7;
    d.neighbors.avg_loss = 1.5f;

    char buf[65536];
    char esc[64];

    // ---- measured region: must allocate nothing ----
    g_allocs = 0;

    const size_t n_drop = write_drop_json(buf, sizeof buf, d);
    if (n_drop == 0) {
        std::printf("FAIL: drop serialization returned 0\n");
        ++g_fail;
    }

    const ClientSample c = make_client(3);
    const size_t n_client = write_client_json(buf, sizeof buf, c);
    if (n_client == 0) {
        std::printf("FAIL: client serialization returned 0\n");
        ++g_fail;
    }

    ServerSample s;
    std::memset(&s, 0, sizeof s);
    s.t = 1.0;
    s.players = 5;
    const size_t n_server = write_server_json(buf, sizeof buf, s);
    if (n_server == 0) {
        std::printf("FAIL: server serialization returned 0\n");
        ++g_fail;
    }

    const size_t n_esc = json_escape("a\"b\\c\n", esc, sizeof esc);
    if (n_esc == 0) {
        std::printf("FAIL: json_escape returned 0\n");
        ++g_fail;
    }

    ModnetRecord mr;
    std::memset(&mr, 0, sizeof mr);
    mr.t = 1712345679.0;
    std::strcpy(mr.module, "easy_http_amxx_i386");
    std::strcpy(mr.so, "easy_http_amxx_i386.so");
    {
        ModnetSnapshot prev{};
        ModnetSnapshot cur{};
        cur.in_bytes = 100;
        cur.out_bytes = 200;
        cur.conns = 1;
        mr.t = 1712345679.0;
        modnet_diff(prev, cur, 1.0, mr);
    }
    const size_t n_modnet = write_modnet_json(buf, sizeof buf, mr);
    if (n_modnet == 0) {
        std::printf("FAIL: modnet serialization returned 0\n");
        ++g_fail;
    }

    PacketStats ps;
    std::memset(&ps, 0, sizeof ps);
    ps.t = 1712345679.0;
    ps.pps = 100;
    std::strcpy(ps.top_ip, "9.9.9.9");
    std::strcpy(ps.top_ips[0].ip, "9.9.9.9");
    ps.top_ips[0].count = 5;
    const size_t n_packet = write_packet_json(buf, sizeof buf, ps);
    if (n_packet == 0) {
        std::printf("FAIL: packet serialization returned 0\n");
        ++g_fail;
    }

    DegradedRecord dr;
    std::memset(&dr, 0, sizeof dr);
    dr.t = 1712345679.0;
    dr.error_count = 3;
    const size_t n_degraded = write_degraded_json(buf, sizeof buf, dr);
    if (n_degraded == 0) {
        std::printf("FAIL: degraded serialization returned 0\n");
        ++g_fail;
    }

    RetentionRecord rr;
    std::memset(&rr, 0, sizeof rr);
    rr.t = 1712345679.0;
    rr.removed = 2;
    const size_t n_retention = write_retention_json(buf, sizeof buf, rr);
    if (n_retention == 0) {
        std::printf("FAIL: retention serialization returned 0\n");
        ++g_fail;
    }

    ConfigSnapshot cfg_a;
    std::memset(&cfg_a, 0, sizeof cfg_a);
    ConfigSnapshot cfg_b;
    std::memset(&cfg_b, 0, sizeof cfg_b);
    cfg_b.present = kCfgSvTimeout | kCfgSvMaxrate;
    cfg_b.sv_timeout = 65.0f;
    cfg_b.sv_maxrate = 30000.0f;
    const bool cfg_diff = config_changed(cfg_a, cfg_b);

    ConfigRecord cr;
    std::memset(&cr, 0, sizeof cr);
    cr.t = 1712345679.0;
    cr.rehlds_major = 3;
    cr.rehlds_minor = 14;
    std::strcpy(cr.map, "de_dust2");
    cr.maxclients = 32;
    cr.config = cfg_b;
    const size_t n_config = write_config_json(buf, sizeof buf, cr);
    if (n_config == 0) {
        std::printf("FAIL: config serialization returned 0\n");
        ++g_fail;
    }

    // Host-metrics (QAS-H4/HM-4): the whole gather -> parse -> compose ->
    // serialize path must stay allocation-free as well.
    HostSnapshot hs_prev;
    std::memset(&hs_prev, 0, sizeof hs_prev);
    HostSnapshot hs_cur;
    std::memset(&hs_cur, 0, sizeof hs_cur);
    char hfile[4096];
    size_t hlen = 0;
    bool htrunc = false;
    if (read_file("/proc/stat", hfile, sizeof hfile, &hlen, &htrunc)) {
        parse_stat(hfile, hs_cur);
    }
    parse_snmp_udp(
        "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors\n"
        "Udp: 10 0 5 20 3\n",
        hs_cur);
    parse_net_dev(
        " face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n"
        " eth0: 1 2 0 4 0 0 0 0 1 2 0 0 0 0 0 0\n",
        hs_cur);
    parse_self_io("read_bytes: 100\nwrite_bytes: 200\n", hs_cur);
    uint64_t hbav = 0;
    if (statvfs_bavail(".", &hbav)) {
        hs_cur.disk_bavail_mb = hbav;
        hs_cur.present |= kHostDiskBavailMb;
    }
    HostRecord hr;
    build_host_record(hs_prev, /*has_prev=*/false, hs_cur, kCaveatNetDev, false,
                      1712345679.0, hr);
    const size_t n_host = write_host_json(buf, sizeof buf, hr);
    if (n_host == 0) {
        std::printf("FAIL: host serialization returned 0\n");
        ++g_fail;
    }

    const size_t allocs_in_measured_region = g_allocs;

    // ---- end measured region ----
    CHECK(allocs_in_measured_region == 0);
    if (allocs_in_measured_region != 0) {
        std::printf("FAIL: %zu allocation(s) in serialization write path\n",
                    allocs_in_measured_region);
    }
    CHECK(n_drop > 0);
    CHECK(n_client > 0);
    CHECK(n_server > 0);
    CHECK(n_esc > 0);
    CHECK(n_modnet > 0);
    CHECK(n_packet > 0);
    CHECK(n_degraded > 0);
    CHECK(n_retention > 0);
    CHECK(n_config > 0);
    CHECK(n_host > 0);
    CHECK(cfg_diff);

    if (g_fail == 0) std::printf("test_no_alloc: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
