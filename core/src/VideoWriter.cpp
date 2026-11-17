#include "crowdsim/core/VideoWriter.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wuseless-cast"  // GCC only; clang rejects unknown groups under -Werror
#endif
#endif
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace crowdsim::core {

namespace {

// --- AVI (RIFF) layout --------------------------------------------------------------------
// RIFF 'AVI '
//   LIST 'hdrl' { 'avih' MainAVIHeader, LIST 'strl' { 'strh' AVIStreamHeader, 'strf' BITMAPINFOHEADER } }
//   LIST 'movi' { '00dc' <jpeg> ... }
//   'idx1' { per frame: '00dc', AVIIF_KEYFRAME, offset from 'movi', size }
// Counts and sizes are written as placeholders and patched in close().

constexpr std::uint32_t kAvifHasIndex = 0x10;
constexpr std::uint32_t kAviifKeyframe = 0x10;

void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}
void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void putFourcc(std::vector<std::uint8_t>& b, const char* f) {
    b.insert(b.end(), f, f + 4);
}

struct IndexEntry {
    std::uint32_t offset;
    std::uint32_t size;
};

struct Job {
    std::vector<std::uint8_t> rgba;
    bool topDown = true;
    std::uint32_t repeat = 1;
};

}  // namespace

struct VideoWriter::Impl {
    VideoWriterOptions opt;
    std::ofstream out;
    // Header field offsets to patch on close.
    std::uint64_t riffSizeAt = 0, avihFramesAt = 0, avihBufferAt = 0, strhLengthAt = 0, strhBufferAt = 0;
    std::uint64_t moviSizeAt = 0, moviStart = 0;
    std::vector<IndexEntry> index;
    std::uint32_t largestFrame = 0;
    bool closed = false;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> queue;
    bool stopping = false;
    VideoWriterStats stats;
    std::thread worker;

    explicit Impl(const VideoWriterOptions& o) : opt(o) {
        if (opt.width == 0 || opt.height == 0 || opt.fps <= 0.0) {
            throw std::runtime_error("video size and frame rate must be positive");
        }
        std::error_code ec;
        if (opt.file.has_parent_path()) {
            std::filesystem::create_directories(opt.file.parent_path(), ec);
        }
        out.open(opt.file, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot write video file '" + opt.file.string() + "'");
        }
        writeHeaders();
        worker = std::thread([this] { run(); });
    }

    void writeHeaders() {
        std::vector<std::uint8_t> h;
        const auto rateScale = 1000u;
        const auto rate = static_cast<std::uint32_t>(opt.fps * rateScale + 0.5);
        putFourcc(h, "RIFF");
        riffSizeAt = h.size();
        put32(h, 0);
        putFourcc(h, "AVI ");

        putFourcc(h, "LIST");
        put32(h, 4 + 8 + 56 + 8 + 4 + 8 + 56 + 8 + 40);  // hdrl size
        putFourcc(h, "hdrl");
        putFourcc(h, "avih");
        put32(h, 56);
        put32(h, static_cast<std::uint32_t>(1'000'000.0 / opt.fps + 0.5));  // microseconds per frame
        put32(h, 0);                                                        // max bytes per second
        put32(h, 0);                                                        // padding granularity
        put32(h, kAvifHasIndex);
        avihFramesAt = h.size();
        put32(h, 0);  // total frames
        put32(h, 0);  // initial frames
        put32(h, 1);  // streams
        avihBufferAt = h.size();
        put32(h, 0);  // suggested buffer size
        put32(h, opt.width);
        put32(h, opt.height);
        for (int i = 0; i < 4; ++i) {
            put32(h, 0);
        }

        putFourcc(h, "LIST");
        put32(h, 4 + 8 + 56 + 8 + 40);  // strl size
        putFourcc(h, "strl");
        putFourcc(h, "strh");
        put32(h, 56);
        putFourcc(h, "vids");
        putFourcc(h, "MJPG");
        put32(h, 0);  // flags
        put16(h, 0);  // priority
        put16(h, 0);  // language
        put32(h, 0);  // initial frames
        put32(h, rateScale);
        put32(h, rate);
        put32(h, 0);  // start
        strhLengthAt = h.size();
        put32(h, 0);  // length (frames)
        strhBufferAt = h.size();
        put32(h, 0);           // suggested buffer size
        put32(h, 0xFFFFFFFF);  // quality: default
        put32(h, 0);           // sample size
        put16(h, 0);
        put16(h, 0);
        put16(h, static_cast<std::uint16_t>(opt.width));
        put16(h, static_cast<std::uint16_t>(opt.height));

        putFourcc(h, "strf");
        put32(h, 40);
        put32(h, 40);  // BITMAPINFOHEADER size
        put32(h, opt.width);
        put32(h, opt.height);
        put16(h, 1);   // planes
        put16(h, 24);  // bit count
        putFourcc(h, "MJPG");
        put32(h, opt.width * opt.height * 3);
        for (int i = 0; i < 4; ++i) {
            put32(h, 0);
        }

        putFourcc(h, "LIST");
        moviSizeAt = h.size();
        put32(h, 0);
        moviStart = h.size();
        putFourcc(h, "movi");
        out.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
    }

    void writeFrame(const std::vector<std::uint8_t>& jpeg) {
        const auto pos = static_cast<std::uint64_t>(out.tellp());
        std::vector<std::uint8_t> chunk;
        putFourcc(chunk, "00dc");
        put32(chunk, static_cast<std::uint32_t>(jpeg.size()));
        out.write(reinterpret_cast<const char*>(chunk.data()), 8);
        out.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        if (jpeg.size() % 2 != 0) {
            out.put('\0');  // RIFF chunks are word aligned
        }
        index.push_back({static_cast<std::uint32_t>(pos - moviStart), static_cast<std::uint32_t>(jpeg.size())});
        largestFrame = std::max(largestFrame, static_cast<std::uint32_t>(jpeg.size()));
    }

    void run() {
        std::vector<std::uint8_t> flipped;
        std::vector<std::uint8_t> jpeg;
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    return;  // stopping and drained
                }
                job = std::move(queue.front());
                queue.pop_front();
            }
            const std::uint8_t* pixels = job.rgba.data();
            const std::size_t row = static_cast<std::size_t>(opt.width) * 4;
            if (!job.topDown) {
                flipped.resize(job.rgba.size());
                for (std::uint32_t y = 0; y < opt.height; ++y) {
                    std::memcpy(flipped.data() + y * row, job.rgba.data() + (opt.height - 1 - y) * row, row);
                }
                pixels = flipped.data();
            }
            jpeg.clear();
            stbi_write_jpg_to_func(
                [](void* ctx, void* data, int size) {
                    auto* buf = static_cast<std::vector<std::uint8_t>*>(ctx);
                    const auto* bytes = static_cast<const std::uint8_t*>(data);
                    buf->insert(buf->end(), bytes, bytes + size);
                },
                &jpeg, static_cast<int>(opt.width), static_cast<int>(opt.height), 4, pixels,
                std::clamp(opt.jpegQuality, 1, 100));
            for (std::uint32_t r = 0; r < job.repeat; ++r) {
                writeFrame(jpeg);
            }
            std::lock_guard lock(mutex);
            stats.framesWritten += job.repeat;
            stats.bytesWritten = static_cast<std::uint64_t>(out.tellp());
        }
    }

    void patch32(std::uint64_t at, std::uint32_t v) {
        out.seekp(static_cast<std::streamoff>(at));
        const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                                   static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
        out.write(reinterpret_cast<const char*>(b), 4);
    }

    void close() {
        if (closed) {
            return;
        }
        closed = true;
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        const auto moviEnd = static_cast<std::uint64_t>(out.tellp());
        std::vector<std::uint8_t> idx;
        putFourcc(idx, "idx1");
        put32(idx, static_cast<std::uint32_t>(index.size() * 16));
        for (const IndexEntry& e : index) {
            putFourcc(idx, "00dc");
            put32(idx, kAviifKeyframe);
            put32(idx, e.offset);
            put32(idx, e.size);
        }
        out.write(reinterpret_cast<const char*>(idx.data()), static_cast<std::streamsize>(idx.size()));
        const auto end = static_cast<std::uint64_t>(out.tellp());
        const auto frames = static_cast<std::uint32_t>(index.size());
        patch32(riffSizeAt, static_cast<std::uint32_t>(end - 8));
        patch32(avihFramesAt, frames);
        patch32(avihBufferAt, largestFrame + 8);
        patch32(strhLengthAt, frames);
        patch32(strhBufferAt, largestFrame + 8);
        patch32(moviSizeAt, static_cast<std::uint32_t>(moviEnd - moviStart));
        out.close();
        std::lock_guard lock(mutex);
        stats.bytesWritten = end;
    }
};

VideoWriter::VideoWriter(const VideoWriterOptions& options) : impl_(std::make_unique<Impl>(options)) {}

VideoWriter::~VideoWriter() {
    close();
}

bool VideoWriter::addFrame(std::span<const std::uint8_t> rgba, bool topDown, std::uint32_t repeat) {
    const std::size_t expected = static_cast<std::size_t>(impl_->opt.width) * impl_->opt.height * 4;
    if (rgba.size() < expected || repeat == 0) {
        return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed || impl_->stopping || impl_->queue.size() >= impl_->opt.maxQueuedFrames) {
            impl_->stats.framesDropped += repeat;
            return false;
        }
        impl_->queue.push_back(
            Job{std::vector<std::uint8_t>(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(expected)), topDown,
                repeat});
    }
    impl_->wake.notify_one();
    return true;
}

void VideoWriter::close() {
    impl_->close();
}

VideoWriterStats VideoWriter::stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

const VideoWriterOptions& VideoWriter::options() const noexcept {
    return impl_->opt;
}

}  // namespace crowdsim::core
