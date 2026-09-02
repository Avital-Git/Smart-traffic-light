// ConflictConfig.cpp
// טוען ומפרסר קונפיגורציית קונפליקטים בין נתיבים מקובץ JSON.
// קונפליקט = זוג נתיבים שאסור שיהיו ירוקים בו-זמנית (למשל: זרימות נגד-כיוון).

#include "ConflictConfig.h"

#include <algorithm> // std::sort, std::unique, std::swap
#include <cstdlib>   // std::getenv
#include <fstream>   // std::ifstream
#include <optional>  // std::optional
#include <regex>     // std::regex, std::sregex_iterator
#include <sstream>   // std::ostringstream

namespace traffic {
namespace {

// בודק אם קובץ קיים וניתן לקריאה
bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream in(path);
    return in.good();
}

// קורא את כל תוכן הקובץ למחרוזת; מחזיר nullopt אם הקובץ לא נפתח
std::optional<std::string> read_all_text(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// מחלץ מערך JSON לפי שם מפתח — מחזיר את מחרוזת המערך כולה (כולל סוגריים)
// מטפל בקינון מערכים בעזרת מונה depth
std::optional<std::string> extract_json_array(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t kpos = json.find(token);
    if (kpos == std::string::npos) return std::nullopt;

    std::size_t pos = json.find('[', kpos);
    if (pos == std::string::npos) return std::nullopt;

    int depth = 0;
    const std::size_t start = pos;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '[') {
            ++depth;         // כניסה לרמה נוספת
        } else if (json[pos] == ']') {
            --depth;
            if (depth == 0) {
                return json.substr(start, pos - start + 1); // מצא את סוף המערך
            }
        }
    }

    return std::nullopt; // מבנה JSON לא תקין
}

// מפרסר את מערך הקונפליקטים מה-JSON ומחזיר זוגות מסודרים ומנוכים כפילויות
// פורמט קלט: { "conflicts": [[0,1],[2,3],...] }
std::vector<std::pair<int, int>> parse_conflict_pairs(const std::string& json) {
    std::vector<std::pair<int, int>> pairs;

    // חילוץ מערך הקונפליקטים מהקובץ
    const auto conflictsArray = extract_json_array(json, "conflicts");
    if (!conflictsArray.has_value()) {
        return pairs; // אין מפתח conflicts — רשימה ריקה
    }

    // regex למציאת זוגות מהצורה [a, b]
    std::regex pairRe("\\[\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*\\]");
    auto begin = std::sregex_iterator(conflictsArray->begin(), conflictsArray->end(), pairRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        int a = -1;
        int b = -1;
        try {
            a = std::stoi((*it)[1].str());
            b = std::stoi((*it)[2].str());
        } catch (...) {
            continue; // ערך לא מספרי — מדלג
        }

        if (a < 0 || b < 0 || a == b) continue; // נתיב לא חוקי או קונפליקט עם עצמו
        if (a > b) std::swap(a, b);              // שמירה על סדר עקבי: a < b תמיד
        pairs.emplace_back(a, b);
    }

    // מיון והסרת כפילויות
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

// קובע את נתיב קובץ הקונפליקטים לפי סדר עדיפויות:
// 1) נתיב מפורש שהועבר מהקורא
// 2) משתנה סביבה TRAFFIC_CONFLICTS_FILE
// 3) חיפוש בנתיבים קבועים (תיקייה נוכחית / cpp / רמה מעלה)
// 4) ברירת מחדל: lane_conflicts.json
std::string resolve_lane_conflicts_path(const std::string& preferredPath) {
    if (!preferredPath.empty()) {
        return preferredPath; // נתיב מפורש — משתמשים בו ישירות
    }

    if (const char* envPath = std::getenv("TRAFFIC_CONFLICTS_FILE"); envPath != nullptr) {
        const std::string candidate = envPath;
        if (!candidate.empty()) {
            return candidate; // נתיב ממשתנה סביבה
        }
    }

    // חיפוש בנתיבים אפשריים — מהקרוב לרחוק
    const std::vector<std::string> candidates = {
        "lane_conflicts.json",
        "cpp/lane_conflicts.json",
        "../lane_conflicts.json",
        "../cpp/lane_conflicts.json",
        "../../lane_conflicts.json",
        "../../cpp/lane_conflicts.json",
    };

    for (const auto& candidate : candidates) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }

    return "lane_conflicts.json"; // ברירת מחדל — יטופל כקובץ חסר
}

} // namespace

// פונקציה ציבורית: טוענת קונפיגורציית קונפליקטים מקובץ JSON
// אם הקובץ חסר — מחזירה רשימה ריקה עם הודעת מקור לאבחון
LaneConflictConfig loadLaneConflictConfig(const std::string& preferredPath) {
    LaneConflictConfig cfg;

    const std::string path = resolve_lane_conflicts_path(preferredPath); // שלב 1: קביעת נתיב
    const auto json = read_all_text(path);                               // שלב 2: קריאת הקובץ
    if (!json.has_value()) {
        cfg.source = "defaults (missing: " + path + ")";
        return cfg; // קובץ חסר — רשימה ריקה
    }

    cfg.conflictPairs = parse_conflict_pairs(*json); // שלב 3: פרסור
    cfg.source = path;
    return cfg;
}

// פונקציה ציבורית: טוענת קונפליקטים עם תמיכה בקובץ ספציפי לצומת
// סדר עדיפויות:
// 1) משתנה סביבה TRAFFIC_CONFLICTS_FILE (עוקף הכל)
// 2) נתיב מפורש שהועבר
// 3) קובץ ספציפי לצומת: lane_conflicts_<id>.json
// 4) קובץ ספציפי ב-cpp/: cpp/lane_conflicts_<id>.json
// 5) טעינה גנרית (lane_conflicts.json)
LaneConflictConfig loadLaneConflictConfigForIntersection(int intersectionId, const std::string& preferredPath) {
    // עדיפות 1: משתנה סביבה גלובלי
    if (const char* envPath = std::getenv("TRAFFIC_CONFLICTS_FILE"); envPath != nullptr) {
        const std::string globalOverride = envPath;
        if (!globalOverride.empty()) {
            return loadLaneConflictConfig(globalOverride);
        }
    }

    // עדיפות 2: נתיב מפורש מהקורא
    if (!preferredPath.empty()) {
        return loadLaneConflictConfig(preferredPath);
    }

    // עדיפות 3+4: קובץ ספציפי לצומת
    if (intersectionId >= 0) {
        const std::string candidateA = "lane_conflicts_" + std::to_string(intersectionId) + ".json";
        if (file_exists(candidateA)) {
            return loadLaneConflictConfig(candidateA);
        }

        const std::string candidateB = "cpp/lane_conflicts_" + std::to_string(intersectionId) + ".json";
        if (file_exists(candidateB)) {
            return loadLaneConflictConfig(candidateB);
        }
    }

    // עדיפות 5: טעינה גנרית
    return loadLaneConflictConfig();
}

} // namespace traffic
