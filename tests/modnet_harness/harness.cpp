// HldsMonitoring — 32-bit synthetic GOT/PLT interposer harness (plan M-P5, MN-3).
//
// Links ONLY the host-testable core `src/modnet.cpp` and the header-free ELF/GOT
// adapter `src/modnet_got.cpp` (+ libc/dl). No engine header, no plugin. It
// dlopen()s the synthetic target objects (built by the Makefile), enables the
// interposer and exercises the end-to-end mechanics that 64-bit host tests
// cannot: attribution, lazy binding (C1), restore-on-disable, rescan and a
// fail-closed probe — plus non-interference of return value / errno.
//
// Everything is loopback/socketpair only; no network egress. The process exits
// non-zero on the first logical failure (accumulated in g_fail).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "modnet.h"
#include "modnet_got.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

using namespace hldsmon;

using send_fn      = ssize_t (*)(int, const void*, size_t, int);
using recv_fn      = ssize_t (*)(int, void*, size_t, int);
using connect_fn   = int (*)(int, const struct sockaddr*, socklen_t);
using accept_fn    = int (*)(int, struct sockaddr*, socklen_t*);
using gai_fn       = int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);

static const char kMsg[] = "hldsmon";

// ---- counter helpers (no public object-index-by-name API needed) ----
static void total_net(ModnetSnapshot& s) {
    s = ModnetSnapshot{};
    for (size_t i = 0; i < HLDSMON_MODNET_MAX_OBJECTS; ++i) {
        ModnetCounters* c = modnet_counters(static_cast<uint16_t>(i));
        if (c == nullptr) {
            continue;
        }
        ModnetSnapshot t;
        modnet_snapshot(*c, t);
        s.in_bytes += t.in_bytes;
        s.out_bytes += t.out_bytes;
        s.conns += t.conns;
        s.conn_fail += t.conn_fail;
        s.connect_attempts += t.connect_attempts;
        s.dns_calls += t.dns_calls;
    }
}

// Asserts that a per-object emit item carries the expected `.so` label.
static void check_label(const char* want_so) {
    ModnetEmitItem items[8];
    const size_t n = modnet_got_collect_emit(items, 8, 1234.0);
    bool found = false;
    for (size_t i = 0; i < n; ++i) {
        if (std::strcmp(items[i].so, want_so) == 0) {
            found = true;
        }
    }
    CHECK(found);
}

struct RtResult {
    ssize_t send_rc = -1;
    int     send_errno = 0;
    ssize_t recv_rc = -1;
    int     recv_errno = 0;
};

// One send/recv round-trip over a fresh socketpair. errno is pre-seeded with a
// sentinel; a transparent wrapper must leave it untouched on success.
static RtResult do_roundtrip(send_fn s, recv_fn r, const char* msg, size_t n) {
    RtResult out;
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return out;
    }
    char rb[64];
    std::memset(rb, 0, sizeof rb);
    errno = 4242;
    out.send_rc = s(sv[0], msg, n, 0);
    out.send_errno = errno;
    errno = 4243;
    out.recv_rc = r(sv[1], rb, n, 0);
    out.recv_errno = errno;
    close(sv[0]);
    close(sv[1]);
    return out;
}

struct EagResult {
    ssize_t rc = -1;
    int     err = 0;
};

// recv() on an empty non-blocking fd must yield -1/EAGAIN through the wrapper,
// with errno neither erased nor replaced.
static EagResult do_eagain(recv_fn r) {
    EagResult out;
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return out;
    }
    int fl = fcntl(sv[0], F_GETFL, 0);
    (void)fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(sv[1], F_GETFL, 0);
    (void)fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);
    char rb[8];
    errno = 0;
    out.rc = r(sv[1], rb, sizeof rb, 0);
    out.err = errno;
    close(sv[0]);
    close(sv[1]);
    return out;
}

// Loopback TCP listener with a small (1) backlog, bound to an ephemeral port.
static int make_listener(struct sockaddr_in* out_addr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    socklen_t al = sizeof a;
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&a), &al) != 0) {
        close(fd);
        return -1;
    }
    *out_addr = a;
    return fd;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: harness <target_lazy.so> <target_now.so>\n");
        return 2;
    }

    char lazy_path[4096];
    char now_path[4096];
    if (realpath(argv[1], lazy_path) == nullptr) {
        std::printf("FAIL: realpath(%s)\n", argv[1]);
        return 2;
    }
    if (realpath(argv[2], now_path) == nullptr) {
        std::printf("FAIL: realpath(%s)\n", argv[2]);
        return 2;
    }

    // The adapter matches objects by basename; the harness itself and libc are
    // hard-excluded by the adapter.
    modnet_got_configure_targets("target_lazy.so target_now.so");

    void* hl = dlopen(lazy_path, RTLD_LAZY | RTLD_LOCAL);
    if (hl == nullptr) {
        std::printf("FAIL: dlopen(%s): %s\n", lazy_path, dlerror());
        return 1;
    }
    send_fn t_send = reinterpret_cast<send_fn>(dlsym(hl, "modnet_target_send"));
    recv_fn t_recv = reinterpret_cast<recv_fn>(dlsym(hl, "modnet_target_recv"));
    connect_fn t_connect = reinterpret_cast<connect_fn>(dlsym(hl, "modnet_target_connect"));
    accept_fn t_accept = reinterpret_cast<accept_fn>(dlsym(hl, "modnet_target_accept"));
    gai_fn t_gai = reinterpret_cast<gai_fn>(dlsym(hl, "modnet_target_getaddrinfo"));
    CHECK(t_send && t_recv && t_connect && t_accept && t_gai);
    if (!(t_send && t_recv && t_connect && t_accept && t_gai)) {
        return 1;
    }

    const size_t mlen = sizeof(kMsg); // includes trailing NUL

    // ---------------------------------------------------------------
    // QAS-M6 baseline while OFF: no counters move, no wrapper on path.
    // Also capture the OFF behavior used for the non-interference checks.
    // ---------------------------------------------------------------
    ModnetSnapshot off0;
    total_net(off0);
    CHECK(off0.in_bytes == 0 && off0.out_bytes == 0);
    CHECK(!modnet_got_status().enabled);

    const RtResult off_rt = do_roundtrip(t_send, t_recv, kMsg, mlen);
    const EagResult off_eag = do_eagain(t_recv);
    CHECK(off_rt.send_rc == static_cast<ssize_t>(mlen));
    CHECK(off_rt.recv_rc == static_cast<ssize_t>(mlen));
    CHECK(off_rt.send_errno == 4242 && off_rt.recv_errno == 4243);
    CHECK(off_eag.rc == -1 && off_eag.err == EAGAIN);

    ModnetSnapshot off1;
    total_net(off1);
    CHECK(off1.in_bytes == 0 && off1.out_bytes == 0);

    // ---------------------------------------------------------------
    // Enable: runs the fail-closed probe, then patches every target slot.
    // ---------------------------------------------------------------
    modnet_enable();
    {
        const ModnetState st = modnet_got_status();
        CHECK(st.enabled);
        CHECK(st.probe_ok);
        CHECK(st.patched_objects >= 1);
        CHECK(st.patched_slots >= 2);
        CHECK(st.lazy_bound); // default-lazy target is detected as lazy (QAS-M9)
    }

    // ---------------------------------------------------------------
    // QAS-M1 non-interference + QAS-M5 attribution: identical return/errno,
    // bytes attributed to the synthetic object (not libc).
    // ---------------------------------------------------------------
    ModnetSnapshot a1;
    total_net(a1);
    const RtResult on_rt = do_roundtrip(t_send, t_recv, kMsg, mlen);
    ModnetSnapshot a2;
    total_net(a2);
    CHECK(on_rt.send_rc == off_rt.send_rc);
    CHECK(on_rt.send_errno == off_rt.send_errno);
    CHECK(on_rt.recv_rc == off_rt.recv_rc);
    CHECK(on_rt.recv_errno == off_rt.recv_errno);
    CHECK(a2.out_bytes - a1.out_bytes == static_cast<uint64_t>(mlen));
    CHECK(a2.in_bytes - a1.in_bytes == static_cast<uint64_t>(mlen));
    check_label("target_lazy.so");

    const EagResult on_eag = do_eagain(t_recv);
    CHECK(on_eag.rc == off_eag.rc && on_eag.err == off_eag.err);

    // ---------------------------------------------------------------
    // QAS-M9 lazy/C1: >=2 consecutive calls keep firing the wrapper; the slot
    // is not re-resolved out from under the wrapper (no one-shot no-op).
    // ---------------------------------------------------------------
    {
        int sv[2] = {-1, -1};
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        char rb[64];
        for (int k = 0; k < 3; ++k) {
            ModnetSnapshot p;
            total_net(p);
            const ssize_t rc = t_send(sv[0], kMsg, mlen, 0);
            const ssize_t rr = t_recv(sv[1], rb, mlen, 0);
            ModnetSnapshot q;
            total_net(q);
            CHECK(rc == static_cast<ssize_t>(mlen));
            CHECK(rr == static_cast<ssize_t>(mlen));
            CHECK(q.out_bytes - p.out_bytes == static_cast<uint64_t>(mlen));
            CHECK(q.in_bytes - p.in_bytes == static_cast<uint64_t>(mlen));
        }
        close(sv[0]);
        close(sv[1]);
    }

    // ---------------------------------------------------------------
    // R1 connect classification + accept + getaddrinfo counters.
    // ---------------------------------------------------------------
    {
        // Blocking connect to a live listener => conns++ (and attempts++).
        struct sockaddr_in laddr;
        int lfd = make_listener(&laddr);
        CHECK(lfd >= 0);
        if (lfd >= 0) {
            int cfd = socket(AF_INET, SOCK_STREAM, 0);
            CHECK(cfd >= 0);
            ModnetSnapshot p;
            total_net(p);
            const int rc = t_connect(cfd, reinterpret_cast<struct sockaddr*>(&laddr), sizeof laddr);
            ModnetSnapshot q;
            total_net(q);
            CHECK(rc == 0);
            CHECK(q.conns - p.conns == 1);
            CHECK(q.connect_attempts - p.connect_attempts == 1);
            CHECK(q.conn_fail - p.conn_fail == 0);

            ModnetSnapshot pa;
            total_net(pa);
            const int afd = t_accept(lfd, nullptr, nullptr);
            ModnetSnapshot qa;
            total_net(qa);
            CHECK(afd >= 0);
            CHECK(qa.conns - pa.conns == 1);

            if (afd >= 0) {
                close(afd);
            }
            close(cfd);
            close(lfd);
        }
    }
    {
        // Blocking connect to a loopback port with no listener => conn_fail++.
        struct sockaddr_in dead;
        std::memset(&dead, 0, sizeof dead);
        dead.sin_family = AF_INET;
        dead.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dead.sin_port = htons(1);
        int cfd = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(cfd >= 0);
        ModnetSnapshot p;
        total_net(p);
        errno = 0;
        const int rc = t_connect(cfd, reinterpret_cast<struct sockaddr*>(&dead), sizeof dead);
        const int e = errno;
        ModnetSnapshot q;
        total_net(q);
        CHECK(rc == -1);
        CHECK(e == ECONNREFUSED);
        CHECK(q.conn_fail - p.conn_fail == 1);
        CHECK(q.connect_attempts - p.connect_attempts == 1);
        CHECK(q.conns - p.conns == 0);
        close(cfd);
    }
    {
        // Non-blocking connect into a saturated accept queue => EINPROGRESS
        // => pending: only connect_attempts grows (R1).
        struct sockaddr_in paddr;
        int pl = make_listener(&paddr);
        CHECK(pl >= 0);
        bool found = false;
        if (pl >= 0) {
            for (int i = 0; i < 128 && !found; ++i) {
                int c = socket(AF_INET, SOCK_STREAM, 0);
                CHECK(c >= 0);
                int fl = fcntl(c, F_GETFL, 0);
                (void)fcntl(c, F_SETFL, fl | O_NONBLOCK);
                ModnetSnapshot p;
                total_net(p);
                errno = 0;
                const int rc = t_connect(c, reinterpret_cast<struct sockaddr*>(&paddr), sizeof paddr);
                const int e = errno;
                ModnetSnapshot q;
                total_net(q);
                if (rc == -1 && e == EINPROGRESS) {
                    CHECK(q.connect_attempts - p.connect_attempts == 1);
                    CHECK(q.conns - p.conns == 0);
                    CHECK(q.conn_fail - p.conn_fail == 0);
                    found = true;
                }
                // Keep successful (backlog-filling) and in-progress sockets open;
                // only recycle outright failures to avoid a needless error path.
                if (rc == -1 && e != EINPROGRESS) {
                    close(c);
                }
            }
            CHECK(found);
            close(pl);
        }
    }
    {
        // getaddrinfo (numeric, no resolver egress) => dns_calls++ only.
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST;
        struct addrinfo* res = nullptr;
        ModnetSnapshot p;
        total_net(p);
        const int rc = t_gai("127.0.0.1", "80", &hints, &res);
        ModnetSnapshot q;
        total_net(q);
        CHECK(rc == 0);
        CHECK(q.dns_calls - p.dns_calls == 1);
        CHECK(q.in_bytes - p.in_bytes == 0);
        CHECK(q.out_bytes - p.out_bytes == 0);
        if (res != nullptr) {
            freeaddrinfo(res);
        }
    }

    // ---------------------------------------------------------------
    // QAS-M3 restore-on-disable: slots restored; behavior identical to OFF and
    // the counters no longer grow.
    // ---------------------------------------------------------------
    modnet_disable();
    {
        const ModnetState off = modnet_got_status();
        CHECK(!off.enabled);
        CHECK(off.patched_slots == 0);
    }
    const RtResult off_rt2 = do_roundtrip(t_send, t_recv, kMsg, mlen);
    CHECK(off_rt2.send_rc == off_rt.send_rc);
    CHECK(off_rt2.send_errno == off_rt.send_errno);
    CHECK(off_rt2.recv_rc == off_rt.recv_rc);
    CHECK(off_rt2.recv_errno == off_rt.recv_errno);

    ModnetSnapshot d1;
    total_net(d1);
    {
        int sv[2] = {-1, -1};
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        char rb[64];
        const ssize_t rc = t_send(sv[0], kMsg, mlen, 0);
        const ssize_t rr = t_recv(sv[1], rb, mlen, 0);
        (void)rr;
        CHECK(rc == static_cast<ssize_t>(mlen));
        close(sv[0]);
        close(sv[1]);
    }
    ModnetSnapshot d2;
    total_net(d2);
    CHECK(d2.in_bytes - d1.in_bytes == 0);
    CHECK(d2.out_bytes - d1.out_bytes == 0);

    // ---------------------------------------------------------------
    // QAS-M7 rescan: a target dlopen()ed AFTER enable is picked up on rescan
    // and starts being counted.
    // ---------------------------------------------------------------
    modnet_enable();
    CHECK(modnet_got_status().patched_objects == 1);

    void* hn = dlopen(now_path, RTLD_NOW | RTLD_LOCAL);
    CHECK(hn != nullptr);
    if (hn != nullptr) {
        send_fn n_send = reinterpret_cast<send_fn>(dlsym(hn, "modnet_target_send"));
        recv_fn n_recv = reinterpret_cast<recv_fn>(dlsym(hn, "modnet_target_recv"));
        CHECK(n_send && n_recv);
        if (n_send && n_recv) {
            const ModnetState pre = modnet_got_status();
            modnet_rescan();
            const ModnetState post = modnet_got_status();
            CHECK(post.patched_objects >= pre.patched_objects + 1);

            ModnetSnapshot p;
            total_net(p);
            int sv[2] = {-1, -1};
            CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
            char rb[64];
            const ssize_t rc = n_send(sv[0], kMsg, mlen, 0);
            const ssize_t rr = n_recv(sv[1], rb, mlen, 0);
            ModnetSnapshot q;
            total_net(q);
            CHECK(rc == static_cast<ssize_t>(mlen));
            CHECK(rr == static_cast<ssize_t>(mlen));
            CHECK(q.out_bytes - p.out_bytes == static_cast<uint64_t>(mlen));
            CHECK(q.in_bytes - p.in_bytes == static_cast<uint64_t>(mlen));
            close(sv[0]);
            close(sv[1]);
            check_label("target_now.so");
        }
    }

    // ---------------------------------------------------------------
    // QAS-M2 fail-closed: no matching relocations => probe fails, stays OFF,
    // no crash.
    // ---------------------------------------------------------------
    modnet_got_configure_targets("absent_synthetic_target.so");
    modnet_disable();
    modnet_enable();
    {
        const ModnetState fc = modnet_got_status();
        CHECK(!fc.enabled);
        CHECK(!fc.probe_ok);
    }

    if (g_fail == 0) {
        std::printf("test-modnet-harness: PASS\n");
    } else {
        std::printf("test-modnet-harness: %d FAILURE(S)\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
