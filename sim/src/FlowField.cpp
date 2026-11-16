#include "crowdsim/sim/FlowField.hpp"

#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

#include <glm/geometric.hpp>

#include "crowdsim/core/Profile.hpp"
#include "crowdsim/sim/Environment.hpp"

namespace crowdsim::sim {

std::uint32_t FlowField::reachableCount() const noexcept {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < next.size(); ++i) {
        n += reachable(i) ? 1u : 0u;
    }
    return n;
}

namespace {

/// Height change below which a move counts as level (sideways across an escalator).
constexpr float kLevelMove = 0.2f;

}  // namespace

FlowField buildFlowField(const NavMesh& nav, const Area& goalArea, std::span<const std::int8_t> climb) {
    CS_PROFILE_ZONE();
    FlowField f;
    const std::uint32_t n = nav.polyCount();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    f.cost.assign(n, kInf);
    f.next.assign(n, kNoPoly);
    f.source.assign(n, 0);
    f.portalLeft.assign(n, glm::vec3(0.0f));
    f.portalRight.assign(n, glm::vec3(0.0f));
    if (n == 0) {
        return f;
    }

    using Item = std::pair<float, std::uint32_t>;  // (cost, dense); ties resolve by index
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;

    for (std::uint32_t d = 0; d < n; ++d) {
        const glm::vec3 c = nav.polyCenter(d);
        if (!nav.isOffMeshConnection(d) && goalArea.contains3d(c, 1.5f)) {
            f.source[d] = 1;
            f.cost[d] = 0.0f;
            open.emplace(0.0f, d);
        }
    }
    // Small goals may contain no polygon centre: seed with the polygon under the goal centre.
    if (open.empty()) {
        if (auto nearest = nav.findNearest(goalArea.center3d(),
                                           {goalArea.halfExtents.x + 1.0f, 2.0f, goalArea.halfExtents.y + 1.0f})) {
            const std::uint32_t d = nav.denseIndex(nearest->ref);
            if (d != kNoPoly) {
                f.source[d] = 1;
                f.cost[d] = 0.0f;
                open.emplace(0.0f, d);
            }
        }
    }

    while (!open.empty()) {
        const auto [cost, u] = open.top();
        open.pop();
        if (cost > f.cost[u]) {
            continue;
        }
        const glm::vec3 cu = nav.polyCenter(u);
        for (std::uint32_t p : nav.predecessors(u)) {
            if (!climb.empty()) {
                // The move is p -> u. On a one-way polygon it must go the way the steps do.
                const std::int8_t way = climb[p] != 0 ? climb[p] : climb[u];
                const float rise = cu.y - nav.polyCenter(p).y;
                if ((way > 0 && rise < -kLevelMove) || (way < 0 && rise > kLevelMove)) {
                    continue;
                }
            }
            const float c = cost + glm::length(nav.polyCenter(p) - cu) + nav.extraCost(p);
            if (c < f.cost[p] || (c == f.cost[p] && u < f.next[p] && f.source[p] == 0)) {
                if (f.source[p] != 0) {
                    continue;
                }
                f.cost[p] = c;
                f.next[p] = u;
                open.emplace(c, p);
            }
        }
    }

    for (std::uint32_t d = 0; d < n; ++d) {
        if (f.next[d] != kNoPoly) {
            nav.portal(nav.refAt(d), nav.refAt(f.next[d]), f.portalLeft[d], f.portalRight[d]);
        }
    }
    return f;
}

}  // namespace crowdsim::sim
