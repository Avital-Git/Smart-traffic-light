#include "RLAgent.h"
#include "ConflictConfig.h"
#include "TrafficConstants.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace traffic {
using namespace traffic::constants;
namespace {
// פונקציה שמחזירה מפתח ייחודי עבור זוג נתיבים, כך שניתן לבדוק אם הם נמצאים בקונפליקט
std::uint64_t conflict_key(int laneA, int laneB) {
    if (laneA > laneB) std::swap(laneA, laneB);
    const std::uint64_t a = static_cast<std::uint32_t>(laneA);// המרת מזהה הנתיב הראשון ל-uint32_t כדי למנוע איבוד מידע
    const std::uint64_t b = static_cast<std::uint32_t>(laneB);// המרת מזהה הנתיב השני ל-uint32_t כדי למנוע איבוד מידע
    return (a << 32) | b;// שילוב מזהי הנתיבים ליצירת מפתח ייחודי לכל זוג נתיבים
}
// פונקציה שמחזירה סט של כל זוגות הנתיבים שנמצאים בקונפליקט, כך שניתן לבדוק אם נתיבים מסוימים נמצאים בקונפליקט
const std::unordered_set<std::uint64_t>& global_conflicts() {
    static const std::unordered_set<std::uint64_t> conflicts = [] {// פונקציה שמטענת את קונפיגורציית הקונפליקטים של הנתיבים ומחזירה סט של כל זוגות הנתיבים שנמצאים בקונפליקט
        const auto cfg = loadLaneConflictConfig();// טוענת את קונפיגורציית הקונפליקטים של הנתיבים
        std::unordered_set<std::uint64_t> out;// יצירת סט של כל זוגות הנתיבים שנמצאים בקונפליקט
        out.reserve(cfg.conflictPairs.size());// שמירת מקום לסט של כל זוגות הנתיבים שנמצאים בקונפליקט
        for (const auto& [a, b] : cfg.conflictPairs) {// לולאה שעוברת על כל זוגות הנתיבים שנמצאים בקונפליקט
            if (a < 0 || b < 0 || a == b) continue;// אם אחד הנתיבים אינו תקין או שהם זהים, ממשיך לזוג הבא
            out.insert(conflict_key(a, b));// מוסיף את הזוג למערך הקונפליקטים
        }
        return out;// מחזיר את הסט של כל זוגות הנתיבים שנמצאים בקונפליקט
    }();
    return conflicts;// מחזיר את הסט של כל זוגות הנתיבים שנמצאים בקונפליקט
}

bool action_is_safe(const Action& action) {// פונקציה שמחזירה האם הפאזה הנוכחית אינה מכילה נתיבים בקונפליקט אחד עם השני
    std::unordered_set<int> seen;// יצירת סט של נתיבים ייחודיים כדי לבדוק אם יש כפילויות
    for (int lane : action.greenLanes) {// לולאה שעוברת על כל הנתיבים הירוקים בפאזה הנוכחית
        if (!seen.insert(lane).second) {// אם הנתיב כבר קיים בסט, מחזיר false כי יש כפילות
            return false;
        }
    }

    const auto& conflicts = global_conflicts();// קבלת הסט של כל זוגות הנתיבים שנמצאים בקונפליקט
    for (std::size_t i = 0; i < action.greenLanes.size(); ++i) {// לולאה שעוברת על כל הנתיבים הירוקים בפאזה הנוכחית
        for (std::size_t j = i + 1; j < action.greenLanes.size(); ++j) {// לולאה פנימית שעוברת על כל הנתיבים הירוקים בפאזה הנוכחית כדי לבדוק אם יש קונפליקטים בין הנתיבים
            if (conflicts.find(conflict_key(action.greenLanes[i], action.greenLanes[j])) != conflicts.end()) {// אם הזוג נמצא בקונפליקט, מחזיר false
                return false;
            }
        }
    }
    return true;
}

struct DecisionContext {// מבנה שמכיל את המידע הדרוש לקבלת החלטה על הפאזה הבאה
    const JunctionState& state;//מצב צומת הנוכחי
    const std::vector<Action>& allActions;// כל הפעולות האפשריות בצומת
    std::vector<Action> candidateActions;// פעולות מועמדות לבחירה
    std::optional<int> emergencyPhase;// פאזה חירום אם קיימת
    const TrafficThresholdConfig& thresholds;// תצורת סף תנועה
};

struct DecisionResult {// מבנה שמכיל את התוצאה של קבלת ההחלטה על הפאזה הבאה
    bool decided = false;// האם ההחלטה התקבלה
    int phaseId = -1;// מזהה הפאזה שנבחרה
    std::unordered_map<int, double> phaseBoost;// מיפוי בין מזהי הפאזות לבין ערכי הבוסט שלהם
};
// ממשק שמייצג כלל לקבלת החלטה על הפאזה הבאה
class IRule {
public:
    virtual ~IRule() = default;// דה-קונסטרקטור וירטואלי ריק
    virtual void apply(DecisionContext& ctx, DecisionResult& result) const = 0;// פונקציה וירטואלית שמיישמת את הכלל על ההקשר הנתון ומעדכנת את התוצאה
};

class EmergencyRule final : public IRule {// כלל שמטפל במצבי חירום בצומת
public:
    void apply(DecisionContext& ctx, DecisionResult& result) const override {// פונקציה שמיישמת את הכלל על ההקשר הנתון ומעדכנת את התוצאה
        if (result.decided) return;// אם ההחלטה כבר התקבלה, מחזיר
        if (!ctx.state.emergencyVehicleActive) return;// אם אין כלי רכב חירום פעיל בצומת, מחזיר

        // Freeze policy during emergency:
        // Prefer a phase that contains the emergency lane and keeps the
        // largest mutually-safe green set static.
        if (ctx.state.emergencyLaneId.has_value()) {// אם יש מזהה נתיב חירום, בודק אם הפאזה הנוכחית מכילה את הנתיב החירום
            const int emergencyLane = *ctx.state.emergencyLaneId;// קבלת מזהה הנתיב החירום

            std::unordered_map<int, std::size_t> laneIndexById;// יצירת מיפוי בין מזהי הנתיבים לבין האינדקס שלהם ברשימת הנתיבים בצומת
            laneIndexById.reserve(ctx.state.laneIds.size());// שמירת מקום למיפוי בין מזהי הנתיבים לבין האינדקס שלהם ברשימת הנתיבים בצומת
            for (std::size_t i = 0; i < ctx.state.laneIds.size(); ++i) {// לולאה שעוברת על כל הנתיבים בצומת
                laneIndexById[ctx.state.laneIds[i]] = i;// מוסיף את הנתיב למיפוי בין מזהי הנתיבים לבין האינדקס שלהם ברשימת הנתיבים בצומת
            }

            bool found = false;// משתנה שמציין אם נמצאה פאזה מתאימה לחירום
            std::size_t bestGreenCount = 0;// מספר הנתיבים הירוקים הטוב ביותר שנמצא עד כה
            int bestTotalQueued = -1;// מספר הרכבים הממתינים הטוב ביותר שנמצא עד כה
            double bestTotalWaiting = -1.0;// זמן ההמתנה הכולל הטוב ביותר שנמצא עד כה
            int bestPhaseId = -1;// מזהה הפאזה הטוב ביותר שנמצא עד כה

            for (const auto& action : ctx.allActions) {// לולאה שעוברת על כל הפעולות האפשריות בצומת
                if (std::find(action.greenLanes.begin(), action.greenLanes.end(), emergencyLane) == action.greenLanes.end()) {
                    continue;// אם הפאזה הנוכחית אינה מכילה את הנתיב החירום, ממשיך לפאזה הבאה
                }

                const std::size_t greenCount = action.greenLanes.size();// מספר הנתיבים הירוקים בפאזה הנוכחית
                int totalQueued = 0;// מספר הרכבים הממתינים בפאזה הנוכחית
                double totalWaiting = 0.0;// זמן ההמתנה הכולל בפאזה הנוכחית
                for (int laneId : action.greenLanes) {// לולאה שעוברת על כל הנתיבים הירוקים בפאזה הנוכחית
                    const auto it = laneIndexById.find(laneId);// קבלת האינדקס של הנתיב ברשימת הנתיבים לפי מזהה הנתיב
                    if (it == laneIndexById.end()) continue;// אם הנתיב אינו קיים ברשימת הנתיבים בצומת, ממשיך לנתיב הבא
                    const std::size_t idx = it->second;// קבלת האינדקס של הנתיב ברשימת הנתיבים לפי מזהה הנתיב
                    if (idx < ctx.state.vehicleCounts.size()) {// אם האינדקס של הנתיב קטן ממספר הנתיבים ברשימת הנתיבים בצומת, מעדכן את מספר הרכבים הממתינים בפאזה הנוכחית
                        totalQueued += std::max(0, ctx.state.vehicleCounts[idx]);
                    }
                    if (idx < ctx.state.waitingTimes.size()) {// אם האינדקס של הנתיב קטן ממספר הנתיבים ברשימת הנתיבים בצומת, מעדכן את זמן ההמתנה הכולל בפאזה הנוכחית
                        totalWaiting += std::max(0.0, ctx.state.waitingTimes[idx]);
                    }
                }
//השוואה בין הפאזות שנמצאו כדי למצוא את הפאזה הבטוחה ביותר שנותנת עדיפות לחירום
                const bool better =
                    (!found) ||
                    (greenCount > bestGreenCount) ||
                    (greenCount == bestGreenCount && totalQueued > bestTotalQueued) ||
                    (greenCount == bestGreenCount && totalQueued == bestTotalQueued && totalWaiting > bestTotalWaiting) ||
                    (greenCount == bestGreenCount && totalQueued == bestTotalQueued &&
                        std::abs(totalWaiting - bestTotalWaiting) < 1e-9 && action.phaseId < bestPhaseId);
// אם הפאזה הנוכחית טובה יותר מהפאזה הטובה ביותר שנמצאה עד כה, מעדכן את המשתנים שמציינים את הפאזה הטובה ביותר
                if (better) {
                    found = true;
                    bestGreenCount = greenCount;
                    bestTotalQueued = totalQueued;
                    bestTotalWaiting = totalWaiting;
                    bestPhaseId = action.phaseId;
                }
            }
//אם נמצאה פאזה מתאימה לחירום, מחזיר את מזהה הפאזה הטובה ביותר שנמצאה
            if (found) {
                result.decided = true;
                result.phaseId = bestPhaseId;
                return;
            }
        }

        // Fallback: use externally-resolved emergency phase when present.
        if (ctx.emergencyPhase.has_value()) {// אם יש פאזה חירום שהתקבלה מבחוץ, מחזיר את מזהה הפאזה החירום
            result.decided = true;// מעדכן את המשתנה שמציין אם ההחלטה התקבלה
            result.phaseId = *ctx.emergencyPhase;// מעדכן את מזהה הפאזה שנבחרה
        }
    }
};
// כלל שמוודא שאין פעולות שמובילות למצב של חוסר אפשרות לפעולה
class MutualExclusionRule final : public IRule {
public:
    void apply(DecisionContext& ctx, DecisionResult& result) const override {// פונקציה שמיישמת את הכלל על ההקשר הנתון ומעדכנת את התוצאה
        if (result.decided) return;// אם ההחלטה כבר התקבלה, מחזיר
        std::vector<Action> safe;
        safe.reserve(ctx.candidateActions.size());// שמירת מקום למערך של פעולות בטוחות
        for (const auto& action : ctx.candidateActions) {// לולאה שעוברת על כל הפעולות המועמדות לבחירה
            if (action_is_safe(action)) {// אם הפאזה הנוכחית אינה מכילה נתיבים בקונפליקט אחד עם השני, מוסיף אותה למערך של פעולות בטוחות
                safe.push_back(action);// מוסיף את הפאזה למערך של פעולות בטוחות
            }
        }
        ctx.candidateActions = std::move(safe);// מעדכן את מערך הפעולות המועמדות לבחירה למערך של פעולות בטוחות
    }
};
// כלל שמטפל במצבי רעב בצומת
class StarvationRule final : public IRule {
public:
    void apply(DecisionContext& ctx, DecisionResult& result) const override {// פונקציה שמיישמת את הכלל על ההקשר הנתון ומעדכנת את התוצאה
        if (result.decided) return;// אם ההחלטה כבר התקבלה, מחזיר
        if (ctx.state.laneIds.empty() || ctx.state.waitingTimes.empty() || ctx.state.vehicleCounts.empty()) return;// אם אין נתונים על הנתיבים, מחזיר

        const std::size_t n = std::min({ctx.state.laneIds.size(), ctx.state.waitingTimes.size(), ctx.state.vehicleCounts.size()});// קבלת מספר הנתיבים המינימלי בין שלושת המערכים
        std::size_t longestIdx = 0;// משתנה שמציין את האינדקס של הנתיב עם זמן ההמתנה הארוך ביותר
        double longestWait = -1.0;// משתנה שמציין את זמן ההמתנה הארוך ביותר
        bool hasQueuedLane = false;// משתנה שמציין אם יש נתיב עם רכבים ממתינים

        for (std::size_t i = 0; i < n; ++i) {// לולאה שעוברת על כל הנתיבים כדי למצוא את הנתיב עם זמן ההמתנה הארוך ביותר
            if (ctx.state.vehicleCounts[i] <= 0) {// אם אין רכבים ממתינים בנתיב הנוכחי, ממשיך לנתיב הבא
                continue;
            }

            hasQueuedLane = true;// מעדכן את המשתנה שמציין אם יש נתיב עם רכבים ממתינים
            if (ctx.state.waitingTimes[i] > longestWait) {// אם זמן ההמתנה בנתיב הנוכחי ארוך יותר מהזמן הארוך ביותר שנמצא עד כה, מעדכן את המשתנים המתאימים
                longestWait = ctx.state.waitingTimes[i];
                longestIdx = i;
            }
        }

        if (!hasQueuedLane) {// אם אין נתיבים עם רכבים ממתינים, מחזיר
            return;
        }

        if (longestWait > ctx.thresholds.waitingTimeSec.mediumMax) {// אם זמן ההמתנה בנתיב הארוך ביותר גדול מהסף המקסימלי של זמן ההמתנה הבינוני, מחפש את הפאזה שמכילה את הנתיב הארוך ביותר ומחזיר אותה
            const int longestLaneId = ctx.state.laneIds[longestIdx];// קבלת מזהה הנתיב הארוך ביותר
            for (const auto& action : ctx.candidateActions) {// לולאה שעוברת על כל הפעולות המועמדות לבחירה
                if (std::find(action.greenLanes.begin(), action.greenLanes.end(), longestLaneId) != action.greenLanes.end()) {// אם הפאזה הנוכחית מכילה את הנתיב הארוך ביותר, מחזיר את מזהה הפאזה
                    result.decided = true;
                    result.phaseId = action.phaseId;
                    return;
                }
            }
            return;
        }

        for (std::size_t i = 0; i < n; ++i) {// לולאה שעוברת על כל הנתיבים כדי לבדוק אם יש נתיבים עם זמן המתנה ארוך יותר מהסף המקסימלי של זמן ההמתנה הנמוך
            if (ctx.state.vehicleCounts[i] <= 0) {// אם אין רכבים ממתינים בנתיב הנוכחי, ממשיך לנתיב הבא
                continue;
            }

            if (ctx.state.waitingTimes[i] <= ctx.thresholds.waitingTimeSec.lowMax) {// אם זמן ההמתנה בנתיב הנוכחי קטן או שווה לסף המקסימלי של זמן ההמתנה הנמוך, ממשיך לנתיב הבא
                continue;
            }

            const int laneId = ctx.state.laneIds[i];// קבלת מזהה הנתיב הנוכחי
            for (const auto& action : ctx.candidateActions) {// לולאה שעוברת על כל הפעולות המועמדות לבחירה
                if (std::find(action.greenLanes.begin(), action.greenLanes.end(), laneId) != action.greenLanes.end()) {// אם הפאזה הנוכחית מכילה את הנתיב הנוכחי, מגדיל את הבוסט של הפאזה
                    result.phaseBoost[action.phaseId] += kStrongStarvationBoost;// מוסיף בוסט חזק לפאזה כדי להעדיף אותה בבחירה
                }
            }
        }
    }
};
// כלל שמטפל במצבי קיפאון בצומת
class DeadlockRule final : public IRule {
public:
    void apply(DecisionContext& ctx, DecisionResult& result) const override {// פונקציה שמיישמת את הכלל על ההקשר הנתון ומעדכנת את התוצאה
        if (result.decided) return;// אם ההחלטה כבר התקבלה, מחזיר
        if (!ctx.candidateActions.empty()) return;// אם יש פעולות מועמדות לבחירה, מחזיר
        if (ctx.allActions.empty()) return;// אם אין פעולות כלל, מחזיר

        const std::size_t n = std::min(ctx.state.laneIds.size(), ctx.state.vehicleCounts.size());// קבלת מספר הנתיבים המינימלי בין שני המערכים
        if (n == 0) {// אם אין נתיבים כלל, מחזיר
            result.decided = true;// מעדכן את המשתנה שמציין אם ההחלטה התקבלה
            result.phaseId = ctx.allActions.front().phaseId;// מעדכן את מזהה הפאזה שנבחרה לפאזה הראשונה ברשימת כל הפעולות
            return;
        }

        std::size_t mostLoadedIdx = 0;// אינדקס הנתיב עם העומס הגבוה ביותר
        int maxLoad = std::numeric_limits<int>::min();// משתנה שמציין את העומס הגבוה ביותר שנמצא עד כה
        for (std::size_t i = 0; i < n; ++i) {// לולאה שעוברת על כל הנתיבים כדי למצוא את הנתיב עם העומס הגבוה ביותר
            const int load = std::max(0, ctx.state.vehicleCounts[i]);// קבלת העומס בנתיב הנוכחי (מספר הרכבים הממתינים)
            if (load > maxLoad) {// אם העומס בנתיב הנוכחי גדול מהעומס הגבוה ביותר שנמצא עד כה, מעדכן את המשתנים המתאימים
                maxLoad = load;
                mostLoadedIdx = i;
            }
        }

        const int laneId = ctx.state.laneIds[mostLoadedIdx];// קבלת מזהה הנתיב עם העומס הגבוה ביותר
        for (const auto& action : ctx.allActions) {// לולאה שעוברת על כל הפעולות הזמינות
            if (std::find(action.greenLanes.begin(), action.greenLanes.end(), laneId) != action.greenLanes.end()) {// אם הפאזה הנוכחית מכילה את הנתיב עם העומס הגבוה ביותר, מחזיר את מזהה הפאזה
                result.decided = true;
                result.phaseId = action.phaseId;
                return;
            }
        }

        result.decided = true;// אם לא נמצאה פאזה שמכילה את הנתיב עם העומס הגבוה ביותר, מחזיר את הפאזה הראשונה ברשימת כל הפעולות
        result.phaseId = ctx.allActions.front().phaseId;// מעדכן את מזהה הפאזה שנבחרה לפאזה הראשונה ברשימת כל הפעולות
    }
};
// מחלקה שמייצגת עץ החלטות שמכיל את כל הכללים לקבלת החלטה על הפאזה הבאה
class DecisionTree final {
public:
    DecisionTree() {
        rules_.push_back(std::make_unique<EmergencyRule>());// מוסיף את כלל החירום לעץ ההחלטות
        rules_.push_back(std::make_unique<MutualExclusionRule>());// מוסיף את כלל ההחרגה ההדדית לעץ ההחלטות
        rules_.push_back(std::make_unique<StarvationRule>());// מוסיף את כלל הרעב לעץ ההחלטות
        rules_.push_back(std::make_unique<DeadlockRule>());// מוסיף את כלל הקיפאון לעץ ההחלטות
    }
// פונקציה שמעריכה את ההקשר הנתון ומחזירה את התוצאה של קבלת ההחלטה על הפאזה הבאה
    DecisionResult evaluate(DecisionContext& ctx) const {
        DecisionResult result;// יצירת משתנה שמכיל את התוצאה של קבלת ההחלטה על הפאזה הבאה
        for (const auto& rule : rules_) {// לולאה שעוברת על כל הכללים בעץ ההחלטות
            rule->apply(ctx, result);// מיישמת את הכלל על ההקשר הנתון ומעדכנת את התוצאה
            if (result.decided) {// אם ההחלטה התקבלה, מחזיר את התוצאה
                return result;
            }
        }
        return result;
    }

private:
    std::vector<std::unique_ptr<IRule>> rules_;// מערך של כל הכללים בעץ ההחלטות
};

} // namespace
//פונקציה ששולחת קודם לעץ ההחלטה ואח"כ מחליטה את הפאזה הבאה על פי ערכי Q
RLAgent::RLAgent(RLConfig cfg, TrafficThresholdConfig thresholds)
    : cfg_(cfg), thresholds_(std::move(thresholds)) {}

int RLAgent::selectAction(// פונקציה שמקבלת את מצב הצומת הנוכחי, את כל הפעולות האפשריות בצומת ואת הפאזה החירום אם קיימת, ומחזירה את מזהה הפאזה שנבחרה
    const JunctionState& state,// מצב הצומת הנוכחי
    const std::vector<Action>& validActions,// כל הפעולות האפשריות בצומת
    std::optional<int> emergencyPhase// מזהה הפאזה החירום אם קיימת
) {
    if (validActions.empty()) return -1;// אם אין פעולות כלל, מחזיר -1

    DecisionContext ctx{state, validActions, validActions, emergencyPhase, thresholds_};// יצירת משתנה שמכיל את ההקשר לקבלת החלטה על הפאזה הבאה
    const DecisionTree tree;// יצירת עץ החלטות שמכיל את כל הכללים לקבלת החלטה על הפאזה הבאה
    const DecisionResult decision = tree.evaluate(ctx);// הערכת ההקשר הנתון והחזרת התוצאה של קבלת ההחלטה על הפאזה הבאה

    if (decision.decided) {//אם ההחלטה התקבלה על ידי העץ
        return decision.phaseId;// מחזירים את הפאזה שנבחרה
    }

    if (ctx.candidateActions.empty()) {// אם אין פעולות מועמדות לבחירה, מחזיר -1
        return -1;
    }

    const std::string key = encodeState(state);// קידוד מצב הצומת למחרוזת ייחודית לשימוש במיפוי ערכי Q
    auto& q = qValuesFor(key, validActions.size());// קבלת ערכי Q עבור מצב הצומת הנוכחי, עם גודל המערך בהתאם למספר הפעולות האפשריות בצומת

    std::vector<int> candidateIndices;// יצירת מערך של אינדקסים של הפעולות המועמדות לבחירה
    candidateIndices.reserve(ctx.candidateActions.size());// שמירת מקום במערך של אינדקסים של הפעולות המועמדות לבחירה
    for (const auto& action : ctx.candidateActions) {// לולאה שעוברת על כל הפעולות המועמדות לבחירה
        const int idx = actionIndexByPhaseId(validActions, action.phaseId);// קבלת האינדקס של הפאזה הנוכחית ברשימת כל הפעולות האפשריות בצומת
        if (idx >= 0) {// אם האינדקס של הפאזה הנוכחית תקין, מוסיף אותו למערך של אינדקסים של הפעולות המועמדות לבחירה
            candidateIndices.push_back(idx);
        }
    }
    if (candidateIndices.empty()) {// אם אין אינדקסים של פעולות מועמדות, מחזיר -1
        return -1;
    }

    std::uniform_real_distribution<double> p(0.0, 1.0);// יצירת התפלגות אחידה של מספרים רנדומליים בין 0 ל-1
    if (p(rng_) < cfg_.epsilon) {// אם המספר הרנדומלי קטן מהערך של אפסילון, בוחרים פעולה אקראית
        std::uniform_int_distribution<int> pick(0, static_cast<int>(candidateIndices.size()) - 1);// יצירת התפלגות אחידה של מספרים רנדומליים בין 0 למספר האינדקסים של הפעולות המועמדות לבחירה
        const int idx = candidateIndices[pick(rng_)];// קבלת אינדקס רנדומלי של פעולה מועמדת לבחירה
        return phaseIdByActionIndex(validActions, idx);// מחזירים את מזהה הפאזה של הפעולה שנבחרה
    }

    double bestQ = -std::numeric_limits<double>::infinity();// משתנה שמציין את ערך Q הטוב ביותר שנמצא עד כה
    std::vector<int> bestIndices;// יצירת מערך של אינדקסים של הפעולות עם ערך Q הטוב ביותר
    bestIndices.reserve(candidateIndices.size());// שמירת מקום במערך של אינדקסים של הפעולות עם ערך Q הטוב ביותר

    for (int idx : candidateIndices) {// לולאה שעוברת על כל האינדקסים של הפעולות המועמדות לבחירה
        const int phaseId = phaseIdByActionIndex(validActions, idx);// קבלת מזהה הפאזה של הפעולה הנוכחית
        const auto boostIt = decision.phaseBoost.find(phaseId);// חיפוש בוסט לפאזה הנוכחית במפה של הבוסט לפאזות
        const double boost = (boostIt != decision.phaseBoost.end()) ? boostIt->second : 0.0;// אם נמצא בוסט, משתמשים בו, אחרת 0
        const double value = q[idx] + boost;// חישוב הערך הכולל של הפאזה הנוכחית על ידי חיבור ערך Q והבוסט
        if (value > bestQ + 1e-12) {// אם הערך הכולל של הפאזה הנוכחית גדול מהערך הטוב ביותר שנמצא עד כה, מעדכנים את המשתנים המתאימים
            bestQ = value;
            bestIndices.clear();
            bestIndices.push_back(idx);
        } else if (std::abs(value - bestQ) < 1e-12) {// אם הערך הכולל של הפאזה הנוכחית שווה לערך הטוב ביותר שנמצא עד כה, מוסיפים את האינדקס למערך של אינדקסים של הפעולות עם ערך Q הטוב ביותר
            bestIndices.push_back(idx);
        }
    }

    if (bestIndices.empty()) {// אם אין אינדקסים של פעולות עם ערך Q הטוב ביותר, מחזיר את הפאזה הראשונה ברשימת כל הפעולות האפשריות בצומת
        return phaseIdByActionIndex(validActions, 0);
    }

    std::uniform_int_distribution<int> tiePick(0, static_cast<int>(bestIndices.size()) - 1);// יצירת התפלגות אחידה של מספרים רנדומליים בין 0 למספר האינדקסים של הפעולות עם ערך Q הטוב ביותר
    const int bestIdx = bestIndices[tiePick(rng_)];// קבלת אינדקס רנדומלי של פעולה עם ערך Q הטוב ביותר
    return phaseIdByActionIndex(validActions, bestIdx);// מחזירים את מזהה הפאזה של הפעולה עם ערך Q הטוב ביותר
}
//שלב העדכון , הלמידה והפקת הלקחים
void RLAgent::update(
    const JunctionState& prevState,// מצב הצומת הקודם
    int actionPhaseId,// מזהה הפאזה שנבחרה בפעולה הקודמת
    double reward,// הערך של התגמול שהתקבל מהפעולה הקודמת
    const JunctionState& nextState,// מצב הצומת הבא
    const std::vector<Action>& nextValidActions// רשימת כל הפעולות האפשריות בצומת הבא
) {
    // Emergency preemption is handled as a hard safety override (freeze state).
    // Do not train Q-values on these forced decisions.
    if (prevState.emergencyVehicleActive || nextState.emergencyVehicleActive) return;// אם יש כלי רכב חירום פעיל במצב הקודם או הבא, מחזיר ולא מעדכן את ערכי Q
    if (nextValidActions.empty()) return;// אם אין פעולות אפשריות במצב הבא, מחזיר ולא מעדכן את ערכי Q

    const std::string prevKey = encodeState(prevState);// קידוד מצב הצומת הקודם למחרוזת ייחודית לשימוש במיפוי ערכי Q
    const std::string nextKey = encodeState(nextState);// קידוד מצב הצומת הבא למחרוזת ייחודית לשימוש במיפוי ערכי Q

    auto& qPrev = qValuesFor(prevKey, nextValidActions.size());// קבלת ערכי Q עבור מצב הצומת הקודם, עם גודל המערך בהתאם למספר הפעולות האפשריות בצומת הבא
    auto& qNext = qValuesFor(nextKey, nextValidActions.size());// קבלת ערכי Q עבור מצב הצומת הבא, עם גודל המערך בהתאם למספר הפעולות האפשריות בצומת הבא

    const int aIdx = actionIndexByPhaseId(nextValidActions, actionPhaseId);// קבלת אינדקס של הפעולה שנבחרה במצב הקודם
    if (aIdx < 0) return;// אם האינדקס של הפעולה שנבחרה במצב הקודם אינו תקין, מחזיר ולא מעדכן את ערכי Q

    const double maxNext = *std::max_element(qNext.begin(), qNext.end());// חישוב הערך המקסימלי של ערכי Q במצב הבא
    const double tdTarget = reward + cfg_.gamma * maxNext;// חישוב יעד TD על פי נוסחת Q-learning
    qPrev[aIdx] += cfg_.alpha * (tdTarget - qPrev[aIdx]);// עדכון ערך Q עבור הפעולה שנבחרה במצב הקודם
}
// פונקציה שמחשבת את התגמול עבור המעבר ממצב צומת קודם למצב צומת הבא, בהתחשב בפעולה שנבחרה ובתנאים נוספים
double RLAgent::computeReward(
    const JunctionState& prevState,// מצב הצומת הקודם
    const JunctionState& nextState,// מצב הצומת הבא
    int actionPhaseId,// מזהה הפאזה שנבחרה בפעולה הקודמת
    bool emergencyLaneGotGreen,// האם נתיב החירום קיבל אור ירוק מיידי
    bool greenSyncedWithNeighbor,// האם האור הירוק מסונכרן עם הצומת השכן
    bool greenOppositeToNeighbor,// האם האור הירוק מנוגד לצומת השכן
    double stepSec// משך הזמן של הצעד הנוכחי בשניות
) const {
    double reward = 0.0;// אתחול המשתנה שמציין את התגמול הכולל עבור המעבר ממצב צומת קודם למצב צומת הבא
    const double dt = std::max(0.0, stepSec);// קבלת משך הזמן של הצעד הנוכחי בשניות, עם ערך מינימלי של 0

    // Emergency: +100 if emergency lane got immediate green, else -10 per second delay.
    if (prevState.emergencyVehicleActive) {// אם יש כלי רכב חירום פעיל במצב הקודם, מחשבים את התגמול עבור נתיב החירום
        if (emergencyLaneGotGreen) {// אם נתיב החירום קיבל אור ירוק מיידי, מוסיפים 100 לתגמול הכולל
            reward += kEmergencyImmediateBonus;// מוסיפים בונוס מיידי עבור נתיב החירום
        } else {
            reward -= kEmergencyDelayPerSecPenalty * dt;// אם נתיב החירום לא קיבל אור ירוק מיידי, מפחיתים מהתגמול בהתאם לעיכוב
        }
    }

    int prevTotalQueue = 0;// אתחול המשתנה שמציין את מספר הרכבים הממתינים הכולל במצב הקודם
    int nextTotalQueue = 0;// אתחול המשתנה שמציין את מספר הרכבים הממתינים הכולל במצב הבא
    int prevMaxQueue = 0;// אתחול המשתנה שמציין את מספר הרכבים הממתינים המקסימלי במצב הקודם
    int nextMaxQueue = 0;// אתחול המשתנה שמציין את מספר הרכבים הממתינים המקסימלי במצב הבא

    for (int c : prevState.vehicleCounts) {// לולאה שעוברת על כל מספרי הרכבים הממתינים במצב הקודם
        const int q = std::max(0, c);// קבלת מספר הרכבים הממתינים במצב הקודם, עם ערך מינימלי של 0
        prevTotalQueue += q;// עדכון מספר הרכבים הממתינים הכולל במצב הקודם
        prevMaxQueue = std::max(prevMaxQueue, q);// עדכון מספר הרכבים הממתינים המקסימלי במצב הקודם
    }

    // Waiting time objective + hunger penalties + queue tracking.
    const std::size_t n = std::min(nextState.vehicleCounts.size(), nextState.waitingTimes.size());// קבלת מספר הנתיבים המינימלי בין שני המערכים במצב הבא
    for (std::size_t i = 0; i < n; ++i) {// לולאה שעוברת על כל הנתיבים במצב הבא
        const int q = std::max(0, nextState.vehicleCounts[i]);// קבלת מספר הרכבים הממתינים במצב הבא, עם ערך מינימלי של 0
        nextTotalQueue += q;// עדכון מספר הרכבים הממתינים הכולל במצב הבא
        nextMaxQueue = std::max(nextMaxQueue, q);// עדכון מספר הרכבים הממתינים המקסימלי במצב הבא
// חישוב התגמול עבור זמן ההמתנה של כל הנתיבים במצב הבא, עם עונשים בהתאם לסף זמן ההמתנה
        reward -= static_cast<double>(q) *
                  std::max(0.0, nextState.waitingTimes[i]) *
                  kWaitPerVehiclePerSecPenalty * dt;

        if (nextState.waitingTimes[i] > thresholds_.waitingTimeSec.mediumMax) {// אם זמן ההמתנה בנתיב הנוכחי גדול מהסף המקסימלי של זמן ההמתנה הבינוני, מפחיתים מהתגמול בהתאם לעונש הרעב הגבוה
            reward -= kHunger90Penalty;// מוסיפים עונש חזק עבור נתיבים עם זמן המתנה ארוך מאוד
        } else if (nextState.waitingTimes[i] > thresholds_.waitingTimeSec.lowMax) {// אם זמן ההמתנה בנתיב הנוכחי גדול מהסף המקסימלי של זמן ההמתנה הנמוך, מפחיתים מהתגמול בהתאם לעונש הרעב הבינוני
            reward -= kHunger60Penalty;// מוסיפים עונש בינוני עבור נתיבים עם זמן המתנה בינוני
        }
    }

    const int queueDelta = prevTotalQueue - nextTotalQueue;// חישוב השינוי במספר הרכבים הממתינים בין המצב הקודם למצב הבא
    if (queueDelta > 0) {// אם מספר הרכבים הממתינים במצב הבא קטן ממספר הרכבים הממתינים במצב הקודם, מוסיפים לתגמול בהתאם לבונוס הפחתת התורים
        reward += static_cast<double>(queueDelta) * kTotalQueueReductionBonus;//
    } else if (queueDelta < 0) {// אם מספר הרכבים הממתינים במצב הבא גדול ממספר הרכבים הממתינים במצב הקודם, מפחיתים מהתגמול בהתאם לעונש על הגדלת התורים
        reward -= static_cast<double>(-queueDelta) * kQueueIncreasePenalty;
    }

    reward -= static_cast<double>(nextMaxQueue) * kMaxQueuePenalty;// מפחיתים מהתגמול בהתאם לעונש על מספר הרכבים הממתינים המקסימלי במצב הבא
    if (nextMaxQueue > prevMaxQueue) {// אם מספר הרכבים הממתינים המקסימלי במצב הבא גדול ממספר הרכבים הממתינים המקסימלי במצב הקודם, מפחיתים מהתגמול בהתאם לעונש על גידול מספר הרכבים הממתינים המקסימלי
        reward -= static_cast<double>(nextMaxQueue - prevMaxQueue) * kMaxQueueGrowthPenalty;
    }

    (void)actionPhaseId;// משתנה שלא בשימוש, אך נשמר למקרה שיידרש בעתיד

    // Green wave reward/penalty with adjacent nodes
    if (greenSyncedWithNeighbor) {// אם האור הירוק מסונכרן עם השכן, מוסיפים או מפחיתים מהתגמול בהתאם לשינוי בתורים
        reward += (queueDelta >= 0) ? kGreenSyncPositiveReward : kGreenSyncNegativeReward;//
    }
    if (greenOppositeToNeighbor) {// אם האור הירוק מנוגד לשכן, מוסיפים או מפחיתים מהתגמול בהתאם לשינוי בתורים
        reward -= (queueDelta < 0) ? kGreenOppNegativePenalty : kGreenOppPositivePenalty;
    }

    return reward;
}
// פונקציה שמקטינה את ערך האפסילון כדי להפחית את הסיכוי לבחירה אקראית של פעולות ככל שהסוכן לומד יותר
void RLAgent::decayExploration() {
    cfg_.epsilon = std::max(cfg_.epsilonMin, cfg_.epsilon * cfg_.epsilonDecay);// הפחתת ערך האפסילון בהתאם לערך המינימלי והקצב שנקבעו בהגדרות
}

double RLAgent::epsilon() const noexcept {// פונקציה שמחזירה את ערך האפסילון הנוכחי של הסוכן
    return cfg_.epsilon;
}

void RLAgent::setThresholdConfig(TrafficThresholdConfig thresholds) {// פונקציה שמעדכנת את הגדרות הסף של התנועה עבור הסוכן
    thresholds_ = std::move(thresholds);
}

const TrafficThresholdConfig& RLAgent::thresholdConfig() const noexcept {// פונקציה שמחזירה את הגדרות הסף הנוכחיות של התנועה עבור הסוכן
    return thresholds_;
}

void RLAgent::setRandomSeed(std::uint32_t seed) noexcept {// פונקציה שמעדכנת את הזרע של מחולל המספרים האקראיים של הסוכן
    rng_.seed(seed);
}

bool RLAgent::loadQTable(const std::string& filePath) {// פונקציה שמטענת את טבלת ערכי Q מקובץ נתון
    std::ifstream in(filePath);
    if (!in.is_open()) {
        return false;
    }

    std::unordered_map<std::string, std::vector<double>> loaded;

    std::string line;
    while (std::getline(in, line)) {// לולאה שקוראת את הקובץ שורה אחר שורה
        if (line.empty()) continue;// אם השורה ריקה, ממשיך לשורה הבאה

        const std::size_t firstTab = line.find('\t');// חיפוש הטאב הראשון בשורה
        const std::size_t secondTab = (firstTab == std::string::npos) ? std::string::npos : line.find('\t', firstTab + 1);// חיפוש הטאב השני בשורה
        if (firstTab == std::string::npos || secondTab == std::string::npos) {// אם לא נמצאו שני טאבים בשורה, ממשיך לשורה הבאה
            continue;
        }

        const std::string key = line.substr(0, firstTab);// קבלת המחרוזת שמייצגת את מצב הצומת
        const std::string actionCountStr = line.substr(firstTab + 1, secondTab - firstTab - 1);// קבלת המחרוזת שמייצגת את מספר הפעולות האפשריות בצומת
        const std::string valuesStr = line.substr(secondTab + 1);// קבלת המחרוזת שמייצגת את ערכי Q עבור מצב הצומת

        int actionCount = 0;// אתחול המשתנה שמציין את מספר הפעולות האפשריות בצומת
        try {
            actionCount = std::stoi(actionCountStr);// המרת המחרוזת למספר שלם
        } catch (...) {
            continue;
        }
        if (actionCount <= 0) continue;// אם מספר הפעולות האפשריות בצומת קטן או שווה ל-0, ממשיך לשורה הבאה

        std::vector<double> q;
        q.reserve(static_cast<std::size_t>(actionCount));// שמירת מקום במערך של ערכי Q בהתאם למספר הפעולות האפשריות בצומת

        std::stringstream ss(valuesStr);// יצירת סטרינג סטרים מהערכים של Q
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.empty()) continue;// אם המחרוזת ריקה, ממשיך למחרוזת הבאה
            try {
                q.push_back(std::stod(token));// המרת המחרוזת למספר ממשי והוספתו למערך
            } catch (...) {
                q.clear();
                break;
            }
        }

        if (static_cast<int>(q.size()) != actionCount) {// אם מספר הערכים של Q אינו תואם למספר הפעולות האפשריות בצומת, ממשיך לשורה הבאה
            continue;
        }

        loaded[key] = std::move(q);// שמירת המערך של ערכי Q במפה עם המפתח שמייצג את מצב הצומת
    }

    if (!loaded.empty()) {// אם המפה של ערכי Q אינה ריקה, מעדכנים את טבלת ערכי Q של הסוכן
        qTable_ = std::move(loaded);// מעדכנים את טבלת ערכי Q של הסוכן עם המפה החדשה
    }

    return true;// מחזירים true אם הטעינה הצליחה, אחרת false
}
// פונקציה ששומרת את טבלת ערכי Q לקובץ נתון
bool RLAgent::saveQTable(const std::string& filePath) const {
    std::ofstream out(filePath, std::ios::trunc);// פתיחת הקובץ לכתיבה, ומחיקה של תוכנו הקודם
    if (!out.is_open()) {// אם לא ניתן לפתוח את הקובץ, מחזירים false
        return false;
    }

    out << std::setprecision(std::numeric_limits<double>::max_digits10);// הגדרת דיוק המספרים הממשיים לכתיבה בקובץ
    for (const auto& [stateKey, values] : qTable_) {// לולאה שעוברת על כל המפתחות והערכים במפה של ערכי Q
        out << stateKey << '\t' << values.size() << '\t';// כתיבת המפתח (מצב הצומת) ומספר הערכים של Q לקובץ
        for (std::size_t i = 0; i < values.size(); ++i) {// לולאה שעוברת על כל הערכים של Q עבור מצב הצומת הנוכחי
            if (i > 0) out << ',';// אם זה לא הערך הראשון, מוסיפים פסיק לפני הערך
            out << values[i];// כתיבת הערך של Q לקובץ
        }
        out << '\n';// כתיבת שורה חדשה בסוף כל מצב צומת
    }

    return out.good();// מחזירים true אם הכתיבה לקובץ הצליחה, אחרת false
}

std::string RLAgent::encodeState(const JunctionState& s) const {// פונקציה שמקבלת את מצב הצומת הנוכחי ומחזירה מחרוזת ייחודית שמייצגת את המצב, לשימוש במיפוי ערכי Q
    // Discretized state encoding for efficient Q-table learning.
    // Thresholds come from runtime config (vehicle_count, waiting_time_sec, density_pct).

    std::ostringstream oss;// יצירת סטרינג סטרים לכתיבת המחרוזת שמייצגת את מצב הצומת

    // Emergency flag (0 or 1)
    oss << (s.emergencyVehicleActive ? 1 : 0) << "|";// כתיבת הדגל שמציין אם יש כלי רכב חירום פעיל במצב הצומת, עם ערך 0 או 1

    // Vehicle-count bucket: light / medium / heavy
    for (int c : s.vehicleCounts) {
        const int level = discretizeThreeLevel(static_cast<double>(c), thresholds_.vehicleCount);
        oss << level << ",";// כתיבת רמת מספר הרכבים במצב הצומת, עם ערכים 0, 1 או 2

    }
    oss << "|";// כתיבת תו מפריד בין חלקי המחרוזת

    // Waiting-time bucket: short / medium / long
    for (double w : s.waitingTimes) {// לולאה שעוברת על כל זמני ההמתנה במצב הצומת
        const int level = discretizeThreeLevel(w, thresholds_.waitingTimeSec);// קבלת רמת זמן ההמתנה במצב הצומת, עם ערכים 0, 1 או 2
        oss << level << ",";// כתיבת רמת זמן ההמתנה במצב הצומת, עם ערכים 0, 1 או 2
    }
    oss << "|";// כתיבת תו מפריד בין חלקי המחרוזת

    // Density bucket: low / medium / high
    for (double d : s.densityPercents) {
        const int level = discretizeThreeLevel(d, thresholds_.densityPct);
        oss << level << ",";// כתיבת רמת הצפיפות במצב הצומת, עם ערכים 0, 1 או 2
    }

    oss << "|";// כתיבת תו מפריד בין חלקי המחרוזת

    // Neighbor signals encoding
    {
        const double localLaneCount = static_cast<double>(std::max<std::size_t>(1, s.laneIds.size()));// קבלת מספר הנתיבים המקומיים במצב הצומת, עם ערך מינימלי של 1
        double avgNeighborNormalizedQueue = 0.0;// אתחול המשתנה שמציין את ממוצע מספר הרכבים הממתינים המנורמל במצב הצומת השכן
        int emergencyNeighborCount = 0;// אתחול המשתנה שמציין את מספר השכנים עם כלי רכב חירום פעיל
        int validPhaseNeighborCount = 0;// אתחול המשתנה שמציין את מספר השכנים עם שלב תקף
        int dominantPhase = -1;// אתחול המשתנה שמציין את השלב הדומיננטי בקרב השכנים
        std::unordered_map<int, int> phaseHistogram;// יצירת מפה לשמירת ההיסטוגרמה של השלבים בקרב השכנים

        for (const auto& neighbor : s.neighborSignals) {// לולאה שעוברת על כל השכנים במצב הצומת
            avgNeighborNormalizedQueue += static_cast<double>(neighbor.totalQueue) / localLaneCount;// חישוב ממוצע מספר הרכבים הממתינים המנורמל בקרב השכנים
            if (neighbor.emergencyActive) {// אם יש כלי רכב חירום פעיל אצל השכן הנוכחי, מגדילים את הספירה של השכנים עם כלי רכב חירום פעיל
                ++emergencyNeighborCount;
            }
            if (neighbor.phaseId >= 0) {// אם לשכן יש שלב תקף, מגדילים את הספירה של השכנים עם שלב תקף ומעדכנים את ההיסטוגרמה של השלבים
                ++validPhaseNeighborCount;// מגדילים את הספירה של השכנים עם שלב תקף
                ++phaseHistogram[neighbor.phaseId];// מעדכנים את ההיסטוגרמה של השלבים בקרב השכנים
            }
        }

        if (!s.neighborSignals.empty()) {// אם יש שכנים במצב הצומת
            avgNeighborNormalizedQueue /= static_cast<double>(s.neighborSignals.size());// חישוב ממוצע מספר הרכבים הממתינים המנורמל בקרב השכנים
        }

        int dominantCount = 0;// אתחול המשתנה שמציין את מספר הפעמים שהשלב הדומיננטי מופיע בקרב השכנים
        for (const auto& [phaseId, count] : phaseHistogram) {// לולאה שעוברת על כל השלבים בהיסטוגרמה של השלבים בקרב השכנים
            if (count > dominantCount) {// אם מספר הפעמים שהשלב הנוכחי מופיע בקרב השכנים גדול ממספר הפעמים של השלב הדומיננטי שנמצא עד כה, מעדכנים את המשתנים המתאימים
                dominantCount = count;// מעדכנים את מספר הפעמים שהשלב הדומיננטי מופיע בקרב השכנים
                dominantPhase = phaseId;// מעדכנים את השלב הדומיננטי בקרב השכנים
            }
        }

        const int neighborQueueLevel = discretizeThreeLevel(avgNeighborNormalizedQueue, thresholds_.vehicleCount);// קבלת רמת ממוצע מספר הרכבים הממתינים המנורמל בקרב השכנים, עם ערכים 0, 1 או 2
        const int neighborCountLevel = discretizeThreeLevel(static_cast<double>(s.neighborSignals.size()), {0.0, 2.0});// קבלת רמת מספר השכנים במצב הצומת, עם ערכים 0, 1 או 2
        const int emergencyNeighborLevel = discretizeThreeLevel(static_cast<double>(emergencyNeighborCount), {0.0, 1.0});// קבלת רמת מספר השכנים עם כלי רכב חירום פעיל, עם ערכים 0, 1 או 2
// קידוד המידע על השכנים למחרוזת ייחודית שמייצגת את מצב הצומת, לשימוש במיפוי ערכי Q
        oss << "nq=" << neighborQueueLevel
            << ",nc=" << neighborCountLevel
            << ",ne=" << emergencyNeighborLevel
            << ",np=" << dominantPhase
            << ",nv=" << validPhaseNeighborCount;
    }

    return oss.str();// מחזירה את המחרוזת שמייצגת את מצב הצומת הנוכחי
}
// פונקציה שמקטינה ערך ממשי לערכים של 0, 1 או 2 בהתאם לספים שנקבעו
int RLAgent::discretizeThreeLevel(double value, const ThreeLevelThresholds& thresholds) const {// פונקציה שמקטינה ערך ממשי לערכים של 0, 1 או 2 בהתאם לספים שנקבעו
    if (value <= thresholds.lowMax) return 0;// אם הערך הממשי קטן או שווה לסף המקסימלי של הרמה הנמוכה, מחזירה 0
    if (value <= thresholds.mediumMax) return 1;// אם הערך הממשי קטן או שווה לסף המקסימלי של הרמה הבינונית, מחזירה 1
    return 2;// אם הערך הממשי גדול מהסף המקסימלי של הרמה הבינונית, מחזירה 2
}

std::vector<double>& RLAgent::qValuesFor(const std::string& stateKey, std::size_t actionCount) {// פונקציה שמחזירה את המערך של ערכי Q עבור מצב הצומת הנוכחי, עם גודל המערך בהתאם למספר הפעולות האפשריות בצומת
    auto& q = qTable_[stateKey];// קבלת המערך של ערכי Q עבור מצב הצומת הנוכחי מתוך המפה של ערכי Q
    if (q.size() != actionCount) {// אם גודל המערך של ערכי Q אינו תואם למספר הפעולות האפשריות בצומת, מאתחלים את המערך עם ערכים של 0.0
        q.assign(actionCount, 0.0);// אתחול המערך של ערכי Q עם ערכים של 0.0
    }
    return q;// מחזירה את המערך של ערכי Q עבור מצב הצומת הנוכחי
}
// פונקציה שמחזירה את האינדקס של הפאזה הנוכחית ברשימת כל הפעולות האפשריות בצומת, בהתבסס על מזהה הפאזה
int RLAgent::actionIndexByPhaseId(const std::vector<Action>& validActions, int phaseId) const {
    for (int i = 0; i < static_cast<int>(validActions.size()); ++i) {// לולאה שעוברת על כל הפעולות האפשריות בצומת
        if (validActions[i].phaseId == phaseId) return i;// אם מזהה הפאזה הנוכחית תואם למזהה הפאזה של הפעולה הנוכחית, מחזירה את האינדקס של הפעולה
    }
    return -1;// אם לא נמצא אינדקס של הפאזה הנוכחית ברשימת כל הפעולות האפשריות בצומת, מחזירה -1
}

int RLAgent::phaseIdByActionIndex(const std::vector<Action>& validActions, int actionIndex) const {// פונקציה שמחזירה את מזהה הפאזה של הפעולה שנבחרה בהתאם לאינדקס שלה ברשימת כל הפעולות האפשריות בצומת
    if (actionIndex < 0 || actionIndex >= static_cast<int>(validActions.size())) return -1;// אם האינדקס של הפעולה שנבחרה אינו תקין, מחזירה -1
    return validActions[actionIndex].phaseId;// מחזירה את מזהה הפאזה של הפעולה שנבחרה בהתאם לאינדקס שלה ברשימת כל הפעולות האפשריות בצומת
}

} // namespace traffic