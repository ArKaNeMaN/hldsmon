// HldsMonitoring — Linux platform helpers (plan Phase 3).
//
// Sys_GetModuleHandle / Sys_GetFactory are declared by the vendored
// cssdk/public/interface.h but their implementation (interface.cpp) is not
// part of our build, so we provide them here with dlopen/dlsym. The version
// script keeps them local: * so they never leak from the plugin.

#include "platform_linux.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

namespace hldsmon {

bool platform_read_rusage(PlatformRusage* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    struct rusage ru;
    std::memset(&ru, 0, sizeof(ru));
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        out->cpu_seconds = 0.0;
        out->minflt = 0;
        out->majflt = 0;
        out->maxrss_kb = 0;
        return false;
    }
    out->cpu_seconds = static_cast<double>(ru.ru_utime.tv_sec) +
                       static_cast<double>(ru.ru_utime.tv_usec) / 1.0e6 +
                       static_cast<double>(ru.ru_stime.tv_sec) +
                       static_cast<double>(ru.ru_stime.tv_usec) / 1.0e6;
    out->minflt = static_cast<uint64_t>(ru.ru_minflt);
    out->majflt = static_cast<uint64_t>(ru.ru_majflt);
    out->maxrss_kb = static_cast<uint32_t>(ru.ru_maxrss);
    return true;
}

double platform_monotonic_seconds() noexcept {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1.0e9;
}

double platform_wall_seconds() noexcept {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0.0;
    }
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1.0e9;
}

namespace {

int g_crash_fd = -1;      // marker file
int g_crash_log_fd = -1;  // dup of the live log file (JSONL crash line)
volatile sig_atomic_t g_graceful_shutdown = 0;  // set by pfnGameShutdown

// Async-signal-safe unsigned decimal. Returns the number of bytes written.
size_t usig_dec(unsigned long v, char* out) {
    char tmp[24];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0) {
            tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10));
            v /= 10;
        }
    }
    for (int i = 0; i < n; ++i) {
        out[i] = tmp[n - 1 - i];
    }
    return static_cast<size_t>(n);
}

// Append the crash JSONL line to the live log fd. Only async-signal-safe calls
// (write/time). A leading '\n' guards against a partially written line.
void write_crash_json_line(int sig) {
    if (g_crash_log_fd < 0) {
        return;
    }
    char buf[80];
    size_t n = 0;
    const char* head = "\n{\"type\":\"crash\",\"t\":";
    for (const char* p = head; *p != '\0'; ++p) {
        buf[n++] = *p;
    }
    const time_t now = time(nullptr);
    n += usig_dec(now > 0 ? static_cast<unsigned long>(now) : 0UL, buf + n);
    const char* mid = ",\"signal\":";
    for (const char* p = mid; *p != '\0'; ++p) {
        buf[n++] = *p;
    }
    n += usig_dec(sig > 0 ? static_cast<unsigned long>(sig) : 0UL, buf + n);
    buf[n++] = '}';
    buf[n++] = '\n';
    const ssize_t rc = write(g_crash_log_fd, buf, n);
    (void)rc;
}

void crash_handler(int sig) {
    // Teardown-time signal after a graceful shutdown: not a crash.
    if (g_graceful_shutdown) {
        kill(getpid(), sig);
        return;
    }
    write_crash_json_line(sig);
    if (g_crash_fd >= 0) {
        char buf[32];
        size_t n = 0;
        const char prefix[] = "crash signal=";
        for (size_t i = 0; i < sizeof(prefix) - 1; ++i) {
            buf[n++] = prefix[i];
        }
        char digits[8];
        int k = 0;
        int v = sig > 0 ? sig : 0;
        if (v == 0) {
            digits[k++] = '0';
        } else {
            while (v > 0) {
                digits[k++] = static_cast<char>('0' + v % 10);
                v /= 10;
            }
        }
        while (k > 0) {
            buf[n++] = digits[--k];
        }
        buf[n++] = '\n';
        ssize_t rc = write(g_crash_fd, buf, n);
        (void)rc;
    }
    kill(getpid(), sig);
}

} // namespace

void platform_set_crash_log_fd(int fd) noexcept {
    if (fd >= 0) {
        const int dupfd = dup(fd);
        if (dupfd < 0) {
            return;
        }
        const int old = g_crash_log_fd;
        g_crash_log_fd = dupfd;
        if (old >= 0) {
            close(old);
        }
    } else {
        const int old = g_crash_log_fd;
        g_crash_log_fd = -1;
        if (old >= 0) {
            close(old);
        }
    }
}

void platform_mark_graceful_shutdown() noexcept {
    g_graceful_shutdown = 1;
}

void platform_install_crash_handler(const char* dir) noexcept {
    if (g_crash_fd >= 0 || dir == nullptr) {
        return;
    }
    char path[512];
    const int n = std::snprintf(path, sizeof(path), "%s/hldsmon-crash.marker", dir);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) {
        return;
    }
    const int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd < 0) {
        return;
    }
    g_crash_fd = fd;

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}

} // namespace hldsmon

// ---- engine module / factory (interface.h declarations) --------------------
//
// Prefer RTLD_NOLOAD so we only borrow a handle to the already-loaded engine;
// fall back to a normal load so the lookup still works on exotic layouts.

CSysModule* Sys_GetModuleHandle(const char* pModuleName) {
    if (pModuleName == nullptr) {
        return nullptr;
    }
    void* h = dlopen(pModuleName, RTLD_NOW | RTLD_NOLOAD);
    if (h == nullptr) {
        h = dlopen(pModuleName, RTLD_NOW);
    }
    return reinterpret_cast<CSysModule*>(h);
}

CreateInterfaceFn Sys_GetFactory(CSysModule* pModule) {
    if (pModule == nullptr) {
        return nullptr;
    }
    void* sym = dlsym(reinterpret_cast<void*>(pModule), CREATEINTERFACE_PROCNAME);
    return reinterpret_cast<CreateInterfaceFn>(sym);
}
