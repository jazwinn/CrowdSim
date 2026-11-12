#include "crowdsim/sim/SpatialHash.hpp"

#include <bit>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Profile.hpp"

namespace crowdsim::sim {

void SpatialHash::build(std::span<const glm::vec2> positions, float cellSize, core::JobSystem& jobs) {
    CS_PROFILE_ZONE();
    cellSize_ = cellSize;
    invCellSize_ = 1.0f / cellSize;

    const auto n = static_cast<std::uint32_t>(positions.size());
    // ~2 buckets per point keeps chains short; at least 64.
    const std::uint32_t buckets = std::bit_ceil(std::max<std::uint32_t>(64, n * 2));
    mask_ = buckets - 1;

    bucketOfPoint_.resize(n);
    jobs.parallelFor(n, 2048, [&](core::IndexRange r, std::uint32_t) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            bucketOfPoint_[i] = bucketOf(cellCoord(positions[i].x), cellCoord(positions[i].y));
        }
    });

    // Counting sort in index order (deterministic).
    cellStart_.assign(buckets + 1, 0);
    for (std::uint32_t i = 0; i < n; ++i) {
        ++cellStart_[bucketOfPoint_[i] + 1];
    }
    for (std::uint32_t b = 0; b < buckets; ++b) {
        cellStart_[b + 1] += cellStart_[b];
    }
    entries_.resize(n);
    cursor_.assign(cellStart_.begin(), cellStart_.end() - 1);
    for (std::uint32_t i = 0; i < n; ++i) {
        entries_[cursor_[bucketOfPoint_[i]]++] = i;
    }
}

}  // namespace crowdsim::sim
