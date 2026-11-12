#pragma once

/// @file ProceduralCharacter.hpp
/// A stylised humanoid built in code (no external art): an 18-joint skeleton,
/// two skinned LOD meshes and four looping clips (idle, walk, run, turn in
/// place). Leg poses come from analytic two-bone IK against authored foot
/// trajectories, so planted feet move backwards at exactly the clip's natural
/// speed and every clip's metres-per-cycle is known. The clips are built and
/// sampled with ozz-animation and baked into a bone-matrix VAT.

#include <cstdint>

#include "crowdsim/snapshot/CharacterAsset.hpp"

namespace crowdsim::animation {

struct BakeOptions {
    std::uint32_t idleFrames = 48;
    std::uint32_t walkFrames = 32;
    std::uint32_t runFrames = 24;
    std::uint32_t turnFrames = 24;
    std::uint32_t keysPerCycle = 32;  ///< authored keys per clip before ozz sampling
};

/// Build and bake the standard crowd character. Throws std::runtime_error if
/// ozz rejects the skeleton or a clip (a programming error).
[[nodiscard]] snapshot::CharacterAsset buildProceduralCharacter(const BakeOptions& options = {});

/// Foot (ankle) height above the ground for `clip` at `phase`, in the clip's
/// model space, for tests: {left, right} as (z forward, y up).
struct FootSample {
    float leftZ, leftY, rightZ, rightY;
};
[[nodiscard]] FootSample sampleFeet(snapshot::ClipId clip, float phase);

}  // namespace crowdsim::animation
