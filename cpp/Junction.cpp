#include "Junction.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace traffic {

Junction::Junction(
    int junctionId,
    std::vector<Lane> lanes,
    std::vector<Action> validPhases,
    double minGreenSec,
    double maxGreenSec,
        std::vector<std::pair<int, int>> conflictPairs,
        double yellowSec,
        double allRedSec
)
    : junctionId_(junctionId),
      lanes_(std::move(lanes)),
      validPhases_(std::move(validPhases)),
      minGreenSec_(minGreenSec),
            maxGreenSec_(maxGreenSec),
            yellowSec_(std::max(0.0, yellowSec)),
            allRedSec_(std::max(0.0, allRedSec)),
            interGreenSec_(std::max(0.0, yellowSec_) + std::max(0.0, allRedSec_)) {
    for (auto [a, b] : conflictPairs) {
        if (a < 0 || b < 0 || a == b) continue;
        conflictPairs_.insert(conflictKey(a, b));
    }
}

int Junction::id() const noexcept {
    return junctionId_;
}

void Junction::updateLaneObservation(int laneId, int exactVehicleCount, bool emergencyOnLane, double densityPct) {
    const int idx = laneIndexById(laneId);
    if (idx < 0) return;

    lanes_[idx].vehicleCount = std::max(0, exactVehicleCount);
    lanes_[idx].densityPct = std::clamp(densityPct, 0.0, 100.0);
    lanes_[idx].hasEmergencyVehicle = emergencyOnLane;

    // Empty lanes should not accumulate starvation history.
    if (lanes_[idx].vehicleCount == 0) {
        lanes_[idx].waitingTimeSec = 0.0;
    }
}

JunctionState Junction::currentState() const {
    JunctionState s;
    s.laneIds.reserve(lanes_.size());
    s.vehicleCounts.reserve(lanes_.size());
    s.densityPercents.reserve(lanes_.size());
    s.waitingTimes.reserve(lanes_.size());

    for (const auto& lane : lanes_) {
        s.laneIds.push_back(lane.id);
        s.vehicleCounts.push_back(lane.vehicleCount);
        s.densityPercents.push_back(lane.densityPct);
        s.waitingTimes.push_back(lane.waitingTimeSec);
    }

    s.emergencyVehicleActive = emergencyActive_;
    s.emergencyLaneId = emergencyLaneId_;
    return s;
}

bool Junction::canSwitchPhase(double nowSec) const {
    if (activePhaseId_ < 0) return true;
    const bool minGreenReached = (nowSec - phaseStartSec_) >= minGreenSec_;
    const bool interGreenReached =
        (lastPhaseSwitchSec_ < 0.0) || ((nowSec - lastPhaseSwitchSec_) >= interGreenSec_);
    return minGreenReached && interGreenReached;
}

bool Junction::mustSwitchPhase(double nowSec) const {
    if (activePhaseId_ < 0) return false;
    return (nowSec - phaseStartSec_) >= maxGreenSec_;
}

bool Junction::applyPhase(int phaseId, double nowSec) {
    auto it = std::find_if(validPhases_.begin(), validPhases_.end(),
        [phaseId](const Action& a) { return a.phaseId == phaseId; });
    if (it == validPhases_.end()) return false;
    if (!isPhaseValid(*it)) {
        std::cerr << "[Junction " << junctionId_ << "] rejected invalid phase " << phaseId << "\n";
        return false;
    }

    if (activePhaseId_ < 0) {
        activePhaseId_ = phaseId;
        phaseStartSec_ = nowSec;
        lastPhaseSwitchSec_ = nowSec;
        return true;
    }

    if (activePhaseId_ == phaseId) return true;

    bool emergencyOverrideForMinGreen = false;
    if (((nowSec - phaseStartSec_) < minGreenSec_) && emergencyActive_ && emergencyLaneId_.has_value()) {
        const int emergencyLane = *emergencyLaneId_;
        emergencyOverrideForMinGreen =
            std::find(it->greenLanes.begin(), it->greenLanes.end(), emergencyLane) != it->greenLanes.end();
    }

    const bool interGreenReached =
        (lastPhaseSwitchSec_ < 0.0) || ((nowSec - lastPhaseSwitchSec_) >= interGreenSec_);

    // Never bypass inter-green safety guard. Emergency can only bypass min-green.
    if (!interGreenReached) return false;

    const bool minGreenReached = (nowSec - phaseStartSec_) >= minGreenSec_;
    if (!minGreenReached && !emergencyOverrideForMinGreen) return false;

    activePhaseId_ = phaseId;
    phaseStartSec_ = nowSec;
    lastPhaseSwitchSec_ = nowSec;
    return true;
}

void Junction::tick(double deltaSec) {
    for (auto& lane : lanes_) {
        if (lane.vehicleCount <= 0) {
            lane.waitingTimeSec = 0.0;
        } else if (isLaneGreen(lane.id)) {
            lane.waitingTimeSec = 0.0;
        } else {
            lane.waitingTimeSec += deltaSec;
        }
    }
}

void Junction::setEmergencySignal(bool active, std::optional<int> emergencyLaneId) {
    emergencyActive_ = active;
    emergencyLaneId_ = emergencyLaneId;
}

std::optional<int> Junction::resolveEmergencyPhase() const {
    if (!emergencyActive_ || !emergencyLaneId_.has_value()) return std::nullopt;

    const int targetLane = *emergencyLaneId_;
    bool found = false;
    std::size_t bestGreenCount = 0;
    int bestTotalQueued = -1;
    double bestTotalWaiting = -1.0;
    int bestPhaseId = -1;

    for (const auto& phase : validPhases_) {
        if (std::find(phase.greenLanes.begin(), phase.greenLanes.end(), targetLane) == phase.greenLanes.end()) continue;
        if (!isPhaseValid(phase)) continue;

        const std::size_t greenCount = phase.greenLanes.size();
        int totalQueued = 0;
        double totalWaiting = 0.0;
        for (int laneId : phase.greenLanes) {
            const int idx = laneIndexById(laneId);
            if (idx < 0) continue;
            totalQueued += std::max(0, lanes_[idx].vehicleCount);
            totalWaiting += std::max(0.0, lanes_[idx].waitingTimeSec);
        }

        const bool better =
            (!found) ||
            (greenCount > bestGreenCount) ||
            (greenCount == bestGreenCount && totalQueued > bestTotalQueued) ||
            (greenCount == bestGreenCount && totalQueued == bestTotalQueued && totalWaiting > bestTotalWaiting) ||
            (greenCount == bestGreenCount && totalQueued == bestTotalQueued &&
                std::abs(totalWaiting - bestTotalWaiting) < 1e-9 && phase.phaseId < bestPhaseId);

        if (better) {
            found = true;
            bestGreenCount = greenCount;
            bestTotalQueued = totalQueued;
            bestTotalWaiting = totalWaiting;
            bestPhaseId = phase.phaseId;
        }
    }

    if (found) return bestPhaseId;
    return std::nullopt;
}

const std::vector<Action>& Junction::validPhases() const noexcept {
    return validPhases_;
}

int Junction::activePhaseId() const noexcept {
    return activePhaseId_;
}

bool Junction::isPhaseValid(const Action& phase) const {
    for (int laneId : phase.greenLanes) {
        if (laneIndexById(laneId) < 0) {
            return false;
        }
    }

    return lanesAreMutuallyExclusive(phase.greenLanes);
}

bool Junction::lanesAreMutuallyExclusive(const std::vector<int>& greenLanes) const {
    std::unordered_set<int> uniqueLanes;
    for (int laneId : greenLanes) {
        if (!uniqueLanes.insert(laneId).second) {
            std::cerr << "[Junction " << junctionId_ << "] duplicate lane in phase: " << laneId << "\n";
            return false;
        }
    }

    for (std::size_t i = 0; i < greenLanes.size(); ++i) {
        for (std::size_t j = i + 1; j < greenLanes.size(); ++j) {
            if (lanesConflict(greenLanes[i], greenLanes[j])) {
                std::cerr << "[Junction " << junctionId_ << "] conflict blocked between lanes "
                          << greenLanes[i] << " and " << greenLanes[j] << "\n";
                return false;
            }
        }
    }

    return true;
}

bool Junction::isLaneGreen(int laneId) const {
    auto it = std::find_if(validPhases_.begin(), validPhases_.end(),
        [this](const Action& a) { return a.phaseId == activePhaseId_; });
    if (it == validPhases_.end()) return false;

    return std::find(it->greenLanes.begin(), it->greenLanes.end(), laneId) != it->greenLanes.end();
}

int Junction::laneIndexById(int laneId) const {
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
        if (lanes_[i].id == laneId) return i;
    }
    return -1;
}

std::uint64_t Junction::conflictKey(int laneA, int laneB) const {
    if (laneA > laneB) std::swap(laneA, laneB);

    const std::uint64_t a = static_cast<std::uint32_t>(laneA);
    const std::uint64_t b = static_cast<std::uint32_t>(laneB);
    return (a << 32) | b;
}

bool Junction::lanesConflict(int laneA, int laneB) const {
    if (laneA == laneB) return true;
    return conflictPairs_.find(conflictKey(laneA, laneB)) != conflictPairs_.end();
}

} // namespace traffic