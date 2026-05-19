#pragma once

#include <string>

namespace traffic {

struct NeighborCoordConfig {
    // Control where neighbor coordination is applied
    bool includeNeighborInStateEncoding = false;
    bool includeNeighborInRuleScoring = true;
    bool includeNeighborInReward = true;

    // Reward-side coordination weights
    double rewardSyncBonus = 5.0;
    double rewardOpposingPenalty = 2.5;
    double rewardSyncWhenQueueWorseScale = 0.25;
    double rewardOpposingWhenQueueWorseScale = 1.25;
    double rewardOpposingWhenQueueBetterScale = 0.60;

    // Rule-based action scoring weights
    double ruleSyncWeight = 1.2;
    double ruleEmergencyBonus = 1.5;
    double ruleOpposingWeight = 0.75;
    double localWeightDivisor = 18.0;

    std::string profileName = "default";
    std::string source = "defaults";
};

// Loads neighbor coordination weights from JSON.
NeighborCoordConfig loadNeighborCoordConfig(const std::string& preferredPath = "");

// Resolves config path by priority:
// 1) preferredPath
// 2) env var TRAFFIC_NEIGHBOR_TUNING_FILE
// 3) ./neighbor_tuning_balanced.json
// 4) ./cpp/neighbor_tuning_balanced.json
std::string resolveNeighborCoordConfigPath(const std::string& preferredPath = "");

} // namespace traffic
