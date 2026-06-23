#pragma once

#include "../RLAgent.h"

#include <string>

namespace traffic {

struct RuntimeJunctionConfig {
    double minGreenSec = 5.0;
    double maxGreenSec = 60.0;
    double yellowSec = 2.0;
    double allRedSec = 1.0;
};

struct RuntimeConfig {
    RLConfig rl;
    RuntimeJunctionConfig junction;
    std::string source = "defaults";
};

RuntimeConfig loadRuntimeConfig(const std::string& preferredPath = "");

} // namespace traffic