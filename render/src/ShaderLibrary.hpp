#pragma once

/// @file ShaderLibrary.hpp
/// Access to shaders compiled into the binary at build time.

#include <bgfx/bgfx.h>

namespace crowdsim::render {

enum class EmbeddedProgram {
    ImGui,
};

/// Create a program for the active renderer backend. Returns an invalid handle
/// (and logs nothing) if this build has no shader for the backend.
[[nodiscard]] bgfx::ProgramHandle createEmbeddedProgram(EmbeddedProgram program);

}  // namespace crowdsim::render
