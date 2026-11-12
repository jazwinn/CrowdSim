#pragma once

/// @file TriangleMesh.hpp
/// Indexed triangle soup with per-triangle surface flags and colours. The one
/// geometry format shared by navmesh building, physics and static rendering.

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

namespace crowdsim::geometry {

/// Per-triangle surface semantics (bit flags).
enum SurfaceFlags : std::uint8_t {
    kSurfaceNone = 0,
    kSurfaceWalkable = 1 << 0,  ///< may become navmesh if the slope allows
    kSurfaceObstacle = 1 << 1,  ///< blocks movement; never walkable
};

struct Bounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    [[nodiscard]] bool valid() const noexcept { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
};

struct TriangleMesh {
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> indices;         ///< 3 per triangle
    std::vector<std::uint8_t> triangleFlags;    ///< SurfaceFlags per triangle
    std::vector<std::uint32_t> triangleColors;  ///< 0xAABBGGRR per triangle (render only)

    [[nodiscard]] std::uint32_t triangleCount() const noexcept {
        return static_cast<std::uint32_t>(indices.size() / 3);
    }
    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }

    /// Append `other`, re-basing its indices.
    void append(const TriangleMesh& other);

    /// Axis-aligned bounds of all vertices (invalid bounds if empty).
    [[nodiscard]] Bounds bounds() const;

    void clear();
};

/// Flat-shaded, non-indexed vertex for rendering static geometry.
struct RenderVertex {
    float x, y, z;
    float nx, ny, nz;
    std::uint32_t abgr;
};

/// Expand to flat-shaded render vertices (3 per triangle, face normals).
[[nodiscard]] std::vector<RenderVertex> toRenderVertices(const TriangleMesh& mesh);

}  // namespace crowdsim::geometry
