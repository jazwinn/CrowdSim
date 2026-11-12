# Compile bgfx .sc shaders into C headers at build time with shaderc, for every
# backend the host toolchain can target. Each output header defines
#   static const uint8_t <name>_<profile>[] = {...};
# and the target gets CROWDSIM_SHADER_<PROFILE>=1 definitions for the profiles built.

find_program(BGFX_SHADERC shaderc
  HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/bgfx"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/bgfx"
  REQUIRED)

find_path(BGFX_SHADER_INCLUDE_DIR bgfx_shader.sh
  HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/bgfx"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
  REQUIRED)

# profile-id:shaderc-platform:shaderc-profile
set(CROWDSIM_SHADER_PROFILES
  "spirv:linux:spirv"
  "glsl:linux:150"
  "metal:osx:metal")
if(CMAKE_HOST_WIN32)
  # HLSL bytecode needs the Windows-only D3D compiler.
  list(APPEND CROWDSIM_SHADER_PROFILES "dx11:windows:s_5_0")
endif()

# crowdsim_compile_shaders(<target> VARYING <file> VERTEX <files...> FRAGMENT <files...>)
function(crowdsim_compile_shaders target)
  cmake_parse_arguments(ARG "" "VARYING" "VERTEX;FRAGMENT" ${ARGN})
  set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
  file(MAKE_DIRECTORY "${out_dir}")
  set(outputs "")
  file(GLOB common_includes "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common/*.sh")

  foreach(type VERTEX FRAGMENT)
    if(type STREQUAL "VERTEX")
      set(shaderc_type vertex)
    else()
      set(shaderc_type fragment)
    endif()
    foreach(src IN LISTS ARG_${type})
      get_filename_component(name "${src}" NAME_WE)
      foreach(entry IN LISTS CROWDSIM_SHADER_PROFILES)
        string(REPLACE ":" ";" entry "${entry}")
        list(GET entry 0 id)
        list(GET entry 1 platform)
        list(GET entry 2 profile)
        set(out "${out_dir}/${name}_${id}.bin.h")
        add_custom_command(
          OUTPUT "${out}"
          COMMAND "${BGFX_SHADERC}" -f "${src}" -o "${out}" --type ${shaderc_type}
                  --platform ${platform} -p ${profile} -i "${BGFX_SHADER_INCLUDE_DIR}"
                  -i "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common"
                  --varyingdef "${ARG_VARYING}" --bin2c ${name}_${id} -O 3
          DEPENDS "${src}" "${ARG_VARYING}" ${common_includes}
          COMMENT "shaderc ${name} (${id})"
          VERBATIM)
        list(APPEND outputs "${out}")
      endforeach()
    endforeach()
  endforeach()

  target_sources(${target} PRIVATE ${outputs})
  target_include_directories(${target} PRIVATE "${out_dir}")
  foreach(entry IN LISTS CROWDSIM_SHADER_PROFILES)
    string(REPLACE ":" ";" entry "${entry}")
    list(GET entry 0 id)
    string(TOUPPER "${id}" id_upper)
    target_compile_definitions(${target} PRIVATE CROWDSIM_SHADER_${id_upper}=1)
  endforeach()
endfunction()
