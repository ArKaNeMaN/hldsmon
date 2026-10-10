// HldsMonitoring — host test for CounterDelta (plan Phase 1).

#include "delta.h"

#include <cstdint>
#include <cstdio>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

int main() {
    using hldsmon::CounterDelta;

    // normal forward increments
    CHECK(CounterDelta::compute_delta(0, 0) == 0);
    CHECK(CounterDelta::compute_delta(100, 150) == 50);
    CHECK(CounterDelta::compute_delta(UINT32_C(1000000), UINT32_C(1000001)) == 1);

    // zero delta (no change)
    CHECK(CounterDelta::compute_delta(5, 5) == 0);
    CHECK(CounterDelta::compute_delta(UINT32_MAX, UINT32_MAX) == 0);

    // 32-bit wrap across zero
    CHECK(CounterDelta::compute_delta(UINT32_MAX - 4u, 3u) == 8u);
    CHECK(CounterDelta::compute_delta(UINT32_MAX - 1u, 0u) == 2u);
    CHECK(CounterDelta::compute_delta(UINT32_MAX, 0u) == 1u);

    // apparent decrease (prev > cur) is treated as a wrap
    CHECK(CounterDelta::compute_delta(200, 100) ==
          (UINT32_MAX - 200u) + 100u + 1u);

    // counter "reset" from a high value back to 0 reads as a wrap distance
    CHECK(CounterDelta::compute_delta(100, 0) ==
          (UINT32_MAX - 100u) + 0u + 1u);

    // largest possible forward jump
    CHECK(CounterDelta::compute_delta(0, UINT32_MAX) == UINT32_MAX);
    CHECK(CounterDelta::compute_delta(1, 0) == UINT32_MAX);

    if (g_fail == 0) std::printf("test_delta: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
