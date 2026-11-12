# geometry/: triangle meshes and primitive builders (box, ramp, stairs, plane).
# Pure data + math; consumed by sim (navmesh), physics (collision) and app/render.
crowdsim_add_library(geometry
  SOURCES
    src/TriangleMesh.cpp
    src/Primitives.cpp
  DEPS
    CrowdSim::core
    glm::glm
)
