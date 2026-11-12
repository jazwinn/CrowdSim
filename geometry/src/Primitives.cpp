#include "crowdsim/geometry/Primitives.hpp"

#include <array>
#include <cmath>

#include <glm/geometric.hpp>

namespace crowdsim::geometry {

namespace {

/// Builds a mesh in local space and transforms it on the way out.
class MeshBuilder {
public:
    MeshBuilder(const PrimitiveDesc& desc) : desc_(desc), c_(std::cos(desc.rotationY)), s_(std::sin(desc.rotationY)) {}

    /// Add a planar quad (a, b, c, d in order around the edge). `outward` is the
    /// intended face direction in local space; winding is fixed up to match it.
    void quad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 outward, std::uint8_t flags) {
        if (glm::dot(glm::cross(b - a, c - a), outward) < 0.0f) {
            std::swap(b, d);
        }
        const auto base = static_cast<std::uint32_t>(mesh_.positions.size());
        for (const glm::vec3& p : {a, b, c, d}) {
            mesh_.positions.push_back(toWorld(p));
        }
        mesh_.indices.insert(mesh_.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        mesh_.triangleFlags.insert(mesh_.triangleFlags.end(), {flags, flags});
        const std::uint32_t color = shade(desc_.color, outward);
        mesh_.triangleColors.insert(mesh_.triangleColors.end(), {color, color});
    }

    /// Axis-aligned box in local space. Top face gets `topFlags`, sides `sideFlags`.
    void box(glm::vec3 min, glm::vec3 max, std::uint8_t topFlags, std::uint8_t sideFlags, bool bottom = true) {
        const glm::vec3 p000{min.x, min.y, min.z}, p100{max.x, min.y, min.z}, p110{max.x, max.y, min.z},
            p010{min.x, max.y, min.z};
        const glm::vec3 p001{min.x, min.y, max.z}, p101{max.x, min.y, max.z}, p111{max.x, max.y, max.z},
            p011{min.x, max.y, max.z};
        quad(p010, p110, p111, p011, {0, 1, 0}, topFlags);  // top
        if (bottom) {
            quad(p000, p001, p101, p100, {0, -1, 0}, sideFlags);
        }
        quad(p000, p100, p110, p010, {0, 0, -1}, sideFlags);  // -Z
        quad(p001, p011, p111, p101, {0, 0, 1}, sideFlags);   // +Z
        quad(p000, p010, p011, p001, {-1, 0, 0}, sideFlags);  // -X
        quad(p100, p101, p111, p110, {1, 0, 0}, sideFlags);   // +X
    }

    TriangleMesh take() { return std::move(mesh_); }

private:
    [[nodiscard]] glm::vec3 toWorld(glm::vec3 p) const {
        // Rotate about +Y (same convention as agent headings: +Z turns towards +X).
        return glm::vec3(c_ * p.x + s_ * p.z, p.y, -s_ * p.x + c_ * p.z) + desc_.position;
    }

    /// Cheap baked lighting so untextured geometry reads well without a light rig.
    static std::uint32_t shade(std::uint32_t abgr, glm::vec3 n) {
        const float k = n.y > 0.5f ? 1.0f : (n.y < -0.5f ? 0.55f : (std::fabs(n.x) > 0.5f ? 0.78f : 0.88f));
        auto ch = [&](int shift) {
            const auto v = static_cast<float>((abgr >> shift) & 0xffu) * k;
            return static_cast<std::uint32_t>(std::min(255.0f, v)) << shift;
        };
        return (abgr & 0xff000000u) | ch(16) | ch(8) | ch(0);
    }

    const PrimitiveDesc& desc_;
    float c_;
    float s_;
    TriangleMesh mesh_;
};

}  // namespace

TriangleMesh buildPrimitive(const PrimitiveDesc& desc) {
    MeshBuilder mb(desc);
    const glm::vec3 h = desc.size * 0.5f;
    const bool obstacle = (desc.flags & kSurfaceObstacle) != 0;
    const std::uint8_t top = obstacle ? static_cast<std::uint8_t>(kSurfaceObstacle) : desc.flags;
    const std::uint8_t side = obstacle ? static_cast<std::uint8_t>(kSurfaceObstacle) : kSurfaceNone;

    switch (desc.kind) {
        case PrimitiveKind::Box:
            mb.box({-h.x, 0.0f, -h.z}, {h.x, desc.size.y, h.z}, top, side);
            break;
        case PrimitiveKind::Plane:
            mb.quad({-h.x, 0.0f, -h.z}, {h.x, 0.0f, -h.z}, {h.x, 0.0f, h.z}, {-h.x, 0.0f, h.z}, {0, 1, 0}, top);
            break;
        case PrimitiveKind::Ramp: {
            const float y = desc.size.y;
            const glm::vec3 a{-h.x, 0.0f, -h.z}, b{h.x, 0.0f, -h.z}, c{h.x, y, h.z}, d{-h.x, y, h.z};
            const glm::vec3 slopeNormal = glm::normalize(glm::vec3(0.0f, desc.size.z, -y));
            mb.quad(a, b, c, d, slopeNormal, top);                            // slope
            mb.quad({-h.x, 0, h.z}, {h.x, 0, h.z}, c, d, {0, 0, 1}, side);    // back
            mb.quad(a, {-h.x, 0, h.z}, {h.x, 0, h.z}, b, {0, -1, 0}, side);   // bottom
            mb.quad(a, d, {-h.x, 0, h.z}, {-h.x, 0, h.z}, {-1, 0, 0}, side);  // left (tri)
            mb.quad(b, {h.x, 0, h.z}, c, c, {1, 0, 0}, side);                 // right (tri)
            break;
        }
        case PrimitiveKind::Stairs: {
            const std::uint32_t n = desc.steps == 0 ? 1 : desc.steps;
            const float rise = desc.size.y / static_cast<float>(n);
            const float run = desc.size.z / static_cast<float>(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                // Each step is a solid block from the ground to its tread, so the
                // underside is closed and Recast sees a clean staircase.
                const float z0 = -h.z + run * static_cast<float>(i);
                mb.box({-h.x, 0.0f, z0}, {h.x, rise * static_cast<float>(i + 1), z0 + run}, top, side, i == 0);
            }
            break;
        }
    }
    return mb.take();
}

std::string_view primitiveKindName(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Box:
            return "box";
        case PrimitiveKind::Ramp:
            return "ramp";
        case PrimitiveKind::Stairs:
            return "stairs";
        case PrimitiveKind::Plane:
            return "plane";
    }
    return "box";
}

std::optional<PrimitiveKind> parsePrimitiveKind(std::string_view name) {
    for (PrimitiveKind k : {PrimitiveKind::Box, PrimitiveKind::Ramp, PrimitiveKind::Stairs, PrimitiveKind::Plane}) {
        if (primitiveKindName(k) == name) {
            return k;
        }
    }
    return std::nullopt;
}

}  // namespace crowdsim::geometry
