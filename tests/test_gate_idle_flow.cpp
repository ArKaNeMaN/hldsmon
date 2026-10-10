// HldsMonitoring — host test for gate_idle_flow: idle_ms >= threshold zeros all
// four flow fields; idle_ms < threshold leaves them unchanged.

#include "metrics.h"

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

static hldsmon::ClientSample make_sample(uint32_t idle_ms) {
    hldsmon::ClientSample s;
    s.flow_in_kbps = 1.5f;
    s.flow_out_kbps = 2.5f;
    s.flow_in_avg = 3.5f;
    s.flow_out_avg = 4.5f;
    s.idle_ms = idle_ms;
    return s;
}

static bool flows_zero(const hldsmon::ClientSample& s) {
    return s.flow_in_kbps == 0.0f && s.flow_out_kbps == 0.0f &&
           s.flow_in_avg == 0.0f && s.flow_out_avg == 0.0f;
}

static bool flows_unchanged(const hldsmon::ClientSample& s) {
    return s.flow_in_kbps == 1.5f && s.flow_out_kbps == 2.5f &&
           s.flow_in_avg == 3.5f && s.flow_out_avg == 4.5f;
}

int main() {
    using hldsmon::gate_idle_flow;
    const uint32_t th = 1500;

    {
        hldsmon::ClientSample s = make_sample(th);
        gate_idle_flow(s, th);
        CHECK(flows_zero(s));
        CHECK(s.idle_ms == th);
    }
    {
        hldsmon::ClientSample s = make_sample(th + 1);
        gate_idle_flow(s, th);
        CHECK(flows_zero(s));
    }
    {
        hldsmon::ClientSample s = make_sample(5000);
        gate_idle_flow(s, th);
        CHECK(flows_zero(s));
    }

    {
        hldsmon::ClientSample s = make_sample(th - 1);
        gate_idle_flow(s, th);
        CHECK(flows_unchanged(s));
    }
    {
        hldsmon::ClientSample s = make_sample(0);
        gate_idle_flow(s, th);
        CHECK(flows_unchanged(s));
    }

    if (g_fail == 0) std::printf("test_gate_idle_flow: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
