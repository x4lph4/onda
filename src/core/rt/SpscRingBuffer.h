#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace onda::rt {

/// Single-producer single-consumer lock-free ring buffer.
/// The producer thread may only call tryPush(), the consumer only tryPop().
/// One slot stays empty so full != empty. RT-safe: no allocation, no locks.
template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "RT ring buffers only carry trivially copyable types");
    static_assert(Capacity > 1);

public:
    bool tryPush(const T& value) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = increment(head);
        if (next == tail_.load(std::memory_order_acquire))
            return false; // full
        buffer_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(T& out) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false; // empty
        out = buffer_[tail];
        tail_.store(increment(tail), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    static constexpr size_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr size_t increment(size_t i) noexcept { return (i + 1) % Capacity; }

    std::array<T, Capacity> buffer_{};

#ifdef _MSC_VER
    // head_/tail_ live on separate cache lines to avoid false sharing;
    // the resulting struct padding is intentional.
    #pragma warning(push)
    #pragma warning(disable: 4324)
#endif
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
#ifdef _MSC_VER
    #pragma warning(pop)
#endif
};

} // namespace onda::rt
