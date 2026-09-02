#include "NeighborCoordConfig.h" // כולל את הגדרות המבנה NeighborCoordConfig

#include <algorithm>  // לפונקציות std::max ו-std::clamp
#include <cstdlib>    // לפונקציה std::getenv לקריאת משתני סביבה
#include <fstream>    // לקריאת קבצים מהדיסק
#include <optional>   // לטיפוס std::optional להחזרת ערכים אופציונליים
#include <regex>      // לחיפוש שדות JSON בעזרת ביטויים רגולריים
#include <sstream>    // לבניית מחרוזות מתוכן קובץ

namespace traffic {
namespace { // namespace אנונימי — כל הפונקציות כאן הן פנימיות לקובץ זה בלבד

bool file_exists(const std::string& path) { // בודקת אם קובץ קיים בנתיב הנתון
    if (path.empty()) return false; // נתיב ריק — הקובץ בוודאי לא קיים
    std::ifstream in(path); // מנסה לפתוח את הקובץ לקריאה
    return in.good(); // מחזירה true אם הפתיחה הצליחה
}

std::optional<std::string> read_all_text(const std::string& path) { // קוראת את כל תוכן הקובץ כמחרוזת אחת
    std::ifstream in(path); // פותחת את הקובץ לקריאה
    if (!in.is_open()) return std::nullopt; // אם הפתיחה נכשלה — מחזירה ריק

    std::ostringstream buffer; // מאגר זמני לאחסון תוכן הקובץ
    buffer << in.rdbuf(); // קוראת את כל תוכן הקובץ למאגר
    return buffer.str(); // מחזירה את התוכן כמחרוזת
}

std::optional<double> extract_double_field(const std::string& json, const std::string& key) { // מחלצת שדה מספרי (double) מ-JSON גולמי בעזרת ביטוי רגולרי
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)"); // בונה ביטוי רגולרי לחיפוש "key": מספר
    std::smatch m; // משתנה לאחסון תוצאת ההתאמה
    if (std::regex_search(json, m, re)) { // מחפשת את השדה ב-JSON
        try {
            return std::stod(m[1].str()); // ממירה את הערך שנמצא למספר עשרוני
        } catch (...) {
            return std::nullopt; // המרה נכשלה — מחזירה ריק
        }
    }
    return std::nullopt; // השדה לא נמצא ב-JSON — מחזירה ריק
}

std::optional<std::string> extract_string_field(const std::string& json, const std::string& key) { // מחלצת שדה מחרוזת מ-JSON גולמי בעזרת ביטוי רגולרי
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""); // בונה ביטוי רגולרי לחיפוש "key": "ערך"
    std::smatch m; // משתנה לאחסון תוצאת ההתאמה
    if (std::regex_search(json, m, re)) { // מחפשת את השדה ב-JSON
        return m[1].str(); // מחזירה את הערך שנמצא בין מרכאות
    }
    return std::nullopt; // השדה לא נמצא ב-JSON — מחזירה ריק
}

std::optional<bool> extract_bool_field(const std::string& json, const std::string& key) { // מחלצת שדה בוליאני (true/false) מ-JSON גולמי
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*(true|false)"); // בונה ביטוי רגולרי לחיפוש "key": true/false
    std::smatch m; // משתנה לאחסון תוצאת ההתאמה
    if (std::regex_search(json, m, re)) { // מחפשת את השדה ב-JSON
        return m[1].str() == "true"; // מחזירה true אם הערך הוא "true", אחרת false
    }
    return std::nullopt; // השדה לא נמצא ב-JSON — מחזירה ריק
}

void clamp_weights(NeighborCoordConfig& cfg) { // מגבילה את כל הפרמטרים לטווחים תקינים כדי למנוע ערכים קיצוניים
    cfg.rewardSyncBonus = std::max(0.0, cfg.rewardSyncBonus); // בונוס סנכרון לא יכול להיות שלילי
    cfg.rewardOpposingPenalty = std::max(0.0, cfg.rewardOpposingPenalty); // קנס ניגוד לא יכול להיות שלילי
    cfg.rewardSyncWhenQueueWorseScale = std::clamp(cfg.rewardSyncWhenQueueWorseScale, 0.0, 2.0); // מכפיל סנכרון בתור גרוע — בין 0 ל-2
    cfg.rewardOpposingWhenQueueWorseScale = std::clamp(cfg.rewardOpposingWhenQueueWorseScale, 0.0, 3.0); // מכפיל ניגוד בתור גרוע — בין 0 ל-3
    cfg.rewardOpposingWhenQueueBetterScale = std::clamp(cfg.rewardOpposingWhenQueueBetterScale, 0.0, 2.0); // מכפיל ניגוד בתור טוב — בין 0 ל-2

    cfg.ruleSyncWeight = std::max(0.0, cfg.ruleSyncWeight); // משקל כלל הסנכרון לא יכול להיות שלילי
    cfg.ruleEmergencyBonus = std::max(0.0, cfg.ruleEmergencyBonus); // בונוס כלל חירום לא יכול להיות שלילי
    cfg.ruleOpposingWeight = std::max(0.0, cfg.ruleOpposingWeight); // משקל כלל הניגוד לא יכול להיות שלילי
    cfg.localWeightDivisor = std::max(1.0, cfg.localWeightDivisor); // מחלק המשקל המקומי חייב להיות לפחות 1 (מניעת חלוקה ב-0)
}

} // namespace — סוף ה-namespace האנונימי

std::string resolveNeighborCoordConfigPath(const std::string& preferredPath) { // קובעת את נתיב קובץ התצורה — לפי עדיפות: ארגומנט → סביבה → חיפוש → ברירת מחדל
    if (!preferredPath.empty()) { // אם הועבר נתיב מפורש
        return preferredPath; // מחזירה אותו ישירות ללא חיפוש
    }

    if (const char* envPath = std::getenv("TRAFFIC_NEIGHBOR_TUNING_FILE"); envPath != nullptr) { // בודקת אם משתנה הסביבה מוגדר
        const std::string candidate = envPath; // ממירה את הערך למחרוזת C++
        if (!candidate.empty()) { // אם הערך אינו ריק
            return candidate; // מחזירה את הנתיב ממשתנה הסביבה
        }
    }

    const std::string candidates[] = { // רשימת נתיבים לחיפוש הקובץ לפי סדר עדיפות
        "neighbor_tuning_balanced.json",        // נתיב ראשון — תיקיית העבודה הנוכחית
        "cpp/neighbor_tuning_balanced.json",    // נתיב שני — תת-תיקיית cpp
        "../neighbor_tuning_balanced.json",     // נתיב שלישי — תיקיית אב
        "../../neighbor_tuning_balanced.json",  // נתיב רביעי — שתי תיקיות למעלה
    };

    for (const auto& candidate : candidates) { // עוברת על כל הנתיבים ובודקת אם הקובץ קיים
        if (file_exists(candidate)) { // אם הקובץ נמצא בנתיב הנוכחי
            return candidate; // מחזירה את הנתיב הראשון שנמצא
        }
    }

    return "neighbor_tuning_balanced.json"; // ברירת מחדל אם לא נמצא קובץ בשום נתיב
}

NeighborCoordConfig loadNeighborCoordConfig(const std::string& preferredPath) { // טוענת את תצורת תיאום השכנים מקובץ JSON ומחזירה מבנה מאוכלס
    NeighborCoordConfig cfg; // יוצרת מבנה תצורה עם ערכי ברירת מחדל

    const std::string path = resolveNeighborCoordConfigPath(preferredPath); // קובעת את הנתיב הסופי לקובץ התצורה
    const auto json = read_all_text(path); // קוראת את תוכן הקובץ כמחרוזת
    if (!json.has_value()) { // אם הקריאה נכשלה (קובץ לא קיים או לא נגיש)
        cfg.source = "defaults (missing: " + path + ")"; // מתעדת שנטענו ערכי ברירת מחדל ואיזה קובץ חסר
        return cfg; // מחזירה את המבנה עם ערכי ברירת מחדל בלבד
    }

    if (const auto profileName = extract_string_field(*json, "profile_name"); profileName.has_value()) { // מחלצת את שם הפרופיל מה-JSON
        cfg.profileName = *profileName; // שומרת את שם הפרופיל במבנה
    }

    if (const auto v = extract_bool_field(*json, "include_neighbor_in_state_encoding"); v.has_value()) cfg.includeNeighborInStateEncoding = *v; // האם לכלול מצב שכן בקידוד מצב ה-RL
    if (const auto v = extract_bool_field(*json, "include_neighbor_in_rule_scoring"); v.has_value()) cfg.includeNeighborInRuleScoring = *v; // האם לכלול שכן בניקוד כללים
    if (const auto v = extract_bool_field(*json, "include_neighbor_in_reward"); v.has_value()) cfg.includeNeighborInReward = *v; // האם לכלול שכן בחישוב הפרס

    if (const auto v = extract_double_field(*json, "reward_sync_bonus"); v.has_value()) cfg.rewardSyncBonus = *v; // בונוס פרס כשפעולת הצומת מסונכרנת עם השכן
    if (const auto v = extract_double_field(*json, "reward_opposing_penalty"); v.has_value()) cfg.rewardOpposingPenalty = *v; // קנס פרס כשהפעולות מנוגדות לשכן
    if (const auto v = extract_double_field(*json, "reward_sync_when_queue_worse_scale"); v.has_value()) cfg.rewardSyncWhenQueueWorseScale = *v; // מכפיל בונוס סנכרון כשהתור בשכן גרוע יותר
    if (const auto v = extract_double_field(*json, "reward_opposing_when_queue_worse_scale"); v.has_value()) cfg.rewardOpposingWhenQueueWorseScale = *v; // מכפיל קנס ניגוד כשהתור בשכן גרוע יותר
    if (const auto v = extract_double_field(*json, "reward_opposing_when_queue_better_scale"); v.has_value()) cfg.rewardOpposingWhenQueueBetterScale = *v; // מכפיל קנס ניגוד כשהתור בשכן טוב יותר
    if (const auto v = extract_double_field(*json, "rule_sync_weight"); v.has_value()) cfg.ruleSyncWeight = *v; // משקל כלל הסנכרון בניקוד הפעולה
    if (const auto v = extract_double_field(*json, "rule_emergency_bonus"); v.has_value()) cfg.ruleEmergencyBonus = *v; // בונוס כלל כשיש חירום פעיל בשכן
    if (const auto v = extract_double_field(*json, "rule_opposing_weight"); v.has_value()) cfg.ruleOpposingWeight = *v; // משקל כלל הניגוד בניקוד הפעולה
    if (const auto v = extract_double_field(*json, "local_weight_divisor"); v.has_value()) cfg.localWeightDivisor = *v; // מחלק למשקל המקומי — מאזן בין שיקול מקומי לשיקול שכן

    clamp_weights(cfg); // מגבילה את כל הפרמטרים לטווחים תקינים
    cfg.source = path; // שומרת את הנתיב שממנו נטענה התצורה
    return cfg; // מחזירה את המבנה המאוכלס
}

} // namespace traffic
