#include "ThresholdConfig.h"
#include "server/Database.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

namespace traffic {
namespace {

TrafficThresholdConfig defaults_config() {
    return TrafficThresholdConfig{};
}

bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream in(path);
    return in.good();
}

std::optional<std::string> read_all_text(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::optional<std::string> extract_json_object(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t kpos = json.find(token);
    if (kpos == std::string::npos) return std::nullopt;

    std::size_t pos = json.find('{', kpos);
    if (pos == std::string::npos) return std::nullopt;

    int depth = 0;
    const std::size_t start = pos;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '{') {
            ++depth;
        } else if (json[pos] == '}') {
            --depth;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<double> extract_double_field(const std::string& json, const std::string& key) {
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        try {
            return std::stod(m[1].str());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void clamp_and_fix(ThreeLevelThresholds& t) {
    t.lowMax = std::max(0.0, t.lowMax);
    t.mediumMax = std::max(0.0, t.mediumMax);
    if (t.mediumMax < t.lowMax) {
        t.mediumMax = t.lowMax;
    }
}

void apply_vehicle_thresholds(const std::string& rootJson, TrafficThresholdConfig& cfg) {
    const auto obj = extract_json_object(rootJson, "vehicle_count");
    if (!obj.has_value()) return;

    if (const auto light = extract_double_field(*obj, "light_max"); light.has_value()) {
        cfg.vehicleCount.lowMax = *light;
    } else if (const auto low = extract_double_field(*obj, "low_max"); low.has_value()) {
        cfg.vehicleCount.lowMax = *low;
    }

    if (const auto medium = extract_double_field(*obj, "medium_max"); medium.has_value()) {
        cfg.vehicleCount.mediumMax = *medium;
    }
}

void apply_waiting_thresholds(const std::string& rootJson, TrafficThresholdConfig& cfg) {
    const auto obj = extract_json_object(rootJson, "waiting_time_sec");
    if (!obj.has_value()) return;

    if (const auto shortMax = extract_double_field(*obj, "short_max"); shortMax.has_value()) {
        cfg.waitingTimeSec.lowMax = *shortMax;
    } else if (const auto low = extract_double_field(*obj, "low_max"); low.has_value()) {
        cfg.waitingTimeSec.lowMax = *low;
    }

    if (const auto medium = extract_double_field(*obj, "medium_max"); medium.has_value()) {
        cfg.waitingTimeSec.mediumMax = *medium;
    }
}

void apply_density_thresholds(const std::string& rootJson, TrafficThresholdConfig& cfg) {
    const auto obj = extract_json_object(rootJson, "density_pct");
    if (!obj.has_value()) return;

    if (const auto low = extract_double_field(*obj, "low_max"); low.has_value()) {
        cfg.densityPct.lowMax = *low;
    }

    if (const auto medium = extract_double_field(*obj, "medium_max"); medium.has_value()) {
        cfg.densityPct.mediumMax = *medium;
    }
}

} // namespace

std::string resolveThresholdConfigPath(const std::string& preferredPath) {
    if (!preferredPath.empty()) {
        return preferredPath;
    }

    if (const char* envPath = std::getenv("TRAFFIC_THRESHOLDS_FILE"); envPath != nullptr) {
        const std::string candidate = envPath;
        if (!candidate.empty()) {
            return candidate;
        }
    }

    const std::vector<std::string> candidates = {
        "traffic_thresholds.json",
        "cpp/traffic_thresholds.json",
        "../traffic_thresholds.json",
        "../cpp/traffic_thresholds.json",
        "../../traffic_thresholds.json",
        "../../cpp/traffic_thresholds.json",
    };

    for (const auto& candidate : candidates) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }

    // Default lookup target even if absent, so callers can log meaningful path.
    return "traffic_thresholds.json";
}

TrafficThresholdConfig loadTrafficThresholdConfig(const std::string& preferredPath) {
    TrafficThresholdConfig cfg = defaults_config();

    const std::string path = resolveThresholdConfigPath(preferredPath);
    const auto json = read_all_text(path);
    if (!json.has_value()) {
        cfg.source = "defaults (missing: " + path + ")";
        return cfg;
    }

    apply_vehicle_thresholds(*json, cfg);
    apply_waiting_thresholds(*json, cfg);
    apply_density_thresholds(*json, cfg);

    clamp_and_fix(cfg.vehicleCount);
    clamp_and_fix(cfg.waitingTimeSec);
    clamp_and_fix(cfg.densityPct);

    cfg.source = path;
    return cfg;
}

TrafficThresholdConfig loadTrafficThresholdConfigForIntersection(int intersectionId, const std::string& preferredPath) {
    TrafficThresholdConfig cfg = loadTrafficThresholdConfig(preferredPath);

    if (const char* envPath = std::getenv("TRAFFIC_THRESHOLDS_FILE"); envPath != nullptr) {
        const std::string globalOverride = envPath;
        if (!globalOverride.empty()) {
            return loadTrafficThresholdConfig(globalOverride);
        }
    }

    if (!preferredPath.empty()) {
        return loadTrafficThresholdConfig(preferredPath);
    }

    if (intersectionId >= 0) {
        const std::string candidateA = "traffic_thresholds_" + std::to_string(intersectionId) + ".json";
        if (file_exists(candidateA)) {
            return loadTrafficThresholdConfig(candidateA);
        }

        const std::string candidateB = "cpp/traffic_thresholds_" + std::to_string(intersectionId) + ".json";
        if (file_exists(candidateB)) {
            return loadTrafficThresholdConfig(candidateB);
        }
    }

    return cfg;
}

} // namespace traffic
