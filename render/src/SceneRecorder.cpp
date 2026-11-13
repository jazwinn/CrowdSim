#include "crowdsim/render/SceneRecorder.hpp"

#include <bgfx/bgfx.h>
#include <spdlog/logger.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include "crowdsim/core/VideoWriter.hpp"
#include "crowdsim/render/Renderer.hpp"

namespace crowdsim::render {

namespace {

/// GPU copies in flight. Read-back lands about two frames after it is requested;
/// four slots keep capturing every frame without waiting.
constexpr std::size_t kSlots = 4;

struct Slot {
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    std::vector<std::uint8_t> pixels;
    std::uint32_t readyFrame = 0;  ///< bgfx frame number when the copy is in `pixels`; 0 = free
    std::uint32_t repeat = 1;      ///< video frames this capture stands for
};

}  // namespace

struct SceneRecorder::Impl {
    spdlog::logger& log;
    RecordingOptions opt;
    std::unique_ptr<core::VideoWriter> writer;
    bgfx::TextureHandle color = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depth = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle frameBuffer = BGFX_INVALID_HANDLE;
    std::array<Slot, kSlots> slots;
    double startTime = 0.0;
    std::uint64_t framesDue = 0;   ///< video frames that real time says should exist by now
    std::uint64_t framesTaken = 0; ///< video frames captured so far (including repeats)
    std::uint32_t lastFrame = 0;   ///< latest bgfx frame number
    bool releasePending = false;   ///< stopped; free GPU resources once copies have landed

    [[nodiscard]] bool copiesInFlight() const {
        return std::any_of(slots.begin(), slots.end(), [](const Slot& s) { return s.readyFrame != 0; });
    }

    explicit Impl(spdlog::logger& l) : log(l) {}

    void destroyGpu() {
        for (Slot& s : slots) {
            if (bgfx::isValid(s.texture)) {
                bgfx::destroy(s.texture);
            }
            s = Slot{};
        }
        if (bgfx::isValid(frameBuffer)) {
            bgfx::destroy(frameBuffer);  // owns color and depth
        }
        frameBuffer = BGFX_INVALID_HANDLE;
        color = BGFX_INVALID_HANDLE;
        depth = BGFX_INVALID_HANDLE;
    }
};

SceneRecorder::SceneRecorder(spdlog::logger& log) : impl_(std::make_unique<Impl>(log)) {}

SceneRecorder::~SceneRecorder() {
    if (active()) {
        stop();
    }
    // bgfx writes read-backs into our buffers a frame or two later: let pending ones
    // land before the buffers go away.
    for (int i = 0; i < 4 && impl_->copiesInFlight(); ++i) {
        impl_->lastFrame = bgfx::frame();
        for (Slot& s : impl_->slots) {
            if (s.readyFrame != 0 && s.readyFrame <= impl_->lastFrame) {
                s.readyFrame = 0;
            }
        }
    }
    impl_->destroyGpu();
}

bool SceneRecorder::supported() const {
    const bgfx::Caps* caps = bgfx::getCaps();
    constexpr std::uint64_t kNeeded = BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK;
    return caps != nullptr && (caps->supported & kNeeded) == kNeeded;
}

std::string SceneRecorder::start(const RecordingOptions& options, double nowSeconds) {
    Impl& m = *impl_;
    if (active()) {
        return "already recording to " + m.opt.file.string();
    }
    if (m.releasePending) {
        return "the previous recording is still finishing; try again in a moment";
    }
    if (!supported()) {
        return std::string("this graphics backend cannot read frames back (") +
               bgfx::getRendererName(bgfx::getRendererType()) + "); try --renderer d3d11 or vulkan";
    }
    if (options.width < 16 || options.height < 16) {
        return "the 3D view is too small to record";
    }
    m.opt = options;
    m.opt.fps = std::clamp(options.fps, 1.0, 120.0);
    try {
        m.writer = std::make_unique<core::VideoWriter>(core::VideoWriterOptions{
            .file = m.opt.file,
            .width = m.opt.width,
            .height = m.opt.height,
            .fps = m.opt.fps,
            .jpegQuality = m.opt.jpegQuality,
        });
    } catch (const std::exception& e) {
        return e.what();
    }

    constexpr std::uint64_t kClamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    m.color = bgfx::createTexture2D(m.opt.width, m.opt.height, false, 1, bgfx::TextureFormat::RGBA8,
                                    BGFX_TEXTURE_RT | kClamp);
    m.depth = bgfx::createTexture2D(m.opt.width, m.opt.height, false, 1, bgfx::TextureFormat::D24S8,
                                    BGFX_TEXTURE_RT_WRITE_ONLY);
    const bgfx::TextureHandle attachments[] = {m.color, m.depth};
    m.frameBuffer = bgfx::createFrameBuffer(2, attachments, true);
    for (Slot& s : m.slots) {
        s.texture = bgfx::createTexture2D(m.opt.width, m.opt.height, false, 1, bgfx::TextureFormat::RGBA8,
                                          BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | kClamp);
        s.pixels.resize(static_cast<std::size_t>(m.opt.width) * m.opt.height * 4);
    }
    m.startTime = nowSeconds;
    m.framesDue = 0;
    m.framesTaken = 0;
    m.log.info("Recording {}x{} at {:.0f} fps to {}", m.opt.width, m.opt.height, m.opt.fps, m.opt.file.string());
    return {};
}

RecordingStatus SceneRecorder::stop() {
    Impl& m = *impl_;
    if (!m.writer) {
        return {};
    }
    // Copies still in flight are lost; at most a few frames at the end.
    m.writer->close();
    const core::VideoWriterStats s = m.writer->stats();
    RecordingStatus out{false, m.opt.file, static_cast<double>(s.framesWritten) / m.opt.fps, s.framesWritten,
                        s.framesDropped, m.opt.width, m.opt.height};
    m.log.info("Recording saved: {} ({} frames, {:.1f} s, {} dropped)", m.opt.file.string(), s.framesWritten,
               out.seconds, s.framesDropped);
    m.writer.reset();
    // Free the GPU side once copies still in flight have landed (afterFrame).
    m.releasePending = true;
    for (Slot& slot : m.slots) {
        if (slot.readyFrame != 0 && slot.readyFrame <= m.lastFrame) {
            slot.readyFrame = 0;
        }
    }
    if (!m.copiesInFlight()) {
        m.destroyGpu();
        m.releasePending = false;
    }
    return out;
}

bool SceneRecorder::active() const noexcept {
    return impl_->writer != nullptr;
}

RecordingStatus SceneRecorder::status(double nowSeconds) const {
    const Impl& m = *impl_;
    if (!m.writer) {
        return {};
    }
    const core::VideoWriterStats s = m.writer->stats();
    return {true, m.opt.file, nowSeconds - m.startTime, s.framesWritten, s.framesDropped, m.opt.width, m.opt.height};
}

OffscreenTarget SceneRecorder::target() const noexcept {
    const Impl& m = *impl_;
    if (!m.writer || !bgfx::isValid(m.frameBuffer)) {
        return {};
    }
    return {m.frameBuffer.idx, m.opt.width, m.opt.height};
}

std::uint64_t SceneRecorder::displayTexture() const noexcept {
    const Impl& m = *impl_;
    return m.writer && bgfx::isValid(m.color) ? static_cast<std::uint64_t>(m.color.idx) + 1 : 0;
}

bool SceneRecorder::bottomLeftOrigin() const noexcept {
    const bgfx::Caps* caps = bgfx::getCaps();
    return caps != nullptr && caps->originBottomLeft;
}

void SceneRecorder::capture(double nowSeconds) {
    Impl& m = *impl_;
    if (!m.writer) {
        return;
    }
    // Real time decides how many video frames should exist; a slow frame is repeated
    // so the video keeps the pace the user saw.
    m.framesDue = static_cast<std::uint64_t>(std::floor((nowSeconds - m.startTime) * m.opt.fps)) + 1;
    if (m.framesDue <= m.framesTaken) {
        return;
    }
    Slot* free = nullptr;
    for (Slot& s : m.slots) {
        if (s.readyFrame == 0) {
            free = &s;
            break;
        }
    }
    if (free == nullptr) {
        return;  // all copies in flight; try again next frame
    }
    // Frames the GPU copies fell behind on repeat this one, so the video keeps real time.
    free->repeat = static_cast<std::uint32_t>(std::min<std::uint64_t>(m.framesDue - m.framesTaken, 60));
    m.framesTaken = m.framesDue;
    bgfx::blit(kViewCapture, free->texture, 0, 0, m.color, 0, 0, m.opt.width, m.opt.height);
    free->readyFrame = bgfx::readTexture(free->texture, free->pixels.data());
}

void SceneRecorder::afterFrame(std::uint32_t frameNumber, double nowSeconds) {
    Impl& m = *impl_;
    m.lastFrame = frameNumber;
    if (!m.writer) {
        if (m.releasePending) {
            for (Slot& s : m.slots) {
                if (s.readyFrame != 0 && s.readyFrame <= frameNumber) {
                    s.readyFrame = 0;  // landed after the recording ended: discard
                }
            }
            if (!m.copiesInFlight()) {
                m.destroyGpu();
                m.releasePending = false;
            }
        }
        return;
    }
    // Hand over finished copies oldest first, so frames stay in order.
    for (;;) {
        Slot* next = nullptr;
        for (Slot& s : m.slots) {
            if (s.readyFrame != 0 && s.readyFrame <= frameNumber && (next == nullptr || s.readyFrame < next->readyFrame)) {
                next = &s;
            }
        }
        if (next == nullptr) {
            break;
        }
        m.writer->addFrame(next->pixels, !bottomLeftOrigin(), next->repeat);
        next->readyFrame = 0;
    }
    if (m.opt.maxSeconds > 0.0 && nowSeconds - m.startTime >= m.opt.maxSeconds) {
        stop();
    }
}

}  // namespace crowdsim::render
