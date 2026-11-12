#include "crowdsim/commands/CommandDispatcher.hpp"

#include <spdlog/logger.h>

#include <exception>

#include "crowdsim/assets/SceneSerializer.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::commands {

void CommandDispatcher::submit(Json command) {
    queue_.push_back(std::move(command));
}

std::vector<CommandOutcome> CommandDispatcher::flush(CommandContext& ctx) {
    std::vector<CommandOutcome> outcomes;
    while (!queue_.empty()) {
        Json command = std::move(queue_.front());
        queue_.pop_front();
        outcomes.push_back(execute(command, ctx));
    }
    return outcomes;
}

CommandOutcome CommandDispatcher::execute(const Json& command, CommandContext& ctx) {
    const std::string name = command.is_object() ? command.value("cmd", std::string()) : std::string();
    if (name == "Undo") {
        return undo(ctx);
    }
    if (name == "Redo") {
        return redo(ctx);
    }
    return run(command, ctx, /*recordUndo=*/true, nullptr);
}

CommandOutcome CommandDispatcher::run(const Json& command, CommandContext& ctx, bool recordUndo, UndoEntry* entryOut) {
    CommandOutcome outcome;
    if (!command.is_object() || !command.contains("cmd") || !command["cmd"].is_string()) {
        outcome.error = "command must be an object {\"cmd\": name, \"params\": {...}}";
        return outcome;
    }
    outcome.command = command["cmd"].get<std::string>();
    const CommandSpec* spec = registry_.find(outcome.command);
    if (spec == nullptr) {
        outcome.error = "unknown command '" + outcome.command + "'";
        ctx.log.warn("{}", outcome.error);
        return outcome;
    }
    const Json params = command.contains("params") ? command["params"] : Json::object();
    const std::uint64_t step = ctx.sim.stepIndex();
    CommandOutput out;
    try {
        out = spec->apply(params, ctx);
    } catch (const std::exception& e) {
        outcome.error = e.what();
        ctx.log.warn("{} failed: {}", outcome.command, outcome.error);
        return outcome;
    }
    outcome.ok = true;
    outcome.result = std::move(out.result);

    Json logged = out.logAs.is_null() ? command : out.logAs;
    if (spec->logged && !ctx.replaying) {
        log_.push_back({step, logged});
    }
    if (!out.undo.is_null()) {
        UndoEntry entry{out.label.empty() ? outcome.command : out.label, std::move(out.undo), std::move(logged),
                        command.value("merge", std::string())};
        if (entryOut != nullptr) {
            *entryOut = entry;
        }
        if (recordUndo) {
            // Continuous edits (gizmo drags) share a merge key: keep the first undo, latest redo.
            if (!entry.mergeKey.empty() && !undoStack_.empty() && undoStack_.back().mergeKey == entry.mergeKey) {
                undoStack_.back().redo = std::move(entry.redo);
            } else {
                undoStack_.push_back(std::move(entry));
                if (undoStack_.size() > kMaxUndo) {
                    undoStack_.erase(undoStack_.begin());
                }
            }
            redoStack_.clear();
        }
    }
    return outcome;
}

CommandOutcome CommandDispatcher::undo(CommandContext& ctx) {
    if (undoStack_.empty()) {
        return {false, "Undo", Json::object(), "nothing to undo"};
    }
    UndoEntry entry = std::move(undoStack_.back());
    undoStack_.pop_back();
    CommandOutcome outcome = run(entry.undo, ctx, /*recordUndo=*/false, nullptr);
    if (outcome.ok) {
        entry.mergeKey.clear();
        redoStack_.push_back(std::move(entry));
    }
    outcome.command = "Undo";
    return outcome;
}

CommandOutcome CommandDispatcher::redo(CommandContext& ctx) {
    if (redoStack_.empty()) {
        return {false, "Redo", Json::object(), "nothing to redo"};
    }
    UndoEntry entry = std::move(redoStack_.back());
    redoStack_.pop_back();
    UndoEntry fresh;
    CommandOutcome outcome = run(entry.redo, ctx, /*recordUndo=*/false, &fresh);
    if (outcome.ok) {
        fresh.label = entry.label;
        fresh.mergeKey.clear();
        undoStack_.push_back(std::move(fresh));
    }
    outcome.command = "Redo";
    return outcome;
}

std::vector<HistoryItem> CommandDispatcher::history() const {
    std::vector<HistoryItem> items;
    for (const UndoEntry& e : undoStack_) {
        items.push_back({e.label, false});
    }
    for (auto it = redoStack_.rbegin(); it != redoStack_.rend(); ++it) {
        items.push_back({it->label, true});
    }
    return items;
}

void CommandDispatcher::clearHistory() {
    undoStack_.clear();
    redoStack_.clear();
}

void CommandDispatcher::restartLog(CommandContext& ctx) {
    log_.clear();
    if (ctx.logPrelude) {
        for (Json& cmd : ctx.logPrelude()) {
            log_.push_back({ctx.sim.stepIndex(), std::move(cmd)});
        }
    }
    log_.push_back({ctx.sim.stepIndex(),
                    makeCommand("RestoreScene",
                                Json{{"scene", assets::saveScene(ctx.sim, ctx.sceneName)}, {"name", ctx.sceneName}})});
}

Json CommandDispatcher::logToJson(const CommandContext& ctx) const {
    Json entries = Json::array();
    for (const LogEntry& e : log_) {
        entries.push_back(Json{{"step", e.step}, {"cmd", e.command}});
    }
    return Json{{"format", "crowdsim.cmdlog"},    {"version", 1},
                {"seed", ctx.sim.config().seed},  {"stepHz", ctx.sim.config().stepHz},
                {"endStep", ctx.sim.stepIndex()}, {"entries", std::move(entries)}};
}

std::uint64_t replayLog(const Json& log, CommandDispatcher& dispatcher, CommandContext& ctx) {
    if (log.value("format", std::string()) != "crowdsim.cmdlog") {
        throw std::invalid_argument("not a crowdsim.cmdlog document");
    }
    const bool wasReplaying = ctx.replaying;
    ctx.replaying = true;
    ctx.sim.setSeed(log.value<std::uint64_t>("seed", ctx.sim.config().seed));
    ctx.sim.clearScene();
    for (const Json& entry : log.at("entries")) {
        const std::uint64_t step = entry.at("step").get<std::uint64_t>();
        while (ctx.sim.stepIndex() < step) {
            ctx.sim.step();
        }
        const CommandOutcome outcome = dispatcher.execute(entry.at("cmd"), ctx);
        if (!outcome.ok) {
            ctx.replaying = wasReplaying;
            throw std::runtime_error("replay failed at step " + std::to_string(step) + " (" + outcome.command +
                                     "): " + outcome.error);
        }
    }
    const std::uint64_t endStep = log.value<std::uint64_t>("endStep", ctx.sim.stepIndex());
    while (ctx.sim.stepIndex() < endStep) {
        ctx.sim.step();
    }
    ctx.replaying = wasReplaying;
    return ctx.sim.stateHash();
}

}  // namespace crowdsim::commands
