# animation/: procedural crowd character (skeleton, skinned LOD meshes, gait
# clips), ozz-based baking into a vertex animation texture, and the per-agent
# gait controller that picks and blends clips from speed. No rendering API.
crowdsim_add_library(animation
  SOURCES
    src/Gait.cpp
    src/ProceduralCharacter.cpp
  DEPS
    CrowdSim::core
    CrowdSim::snapshot
    glm::glm
  PRIVATE_DEPS
    ozz_animation
    ozz_animation_offline
    ozz_base
)
