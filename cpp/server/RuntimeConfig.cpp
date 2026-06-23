#include "RuntimeConfig.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace traffic {
namespace {

using json = nlohmann::json;

std::optional<std::string> read_text_file(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string resolve_runtime_config_path(const std::string& preferredPath) {
    if (!preferredPath.empty()) {
        return preferredPath;
    }

    const std::vector<std::string> candidates = {
        "config.production.json",
        "server/config.production.json",
        "cpp/config.production.json",
        "../config.production.json",
        "../server/config.production.json",
        "../cpp/config.production.json",
        "../../config.production.json",
        "../../server/config.production.json",
        "../../cpp/config.production.json",
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return "config.production.json";
}

void apply_rl_config(const json& root, RuntimeConfig& cfg) {
    if (!root.contains("rl") || !root["rl"].is_object()) return;
    const auto& rl = root["rl"];

    if (rl.contains("alpha") && rl["alpha"].is_number()) {
        cfg.rl.alpha = std::clamp(rl["alpha"].get<double>(), 0.0, 1.0);
    }
    if (rl.contains("gamma") && rl["gamma"].is_number()) {
        cfg.rl.gamma = std::clamp(rl["gamma"].get<double>(), 0.0, 1.0);
    }
    if (rl.contains("epsilon") && rl["epsilon"].is_number()) {
        cfg.rl.epsilon = std::clamp(rl["epsilon"].get<double>(), 0.0, 1.0);
    } else if (rl.contains("epsilon_initial") && rl["epsilon_initial"].is_number()) {
        cfg.rl.epsilon = std::clamp(rl["epsilon_initial"].get<double>(), 0.0, 1.0);
    }
    if (rl.contains("epsilon_min") && rl["epsilon_min"].is_number()) {
        cfg.rl.epsilonMin = std::clamp(rl["epsilon_min"].get<double>(), 0.0, 1.0);
    } else if (rl.contains("epsilon_final") && rl["epsilon_final"].is_number()) {
        cfg.rl.epsilonMin = std::clamp(rl["epsilon_final"].get<double>(), 0.0, 1.0);
    }
    if (rl.contains("epsilon_decay") && rl["epsilon_decay"].is_number()) {
        cfg.rl.epsilonDecay = std::clamp(rl["epsilon_decay"].get<double>(), 0.0, 1.0);
    }
}

void apply_junction_config(const json& root, RuntimeConfig& cfg) {
    if (!root.contains("junction") || !root["junction"].is_object()) return;
    const auto& junction = root["junction"];

    if (junction.contains("min_green_sec") && junction["min_green_sec"].is_number()) {
        cfg.junction.minGreenSec = std::max(0.0, junction["min_green_sec"].get<double>());
    }
    if (junction.contains("max_green_sec") && junction["max_green_sec"].is_number()) {
        cfg.junction.maxGreenSec = std::max(0.0, junction["max_green_sec"].get<double>());
    }
    if (junction.contains("yellow_sec") && junction["yellow_sec"].is_number()) {
        cfg.junction.yellowSec = std::max(0.0, junction["yellow_sec"].get<double>());
    }
    if (junction.contains("all_red_sec") && junction["all_red_sec"].is_number()) {
        cfg.junction.allRedSec = std::max(0.0, junction["all_red_sec"].get<double>());
    }
}

} // namespace

RuntimeConfig loadRuntimeConfig(const std::string& preferredPath) {
    RuntimeConfig cfg;
    const std::string path = resolve_runtime_config_path(preferredPath);
    const auto text = read_text_file(path);
    if (!text.has_value()) {
        cfg.source = "defaults (missing: " + path + ")";
        return cfg;
    }

    try {
        const json root = json::parse(*text);
        apply_rl_config(root, cfg);
        apply_junction_config(root, cfg);
        cfg.source = path;
    } catch (...) {
        cfg.source = "defaults (invalid: " + path + ")";
    }

    return cfg;
}

} // namespace traffic