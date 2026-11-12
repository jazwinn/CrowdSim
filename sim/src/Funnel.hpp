#pragma once

/// @file Funnel.hpp
/// String pulling over a short list of portals (simple stupid funnel algorithm,
/// same orientation conventions as Detour). Internal to sim/.

#include <DetourCommon.h>

#include <algorithm>
#include <cstdint>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace crowdsim::sim::funnel {

inline constexpr float kPortalShrink = 0.2f;  ///< m pulled in from each portal end

struct Corner {
    glm::vec3 point;
    std::uint32_t portalIndex;  ///< portal the corner lies on; == count when it is `end`
};

/// First corner of the string-pulled path through `count` portals towards `end`.
inline Corner firstCorner(glm::vec3 start, const glm::vec3* lefts, const glm::vec3* rights, std::uint32_t count,
                          glm::vec3 end) noexcept {
    const float apex[3] = {start.x, start.y, start.z};
    float portalLeft[3] = {start.x, start.y, start.z};
    float portalRight[3] = {start.x, start.y, start.z};
    std::uint32_t leftIndex = 0;
    std::uint32_t rightIndex = 0;
    for (std::uint32_t i = 0; i <= count; ++i) {
        const glm::vec3 l = i < count ? lefts[i] : end;
        const glm::vec3 r = i < count ? rights[i] : end;
        const float left[3] = {l.x, l.y, l.z};
        const float right[3] = {r.x, r.y, r.z};

        if (dtTriArea2D(apex, portalRight, right) <= 0.0f) {
            if (dtVequal(apex, portalRight) || dtTriArea2D(apex, portalLeft, right) > 0.0f) {
                dtVcopy(portalRight, right);
                rightIndex = i;
            } else {
                return {{portalLeft[0], portalLeft[1], portalLeft[2]}, leftIndex};
            }
        }
        if (dtTriArea2D(apex, portalLeft, left) >= 0.0f) {
            if (dtVequal(apex, portalLeft) || dtTriArea2D(apex, portalRight, left) < 0.0f) {
                dtVcopy(portalLeft, left);
                leftIndex = i;
            } else {
                return {{portalRight[0], portalRight[1], portalRight[2]}, rightIndex};
            }
        }
    }
    return {end, count};
}

/// Steering target along a corridor: the first corner, or the following one once
/// the first is reached (corners sit on portal ends; stalling there is common).
inline glm::vec3 steeringTarget(glm::vec3 start, const glm::vec3* lefts, const glm::vec3* rights, std::uint32_t count,
                                glm::vec3 end) noexcept {
    constexpr float kCornerSkipDistance = 0.1f;  // only once the corner is reached
    Corner c = firstCorner(start, lefts, rights, count, end);
    for (int k = 0; k < 2 && c.portalIndex < count; ++k) {
        const float dx = c.point.x - start.x;
        const float dz = c.point.z - start.z;
        if (dx * dx + dz * dz > kCornerSkipDistance * kCornerSkipDistance) {
            break;
        }
        const std::uint32_t next = c.portalIndex + 1;
        Corner after = firstCorner(c.point, lefts + next, rights + next, count - next, end);
        after.portalIndex += next;
        c = after;
    }
    return c.point;
}

/// Pull both ends of a portal inwards so string-pulled corners lie inside
/// walkable space instead of exactly on navmesh boundary vertices.
inline void shrinkPortal(glm::vec3& left, glm::vec3& right) noexcept {
    const float dx = right.x - left.x;
    const float dz = right.z - left.z;
    const float width = std::sqrt(dx * dx + dz * dz);
    if (width <= 1e-3f) {
        return;
    }
    const float t = std::min(kPortalShrink, 0.25f * width) / width;
    const glm::vec3 l = left;
    left = l + (right - l) * t;
    right = right + (l - right) * t;
}

}  // namespace crowdsim::sim::funnel
