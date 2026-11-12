#include "crowdsim/animation/ProceduralCharacter.hpp"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace crowdsim::animation {

namespace {

constexpr float kPi = 3.14159265358979f;

// --------------------------------------------------------------------------- skeleton

enum Joint : int {
    Root,
    Pelvis,
    Spine,
    Chest,
    Neck,
    Head,
    UpperArmR,
    ForearmR,
    HandR,
    UpperArmL,
    ForearmL,
    HandL,
    ThighR,
    ShinR,
    FootR,
    ThighL,
    ShinL,
    FootL,
    kJointCount
};

struct JointDef {
    const char* name;
    int parent;
    glm::vec3 offset;  ///< bind translation relative to the parent (bind rotations are identity)
};

// +Y up, the character faces +Z, its right side is +X. Arms hang down in the bind pose.
const std::array<JointDef, kJointCount> kJoints = {{
    {"root", -1, {0.0f, 0.0f, 0.0f}},
    {"pelvis", Root, {0.0f, 0.95f, 0.0f}},
    {"spine", Pelvis, {0.0f, 0.11f, 0.0f}},
    {"chest", Spine, {0.0f, 0.21f, 0.0f}},
    {"neck", Chest, {0.0f, 0.20f, 0.0f}},
    {"head", Neck, {0.0f, 0.09f, 0.0f}},
    {"upperarm_r", Chest, {0.205f, 0.15f, 0.0f}},
    {"forearm_r", UpperArmR, {0.0f, -0.28f, 0.0f}},
    {"hand_r", ForearmR, {0.0f, -0.25f, 0.0f}},
    {"upperarm_l", Chest, {-0.205f, 0.15f, 0.0f}},
    {"forearm_l", UpperArmL, {0.0f, -0.28f, 0.0f}},
    {"hand_l", ForearmL, {0.0f, -0.25f, 0.0f}},
    {"thigh_r", Pelvis, {0.095f, -0.04f, 0.0f}},
    {"shin_r", ThighR, {0.0f, -0.42f, 0.0f}},
    {"foot_r", ShinR, {0.0f, -0.42f, 0.0f}},
    {"thigh_l", Pelvis, {-0.095f, -0.04f, 0.0f}},
    {"shin_l", ThighL, {0.0f, -0.42f, 0.0f}},
    {"foot_l", ShinL, {0.0f, -0.42f, 0.0f}},
}};

constexpr float kThigh = 0.42f;
constexpr float kShin = 0.42f;
constexpr float kLegReach = 0.835f;  ///< slightly less than thigh + shin: never lock the knee
constexpr float kHipDrop = 0.04f;    ///< hip joints sit this far below the pelvis joint
constexpr float kFootX = 0.10f;      ///< lateral ankle placement while walking

glm::vec3 bindModelPosition(int j) {
    glm::vec3 p(0.0f);
    for (int k = j; k >= 0; k = kJoints[static_cast<std::size_t>(k)].parent) {
        p += kJoints[static_cast<std::size_t>(k)].offset;
    }
    return p;
}

glm::quat rotX(float a) {
    return glm::angleAxis(a, glm::vec3(1.0f, 0.0f, 0.0f));
}
glm::quat rotY(float a) {
    return glm::angleAxis(a, glm::vec3(0.0f, 1.0f, 0.0f));
}
glm::quat rotZ(float a) {
    return glm::angleAxis(a, glm::vec3(0.0f, 0.0f, 1.0f));
}

float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float wrap01(float x) {
    return x - std::floor(x);
}

// --------------------------------------------------------------------------- poses

struct Pose {
    std::array<glm::quat, kJointCount> rot;
    glm::vec3 pelvis{0.0f, kJoints[Pelvis].offset.y, 0.0f};
    Pose() { rot.fill(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)); }
};

/// Ankle position (z forward, y up, relative to the point under the hips) and foot pitch.
struct FootPose {
    float z = 0.0f;
    float y = 0.07f;
    float pitch = 0.0f;  ///< rotation about +X; positive lifts the heel
};

struct GaitParams {
    float stance = 0.6f;   ///< fraction of the cycle a foot is on the ground
    float travel = 0.78f;  ///< ground distance passing under a planted foot (= cycleDistance * stance)
    float reach = 0.30f;   ///< heel position ahead of the hips at heel strike
    float lift = 0.10f;    ///< swing apex height
    float strikePitch = -0.25f;
    float toeOffPitch = 0.55f;
    float pelvisHeight = 0.93f;
    float pelvisBob = 0.02f;  ///< + : highest at mid stance (walk); - : lowest at mid stance (run)
};

struct UpperBody {
    float lean = 0.05f;
    float armSwing = 0.32f;
    float armAbduct = 0.08f;
    float elbowBase = 0.25f;
    float elbowSwing = 0.25f;
    float pelvisYaw = 0.07f;
    float pelvisRoll = 0.03f;
    float sway = 0.015f;
};

// Foot geometry relative to the ankle, in the foot frame (z forward, y up).
constexpr glm::vec2 kHeel{-0.06f, -0.07f};
constexpr glm::vec2 kToe{0.17f, -0.07f};

/// Rotate a (z, y) offset about +X by `p`.
glm::vec2 pitchOffset(glm::vec2 zy, float p) {
    const float s = std::sin(p);
    const float c = std::cos(p);
    return {zy.y * s + zy.x * c, zy.y * c - zy.x * s};
}

/// Foot at phase `u` of its own cycle (u = 0: heel strike). During stance the
/// ground contact point (heel, then toe) stays fixed on a ground line that moves
/// backwards at the clip's natural speed; during swing the foot returns forward.
FootPose footAt(const GaitParams& g, float u) {
    auto stance = [&](float s) {
        const float ground = g.reach - g.travel * s;  // where the heel touched down, moved with the ground
        constexpr float kHeelEnd = 0.12f;
        constexpr float kToeStart = 0.65f;
        FootPose f;
        if (s < kHeelEnd) {  // heel rocker: toes come down
            f.pitch = g.strikePitch * (1.0f - s / kHeelEnd);
            const glm::vec2 off = pitchOffset(kHeel, f.pitch);
            f.z = ground - off.x;
            f.y = -off.y;
        } else if (s < kToeStart) {  // flat foot
            f.z = ground - kHeel.x;
            f.y = -kHeel.y;
        } else {  // heel off: roll over the toes
            const float t = (s - kToeStart) / (1.0f - kToeStart);
            f.pitch = g.toeOffPitch * t * t;
            const float toe = ground + (kToe.x - kHeel.x);
            const glm::vec2 off = pitchOffset(kToe, f.pitch);
            f.z = toe - off.x;
            f.y = -off.y;
        }
        return f;
    };
    u = wrap01(u);
    if (u < g.stance) {
        return stance(u / g.stance);
    }
    const float w = (u - g.stance) / (1.0f - g.stance);
    const FootPose a = stance(1.0f);
    const FootPose b = stance(0.0f);
    const float e = w * w * w * (w * (w * 6.0f - 15.0f) + 10.0f);
    FootPose f;
    f.z = a.z + (b.z - a.z) * e;
    f.y = a.y + (b.y - a.y) * w + g.lift * std::sin(kPi * std::pow(w, 0.75f));
    f.pitch = a.pitch + (b.pitch - a.pitch) * smoothstep01(w);
    return f;
}

/// Thigh and shin pitch placing the ankle at (dz, dy) from the hip (two-bone IK
/// in the sagittal plane; knees bend forward).
void legIk(float dz, float dy, float& thigh, float& shin) {
    const float d = std::clamp(std::sqrt(dz * dz + dy * dy), 0.05f, kThigh + kShin - 1e-4f);
    const float cosKnee = (kThigh * kThigh + kShin * kShin - d * d) / (2.0f * kThigh * kShin);
    const float bend = kPi - std::acos(std::clamp(cosKnee, -1.0f, 1.0f));
    const float cosHip = (kThigh * kThigh + d * d - kShin * kShin) / (2.0f * kThigh * d);
    const float beta = std::acos(std::clamp(cosHip, -1.0f, 1.0f));
    thigh = std::atan2(-dz, -dy) - beta;
    shin = bend;
}

/// Full-body pose from foot placements and upper-body settings. `armPhase` in
/// radians: arms swing with cos(armPhase), right arm forward at 0.
Pose buildPose(const FootPose& left, const FootPose& right, float footX, float pelvisHeight, float pelvisX,
               float pelvisYaw, float pelvisRoll, float armPhase, const UpperBody& ub, float breath, float headYaw) {
    Pose pose;
    const glm::quat pelvisRot = rotY(pelvisYaw) * rotZ(pelvisRoll);

    // Lower the pelvis where a leg could not otherwise reach its foot.
    float height = pelvisHeight;
    const FootPose* feet[2] = {&right, &left};
    const float sides[2] = {1.0f, -1.0f};
    for (int k = 0; k < 2; ++k) {
        const glm::vec3 hipOffset = pelvisRot * glm::vec3(sides[k] * 0.095f, -kHipDrop, 0.0f);
        const float dx = sides[k] * footX - (pelvisX + hipOffset.x);
        const float dz = feet[k]->z - hipOffset.z;
        const float horizontal = dx * dx + dz * dz;
        if (horizontal < kLegReach * kLegReach) {
            height = std::min(height, feet[k]->y + std::sqrt(kLegReach * kLegReach - horizontal) - hipOffset.y);
        }
    }
    pose.pelvis = glm::vec3(pelvisX, height, 0.0f);
    pose.rot[Pelvis] = pelvisRot;

    // Legs: IK in the pelvis frame, then a little roll to reach the foot's x.
    const Joint thighs[2] = {ThighR, ThighL};
    const Joint shins[2] = {ShinR, ShinL};
    const Joint footJoints[2] = {FootR, FootL};
    const glm::quat invPelvis = glm::inverse(pelvisRot);
    for (int k = 0; k < 2; ++k) {
        const glm::vec3 hip = pose.pelvis + pelvisRot * kJoints[static_cast<std::size_t>(thighs[k])].offset;
        const glm::vec3 ankle(sides[k] * footX, feet[k]->y, feet[k]->z);
        const glm::vec3 local = invPelvis * (ankle - hip);
        float thigh = 0.0f;
        float shin = 0.0f;
        legIk(local.z, local.y, thigh, shin);
        const float roll = std::asin(std::clamp(local.x / std::max(0.2f, glm::length(local)), -0.5f, 0.5f));
        pose.rot[thighs[k]] = rotZ(roll) * rotX(thigh);
        pose.rot[shins[k]] = rotX(shin);
        pose.rot[footJoints[k]] = rotX(feet[k]->pitch - thigh - shin);
    }

    // Torso counter-rotates against the hips; the head stays level.
    pose.rot[Spine] = rotX(ub.lean * 0.5f) * rotY(-0.8f * pelvisYaw);
    pose.rot[Chest] = rotX(ub.lean * 0.5f + breath) * rotY(-0.8f * pelvisYaw);
    pose.rot[Neck] = rotX(-ub.lean * 0.6f);
    pose.rot[Head] = rotY(headYaw + 0.6f * pelvisYaw) * rotX(-ub.lean * 0.4f - breath);

    const float swing = std::cos(armPhase);
    const float armR = -ub.armSwing * swing;  // negative = forward
    const float armL = ub.armSwing * swing;
    pose.rot[UpperArmR] = rotZ(ub.armAbduct) * rotX(armR);
    pose.rot[UpperArmL] = rotZ(-ub.armAbduct) * rotX(armL);
    const float flexR = ub.elbowBase + ub.elbowSwing * std::max(0.0f, -armR / std::max(0.01f, ub.armSwing));
    const float flexL = ub.elbowBase + ub.elbowSwing * std::max(0.0f, -armL / std::max(0.01f, ub.armSwing));
    pose.rot[ForearmR] = rotX(-flexR);
    pose.rot[ForearmL] = rotX(-flexL);
    pose.rot[HandR] = rotX(-0.1f);
    pose.rot[HandL] = rotX(-0.1f);
    return pose;
}

Pose gaitPose(const GaitParams& g, const UpperBody& ub, float phase) {
    const FootPose left = footAt(g, phase);
    const FootPose right = footAt(g, phase + 0.5f);
    const float midStance = 0.5f * g.stance;
    const float height = g.pelvisHeight + g.pelvisBob * std::cos(4.0f * kPi * (phase - midStance));
    const float c = std::cos(2.0f * kPi * phase);
    // Left heel strike at phase 0: left hip forward, right arm forward.
    return buildPose(left, right, kFootX, height, ub.sway * std::sin(2.0f * kPi * (phase - midStance)),
                     ub.pelvisYaw * c, ub.pelvisRoll * std::sin(2.0f * kPi * phase), 2.0f * kPi * phase, ub, 0.0f,
                     0.0f);
}

// Clip definitions ----------------------------------------------------------------------

const GaitParams kWalk{0.60f, 1.30f * 0.60f, 0.30f, 0.10f, -0.25f, 0.55f, 0.93f, 0.02f};
const UpperBody kWalkUpper{0.05f, 0.32f, 0.08f, 0.25f, 0.25f, 0.07f, 0.03f, 0.015f};
constexpr float kWalkDuration = 1.0f;
constexpr float kWalkDistance = 1.30f;

const GaitParams kRun{0.36f, 2.10f * 0.36f, 0.28f, 0.28f, -0.08f, 0.70f, 0.90f, -0.035f};
const UpperBody kRunUpper{0.20f, 0.55f, 0.12f, 1.35f, 0.25f, 0.10f, 0.02f, 0.01f};
constexpr float kRunDuration = 0.70f;
constexpr float kRunDistance = 2.10f;

const GaitParams kTurn{0.60f, 0.0f, 0.03f, 0.06f, 0.0f, 0.15f, 0.93f, 0.005f};
const UpperBody kTurnUpper{0.02f, 0.08f, 0.10f, 0.2f, 0.05f, 0.03f, 0.02f, 0.01f};
constexpr float kTurnDuration = 0.9f;

constexpr float kIdleDuration = 4.0f;

Pose idlePose(float phase) {
    const float a = 2.0f * kPi * phase;
    const FootPose foot{0.02f, 0.07f, 0.0f};
    UpperBody ub{0.0f, 0.04f, 0.10f, 0.15f, 0.0f, 0.0f, 0.0f, 0.0f};
    return buildPose(foot, foot, 0.11f, 0.935f + 0.004f * std::sin(2.0f * a), 0.012f * std::sin(a), 0.03f * std::sin(a),
                     0.01f * std::sin(a), a + 1.0f, ub, 0.015f * std::sin(3.0f * a), 0.2f * std::sin(a + 1.0f));
}

Pose clipPose(snapshot::ClipId clip, float phase) {
    switch (clip) {
        case snapshot::ClipId::Idle:
            return idlePose(phase);
        case snapshot::ClipId::Walk:
            return gaitPose(kWalk, kWalkUpper, phase);
        case snapshot::ClipId::Run:
            return gaitPose(kRun, kRunUpper, phase);
        case snapshot::ClipId::Turn:
        case snapshot::ClipId::Count:
            break;
    }
    return gaitPose(kTurn, kTurnUpper, phase);
}

// --------------------------------------------------------------------------- mesh

struct Material {
    std::uint8_t r, g, b, code;
};
constexpr Material kShirt{230, 230, 230, 255};
constexpr Material kTrousers{60, 64, 80, 170};
constexpr Material kSkin{224, 172, 140, 85};
constexpr Material kHair{50, 36, 28, 40};
constexpr Material kShoes{38, 36, 34, 0};

class MeshBuilder {
public:
    explicit MeshBuilder(const std::array<int, kJointCount>& toOzz) : toOzz_(toOzz) {}

    std::uint16_t vertex(glm::vec3 p, glm::vec3 n, int j0, int j1, float w1, Material m) {
        snapshot::SkinnedVertex v{};
        n = glm::normalize(n);
        v.x = p.x;
        v.y = p.y;
        v.z = p.z;
        v.nx = n.x;
        v.ny = n.y;
        v.nz = n.z;
        const auto b = static_cast<std::uint8_t>(std::lround(std::clamp(w1, 0.0f, 1.0f) * 255.0f));
        v.joints[0] = static_cast<std::uint8_t>(toOzz_[static_cast<std::size_t>(j0)]);
        v.joints[1] = static_cast<std::uint8_t>(toOzz_[static_cast<std::size_t>(j1 >= 0 ? j1 : j0)]);
        v.weights[0] = static_cast<std::uint8_t>(255 - b);
        v.weights[1] = b;
        v.color[0] = m.r;
        v.color[1] = m.g;
        v.color[2] = m.b;
        v.color[3] = m.code;
        mesh.vertices.push_back(v);
        return static_cast<std::uint16_t>(mesh.vertices.size() - 1);
    }

    /// Triangle wound so its face normal agrees with the vertex normals (outward).
    void tri(std::uint16_t a, std::uint16_t b, std::uint16_t c) {
        const auto pos = [&](std::uint16_t i) {
            const auto& v = mesh.vertices[i];
            return glm::vec3(v.x, v.y, v.z);
        };
        const auto nrm = [&](std::uint16_t i) {
            const auto& v = mesh.vertices[i];
            return glm::vec3(v.nx, v.ny, v.nz);
        };
        const glm::vec3 face = glm::cross(pos(b) - pos(a), pos(c) - pos(a));
        if (glm::dot(face, nrm(a) + nrm(b) + nrm(c)) < 0.0f) {
            std::swap(b, c);
        }
        mesh.indices.insert(mesh.indices.end(), {a, b, c});
    }

    /// Vertical elliptic tube from y0 to y1 (the bone direction), blended with the
    /// parent joint near y0 and the child joint near y1 for smooth bends.
    void tube(glm::vec2 xz, float y0, float y1, glm::vec2 r0, glm::vec2 r1, int owner, int parent, int child,
              float blend, int rings, int segs, Material m, bool cap0, bool cap1) {
        std::vector<std::uint16_t> ring(static_cast<std::size_t>(segs));
        std::vector<std::uint16_t> prev;
        std::vector<std::uint16_t> first;
        const float slopeX = (r1.x - r0.x) / (y1 - y0);
        const float slopeZ = (r1.y - r0.y) / (y1 - y0);
        for (int i = 0; i < rings; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(rings - 1);
            const float y = y0 + (y1 - y0) * t;
            const glm::vec2 r = r0 + (r1 - r0) * t;
            int j1 = owner;
            float w1 = 0.0f;
            if (parent >= 0 && t < blend) {
                j1 = parent;
                w1 = 0.5f * (1.0f - t / blend);
            } else if (child >= 0 && t > 1.0f - blend) {
                j1 = child;
                w1 = 0.5f * (t - (1.0f - blend)) / blend;
            }
            for (int s = 0; s < segs; ++s) {
                const float a = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segs);
                const float cx = std::sin(a);
                const float cz = std::cos(a);
                const glm::vec3 p(xz.x + cx * r.x, y, xz.y + cz * r.y);
                const glm::vec3 n(cx / r.x, -(cx * cx * slopeX / r.x + cz * cz * slopeZ / r.y), cz / r.y);
                ring[static_cast<std::size_t>(s)] = vertex(p, n, owner, j1, w1, m);
            }
            if (!prev.empty()) {
                for (int s = 0; s < segs; ++s) {
                    const auto s1 = static_cast<std::size_t>((s + 1) % segs);
                    const auto s0 = static_cast<std::size_t>(s);
                    tri(prev[s0], prev[s1], ring[s0]);
                    tri(prev[s1], ring[s1], ring[s0]);
                }
            } else {
                first = ring;
            }
            prev = ring;
        }
        auto cap = [&](const std::vector<std::uint16_t>& rr, float y, float sign, int j1, float w1) {
            const std::uint16_t c = vertex({xz.x, y, xz.y}, {0.0f, sign, 0.0f}, owner, j1, w1, m);
            for (int s = 0; s < segs; ++s) {
                tri(c, rr[static_cast<std::size_t>(s)], rr[static_cast<std::size_t>((s + 1) % segs)]);
            }
        };
        const float up = y1 > y0 ? 1.0f : -1.0f;
        if (cap0) {
            cap(first, y0, -up, parent >= 0 ? parent : owner, parent >= 0 ? 0.5f : 0.0f);
        }
        if (cap1) {
            cap(prev, y1, up, child >= 0 ? child : owner, child >= 0 ? 0.5f : 0.0f);
        }
    }

    /// Ellipsoid bound to one joint; vertices below `cutY` are dropped (hair caps).
    void ellipsoid(glm::vec3 c, glm::vec3 r, int owner, int lat, int lon, Material m, float cutY = -1e9f) {
        std::vector<std::vector<int>> grid(static_cast<std::size_t>(lat + 1));
        for (int i = 0; i <= lat; ++i) {
            const float th = kPi * static_cast<float>(i) / static_cast<float>(lat);  // 0 = top
            for (int s = 0; s < lon; ++s) {
                const float ph = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(lon);
                const glm::vec3 d(std::sin(th) * std::sin(ph), std::cos(th), std::sin(th) * std::cos(ph));
                const glm::vec3 p = c + d * r;
                if (p.y < cutY) {
                    grid[static_cast<std::size_t>(i)].push_back(-1);
                    continue;
                }
                grid[static_cast<std::size_t>(i)].push_back(vertex(p, d / r, owner, owner, 0.0f, m));
            }
        }
        for (int i = 0; i < lat; ++i) {
            for (int s = 0; s < lon; ++s) {
                const auto si = static_cast<std::size_t>(s);
                const auto sn = static_cast<std::size_t>((s + 1) % lon);
                const auto& r0 = grid[static_cast<std::size_t>(i)];
                const auto& r1 = grid[static_cast<std::size_t>(i + 1)];
                if (r0[si] >= 0 && r0[sn] >= 0 && r1[si] >= 0) {
                    tri(static_cast<std::uint16_t>(r0[si]), static_cast<std::uint16_t>(r0[sn]),
                        static_cast<std::uint16_t>(r1[si]));
                }
                if (r0[sn] >= 0 && r1[sn] >= 0 && r1[si] >= 0) {
                    tri(static_cast<std::uint16_t>(r0[sn]), static_cast<std::uint16_t>(r1[sn]),
                        static_cast<std::uint16_t>(r1[si]));
                }
            }
        }
    }

    void box(glm::vec3 lo, glm::vec3 hi, int owner, Material m) {
        const glm::vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const glm::vec3& f : n) {
            // Two tangent axes for this face.
            const glm::vec3 u = std::abs(f.x) > 0.5f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            const glm::vec3 v = glm::cross(f, u);
            const glm::vec3 c = (lo + hi) * 0.5f + f * (hi - lo) * 0.5f;
            const glm::vec3 hu = u * (hi - lo) * 0.5f;
            const glm::vec3 hv = v * (hi - lo) * 0.5f;
            const std::uint16_t a = vertex(c - hu - hv, f, owner, owner, 0.0f, m);
            const std::uint16_t b = vertex(c + hu - hv, f, owner, owner, 0.0f, m);
            const std::uint16_t d = vertex(c + hu + hv, f, owner, owner, 0.0f, m);
            const std::uint16_t e = vertex(c - hu + hv, f, owner, owner, 0.0f, m);
            tri(a, b, d);
            tri(a, d, e);
        }
    }

    snapshot::CharacterMesh mesh;

private:
    const std::array<int, kJointCount>& toOzz_;
};

snapshot::CharacterMesh buildMesh(const std::array<int, kJointCount>& toOzz, bool detailed) {
    MeshBuilder b(toOzz);
    const int segs = detailed ? 12 : 6;
    const int rings = detailed ? 5 : 2;
    const int lat = detailed ? 10 : 5;
    const int lon = detailed ? 14 : 7;
    // Torso
    b.tube({0.0f, 0.0f}, 0.80f, 1.03f, {0.165f, 0.11f}, {0.155f, 0.105f}, Pelvis, -1, Spine, 0.25f, rings, segs,
           kTrousers, true, false);
    b.tube({0.0f, 0.0f}, 1.03f, 1.24f, {0.155f, 0.105f}, {0.175f, 0.11f}, Spine, Pelvis, Chest, 0.3f, rings, segs,
           kShirt, false, false);
    b.tube({0.0f, 0.0f}, 1.24f, 1.47f, {0.175f, 0.11f}, {0.155f, 0.10f}, Chest, Spine, -1, 0.25f, rings, segs, kShirt,
           false, true);
    b.tube({0.0f, 0.0f}, 1.45f, 1.60f, {0.05f, 0.05f}, {0.048f, 0.048f}, Neck, Chest, Head, 0.3f, 2, segs, kSkin, false,
           false);
    // Head, hair, nose
    b.ellipsoid({0.0f, 1.67f, 0.01f}, {0.095f, 0.115f, 0.105f}, Head, lat, lon, kSkin);
    b.ellipsoid({0.0f, 1.695f, -0.012f}, {0.102f, 0.108f, 0.113f}, Head, lat, lon, kHair, 1.655f);
    if (detailed) {
        b.ellipsoid({0.0f, 1.655f, 0.108f}, {0.016f, 0.024f, 0.02f}, Head, 4, 6, kSkin);
    }
    // Arms
    for (const float side : {1.0f, -1.0f}) {
        const bool right = side > 0.0f;
        const int upper = right ? UpperArmR : UpperArmL;
        const int fore = right ? ForearmR : ForearmL;
        const int hand = right ? HandR : HandL;
        const float x = side * 0.205f;
        b.ellipsoid({side * 0.19f, 1.42f, 0.0f}, {0.07f, 0.06f, 0.07f}, upper, lat / 2 + 2, lon, kShirt);
        b.tube({x, 0.0f}, 1.43f, 1.15f, {0.055f, 0.055f}, {0.045f, 0.045f}, upper, Chest, fore, 0.15f, rings, segs,
               kShirt, false, false);
        b.tube({x, 0.0f}, 1.15f, 0.90f, {0.043f, 0.043f}, {0.032f, 0.032f}, fore, upper, hand, 0.2f, rings, segs, kSkin,
               false, true);
        b.ellipsoid({x, 0.845f, 0.005f}, {0.03f, 0.06f, 0.04f}, hand, lat / 2 + 2, lon / 2 + 2, kSkin);
    }
    // Legs
    for (const float side : {1.0f, -1.0f}) {
        const bool right = side > 0.0f;
        const int thigh = right ? ThighR : ThighL;
        const int shin = right ? ShinR : ShinL;
        const int foot = right ? FootR : FootL;
        const float x = side * 0.095f;
        b.tube({x, 0.0f}, 0.92f, 0.49f, {0.085f, 0.085f}, {0.058f, 0.058f}, thigh, Pelvis, shin, 0.2f, rings + 1, segs,
               kTrousers, false, false);
        b.tube({x, 0.0f}, 0.49f, 0.08f, {0.056f, 0.056f}, {0.042f, 0.042f}, shin, thigh, foot, 0.2f, rings + 1, segs,
               kTrousers, false, true);
        b.box({x - 0.045f, 0.0f, -0.075f}, {x + 0.045f, 0.10f, 0.19f}, foot, kShoes);
    }
    return std::move(b.mesh);
}

// --------------------------------------------------------------------------- ozz

glm::mat4 toGlm(const ozz::math::Float4x4& m) {
    glm::mat4 out(1.0f);
    for (int c = 0; c < 4; ++c) {
        float v[4];
        ozz::math::StorePtrU(m.cols[c], v);
        out[c] = glm::vec4(v[0], v[1], v[2], v[3]);
    }
    return out;
}

ozz::math::Float3 toOzz(glm::vec3 v) {
    return {v.x, v.y, v.z};
}

void fillRawJoint(int j, ozz::animation::offline::RawSkeleton::Joint& out) {
    out.name = kJoints[static_cast<std::size_t>(j)].name;
    out.transform = ozz::math::Transform::identity();
    out.transform.translation = toOzz(kJoints[static_cast<std::size_t>(j)].offset);
    std::vector<int> children;
    for (int c = 0; c < kJointCount; ++c) {
        if (kJoints[static_cast<std::size_t>(c)].parent == j) {
            children.push_back(c);
        }
    }
    out.children.resize(children.size());
    for (std::size_t k = 0; k < children.size(); ++k) {
        fillRawJoint(children[k], out.children[k]);
    }
}

ozz::unique_ptr<ozz::animation::Animation> buildClip(const ozz::animation::Skeleton& skeleton,
                                                     const std::array<int, kJointCount>& toOzzIndex,
                                                     snapshot::ClipId clip, float duration, std::uint32_t keys) {
    ozz::animation::offline::RawAnimation raw;
    raw.duration = duration;
    raw.tracks.resize(static_cast<std::size_t>(skeleton.num_joints()));
    for (int j = 0; j < kJointCount; ++j) {
        auto& track = raw.tracks[static_cast<std::size_t>(toOzzIndex[static_cast<std::size_t>(j)])];
        if (j != Pelvis) {
            track.translations.push_back({0.0f, toOzz(kJoints[static_cast<std::size_t>(j)].offset)});
        }
    }
    for (std::uint32_t k = 0; k <= keys; ++k) {
        const float phase = static_cast<float>(k) / static_cast<float>(keys);
        const Pose pose = clipPose(clip, k == keys ? 0.0f : phase);  // last key closes the loop exactly
        const float t = phase * duration;
        for (int j = 0; j < kJointCount; ++j) {
            auto& track = raw.tracks[static_cast<std::size_t>(toOzzIndex[static_cast<std::size_t>(j)])];
            const glm::quat& q = pose.rot[static_cast<std::size_t>(j)];
            track.rotations.push_back({t, ozz::math::Quaternion(q.x, q.y, q.z, q.w)});
        }
        raw.tracks[static_cast<std::size_t>(toOzzIndex[Pelvis])].translations.push_back({t, toOzz(pose.pelvis)});
    }
    if (!raw.Validate()) {
        throw std::runtime_error("procedural character: invalid raw animation");
    }
    auto anim = ozz::animation::offline::AnimationBuilder()(raw);
    if (!anim) {
        throw std::runtime_error("procedural character: ozz rejected an animation");
    }
    return anim;
}

}  // namespace

FootSample sampleFeet(snapshot::ClipId clip, float phase) {
    const GaitParams* g = nullptr;
    switch (clip) {
        case snapshot::ClipId::Walk:
            g = &kWalk;
            break;
        case snapshot::ClipId::Run:
            g = &kRun;
            break;
        case snapshot::ClipId::Turn:
            g = &kTurn;
            break;
        default:
            return {0.02f, 0.07f, 0.02f, 0.07f};
    }
    const FootPose l = footAt(*g, phase);
    const FootPose r = footAt(*g, phase + 0.5f);
    return {l.z, l.y, r.z, r.y};
}

snapshot::CharacterAsset buildProceduralCharacter(const BakeOptions& options) {
    // Skeleton
    ozz::animation::offline::RawSkeleton rawSkeleton;
    rawSkeleton.roots.resize(1);
    fillRawJoint(Root, rawSkeleton.roots[0]);
    if (!rawSkeleton.Validate()) {
        throw std::runtime_error("procedural character: invalid raw skeleton");
    }
    const auto skeleton = ozz::animation::offline::SkeletonBuilder()(rawSkeleton);
    if (!skeleton) {
        throw std::runtime_error("procedural character: ozz rejected the skeleton");
    }
    const int numJoints = skeleton->num_joints();
    std::array<int, kJointCount> toOzzIndex{};
    for (int j = 0; j < kJointCount; ++j) {
        toOzzIndex[static_cast<std::size_t>(j)] = -1;
        for (int k = 0; k < numJoints; ++k) {
            if (std::strcmp(skeleton->joint_names()[static_cast<std::size_t>(k)],
                            kJoints[static_cast<std::size_t>(j)].name) == 0) {
                toOzzIndex[static_cast<std::size_t>(j)] = k;
            }
        }
        if (toOzzIndex[static_cast<std::size_t>(j)] < 0) {
            throw std::runtime_error("procedural character: joint missing after build");
        }
    }

    // Inverse bind matrices from the rest pose.
    std::vector<ozz::math::Float4x4> models(static_cast<std::size_t>(numJoints));
    {
        ozz::animation::LocalToModelJob job;
        job.skeleton = skeleton.get();
        job.input = skeleton->joint_rest_poses();
        job.output = ozz::make_span(models);
        if (!job.Run()) {
            throw std::runtime_error("procedural character: rest pose local-to-model failed");
        }
    }
    std::vector<glm::mat4> inverseBind(static_cast<std::size_t>(numJoints));
    for (int k = 0; k < numJoints; ++k) {
        inverseBind[static_cast<std::size_t>(k)] = glm::inverse(toGlm(models[static_cast<std::size_t>(k)]));
    }

    snapshot::CharacterAsset asset;
    asset.name = "procedural_humanoid";
    asset.height = 1.79f;
    asset.radius = 0.27f;
    asset.jointCount = static_cast<std::uint32_t>(numJoints);
    asset.textureWidth = asset.jointCount * 3;
    asset.jointNames.resize(static_cast<std::size_t>(numJoints));
    asset.jointBindPositions.resize(static_cast<std::size_t>(numJoints) * 3);
    for (int j = 0; j < kJointCount; ++j) {
        const auto k = static_cast<std::size_t>(toOzzIndex[static_cast<std::size_t>(j)]);
        asset.jointNames[k] = kJoints[static_cast<std::size_t>(j)].name;
        const glm::vec3 p = bindModelPosition(j);
        asset.jointBindPositions[k * 3] = p.x;
        asset.jointBindPositions[k * 3 + 1] = p.y;
        asset.jointBindPositions[k * 3 + 2] = p.z;
    }

    struct ClipSpec {
        snapshot::ClipId id;
        const char* name;
        float duration;
        float distance;
        std::uint32_t frames;
    };
    const ClipSpec specs[] = {
        {snapshot::ClipId::Idle, "idle", kIdleDuration, 0.0f, options.idleFrames},
        {snapshot::ClipId::Walk, "walk", kWalkDuration, kWalkDistance, options.walkFrames},
        {snapshot::ClipId::Run, "run", kRunDuration, kRunDistance, options.runFrames},
        {snapshot::ClipId::Turn, "turn", kTurnDuration, 0.0f, options.turnFrames},
    };

    ozz::animation::SamplingJob::Context context(numJoints);
    std::vector<ozz::math::SoaTransform> locals(static_cast<std::size_t>(skeleton->num_soa_joints()));
    std::uint32_t row = 0;
    for (const ClipSpec& spec : specs) {
        const auto anim = buildClip(*skeleton, toOzzIndex, spec.id, spec.duration, options.keysPerCycle);
        asset.clips.push_back({spec.name, row, spec.frames, spec.duration, spec.distance});
        for (std::uint32_t f = 0; f < spec.frames; ++f, ++row) {
            ozz::animation::SamplingJob sampling;
            sampling.animation = anim.get();
            sampling.context = &context;
            sampling.ratio = static_cast<float>(f) / static_cast<float>(spec.frames);
            sampling.output = ozz::make_span(locals);
            ozz::animation::LocalToModelJob ltm;
            ltm.skeleton = skeleton.get();
            ltm.input = ozz::make_span(locals);
            ltm.output = ozz::make_span(models);
            if (!sampling.Run() || !ltm.Run()) {
                throw std::runtime_error("procedural character: sampling failed");
            }
            for (int k = 0; k < numJoints; ++k) {
                const glm::mat4 skin =
                    toGlm(models[static_cast<std::size_t>(k)]) * inverseBind[static_cast<std::size_t>(k)];
                for (int r = 0; r < 3; ++r) {
                    asset.boneTexture.insert(asset.boneTexture.end(), {skin[0][r], skin[1][r], skin[2][r], skin[3][r]});
                }
            }
        }
    }
    asset.textureHeight = row;
    asset.lods.push_back(buildMesh(toOzzIndex, true));
    asset.lods.push_back(buildMesh(toOzzIndex, false));
    return asset;
}

}  // namespace crowdsim::animation
