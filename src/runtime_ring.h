// HldsMonitoring — runtime-sized bounded ring (plan Phase 3, INV-2).
//
// Unlike RingBuffer<T,N> (compile-time capacity, value storage), this ring owns
// a single heap buffer sized once at init(). After init() there are NO further
// allocations — push()/at()/last()/clear() only touch the preallocated storage.
// This keeps the hot path allocation-free while allowing capacities driven by
// cvars (hldsmon_client_ring / hldsmon_server_ring).
//
// Single writer / single reader model (design §10): the server thread owns the
// ring; no locks. Logical order is oldest-first: at(0) is the oldest retained
// element, at(size()-1) is the newest. When full, push() overwrites the oldest.

#ifndef HLDSMON_RUNTIME_RING_H
#define HLDSMON_RUNTIME_RING_H

#include <cstddef>
#include <new>

namespace hldsmon {

template <class T>
class RuntimeRing {
public:
    RuntimeRing() noexcept = default;
    ~RuntimeRing() { delete[] buf_; }

    RuntimeRing(const RuntimeRing&) = delete;
    RuntimeRing& operator=(const RuntimeRing&) = delete;

    // (Re)allocate storage for `cap` elements. This is the ONLY allocating
    // operation. Returns false only when cap > 0 and the allocation failed.
    // On success the ring is empty; on any call the previous buffer is freed.
    bool init(size_t cap) {
        delete[] buf_;
        buf_ = nullptr;
        cap_ = 0;
        head_ = 0;
        count_ = 0;
        if (cap == 0) {
            return true;
        }
        buf_ = new (std::nothrow) T[cap];
        if (buf_ == nullptr) {
            return false;
        }
        cap_ = cap;
        return true;
    }

    // Append v, overwriting the oldest element when full. Never allocates.
    void push(const T& v) {
        if (cap_ == 0) {
            return;
        }
        buf_[head_] = v;
        head_ = (head_ + 1) % cap_;
        if (count_ < cap_) {
            ++count_;
        }
    }

    size_t size() const noexcept { return count_; }
    size_t capacity() const noexcept { return cap_; }

    // Logical access, oldest-first. Precondition: i < size().
    const T& at(size_t i) const { return buf_[physical(i)]; }
    T& at(size_t i) { return buf_[physical(i)]; }

    // Copy up to n newest elements into out[], oldest-first (newest last).
    // Returns the number written (min(n, size())). No allocation.
    size_t last(size_t n, T* out) const {
        if (n > count_) {
            n = count_;
        }
        const size_t first = count_ - n;
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
        const size_t oldest = (head_ + cap_ - count_) % cap_;
        return (oldest + logical) % cap_;
    }

    T*     buf_ = nullptr;
    size_t cap_ = 0;
    size_t head_ = 0;
    size_t count_ = 0;
};

} // namespace hldsmon

#endif // HLDSMON_RUNTIME_RING_H
