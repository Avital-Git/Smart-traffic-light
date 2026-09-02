#pragma once

#include "Junction.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace traffic {

struct PhaseConfig {
    std::unordered_map<int, std::vector<Action>> phasesByIntersection;// מיפוי של מזהי צומת לפאזות המוגדרות עבורם
    std::string source = "defaults";// מקור הקונפיגורציה 
};

// Loads phase topology config from JSON file path.
// Falls back to empty config when file is missing/invalid.
PhaseConfig loadPhaseConfig(const std::string& preferredPath = "");// טוען את קונפיגורציית הפאזות מקובץ JSON. אם הקובץ חסר או לא תקין, מחזיר קונפיגורציה ריקה.

// Returns configured phases for an intersection after filtering lanes that do not exist
// in the current topology. Returns empty if not configured or invalid.
std::vector<Action> resolveConfiguredPhases(// מחזיר את הפאזות המוגדרות עבור צומת מסוים לאחר סינון הנתיבים שאינם קיימים בטופולוגיה הנוכחית. מחזיר ריק אם לא מוגדר או לא תקין.
    int intersectionId,
    const std::vector<int>& availableLaneIds,// רשימת מזהי הנתיבים הזמינים בצומת
    const PhaseConfig& config
);

} // namespace traffic
