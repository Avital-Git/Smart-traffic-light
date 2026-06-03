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
