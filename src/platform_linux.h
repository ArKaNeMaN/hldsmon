// HldsMonitoring — Linux platform helpers (plan Phase 3, design §4).
//
// Provides process resource accounting, monotonic/wall clock time and the
// engine module / interface-factory lookup used by bootstrap. Engine-TU only
// (it pulls interface.h for CSysModule/CreateInterfaceFn); host tests never
// include this header.

#ifndef HLDSMON_PLATFORM_LINUX_H
#define HLDSMON_PLATFORM_LINUX_H

#include <cstdint>

#include <extdll.h>    // EXT_FUNC et al. required by interface.h
#include <interface.h> // CSysModule, CreateInterfaceFn, Sys_GetModuleHandle/Sys_GetFactory

namespace hldsmon {

// Snapshot of getrusage(RUSAGE_SELF).
struct PlatformRusage {
    double   cpu_seconds; // ru_utime + ru_stime
    uint64_t minflt;      // cumulative minor faults
    uint64_t majflt;      // cumulative major faults
    uint32_t maxrss_kb;   // peak resident set size, KiB
};

// Fills `out`; returns false when getrusage fails (fields are zeroed).
bool platform_read_rusage(PlatformRusage* out) noexcept;

// Seconds from CLOCK_MONOTONIC (never goes backwards).
double platform_monotonic_seconds() noexcept;

// Seconds from CLOCK_REALTIME (Unix epoch, used for log rotation / t field).
double platform_wall_seconds() noexcept;

// Async-signal-safe crash markers (SIGSEGV/SIGABRT/SIGBUS/SIGFPE) written to a
// pre-opened <dir>/hldsmon-crash.marker fd. Best-effort; call once.
void platform_install_crash_handler(const char* dir) noexcept;

// Register the current log file's raw fd so the crash handler can append a
// `crash` JSONL line directly (bypassing the non-async-signal-safe LogWriter).
// The fd is dup()'d; pass -1 when the log is closed. Called from LogWriter via
// set_log_fd_observer. Safe to call repeatedly / before the handler is armed.
void platform_set_crash_log_fd(int fd) noexcept;

// Set once a graceful shutdown has been recorded; suppresses the crash record
// for teardown-time fatal signals (some servers abort() right after `quit`).
void platform_mark_graceful_shutdown() noexcept;

} // namespace hldsmon

#endif // HLDSMON_PLATFORM_LINUX_H
