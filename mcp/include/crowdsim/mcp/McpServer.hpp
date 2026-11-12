#pragma once

/// @file McpServer.hpp
/// Model Context Protocol server core (JSON-RPC 2.0), independent of the
/// transport: initialize, ping, tools/list, tools/call, resources/list,
/// resources/read (plus empty prompts / resource templates). Tools and
/// resources are registered by the composition root.
///
/// Threading: handle() must run on the thread that owns the engine (the
/// HTTP transport queues requests to it, see McpRequestQueue). A tool may
/// finish later (e.g. a GUI screenshot): it then returns a future, and
/// the returned response future completes when the tool does.

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace crowdsim::mcp {

using Json = nlohmann::json;

inline constexpr const char* kProtocolVersion = "2025-06-18";

struct ToolResult {
    Json content = Json::array();  ///< MCP content blocks ({type: text|image, ...})
    Json structured;               ///< structuredContent (an object) or null
    bool isError = false;

    static ToolResult json(const Json& value);  ///< text block with the JSON + structured copy
    static ToolResult text(std::string text);
    static ToolResult error(std::string message);
    static ToolResult image(std::string base64Png, std::string caption = {});
};

struct Tool {
    std::string name;
    std::string description;
    Json inputSchema = Json{{"type", "object"}, {"properties", Json::object()}};
    std::function<ToolResult(const Json& args)> run;                        ///< synchronous tools
    std::function<std::shared_future<ToolResult>(const Json& args)> start;  ///< asynchronous tools
};

struct Resource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType = "application/json";
};

struct ResourceProvider {
    std::function<std::vector<Resource>()> list;
    /// {mimeType, text} for a uri, or nullopt if unknown.
    std::function<std::optional<std::pair<std::string, std::string>>(const std::string& uri)> read;
};

class McpServer {
public:
    McpServer(std::string name, std::string version, std::string instructions);

    void addTool(Tool tool);
    void setResources(ResourceProvider provider) { resources_ = std::move(provider); }
    [[nodiscard]] const std::vector<Tool>& tools() const noexcept { return tools_; }

    /// Handle one JSON-RPC message. The future yields the response, or null for
    /// notifications (nothing is sent back).
    std::shared_future<Json> handle(const Json& message);
    /// handle() and wait; only for transports on the engine thread (no async tools).
    Json handleNow(const Json& message) { return handle(message).get(); }

    static Json errorResponse(const Json& id, int code, const std::string& message);

private:
    Json dispatch(const std::string& method, const Json& params, const Json& id, bool& deferred,
                  std::shared_future<ToolResult>& pending);

    std::string name_;
    std::string version_;
    std::string instructions_;
    std::vector<Tool> tools_;
    ResourceProvider resources_;
};

/// Standard base64 (for image content blocks).
[[nodiscard]] std::string base64Encode(std::string_view bytes);

/// Hands messages from transport threads to the engine thread.
class McpRequestQueue {
public:
    /// Any thread: queue a message; the outer future completes once the engine
    /// thread has taken it, the inner one with the response.
    std::future<std::shared_future<Json>> submit(Json message);
    /// Engine thread: handle up to `max` queued messages. Returns how many.
    std::size_t process(McpServer& server, std::size_t max = 32);
    /// Engine thread: block until a message arrives or `timeout` passes.
    bool waitForWork(std::chrono::milliseconds timeout);

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::pair<Json, std::promise<std::shared_future<Json>>>> queue_;
};

}  // namespace crowdsim::mcp
