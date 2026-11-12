#include "crowdsim/mcp/McpServer.hpp"

#include <stdexcept>

namespace crowdsim::mcp {

namespace {

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

Json ok(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json toolResultJson(const ToolResult& r) {
    Json j{{"content", r.content}, {"isError", r.isError}};
    if (r.structured.is_object()) {
        j["structuredContent"] = r.structured;
    }
    return j;
}

std::shared_future<Json> ready(Json value) {
    std::promise<Json> p;
    p.set_value(std::move(value));
    return p.get_future().share();
}

}  // namespace

ToolResult ToolResult::json(const Json& value) {
    ToolResult r;
    r.content.push_back(Json{{"type", "text"}, {"text", value.dump()}});
    if (value.is_object()) {
        r.structured = value;
    }
    return r;
}

ToolResult ToolResult::text(std::string text) {
    ToolResult r;
    r.content.push_back(Json{{"type", "text"}, {"text", std::move(text)}});
    return r;
}

ToolResult ToolResult::error(std::string message) {
    ToolResult r = text(std::move(message));
    r.isError = true;
    return r;
}

ToolResult ToolResult::image(std::string base64Png, std::string caption) {
    ToolResult r;
    r.content.push_back(Json{{"type", "image"}, {"data", std::move(base64Png)}, {"mimeType", "image/png"}});
    if (!caption.empty()) {
        r.content.push_back(Json{{"type", "text"}, {"text", std::move(caption)}});
    }
    return r;
}

McpServer::McpServer(std::string name, std::string version, std::string instructions)
    : name_(std::move(name)), version_(std::move(version)), instructions_(std::move(instructions)) {}

void McpServer::addTool(Tool tool) {
    for (Tool& t : tools_) {
        if (t.name == tool.name) {
            t = std::move(tool);
            return;
        }
    }
    tools_.push_back(std::move(tool));
}

Json McpServer::errorResponse(const Json& id, int code, const std::string& message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

std::shared_future<Json> McpServer::handle(const Json& message) {
    if (!message.is_object() || !message.contains("method") || !message["method"].is_string()) {
        // Responses from the client (we never send requests) and malformed messages.
        if (message.is_object() && (message.contains("result") || message.contains("error"))) {
            return ready(nullptr);
        }
        return ready(errorResponse(message.is_object() ? message.value("id", Json()) : Json(), kInvalidRequest,
                                   "invalid JSON-RPC request"));
    }
    const std::string method = message["method"].get<std::string>();
    const bool notification = !message.contains("id");
    const Json id = notification ? Json() : message["id"];
    const Json params = message.value("params", Json::object());
    bool deferred = false;
    std::shared_future<ToolResult> pending;
    Json response;
    try {
        response = dispatch(method, params, id, deferred, pending);
    } catch (const std::exception& e) {
        response = errorResponse(id, kInternalError, e.what());
    }
    if (notification) {
        return ready(nullptr);
    }
    if (deferred) {
        // Completes in whichever thread waits on it (the transport), never the engine thread.
        return std::async(std::launch::deferred, [pending, id] { return ok(id, toolResultJson(pending.get())); })
            .share();
    }
    return ready(std::move(response));
}

Json McpServer::dispatch(const std::string& method, const Json& params, const Json& id, bool& deferred,
                         std::shared_future<ToolResult>& pending) {
    if (method == "initialize") {
        const std::string requested = params.value("protocolVersion", std::string(kProtocolVersion));
        const bool known = requested == "2025-06-18" || requested == "2025-03-26" || requested == "2024-11-05";
        return ok(id,
                  Json{{"protocolVersion", known ? requested : std::string(kProtocolVersion)},
                       {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}}}}},
                       {"serverInfo", {{"name", name_}, {"version", version_}}},
                       {"instructions", instructions_}});
    }
    if (method == "ping") {
        return ok(id, Json::object());
    }
    if (method.rfind("notifications/", 0) == 0) {
        return nullptr;
    }
    if (method == "tools/list") {
        Json list = Json::array();
        for (const Tool& t : tools_) {
            list.push_back(Json{{"name", t.name}, {"description", t.description}, {"inputSchema", t.inputSchema}});
        }
        return ok(id, Json{{"tools", list}});
    }
    if (method == "tools/call") {
        const std::string name = params.value("name", std::string());
        const Json args = params.value("arguments", Json::object());
        if (!args.is_object()) {
            return errorResponse(id, kInvalidParams, "arguments must be an object");
        }
        for (const Tool& t : tools_) {
            if (t.name != name) {
                continue;
            }
            try {
                if (t.start) {
                    pending = t.start(args);
                    deferred = true;
                    return nullptr;
                }
                return ok(id, toolResultJson(t.run(args)));
            } catch (const std::exception& e) {
                // Tool failures are results the model can read, not protocol errors.
                return ok(id, toolResultJson(ToolResult::error(e.what())));
            }
        }
        return errorResponse(id, kInvalidParams, "unknown tool '" + name + "'");
    }
    if (method == "resources/list") {
        Json list = Json::array();
        if (resources_.list) {
            for (const Resource& r : resources_.list()) {
                list.push_back(
                    Json{{"uri", r.uri}, {"name", r.name}, {"description", r.description}, {"mimeType", r.mimeType}});
            }
        }
        return ok(id, Json{{"resources", list}});
    }
    if (method == "resources/read") {
        const std::string uri = params.value("uri", std::string());
        const auto content = resources_.read ? resources_.read(uri) : std::nullopt;
        if (!content) {
            return errorResponse(id, -32002, "resource not found: " + uri);
        }
        return ok(id,
                  Json{{"contents",
                        Json::array({Json{{"uri", uri}, {"mimeType", content->first}, {"text", content->second}}})}});
    }
    if (method == "resources/templates/list") {
        return ok(id, Json{{"resourceTemplates", Json::array()}});
    }
    if (method == "prompts/list") {
        return ok(id, Json{{"prompts", Json::array()}});
    }
    (void)kParseError;
    return errorResponse(id, kMethodNotFound, "method not found: " + method);
}

std::string base64Encode(std::string_view bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const auto v = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                       static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i < bytes.size()) {
        auto v = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
        if (i + 1 < bytes.size()) {
            v |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8;
        }
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::future<std::shared_future<Json>> McpRequestQueue::submit(Json message) {
    std::promise<std::shared_future<Json>> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lock(mutex_);
        queue_.emplace_back(std::move(message), std::move(promise));
    }
    ready_.notify_one();
    return future;
}

std::size_t McpRequestQueue::process(McpServer& server, std::size_t max) {
    std::size_t handled = 0;
    while (handled < max) {
        std::pair<Json, std::promise<std::shared_future<Json>>> item;
        {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) {
                break;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        item.second.set_value(server.handle(item.first));
        ++handled;
    }
    return handled;
}

bool McpRequestQueue::waitForWork(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, timeout, [&] { return !queue_.empty(); });
}

}  // namespace crowdsim::mcp
