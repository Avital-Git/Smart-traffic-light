#include "ThresholdConfig.h"
#include "server/Database.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

namespace traffic {
namespace {
//אם הקובץ לא נמצא או לא ניתן לטעון אותו, מחזיר את ערכי ברירת המחדל.
TrafficThresholdConfig defaults_config() {
    return TrafficThresholdConfig{};
}
//פונקציה שמחזירה האם הקובץ קיים
bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream in(path);
    return in.good();
}
//פונקציה שמחזירה את תוכן הקובץ כולו כמחרוזת
std::optional<std::string> read_all_text(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

//פונקציה שמחזירה אובייקט JSON לפי מפתח
std::optional<std::string> extract_json_object(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t kpos = json.find(token);
    if (kpos == std::string::npos) return std::nullopt;

    std::size_t pos = json.find('{', kpos);
    if (pos == std::string::npos) return std::nullopt;

    int depth = 0;
    const std::size_t start = pos;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '{') {
            ++depth;
        } else if (json[pos] == '}') {
            --depth;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);
            }
        }
    }
    return std::nullopt;
}
//פונקציה שמחזירה מערך JSON לפי מפתח
std::optional<double> extract_double_field(const std::string& json, const std::string& key) {
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");// regex שמחפש את המפתח ומחזיר את הערך שלו כמספר עשרוני
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        try {
            return std::stod(m[1].str());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}
//פונקציה שמבצעת הגבלה ותיקון של הספים
void clamp_and_fix(ThreeLevelThresholds& t) {
    t.lowMax = std::max(0.0, t.lowMax);// אם הסף הנמוך קטן מ-0, מחזיר אותו ל-0
    t.mediumMax = std::max(0.0, t.mediumMax);// אם הסף הבינוני קטן מ-0, מחזיר אותו ל-0
    if (t.mediumMax < t.lowMax) {
        t.mediumMax = t.lowMax;// אם הסף הבינוני קטן מהסף הנמוך, מחזיר אותו לערך הסף הנמוך
    }
}
//פונקציה שמחילה את הספים של vehicle_count על התצורה
void apply_vehicle_thresholds(const std::string& rootJson, TrafficThresholdConfig& cfg) {
    const auto obj = extract_json_object(rootJson, "vehicle_count");// מחפש את האובייקט של vehicle_count במחרוזת JSON
    if (!obj.has_value()) return;// אם לא נמצא האובייקט, מחזיר

    if (const auto light = extract_double_field(*obj, "light_max"); light.has_value()) {// אם נמצא את הערך של light_max, מחזיר אותו לסף הנמוך
        cfg.vehicleCount.lowMax = *light;
    } else if (const auto low = extract_double_field(*obj, "low_max"); low.has_value()) {// אם לא נמצא את הערך של light_max, מחפש את הערך של low_max ומחזיר אותו לסף הנמוך
        cfg.vehicleCount.lowMax = *low;
    }

    if (const auto medium = extract_double_field(*obj, "medium_max"); medium.has_value()) {// אם נמצא את הערך של medium_max, מחזיר אותו לסף הבינוני
        cfg.vehicleCount.mediumMax = *medium;// אם לא נמצא את הערך של medium_max, מחפש את הערך של medium_max ומחזיר אותו לסף הבינוני
    }
}
//פונקציה שמחילה את הספים של waiting_time_sec על התצורה
void apply_waiting_thresholds(const std::string& rootJson, TrafficThresholdConfig& cfg) {
    const auto obj = extract_json_object(rootJson, "waiting_time_sec");// מחפש את האובייקט של waiting_time_sec במחרוזת JSON
    if (!obj.has_value()) return;// אם לא נמצא האובייקט, מחזיר

    if (const auto shortMax = extract_double_field(*obj, "short_max"); shortMax.has_value()) {// אם נמצא את הערך של short_max, מחזיר אותו לסף הנמוך
        cfg.waitingTimeSec.lowMax = *shortMax;
    } else if (const auto low = extract_double_field(*obj, "low_max"); low.has_value()) {// אם לא נמצא את הערך של short_max, מחפש את הערך של low_max ומחזיר אותו לסף הנמוך
        cfg.waitingTimeSec.lowMax = *low;
    }

    if (const auto medium = extract_double_field(*obj, "medium_max"); medium.has_value()) {// אם נמצא את הערך של medium_max, מחזיר אותו לסף הבינוני
        cfg.waitingTimeSec.mediumMax = *medium;// אם לא נמצא את הערך של medium_max, מחפש את הערך של medium_max ומחזיר אותו לסף הבינוני
    }
}
//פונקציה שמחילה את הספים של density_pct על התצורה
void apply_density_thresholds(const std::string& rootJson, TrafficThresholdConfig& cfg) {
    const auto obj = extract_json_object(rootJson, "density_pct");// מחפש את האובייקט של density_pct במחרוזת JSON
    if (!obj.has_value()) return;// אם לא נמצא האובייקט, מחזיר

    if (const auto low = extract_double_field(*obj, "low_max"); low.has_value()) {// אם נמצא את הערך של low_max, מחזיר אותו לסף הנמוך
        cfg.densityPct.lowMax = *low;
    }

    if (const auto medium = extract_double_field(*obj, "medium_max"); medium.has_value()) {// אם נמצא את הערך של medium_max, מחזיר אותו לסף הבינוני
        cfg.densityPct.mediumMax = *medium;// אם לא נמצא את הערך של medium_max, מחפש את הערך של medium_max ומחזיר אותו לסף הבינוני
    }
}

} // namespace
//פונקציה שמחזירה את הנתיב של קובץ התצורה של הסף לפי סדר עדיפות
std::string resolveThresholdConfigPath(const std::string& preferredPath) {
    if (!preferredPath.empty()) {
        return preferredPath;
    }

    if (const char* envPath = std::getenv("TRAFFIC_THRESHOLDS_FILE"); envPath != nullptr) {// אם משתנה הסביבה TRAFFIC_THRESHOLDS_FILE מוגדר, מחזיר את הנתיב שלו
        const std::string candidate = envPath;
        if (!candidate.empty()) {
            return candidate;
        }
    }
// רשימת נתיבים אפשריים לקובץ התצורה של הסף
    const std::vector<std::string> candidates = {
        "traffic_thresholds.json",
        "cpp/traffic_thresholds.json",
        "../traffic_thresholds.json",
        "../cpp/traffic_thresholds.json",
        "../../traffic_thresholds.json",
        "../../cpp/traffic_thresholds.json",
    };
// בדיקה אם הקובץ קיים באחד הנתיבים האפשריים
    for (const auto& candidate : candidates) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }

    // ברירת מחדל למקרה שהקובץ לא קיים, כך שהקריאות יכולות לרשום נתיב משמעותי.
    return "traffic_thresholds.json";
}
// פונקציה שמטענת את קובץ התצורה של הסף ומחזירה את האובייקט TrafficThresholdConfig
TrafficThresholdConfig loadTrafficThresholdConfig(const std::string& preferredPath) {// אם הנתיב ריק, מחפש במיקומים ברירת מחדל
    TrafficThresholdConfig cfg = defaults_config();// מחזיר את ערכי ברירת המחדל אם הקובץ לא נמצא או לא ניתן לטעון אותו

    const std::string path = resolveThresholdConfigPath(preferredPath);// מחזיר את הנתיב של קובץ התצורה של הסף לפי סדר עדיפות
    const auto json = read_all_text(path);
    if (!json.has_value()) {// אם לא מצליח לקרוא את הקובץ, מחזיר קונפיגורציה ריקה עם מקור ברירת מחדל
        cfg.source = "defaults (missing: " + path + ")";
        return cfg;
    }
// מנסה לשלוף את האובייקט של הצמתים מתוך הקובץ JSON
    apply_vehicle_thresholds(*json, cfg);
    apply_waiting_thresholds(*json, cfg);
    apply_density_thresholds(*json, cfg);
// אם הספים אינם תקינים, מחזיר אותם לערכים תקינים
    clamp_and_fix(cfg.vehicleCount);
    clamp_and_fix(cfg.waitingTimeSec);
    clamp_and_fix(cfg.densityPct);

    cfg.source = path;
    return cfg;
}
// פונקציה שמטענת את קובץ התצורה של הסף עם תמיכה בהחלפה לכל צומת
TrafficThresholdConfig loadTrafficThresholdConfigForIntersection(int intersectionId, const std::string& preferredPath) {
    TrafficThresholdConfig cfg = loadTrafficThresholdConfig(preferredPath);// מחזיר את ערכי ברירת המחדל אם הקובץ לא נמצא או לא ניתן לטעון אותו

    if (const char* envPath = std::getenv("TRAFFIC_THRESHOLDS_FILE"); envPath != nullptr) {// אם משתנה הסביבה TRAFFIC_THRESHOLDS_FILE מוגדר, מחזיר את הנתיב שלו
        const std::string globalOverride = envPath;
        if (!globalOverride.empty()) {// אם משתנה הסביבה TRAFFIC_THRESHOLDS_FILE מוגדר, מחזיר את הנתיב שלו
            return loadTrafficThresholdConfig(globalOverride);
        }
    }

    if (!preferredPath.empty()) {// אם הנתיב שנשלח כארגומנט אינו ריק, מחזיר את הנתיב שלו
        return loadTrafficThresholdConfig(preferredPath);
    }

    if (intersectionId >= 0) {// אם מזהה הצומת תקין, מחפש את קובץ התצורה של הסף לפי סדר עדיפות
        const std::string candidateA = "traffic_thresholds_" + std::to_string(intersectionId) + ".json";// מחפש את הקובץ traffic_thresholds_<intersectionId>.json
        if (file_exists(candidateA)) {// אם הקובץ קיים, מחזיר את הנתיב שלו
            return loadTrafficThresholdConfig(candidateA);// אם הקובץ לא קיים, מחפש את הקובץ cpp/traffic_thresholds_<intersectionId>.json
        }

        const std::string candidateB = "cpp/traffic_thresholds_" + std::to_string(intersectionId) + ".json";// מחפש את הקובץ cpp/traffic_thresholds_<intersectionId>.json
        if (file_exists(candidateB)) {// אם הקובץ קיים, מחזיר את הנתיב שלו
            return loadTrafficThresholdConfig(candidateB);// אם הקובץ לא קיים, מחזיר את התצורה הכללית
        }
    }

    return cfg;// מחזיר את התצורה הכללית אם לא נמצא קובץ תצורה ספציפי לצומת
}

} // namespace traffic
