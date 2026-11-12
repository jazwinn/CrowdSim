#pragma once

/// @file Id.hpp
/// Strongly typed integer IDs, used instead of pointers across module boundaries.

#include <compare>
#include <cstdint>
#include <functional>

namespace crowdsim::core {

/// A typed 32-bit identifier. `Tag` is an empty struct that makes IDs of
/// different kinds incompatible (a SpawnerId is not a GoalId).
template <typename Tag>
struct Id {
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
    std::uint32_t value = kInvalid;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != kInvalid; }
    constexpr auto operator<=>(const Id&) const noexcept = default;
};

}  // namespace crowdsim::core

template <typename Tag>
struct std::hash<crowdsim::core::Id<Tag>> {
    std::size_t operator()(crowdsim::core::Id<Tag> id) const noexcept { return std::hash<std::uint32_t>{}(id.value); }
};
