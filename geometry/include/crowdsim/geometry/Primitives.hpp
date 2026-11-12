#pragma once

/// @file Primitives.hpp
/// Parametric building blocks for environments: floors, walls, pillars,
/// ramps and stairs. Each primitive stands on its `position` (the centre of its
/// footprint at its lowest point) and is rotated about +Y by `rotationY`.
/// Local +Z is the "up-slope" direction for ramps and stairs.

#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/vec3.hpp>

#include "crowdsim/geometry/TriangleMesh.hpp"

namespace crowdsim::geometry {

enum class PrimitiveKind : std::uint8_t {
    Box,     ///< floor slab, wall, pillar, obstacle
    Ramp,    ///< wedge rising from local -Z (height 0) to +Z (height size.y)
    Stairs,  ///< `steps` boxes rising towards local +Z
    Plane,   ///< single upward-facing quad (size.y ignored)
    Mesh,    ///< external mesh (imported asset), placed by position/rotationY/size.x as scale
};

struct PrimitiveDesc {
    PrimitiveKind kind = PrimitiveKind::Box;
    glm::vec3 position{0.0f};  ///< centre of the footprint, at the bottom
    float rotationY = 0.0f;    ///< radians
    glm::vec3 size{1.0f};      ///< width (x), height (y), depth (z)
    std::uint32_t steps = 10;  ///< stairs only
    std::uint8_t flags = kSurfaceWalkable;
    std::uint32_t color = 0xffb4b4b4u;  ///< 0xAABBGGRR
};

/// Triangulate one primitive in world space. Faces are wound so that
/// cross(b - a, c - a) points outwards.
[[nodiscard]] TriangleMesh buildPrimitive(const PrimitiveDesc& desc);

/// Place a local-space mesh in the world: uniform `scale`, rotate about +Y, translate.
/// If `flagsOverride` is non-zero it replaces every triangle's surface flags.
[[nodiscard]] TriangleMesh transformMesh(const TriangleMesh& local, glm::vec3 position, float rotationY, float scale,
                                         std::uint8_t flagsOverride = 0);

[[nodiscard]] std::string_view primitiveKindName(PrimitiveKind kind);
[[nodiscard]] std::optional<PrimitiveKind> parsePrimitiveKind(std::string_view name);

}  // namespace crowdsim::geometry
