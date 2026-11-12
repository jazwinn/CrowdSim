/// GUI entry point: the composition root for the windowed editor.

#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

#include "AppConfig.hpp"
#include "EngineContext.hpp"
#include "GuiApp.hpp"

// SDL_main.h is deliberately not included: a plain main() works on all desktop targets.

int main(int argc, char** argv) {
    using namespace crowdsim;

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    const app::ParseResult parsed = app::parseCommandLine(args, /*gui=*/true);
    if (parsed.error) {
        std::fprintf(stderr, "error: %s\n\n%s", parsed.error->c_str(), app::usage("crowdsim", true).c_str());
        return 2;
    }
    if (parsed.config.showHelp) {
        std::printf("%s", app::usage("crowdsim", true).c_str());
        return 0;
    }
    if (parsed.config.showVersion) {
        std::printf("crowdsim %s\n", CROWDSIM_VERSION);
        return 0;
    }

    try {
        app::EngineContext engine(parsed.config);
        return app::runGui(engine);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
