#pragma once

/// @file Hash.hpp
/// FNV-1a 64-bit hashing used for state hashes (determinism tests) and content keys.

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace crowdsim::core {

class Fnv1a64 {
public:
    constexpr void addBytes(std::span<const std::byte> bytes) noexcept {
        for (std::byte b : bytes) {
            hash_ ^= static_cast<std::uint64_t>(b);
            hash_ *= 0x100000001b3ULL;
        }
    }

    /// Hash the object representation of a trivially copyable value.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void add(const T& value) noexcept {
        addBytes(std::as_bytes(std::span<const T, 1>(&value, 1)));
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_ = 0xcbf29ce484222325ULL;
};

}  // namespace crowdsim::core
