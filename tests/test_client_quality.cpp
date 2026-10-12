// HldsMonitoring — host tests for the per-client jitter accumulator (CQ1) and
// its serialization / drop-line headroom (CQ3, CQ-1 ripple). Native g++
// (no -m32, no engine headers), compiled by test-host-core.

#include "metrics.h"
#include "serialization.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

using namespace hldsmon;

static bool has(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}

// First packet / no data: no delta was observed, so the triple is 0/0/0.
static void test_first_packet_and_delta_guard() {
    uint16_t mn = 9;
    uint16_t av = 9;
    uint16_t mx = 9;
    jitter_finalize(0, 0, 0, 0, mn, av, mx);
    CHECK(mn == 0 && av == 0 && mx == 0);

    uint32_t ms = 123;
    CHECK(!jitter_delta_ms(5.0, 5.0, ms));   // zero delta (first packet)
    CHECK(!jitter_delta_ms(5.0, 6.0, ms));   // clock went backwards
    CHECK(!jitter_delta_ms(0.0, 0.0, ms));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(!jitter_delta_ms(nan, 1.0, ms));
    CHECK(!jitter_delta_ms(1.0, nan, ms));
    CHECK(ms == 123);                        // untouched on rejection

    CHECK(jitter_delta_ms(6.5, 6.0, ms));
    CHECK(ms == 500);
}

static void test_min_avg_max() {
    uint32_t mn = 0;
    uint32_t sum = 0;
    uint32_t mx = 0;
    uint32_t count = 0;
    jitter_reset(mn, sum, mx, count);

    const double t = 10.0;
    const double deltas[] = {0.100, 0.300, 0.200};
    uint32_t ms = 0;
    double prev = t;
    for (const double d : deltas) {
        CHECK(jitter_delta_ms(prev + d, prev, ms));
        jitter_add_ms(mn, sum, mx, count, ms);
        prev += d;
    }
    CHECK(count == 3);
    CHECK(mn == 100);
    CHECK(sum == 600);
    CHECK(mx == 300);

    uint16_t omin = 0;
    uint16_t oavg = 0;
    uint16_t omax = 0;
    jitter_finalize(mn, sum, mx, count, omin, oavg, omax);
    CHECK(omin == 100);
    CHECK(oavg == 200);
    CHECK(omax == 300);

    jitter_reset(mn, sum, mx, count);
    CHECK(mn == 0 && sum == 0 && mx == 0 && count == 0);
    jitter_finalize(mn, sum, mx, count, omin, oavg, omax);
    CHECK(omin == 0 && oavg == 0 && omax == 0);
}

static void test_clamp() {
    uint32_t ms = 0;
    CHECK(jitter_delta_ms(100.0, 0.0, ms));   // 100 s interval
    CHECK(ms == 65535u);

    uint32_t mn = 0;
    uint32_t sum = 0;
    uint32_t mx = 0;
    uint32_t count = 0;
    jitter_add_ms(mn, sum, mx, count, 70000u);
    uint16_t omin = 0;
    uint16_t oavg = 0;
    uint16_t omax = 0;
    jitter_finalize(mn, sum, mx, count, omin, oavg, omax);
    CHECK(omin == 65535u);
    CHECK(oavg == 65535u);
    CHECK(omax == 65535u);

    // A pathological interval must not wrap the running sum.
    jitter_reset(mn, sum, mx, count);
    for (int i = 0; i < 3; ++i) {
        jitter_add_ms(mn, sum, mx, count, 4294967290u);
    }
    CHECK(count == 3);
    CHECK(sum == 0xFFFFFFFFu);
}

static void test_serialization() {
    ClientSample c;
    std::memset(&c, 0, sizeof c);
    c.t = 2.0;
    c.id = 3;
    c.ping = 40;
    c.jit_min_ms = 12;
    c.jit_avg_ms = 34;
    c.jit_max_ms = 56;

    char buf[4096];
    const size_t n = write_client_json(buf, sizeof buf, c);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    CHECK(has(buf, "\"jit_min_ms\":12,\"jit_avg_ms\":34,\"jit_max_ms\":56,"));
}

// The largest drop line (full window, worst-case identity block) must still
// fit the plugin's kLineBufSize (65536) after the jitter fields (CQ-1).
static void test_drop_line_headroom() {
    DropRecord d;
    std::memset(&d, 0, sizeof d);
    d.t = 1712345679.0;
    d.id = 7;
    d.gen = 1712000000u;
    std::strcpy(d.reason, "Reliable channel overflowed");
    std::strcpy(d.category, "overflow");
    std::strcpy(d.observed_via, "sv_dropclient");
    for (int i = 0; i < 64; ++i) {
        ClientSample& w = d.window[i];
        std::memset(&w, 0, sizeof w);
        std::memset(w.name, '"', sizeof(w.name) - 1);
        std::memset(w.authid, '"', sizeof(w.authid) - 1);
        std::memset(w.steamid64, '9', sizeof(w.steamid64) - 1);
        std::memset(w.ip, '1', sizeof(w.ip) - 1);
        w.port = 65535;
        w.userid = 65535;
        w.slot = 32;
        w.jit_min_ms = 65535;
        w.jit_avg_ms = 65535;
        w.jit_max_ms = 65535;
    }
    d.window_count = 64;

    char buf[65536];
    const size_t n = write_drop_json(buf, sizeof buf, d);
    CHECK(n > 0);
    CHECK(n < sizeof buf);
    CHECK(std::strlen(buf) == n);
}

int main() {
    test_first_packet_and_delta_guard();
    test_min_avg_max();
    test_clamp();
    test_serialization();
    test_drop_line_headroom();

    if (g_fail == 0) std::printf("test_client_quality: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
