#pragma once

#include <string>

namespace traffic_sim {

struct SimulationResult {
    double bestNeighborScore = 0.0;
    double noNeighborScore = 0.0;
    double scoreDelta = 0.0;
    bool neighborImprovement = false;
    std::string bestProfileName;
};

SimulationResult run_simulation_comparison();
void run_simulation_comparison_verbose();

} // namespace traffic_sim
