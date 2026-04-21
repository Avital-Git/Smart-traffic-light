/**
 * main.cpp
 * --------
 * תוכנית ראשית של בקר התנועה ב-C++.
 * אביטל חדד | מכללת בנות בת שבע
 *
 * מטרה:
 *   1. מתחברת לשרת FastAPI
 *   2. מקבלת מצב צמתים
 *   3. מפעילה Q-Learning agent לבחירת פעולה
 *   4. שולחת את הפעולה חזרה
 *
 * הרצה: cmake --build . && ./smart_traffic_controller
 */

#include "rl_agent.h"
#include "http_client.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace smart_traffic;

/// הדגמה של הבקר עם נתונים לדוגמה (בלי שרת)
void demo_without_server() {
    std::cout << "\n=== DEMO MODE (no server) ===\n\n";

    MultiAgentController controller;

    // צומת 1: 4 נתיבים
    {
        auto& agent = controller.get_or_create_agent(1, 4);
        IntersectionState state;
        state.intersection_id = 1;
        state.num_lanes = 4;
        state.timestamp = 1.0;
        state.lanes = {
            {0, 12, 1, 35.0, 10.0},
            {1, 8, 0, 20.0, 5.0},
            {2, 5, 2, 45.0, 3.0},
            {3, 3, 0, 10.0, 2.0},
        };

        auto rl_vec = state.to_rl_vector();
        std::cout << "Intersection #1 (4 lanes) | RL vector size: " << rl_vec.size() << "\n";
        std::cout << "  RL vector: [";
        for (size_t i = 0; i < rl_vec.size(); i++) {
            if (i > 0) std::cout << ", ";
            std::cout << rl_vec[i];
        }
        std::cout << "]\n";

        Action action = controller.select_action(state);
        std::cout << "  Action: " << action.to_string() << "\n";
        std::cout << "  Total vehicles: " << state.total_vehicles() << "\n\n";
    }

    // צומת 2: 3 נתיבים
    {
        auto& agent = controller.get_or_create_agent(2, 3);
        IntersectionState state;
        state.intersection_id = 2;
        state.num_lanes = 3;
        state.timestamp = 1.0;
        state.lanes = {
            {0, 6, 3, 60.0, 8.0},
            {1, 2, 0, 15.0, 1.0},
            {2, 9, 1, 80.0, 12.0},
        };

        auto rl_vec = state.to_rl_vector();
        std::cout << "Intersection #2 (3 lanes) | RL vector size: " << rl_vec.size() << "\n";

        Action action = controller.select_action(state);
        std::cout << "  Action: " << action.to_string() << "\n\n";
    }

    // צומת 3: חירום!
    {
        controller.get_or_create_agent(3, 4);
        IntersectionState state;
        state.intersection_id = 3;
        state.num_lanes = 4;
        state.timestamp = 1.0;
        state.emergency_signal = {true, 2, "AMB-001", 1.0};
        state.lanes = {
            {0, 5, 0, 25.0, 3.0},
            {1, 8, 1, 40.0, 6.0},
            {2, 3, 0, 15.0, 2.0},  // נתיב 2 — רכב חירום!
            {3, 7, 0, 35.0, 5.0},
        };

        Action action = controller.select_action(state);
        std::cout << "Intersection #3 (EMERGENCY on lane 2)\n";
        std::cout << "  Action: " << action.to_string() << "  <-- priority!\n\n";
    }

    // Q-Learning training loop demo
    std::cout << "=== Q-Learning Training Demo (100 episodes) ===\n";
    auto& agent = controller.get_or_create_agent(99, 4);
    IntersectionState prev_state;
    prev_state.intersection_id = 99;
    prev_state.num_lanes = 4;
    prev_state.lanes = {{0,5,0,30,3}, {1,8,1,50,6}, {2,3,0,15,2}, {3,7,0,35,5}};

    for (int ep = 0; ep < 100; ep++) {
        Action a = agent.select_action(prev_state);
        // Simulate next state
        IntersectionState next_state = prev_state;
        if (a.green_lane_id >= 0 && a.green_lane_id < 4) {
            next_state.lanes[a.green_lane_id].vehicle_count =
                std::max(0, next_state.lanes[a.green_lane_id].vehicle_count - 3);
            next_state.lanes[a.green_lane_id].waiting_time_sec = 0;
        }
        double reward = QLearningAgent::compute_reward(prev_state, next_state, a.green_lane_id);
        std::string sk = agent.state_key(prev_state);
        std::string nsk = agent.state_key(next_state);
        agent.update(sk, a.green_lane_id, reward, nsk);
        prev_state = next_state;
        // Re-add vehicles
        for (auto& l : prev_state.lanes) l.vehicle_count += rand() % 4;
    }
    std::cout << "  Training complete. Agent learned from 100 episodes.\n";
    Action final_a = agent.select_action(prev_state);
    std::cout << "  Final action: " << final_a.to_string() << "\n";
}


/// מצב עם חיבור לשרת FastAPI
void run_with_server(const std::string& host, int port) {
    std::cout << "\n=== CONNECTED MODE ===\n";
    std::cout << "Server: " << host << ":" << port << "\n\n";

    HttpClient client(host, port);
    MultiAgentController controller;

    // בדיקת חיבור
    std::string health = client.get("/health");
    if (health.empty()) {
        std::cerr << "Cannot connect to server. Run: python -m uvicorn server.app:app --reload\n";
        return;
    }
    std::cout << "Health: " << health << "\n\n";

    // לולאה — קורא מצב, מחליט פעולה
    for (int i = 0; i < 10; i++) {
        std::string resp = client.get("/intersection/1");
        if (!resp.empty()) {
            std::cout << "State: " << resp.substr(0, 120) << "...\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}


int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "  Smart Traffic Controller (C++)\n";
    std::cout << "  Q-Learning + Rule-Based Agent\n";
    std::cout << "========================================\n";

    if (argc > 1 && std::string(argv[1]) == "--server") {
        std::string host = argc > 2 ? argv[2] : "127.0.0.1";
        int port = argc > 3 ? std::atoi(argv[3]) : 8000;
        run_with_server(host, port);
    } else {
        demo_without_server();
    }

    return 0;
}
