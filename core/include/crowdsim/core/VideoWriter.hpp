#pragma once

/// @file VideoWriter.hpp
/// Motion-JPEG AVI writer: frames in, a self-contained .avi out (VLC, browsers'
/// players, Windows Media Player and video editors read it; no codec install).
/// Encoding runs on the writer's own thread so a caller on the render thread only
/// copies pixels; frames that arrive while too many are queued are dropped and
/// counted rather than stalling the caller.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace crowdsim::core {

struct VideoWriterOptions {
    std::filesystem::path file;
    std::uint32_t width = 0;   ///< every frame must have this size
    std::uint32_t height = 0;
    double fps = 30.0;
    int jpegQuality = 85;              ///< 1..100
    std::uint32_t maxQueuedFrames = 8;  ///< beyond this, new frames are dropped
};

struct VideoWriterStats {
    std::uint64_t framesWritten = 0;
    std::uint64_t framesDropped = 0;
    std::uint64_t bytesWritten = 0;
};

class VideoWriter {
public:
    /// Opens the file (creating its folder). Throws std::runtime_error on failure.
    explicit VideoWriter(const VideoWriterOptions& options);
    /// Finishes the file (see close()).
    ~VideoWriter();
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;

    /// Queue one frame of tightly packed RGBA8 pixels, `repeat` times (to keep real
    /// time when frames are captured less often than `fps`). `topDown` = first row is
    /// the top of the image. Returns false if the frame was dropped.
    bool addFrame(std::span<const std::uint8_t> rgba, bool topDown = true, std::uint32_t repeat = 1);

    /// Encode what is queued, write the index and fix up the headers. Idempotent.
    void close();

    [[nodiscard]] VideoWriterStats stats() const;
    [[nodiscard]] const VideoWriterOptions& options() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace crowdsim::core
