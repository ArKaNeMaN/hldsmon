// HldsMonitoring — host tests for the modnet core (plan M-P1; QAS-M5/M8/M9).
// Pure logic: connect classifier (R1), range cache / bounded overflow, the
// snapshot/diff emitter helpers and the lock-free range publication.

#include "modnet.h"

#include <cerrno>
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
    // ---- R1: connect() classification ----
    CHECK(modnet_classify_connect(0, 0) == ConnectClass::Blocking);
    CHECK(modnet_classify_connect(-1, EINPROGRESS) == ConnectClass::Pending);
    CHECK(modnet_classify_connect(-1, EALREADY) == ConnectClass::Pending);
    CHECK(modnet_classify_connect(-1, ECONNREFUSED) == ConnectClass::Failed);
    CHECK(modnet_classify_connect(-1, ETIMEDOUT) == ConnectClass::Failed);
    CHECK(modnet_classify_connect(-1, 0) == ConnectClass::Failed);
    CHECK(modnet_classify_connect(7, 0) == ConnectClass::Failed);

    // ---- RangeCache: hit/miss/boundaries (INV-M5, QAS-M5) ----
    {
        RangeCache rc;
        rc.reset();
        CHECK(rc.lookup(0x1000u) == -1);
        CHECK(rc.add(0x1000u, 0x2000u, 3));
        CHECK(rc.add(0x2000u, 0x3000u, 4));
        CHECK(!rc.overflowed());
        CHECK(rc.lookup(0x1000u) == 3);
        CHECK(rc.lookup(0x1FFFu) == 3);
        CHECK(rc.lookup(0x2000u) == 4); // [lo, hi) boundary
        CHECK(rc.lookup(0x2FFFu) == 4);
        CHECK(rc.lookup(0x3000u) == -1);
        CHECK(rc.lookup(0x0FFFu) == -1);
    }

    // ---- RangeCache: bounded overflow fails closed ----
    {
        RangeCache rc;
        rc.reset();
        bool all_added = true;
        for (size_t i = 0; i < RangeCache::kCapacity; ++i) {
            all_added = all_added && rc.add(i * 16u, i * 16u + 8u, static_cast<int16_t>(i % 8u));
        }
        CHECK(all_added);
        CHECK(!rc.add(0x900000u, 0x900100u, 1));
        CHECK(rc.overflowed());
        CHECK(rc.lookup(8u) == -1); // fail-closed on every address
    }

    // ---- snapshot / diff ----
    {
        ModnetCounters ctr;
        ctr.add_out(2048);
        ctr.add_in(1024);
        ctr.add_conn();
        ctr.add_conn_fail();
        ctr.add_connect_attempt();
        ctr.add_connect_attempt();
        ctr.add_dns_call();

        ModnetSnapshot cur;
        modnet_snapshot(ctr, cur);
        CHECK(cur.out_bytes == 2048 && cur.in_bytes == 1024);
        CHECK(cur.conns == 1 && cur.conn_fail == 1);
        CHECK(cur.connect_attempts == 2 && cur.dns_calls == 1);

        ModnetSnapshot prev{};
        ModnetRecord r;
        std::memset(&r, 0, sizeof(r));
        modnet_diff(prev, cur, 1.0, r);
        CHECK(r.out_bytes == 2048 && r.in_bytes == 1024);
        CHECK(r.conns == 1 && r.conn_fail == 1 && r.connect_attempts == 2 && r.dns_calls == 1);
        CHECK(r.out_kbps > 1.999f && r.out_kbps < 2.001f); // 2048 / 1 / 1024
        CHECK(r.in_kbps > 0.999f && r.in_kbps < 1.001f);
        CHECK(modnet_has_activity(r));

        modnet_diff(prev, cur, 0.0, r); // dt <= 0 => kbps 0
        CHECK(r.in_kbps == 0.0f && r.out_kbps == 0.0f);
        CHECK(r.out_bytes == 2048);
    }

    // ---- diff is uint64 wrap-safe ----
    {
        ModnetSnapshot prev{};
        ModnetSnapshot cur{};
        prev.out_bytes = 0xFFFFFFFFFFFFFFFFull - 5ull;
        cur.out_bytes = 10ull;
        ModnetRecord r;
        std::memset(&r, 0, sizeof(r));
        modnet_diff(prev, cur, 1.0, r);
        CHECK(r.out_bytes == 16ull);
    }

    // ---- emission gate ----
    {
        ModnetRecord r;
        std::memset(&r, 0, sizeof(r));
        CHECK(!modnet_has_activity(r));
        r.dns_calls = 1;
        CHECK(modnet_has_activity(r));
        r.dns_calls = 0;
        r.conn_fail = 5;
        CHECK(modnet_has_activity(r));
    }

    // ---- counter storage bounds ----
    {
        CHECK(modnet_counters(0) != nullptr);
        CHECK(modnet_counters(static_cast<uint16_t>(HLDSMON_MODNET_MAX_OBJECTS - 1)) != nullptr);
        CHECK(modnet_counters(static_cast<uint16_t>(HLDSMON_MODNET_MAX_OBJECTS)) == nullptr);
    }

    // ---- lock-free range publication ----
    {
        RangeCache rc;
        rc.reset();
        CHECK(rc.add(0x4000u, 0x5000u, 7));
        modnet_ranges_publish(&rc);
        CHECK(modnet_ranges_lookup(0x4500u) == 7);
        CHECK(modnet_ranges_lookup(0x6000u) == -1);
        modnet_ranges_publish(nullptr);
        CHECK(modnet_ranges_lookup(0x4500u) == -1);
    }

    // ---- record size budget (QAS-M8) ----
    CHECK(sizeof(ModnetRecord) == 192);

    if (g_fail == 0) std::printf("test_modnet_core: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
