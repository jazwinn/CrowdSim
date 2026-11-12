// M5: procedural character baking and the gait controller.

#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "crowdsim/animation/Gait.hpp"
#include "crowdsim/animation/ProceduralCharacter.hpp"

using namespace crowdsim;
using snapshot::ClipId;

namespace {

/// Model-space position of joint `name` at texture row `row` (skin matrix * bind position).
struct Vec3 {
    float x, y, z;
};
Vec3 jointAt(const snapshot::CharacterAsset& c, std::uint32_t row, const std::string& name) {
    std::size_t j = 0;
    while (c.jointNames[j] != name) {
        ++j;
    }
    const float* b = &c.jointBindPositions[j * 3];
    const float* m = &c.boneTexture[(static_cast<std::size_t>(row) * c.textureWidth + j * 3) * 4];
    Vec3 out{};
    float* o[3] = {&out.x, &out.y, &out.z};
    for (int r = 0; r < 3; ++r) {
        const float* rowM = m + r * 4;
        *o[r] = rowM[0] * b[0] + rowM[1] * b[1] + rowM[2] * b[2] + rowM[3];
    }
    return out;
}

}  // namespace

TEST_CASE("Procedural character bakes a consistent VAT and LOD meshes") {
    const snapshot::CharacterAsset c = animation::buildProceduralCharacter();
    CHECK(c.jointCount == 18);
    CHECK(c.textureWidth == c.jointCount * 3);
    REQUIRE(c.clips.size() == static_cast<std::size_t>(ClipId::Count));
    std::uint32_t rows = 0;
    for (const snapshot::AnimationClip& clip : c.clips) {
        CHECK(clip.firstRow == rows);
        CHECK(clip.frameCount > 8);
        rows += clip.frameCount;
    }
    CHECK(c.textureHeight == rows);
    CHECK(c.boneTexture.size() == static_cast<std::size_t>(c.textureWidth) * c.textureHeight * 4);
    REQUIRE(c.lods.size() == 2);
    CHECK(c.lods[1].indices.size() < c.lods[0].indices.size() / 2);
    for (const snapshot::CharacterMesh& mesh : c.lods) {
        CHECK(mesh.indices.size() % 3 == 0);
        for (std::uint16_t i : mesh.indices) {
            REQUIRE(i < mesh.vertices.size());
        }
        for (const snapshot::SkinnedVertex& v : mesh.vertices) {
            CHECK(v.joints[0] < c.jointCount);
            CHECK(v.joints[1] < c.jointCount);
            CHECK(v.weights[0] + v.weights[1] == 255);
        }
    }
    // The head ends up near the top of the character in every frame.
    for (std::uint32_t r = 0; r < c.textureHeight; r += 7) {
        const Vec3 head = jointAt(c, r, "head");
        CHECK(head.y > 1.35f);
        CHECK(head.y < 1.75f);
    }
}

TEST_CASE("Walk and run: a planted foot moves backwards at the clip's natural speed and touches the ground") {
    const snapshot::CharacterAsset c = animation::buildProceduralCharacter();
    for (const ClipId id : {ClipId::Walk, ClipId::Run}) {
        CAPTURE(static_cast<int>(id));
        const snapshot::AnimationClip& clip = c.clip(id);
        const float stance = id == ClipId::Walk ? 0.60f : 0.36f;
        // Flat-foot window of the left foot: 12%..65% of its stance.
        const auto first = static_cast<std::uint32_t>(std::ceil(0.15f * stance * static_cast<float>(clip.frameCount)));
        const auto last = static_cast<std::uint32_t>(std::floor(0.60f * stance * static_cast<float>(clip.frameCount)));
        REQUIRE(last > first);
        const Vec3 a = jointAt(c, clip.firstRow + first, "foot_l");
        const Vec3 b = jointAt(c, clip.firstRow + last, "foot_l");
        const float dPhase = static_cast<float>(last - first) / static_cast<float>(clip.frameCount);
        const float metresPerCycle = -(b.z - a.z) / dPhase;
        CHECK(metresPerCycle == doctest::Approx(clip.cycleDistance).epsilon(0.06));
        CHECK(a.y == doctest::Approx(0.07f).epsilon(0.15));  // ankle at its standing height: on the ground
        CHECK(b.y == doctest::Approx(0.07f).epsilon(0.15));
        CHECK(std::abs(b.x - a.x) < 0.01f);
    }
}

TEST_CASE("Gait controller: distance-driven phase and clip selection by speed") {
    const snapshot::CharacterAsset c = animation::buildProceduralCharacter();
    animation::GaitState s = animation::initialGaitState(7);
    const float start = s.gaitPhase;
    const float walkCycle = c.clip(ClipId::Walk).cycleDistance;

    // Walking at exactly the clip speed for a quarter cycle advances the phase a quarter.
    animation::GaitSample g{};
    const float speed = walkCycle / c.clip(ClipId::Walk).duration;
    for (int i = 0; i < 25; ++i) {
        g = animation::updateGait(c, s, speed, 0.0f, 1.0f, 0.01f);
    }
    float advanced = s.gaitPhase - start;
    advanced -= std::floor(advanced);
    CHECK(advanced == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(g.clipB == ClipId::Walk);
    CHECK(g.blend == doctest::Approx(1.0f));

    // A larger character takes proportionally longer strides.
    animation::GaitState big = animation::initialGaitState(7);
    for (int i = 0; i < 25; ++i) {
        (void)animation::updateGait(c, big, speed, 0.0f, 1.25f, 0.01f);
    }
    float bigAdvanced = big.gaitPhase - start;
    bigAdvanced -= std::floor(bigAdvanced);
    CHECK(bigAdvanced == doctest::Approx(0.2f).epsilon(0.01));

    // Standing still -> idle; turning on the spot -> turn clip; fast -> run.
    animation::GaitState idle = animation::initialGaitState(1);
    g = animation::updateGait(c, idle, 0.0f, 0.0f, 1.0f, 0.016f);
    CHECK(g.clipA == ClipId::Idle);
    CHECK(g.blend == doctest::Approx(0.0f));
    g = animation::updateGait(c, idle, 0.0f, 2.0f, 1.0f, 0.016f);
    CHECK(g.clipB == ClipId::Turn);
    CHECK(g.blend > 0.9f);
    animation::GaitState fast = animation::initialGaitState(2);
    g = animation::updateGait(c, fast, 3.2f, 0.0f, 1.0f, 0.016f);
    CHECK(g.clipB == ClipId::Run);
    CHECK(g.blend == doctest::Approx(1.0f));
    // Rows stay inside the clip and the fraction in [0, 1).
    CHECK(g.rowB0 >= static_cast<float>(c.clip(ClipId::Run).firstRow));
    CHECK(g.rowB1 < static_cast<float>(c.clip(ClipId::Run).firstRow + c.clip(ClipId::Run).frameCount));
    CHECK(g.fracB >= 0.0f);
    CHECK(g.fracB < 1.0f);
}
