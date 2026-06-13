// WebSocketHub.cpp
// Minimal RFC-6455 WebSocket server for Windows using raw WinSock + WinCrypt.
// No extra libraries required beyond what is already linked (ws2_32, advapi32).

// winsock2.h must come before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include "WebSocketHub.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ── Convenience alias ────────────────────────────────────────────────────────
static inline SOCKET to_sock(uintptr_t v) { return (SOCKET)v; }
static inline uintptr_t to_uint(SOCKET s) { return (uintptr_t)s; }
static const uintptr_t INVALID = to_uint(INVALID_SOCKET);

// ── Base64 ───────────────────────────────────────────────────────────────────
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* d, size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b = (uint32_t)d[i] << 16;
        if (i+1 < n) b |= (uint32_t)d[i+1] << 8;
        if (i+2 < n) b |= d[i+2];
        out += B64[(b >> 18) & 63];
        out += B64[(b >> 12) & 63];
        out += (i+1 < n) ? B64[(b >>  6) & 63] : '=';
        out += (i+2 < n) ? B64[ b        & 63] : '=';
    }
    return out;
}

// ── SHA-1 via WinCrypt (advapi32) ────────────────────────────────────────────
static std::vector<uint8_t> sha1(const std::string& data) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::vector<uint8_t> result(20, 0);
    if (!CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return result;
    if (CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptHashData(hHash, (const BYTE*)data.data(), (DWORD)data.size(), 0);
        DWORD len = 20;
        CryptGetHashParam(hHash, HP_HASHVAL, result.data(), &len, 0);
        CryptDestroyHash(hHash);
    }
    CryptReleaseContext(hProv, 0);
    return result;
}

static std::string ws_accept_key(const std::string& key) {
    auto h = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64_encode(h.data(), h.size());
}

// ── TCP helpers ───────────────────────────────────────────────────────────────
// Receive exactly 'len' bytes. Returns >0 on success, ≤0 on error/close.
static int recv_exact(SOCKET sock, void* buf, int len) {
    int done = 0;
    while (done < len) {
        int n = ::recv(sock, (char*)buf + done, len - done, 0);
        if (n <= 0) return n == 0 ? 0 : -1;
        done += n;
    }
    return done;
}

// Read until \r\n\r\n (HTTP request end) or buffer full.
static std::string read_http_request(SOCKET sock) {
    std::string buf;
    buf.reserve(2048);
    char c;
    while (buf.size() < 8192) {
        if (::recv(sock, &c, 1, 0) <= 0) break;
        buf += c;
        if (buf.size() >= 4 &&
            buf[buf.size()-4] == '\r' && buf[buf.size()-3] == '\n' &&
            buf[buf.size()-2] == '\r' && buf[buf.size()-1] == '\n')
            break;
    }
    return buf;
}

// ── HTTP header helpers ───────────────────────────────────────────────────────
static std::string req_header(const std::string& req, const std::string& name) {
    // Case-insensitive search for "\r\n<name>: <value>\r\n"
    std::string lower_req = req;
    std::transform(lower_req.begin(), lower_req.end(), lower_req.begin(), ::tolower);
    std::string target = "\r\n" + name + ":";
    std::transform(target.begin(), target.end(), target.begin(), ::tolower);

    auto pos = lower_req.find(target);
    if (pos == std::string::npos) return "";
    pos += target.size();
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) ++pos;
    auto end = req.find("\r\n", pos);
    if (end == std::string::npos) end = req.size();
    return req.substr(pos, end - pos);
}

static std::string req_path(const std::string& req) {
    // "GET /path HTTP/1.1\r\n..."
    auto p1 = req.find(' ');
    if (p1 == std::string::npos) return "";
    auto p2 = req.find(' ', p1 + 1);
    if (p2 == std::string::npos) p2 = req.find('\r', p1 + 1);
    if (p2 == std::string::npos) return "";
    return req.substr(p1 + 1, p2 - p1 - 1);
}

// ── WebSocket frame helpers ───────────────────────────────────────────────────
// Send an unmasked text frame (server → client direction is always unmasked).
static bool ws_send(SOCKET sock, const std::string& text) {
    size_t len = text.size();
    std::vector<uint8_t> frame;
    frame.reserve(len + 4);
    frame.push_back(0x81);           // FIN + opcode=1 (text)
    if (len < 126) {
        frame.push_back((uint8_t)len);
    } else {
        frame.push_back(0x7e);       // 16-bit extended length
        frame.push_back((uint8_t)(len >> 8));
        frame.push_back((uint8_t)(len & 0xff));
    }
    frame.insert(frame.end(), (const uint8_t*)text.data(),
                              (const uint8_t*)text.data() + len);
    int r = ::send(sock, (const char*)frame.data(), (int)frame.size(), 0);
    return r == (int)frame.size();
}

static void ws_close_frame(SOCKET sock) {
    uint8_t f[2] = {0x88, 0x00};    // FIN + opcode=8 (close), no payload
    ::send(sock, (const char*)f, 2, 0);
}

// ── WebSocketHub ──────────────────────────────────────────────────────────────

WebSocketHub::WebSocketHub(int port) : port_(port) {}

WebSocketHub::~WebSocketHub() { stop(); }

void WebSocketHub::start() {
    running_ = true;
    if (!own_listener_enabled_) {
        std::cout << "[WSHub] running in adopt-only mode (external Router owns the port)\n";
        return;
    }
    accept_thread_ = std::thread(&WebSocketHub::accept_loop, this);
}

void WebSocketHub::stop() {
    running_ = false;

    // Close the listening socket to unblock accept().
    if (listen_sock_ != INVALID) {
        closesocket(to_sock(listen_sock_));
        listen_sock_ = INVALID;
    }

    // Close every registered client socket (detached threads will unblock).
    std::vector<SOCKET> to_close;
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        for (auto& c : clients_) {
            if (c->alive.exchange(false))
                to_close.push_back(to_sock(c->sock));
        }
        clients_.clear();
    }
    for (SOCKET s : to_close) closesocket(s);
    // Accept thread exits on its own after listen_sock_ is closed.
}

// ── Accept loop ───────────────────────────────────────────────────────────────
void WebSocketHub::accept_loop() {
    SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        std::cerr << "[WSHub] socket() failed\n";
        return;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port_);

    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        ::listen(srv, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[WSHub] bind/listen failed on port " << port_ << "\n";
        closesocket(srv);
        return;
    }

    listen_sock_ = to_uint(srv);
    std::cout << "[WSHub] WebSocket listening on ws://0.0.0.0:" << port_ << "\n";
    std::cout << "[WSHub]   /ws/updates              (global)\n";
    std::cout << "[WSHub]   /ws/intersection/{id}    (per-intersection)\n";

    while (running_) {
        SOCKET client = ::accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;   // stop() closed listen_sock_

        // Each client handled in its own detached thread.
        std::thread(&WebSocketHub::handle_client, this, to_uint(client)).detach();
    }

    closesocket(srv);
    listen_sock_ = INVALID;
}

// ── Per-client handler ────────────────────────────────────────────────────────
void WebSocketHub::handle_client(uintptr_t raw_sock) {
    SOCKET sock = to_sock(raw_sock);

    // Set a 1-second receive timeout so the read loop can notice stop().
    DWORD tv = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

    int iid = -1;
    if (!perform_handshake(sock, iid)) {
        closesocket(sock);
        return;
    }

    auto client = std::make_shared<WSClient>(raw_sock, iid);

    // Send welcome message (mirrors Python server)
    double ts = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    json welcome;
    if (iid < 0) {
        welcome = {
            {"event",           "welcome"},
            {"intersection_id", nullptr},
            {"timestamp",       ts},
            {"payload",         {{"message", "connected_to_global_updates"}}},
        };
    } else {
        welcome = {
            {"event",           "welcome"},
            {"intersection_id", iid},
            {"timestamp",       ts},
            {"payload",         {
                {"message",   "connected_to_intersection_updates"},
                {"has_state", false},
            }},
        };
    }
    {
        std::lock_guard<std::mutex> lk(client->send_mutex);
        ws_send(sock, welcome.dump());
    }

    // Register client in hub
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        clients_.push_back(client);
    }

    // ── Read loop: drain frames until disconnect ──────────────────────────────
    while (client->alive && running_) {
        uint8_t hdr[2];
        int n = recv_exact(sock, hdr, 2);
        if (n == 0) break;          // graceful close
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;  // normal 1-sec wakeup
            break;                  // real socket error
        }

        uint8_t  opcode = hdr[0] & 0x0f;
        bool     masked = (hdr[1] & 0x80) != 0;
        uint64_t plen   = hdr[1] & 0x7f;

        if (plen == 126) {
            uint8_t ext[2];
            if (recv_exact(sock, ext, 2) <= 0) break;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8];
            if (recv_exact(sock, ext, 8) <= 0) break;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
        }

        // Sanity limit: no single frame > 64 KiB expected from browser clients.
        if (plen > 65536) break;

        uint8_t mask[4] = {};
        if (masked) {
            if (recv_exact(sock, mask, 4) <= 0) break;
        }

        if (plen > 0) {
            std::vector<uint8_t> payload((size_t)plen);
            if (recv_exact(sock, payload.data(), (int)plen) <= 0) break;
            // (Payload content ignored — clients only send keep-alive pings)
        }

        if (opcode == 0x8) {        // close frame
            ws_close_frame(sock);
            break;
        }
        // opcode 0x9 = ping — browser keep-alives handled by recv timeout
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    if (client->alive.exchange(false))
        closesocket(sock);

    // Stale entry will be pruned on the next broadcast.
}

// ── HTTP → WebSocket upgrade handshake ───────────────────────────────────────
bool WebSocketHub::perform_handshake(uintptr_t raw_sock, int& out_iid) {
    SOCKET sock = to_sock(raw_sock);
    std::string req = read_http_request(sock);
    if (req.empty()) return false;

    // Must contain "Upgrade: websocket"
    std::string upgrade = req_header(req, "Upgrade");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
    if (upgrade.find("websocket") == std::string::npos) return false;

    std::string ws_key = req_header(req, "Sec-WebSocket-Key");
    if (ws_key.empty()) return false;

    // Parse path → determine subscription type
    std::string path = req_path(req);
    out_iid = -1;  // default: global

    const std::string iid_prefix = "/ws/intersection/";
    if (path.rfind(iid_prefix, 0) == 0) {
        try { out_iid = std::stoi(path.substr(iid_prefix.size())); }
        catch (...) { out_iid = -1; }
    }
    // /ws/updates → stays -1

    // Build RFC-6455 101 Switching Protocols response
    std::string accept = ws_accept_key(ws_key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "\r\n";

    int r = ::send(sock, resp.data(), (int)resp.size(), 0);
    return r == (int)resp.size();
}

// ── Broadcast ─────────────────────────────────────────────────────────────────
void WebSocketHub::broadcast_all(const std::string& msg) {
    broadcast_impl(-1, msg);
}

void WebSocketHub::adopt_socket(uintptr_t raw_sock) {
    // Run the same per-client handler on a detached thread so the router
    // can return immediately to accept the next connection.
    running_ = true; // safe if start() hasn't been called yet
    std::thread(&WebSocketHub::handle_client, this, raw_sock).detach();
}

void WebSocketHub::broadcast_intersection(int iid, const std::string& msg) {
    broadcast_impl(iid, msg);
}

void WebSocketHub::broadcast_impl(int filter_iid, const std::string& msg) {
    // Snapshot the client list under lock (brief), then send outside lock.
    std::vector<std::shared_ptr<WSClient>> snapshot;
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        // Prune dead entries while we have the lock.
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [](const auto& c){ return !c->alive; }),
            clients_.end());
        snapshot = clients_;
    }

    for (auto& c : snapshot) {
        if (!c->alive) continue;
        // filter_iid < 0  → broadcast_all  → send to everyone
        // filter_iid >= 0 → per-intersection → matching subscribers AND
        //                   global ("/ws/updates", intersection_id == -1)
        //                   subscribers, which mirrors the Python LiveUpdateHub.
        if (filter_iid >= 0 &&
            c->intersection_id != filter_iid &&
            c->intersection_id != -1) continue;

        std::lock_guard<std::mutex> lk(c->send_mutex);
        if (!ws_send(to_sock(c->sock), msg)) {
            if (c->alive.exchange(false))
                closesocket(to_sock(c->sock));
        }
    }
}
