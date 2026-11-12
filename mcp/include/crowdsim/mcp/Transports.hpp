#pragma once

/// @file Transports.hpp
/// MCP transports.
///  * stdio: newline-delimited JSON-RPC on stdin/stdout, run on the engine
///    thread (headless). Logs must go to stderr.
///  * Streamable HTTP: POST /mcp on a background thread (cpp-httplib), bound
///    to localhost by default, optional bearer token. Requests are executed on
///    the engine thread through an McpRequestQueue.

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

namespace crowdsim::mcp {

class McpServer;
class McpRequestQueue;

/// Serve until stdin closes. `onIdle` (optional) runs between messages.
void runStdio(McpServer& server, std::istream& in, std::ostream& out);

class HttpTransport {
public:
    HttpTransport(McpRequestQueue& queue, std::string token = {});
    ~HttpTransport();
    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;

    /// Bind and serve on a background thread; throws std::runtime_error if the
    /// address cannot be bound. `port` 0 picks a free port (see port()).
    void start(const std::string& host, int port);
    void stop();
    [[nodiscard]] int port() const noexcept { return port_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int port_ = 0;
};

/// "127.0.0.1:8765" -> {host, port}; a bare port means 127.0.0.1.
[[nodiscard]] std::pair<std::string, int> parseHostPort(const std::string& text);

}  // namespace crowdsim::mcp
