// HldsMonitoring — host tests for the retention core (plan R-P0; QAS-R1..R4/R8).
// Pure logic: date parsing/validation, day arithmetic and file selection.

#include "retention.h"

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

int main() {
    // ---- parse_log_date: exact template + calendar validation ----
    {
        int y = -1, m = -1, d = -1;
        CHECK(parse_log_date("hldsmon-2026-10-11.jsonl", &y, &m, &d));
        CHECK(y == 2026 && m == 10 && d == 11);
        CHECK(parse_log_date("hldsmon-2024-02-29.jsonl", &y, &m, &d));
        CHECK(parse_log_date("hldsmon-1970-01-01.jsonl", &y, &m, &d));
        CHECK(parse_log_date("hldsmon-9999-12-31.jsonl", &y, &m, &d));
        CHECK(parse_log_date("hldsmon-2026-10-11.jsonl", nullptr, nullptr, nullptr));

        CHECK(!parse_log_date("hldsmon-2026-02-29.jsonl", &y, &m, &d));
        CHECK(!parse_log_date("hldsmon-2026-13-01.jsonl", &y, &m, &d));
        CHECK(!parse_log_date("hldsmon-2026-00-01.jsonl", &y, &m, &d));
        CHECK(!parse_log_date("hldsmon-2026-04-31.jsonl", &y, &m, &d));
        CHECK(!parse_log_date("hldsmon-2026-04-00.jsonl", &y, &m, &d));
        CHECK(!parse_log_date("hldsmon-1969-12-31.jsonl", &y, &m, &d));

        CHECK(!parse_log_date("hldsmon-crash.marker", &y, &m, &d));
        CHECK(!parse_log_date("foo.jsonl", &y, &m, &d));
        CHECK(!parse_log_date("hldsmon-2026-1-01.jsonl", &y, &m, &d));
        CHECK(!parse_log_date("hldsmon-2026-10-011.jsonl", &y, &m, &d));
        CHECK(!parse_log_date("hldsmon-2026-10-11.json", &y, &m, &d));
        CHECK(!parse_log_date("xhldsmon-2026-10-11.jsonl", &y, &m, &d));
        CHECK(!parse_log_date("hldsmon-2026-10-11.jsonlx", &y, &m, &d));
        CHECK(!parse_log_date("", &y, &m, &d));
        CHECK(!parse_log_date(nullptr, &y, &m, &d));
    }

    // ---- days_from_civil: ordering, boundaries, leap years ----
    {
        CHECK(days_from_civil(1970, 1, 1) == 0);
        CHECK(days_from_civil(1970, 1, 2) == 1);
        CHECK(days_from_civil(1970, 2, 1) == 31);
        CHECK(days_from_civil(2024, 3, 1) - days_from_civil(2024, 2, 28) == 2);
        CHECK(days_from_civil(2023, 3, 1) - days_from_civil(2023, 2, 28) == 1);
        CHECK(days_from_civil(2026, 1, 1) - days_from_civil(2025, 12, 31) == 1);
        CHECK(days_from_civil(2025, 1, 1) - days_from_civil(2024, 1, 1) == 366);
        CHECK(days_from_civil(2024, 1, 1) - days_from_civil(2023, 1, 1) == 365);
    }

    // ---- N=3 keeps T,T-1,T-2; non-daily names ignored (QAS-R1/R4) ----
    {
        const char* names[] = {
            "hldsmon-2026-10-11.jsonl", // T
            "hldsmon-2026-10-10.jsonl", // T-1
            "hldsmon-2026-10-09.jsonl", // T-2
            "hldsmon-2026-10-08.jsonl", // T-3 -> delete
            "hldsmon-2026-10-07.jsonl", // T-4 -> delete
            "hldsmon-crash.marker",
            "foo.jsonl",
        };
        const char* out[16];
        size_t n = 12345;
        CHECK(retention_select(names, 7, 2026, 10, 11, 3, out, 16, &n));
        CHECK(n == 2);
        CHECK(std::strcmp(out[0], "hldsmon-2026-10-08.jsonl") == 0);
        CHECK(std::strcmp(out[1], "hldsmon-2026-10-07.jsonl") == 0);
    }

    // ---- month/year boundary: T=2026-01-01, N=3 ----
    {
        const char* names[] = {
            "hldsmon-2026-01-01.jsonl", // T
            "hldsmon-2025-12-31.jsonl", // T-1
            "hldsmon-2025-12-30.jsonl", // T-2
            "hldsmon-2025-12-29.jsonl", // T-3 -> delete
            "hldsmon-2025-12-28.jsonl", // T-4 -> delete
        };
        const char* out[8];
        size_t n = 0;
        CHECK(retention_select(names, 5, 2026, 1, 1, 3, out, 8, &n));
        CHECK(n == 2);
        CHECK(std::strcmp(out[0], "hldsmon-2025-12-29.jsonl") == 0);
        CHECK(std::strcmp(out[1], "hldsmon-2025-12-28.jsonl") == 0);
    }

    // ---- N=1: only today survives; current file never selected (QAS-R2) ----
    {
        const char* names[] = {
            "hldsmon-2026-10-11.jsonl", // T -> keep
            "hldsmon-2026-10-10.jsonl", // T-1 -> delete
        };
        const char* out[4];
        size_t n = 0;
        CHECK(retention_select(names, 2, 2026, 10, 11, 1, out, 4, &n));
        CHECK(n == 1);
        CHECK(std::strcmp(out[0], "hldsmon-2026-10-10.jsonl") == 0);
    }

    // ---- N<=0 disables retention: keep-all (QAS-R3) ----
    {
        const char* names[] = {
            "hldsmon-2020-01-01.jsonl",
            "hldsmon-2026-10-11.jsonl",
        };
        const char* out[4] = {nullptr, nullptr, nullptr, nullptr};
        size_t n = 999;
        CHECK(retention_select(names, 2, 2026, 10, 11, 0, out, 4, &n));
        CHECK(n == 0 && out[0] == nullptr);
        n = 999;
        CHECK(retention_select(names, 2, 2026, 10, 11, -5, out, 4, &n));
        CHECK(n == 0 && out[0] == nullptr);
    }

    // ---- future-dated file is kept (R-8, safe direction) ----
    {
        const char* names[] = {
            "hldsmon-2026-10-11.jsonl",
            "hldsmon-2026-12-31.jsonl",
        };
        const char* out[4];
        size_t n = 0;
        CHECK(retention_select(names, 2, 2026, 10, 11, 1, out, 4, &n));
        CHECK(n == 0);
    }

    // ---- out_cap overflow: false, *out_n untouched, no writes (QAS-R8/R-2) ----
    {
        const char* names[] = {
            "hldsmon-2026-10-08.jsonl",
            "hldsmon-2026-10-07.jsonl",
            "hldsmon-2026-10-06.jsonl",
        };
        static const char kA = 'A';
        static const char kB = 'B';
        const char* out[2] = {&kA, &kB};
        size_t n = 424242;
        CHECK(!retention_select(names, 3, 2026, 10, 11, 3, out, 2, &n));
        CHECK(n == 424242);
        CHECK(out[0] == &kA && out[1] == &kB);
    }

    // ---- exact-fit (candidates == out_cap) is allowed ----
    {
        const char* names[] = {
            "hldsmon-2026-10-08.jsonl",
            "hldsmon-2026-10-07.jsonl",
        };
        const char* out[2];
        size_t n = 0;
        CHECK(retention_select(names, 2, 2026, 10, 11, 3, out, 2, &n));
        CHECK(n == 2);
    }

    // ---- null entries / null names array tolerated ----
    {
        const char* names[] = {nullptr, "hldsmon-2026-10-07.jsonl"};
        const char* out[2];
        size_t n = 0;
        CHECK(retention_select(names, 2, 2026, 10, 11, 3, out, 2, &n));
        CHECK(n == 1);
        n = 7;
        CHECK(retention_select(nullptr, 0, 2026, 10, 11, 3, out, 2, &n));
        CHECK(n == 0);
    }

    // ---- null out_n reports failure ----
    {
        const char* names[] = {"hldsmon-2026-10-07.jsonl"};
        const char* out[2];
        CHECK(!retention_select(names, 1, 2026, 10, 11, 3, out, 2, nullptr));
    }

    if (g_fail == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAILED: %d check(s)\n", g_fail);
    return 1;
}
