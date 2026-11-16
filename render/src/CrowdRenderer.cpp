#include "CrowdRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include <glm/geometric.hpp>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Profile.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

namespace crowdsim::render {

namespace {

float hash01(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return static_cast<float>(x & 0xFFFFFFu) / static_cast<float>(0x1000000);
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

Frustum Frustum::fromViewProj(const float* m, bool homogeneousDepth) {
    auto col = [m](int j) { return glm::vec4(m[0 * 4 + j], m[1 * 4 + j], m[2 * 4 + j], m[3 * 4 + j]); };
    const glm::vec4 c0 = col(0);
    const glm::vec4 c1 = col(1);
    const glm::vec4 c2 = col(2);
    const glm::vec4 c3 = col(3);
    Frustum f;
    f.planes = {c3 + c0, c3 - c0, c3 + c1, c3 - c1, homogeneousDepth ? c3 + c2 : c2, c3 - c2};
    for (glm::vec4& p : f.planes) {
        const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len > 0.0f) {
            p /= len;
        }
    }
    return f;
}

bool Frustum::intersectsSphere(glm::vec3 c, float r) const noexcept {
    for (const glm::vec4& p : planes) {
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) {
            return false;
        }
    }
    return true;
}

void CrowdRenderer::Gpu::destroy() {
    for (int i = 0; i < 2; ++i) {
        if (bgfx::isValid(vb[i])) {
            bgfx::destroy(vb[i]);
        }
        if (bgfx::isValid(ib[i])) {
            bgfx::destroy(ib[i]);
        }
        vb[i] = BGFX_INVALID_HANDLE;
        ib[i] = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(bones)) {
        bgfx::destroy(bones);
    }
    bones = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(bonesSampler)) {
        bgfx::destroy(bonesSampler);
    }
    bonesSampler = BGFX_INVALID_HANDLE;
}

CrowdRenderer::CrowdRenderer() {
    // Joint indices and weights travel as normalized bytes in TEXCOORD2/3 (see vs_crowd.sc).
    layout_.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord2, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::TexCoord3, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
}

CrowdRenderer::~CrowdRenderer() {
    gpu_.destroy();
}

void CrowdRenderer::setCharacter(std::shared_ptr<const snapshot::CharacterAsset> character) {
    gpu_.destroy();
    character_ = std::move(character);
    if (!character_ || character_->lods.size() < 2) {
        character_.reset();
        return;
    }
    static_assert(sizeof(snapshot::SkinnedVertex) == 36);
    for (int i = 0; i < 2; ++i) {
        const snapshot::CharacterMesh& mesh = character_->lods[static_cast<std::size_t>(i)];
        gpu_.vb[i] = bgfx::createVertexBuffer(
            bgfx::copy(mesh.vertices.data(),
                       static_cast<std::uint32_t>(mesh.vertices.size() * sizeof(snapshot::SkinnedVertex))),
            layout_);
        gpu_.ib[i] = bgfx::createIndexBuffer(
            bgfx::copy(mesh.indices.data(), static_cast<std::uint32_t>(mesh.indices.size() * sizeof(std::uint16_t))));
    }
    gpu_.bones = bgfx::createTexture2D(
        static_cast<std::uint16_t>(character_->textureWidth), static_cast<std::uint16_t>(character_->textureHeight),
        false, 1, bgfx::TextureFormat::RGBA32F, BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP,
        bgfx::copy(character_->boneTexture.data(),
                   static_cast<std::uint32_t>(character_->boneTexture.size() * sizeof(float))));
    gpu_.bonesSampler = bgfx::createUniform("s_bones", bgfx::UniformType::Sampler);
}

void CrowdRenderer::assignSlots(const snapshot::RenderSnapshot& snap) {
    const auto n = static_cast<std::uint32_t>(snap.agents.size());
    agentSlot_.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t id = snap.agents[i].agentId;
        if (id >= slotOf_.size()) {
            slotOf_.resize(static_cast<std::size_t>(id) + 1 + slotOf_.size() / 2, 0);
        }
        if (slotOf_[id] == 0) {
            std::uint32_t slot = 0;
            if (!freeSlots_.empty()) {
                slot = freeSlots_.back();
                freeSlots_.pop_back();
            } else {
                slot = static_cast<std::uint32_t>(slots_.size());
                slots_.emplace_back();
            }
            slots_[slot] = Slot{animation::initialGaitState(id), frame_, id, true};
            slotOf_[id] = slot + 1;
        }
        agentSlot_[i] = slotOf_[id] - 1;
        slots_[agentSlot_[i]].lastFrame = frame_;
    }
    // Forget agents that have not been seen for a while (despawned).
    if (frame_ % 64 == 0) {
        for (std::uint32_t k = 0; k < slots_.size(); ++k) {
            Slot& s = slots_[k];
            if (s.used && s.lastFrame + 2 < frame_) {
                slotOf_[s.id] = 0;
                s.used = false;
                freeSlots_.push_back(k);
            }
        }
    }
}

void CrowdRenderer::prepare(const CrowdFrameInput& in, core::JobSystem* jobs) {
    CS_PROFILE_ZONE();
    ++frame_;
    lists_.near.clear();
    lists_.mid.clear();
    lists_.far.clear();
    lists_.shadowVat.clear();
    lists_.shadowCapsule.clear();
    lists_.culled = 0;
    if (in.snapshot == nullptr) {
        return;
    }
    const snapshot::RenderSnapshot& snap = *in.snapshot;
    const auto n = static_cast<std::uint32_t>(snap.agents.size());
    const bool animated = in.animated && character_ != nullptr;
    if (animated) {
        assignSlots(snap);
    }
    lod_.resize(n);
    shadow_.resize(n);
    vat_.resize(n);
    capsule_.resize(n);

    const float nearSq = in.lodNear * in.lodNear;
    const float farSq = in.lodFar * in.lodFar;
    auto body = [&](std::uint32_t begin, std::uint32_t end) {
        for (std::uint32_t i = begin; i < end; ++i) {
            const snapshot::AgentInstance& a = snap.agents[i];
            const glm::vec3 p = a.prevPosition + (a.position - a.prevPosition) * in.alpha;
            const float heading = lerpAngle(a.prevHeading, a.heading, in.alpha);
            const glm::vec3 centre = p + glm::vec3(0.0f, 0.9f, 0.0f);
            const glm::vec3 d = p - in.eye;
            const float distSq = glm::dot(d, d);
            const bool visible = in.camera.intersectsSphere(centre, 1.0f);
            const bool caster = in.shadows && in.light.intersectsSphere(centre, 1.0f);

            const bool lying = a.fallen != 0;
            std::uint8_t lod = 0;
            if (visible) {
                lod = lying || !animated || distSq > farSq ? 3 : (distSq > nearSq ? 2 : 1);
            }
            std::uint8_t shadow = 0;
            if (caster) {
                shadow = animated && distSq <= farSq && !lying ? 1 : 2;
            }
            lod_[i] = lod;
            shadow_[i] = shadow;

            if (lying) {
                // Lying along the heading, raised onto the people underneath; tinted so a
                // pile stands out from the crowd around it. Negative scale = lying (vs_agent).
                const glm::vec3 tint = a.color * 0.45f + glm::vec3(0.55f, 0.08f, 0.08f);
                const float lift = 0.28f * static_cast<float>(std::max<int>(0, a.pileLayer - 1));
                capsule_[i] = CapsuleInstance{p.x, p.y + lift, p.z, heading, tint.r, tint.g, tint.b,
                                              -a.radius / in.capsuleRadius};
            } else {
                capsule_[i] = CapsuleInstance{p.x,       p.y,       p.z,       heading,
                                              a.color.r, a.color.g, a.color.b, a.radius / in.capsuleRadius};
            }
            if (lod == 1 || lod == 2 || shadow == 1) {
                const float scale = std::clamp(1.0f + 0.5f * (a.radius / character_->radius - 1.0f), 0.9f, 1.1f);
                animation::GaitState& gait = slots_[agentSlot_[i]].gait;
                const animation::GaitSample g =
                    animation::updateGait(*character_, gait, a.speed, a.turnRate, scale, in.dt);
                vat_[i] = VatInstance{{p.x, p.y, p.z, heading},
                                      {a.color.r, a.color.g, a.color.b, scale},
                                      {g.rowA0, g.rowA1, g.fracA, g.blend},
                                      {g.rowB0, g.rowB1, g.fracB, hash01(a.agentId)}};
            }
        }
    };
    if (jobs != nullptr && n > 2048) {
        jobs->parallelFor(n, 1024, [&](core::IndexRange r, std::uint32_t) { body(r.begin, r.end); });
    } else {
        body(0, n);
    }

    // Compact (serial, in agent order: deterministic draw order).
    for (std::uint32_t i = 0; i < n; ++i) {
        switch (lod_[i]) {
            case 0:
                ++lists_.culled;
                break;
            case 1:
                lists_.near.push_back(vat_[i]);
                break;
            case 2:
                lists_.mid.push_back(vat_[i]);
                break;
            default:
                lists_.far.push_back(capsule_[i]);
                break;
        }
        if (shadow_[i] == 1) {
            lists_.shadowVat.push_back(vat_[i]);
        } else if (shadow_[i] == 2) {
            lists_.shadowCapsule.push_back(capsule_[i]);
        }
    }
}

std::uint32_t CrowdRenderer::drawVat(bgfx::ViewId view, int lod, const std::vector<VatInstance>& instances,
                                     bgfx::ProgramHandle program, std::uint64_t state,
                                     const std::function<void()>& bind) {
    if (!character_ || instances.empty() || !bgfx::isValid(program)) {
        return 0;
    }
    constexpr std::uint16_t kStride = sizeof(VatInstance);
    const auto total = static_cast<std::uint32_t>(instances.size());
    std::uint32_t offset = 0;
    std::uint32_t draws = 0;
    while (offset < total) {
        const std::uint32_t count = bgfx::getAvailInstanceDataBuffer(total - offset, kStride);
        if (count == 0) {
            break;  // out of transient memory this frame
        }
        bgfx::InstanceDataBuffer idb;
        bgfx::allocInstanceDataBuffer(&idb, count, kStride);
        std::memcpy(idb.data, instances.data() + offset, static_cast<std::size_t>(count) * kStride);
        bgfx::setVertexBuffer(0, gpu_.vb[lod]);
        bgfx::setIndexBuffer(gpu_.ib[lod]);
        bgfx::setInstanceDataBuffer(&idb);
        bgfx::setTexture(0, gpu_.bonesSampler, gpu_.bones);
        if (bind) {
            bind();
        }
        bgfx::setState(state);
        bgfx::submit(view, program);
        ++draws;
        offset += count;
    }
    return draws;
}

}  // namespace crowdsim::render
