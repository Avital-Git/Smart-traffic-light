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

    // Expose the hub so an external Router can hand off WS upgrade sockets.
    WebSocketHub* hub() { return hub_.get(); }

    // When set, the HTTP server binds to 127.0.0.1:<internal_port> instead
    // of 0.0.0.0:<port_>, and the WebSocketHub's own TCP listener is
    // disabled. An external Router is then expected to accept connections
    // on the public port and proxy / hand-off accordingly.
    void enable_router_mode(int internal_http_port);

private:
    void register_routes();
    void load_neighbor_topology();
    void load_intersection_locations(); // טוענת קואורדינטות GPS של כל הצמתות מה-DB — נקראת בקונסטרקטור, משמשת ל-POST /emergency/locate
    void load_emergency_auth_config();
    bool validate_emergency_signal(const nlohmann::json& state_body, std::string& reason_out);
    nlohmann::json build_neighbor_summaries(int intersection_id);

    // Compute a greedy phase-selection action with aging from a state body.
    // Used as the server-side fallback when no controller has reported.
    nlohmann::json decide_action_fallback(int intersection_id, const nlohmann::json& state_body);

    // Latch / expiry helpers (mirror Python _latch_emergency_signal etc.).
    void          latch_emergency(int intersection_id, const nlohmann::json& signal);
    void          clear_emergency_latch(int intersection_id);
    nlohmann::json active_emergency_signal(int intersection_id); // null if expired/none

    // Broadcast helpers — fan out to /ws/intersection/{id} AND /ws/updates.
    void broadcast_event(int intersection_id, const std::string& event_name,
                         const nlohmann::json& payload);

    // Returns cached DB lane schema for an intersection (fetched once, then cached).
    // Returns empty vector if DB is unreachable or has no lanes for that ID.
    const std::vector<LaneRow>& get_cached_lane_schema(int intersection_id);

    // Drop cached lane schema for an intersection so the next request reloads
    // from DB. Used after admin CRUD on dbo.intersection_lanes.
    void invalidate_lane_cache(int intersection_id);

    int    port_;
    int    ws_port_;
    bool   router_mode_{false};
    int    internal_http_port_{0};
    std::string http_bind_host_{"0.0.0.0"};
    httplib::Server*              svr_;   // raw ptr — keeps httplib.h out of header
    std::unique_ptr<WebSocketHub> hub_;   // WebSocket hub on ws_port_

    // In-memory stores (mirrors Python state_store / action_store).
    std::mutex                           store_mutex_;
    std::unordered_map<int, std::string> state_store_;
    std::unordered_map<int, std::string> action_store_;

    // Action source + recency, used for stickiness (mirrors Python).
    //   "cpp_controller"     — sticky for CPP_ACTION_STICKY_SEC
    //   "manual_dashboard"   — sticky for MANUAL_ACTION_STICKY_SEC
    //   "emergency_preempt"  — overrides everything while emergency active
    //   "server_fallback"    — replaceable any time
    std::unordered_map<int, std::string> action_source_store_;
    std::unordered_map<int, double>      action_updated_at_store_;

    // Emergency latch (mirrors Python _emergency_latch_by_intersection).
    std::mutex                           emergency_latch_mutex_;
    std::unordered_map<int, std::string> emergency_latch_;          // signal JSON
    std::unordered_map<int, double>      emergency_latch_expiry_;

    std::unordered_map<int, std::vector<int>>       neighbor_topology_;
    std::unordered_map<int, std::vector<NeighborRow>> neighbor_topology_full_;

    // מטמון מיקומי GPS: intersection_id → {latitude, longitude}
    // נטענת פעם אחת בהפעלה, משמשת ל-POST /emergency/locate לחישוב Haversine
    std::unordered_map<int, std::pair<double, double>> intersection_locations_; // מפה מ-id צומת לזוג קואורדינטות (lat,lon)

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
