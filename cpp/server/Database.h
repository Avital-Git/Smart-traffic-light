#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct IntersectionRow {
    int id;
    std::string code;
    std::string name;
    std::string city;
};

struct LaneRow {
    int lane_id;
    int camera_index;
    std::string direction;
    std::string description;   // may be empty
};

struct ConflictRow {
    int conflict_id;
    int lane_id_1;
    int lane_id_2;
    std::string conflict_type; // e.g. "crossing"
    std::string created_at;    // ISO string, may be empty
};

struct NeighborRow {
    int adjacent_intersection_id{0};
    std::string direction_from;   // "N","S","E","W" etc., may be empty
    int distance_m{0};
};

struct AdminUserRow {
    int user_id;
    std::string username;
    std::string created_at;  // ISO string, may be empty
    std::string last_login;  // ISO string, may be empty
};

// Query dbo.intersections via ODBC.
// On any connection or query error falls back to hardcoded defaults identical
// to the ones in the Python server.
std::vector<IntersectionRow> db_fetch_intersections();

// Query dbo.intersection_lanes for one intersection (ordered by camera_index, lane_id).
// Returns empty vector if DB is unreachable.
std::vector<LaneRow> db_fetch_lanes(int intersection_id);

// Query dbo.lane_conflicts for one intersection (ordered newest-first).
// Returns empty vector if DB is unreachable.
std::vector<ConflictRow> db_fetch_conflicts(int intersection_id);

// Check if a specific intersection exists in dbo.intersections.
bool db_intersection_exists(int intersection_id);

// Insert a new intersection row into dbo.intersections.
// Returns true on success; false on DB error. Error details are placed in error_out.
bool db_insert_intersection(
    const std::string& code,
    const std::string& name,
    double latitude,
    double longitude,
    int num_cameras,
    const std::optional<std::string>& city,
    const std::optional<std::string>& region,
    const std::optional<std::string>& description,
    std::string& error_out);

// Update allowed fields on dbo.intersections for one row.
// found_out=false means intersection_id was not present.
bool db_update_intersection(
    int intersection_id,
    const std::optional<std::string>& name,
    const std::optional<int>& num_cameras,
    const std::optional<std::string>& city,
    const std::optional<std::string>& region,
    const std::optional<std::string>& description,
    bool& found_out,
    std::string& error_out);

// Query neighbor topology from dbo.intersection_neighbors.
// Returns map: intersection_id -> list of adjacent_intersection_id.
// Returns empty map if DB is unreachable or query fails.
std::unordered_map<int, std::vector<int>> db_fetch_neighbor_topology();

// Same as above but includes direction_from and distance_m per neighbor row.
std::unordered_map<int, std::vector<NeighborRow>> db_fetch_neighbor_full_topology();

// ── Lane CRUD ────────────────────────────────────────────────────────────────
// Insert a new lane row. Server normalises the direction (caller supplies it
// pre-normalised; e.g. "N", "S", "E", "W"). Description may be empty/null.
bool db_insert_lane(
    int intersection_id,
    int camera_index,
    const std::string& direction,
    const std::optional<std::string>& description,
    std::string& error_out);

// Update direction and/or description of a lane.
// `found_out=false` means (lane_id, intersection_id) was not present.
bool db_update_lane(
    int intersection_id,
    int lane_id,
    const std::optional<std::string>& direction,
    const std::optional<std::string>& description,
    bool& found_out,
    std::string& error_out);

// Delete a lane. `found_out=false` means the lane did not exist.
bool db_delete_lane(
    int intersection_id,
    int lane_id,
    bool& found_out,
    std::string& error_out);

// ── Conflict CRUD ────────────────────────────────────────────────────────────
// Insert a conflict row. Caller is responsible for ordering (lane_id_1 < lane_id_2).
// `missing_lane_out` is set if either lane does not belong to the intersection.
// `duplicate_out`    is set if a conflict for that pair already exists.
bool db_insert_conflict(
    int intersection_id,
    int lane_id_1,
    int lane_id_2,
    const std::string& conflict_type,
    bool& missing_lane_out,
    bool& duplicate_out,
    std::string& error_out);

// Delete a conflict. `found_out=false` means the conflict was not present.
bool db_delete_conflict(
    int intersection_id,
    int conflict_id,
    bool& found_out,
    std::string& error_out);

// ── Admin Users (Authentication + Management) ─────────────────────────────
// Returns list of all admin users (without sensitive fields), ordered by user_id.
std::vector<AdminUserRow> db_list_admin_users();

// Fetch credential material for one username.
// Returns true if user exists and outputs password_hash/salt.
bool db_get_admin_credentials(
    const std::string& username,
    std::string& password_hash_out,
    std::string& salt_out);

// Update last_login for a username. Best-effort; returns false on DB error.
bool db_touch_admin_last_login(const std::string& username);

// Insert a new admin user. Returns new user_id in user_id_out.
// If username already exists, duplicate_username_out is set to true.
bool db_insert_admin_user(
    const std::string& username,
    const std::string& password_hash,
    const std::string& salt,
    int& user_id_out,
    bool& duplicate_username_out,
    std::string& error_out);

// Delete admin user by id. found_out=false means user_id did not exist.
bool db_delete_admin_user(
    int user_id,
    bool& found_out,
    std::string& error_out);

// Update password hash+salt for an admin user by id.
// found_out=false means user_id did not exist.
bool db_update_admin_user_password(
    int user_id,
    const std::string& password_hash,
    const std::string& salt,
    bool& found_out,
    std::string& error_out);

// Resolve username for a user_id. found_out=false means user_id did not exist.
bool db_get_admin_username_by_id(
    int user_id,
    std::string& username_out,
    bool& found_out,
    std::string& error_out);

// Count admin users in table.
bool db_count_admin_users(int& count_out, std::string& error_out);
