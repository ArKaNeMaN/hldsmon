// HldsMonitoring — host tests for the pure engine-layout probe predicate
// (plan Phase 3, INV-4, QAS-6 adjacency). No engine headers involved.

#include "engine_layout.h"

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

using hldsmon::ClientProbeView;
using hldsmon::probe_ok;

int main() {
    ClientProbeView v;
    std::memset(&v, 0, sizeof(v));
    v.id = 3;
    v.expected_id = 3;
    v.userid = 7;
    v.remote_ip = 0x01020304u;
    v.expected_ip = 0x01020304u;
    v.connect_time = 10.0;
    v.last_received = 20.0;
    v.now = 100.0;

    CHECK(probe_ok(v));

    v.expected_id = 4;
    CHECK(!probe_ok(v));
    v.expected_id = 3;

    v.remote_ip = 0xDEADBEEFu;
    CHECK(!probe_ok(v));
    v.remote_ip = 0x01020304u;

    v.userid = 0x10000u;
    CHECK(!probe_ok(v));
    v.userid = 0xFFFFu;
    CHECK(probe_ok(v));
    v.userid = 7;

    v.connect_time = -1.0;
    CHECK(!probe_ok(v));
    v.connect_time = 10.0;

    v.connect_time = 101.0;
    CHECK(probe_ok(v));
    v.connect_time = 101.0001;
    CHECK(!probe_ok(v));
    v.connect_time = 10.0;

    v.last_received = -0.5;
    CHECK(!probe_ok(v));
    v.last_received = 101.0;
    CHECK(probe_ok(v));
    v.last_received = 200.0;
    CHECK(!probe_ok(v));
    v.last_received = 20.0;

    ClientProbeView idle;
    std::memset(&idle, 0, sizeof(idle));
    idle.id = 0;
    idle.expected_id = 0;
    idle.now = 50.0;
    CHECK(probe_ok(idle));

    if (g_fail == 0) std::printf("test_engine_layout: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
