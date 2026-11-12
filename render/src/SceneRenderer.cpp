#include "crowdsim/render/SceneRenderer.hpp"

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include <glm/geometric.hpp>

#include "CrowdRenderer.hpp"
#include "ShaderLibrary.hpp"
#include "crowdsim/core/Profile.hpp"
#include "crowdsim/render/Renderer.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

namespace crowdsim::render {

glm::vec3 Camera::eye() const {
    const float cp = std::cos(pitch);
    return target + distance * glm::vec3(cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw));
}

void cameraMatrices(const Camera& camera, float aspect, float* view16, float* proj16) {
    const glm::vec3 eye = camera.eye();
    bx::mtxLookAt(view16, bx::Vec3(eye.x, eye.y, eye.z), bx::Vec3(camera.target.x, camera.target.y, camera.target.z),
                  bx::Vec3(0.0f, 1.0f, 0.0f));
    bx::mtxProj(proj16, camera.fovYDegrees, aspect, camera.nearPlane, camera.farPlane,
                bgfx::getCaps()->homogeneousDepth);
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

constexpr std::uint16_t kShadowMapSize = 2048;
constexpr float kShadowDepthBias = 0.0004f;
constexpr float kShadowNormalOffset = 0.04f;

/// Instanced capsules (far LOD / no character), in transient batches. Returns draw calls.
template <typename Bind>
std::uint32_t drawCapsules(bgfx::ViewId view, const std::vector<CapsuleInstance>& list, bgfx::VertexBufferHandle vb,
                           bgfx::IndexBufferHandle ib, bgfx::ProgramHandle program, std::uint64_t state, Bind&& bind) {
    constexpr std::uint16_t kStride = sizeof(CapsuleInstance);
    const auto total = static_cast<std::uint32_t>(list.size());
    std::uint32_t offset = 0;
    std::uint32_t draws = 0;
    while (offset < total && bgfx::isValid(program)) {
        const std::uint32_t count = bgfx::getAvailInstanceDataBuffer(total - offset, kStride);
        if (count == 0) {
            break;
        }
        bgfx::InstanceDataBuffer idb;
        bgfx::allocInstanceDataBuffer(&idb, count, kStride);
        std::memcpy(idb.data, list.data() + offset, static_cast<std::size_t>(count) * kStride);
        bgfx::setVertexBuffer(0, vb);
        bgfx::setIndexBuffer(ib);
        bgfx::setInstanceDataBuffer(&idb);
        bind();
        bgfx::setState(state);
        bgfx::submit(view, program);
        ++draws;
        offset += count;
    }
    return draws;
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

    // Metrics heatmap (rebuilt when its version changes).
    StaticBuffer heatmap;
    std::uint64_t heatmapVersion = ~0ull;

    // Animated crowd and shadows.
    CrowdRenderer crowd;
    bgfx::ProgramHandle crowdProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle crowdShadowProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle agentShadowProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle meshShadowProgram = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle shadowMap = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle shadowFb = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle lightMtx = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadowParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shadowSampler = BGFX_INVALID_HANDLE;
    bool shadowsSupported = false;

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

    r.crowdProgram = createEmbeddedProgram(EmbeddedProgram::Crowd);
    r.crowdShadowProgram = createEmbeddedProgram(EmbeddedProgram::CrowdShadow);
    r.agentShadowProgram = createEmbeddedProgram(EmbeddedProgram::AgentShadow);
    r.meshShadowProgram = createEmbeddedProgram(EmbeddedProgram::MeshShadow);
    r.lightMtx = bgfx::createUniform("u_lightMtx", bgfx::UniformType::Mat4);
    r.shadowParams = bgfx::createUniform("u_shadowParams", bgfx::UniformType::Vec4);
    r.shadowSampler = bgfx::createUniform("s_shadowMap", bgfx::UniformType::Sampler);
    // Depth texture compared in the shader (PCF). Always created so lit shaders have
    // something bound; only rendered into when shadows are on and supported.
    constexpr std::uint64_t kShadowFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL | BGFX_SAMPLER_UVW_CLAMP;
    const bgfx::TextureFormat::Enum format = bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, kShadowFlags)
                                                 ? bgfx::TextureFormat::D32F
                                                 : bgfx::TextureFormat::D16;
    r.shadowsSupported = (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_COMPARE_LEQUAL) != 0 &&
                         bgfx::isTextureValid(0, false, 1, format, kShadowFlags);
    r.shadowMap = bgfx::createTexture2D(kShadowMapSize, kShadowMapSize, false, 1, format, kShadowFlags);
    if (r.shadowsSupported) {
        r.shadowFb = bgfx::createFrameBuffer(1, &r.shadowMap, false);
    }
    // Without every shadow program there is no consistent shadow map: turn shadows off.
    r.shadowsSupported = r.shadowsSupported && bgfx::isValid(r.crowdShadowProgram) &&
                         bgfx::isValid(r.agentShadowProgram) && bgfx::isValid(r.meshShadowProgram);
    bgfx::setViewName(kViewShadow, "Shadow");
    bgfx::setViewClear(kViewShadow, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
}

bool SceneRenderer::shadowsSupported() const noexcept {
    return res_->shadowsSupported;
}

void SceneRenderer::setCharacter(std::shared_ptr<const snapshot::CharacterAsset> character) {
    if (res_->enabled) {
        res_->crowd.setCharacter(std::move(character));
    }
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
    r.heatmap.reset();
    bgfx::destroy(r.meshProgram);
    bgfx::destroy(r.lineProgram);
    r.crowd.setCharacter(nullptr);
    for (bgfx::ProgramHandle p : {r.crowdProgram, r.crowdShadowProgram, r.agentShadowProgram, r.meshShadowProgram}) {
        if (bgfx::isValid(p)) {
            bgfx::destroy(p);
        }
    }
    if (bgfx::isValid(r.shadowFb)) {
        bgfx::destroy(r.shadowFb);
    }
    bgfx::destroy(r.shadowMap);
    bgfx::destroy(r.lightMtx);
    bgfx::destroy(r.shadowParams);
    bgfx::destroy(r.shadowSampler);
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
                                 const ViewRect& rect, float frameSeconds) {
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
    cameraMatrices(camera, static_cast<float>(rect.width) / static_cast<float>(rect.height), view, proj);
    bgfx::setViewRect(kViewScene, rect.x, rect.y, rect.width, rect.height);
    bgfx::setViewTransform(kViewScene, view, proj);

    const float light[4] = {0.45f, 0.8f, 0.35f, 0.0f};
    const float cam[4] = {eye.x, eye.y, eye.z, kFogDensity};
    const bgfx::Caps* caps = bgfx::getCaps();

    // --- Directional light shadow map: an orthographic square around the camera target,
    // snapped to whole texels so it does not shimmer while the camera pans.
    const bool shadowsOn = settings.shadows && r.shadowsSupported;
    float lightMtx[16];
    bx::mtxIdentity(lightMtx);
    float lightViewProj[16];
    bx::mtxIdentity(lightViewProj);
    const float shadowParams[4] = {1.0f / kShadowMapSize, kShadowDepthBias, shadowsOn ? 1.0f : 0.0f,
                                   kShadowNormalOffset};
    if (shadowsOn) {
        const glm::vec3 toLight = glm::normalize(glm::vec3(light[0], light[1], light[2]));
        const glm::vec3 fwd = -toLight;
        const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), fwd));
        const glm::vec3 up = glm::cross(fwd, right);
        const float extent = std::clamp(camera.distance * 0.9f, 15.0f, std::max(15.0f, settings.shadowDistance));
        const float texel = 2.0f * extent / kShadowMapSize;
        const glm::vec3 target = camera.target;
        const float fr = std::floor(glm::dot(target, right) / texel) * texel;
        const float fu = std::floor(glm::dot(target, up) / texel) * texel;
        const glm::vec3 focus = right * fr + up * fu + fwd * glm::dot(target, fwd);
        const glm::vec3 lightEye = focus + toLight * 120.0f;
        float lightView[16];
        float lightProj[16];
        bx::mtxLookAt(lightView, bx::Vec3(lightEye.x, lightEye.y, lightEye.z), bx::Vec3(focus.x, focus.y, focus.z),
                      bx::Vec3(0.0f, 1.0f, 0.0f));
        bx::mtxOrtho(lightProj, -extent, extent, -extent, extent, 1.0f, 260.0f, 0.0f, caps->homogeneousDepth);
        bx::mtxMul(lightViewProj, lightView, lightProj);
        const float sy = caps->originBottomLeft ? 0.5f : -0.5f;
        const float sz = caps->homogeneousDepth ? 0.5f : 1.0f;
        const float tz = caps->homogeneousDepth ? 0.5f : 0.0f;
        const float crop[16] = {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, sy,   0.0f, 0.0f,
                                0.0f, 0.0f, sz,   0.0f, 0.5f, 0.5f, tz,   1.0f};
        float tmp[16];
        bx::mtxMul(tmp, lightProj, crop);
        bx::mtxMul(lightMtx, lightView, tmp);
        bgfx::setViewRect(kViewShadow, 0, 0, kShadowMapSize, kShadowMapSize);
        bgfx::setViewFrameBuffer(kViewShadow, r.shadowFb);
        bgfx::setViewTransform(kViewShadow, lightView, lightProj);
        bgfx::touch(kViewShadow);
    }
    auto bindShadow = [&]() {
        bgfx::setTexture(1, r.shadowSampler, r.shadowMap);
        bgfx::setUniform(r.lightMtx, lightMtx);
        bgfx::setUniform(r.shadowParams, shadowParams);
    };

    // --- Crowd: cull, pick LODs, animate (parallel).
    {
        const auto start = std::chrono::steady_clock::now();
        float viewProj[16];
        bx::mtxMul(viewProj, view, proj);
        CrowdFrameInput in;
        in.snapshot = &snap;
        in.alpha = alpha;
        in.dt = frameSeconds;
        in.eye = eye;
        in.camera = Frustum::fromViewProj(viewProj, caps->homogeneousDepth);
        in.light = Frustum::fromViewProj(lightViewProj, caps->homogeneousDepth);
        in.shadows = shadowsOn;
        in.animated = settings.animatedCharacters;
        in.lodNear = settings.lodNearDistance;
        in.lodFar = std::max(settings.lodNearDistance, settings.lodFarDistance);
        in.capsuleRadius = kBodyRadius;
        r.crowd.prepare(in, jobs_);
        stats.prepareMs = static_cast<float>(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const CrowdLists& crowd = r.crowd.lists();

    // Ground
    if (overlays.ground) {
        float model[16];
        // Reference grid just below the scene so it never z-fights with floors at y = 0.
        bx::mtxSRT(model, kGroundHalfSize, 1.0f, kGroundHalfSize, 0.0f, 0.0f, 0.0f, 0.0f, kGroundY, 0.0f);
        bgfx::setTransform(model);
        bgfx::setVertexBuffer(0, r.quadVb);
        bgfx::setIndexBuffer(r.quadIb, 0, 6);
        bgfx::setUniform(r.cameraPos, cam);
        bindShadow();
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                       BGFX_STATE_MSAA);
        bgfx::submit(kViewScene, r.groundProgram);
        ++stats.drawCalls;
    }

    // Static environment.
    if (snap.staticScene && snap.staticScene->version != r.staticVersion) {
        uploadStatic(*snap.staticScene);
    }
    constexpr std::uint64_t kShadowState = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
    if (shadowsOn) {
        if (bgfx::isValid(r.mesh.vb) && bgfx::isValid(r.meshShadowProgram)) {
            bgfx::setVertexBuffer(0, r.mesh.vb);
            bgfx::setState(kShadowState);
            bgfx::submit(kViewShadow, r.meshShadowProgram);
            ++stats.drawCalls;
        }
        stats.drawCalls +=
            r.crowd.drawVat(kViewShadow, 1, crowd.shadowVat, r.crowdShadowProgram, kShadowState, nullptr);
        stats.drawCalls += drawCapsules(kViewShadow, crowd.shadowCapsule, r.capsuleVb, r.capsuleIb,
                                        r.agentShadowProgram, kShadowState, [] {});
        stats.shadowCasters = static_cast<std::uint32_t>(crowd.shadowVat.size() + crowd.shadowCapsule.size());
    }
    if (bgfx::isValid(r.mesh.vb)) {
        bgfx::setVertexBuffer(0, r.mesh.vb);
        bgfx::setUniform(r.lightDir, light);
        bgfx::setUniform(r.cameraPos, cam);
        bindShadow();
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

    // Spawner / goal areas: translucent decals lifted slightly above their floor and depth
    // tested (without writing depth), so walls in front hide them; agents draw over them.
    if (overlays.areas) {
        for (const snapshot::AreaMarker& area : snap.areas) {
            float model[16];
            bx::mtxSRT(model, area.halfExtents.x, 1.0f, area.halfExtents.y, 0.0f, 0.0f, 0.0f, area.center.x,
                       area.center.y + 0.03f, area.center.z);
            bgfx::setTransform(model);
            bgfx::setVertexBuffer(0, r.quadVb);
            bgfx::setIndexBuffer(r.quadIb, 0, 6);
            const float c[4] = {area.color.r, area.color.g, area.color.b, area.color.a};
            bgfx::setUniform(r.color, c);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA |
                           BGFX_STATE_MSAA);
            bgfx::submit(kViewScene, r.flatProgram);
            ++stats.drawCalls;
        }
    }

    // Metrics heatmap: translucent cells just above the floor.
    if (heatmap.version != r.heatmapVersion) {
        r.heatmapVersion = heatmap.version;
        r.heatmap.reset();
        std::vector<LineVertex> tris;
        const float maxValue = std::max(heatmap.maxValue, 1e-6f);
        for (int z = 0; z < heatmap.height; ++z) {
            for (int x = 0; x < heatmap.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(z * heatmap.width + x);
                const float t = i < heatmap.values.size() ? std::clamp(heatmap.values[i] / maxValue, 0.0f, 1.0f) : 0.0f;
                if (t <= 0.02f) {
                    continue;
                }
                // Blue -> cyan -> green -> yellow -> red.
                const float rC = std::clamp(t * 4.0f - 2.0f, 0.0f, 1.0f);
                const float gC =
                    t < 0.75f ? std::clamp(t * 4.0f - 0.5f, 0.0f, 1.0f) : std::clamp((1.0f - t) * 4.0f, 0.0f, 1.0f);
                const float bC = std::clamp(1.5f - t * 3.0f, 0.0f, 1.0f);
                const auto c = [](float v) { return static_cast<std::uint32_t>(std::lround(v * 255.0f)); };
                const std::uint32_t abgr = (c(0.30f + 0.45f * t) << 24) | (c(bC) << 16) | (c(gC) << 8) | c(rC);
                const float x0 = heatmap.origin.x + static_cast<float>(x) * heatmap.cell;
                const float z0 = heatmap.origin.y + static_cast<float>(z) * heatmap.cell;
                const float x1 = x0 + heatmap.cell;
                const float z1 = z0 + heatmap.cell;
                const float y = heatmap.elevation + 0.04f;
                tris.insert(tris.end(),
                            {LineVertex{x0, y, z0, abgr}, LineVertex{x1, y, z1, abgr}, LineVertex{x1, y, z0, abgr},
                             LineVertex{x0, y, z0, abgr}, LineVertex{x0, y, z1, abgr}, LineVertex{x1, y, z1, abgr}});
            }
        }
        r.heatmap = makeBuffer(tris, r.lineLayout);
    }
    if (bgfx::isValid(r.heatmap.vb)) {
        bgfx::setVertexBuffer(0, r.heatmap.vb);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
        bgfx::submit(kViewScene, r.lineProgram);
        ++stats.drawCalls;
    }

    // Agents: animated characters near / mid, capsules far (or everywhere without a character).
    const auto total = static_cast<std::uint32_t>(snap.agents.size());
    {
        constexpr std::uint64_t kAgentState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                                              BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CCW | BGFX_STATE_MSAA;
        auto bindLit = [&]() {
            bgfx::setUniform(r.lightDir, light);
            bgfx::setUniform(r.cameraPos, cam);
            bindShadow();
        };
        stats.drawCalls += r.crowd.drawVat(kViewScene, 0, crowd.near, r.crowdProgram, kAgentState, bindLit);
        stats.drawCalls += r.crowd.drawVat(kViewScene, 1, crowd.mid, r.crowdProgram, kAgentState, bindLit);
        stats.drawCalls +=
            drawCapsules(kViewScene, crowd.far, r.capsuleVb, r.capsuleIb, r.agentProgram, kAgentState, bindLit);
        stats.agentsNear = static_cast<std::uint32_t>(crowd.near.size());
        stats.agentsMid = static_cast<std::uint32_t>(crowd.mid.size());
        stats.agentsFar = static_cast<std::uint32_t>(crowd.far.size());
        stats.agentsCulled = crowd.culled;
        stats.agentsDrawn = stats.agentsNear + stats.agentsMid + stats.agentsFar;
    }

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

    // Extra lines (selection outline etc.), drawn on top of the scene.
    if (!extraLines.empty()) {
        const auto wanted = static_cast<std::uint32_t>(extraLines.size() * 2);
        const std::uint32_t verts = std::min(wanted, bgfx::getAvailTransientVertexBuffer(wanted, r.lineLayout)) & ~1u;
        if (verts >= 2) {
            bgfx::TransientVertexBuffer tvb;
            bgfx::allocTransientVertexBuffer(&tvb, verts, r.lineLayout);
            auto* v = reinterpret_cast<LineVertex*>(tvb.data);
            for (std::uint32_t i = 0; i < verts / 2; ++i) {
                const DebugLine& l = extraLines[i];
                v[2 * i] = {l.a.x, l.a.y, l.a.z, l.abgr};
                v[2 * i + 1] = {l.b.x, l.b.y, l.b.z, l.abgr};
            }
            bgfx::setVertexBuffer(0, &tvb);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_PT_LINES |
                           BGFX_STATE_LINEAA | BGFX_STATE_BLEND_ALPHA);
            bgfx::submit(kViewScene, r.lineProgram);
            ++stats.drawCalls;
        }
    }

    return stats;
}

}  // namespace crowdsim::render
