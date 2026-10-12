// HldsMonitoring — cumulative counter delta with 32-bit wrap (plan Phase 1).
//
// Engine counters (packet_loss, choke, sequences, ...) are cumulative and are
// stored in unsigned 32-bit fields that wrap at 2^32. This computes the true
// forward increment across a wrap.

#ifndef HLDSMON_DELTA_H
#define HLDSMON_DELTA_H

#include <cstdint>

namespace hldsmon {

struct CounterDelta {
    // Returns (cur - prev) modulo 2^32. If the counter did not wrap this is the
    // plain non-negative difference; if it wrapped, the wrapped distance.
    [[nodiscard]] static constexpr uint32_t compute_delta(uint32_t prev,
                                                          uint32_t cur) noexcept {
        return cur >= prev
                   ? (cur - prev)
                   : (UINT32_MAX - prev) + cur + 1u;
    }
};

} // namespace hldsmon

#endif // HLDSMON_DELTA_H
