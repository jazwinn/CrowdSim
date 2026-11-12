#include "crowdsim/render/SceneRenderer.hpp"

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <algorithm>
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
constexpr float kGroundY = -0.25f;
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

/// A static vertex buffer and its vertex count.
struct StaticBuffer {
    bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
    std::uint32_t count = 0;
    void reset() {
        if (bgfx::isValid(vb)) {
            bgfx::destroy(vb);
        }
        vb = BGFX_INVALID_HANDLE;
        count = 0;
    }
};

struct LineVertex {
    float x, y, z;
    std::uint32_t abgr;
};

template <typename V>
StaticBuffer makeBuffer(const std::vector<V>& vertices, const bgfx::VertexLayout& layout) {
    StaticBuffer b;
    if (vertices.empty()) {
        return b;
    }
    b.vb = bgfx::createVertexBuffer(
        bgfx::copy(vertices.data(), static_cast<std::uint32_t>(vertices.size() * sizeof(V))), layout);
    b.count = static_cast<std::uint32_t>(vertices.size());
    return b;
}

std::vector<LineVertex> colored(const std::vector<glm::vec3>& points, std::uint32_t abgr, float lift = 0.0f) {
    std::vector<LineVertex> out;
    out.reserve(points.size());
    for (const glm::vec3& p : points) {
        out.push_back({p.x, p.y + lift, p.z, abgr});
    }
    return out;
}

/// Arrow heads for line pairs (from, to): two short barbs at each `to`.
std::vector<glm::vec3> withArrowHeads(const std::vector<glm::vec3>& pairs) {
    std::vector<glm::vec3> out = pairs;
    for (std::size_t i = 0; i + 1 < pairs.size(); i += 2) {
        const glm::vec3 a = pairs[i];
        const glm::vec3 b = pairs[i + 1];
        glm::vec3 d = b - a;
        const float len = std::sqrt(d.x * d.x + d.z * d.z);
        if (len < 1e-3f) {
            continue;
        }
        d /= len;
        const float head = std::min(0.6f, 0.35f * len);
        const glm::vec3 side(-d.z, 0.0f, d.x);
        out.push_back(b);
        out.push_back(b - d * head + side * head * 0.5f);
        out.push_back(b);
        out.push_back(b - d * head - side * head * 0.5f);
    }
    return out;
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

    // Static scene (re-uploaded when the snapshot's StaticScene version changes).
    bgfx::VertexLayout meshLayout;
    bgfx::VertexLayout lineLayout;
    bgfx::ProgramHandle meshProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle lineProgram = BGFX_INVALID_HANDLE;
    std::uint64_t staticVersion = ~0ull;
    StaticBuffer mesh;
    StaticBuffer navTriangles;
    StaticBuffer navWalls;
    StaticBuffer links;
    std::vector<StaticBuffer> flow;

    void destroyStatic() {
        mesh.reset();
        navTriangles.reset();
        navWalls.reset();
        links.reset();
        for (StaticBuffer& f : flow) {
            f.reset();
        }
        flow.clear();
    }
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

    r.meshLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    r.lineLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    r.meshProgram = createEmbeddedProgram(EmbeddedProgram::Mesh);
    r.lineProgram = createEmbeddedProgram(EmbeddedProgram::Line);
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
    r.destroyStatic();
    bgfx::destroy(r.meshProgram);
    bgfx::destroy(r.lineProgram);
}

void SceneRenderer::uploadStatic(const snapshot::StaticScene& scene) {
    Resources& r = *res_;
    r.destroyStatic();
    static_assert(sizeof(snapshot::StaticVertex) == 28);
    r.mesh = makeBuffer(scene.meshVertices, r.meshLayout);
    r.navTriangles = makeBuffer(colored(scene.navTriangles, 0x5530c060u), r.lineLayout);  // translucent green
    r.navWalls = makeBuffer(colored(scene.navWalls, 0xff3050ffu, 0.02f), r.lineLayout);   // red-orange
    r.links = makeBuffer(colored(withArrowHeads(scene.offMeshLinks), 0xff20e0ffu, 0.1f), r.lineLayout);
    for (const std::vector<glm::vec3>& arrows : scene.flowArrows) {
        r.flow.push_back(makeBuffer(colored(withArrowHeads(arrows), 0xffffd040u), r.lineLayout));
    }
    r.staticVersion = scene.version;
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
    if (overlays.ground) {
        float model[16];
        // Reference grid just below the scene so it never z-fights with floors at y = 0.
        bx::mtxSRT(model, kGroundHalfSize, 1.0f, kGroundHalfSize, 0.0f, 0.0f, 0.0f, 0.0f, kGroundY, 0.0f);
        bgfx::setTransform(model);
        bgfx::setVertexBuffer(0, r.quadVb);
        bgfx::setIndexBuffer(r.quadIb, 0, 6);
        bgfx::setUniform(r.cameraPos, cam);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                       BGFX_STATE_MSAA);
        bgfx::submit(kViewScene, r.groundProgram);
        ++stats.drawCalls;
    }

    // Static environment.
    if (snap.staticScene && snap.staticScene->version != r.staticVersion) {
        uploadStatic(*snap.staticScene);
    }
    if (bgfx::isValid(r.mesh.vb)) {
        bgfx::setVertexBuffer(0, r.mesh.vb);
        bgfx::setUniform(r.lightDir, light);
        bgfx::setUniform(r.cameraPos, cam);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                       BGFX_STATE_MSAA);
        bgfx::submit(kViewScene, r.meshProgram);
        ++stats.drawCalls;
    }
    auto drawDebug = [&](const StaticBuffer& b, std::uint64_t extra) {
        if (!bgfx::isValid(b.vb)) {
            return;
        }
        bgfx::setVertexBuffer(0, b.vb);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA | extra);
        bgfx::submit(kViewScene, r.lineProgram);
        ++stats.drawCalls;
    };
    if (overlays.navmesh) {
        drawDebug(r.navTriangles, 0);
        drawDebug(r.navWalls, BGFX_STATE_PT_LINES | BGFX_STATE_LINEAA);
    }
    if (overlays.flowField && overlays.flowGoal >= 0 && static_cast<std::size_t>(overlays.flowGoal) < r.flow.size()) {
        drawDebug(r.flow[static_cast<std::size_t>(overlays.flowGoal)], BGFX_STATE_PT_LINES | BGFX_STATE_LINEAA);
    }
    if (overlays.links) {
        drawDebug(r.links, BGFX_STATE_PT_LINES | BGFX_STATE_LINEAA);
    }

    // Spawner / goal areas: decals on the ground. Drawn right after the ground without
    // depth testing (the view is sequential), so they never z-fight with it, and before
    // the agents, which then draw over them.
    if (overlays.areas) {
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

    // Velocity lines (transient), from each agent's chest along its velocity.
    if (overlays.velocities && total > 0) {
        const std::uint32_t verts =
            std::min<std::uint32_t>(total * 2, bgfx::getAvailTransientVertexBuffer(total * 2, r.lineLayout)) & ~1u;
        if (verts >= 2) {
            bgfx::TransientVertexBuffer tvb;
            bgfx::allocTransientVertexBuffer(&tvb, verts, r.lineLayout);
            auto* v = reinterpret_cast<LineVertex*>(tvb.data);
            for (std::uint32_t i = 0; i < verts / 2; ++i) {
                const snapshot::AgentInstance& a = snap.agents[i];
                const glm::vec3 p =
                    a.prevPosition + (a.position - a.prevPosition) * alpha + glm::vec3(0.0f, 1.2f, 0.0f);
                v[2 * i] = {p.x, p.y, p.z, 0xff40ff40u};
                v[2 * i + 1] = {p.x + a.velocity.x, p.y, p.z + a.velocity.y, 0xff40ff40u};
            }
            bgfx::setVertexBuffer(0, &tvb);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES |
                           BGFX_STATE_LINEAA);
            bgfx::submit(kViewScene, r.lineProgram);
            ++stats.drawCalls;
        }
    }

    return stats;
}

}  // namespace crowdsim::render
