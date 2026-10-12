// HldsMonitoring — host-testable modnet core (plan M-P1, design §19.6).
//
// Pure logic with no engine headers: per-object atomic counters, the
// connect()-result classifier (R1), the fixed-capacity address->object range
// cache (INV-M5) and the snapshot/diff helpers used by the 1 Hz emitter.
// Wrappers increment the counters through these free functions and never touch
// the server-thread-only PluginContext (INV-3 amend / MN-7).

#ifndef HLDSMON_MODNET_H
#define HLDSMON_MODNET_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "metrics.h"

namespace hldsmon {

// Fixed bounds (no allocation anywhere on the wrapper path).
constexpr size_t HLDSMON_MODNET_MAX_OBJECTS = 32;
constexpr size_t HLDSMON_MODNET_RANGE_MAX   = 128;

// Per-object cumulative atomic counters (INV-M4). Storage is a file-static
// array in modnet.cpp indexed by object slot.
struct ModnetCounters {
    std::atomic<uint64_t> in_bytes{0};
    std::atomic<uint64_t> out_bytes{0};
    std::atomic<uint64_t> conns{0};
    std::atomic<uint64_t> conn_fail{0};
    std::atomic<uint64_t> connect_attempts{0};
    std::atomic<uint64_t> dns_calls{0};

    void add_in(uint64_t n) noexcept { in_bytes.fetch_add(n, std::memory_order_relaxed); }
    void add_out(uint64_t n) noexcept { out_bytes.fetch_add(n, std::memory_order_relaxed); }
    void add_conn() noexcept { conns.fetch_add(1, std::memory_order_relaxed); }
    void add_conn_fail() noexcept { conn_fail.fetch_add(1, std::memory_order_relaxed); }
    void add_connect_attempt() noexcept { connect_attempts.fetch_add(1, std::memory_order_relaxed); }
    void add_dns_call() noexcept { dns_calls.fetch_add(1, std::memory_order_relaxed); }

    void reset() noexcept {
        in_bytes.store(0, std::memory_order_relaxed);
        out_bytes.store(0, std::memory_order_relaxed);
        conns.store(0, std::memory_order_relaxed);
        conn_fail.store(0, std::memory_order_relaxed);
        connect_attempts.store(0, std::memory_order_relaxed);
        dns_calls.store(0, std::memory_order_relaxed);
    }
};

// Fixed counter storage accessor; nullptr for an out-of-range object index.
ModnetCounters* modnet_counters(uint16_t obj) noexcept;

// R1: classify a connect() result. rc==0 => Blocking (counted in `conns`);
// rc<0 with errno EINPROGRESS/EALREADY => Pending (only `connect_attempts`);
// otherwise => Failed (`conn_fail`). Never fails on a positive rc.
enum class ConnectClass { Blocking, Pending, Failed };
[[nodiscard]] ConnectClass modnet_classify_connect(int rc, int err) noexcept;

// Fixed-capacity address->object range cache (INV-M5). `add` returns false on
// capacity overflow; `lookup` then fails closed (-1) for every address.
struct ModnetRangeEntry {
    uintptr_t lo;
    uintptr_t hi;
    int16_t   obj;
};

class RangeCache {
public:
    static constexpr size_t kCapacity = HLDSMON_MODNET_RANGE_MAX;

    void reset() noexcept {
        count_ = 0;
        overflowed_ = false;
    }

    bool add(uintptr_t lo, uintptr_t hi, int16_t obj) noexcept {
        if (count_ >= kCapacity) {
            overflowed_ = true;
            return false;
        }
        entries_[count_].lo = lo;
        entries_[count_].hi = hi;
        entries_[count_].obj = obj;
        ++count_;
        return true;
    }

    // Returns the object index containing `addr`, or -1 (miss or overflow).
    int16_t lookup(uintptr_t addr) const noexcept {
        if (overflowed_) {
            return -1;
        }
        for (size_t i = 0; i < count_; ++i) {
            const ModnetRangeEntry& e = entries_[i];
            if (addr >= e.lo && addr < e.hi) {
                return e.obj;
            }
        }
        return -1;
    }

    bool   overflowed() const noexcept { return overflowed_; }
    size_t count() const noexcept { return count_; }

private:
    ModnetRangeEntry entries_[kCapacity];
    size_t count_ = 0;
    bool   overflowed_ = false;
};

// Lock-free publish/read of the immutable range snapshot. Single writer (the
// server thread on rescan); readers are the wrappers on caller threads.
void    modnet_ranges_publish(const RangeCache* snapshot) noexcept;
int16_t modnet_ranges_lookup(uintptr_t addr) noexcept;

// Cumulative counter snapshot (consistent read of the atomic counters).
struct ModnetSnapshot {
    uint64_t in_bytes = 0;
    uint64_t out_bytes = 0;
    uint64_t conns = 0;
    uint64_t conn_fail = 0;
    uint64_t connect_attempts = 0;
    uint64_t dns_calls = 0;
};

void modnet_snapshot(const ModnetCounters& src, ModnetSnapshot& out) noexcept;

// Delta over [prev, cur] into a record (uint64 wrap-safe); kbps uses dt seconds
// (0.0 when dt<=0). Leaves `t`/`module`/`so` for the caller to set.
void modnet_diff(const ModnetSnapshot& prev, const ModnetSnapshot& cur,
                 double dt, ModnetRecord& out) noexcept;

// Emission gate: true when any delta in the record is non-zero.
bool modnet_has_activity(const ModnetRecord& r) noexcept;

} // namespace hldsmon

#endif // HLDSMON_MODNET_H
