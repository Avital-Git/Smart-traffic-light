#include "PhaseConfig.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace traffic {
namespace {
//פננקציית עזר לבדיקת קיימות הקובץ
bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream in(path);//מנסה לפתוח את הקובץ
    return in.good();
}
//פונקציית עזר לקביעת הנתיב לקובץ הקונפיגורציה של הפאזות
std::string resolve_phase_config_path(const std::string& preferredPath) {
    if (!preferredPath.empty()) {
        return preferredPath;
    }
//בדיקה אם משתנה הסביבה TRAFFIC_PHASES_FILE מוגדר
    if (const char* envPath = std::getenv("TRAFFIC_PHASES_FILE"); envPath != nullptr) {
        const std::string candidate = envPath;
        if (!candidate.empty()) return candidate;
    }
//רשימת נתיבים אפשריים לקובץ הקונפיגורציה
    const std::vector<std::string> candidates = {
        "traffic_phases.json",
        "cpp/traffic_phases.json",
        "../traffic_phases.json",
        "../cpp/traffic_phases.json",
        "../../traffic_phases.json",
        "../../cpp/traffic_phases.json",
    };
//בדיקה אם הקובץ קיים באחד הנתיבים האפשריים
    for (const auto& candidate : candidates) {
        if (file_exists(candidate)) return candidate;
    }

    return "traffic_phases.json";//ברירת מחדל אם לא נמצא קובץ
}
//פונקציה לקריאת תוכן הקובץ כולו כטקסט
std::optional<std::string> read_all_text(const std::string& path) {
    std::ifstream in(path);//מנסה לפתוח את הקובץ
    if (!in.is_open()) return std::nullopt;//אם לא מצליח לפתוח את הקובץ מחזיר ערך ריק

    std::ostringstream buffer;//יוצר סטרינג סטרים לאגירת תוכן הקובץ
    buffer << in.rdbuf();//קורא את תוכן הקובץ ומכניס אותו ל-buffer
    return buffer.str();//מחזיר את תוכן הקובץ כמחרוזת
}
//פונקציה לשליפת תוכן מקובץ הנמצא בתוך סוגריים מאוזנים (למשל JSON) מהטקסט הנתון
std::optional<std::string> extract_balanced(const std::string& text, std::size_t startPos, char openChar, char closeChar) {//פונקציה שמקבלת מחרוזת, מיקום התחלה, תו פתיחה ותו סגירה
    if (startPos >= text.size() || text[startPos] != openChar) return std::nullopt;//בודקת אם המיקום ההתחלתי תקין ואם התו במיקום זה הוא תו הפתיחה

    int depth = 0;//משתנה למעקב אחרי עומק הסוגריים
    const std::size_t begin = startPos;//שומרת את מיקום ההתחלה
    for (std::size_t i = startPos; i < text.size(); ++i) {//לולאה שעוברת על כל התווים במחרוזת מהתחלה
        if (text[i] == openChar) {//אם התו הוא תו פתיחה, מגדילה את העומק
            ++depth;
        } else if (text[i] == closeChar) {//אם התו הוא תו סגירה, מקטינה את העומק
            --depth;
            if (depth == 0) {//אם העומק חזר ל-0, זה אומר שמצאנו את הסוגריים התואמים
                return text.substr(begin, i - begin + 1);//מחזירה את התוכן שבין הסוגריים
            }
        }
    }
    return std::nullopt;//אם לא נמצא זוג סוגריים תואם, מחזירה ערך ריק
}
//פונקציה לשליפת אובייקט JSON מתוך מחרוזת JSON לפי מפתח נתון
std::optional<std::string> extract_json_object(const std::string& json, const std::string& key) {//פונקציה שמקבלת מחרוזת JSON ומפתח, ומחזירה את האובייקט התואם למפתח זה
    const std::string token = "\"" + key + "\"";// יוצר את המחרוזת שמייצגת את המפתח בתוך JSON (למשל "key")
    const std::size_t keyPos = json.find(token);// מחפש את המיקום של המפתח במחרוזת JSON
    if (keyPos == std::string::npos) return std::nullopt;// אם המפתח לא נמצא, מחזיר ערך ריק

    const std::size_t openPos = json.find('{', keyPos);// מחפש את המיקום של הסוגריים הפותחים של האובייקט
    if (openPos == std::string::npos) return std::nullopt;// אם לא נמצא סוגריים פותחים, מחזיר ערך ריק

    return extract_balanced(json, openPos, '{', '}');// מחזיר את האובייקט המלא כולל הסוגריים

}
//פונקציה לשליפת מערך JSON מתוך מחרוזת JSON לפי מפתח נתון
std::optional<std::string> extract_json_array(const std::string& json, const std::string& key) {//פונקציה שמקבלת מחרוזת JSON ומפתח, ומחזירה את המערך התואם למפתח זה
    const std::string token = "\"" + key + "\"";// יוצר את המחרוזת שמייצגת את המפתח בתוך JSON (למשל "key")
    const std::size_t keyPos = json.find(token);// מחפש את המיקום של המפתח במחרוזת JSON
    if (keyPos == std::string::npos) return std::nullopt;// אם המפתח לא נמצא, מחזיר ערך ריק

    const std::size_t openPos = json.find('[', keyPos);// מחפש את המיקום של הסוגריים הפותחים של המערך
    if (openPos == std::string::npos) return std::nullopt;// אם לא נמצא סוגריים פותחים, מחזיר ערך ריק

    return extract_balanced(json, openPos, '[', ']');// מחזיר את המערך המלא כולל הסוגריים
}
// פונקציה שמפרקת מחרוזת JSON שמייצגת מערך של מספרים שלמים ומחזירה וקטור של מספרים שלמים
std::vector<int> parse_int_list(const std::string& jsonArray) {
    std::vector<int> out;

    std::regex intRe("-?\\d+");// regex שמזהה מספרים שלמים (כולל שליליים)
    auto begin = std::sregex_iterator(jsonArray.begin(), jsonArray.end(), intRe);// יוצר איטרטור שמתחיל מההתחלה של המחרוזת ומחפש את כל ההתאמות ל-regex
    auto end = std::sregex_iterator();// איטרטור שמסמן את סוף ההתאמות
    for (auto it = begin; it != end; ++it) {// לולאה שעוברת על כל ההתאמות שנמצאו
        try {
            out.push_back(std::stoi((*it)[0].str()));// ממיר את ההתאמה למספר שלם ומוסיף לווקטור
        } catch (...) {
            // אם המחרוזת אינה תקינה, מתעלם מהטוקן
        }
    }

    return out;// מחזיר את הווקטור עם כל המספרים שנמצאו
}
//פונקציה שמחזירה את הפאזות המוגדרות עבור הצומת
std::vector<Action> parse_phases_from_intersection_object(const std::string& intersectionObj) {//פונקציה שמקבלת מחרוזת JSON שמייצגת אובייקט של צומת ומחזירה וקטור של פעולות (פאזות) המוגדרות עבור הצומת
    std::vector<Action> phases;//יוצר וקטור ריק לאחסון הפאזות

    const auto phasesArray = extract_json_array(intersectionObj, "phases");//מנסה לשלוף את המערך של הפאזות מתוך האובייקט של הצומת
    if (!phasesArray.has_value()) return phases;//אם לא נמצא מערך פאזות, מחזיר את הווקטור הריק

    std::regex phaseObjRe("\\{[^\\}]*\\\"phase_id\\\"\\s*:\\s*(-?\\d+)[^\\}]*\\\"green_lanes\\\"\\s*:\\s*(\\[[^\\]]*\\])[^\\}]*\\}");// regex שמזהה אובייקט JSON שמייצג פאזה עם מזהה הפאזה ומערך הנתיבים הירוקים
    auto begin = std::sregex_iterator(phasesArray->begin(), phasesArray->end(), phaseObjRe);// יוצר איטרטור שמתחיל מההתחלה של המחרוזת ומחפש את כל ההתאמות ל-regex
    auto end = std::sregex_iterator();// איטרטור שמסמן את סוף ההתאמות

    for (auto it = begin; it != end; ++it) {//לולאה שעוברת על כל ההתאמות שנמצאו
        Action action;//יוצר אובייקט Action חדש
        action.phaseId = -1;//מאתחל את מזהה הפאזה לערך ברירת מחדל
        try {
            action.phaseId = std::stoi((*it)[1].str());//ממיר את המחרוזת שמייצגת את מזהה הפאזה למספר שלם ומאחסן אותו באובייקט Action
        } catch (...) {
            continue;//אם המחרוזת אינה תקינה, ממשיך לאובייקט הבא
        }

        action.greenLanes = parse_int_list((*it)[2].str());//ממיר את המחרוזת שמייצגת את מערך הנתיבים הירוקים לווקטור של מספרים שלמים ומאחסן אותו באובייקט Action
        action.greenLanes.erase(// מסיר נתיבים עם מזהים שליליים מהווקטור של הנתיבים הירוקים
            std::remove_if(action.greenLanes.begin(), action.greenLanes.end(), [](int laneId) { return laneId < 0; }),// פונקציה שמסירה נתיבים עם מזהים שליליים מהווקטור של הנתיבים הירוקים
            action.greenLanes.end()// מסמן את סוף הווקטור לאחר הסרת הנתיבים הלא תקינים
        );

        std::sort(action.greenLanes.begin(), action.greenLanes.end());// ממיין את הווקטור של הנתיבים הירוקים בסדר עולה
        action.greenLanes.erase(std::unique(action.greenLanes.begin(), action.greenLanes.end()), action.greenLanes.end());// מסיר כפילויות מהווקטור של הנתיבים הירוקים

        if (action.phaseId < 0 || action.greenLanes.empty()) continue;// אם מזהה הפאזה אינו תקין או שאין נתיבים ירוקים, ממשיך לאובייקט הבא
        phases.push_back(std::move(action));// מוסיף את האובייקט Action לווקטור של הפאזות
    }

    std::sort(phases.begin(), phases.end(), [](const Action& a, const Action& b) {// ממיין את הווקטור של הפאזות לפי מזהי הפאזות בסדר עולה
        return a.phaseId < b.phaseId;// פונקציה שמחזירה true אם מזהה הפאזה של האובייקט הראשון קטן מזה של האובייקט השני
    });

    return phases;// מחזיר את הווקטור של הפאזות הממוינות
}

} // namespace
// פונקציה שטוענת את קונפיגורציות הפאזות מהקובץ אם לא תקין או חסר מחזירה ברירית מחדל
PhaseConfig loadPhaseConfig(const std::string& preferredPath) {
    PhaseConfig cfg;

    const std::string path = resolve_phase_config_path(preferredPath);//קובע את הנתיב לקובץ הקונפיגורציה של הפאזות
    const auto json = read_all_text(path);//מנסה לקרוא את תוכן הקובץ כולו כטקסט
    if (!json.has_value()) {//אם לא מצליח לקרוא את הקובץ, מחזיר קונפיגורציה ריקה עם מקור ברירת מחדל
        cfg.source = "defaults (missing: " + path + ")";//מעדכן את מקור הקונפיגורציה למצב של קובץ חסר
        return cfg;// מחזיר את הקונפיגורציה הריקה
    }

    const auto intersectionsObj = extract_json_object(*json, "intersections");//מנסה לשלוף את האובייקט של הצמתים מתוך הקובץ JSON
    if (!intersectionsObj.has_value()) {
        cfg.source = "defaults (invalid: " + path + ")";//מעדכן את מקור הקונפיגורציה למצב של קובץ לא תקין
        return cfg;// מחזיר את הקונפיגורציה הריקה
    }

    std::regex intersectionKeyRe("\\\"(\\d+)\\\"\\s*:");// regex שמזהה מפתחות של צמתים במחרוזת JSON (למשל "123":)
    auto begin = std::sregex_iterator(intersectionsObj->begin(), intersectionsObj->end(), intersectionKeyRe);// יוצר איטרטור שמתחיל מההתחלה של המחרוזת ומחפש את כל ההתאמות ל-regex
    auto end = std::sregex_iterator();// איטרטור שמסמן את סוף ההתאמות

    for (auto it = begin; it != end; ++it) {//לולאה שעוברת על כל ההתאמות שנמצאו
        int intersectionId = -1;//מאתחל את מזהה הצומת לערך ברירת מחדל
        try {
            intersectionId = std::stoi((*it)[1].str());//ממיר את המחרוזת שמייצגת את מזהה הצומת למספר שלם
        } catch (...) {
            continue;//אם המחרוזת אינה תקינה, ממשיך לאובייקט הבא
        }

        if (intersectionId < 0) continue;//אם מזהה הצומת אינו תקין, ממשיך לאובייקט הבא

        const std::size_t keyPos = static_cast<std::size_t>((*it).position());//שומר את המיקום של המפתח במחרוזת JSON
        const std::size_t objectOpenPos = intersectionsObj->find('{', keyPos);//מחפש את המיקום של הסוגר הפותח של האובייקט
        if (objectOpenPos == std::string::npos) continue;//אם לא נמצא הסוגר הפותח, ממשיך לאובייקט הבא

        const auto intersectionObj = extract_balanced(*intersectionsObj, objectOpenPos, '{', '}');//מנסה לשלוף את האובייקט המלא של הצומת
        if (!intersectionObj.has_value()) continue;//אם לא מצליח לשלוף את האובייקט, ממשיך לאובייקט הבא

        auto phases = parse_phases_from_intersection_object(*intersectionObj);//מנסה לפרק את הפאזות מתוך האובייקט של הצומת
        if (!phases.empty()) {//אם נמצאו פאזות תקינות, מוסיף אותן למיפוי של הצמתים לפאזות
            cfg.phasesByIntersection[intersectionId] = std::move(phases);//מעביר את הווקטור של הפאזות למיפוי ומונע העתקה מיותרת
        }
    }

    cfg.source = path;//מעדכן את מקור הקונפיגורציה לנתיב הקובץ שנמצא
    return cfg;// מחזיר את הקונפיגורציה עם הפאזות שנמצאו
}
// פונקציה שמחזירה את הפאזות המוגדרות עבור צומת מסוים לאחר סינון הנתיבים שאינם קיימים בטופולוגיה הנוכחית
std::vector<Action> resolveConfiguredPhases(
    int intersectionId,
    const std::vector<int>& availableLaneIds,// רשימת מזהי הנתיבים הזמינים בצומת
    const PhaseConfig& config// קונפיגורציה של הפאזות
) {
    const auto found = config.phasesByIntersection.find(intersectionId);// מחפש את הצומת במיפוי של הצמתים לפאזות
    if (found == config.phasesByIntersection.end()) {// אם הצומת לא נמצא במיפוי, מחזיר וקטור ריק
        return {};
    }

    std::unordered_set<int> laneSet(availableLaneIds.begin(), availableLaneIds.end());// יוצר סט של מזהי הנתיבים הזמינים לצורך בדיקה מהירה של קיום הנתיבים

    std::vector<Action> filtered;// יוצר וקטור ריק לאחסון הפאזות לאחר הסינון
    filtered.reserve(found->second.size());// שומר מקום מראש לווקטור כדי למנוע הקצאות מיותרות

    for (const auto& action : found->second) {// עובר על כל הפאזות שנמצאו עבור הצומת
        bool allLanesExist = true;// משתנה שמציין אם כל הנתיבים הירוקים בפאזה קיימים בטופולוגיה הנוכחית
        for (int laneId : action.greenLanes) {// עובר על כל הנתיבים הירוקים בפאזה
            if (laneSet.find(laneId) == laneSet.end()) {// אם הנתיב הירוק אינו נמצא בסט של הנתיבים הזמינים, מסמן שהפאזה אינה תקינה
                allLanesExist = false;// מסמן שהפאזה אינה תקינה
                break;// עוצר את הלולאה כי מצאנו נתיב שאינו קיים
            }
        }
        if (!allLanesExist) continue;// אם הפאזה אינה תקינה, ממשיך לפאזה הבאה
        filtered.push_back(action);// אם הפאזה תקינה, מוסיף אותה לווקטור של הפאזות לאחר הסינון
    }

    return filtered;// מחזיר את הווקטור של הפאזות לאחר הסינון
}

} // namespace traffic
