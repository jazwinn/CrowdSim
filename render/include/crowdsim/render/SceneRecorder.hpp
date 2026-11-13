#pragma once

/// @file SceneRecorder.hpp
/// Records the 3D view to a video file without any editor UI. While recording,
/// the scene renders into an off-screen target (SceneRenderer::offscreen) instead
/// of the window; the app shows that target in the viewport under the panels, and
/// each frame is copied back from the GPU (a small ring of read-back textures, so
/// nothing stalls) and handed to a core::VideoWriter thread.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace crowdsim::render {

/// Where the scene view renders instead of the window (bgfx handle index + size).
struct OffscreenTarget {
    std::uint16_t frameBuffer = 0xFFFF;  ///< bgfx::FrameBufferHandle::idx; 0xFFFF = none
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

struct RecordingOptions {
    std::filesystem::path file;  ///< .avi
    std::uint16_t width = 0;     ///< video size (the viewport size when recording starts)
    std::uint16_t height = 0;
    double fps = 30.0;
    int jpegQuality = 85;
    double maxSeconds = 0.0;  ///< stop automatically after this long; 0 = until stopped
};

struct RecordingStatus {
    bool active = false;
    std::filesystem::path file;
    double seconds = 0.0;
    std::uint64_t framesWritten = 0;
    std::uint64_t framesDropped = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

class SceneRecorder {
public:
    explicit SceneRecorder(spdlog::logger& log);
    ~SceneRecorder();
    SceneRecorder(const SceneRecorder&) = delete;
    SceneRecorder& operator=(const SceneRecorder&) = delete;

    /// Whether this GPU backend can copy textures back (blit + read-back).
    [[nodiscard]] bool supported() const;

    /// Start recording. Returns an empty string, or why it could not start.
    [[nodiscard]] std::string start(const RecordingOptions& options, double nowSeconds);
    /// Stop and finish the file (pending frames are written). Returns the final status.
    RecordingStatus stop();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] RecordingStatus status(double nowSeconds) const;

    /// Target for SceneRenderer::offscreen this frame (none when not recording).
    [[nodiscard]] OffscreenTarget target() const noexcept;
    /// ImTextureID value of the off-screen colour (as ImGuiRenderer maps bgfx textures), 0 = none.
    [[nodiscard]] std::uint64_t displayTexture() const noexcept;
    /// True when the target's rows are stored bottom-up (OpenGL): flip when displaying.
    [[nodiscard]] bool bottomLeftOrigin() const noexcept;

    /// After the scene is submitted: queue a GPU copy of this frame if one is due.
    void capture(double nowSeconds);
    /// After bgfx::frame() returned `frameNumber`: hand finished copies to the writer,
    /// and stop if maxSeconds has passed.
    void afterFrame(std::uint32_t frameNumber, double nowSeconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace crowdsim::render
