# assets/: file formats. glTF import (cgltf), scene JSON (de)serialization.
# Depends on sim types (it reads/writes the simulation's scene description).
crowdsim_add_library(assets
  SOURCES
    src/AssetLibrary.cpp
    src/GltfImport.cpp
    src/SceneSerializer.cpp
    src/JsonTypes.cpp
    src/BehaviorJson.cpp
    src/BtGraph.cpp
  DEPS
    CrowdSim::core
    CrowdSim::geometry
    CrowdSim::sim
    CrowdSim::reflect
    nlohmann_json::nlohmann_json
)
target_include_directories(crowdsim_assets PRIVATE "${CGLTF_INCLUDE_DIR}")
