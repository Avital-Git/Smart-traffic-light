#include "TrafficServer.h"
#include "Router.h"
#include "WebSocketHub.h"

// winsock2.h must precede windows.h, and WSAStartup must be called once
// process-wide before any socket / hub / router code runs.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

static TrafficServer* g_server = nullptr;
static Router*        g_router = nullptr;

static void handle_signal(int /*sig*/)
{
    std::cout << "\n[TrafficServer] Shutting down...\n";
    if (g_router) g_router->stop();
    if (g_server) g_server->stop();
}

int main(int argc, char* argv[])
{
    // Default to the same port the React client and Python FastAPI use,
    // so the existing client/styling/WebSocket URLs keep working unchanged.
    int public_port   = 8000;
    int internal_port = 18000; // cpp-httplib bound to loopback behind router

    if (argc >= 2) { try { public_port   = std::stoi(argv[1]); } catch (...) {} }
    if (argc >= 3) { try { internal_port = std::stoi(argv[2]); } catch (...) {} }

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[TrafficServer] WSAStartup failed\n";
        return 1;
    }

    TrafficServer server(internal_port, /*ws_port unused in router mode*/ 0);
    server.enable_router_mode(internal_port);
    g_server = &server;

    // Start TrafficServer (HTTP on 127.0.0.1:internal_port) on a worker
    // thread so the Router can bind the public port on the main thread.
    std::thread http_thread([&server]() { server.run(); });

    // Give httplib a brief moment to bind before the router starts proxying.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Router router(public_port, internal_port, server.hub());
    g_router = &router;
    router.start();

    std::cout << "[TrafficServer] Ready. Public endpoint: http://127.0.0.1:"
              << public_port << "  (HTTP + WebSocket)\n";

    http_thread.join();
    router.stop();
    WSACleanup();
    return 0;
}
