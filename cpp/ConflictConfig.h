#pragma once

// הגדרת מבנה הנתונים וממשק הטעינה של קונפיגורציית קונפליקטים בין נתיבים

#include <string>   // std::string
#include <utility>  // std::pair
#include <vector>   // std::vector

namespace traffic {

// מבנה המכיל את רשימת זוגות הנתיבים הסותרים
struct LaneConflictConfig {
    // זוגות מזהי נתיבים שאסור שיהיו ירוקים בו-זמנית
    std::vector<std::pair<int, int>> conflictPairs;

    // מקור הקונפיגורציה — לצורך אבחון ולוגים
    std::string source = "defaults";
};

// טוען קונפליקטים מקובץ JSON בנתיב נתון.
// אם הנתיב ריק — מחפש בנתיבי ברירת מחדל.
// אם הקובץ חסר או פגום — מחזיר רשימת קונפליקטים ריקה.
LaneConflictConfig loadLaneConflictConfig(const std::string& preferredPath = "");

// טוען קונפליקטים עם תמיכה בעקיפה ספציפית לצומת.
// סדר עדיפויות:
// 1) משתנה סביבה TRAFFIC_CONFLICTS_FILE
// 2) ./lane_conflicts_<intersectionId>.json
// 3) ./cpp/lane_conflicts_<intersectionId>.json
// 4) טעינה גנרית דרך loadLaneConflictConfig(preferredPath)
LaneConflictConfig loadLaneConflictConfigForIntersection(int intersectionId, const std::string& preferredPath = "");

} // namespace traffic
