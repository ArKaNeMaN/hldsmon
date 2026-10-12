// HldsMonitoring — host tests for the Stage 9 (`event-enrich`) additions:
// drop.detector classification, packet.top_ips selection, server.edicts_churn
// serialization, and the degraded/retention types (design §26, plan EN-P0/P1).
//
// Native g++ (no -m32, no engine headers), compiled by test-host-core.

#include "metrics.h"
#include "serialization.h"

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

static bool has(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}

static void check_layout_constants() {
    CHECK(sizeof(ServerSample) == 150);
    CHECK(sizeof(DropRecord) == 16872);
    CHECK(sizeof(PacketStats) == 308);
    CHECK(sizeof(IpCount) == 68);
    CHECK(sizeof(DegradedRecord) == 16);
    CHECK(sizeof(RetentionRecord) == 12);
}

// Real rehlds_security SV_DropClient reason strings (QAS-E1/EN-3).
static void test_detector_match() {
    char buf[32];

    classify_detector("Kicked for move commands flooding (burst)", buf, sizeof buf);
    CHECK(std::strcmp(buf, "movecmd") == 0);
    classify_detector("Banned for move commands flooding (Avg)", buf, sizeof buf);
    CHECK(std::strcmp(buf, "movecmd") == 0);

    classify_detector("Kicked for string commands flooding (burst)", buf, sizeof buf);
    CHECK(std::strcmp(buf, "stringcmd") == 0);
    classify_detector("Banned for string commands flooding (Avg)", buf, sizeof buf);
    CHECK(std::strcmp(buf, "stringcmd") == 0);

    classify_detector("Kicked for dlfile flooding", buf, sizeof buf);
    CHECK(std::strcmp(buf, "dlfile") == 0);
    classify_detector("Banned for dlfile flooding", buf, sizeof buf);
    CHECK(std::strcmp(buf, "dlfile") == 0);

    classify_detector("Kicked for speedhack", buf, sizeof buf);
    CHECK(std::strcmp(buf, "speedhack") == 0);
    classify_detector("Banned for speedhack", buf, sizeof buf);
    CHECK(std::strcmp(buf, "speedhack") == 0);

    // Miss => empty; the drop record itself is unaffected (best-effort).
    classify_detector("Reliable channel overflowed", buf, sizeof buf);
    CHECK(buf[0] == '\0');
    classify_detector("Timed out", buf, sizeof buf);
    CHECK(buf[0] == '\0');
    classify_detector(nullptr, buf, sizeof buf);
    CHECK(buf[0] == '\0');
    classify_detector("Steam_NotifyClientDisconnect", buf, sizeof buf);
    CHECK(buf[0] == '\0');

    // Bounded copy never runs past the small buffer.
    char tiny[5];
    std::memset(tiny, 0x7f, sizeof tiny);
    classify_detector("Kicked for speedhack", tiny, sizeof tiny);
    CHECK(std::strcmp(tiny, "spee") == 0);
}

static void test_select_top_ips() {
    IpEntry tab[8];
    IpCount top[HLDSMON_TOP_IPS];

    std::memset(tab, 0, sizeof tab);
    std::memset(top, 0, sizeof top);
    CHECK(select_top_ips(nullptr, 8, top) == 0);
    CHECK(select_top_ips(tab, 8, top) == 0);   // all slots empty

    tab[0].ip = 0x0A000001u; tab[0].count = 5;   // 10.0.0.1
    tab[1].ip = 0x0A000002u; tab[1].count = 9;   // 10.0.0.2
    tab[3].ip = 0x0A000003u; tab[3].count = 9;   // 10.0.0.3 (tie, later slot)
    tab[4].ip = 0x0A000004u; tab[4].count = 1;   // 10.0.0.4
    tab[5].ip = 0x0A000005u; tab[5].count = 0;   // zero count => empty

    size_t n = select_top_ips(tab, 8, top);
    CHECK(n == 3);
    CHECK(top[0].count == 9 && std::strcmp(top[0].ip, "10.0.0.2") == 0);
    CHECK(top[1].count == 9 && std::strcmp(top[1].ip, "10.0.0.3") == 0);
    CHECK(top[2].count == 5 && std::strcmp(top[2].ip, "10.0.0.1") == 0);

    // A later leader replaces the smallest of the full top-3.
    tab[6].ip = 0x0A000006u; tab[6].count = 20;
    std::memset(top, 0, sizeof top);
    n = select_top_ips(tab, 8, top);
    CHECK(n == 3);
    CHECK(top[0].count == 20 && std::strcmp(top[0].ip, "10.0.0.6") == 0);
    CHECK(top[1].count == 9 && std::strcmp(top[1].ip, "10.0.0.2") == 0);
    CHECK(top[2].count == 9 && std::strcmp(top[2].ip, "10.0.0.3") == 0);

    // Fewer than N entries => only that many valid.
    IpEntry two[2];
    std::memset(two, 0, sizeof two);
    two[0].ip = 0x01020304u; two[0].count = 2;   // 1.2.3.4
    two[1].ip = 0x05060708u; two[1].count = 7;   // 5.6.7.8
    std::memset(top, 0, sizeof top);
    n = select_top_ips(two, 2, top);
    CHECK(n == 2);
    CHECK(std::strcmp(top[0].ip, "5.6.7.8") == 0);
    CHECK(std::strcmp(top[1].ip, "1.2.3.4") == 0);
}

static void test_server_churn_json() {
    char buf[1024];
    ServerSample s;
    std::memset(&s, 0, sizeof s);
    s.t = 3.0;
    s.edicts = 245;
    size_t n = write_server_json(buf, sizeof buf, s);
    CHECK(n > 0);
    CHECK(has(buf, "\"edicts\":245,\"edicts_churn\":null,\"map\":\"\""));

    s.edicts_churn = -7;
    s.edicts_churn_present = 1;
    n = write_server_json(buf, sizeof buf, s);
    CHECK(n > 0);
    CHECK(has(buf, "\"edicts\":245,\"edicts_churn\":-7,"));
}

static void test_packet_empty_top_ips() {
    char buf[1024];
    PacketStats p;
    std::memset(&p, 0, sizeof p);
    p.t = 1.0;
    const size_t n = write_packet_json(buf, sizeof buf, p);
    CHECK(n > 0);
    CHECK(has(buf, "\"top_ip\":\"\",\"top_ips\":[{\"ip\":\"\",\"count\":0},"
                   "{\"ip\":\"\",\"count\":0},{\"ip\":\"\",\"count\":0}]"));
}

static void test_drop_empty_detector() {
    char buf[8192];
    DropRecord d;
    std::memset(&d, 0, sizeof d);
    d.t = 1.0;
    std::strcpy(d.reason, "Timed out");
    std::strcpy(d.category, "timeout");
    std::strcpy(d.observed_via, "sv_dropclient");
    const size_t n = write_drop_json(buf, sizeof buf, d);
    CHECK(n > 0);
    CHECK(has(buf, "\"observed_via\":\"sv_dropclient\",\"detector\":\"\","));
}

static void test_degraded_retention_json() {
    char buf[128];

    DegradedRecord d;
    std::memset(&d, 0, sizeof d);
    d.t = 1.5;
    d.error_count = 7;
    size_t n = write_degraded_json(buf, sizeof buf, d);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    CHECK(std::strcmp(buf, "{\"type\":\"degraded\",\"t\":1.500,\"error_count\":7}") == 0);

    RetentionRecord r;
    std::memset(&r, 0, sizeof r);
    r.t = 2.0;
    r.removed = 3;
    n = write_retention_json(buf, sizeof buf, r);
    CHECK(n > 0);
    CHECK(std::strcmp(buf, "{\"type\":\"retention\",\"t\":2.000,\"removed\":3}") == 0);

    char tiny[8];
    std::memset(tiny, 0x7f, sizeof tiny);
    CHECK(write_degraded_json(tiny, sizeof tiny, d) == 0);
    CHECK(static_cast<unsigned char>(tiny[0]) == 0x7f);
    CHECK(write_retention_json(tiny, sizeof tiny, r) == 0);
}

int main() {
    check_layout_constants();
    test_detector_match();
    test_select_top_ips();
    test_server_churn_json();
    test_packet_empty_top_ips();
    test_drop_empty_detector();
    test_degraded_retention_json();

    if (g_fail == 0) std::printf("test_event_enrich: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
