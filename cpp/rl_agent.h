#pragma once
/**
 * rl_agent.h
 * ----------
 * בקר למידת חיזוק (Rule-Based + Q-Learning) למערכת רמזורים חכמה.
 * אביטל חדד | מכללת בנות בת שבע
 *
 * תומך בנתיבים דינאמיים — כל צומת יכולה להיות בעלת מספר שונה של נתיבים.
 * RL vector זהה ל-Python: (4 * num_lanes + 1)
 */

#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <random>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace smart_traffic {

// ══════════════════════════════════════════════════════
// מבני נתונים
// ══════════════════════════════════════════════════════

struct EmergencySignal {
    bool active = false;
    int lane_id = -1;
    std::string vehicle_id;
    double timestamp = 0.0;
};

struct LaneObservation {
    int lane_id = 0;
    int vehicle_count = 0;
    int pedestrian_count = 0;
    double density_pct = 0.0;
    double waiting_time_sec = 0.0;

    double compute_score(double vw = 1.5, double dw = 0.5, double ww = 0.2) const {
        return vehicle_count * vw + density_pct * dw + waiting_time_sec * ww;
    }
};

struct IntersectionState {
    int intersection_id = 0;
    int num_lanes = 0;
    double timestamp = 0.0;
    std::vector<LaneObservation> lanes;
    EmergencySignal emergency_signal;
    std::vector<int> neighbor_ids;

    std::vector<double> to_rl_vector() const {
        std::vector<double> vec;
        vec.reserve(4 * num_lanes + 1);
        for (auto& l : lanes) vec.push_back((double)l.vehicle_count);
        for (auto& l : lanes) vec.push_back((double)l.pedestrian_count);
        for (auto& l : lanes) vec.push_back(l.density_pct / 100.0);
        for (auto& l : lanes) vec.push_back(l.waiting_time_sec);
        vec.push_back(emergency_signal.active ? 1.0 : 0.0);
        return vec;
    }

    int total_vehicles() const {
        int s = 0;
        for (auto& l : lanes) s += l.vehicle_count;
        return s;
    }
};

struct Action {
    int green_lane_id;  // -1 = Hold
    std::string to_string() const {
        if (green_lane_id < 0) return "Hold";
        return "Green" + std::to_string(green_lane_id);
    }
};

// ══════════════════════════════════════════════════════
// Q-Learning Agent
// ══════════════════════════════════════════════════════

class QLearningAgent {
public:
    double alpha, gamma, epsilon;
    int num_lanes;

    QLearningAgent(int n = 4, double a = 0.1, double g = 0.95, double e = 0.15)
        : alpha(a), gamma(g), epsilon(e), num_lanes(n), rng_(42) {}

    std::string state_key(const IntersectionState& s) const {
        std::ostringstream oss;
        for (auto& l : s.lanes) oss << std::min(l.vehicle_count / 4, 2) << ",";
        oss << (s.emergency_signal.active ? "E" : "_");
        return oss.str();
    }

    Action select_action(const IntersectionState& s) {
        std::uniform_real_distribution<double> d(0.0, 1.0);
        if (d(rng_) < epsilon) {
            std::uniform_int_distribution<int> ad(-1, num_lanes - 1);
            return Action{ad(rng_)};
        }
        return best_action(state_key(s));
    }

    void update(const std::string& sk, int aid, double reward, const std::string& nsk) {
        double old_q = get_q(sk, aid);
        double new_q = old_q + alpha * (reward + gamma * max_q(nsk) - old_q);
        q_table_[sk][aid] = new_q;
    }

    static double compute_reward(const IntersectionState& prev,
                                  const IntersectionState& curr, int action_lane) {
        double r = 0.0;
        for (auto& l : curr.lanes) r -= l.waiting_time_sec * 0.1;
        if (action_lane >= 0 && action_lane < (int)prev.lanes.size()
            && action_lane < (int)curr.lanes.size()) {
            r += (prev.lanes[action_lane].vehicle_count
                  - curr.lanes[action_lane].vehicle_count) * 2.0;
        }
        if (curr.emergency_signal.active && action_lane == curr.emergency_signal.lane_id)
            r += 20.0;
        return r;
    }

private:
    std::map<std::string, std::map<int, double>> q_table_;
    std::mt19937 rng_;

    double get_q(const std::string& k, int a) const {
        auto it = q_table_.find(k);
        if (it == q_table_.end()) return 0.0;
        auto it2 = it->second.find(a);
        return it2 != it->second.end() ? it2->second : 0.0;
    }
    double max_q(const std::string& k) const {
        auto it = q_table_.find(k);
        if (it == q_table_.end()) return 0.0;
        double mx = -1e9;
        for (auto& [_, v] : it->second) mx = std::max(mx, v);
        return mx < -1e8 ? 0.0 : mx;
    }
    Action best_action(const std::string& k) const {
        auto it = q_table_.find(k);
        if (it == q_table_.end()) return Action{0};
        int best = 0; double bv = -1e9;
        for (auto& [a, v] : it->second) if (v > bv) { bv = v; best = a; }
        return Action{best};
    }
};

// ══════════════════════════════════════════════════════
// בקר ראשי
// ══════════════════════════════════════════════════════

class MultiAgentController {
public:
    Action select_action(const IntersectionState& state) {
        if (state.emergency_signal.active && state.emergency_signal.lane_id >= 0)
            return Action{state.emergency_signal.lane_id};
        auto it = agents_.find(state.intersection_id);
        if (it != agents_.end()) return it->second.select_action(state);
        return rule_based(state);
    }

    QLearningAgent& get_or_create_agent(int id, int num_lanes) {
        if (agents_.find(id) == agents_.end()) {
            agents_.emplace(id, QLearningAgent(num_lanes));
            std::cout << "[Controller] Q-agent created: intersection "
                      << id << " (" << num_lanes << " lanes)\n";
        }
        return agents_[id];
    }

private:
    std::map<int, QLearningAgent> agents_;

    Action rule_based(const IntersectionState& s) const {
        int best = 0; double bs = -1;
        for (auto& l : s.lanes) {
            double sc = l.compute_score();
            if (sc > bs) { bs = sc; best = l.lane_id; }
        }
        return Action{best};
    }
};

}  // namespace smart_traffic
