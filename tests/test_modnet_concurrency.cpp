// HldsMonitoring — modnet atomic-counter concurrency test (plan M-P1, QAS-M4).
// N threads concurrently increment one object's counters; the totals must be
// exact (no lost updates). Links with -pthread (MN-6).

#include "modnet.h"

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

using namespace hldsmon;

int main() {
    constexpr int kThreads = 8;
    constexpr int kIters = 200000;
    constexpr uint64_t kTotal = static_cast<uint64_t>(kThreads) * static_cast<uint64_t>(kIters);

    ModnetCounters* c = modnet_counters(0);
    CHECK(c != nullptr);
    if (c == nullptr) {
        std::printf("test_modnet_concurrency: setup failed\n");
        return 1;
    }
    c->reset();

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([c]() {
            for (int i = 0; i < kIters; ++i) {
                c->add_in(1);
                c->add_out(2);
                c->add_connect_attempt();
            }
        });
    }
    for (std::thread& w : workers) {
        w.join();
    }

    ModnetSnapshot s;
    modnet_snapshot(*c, s);
    CHECK(s.in_bytes == kTotal);
    CHECK(s.out_bytes == kTotal * 2u);
    CHECK(s.connect_attempts == kTotal);

    if (g_fail == 0) std::printf("test_modnet_concurrency: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
