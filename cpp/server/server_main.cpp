#include "TrafficServer.h"
#include <iostream>
#include <csignal>
#include <string>

// Global pointer for the signal handler — kept minimal intentionally.
static TrafficServer* g_server = nullptr;

static void handle_signal(int /*sig*/)
{
    std::cout << "\n[TrafficServer] Shutting down...\n";
    if (g_server) g_server->stop();
}

int main(int argc, char* argv[])
{
    int port = 9000;
    int ws_port = 9001;
    if (argc >= 2) { try { port    = std::stoi(argv[1]); } catch (...) {} }
    if (argc >= 3) { try { ws_port = std::stoi(argv[2]); } catch (...) {} }

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    TrafficServer server(port, ws_port);
    g_server = &server;

    server.run();   // blocks until stop() is called
    return 0;
}
