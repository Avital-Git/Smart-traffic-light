// Router.cpp — port unification for HTTP + WebSocket on the same port.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "Router.h"
#include "WebSocketHub.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

static inline SOCKET to_sock(uintptr_t v) { return (SOCKET)v; }
static inline uintptr_t to_uint(SOCKET s) { return (uintptr_t)s; }
static const uintptr_t INVALID_U = to_uint(INVALID_SOCKET);

bool ends_with_blankline(const std::string& buf) {
    return buf.size() >= 4 &&
        buf[buf.size()-4] == '\r' && buf[buf.size()-3] == '\n' &&
        buf[buf.size()-2] == '\r' && buf[buf.size()-1] == '\n';
}

// Peek (without consuming) up to a full HTTP request-line + headers. Returns
// whatever we have once "\r\n\r\n" is seen, or after we run out of buffer.
// The returned string does NOT remove data from the socket; we use MSG_PEEK
// so the downstream HTTP server still sees the full request.
std::string peek_http_preface(SOCKET sock) {
    constexpr int kMax = 8192;
    std::vector<char> buf(kMax);
    int total = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);

    while (total < kMax) {
        int n = ::recv(sock, buf.data(), kMax, MSG_PEEK);
        if (n <= 0) {
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
                if (std::chrono::steady_clock::now() > deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            break;
        }
        total = n;
        std::string preface(buf.data(), buf.data() + n);
        if (ends_with_blankline(preface)) {
            return preface;
        }
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::string(buf.data(), buf.data() + total);
}

std::string header_value(const std::string& preface, const std::string& name_lower) {
    std::string lower(preface.size(), '\0');
    std::transform(preface.begin(), preface.end(), lower.begin(),
                   [](char c){ return (char)std::tolower((unsigned char)c); });
    const std::string needle = "\r\n" + name_lower + ":";
    auto pos = lower.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < preface.size() && (preface[pos] == ' ' || preface[pos] == '\t')) ++pos;
    auto end = preface.find("\r\n", pos);
    if (end == std::string::npos) end = preface.size();
    return preface.substr(pos, end - pos);
}

std::string request_path(const std::string& preface) {
    auto p1 = preface.find(' ');
    if (p1 == std::string::npos) return "";
    auto p2 = preface.find(' ', p1 + 1);
    if (p2 == std::string::npos) p2 = preface.find('\r', p1 + 1);
    if (p2 == std::string::npos) return "";
    return preface.substr(p1 + 1, p2 - p1 - 1);
}

bool is_websocket_upgrade(const std::string& preface) {
    std::string upgrade = header_value(preface, "upgrade");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(),
                   [](char c){ return (char)std::tolower((unsigned char)c); });
    if (upgrade.find("websocket") == std::string::npos) return false;
    const std::string path = request_path(preface);
    return path.rfind("/ws/", 0) == 0;
}

SOCKET connect_internal(int port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

void pipe_bytes(SOCKET from, SOCKET to, std::atomic<bool>& done) {
    constexpr int kBuf = 8192;
    std::vector<char> buf(kBuf);
    while (!done) {
        int n = ::recv(from, buf.data(), kBuf, 0);
        if (n <= 0) break;
        int sent = 0;
        while (sent < n) {
            int m = ::send(to, buf.data() + sent, n - sent, 0);
            if (m <= 0) { done = true; return; }
            sent += m;
        }
    }
    done = true;
    shutdown(to, SD_SEND);
}

} // namespace

// ---------------------------------------------------------------------------

Router::Router(int public_port, int internal_http_port, WebSocketHub* hub)
    : public_port_(public_port), internal_port_(internal_http_port), hub_(hub) {}

Router::~Router() { stop(); }

void Router::start() {
    running_ = true;
    accept_thread_ = std::thread(&Router::accept_loop, this);
}

void Router::stop() {
    running_ = false;
    if (listen_sock_ != INVALID_U) {
        closesocket(to_sock(listen_sock_));
        listen_sock_ = INVALID_U;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

void Router::accept_loop() {
    SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        std::cerr << "[Router] socket() failed\n";
        return;
    }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)public_port_);

    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        ::listen(srv, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[Router] bind/listen failed on port " << public_port_ << "\n";
        closesocket(srv);
        return;
    }
    listen_sock_ = to_uint(srv);
    std::cout << "[Router] public port: " << public_port_
              << "  (HTTP -> 127.0.0.1:" << internal_port_
              << ", WebSocket -> in-process hub)\n";

    while (running_) {
        SOCKET client = ::accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        std::thread(&Router::handle_connection, this, to_uint(client)).detach();
    }
    closesocket(srv);
    listen_sock_ = INVALID_U;
}

void Router::handle_connection(uintptr_t raw_sock) {
    SOCKET sock = to_sock(raw_sock);

    // Brief receive timeout so peek doesn't stall forever on a half-open
    // connection. Real WS / HTTP traffic completes the request promptly.
    DWORD tv = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

    std::string preface = peek_http_preface(sock);
    if (preface.empty()) {
        closesocket(sock);
        return;
    }

    if (is_websocket_upgrade(preface) && hub_) {
        // Restore blocking semantics; the hub manages its own timeouts.
        DWORD tv0 = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv0, sizeof(tv0));
        hub_->adopt_socket(raw_sock);
        return; // hub now owns the socket
    }

    proxy_http(raw_sock, preface);
}

void Router::proxy_http(uintptr_t raw_sock, const std::string& /*preface*/) {
    SOCKET client_sock = to_sock(raw_sock);

    SOCKET upstream = connect_internal(internal_port_);
    if (upstream == INVALID_SOCKET) {
        const char* resp =
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        ::send(client_sock, resp, (int)std::strlen(resp), 0);
        closesocket(client_sock);
        return;
    }

    // Restore blocking semantics for both sockets.
    DWORD tv0 = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv0, sizeof(tv0));

    std::atomic<bool> done{false};
    std::thread t1(pipe_bytes, client_sock, upstream, std::ref(done));
    pipe_bytes(upstream, client_sock, done);
    if (t1.joinable()) t1.join();

    closesocket(upstream);
    closesocket(client_sock);
}
