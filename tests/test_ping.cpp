// HldsMonitoring — host test for ping_ms_from_latency (design §21, Стадия 4).
//
// ReHLDS client_t::latency is a float in SECONDS; the helper converts it to the
// rounded, clamped millisecond value stored in the uint16 ping fields. Covers
// the numeric edges folded from the design review (PG-1 NaN, PG-2 clamp).

#include "metrics.h"

#include <cstdint>
#include <cstdio>
#include <limits>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

int main() {
    using hldsmon::ping_ms_from_latency;

    // Typical latencies: seconds -> ms (round half up).
    CHECK(ping_ms_from_latency(0.040f) == 40);
    CHECK(ping_ms_from_latency(0.001f) == 1);
    CHECK(ping_ms_from_latency(0.0005f) == 1);   // 0.5 ms rounds up
    CHECK(ping_ms_from_latency(0.1234f) == 123);

    // Unknown / non-positive => 0.
    CHECK(ping_ms_from_latency(0.0f) == 0);
    CHECK(ping_ms_from_latency(-0.5f) == 0);

    // NaN must not fall through to an out-of-range cast (PG-1).
    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    CHECK(ping_ms_from_latency(nan_v) == 0);

    // Saturation at the uint16 ceiling, incl. the rounding edge (PG-2).
    CHECK(ping_ms_from_latency(65.535f) == 65535);
    CHECK(ping_ms_from_latency(65.5347f) == 65535);
    CHECK(ping_ms_from_latency(70.0f) == 65535);

    if (g_fail == 0) std::printf("test_ping: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
