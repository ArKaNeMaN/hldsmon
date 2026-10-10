// HldsMonitoring — header-only bounded ring buffer (plan Phase 1).
//
// Fixed-capacity, value-stored (T buf_[N]) — zero heap allocations. Single
// writer / single reader model (design §10): push() from the server thread,
// at()/last()/size() from the same thread. No locks.

#ifndef HLDSMON_RING_BUFFER_H
#define HLDSMON_RING_BUFFER_H

#include <cstddef>

namespace hldsmon {

// Bounded ring. Logical order is oldest-first: at(0) is the oldest retained
// element, at(size()-1) is the newest. When full, push() overwrites the oldest.
template <class T, size_t N>
class RingBuffer {
    static_assert(N > 0, "RingBuffer capacity must be > 0");

public:
    RingBuffer() noexcept : head_(0), count_(0) {}

    // Append v, overwriting the oldest element when full. Never allocates.
    void push(const T& v) {
        buf_[head_] = v;
        head_ = (head_ + 1) % N;
        if (count_ < N) {
            ++count_;
        }
    }

    size_t size() const noexcept { return count_; }

    static constexpr size_t capacity() noexcept { return N; }

    // Logical access, oldest-first. Precondition: i < size().
    const T& at(size_t i) const { return buf_[physical(i)]; }
    T& at(size_t i) { return buf_[physical(i)]; }

    // Copy up to n newest elements into out[], oldest-first (newest last).
    // Returns the number of elements written (min(n, size())). No allocation.
    size_t last(size_t n, T* out) const {
        if (n > count_) {
            n = count_;
        }
        const size_t first = count_ - n; // logical index of oldest selected
        for (size_t k = 0; k < n; ++k) {
            out[k] = at(first + k);
        }
        return n;
    }

    void clear() noexcept {
        head_ = 0;
        count_ = 0;
    }

private:
    size_t physical(size_t logical) const {
        // Oldest element sits (head_ - count_) mod N.
        const size_t oldest = (head_ + N - count_) % N;
        return (oldest + logical) % N;
    }

    T buf_[N];
    size_t head_;
    size_t count_;
};

} // namespace hldsmon

#endif // HLDSMON_RING_BUFFER_H
