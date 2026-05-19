#include "ConflictConfig.h"

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

std::optional<std::string> extract_json_array(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t kpos = json.find(token);
    if (kpos == std::string::npos) return std::nullopt;

    std::size_t pos = json.find('[', kpos);
    if (pos == std::string::npos) return std::nullopt;

    int depth = 0;
    const std::size_t start = pos;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '[') {
            ++depth;
        } else if (json[pos] == ']') {
            --depth;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);
            }
        }
    }

    return std::nullopt;
}

std::vector<std::pair<int, int>> parse_conflict_pairs(const std::string& json) {
    std::vector<std::pair<int, int>> pairs;

    const auto conflictsArray = extract_json_array(json, "conflicts");
    if (!conflictsArray.has_value()) {
        return pairs;
    }

    std::regex pairRe("\\[\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*\\]");
    auto begin = std::sregex_iterator(conflictsArray->begin(), conflictsArray->end(), pairRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        int a = -1;
        int b = -1;
        try {
            a = std::stoi((*it)[1].str());
            b = std::stoi((*it)[2].str());
        } catch (...) {
            continue;
        }

        if (a < 0 || b < 0 || a == b) continue;
        if (a > b) std::swap(a, b);
        pairs.emplace_back(a, b);
    }

    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

std::string resolve_lane_conflicts_path(const std::string& preferredPath) {
    if (!preferredPath.empty()) {
        return preferredPath;
    }

    if (const char* envPath = std::getenv("TRAFFIC_CONFLICTS_FILE"); envPath != nullptr) {
        const std::string candidate = envPath;
        if (!candidate.empty()) {
            return candidate;
        }
    }

    const std::vector<std::string> candidates = {
        "lane_conflicts.json",
        "cpp/lane_conflicts.json",
        "../lane_conflicts.json",
        "../cpp/lane_conflicts.json",
        "../../lane_conflicts.json",
        "../../cpp/lane_conflicts.json",
    };

    for (const auto& candidate : candidates) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }

    return "lane_conflicts.json";
}

} // namespace

LaneConflictConfig loadLaneConflictConfig(const std::string& preferredPath) {
    LaneConflictConfig cfg;

    const std::string path = resolve_lane_conflicts_path(preferredPath);
    const auto json = read_all_text(path);
    if (!json.has_value()) {
        cfg.source = "defaults (missing: " + path + ")";
        return cfg;
    }

    cfg.conflictPairs = parse_conflict_pairs(*json);
    cfg.source = path;
    return cfg;
}

LaneConflictConfig loadLaneConflictConfigForIntersection(int intersectionId, const std::string& preferredPath) {
    if (const char* envPath = std::getenv("TRAFFIC_CONFLICTS_FILE"); envPath != nullptr) {
        const std::string globalOverride = envPath;
        if (!globalOverride.empty()) {
            return loadLaneConflictConfig(globalOverride);
        }
    }

    if (!preferredPath.empty()) {
        return loadLaneConflictConfig(preferredPath);
    }

    if (intersectionId >= 0) {
        const std::string candidateA = "lane_conflicts_" + std::to_string(intersectionId) + ".json";
        if (file_exists(candidateA)) {
            return loadLaneConflictConfig(candidateA);
        }

        const std::string candidateB = "cpp/lane_conflicts_" + std::to_string(intersectionId) + ".json";
        if (file_exists(candidateB)) {
            return loadLaneConflictConfig(candidateB);
        }
    }

    return loadLaneConflictConfig();
}

} // namespace traffic
