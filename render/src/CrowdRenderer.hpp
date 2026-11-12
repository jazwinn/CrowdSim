#pragma once

/// @file CrowdRenderer.hpp
/// Per-frame crowd preparation and VAT character drawing (internal to render/).
///
/// prepare() runs in parallel over the snapshot's agents: interpolates
/// position/heading, frustum-culls against the camera and the shadow light,
/// picks a LOD by distance (VAT near / VAT mid / capsule far), advances each
/// visible agent's gait (state kept per agent id) and fills compact instance
/// lists for the camera pass and the shadow pass.

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "crowdsim/animation/Gait.hpp"
#include "crowdsim/snapshot/CharacterAsset.hpp"

namespace crowdsim::core {
class JobSystem;
}
namespace crowdsim::snapshot {
struct RenderSnapshot;
}

namespace crowdsim::render {

/// Matches i_data0..3 in vs_crowd.sc.
struct VatInstance {
    float d0[4];  ///< x, y, z, heading
    float d1[4];  ///< shirt rgb, scale
    float d2[4];  ///< clip A row0, row1, frac, blend
    float d3[4];  ///< clip B row0, row1, frac, hash
};
static_assert(sizeof(VatInstance) == 64);

/// Matches i_data0..1 in vs_agent.sc.
struct CapsuleInstance {
    float x, y, z, heading;
    float r, g, b, scale;
};
static_assert(sizeof(CapsuleInstance) == 32);

/// Six inward-facing planes (a, b, c, d): inside when a*x + b*y + c*z + d >= 0.
struct Frustum {
    std::array<glm::vec4, 6> planes{};
    /// From a bx-convention (row-vector) view * projection matrix.
    static Frustum fromViewProj(const float* viewProj16, bool homogeneousDepth);
    [[nodiscard]] bool intersectsSphere(glm::vec3 c, float r) const noexcept;
};

struct CrowdFrameInput {
    const snapshot::RenderSnapshot* snapshot = nullptr;
    float alpha = 1.0f;
    float dt = 1.0f / 60.0f;
    glm::vec3 eye{0.0f};
    Frustum camera;
    Frustum light;
    bool shadows = true;
    bool animated = true;
    float lodNear = 30.0f;
    float lodFar = 90.0f;
    float capsuleRadius = 0.28f;  ///< radius the capsule mesh was built with
};

struct CrowdLists {
    std::vector<VatInstance> near;
    std::vector<VatInstance> mid;
    std::vector<CapsuleInstance> far;
    std::vector<VatInstance> shadowVat;          ///< casters drawn with the mid mesh
    std::vector<CapsuleInstance> shadowCapsule;  ///< distant casters
    std::uint32_t culled = 0;
};

class CrowdRenderer {
public:
    CrowdRenderer();
    ~CrowdRenderer();
    CrowdRenderer(const CrowdRenderer&) = delete;
    CrowdRenderer& operator=(const CrowdRenderer&) = delete;

    /// Upload a character (mesh LODs + bone texture). Null clears it.
    void setCharacter(std::shared_ptr<const snapshot::CharacterAsset> character);
    [[nodiscard]] bool hasCharacter() const noexcept { return character_ != nullptr; }

    /// Build this frame's instance lists. `jobs` may be null (single-threaded).
    void prepare(const CrowdFrameInput& in, core::JobSystem* jobs);
    [[nodiscard]] const CrowdLists& lists() const noexcept { return lists_; }

    /// Submit VAT instances with mesh LOD `lod` (0 near, 1 mid). `bind` sets per-draw
    /// uniforms / textures (called once per instance batch). Returns draw calls.
    std::uint32_t drawVat(bgfx::ViewId view, int lod, const std::vector<VatInstance>& instances,
                          bgfx::ProgramHandle program, std::uint64_t state, const std::function<void()>& bind);

private:
    struct Gpu {
        bgfx::VertexBufferHandle vb[2] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
        bgfx::IndexBufferHandle ib[2] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
        bgfx::TextureHandle bones = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle bonesSampler = BGFX_INVALID_HANDLE;
        void destroy();
    };

    struct Slot {
        animation::GaitState gait;
        std::uint64_t lastFrame = 0;
        std::uint32_t id = 0;
        bool used = false;
    };

    void assignSlots(const snapshot::RenderSnapshot& snap);

    std::shared_ptr<const snapshot::CharacterAsset> character_;
    Gpu gpu_;
    bgfx::VertexLayout layout_;

    std::vector<std::uint32_t> slotOf_;  ///< agent id -> slot + 1 (0 = none); ids are dense
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::uint64_t frame_ = 0;

    // Per-agent scratch filled in parallel, then compacted.
    std::vector<std::uint32_t> agentSlot_;
    std::vector<std::uint8_t> lod_;     ///< 0 culled, 1 near, 2 mid, 3 far
    std::vector<std::uint8_t> shadow_;  ///< 0 none, 1 VAT, 2 capsule
    std::vector<VatInstance> vat_;
    std::vector<CapsuleInstance> capsule_;
    CrowdLists lists_;
};

}  // namespace crowdsim::render
