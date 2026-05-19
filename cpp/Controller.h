#pragma once

#include "Junction.h"
#include "RLAgent.h"

#include <memory>
#include <vector>
#include <optional>

namespace traffic {

/**
 * IController — Interface לבקרים שונים (RL, Baseline, etc.)
 * 
 * כל בקר מימש בחירת פאזה לפי אסטרטגיה משלו.
 */
class IController {
public:
    virtual ~IController() = default;

    /**
     * בחר את הפאזה הבאה בהתאם לאסטרטגיה של הבקר.
     * 
     * @param state        מצב הצומת הנוכחי
     * @param validPhases  רשימת הפאזות האפשריות
     * @param emergencyPhase פאזה לחירום (אם קיימת)
     * @return phase ID שנבחר
     */
    virtual int selectAction(
        const JunctionState& state,
        const std::vector<Action>& validPhases,
        std::optional<int> emergencyPhase
    ) = 0;
};


/**
 * RLController — בקר המשתמש ב-Q-Learning (RL Agent)
 * 
 * משתמש בטבלת Q שנלמדה להחליט איזו פאזה היא הטובה ביותר.
 */
class RLController : public IController {
public:
    explicit RLController(std::shared_ptr<RLAgent> agent)
        : agent_(agent) {}

    int selectAction(
        const JunctionState& state,
        const std::vector<Action>& validPhases,
        std::optional<int> emergencyPhase
    ) override {
        if (!agent_) return -1;
        return agent_->selectAction(state, validPhases, emergencyPhase);
    }

private:
    std::shared_ptr<RLAgent> agent_;
};


/**
 * BaselineController — בקר עם כללים קבועים (Round-Robin)
 * 
 * עובד לפי סדר קבוע: Phase0 → Phase1 → Phase0 → ...
 * לא מסתגל למצב התנועה.
 */
class BaselineController : public IController {
public:
    explicit BaselineController(const std::vector<Action>& phases)
        : phases_(phases), fixedTargetPhase_(0) {}

    int selectAction(
        const JunctionState& state,
        const std::vector<Action>& validPhases,
        std::optional<int> emergencyPhase
    ) override {
        (void)state;          // לא משתמשים במצב
        (void)validPhases;    // לא משתמשים בפאזות חוקיות

        if (phases_.empty()) return -1;

        // השתמש ב-emergencyPhase אם קיימת בעדיפות גבוהה
        if (emergencyPhase.has_value()) {
            fixedTargetPhase_ = *emergencyPhase;
            return fixedTargetPhase_;
        }

        // אחרת, סיבוב קבוע בין הפאזות
        int nextIdx = 0;
        for (size_t i = 0; i < phases_.size(); ++i) {
            if (phases_[i].phaseId == fixedTargetPhase_) {
                nextIdx = (i + 1) % phases_.size();
                break;
            }
        }

        fixedTargetPhase_ = phases_[nextIdx].phaseId;
        return fixedTargetPhase_;
    }

private:
    const std::vector<Action>& phases_;
    int fixedTargetPhase_;
};

} // namespace traffic
