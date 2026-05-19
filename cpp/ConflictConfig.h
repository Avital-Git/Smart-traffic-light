#pragma once

#include <string>
#include <utility>
#include <vector>

namespace traffic {

struct LaneConflictConfig {
    // Pairs of lane IDs that must never be green simultaneously.
    std::vector<std::pair<int, int>> conflictPairs;

    // Metadata for diagnostics.
    std::string source = "defaults";
};

// Loads lane conflicts from JSON file path. If path is empty, resolve default search locations.
// Falls back to empty conflict list when file is missing/invalid.
LaneConflictConfig loadLaneConflictConfig(const std::string& preferredPath = "");

// Loads lane conflicts with per-intersection override support.
// Resolution order:
// 1) env var TRAFFIC_CONFLICTS_FILE
// 2) ./lane_conflicts_<intersectionId>.json
// 3) ./cpp/lane_conflicts_<intersectionId>.json
// 4) generic loader loadLaneConflictConfig(preferredPath)
LaneConflictConfig loadLaneConflictConfigForIntersection(int intersectionId, const std::string& preferredPath = "");

} // namespace traffic
