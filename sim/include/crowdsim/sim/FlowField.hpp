#pragma once

/// @file FlowField.hpp
/// Per-goal flow field over navmesh polygons. Computed once per goal (Dijkstra
/// from the goal polygons over the reverse polygon graph) and shared by every
/// agent heading there. Each polygon stores its next polygon towards the goal
/// and the portal into it, so agents can string-pull a short look-ahead
/// corridor instead of running a path query.

#include <cstdint>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include "crowdsim/sim/NavMesh.hpp"

namespace crowdsim::sim {

struct Area;

struct FlowField {
    std::vector<float> cost;            ///< path length to the goal (inf if unreachable)
    std::vector<std::uint32_t> next;    ///< next dense polygon, or kNoPoly
    std::vector<std::uint8_t> source;   ///< 1 if the polygon is part of the goal
    std::vector<glm::vec3> portalLeft;  ///< portal from this polygon into `next`
    std::vector<glm::vec3> portalRight;

    [[nodiscard]] bool reachable(std::uint32_t dense) const noexcept {
        return dense < next.size() && (source[dense] != 0 || next[dense] != kNoPoly);
    }
    [[nodiscard]] std::uint32_t reachableCount() const noexcept;
};

/// Build the field for a goal area on `nav`. Deterministic (ties broken by index).
/// `climb` (optional, one entry per dense polygon) makes polygons one-way in height:
/// +1 = moves into or out of it may only go up (an up escalator), -1 = only down,
/// 0 = either way. `closed` (optional, per dense polygon) leaves polygons out entirely.
[[nodiscard]] FlowField buildFlowField(const NavMesh& nav, const Area& goalArea,
                                       std::span<const std::int8_t> climb = {},
                                       std::span<const std::uint8_t> closed = {});

}  // namespace crowdsim::sim
