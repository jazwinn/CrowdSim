#include "crowdsim/mcp/Transports.hpp"

#include <httplib.h>

#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

#include "crowdsim/mcp/McpServer.hpp"

namespace crowdsim::mcp {

void runStdio(McpServer& server, std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        Json response;
        try {
            response = server.handleNow(Json::parse(line));
        } catch (const Json::parse_error& e) {
            response = McpServer::errorResponse(nullptr, -32700, std::string("parse error: ") + e.what());
        }
        if (!response.is_null()) {
            out << response.dump() << '\n';
            out.flush();
        }
    }
}

std::pair<std::string, int> parseHostPort(const std::string& text) {
    const auto colon = text.rfind(':');
    std::string host = colon == std::string::npos ? "127.0.0.1" : text.substr(0, colon);
    const std::string port = colon == std::string::npos ? text : text.substr(colon + 1);
    if (host.empty()) {
        host = "127.0.0.1";
    }
    try {
        return {host, std::stoi(port)};
    } catch (const std::exception&) {
        throw std::invalid_argument("expected host:port, got '" + text + "'");
    }
}

struct HttpTransport::Impl {
    McpRequestQueue& queue;
    std::string token;
    httplib::Server server;
    std::thread thread;
    std::string sessionId;

    Impl(McpRequestQueue& q, std::string t) : queue(q), token(std::move(t)) {
        std::random_device rd;
        sessionId = std::to_string(rd()) + std::to_string(rd());
    }
};

HttpTransport::HttpTransport(McpRequestQueue& queue, std::string token)
    : impl_(std::make_unique<Impl>(queue, std::move(token))) {}

HttpTransport::~HttpTransport() {
    stop();
}

void HttpTransport::start(const std::string& host, int port) {
    Impl& im = *impl_;
    im.server.Post("/mcp", [&im](const httplib::Request& req, httplib::Response& res) {
        if (!im.token.empty() && req.get_header_value("Authorization") != "Bearer " + im.token) {
            res.status = 401;
            res.set_content(R"({"error":"missing or invalid bearer token"})", "application/json");
            return;
        }
        Json message;
        try {
            message = Json::parse(req.body);
        } catch (const Json::parse_error& e) {
            res.status = 400;
            res.set_content(McpServer::errorResponse(nullptr, -32700, e.what()).dump(), "application/json");
            return;
        }
        // Executed on the engine thread; this thread only waits.
        auto taken = im.queue.submit(message);
        if (taken.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
            res.status = 503;
            res.set_content(R"({"error":"engine busy"})", "application/json");
            return;
        }
        std::shared_future<Json> response = taken.get();
        // Deferred responses (async tools) run their wait inside get(); others may still be pending.
        if (response.wait_for(std::chrono::seconds(0)) == std::future_status::timeout &&
            response.wait_for(std::chrono::seconds(600)) != std::future_status::ready) {
            res.status = 504;
            return;
        }
        const Json body = response.get();
        if (message.is_object() && message.value("method", std::string()) == "initialize") {
            res.set_header("Mcp-Session-Id", im.sessionId);
        }
        if (body.is_null()) {
            res.status = 202;  // notification or client response: accepted, no body
            return;
        }
        res.set_content(body.dump(), "application/json");
    });
    im.server.Get("/mcp", [](const httplib::Request&, httplib::Response& res) {
        res.status = 405;  // no server-initiated stream
    });
    im.server.Delete("/mcp", [](const httplib::Request&, httplib::Response& res) { res.status = 200; });

    if (port == 0) {
        port_ = im.server.bind_to_any_port(host);
        if (port_ <= 0) {
            throw std::runtime_error("MCP: cannot bind " + host);
        }
    } else {
        if (!im.server.bind_to_port(host, port)) {
            throw std::runtime_error("MCP: cannot bind " + host + ":" + std::to_string(port));
        }
        port_ = port;
    }
    im.thread = std::thread([&im] { im.server.listen_after_bind(); });
}

void HttpTransport::stop() {
    if (!impl_) {
        return;
    }
    impl_->server.stop();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

}  // namespace crowdsim::mcp
