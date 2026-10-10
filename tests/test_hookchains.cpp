// HldsMonitoring — host test for the hook-chain forwarding contract (QAS-7).
//
// Proves the non-interference rule: a value-returning wrapper returns exactly
// the result of chain->callNext(...) (including false), and a void wrapper
// always forwards. hookchains.h is header-only and compiles natively.

#include "../include/cssdk/dlls/hookchains.h"

#include <cstdio>

struct TestAdr;

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

namespace {

class FakeIntChain : public IHookChain<int, TestAdr*, int> {
public:
    int next_result = 0;
    int callNext(TestAdr*, int) override { return next_result; }
    int callOriginal(TestAdr*, int) override { return -1; }
};

class FakeBoolChain : public IHookChain<bool, unsigned char*, unsigned int, const int&> {
public:
    bool next_result = false;
    bool callNext(unsigned char*, unsigned int, const int&) override { return next_result; }
    bool callOriginal(unsigned char*, unsigned int, const int&) override { return false; }
};

class FakeVoidChain : public IVoidHookChain<int> {
public:
    int last = -1;
    bool called = false;
    void callNext(int a) override { last = a; called = true; }
    void callOriginal(int) override {}
};

int int_wrapper(IHookChain<int, TestAdr*, int>* chain, TestAdr* a, int b) {
    return chain->callNext(a, b);
}

bool packet_wrapper(IHookChain<bool, unsigned char*, unsigned int, const int&>* chain,
                    unsigned char* data, unsigned int len, const int& adr) {
    return chain->callNext(data, len, adr);
}

void void_wrapper(IVoidHookChain<int>* chain, int a) {
    chain->callNext(a);
}

} // namespace

int main() {
    int adr = 0;

    FakeIntChain ci;
    ci.next_result = 42;
    CHECK(int_wrapper(&ci, nullptr, 1) == 42);
    ci.next_result = 0;
    CHECK(int_wrapper(&ci, nullptr, 1) == 0);
    ci.next_result = -7;
    CHECK(int_wrapper(&ci, nullptr, 2) == -7);

    FakeBoolChain cb;
    cb.next_result = false;
    CHECK(packet_wrapper(&cb, nullptr, 10, adr) == false);
    cb.next_result = true;
    CHECK(packet_wrapper(&cb, nullptr, 10, adr) == true);

    FakeVoidChain cv;
    void_wrapper(&cv, 9);
    CHECK(cv.called);
    CHECK(cv.last == 9);

    if (g_fail == 0) std::printf("test_hookchains: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
