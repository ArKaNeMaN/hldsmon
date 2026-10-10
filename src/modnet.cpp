// HldsMonitoring — host-testable modnet core implementation (plan M-P1).
//
// No engine headers, no allocation. Counter storage is a file-static array so
// wrappers can increment it from any thread without touching PluginContext.

#include "modnet.h"

#include <cerrno>

namespace hldsmon {
namespace {

ModnetCounters g_counters[HLDSMON_MODNET_MAX_OBJECTS];

std::atomic<const RangeCache*> g_published_ranges{nullptr};

} // namespace

ModnetCounters* modnet_counters(uint16_t obj) noexcept {
    if (obj >= HLDSMON_MODNET_MAX_OBJECTS) {
        return nullptr;
    }
    return &g_counters[obj];
}

ConnectClass modnet_classify_connect(int rc, int err) noexcept {
    if (rc == 0) {
        return ConnectClass::Blocking;
    }
    if (rc < 0 && (err == EINPROGRESS || err == EALREADY)) {
        return ConnectClass::Pending;
    }
    return ConnectClass::Failed;
}

void modnet_ranges_publish(const RangeCache* snapshot) noexcept {
    g_published_ranges.store(snapshot, std::memory_order_release);
}

int16_t modnet_ranges_lookup(uintptr_t addr) noexcept {
    const RangeCache* snap = g_published_ranges.load(std::memory_order_acquire);
    if (snap == nullptr) {
        return -1;
    }
    return snap->lookup(addr);
}

void modnet_snapshot(const ModnetCounters& src, ModnetSnapshot& out) noexcept {
    out.in_bytes = src.in_bytes.load(std::memory_order_relaxed);
    out.out_bytes = src.out_bytes.load(std::memory_order_relaxed);
    out.conns = src.conns.load(std::memory_order_relaxed);
    out.conn_fail = src.conn_fail.load(std::memory_order_relaxed);
    out.connect_attempts = src.connect_attempts.load(std::memory_order_relaxed);
    out.dns_calls = src.dns_calls.load(std::memory_order_relaxed);
}

void modnet_diff(const ModnetSnapshot& prev, const ModnetSnapshot& cur,
                 double dt, ModnetRecord& out) noexcept {
    out.in_bytes = cur.in_bytes - prev.in_bytes;
    out.out_bytes = cur.out_bytes - prev.out_bytes;
    out.conns = cur.conns - prev.conns;
    out.conn_fail = cur.conn_fail - prev.conn_fail;
    out.connect_attempts = cur.connect_attempts - prev.connect_attempts;
    out.dns_calls = cur.dns_calls - prev.dns_calls;

    if (dt > 0.0) {
        out.in_kbps = static_cast<float>(static_cast<double>(out.in_bytes) / dt / 1024.0);
        out.out_kbps = static_cast<float>(static_cast<double>(out.out_bytes) / dt / 1024.0);
    } else {
        out.in_kbps = 0.0f;
        out.out_kbps = 0.0f;
    }
}

bool modnet_has_activity(const ModnetRecord& r) noexcept {
    return r.in_bytes != 0 || r.out_bytes != 0 || r.conns != 0 ||
           r.conn_fail != 0 || r.connect_attempts != 0 || r.dns_calls != 0;
}

} // namespace hldsmon
