#include "crowdsim/render/ImGuiRenderer.hpp"

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "ShaderLibrary.hpp"
#include "crowdsim/render/Renderer.hpp"

namespace crowdsim::render {

struct ImGuiRenderer::Resources {
    bgfx::VertexLayout layout;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle texSampler = BGFX_INVALID_HANDLE;
};

namespace {

// ImGui reserves ImTextureID 0 as "invalid", but 0 is a valid bgfx handle index,
// so ids are stored as handle index + 1.
bgfx::TextureHandle toHandle(ImTextureID id) {
    return bgfx::TextureHandle{static_cast<std::uint16_t>(id - 1)};
}

ImTextureID toTexId(bgfx::TextureHandle handle) {
    return static_cast<ImTextureID>(handle.idx) + 1;
}

}  // namespace

ImGuiRenderer::ImGuiRenderer(Renderer& /*renderer*/, std::uint16_t viewId)
    : res_(std::make_unique<Resources>()), viewId_(viewId) {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "crowdsim_bgfx";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    res_->layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    res_->texSampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    if (bgfx::getRendererType() != bgfx::RendererType::Noop) {
        res_->program = createEmbeddedProgram(EmbeddedProgram::ImGui);
        if (!bgfx::isValid(res_->program)) {
            throw std::runtime_error("no ImGui shader for this renderer backend");
        }
    }
    bgfx::setViewName(viewId_, "ImGui");
    bgfx::setViewMode(viewId_, bgfx::ViewMode::Sequential);
}

ImGuiRenderer::~ImGuiRenderer() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->RefCount == 1 && tex->TexID != ImTextureID_Invalid) {
            bgfx::destroy(toHandle(tex->TexID));
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    if (bgfx::isValid(res_->program)) {
        bgfx::destroy(res_->program);
    }
    bgfx::destroy(res_->texSampler);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
}

void ImGuiRenderer::updateTexture(ImTextureData* tex) {
    IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
    if (tex->Status == ImTextureStatus_WantCreate) {
        const auto w = static_cast<std::uint16_t>(tex->Width);
        const auto h = static_cast<std::uint16_t>(tex->Height);
        // Passing initial data to createTexture2D makes a bgfx texture immutable, and
        // later updateTexture2D calls are silently dropped in release builds. ImGui's
        // atlas grows glyph by glyph, so create it empty (mutable) and upload separately.
        const bgfx::TextureHandle handle = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::RGBA8,
                                                                 BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, nullptr);
        bgfx::updateTexture2D(handle, 0, 0, 0, 0, w, h,
                              bgfx::copy(tex->GetPixels(), static_cast<std::uint32_t>(tex->GetSizeInBytes())));
        tex->SetTexID(toTexId(handle));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        const bgfx::TextureHandle handle = toHandle(tex->TexID);
        for (const ImTextureRect& r : tex->Updates) {
            const std::uint32_t rowBytes =
                static_cast<std::uint32_t>(r.w) * static_cast<std::uint32_t>(tex->BytesPerPixel);
            const bgfx::Memory* mem = bgfx::alloc(rowBytes * r.h);
            for (int y = 0; y < r.h; ++y) {
                std::memcpy(mem->data + static_cast<std::size_t>(y) * rowBytes, tex->GetPixelsAt(r.x, r.y + y),
                            rowBytes);
            }
            bgfx::updateTexture2D(handle, 0, 0, r.x, r.y, r.w, r.h, mem);
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        bgfx::destroy(toHandle(tex->TexID));
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void ImGuiRenderer::render(ImDrawData* drawData) {
    if (drawData == nullptr) {
        return;
    }
    if (drawData->Textures != nullptr) {
        for (ImTextureData* tex : *drawData->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                updateTexture(tex);
            }
        }
    }

    const float fbW = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    const float fbH = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    if (fbW <= 0.0f || fbH <= 0.0f || !bgfx::isValid(res_->program)) {
        return;
    }

    const bgfx::Caps* caps = bgfx::getCaps();
    {
        float ortho[16];
        const float x = drawData->DisplayPos.x;
        const float y = drawData->DisplayPos.y;
        bx::mtxOrtho(ortho, x, x + drawData->DisplaySize.x, y + drawData->DisplaySize.y, y, 0.0f, 1000.0f, 0.0f,
                     caps->homogeneousDepth);
        bgfx::setViewTransform(viewId_, nullptr, ortho);
        bgfx::setViewRect(viewId_, 0, 0, static_cast<std::uint16_t>(fbW), static_cast<std::uint16_t>(fbH));
    }

    const ImVec2 clipOff = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;
    constexpr std::uint64_t kState =
        BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA |
        BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE,
                                       BGFX_STATE_BLEND_INV_SRC_ALPHA);

    for (const ImDrawList* list : drawData->CmdLists) {
        const auto numVertices = static_cast<std::uint32_t>(list->VtxBuffer.size());
        const auto numIndices = static_cast<std::uint32_t>(list->IdxBuffer.size());
        if (bgfx::getAvailTransientVertexBuffer(numVertices, res_->layout) != numVertices ||
            bgfx::getAvailTransientIndexBuffer(numIndices, sizeof(ImDrawIdx) == 4) != numIndices) {
            break;  // out of transient memory this frame; skip the rest
        }
        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, numVertices, res_->layout);
        bgfx::allocTransientIndexBuffer(&tib, numIndices, sizeof(ImDrawIdx) == 4);
        std::memcpy(tvb.data, list->VtxBuffer.Data, numVertices * sizeof(ImDrawVert));
        std::memcpy(tib.data, list->IdxBuffer.Data, numIndices * sizeof(ImDrawIdx));

        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback != nullptr) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) {
                    cmd.UserCallback(list, &cmd);
                }
                continue;
            }
            if (cmd.ElemCount == 0) {
                continue;
            }
            const float x0 = std::max((cmd.ClipRect.x - clipOff.x) * clipScale.x, 0.0f);
            const float y0 = std::max((cmd.ClipRect.y - clipOff.y) * clipScale.y, 0.0f);
            const float x1 = std::min((cmd.ClipRect.z - clipOff.x) * clipScale.x, fbW);
            const float y1 = std::min((cmd.ClipRect.w - clipOff.y) * clipScale.y, fbH);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            bgfx::setScissor(static_cast<std::uint16_t>(x0), static_cast<std::uint16_t>(y0),
                             static_cast<std::uint16_t>(x1 - x0), static_cast<std::uint16_t>(y1 - y0));
            bgfx::setState(kState);
            bgfx::setTexture(0, res_->texSampler, toHandle(cmd.GetTexID()));
            bgfx::setVertexBuffer(0, &tvb, cmd.VtxOffset, numVertices - cmd.VtxOffset);
            bgfx::setIndexBuffer(&tib, cmd.IdxOffset, cmd.ElemCount);
            bgfx::submit(viewId_, res_->program);
        }
    }
}

}  // namespace crowdsim::render
