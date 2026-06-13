/**
 * main.cpp
 * --------
 * Unified controller entrypoint (single RL path):
 * - traffic::Junction
 * - traffic::RLAgent
 */

#include "http_client.h"
#include "Junction.h"
#include "RLAgent.h"
#include "Simulation.h"
#include "ConflictConfig.h"
#include "GreedyAgingController.h"
#include "NeighborCoordConfig.h"
#include "PhaseConfig.h"
#include "SelfTests.h"
#include "ThresholdConfig.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <cctype>
#include <cstdint>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace {

struct ParsedLane {
    int laneId = -1;
    int vehicleCount = 0;
    double densityPct = 0.0;
    double waitingSec = 0.0;
};

struct ParsedPacketState {
    int intersectionId = 1;
    double timestamp = 0.0;
    bool emergencyActive = false;
    std::optional<int> emergencyLaneId;
    std::vector<ParsedLane> lanes;
    std::vector<traffic::NeighborSignal> neighbors;
};

struct NeighborAuthConfig {
    std::string sharedKey = "demo-neighbor-message-key";
    double maxSignatureSkewSec = 10.0;
    std::string source = "defaults";
};

std::optional<std::string> read_text_file(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

NeighborAuthConfig load_neighbor_auth_config() {
    const std::vector<std::string> candidates = {
        "neighbor_message_auth.json",
        "cpp/neighbor_message_auth.json",
        "../neighbor_message_auth.json",
        "../../neighbor_message_auth.json",
        "python/server/neighbor_message_auth.json",
    };

    NeighborAuthConfig cfg;
    for (const auto& path : candidates) {
        const auto text = read_text_file(path);
        if (!text.has_value()) continue;

        std::smatch m;
        std::regex keyRe("\\\"shared_hmac_key\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
        if (!std::regex_search(*text, m, keyRe)) {
            continue;
        }
        cfg.sharedKey = m[1].str();

        std::regex skewRe("\\\"max_signature_skew_sec\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
        if (std::regex_search(*text, m, skewRe)) {
            try {
                cfg.maxSignatureSkewSec = std::max(1.0, std::stod(m[1].str()));
            } catch (...) {
                cfg.maxSignatureSkewSec = 10.0;
            }
        }

        cfg.source = path;
        return cfg;
    }

    return cfg;
}

std::string to_hex_lower(const std::vector<unsigned char>& data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char b : data) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string to_lower_copy(std::string s) {
    for (auto& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::optional<std::string> hmac_sha256_hex(const std::string& key, const std::string& payload) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) return std::nullopt;

    DWORD objLen = 0;
    DWORD hashLen = 0;
    DWORD cbResult = 0;
    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cbResult, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::nullopt;
    }

    status = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cbResult, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::nullopt;
    }

    std::vector<unsigned char> obj(objLen);
    std::vector<unsigned char> hashBytes(hashLen);

    status = BCryptCreateHash(
        alg,
        &hash,
        obj.data(),
        objLen,
        reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
        static_cast<ULONG>(key.size()),
        0
    );
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::nullopt;
    }

    status = BCryptHashData(
        hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(payload.data())),
        static_cast<ULONG>(payload.size()),
        0
    );
    if (status >= 0) {
        status = BCryptFinishHash(hash, hashBytes.data(), hashLen, 0);
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (status < 0) return std::nullopt;
    return to_hex_lower(hashBytes);
#else
    (void)key;
    (void)payload;
    return std::nullopt;
#endif
}

bool signature_equals(std::string a, std::string b) {
    a = to_lower_copy(std::move(a));
    b = to_lower_copy(std::move(b));
    return a == b;
}

std::string canonical_neighbor_payload(const traffic::NeighborSignal& n) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(3);
    oss << n.intersectionId
        << "|" << n.phaseId
        << "|" << n.totalQueue
        << "|" << n.avgWaitingSec
        << "|" << (n.emergencyActive ? 1 : 0)
        << "|" << n.signedAtSec;
    return oss.str();
}

bool verify_neighbor_signature(
    const traffic::NeighborSignal& neighbor,
    const std::string& signature,
    const NeighborAuthConfig& auth
) {
    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    if (std::abs(now - neighbor.signedAtSec) > auth.maxSignatureSkewSec) {
        return false;
    }

    const auto expected = hmac_sha256_hex(auth.sharedKey, canonical_neighbor_payload(neighbor));
    if (!expected.has_value()) {
        return false;
    }

    return signature_equals(*expected, signature);
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
        if (json[pos] == '{') depth++;
        else if (json[pos] == '}') {
            depth--;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);
            }
        }
    }
    return std::nullopt;
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
        if (json[pos] == '[') depth++;
        else if (json[pos] == ']') {
            depth--;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<int> extract_int_field(const std::string& json, const std::string& key) {
    std::regex re("\\\"" + key + "\\\"\\s*:\\s*(-?\\d+)");
    std::smatch m;
    if (std::regex_search(json, m, re)) return std::stoi(m[1].str());
    return std::nullopt;
}

std::optional<double> extract_double_field(const std::string& json, const std::string& key) {
    std::regex re("\\\"" + key + "\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    std::smatch m;
    if (std::regex_search(json, m, re)) return std::stod(m[1].str());
    return std::nullopt;
}

std::optional<bool> extract_bool_field(const std::string& json, const std::string& key) {
    std::regex re("\\\"" + key + "\\\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (std::regex_search(json, m, re)) return m[1].str() == "true";
    return std::nullopt;
}

bool parse_packet_state(const std::string& packetJson, ParsedPacketState& out, const NeighborAuthConfig& neighborAuth) {
    const auto stateObj = extract_json_object(packetJson, "state");
    if (!stateObj.has_value()) return false;

    out.intersectionId = extract_int_field(*stateObj, "intersection_id").value_or(1);
    out.timestamp = extract_double_field(*stateObj, "timestamp").value_or(0.0);
    out.lanes.clear();
    out.neighbors.clear();

    const auto lanesArray = extract_json_array(*stateObj, "lanes");
    if (lanesArray.has_value()) {
        std::regex laneRe(
            "\\{[^\\}]*\\\"lane_id\\\"\\s*:\\s*(\\d+)"
            "[^\\}]*\\\"vehicle_count\\\"\\s*:\\s*(-?\\d+)"
            "[^\\}]*\\\"density_pct\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)"
            "[^\\}]*\\\"waiting_time_sec\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)"
            "[^\\}]*\\}");

        auto begin = std::sregex_iterator(lanesArray->begin(), lanesArray->end(), laneRe);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            ParsedLane lane;
            lane.laneId = std::stoi((*it)[1].str());
            lane.vehicleCount = std::stoi((*it)[2].str());
            lane.densityPct = std::stod((*it)[3].str());
            lane.waitingSec = std::stod((*it)[4].str());
            out.lanes.push_back(lane);
        }
    }

    out.emergencyActive = false;
    out.emergencyLaneId = std::nullopt;
    if (stateObj->find("\"emergency_signal\": null") == std::string::npos) {
        if (const auto emObj = extract_json_object(*stateObj, "emergency_signal"); emObj.has_value()) {
            out.emergencyActive = extract_bool_field(*emObj, "active").value_or(false);
            const auto lane = extract_int_field(*emObj, "lane_id");
            if (lane.has_value() && *lane >= 0) {
                out.emergencyLaneId = *lane;
            }
        }
    }

    if (const auto neighborsArray = extract_json_array(packetJson, "neighbors"); neighborsArray.has_value()) {
        std::regex neighborRe(
            "\\{[^\\}]*\\\"intersection_id\\\"\\s*:\\s*(\\d+)"
            "[^\\}]*\\\"action\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""
            "[^\\}]*\\\"phase_id\\\"\\s*:\\s*(null|-?\\d+)"
            "[^\\}]*\\\"total_queue\\\"\\s*:\\s*(-?\\d+)"
            "[^\\}]*\\\"avg_waiting_sec\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)"
            "[^\\}]*\\\"emergency_active\\\"\\s*:\\s*(true|false)"
            "[^\\}]*\\\"signed_at\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)"
            "[^\\}]*\\\"signature\\\"\\s*:\\s*\\\"([0-9a-fA-F]+)\\\""
            "[^\\}]*\\}");

        auto begin = std::sregex_iterator(neighborsArray->begin(), neighborsArray->end(), neighborRe);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            traffic::NeighborSignal neighbor;
            neighbor.intersectionId = std::stoi((*it)[1].str());
            const std::string phaseIdText = (*it)[3].str();
            neighbor.phaseId = (phaseIdText == "null") ? -1 : std::stoi(phaseIdText);
            neighbor.totalQueue = std::max(0, std::stoi((*it)[4].str()));
            neighbor.avgWaitingSec = std::max(0.0, std::stod((*it)[5].str()));
            neighbor.emergencyActive = ((*it)[6].str() == "true");
            neighbor.signedAtSec = std::stod((*it)[7].str());
            const std::string signature = (*it)[8].str();
            neighbor.signatureVerified = verify_neighbor_signature(neighbor, signature, neighborAuth);
            if (neighbor.signatureVerified) {
                out.neighbors.push_back(neighbor);
            }
        }
    }

    return !out.lanes.empty();
}

traffic::JunctionState with_neighbor_signals(traffic::JunctionState state, const std::vector<traffic::NeighborSignal>& neighbors) {
    state.neighborSignals = neighbors;
    return state;
}

bool has_busy_synced_neighbor(
    const traffic::JunctionState& state,
    int selectedPhase,
    const traffic::TrafficThresholdConfig& thresholds
) {
    const double localLaneCount = std::max<std::size_t>(1, state.laneIds.size());
    for (const auto& neighbor : state.neighborSignals) {
        const double normalizedQueue = static_cast<double>(neighbor.totalQueue) / static_cast<double>(localLaneCount);
        if (neighbor.phaseId == selectedPhase && normalizedQueue > thresholds.vehicleCount.lowMax) {
            return true;
        }
    }
    return false;
}

bool has_busy_opposing_neighbor(
    const traffic::JunctionState& state,
    int selectedPhase,
    const traffic::TrafficThresholdConfig& thresholds
) {
    const double localLaneCount = std::max<std::size_t>(1, state.laneIds.size());
    for (const auto& neighbor : state.neighborSignals) {
        const double normalizedQueue = static_cast<double>(neighbor.totalQueue) / static_cast<double>(localLaneCount);
        if (neighbor.phaseId >= 0 && neighbor.phaseId != selectedPhase && normalizedQueue > thresholds.vehicleCount.mediumMax) {
            return true;
        }
    }
    return false;
}

bool phase_contains_lane(const std::vector<traffic::Action>& validActions, int phaseId, int laneId) {
    for (const auto& phase : validActions) {
        if (phase.phaseId != phaseId) continue;
        return std::find(phase.greenLanes.begin(), phase.greenLanes.end(), laneId) != phase.greenLanes.end();
    }
    return false;
}

std::vector<traffic::Action> build_phases_from_lanes(const std::vector<ParsedLane>& lanes) {
    std::vector<int> even;
    std::vector<int> odd;
    for (const auto& lane : lanes) {
        if (lane.laneId % 2 == 0) {
            even.push_back(lane.laneId);
        } else {
            odd.push_back(lane.laneId);
        }
    }

    std::vector<traffic::Action> phases;
    if (!even.empty()) phases.push_back({0, even});
    if (!odd.empty()) phases.push_back({1, odd});

    if (phases.empty()) {
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            phases.push_back({static_cast<int>(i), {lanes[i].laneId}});
        }
    }

    return phases;
}

std::vector<int> lane_ids_from_lanes(const std::vector<ParsedLane>& lanes) {
    std::vector<int> ids;
    ids.reserve(lanes.size());
    for (const auto& lane : lanes) {
        ids.push_back(lane.laneId);
    }
    return ids;
}

traffic::LaneConflictConfig load_lane_conflicts_from_api(
    const smart_traffic::HttpClient& client,
    int intersectionId
) {
    traffic::LaneConflictConfig cfg;
    cfg.source = "/intersection/" + std::to_string(intersectionId) + "/conflicts";

    const std::string payload = client.get(cfg.source);
    if (payload.empty()) {
        cfg.source += " (empty response)";
        return cfg;
    }

    const auto conflictsArray = extract_json_array(payload, "conflicts");
    if (!conflictsArray.has_value()) {
        cfg.source += " (missing conflicts array)";
        return cfg;
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
        cfg.conflictPairs.emplace_back(a, b);
    }

    std::sort(cfg.conflictPairs.begin(), cfg.conflictPairs.end());
    cfg.conflictPairs.erase(std::unique(cfg.conflictPairs.begin(), cfg.conflictPairs.end()), cfg.conflictPairs.end());
    return cfg;
}

void print_loaded_conflicts(int intersectionId, const traffic::LaneConflictConfig& cfg) {
    std::cout << "Loaded conflict pairs for intersection " << intersectionId
              << " from " << cfg.source
              << " (pairs=" << cfg.conflictPairs.size() << ")\n";
    if (cfg.conflictPairs.empty()) {
        std::cout << "  - No conflict pairs returned by API\n";
        return;
    }

    for (const auto& p : cfg.conflictPairs) {
        std::cout << "  - conflict: lane " << p.first << " <-> lane " << p.second << "\n";
    }
}

void run_conflict_enforcement_smoke_test(
    int intersectionId,
    const traffic::LaneConflictConfig& cfg
) {
    if (cfg.conflictPairs.empty()) {
        std::cout << "[Conflict smoke test] intersection " << intersectionId
                  << ": skipped (no conflicts from API)\n";
        return;
    }

    const int a = cfg.conflictPairs.front().first;
    const int b = cfg.conflictPairs.front().second;

    std::vector<traffic::Lane> lanes = {
        {a, 0, 0.0, 0.0, false},
        {b, 0, 0.0, 0.0, false},
    };

    std::vector<traffic::Action> phases = {
        {900, {a, b}}, // must be rejected
        {901, {a}},    // should be valid
    };

    traffic::Junction testJunction(
        intersectionId,
        lanes,
        phases,
        0.0,
        10.0,
        cfg.conflictPairs
    );

    const bool blocked = !testJunction.applyPhase(900, 0.0);
    const bool allowed = testJunction.applyPhase(901, 1.0);

    std::cout << "[Conflict smoke test] pair(" << a << "," << b << ")"
              << " | phase{900:[" << a << "," << b << "]} => " << (blocked ? "BLOCKED" : "ALLOWED")
              << " | phase{901:[" << a << "]} => " << (allowed ? "ALLOWED" : "BLOCKED")
              << "\n";
}

std::string lane_topology_key(const ParsedPacketState& s) {
    std::ostringstream oss;
    oss << s.intersectionId << "|";
    for (const auto& lane : s.lanes) {
        oss << lane.laneId << ",";
    }
    return oss.str();
}

void run_with_server(const std::string& host, int port, bool useGreedyController = false) {
    std::cout << "\n=== CONNECTED MODE (" << (useGreedyController ? "Greedy-Aging" : "Unified RL") << " Path) ===\n";
    std::cout << "Server: " << host << ":" << port << "\n\n";

    smart_traffic::HttpClient client(host, port);
    const NeighborAuthConfig neighborAuth = load_neighbor_auth_config();
    const std::string health = client.get("/health");
    if (health.empty()) {
        std::cerr << "Cannot connect to server. Run FastAPI first.\n";
        return;
    }
    std::cout << "Health: " << health << "\n\n";
    std::cout << "Neighbor auth source: " << neighborAuth.source
              << " | skew=" << neighborAuth.maxSignatureSkewSec << "s\n";

    traffic::TrafficThresholdConfig thresholds = traffic::loadTrafficThresholdConfig();
    std::cout << "Threshold config: " << thresholds.source << "\n";
    const traffic::NeighborCoordConfig neighborConfig = traffic::loadNeighborCoordConfig();
    std::cout << "Neighbor tuning: " << neighborConfig.profileName
              << " (" << neighborConfig.source << ")\n";
    traffic::LaneConflictConfig laneConflicts;
    laneConflicts.source = "api (pending intersection)";
    traffic::PhaseConfig phaseConfig = traffic::loadPhaseConfig();
    std::cout << "Phase config: " << phaseConfig.source
              << " (intersections=" << phaseConfig.phasesByIntersection.size() << ")\n";
    traffic::RLAgent agent({}, thresholds, neighborConfig);
    traffic::GreedyAgingController greedy;
    int activeThresholdIntersection = -1;
    int activeConflictIntersection = -1;
    const std::string qTablePath = "qtable.tsv";
    const bool loaded = agent.loadQTable(qTablePath);
    std::cout << "Q-table load: " << (loaded ? "ok" : "new") << " (" << qTablePath << ")\n";
    std::unique_ptr<traffic::Junction> junction;
    std::vector<traffic::Action> phases;

    std::string currentTopology;
    double nowSec = 0.0;
    constexpr double kStepSec = 0.5;

    while (true) {
        const std::string packet = client.get("/intersection/1/packet");
        if (packet.empty()) {
            std::cerr << "Packet request failed\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        ParsedPacketState parsed;
        if (!parse_packet_state(packet, parsed, neighborAuth)) {
            std::cerr << "Failed to parse /packet response\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        if (parsed.intersectionId != activeThresholdIntersection) {
            thresholds = traffic::loadTrafficThresholdConfigForIntersection(parsed.intersectionId);
            agent.setThresholdConfig(thresholds);
            activeThresholdIntersection = parsed.intersectionId;
            std::cout << "Threshold config for intersection " << parsed.intersectionId
                      << ": " << thresholds.source << "\n";
        }

        if (parsed.intersectionId != activeConflictIntersection) {
            laneConflicts = load_lane_conflicts_from_api(client, parsed.intersectionId);
            activeConflictIntersection = parsed.intersectionId;
            std::cout << "Lane conflicts API for intersection " << parsed.intersectionId
                      << ": " << laneConflicts.source
                      << " (pairs=" << laneConflicts.conflictPairs.size() << ")\n";
            print_loaded_conflicts(parsed.intersectionId, laneConflicts);
            run_conflict_enforcement_smoke_test(parsed.intersectionId, laneConflicts);
        }

        const std::string topology = lane_topology_key(parsed);
        if (!junction || topology != currentTopology) {
            std::vector<traffic::Lane> lanes;
            lanes.reserve(parsed.lanes.size());
            for (const auto& l : parsed.lanes) {
                lanes.push_back({
                    l.laneId,
                    std::max(0, l.vehicleCount),
                    std::clamp(l.densityPct, 0.0, 100.0),
                    std::max(0.0, l.waitingSec),
                    parsed.emergencyActive && parsed.emergencyLaneId.has_value() && *parsed.emergencyLaneId == l.laneId
                });
            }

            phases = traffic::resolveConfiguredPhases(
                parsed.intersectionId,
                lane_ids_from_lanes(parsed.lanes),
                phaseConfig
            );
            if (phases.empty()) {
                phases = build_phases_from_lanes(parsed.lanes);
                std::cout << "Phase config fallback for intersection " << parsed.intersectionId
                          << ": using auto phase builder\n";
            }
            if (phases.empty()) {
                std::cerr << "No valid phases from packet lanes\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            junction = std::make_unique<traffic::Junction>(
                parsed.intersectionId,
                lanes,
                phases,
                3.0,   // minGreenSec: 3 seconds - enables fast switching on empty lanes
                45.0,  // maxGreenSec: 45 seconds - prevents starvation
                laneConflicts.conflictPairs
            );
            currentTopology = topology;
            nowSec = 0.0;
        }

        for (const auto& l : parsed.lanes) {
            const bool emergencyOnLane = parsed.emergencyActive && parsed.emergencyLaneId.has_value() && *parsed.emergencyLaneId == l.laneId;
            junction->updateLaneObservation(l.laneId, l.vehicleCount, emergencyOnLane, l.densityPct);
        }
        junction->setEmergencySignal(parsed.emergencyActive, parsed.emergencyLaneId);

        const traffic::JunctionState prevState = with_neighbor_signals(junction->currentState(), parsed.neighbors);
        const auto emergencyPhase = junction->resolveEmergencyPhase();

        int selectedPhase = useGreedyController
            ? greedy.selectAction(prevState, junction->validPhases(), emergencyPhase)
            : agent.selectAction(prevState, junction->validPhases(), emergencyPhase);
        if (!junction->applyPhase(selectedPhase, nowSec)) {
            selectedPhase = junction->activePhaseId();
            if (selectedPhase < 0 && !junction->validPhases().empty()) {
                selectedPhase = junction->validPhases().front().phaseId;
                (void)junction->applyPhase(selectedPhase, nowSec);
            }
        }

        bool emergencyLaneGotGreen = false;
        if (prevState.emergencyVehicleActive && prevState.emergencyLaneId.has_value()) {
            emergencyLaneGotGreen = phase_contains_lane(junction->validPhases(), selectedPhase, *prevState.emergencyLaneId);
        }

        junction->tick(kStepSec);
        nowSec += kStepSec;
        const traffic::JunctionState nextState = with_neighbor_signals(junction->currentState(), parsed.neighbors);

        const bool greenSyncedWithNeighbor = has_busy_synced_neighbor(prevState, selectedPhase, agent.thresholdConfig());
        const bool greenOppositeToNeighbor = has_busy_opposing_neighbor(prevState, selectedPhase, agent.thresholdConfig());

        const double reward = agent.computeReward(
            prevState,
            nextState,
            selectedPhase,
            emergencyLaneGotGreen,
            greenSyncedWithNeighbor,
            greenOppositeToNeighbor,
            kStepSec
        );
        agent.update(prevState, selectedPhase, reward, nextState, junction->validPhases());
        agent.decayExploration();

        const std::string actionText = (selectedPhase >= 0)
            ? ("Phase" + std::to_string(selectedPhase))
            : "Hold";

        std::ostringstream body;
        body << "{\"action\":\"" << actionText << "\",";
        body << "\"phase_id\":" << selectedPhase << ",";
        body << "\"reason\":\"cpp_rl_unified\",";
        body << "\"timestamp\":" << parsed.timestamp << "}";

        const std::string postPath = "/intersection/" + std::to_string(parsed.intersectionId) + "/action";
        const std::string postResp = client.post(postPath, body.str());

        int totalVehicles = 0;
        for (int c : prevState.vehicleCounts) totalVehicles += c;

        std::cout << "Intersection " << parsed.intersectionId
                  << " | lanes=" << parsed.lanes.size()
                  << " | vehicles=" << totalVehicles
                  << " | phase=" << selectedPhase
                  << " | action=" << actionText
                  << " | reward=" << reward
                  << " | epsilon=" << agent.epsilon()
                  << " | post=" << (postResp.empty() ? "failed" : "ok")
                  << "\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    const bool saved = agent.saveQTable(qTablePath);
    std::cout << "Q-table save: " << (saved ? "ok" : "failed") << " (" << qTablePath << ")\n";
}

void run_conflict_check_once(const std::string& host, int port, int intersectionId) {
    std::cout << "\n=== CONFLICT CHECK MODE (API) ===\n";
    std::cout << "Server: " << host << ":" << port << " | intersection=" << intersectionId << "\n\n";

    smart_traffic::HttpClient client(host, port);
    const std::string health = client.get("/health");
    if (health.empty()) {
        std::cerr << "Cannot connect to server. Run FastAPI first.\n";
        return;
    }

    traffic::LaneConflictConfig laneConflicts = load_lane_conflicts_from_api(client, intersectionId);
    print_loaded_conflicts(intersectionId, laneConflicts);
    run_conflict_enforcement_smoke_test(intersectionId, laneConflicts);
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "  Smart Traffic Controller (C++)\n";
    std::cout << "  Unified RL Path: Junction + RLAgent\n";
    std::cout << "========================================\n";

    if (argc > 1 && std::string(argv[1]) == "--server") {
        std::string host = "127.0.0.1";
        int port = 8000;
        bool useGreedy = false;
        std::vector<std::string> positional;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--greedy") useGreedy = true;
            else                 positional.push_back(a);
        }
        if (positional.size() >= 1) host = positional[0];
        if (positional.size() >= 2) port = std::atoi(positional[1].c_str());
        run_with_server(host, port, useGreedy);
    } else if (argc > 1 && std::string(argv[1]) == "--conflict-check") {
        std::string host = argc > 2 ? argv[2] : "127.0.0.1";
        int port = argc > 3 ? std::atoi(argv[3]) : 8000;
        int intersectionId = argc > 4 ? std::atoi(argv[4]) : 1;
        if (intersectionId <= 0) intersectionId = 1;
        run_conflict_check_once(host, port, intersectionId);
    } else if (argc > 1 && std::string(argv[1]) == "--selftest") {
        const bool ok = traffic_tests::run_all_self_tests();
        return ok ? 0 : 1;
    } else if (argc > 1 && std::string(argv[1]) == "--simulate") {
        traffic_sim::run_simulation_comparison_verbose();
    } else {
        std::cout << "Usage:\n";
        std::cout << "  smart_traffic_controller.exe --simulate\n";
        std::cout << "  smart_traffic_controller.exe --server [host] [port] [--greedy]\n\n";
        std::cout << "  smart_traffic_controller.exe --conflict-check [host] [port] [intersection_id]\n\n";
        std::cout << "  smart_traffic_controller.exe --selftest\n\n";
        traffic_sim::run_simulation_comparison_verbose();
    }

    return 0;
}
