// HldsMonitoring — host tests for the connect-timings stage (design §25):
// the host-testable milliseconds converter/guard (CTP-4) and the nullable
// reject/connect/mapchange timing fields (CTP-1/CT-2). Native g++ (no -m32,
// no engine headers), compiled by test-host-core from the pure core sources.

#include "metrics.h"
#include "serialization.h"

#include <cmath>
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

static void check_layout_constants() {
    CHECK(sizeof(RejectRecord) == 305);
    CHECK(sizeof(ConnectRecord) == 178);
    CHECK(sizeof(MapChangeRecord) == 77);
}

static void test_timing_delta_ms() {
    uint32_t ms = 0;
    CHECK(timing_delta_ms(0.5, ms) && ms == 500);
    CHECK(timing_delta_ms(0.0004, ms) && ms == 0);   // rounded, still present
    CHECK(timing_delta_ms(0.0015, ms) && ms == 2);   // round half up
    CHECK(timing_delta_ms(1.0, ms) && ms == 1000);

    // guard: <= 0 / non-finite => false, out untouched (present bit cleared)
    ms = 777;
    CHECK(!timing_delta_ms(0.0, ms));
    CHECK(!timing_delta_ms(-0.001, ms));
    CHECK(!timing_delta_ms(std::numeric_limits<double>::quiet_NaN(), ms));
    CHECK(!timing_delta_ms(-std::numeric_limits<double>::infinity(), ms));
    CHECK(ms == 777);

    // positive infinity saturates like any value above the uint32 ceiling
    CHECK(timing_delta_ms(std::numeric_limits<double>::infinity(), ms));
    CHECK(ms == 0xFFFFFFFFu);
    CHECK(timing_delta_ms(4294967.295, ms));
    CHECK(ms == 0xFFFFFFFFu);
    CHECK(timing_delta_ms(4294967.294, ms));
    CHECK(ms == 4294967294u);
}

static void test_reject_timing_json() {
    char buf[512];
    RejectRecord r;
    std::memset(&r, 0, sizeof r);
    r.t = 1.0;
    std::strcpy(r.ip, "1.2.3.4");
    std::strcpy(r.stage, "SV_CheckProtocol");
    r.stage_ms = 12;
    r.total_ms = 34;
    r.timing_present = kTimingStageMs | kTimingTotalMs;

    size_t n = write_reject_json(buf, sizeof buf, r);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    CHECK(has(buf, "\"type\":\"reject\""));
    CHECK(has(buf, "\"detail\":\"\",\"stage_ms\":12,\"total_ms\":34}"));
    CHECK(!has(buf, "null"));

    std::memset(&r, 0, sizeof r);
    n = write_reject_json(buf, sizeof buf, r);
    CHECK(n > 0);
    CHECK(has(buf, "\"stage_ms\":null"));
    CHECK(has(buf, "\"total_ms\":null"));

    // an independently cleared bit only nulls its own field
    r.stage_ms = 9;
    r.timing_present = kTimingStageMs;
    n = write_reject_json(buf, sizeof buf, r);
    CHECK(n > 0);
    CHECK(has(buf, "\"stage_ms\":9"));
    CHECK(has(buf, "\"total_ms\":null"));
}

static void test_connect_timing_json() {
    char buf[512];
    ConnectRecord c;
    std::memset(&c, 0, sizeof c);
    c.t = 2.0;
    c.id = 3;
    c.auth_ms = 1500;
    c.steam_ms = 45;
    c.timing_present = kTimingAuthMs | kTimingSteamMs;

    size_t n = write_connect_json(buf, sizeof buf, c);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    CHECK(has(buf, "\"type\":\"connect\""));
    CHECK(has(buf, "\"name\":\"\",\"auth_ms\":1500,\"steam_ms\":45}"));
    CHECK(!has(buf, "null"));

    std::memset(&c, 0, sizeof c);
    n = write_connect_json(buf, sizeof buf, c);
    CHECK(n > 0);
    CHECK(has(buf, "\"auth_ms\":null"));
    CHECK(has(buf, "\"steam_ms\":null"));

    c.auth_ms = 7;
    c.timing_present = kTimingAuthMs;
    n = write_connect_json(buf, sizeof buf, c);
    CHECK(n > 0);
    CHECK(has(buf, "\"auth_ms\":7"));
    CHECK(has(buf, "\"steam_ms\":null"));
}

static void test_mapchange_load_json() {
    char buf[256];
    MapChangeRecord m;
    std::memset(&m, 0, sizeof m);
    m.t = 10.0;
    std::strcpy(m.map, "de_nuke");
    m.load_ms = 250;
    m.load_present = 1;

    const size_t n = write_mapchange_json(buf, sizeof buf, m);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    CHECK(std::strcmp(buf,
                      "{\"type\":\"mapchange\",\"t\":10.000,\"map\":\"de_nuke\","
                      "\"load_ms\":250}") == 0);
    CHECK(buf[n] == '\0');

    m.load_ms = 0;
    m.load_present = 0;
    const size_t m2 = write_mapchange_json(buf, sizeof buf, m);
    CHECK(m2 > 0);
    CHECK(has(buf, "\"load_ms\":null"));

    char tiny[8];
    std::memset(tiny, 0x7f, sizeof tiny);
    CHECK(write_mapchange_json(tiny, sizeof tiny, m) == 0);
    CHECK(static_cast<unsigned char>(tiny[0]) == 0x7f);
}

int main() {
    check_layout_constants();
    test_timing_delta_ms();
    test_reject_timing_json();
    test_connect_timing_json();
    test_mapchange_load_json();

    if (g_fail == 0) std::printf("test_connect_timings: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
