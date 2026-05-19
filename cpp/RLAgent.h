#pragma once

#include "Junction.h"
#include "ThresholdConfig.h"
#include "NeighborCoordConfig.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <random>
#include <cstdint>

namespace traffic {

struct RLConfig {
    double alpha = 0.10;
    double gamma = 0.95;
    double epsilon = 0.15;
    double epsilonMin = 0.02;
    double epsilonDecay = 0.995;
};

class RLAgent {
public:
    explicit RLAgent(
        RLConfig cfg = {},
        TrafficThresholdConfig thresholds = {},
        NeighborCoordConfig neighborConfig = {}
    );

    // Choose next action for a junction state + available phases
    int selectAction(
        const JunctionState& state,
        const std::vector<Action>& validActions,
        std::optional<int> emergencyPhase
    );

    // Q-learning update
    void update(
        const JunctionState& prevState,
        int actionPhaseId,
        double reward,
        const JunctionState& nextState,
        const std::vector<Action>& nextValidActions
    );

    // Reward skeleton: waiting-time penalty + emergency priority
    double computeReward(
        const JunctionState& prevState,
        const JunctionState& nextState,
        int actionPhaseId,
        bool emergencyLaneGotGreen,
        bool greenSyncedWithNeighbor = false,
        bool greenOppositeToNeighbor = false,
        double stepSec = 1.0
    ) const;

    void decayExploration();
    double epsilon() const noexcept;
    void setThresholdConfig(TrafficThresholdConfig thresholds);
    const TrafficThresholdConfig& thresholdConfig() const noexcept;
    void setNeighborCoordConfig(NeighborCoordConfig config);
    const NeighborCoordConfig& neighborCoordConfig() const noexcept;
    void setRandomSeed(std::uint32_t seed) noexcept;

    bool loadQTable(const std::string& filePath);
    bool saveQTable(const std::string& filePath) const;

private:
    int selectRuleBasedAction(const JunctionState& state, const std::vector<Action>& validActions) const;
    int discretizeThreeLevel(double value, const ThreeLevelThresholds& thresholds) const;
    std::string encodeState(const JunctionState& s) const;
    std::vector<double>& qValuesFor(const std::string& stateKey, std::size_t actionCount);
    int actionIndexByPhaseId(const std::vector<Action>& validActions, int phaseId) const;
    int phaseIdByActionIndex(const std::vector<Action>& validActions, int actionIndex) const;

private:
    RLConfig cfg_;
    TrafficThresholdConfig thresholds_;
    NeighborCoordConfig neighborCfg_;
    std::unordered_map<std::string, std::vector<double>> qTable_;
    mutable std::mt19937 rng_{std::random_device{}()};
};

} // namespace traffic