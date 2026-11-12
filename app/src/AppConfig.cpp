#include "AppConfig.hpp"

#include <charconv>
#include <cstdlib>
#include <format>
#include <type_traits>

namespace crowdsim::app {

namespace {

template <typename T>
bool parseNumber(std::string_view text, T& out) {
    if constexpr (std::is_floating_point_v<T>) {
        // Floating-point from_chars is missing from some shipping libc++ versions.
        const std::string copy(text);
        char* end = nullptr;
        const double v = std::strtod(copy.c_str(), &end);
        if (copy.empty() || end != copy.c_str() + copy.size()) {
            return false;
        }
        out = static_cast<T>(v);
        return true;
    } else {
        const char* end = text.data() + text.size();
        auto [ptr, ec] = std::from_chars(text.data(), end, out);
        return ec == std::errc{} && ptr == end;
    }
}

std::optional<spdlog::level::level_enum> parseLevel(std::string_view text) {
    const auto level = spdlog::level::from_str(std::string(text));
    if (level == spdlog::level::off && text != "off") {
        return std::nullopt;
    }
    return level;
}

}  // namespace

ParseResult parseCommandLine(std::span<const std::string_view> args, bool gui) {
    ParseResult result;
    AppConfig& c = result.config;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= args.size()) {
                result.error = std::format("missing value for {}", arg);
                return std::nullopt;
            }
            return args[++i];
        };
        auto number = [&](auto& out) {
            if (auto v = value()) {
                if (!parseNumber(*v, out)) {
                    result.error = std::format("invalid number for {}: '{}'", arg, *v);
                }
            }
        };

        if (arg == "-h" || arg == "--help") {
            c.showHelp = true;
        } else if (arg == "--version") {
            c.showVersion = true;
        } else if (arg == "--seed") {
            number(c.sim.seed);
        } else if (arg == "--hz") {
            number(c.sim.stepHz);
            if (!result.error && (c.sim.stepHz < 1.0 || c.sim.stepHz > 1000.0)) {
                result.error = "--hz must be in [1, 1000]";
            }
        } else if (arg == "--scenario") {
            if (auto v = value()) {
                if (auto kind = sim::parseScenario(*v)) {
                    c.scenario = *kind;
                } else {
                    result.error = std::format("unknown scenario '{}'", *v);
                }
            }
        } else if (arg == "--agents") {
            number(c.agents);
        } else if (gui && arg == "--no-vsync") {
            c.vsync = false;
        } else if (gui && arg == "--sim-speed") {
            number(c.simSpeed);
        } else if (gui && arg == "--camera") {
            if (auto v = value()) {
                std::array<float, 5> cam{};
                std::size_t start = 0;
                std::size_t n = 0;
                bool ok = true;
                while (ok && n < cam.size()) {
                    const std::size_t comma = v->find(',', start);
                    const std::string_view part =
                        v->substr(start, comma == std::string_view::npos ? v->npos : comma - start);
                    ok = parseNumber(part, cam[n++]);
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    start = comma + 1;
                }
                if (!ok || n != cam.size()) {
                    result.error = std::format("invalid --camera '{}', expected x,z,distance,yaw,pitch", *v);
                } else {
                    c.camera = cam;
                }
            }
        } else if (arg == "--threads") {
            number(c.threads);
        } else if (arg == "--log-level") {
            if (auto v = value()) {
                if (auto level = parseLevel(*v)) {
                    c.log.level = *level;
                } else {
                    result.error = std::format("unknown log level '{}'", *v);
                }
            }
        } else if (arg == "--log-file") {
            if (auto v = value()) {
                c.log.filePath = std::string(*v);
            }
        } else if (!gui && arg == "--steps") {
            number(c.steps);
        } else if (gui && arg == "--frames") {
            number(c.frames);
        } else if (gui && arg == "--screenshot") {
            if (auto v = value()) {
                c.screenshotPath = std::string(*v);
            }
        } else if (gui && arg == "--renderer") {
            if (auto v = value()) {
                c.renderer = std::string(*v);
            }
        } else if (gui && arg == "--size") {
            if (auto v = value()) {
                const auto x = v->find('x');
                if (x == std::string_view::npos || !parseNumber(v->substr(0, x), c.width) ||
                    !parseNumber(v->substr(x + 1), c.height)) {
                    result.error = std::format("invalid --size '{}', expected WIDTHxHEIGHT", *v);
                }
            }
        } else {
            result.error = std::format("unknown argument '{}'", arg);
        }

        if (result.error) {
            break;
        }
    }
    return result;
}

std::string usage(std::string_view exeName, bool gui) {
    std::string text = std::format("Usage: {} [options]\n\nCommon options:\n", exeName);
    text +=
        "  --seed N            world RNG seed (default 1)\n"
        "  --hz N              simulation rate in Hz (default 30)\n"
        "  --scenario NAME     stream | counterflow | benchmark (default stream)\n"
        "  --agents N          total agents the scenario spawns (default 5000)\n"
        "  --threads N         worker threads incl. main (default: all cores)\n"
        "  --log-level L       trace|debug|info|warn|err|critical|off\n"
        "  --log-file PATH     also write the log to PATH\n"
        "  --version           print version and exit\n"
        "  -h, --help          show this help\n";
    if (gui) {
        text +=
            "\nGUI options:\n"
            "  --renderer R        auto|d3d11|d3d12|vulkan|metal|opengl|noop\n"
            "  --size WxH          initial window size (default 1600x900)\n"
            "  --no-vsync          present without vsync (for measuring frame time)\n"
            "  --sim-speed X       initial simulation speed multiplier (default 1)\n"
            "  --camera x,z,d,y,p  initial camera: target x/z, distance, yaw and pitch in degrees\n"
            "  --frames N          exit after N frames (smoke tests)\n"
            "  --screenshot PATH   save a PNG of the final frame (use with --frames)\n";
    } else {
        text +=
            "\nHeadless options:\n"
            "  --steps N           number of fixed simulation steps to run (default 300)\n";
    }
    return text;
}

}  // namespace crowdsim::app
