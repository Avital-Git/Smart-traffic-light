#pragma once

#include <vector>
#include <optional>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace traffic {

struct NeighborSignal {//מידע על אותות שכנים לצורך תיאום
    int intersectionId = -1;//מזהה הצומת של השכן
    int phaseId = -1;//פאזה פעילה אצל השכן
    int totalQueue = 0;//מספר כלי הרכב בתור אצל השכן
    double avgWaitingSec = 0.0;//זמן המתנה ממוצע של כלי הרכב בתור אצל השכן
    bool emergencyActive = false;// האם יש מצב חירום פעיל אצל השכן
    int travelTimeSec = 30;//זמן נסיעה משוער מהשכן אלינו - יכול לשמש לחיזוי מתי יגיעו כלי רכב מהשכן
    double signedAtSec = 0.0;// כדי לא להשתמש במידע פג תוקף, טיימסטמפ שליחת המידע מהשכן - לשם אימות תקינות המידע
    bool signatureVerified = false;// האם האותות מהשכן עברו אימות חתימה דיגיטלית - כדי להבטיח שהמידע אמין ולא מזויף
};

struct Lane {
    int id = -1;// מזהה ייחודי של הנתיב
    int vehicleCount = 0;          // מספר כלי הרכב המדויק בנתיב
    double densityPct = 0.0;       // אחוז צפיפות הנתיב 0..100 
    double waitingTimeSec = 0.0;   //  זמן המתנה ממוצע של כלי הרכב בנתיב + מדד מניעת הרעבה
    bool hasEmergencyVehicle = false;// האם יש כלי רכב חירום בנתיב
};

struct JunctionState {
    std::vector<int> laneIds;            // מזהה נתיב size N (index-aligned with vectors below)
    std::vector<int> vehicleCounts;      // מבפר כלי הרכב size N
    std::vector<double> densityPercents; // אחוז צפיפות size N
    std::vector<double> waitingTimes;    // זמן המתנה size N
    std::vector<NeighborSignal> neighborSignals;// מידע על אותות שכנים size M
    bool emergencyVehicleActive = false; // האם יש כלי רכב חירום פעיל בצומת
    std::optional<int> emergencyLaneId;  // אם יש מצב חירום, מזהה הנתיב שבו נמצא כלי הרכב החירום
};

struct Action {
    int phaseId = -1;// מזהה הפאזה של הפעולה                   
    std::vector<int> greenLanes;// רשימת הנתיבים שמקבלים אור ירוק בפאזה זו       
};

class Junction {//ייצוג של צומת עם נתיבים ופאזות
public:
    Junction(
        int junctionId,//מזהה ייחודי של הצומת
        std::vector<Lane> lanes,//רשימת הנתיבים בצומת
        std::vector<Action> validPhases,//רשימת הפאזות החוקיות 
        double minGreenSec,//זמן ירוק מינימלי לכל פאזה 
        double maxGreenSec,//זמן ירוק מקסימלילכל פאזה
        std::vector<std::pair<int, int>> conflictPairs = {},//זוגות קונפליקטים
        double yellowSec = 2.0,//זמן צהוב בין פאזה לפאזה
        double allRedSec = 1.0//זמן "הכול אדום" בין פאזה לפאזה
    );

    int id() const noexcept;// קבלת מזהה הצומת

    // Dynamic lane updates from vision/camera pipeline
    void updateLaneObservation(int laneId, int exactVehicleCount, bool emergencyOnLane, double densityPct = 0.0);

    // Build dynamic state for RL (N lanes, no hardcoding)
    JunctionState currentState() const;

    // Safety + operational constraints
    bool canSwitchPhase(double nowSec) const;   // לפי זמן ירוק מינימלי
    bool mustSwitchPhase(double nowSec) const;  // לפי זמן ירוק מקסימלי

    // Apply action if valid and safe
    bool applyPhase(int phaseId, double nowSec);//מחליפה פאזה בתנאי שזה בטוח ומחזירה האם בוצעה בפועל

    // Update waiting counters based on active phase
    void tick(double deltaSec);

    // Emergency interrupt handling
    void setEmergencySignal(bool active, std::optional<int> emergencyLaneId);
    std::optional<int> resolveEmergencyPhase() const; //הפאזה הבטוחה ביותר שנותנת עדיפות לחירום 

    const std::vector<Action>& validPhases() const noexcept;//קבלת רשימת הפאזות החוקיות
    int activePhaseId() const noexcept;//קבלת מזהה הפאזה הפעילה כרגע

private:
    bool isPhaseValid(const Action& phase) const;//בדיקה אם הפאזה חוקית - כלומר אין קונפליקטים בין הנתיבים שמקבלים אור ירוק
    bool lanesAreMutuallyExclusive(const std::vector<int>& greenLanes) const;// בדיקה אם הנתיבים שמקבלים אור ירוק בפאזה מסוימת אינם בקונפליקט אחד עם השני
    bool isLaneGreen(int laneId) const;// בדיקה אם נתיב מסוים מקבל אור ירוק בפאזה הפעילה כרגע
    int laneIndexById(int laneId) const;// קבלת האינדקס של הנתיב ברשימת הנתיבים לפי מזהה הנתיב
    std::uint64_t conflictKey(int laneA, int laneB) const;// יצירת מפתח ייחודי לזוג נתיבים כדי לבדוק אם הם בקונפליקט
    bool lanesConflict(int laneA, int laneB) const;// בדיקה אם שני נתיבים נמצאים בקונפליקט

private:
    int junctionId_;// מזהה ייחודי של הצומת
    std::vector<Lane> lanes_;// רשימת הנתיבים בצומת
    std::vector<Action> validPhases_;// רשימת הפאזות החוקיות בצומת

    int activePhaseId_ = -1;// מזהה הפאזה הפעילה כרגע
    double phaseStartSec_ = 0.0;// זמן התחלת הפאזה הפעילה
    double lastPhaseSwitchSec_ = -1.0;// זמן ההחלפה האחרון של הפאזות
    double minGreenSec_ = 5.0;// זמן ירוק מינימלי לכל פאזה
    double maxGreenSec_ = 60.0;// זמן ירוק מקסימלילכל פאזה
    double yellowSec_ = 2.0;// זמן צהוב בין פאזה לפאזה
    double allRedSec_ = 1.0;// זמן "הכול אדום" בין פאזה לפאזה
    double interGreenSec_ = 3.0;// זמן כולל של צהוב + הכול אדום בין פאזה לפאזה

    bool emergencyActive_ = false;// האם יש מצב חירום פעיל בצומת
    std::optional<int> emergencyLaneId_;//הנתיב בו נמצא החירום במידה ופעיל
    std::unordered_set<std::uint64_t> conflictPairs_;// סט של זוגות נתיבים בקונפליקט, מאוחסן כמפתחות ייחודיים
};

} // namespace traffic