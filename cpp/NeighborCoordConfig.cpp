#include "NeighborCoordConfig.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>

namespace traffic {
namespace {

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

std::optional<std::string> extract_string_field(const std::string& json, const std::string& key) {
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        return m[1].str();
    }
    return std::nullopt;
}

std::optional<bool> extract_bool_field(const std::string& json, const std::string& key) {
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        return m[1].str() == "true";
    }
    return std::nullopt;
}

void clamp_weights(NeighborCoordConfig& cfg) {
    cfg.rewardSyncBonus = std::max(0.0, cfg.rewardSyncBonus);
    cfg.rewardOpposingPenalty = std::max(0.0, cfg.rewardOpposingPenalty);
    cfg.rewardSyncWhenQueueWorseScale = std::clamp(cfg.rewardSyncWhenQueueWorseScale, 0.0, 2.0);
    cfg.rewardOpposingWhenQueueWorseScale = std::clamp(cfg.rewardOpposingWhenQueueWorseScale, 0.0, 3.0);
    cfg.rewardOpposingWhenQueueBetterScale = std::clamp(cfg.rewardOpposingWhenQueueBetterScale, 0.0, 2.0);

    cfg.ruleSyncWeight = std::max(0.0, cfg.ruleSyncWeight);
    cfg.ruleEmergencyBonus = std::max(0.0, cfg.ruleEmergencyBonus);
    cfg.ruleOpposingWeight = std::max(0.0, cfg.ruleOpposingWeight);
    cfg.localWeightDivisor = std::max(1.0, cfg.localWeightDivisor);
}

} // namespace

std::string resolveNeighborCoordConfigPath(const std::string& preferredPath) {
    if (!preferredPath.empty()) {
        return preferredPath;
    }

    if (const char* envPath = std::getenv("TRAFFIC_NEIGHBOR_TUNING_FILE"); envPath != nullptr) {
        const std::string candidate = envPath;
        if (!candidate.empty()) {
            return candidate;
        }
    }

    const std::string candidates[] = {
        "neighbor_tuning_balanced.json",
        "cpp/neighbor_tuning_balanced.json",
        "../neighbor_tuning_balanced.json",
        "../../neighbor_tuning_balanced.json",
    };

    for (const auto& candidate : candidates) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }

    return "neighbor_tuning_balanced.json";
}

NeighborCoordConfig loadNeighborCoordConfig(const std::string& preferredPath) {
    NeighborCoordConfig cfg;

    const std::string path = resolveNeighborCoordConfigPath(preferredPath);
    const auto json = read_all_text(path);
    if (!json.has_value()) {
        cfg.source = "defaults (missing: " + path + ")";
        return cfg;
    }

    if (const auto profileName = extract_string_field(*json, "profile_name"); profileName.has_value()) {
        cfg.profileName = *profileName;
    }

    if (const auto v = extract_bool_field(*json, "include_neighbor_in_state_encoding"); v.has_value()) cfg.includeNeighborInStateEncoding = *v;
    if (const auto v = extract_bool_field(*json, "include_neighbor_in_rule_scoring"); v.has_value()) cfg.includeNeighborInRuleScoring = *v;
    if (const auto v = extract_bool_field(*json, "include_neighbor_in_reward"); v.has_value()) cfg.includeNeighborInReward = *v;

    if (const auto v = extract_double_field(*json, "reward_sync_bonus"); v.has_value()) cfg.rewardSyncBonus = *v;
    if (const auto v = extract_double_field(*json, "reward_opposing_penalty"); v.has_value()) cfg.rewardOpposingPenalty = *v;
    if (const auto v = extract_double_field(*json, "reward_sync_when_queue_worse_scale"); v.has_value()) cfg.rewardSyncWhenQueueWorseScale = *v;
    if (const auto v = extract_double_field(*json, "reward_opposing_when_queue_worse_scale"); v.has_value()) cfg.rewardOpposingWhenQueueWorseScale = *v;
    if (const auto v = extract_double_field(*json, "reward_opposing_when_queue_better_scale"); v.has_value()) cfg.rewardOpposingWhenQueueBetterScale = *v;
    if (const auto v = extract_double_field(*json, "rule_sync_weight"); v.has_value()) cfg.ruleSyncWeight = *v;
    if (const auto v = extract_double_field(*json, "rule_emergency_bonus"); v.has_value()) cfg.ruleEmergencyBonus = *v;
    if (const auto v = extract_double_field(*json, "rule_opposing_weight"); v.has_value()) cfg.ruleOpposingWeight = *v;
    if (const auto v = extract_double_field(*json, "local_weight_divisor"); v.has_value()) cfg.localWeightDivisor = *v;

    clamp_weights(cfg);
    cfg.source = path;
    return cfg;
}

} // namespace traffic
