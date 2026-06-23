#include "RLAgent.h"
#include "ConflictConfig.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace traffic {
namespace {

constexpr double kStrongStarvationBoost = 50.0;

std::uint64_t conflict_key(int laneA, int laneB) {
    if (laneA > laneB) std::swap(laneA, laneB);
    const std::uint64_t a = static_cast<std::uint32_t>(laneA);
    const std::uint64_t b = static_cast<std::uint32_t>(laneB);
    return (a << 32) | b;
}

const std::unordered_set<std::uint64_t>& global_conflicts() {
    static const std::unordered_set<std::uint64_t> conflicts = [] {
        const auto cfg = loadLaneConflictConfig();
        std::unordered_set<std::uint64_t> out;
        out.reserve(cfg.conflictPairs.size());
        for (const auto& [a, b] : cfg.conflictPairs) {
            if (a < 0 || b < 0 || a == b) continue;
            out.insert(conflict_key(a, b));
        }
        return out;
    }();
    return conflicts;
}

bool action_is_safe(const Action& action) {
    std::unordered_set<int> seen;
    for (int lane : action.greenLanes) {
        if (!seen.insert(lane).second) {
            return false;
        }
    }

    const auto& conflicts = global_conflicts();
    for (std::size_t i = 0; i < action.greenLanes.size(); ++i) {
        for (std::size_t j = i + 1; j < action.greenLanes.size(); ++j) {
            if (conflicts.find(conflict_key(action.greenLanes[i], action.greenLanes[j])) != conflicts.end()) {
                return false;
            }
        }
    }
    return true;
}

struct DecisionContext {
    const JunctionState& state;
    const std::vector<Action>& allActions;
    std::vector<Action> candidateActions;
    std::optional<int> emergencyPhase;
    const TrafficThresholdConfig& thresholds;
};

struct DecisionResult {
    bool decided = false;
    int phaseId = -1;
    std::unordered_map<int, double> phaseBoost;
};

class IRule {
public:
    virtual ~IRule() = default;
    virtual void apply(DecisionContext& ctx, DecisionResult& result) const = 0;
};

class EmergencyRule final : public IRule {
public:
    void apply(DecisionContext& ctx, DecisionResult& result) const override {
        if (result.decided) return;
        if (!ctx.state.emergencyVehicleActive) return;

        // Freeze policy during emergency:
        // Prefer a phase that contains the emergency lane and keeps the
        // largest mutually-safe green set static.
        if (ctx.state.emergencyLaneId.has_value()) {
            const int emergencyLane = *ctx.state.emergencyLaneId;

            std::unordered_map<int, std::size_t> laneIndexById;
            laneIndexById.reserve(ctx.state.laneIds.size());
            for (std::size_t i = 0; i < ctx.state.laneIds.size(); ++i) {
                laneIndexById[ctx.state.laneIds[i]] = i;
            }

            bool found = false;
            std::size_t bestGreenCount = 0;
            int bestTotalQueued = -1;
            double bestTotalWaiting = -1.0;
            int bestPhaseId = -1;

            for (const auto& action : ctx.allActions) {
                if (std::find(action.greenLanes.begin(), action.greenLanes.end(), emergencyLane) == action.greenLanes.end()) {
                    continue;
                }

                const std::size_t greenCount = action.greenLanes.size();
                int totalQueued = 0;
                double totalWaiting = 0.0;
                for (int laneId : action.greenLanes) {
                    const auto it = laneIndexById.find(laneId);
                    if (it == laneIndexById.end()) continue;
                    const std::size_t idx = it->second;
                    if (idx < ctx.state.vehicleCounts.size()) {
                        totalQueued += std::max(0, ctx.state.vehicleCounts[idx]);
                    }
                    if (idx < ctx.state.waitingTimes.size()) {
                        totalWaiting += std::max(0.0, ctx.state.waitingTimes[idx]);
                    }
                }

                const bool better =
                    (!found) ||
                    (greenCount > bestGreenCount) ||
                    (greenCount == bestGreenCount && totalQueued > bestTotalQueued) ||
                    (greenCount == bestGreenCount && totalQueued == bestTotalQueued && totalWaiting > bestTotalWaiting) ||
                    (greenCount == bestGreenCount && totalQueued == bestTotalQueued &&
                        std::abs(totalWaiting - bestTotalWaiting) < 1e-9 && action.phaseId < bestPhaseId);

                if (better) {
                    found = true;
                    bestGreenCount = greenCount;
                    bestTotalQueued = totalQueued;
                    bestTotalWaiting = totalWaiting;
                    bestPhaseId = action.phaseId;
                }
            }

            if (found) {
                result.decided = true;
                result.phaseId = bestPhaseId;
                return;
            }
        }

        // Fallback: use externally-resolved emergency phase when present.
        if (ctx.emergencyPhase.has_value()) {
            result.decided = true;
            result.phaseId = *ctx.emergencyPhase;
        }
    }
};

class MutualExclusionRule final : public IRule {
public:
    void apply(DecisionContext& ctx, DecisionResult& result) const override {
        if (result.decided) return;
        std::vector<Action> safe;
        safe.reserve(ctx.candidateActions.size());
        for (const auto& action : ctx.candidateActions) {
            if (action_is_safe(action)) {
                safe.push_back(action);
            }
        }
        ctx.candidateActions = std::move(safe);
    }
};

class StarvationRule final : public IRule {
public:
    void apply(DecisionContext& ctx, DecisionResult& result) const override {
        if (result.decided) return;
        if (ctx.state.laneIds.empty() || ctx.state.waitingTimes.empty() || ctx.state.vehicleCounts.empty()) return;

        const std::size_t n = std::min({ctx.state.laneIds.size(), ctx.state.waitingTimes.size(), ctx.state.vehicleCounts.size()});
        std::size_t longestIdx = 0;
        double longestWait = -1.0;
        bool hasQueuedLane = false;

        for (std::size_t i = 0; i < n; ++i) {
            if (ctx.state.vehicleCounts[i] <= 0) {
                continue;
            }

            hasQueuedLane = true;
            if (ctx.state.waitingTimes[i] > longestWait) {
                longestWait = ctx.state.waitingTimes[i];
                longestIdx = i;
            }
        }

        if (!hasQueuedLane) {
            return;
        }

        if (longestWait > ctx.thresholds.waitingTimeSec.mediumMax) {
            const int longestLaneId = ctx.state.laneIds[longestIdx];
            for (const auto& action : ctx.candidateActions) {
                if (std::find(action.greenLanes.begin(), action.greenLanes.end(), longestLaneId) != action.greenLanes.end()) {
                    result.decided = true;
                    result.phaseId = action.phaseId;
                    return;
                }
            }
            return;
        }

        for (std::size_t i = 0; i < n; ++i) {
            if (ctx.state.vehicleCounts[i] <= 0) {
                continue;
            }

            if (ctx.state.waitingTimes[i] <= ctx.thresholds.waitingTimeSec.lowMax) {
                continue;
            }

            const int laneId = ctx.state.laneIds[i];
            for (const auto& action : ctx.candidateActions) {
                if (std::find(action.greenLanes.begin(), action.greenLanes.end(), laneId) != action.greenLanes.end()) {
                    result.phaseBoost[action.phaseId] += kStrongStarvationBoost;
                }
            }
        }
    }
};

class DeadlockRule final : public IRule {
public:
    void apply(DecisionContext& ctx, DecisionResult& result) const override {
        if (result.decided) return;
        if (!ctx.candidateActions.empty()) return;
        if (ctx.allActions.empty()) return;

        const std::size_t n = std::min(ctx.state.laneIds.size(), ctx.state.vehicleCounts.size());
        if (n == 0) {
            result.decided = true;
            result.phaseId = ctx.allActions.front().phaseId;
            return;
        }

        std::size_t mostLoadedIdx = 0;
        int maxLoad = std::numeric_limits<int>::min();
        for (std::size_t i = 0; i < n; ++i) {
            const int load = std::max(0, ctx.state.vehicleCounts[i]);
            if (load > maxLoad) {
                maxLoad = load;
                mostLoadedIdx = i;
            }
        }

        const int laneId = ctx.state.laneIds[mostLoadedIdx];
        for (const auto& action : ctx.allActions) {
            if (std::find(action.greenLanes.begin(), action.greenLanes.end(), laneId) != action.greenLanes.end()) {
                result.decided = true;
                result.phaseId = action.phaseId;
                return;
            }
        }

        result.decided = true;
        result.phaseId = ctx.allActions.front().phaseId;
    }
};

class DecisionTree final {
public:
    DecisionTree() {
        rules_.push_back(std::make_unique<EmergencyRule>());
        rules_.push_back(std::make_unique<MutualExclusionRule>());
        rules_.push_back(std::make_unique<StarvationRule>());
        rules_.push_back(std::make_unique<DeadlockRule>());
    }

    DecisionResult evaluate(DecisionContext& ctx) const {
        DecisionResult result;
        for (const auto& rule : rules_) {
            rule->apply(ctx, result);
            if (result.decided) {
                return result;
            }
        }
        return result;
    }

private:
    std::vector<std::unique_ptr<IRule>> rules_;
};

} // namespace

RLAgent::RLAgent(RLConfig cfg, TrafficThresholdConfig thresholds, NeighborCoordConfig neighborConfig)
    : cfg_(cfg), thresholds_(std::move(thresholds)), neighborCfg_(std::move(neighborConfig)) {}

int RLAgent::selectAction(
    const JunctionState& state,
    const std::vector<Action>& validActions,
    std::optional<int> emergencyPhase
) {
    if (validActions.empty()) return -1;

    DecisionContext ctx{state, validActions, validActions, emergencyPhase, thresholds_};
    const DecisionTree tree;
    const DecisionResult decision = tree.evaluate(ctx);

    if (decision.decided) {
        return decision.phaseId;
    }

    if (ctx.candidateActions.empty()) {
        return -1;
    }

    const std::string key = encodeState(state);
    auto& q = qValuesFor(key, validActions.size());

    std::vector<int> candidateIndices;
    candidateIndices.reserve(ctx.candidateActions.size());
    for (const auto& action : ctx.candidateActions) {
        const int idx = actionIndexByPhaseId(validActions, action.phaseId);
        if (idx >= 0) {
            candidateIndices.push_back(idx);
        }
    }
    if (candidateIndices.empty()) {
        return -1;
    }

    std::uniform_real_distribution<double> p(0.0, 1.0);
    if (p(rng_) < cfg_.epsilon) {
        std::uniform_int_distribution<int> pick(0, static_cast<int>(candidateIndices.size()) - 1);
        const int idx = candidateIndices[pick(rng_)];
        return phaseIdByActionIndex(validActions, idx);
    }

    double bestQ = -std::numeric_limits<double>::infinity();
    std::vector<int> bestIndices;
    bestIndices.reserve(candidateIndices.size());

    for (int idx : candidateIndices) {
        const int phaseId = phaseIdByActionIndex(validActions, idx);
        const auto boostIt = decision.phaseBoost.find(phaseId);
        const double boost = (boostIt != decision.phaseBoost.end()) ? boostIt->second : 0.0;
        const double value = q[idx] + boost;

        if (value > bestQ + 1e-12) {
            bestQ = value;
            bestIndices.clear();
            bestIndices.push_back(idx);
        } else if (std::abs(value - bestQ) < 1e-12) {
            bestIndices.push_back(idx);
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
    // Emergency preemption is handled as a hard safety override (freeze state).
    // Do not train Q-values on these forced decisions.
    if (prevState.emergencyVehicleActive || nextState.emergencyVehicleActive) return;
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

            const int rawCount = std::max(0, state.vehicleCounts[idx]);
            if (rawCount <= 0) {
                continue;
            }

            const int vehicleLevel = discretizeThreeLevel(static_cast<double>(rawCount), thresholds_.vehicleCount);
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