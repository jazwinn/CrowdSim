# LuaJIT from vcpkg ships headers in include/luajit and a static library
# (lua51 on Windows, luajit-5.1 elsewhere) without a CMake package; wrap it as
# the imported target luajit::luajit with per-configuration libraries.

include(SelectLibraryConfigurations)

set(_luajit_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_path(LUAJIT_INCLUDE_DIR luajit.h PATHS "${_luajit_root}/include" PATH_SUFFIXES luajit luajit-2.1 REQUIRED)
find_library(LUAJIT_LIBRARY_RELEASE NAMES lua51 luajit-5.1 luajit PATHS "${_luajit_root}/lib" NO_DEFAULT_PATH)
find_library(LUAJIT_LIBRARY_DEBUG NAMES lua51 luajit-5.1 luajit PATHS "${_luajit_root}/debug/lib" NO_DEFAULT_PATH)
select_library_configurations(LUAJIT)
if(NOT LUAJIT_LIBRARY)
  message(FATAL_ERROR "LuaJIT library not found under ${_luajit_root}")
endif()

add_library(luajit::luajit INTERFACE IMPORTED)
set_target_properties(luajit::luajit PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${LUAJIT_INCLUDE_DIR}")
if(LUAJIT_LIBRARY_DEBUG AND LUAJIT_LIBRARY_RELEASE)
  target_link_libraries(luajit::luajit INTERFACE
    "$<$<CONFIG:Debug>:${LUAJIT_LIBRARY_DEBUG}>" "$<$<NOT:$<CONFIG:Debug>>:${LUAJIT_LIBRARY_RELEASE}>")
else()
  target_link_libraries(luajit::luajit INTERFACE "${LUAJIT_LIBRARY}")
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(luajit::luajit INTERFACE m dl)
endif()
