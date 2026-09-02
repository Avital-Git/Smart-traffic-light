#pragma once

#include <string>

namespace traffic {

struct ThreeLevelThresholds {// מבנה שמכיל את הערכים המקסימליים של שלוש רמות שונות
    double lowMax = 0.0;// הערך המקסימלי של הרמה הנמוכה
    double mediumMax = 0.0;// הערך המקסימלי של הרמה הבינונית
};

struct TrafficThresholdConfig {
    // vehicle_count: light <= lowMax, medium <= mediumMax, heavy > mediumMax
    ThreeLevelThresholds vehicleCount{5.0, 15.0};

    // waiting_time_sec: short <= lowMax, medium <= mediumMax, long > mediumMax
    ThreeLevelThresholds waitingTimeSec{20.0, 60.0};

    // density_pct: low <= lowMax, medium <= mediumMax, high > mediumMax
    ThreeLevelThresholds densityPct{30.0, 70.0};

    // Metadata: where the active values came from.
    std::string source = "defaults";
};

// טוען את קובץ התצורה של הסף. אם הנתיב ריק, מחפש במיקומים ברירת מחדל.
// אם הקובץ לא נמצא או לא ניתן לטעון אותו, מחזיר את ערכי ברירת המחדל.
TrafficThresholdConfig loadTrafficThresholdConfig(const std::string& preferredPath = "");

// טוען את הספים עם תמיכה בהחלפה לכל צומת.
// סדר הפתרון:
// 1) משתנה סביבה TRAFFIC_THRESHOLDS_FILE (החלפה גלובלית)
// 2) ./traffic_thresholds_<intersectionId>.json
// 3) ./cpp/traffic_thresholds_<intersectionId>.json
// 4) טוען את קובץ התצורה הכללי loadTrafficThresholdConfig(preferredPath)
TrafficThresholdConfig loadTrafficThresholdConfigForIntersection(int intersectionId, const std::string& preferredPath = "");

// פותר את נתיב קובץ התצורה של הסף לפי סדר עדיפות:
// 1) ארגומנט preferredPath (אם לא ריק)
// 2) משתנה סביבה TRAFFIC_THRESHOLDS_FILE
// 3) ./traffic_thresholds.json
// 4) ./cpp/traffic_thresholds.json
std::string resolveThresholdConfigPath(const std::string& preferredPath = "");

} // namespace traffic
