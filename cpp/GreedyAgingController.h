#pragma once

#include "Controller.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace traffic {

/**
 * GreedyAgingController — pure-heuristic controller used by the C++ HTTP
 * server's fallback path and as an alternative to the RL agent.
 *
 * Scoring (per lane) matches the Python `decide_action()` heuristic so that
 * the React dashboard sees identical labels regardless of which side of the
 * hybrid stack produced the action:
 *
 *     score = vehicle_count * w_count
 *           + density_pct   * w_density
 *           + waiting_sec   * w_aging
 *
 * The phase whose green lanes include the highest-scoring lane wins. Ties
 * are broken in favour of the lower phase id (deterministic).
 *
 * Emergency handling is delegated to the caller via `emergencyPhase`, which
 * (when present) is returned immediately — emergency preemption always wins.
 */
class GreedyAgingController : public IController {
public:
    struct Weights {
        double count   = 1.5;
        double density = 0.5;
        double aging   = 0.2;
    };

    explicit GreedyAgingController(Weights w = {}) : w_(w) {}

    int selectAction(
        const JunctionState& state,
        const std::vector<Action>& validPhases,
        std::optional<int> emergencyPhase) override
    {
        if (emergencyPhase.has_value()) return *emergencyPhase;
        if (validPhases.empty()) return -1;

        // 1. Score every lane in the incoming state.
        const size_t n = state.laneIds.size();
        if (n == 0) return validPhases.front().phaseId;

        double bestLaneScore = -std::numeric_limits<double>::infinity();
        int    bestLaneId    = state.laneIds.front();

        for (size_t i = 0; i < n; ++i) {
            const double count = (i < state.vehicleCounts.size())   ? static_cast<double>(state.vehicleCounts[i])   : 0.0;
            const double dens  = (i < state.densityPercents.size()) ? state.densityPercents[i]                       : 0.0;
            const double wait  = (i < state.waitingTimes.size())    ? state.waitingTimes[i]                          : 0.0;
            const double s     = count * w_.count + dens * w_.density + wait * w_.aging;
            if (s > bestLaneScore) {
                bestLaneScore = s;
                bestLaneId    = state.laneIds[i];
            }
        }

        // 2. Pick the phase whose green lanes contain the best-scoring lane.
        //    Fall back to phase_id = (bestLaneId % 2) when no phase advertises it.
        for (const auto& phase : validPhases) {
            if (std::find(phase.greenLanes.begin(), phase.greenLanes.end(), bestLaneId)
                != phase.greenLanes.end())
            {
                return phase.phaseId;
            }
        }
        const int fallback = bestLaneId % 2 == 0 ? 0 : 1;
        for (const auto& phase : validPhases) {
            if (phase.phaseId == fallback) return phase.phaseId;
        }
        return validPhases.front().phaseId;
    }

private:
    Weights w_;
};

} // namespace traffic
