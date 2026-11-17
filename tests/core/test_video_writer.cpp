// VideoWriter: frames in, a valid MJPEG AVI out.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "crowdsim/core/VideoWriter.hpp"

using namespace crowdsim;

namespace {

std::uint32_t le32(const std::vector<char>& b, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

std::size_t find(const std::vector<char>& b, const char* fourcc, std::size_t from = 0) {
    for (std::size_t i = from; i + 4 <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, fourcc, 4) == 0) {
            return i;
        }
    }
    return std::string::npos;
}

}  // namespace

TEST_CASE("VideoWriter writes a readable MJPEG AVI with every frame indexed") {
    const auto file = std::filesystem::temp_directory_path() / "crowdsim_test_video.avi";
    constexpr std::uint32_t kW = 64;
    constexpr std::uint32_t kH = 48;
    {
        core::VideoWriter writer({.file = file, .width = kW, .height = kH, .fps = 25.0, .maxQueuedFrames = 64});
        std::vector<std::uint8_t> rgba(kW * kH * 4);
        for (int f = 0; f < 4; ++f) {
            for (std::size_t p = 0; p < rgba.size(); p += 4) {
                rgba[p] = static_cast<std::uint8_t>(f * 60);
                rgba[p + 1] = static_cast<std::uint8_t>(p % 255);
                rgba[p + 2] = 128;
                rgba[p + 3] = 255;
            }
            CHECK(writer.addFrame(rgba, f % 2 == 0, f == 3 ? 2u : 1u));
        }
        CHECK_FALSE(writer.addFrame(std::vector<std::uint8_t>(10)));  // wrong size
        writer.close();
        CHECK(writer.stats().framesWritten == 5);
        CHECK(writer.stats().framesDropped == 0);
    }
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(b.size() > 200);
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(std::memcmp(b.data() + 8, "AVI ", 4) == 0);
    CHECK(le32(b, 4) == b.size() - 8);
    const std::size_t avih = find(b, "avih");
    REQUIRE(avih != std::string::npos);
    CHECK(le32(b, avih + 8) == 40000);   // microseconds per frame at 25 fps
    CHECK(le32(b, avih + 8 + 16) == 5);  // total frames
    CHECK(le32(b, avih + 8 + 32) == kW);
    CHECK(le32(b, avih + 8 + 36) == kH);
    const std::size_t strf = find(b, "strf");
    REQUIRE(strf != std::string::npos);
    CHECK(std::memcmp(b.data() + strf + 8 + 16, "MJPG", 4) == 0);
    // Five indexed frames, each a JPEG (starts with FF D8) at the offset the index gives.
    const std::size_t movi = find(b, "movi");
    const std::size_t idx1 = find(b, "idx1", movi);
    REQUIRE(idx1 != std::string::npos);
    CHECK(le32(b, idx1 + 4) == 5 * 16);
    for (int f = 0; f < 5; ++f) {
        const std::size_t entry = idx1 + 8 + static_cast<std::size_t>(f) * 16;
        const std::size_t chunk = movi + le32(b, entry + 8);
        CHECK(std::memcmp(b.data() + chunk, "00dc", 4) == 0);
        CHECK(static_cast<unsigned char>(b[chunk + 8]) == 0xFF);
        CHECK(static_cast<unsigned char>(b[chunk + 9]) == 0xD8);
    }
    in.close();
    std::filesystem::remove(file);
}
