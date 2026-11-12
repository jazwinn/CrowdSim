#include "ContentWatcher.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace crowdsim::app {

namespace fs = std::filesystem;

ContentWatcher::ContentWatcher(fs::path contentRoot) : root_(std::move(contentRoot)) {}

std::vector<ContentChange> ContentWatcher::poll(bool force, std::chrono::milliseconds interval) {
    std::vector<ContentChange> out;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - lastScan_ < interval) {
        return out;
    }
    lastScan_ = now;
    scanDirectory(root_ / "scripts", ".lua", "LoadScript", "source", out, true);
    scanDirectory(root_ / "behaviours", ".bt.json", "SetBehaviorTree", "tree", out);
    scanDirectory(root_ / "agents", ".agent.json", "DefineAgentType", "type", out);
    return out;
}

void ContentWatcher::scanDirectory(const fs::path& dir, const std::string& suffix, const char* command,
                                   const char* param, std::vector<ContentChange>& out, bool script) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return;
    }
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(ec) && name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());  // deterministic load order
    for (const fs::path& file : files) {
        const auto mtime = fs::last_write_time(file, ec);
        if (ec) {
            continue;
        }
        const auto it = seen_.find(file);
        if (it != seen_.end() && it->second == mtime) {
            continue;
        }
        seen_[file] = mtime;
        ContentChange change{file, nullptr, {}};
        try {
            std::ifstream in(file, std::ios::binary);
            if (script) {
                const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const std::string name = file.filename().string();
                change.command = nlohmann::json{
                    {"cmd", command},
                    {"params", {{"name", name.substr(0, name.size() - suffix.size())}, {param, source}}}};
                out.push_back(std::move(change));
                continue;
            }
            const nlohmann::json doc = nlohmann::json::parse(in);
            change.command = nlohmann::json{{"cmd", command}, {"params", {{param, doc}}}};
        } catch (const std::exception& e) {
            change.error = e.what();
        }
        out.push_back(std::move(change));
    }
}

}  // namespace crowdsim::app
