#include "runtime_ring.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

using hldsmon::RuntimeRing;

static int g_fail = 0;
static std::size_t g_allocs = 0;

void* operator new(std::size_t n) { ++g_allocs; return std::malloc(n ? n : 1); }
void* operator new[](std::size_t n) { ++g_allocs; return std::malloc(n ? n : 1); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { ++g_allocs; return std::malloc(n ? n : 1); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { ++g_allocs; return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

static void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_fail;
    }
}

int main() {
    {
        RuntimeRing<int> r;
        check(r.capacity() == 0, "default capacity 0");
        check(r.size() == 0, "default size 0");
        r.push(1);
        check(r.size() == 0, "push into uninitialized is noop");
        int out[4] = {0, 0, 0, 0};
        check(r.last(4, out) == 0, "last on empty returns 0");
    }

    {
        RuntimeRing<int> r;
        const std::size_t before = g_allocs;
        check(r.init(3), "init(3) ok");
        check(g_allocs == before + 1, "init allocates once");
        check(r.capacity() == 3, "capacity 3");
        check(r.size() == 0, "empty after init");

        const std::size_t after_init = g_allocs;
        r.push(10);
        r.push(20);
        r.push(30);
        check(r.size() == 3, "size 3");
        check(r.at(0) == 10 && r.at(1) == 20 && r.at(2) == 30, "oldest-first order");

        r.push(40);
        check(r.size() == 3, "size stays capped");
        check(r.at(0) == 20 && r.at(1) == 30 && r.at(2) == 40, "overwrite oldest");
        check(g_allocs == after_init, "push does not allocate");

        int two[2] = {0, 0};
        check(r.last(2, two) == 2, "last(2) count");
        check(two[0] == 30 && two[1] == 40, "last(2) newest last");

        int all[8] = {0};
        check(r.last(8, all) == 3, "last(n>size) clamps");
        check(all[0] == 20 && all[1] == 30 && all[2] == 40, "last clamped content");

        r.clear();
        check(r.size() == 0, "clear empties");

        check(r.init(2), "re-init smaller ok");
        check(r.capacity() == 2, "capacity 2 after re-init");
        r.push(7);
        r.push(8);
        r.push(9);
        check(r.size() == 2 && r.at(0) == 8 && r.at(1) == 9, "re-init overwrite");

        check(r.init(0), "init(0) ok");
        check(r.capacity() == 0, "capacity 0 after init(0)");

        const std::size_t before2 = g_allocs;
        check(r.init(4), "init(4) from 0 ok");
        check(g_allocs == before2 + 1, "re-init allocates once");
        r.push(1);
        check(r.size() == 1 && r.at(0) == 1, "usable after re-init");
    }

    if (g_fail == 0) {
        std::printf("PASS\n");
    }
    return g_fail == 0 ? 0 : 1;
}
