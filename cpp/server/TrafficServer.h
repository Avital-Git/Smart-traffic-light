#pragma once

#include "Database.h"

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declare heavy types to keep this header light.
namespace httplib { class Server; }
class WebSocketHub;

class TrafficServer {
public:
    explicit TrafficServer(int port = 9000, int ws_port = 9001);
    ~TrafficServer();

    void run();   // blocks until stop() is called
    void stop();  // thread-safe

    int port()    const { return port_; }
    int ws_port() const { return ws_port_; }

private:
    void register_routes();
    void load_neighbor_topology();
    void load_emergency_auth_config();
    bool validate_emergency_signal(const nlohmann::json& state_body, std::string& reason_out);
    nlohmann::json build_neighbor_summaries(int intersection_id);

    // Returns cached DB lane schema for an intersection (fetched once, then cached).
    // Returns empty vector if DB is unreachable or has no lanes for that ID.
    const std::vector<LaneRow>& get_cached_lane_schema(int intersection_id);

    int    port_;
    int    ws_port_;
    httplib::Server*              svr_;   // raw ptr — keeps httplib.h out of header
    std::unique_ptr<WebSocketHub> hub_;   // WebSocket hub on ws_port_

    // In-memory stores (mirrors Python state_store / action_store).
    std::mutex                           store_mutex_;
    std::unordered_map<int, std::string> state_store_;
    std::unordered_map<int, std::string> action_store_;

    std::unordered_map<int, std::vector<int>> neighbor_topology_;

    std::mutex emergency_mutex_;
    std::unordered_map<std::string, std::string> emergency_keys_;
    std::unordered_map<std::string, double> emergency_last_timestamp_by_vehicle_;
    double emergency_max_clock_skew_sec_ = 30.0;

    // DB lane schema cache: populated on first POST /state for each intersection.
    // Once loaded the schema never changes during a server session.
    std::mutex                                        lane_cache_mutex_;
    std::unordered_map<int, std::vector<LaneRow>>     lane_schema_cache_;
    std::unordered_map<int, bool>                     lane_cache_loaded_;
};
