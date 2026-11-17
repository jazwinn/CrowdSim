#pragma once

/// @file ContentWatcher.hpp
/// Watches content/scripts/*.lua, content/behaviours/*.bt.json and
/// content/agents/*.agent.json and turns new or modified files into LoadScript,
/// SetBehaviorTree and DefineAgentType commands (hot reload). Polls modification
/// times; no OS-specific watcher needed.

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace crowdsim::app {

/// A command produced from a content file.
struct ContentChange {
    std::filesystem::path file;
    nlohmann::json command;  ///< empty (null) if the file could not be parsed
    std::string error;       ///< parse error, if any
};

class ContentWatcher {
public:
    explicit ContentWatcher(std::filesystem::path contentRoot);

    /// Commands for files that are new or changed since the last scan: scripts
    /// first (trees may use their nodes), then trees, then agent types (types
    /// reference trees). Scans at most every
    /// `interval` unless `force` is set.
    [[nodiscard]] std::vector<ContentChange> poll(bool force = false,
                                                  std::chrono::milliseconds interval = std::chrono::milliseconds(500));

private:
    /// `script`: pass the file as source text; `named`: also pass the file's name (stem).
    void scanDirectory(const std::filesystem::path& dir, const std::string& suffix, const char* command,
                       const char* param, std::vector<ContentChange>& out, bool script = false, bool named = false);

    std::filesystem::path root_;
    std::map<std::filesystem::path, std::filesystem::file_time_type> seen_;
    std::chrono::steady_clock::time_point lastScan_{};
};

}  // namespace crowdsim::app
