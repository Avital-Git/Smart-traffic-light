#pragma once // מונע הכללה כפולה של קובץ זה ביחידות קומפילציה שונות

#include <string> // לטיפוס std::string המשמש לשם פרופיל ומקור

namespace traffic { // כל הגדרות הפרויקט נמצאות תחת namespace זה

struct NeighborCoordConfig { // מבנה המחזיק את כל פרמטרי תיאום השכנים בין צמתים
    // Control where neighbor coordination is applied
    bool includeNeighborInStateEncoding = false; // האם לכלול נתוני שכן בוקטור המצב שמוזן לסוכן RL (ברירת מחדל: כבוי)
    bool includeNeighborInRuleScoring = true;    // האם לכלול שכן בניקוד הכללים של הבקר הממוצע (ברירת מחדל: פעיל)
    bool includeNeighborInReward = true;         // האם לשקלל סנכרון/ניגוד עם שכן בחישוב הפרס (ברירת מחדל: פעיל)

    // Reward-side coordination weights
    double rewardSyncBonus = 5.0;                      // בונוס פרס כשהצומת ושכנו בוחרים באותה פאזה (סנכרון)
    double rewardOpposingPenalty = 2.5;                // קנס פרס כשהצומת ושכנו בוחרים בפאזות מנוגדות
    double rewardSyncWhenQueueWorseScale = 0.25;       // מכפיל הקטנת בונוס סנכרון כשהתור בשכן גרוע יותר — מרסן עידוד עיוור
    double rewardOpposingWhenQueueWorseScale = 1.25;   // מכפיל הגדלת קנס ניגוד כשהתור בשכן גרוע יותר — מחמיר את העונש
    double rewardOpposingWhenQueueBetterScale = 0.60;  // מכפיל הקטנת קנס ניגוד כשהתור בשכן טוב יותר — מקל כי הניגוד מועיל

    // Rule-based action scoring weights
    double ruleSyncWeight = 1.2;      // משקל תוספת ניקוד כשפעולה מסונכרנת עם פעולת השכן
    double ruleEmergencyBonus = 1.5;  // בונוס ניקוד כשיש רכב חירום פעיל בשכן ויש לפנות לו דרך
    double ruleOpposingWeight = 0.75; // משקל הפחתת ניקוד כשפעולה מנוגדת לשכן שנמצא במצב לחוץ
    double localWeightDivisor = 18.0; // מחלק המאזן בין ניקוד מקומי לניקוד שכן — ערך גבוה מקטין השפעת שכן

    std::string profileName = "default"; // שם הפרופיל שנטען (לדיווח ולוגים)
    std::string source = "defaults";     // מקור התצורה: שם הקובץ שנטען או "defaults" אם לא נמצא קובץ
};

// Loads neighbor coordination weights from JSON.
NeighborCoordConfig loadNeighborCoordConfig(const std::string& preferredPath = ""); // טוענת את תצורת תיאום השכנים מקובץ JSON — נתיב אופציונלי, ברירת מחדל: חיפוש אוטומטי

// Resolves config path by priority:
// 1) preferredPath
// 2) env var TRAFFIC_NEIGHBOR_TUNING_FILE
// 3) ./neighbor_tuning_balanced.json
// 4) ./cpp/neighbor_tuning_balanced.json
std::string resolveNeighborCoordConfigPath(const std::string& preferredPath = ""); // קובעת את נתיב קובץ התצורה לפי סדר עדיפויות: ארגומנט → משתנה סביבה → חיפוש בנתיבים ידועים

} // namespace traffic
