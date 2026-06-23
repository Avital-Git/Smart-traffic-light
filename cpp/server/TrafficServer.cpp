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
    if (router_mode_) {
        std::cout << "[TrafficServer] HTTP  listening on http://" << http_bind_host_
                  << ":" << internal_http_port_ << " (router-internal)\n";
        std::cout << "[TrafficServer] WS    served via external Router (adopt mode)\n";
        svr_->listen(http_bind_host_.c_str(), internal_http_port_);
    } else {
        std::cout << "[TrafficServer] HTTP  listening on http://0.0.0.0:" << port_ << "\n";
        std::cout << "[TrafficServer] WS    listening on ws://0.0.0.0:"   << ws_port_ << "\n";
        svr_->listen("0.0.0.0", port_);
    }
    hub_->stop();
}

void TrafficServer::enable_router_mode(int internal_http_port)
{
    router_mode_       = true;
    internal_http_port_ = internal_http_port;
    http_bind_host_    = "127.0.0.1";
    hub_->disable_own_listener();
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

        // Auth-gated routes: every /admin/* (except /admin/login), plus the two
        // emergency simulate/clear endpoints (mirrors Python admin-only access).
        const bool is_admin_path =
            req.path.rfind("/admin/", 0) == 0 && req.path != "/admin/login";
        const bool is_emergency_admin_path =
            (req.path.find("/simulate-emergency") != std::string::npos) ||
            (req.path.find("/clear-emergency")    != std::string::npos);

        if (!is_admin_path && !is_emergency_admin_path) {
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

        std::string user_role = "regular_admin";
        std::string role_error;
        db_get_admin_role(username, user_role, role_error);

        const auto token = jwt_auth::create_admin_token(username, user_role);
        res.set_content(json({
            {"access_token", token.access_token},
            {"token_type", token.token_type},
            {"expires_in", token.expires_in},
            {"role", user_role},
        }).dump(), "application/json");
    });

    auto validate_username = [](const std::string& username) -> std::optional<std::string> {
        if (username.size() < 3) {
            return std::string("username must be at least 3 characters");
        }
        for (char ch : username) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                return std::string("username must not contain spaces");
            }
        }
        return std::nullopt;
    };

    auto validate_password = [](const std::string& password) -> std::optional<std::string> {
        if (password.size() < 8) {
            return std::string("password must be at least 8 characters");
        }
        return std::nullopt;
    };

    auto extract_current_admin_username = [](const httplib::Request& req) -> std::optional<std::string> {
        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));
        if (!token) {
            return std::nullopt;
        }
        std::string username;
        if (!jwt_auth::validate_admin_token(*token, username)) {
            return std::nullopt;
        }
        if (username.empty()) {
            return std::nullopt;
        }
        return username;
    };

    // ── GET /admin/users ────────────────────────────────────────────────────
    svr_->Get("/admin/users", [](const httplib::Request& /*req*/, httplib::Response& res) {
        const auto users = db_list_admin_users();
        json items = json::array();
        for (const auto& user : users) {
            items.push_back({
                {"user_id", user.user_id},
                {"username", user.username},
                {"role", user.role.empty() ? "regular_admin" : user.role},
                {"created_at", user.created_at.empty() ? json(nullptr) : json(user.created_at)},
                {"last_login", user.last_login.empty() ? json(nullptr) : json(user.last_login)},
            });
        }

        res.set_content(json({
            {"status", "success"},
            {"count", static_cast<int>(items.size())},
            {"users", items},
        }).dump(), "application/json");
    });

    // ── POST /admin/users ───────────────────────────────────────────────────
    svr_->Post("/admin/users", [validate_username, validate_password](const httplib::Request& req, httplib::Response& res) {
        // Check if current user is super_admin
        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));
        if (!token) {
            res.status = 401;
            res.set_content(json({{"detail", "Missing authorization token"}}).dump(), "application/json");
            return;
        }
        std::string current_username;
        std::string current_role;
        if (!jwt_auth::validate_admin_token_with_role(*token, current_username, current_role)) {
            res.status = 401;
            res.set_content(json({{"detail", "Invalid authorization token"}}).dump(), "application/json");
            return;
        }
        if (current_role != "super_admin") {
            res.status = 403;
            res.set_content(json({{"detail", "Only super_admin can create users"}}).dump(), "application/json");
            return;
        }

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

        if (auto e = validate_username(username)) {
            res.status = 422;
            res.set_content(json({{"detail", *e}}).dump(), "application/json");
            return;
        }
        if (auto e = validate_password(password)) {
            res.status = 422;
            res.set_content(json({{"detail", *e}}).dump(), "application/json");
            return;
        }
        if (body.contains("confirm_password") && body["confirm_password"].is_string()) {
            if (body["confirm_password"].get<std::string>() != password) {
                res.status = 422;
                res.set_content(json({{"detail", "password and confirm_password do not match"}}).dump(), "application/json");
                return;
            }
        }

        const std::string salt = jwt_auth::generate_salt_hex(16);
        const std::string hash = jwt_auth::hash_password_with_salt(salt, password);

        int new_user_id = 0;
        bool duplicate_username = false;
        std::string error;
        const bool ok = db_insert_admin_user(username, hash, salt, new_user_id, duplicate_username, error);
        if (!ok) {
            if (duplicate_username) {
                res.status = 409;
                res.set_content(json({{"detail", "username already exists"}}).dump(), "application/json");
                return;
            }
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "failed to create admin user" : error}}).dump(), "application/json");
            return;
        }

        res.status = 201;
        res.set_content(json({
            {"status", "success"},
            {"message", "Admin user created"},
            {"user", {
                {"user_id", new_user_id},
                {"username", username},
            }},
        }).dump(), "application/json");
    });

    // ── DELETE /admin/users/{user_id} ───────────────────────────────────────
    svr_->Delete(R"(/admin/users/(\d+))", [extract_current_admin_username](const httplib::Request& req, httplib::Response& res) {
        const int user_id = std::stoi(req.matches[1]);
        
        // Check if current user is super_admin
        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));
        if (!token) {
            res.status = 401;
            res.set_content(json({{"detail", "Missing authorization token"}}).dump(), "application/json");
            return;
        }
        std::string current_username;
        std::string current_role;
        if (!jwt_auth::validate_admin_token_with_role(*token, current_username, current_role)) {
            res.status = 401;
            res.set_content(json({{"detail", "Invalid authorization token"}}).dump(), "application/json");
            return;
        }
        if (current_role != "super_admin") {
            res.status = 403;
            res.set_content(json({{"detail", "Only super_admin can delete users"}}).dump(), "application/json");
            return;
        }

        std::string target_username;
        bool target_found = false;
        std::string error;
        if (!db_get_admin_username_by_id(user_id, target_username, target_found, error)) {
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "Failed to resolve user" : error}}).dump(), "application/json");
            return;
        }
        if (!target_found) {
            res.status = 404;
            res.set_content(json({{"detail", "Admin user not found"}}).dump(), "application/json");
            return;
        }

        if (target_username == current_username) {
            res.status = 400;
            res.set_content(json({{"detail", "Cannot delete yourself"}}).dump(), "application/json");
            return;
        }

        int admin_count = 0;
        if (!db_count_admin_users(admin_count, error)) {
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "Failed to count admin users" : error}}).dump(), "application/json");
            return;
        }
        if (admin_count <= 1) {
            res.status = 400;
            res.set_content(json({{"detail", "Cannot delete the last admin user"}}).dump(), "application/json");
            return;
        }

        bool found = false;
        if (!db_delete_admin_user(user_id, found, error)) {
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "Failed to delete admin user" : error}}).dump(), "application/json");
            return;
        }
        if (!found) {
            res.status = 404;
            res.set_content(json({{"detail", "Admin user not found"}}).dump(), "application/json");
            return;
        }

        res.set_content(json({
            {"status", "success"},
            {"message", "Admin user deleted"},
            {"deleted_user_id", user_id},
        }).dump(), "application/json");
    });

    // ── PUT /admin/users/{user_id}/password ─────────────────────────────────
    svr_->Put(R"(/admin/users/(\d+)/password)", [validate_password](const httplib::Request& req, httplib::Response& res) {
        const int user_id = std::stoi(req.matches[1]);

        // Role check: regular_admin can only change their own password
        {
            const auto tok = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));
            if (!tok) {
                res.status = 401;
                res.set_content(json({{"detail", "Missing authorization token"}}).dump(), "application/json");
                return;
            }
            std::string cur_user;
            std::string cur_role;
            if (!jwt_auth::validate_admin_token_with_role(*tok, cur_user, cur_role)) {
                res.status = 401;
                res.set_content(json({{"detail", "Invalid authorization token"}}).dump(), "application/json");
                return;
            }
            if (cur_role != "super_admin") {
                std::string target_user;
                bool target_found = false;
                std::string target_error;
                db_get_admin_username_by_id(user_id, target_user, target_found, target_error);
                if (!target_found || target_user != cur_user) {
                    res.status = 403;
                    res.set_content(json({{"detail", "Regular admin can only change their own password"}}).dump(), "application/json");
                    return;
                }
            }
        }

        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }

        const std::string password = body.value("password", "");
        if (auto e = validate_password(password)) {
            res.status = 422;
            res.set_content(json({{"detail", *e}}).dump(), "application/json");
            return;
        }
        if (body.contains("confirm_password") && body["confirm_password"].is_string()) {
            if (body["confirm_password"].get<std::string>() != password) {
                res.status = 422;
                res.set_content(json({{"detail", "password and confirm_password do not match"}}).dump(), "application/json");
                return;
            }
        }

        const std::string salt = jwt_auth::generate_salt_hex(16);
        const std::string hash = jwt_auth::hash_password_with_salt(salt, password);

        bool found = false;
        std::string error;
        if (!db_update_admin_user_password(user_id, hash, salt, found, error)) {
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "Failed to update password" : error}}).dump(), "application/json");
            return;
        }
        if (!found) {
            res.status = 404;
            res.set_content(json({{"detail", "Admin user not found"}}).dump(), "application/json");
            return;
        }

        res.set_content(json({
            {"status", "success"},
            {"message", "Password updated"},
            {"user_id", user_id},
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

                // Build lookup keyed by Python's 0-based lane_id, which
                // equals camera_index (the positional slot in the schema).
                // Using camera_index avoids the DB PK mismatch where
                // Python sends 0,1,2,3 but row.lane_id is an auto-increment
                // PK (e.g. 5,6,7 for intersection 2).
                std::unordered_map<int, json> live_by_camera;
                if (body.contains("lanes") && body["lanes"].is_array()) {
                    for (const auto& lane : body["lanes"]) {
                        if (lane.contains("lane_id") && lane["lane_id"].is_number_integer()) {
                            live_by_camera[lane["lane_id"].get<int>()] = lane;
                        }
                    }
                }

                // Rebuild lanes array from DB schema order.
                // schema is sorted by camera_index, so schema[i].camera_index == i.
                // Python lane_id == camera_index, so look up by i.
                // Lane IDs in output are always 0-based so React can index
                // directly into the lane_directions array from /layout.
                json normalized_lanes = json::array();
                for (int i = 0; i < static_cast<int>(schema.size()); ++i) {
                    const auto& row = schema[i];
                    // Primary key: camera_index position (== Python lane_id).
                    // Fallback: try row.camera_index explicitly, then DB PK.
                    auto it = live_by_camera.find(i);
                    if (it == live_by_camera.end())
                        it = live_by_camera.find(row.camera_index);
                    if (it == live_by_camera.end())
                        it = live_by_camera.find(row.lane_id);

                    if (it != live_by_camera.end()) {
                        // Use live metric values; always stamp canonical
                        // lane_id and direction from DB schema.
                        json lane = it->second;
                        lane["lane_id"]  = i;              // 0-based output
                        lane["direction"] = row.direction; // canonical DB direction
                        normalized_lanes.push_back(std::move(lane));
                    } else {
                        // Lane absent from live data — pad with zeros.
                        json zero_lane = {
                            {"lane_id",          i},
                            {"direction",        row.direction},
                            {"vehicle_count",    0},
                            {"queue_length",     0},
                            {"density_pct",      0.0},
                            {"waiting_time_sec", 0.0},
                            //{"pedestrian_count", 0},
                        };
                        normalized_lanes.push_back(std::move(zero_lane));
                    }
                }

                body["lanes"]     = std::move(normalized_lanes);
                body["num_lanes"] = db_lane_count;
            }
        }
        // ─────────────────────────────────────────────────────────────────────

        // Latch emergency signal if active + fresh (mirrors Python).
        if (body.contains("emergency_signal") && body["emergency_signal"].is_object()) {
            const auto& sig = body["emergency_signal"];
            if (sig.value("active", false)) {
                latch_emergency(id, sig);
            }
        }

        // Store the (lane-normalised) state.
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            state_store_[id] = body.dump();
        }

        // Determine the action to return, mirroring the Python decision flow:
        //   1) If emergency is currently latched → emergency_preempt overrides.
        //   2) Otherwise if a controller / manual action is "sticky", keep it.
        //   3) Otherwise compute a server-side greedy-with-aging fallback.
        constexpr double CPP_ACTION_STICKY_SEC    = 5.0;
        constexpr double MANUAL_ACTION_STICKY_SEC = 20.0;

        const double now_s = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        json action;
        std::string chosen_source = "server_fallback";

        json active_emergency = active_emergency_signal(id);
        if (!active_emergency.is_null()) {
            const int em_lane = active_emergency.value("lane_id", 0);
            const int phase_id = (em_lane % 2 == 0) ? 0 : 1;
            std::cout << "[GreedyAging] Intersection " << id
                      << " EMERGENCY OVERRIDE: lane=" << em_lane
                      << " vehicle_id=" << active_emergency.value("vehicle_id", std::string("?"))
                      << " => Phase" << phase_id << "\n";
            action = {
                {"action",         std::string("Phase") + std::to_string(phase_id)},
                {"reason",         "emergency_preempt"},
                {"intersection_id", id},
                {"phase_id",       phase_id},
            };
            chosen_source = "emergency_preempt";
        } else {
            std::string prev_source;
            double      prev_updated_at = 0.0;
            json        prev_action;
            {
                std::lock_guard<std::mutex> lk(store_mutex_);
                auto src_it = action_source_store_.find(id);
                auto upd_it = action_updated_at_store_.find(id);
                auto act_it = action_store_.find(id);
                if (src_it != action_source_store_.end()) prev_source = src_it->second;
                if (upd_it != action_updated_at_store_.end()) prev_updated_at = upd_it->second;
                if (act_it != action_store_.end()) {
                    try { prev_action = json::parse(act_it->second); } catch (...) {}
                }
            }

            const double age = now_s - prev_updated_at;
            const bool cpp_sticky    = (prev_source == "cpp_controller"
                                        && prev_action.is_object()
                                        && age <= CPP_ACTION_STICKY_SEC);
            const bool manual_sticky = (prev_source == "manual_dashboard"
                                        && prev_action.is_object()
                                        && age <= MANUAL_ACTION_STICKY_SEC);

            if (cpp_sticky || manual_sticky) {
                std::cout << "[GreedyAging] Intersection " << id
                          << " STICKY action retained (source=" << prev_source
                          << " age=" << std::fixed << std::setprecision(1) << age << "s)"
                          << " action=" << (prev_action.contains("action") ? prev_action["action"].get<std::string>() : "?")
                          << "\n";
                action = prev_action;
                chosen_source = prev_source;
            } else {
                action = decide_action_fallback(id, body);
                chosen_source = "server_fallback";
            }
        }

        // Persist the chosen action with source / timestamp.
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            action_store_[id]            = action.dump();
            action_source_store_[id]     = chosen_source;
            action_updated_at_store_[id] = now_s;
        }

        // Broadcast state_updated (per-intersection AND global subscribers).
        broadcast_event(id, "state_updated", json({
            {"state",  body},
            {"action", action},
            {"source", chosen_source},
        }));

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

        // Classify source from the reason hint, mirroring Python:
        //   reasons containing "manual"/"dashboard" → manual_dashboard (20s sticky)
        //   everything else                         → cpp_controller   (5s sticky)
        std::string source = "cpp_controller";
        std::string lower_reason = reason;
        for (auto& ch : lower_reason) ch = (char)std::tolower((unsigned char)ch);
        if (lower_reason.find("manual") != std::string::npos ||
            lower_reason.find("dashboard") != std::string::npos) {
            source = "manual_dashboard";
        }

        json action = {
            {"action",         action_str},
            {"reason",         reason},
            {"intersection_id", id},
            {"phase_id",       phase_id},
        };

        const double now_s = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            action_store_[id]            = action.dump();
            action_source_store_[id]     = source;
            action_updated_at_store_[id] = now_s;
        }

        broadcast_event(id, "action_updated", json({
            {"action", action},
            {"source", source},
        }));

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

    // ── GET / ───────────────────────────────────────────────────────────────
    svr_->Get("/", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(json({
            {"status",  "running"},
            {"message", "Smart Traffic Server (C++) — hybrid backend"},
            {"docs",    "/health"},
            {"health",  "/health"},
            {"intersections", "/intersections"},
            {"security", {
                {"jwt",         "enabled"},
                {"cors_origin", "*"},
            }},
            {"configuration", {
                {"port",            port_},
                {"router_mode",     router_mode_},
                {"internal_port",   internal_http_port_},
            }},
            {"emergency_auth", {
                {"vehicle_key_count", static_cast<int>(emergency_keys_.size())},
                {"max_clock_skew_sec", emergency_max_clock_skew_sec_},
            }},
            {"hardware", json::object()},
        }).dump(), "application/json");
    });

    // ── GET /config ─────────────────────────────────────────────────────────
    svr_->Get("/config", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(json({
            {"status",        "ok"},
            {"configuration", json::object()},
            {"thresholds",    json::object()},
            {"network",       json::object()},
            {"emergency",     json::object()},
            {"rl_agent",      json::object()},
            {"hardware",      {{"real_mode", false}, {"manual_emergency_enabled", true}}},
        }).dump(), "application/json");
    });

    // ── GET /metrics/summary ────────────────────────────────────────────────
    svr_->Get("/metrics/summary", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        const double ts = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        json by_intersection = json::array();
        int  total_queue     = 0;
        double waiting_sum   = 0.0;
        int    lane_total    = 0;

        std::lock_guard<std::mutex> lk(store_mutex_);
        for (const auto& kv : state_store_) {
            int id = kv.first;
            json state;
            try { state = json::parse(kv.second); } catch (...) { continue; }

            int  queue   = 0;
            double wait  = 0.0;
            int  lanes_n = 0;
            if (state.contains("lanes") && state["lanes"].is_array()) {
                for (const auto& l : state["lanes"]) {
                    if (l.contains("vehicle_count") && l["vehicle_count"].is_number()) {
                        queue += std::max(0, (int)l["vehicle_count"].get<double>());
                    }
                    if (l.contains("waiting_time_sec") && l["waiting_time_sec"].is_number()) {
                        wait += std::max(0.0, l["waiting_time_sec"].get<double>());
                    }
                    ++lanes_n;
                }
            }
            const double avg_wait = lanes_n > 0 ? wait / lanes_n : 0.0;
            total_queue += queue;
            waiting_sum += avg_wait;
            ++lane_total;

            by_intersection.push_back({
                {"intersection_id", id},
                {"total_queue",     queue},
                {"avg_waiting_sec", avg_wait},
            });
        }

        res.set_content(json({
            {"timestamp",                ts},
            {"intersection_count",       static_cast<int>(state_store_.size())},
            {"total_network_queue",      total_queue},
            {"avg_network_waiting_sec",  lane_total > 0 ? waiting_sum / lane_total : 0.0},
            {"intersections",            by_intersection},
            {"security",                 {{"jwt", "enabled"}}},
        }).dump(), "application/json");
    });

    // ── GET /intersection/{id} ──────────────────────────────────────────────
    svr_->Get(R"(/intersection/(\d+))",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);

        // Try the live state first.
        std::string state_str;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            auto it = state_store_.find(id);
            if (it != state_store_.end()) {
                state_str = it->second;
                found = true;
            }
        }

        if (!found) {
            // No state posted yet — synthesize a zero-state from the DB lane
            // schema so the React grid renders immediately (all zeros, no loading).
            const auto& schema = get_cached_lane_schema(id);
            if (schema.empty()) {
                res.status = 404;
                res.set_content(json({{"detail", "Intersection state not found"}}).dump(),
                                "application/json");
                return;
            }
            const double now_ts = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            json zero_state = {
                {"intersection_id",  id},
                {"timestamp",        now_ts},
                {"num_lanes",        static_cast<int>(schema.size())},
                {"total_vehicles",   0},
                {"emergency_signal", nullptr},
            };
            json lanes = json::array();
            for (int i = 0; i < static_cast<int>(schema.size()); ++i) {
                lanes.push_back({
                    {"lane_id",          i},
                    {"direction",        schema[i].direction},
                    {"vehicle_count",    0},
                    {"queue_length",     0},
                    {"density_pct",      0.0},
                    {"waiting_time_sec", 0.0},
                    //{"pedestrian_count", 0},
                });
            }
            zero_state["lanes"] = std::move(lanes);
            state_str = zero_state.dump();
        }

        res.set_content(state_str, "application/json");
    });

    // ── GET /intersection/{id}/layout ───────────────────────────────────────
    svr_->Get(R"(/intersection/(\d+)/layout)",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        const auto& schema = get_cached_lane_schema(id);

        json directions = json::array();
        for (const auto& row : schema) directions.push_back(row.direction);

        json neighbors = json::array();
        // Prefer the full topology (includes direction_from) for the React layout contract.
        auto it_full = neighbor_topology_full_.find(id);
        if (it_full != neighbor_topology_full_.end()) {
            for (const auto& n : it_full->second) {
                json obj = {
                    {"adjacent_intersection_id", n.adjacent_intersection_id},
                    {"direction_from", n.direction_from.empty()
                        ? json(nullptr) : json(n.direction_from)},
                };
                if (n.distance_m > 0) obj["distance_m"] = n.distance_m;
                neighbors.push_back(std::move(obj));
            }
        } else {
            // Fallback: plain IDs without direction info.
            auto it = neighbor_topology_.find(id);
            if (it != neighbor_topology_.end()) {
                for (int nid : it->second) {
                    neighbors.push_back({
                        {"adjacent_intersection_id", nid},
                        {"direction_from", nullptr},
                    });
                }
            }
        }

        res.set_content(json({
            {"intersection_id", id},
            {"num_lanes",       static_cast<int>(schema.size())},
            {"lane_directions", directions},
            {"neighbors",       neighbors},
        }).dump(), "application/json");
    });

    // ── POST /intersection/{id}/simulate-emergency ──────────────────────────
    svr_->Post(R"(/intersection/(\d+)/simulate-emergency)",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }

        const int lane_id  = body.value("lane_id", 0);
        const std::string vehicle_id = body.value("vehicle_id", std::string("dashboard-sim"));
        const double now_s = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        json signal = {
            {"active",     true},
            {"lane_id",    lane_id},
            {"vehicle_id", vehicle_id},
            {"timestamp",  now_s},
            {"signature",  nullptr},
            {"simulated",  true},
        };
        latch_emergency(id, signal);

        const int phase_id = (lane_id % 2 == 0) ? 0 : 1;
        json action = {
            {"action",          std::string("Phase") + std::to_string(phase_id)},
            {"reason",          "emergency_preempt"},
            {"intersection_id", id},
            {"phase_id",        phase_id},
        };
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            action_store_[id]            = action.dump();
            action_source_store_[id]     = "emergency_preempt";
            action_updated_at_store_[id] = now_s;
        }
        broadcast_event(id, "action_updated", json({
            {"action",  action},
            {"source",  "emergency_preempt"},
            {"signal",  signal},
        }));
        res.set_content(action.dump(), "application/json");
    });

    // ── POST /intersection/{id}/clear-emergency ─────────────────────────────
    svr_->Post(R"(/intersection/(\d+)/clear-emergency)",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        clear_emergency_latch(id);

        // Reset action stickiness so the next POST /state recomputes a fallback.
        const double now_s = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json action = {
            {"action",          "Phase0"},
            {"reason",          "emergency_cleared"},
            {"intersection_id", id},
            {"phase_id",        0},
        };
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            action_store_[id]            = action.dump();
            action_source_store_[id]     = "server_fallback";
            action_updated_at_store_[id] = now_s;
        }
        broadcast_event(id, "action_updated", json({
            {"action",  action},
            {"source",  "emergency_cleared"},
        }));
        res.set_content(action.dump(), "application/json");
    });

    // ── GET /admin/auth/verify ──────────────────────────────────────────────
    svr_->Get("/admin/auth/verify", [](const httplib::Request& req, httplib::Response& res) {
        // pre_routing_handler already validated the token if we reach here.
        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));
        std::string username;
        std::string role = "regular_admin";
        if (token) jwt_auth::validate_admin_token_with_role(*token, username, role);
        res.set_content(json({
            {"status",   "ok"},
            {"username", username},
            {"role",     role},
            {"message",  "Token is valid"},
        }).dump(), "application/json");
    });

    // ── POST /admin/manual-control ──────────────────────────────────────────
    svr_->Post("/admin/manual-control", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }
        if (!body.contains("intersection_id") || !body["intersection_id"].is_number_integer() ||
            !body.contains("phase_id")        || !body["phase_id"].is_number_integer()) {
            res.status = 422;
            res.set_content(json({{"detail", "intersection_id and phase_id are required"}}).dump(),
                            "application/json");
            return;
        }
        const int id       = body["intersection_id"].get<int>();
        const int phase_id = body["phase_id"].get<int>();
        const std::string reason = body.value("reason", std::string("manual_dashboard"));

        json action = {
            {"action",          std::string("Phase") + std::to_string(phase_id)},
            {"reason",          reason + " (manual)"},
            {"intersection_id", id},
            {"phase_id",        phase_id},
        };
        const double now_s = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            action_store_[id]            = action.dump();
            action_source_store_[id]     = "manual_dashboard";
            action_updated_at_store_[id] = now_s;
        }
        broadcast_event(id, "action_updated", json({
            {"action", action},
            {"source", "manual_dashboard"},
        }));
        res.set_content(json({
            {"status",  "success"},
            {"message", "Manual control accepted"},
            {"action",  action},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id} ────────────────────────────────────────
    svr_->Get(R"(/admin/intersection/(\d+))",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);

        json intersection_obj = nullptr;
        for (const auto& r : db_fetch_intersections()) {
            if (r.id == id) {
                intersection_obj = {
                    {"id",   r.id},
                    {"code", r.code},
                    {"name", r.name},
                    {"city", r.city},
                };
                break;
            }
        }
        if (intersection_obj.is_null()) {
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");
            return;
        }

        const auto& lanes = get_cached_lane_schema(id);
        json lanes_arr = json::array();
        for (const auto& l : lanes) {
            lanes_arr.push_back({
                {"lane_id",      l.lane_id},
                {"camera_index", l.camera_index},
                {"direction",    l.direction},
                {"description",  l.description.empty() ? json(nullptr) : json(l.description)},
            });
        }

        json neighbors_arr = json::array();
        auto nit = neighbor_topology_.find(id);
        if (nit != neighbor_topology_.end()) {
            for (int n : nit->second) neighbors_arr.push_back(n);
        }

        json state_obj  = nullptr;
        json action_obj = nullptr;
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            auto si = state_store_.find(id);
            auto ai = action_store_.find(id);
            if (si != state_store_.end()) {
                try { state_obj = json::parse(si->second); } catch (...) {}
            }
            if (ai != action_store_.end()) {
                try { action_obj = json::parse(ai->second); } catch (...) {}
            }
        }

        res.set_content(json({
            {"status",         "success"},
            {"intersection",   intersection_obj},
            {"neighbors",      neighbors_arr},
            {"lanes",          lanes_arr},
            {"current_state",  state_obj},
            {"current_action", action_obj},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id}/neighbors ──────────────────────────────
    svr_->Get(R"(/admin/intersection/(\d+)/neighbors)",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        json neighbors = json::array();
        auto it = neighbor_topology_.find(id);
        if (it != neighbor_topology_.end()) {
            std::lock_guard<std::mutex> lk(store_mutex_);
            for (int nid : it->second) {
                json state = nullptr, action = nullptr;
                auto si = state_store_.find(nid);
                auto ai = action_store_.find(nid);
                if (si != state_store_.end()) { try { state  = json::parse(si->second); } catch (...) {} }
                if (ai != action_store_.end()) { try { action = json::parse(ai->second); } catch (...) {} }
                neighbors.push_back({{"id", nid}, {"state", state}, {"action", action}});
            }
        }
        res.set_content(json({
            {"status",          "success"},
            {"intersection_id", id},
            {"neighbor_count",  static_cast<int>(neighbors.size())},
            {"neighbors",       neighbors},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id}/phase-options ──────────────────────────
    svr_->Get(R"(/admin/intersection/(\d+)/phase-options)",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        const auto& lanes = get_cached_lane_schema(id);

        // Even-lane phase (Phase0) and odd-lane phase (Phase1) — simple
        // 2-phase model matching the legacy Python heuristic. Real phase
        // computation lives in PhaseConfig / ConflictConfig on the controller
        // side; the dashboard only needs labels to populate its dropdown.
        json options = json::array();
        json even, odd;
        for (const auto& l : lanes) {
            if (l.lane_id % 2 == 0) even.push_back(l.lane_id);
            else                    odd.push_back(l.lane_id);
        }
        options.push_back({
            {"phase_id", 0}, {"label", "Phase 0 (even lanes)"}, {"green_lanes", even},
        });
        options.push_back({
            {"phase_id", 1}, {"label", "Phase 1 (odd lanes)"},  {"green_lanes", odd},
        });
        res.set_content(json({
            {"status",          "success"},
            {"intersection_id", id},
            {"count",           static_cast<int>(options.size())},
            {"phase_options",   options},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id}/lanes ──────────────────────────────────
    svr_->Get(R"(/admin/intersection/(\d+)/lanes)",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        const auto& lanes = get_cached_lane_schema(id);
        json arr = json::array();
        for (const auto& l : lanes) {
            arr.push_back({
                {"lane_id",      l.lane_id},
                {"camera_index", l.camera_index},
                {"direction",    l.direction},
                {"description",  l.description.empty() ? json(nullptr) : json(l.description)},
            });
        }
        res.set_content(json({
            {"status",          "success"},
            {"intersection_id", id},
            {"count",           static_cast<int>(arr.size())},
            {"lanes",           arr},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id}/conflicts ──────────────────────────────
    svr_->Get(R"(/admin/intersection/(\d+)/conflicts)",
        [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        auto rows = db_fetch_conflicts(id);
        json arr = json::array();
        for (const auto& r : rows) {
            arr.push_back({
                {"conflict_id",   r.conflict_id},
                {"lane_id_1",     r.lane_id_1},
                {"lane_id_2",     r.lane_id_2},
                {"conflict_type", r.conflict_type},
                {"created_at",    r.created_at.empty() ? json(nullptr) : json(r.created_at)},
            });
        }
        res.set_content(json({
            {"status",          "success"},
            {"intersection_id", id},
            {"count",           static_cast<int>(arr.size())},
            {"conflicts",       arr},
        }).dump(), "application/json");
    });

    // ── helper lambdas reused by lane/conflict write routes ────────────────
    auto build_lane_array = [](int id) {
        auto rows = db_fetch_lanes(id);
        json arr = json::array();
        for (const auto& l : rows) {
            arr.push_back({
                {"lane_id",      l.lane_id},
                {"camera_index", l.camera_index},
                {"direction",    l.direction},
                {"description",  l.description.empty() ? json(nullptr) : json(l.description)},
            });
        }
        return arr;
    };
    auto build_conflict_array = [](int id) {
        auto rows = db_fetch_conflicts(id);
        json arr = json::array();
        for (const auto& r : rows) {
            arr.push_back({
                {"conflict_id",   r.conflict_id},
                {"intersection_id", id},
                {"lane_id_1",     r.lane_id_1},
                {"lane_id_2",     r.lane_id_2},
                {"conflict_type", r.conflict_type},
                {"created_at",    r.created_at.empty() ? json(nullptr) : json(r.created_at)},
            });
        }
        return arr;
    };
    auto normalize_direction = [](std::string s) -> std::string {
        // Trim + uppercase first letter; accept "N"/"S"/"E"/"W" or names.
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (s == "NORTH") s = "N";
        else if (s == "SOUTH") s = "S";
        else if (s == "EAST")  s = "E";
        else if (s == "WEST")  s = "W";
        return s;
    };

    // ── POST /admin/intersection/{id}/lanes ────────────────────────────────
    svr_->Post(R"(/admin/intersection/(\d+)/lanes)",
        [this, build_lane_array, normalize_direction]
        (const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        if (!db_intersection_exists(id)) {
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");
            return;
        }
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }
        if (!body.contains("camera_index") || !body["camera_index"].is_number_integer() ||
            !body.contains("direction")    || !body["direction"].is_string()) {
            res.status = 422;
            res.set_content(json({{"detail", "camera_index (int) and direction (string) are required"}}).dump(),
                            "application/json");
            return;
        }
        const int cam   = body["camera_index"].get<int>();
        const std::string dir = normalize_direction(body["direction"].get<std::string>());
        std::optional<std::string> desc;
        if (body.contains("description") && body["description"].is_string()) {
            desc = body["description"].get<std::string>();
        }

        std::string err;
        if (!db_insert_lane(id, cam, dir, desc, err)) {
            res.status = 400;
            res.set_content(json({{"detail", std::string("Failed creating lane: ") + err}}).dump(),
                            "application/json");
            return;
        }
        invalidate_lane_cache(id);
        res.set_content(json({
            {"status",  "success"},
            {"message", "Lane created successfully"},
            {"lanes",   build_lane_array(id)},
        }).dump(), "application/json");
    });

    // ── PUT /admin/intersection/{id}/lanes/{lane_id} ───────────────────────
    svr_->Put(R"(/admin/intersection/(\d+)/lanes/(\d+))",
        [this, build_lane_array, normalize_direction]
        (const httplib::Request& req, httplib::Response& res) {
        int id      = std::stoi(req.matches[1]);
        int lane_id = std::stoi(req.matches[2]);

        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }

        std::optional<std::string> dir;
        std::optional<std::string> desc;
        if (body.contains("direction") && body["direction"].is_string()) {
            dir = normalize_direction(body["direction"].get<std::string>());
        }
        if (body.contains("description") && body["description"].is_string()) {
            desc = body["description"].get<std::string>();
        }
        if (!dir && !desc) {
            res.status = 400;
            res.set_content(json({{"detail", "No fields to update"}}).dump(), "application/json");
            return;
        }

        bool found = false;
        std::string err;
        if (!db_update_lane(id, lane_id, dir, desc, found, err)) {
            res.status = 400;
            res.set_content(json({{"detail", std::string("Failed updating lane: ") + err}}).dump(),
                            "application/json");
            return;
        }
        if (!found) {
            res.status = 404;
            res.set_content(json({{"detail", "Lane not found"}}).dump(), "application/json");
            return;
        }
        invalidate_lane_cache(id);
        res.set_content(json({
            {"status",  "success"},
            {"message", "Lane updated successfully"},
            {"lanes",   build_lane_array(id)},
        }).dump(), "application/json");
    });

    // ── DELETE /admin/intersection/{id}/lanes/{lane_id} ────────────────────
    svr_->Delete(R"(/admin/intersection/(\d+)/lanes/(\d+))",
        [this, build_lane_array](const httplib::Request& req, httplib::Response& res) {
        int id      = std::stoi(req.matches[1]);
        int lane_id = std::stoi(req.matches[2]);

        bool found = false;
        std::string err;
        if (!db_delete_lane(id, lane_id, found, err)) {
            res.status = 400;
            res.set_content(json({{"detail", std::string("Failed deleting lane: ") + err}}).dump(),
                            "application/json");
            return;
        }
        if (!found) {
            res.status = 404;
            res.set_content(json({{"detail", "Lane not found"}}).dump(), "application/json");
            return;
        }
        invalidate_lane_cache(id);
        res.set_content(json({
            {"status",  "success"},
            {"message", "Lane deleted successfully"},
            {"lanes",   build_lane_array(id)},
        }).dump(), "application/json");
    });

    // ── POST /admin/intersection/{id}/conflicts ────────────────────────────
    svr_->Post(R"(/admin/intersection/(\d+)/conflicts)",
        [build_conflict_array](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        if (!db_intersection_exists(id)) {
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");
            return;
        }
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }
        if (!body.contains("lane_id_1") || !body["lane_id_1"].is_number_integer() ||
            !body.contains("lane_id_2") || !body["lane_id_2"].is_number_integer()) {
            res.status = 422;
            res.set_content(json({{"detail", "lane_id_1 and lane_id_2 are required"}}).dump(),
                            "application/json");
            return;
        }
        const int a = body["lane_id_1"].get<int>();
        const int b = body["lane_id_2"].get<int>();
        if (a == b) {
            res.status = 400;
            res.set_content(json({{"detail", "lane_id_1 and lane_id_2 must differ"}}).dump(),
                            "application/json");
            return;
        }
        const int l1 = std::min(a, b);
        const int l2 = std::max(a, b);
        const std::string ctype = (body.contains("conflict_type") && body["conflict_type"].is_string())
            ? body["conflict_type"].get<std::string>() : std::string("crossing");

        bool missing = false, dup = false;
        std::string err;
        if (!db_insert_conflict(id, l1, l2, ctype, missing, dup, err)) {
            if (missing) {
                res.status = 404;
                res.set_content(json({{"detail", "Lane not found in intersection"}}).dump(),
                                "application/json");
            } else if (dup) {
                res.status = 400;
                res.set_content(json({{"detail", "This conflict pair already exists"}}).dump(),
                                "application/json");
            } else {
                res.status = 400;
                res.set_content(json({{"detail", std::string("Failed creating conflict: ") + err}}).dump(),
                                "application/json");
            }
            return;
        }
        json arr = build_conflict_array(id);
        res.set_content(json({
            {"status",          "success"},
            {"message",         "Lane conflict created successfully"},
            {"intersection_id", id},
            {"count",           static_cast<int>(arr.size())},
            {"conflicts",       arr},
        }).dump(), "application/json");
    });

    // ── DELETE /admin/intersection/{id}/conflicts/{conflict_id} ────────────
    svr_->Delete(R"(/admin/intersection/(\d+)/conflicts/(\d+))",
        [build_conflict_array](const httplib::Request& req, httplib::Response& res) {
        int id  = std::stoi(req.matches[1]);
        int cid = std::stoi(req.matches[2]);
        if (!db_intersection_exists(id)) {
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");
            return;
        }
        bool found = false;
        std::string err;
        if (!db_delete_conflict(id, cid, found, err)) {
            res.status = 400;
            res.set_content(json({{"detail", std::string("Failed deleting conflict: ") + err}}).dump(),
                            "application/json");
            return;
        }
        if (!found) {
            res.status = 404;
            res.set_content(json({{"detail", "Conflict not found"}}).dump(), "application/json");
            return;
        }
        json arr = build_conflict_array(id);
        res.set_content(json({
            {"status",          "success"},
            {"message",         "Lane conflict deleted successfully"},
            {"intersection_id", id},
            {"count",           static_cast<int>(arr.size())},
            {"conflicts",       arr},
        }).dump(), "application/json");
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

void TrafficServer::invalidate_lane_cache(int intersection_id)
{
    std::lock_guard<std::mutex> lk(lane_cache_mutex_);
    lane_schema_cache_.erase(intersection_id);
    lane_cache_loaded_.erase(intersection_id);
}

void TrafficServer::load_neighbor_topology()
{
    auto db_topology      = db_fetch_neighbor_topology();
    auto db_topology_full = db_fetch_neighbor_full_topology();
    if (!db_topology.empty()) {
        neighbor_topology_      = std::move(db_topology);
        neighbor_topology_full_ = std::move(db_topology_full);
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

// (helpers appended below)

// ── broadcast_event ──────────────────────────────────────────────────────────
// Wraps a payload in the canonical envelope and ships it to BOTH
// /ws/intersection/{id} subscribers and /ws/updates global subscribers.
void TrafficServer::broadcast_event(int intersection_id,
                                    const std::string& event_name,
                                    const nlohmann::json& payload)
{
    const double ts = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const nlohmann::json envelope = {
        {"event",           event_name},
        {"intersection_id", intersection_id},
        {"timestamp",       ts},
        {"payload",         payload},
    };
    if (hub_) hub_->broadcast_intersection(intersection_id, envelope.dump());
}

// ── latch_emergency / clear_emergency_latch / active_emergency_signal ────────
void TrafficServer::latch_emergency(int intersection_id, const nlohmann::json& signal)
{
    constexpr double EMERGENCY_LATCH_SEC = 8.0;
    const double now_s = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(emergency_latch_mutex_);
    emergency_latch_[intersection_id]        = signal.dump();
    emergency_latch_expiry_[intersection_id] = now_s + EMERGENCY_LATCH_SEC;
}

void TrafficServer::clear_emergency_latch(int intersection_id)
{
    std::lock_guard<std::mutex> lk(emergency_latch_mutex_);
    emergency_latch_.erase(intersection_id);
    emergency_latch_expiry_.erase(intersection_id);
}

nlohmann::json TrafficServer::active_emergency_signal(int intersection_id)
{
    const double now_s = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(emergency_latch_mutex_);
    auto exp_it = emergency_latch_expiry_.find(intersection_id);
    if (exp_it == emergency_latch_expiry_.end() || exp_it->second < now_s) {
        emergency_latch_.erase(intersection_id);
        emergency_latch_expiry_.erase(intersection_id);
        return nullptr;
    }
    auto sig_it = emergency_latch_.find(intersection_id);
    if (sig_it == emergency_latch_.end()) return nullptr;
    try { return nlohmann::json::parse(sig_it->second); }
    catch (...) { return nullptr; }
}

// ── decide_action_fallback ───────────────────────────────────────────────────
// Server-side greedy-with-aging fallback used when no controller / manual /
// emergency action is in force. Scores each lane by
//     vehicle_count * 1.5 + density_pct * 0.5 + waiting_time_sec * 0.2
// then maps the winner's lane index to phase 0 (even) or phase 1 (odd).
// Matches Python decide_action() so the React UI sees the same labels.
nlohmann::json TrafficServer::decide_action_fallback(int intersection_id,
                                                    const nlohmann::json& state_body)
{
    int    best_lane  = 0;
    double best_score = -1.0;
    bool   any_lane   = false;

    std::cout << "[GreedyAging] Intersection " << intersection_id
              << " — scoring lanes:\n";

    if (state_body.contains("lanes") && state_body["lanes"].is_array()) {
        for (const auto& lane : state_body["lanes"]) {
            if (!lane.is_object()) continue;
            const int    lid   = lane.value("lane_id", 0);
            const std::string dir = lane.value("direction", std::string(""));
            const double vc    = lane.contains("vehicle_count")    && lane["vehicle_count"].is_number()    ? lane["vehicle_count"].get<double>()    : 0.0;
            const double dens  = lane.contains("density_pct")      && lane["density_pct"].is_number()      ? lane["density_pct"].get<double>()      : 0.0;
            const double wait  = lane.contains("waiting_time_sec") && lane["waiting_time_sec"].is_number() ? lane["waiting_time_sec"].get<double>() : 0.0;
            const double score = vc * 1.5 + dens * 0.5 + wait * 0.2;

            std::cout << "  Lane " << lid
                      << " (" << (dir.empty() ? "?" : dir) << ")"
                      << "  vc=" << vc
                      << "  dens=" << dens << "%"
                      << "  wait=" << wait << "s"
                      << "  => score=" << std::fixed << std::setprecision(2) << score
                      << (score > best_score ? "  <-- best so far" : "")
                      << "\n";

            if (score > best_score) { best_score = score; best_lane = lid; }
            any_lane = true;
        }
    }

    const int phase_id = (best_lane % 2 == 0) ? 0 : 1;

    if (any_lane) {
        std::cout << "[GreedyAging] Winner: lane " << best_lane
                  << "  score=" << std::fixed << std::setprecision(2) << best_score
                  << "  => Phase" << phase_id
                  << "  (reason: highest_score)\n";
    } else {
        std::cout << "[GreedyAging] No lane data — defaulting to Phase0\n";
    }

    return nlohmann::json({
        {"action",          std::string("Phase") + std::to_string(phase_id)},
        {"reason",          std::string("server_fallback_greedy_aging lane=") + std::to_string(best_lane)},
        {"intersection_id", intersection_id},
        {"phase_id",        phase_id},
    });
}

