#include "Simulation.h"

#include "Junction.h"
#include "RLAgent.h"
#include "Controller.h"
#include "ConflictConfig.h"
#include "NeighborCoordConfig.h"
#include "PhaseConfig.h"
#include "ThresholdConfig.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <optional>
#include <random>
#include <vector>
#include <memory>
#include <string>

namespace traffic_sim {
namespace {

struct SimMetrics {
    double avgWaitingSec = 0.0;
    int maxQueue = 0;
    int throughput = 0;
    int phaseSwitches = 0;
};

SimMetrics add_metrics(const SimMetrics& a, const SimMetrics& b) {
    SimMetrics m;
    m.avgWaitingSec = a.avgWaitingSec + b.avgWaitingSec;
    m.maxQueue = a.maxQueue + b.maxQueue;
    m.throughput = a.throughput + b.throughput;
    m.phaseSwitches = a.phaseSwitches + b.phaseSwitches;
    return m;
}

SimMetrics scale_metrics(const SimMetrics& m, double s) {
    SimMetrics out;
    out.avgWaitingSec = m.avgWaitingSec * s;
    out.maxQueue = static_cast<int>(std::lround(static_cast<double>(m.maxQueue) * s));
    out.throughput = static_cast<int>(std::lround(static_cast<double>(m.throughput) * s));
    out.phaseSwitches = static_cast<int>(std::lround(static_cast<double>(m.phaseSwitches) * s));
    return out;
}

double profile_score(const SimMetrics& m) {
    // Lower is better. Prioritize avg waiting, then queue, then throughput.
    return (m.avgWaitingSec * 100.0) + static_cast<double>(m.maxQueue) - (static_cast<double>(m.throughput) * 0.02);
}

bool config_loaded_from_file(const traffic::NeighborCoordConfig& cfg) {
    return cfg.source.rfind("defaults (missing:", 0) != 0 &&
           cfg.source.rfind("defaults (invalid:", 0) != 0;
}

traffic::NeighborCoordConfig load_profile_from_paths(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        const auto cfg = traffic::loadNeighborCoordConfig(path);
        if (config_loaded_from_file(cfg)) {
            return cfg;
        }
    }

    if (!paths.empty()) {
        return traffic::loadNeighborCoordConfig(paths.front());
    }

    return traffic::loadNeighborCoordConfig();
}

bool phase_contains_lane(const std::vector<traffic::Action>& phases, int phaseId, int laneId) {
    for (const auto& p : phases) {
        if (p.phaseId != phaseId) continue;
        return std::find(p.greenLanes.begin(), p.greenLanes.end(), laneId) != p.greenLanes.end();
    }
    return false;
}

traffic::JunctionState add_simulated_neighbors(traffic::JunctionState state, int step) {
    traffic::NeighborSignal upstream;
    upstream.intersectionId = 2;
    upstream.phaseId = ((step / 20) % 2 == 0) ? 0 : 1;
    upstream.totalQueue = (upstream.phaseId == 0) ? 26 : 12;
    upstream.avgWaitingSec = (upstream.phaseId == 0) ? 18.0 : 8.0;
    upstream.emergencyActive = false;

    traffic::NeighborSignal downstream;
    downstream.intersectionId = 3;
    downstream.phaseId = ((step / 25) % 2 == 0) ? 1 : 0;
    downstream.totalQueue = (downstream.phaseId == 1) ? 24 : 10;
    downstream.avgWaitingSec = (downstream.phaseId == 1) ? 16.0 : 7.0;
    downstream.emergencyActive = (step >= 180 && step < 190);

    state.neighborSignals = {upstream, downstream};
    return state;
}

bool has_busy_synced_neighbor(
    const traffic::JunctionState& state,
    int selectedPhase,
    const traffic::TrafficThresholdConfig& thresholds
) {
    const double localLaneCount = static_cast<double>(std::max<std::size_t>(1, state.laneIds.size()));
    for (const auto& neighbor : state.neighborSignals) {
        const double normalizedQueue = static_cast<double>(neighbor.totalQueue) / localLaneCount;
        if (neighbor.phaseId == selectedPhase && normalizedQueue > thresholds.vehicleCount.lowMax) {
            return true;
        }
    }
    return false;
}

bool has_busy_opposing_neighbor(
    const traffic::JunctionState& state,
    int selectedPhase,
    const traffic::TrafficThresholdConfig& thresholds
) {
    const double localLaneCount = static_cast<double>(std::max<std::size_t>(1, state.laneIds.size()));
    for (const auto& neighbor : state.neighborSignals) {
        const double normalizedQueue = static_cast<double>(neighbor.totalQueue) / localLaneCount;
        if (neighbor.phaseId >= 0 && neighbor.phaseId != selectedPhase && normalizedQueue > thresholds.vehicleCount.mediumMax) {
            return true;
        }
    }
    return false;
}

SimMetrics run_single_simulation(
    bool useRlPolicy,
    bool useNeighborCoordination,
    const traffic::NeighborCoordConfig& neighborConfig,
    int steps,
    std::uint32_t seed
) {
    std::mt19937 rng(seed);
    std::poisson_distribution<int> arrivals(1.2);

    std::vector<traffic::Lane> lanes = {
        {0, 8, 64.0, 0.0, false},
        {1, 5, 40.0, 0.0, false},
        {2, 7, 56.0, 0.0, false},
        {3, 4, 32.0, 0.0, false}
    };

    std::vector<traffic::Action> phases = {
        {0, {0, 2}},
        {1, {1, 3}}
    };

    const traffic::PhaseConfig phaseConfig = traffic::loadPhaseConfig();
    const std::vector<int> availableLaneIds = {0, 1, 2, 3};
    const auto configuredPhases = traffic::resolveConfiguredPhases(1, availableLaneIds, phaseConfig);
    if (!configuredPhases.empty()) {
        phases = configuredPhases;
    }

    const traffic::LaneConflictConfig laneConflicts = traffic::loadLaneConflictConfigForIntersection(1);
    traffic::Junction junction(1, lanes, phases, 5.0, 30.0, laneConflicts.conflictPairs);
    
    // Strategy Pattern: בחר את הבקר המתאים
    std::shared_ptr<traffic::RLAgent> rlAgent;
    std::shared_ptr<traffic::IController> controller;
    
    if (useRlPolicy) {
        const traffic::TrafficThresholdConfig thresholds = traffic::loadTrafficThresholdConfigForIntersection(1);
        rlAgent = std::make_shared<traffic::RLAgent>(traffic::RLConfig{}, thresholds, neighborConfig);
        const std::uint32_t modeSalt = useNeighborCoordination ? 0x9E3779B9u : 0x85EBCA6Bu;
        rlAgent->setRandomSeed(seed ^ modeSalt);
        controller = std::make_shared<traffic::RLController>(rlAgent);
    } else {
        controller = std::make_shared<traffic::BaselineController>(phases);
    }

    std::vector<int> queueByLane = {8, 5, 7, 4};
    const int saturationPerGreenSec = 2;
    const double stepSec = 1.0;

    int totalDeparted = 0;
    int phaseSwitches = 0;
    int maxQueue = 0;
    double waitingIntegral = 0.0;

    for (int t = 0; t < steps; ++t) {
        const bool emergencyActive = (t >= 120 && t < 130);
        std::optional<int> emergencyLane = emergencyActive ? std::optional<int>(2) : std::nullopt;
        junction.setEmergencySignal(emergencyActive, emergencyLane);

        for (int laneId = 0; laneId < static_cast<int>(queueByLane.size()); ++laneId) {
            const bool emergencyOnLane = emergencyActive && emergencyLane.has_value() && laneId == *emergencyLane;
            const double density = std::clamp(static_cast<double>(queueByLane[laneId]) * 8.0, 0.0, 100.0);
            junction.updateLaneObservation(laneId, queueByLane[laneId], emergencyOnLane, density);
        }

        traffic::JunctionState prevState = junction.currentState();
        if (useNeighborCoordination) {
            prevState = add_simulated_neighbors(std::move(prevState), t);
        }
        const std::optional<int> emergencyPhase = junction.resolveEmergencyPhase();

        // Strategy Pattern: קרא לבקר המתאים — הוא יודע להחליט!
        int actionPhaseId = controller->selectAction(prevState, junction.validPhases(), emergencyPhase);

        const int phaseBeforeApply = junction.activePhaseId();
        (void)junction.applyPhase(actionPhaseId, static_cast<double>(t));
        const int appliedPhase = junction.activePhaseId();
        if (phaseBeforeApply >= 0 && appliedPhase != phaseBeforeApply) {
            ++phaseSwitches;
        }
        if (phaseBeforeApply < 0 && appliedPhase >= 0) {
            ++phaseSwitches;
        }

        int stepDeparted = 0;
        for (int laneId = 0; laneId < static_cast<int>(queueByLane.size()); ++laneId) {
            const bool isGreen = phase_contains_lane(phases, appliedPhase, laneId);
            if (isGreen) {
                const int departed = std::min(queueByLane[laneId], saturationPerGreenSec);
                queueByLane[laneId] -= departed;
                stepDeparted += departed;
            }

            const int arrived = std::max(0, arrivals(rng));
            queueByLane[laneId] += arrived;
            maxQueue = std::max(maxQueue, queueByLane[laneId]);

            const bool emergencyOnLane = emergencyActive && emergencyLane.has_value() && laneId == *emergencyLane;
            const double density = std::clamp(static_cast<double>(queueByLane[laneId]) * 8.0, 0.0, 100.0);
            junction.updateLaneObservation(laneId, queueByLane[laneId], emergencyOnLane, density);
        }

        junction.tick(stepSec);
        totalDeparted += stepDeparted;

        traffic::JunctionState nextState = junction.currentState();
        if (useNeighborCoordination) {
            nextState = add_simulated_neighbors(std::move(nextState), t + 1);
        }
        waitingIntegral += std::accumulate(nextState.waitingTimes.begin(), nextState.waitingTimes.end(), 0.0);

        if (useRlPolicy && rlAgent) {
            bool emergencyLaneGotGreen = false;
            if (prevState.emergencyVehicleActive && prevState.emergencyLaneId.has_value()) {
                emergencyLaneGotGreen = phase_contains_lane(phases, appliedPhase, *prevState.emergencyLaneId);
            }

            bool greenSyncedWithNeighbor = false;
            bool greenOppositeToNeighbor = false;
            if (useNeighborCoordination) {
                greenSyncedWithNeighbor = has_busy_synced_neighbor(prevState, appliedPhase, rlAgent->thresholdConfig());
                greenOppositeToNeighbor = has_busy_opposing_neighbor(prevState, appliedPhase, rlAgent->thresholdConfig());
            }

            const double reward = rlAgent->computeReward(
                prevState,
                nextState,
                appliedPhase,
                emergencyLaneGotGreen,
                greenSyncedWithNeighbor,
                greenOppositeToNeighbor,
                stepSec
            );

            rlAgent->update(prevState, appliedPhase, reward, nextState, junction.validPhases());
            rlAgent->decayExploration();
        }
    }

    SimMetrics m;
    m.avgWaitingSec = waitingIntegral / static_cast<double>(steps * queueByLane.size());
    m.maxQueue = maxQueue;
    m.throughput = totalDeparted;
    m.phaseSwitches = phaseSwitches;
    return m;
}

void print_metrics(const char* name, const SimMetrics& m) {
    std::cout << name
              << " | avg_wait=" << m.avgWaitingSec
              << " | max_queue=" << m.maxQueue
              << " | throughput=" << m.throughput
              << " | phase_switches=" << m.phaseSwitches
              << "\n";
}

} // namespace

void run_simulation_comparison_verbose() {
    std::cout << "\n=== Simulation: Baseline vs RL ===\n";
    constexpr int kSteps = 300;

    const std::vector<std::uint32_t> seeds = {12345u, 23456u, 34567u, 45678u, 56789u};

    const traffic::NeighborCoordConfig defaultNeighborCfg = traffic::loadNeighborCoordConfig();

    SimMetrics baselineSum;
    SimMetrics rlNoCoordSum;
    for (std::uint32_t seed : seeds) {
        baselineSum = add_metrics(baselineSum, run_single_simulation(false, false, defaultNeighborCfg, kSteps, seed));
        rlNoCoordSum = add_metrics(rlNoCoordSum, run_single_simulation(true, false, defaultNeighborCfg, kSteps, seed));
    }

    std::vector<traffic::NeighborCoordConfig> candidateProfiles;
    candidateProfiles.push_back(load_profile_from_paths({
        "neighbor_tuning_conservative.json",
        "../neighbor_tuning_conservative.json",
        "../../neighbor_tuning_conservative.json",
        "cpp/neighbor_tuning_conservative.json",
    }));
    candidateProfiles.push_back(load_profile_from_paths({
        "neighbor_tuning_balanced.json",
        "../neighbor_tuning_balanced.json",
        "../../neighbor_tuning_balanced.json",
        "cpp/neighbor_tuning_balanced.json",
    }));
    candidateProfiles.push_back(load_profile_from_paths({
        "neighbor_tuning_aggressive.json",
        "../neighbor_tuning_aggressive.json",
        "../../neighbor_tuning_aggressive.json",
        "cpp/neighbor_tuning_aggressive.json",
    }));

    std::vector<std::pair<traffic::NeighborCoordConfig, SimMetrics>> profileResults;
    profileResults.reserve(candidateProfiles.size());

    for (const auto& profile : candidateProfiles) {
        SimMetrics sum;
        for (std::uint32_t seed : seeds) {
            sum = add_metrics(sum, run_single_simulation(true, true, profile, kSteps, seed));
        }
        profileResults.push_back({profile, scale_metrics(sum, 1.0 / static_cast<double>(seeds.size()))});
    }

    const double invN = 1.0 / static_cast<double>(seeds.size());
    const SimMetrics baseline = scale_metrics(baselineSum, invN);
    const SimMetrics rlNoCoord = scale_metrics(rlNoCoordSum, invN);

    std::cout << "(averaged over " << seeds.size() << " seeds)\n";

    print_metrics("Baseline", baseline);
    print_metrics("RL(no-neighbor)", rlNoCoord);

    const auto bestIt = std::min_element(
        profileResults.begin(),
        profileResults.end(),
        [](const auto& a, const auto& b) {
            return profile_score(a.second) < profile_score(b.second);
        }
    );

    for (const auto& [profile, metrics] : profileResults) {
        std::string label = "RL(neighbor:" + profile.profileName + ")";
        print_metrics(label.c_str(), metrics);
    }

    if (bestIt != profileResults.end()) {
        const double bestScore = profile_score(bestIt->second);
        const double noNeighborScore = profile_score(rlNoCoord);
        const double scoreDelta = noNeighborScore - bestScore;

        std::cout << "Best neighbor profile: " << bestIt->first.profileName
                  << " | source=" << bestIt->first.source
                  << " | score=" << bestScore
                  << " | delta_vs_no_neighbor=" << scoreDelta
                  << "\n";

        if (bestScore + 1e-9 < noNeighborScore) {
            std::cout << "Neighbor coordination check: PASS\n";
        } else {
            std::cout << "Neighbor coordination check: FAIL\n";
        }
    }

    std::cout << "\nExpected: best RL(neighbor:profile) should beat RL(no-neighbor) after tuning.\n";
}

SimulationResult run_simulation_comparison() {
    SimulationResult result;

    constexpr int kSteps = 150; // Shorter for regression test
    const std::vector<std::uint32_t> seeds = {12345u, 23456u}; // Just 2 seeds for speed

    const traffic::NeighborCoordConfig defaultNeighborCfg = traffic::loadNeighborCoordConfig();

    SimMetrics baselineSum;
    SimMetrics rlNoCoordSum;
    for (std::uint32_t seed : seeds) {
        baselineSum = add_metrics(baselineSum, run_single_simulation(false, false, defaultNeighborCfg, kSteps, seed));
        rlNoCoordSum = add_metrics(rlNoCoordSum, run_single_simulation(true, false, defaultNeighborCfg, kSteps, seed));
    }

    std::vector<traffic::NeighborCoordConfig> candidateProfiles;
    candidateProfiles.push_back(load_profile_from_paths({
        "neighbor_tuning_conservative.json",
        "../neighbor_tuning_conservative.json",
        "../../neighbor_tuning_conservative.json",
        "cpp/neighbor_tuning_conservative.json",
    }));
    candidateProfiles.push_back(load_profile_from_paths({
        "neighbor_tuning_balanced.json",
        "../neighbor_tuning_balanced.json",
        "../../neighbor_tuning_balanced.json",
        "cpp/neighbor_tuning_balanced.json",
    }));
    candidateProfiles.push_back(load_profile_from_paths({
        "neighbor_tuning_aggressive.json",
        "../neighbor_tuning_aggressive.json",
        "../../neighbor_tuning_aggressive.json",
        "cpp/neighbor_tuning_aggressive.json",
    }));

    std::vector<std::pair<traffic::NeighborCoordConfig, SimMetrics>> profileResults;
    profileResults.reserve(candidateProfiles.size());

    for (const auto& profile : candidateProfiles) {
        SimMetrics sum;
        for (std::uint32_t seed : seeds) {
            sum = add_metrics(sum, run_single_simulation(true, true, profile, kSteps, seed));
        }
        profileResults.push_back({profile, scale_metrics(sum, 1.0 / static_cast<double>(seeds.size()))});
    }

    const double invN = 1.0 / static_cast<double>(seeds.size());
    const SimMetrics rlNoCoord = scale_metrics(rlNoCoordSum, invN);
    result.noNeighborScore = profile_score(rlNoCoord);

    const auto bestIt = std::min_element(
        profileResults.begin(),
        profileResults.end(),
        [](const auto& a, const auto& b) {
            return profile_score(a.second) < profile_score(b.second);
        }
    );

    if (bestIt != profileResults.end()) {
        result.bestNeighborScore = profile_score(bestIt->second);
        result.bestProfileName = bestIt->first.profileName;
        result.scoreDelta = result.noNeighborScore - result.bestNeighborScore;
        result.neighborImprovement = result.bestNeighborScore + 1e-9 < result.noNeighborScore;
    }

    return result;
}

} // namespace traffic_sim
