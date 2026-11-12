#pragma once

/// @file CommandDispatcher.hpp
/// Queues commands and applies them between simulation steps, maintains the
/// undo/redo stacks and the replayable command log.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "crowdsim/commands/Command.hpp"

namespace crowdsim::commands {

struct CommandOutcome {
    bool ok = false;
    std::string command;
    Json result = Json::object();
    std::string error;
};

struct LogEntry {
    std::uint64_t step = 0;  ///< simulation step index at which the command was applied
    Json command;
};

struct HistoryItem {
    std::string label;
    bool undone = false;
};

class CommandDispatcher {
public:
    explicit CommandDispatcher(const CommandRegistry& registry) : registry_(registry) {}

    /// Queue a command; it is applied by the next flush() (between sim steps).
    void submit(Json command);
    /// Apply all queued commands in order.
    std::vector<CommandOutcome> flush(CommandContext& ctx);
    /// Apply one command immediately (headless / MCP / tests). "Undo" and "Redo"
    /// are accepted as commands too.
    CommandOutcome execute(const Json& command, CommandContext& ctx);

    CommandOutcome undo(CommandContext& ctx);
    CommandOutcome redo(CommandContext& ctx);
    [[nodiscard]] bool canUndo() const noexcept { return !undoStack_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redoStack_.empty(); }
    /// Undo labels oldest-first, followed by redoable items (marked undone).
    [[nodiscard]] std::vector<HistoryItem> history() const;
    void clearHistory();

    // --- Command log ---------------------------------------------------------------
    /// Start a fresh log whose first entry restores the current scene at step 0.
    /// Call right after a reset (scene load), so replay starts from identical state.
    void restartLog(CommandContext& ctx);
    [[nodiscard]] const std::vector<LogEntry>& log() const noexcept { return log_; }
    /// {"format": "crowdsim.cmdlog", "version": 1, "seed", "stepHz", "endStep", "entries": [...]}
    [[nodiscard]] Json logToJson(const CommandContext& ctx) const;

    [[nodiscard]] const CommandRegistry& registry() const noexcept { return registry_; }

private:
    struct UndoEntry {
        std::string label;
        Json undo;
        Json redo;
        std::string mergeKey;
    };
    CommandOutcome run(const Json& command, CommandContext& ctx, bool recordUndo, UndoEntry* entryOut);

    const CommandRegistry& registry_;
    std::deque<Json> queue_;
    std::vector<UndoEntry> undoStack_;
    std::vector<UndoEntry> redoStack_;
    std::vector<LogEntry> log_;
    static constexpr std::size_t kMaxUndo = 200;
};

/// Replay a command log produced by logToJson(): clears the scene, applies each
/// entry at its recorded step (stepping the simulation in between) and finally
/// steps to "endStep". Returns the simulation's state hash.
std::uint64_t replayLog(const Json& log, CommandDispatcher& dispatcher, CommandContext& ctx);

}  // namespace crowdsim::commands
