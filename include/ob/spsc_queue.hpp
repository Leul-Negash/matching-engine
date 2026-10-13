#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace ob {

// Bounded lock-free queue for exactly one producer thread and one consumer
// thread (the gateway -> matching engine -> gateway hand-off).
//
// - No locks or allocation after construction, so neither side can block the
//   other or stall in the allocator.
// - Head and tail live on separate cache lines, so the two threads don't
//   invalidate each other's line on every operation (false sharing).
// - Each side caches the other side's index and re-reads the shared atomic
//   only when the cached value says the queue looks full/empty, so the common
//   case touches no shared cache line at all.
// - Indices increase forever (64-bit, never wrap in practice) and are masked
//   into a power-of-two buffer; full is tail - head == capacity.
//
// Memory ordering: the producer writes the slot, then publishes it with a
// release store of tail; the consumer's acquire load of tail makes the slot
// contents visible. The same pairing on head hands slots back.
template <class T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>, "elements are copied as plain data");

public:
    // Capacity is rounded up to a power of two.
    explicit SpscQueue(std::size_t min_capacity) {
        if (min_capacity == 0) throw std::invalid_argument("SpscQueue capacity must be positive");
        std::size_t cap = 1;
        while (cap < min_capacity) cap <<= 1;
        mask_ = cap - 1;
        buf_ = std::make_unique<T[]>(cap);
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    std::size_t capacity() const { return mask_ + 1; }

    // Producer only. Returns false if the queue is full.
    bool try_push(const T& value) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - producer_cached_head_ == capacity()) {
            producer_cached_head_ = head_.load(std::memory_order_acquire);
            if (t - producer_cached_head_ == capacity()) return false;
        }
        buf_[t & mask_] = value;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Pops up to `max` elements into `out`; returns how many.
    std::size_t try_pop(T* out, std::size_t max) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (consumer_cached_tail_ == h) {
            consumer_cached_tail_ = tail_.load(std::memory_order_acquire);
            if (consumer_cached_tail_ == h) return 0;
        }
        std::size_t n = consumer_cached_tail_ - h;
        if (n > max) n = max;
        for (std::size_t i = 0; i < n; ++i) out[i] = buf_[(h + i) & mask_];
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    bool try_pop(T& out) { return try_pop(&out, 1) == 1; }

    // Approximate when called concurrently; exact when both sides are idle.
    std::size_t size() const {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }
    bool empty() const { return size() == 0; }

private:
    static constexpr std::size_t kLine = 64;

    alignas(kLine) std::atomic<std::size_t> head_{0};   // next slot to read; written by consumer
    alignas(kLine) std::size_t consumer_cached_tail_ = 0;
    alignas(kLine) std::atomic<std::size_t> tail_{0};   // next slot to write; written by producer
    alignas(kLine) std::size_t producer_cached_head_ = 0;
    alignas(kLine) std::size_t mask_ = 0;
    std::unique_ptr<T[]> buf_;
};

}  // namespace ob
