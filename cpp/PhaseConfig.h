#pragma once

#include "Junction.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace traffic {

struct PhaseConfig {
    std::unordered_map<int, std::vector<Action>> phasesByIntersection;
    std::string source = "defaults";
};

// Loads phase topology config from JSON file path.
// Falls back to empty config when file is missing/invalid.
PhaseConfig loadPhaseConfig(const std::string& preferredPath = "");

// Returns configured phases for an intersection after filtering lanes that do not exist
// in the current topology. Returns empty if not configured or invalid.
std::vector<Action> resolveConfiguredPhases(
    int intersectionId,
    const std::vector<int>& availableLaneIds,
    const PhaseConfig& config
);

} // namespace traffic
