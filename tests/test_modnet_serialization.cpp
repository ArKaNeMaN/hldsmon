// HldsMonitoring — host tests for the modnet JSONL writer (plan M-P1).
// Verifies every field, the zero/null rules and the no-partial-line contract.

#include "serialization.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

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

int main() {
    char buf[1024];

    // ---- full record: every field ----
    {
        ModnetRecord r;
        std::memset(&r, 0, sizeof(r));
        r.t = 1712345678.123;
        std::strcpy(r.module, "easy_http_amxx_i386");
        std::strcpy(r.so, "easy_http_amxx_i386.so");
        r.conns = 3;
        r.conn_fail = 1;
        r.connect_attempts = 5;
        r.dns_calls = 2;
        r.in_bytes = 1024;
        r.out_bytes = 2048;
        r.in_kbps = 1.5f;
        r.out_kbps = 3.25f;

        const size_t n = write_modnet_json(buf, r);
        CHECK(n > 0);
        CHECK(std::strlen(buf) == n);
        CHECK(buf[0] == '{');
        CHECK(buf[n - 1] == '}');
        CHECK(has(buf, "\"type\":\"modnet\""));
        CHECK(has(buf, "\"t\":1712345678.123"));
        CHECK(has(buf, "\"module\":\"easy_http_amxx_i386\""));
        CHECK(has(buf, "\"so\":\"easy_http_amxx_i386.so\""));
        CHECK(has(buf, "\"conns\":3"));
        CHECK(has(buf, "\"conn_fail\":1"));
        CHECK(has(buf, "\"connect_attempts\":5"));
        CHECK(has(buf, "\"dns_calls\":2"));
        CHECK(has(buf, "\"in_bytes\":1024"));
        CHECK(has(buf, "\"out_bytes\":2048"));
        CHECK(has(buf, "\"in_kbps\":1.500"));
        CHECK(has(buf, "\"out_kbps\":3.250"));
    }

    // ---- zero record: counters are 0, never null ----
    {
        ModnetRecord z;
        std::memset(&z, 0, sizeof(z));
        const size_t n = write_modnet_json(buf, z);
        CHECK(n > 0);
        CHECK(has(buf, "\"conns\":0"));
        CHECK(has(buf, "\"conn_fail\":0"));
        CHECK(has(buf, "\"connect_attempts\":0"));
        CHECK(has(buf, "\"dns_calls\":0"));
        CHECK(has(buf, "\"in_bytes\":0"));
        CHECK(has(buf, "\"out_bytes\":0"));
        CHECK(has(buf, "\"in_kbps\":0.000"));
        CHECK(has(buf, "\"out_kbps\":0.000"));
        CHECK(!has(buf, "null"));
    }

    // ---- escaping of module/so ----
    {
        ModnetRecord e;
        std::memset(&e, 0, sizeof(e));
        std::strcpy(e.module, "a\"b\\c");
        std::strcpy(e.so, "tab\tname.so");
        const size_t n = write_modnet_json(buf, e);
        CHECK(n > 0);
        CHECK(has(buf, "\"module\":\"a\\\"b\\\\c\""));
        CHECK(has(buf, "\"so\":\"tab\\tname.so\""));
    }

    // ---- capacity handling: no partial line ----
    {
        ModnetRecord r;
        std::memset(&r, 0, sizeof(r));
        r.t = 1.0;
        std::strcpy(r.module, "sockets_amxx_i386");
        std::strcpy(r.so, "sockets_amxx_i386.so");
        r.in_bytes = 42;

        char big[1024];
        const size_t need = write_modnet_json(big, r);
        CHECK(need > 0);

        char sink[1024];
        std::memset(sink, 0x7f, sizeof sink);
        CHECK(write_modnet_json(std::span<char>(sink, need), r) == 0);
        bool touched = false;
        for (size_t i = 0; i < sizeof sink; ++i) {
            if (static_cast<unsigned char>(sink[i]) != 0x7f) {
                touched = true;
                break;
            }
        }
        CHECK(!touched);

        char zero[8];
        std::memset(zero, 0x7f, sizeof zero);
        CHECK(write_modnet_json(std::span<char>(zero, 0), r) == 0);

        char exact[1024];
        CHECK(write_modnet_json(std::span<char>(exact, need + 1), r) == need);
        CHECK(exact[need] == '\0');
    }

    if (g_fail == 0) std::printf("test_modnet_serialization: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
