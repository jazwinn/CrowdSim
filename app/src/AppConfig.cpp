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
        } else if (arg == "--content") {
            if (auto v = value()) {
                c.contentDir = std::string(*v);
            }
        } else if (arg == "--scene") {
            if (auto v = value()) {
                c.scenePath = std::string(*v);
            }
        } else if (!gui && arg == "--commands") {
            if (auto v = value()) {
                c.commandsFile = std::string(*v);
            }
        } else if (!gui && arg == "--replay") {
            if (auto v = value()) {
                c.replayFile = std::string(*v);
            }
        } else if (!gui && arg == "--save-log") {
            if (auto v = value()) {
                c.saveLogFile = std::string(*v);
            }
        } else if (!gui && arg == "--export-scene") {
            if (auto v = value()) {
                c.exportScene = std::string(*v);
            }
        } else if (gui && arg == "--no-vsync") {
            c.vsync = false;
        } else if (gui && arg == "--select") {
            number(c.selectId);
        } else if (gui && arg == "--edit-bt") {
            if (auto v = value()) {
                c.editBehaviour = std::string(*v);
            }
        } else if (gui && arg == "--select-agent") {
            number(c.selectAgentId);
        } else if (gui && arg == "--select-type") {
            if (auto v = value()) {
                c.selectType = std::string(*v);
            }
        } else if (gui && arg == "--overlays") {
            if (auto v = value()) {
                c.overlays = std::string(*v);
            }
        } else if (gui && arg == "--sim-speed") {
            number(c.simSpeed);
        } else if (gui && arg == "--lod") {
            if (auto v = value()) {
                const std::size_t comma = v->find(',');
                float nearD = 0.0f;
                float farD = 0.0f;
                if (comma == std::string_view::npos || !parseNumber(v->substr(0, comma), nearD) ||
                    !parseNumber(v->substr(comma + 1), farD) || nearD < 0.0f || farD < nearD) {
                    result.error = std::format("invalid --lod '{}', expected near,far in metres (near <= far)", *v);
                } else {
                    c.lodDistances = std::array<float, 2>{nearD, farD};
                }
            }
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
        } else if (arg == "--script") {
            if (auto v = value()) {
                c.scriptPath = std::string(*v);
            }
        } else if (!gui && arg == "--profile") {
            c.profile = true;
        } else if (!gui && arg == "--mcp-stdio") {
            c.mcpStdio = true;
        } else if (arg == "--mcp-http") {
            if (auto v = value()) {
                c.mcpHttp = std::string(*v);
            }
        } else if (arg == "--mcp-token") {
            if (auto v = value()) {
                c.mcpToken = std::string(*v);
            }
        } else if (arg == "--no-content") {
            c.loadContent = false;
        } else if (!gui && arg == "--list-commands") {
            c.listCommands = true;
        } else if (!gui && arg == "--dump-remaining") {
            c.dumpRemaining = true;
        } else if (!gui && arg == "--steps") {
            number(c.steps);
        } else if (gui && arg == "--frames") {
            number(c.frames);
        } else if (gui && arg == "--record") {
            if (auto v = value()) {
                c.recordPath = std::string(*v);
            }
        } else if (gui && arg == "--record-seconds") {
            number(c.recordSeconds);
        } else if (gui && arg == "--record-fps") {
            number(c.recordFps);
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
        "  --scene PATH        load a *.scene.json (relative to the content dir) instead of a scenario\n"
        "  --content DIR       content root (default: the repository's content/)\n"
        "  --no-content        skip content/scripts, behaviours and agents (built-in definitions only)\n"
        "  --script PATH       run a Lua script (content-relative) after the scene loads\n"
        "  --mcp-http ADDR     serve MCP (Streamable HTTP) on ADDR, e.g. 127.0.0.1:8765\n"
        "  --mcp-token TOKEN   require 'Authorization: Bearer TOKEN' on MCP HTTP requests\n"
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
            "  --overlays LIST     enable overlays: navmesh,flow,velocities,cones,nogrid,noareas,nolinks;\n"
            "                      metrics or attention bring that panel to the front\n"
            "  --camera x,z,d,y,p  initial camera: target x/z, distance, yaw and pitch in degrees\n"
            "  --lod NEAR,FAR      character LOD distances in metres (default 30,90; capsules beyond FAR)\n"
            "  --select ID         select a scene entity at startup\n"
            "  --select-agent ID   inspect (and follow) an agent at startup\n"
            "  --select-type NAME  inspect an agent type at startup\n"
            "  --edit-bt NAME      open a behaviour tree in the node editor at startup\n"
            "  --frames N          exit after N frames (smoke tests)\n"
            "  --screenshot PATH   save a PNG of the final frame (use with --frames)\n"
            "  --record PATH       record the 3D view (no UI) to an .avi video from the start\n"
            "  --record-seconds S  stop that recording after S seconds (default: at exit)\n"
            "  --record-fps F      video frame rate (default 30)\n";
    } else {
        text +=
            "\nHeadless options:\n"
            "  --steps N           number of fixed simulation steps to run (default 300)\n"
            "  --list-commands     print every command (name, description, params schema) as JSON\n"
            "  --mcp-stdio         serve MCP on stdin/stdout (for AI clients; logs go to stderr)\n"
            "  --profile           print the average wall time of each step phase (JSON)\n"
            "  --dump-remaining    print agents still alive at the end (diagnostics)\n"
            "  --commands FILE     execute a JSON array of commands (prints one result per line)\n"
            "  --replay FILE       replay a command log and print the final state hash\n"
            "  --save-log FILE     write the command log at exit\n"
            "  --export-scene FILE save the scene at exit\n";
    }
    return text;
}

}  // namespace crowdsim::app
