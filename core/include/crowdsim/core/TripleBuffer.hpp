#pragma once

/// @file TripleBuffer.hpp
/// Single-producer / single-consumer triple buffer. The producer always has a
/// slot to write into and never waits; the consumer always reads the most
/// recently published complete value. Used to hand simulation snapshots to
/// the renderer without either side blocking the other.

#include <array>
#include <atomic>
#include <cstdint>

namespace crowdsim::core {

template <typename T>
class TripleBuffer {
public:
    /// Slot the producer may write into. Stays valid until publish().
    T& writeSlot() noexcept { return slots_[write_]; }

    /// Make the write slot the latest value and take a fresh slot to write into.
    void publish() noexcept {
        const std::uint8_t prev =
            middle_.exchange(static_cast<std::uint8_t>(write_ | kDirty), std::memory_order_acq_rel);
        write_ = prev & kIndexMask;
    }

    /// Latest published value. Returns true if it changed since the last call.
    bool acquire() noexcept {
        if ((middle_.load(std::memory_order_relaxed) & kDirty) == 0) {
            return false;
        }
        const std::uint8_t prev = middle_.exchange(read_, std::memory_order_acq_rel);
        read_ = prev & kIndexMask;
        return true;
    }

    /// The value obtained by the last acquire(). Stays valid until the next acquire().
    [[nodiscard]] const T& readSlot() const noexcept { return slots_[read_]; }
    [[nodiscard]] T& readSlot() noexcept { return slots_[read_]; }

private:
    static constexpr std::uint8_t kDirty = 0x4;
    static constexpr std::uint8_t kIndexMask = 0x3;

    std::array<T, 3> slots_{};
    std::uint8_t write_ = 0;               // owned by the producer
    std::atomic<std::uint8_t> middle_{1};  // shared hand-off slot (+ dirty bit)
    std::uint8_t read_ = 2;                // owned by the consumer
};

}  // namespace crowdsim::core
