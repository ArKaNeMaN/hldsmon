// HldsMonitoring — header-free 32-bit ELF/GOT adapter (plan M-P2, design §19.3).
//
// This header is ENGINE-FREE (only modnet.h), so modnet_got.cpp links both into
// the plugin and into the 32-bit synthetic harness (M-P5, MN-3). The adapter
// owns all GOT state; the engine side mirrors it into PluginContext for
// `hldsmon_status` and the sampler.

#ifndef HLDSMON_MODNET_GOT_H
#define HLDSMON_MODNET_GOT_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "modnet.h"

namespace hldsmon {

// Mirror of the adapter state (plan M-P0). Fixed buffers, no pointers.
struct ModnetState {
    bool     enabled = false;
    bool     probe_ok = false;
    bool     lazy_bound = false;
    uint32_t patched_objects = 0;
    uint32_t patched_slots = 0;
    uint32_t restored_slots = 0;
    uint32_t conflicts = 0;
    int      target_count = 0;
    double   next_rescan_t = 0.0;
    char     targets[512] = {0};
    char     last_error[64] = {0};
};

// Configure the target basenames (space-separated; empty => built-in list).
// Call before enable. Safe to call repeatedly.
void modnet_got_configure_targets(const char* space_separated) noexcept;

const char* modnet_got_builtin_targets() noexcept;

// Probe: resolve originals, find relocations, verify mprotect + a socketpair
// round-trip through the wrapper with >=2 consecutive increments, then RESTORE
// every temporarily patched slot before returning (steady-state OFF is
// untouched, QAS-M6). Fail-closed: any failure => false, no crash.
bool modnet_probe() noexcept;

// Enable: run the probe (the only probe call site), then patch all eligible
// target objects. No-op when already enabled.
void modnet_enable() noexcept;

// Disable: restore every patched slot (re-validate + compare-then-store).
void modnet_disable() noexcept;

// Rescan: pick up newly loaded objects, re-verify (policy (b)) and re-patch any
// clobbered slot, rebuild+republish the range cache. No-op when disabled.
void modnet_rescan() noexcept;

// Request an immediate rescan on the next server tick (SV_Spawn_f trigger).
void modnet_request_rescan() noexcept;

// Consume a pending request set by modnet_request_rescan().
bool modnet_rescan_requested() noexcept;

ModnetState modnet_got_status() noexcept;

// One 1 Hz emitter item per patched object (server thread only).
struct ModnetEmitItem {
    char           so[64];
    char           module[64];
    ModnetSnapshot prev;
    ModnetSnapshot cur;
    double         dt;
};

// Fills up to out.size() items and advances the stored per-object baselines.
// Returns the number of items written.
size_t modnet_got_collect_emit(std::span<ModnetEmitItem> out, double now) noexcept;

} // namespace hldsmon

#endif // HLDSMON_MODNET_GOT_H
