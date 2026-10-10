// HldsMonitoring — host tests for the shutdown/crash writers and per-client
// identity serialization (plan work items A/B). Native g++ (no -m32, no
// engine headers), compiled by test-host-core from the pure core sources.

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

// metrics.h static_asserts are compile-time; these runtime checks pin the same
// numbers so a reader can see the ABI they guard.
static void check_layout_constants() {
    CHECK(sizeof(ShutdownRecord) == 24);
    CHECK(sizeof(CrashRecord) == 12);
    CHECK(sizeof(ClientSample) == 247);
    CHECK(sizeof(NetchanSnapshot) == 62);
    CHECK(sizeof(DropRecord) == 16413);
}

static ClientSample make_identified_client() {
    ClientSample c;
    std::memset(&c, 0, sizeof c);
    c.t = 2.0;
    c.id = 2;
    c.gen = 1712000000u;
    std::strcpy(c.name, "Ann \"Q\"");
    std::strcpy(c.authid, "STEAM_0:1:2");
    std::strcpy(c.steamid64, "76561197960265729");
    std::strcpy(c.ip, "1.2.3.4");
    c.port = 27015;
    c.userid = 7;
    c.slot = 3;
    c.conn_s = 12.5;
    c.fake = false;
    c.proxy = true;
    return c;
}

static void test_shutdown() {
    char buf[256];
    ShutdownRecord r;
    std::memset(&r, 0, sizeof r);
    r.t = 1.5;
    std::strcpy(r.reason, "graceful");

    const size_t n = write_shutdown_json(buf, sizeof buf, r);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    CHECK(std::strcmp(buf, "{\"type\":\"shutdown\",\"t\":1.500,\"reason\":\"graceful\"}") == 0);
    CHECK(buf[n] == '\0');

    // reason is escaped like any other JSON string value.
    ShutdownRecord e;
    std::memset(&e, 0, sizeof e);
    e.t = 0.0;
    std::strcpy(e.reason, "a\"b");
    const size_t m = write_shutdown_json(buf, sizeof buf, e);
    CHECK(m > 0);
    CHECK(std::strcmp(buf, "{\"type\":\"shutdown\",\"t\":0.000,\"reason\":\"a\\\"b\"}") == 0);

    // short buffer => nothing written, no partial line.
    char tiny[8];
    std::memset(tiny, 0x7f, sizeof tiny);
    CHECK(write_shutdown_json(tiny, sizeof tiny, r) == 0);
    CHECK(static_cast<unsigned char>(tiny[0]) == 0x7f);
}

static void test_crash() {
    char buf[256];
    CrashRecord r;
    std::memset(&r, 0, sizeof r);
    r.t = 12.0;
    r.signal = 11;

    const size_t n = write_crash_json(buf, sizeof buf, r);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    CHECK(std::strcmp(buf, "{\"type\":\"crash\",\"t\":12.000,\"signal\":11}") == 0);
    CHECK(buf[n] == '\0');

    char tiny[4];
    std::memset(tiny, 0x7f, sizeof tiny);
    CHECK(write_crash_json(tiny, sizeof tiny, r) == 0);
    CHECK(static_cast<unsigned char>(tiny[0]) == 0x7f);
}

static void test_client_identity() {
    char buf[2048];
    const ClientSample c = make_identified_client();

    const size_t n = write_client_json(buf, sizeof buf, c);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    CHECK(has(buf, "\"type\":\"client\""));
    CHECK(has(buf, "\"name\":\"Ann \\\"Q\\\"\""));
    CHECK(has(buf, "\"authid\":\"STEAM_0:1:2\""));
    CHECK(has(buf, "\"steamid64\":\"76561197960265729\""));
    CHECK(has(buf, "\"ip\":\"1.2.3.4\""));
    CHECK(has(buf, "\"port\":27015"));
    CHECK(has(buf, "\"userid\":7"));
    CHECK(has(buf, "\"slot\":3"));
    CHECK(has(buf, "\"conn_s\":12.500"));
    CHECK(has(buf, "\"fake\":false"));
    CHECK(has(buf, "\"proxy\":true"));

    // Unknown identity serializes with sentinels, never a partial line.
    ClientSample u;
    std::memset(&u, 0, sizeof u);
    u.t = 0.0;
    u.port = -1;
    u.userid = -1;
    const size_t m = write_client_json(buf, sizeof buf, u);
    CHECK(m > 0);
    CHECK(has(buf, "\"port\":-1"));
    CHECK(has(buf, "\"userid\":-1"));
    CHECK(has(buf, "\"name\":\"\""));
}

static void test_drop_identity() {
    char buf[8192];
    DropRecord d;
    std::memset(&d, 0, sizeof d);
    d.t = 100.0;
    d.id = 2;
    std::strcpy(d.reason, "timed out");
    std::strcpy(d.category, "timeout");
    std::strcpy(d.observed_via, "sv_dropclient");
    std::strcpy(d.name, "Bob");
    std::strcpy(d.authid, "STEAM_0:0:5");
    std::strcpy(d.steamid64, "76561197960265733");
    std::strcpy(d.ip, "10.0.0.9");
    d.port = 27005;
    d.userid = 12;
    d.slot = 3;
    d.conn_s = 42.0;
    d.fake = false;
    d.proxy = false;

    const size_t n = write_drop_json(buf, sizeof buf, d);
    CHECK(n > 0);
    CHECK(std::strlen(buf) == n);
    CHECK(has(buf, "\"type\":\"drop\""));
    CHECK(has(buf, "\"name\":\"Bob\""));
    CHECK(has(buf, "\"authid\":\"STEAM_0:0:5\""));
    CHECK(has(buf, "\"steamid64\":\"76561197960265733\""));
    CHECK(has(buf, "\"ip\":\"10.0.0.9\""));
    CHECK(has(buf, "\"port\":27005"));
    CHECK(has(buf, "\"slot\":3"));
    CHECK(has(buf, "\"conn_s\":42.000"));

    // Identity block precedes the nested "client" slice on the drop record.
    const char* idp = std::strstr(buf, "\"authid\":");
    const char* clp = std::strstr(buf, "\"client\":{");
    CHECK(idp != nullptr && clp != nullptr && idp < clp);
}

static void test_timeout_fields() {
    char buf[8192];
    ClientSample c;
    std::memset(&c, 0, sizeof c);
    c.t = 2.0;
    c.id = 4;
    c.msg_flags = 1;
    c.idle_ms = 2500;

    const size_t cn = write_client_json(buf, sizeof buf, c);
    CHECK(cn > 0);
    CHECK(has(buf, "\"idle_ms\":2500"));
    CHECK(has(buf, "\"msg_flags\":1,\"idle_ms\":2500,"));

    DropRecord d;
    std::memset(&d, 0, sizeof d);
    d.t = 100.0;
    d.id = 4;
    std::strcpy(d.reason, "Timed out");
    std::strcpy(d.category, "timeout");
    d.netchan.in_seq = 12345;
    d.netchan.in_ack = 12300;
    d.netchan.in_rel_ack = 1;
    d.netchan.in_rel_seq = 2;
    d.netchan.out_seq = 12000;
    d.netchan.rel_seq = 3;
    d.netchan.last_rel_seq = 3;
    d.netchan.rel_len = 128;
    d.netchan.rel_fill = 4;
    d.netchan.rate = 20000;
    d.netchan.cleartime = 1.5f;
    d.netchan.msg_flags = 1;
    d.netchan.last_received = 1000.0f;
    d.netchan.connect_time = 900.0f;
    d.netchan.now_real = 1002.5f;
    d.netchan.idle_ms = 2500;
    d.netchan.sv_timeout = 60.0f;

    const size_t n = write_drop_json(buf, sizeof buf, d);
    CHECK(n > 0);
    const char* expect =
        "\"netchan\":{\"in_seq\":12345,\"in_ack\":12300,\"in_rel_ack\":1,"
        "\"in_rel_seq\":2,\"out_seq\":12000,\"rel_seq\":3,\"last_rel_seq\":3,"
        "\"rel_len\":128,\"rel_fill\":4,\"rate\":20000,\"cleartime\":1.500,"
        "\"msg_flags\":1,\"last_received\":1000.000,\"connect_time\":900.000,"
        "\"now_real\":1002.500,\"idle_ms\":2500,\"sv_timeout\":60.000}";
    CHECK(has(buf, expect));
}

int main() {
    check_layout_constants();
    test_shutdown();
    test_crash();
    test_client_identity();
    test_drop_identity();
    test_timeout_fields();

    if (g_fail == 0) std::printf("test_serialization_events: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
