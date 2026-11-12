#include "crowdsim/geometry/TriangleMesh.hpp"

#include <algorithm>
#include <limits>

#include <glm/geometric.hpp>

namespace crowdsim::geometry {

void TriangleMesh::append(const TriangleMesh& other) {
    const auto base = static_cast<std::uint32_t>(positions.size());
    positions.insert(positions.end(), other.positions.begin(), other.positions.end());
    indices.reserve(indices.size() + other.indices.size());
    for (std::uint32_t i : other.indices) {
        indices.push_back(base + i);
    }
    triangleFlags.insert(triangleFlags.end(), other.triangleFlags.begin(), other.triangleFlags.end());
    triangleColors.insert(triangleColors.end(), other.triangleColors.begin(), other.triangleColors.end());
}

Bounds TriangleMesh::bounds() const {
    Bounds b{glm::vec3(std::numeric_limits<float>::max()), glm::vec3(std::numeric_limits<float>::lowest())};
    for (const glm::vec3& p : positions) {
        b.min = glm::min(b.min, p);
        b.max = glm::max(b.max, p);
    }
    return b;
}

void TriangleMesh::clear() {
    positions.clear();
    indices.clear();
    triangleFlags.clear();
    triangleColors.clear();
}

std::vector<RenderVertex> toRenderVertices(const TriangleMesh& mesh) {
    std::vector<RenderVertex> out;
    out.reserve(mesh.indices.size());
    for (std::uint32_t t = 0; t < mesh.triangleCount(); ++t) {
        const glm::vec3 a = mesh.positions[mesh.indices[3 * t]];
        const glm::vec3 b = mesh.positions[mesh.indices[3 * t + 1]];
        const glm::vec3 c = mesh.positions[mesh.indices[3 * t + 2]];
        glm::vec3 n = glm::cross(b - a, c - a);
        const float len = glm::length(n);
        n = len > 0.0f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
        const std::uint32_t color = t < mesh.triangleColors.size() ? mesh.triangleColors[t] : 0xffb0b0b0u;
        for (const glm::vec3& p : {a, b, c}) {
            out.push_back({p.x, p.y, p.z, n.x, n.y, n.z, color});
        }
    }
    return out;
}

}  // namespace crowdsim::geometry
