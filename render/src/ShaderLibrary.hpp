#pragma once

/// @file ShaderLibrary.hpp
/// Access to shaders compiled into the binary at build time.

#include <bgfx/bgfx.h>

namespace crowdsim::render {

enum class EmbeddedProgram {
    ImGui,
    Agent,   ///< instanced agent bodies
    Ground,  ///< ground plane with grid
    Flat,    ///< single-colour geometry (area markers, debug)
    Mesh,    ///< static environment meshes with vertex colours
    Line,    ///< unlit coloured debug lines / triangles
};

/// Create a program for the active renderer backend. Returns an invalid handle
/// if this build has no shader for the backend (e.g. the noop renderer).
[[nodiscard]] bgfx::ProgramHandle createEmbeddedProgram(EmbeddedProgram program);

}  // namespace crowdsim::render
