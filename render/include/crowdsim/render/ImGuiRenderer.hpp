#pragma once

/// @file ImGuiRenderer.hpp
/// Dear ImGui renderer backend on bgfx, supporting ImGui's dynamic texture
/// protocol (ImGuiBackendFlags_RendererHasTextures).

#include <cstdint>
#include <memory>

struct ImDrawData;
struct ImTextureData;

namespace crowdsim::render {

class Renderer;

class ImGuiRenderer {
public:
    /// Requires a current ImGui context and an initialised Renderer.
    ImGuiRenderer(Renderer& renderer, std::uint16_t viewId);
    ~ImGuiRenderer();

    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    /// Submit ImGui::GetDrawData() to the ImGui view.
    void render(ImDrawData* drawData);

private:
    void updateTexture(ImTextureData* tex);

    struct Resources;
    std::unique_ptr<Resources> res_;
    std::uint16_t viewId_;
};

}  // namespace crowdsim::render
