# snapshot/: plain data handed from sim/ to render/. Header-only; depends on
# glm only, so neither side has to know about the other.
add_library(crowdsim_snapshot INTERFACE)
add_library(CrowdSim::snapshot ALIAS crowdsim_snapshot)
target_include_directories(crowdsim_snapshot INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(crowdsim_snapshot INTERFACE glm::glm)
target_compile_features(crowdsim_snapshot INTERFACE cxx_std_20)
