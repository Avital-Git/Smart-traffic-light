#pragma once

#include <vector>
#include <optional>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace traffic {

struct NeighborSignal {
    int intersectionId = -1;
    int phaseId = -1;
    int totalQueue = 0;
    double avgWaitingSec = 0.0;
    bool emergencyActive = false;
    double signedAtSec = 0.0;
    bool signatureVerified = false;
};

struct Lane {
    int id = -1;
    int vehicleCount = 0;          // exact integer count
    double densityPct = 0.0;       // 0..100 lane occupancy estimate
    double waitingTimeSec = 0.0;   // starvation prevention metric
    bool hasEmergencyVehicle = false;
};

struct JunctionState {
    std::vector<int> laneIds;            // size N (index-aligned with vectors below)
    std::vector<int> vehicleCounts;      // size N
    std::vector<double> densityPercents; // size N
    std::vector<double> waitingTimes;    // size N
    std::vector<NeighborSignal> neighborSignals;
    bool emergencyVehicleActive = false; // global flag
    std::optional<int> emergencyLaneId;  // lane carrying emergency vehicle
};

struct Action {
    int phaseId = -1;                    // refers to a valid phase/configuration
    std::vector<int> greenLanes;         // lanes that are green in this phase
};

class Junction {
public:
    Junction(
        int junctionId,
        std::vector<Lane> lanes,
        std::vector<Action> validPhases,
        double minGreenSec,
        double maxGreenSec,
        std::vector<std::pair<int, int>> conflictPairs = {},
        double yellowSec = 2.0,
        double allRedSec = 1.0
    );

    int id() const noexcept;

    // Dynamic lane updates from vision/camera pipeline
    void updateLaneObservation(int laneId, int exactVehicleCount, bool emergencyOnLane, double densityPct = 0.0);

    // Build dynamic state for RL (N lanes, no hardcoding)
    JunctionState currentState() const;

    // Safety + operational constraints
    bool canSwitchPhase(double nowSec) const;   // enforces min green time
    bool mustSwitchPhase(double nowSec) const;  // enforces max green time

    // Apply action if valid and safe
    bool applyPhase(int phaseId, double nowSec);

    // Update waiting counters based on active phase
    void tick(double deltaSec);

    // Emergency interrupt handling
    void setEmergencySignal(bool active, std::optional<int> emergencyLaneId);
    std::optional<int> resolveEmergencyPhase() const; // safest phase that clears emergency lane

    const std::vector<Action>& validPhases() const noexcept;
    int activePhaseId() const noexcept;

private:
    bool isPhaseValid(const Action& phase) const;
    bool lanesAreMutuallyExclusive(const std::vector<int>& greenLanes) const;
    bool isLaneGreen(int laneId) const;
    int laneIndexById(int laneId) const;
    std::uint64_t conflictKey(int laneA, int laneB) const;
    bool lanesConflict(int laneA, int laneB) const;

private:
    int junctionId_;
    std::vector<Lane> lanes_;
    std::vector<Action> validPhases_;

    int activePhaseId_ = -1;
    double phaseStartSec_ = 0.0;
    double lastPhaseSwitchSec_ = -1.0;
    double minGreenSec_ = 5.0;
    double maxGreenSec_ = 60.0;
    double yellowSec_ = 2.0;
    double allRedSec_ = 1.0;
    double interGreenSec_ = 3.0;

    bool emergencyActive_ = false;
    std::optional<int> emergencyLaneId_;
    std::unordered_set<std::uint64_t> conflictPairs_;
};

} // namespace traffic