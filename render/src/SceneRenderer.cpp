#include "crowdsim/render/SceneRenderer.hpp"

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include "ShaderLibrary.hpp"
#include "crowdsim/core/Profile.hpp"
#include "crowdsim/render/Renderer.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

namespace crowdsim::render {

glm::vec3 Camera::eye() const {
    const float cp = std::cos(pitch);
    return target + distance * glm::vec3(cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw));
}

namespace {

constexpr float kBodyRadius = 0.28f;  ///< mesh radius; instances scale x/z by radius / kBodyRadius
constexpr float kBodyHeight = 1.75f;
constexpr float kGroundHalfSize = 1000.0f;
constexpr float kFogDensity = 0.0025f;

struct PosNormalVertex {
    float x, y, z;
    float nx, ny, nz;
};

struct PosVertex {
    float x, y, z;
};

/// Instance data matching i_data0 / i_data1 in vs_agent.sc.
struct AgentInstanceData {
    float x, y, z, heading;
    float r, g, b, scale;
};
static_assert(sizeof(AgentInstanceData) == 32);

/// Upright capsule standing on y = 0, facing +Z.
void buildCapsule(std::vector<PosNormalVertex>& vertices, std::vector<std::uint16_t>& indices) {
    constexpr int kSegments = 16;
    constexpr int kCapRings = 5;
    constexpr float kPi = std::numbers::pi_v<float>;
    const float r = kBodyRadius;
    const float cylBottom = r;
    const float cylTop = kBodyHeight - r;

    // Ring list from bottom pole to top pole: (y-centre of hemisphere, polar angle).
    struct Ring {
        float y;
        float ringRadius;
        float ny;
    };
    std::vector<Ring> rings;
    for (int i = 0; i <= kCapRings; ++i) {  // bottom hemisphere, pole -> equator
        const float a = -kPi * 0.5f + (kPi * 0.5f) * static_cast<float>(i) / kCapRings;
        rings.push_back({cylBottom + r * std::sin(a), r * std::cos(a), std::sin(a)});
    }
    for (int i = 0; i <= kCapRings; ++i) {  // top hemisphere, equator -> pole
        const float a = (kPi * 0.5f) * static_cast<float>(i) / kCapRings;
        rings.push_back({cylTop + r * std::sin(a), r * std::cos(a), std::sin(a)});
    }

    for (const Ring& ring : rings) {
        const float horizontal = std::sqrt(std::max(0.0f, 1.0f - ring.ny * ring.ny));
        for (int s = 0; s <= kSegments; ++s) {
            const float t = 2.0f * kPi * static_cast<float>(s) / kSegments;
            const float cx = std::sin(t);
            const float cz = std::cos(t);
            vertices.push_back(
                {cx * ring.ringRadius, ring.y, cz * ring.ringRadius, cx * horizontal, ring.ny, cz * horizontal});
        }
    }
    const int stride = kSegments + 1;
    for (int ringIdx = 0; ringIdx + 1 < static_cast<int>(rings.size()); ++ringIdx) {
        for (int s = 0; s < kSegments; ++s) {
            const auto a = static_cast<std::uint16_t>(ringIdx * stride + s);
            const auto b = static_cast<std::uint16_t>(a + stride);
            const auto c = static_cast<std::uint16_t>(a + 1);
            const auto d = static_cast<std::uint16_t>(b + 1);
            // Wound so outward faces survive BGFX_STATE_CULL_CCW (verified visually).
            indices.insert(indices.end(), {a, c, b, c, d, b});
        }
    }
}

float lerpAngle(float a, float b, float t) {
    constexpr float kPi = std::numbers::pi_v<float>;
    float d = std::fmod(b - a + kPi, 2.0f * kPi);
    if (d < 0.0f) {
        d += 2.0f * kPi;
    }
    return a + (d - kPi) * t;
}

}  // namespace

struct SceneRenderer::Resources {
    bgfx::VertexLayout posNormalLayout;
    bgfx::VertexLayout posLayout;
    bgfx::VertexBufferHandle capsuleVb = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle capsuleIb = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle quadVb = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle quadIb = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle agentProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle groundProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle flatProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle lightDir = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle cameraPos = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle color = BGFX_INVALID_HANDLE;
    bool enabled = false;
};

SceneRenderer::SceneRenderer() : res_(std::make_unique<Resources>()) {
    Resources& r = *res_;
    r.enabled = bgfx::getRendererType() != bgfx::RendererType::Noop;
    if (!r.enabled) {
        return;
    }

    r.posNormalLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .end();
    r.posLayout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();

    std::vector<PosNormalVertex> vertices;
    std::vector<std::uint16_t> indices;
    buildCapsule(vertices, indices);
    r.capsuleVb = bgfx::createVertexBuffer(
        bgfx::copy(vertices.data(), static_cast<std::uint32_t>(vertices.size() * sizeof(PosNormalVertex))),
        r.posNormalLayout);
    r.capsuleIb = bgfx::createIndexBuffer(
        bgfx::copy(indices.data(), static_cast<std::uint32_t>(indices.size() * sizeof(std::uint16_t))));

    // Unit quad in XZ ([-1, 1]^2), scaled by the model matrix.
    constexpr PosVertex kQuad[] = {{-1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f}};
    constexpr std::uint16_t kQuadIdx[] = {0, 2, 1, 0, 3, 2, 0, 1, 2, 0, 2, 3};  // both windings
    r.quadVb = bgfx::createVertexBuffer(bgfx::copy(kQuad, sizeof(kQuad)), r.posLayout);
    r.quadIb = bgfx::createIndexBuffer(bgfx::copy(kQuadIdx, sizeof(kQuadIdx)));

    r.agentProgram = createEmbeddedProgram(EmbeddedProgram::Agent);
    r.groundProgram = createEmbeddedProgram(EmbeddedProgram::Ground);
    r.flatProgram = createEmbeddedProgram(EmbeddedProgram::Flat);
    r.lightDir = bgfx::createUniform("u_lightDir", bgfx::UniformType::Vec4);
    r.cameraPos = bgfx::createUniform("u_cameraPos", bgfx::UniformType::Vec4);
    r.color = bgfx::createUniform("u_color", bgfx::UniformType::Vec4);
}

SceneRenderer::~SceneRenderer() {
    Resources& r = *res_;
    if (!r.enabled) {
        return;
    }
    bgfx::destroy(r.capsuleVb);
    bgfx::destroy(r.capsuleIb);
    bgfx::destroy(r.quadVb);
    bgfx::destroy(r.quadIb);
    bgfx::destroy(r.agentProgram);
    bgfx::destroy(r.groundProgram);
    bgfx::destroy(r.flatProgram);
    bgfx::destroy(r.lightDir);
    bgfx::destroy(r.cameraPos);
    bgfx::destroy(r.color);
}

SceneStats SceneRenderer::render(const snapshot::RenderSnapshot& snap, float alpha, const Camera& camera,
                                 const ViewRect& rect) {
    CS_PROFILE_ZONE();
    SceneStats stats;
    Resources& r = *res_;
    if (!r.enabled || rect.width == 0 || rect.height == 0) {
        return stats;
    }

    // View / projection for this backend's depth range.
    const glm::vec3 eye = camera.eye();
    float view[16];
    float proj[16];
    bx::mtxLookAt(view, bx::Vec3(eye.x, eye.y, eye.z), bx::Vec3(camera.target.x, camera.target.y, camera.target.z),
                  bx::Vec3(0.0f, 1.0f, 0.0f));
    bx::mtxProj(proj, camera.fovYDegrees, static_cast<float>(rect.width) / static_cast<float>(rect.height),
                camera.nearPlane, camera.farPlane, bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewRect(kViewScene, rect.x, rect.y, rect.width, rect.height);
    bgfx::setViewTransform(kViewScene, view, proj);

    const float light[4] = {0.45f, 0.8f, 0.35f, 0.0f};
    const float cam[4] = {eye.x, eye.y, eye.z, kFogDensity};

    // Ground
    if (showGround) {
        float model[16];
        bx::mtxScale(model, kGroundHalfSize, 1.0f, kGroundHalfSize);
        bgfx::setTransform(model);
        bgfx::setVertexBuffer(0, r.quadVb);
        bgfx::setIndexBuffer(r.quadIb, 0, 6);
        bgfx::setUniform(r.cameraPos, cam);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                       BGFX_STATE_MSAA);
        bgfx::submit(kViewScene, r.groundProgram);
        ++stats.drawCalls;
    }

    // Spawner / goal areas: decals on the ground. Drawn right after the ground without
    // depth testing (the view is sequential), so they never z-fight with it, and before
    // the agents, which then draw over them.
    if (showAreas) {
        for (const snapshot::AreaMarker& area : snap.areas) {
            float model[16];
            bx::mtxSRT(model, area.halfExtents.x, 1.0f, area.halfExtents.y, 0.0f, 0.0f, 0.0f, area.center.x,
                       area.center.y, area.center.z);
            bgfx::setTransform(model);
            bgfx::setVertexBuffer(0, r.quadVb);
            bgfx::setIndexBuffer(r.quadIb, 0, 6);
            const float c[4] = {area.color.r, area.color.g, area.color.b, area.color.a};
            bgfx::setUniform(r.color, c);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
            bgfx::submit(kViewScene, r.flatProgram);
            ++stats.drawCalls;
        }
    }

    // Agents, instanced. Interpolate previous -> current step.
    const auto total = static_cast<std::uint32_t>(snap.agents.size());
    constexpr std::uint16_t kStride = sizeof(AgentInstanceData);
    std::uint32_t offset = 0;
    while (offset < total) {
        const std::uint32_t wanted = total - offset;
        const std::uint32_t count = bgfx::getAvailInstanceDataBuffer(wanted, kStride);
        if (count == 0) {
            break;  // out of transient memory this frame
        }
        bgfx::InstanceDataBuffer idb;
        bgfx::allocInstanceDataBuffer(&idb, count, kStride);
        auto* out = reinterpret_cast<AgentInstanceData*>(idb.data);
        for (std::uint32_t i = 0; i < count; ++i) {
            const snapshot::AgentInstance& a = snap.agents[offset + i];
            const glm::vec3 p = a.prevPosition + (a.position - a.prevPosition) * alpha;
            out[i] = AgentInstanceData{p.x,       p.y,       p.z,       lerpAngle(a.prevHeading, a.heading, alpha),
                                       a.color.r, a.color.g, a.color.b, a.radius / kBodyRadius};
        }
        bgfx::setVertexBuffer(0, r.capsuleVb);
        bgfx::setIndexBuffer(r.capsuleIb);
        bgfx::setInstanceDataBuffer(&idb);
        bgfx::setUniform(r.lightDir, light);
        bgfx::setUniform(r.cameraPos, cam);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                       BGFX_STATE_CULL_CCW | BGFX_STATE_MSAA);
        bgfx::submit(kViewScene, r.agentProgram);
        ++stats.drawCalls;
        offset += count;
    }
    stats.agentsDrawn = offset;

    return stats;
}

}  // namespace crowdsim::render
