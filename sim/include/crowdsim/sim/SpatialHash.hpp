#pragma once

/// @file SpatialHash.hpp
/// Uniform-grid spatial hash over ground-plane positions, rebuilt every step.
/// Build is a counting sort: bucket ids are computed in parallel, then counts,
/// prefix sums and the scatter run in index order, so the resulting entry order
/// (and therefore neighbour order) is deterministic.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace crowdsim::core {
class JobSystem;
}

namespace crowdsim::sim {

class SpatialHash {
public:
    /// Rebuild for `positions`. `cellSize` should be about the neighbour query radius.
    /// `heights` (optional, same size) is stored with each entry for queryPoints().
    void build(std::span<const glm::vec2> positions, float cellSize, core::JobSystem& jobs,
               std::span<const float> heights = {});

    /// Invoke `visit(index)` for every point whose cell overlaps the square of
    /// half-size `radius` around `p`. Candidates are not distance-filtered.
    /// Each candidate is visited once. Allocation-free.
    template <typename Visit>
    void query(glm::vec2 p, float radius, Visit&& visit) const {
        if (entries_.empty()) {
            return;
        }
        const int x0 = cellCoord(p.x - radius);
        const int x1 = cellCoord(p.x + radius);
        const int y0 = cellCoord(p.y - radius);
        const int y1 = cellCoord(p.y + radius);

        // Distinct cells can share a bucket; visit each bucket once.
        std::array<std::uint32_t, kMaxQueryCells> buckets{};
        std::uint32_t numBuckets = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const std::uint32_t b = bucketOf(x, y);
                if (std::find(buckets.begin(), buckets.begin() + numBuckets, b) != buckets.begin() + numBuckets) {
                    continue;
                }
                if (numBuckets == kMaxQueryCells) {
                    break;  // radius far larger than cell size; callers keep radius <= 2 cells
                }
                buckets[numBuckets++] = b;
            }
        }
        for (std::uint32_t k = 0; k < numBuckets; ++k) {
            const std::uint32_t b = buckets[k];
            for (std::uint32_t e = cellStart_[b]; e < cellStart_[b + 1]; ++e) {
                visit(entries_[e]);
            }
        }
    }

    /// Like query(), but passes each candidate's position and height as stored in the
    /// hash (contiguous, in bucket order) and skips candidates outside `radius`:
    /// `visit(index, position, height)`. Same candidates, same order as query().
    template <typename Visit>
    void queryPoints(glm::vec2 p, float radius, Visit&& visit) const {
        if (entries_.empty()) {
            return;
        }
        const float r2 = radius * radius;
        const int x0 = cellCoord(p.x - radius);
        const int x1 = cellCoord(p.x + radius);
        const int y0 = cellCoord(p.y - radius);
        const int y1 = cellCoord(p.y + radius);
        std::array<std::uint32_t, kMaxQueryCells> buckets{};
        std::uint32_t numBuckets = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const std::uint32_t b = bucketOf(x, y);
                if (std::find(buckets.begin(), buckets.begin() + numBuckets, b) != buckets.begin() + numBuckets) {
                    continue;
                }
                if (numBuckets == kMaxQueryCells) {
                    break;
                }
                buckets[numBuckets++] = b;
            }
        }
        for (std::uint32_t k = 0; k < numBuckets; ++k) {
            const std::uint32_t b = buckets[k];
            for (std::uint32_t e = cellStart_[b]; e < cellStart_[b + 1]; ++e) {
                const glm::vec3& q = entryPoints_[e];
                const float dx = q.x - p.x;
                const float dz = q.y - p.y;
                if (dx * dx + dz * dz < r2) {
                    visit(entries_[e], glm::vec2(q.x, q.y), q.z);
                }
            }
        }
    }

    [[nodiscard]] float cellSize() const noexcept { return cellSize_; }
    [[nodiscard]] std::uint32_t bucketCount() const noexcept { return mask_ + 1; }

private:
    static constexpr std::uint32_t kMaxQueryCells = 25;  // up to 5x5 cells

    [[nodiscard]] int cellCoord(float v) const noexcept { return static_cast<int>(std::floor(v * invCellSize_)); }
    [[nodiscard]] std::uint32_t bucketOf(int x, int y) const noexcept {
        const auto h = (static_cast<std::uint32_t>(x) * 73856093u) ^ (static_cast<std::uint32_t>(y) * 19349663u);
        return h & mask_;
    }

    float cellSize_ = 1.0f;
    float invCellSize_ = 1.0f;
    std::uint32_t mask_ = 0;
    std::vector<std::uint32_t> bucketOfPoint_;
    std::vector<std::uint32_t> cellStart_;  // size = buckets + 1
    std::vector<std::uint32_t> entries_;    // point indices grouped by bucket
    std::vector<glm::vec3> entryPoints_;    // (x, z, height) per entry, same order
    std::vector<std::uint32_t> cursor_;     // scatter scratch, kept to avoid reallocating
};

}  // namespace crowdsim::sim
