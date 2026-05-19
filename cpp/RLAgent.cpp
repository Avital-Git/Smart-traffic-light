#include "RLAgent.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <cmath>
#include <unordered_map>

namespace traffic {

RLAgent::RLAgent(RLConfig cfg, TrafficThresholdConfig thresholds, NeighborCoordConfig neighborConfig)
    : cfg_(cfg), thresholds_(std::move(thresholds)), neighborCfg_(std::move(neighborConfig)) {}

int RLAgent::selectAction(
    const JunctionState& state,
    const std::vector<Action>& validActions,
    std::optional<int> emergencyPhase
) {
    if (validActions.empty()) return -1;

    // Emergency interrupt has highest priority
    if (state.emergencyVehicleActive && emergencyPhase.has_value()) {
        return *emergencyPhase;
    }

    const std::string key = encodeState(state);
    auto& q = qValuesFor(key, validActions.size());

    std::uniform_real_distribution<double> p(0.0, 1.0);
    if (p(rng_) < cfg_.epsilon) {
        // Exploration path uses deterministic rule-based decision tree instead of pure random,
        // making thresholds operational in live policy behavior.
        return selectRuleBasedAction(state, validActions);
    }

    const double bestQ = *std::max_element(q.begin(), q.end());
    std::vector<int> bestIndices;
    bestIndices.reserve(q.size());
    for (int i = 0; i < static_cast<int>(q.size()); ++i) {
        if (std::abs(q[i] - bestQ) < 1e-12) {
            bestIndices.push_back(i);
        }
    }
    if (bestIndices.empty()) {
        return phaseIdByActionIndex(validActions, 0);
    }

    std::uniform_int_distribution<int> tiePick(0, static_cast<int>(bestIndices.size()) - 1);
    const int bestIdx = bestIndices[tiePick(rng_)];
    return phaseIdByActionIndex(validActions, bestIdx);
}

void RLAgent::update(
    const JunctionState& prevState,
    int actionPhaseId,
    double reward,
    const JunctionState& nextState,
    const std::vector<Action>& nextValidActions
) {
    if (nextValidActions.empty()) return;

    const std::string prevKey = encodeState(prevState);
    const std::string nextKey = encodeState(nextState);

    auto& qPrev = qValuesFor(prevKey, nextValidActions.size());
    auto& qNext = qValuesFor(nextKey, nextValidActions.size());

    const int aIdx = actionIndexByPhaseId(nextValidActions, actionPhaseId);
    if (aIdx < 0) return;

    const double maxNext = *std::max_element(qNext.begin(), qNext.end());
    const double tdTarget = reward + cfg_.gamma * maxNext;
    qPrev[aIdx] += cfg_.alpha * (tdTarget - qPrev[aIdx]);
}

double RLAgent::computeReward(
    const JunctionState& prevState,
    const JunctionState& nextState,
    int actionPhaseId,
    bool emergencyLaneGotGreen,
    bool greenSyncedWithNeighbor,
    bool greenOppositeToNeighbor,
    double stepSec
) const {
    constexpr double kEmergencyImmediateBonus = 100.0;
    constexpr double kEmergencyDelayPerSecPenalty = 10.0;
    constexpr double kWaitPerVehiclePerSecPenalty = 1.0;
    constexpr double kHunger60Penalty = 30.0;
    constexpr double kHunger90Penalty = 80.0;
    constexpr double kTotalQueueReductionBonus = 4.0;
    constexpr double kQueueIncreasePenalty = 3.0;
    constexpr double kMaxQueuePenalty = 0.8;
    constexpr double kMaxQueueGrowthPenalty = 4.0;
    double reward = 0.0;
    const double dt = std::max(0.0, stepSec);

    // Emergency: +100 if emergency lane got immediate green, else -10 per second delay.
    if (prevState.emergencyVehicleActive) {
        if (emergencyLaneGotGreen) {
            reward += kEmergencyImmediateBonus;
        } else {
            reward -= kEmergencyDelayPerSecPenalty * dt;
        }
    }

    int prevTotalQueue = 0;
    int nextTotalQueue = 0;
    int prevMaxQueue = 0;
    int nextMaxQueue = 0;

    for (int c : prevState.vehicleCounts) {
        const int q = std::max(0, c);
        prevTotalQueue += q;
        prevMaxQueue = std::max(prevMaxQueue, q);
    }

    // Waiting time objective + hunger penalties + queue tracking.
    const std::size_t n = std::min(nextState.vehicleCounts.size(), nextState.waitingTimes.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int q = std::max(0, nextState.vehicleCounts[i]);
        nextTotalQueue += q;
        nextMaxQueue = std::max(nextMaxQueue, q);

        reward -= static_cast<double>(q) *
                  std::max(0.0, nextState.waitingTimes[i]) *
                  kWaitPerVehiclePerSecPenalty * dt;

        if (nextState.waitingTimes[i] > thresholds_.waitingTimeSec.mediumMax) {
            reward -= kHunger90Penalty;
        } else if (nextState.waitingTimes[i] > thresholds_.waitingTimeSec.lowMax) {
            reward -= kHunger60Penalty;
        }
    }

    const int queueDelta = prevTotalQueue - nextTotalQueue;
    if (queueDelta > 0) {
        reward += static_cast<double>(queueDelta) * kTotalQueueReductionBonus;
    } else if (queueDelta < 0) {
        reward -= static_cast<double>(-queueDelta) * kQueueIncreasePenalty;
    }

    reward -= static_cast<double>(nextMaxQueue) * kMaxQueuePenalty;
    if (nextMaxQueue > prevMaxQueue) {
        reward -= static_cast<double>(nextMaxQueue - prevMaxQueue) * kMaxQueueGrowthPenalty;
    }

    (void)actionPhaseId;

    // Green wave with adjacent nodes (context-aware):
    // reward coordination mainly when local queues are not getting worse.
    if (neighborCfg_.includeNeighborInReward) {
        if (greenSyncedWithNeighbor) {
            reward += (queueDelta >= 0)
                ? neighborCfg_.rewardSyncBonus
                : (neighborCfg_.rewardSyncBonus * neighborCfg_.rewardSyncWhenQueueWorseScale);
        }
        if (greenOppositeToNeighbor) {
            reward -= (queueDelta < 0)
                ? (neighborCfg_.rewardOpposingPenalty * neighborCfg_.rewardOpposingWhenQueueWorseScale)
                : (neighborCfg_.rewardOpposingPenalty * neighborCfg_.rewardOpposingWhenQueueBetterScale);
        }
    }

    return reward;
}

void RLAgent::decayExploration() {
    cfg_.epsilon = std::max(cfg_.epsilonMin, cfg_.epsilon * cfg_.epsilonDecay);
}

double RLAgent::epsilon() const noexcept {
    return cfg_.epsilon;
}

void RLAgent::setThresholdConfig(TrafficThresholdConfig thresholds) {
    thresholds_ = std::move(thresholds);
}

const TrafficThresholdConfig& RLAgent::thresholdConfig() const noexcept {
    return thresholds_;
}

void RLAgent::setNeighborCoordConfig(NeighborCoordConfig config) {
    neighborCfg_ = std::move(config);
}

const NeighborCoordConfig& RLAgent::neighborCoordConfig() const noexcept {
    return neighborCfg_;
}

void RLAgent::setRandomSeed(std::uint32_t seed) noexcept {
    rng_.seed(seed);
}

bool RLAgent::loadQTable(const std::string& filePath) {
    std::ifstream in(filePath);
    if (!in.is_open()) {
        return false;
    }

    std::unordered_map<std::string, std::vector<double>> loaded;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;

        const std::size_t firstTab = line.find('\t');
        const std::size_t secondTab = (firstTab == std::string::npos) ? std::string::npos : line.find('\t', firstTab + 1);
        if (firstTab == std::string::npos || secondTab == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, firstTab);
        const std::string actionCountStr = line.substr(firstTab + 1, secondTab - firstTab - 1);
        const std::string valuesStr = line.substr(secondTab + 1);

        int actionCount = 0;
        try {
            actionCount = std::stoi(actionCountStr);
        } catch (...) {
            continue;
        }
        if (actionCount <= 0) continue;

        std::vector<double> q;
        q.reserve(static_cast<std::size_t>(actionCount));

        std::stringstream ss(valuesStr);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.empty()) continue;
            try {
                q.push_back(std::stod(token));
            } catch (...) {
                q.clear();
                break;
            }
        }

        if (static_cast<int>(q.size()) != actionCount) {
            continue;
        }

        loaded[key] = std::move(q);
    }

    if (!loaded.empty()) {
        qTable_ = std::move(loaded);
    }

    return true;
}

bool RLAgent::saveQTable(const std::string& filePath) const {
    std::ofstream out(filePath, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const auto& [stateKey, values] : qTable_) {
        out << stateKey << '\t' << values.size() << '\t';
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) out << ',';
            out << values[i];
        }
        out << '\n';
    }

    return out.good();
}

std::string RLAgent::encodeState(const JunctionState& s) const {
    // Discretized state encoding for efficient Q-table learning.
    // Thresholds come from runtime config (vehicle_count, waiting_time_sec, density_pct).

    std::ostringstream oss;

    // Emergency flag (0 or 1)
    oss << (s.emergencyVehicleActive ? 1 : 0) << "|";

    // Vehicle-count bucket: light / medium / heavy
    for (int c : s.vehicleCounts) {
        const int level = discretizeThreeLevel(static_cast<double>(c), thresholds_.vehicleCount);
        oss << level << ",";
    }
    oss << "|";

    // Waiting-time bucket: short / medium / long
    for (double w : s.waitingTimes) {
        const int level = discretizeThreeLevel(w, thresholds_.waitingTimeSec);
        oss << level << ",";
    }
    oss << "|";

    // Density bucket: low / medium / high
    for (double d : s.densityPercents) {
        const int level = discretizeThreeLevel(d, thresholds_.densityPct);
        oss << level << ",";
    }

    oss << "|";

    if (neighborCfg_.includeNeighborInStateEncoding) {
        const double localLaneCount = static_cast<double>(std::max<std::size_t>(1, s.laneIds.size()));
        double avgNeighborNormalizedQueue = 0.0;
        int emergencyNeighborCount = 0;
        int validPhaseNeighborCount = 0;
        int dominantPhase = -1;
        std::unordered_map<int, int> phaseHistogram;

        for (const auto& neighbor : s.neighborSignals) {
            avgNeighborNormalizedQueue += static_cast<double>(neighbor.totalQueue) / localLaneCount;
            if (neighbor.emergencyActive) {
                ++emergencyNeighborCount;
            }
            if (neighbor.phaseId >= 0) {
                ++validPhaseNeighborCount;
                ++phaseHistogram[neighbor.phaseId];
            }
        }

        if (!s.neighborSignals.empty()) {
            avgNeighborNormalizedQueue /= static_cast<double>(s.neighborSignals.size());
        }

        int dominantCount = 0;
        for (const auto& [phaseId, count] : phaseHistogram) {
            if (count > dominantCount) {
                dominantCount = count;
                dominantPhase = phaseId;
            }
        }

        const int neighborQueueLevel = discretizeThreeLevel(avgNeighborNormalizedQueue, thresholds_.vehicleCount);
        const int neighborCountLevel = discretizeThreeLevel(static_cast<double>(s.neighborSignals.size()), {0.0, 2.0});
        const int emergencyNeighborLevel = discretizeThreeLevel(static_cast<double>(emergencyNeighborCount), {0.0, 1.0});

        oss << "nq=" << neighborQueueLevel
            << ",nc=" << neighborCountLevel
            << ",ne=" << emergencyNeighborLevel
            << ",np=" << dominantPhase
            << ",nv=" << validPhaseNeighborCount;
    } else {
        oss << "nq=off";
    }

    return oss.str();
}

int RLAgent::selectRuleBasedAction(const JunctionState& state, const std::vector<Action>& validActions) const {
    if (validActions.empty()) return -1;

    std::unordered_map<int, std::size_t> laneIdToIndex;
    laneIdToIndex.reserve(state.laneIds.size());
    for (std::size_t i = 0; i < state.laneIds.size(); ++i) {
        laneIdToIndex[state.laneIds[i]] = i;
    }

    double bestScore = -std::numeric_limits<double>::infinity();
    std::vector<int> bestPhaseIds;

    for (const auto& action : validActions) {
        double phaseScore = 0.0;

        for (int laneId : action.greenLanes) {
            const auto it = laneIdToIndex.find(laneId);
            if (it == laneIdToIndex.end()) continue;

            const std::size_t idx = it->second;
            if (idx >= state.vehicleCounts.size() || idx >= state.waitingTimes.size()) continue;

            const int vehicleLevel = discretizeThreeLevel(static_cast<double>(state.vehicleCounts[idx]), thresholds_.vehicleCount);
            const int waitingLevel = discretizeThreeLevel(state.waitingTimes[idx], thresholds_.waitingTimeSec);

            int densityLevel = 0;
            if (idx < state.densityPercents.size()) {
                densityLevel = discretizeThreeLevel(state.densityPercents[idx], thresholds_.densityPct);
            }

            // Rule-based score: prioritize long waits, then queue size, then occupancy.
            phaseScore += static_cast<double>(waitingLevel) * 5.0;
            phaseScore += static_cast<double>(vehicleLevel) * 3.0;
            phaseScore += static_cast<double>(densityLevel) * 2.0;

            if (state.waitingTimes[idx] > thresholds_.waitingTimeSec.mediumMax) {
                phaseScore += 6.0; // starvation guard boost
            }
        }

        if (neighborCfg_.includeNeighborInRuleScoring) {
            const double localScore = phaseScore;
            const double localWeightFactor = std::clamp(localScore / neighborCfg_.localWeightDivisor, 0.25, 1.0);

            const double localLaneCount = static_cast<double>(std::max<std::size_t>(1, state.laneIds.size()));
            for (const auto& neighbor : state.neighborSignals) {
                const double normalizedQueue = static_cast<double>(neighbor.totalQueue) / localLaneCount;
                const int queueLevel = discretizeThreeLevel(normalizedQueue, thresholds_.vehicleCount);

                if (neighbor.phaseId == action.phaseId) {
                    phaseScore += neighborCfg_.ruleSyncWeight * static_cast<double>(queueLevel + 1) * localWeightFactor;
                    if (neighbor.emergencyActive) {
                        phaseScore += neighborCfg_.ruleEmergencyBonus * localWeightFactor;
                    }
                } else if (neighbor.phaseId >= 0) {
                    phaseScore -= neighborCfg_.ruleOpposingWeight * static_cast<double>(queueLevel) * localWeightFactor;
                }
            }
        }

        if (phaseScore > bestScore + 1e-12) {
            bestScore = phaseScore;
            bestPhaseIds.clear();
            bestPhaseIds.push_back(action.phaseId);
        } else if (std::abs(phaseScore - bestScore) <= 1e-12) {
            bestPhaseIds.push_back(action.phaseId);
        }
    }

    if (bestPhaseIds.empty()) {
        return validActions.front().phaseId;
    }

    std::uniform_int_distribution<int> tiePick(0, static_cast<int>(bestPhaseIds.size()) - 1);
    return bestPhaseIds[tiePick(rng_)];
}

int RLAgent::discretizeThreeLevel(double value, const ThreeLevelThresholds& thresholds) const {
    if (value <= thresholds.lowMax) return 0;
    if (value <= thresholds.mediumMax) return 1;
    return 2;
}

std::vector<double>& RLAgent::qValuesFor(const std::string& stateKey, std::size_t actionCount) {
    auto& q = qTable_[stateKey];
    if (q.size() != actionCount) {
        q.assign(actionCount, 0.0);
    }
    return q;
}

int RLAgent::actionIndexByPhaseId(const std::vector<Action>& validActions, int phaseId) const {
    for (int i = 0; i < static_cast<int>(validActions.size()); ++i) {
        if (validActions[i].phaseId == phaseId) return i;
    }
    return -1;
}

int RLAgent::phaseIdByActionIndex(const std::vector<Action>& validActions, int actionIndex) const {
    if (actionIndex < 0 || actionIndex >= static_cast<int>(validActions.size())) return -1;
    return validActions[actionIndex].phaseId;
}

} // namespace traffic