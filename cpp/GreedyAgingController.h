#pragma once

#include "Controller.h"
#include "TrafficConstants.h"  // קבועי מערכת — kGreedyWeight*

#include <algorithm>
#include <limits>
#include <vector>

namespace traffic {

/**
 * GreedyAgingController — בקר היוריסטי טהור המשמש כנתיב fallback של שרת ה-C++
 * וכחלופה לסוכן ה-RL.
 *
 * חישוב הציון (לכל נתיב) תואם את ההיוריסטיקה של Python `decide_action()`,
 * כך שדשבורד ה-React יראה תוויות זהות ללא קשר לאיזה צד של המחסנית ייצר את הפעולה:
 *
 *     score = vehicle_count * w_count
 *           + density_pct   * w_density
 *           + waiting_sec   * w_aging
 *
 * הפאזה שהנתיבים הירוקים שלה כוללים את הנתיב עם הציון הגבוה ביותר מנצחת.
 * שוויון נשבר לטובת ה-phase id הנמוך יותר (דטרמיניסטי).
 *
 * טיפול בחירום מועבר לקורא דרך `emergencyPhase` —
 * כשהיא קיימת מוחזרת מיד; חירום תמיד גובר על כל שיקול אחר.
 */
class GreedyAgingController : public IController {
public:
    struct Weights {// משקלות לחישוב הציון של כל נתיב
        double count   = constants::kGreedyWeightVehicleCount;
        double density = constants::kGreedyWeightDensityPct;
        double aging   = constants::kGreedyWeightWaitingSec;
    };

    explicit GreedyAgingController(Weights w = {}) : w_(w) {}

    int selectAction(// בוחר את הפאזה הבאה בהתאם לאסטרטגיה של הבקר
        const JunctionState& state,
        const std::vector<Action>& validPhases,
        std::optional<int> emergencyPhase) override// בוחר את הפאזה הבאה בהתאם לאסטרטגיה של הבקר
    {
        if (emergencyPhase.has_value()) return *emergencyPhase;// אם יש מצב חירום, מחזיר את הפאזה לחירום
        if (validPhases.empty()) return -1;// אין פאזות תקפות

        // שלב 1: חישוב ציון לכל נתיב במצב הנוכחי
        const size_t n = state.laneIds.size();// מספר הנתיבים בצומת
        if (n == 0) return validPhases.front().phaseId;// אין נתיבים — מחזיר פאזה ראשונה

        double bestLaneScore = -std::numeric_limits<double>::infinity();// הציון הגבוה ביותר שנמצא עד כה
        int    bestLaneId    = state.laneIds.front();// מזהה הנתיב בעל הציון הגבוה ביותר

        for (size_t i = 0; i < n; ++i) {
            const double count = (i < state.vehicleCounts.size())   ? static_cast<double>(state.vehicleCounts[i])   : 0.0;// כמות כלי רכב
            const double dens  = (i < state.densityPercents.size()) ? state.densityPercents[i]                       : 0.0;// אחוז צפיפות
            const double wait  = (i < state.waitingTimes.size())    ? state.waitingTimes[i]                          : 0.0;// זמן המתנה
            const double s     = count * w_.count + dens * w_.density + wait * w_.aging;// ציון כולל
            if (s > bestLaneScore) {
                bestLaneScore = s;
                bestLaneId    = state.laneIds[i];
            }
        }

        // שלב 2: בחירת הפאזה שהנתיבים הירוקים שלה כוללים את הנתיב עם הציון הגבוה ביותר
        //        fallback: phase_id = (bestLaneId % 2) אם אף פאזה לא מכילה אותו
        for (const auto& phase : validPhases) {
            if (std::find(phase.greenLanes.begin(), phase.greenLanes.end(), bestLaneId)
                != phase.greenLanes.end())
            {
                return phase.phaseId;// נמצאה פאזה מתאימה
            }
        }
        const int fallback = bestLaneId % 2 == 0 ? 0 : 1;// fallback לפי זוגיות מזהה הנתיב
        for (const auto& phase : validPhases) {
            if (phase.phaseId == fallback) return phase.phaseId;
        }
        return validPhases.front().phaseId;// ברירת מחדל: הפאזה הראשונה
    }

private:
    Weights w_;// משקלות לחישוב הציון של כל נתיב
};

} // namespace traffic
