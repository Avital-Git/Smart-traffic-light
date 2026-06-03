#include "TrafficServer.h"
#include "Database.h"
#include "JwtAuth.h"
#include "WebSocketHub.h"

// httplib is header-only — include the full header only in the .cpp
// (CPPHTTPLIB_NO_EXCEPTIONS / CPPHTTPLIB_OPENSSL_SUPPORT are set via CMake target_compile_definitions)
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

using json = nlohmann::json;

#pragma comment(lib, "bcrypt.lib")

namespace {

const std::unordered_map<int, std::vector<int>> DEFAULT_NEIGHBOR_TOPOLOGY = {
    {1, {2}},
    {2, {1, 3}},
    {3, {2, 4}},
    {4, {3}},
};

struct EmergencyAuthConfig {
    std::unordered_map<std::string, std::string> vehicle_keys;
    double max_clock_skew_sec = 30.0;
    std::string source = "defaults";
};

std::string env_or(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return (v && v[0]) ? std::string(v) : std::string(fallback);
}

std::optional<std::unordered_map<int, std::vector<int>>> load_neighbor_topology_from_json_file(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }

    try {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in.is_open()) {
            return std::nullopt;
        }

        json payload;
        in >> payload;
        if (!payload.is_object() || !payload.contains("intersections") || !payload["intersections"].is_object()) {
            return std::nullopt;
        }

        std::unordered_map<int, std::vector<int>> topology;
        for (auto it = payload["intersections"].begin(); it != payload["intersections"].end(); ++it) {
            int intersection_id = 0;
            try {
                intersection_id = std::stoi(it.key());
            } catch (...) {
                continue;
            }

            if (!it.value().is_object() || !it.value().contains("neighbors") || !it.value()["neighbors"].is_array()) {
                topology[intersection_id] = {};
                continue;
            }

            std::vector<int> neighbors;
            for (const auto& n : it.value()["neighbors"]) {
                try {
                    neighbors.push_back(n.get<int>());
                } catch (...) {
                    continue;
                }
            }
            topology[intersection_id] = neighbors;
        }

        if (!topology.empty()) {
            return topology;
        }
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

std::vector<unsigned char> hmac_sha256_bytes(const std::string& key, const std::string& input)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_len = 0;
    DWORD data_len = 0;
    DWORD hash_len = 0;
    std::vector<unsigned char> hash_object;
    std::vector<unsigned char> hash_value;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    hash_object.resize(object_len);
    hash_value.resize(hash_len);

    if (BCryptCreateHash(
            alg,
            &hash,
            hash_object.data(),
            object_len,
            reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
            static_cast<ULONG>(key.size()),
            0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptFinishHash(hash, hash_value.data(), hash_len, 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash_value;
}

std::string to_hex(const std::vector<unsigned char>& bytes)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char value : bytes) {
        out << std::setw(2) << static_cast<int>(value);
    }
    return out.str();
}

std::string to_lower_copy(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::optional<EmergencyAuthConfig> load_emergency_auth_from_json_file(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }

    try {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in.is_open()) {
            return std::nullopt;
        }

        json payload;
        in >> payload;
        if (!payload.is_object() || !payload.contains("vehicle_keys") || !payload["vehicle_keys"].is_object()) {
            return std::nullopt;
        }

        EmergencyAuthConfig cfg;
        cfg.source = path.string();

        for (auto it = payload["vehicle_keys"].begin(); it != payload["vehicle_keys"].end(); ++it) {
            if (!it.value().is_string()) {
                continue;
            }
            const std::string key_name = it.key();
            const std::string key_value = it.value().get<std::string>();
            if (!key_name.empty() && !key_value.empty()) {
                cfg.vehicle_keys[key_name] = key_value;
            }
        }

        if (payload.contains("max_clock_skew_sec") && payload["max_clock_skew_sec"].is_number()) {
            cfg.max_clock_skew_sec = std::max(1.0, payload["max_clock_skew_sec"].get<double>());
        }

        if (!cfg.vehicle_keys.empty()) {
            return cfg;
        }
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

TrafficServer::TrafficServer(int port, int ws_port)
    : port_(port), ws_port_(ws_port),
      svr_(new httplib::Server()),
      hub_(new WebSocketHub(ws_port))
{
    load_neighbor_topology();
    load_emergency_auth_config();
    register_routes();
}

TrafficServer::~TrafficServer()
{
    delete svr_;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TrafficServer::run()
{
    hub_->start();
    std::cout << "[TrafficServer] HTTP  listening on http://0.0.0.0:" << port_ << "\n";
    std::cout << "[TrafficServer] WS    listening on ws://0.0.0.0:"   << ws_port_ << "\n";
    svr_->listen("0.0.0.0", port_);
    hub_->stop();
}

void TrafficServer::stop()
{
    svr_->stop();
    // hub_->stop() is called by run() once svr_->listen() returns.
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void TrafficServer::register_routes()
{
    auto apply_cors = [](httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    };

    // Ensure CORS headers are present on normal routed responses.
    svr_->set_post_routing_handler([apply_cors](const httplib::Request& /*req*/, httplib::Response& res) {
        apply_cors(res);
    });

    svr_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        // Handle CORS preflight globally.
        if (req.method == "OPTIONS") {
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }

        if (req.path.rfind("/admin/", 0) != 0) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        if (req.path == "/admin/login") {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));
        if (!token) {
            res.status = 401;
            res.set_content(json({{"detail", "Missing authentication credentials"}}).dump(), "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        std::string username;
        if (!jwt_auth::validate_admin_token(*token, username)) {
            res.status = 401;
            res.set_content(json({{"detail", "Invalid token"}}).dump(), "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ── GET /health ─────────────────────────────────────────────────────────
    svr_->Get("/health", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // ── GET /intersections ───────────────────────────────────────────────────
    // Queries dbo.intersections via ODBC; falls back to hardcoded defaults.
    svr_->Get("/intersections", [](const httplib::Request& /*req*/, httplib::Response& res) {
        auto rows = db_fetch_intersections();

        json arr = json::array();
        for (const auto& r : rows) {
            arr.push_back({
                {"id",   r.id},
                {"code", r.code},
                {"name", r.name},
                {"city", r.city},
            });
        }
        res.set_content(arr.dump(), "application/json");
    });

    // ── POST /admin/login ───────────────────────────────────────────────────
    svr_->Post("/admin/login", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }

        const std::string username = body.value("username", "");
        const std::string password = body.value("password", "");
        if (!jwt_auth::verify_admin_password(username, password)) {
            res.status = 401;
            res.set_content(json({{"detail", "Invalid credentials"}}).dump(), "application/json");
            return;
        }

        const auto token = jwt_auth::create_admin_token(username);
        res.set_content(json({
            {"access_token", token.access_token},
            {"token_type", token.token_type},
            {"expires_in", token.expires_in},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersections ────────────────────────────────────────────
    svr_->Get("/admin/intersections", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        auto rows = db_fetch_intersections();
        json items = json::array();

        std::lock_guard<std::mutex> lk(store_mutex_);
        for (const auto& row : rows) {
            auto state_it = state_store_.find(row.id);
            auto action_it = action_store_.find(row.id);

            items.push_back({
                {"intersection", {
                    {"id", row.id},
                    {"code", row.code},
                    {"name", row.name},
                    {"city", row.city},
                }},
                {"current_state", state_it != state_store_.end() ? json::parse(state_it->second) : json(nullptr)},
                {"current_action", action_it != action_store_.end() ? json::parse(action_it->second) : json(nullptr)},
            });
        }

        res.set_content(json({
            {"status", "success"},
            {"count", static_cast<int>(items.size())},
            {"intersections", items},
        }).dump(), "application/json");
    });

    auto create_admin_intersection = [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }

        const std::string code = body.value("code", "");
        const std::string name = body.value("name", "");
        if (code.empty() || name.empty() || !body.contains("latitude") || !body.contains("longitude")) {
            res.status = 422;
            res.set_content(json({{"detail", "code, name, latitude, longitude are required"}}).dump(), "application/json");
            return;
        }

        for (const auto& existing : db_fetch_intersections()) {
            if (existing.code == code) {
                res.status = 409;
                res.set_content(json({{"detail", "Intersection code already exists"}}).dump(), "application/json");
                return;
            }
        }

        std::string error;
        const bool ok = db_insert_intersection(
            code,
            name,
            body["latitude"].get<double>(),
            body["longitude"].get<double>(),
            body.value("num_cameras", 4),
            body.contains("city") && !body["city"].is_null() ? std::optional<std::string>(body["city"].get<std::string>()) : std::nullopt,
            body.contains("region") && !body["region"].is_null() ? std::optional<std::string>(body["region"].get<std::string>()) : std::nullopt,
            body.contains("description") && !body["description"].is_null() ? std::optional<std::string>(body["description"].get<std::string>()) : std::nullopt,
            error);
        if (!ok) {
            res.status = 400;
            res.set_content(json({{"detail", error}}).dump(), "application/json");
            return;
        }

        res.set_content(json({
            {"status", "success"},
            {"message", "Intersection created successfully"},
            {"data", body},
        }).dump(), "application/json");
    };
    svr_->Post("/admin/intersection", create_admin_intersection);
    svr_->Post("/admin/intersection/create", create_admin_intersection);

    // ── PUT /admin/intersection/{id} ────────────────────────────────────────
    svr_->Put(R"(/admin/intersection/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }

        const int id = std::stoi(req.matches[1]);
        const auto name = body.contains("name") && !body["name"].is_null() ? std::optional<std::string>(body["name"].get<std::string>()) : std::nullopt;
        const auto num_cameras = body.contains("num_cameras") && !body["num_cameras"].is_null() ? std::optional<int>(body["num_cameras"].get<int>()) : std::nullopt;
        const auto city = body.contains("city") && !body["city"].is_null() ? std::optional<std::string>(body["city"].get<std::string>()) : std::nullopt;
        const auto region = body.contains("region") && !body["region"].is_null() ? std::optional<std::string>(body["region"].get<std::string>()) : std::nullopt;
        const auto description = body.contains("description") && !body["description"].is_null() ? std::optional<std::string>(body["description"].get<std::string>()) : std::nullopt;

        bool found = false;
        std::string error;
        const bool ok = db_update_intersection(id, name, num_cameras, city, region, description, found, error);
        if (!ok && error == "No updatable fields provided") {
            res.status = 400;
            res.set_content(json({{"detail", error}}).dump(), "application/json");
            return;
        }
        if (!found) {
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");
            return;
        }
        if (!ok) {
            res.status = 400;
            res.set_content(json({{"detail", error}}).dump(), "application/json");
            return;
        }

        json data = json::object();
        if (name) data["name"] = *name;
        if (num_cameras) data["num_cameras"] = *num_cameras;
        if (city) data["city"] = *city;
        if (region) data["region"] = *region;
        if (description) data["description"] = *description;

        res.set_content(json({
            {"status", "success"},
            {"message", "Intersection updated successfully"},
            {"data", data},
        }).dump(), "application/json");
    });

    // ── POST /state ──────────────────────────────────────────────────────────
    // Receives IntersectionState JSON from Python vision module.
    // Stores state, then returns the last RL-selected action (posted by
    // smart_traffic_controller via POST /intersection/{id}/action).
    // Falls back to Phase0 only if no RL action has been received yet.
    svr_->Post("/state", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const json::exception& e) {
            res.status = 400;
            res.set_content(
                json({{"detail", std::string("Invalid JSON: ") + e.what()}}).dump(),
                "application/json");
            return;
        }

        if (!body.contains("intersection_id") || !body["intersection_id"].is_number_integer()) {
            res.status = 422;
            res.set_content(
                json({{"detail", "intersection_id is required and must be an integer"}}).dump(),
                "application/json");
            return;
        }

        int id = body["intersection_id"].get<int>();

        std::string emergency_validation_reason;
        if (!validate_emergency_signal(body, emergency_validation_reason)) {
            res.status = 401;
            res.set_content(
                json({{"detail", std::string("Invalid emergency signal: ") + emergency_validation_reason}}).dump(),
                "application/json");
            return;
        }

        // ── Normalize lane count to DB schema ─────────────────────────────────
        // DB is the single source of truth for lane count.
        // Pad with zeros if live state has fewer lanes; truncate if more.
        {
            const auto& schema = get_cached_lane_schema(id);
            if (!schema.empty()) {
                const int db_lane_count = static_cast<int>(schema.size());

                // Build lookup of incoming lanes by lane_id for fast access.
                std::unordered_map<int, json> live_by_id;
                if (body.contains("lanes") && body["lanes"].is_array()) {
                    for (const auto& lane : body["lanes"]) {
                        if (lane.contains("lane_id") && lane["lane_id"].is_number_integer()) {
                            live_by_id[lane["lane_id"].get<int>()] = lane;
                        }
                    }
                }

                // Rebuild lanes array exactly from DB schema order/count.
                json normalized_lanes = json::array();
                for (const auto& row : schema) {
                    auto it = live_by_id.find(row.lane_id);
                    if (it != live_by_id.end()) {
                        // Use live values but ensure lane_id and direction are canonical.
                        json lane = it->second;
                        lane["lane_id"] = row.lane_id;
                        if (lane["direction"].is_null() || !lane["direction"].is_string() ||
                                lane["direction"].get<std::string>().empty()) {
                            lane["direction"] = row.direction;
                        }
                        normalized_lanes.push_back(std::move(lane));
                    } else {
                        // Lane missing from live data — pad with zeros.
                        json zero_lane = {
                            {"lane_id",          row.lane_id},
                            {"direction",        row.direction},
                            {"vehicle_count",    0},
                            {"queue_length",     0},
                            {"density_pct",      0.0},
                            {"waiting_time_sec", 0.0},
                            {"pedestrian_count", 0},
                        };
                        normalized_lanes.push_back(std::move(zero_lane));
                    }
                }

                body["lanes"]     = std::move(normalized_lanes);
                body["num_lanes"] = db_lane_count;
            }
        }
        // ─────────────────────────────────────────────────────────────────────

        // Retrieve the last RL action (written by smart_traffic_controller).
        // Do NOT overwrite it — the RL controller owns action_store_.
        json action;
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            state_store_[id] = body.dump();

            auto ai = action_store_.find(id);
            if (ai != action_store_.end()) {
                try {
                    action = json::parse(ai->second);
                } catch (...) {
                    action = nullptr;
                }
            }
        }

        // Fall back to Phase0 only when no RL action is available yet.
        if (action.is_null() || !action.contains("action")) {
            action = {
                {"action",          "Phase0"},
                {"reason",          "server_fallback_no_rl"},
                {"intersection_id", id},
                {"phase_id",        0},
            };
        }

        // Broadcast state_updated (mirrors Python LIVE_UPDATES.broadcast)
        double ts = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json evt = {
            {"event",           "state_updated"},
            {"intersection_id", id},
            {"timestamp",       ts},
            {"payload",         {{"state", body}, {"action", action}}},
        };
        hub_->broadcast_intersection(id, evt.dump());

        res.set_content(action.dump(), "application/json");
    });

    // ── GET /intersection/{id}/packet ────────────────────────────────────────
    // Returns the last stored state + action for an intersection.
    svr_->Get(R"(/intersection/(\d+)/packet)",
        [this](const httplib::Request& req, httplib::Response& res) {

        int id = std::stoi(req.matches[1]);

        std::string state_str, action_str;
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            auto si = state_store_.find(id);
            auto ai = action_store_.find(id);
            if (si == state_store_.end()) {
                res.status = 404;
                res.set_content(
                    json({{"detail", "Intersection state not found"}}).dump(),
                    "application/json");
                return;
            }
            state_str  = si->second;
            action_str = (ai != action_store_.end()) ? ai->second : "null";
        }

        double ts = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        json packet = {
            {"timestamp",   ts},
            {"state",       json::parse(state_str)},
            {"last_action", json::parse(action_str)},
            {"neighbors",   build_neighbor_summaries(id)},
        };

        res.set_content(packet.dump(), "application/json");
    });

    // ── GET /intersection/{id}/action ────────────────────────────────────────
    // Returns the last recorded action for an intersection.
    svr_->Get(R"(/intersection/(\d+)/action)",
        [this](const httplib::Request& req, httplib::Response& res) {

        int id = std::stoi(req.matches[1]);
        std::lock_guard<std::mutex> lk(store_mutex_);

        auto si = state_store_.find(id);
        if (si == state_store_.end()) {
            res.status = 404;
            res.set_content(
                json({{"detail", "Intersection state not found"}}).dump(),
                "application/json");
            return;
        }

        auto ai = action_store_.find(id);
        if (ai != action_store_.end()) {
            res.set_content(ai->second, "application/json");
        } else {
            // No action yet — return a Phase0 fallback
            json fallback = {
                {"action",         "Phase0"},
                {"reason",         "server_fallback"},
                {"intersection_id", id},
                {"phase_id",       0},
            };
            res.set_content(fallback.dump(), "application/json");
        }
    });

    // ── POST /intersection/{id}/action ───────────────────────────────────────
    // C++ controller (or admin dashboard) reports the chosen action.
    // Body: { "action": "Phase0", "phase_id": 0, "reason": "..." }
    svr_->Post(R"(/intersection/(\d+)/action)",
        [this](const httplib::Request& req, httplib::Response& res) {

        int id = std::stoi(req.matches[1]);

        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }

        if (!body.contains("action") || !body["action"].is_string()) {
            res.status = 422;
            res.set_content(json({{"detail", "action field is required"}}).dump(), "application/json");
            return;
        }

        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            if (state_store_.find(id) == state_store_.end()) {
                res.status = 404;
                res.set_content(
                    json({{"detail", "Intersection state not found"}}).dump(),
                    "application/json");
                return;
            }
        }

        int phase_id = body.value("phase_id", 0);
        std::string action_str = body["action"].get<std::string>();
        std::string reason     = body.value("reason", "selected_by_cpp_controller");

        json action = {
            {"action",         action_str},
            {"reason",         reason},
            {"intersection_id", id},
            {"phase_id",       phase_id},
        };

        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            action_store_[id] = action.dump();
        }

        // Broadcast action_updated (mirrors Python LIVE_UPDATES.broadcast)
        double ts2 = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json evt = {
            {"event",           "action_updated"},
            {"intersection_id", id},
            {"timestamp",       ts2},
            {"payload",         {{"action", action}, {"source", reason}}},
        };
        hub_->broadcast_intersection(id, evt.dump());

        res.set_content(action.dump(), "application/json");
    });

    // ── GET /intersection/{id}/lanes ─────────────────────────────────────────
    // Reads lane definitions from dbo.intersection_lanes.
    svr_->Get(R"(/intersection/(\d+)/lanes)",
        [](const httplib::Request& req, httplib::Response& res) {

        int id = std::stoi(req.matches[1]);
        auto rows = db_fetch_lanes(id);

        json arr = json::array();
        for (const auto& r : rows) {
            json lane = {
                {"lane_id",      r.lane_id},
                {"camera_index", r.camera_index},
                {"direction",    r.direction},
            };
            if (!r.description.empty())
                lane["description"] = r.description;
            else
                lane["description"] = nullptr;
            arr.push_back(lane);
        }
        res.set_content(arr.dump(), "application/json");
    });

    // ── GET /intersection/{id}/conflicts ─────────────────────────────────────
    // Reads conflict pairs from dbo.lane_conflicts.
    // Response shape matches the Python server exactly.
    svr_->Get(R"(/intersection/(\d+)/conflicts)",
        [](const httplib::Request& req, httplib::Response& res) {

        int id = std::stoi(req.matches[1]);
        auto rows = db_fetch_conflicts(id);

        json pair_list = json::array();
        json conflict_rows = json::array();
        for (const auto& r : rows) {
            pair_list.push_back({r.lane_id_1, r.lane_id_2});
            json row = {
                {"conflict_id",   r.conflict_id},
                {"lane_id_1",     r.lane_id_1},
                {"lane_id_2",     r.lane_id_2},
                {"conflict_type", r.conflict_type},
            };
            if (!r.created_at.empty())
                row["created_at"] = r.created_at;
            else
                row["created_at"] = nullptr;
            conflict_rows.push_back(row);
        }

        json response = {
            {"status",         "success"},
            {"intersection_id", id},
            {"count",          (int)rows.size()},
            {"conflicts",      pair_list},
            {"conflict_rows",  conflict_rows},
        };
        res.set_content(response.dump(), "application/json");
    });
}

// ── get_cached_lane_schema ────────────────────────────────────────────────────
// Fetches lane schema from dbo.intersection_lanes on first call per intersection
// and caches it for the lifetime of the server.  Thread-safe.
const std::vector<LaneRow>& TrafficServer::get_cached_lane_schema(int intersection_id)
{
    std::lock_guard<std::mutex> lk(lane_cache_mutex_);

    // Return cached copy if already loaded (even if empty — DB may have no rows).
    if (lane_cache_loaded_.count(intersection_id)) {
        return lane_schema_cache_[intersection_id];
    }

    // First call for this intersection: query the DB.
    lane_schema_cache_[intersection_id] = db_fetch_lanes(intersection_id);
    lane_cache_loaded_[intersection_id] = true;

    const auto& schema = lane_schema_cache_[intersection_id];
    std::cout << "[LaneCache] intersection_id=" << intersection_id
              << " loaded " << schema.size() << " lane(s) from DB.\n";

    return lane_schema_cache_[intersection_id];
}

void TrafficServer::load_neighbor_topology()
{
    auto db_topology = db_fetch_neighbor_topology();
    if (!db_topology.empty()) {
        neighbor_topology_ = std::move(db_topology);
        std::cout << "[TrafficServer] Loaded neighbor topology from DB ("
                  << neighbor_topology_.size() << " intersections)\n";
        return;
    }

    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path("python/server/neighbor_topology.json"),
        std::filesystem::path("../python/server/neighbor_topology.json"),
        std::filesystem::path("../../python/server/neighbor_topology.json"),
        std::filesystem::path("neighbor_topology.json"),
    };

    for (const auto& path : candidates) {
        auto loaded = load_neighbor_topology_from_json_file(path);
        if (loaded.has_value() && !loaded->empty()) {
            neighbor_topology_ = std::move(*loaded);
            std::cout << "[TrafficServer] Loaded neighbor topology from " << path.string() << "\n";
            return;
        }
    }

    neighbor_topology_ = DEFAULT_NEIGHBOR_TOPOLOGY;
    std::cout << "[TrafficServer] Loaded fallback neighbor topology defaults\n";
}

void TrafficServer::load_emergency_auth_config()
{
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path("python/server/emergency_keys.json"),
        std::filesystem::path("../python/server/emergency_keys.json"),
        std::filesystem::path("../../python/server/emergency_keys.json"),
        std::filesystem::path("emergency_keys.json"),
    };

    for (const auto& path : candidates) {
        const auto loaded = load_emergency_auth_from_json_file(path);
        if (!loaded.has_value()) {
            continue;
        }

        emergency_keys_ = loaded->vehicle_keys;
        emergency_max_clock_skew_sec_ = loaded->max_clock_skew_sec;
        std::cout << "[TrafficServer] Loaded emergency auth from " << loaded->source
                  << " (keys=" << emergency_keys_.size()
                  << ", skew=" << emergency_max_clock_skew_sec_ << "s)\n";
        return;
    }

    emergency_keys_.clear();
    emergency_keys_["AMB001"] = "demo-emergency-key-001";
    emergency_keys_["POL001"] = "demo-emergency-key-002";
    emergency_max_clock_skew_sec_ = 30.0;
    std::cout << "[TrafficServer] Loaded fallback emergency auth defaults\n";
}

bool TrafficServer::validate_emergency_signal(const json& state_body, std::string& reason_out)
{
    reason_out = "ok";

    if (!state_body.contains("emergency_signal") || state_body["emergency_signal"].is_null()) {
        return true;
    }
    if (!state_body["emergency_signal"].is_object()) {
        reason_out = "invalid_emergency_object";
        return false;
    }

    const json& signal = state_body["emergency_signal"];
    const bool active = signal.value("active", false);
    if (!active) {
        return true;
    }

    if (!signal.contains("lane_id") || !signal["lane_id"].is_number_integer()) {
        reason_out = "invalid_lane_id";
        return false;
    }
    const int lane_id = signal["lane_id"].get<int>();
    if (lane_id < 0) {
        reason_out = "invalid_lane_id";
        return false;
    }

    if (!signal.contains("vehicle_id") || !signal["vehicle_id"].is_string()) {
        reason_out = "missing_vehicle_id";
        return false;
    }
    const std::string vehicle_id = signal["vehicle_id"].get<std::string>();
    if (vehicle_id.empty()) {
        reason_out = "missing_vehicle_id";
        return false;
    }

    if (!signal.contains("timestamp") || !signal["timestamp"].is_number()) {
        reason_out = "missing_timestamp";
        return false;
    }
    const double timestamp = signal["timestamp"].get<double>();

    if (!signal.contains("signature") || !signal["signature"].is_string()) {
        reason_out = "missing_signature";
        return false;
    }
    const std::string signature = signal["signature"].get<std::string>();
    if (signature.empty()) {
        reason_out = "missing_signature";
        return false;
    }

    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (std::fabs(now - timestamp) > emergency_max_clock_skew_sec_) {
        reason_out = "timestamp_out_of_window";
        return false;
    }

    std::string secret;
    {
        std::lock_guard<std::mutex> lk(emergency_mutex_);
        auto vehicle_it = emergency_keys_.find(vehicle_id);
        if (vehicle_it != emergency_keys_.end()) {
            secret = vehicle_it->second;
        } else {
            auto wildcard_it = emergency_keys_.find("*");
            if (wildcard_it != emergency_keys_.end()) {
                secret = wildcard_it->second;
            }
        }

        if (secret.empty()) {
            reason_out = "unknown_vehicle_id";
            return false;
        }

        auto replay_it = emergency_last_timestamp_by_vehicle_.find(vehicle_id);
        if (replay_it != emergency_last_timestamp_by_vehicle_.end() && timestamp <= replay_it->second) {
            reason_out = "replay_detected";
            return false;
        }
    }

    std::ostringstream payload;
    payload << vehicle_id << "|" << lane_id << "|"
            << std::fixed << std::setprecision(3) << timestamp;

    const std::string expected_signature = to_hex(hmac_sha256_bytes(secret, payload.str()));
    if (expected_signature.empty()) {
        reason_out = "signature_verification_failed";
        return false;
    }

    if (to_lower_copy(expected_signature) != to_lower_copy(signature)) {
        reason_out = "invalid_signature";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(emergency_mutex_);
        emergency_last_timestamp_by_vehicle_[vehicle_id] = timestamp;
    }

    return true;
}

json TrafficServer::build_neighbor_summaries(int intersection_id)
{
    json summaries = json::array();

    const auto topo_it = neighbor_topology_.find(intersection_id);
    if (topo_it == neighbor_topology_.end() || topo_it->second.empty()) {
        return summaries;
    }

    const std::string neighbor_key = env_or("NEIGHBOR_MESSAGE_KEY", "demo-neighbor-message-key");

    for (int neighbor_id : topo_it->second) {
        std::string neighbor_state_str;
        std::string neighbor_action_str;
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            auto si = state_store_.find(neighbor_id);
            if (si == state_store_.end()) {
                continue;
            }
            neighbor_state_str = si->second;

            auto ai = action_store_.find(neighbor_id);
            if (ai != action_store_.end()) {
                neighbor_action_str = ai->second;
            }
        }

        json neighbor_state;
        try {
            neighbor_state = json::parse(neighbor_state_str);
        } catch (...) {
            continue;
        }

        json neighbor_action;
        if (!neighbor_action_str.empty()) {
            try {
                neighbor_action = json::parse(neighbor_action_str);
            } catch (...) {
                neighbor_action = json::object();
            }
        } else {
            neighbor_action = {
                {"action", "Phase0"},
                {"phase_id", 0},
            };
        }

        int total_queue = 0;
        double waiting_sum = 0.0;
        int lane_count = 0;

        if (neighbor_state.contains("lanes") && neighbor_state["lanes"].is_array()) {
            for (const auto& lane : neighbor_state["lanes"]) {
                if (!lane.is_object()) continue;

                int vehicle_count = 0;
                if (lane.contains("vehicle_count") && lane["vehicle_count"].is_number()) {
                    vehicle_count = std::max(0, static_cast<int>(lane["vehicle_count"].get<double>()));
                }
                total_queue += vehicle_count;

                double waiting = 0.0;
                if (lane.contains("waiting_time_sec") && lane["waiting_time_sec"].is_number()) {
                    waiting = std::max(0.0, lane["waiting_time_sec"].get<double>());
                }
                waiting_sum += waiting;
                ++lane_count;
            }
        }

        const double avg_waiting_sec = lane_count > 0 ? (waiting_sum / lane_count) : 0.0;
        const bool emergency_active =
            neighbor_state.contains("emergency_signal") &&
            neighbor_state["emergency_signal"].is_object() &&
            neighbor_state["emergency_signal"].value("active", false);

        int phase_id = -1;
        if (neighbor_action.contains("phase_id") && neighbor_action["phase_id"].is_number_integer()) {
            phase_id = neighbor_action["phase_id"].get<int>();
        }

        const double signed_at = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::ostringstream payload;
        payload << neighbor_id << "|"
                << phase_id << "|"
                << total_queue << "|"
                << std::fixed << std::setprecision(3) << avg_waiting_sec << "|"
                << (emergency_active ? 1 : 0) << "|"
                << std::fixed << std::setprecision(3) << signed_at;

        const std::string signature = to_hex(hmac_sha256_bytes(neighbor_key, payload.str()));

        json summary = {
            {"intersection_id", neighbor_id},
            {"action", neighbor_action.value("action", "Phase0")},
            {"phase_id", phase_id >= 0 ? json(phase_id) : json(nullptr)},
            {"total_queue", total_queue},
            {"avg_waiting_sec", avg_waiting_sec},
            {"emergency_active", emergency_active},
            {"signed_at", signed_at},
            {"signature", signature.empty() ? json(nullptr) : json(signature)},
        };
        summaries.push_back(summary);
    }

    return summaries;
}
