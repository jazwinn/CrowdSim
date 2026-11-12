# ozz-animation is not in the vcpkg baseline, so it is fetched from its official
# release tarball (pinned by version and SHA-256) and built as static libraries
# with only the runtime and offline (builder) libraries.

include(FetchContent)

set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf OFF CACHE BOOL "" FORCE)
set(ozz_build_data OFF CACHE BOOL "" FORCE)
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_postfix OFF CACHE BOOL "" FORCE)
# Our vcpkg triplets use the DLL C runtime (x64-windows-static-md).
set(ozz_build_msvc_rt_dll ON CACHE BOOL "" FORCE)

FetchContent_Declare(ozz
  URL https://github.com/guillaumeblanc/ozz-animation/archive/refs/tags/0.16.0.tar.gz
  URL_HASH SHA256=a7a34322344e9d839eaf637bbc463404c6aed3f52583dea95c856fea580c2693
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
FetchContent_MakeAvailable(ozz)

# Treat ozz headers as system headers so the project warning policy ignores them.
foreach(t ozz_base ozz_animation ozz_animation_offline)
  get_target_property(dirs ${t} INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(${t} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
endforeach()
