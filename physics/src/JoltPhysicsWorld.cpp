#include "crowdsim/physics/PhysicsWorld.hpp"

// Jolt must be included first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <spdlog/logger.h>

#include <vector>

namespace crowdsim::physics {

namespace {

namespace Layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::uint kCount = 2;
}  // namespace Layers

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override { return Layers::kCount; }
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return static_cast<JPH::BroadPhaseLayer::Type>(layer) == 0 ? "Static" : "Moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == Layers::kMoving || b == Layers::kMoving;
    }
};

class JoltPhysicsWorld final : public IPhysicsWorld {
public:
    JoltPhysicsWorld(spdlog::logger& log, std::uint32_t maxBodies) : log_(log) {
        system_.Init(maxBodies, 0, 1024, 1024, broadPhase_, objectVsBroadPhase_, objectPairs_);
    }

    ~JoltPhysicsWorld() override { clear(); }

    void clear() override {
        JPH::BodyInterface& bi = system_.GetBodyInterface();
        for (const JPH::BodyID& id : bodies_) {
            bi.RemoveBody(id);
            bi.DestroyBody(id);
        }
        bodies_.clear();
    }

    BodyId addStaticMesh(const geometry::TriangleMesh& mesh) override {
        if (mesh.empty()) {
            return {};
        }
        JPH::VertexList verts;
        verts.reserve(mesh.positions.size());
        for (const glm::vec3& p : mesh.positions) {
            verts.push_back(JPH::Float3(p.x, p.y, p.z));
        }
        JPH::IndexedTriangleList tris;
        tris.reserve(mesh.triangleCount());
        for (std::uint32_t t = 0; t < mesh.triangleCount(); ++t) {
            tris.push_back(
                JPH::IndexedTriangle(mesh.indices[3 * t], mesh.indices[3 * t + 1], mesh.indices[3 * t + 2], 0));
        }
        JPH::MeshShapeSettings settings(std::move(verts), std::move(tris));
        JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) {
            log_.error("Physics: mesh shape creation failed: {}", result.GetError().c_str());
            return {};
        }
        const JPH::BodyCreationSettings bcs(result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                            JPH::EMotionType::Static, Layers::kStatic);
        JPH::BodyInterface& bi = system_.GetBodyInterface();
        const JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
        if (id.IsInvalid()) {
            log_.error("Physics: out of bodies");
            return {};
        }
        bodies_.push_back(id);
        system_.OptimizeBroadPhase();
        return BodyId{id.GetIndexAndSequenceNumber()};
    }

    std::optional<RayHit> raycast(glm::vec3 origin, glm::vec3 direction, float maxDistance) const override {
        const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        if (len <= 0.0f || maxDistance <= 0.0f) {
            return std::nullopt;
        }
        const glm::vec3 d = direction / len * maxDistance;
        const JPH::RRayCast ray{JPH::RVec3(origin.x, origin.y, origin.z), JPH::Vec3(d.x, d.y, d.z)};
        JPH::RayCastResult hit;
        if (!system_.GetNarrowPhaseQuery().CastRay(ray, hit)) {
            return std::nullopt;
        }
        const JPH::RVec3 p = ray.GetPointOnRay(hit.mFraction);
        RayHit out;
        out.position = {static_cast<float>(p.GetX()), static_cast<float>(p.GetY()), static_cast<float>(p.GetZ())};
        out.distance = hit.mFraction * maxDistance;
        out.body = BodyId{hit.mBodyID.GetIndexAndSequenceNumber()};
        const JPH::BodyLockRead lock(system_.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, p);
            out.normal = {n.GetX(), n.GetY(), n.GetZ()};
        }
        return out;
    }

    std::uint32_t bodyCount() const override { return static_cast<std::uint32_t>(bodies_.size()); }

private:
    spdlog::logger& log_;
    // Layer interfaces must outlive the system.
    BroadPhaseLayers broadPhase_;
    ObjectVsBroadPhase objectVsBroadPhase_;
    ObjectPairs objectPairs_;
    JPH::PhysicsSystem system_;
    std::vector<JPH::BodyID> bodies_;
};

}  // namespace

JoltRuntime::JoltRuntime() {
    JPH::RegisterDefaultAllocator();
    // Jolt's factory is a library-level singleton by design; this object owns it.
    JPH::Factory::sInstance = new JPH::Factory();  // global-state-ok: Jolt API, owned by JoltRuntime
    JPH::RegisterTypes();
}

JoltRuntime::~JoltRuntime() {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;  // global-state-ok: Jolt API, owned by JoltRuntime
    JPH::Factory::sInstance = nullptr;
}

std::unique_ptr<IPhysicsWorld> createJoltPhysicsWorld(const JoltRuntime& /*runtime*/, spdlog::logger& log,
                                                      std::uint32_t maxBodies) {
    return std::make_unique<JoltPhysicsWorld>(log, maxBodies);
}

}  // namespace crowdsim::physics
