#pragma once

/// @file NavMesh.hpp
/// Tiled Recast/Detour navigation mesh built from scene geometry, plus the
/// lookup structures the locomotion layer needs: dense polygon indices, a
/// reverse adjacency graph for flow fields, wall segments for avoidance and a
/// pool of per-thread Detour queries.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include "crowdsim/geometry/TriangleMesh.hpp"

class dtNavMesh;
class dtNavMeshQuery;
class dtQueryFilter;

namespace spdlog {
class logger;
}

namespace crowdsim::core {
class JobSystem;
}

namespace crowdsim::sim {

using PolyRef = std::uint32_t;  ///< dtPolyRef (32-bit build of Detour)
inline constexpr std::uint32_t kNoPoly = 0xFFFFFFFFu;

struct NavBuildSettings {
    float cellSize = 0.2f;
    float cellHeight = 0.1f;
    float agentHeight = 1.9f;
    float agentRadius = 0.3f;  ///< navmesh is eroded by this much
    float agentMaxClimb = 0.45f;
    float agentMaxSlopeDegrees = 45.0f;
    float regionMinSize = 8.0f;
    float regionMergeSize = 20.0f;
    float edgeMaxLength = 12.0f;
    float edgeMaxError = 1.3f;
    int vertsPerPoly = 6;
    float detailSampleDistance = 6.0f;
    float detailSampleMaxError = 1.0f;
    int tileSizeCells = 64;
};

/// Connection that is not walkable surface: ladders, escalators, jumps.
struct OffMeshLink {
    glm::vec3 start{0.0f};
    glm::vec3 end{0.0f};
    float radius = 0.6f;
    bool bidirectional = true;
    /// Extra path cost (m) for routing through this link. Ladders and escalators
    /// are slow, single-file connections, so flow fields should only prefer them
    /// when they save a real detour.
    float extraCost = 25.0f;
};

/// A boundary edge of the navmesh (a wall, ledge or obstacle edge).
struct WallSegment {
    glm::vec3 a;
    glm::vec3 b;
};

struct NearestPoly {
    PolyRef ref;
    glm::vec3 point;
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    /// Build from `mesh`. Tiles are rasterised in parallel and added in a fixed
    /// order, so the result is deterministic. Returns false if nothing walkable
    /// was found (the mesh is then empty but queries are safe).
    bool build(const geometry::TriangleMesh& mesh, std::span<const OffMeshLink> links, const NavBuildSettings& settings,
               core::JobSystem& jobs, spdlog::logger& log);

    void clear();

    [[nodiscard]] bool valid() const noexcept { return polyCount_ > 0; }
    [[nodiscard]] std::uint32_t polyCount() const noexcept { return polyCount_; }
    [[nodiscard]] std::uint32_t tileCount() const noexcept { return tileCount_; }

    // --- Polygon indexing ----------------------------------------------------------
    /// Dense index in [0, polyCount) for a valid ref.
    [[nodiscard]] std::uint32_t denseIndex(PolyRef ref) const noexcept;
    [[nodiscard]] PolyRef refAt(std::uint32_t dense) const noexcept { return denseToRef_[dense]; }
    [[nodiscard]] glm::vec3 polyCenter(std::uint32_t dense) const noexcept { return polyCenter_[dense]; }
    [[nodiscard]] bool isOffMeshConnection(std::uint32_t dense) const noexcept { return offMesh_[dense] != 0; }
    /// Extra traversal cost of a polygon (non-zero for off-mesh links).
    [[nodiscard]] float extraCost(std::uint32_t dense) const noexcept { return extraCost_[dense]; }
    /// Polygons from which `dense` can be entered directly (reverse adjacency).
    [[nodiscard]] std::span<const std::uint32_t> predecessors(std::uint32_t dense) const noexcept;
    /// Connected-component label of a polygon (the smallest dense index in it).
    [[nodiscard]] std::uint32_t component(std::uint32_t dense) const noexcept { return component_[dense]; }
    /// True if `a` and `b` are valid refs on the same connected piece of navmesh.
    [[nodiscard]] bool connected(PolyRef a, PolyRef b) const noexcept;

    // --- Queries -------------------------------------------------------------------
    /// Per-thread query object; `threadIndex` < the job system's thread count.
    [[nodiscard]] dtNavMeshQuery& query(std::uint32_t threadIndex) const noexcept { return *queries_[threadIndex]; }
    [[nodiscard]] const dtQueryFilter& filter() const noexcept { return *filter_; }
    [[nodiscard]] const dtNavMesh* detour() const noexcept { return navMesh_.get(); }

    [[nodiscard]] std::optional<NearestPoly> findNearest(glm::vec3 p, glm::vec3 halfExtents,
                                                         std::uint32_t threadIndex = 0) const;

    /// Closest point on polygon `ref` to `p` (on its detail surface). Nullopt if `ref` is invalid.
    [[nodiscard]] std::optional<glm::vec3> closestPointOnPoly(PolyRef ref, glm::vec3 p,
                                                              std::uint32_t threadIndex = 0) const;

    /// Left/right endpoints of the portal from `from` into `to` (as seen when moving from -> to).
    bool portal(PolyRef from, PolyRef to, glm::vec3& left, glm::vec3& right, std::uint32_t threadIndex = 0) const;

    /// Visit wall segments whose bounding box comes within `radius` of `p` (in XZ).
    template <typename Visit>
    void queryWalls(glm::vec3 p, float radius, Visit&& visit) const {
        if (wallGridW_ == 0) {
            return;
        }
        const int x0 = std::max(0, cellX(p.x - radius));
        const int x1 = std::min(wallGridW_ - 1, cellX(p.x + radius));
        const int z0 = std::max(0, cellZ(p.z - radius));
        const int z1 = std::min(wallGridH_ - 1, cellZ(p.z + radius));
        std::uint32_t seen[64];
        std::uint32_t numSeen = 0;
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const auto cell = static_cast<std::uint32_t>(z * wallGridW_ + x);
                for (std::uint32_t e = wallCellStart_[cell]; e < wallCellStart_[cell + 1]; ++e) {
                    const std::uint32_t w = wallEntries_[e];
                    bool dup = false;
                    for (std::uint32_t k = 0; k < numSeen && !dup; ++k) {
                        dup = seen[k] == w;
                    }
                    if (dup) {
                        continue;
                    }
                    if (numSeen < 64) {
                        seen[numSeen++] = w;
                    }
                    visit(walls_[w]);
                }
            }
        }
    }

    // --- Debug / rendering ---------------------------------------------------------
    [[nodiscard]] const std::vector<WallSegment>& walls() const noexcept { return walls_; }
    /// Detail-mesh triangles (3 vertices each), lifted slightly for display.
    [[nodiscard]] const std::vector<glm::vec3>& debugTriangles() const noexcept { return debugTriangles_; }
    [[nodiscard]] const std::vector<OffMeshLink>& offMeshLinks() const noexcept { return links_; }

private:
    void buildIndex();
    void buildWalls();
    [[nodiscard]] int cellX(float x) const noexcept { return static_cast<int>((x - wallOrigin_.x) / kWallCell); }
    [[nodiscard]] int cellZ(float z) const noexcept { return static_cast<int>((z - wallOrigin_.z) / kWallCell); }

    static constexpr float kWallCell = 2.0f;

    struct NavMeshDeleter {
        void operator()(dtNavMesh* p) const noexcept;
    };
    struct QueryDeleter {
        void operator()(dtNavMeshQuery* p) const noexcept;
    };

    std::unique_ptr<dtNavMesh, NavMeshDeleter> navMesh_;
    std::vector<std::unique_ptr<dtNavMeshQuery, QueryDeleter>> queries_;
    std::unique_ptr<dtQueryFilter> filter_;
    std::vector<OffMeshLink> links_;

    std::uint32_t polyCount_ = 0;
    std::uint32_t tileCount_ = 0;
    std::vector<std::uint32_t> tileOffset_;  ///< by tile index; dense = tileOffset + poly index
    std::vector<PolyRef> denseToRef_;
    std::vector<glm::vec3> polyCenter_;
    std::vector<std::uint8_t> offMesh_;
    std::vector<float> extraCost_;
    std::vector<std::uint32_t> predStart_;  ///< CSR reverse adjacency
    std::vector<std::uint32_t> predList_;
    std::vector<std::uint32_t> component_;

    std::vector<WallSegment> walls_;
    glm::vec3 wallOrigin_{0.0f};
    int wallGridW_ = 0;
    int wallGridH_ = 0;
    std::vector<std::uint32_t> wallCellStart_;
    std::vector<std::uint32_t> wallEntries_;

    std::vector<glm::vec3> debugTriangles_;
};

}  // namespace crowdsim::sim
