#pragma once

// Router.h
// Port-unification listener: accepts every connection on the public port
// (default 8000), peeks the HTTP request line + headers, and dispatches:
//   - "Upgrade: websocket" + path starts with /ws/  -> WebSocketHub::adopt_socket()
//   - everything else                               -> piped to internal cpp-httplib
//
// Rationale: cpp-httplib does not expose the underlying socket from a
// handler, so the React client cannot upgrade to WebSocket on the same
// port via cpp-httplib alone. Owning the listening socket here is the
// cleanest way to keep React's hardcoded ws://127.0.0.1:8000 working.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class WebSocketHub;

class Router {
public:
    Router(int public_port, int internal_http_port, WebSocketHub* hub);
    ~Router();

    void start(); // launches background accept thread
    void stop();  // safe to call multiple times

    int public_port()   const { return public_port_; }
    int internal_port() const { return internal_port_; }

private:
    void accept_loop();
    void handle_connection(uintptr_t raw_sock);
    void proxy_http(uintptr_t raw_sock, const std::string& preface);

    int               public_port_;
    int               internal_port_;
    WebSocketHub*     hub_; // not owned
    uintptr_t         listen_sock_{~uintptr_t(0)};
    std::atomic<bool> running_{false};
    std::thread       accept_thread_;
};
