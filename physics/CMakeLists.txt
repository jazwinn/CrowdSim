# physics/: Jolt Physics behind IPhysicsWorld. Static environment collision and
# raycasts (picking, line of sight); dynamic obstacles later. Agents are NOT Jolt
# bodies. Depends on core + geometry only.
crowdsim_add_library(physics
  SOURCES
    src/JoltPhysicsWorld.cpp
  DEPS
    CrowdSim::core
    CrowdSim::geometry
  PRIVATE_DEPS
    Jolt::Jolt
)
