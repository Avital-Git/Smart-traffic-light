#include "RuntimeConfig.h"

// ── ספריות C++ סטנדרטיות ────────────────────────────────────────────────
#include <algorithm>    // std::clamp — הגבלת ערכים לטווח תקף
#include <cstddef>      // size_t
#include <filesystem>   // std::filesystem::exists — בדיקת קיום קובץ
#include <fstream>      // std::ifstream — קריאת קובץ
#include <optional>     // std::optional — לערך החזרה שיכול להיות ריק
#include <sstream>      // std::ostringstream — קריאת תוכן לזיכרון
#include <string>       // std::string
#include <vector>       // std::vector — רשימת נתיבים אפשריים לקובץ

#include <nlohmann/json.hpp>  // ספריית JSON — פרסור קובץ הקונפיגורציה

namespace traffic {
namespace {  // anonymous namespace — כל מה שכאן פנימי לקובץ זה בלבד

using json = nlohmann::json;  // קיצור נוחות

// קורא קובץ טקסט לזיכרון ומחזיר את תוכנו כ-std::string
// מחזיר nullopt אם הקובץ לא קיים או לא ניתן לפתוח
std::optional<std::string> read_text_file(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;  // קובץ לא קיים או לא ניתן לפתיחה

    std::ostringstream buffer;
    buffer << in.rdbuf();  // קורא את כל התוכן לזיכרון בצעד אחד
    return buffer.str();
}

// מחפש את קובץ config.production.json בנתיבים נפוצות
// אם ניתן preferredPath — משתמש בו ישירות
// אותרת: נותן להפעיל מתיקייה מתיקיות שונות (תיקיית build, server, cpp)
std::string resolve_runtime_config_path(const std::string& preferredPath) {
    if (!preferredPath.empty()) {
        return preferredPath;  // נתיב מפורש נתנו — משתמש בו
    }

    // רשימת נתיבים לחיפוש אוטומטי לפי סדר ירידה
    const std::vector<std::string> candidates = {
        "config.production.json",             // תיקיית עבודה נוכחית
        "server/config.production.json",
        "cpp/config.production.json",
        "../config.production.json",          // תיקייה אחת מעלה
        "../server/config.production.json",
        "../cpp/config.production.json",
        "../../config.production.json",       // שתי תיקיות מעלה
        "../../server/config.production.json",
        "../../cpp/config.production.json",
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;  // נמצא קובץ תואם
        }
    }

    return "config.production.json";  // ברירת מחדל אם אין אף אחד קיים
}

// טוען פרמטרי RL מה-JSON אל מבנה cfg
// בודק קיום וסוג לפני כל פרמטר; std::clamp מגביל לטווח [0.0, 1.0]
void apply_rl_config(const json& root, RuntimeConfig& cfg) {
    if (!root.contains("rl") || !root["rl"].is_object()) return;  // אין מפתח rl — דילוג
    const auto& rl = root["rl"];

    if (rl.contains("alpha") && rl["alpha"].is_number()) {
        cfg.rl.alpha = std::clamp(rl["alpha"].get<double>(), 0.0, 1.0);  // קצב למידה (learning rate)
    }
    if (rl.contains("gamma") && rl["gamma"].is_number()) {
        cfg.rl.gamma = std::clamp(rl["gamma"].get<double>(), 0.0, 1.0);  // קצב הנחה (discount factor)
    }
    if (rl.contains("epsilon") && rl["epsilon"].is_number()) {
        cfg.rl.epsilon = std::clamp(rl["epsilon"].get<double>(), 0.0, 1.0);  // סיכוי ניסוי (חקירה/פיעול)
    } else if (rl.contains("epsilon_initial") && rl["epsilon_initial"].is_number()) {
        cfg.rl.epsilon = std::clamp(rl["epsilon_initial"].get<double>(), 0.0, 1.0);  // שם חלופי ל-epsilon
    }
    if (rl.contains("epsilon_min") && rl["epsilon_min"].is_number()) {
        cfg.rl.epsilonMin = std::clamp(rl["epsilon_min"].get<double>(), 0.0, 1.0);  // סף הנסיון המינימלי
    } else if (rl.contains("epsilon_final") && rl["epsilon_final"].is_number()) {
        cfg.rl.epsilonMin = std::clamp(rl["epsilon_final"].get<double>(), 0.0, 1.0);  // שם חלופי
    }
    if (rl.contains("epsilon_decay") && rl["epsilon_decay"].is_number()) {
        cfg.rl.epsilonDecay = std::clamp(rl["epsilon_decay"].get<double>(), 0.0, 1.0);  // קצב דעיכת הנסיון
    }
}

// טוען פרמטרי טיימינג של הצומת מה-JSON
// std::max מבטיח שלא ייכנסו ערכים שליליים
void apply_junction_config(const json& root, RuntimeConfig& cfg) {
    if (!root.contains("junction") || !root["junction"].is_object()) return;  // אין מפתח junction — דילוג
    const auto& junction = root["junction"];

    if (junction.contains("min_green_sec") && junction["min_green_sec"].is_number()) {
        cfg.junction.minGreenSec = std::max(0.0, junction["min_green_sec"].get<double>());  // זמן ירוק מינימלי בשניות
    }
    if (junction.contains("max_green_sec") && junction["max_green_sec"].is_number()) {
        cfg.junction.maxGreenSec = std::max(0.0, junction["max_green_sec"].get<double>());  // זמן ירוק מקסימלי
    }
    if (junction.contains("yellow_sec") && junction["yellow_sec"].is_number()) {
        cfg.junction.yellowSec = std::max(0.0, junction["yellow_sec"].get<double>());  // זמן צהוב
    }
    if (junction.contains("all_red_sec") && junction["all_red_sec"].is_number()) {
        cfg.junction.allRedSec = std::max(0.0, junction["all_red_sec"].get<double>());  // זמן אדום כללי
    }
}

} // namespace — סוף ה-anonymous namespace

// פונקציה ציבורית — טוענת קובץ JSON ומחזירת RuntimeConfig
// סדר פעולות:
//   1. מחפש את נתיב הקובץ
//   2. קורא את התוכן
//   3. מפעיל את apply_rl_config + apply_junction_config
//   4. אם הקובץ חסר/פגום — מחזיר ערךי ברירת מחדל
RuntimeConfig loadRuntimeConfig(const std::string& preferredPath) {
    RuntimeConfig cfg;  // מאותחל עם ערכי ברירת מחדל
    const std::string path = resolve_runtime_config_path(preferredPath);  // מחפש את הקובץ
    const auto text = read_text_file(path);  // קורא את תוכן הקובץ
    if (!text.has_value()) {
        cfg.source = "defaults (missing: " + path + ")";  // הקובץ לא נמצא
        return cfg;  // מחזיר ערךי ברירת מחדל
    }

    try {
        const json root = json::parse(*text);  // פרסור JSON
        apply_rl_config(root, cfg);            // טעינת פרמטרי RL
        apply_junction_config(root, cfg);      // טעינת פרמטרי צומת
        cfg.source = path;                     // תיעוד שהקונפיגורציה נטענה מקובץ
    } catch (...) {
        cfg.source = "defaults (invalid: " + path + ")";  // JSON פגום — מחזיר ברירת מחדל
    }

    return cfg;
}

} // namespace traffic