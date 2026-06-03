#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Keep winsock out of this header — included only in the .cpp.
// We use a SOCKET-compatible alias (uintptr_t on both 32 and 64-bit Windows).
#include <cstdint>

struct WSClient {
    uintptr_t         sock;           // SOCKET value
    int               intersection_id; // -1 = global (/ws/updates)
    std::atomic<bool> alive;
    std::mutex        send_mutex;     // serialise sends to this socket

    WSClient(uintptr_t s, int iid) : sock(s), intersection_id(iid), alive(true) {}
};

// ---------------------------------------------------------------------------
// WebSocketHub
//
// Listens on a dedicated TCP port.  Performs the RFC-6455 HTTP-upgrade
// handshake, then keeps each connection alive in a detached thread.
//
// Routes supported:
//   /ws/updates            → global subscriber  (intersection_id = -1)
//   /ws/intersection/{id}  → per-intersection subscriber
//
// Thread-safety: all public methods are safe to call from any thread.
// ---------------------------------------------------------------------------
class WebSocketHub {
public:
    explicit WebSocketHub(int port = 9001);
    ~WebSocketHub();

    void start();   // launches background accept thread
    void stop();    // closes all connections; safe to call multiple times

    // Broadcast a JSON message to ALL connected clients.
    void broadcast_all(const std::string& json_msg);

    // Broadcast only to clients subscribed to a specific intersection.
    void broadcast_intersection(int intersection_id, const std::string& json_msg);

    int  port() const { return port_; }

private:
    int               port_;
    uintptr_t         listen_sock_{~uintptr_t(0)}; // INVALID_SOCKET
    std::atomic<bool> running_{false};
    std::thread       accept_thread_;

    std::mutex                              clients_mutex_;
    std::vector<std::shared_ptr<WSClient>> clients_;

    void accept_loop();
    void handle_client(uintptr_t sock);
    bool perform_handshake(uintptr_t sock, int& out_intersection_id);
    void broadcast_impl(int filter_iid, const std::string& msg);
};
