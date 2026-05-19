#pragma once

#include <string>

namespace traffic {

struct ThreeLevelThresholds {
    double lowMax = 0.0;
    double mediumMax = 0.0;
};

struct TrafficThresholdConfig {
    // vehicle_count: light <= lowMax, medium <= mediumMax, heavy > mediumMax
    ThreeLevelThresholds vehicleCount{5.0, 15.0};

    // waiting_time_sec: short <= lowMax, medium <= mediumMax, long > mediumMax
    ThreeLevelThresholds waitingTimeSec{20.0, 60.0};

    // density_pct: low <= lowMax, medium <= mediumMax, high > mediumMax
    ThreeLevelThresholds densityPct{30.0, 70.0};

    // Metadata: where the active values came from.
    std::string source = "defaults";
};

// Loads thresholds from JSON file path. If path is empty, resolve default search locations.
// Falls back to sensible defaults when file is missing/invalid.
TrafficThresholdConfig loadTrafficThresholdConfig(const std::string& preferredPath = "");

// Loads thresholds with per-intersection override support.
// Resolution order:
// 1) env var TRAFFIC_THRESHOLDS_FILE (global override)
// 2) ./traffic_thresholds_<intersectionId>.json
// 3) ./cpp/traffic_thresholds_<intersectionId>.json
// 4) generic loader loadTrafficThresholdConfig(preferredPath)
TrafficThresholdConfig loadTrafficThresholdConfigForIntersection(int intersectionId, const std::string& preferredPath = "");

// Resolves threshold config path by priority:
// 1) preferredPath argument (if non-empty)
// 2) env var TRAFFIC_THRESHOLDS_FILE
// 3) ./traffic_thresholds.json
// 4) ./cpp/traffic_thresholds.json
std::string resolveThresholdConfigPath(const std::string& preferredPath = "");

} // namespace traffic
