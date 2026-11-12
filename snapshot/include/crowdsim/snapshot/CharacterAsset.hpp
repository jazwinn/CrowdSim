#pragma once

/// @file CharacterAsset.hpp
/// A crowd character ready for GPU instancing: skinned meshes (one per LOD) and
/// its animation clips baked into a bone-matrix vertex animation texture (VAT).
/// Plain data: produced by animation/ (ozz baking), consumed by render/.
///
/// Texture layout: RGBA32F, `textureWidth = jointCount * 3`, one row per baked
/// frame. Texels (3j, 3j+1, 3j+2) of a row hold the top three rows of joint j's
/// skinning matrix (model * inverse bind) for that frame. Clips occupy
/// consecutive rows; looping clips wrap within their own rows.

#include <cstdint>
#include <string>
#include <vector>

namespace crowdsim::snapshot {

/// Skinned vertex (36 bytes). `material` in color[3]: 255 = shirt (tinted by
/// the agent colour), 170 = trousers, 85 = skin, 0 = fixed colour (rgb).
struct SkinnedVertex {
    float x, y, z;
    float nx, ny, nz;
    std::uint8_t joints[4];
    std::uint8_t weights[4];  ///< normalized, sum 255
    std::uint8_t color[4];
};
static_assert(sizeof(SkinnedVertex) == 36);

struct CharacterMesh {
    std::vector<SkinnedVertex> vertices;
    std::vector<std::uint16_t> indices;
};

/// One baked clip.
struct AnimationClip {
    std::string name;
    std::uint32_t firstRow = 0;
    std::uint32_t frameCount = 0;
    float duration = 1.0f;       ///< seconds per cycle at natural playback
    float cycleDistance = 0.0f;  ///< metres the character advances per cycle (0 = in place)
};

/// Standard clip slots, in `clips` order.
enum class ClipId : std::uint8_t { Idle = 0, Walk = 1, Run = 2, Turn = 3, Count = 4 };

struct CharacterAsset {
    std::string name;
    float height = 1.75f;
    float radius = 0.27f;  ///< footprint radius the mesh was built for
    std::uint32_t jointCount = 0;
    std::uint32_t textureWidth = 0;
    std::uint32_t textureHeight = 0;
    std::vector<float> boneTexture;         ///< textureWidth * textureHeight * 4 floats
    std::vector<std::string> jointNames;    ///< by joint index (texture column / 3)
    std::vector<float> jointBindPositions;  ///< xyz per joint, model space
    std::vector<AnimationClip> clips;
    std::vector<CharacterMesh> lods;  ///< 0 = near (full detail), 1 = mid

    [[nodiscard]] const AnimationClip& clip(ClipId id) const { return clips[static_cast<std::size_t>(id)]; }
};

}  // namespace crowdsim::snapshot
