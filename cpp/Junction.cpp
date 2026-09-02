#include "Junction.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace traffic {

Junction::Junction(
    int junctionId,// מזהה הצומת
    std::vector<Lane> lanes,// רשימת הנתיבים בצומת
    std::vector<Action> validPhases,// רשימת הפאזות החוקיות בצומת
    double minGreenSec,// זמן ירוק מינימלי לכל פאזה
    double maxGreenSec,// זמן ירוק מקסימלי לכל פאזה
        std::vector<std::pair<int, int>> conflictPairs,// זוגות נתיבים שסותרים זה את זה
        double yellowSec,// זמן צהוב
        double allRedSec// זמן אדום לכל הנתיבים
)
    : junctionId_(junctionId),// אתחול מזהה הצומת
      lanes_(std::move(lanes)),// אתחול רשימת הנתיבים בצומת
      validPhases_(std::move(validPhases)),// אתחול רשימת הפאזות החוקיות בצומת
      minGreenSec_(minGreenSec),// אתחול זמן ירוק מינימלי לכל פאזה
            maxGreenSec_(maxGreenSec),// אתחול זמן ירוק מקסימלי לכל פאזה
            yellowSec_(std::max(0.0, yellowSec)),// אתחול זמן צהוב בין פאזה לפאזה
            allRedSec_(std::max(0.0, allRedSec)),// אתחול זמן "הכול אדום" בין פאזה לפאזה
            interGreenSec_(std::max(0.0, yellowSec_) + std::max(0.0, allRedSec_)) {// אתחול זמן "ירוק ביניים" בין פאזה לפאזה
    for (auto [a, b] : conflictPairs) {// לולאה שעוברת על כל זוגות הנתיבים שסותרים זה את זה
        if (a < 0 || b < 0 || a == b) continue;// אם אחד הנתיבים אינו תקין או שהם זהים, ממשיך לזוג הבא
        conflictPairs_.insert(conflictKey(a, b));// מוסיף את הזוג למערך הקונפליקטים
    }
}

int Junction::id() const noexcept {// קבלת מזהה הצומת
    return junctionId_;// מחזיר את מזהה הצומת
}
//פונקצית העדכון של הצומת בזמן אמת, מעדכנת את מספר כלי הרכב המדויק בנתיב, אחוז צפיפות הנתיב והאם יש כלי רכב חירום בנתיב. אם אין כלי רכב בנתיב, מאפס את זמן ההמתנה
void Junction::updateLaneObservation(int laneId, int exactVehicleCount, bool emergencyOnLane, double densityPct) {
    const int idx = laneIndexById(laneId);// קבלת האינדקס של הנתיב ברשימת הנתיבים לפי מזהה הנתיב
    if (idx < 0) return;// אם הנתיב אינו קיים בצומת, מחזיר

    lanes_[idx].vehicleCount = std::max(0, exactVehicleCount);// עדכון מספר כלי הרכב המדויק בנתיב (לא יכול להיות שלילי)
    lanes_[idx].densityPct = std::clamp(densityPct, 0.0, 100.0);// עדכון אחוז צפיפות הנתיב (מוגבל בין 0 ל-100)
    lanes_[idx].hasEmergencyVehicle = emergencyOnLane;// עדכון האם יש כלי רכב חירום בנתיב

    // Empty lanes should not accumulate starvation history.
    if (lanes_[idx].vehicleCount == 0) {//אם אין רכב בנתיב
        lanes_[idx].waitingTimeSec = 0.0;//איפוס זמן ההמתנה
    }
}
//מחזירה סטייט -  פונקציה שמחזירה את המצב הנוכחי של הצומת, כולל מזהי הנתיבים, מספר כלי הרכב בכל נתיב, אחוז צפיפות הנתיב, זמן ההמתנה בכל נתיב ומידע על כלי רכב חירום בצומת
JunctionState Junction::currentState() const {//לקריאה בלבד
    JunctionState s;// יצירת אובייקט JunctionState חדש
    s.laneIds.reserve(lanes_.size());// שמירת מקום למזהי הנתיבים
    s.vehicleCounts.reserve(lanes_.size());// שמירת מקום למספר כלי הרכב בכל נתיב
    s.densityPercents.reserve(lanes_.size());// שמירת מקום לאחוז צפיפות הנתיב
    s.waitingTimes.reserve(lanes_.size());// שמירת מקום לזמן ההמתנה בכל נתיב

    for (const auto& lane : lanes_) {// לולאה שעוברת על כל הנתיבים בצומת 
        s.laneIds.push_back(lane.id);// הוספת מזהה הנתיב לוקטור מזהי הנתיבים
        s.vehicleCounts.push_back(lane.vehicleCount);// הוספת מספר כלי הרכב בנתיב לוקטור
        s.densityPercents.push_back(lane.densityPct);// הוספת אחוז צפיפות הנתיב לוקטור
        s.waitingTimes.push_back(lane.waitingTimeSec);// הוספת זמן ההמתנה בנתיב לוקטור
    }

    s.emergencyVehicleActive = emergencyActive_;// עדכון האם יש כלי רכב חירום פעיל בצומת
    s.emergencyLaneId = emergencyLaneId_;// עדכון מזהה הנתיב שבו יש כלי רכב חירום
    return s;
}
//פונקציה שקובעת האם מותר להחליף את האור בצומת
bool Junction::canSwitchPhase(double nowSec) const {
    if (activePhaseId_ < 0) return true;// אם אין פאזה פעילה כרגע, מותר להחליף את האור
    const bool minGreenReached = (nowSec - phaseStartSec_) >= minGreenSec_;// בדיקה אם הזמן המינימלי של הירוק הושג
    const bool interGreenReached =
        (lastPhaseSwitchSec_ < 0.0) || ((nowSec - lastPhaseSwitchSec_) >= interGreenSec_);// בדיקה אם הזמן של "ירוק ביניים" הושג
    return minGreenReached && interGreenReached;// אם הזמן המינימלי של הירוק והזמן של "ירוק ביניים" הושגו, מותר להחליף את האור
}
//פונקציה שקובעת האם חייבים להחליף את האור בצומת
bool Junction::mustSwitchPhase(double nowSec) const {
    if (activePhaseId_ < 0) return false;// אם אין פאזה פעילה כרגע, אין חובה להחליף את האור
    return (nowSec - phaseStartSec_) >= maxGreenSec_;// בדיקה אם הזמן המקסימלי של הירוק הושג
}
//פונקציה שמחליפה את הפאזה בצומת בתנאי שזה בטוח ומחזירה האם בוצעה בפועל כולל קבלת חירום
bool Junction::applyPhase(int phaseId, double nowSec) {
    auto it = std::find_if(validPhases_.begin(), validPhases_.end(),// חיפוש הפאזה ברשימת הפאזות החוקיות
        [phaseId](const Action& a) { return a.phaseId == phaseId; });
    if (it == validPhases_.end()) return false;// אם הפאזה אינה חוקית, מחזיר false
    if (!isPhaseValid(*it)) {// אם הפאזה אינה תקינה, מחזיר false
        std::cerr << "[Junction " << junctionId_ << "] rejected invalid phase " << phaseId << "\n";
        return false;
    }

    if (activePhaseId_ < 0) {// אם אין פאזה פעילה כרגע, מחליף את הפאזה ומעדכן את הזמן
        activePhaseId_ = phaseId;// עדכון מזהה הפאזה הפעילה
        phaseStartSec_ = nowSec;// עדכון זמן התחלת הפאזה הפעילה
        lastPhaseSwitchSec_ = nowSec;// עדכון זמן ההחלפה האחרון של הפאזות
        return true;// מחזיר true כי הפאזה הוחלפה בהצלחה
    }

    if (activePhaseId_ == phaseId) return true;// אם הפאזה המבוקשת היא הפאזה הפעילה כרגע, מחזיר true כי אין צורך להחליף את הפאזה
//חירום דורס את זמן הירוק המינימלי...
    bool emergencyOverrideForMinGreen = false;// משתנה שמציין אם יש חירום שמאפשר לעקוף את זמן הירוק המינימלי
    if (((nowSec - phaseStartSec_) < minGreenSec_) && emergencyActive_ && emergencyLaneId_.has_value()) {// אם הזמן המינימלי של הירוק לא הושג ויש חירום פעיל בצומת
        const int emergencyLane = *emergencyLaneId_;// קבלת מזהה הנתיב שבו יש חירום
        emergencyOverrideForMinGreen =
            std::find(it->greenLanes.begin(), it->greenLanes.end(), emergencyLane) != it->greenLanes.end();// בדיקה אם הפאזה המבוקשת כוללת את הנתיב שבו יש חירום
    }

    const bool interGreenReached =
        (lastPhaseSwitchSec_ < 0.0) || ((nowSec - lastPhaseSwitchSec_) >= interGreenSec_);// בדיקה אם הזמן של "ירוק ביניים" הושג

    // Never bypass inter-green safety guard. Emergency can only bypass min-green.
    if (!interGreenReached) return false;// אם הזמן של "ירוק ביניים" לא הושג, מחזיר false כי אסור לעקוף את הבטיחות של "ירוק ביניים"

    const bool minGreenReached = (nowSec - phaseStartSec_) >= minGreenSec_;// בדיקה אם הזמן המינימלי של הירוק הושג
    if (!minGreenReached && !emergencyOverrideForMinGreen) return false;// אם הזמן המינימלי של הירוק לא הושג ואין חירום שמאפשר לעקוף את הזמן המינימלי, מחזיר false

    activePhaseId_ = phaseId;// עדכון מזהה הפאזה הפעילה
    phaseStartSec_ = nowSec;// עדכון זמן התחלת הפאזה הפעילה
    lastPhaseSwitchSec_ = nowSec;// עדכון זמן ההחלפה האחרון של הפאזות
    return true;// מחזיר true כי הפאזה הוחלפה בהצלחה
}
// עדכון זמני ההמתנה של הנתיבים בצומת בהתאם לפאזה הפעילה
void Junction::tick(double deltaSec) {
    for (auto& lane : lanes_) {// לולאה שעוברת על כל הנתיבים בצומת
        if (lane.vehicleCount <= 0) {//אם אין רכבים
            lane.waitingTimeSec = 0.0;//מאפסים את זמן ההמתנה
        } else if (isLaneGreen(lane.id)) {//אם הנתיב ירוק
            lane.waitingTimeSec = 0.0;//מאפסים את זמן ההמתנה
        } else {//אם הנתיב אדום
            lane.waitingTimeSec += deltaSec;//מעדכנים את זמן ההמתנה
        }
    }
}
//קבלת אות החירום
void Junction::setEmergencySignal(bool active, std::optional<int> emergencyLaneId) {
    emergencyActive_ = active;// עדכון מצב החירום בצומת
    emergencyLaneId_ = emergencyLaneId;// עדכון מזהה הנתיב שבו יש חירום (אם קיים)
}
//פונקציה שמחזירה את הפאזה הבטוחה ביותר שנותנת עדיפות לחירום
std::optional<int> Junction::resolveEmergencyPhase() const {
    if (!emergencyActive_ || !emergencyLaneId_.has_value()) return std::nullopt;// אם אין חירום פעיל בצומת או שאין נתיב חירום, מחזיר nullopt

    const int targetLane = *emergencyLaneId_;// קבלת מזהה הנתיב שבו יש חירום
    bool found = false;// משתנה שמציין אם נמצאה פאזה מתאימה לחירום
    std::size_t bestGreenCount = 0;// משתנה שמציין את מספר הנתיבים הירוקים בפאזה הטובה ביותר שנמצאה
    int bestTotalQueued = -1;// משתנה שמציין את מספר כלי הרכב הכולל בפאזה הטובה ביותר שנמצאה
    double bestTotalWaiting = -1.0;// משתנה שמציין את זמן ההמתנה הכולל בפאזה הטובה ביותר שנמצאה
    int bestPhaseId = -1;// משתנה שמציין את מזהה הפאזה הטובה ביותר שנמצאה

    for (const auto& phase : validPhases_) {// לולאה שעוברת על כל הפאזות החוקיות בצומת
        if (std::find(phase.greenLanes.begin(), phase.greenLanes.end(), targetLane) == phase.greenLanes.end()) continue;// אם הפאזה אינה כוללת את הנתיב שבו יש חירום, ממשיך לפאזה הבאה
        if (!isPhaseValid(phase)) continue;// אם הפאזה אינה חוקית, ממשיך לפאזה הבאה

        const std::size_t greenCount = phase.greenLanes.size();// מספר הנתיבים הירוקים בפאזה הנוכחית
        int totalQueued = 0;// משתנה שמציין את מספר כלי הרכב הכולל בפאזה הנוכחית
        double totalWaiting = 0.0;// משתנה שמציין את זמן ההמתנה הכולל בפאזה הנוכחית
        for (int laneId : phase.greenLanes) {// לולאה שעוברת על כל הנתיבים הירוקים בפאזה הנוכחית
            const int idx = laneIndexById(laneId);// קבלת האינדקס של הנתיב ברשימת הנתיבים לפי מזהה הנתיב
            if (idx < 0) continue;// אם הנתיב אינו קיים בצומת, ממשיך לנתיב הבא
            totalQueued += std::max(0, lanes_[idx].vehicleCount);// עדכון מספר כלי הרכב הכולל בפאזה הנוכחית
            totalWaiting += std::max(0.0, lanes_[idx].waitingTimeSec);// עדכון זמן ההמתנה הכולל בפאזה הנוכחית
        }
//השוואה בין הפאזות שנמצאו כדי למצוא את הפאזה הבטוחה ביותר שנותנת עדיפות לחירום
        const bool better =
            (!found) ||
            (greenCount > bestGreenCount) ||
            (greenCount == bestGreenCount && totalQueued > bestTotalQueued) ||
            (greenCount == bestGreenCount && totalQueued == bestTotalQueued && totalWaiting > bestTotalWaiting) ||
            (greenCount == bestGreenCount && totalQueued == bestTotalQueued &&
                std::abs(totalWaiting - bestTotalWaiting) < 1e-9 && phase.phaseId < bestPhaseId);// אם הפאזה הנוכחית טובה יותר מהפאזה הטובה ביותר שנמצאה עד כה, מעדכן את המשתנים שמציינים את הפאזה הטובה ביותר

        if (better) {// אם הפאזה הנוכחית טובה יותר מהפאזה הטובה ביותר שנמצאה עד כה
            found = true;// מעדכן את המשתנה שמציין אם נמצאה פאזה מתאימה לחירום
            bestGreenCount = greenCount;// מעדכן את מספר הנתיבים הירוקים בפאזה הטובה ביותר שנמצאה
            bestTotalQueued = totalQueued;// מעדכן את מספר כלי הרכב הכולל בפאזה הטובה ביותר שנמצאה
            bestTotalWaiting = totalWaiting;// מעדכן את זמן ההמתנה הכולל בפאזה הטובה ביותר שנמצאה
            bestPhaseId = phase.phaseId;// מעדכן את מזהה הפאזה הטובה ביותר שנמצאה
        }
    }

    if (found) return bestPhaseId;// אם נמצאה פאזה מתאימה לחירום, מחזיר את מזהה הפאזה הטובה ביותר שנמצאה
    return std::nullopt;// אם לא נמצאה פאזה מתאימה לחירום, מחזיר nullopt
}
//קבלת רשימת הפאזות החוקיות בצומת
const std::vector<Action>& Junction::validPhases() const noexcept {
    return validPhases_;
}
//קבלת מזהה הפאזה הפעילה כרגע
int Junction::activePhaseId() const noexcept {
    return activePhaseId_;
}
//פונקציה שמחזירה האם הפאזה חוקית - כלומר אין קונפליקטים בין הנתיבים שמקבלים אור ירוק
bool Junction::isPhaseValid(const Action& phase) const {
    for (int laneId : phase.greenLanes) {// לולאה שעוברת על כל הנתיבים הירוקים בפאזה הנוכחית
        if (laneIndexById(laneId) < 0) {// אם הנתיב אינו קיים בצומת, מחזיר false
            return false;
        }
    }

    return lanesAreMutuallyExclusive(phase.greenLanes);// בדיקה אם הנתיבים הירוקים בפאזה הנוכחית אינם בקונפליקט אחד עם השני
}
//פונקציה שמחזירה האם הנתיבים שמקבלים אור ירוק בפאזה מסוימת אינם בקונפליקט אחד עם השני
bool Junction::lanesAreMutuallyExclusive(const std::vector<int>& greenLanes) const {
    std::unordered_set<int> uniqueLanes;// יצירת סט של נתיבים ייחודיים כדי לבדוק אם יש כפילויות
    for (int laneId : greenLanes) {// לולאה שעוברת על כל הנתיבים הירוקים בפאזה הנוכחית
        if (!uniqueLanes.insert(laneId).second) {// אם הנתיב כבר קיים בסט, מחזיר false כי יש כפילות
            std::cerr << "[Junction " << junctionId_ << "] duplicate lane in phase: " << laneId << "\n";// הדפסת הודעת שגיאה על כפילות הנתיב בפאזה
            return false;
        }
    }

    for (std::size_t i = 0; i < greenLanes.size(); ++i) {// לולאה שעוברת על כל הנתיבים הירוקים בפאזה הנוכחית
        for (std::size_t j = i + 1; j < greenLanes.size(); ++j) {// לולאה פנימית שעוברת על כל הנתיבים הירוקים בפאזה הנוכחית כדי לבדוק אם יש קונפליקטים בין הנתיבים
            if (lanesConflict(greenLanes[i], greenLanes[j])) {// אם הנתיבים נמצאים בקונפליקט אחד עם השני, מחזיר false
                std::cerr << "[Junction " << junctionId_ << "] conflict blocked between lanes "// הדפסת הודעת שגיאה על קונפליקט בין הנתיבים בפאזה
                          << greenLanes[i] << " and " << greenLanes[j] << "\n";
                return false;
            }
        }
    }

    return true;// אם אין כפילויות ואין קונפליקטים בין הנתיבים הירוקים בפאזה הנוכחית, מחזיר true
}

//פונקציה שמחזירה האם הנתיב מקבל אור ירוק בפאזה הפעילה
bool Junction::isLaneGreen(int laneId) const {
    auto it = std::find_if(validPhases_.begin(), validPhases_.end(),// חיפוש הפאזה הפעילה ברשימת הפאזות החוקיות
        [this](const Action& a) { return a.phaseId == activePhaseId_; });
    if (it == validPhases_.end()) return false;//  אם הפאזה הפעילה אינה חוקית, מחזיר false

    return std::find(it->greenLanes.begin(), it->greenLanes.end(), laneId) != it->greenLanes.end();// אם הנתיב נמצא ברשימת הנתיבים הירוקים בפאזה הפעילה, מחזיר true, אחרת מחזיר false
}

int Junction::laneIndexById(int laneId) const {// פונקציה שמחזירה את האינדקס של הנתיב ברשימת הנתיבים לפי מזהה הנתיב
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {// לולאה שעוברת על כל הנתיבים בצומת
        if (lanes_[i].id == laneId) return i;// אם מזהה הנתיב תואם למזהה הנתיב המבוקש, מחזיר את האינדקס של הנתיב ברשימת הנתיבים
    }
    return -1;// אם הנתיב אינו קיים בצומת, מחזיר -1
}
//אם 1 2 עם קונפליקט אז גם 2 1 עם אותו קונפליקט
std::uint64_t Junction::conflictKey(int laneA, int laneB) const {
    if (laneA > laneB) std::swap(laneA, laneB);// סידור הנתיבים לפי סדר עולה כדי ליצור מפתח ייחודי לכל זוג נתיבים

    const std::uint64_t a = static_cast<std::uint32_t>(laneA);// המרת מזהה הנתיב הראשון ל-uint32_t כדי למנוע איבוד מידע
    const std::uint64_t b = static_cast<std::uint32_t>(laneB);// המרת מזהה הנתיב השני ל-uint32_t כדי למנוע איבוד מידע
    return (a << 32) | b;// שילוב מזהי הנתיבים ליצירת מפתח ייחודי לכל זוג נתיבים
}

bool Junction::lanesConflict(int laneA, int laneB) const {// פונקציה שמחזירה האם הנתיבים נמצאים בקונפליקט אחד עם השני
    if (laneA == laneB) return true;// אם הנתיבים זהים, מחזיר true כי הם בקונפליקט אחד עם השני
    return conflictPairs_.find(conflictKey(laneA, laneB)) != conflictPairs_.end();// אם המפתח של זוג הנתיבים נמצא במערך הקונפליקטים, מחזיר true כי הם בקונפליקט אחד עם השני
}

} // namespace traffic