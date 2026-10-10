// HldsMonitoring — host test for RingBuffer (plan Phase 1).

#include "ring_buffer.h"

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
    using hldsmon::RingBuffer;

    // capacity / empty state
    {
        RingBuffer<int, 4> r;
        CHECK(r.capacity() == 4);
        CHECK(r.size() == 0);
    }

    // push preserves logical (oldest-first) order
    {
        RingBuffer<int, 4> r;
        r.push(10);
        r.push(20);
        r.push(30);
        CHECK(r.size() == 3);
        CHECK(r.at(0) == 10);
        CHECK(r.at(1) == 20);
        CHECK(r.at(2) == 30);
    }

    // duplicate values stay positionally distinct
    {
        RingBuffer<int, 4> r;
        r.push(7);
        r.push(7);
        r.push(8);
        CHECK(r.at(0) == 7);
        CHECK(r.at(1) == 7);
        CHECK(r.at(2) == 8);
    }

    // wrap: oldest overwritten, size capped at capacity
    {
        RingBuffer<int, 3> r;
        r.push(1);
        r.push(2);
        r.push(3);
        CHECK(r.size() == 3);
        r.push(4); // overwrites 1
        CHECK(r.size() == 3);
        CHECK(r.at(0) == 2);
        CHECK(r.at(1) == 3);
        CHECK(r.at(2) == 4);
        r.push(5); // overwrites 2
        CHECK(r.at(0) == 3);
        CHECK(r.at(1) == 4);
        CHECK(r.at(2) == 5);
    }

    // last(n) returns newest-last ordering
    {
        RingBuffer<int, 8> r;
        for (int i = 1; i <= 5; ++i) r.push(i);
        int out[8] = {0};
        const size_t n = r.last(3, out);
        CHECK(n == 3);
        CHECK(out[0] == 3);
        CHECK(out[1] == 4);
        CHECK(out[2] == 5);
    }

    // last(n) with n > size returns size, oldest-first
    {
        RingBuffer<int, 8> r;
        r.push(11);
        r.push(12);
        int out[8] = {0};
        const size_t n = r.last(5, out);
        CHECK(n == 2);
        CHECK(out[0] == 11);
        CHECK(out[1] == 12);
    }

    // last(0) returns 0
    {
        RingBuffer<int, 4> r;
        r.push(1);
        int out[4] = {0};
        CHECK(r.last(0, out) == 0);
    }

    // last(n) after wrap
    {
        RingBuffer<int, 4> r;
        for (int i = 1; i <= 6; ++i) r.push(i); // retains 3,4,5,6
        int out[4] = {0};
        const size_t n = r.last(2, out);
        CHECK(n == 2);
        CHECK(out[0] == 5);
        CHECK(out[1] == 6);
    }

    // clear resets to empty and re-usable
    {
        RingBuffer<int, 4> r;
        r.push(1);
        r.push(2);
        r.clear();
        CHECK(r.size() == 0);
        r.push(9);
        CHECK(r.size() == 1);
        CHECK(r.at(0) == 9);
    }

    // boundary: exactly-capacity writes stay valid
    {
        RingBuffer<int, 2> r;
        r.push(100);
        r.push(200);
        CHECK(r.at(0) == 100);
        CHECK(r.at(1) == 200);
        r.push(300);
        CHECK(r.at(0) == 200);
        CHECK(r.at(1) == 300);
    }

    if (g_fail == 0) std::printf("test_ring: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
