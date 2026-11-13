#include "crowdsim/sim/Orca.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/geometric.hpp>

namespace crowdsim::sim::orca {

namespace {

constexpr float kEpsilon = 1e-5f;

struct Line {
    glm::vec2 point;
    glm::vec2 direction;
};

/// Small fixed-capacity line set (the half-planes of the ORCA LP).
struct LineSet {
    static constexpr std::uint32_t kCapacity = kMaxNeighbors + kMaxWalls;
    std::array<Line, kCapacity> lines{};
    std::uint32_t count = 0;
    void push(const Line& l) noexcept {
        if (count < kCapacity) {
            lines[count++] = l;
        }
    }
};

inline float det(glm::vec2 a, glm::vec2 b) noexcept {
    return a.x * b.y - a.y * b.x;
}
inline float absSq(glm::vec2 v) noexcept {
    return glm::dot(v, v);
}

/// Solve the 1-D LP on line `lineNo`, constrained by lines [0, lineNo) and the speed disc.
bool linearProgram1(const LineSet& set, std::uint32_t lineNo, float radius, glm::vec2 optVelocity, bool directionOpt,
                    glm::vec2& result) noexcept {
    const Line& line = set.lines[lineNo];
    const float dotProduct = glm::dot(line.point, line.direction);
    const float discriminant = dotProduct * dotProduct + radius * radius - absSq(line.point);
    if (discriminant < 0.0f) {
        return false;  // max-speed disc fully invalidates this line
    }
    const float sqrtDisc = std::sqrt(discriminant);
    float tLeft = -dotProduct - sqrtDisc;
    float tRight = -dotProduct + sqrtDisc;

    for (std::uint32_t i = 0; i < lineNo; ++i) {
        const float denominator = det(line.direction, set.lines[i].direction);
        const float numerator = det(set.lines[i].direction, line.point - set.lines[i].point);
        if (std::fabs(denominator) <= kEpsilon) {
            if (numerator < 0.0f) {
                return false;  // parallel and infeasible
            }
            continue;
        }
        const float t = numerator / denominator;
        if (denominator >= 0.0f) {
            tRight = std::min(tRight, t);
        } else {
            tLeft = std::max(tLeft, t);
        }
        if (tLeft > tRight) {
            return false;
        }
    }

    if (directionOpt) {
        result = glm::dot(optVelocity, line.direction) > 0.0f ? line.point + tRight * line.direction
                                                              : line.point + tLeft * line.direction;
    } else {
        const float t = glm::dot(line.direction, optVelocity - line.point);
        if (t < tLeft) {
            result = line.point + tLeft * line.direction;
        } else if (t > tRight) {
            result = line.point + tRight * line.direction;
        } else {
            result = line.point + t * line.direction;
        }
    }
    return true;
}

/// Solve the 2-D LP. Returns the index of the first line that failed, or `set.count` on success.
std::uint32_t linearProgram2(const LineSet& set, float radius, glm::vec2 optVelocity, bool directionOpt,
                             glm::vec2& result) noexcept {
    if (directionOpt) {
        result = optVelocity * radius;  // optVelocity is a unit direction here
    } else if (absSq(optVelocity) > radius * radius) {
        result = glm::normalize(optVelocity) * radius;
    } else {
        result = optVelocity;
    }
    for (std::uint32_t i = 0; i < set.count; ++i) {
        if (det(set.lines[i].direction, set.lines[i].point - result) > 0.0f) {
            const glm::vec2 temp = result;
            if (!linearProgram1(set, i, radius, optVelocity, directionOpt, result)) {
                result = temp;
                return i;
            }
        }
    }
    return set.count;
}

/// Infeasible case: minimise the maximum penetration into the half-planes from `beginLine` on.
/// The first `numObstLines` lines (walls) stay hard constraints.
void linearProgram3(const LineSet& set, std::uint32_t numObstLines, std::uint32_t beginLine, float radius,
                    glm::vec2& result) noexcept {
    float distance = 0.0f;
    for (std::uint32_t i = beginLine; i < set.count; ++i) {
        const Line& li = set.lines[i];
        if (det(li.direction, li.point - result) <= distance) {
            continue;
        }
        LineSet projected;
        for (std::uint32_t j = 0; j < numObstLines; ++j) {
            projected.push(set.lines[j]);
        }
        for (std::uint32_t j = numObstLines; j < i; ++j) {
            const Line& lj = set.lines[j];
            Line line{};
            const float determinant = det(li.direction, lj.direction);
            if (std::fabs(determinant) <= kEpsilon) {
                if (glm::dot(li.direction, lj.direction) > 0.0f) {
                    continue;  // same direction
                }
                line.point = 0.5f * (li.point + lj.point);  // opposite direction
            } else {
                line.point = li.point + (det(lj.direction, li.point - lj.point) / determinant) * li.direction;
            }
            const glm::vec2 d = lj.direction - li.direction;
            const float len = std::sqrt(absSq(d));
            if (len <= kEpsilon) {
                continue;
            }
            line.direction = d / len;
            projected.push(line);
        }
        const glm::vec2 temp = result;
        if (linearProgram2(projected, radius, glm::vec2(-li.direction.y, li.direction.x), true, result) <
            projected.count) {
            result = temp;  // should not happen in principle; keep previous result
        }
        distance = det(li.direction, li.point - result);
    }
}

}  // namespace

float massResponsibility(float self, float other) noexcept {
    if (self <= 0.0f || other <= 0.0f || self == other) {
        return 0.5f;
    }
    const float r = std::min(self, other) / std::max(self, other);
    return self < other ? 1.0f - 0.5f * r : 0.5f * r;
}

glm::vec2 computeNewVelocity(const AgentState& self, std::span<const Neighbor> neighbors, std::span<const Wall> walls,
                             const Params& params) noexcept {
    LineSet set;
    const float invTimeHorizon = 1.0f / params.timeHorizon;
    const float invTimeStep = 1.0f / params.timeStep;

    // Walls: a hard half-plane per nearby segment limiting the velocity component
    // towards the segment's closest point, so the agent cannot reach it within
    // the obstacle time horizon (or is pushed out within one step if overlapping).
    for (const Wall& wall : walls) {
        const glm::vec2 ab = wall.b - wall.a;
        const float lenSq = absSq(ab);
        const float t = lenSq > kEpsilon ? std::clamp(glm::dot(self.position - wall.a, ab) / lenSq, 0.0f, 1.0f) : 0.0f;
        const glm::vec2 toWall = wall.a + t * ab - self.position;
        const float dist = std::sqrt(absSq(toWall));
        if (dist <= kEpsilon) {
            continue;
        }
        // The navmesh is already eroded by the agent radius, so boundary edges are
        // where agent centres may go: keep only a small clearance from them. An
        // agent already inside the clearance may not approach further but is not
        // pushed out either (hard push-outs pin agents in corners); the navmesh
        // move slides it along the boundary.
        const float gap = std::max(0.0f, dist - params.wallClearance);
        const glm::vec2 n = toWall / dist;
        const float maxApproach = gap / params.obstacleTimeHorizon;
        Line line{};
        line.point = n * maxApproach;
        line.direction = glm::vec2(-n.y, n.x);  // permitted side: v . n <= maxApproach
        set.push(line);
    }
    const std::uint32_t numObstLines = set.count;

    for (const Neighbor& other : neighbors) {
        const glm::vec2 relativePosition = other.position - self.position;
        const glm::vec2 relativeVelocity = self.velocity - other.velocity;
        const float distSq = absSq(relativePosition);
        const float combinedRadius = self.radius + other.radius;
        const float combinedRadiusSq = combinedRadius * combinedRadius;

        Line line{};
        glm::vec2 u{};

        if (distSq > combinedRadiusSq) {
            // No collision yet: cut-off centre of the truncated velocity-obstacle cone.
            const glm::vec2 w = relativeVelocity - invTimeHorizon * relativePosition;
            const float wLengthSq = absSq(w);
            const float dotProduct1 = glm::dot(w, relativePosition);

            if (dotProduct1 < 0.0f && dotProduct1 * dotProduct1 > combinedRadiusSq * wLengthSq) {
                // Project on the cut-off circle.
                const float wLength = std::sqrt(wLengthSq);
                const glm::vec2 unitW = w / wLength;
                line.direction = glm::vec2(unitW.y, -unitW.x);
                u = (combinedRadius * invTimeHorizon - wLength) * unitW;
            } else {
                // Project on a leg of the cone.
                const float leg = std::sqrt(distSq - combinedRadiusSq);
                const glm::vec2 rp = relativePosition;
                if (det(rp, w) > 0.0f) {
                    line.direction =
                        glm::vec2(rp.x * leg - rp.y * combinedRadius, rp.x * combinedRadius + rp.y * leg) / distSq;
                } else {
                    line.direction =
                        -glm::vec2(rp.x * leg + rp.y * combinedRadius, -rp.x * combinedRadius + rp.y * leg) / distSq;
                }
                const float dotProduct2 = glm::dot(relativeVelocity, line.direction);
                u = dotProduct2 * line.direction - relativeVelocity;
            }
        } else {
            // Already overlapping: resolve within one time step.
            glm::vec2 w = relativeVelocity - invTimeStep * relativePosition;
            float wLength = std::sqrt(absSq(w));
            if (wLength <= kEpsilon) {
                // Exact overlap with equal velocity: separate along a deterministic axis.
                w = glm::vec2(self.index < other.index ? -1.0f : 1.0f, 0.0f);
                wLength = 1.0f;
            }
            const glm::vec2 unitW = w / wLength;
            line.direction = glm::vec2(unitW.y, -unitW.x);
            u = (combinedRadius * invTimeStep - wLength) * unitW;
        }

        // Reciprocity: each agent takes half the responsibility.
        line.point = self.velocity + other.responsibility * u;
        set.push(line);
    }

    glm::vec2 result{};
    const std::uint32_t lineFail = linearProgram2(set, self.maxSpeed, self.preferredVelocity, false, result);
    if (lineFail < set.count) {
        linearProgram3(set, numObstLines, lineFail, self.maxSpeed, result);
    }
    return result;
}

}  // namespace crowdsim::sim::orca
