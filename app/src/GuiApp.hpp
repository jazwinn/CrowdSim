#pragma once

/// @file GuiApp.hpp
/// The windowed editor's main loop. Wires platform, renderer, ImGui and the
/// editor shell around the EngineContext.

namespace crowdsim::app {

class EngineContext;

/// Runs until the window closes (or --frames elapses). Returns the process exit code.
int runGui(EngineContext& engine);

}  // namespace crowdsim::app
