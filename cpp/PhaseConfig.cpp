#include "PhaseConfig.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace traffic {
namespace {

bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream in(path);
    return in.good();
}

std::string resolve_phase_config_path(const std::string& preferredPath) {
    if (!preferredPath.empty()) {
        return preferredPath;
    }

    if (const char* envPath = std::getenv("TRAFFIC_PHASES_FILE"); envPath != nullptr) {
        const std::string candidate = envPath;
        if (!candidate.empty()) return candidate;
    }

    const std::vector<std::string> candidates = {
        "traffic_phases.json",
        "cpp/traffic_phases.json",
        "../traffic_phases.json",
        "../cpp/traffic_phases.json",
        "../../traffic_phases.json",
        "../../cpp/traffic_phases.json",
    };

    for (const auto& candidate : candidates) {
        if (file_exists(candidate)) return candidate;
    }

    return "traffic_phases.json";
}

std::optional<std::string> read_all_text(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::optional<std::string> extract_balanced(const std::string& text, std::size_t startPos, char openChar, char closeChar) {
    if (startPos >= text.size() || text[startPos] != openChar) return std::nullopt;

    int depth = 0;
    const std::size_t begin = startPos;
    for (std::size_t i = startPos; i < text.size(); ++i) {
        if (text[i] == openChar) {
            ++depth;
        } else if (text[i] == closeChar) {
            --depth;
            if (depth == 0) {
                return text.substr(begin, i - begin + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> extract_json_object(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t keyPos = json.find(token);
    if (keyPos == std::string::npos) return std::nullopt;

    const std::size_t openPos = json.find('{', keyPos);
    if (openPos == std::string::npos) return std::nullopt;

    return extract_balanced(json, openPos, '{', '}');
}

std::optional<std::string> extract_json_array(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t keyPos = json.find(token);
    if (keyPos == std::string::npos) return std::nullopt;

    const std::size_t openPos = json.find('[', keyPos);
    if (openPos == std::string::npos) return std::nullopt;

    return extract_balanced(json, openPos, '[', ']');
}

std::vector<int> parse_int_list(const std::string& jsonArray) {
    std::vector<int> out;

    std::regex intRe("-?\\d+");
    auto begin = std::sregex_iterator(jsonArray.begin(), jsonArray.end(), intRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        try {
            out.push_back(std::stoi((*it)[0].str()));
        } catch (...) {
            // skip malformed tokens
        }
    }

    return out;
}

std::vector<Action> parse_phases_from_intersection_object(const std::string& intersectionObj) {
    std::vector<Action> phases;

    const auto phasesArray = extract_json_array(intersectionObj, "phases");
    if (!phasesArray.has_value()) return phases;

    std::regex phaseObjRe("\\{[^\\}]*\\\"phase_id\\\"\\s*:\\s*(-?\\d+)[^\\}]*\\\"green_lanes\\\"\\s*:\\s*(\\[[^\\]]*\\])[^\\}]*\\}");
    auto begin = std::sregex_iterator(phasesArray->begin(), phasesArray->end(), phaseObjRe);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        Action action;
        action.phaseId = -1;
        try {
            action.phaseId = std::stoi((*it)[1].str());
        } catch (...) {
            continue;
        }

        action.greenLanes = parse_int_list((*it)[2].str());
        action.greenLanes.erase(
            std::remove_if(action.greenLanes.begin(), action.greenLanes.end(), [](int laneId) { return laneId < 0; }),
            action.greenLanes.end()
        );

        std::sort(action.greenLanes.begin(), action.greenLanes.end());
        action.greenLanes.erase(std::unique(action.greenLanes.begin(), action.greenLanes.end()), action.greenLanes.end());

        if (action.phaseId < 0 || action.greenLanes.empty()) continue;
        phases.push_back(std::move(action));
    }

    std::sort(phases.begin(), phases.end(), [](const Action& a, const Action& b) {
        return a.phaseId < b.phaseId;
    });

    return phases;
}

} // namespace

PhaseConfig loadPhaseConfig(const std::string& preferredPath) {
    PhaseConfig cfg;

    const std::string path = resolve_phase_config_path(preferredPath);
    const auto json = read_all_text(path);
    if (!json.has_value()) {
        cfg.source = "defaults (missing: " + path + ")";
        return cfg;
    }

    const auto intersectionsObj = extract_json_object(*json, "intersections");
    if (!intersectionsObj.has_value()) {
        cfg.source = "defaults (invalid: " + path + ")";
        return cfg;
    }

    std::regex intersectionKeyRe("\\\"(\\d+)\\\"\\s*:");
    auto begin = std::sregex_iterator(intersectionsObj->begin(), intersectionsObj->end(), intersectionKeyRe);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        int intersectionId = -1;
        try {
            intersectionId = std::stoi((*it)[1].str());
        } catch (...) {
            continue;
        }

        if (intersectionId < 0) continue;

        const std::size_t keyPos = static_cast<std::size_t>((*it).position());
        const std::size_t objectOpenPos = intersectionsObj->find('{', keyPos);
        if (objectOpenPos == std::string::npos) continue;

        const auto intersectionObj = extract_balanced(*intersectionsObj, objectOpenPos, '{', '}');
        if (!intersectionObj.has_value()) continue;

        auto phases = parse_phases_from_intersection_object(*intersectionObj);
        if (!phases.empty()) {
            cfg.phasesByIntersection[intersectionId] = std::move(phases);
        }
    }

    cfg.source = path;
    return cfg;
}

std::vector<Action> resolveConfiguredPhases(
    int intersectionId,
    const std::vector<int>& availableLaneIds,
    const PhaseConfig& config
) {
    const auto found = config.phasesByIntersection.find(intersectionId);
    if (found == config.phasesByIntersection.end()) {
        return {};
    }

    std::unordered_set<int> laneSet(availableLaneIds.begin(), availableLaneIds.end());

    std::vector<Action> filtered;
    filtered.reserve(found->second.size());

    for (const auto& action : found->second) {
        bool allLanesExist = true;
        for (int laneId : action.greenLanes) {
            if (laneSet.find(laneId) == laneSet.end()) {
                allLanesExist = false;
                break;
            }
        }
        if (!allLanesExist) continue;
        filtered.push_back(action);
    }

    return filtered;
}

} // namespace traffic
