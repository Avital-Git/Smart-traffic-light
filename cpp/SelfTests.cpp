#include "SelfTests.h"

#include "Junction.h"
#include "RLAgent.h"
#include "PhaseConfig.h"
#include "ThresholdConfig.h"
#include "NeighborCoordConfig.h"
#include "Simulation.h"

#include <iostream>
#include <vector>

namespace traffic_tests {
namespace {

bool test_conflict_phase_rejected() {
    using namespace traffic;

    std::vector<Lane> lanes = {
        {0, 2, 10.0, 0.0, false},
        {1, 2, 10.0, 0.0, false}
    };

    std::vector<Action> phases = {
        {0, {0, 1}}
    };

    std::vector<std::pair<int, int>> conflicts = {
        {0, 1}
    };

    Junction junction(1, lanes, phases, 5.0, 30.0, conflicts);
    const bool applied = junction.applyPhase(0, 0.0);
    return applied == false;
}

bool test_emergency_override_min_green() {
    using namespace traffic;

    std::vector<Lane> lanes = {
        {0, 5, 30.0, 0.0, false},
        {1, 5, 30.0, 0.0, true}
    };

    std::vector<Action> phases = {
        {0, {0}},
        {1, {1}}
    };

    Junction junction(1, lanes, phases, 10.0, 30.0, {});

    if (!junction.applyPhase(0, 0.0)) return false;

    // Too early to switch by policy.
    if (junction.canSwitchPhase(1.0)) return false;

    // Emergency can bypass min-green, but not inter-green safety guard.
    junction.setEmergencySignal(true, 1);
    const bool switchedTooEarly = junction.applyPhase(1, 1.0);
    if (switchedTooEarly) return false;

    // After inter-green elapsed (yellow+all-red), emergency switch should be allowed.
    const bool switchedSafely = junction.applyPhase(1, 3.1);
    return switchedSafely && junction.activePhaseId() == 1;
}

bool test_phase_config_resolve_filters_missing_lanes() {
    using namespace traffic;

    PhaseConfig cfg;
    cfg.phasesByIntersection[1] = {
        {0, {0, 2}},
        {1, {1, 3}}
    };

    const std::vector<int> availableLaneIds = {0, 1, 2}; // lane 3 missing
    const auto resolved = resolveConfiguredPhases(1, availableLaneIds, cfg);

    if (resolved.size() != 1) return false;
    return resolved.front().phaseId == 0;
}

bool test_threshold_defaults_sane() {
    const auto cfg = traffic::loadTrafficThresholdConfig("missing_threshold_file.json");
    return cfg.vehicleCount.mediumMax >= cfg.vehicleCount.lowMax &&
           cfg.waitingTimeSec.mediumMax >= cfg.waitingTimeSec.lowMax &&
           cfg.densityPct.mediumMax >= cfg.densityPct.lowMax;
}

bool test_neighbor_tuning_defaults_sane() {
    const auto cfg = traffic::loadNeighborCoordConfig("missing_neighbor_tuning_file.json");
    return cfg.rewardSyncBonus >= 0.0 &&
           cfg.rewardOpposingPenalty >= 0.0 &&
           cfg.localWeightDivisor >= 1.0;
}

bool test_neighbor_sync_increases_reward() {
    using namespace traffic;

    RLAgent agent;

    JunctionState prev;
    prev.vehicleCounts = {6, 6};
    prev.waitingTimes = {10.0, 10.0};

    JunctionState next;
    next.vehicleCounts = {4, 4};
    next.waitingTimes = {8.0, 8.0};

    const double rewardNoSync = agent.computeReward(prev, next, 0, false, false, false, 1.0);
    const double rewardWithSync = agent.computeReward(prev, next, 0, false, true, false, 1.0);

    return rewardWithSync > rewardNoSync;
}

bool test_neighbor_opposing_decreases_reward() {
    using namespace traffic;

    RLAgent agent;

    JunctionState prev;
    prev.vehicleCounts = {2, 2};
    prev.waitingTimes = {5.0, 5.0};

    JunctionState next;
    next.vehicleCounts = {7, 7}; // queue worsened to amplify opposing penalty path
    next.waitingTimes = {12.0, 12.0};

    const double rewardNoOpposing = agent.computeReward(prev, next, 0, false, false, false, 1.0);
    const double rewardOpposing = agent.computeReward(prev, next, 0, false, false, true, 1.0);

    return rewardOpposing < rewardNoOpposing;
}

bool test_neighbor_rule_prefers_matching_phase() {
    using namespace traffic;

    RLConfig rlCfg;
    rlCfg.epsilon = 1.0;      // force exploration path => rule-based policy
    rlCfg.epsilonMin = 1.0;
    rlCfg.epsilonDecay = 1.0;

    NeighborCoordConfig neighborCfg;
    neighborCfg.ruleSyncWeight = 4.0;
    neighborCfg.ruleEmergencyBonus = 0.0;
    neighborCfg.ruleOpposingWeight = 0.2;

    RLAgent agent(rlCfg, TrafficThresholdConfig{}, neighborCfg);

    JunctionState state;
    state.laneIds = {0, 1};
    state.vehicleCounts = {3, 3};
    state.waitingTimes = {5.0, 5.0};
    state.densityPercents = {10.0, 10.0};
    state.neighborSignals = {
        NeighborSignal{2, 1, 20, 10.0, false},
        NeighborSignal{3, 1, 18, 9.0, false}
    };

    const std::vector<Action> actions = {
        {0, {0}},
        {1, {1}}
    };

    const int chosen = agent.selectAction(state, actions, std::nullopt);
    return chosen == 1;
}

bool test_kpi_neighbor_regression() {
    // Regression gate: ensure neighbor coordination doesn't degrade KPI.
    // Minimum threshold: neighbor profile beats no-neighbor by at least 0.3.
    constexpr double kMinImprovement = 0.3;

    const auto result = traffic_sim::run_simulation_comparison();
    
    if (!result.neighborImprovement) {
        return false;
    }
    
    if (result.scoreDelta < kMinImprovement) {
        return false;
    }
    
    return true;
}

} // namespace

bool run_all_self_tests() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const std::vector<Case> tests = {
        {"conflict phase rejected", test_conflict_phase_rejected},
        {"emergency override min-green", test_emergency_override_min_green},
        {"phase config resolve filters missing lanes", test_phase_config_resolve_filters_missing_lanes},
        {"threshold defaults sane", test_threshold_defaults_sane},
        {"neighbor tuning defaults sane", test_neighbor_tuning_defaults_sane},
        {"neighbor sync increases reward", test_neighbor_sync_increases_reward},
        {"neighbor opposing decreases reward", test_neighbor_opposing_decreases_reward},
        {"neighbor rule prefers matching phase", test_neighbor_rule_prefers_matching_phase},
        {"KPI neighbor regression gate", test_kpi_neighbor_regression},
    };

    int passed = 0;
    std::cout << "\n=== Self Tests ===\n";
    for (const auto& test : tests) {
        const bool ok = test.fn();
        std::cout << " - " << test.name << ": " << (ok ? "PASS" : "FAIL") << "\n";
        if (ok) ++passed;
    }

    std::cout << "Self tests result: " << passed << "/" << tests.size() << " passed\n";
    return passed == static_cast<int>(tests.size());
}

} // namespace traffic_tests
