#include "TrafficServer.h"
#include "Database.h"
#include "JwtAuth.h"
#include "WebSocketHub.h"
#include "../TrafficConstants.h"

// httplib is header-only — include the full header only in the .cpp
// (CPPHTTPLIB_NO_EXCEPTIONS / CPPHTTPLIB_OPENSSL_SUPPORT are set via CMake target_compile_definitions)
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

using json = nlohmann::json;

#pragma comment(lib, "bcrypt.lib")

namespace {

const std::unordered_map<int, std::vector<int>> DEFAULT_NEIGHBOR_TOPOLOGY = {
    {1, {2}},
    {2, {1, 3}},
    {3, {2, 4}},
    {4, {3}},
};
// מבנה שמכיל את המידע הדרוש לאימות חירום
struct EmergencyAuthConfig {
    std::unordered_map<std::string, std::string> vehicle_keys;
    double max_clock_skew_sec = traffic::constants::kDefaultEmergencyClockSkewSec;
    std::string source = "defaults";
};
// פונקציה שטוענת את קונפיגורציית האימות של חירום מקובץ JSON
std::string env_or(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return (v && v[0]) ? std::string(v) : std::string(fallback);
}
// פונקציה שטוענת את הטופולוגיה של השכנים מקובץ JSON
std::optional<std::unordered_map<int, std::vector<int>>> load_neighbor_topology_from_json_file(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }
// מנסה לקרוא את תוכן הקובץ
    try {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in.is_open()) {
            return std::nullopt;
        }

        json payload;
        in >> payload;
        if (!payload.is_object() || !payload.contains("intersections") || !payload["intersections"].is_object()) {
            return std::nullopt;
        }
// יוצרת מיפוי של מזהי צומת לרשימת השכנים שלהם
        std::unordered_map<int, std::vector<int>> topology;
        for (auto it = payload["intersections"].begin(); it != payload["intersections"].end(); ++it) {
            int intersection_id = 0;
            try {
                intersection_id = std::stoi(it.key());
            } catch (...) {
                continue;
            }
// בודקת אם המידע על השכנים בצומת הוא תקין
            if (!it.value().is_object() || !it.value().contains("neighbors") || !it.value()["neighbors"].is_array()) {
                topology[intersection_id] = {};
                continue;
            }
// יוצרת רשימה של מזהי השכנים בצומת
            std::vector<int> neighbors;
            for (const auto& n : it.value()["neighbors"]) {
                try {
                    neighbors.push_back(n.get<int>());
                } catch (...) {
                    continue;
                }
            }
            topology[intersection_id] = neighbors;
        }

        if (!topology.empty()) {
            return topology;
        }
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}
// פונקציה שטוענת את קונפיגורציית האימות של חירום מקובץ JSON
std::vector<unsigned char> hmac_sha256_bytes(const std::string& key, const std::string& input)
{
    BCRYPT_ALG_HANDLE alg = nullptr;// מצביע למזהה האלגוריתם של HMAC-SHA256
    BCRYPT_HASH_HANDLE hash = nullptr;// מצביע למזהה ההאש של HMAC-SHA256
    DWORD object_len = 0;// משתנה לאחסון אורך האובייקט של HMAC-SHA256
    DWORD data_len = 0;// משתנה לאחסון אורך הנתונים של HMAC-SHA256
    DWORD hash_len = 0;// משתנה לאחסון אורך ההאש של HMAC-SHA256
    std::vector<unsigned char> hash_object;
    std::vector<unsigned char> hash_value;
// בודק אם ניתן לפתוח את האלגוריתם של HMAC-SHA256
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &data_len, 0) != 0) {// בודק אם ניתן לקבל את אורך האובייקט של HMAC-SHA256
        BCryptCloseAlgorithmProvider(alg, 0);// סוגר את האלגוריתם של HMAC-SHA256
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &data_len, 0) != 0) {// בודק אם ניתן לקבל את אורך ההאש של HMAC-SHA256
        BCryptCloseAlgorithmProvider(alg, 0);// סוגר את האלגוריתם של HMAC-SHA256
        return {};
    }

    hash_object.resize(object_len);// משנה את גודל האובייקט של HMAC-SHA256 לגודל המתאים
    hash_value.resize(hash_len);// משנה את גודל ההאש של HMAC-SHA256 לגודל המתאים
// בודק אם ניתן ליצור את ההאש של HMAC-SHA256
    if (BCryptCreateHash(
            alg,// מצביע לאלגוריתם של HMAC-SHA256
            &hash,// מצביע למזהה ההאש של HMAC-SHA256
            hash_object.data(),// מצביע לאובייקט ההאש של HMAC-SHA256
            object_len,// אורך האובייקט של HMAC-SHA256
            reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),// מצביע למפתח של HMAC-SHA256
            static_cast<ULONG>(key.size()),// אורך המפתח של HMAC-SHA256
            0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);// סוגר את האלגוריתם של HMAC-SHA256
        return {};// מחזיר וקטור ריק אם לא ניתן ליצור את ההאש של HMAC-SHA256
    }
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0) != 0) {// בודק אם ניתן להוסיף את הנתונים של HMAC-SHA256
        BCryptDestroyHash(hash);// משמיד את ההאש של HMAC-SHA256
        BCryptCloseAlgorithmProvider(alg, 0);// סוגר את האלגוריתם של HMAC-SHA256
        return {};// מחזיר וקטור ריק אם לא ניתן להוסיף את הנתונים של HMAC-SHA256
    }
    if (BCryptFinishHash(hash, hash_value.data(), hash_len, 0) != 0) {// בודק אם ניתן לסיים את ההאש של HMAC-SHA256
        BCryptDestroyHash(hash);// משמיד את ההאש של HMAC-SHA256
        BCryptCloseAlgorithmProvider(alg, 0);// סוגר את האלגוריתם של HMAC-SHA256
        return {};// מחזיר וקטור ריק אם לא ניתן לסיים את ההאש של HMAC-SHA256
    }

    BCryptDestroyHash(hash);// משמיד את ההאש של HMAC-SHA256
    BCryptCloseAlgorithmProvider(alg, 0);// סוגר את האלגוריתם של HMAC-SHA256
    return hash_value;// מחזיר את ההאש של HMAC-SHA256

}
// פונקציה שממירה את ה-HMAC-SHA256 למחרוזת הקסדצימלית באותיות קטנות
std::string to_hex(const std::vector<unsigned char>& bytes)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char value : bytes) {
        out << std::setw(2) << static_cast<int>(value);
    }
    return out.str();
}
// פונקציה שממירה את ה-HMAC-SHA256 למחרוזת הקסדצימלית באותיות קטנות
std::string to_lower_copy(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}
// פונקציה שממירה את ה-HMAC-SHA256 למחרוזת הקסדצימלית באותיות קטנות
std::optional<EmergencyAuthConfig> load_emergency_auth_from_json_file(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }

    try {// מנסה לקרוא את הקובץ JSON
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in.is_open()) {
            return std::nullopt;
        }

        json payload;
        in >> payload;
        if (!payload.is_object() || !payload.contains("vehicle_keys") || !payload["vehicle_keys"].is_object()) {
            return std::nullopt;
        }
// יוצרת אובייקט EmergencyAuthConfig וממלאת אותו במידע מהקובץ JSON
        EmergencyAuthConfig cfg;
        cfg.source = path.string();
// בודקת אם יש מפתח "max_clock_skew_sec" בקובץ JSON ומעדכנת את הערך במידת הצורך
        for (auto it = payload["vehicle_keys"].begin(); it != payload["vehicle_keys"].end(); ++it) {
            if (!it.value().is_string()) {
                continue;
            }
            const std::string key_name = it.key();// שם המפתח של הרכב
            const std::string key_value = it.value().get<std::string>();// הערך של המפתח של הרכב
            if (!key_name.empty() && !key_value.empty()) {// בודקת אם שם המפתח והערך אינם ריקים
                cfg.vehicle_keys[key_name] = key_value;// מוסיפה את המפתח של הרכב למיפוי של vehicle_keys
            }
        }
// בודקת אם יש מפתח "max_clock_skew_sec" בקובץ JSON ומעדכנת את הערך במידת הצורך
        if (payload.contains("max_clock_skew_sec") && payload["max_clock_skew_sec"].is_number()) {
            cfg.max_clock_skew_sec = std::max(1.0, payload["max_clock_skew_sec"].get<double>());// מעדכנת את הערך של max_clock_skew_sec אם הוא גדול מ-1.0
        }
// מחזירה את האובייקט EmergencyAuthConfig אם יש מפתחות של רכבים, אחרת מחזירה std::nullopt
        if (!cfg.vehicle_keys.empty()) {
            return cfg;
        }
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

} // namespace


// Constructor / Destructor

//פונקציה שמייצרת אובייקט TrafficServer ומאתחלת את הפורטים של השרת וה-WebSocket
TrafficServer::TrafficServer(int port, int ws_port)
    : port_(port), ws_port_(ws_port),// יוצרת אובייקט httplib::Server ומאתחלת את השרת
      svr_(new httplib::Server()),// יוצרת אובייקט WebSocketHub ומאתחלת את ה-WebSocket
      hub_(new WebSocketHub(ws_port))// יוצרת אובייקט WebSocketHub ומאתחלת את ה-WebSocket
{
    load_neighbor_topology();//טוענת את הטופולוגיה של השכנים
    load_intersection_locations(); // טוענת קואורדינטות GPS של הצמתות
    load_emergency_auth_config();// טוענת את קונפיגורציית האימות של חירום
    register_routes();// מרשמת את הנתיבים של השרת
}
//פונקציה שמוחקת את האובייקט TrafficServer ומשחררת את המשאבים שהוקצו
TrafficServer::~TrafficServer()
{
    delete svr_;// משחררת את האובייקט httplib::Server
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
// פונקציה שמריצה את השרת ומאזינה לבקשות HTTP ו-WebSocket
void TrafficServer::run()
{
    hub_->start();// מפעילה את ה-WebSocketHub ומאזינה לחיבורים חדשים
    if (router_mode_) {// אם מצב הריצה הוא router_mode, השרת מאזין רק ל-HTTP על הפורט הפנימי ומקבל חיבורים ל-WebSocket דרך ה-Router החיצוני
        std::cout << "[TrafficServer] HTTP  listening on http://" << http_bind_host_// מדפיסה הודעה שמציינת שהשרת מאזין לבקשות HTTP על הפורט הפנימי
                  << ":" << internal_http_port_ << " (router-internal)\n";// מדפיסה הודעה שמציינת שהשרת מאזין לבקשות HTTP על הפורט הפנימי
        std::cout << "[TrafficServer] WS    served via external Router (adopt mode)\n";//   מדפיסה הודעה שמציינת שה-WebSocketHub מקבל חיבורים דרך ה-Router החיצוני
        svr_->listen(http_bind_host_.c_str(), internal_http_port_);// מאזינה לבקשות HTTP על הפורט הפנימי
    } else {// אם מצב הריצה הוא רגיל, השרת מאזין ל-HTTP על הפורט הציבורי ומקבל חיבורים ל-WebSocket ישירות
        std::cout << "[TrafficServer] HTTP  listening on http://0.0.0.0:" << port_ << "\n";// מדפיסה הודעה שמציינת שהשרת מאזין לבקשות HTTP על הפורט הציבורי
        std::cout << "[TrafficServer] WS    listening on ws://0.0.0.0:"   << ws_port_ << "\n";// מדפיסה הודעה שמציינת שה-WebSocketHub מאזין לחיבורים על הפורט הציבורי
        svr_->listen("0.0.0.0", port_);// מאזינה לבקשות HTTP על הפורט הציבורי
    }
    hub_->stop();
}
// פונקציה שמפעילה את מצב router_mode ומעדכנת את הפורט הפנימי של השרת
void TrafficServer::enable_router_mode(int internal_http_port)
{
    router_mode_       = true;// מעדכנת את מצב הריצה ל-router_mode
    internal_http_port_ = internal_http_port;// מעדכנת את הפורט הפנימי של השרת
    http_bind_host_    = "127.0.0.1";// מעדכנת את כתובת ה-IP של השרת ל-
    hub_->disable_own_listener();// מפעילה את מצב האימוץ של ה-WebSocketHub ומונעת ממנו להאזין לחיבורים ישירות
}

void TrafficServer::stop()// מפסיקה את ההאזנה לבקשות HTTP ומפסיקה את השרת
{
    svr_->stop();// מפסיקה את ההאזנה לבקשות HTTP
    // hub_->stop() is called by run() once svr_->listen() returns.
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------
// פונקציה שמרשמת את הנתיבים של השרת ומגדירה את ההתנהגות שלהם
void TrafficServer::register_routes()
{
    auto apply_cors = [](httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    };

    // Ensure CORS headers are present on normal routed responses.
    svr_->set_post_routing_handler([apply_cors](const httplib::Request& /*req*/, httplib::Response& res) {// מוסיפה את הכותרות של CORS לכל התגובות של השרת
        apply_cors(res);
    });

    svr_->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {// מוסיפה את הכותרות של CORS לכל הבקשות של השרת
        // Handle CORS preflight globally.// מטפלת בבקשות CORS preflight באופן גלובלי
        if (req.method == "OPTIONS") {// אם הבקשה היא OPTIONS, מחזירה תגובה עם סטטוס 204 ומוסיפה את הכותרות של CORS
            res.status = 204;// מחזירה סטטוס 204 No Content
            return httplib::Server::HandlerResponse::Handled;// מחזירה שהבקשה טופלה
        }

        // Auth-gated routes: every /admin/* (except /admin/login), plus the two
        // emergency simulate/clear endpoints (mirrors Python admin-only access).
        const bool is_admin_path =// בודקת אם הנתיב הוא /admin/* (למעט /admin/login)
            req.path.rfind("/admin/", 0) == 0 && req.path != "/admin/login";
        const bool is_emergency_admin_path =
            (req.path.find("/simulate-emergency") != std::string::npos) ||
            (req.path.find("/clear-emergency")    != std::string::npos);
// אם הנתיב הוא לא /admin/* ולא אחד משני הנתיבים של חירום, מחזירה שהבקשה לא טופלה
        if (!is_admin_path && !is_emergency_admin_path) {
            return httplib::Server::HandlerResponse::Unhandled;// מחזירה שהבקשה לא טופלה
        }
// אם הנתיב הוא /admin/* או אחד משני הנתיבים של חירום, בודקת את הכותרת Authorization ומחלצת את הטוקן
        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));
        if (!token) {// אם אין טוקן, מחזירה סטטוס 401 ומסר שגיאה
            res.status = 401;
            res.set_content(json({{"detail", "Missing authentication credentials"}}).dump(), "application/json");
            return httplib::Server::HandlerResponse::Handled;// מחזירה שהבקשה טופלה
        }

        std::string username;// משתנה לאחסון שם המשתמש שמזוהה מהטוקן
        if (!jwt_auth::validate_admin_token(*token, username)) {// אם הטוקן אינו תקין, מחזירה סטטוס 401 ומסר שגיאה
            res.status = 401;
            res.set_content(json({{"detail", "Invalid token"}}).dump(), "application/json");// מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            return httplib::Server::HandlerResponse::Handled;// מחזירה שהבקשה טופלה
        }
// אם הטוקן תקין, מחזירה שהבקשה לא טופלה וממשיכה לטפל בה במסלול הרגיל
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ── GET /health ─────────────────────────────────────────────────────────
    svr_->Get("/health", [](const httplib::Request& /*req*/, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /health ומחזירה סטטוס 200 OK עם תוכן JSON {"status":"ok"}
        res.set_content("{\"status\":\"ok\"}", "application/json");// מחזירה סטטוס 200 OK עם תוכן JSON {"status":"ok"}
    });

    // ── GET /intersections ───────────────────────────────────────────────────
    // מטפלת בבקשה GET לנתיב /intersections ומחזירה את רשימת הצמתים מהמסד נתונים בפורמט JSON
    svr_->Get("/intersections", [](const httplib::Request& /*req*/, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /intersections ומחזירה את רשימת הצמתים מהמסד נתונים בפורמט JSON
        auto rows = db_fetch_intersections();// שולפת את רשימת הצמתים מהמסד נתונים

        json arr = json::array();// יוצרת מערך JSON ריק
        for (const auto& r : rows) {// עוברת על כל הצמתים ומוסיפה אותם למערך JSON
            arr.push_back({// מוסיפה אובייקט JSON עם המידע של הצומת למערך JSON
                {"id",   r.id},
                {"code", r.code},
                {"name", r.name},
                {"city", r.city},
            });
        }
        res.set_content(arr.dump(), "application/json");// מחזירה את המערך JSON עם רשימת הצמתים
    });

    // ── POST /admin/login ───────────────────────────────────────────────────
    svr_->Post("/admin/login", [](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה POST לנתיב /admin/login ומחזירה טוקן JWT אם שם המשתמש והסיסמה תקינים
        json body;// יוצרת אובייקט JSON ריק
        try {
            body = json::parse(req.body);// מנסה לפרסר את גוף הבקשה ל-JSON
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה
            return;
        }
// מחלצת את שם המשתמש והסיסמה מהגוף של הבקשה
        const std::string username = body.value("username", "");
        const std::string password = body.value("password", "");
        if (!jwt_auth::verify_admin_password(username, password)) {// אם שם המשתמש או הסיסמה אינם תקינים, מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            res.status = 401;
            res.set_content(json({{"detail", "Invalid credentials"}}).dump(), "application/json");// מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            return;
        }

        std::string user_role = "regular_admin";// ברירת המחדל היא "regular_admin"
        std::string role_error;// משתנה לאחסון שגיאה אם יש בעיה בקבלת התפקיד של המשתמש
        db_get_admin_role(username, user_role, role_error);// שולף את התפקיד של המשתמש מהמסד נתונים

        const auto token = jwt_auth::create_admin_token(username, user_role);// יוצר טוקן JWT עם שם המשתמש והתפקיד של המשתמש
        res.set_content(json({// מחזירה את הטוקן שנוצר בפורמט JSON
            {"access_token", token.access_token},
            {"token_type", token.token_type},
            {"expires_in", token.expires_in},
            {"role", user_role},
        }).dump(), "application/json");// מחזירה את הטוקן שנוצר בפורמט JSON
    });

    auto validate_username = [](const std::string& username) -> std::optional<std::string> {// פונקציה שמוודאת את תקינות שם המשתמש ומחזירה שגיאה אם הוא לא תקין
        if (username.size() < traffic::constants::kMinUsernameLength) {// אם שם המשתמש קצר מ-3 תווים, מחזירה שגיאה
            return std::string("username must be at least 3 characters");// מחזירה שגיאה אם שם המשתמש קצר מ-3 תווים
        }
        for (char ch : username) {// עוברת על כל התווים בשם המשתמש ובודקת אם יש רווחים
            if (std::isspace(static_cast<unsigned char>(ch))) {
                return std::string("username must not contain spaces");// מחזירה שגיאה אם שם המשתמש מכיל רווחים
            }
        }
        return std::nullopt;// מחזירה std::nullopt אם שם המשתמש תקין
    };

    auto validate_password = [](const std::string& password) -> std::optional<std::string> {// פונקציה שמוודאת את תקינות הסיסמה ומחזירה שגיאה אם היא לא תקינה
        if (password.size() < traffic::constants::kMinPasswordLength) {// אם הסיסמה קצרה מ-8 תווים, מחזירה שגיאה
            return std::string("password must be at least 8 characters");// מחזירה שגיאה אם הסיסמה קצרה מ-8 תווים
        }
        return std::nullopt;// מחזירה std::nullopt אם הסיסמה תקינה
    };
// פונקציה שמחלצת את שם המשתמש של המנהל הנוכחי מהבקשה ומחזירה std::nullopt אם אין טוקן או אם הטוקן אינו תקין
    auto extract_current_admin_username = [](const httplib::Request& req) -> std::optional<std::string> {
        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));// מחלצת את הטוקן מהכותרת Authorization של הבקשה
        if (!token) {// אם אין טוקן, מחזירה std::nullopt
            return std::nullopt;
        }
        std::string username;
        if (!jwt_auth::validate_admin_token(*token, username)) {// אם הטוקן אינו תקין, מחזירה std::nullopt
            return std::nullopt;
        }
        if (username.empty()) {// אם שם המשתמש ריק, מחזירה std::nullopt
            return std::nullopt;
        }
        return username;
    };

    // ── GET /admin/users ────────────────────────────────────────────────────
    svr_->Get("/admin/users", [](const httplib::Request& /*req*/, httplib::Response& res) {
        const auto users = db_list_admin_users();// שולף את רשימת המשתמשים מהמסד נתונים
        json items = json::array();// יוצרת מערך JSON ריק
        for (const auto& user : users) {// עוברת על כל המשתמשים ומוסיפה אותם למערך JSON
            items.push_back({// מוסיפה אובייקט JSON עם המידע של המשתמש למערך JSON
                {"user_id", user.user_id},// מוסיפה את מזהה המשתמש ל-JSON
                {"username", user.username},// מוסיפה את שם המשתמש ל-JSON
                {"role", user.role.empty() ? "regular_admin" : user.role},// מוסיפה את התפקיד ל-JSON
                {"created_at", user.created_at.empty() ? json(nullptr) : json(user.created_at)},// מוסיפה את תאריך היצירה ל-JSON
                {"last_login", user.last_login.empty() ? json(nullptr) : json(user.last_login)},// מוסיפה את תאריך הכניסה האחרון ל-JSON
            });
        }
// מחזירה את המערך JSON עם רשימת המשתמשים
        res.set_content(json({
            {"status", "success"},
            {"count", static_cast<int>(items.size())},
            {"users", items},
        }).dump(), "application/json");// מחזירה את המערך JSON עם רשימת המשתמשים
    });

    // ── POST /admin/users 
    //הוספת מנהל חדש 
    svr_->Post("/admin/users", [validate_username, validate_password](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה POST לנתיב /admin/users ומוסיפה משתמש חדש למסד הנתונים
        // Check if current user is super_admin
        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));// מחלצת את הטוקן מהכותרת Authorization של הבקשה
        if (!token) {// אם אין טוקן, מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            res.status = 401;
            res.set_content(json({{"detail", "Missing authorization token"}}).dump(), "application/json");// מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            return;
        }
        std::string current_username;// משתנה לאחסון שם המשתמש שמזוהה מהטוקן
        std::string current_role;// משתנה לאחסון התפקיד של המשתמש שמזוהה מהטוקן
        if (!jwt_auth::validate_admin_token_with_role(*token, current_username, current_role)) {// אם הטוקן אינו תקין, מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            res.status = 401;
            res.set_content(json({{"detail", "Invalid authorization token"}}).dump(), "application/json");// מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            return;
        }
        if (current_role != "super_admin") {
            res.status = 403;
            res.set_content(json({{"detail", "Only super_admin can create users"}}).dump(), "application/json");// מחזירה סטטוס 403 Forbidden ומסר שגיאה

            return;
        }
// מחלצת את שם המשתמש והסיסמה מהגוף של הבקשה ומוודאת שהם תקינים
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }
// מחלצת את שם המשתמש והסיסמה מהגוף של הבקשה
        const std::string username = body.value("username", "");
        const std::string password = body.value("password", "");
// בודקת אם שם המשתמש והסיסמה תקינים ומחזירה שגיאה אם הם לא תקינים
        if (auto e = validate_username(username)) {
            res.status = 422;
            res.set_content(json({{"detail", *e}}).dump(), "application/json");// מחזירה סטטוס 422 Unprocessable Entity ומסר שגיאה אם שם המשתמש אינו תקין
            return;
        }
        if (auto e = validate_password(password)) {// מחזירה סטטוס 422 Unprocessable Entity ומסר שגיאה אם הסיסמה אינה תקינה
            res.status = 422;
            res.set_content(json({{"detail", *e}}).dump(), "application/json");// מחזירה סטטוס 422 Unprocessable Entity ומסר שגיאה אם הסיסמה אינה תקינה
            return;
        }// בודקת אם יש מפתח "confirm_password" בגוף הבקשה ואם הוא תואם לסיסמה
        if (body.contains("confirm_password") && body["confirm_password"].is_string()) {
            if (body["confirm_password"].get<std::string>() != password) {
                res.status = 422;
                res.set_content(json({{"detail", "password and confirm_password do not match"}}).dump(), "application/json");
                return;
            }
        }
// יוצר סולט חדש ומחשב את ההאש של הסיסמה עם הסולט
        const std::string salt = jwt_auth::generate_salt_hex(16);
        const std::string hash = jwt_auth::hash_password_with_salt(salt, password);
// מוסיף את המשתמש החדש למסד הנתונים ומחזיר את מזהה המשתמש החדש או שגיאה אם שם המשתמש כבר קיים
        int new_user_id = 0;
        bool duplicate_username = false;
        std::string error;
        const bool ok = db_insert_admin_user(username, hash, salt, new_user_id, duplicate_username, error);// מוסיף את המשתמש החדש למסד הנתונים ומחזיר את מזהה המשתמש החדש או שגיאה אם שם המשתמש כבר קיים
        if (!ok) {
            if (duplicate_username) {// אם שם המשתמש כבר קיים, מחזירה סטטוס 409 Conflict ומסר שגיאה
                res.status = 409;
                res.set_content(json({{"detail", "username already exists"}}).dump(), "application/json");// מחזירה סטטוס 409 Conflict ומסר שגיאה אם שם המשתמש כבר קיים
                return;
            }
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "failed to create admin user" : error}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה אם נכשלה יצירת המשתמש
            return;
        }
// מחזירה סטטוס 201 Created ומסר הצלחה עם מזהה המשתמש החדש ושם המשתמש
        res.status = 201;
        res.set_content(json({
            {"status", "success"},
            {"message", "Admin user created"},
            {"user", {
                {"user_id", new_user_id},
                {"username", username},
            }},
        }).dump(), "application/json");// מחזירה סטטוס 201 Created ומסר הצלחה עם מזהה המשתמש החדש ושם המשתמש
    });

    // ── DELETE /admin/users/{user_id} ──
    svr_->Delete(R"(/admin/users/(\d+))", [extract_current_admin_username](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה DELETE לנתיב /admin/users/{user_id} ומוחקת את המשתמש מהמסד הנתונים
        const int user_id = std::stoi(req.matches[1]);// מחלצת את מזהה המשתמש מהנתיב של הבקשה
        
        // Check if current user is super_admin
        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));// מחלצת את הטוקן מהכותרת Authorization של הבקשה
        if (!token) {// אם אין טוקן, מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            res.status = 401;
            res.set_content(json({{"detail", "Missing authorization token"}}).dump(), "application/json");// מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            return;
        }
        std::string current_username;// משתנה לאחסון שם המשתמש שמזוהה מהטוקן
        std::string current_role;// משתנה לאחסון התפקיד של המשתמש שמזוהה מהטוקן
        if (!jwt_auth::validate_admin_token_with_role(*token, current_username, current_role)) {// אם הטוקן אינו תקין, מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            res.status = 401;
            res.set_content(json({{"detail", "Invalid authorization token"}}).dump(), "application/json");// מחזירה סטטוס 401 Unauthorized ומסר שגיאה
            return;
        }
        if (current_role != "super_admin") {// אם המשתמש הנוכחי אינו super_admin, מחזירה סטטוס 403 Forbidden ומסר שגיאה
            res.status = 403;
            res.set_content(json({{"detail", "Only super_admin can delete users"}}).dump(), "application/json");// מחזירה סטטוס 403 Forbidden ומסר שגיאה אם המשתמש הנוכחי אינו super_admin
            return;
        }

        std::string target_username;// משתנה לאחסון שם המשתמש של המשתמש שמוחק
        bool target_found = false;// משתנה לאחסון אם המשתמש שמוחק נמצא במסד הנתונים
        std::string error;
        if (!db_get_admin_username_by_id(user_id, target_username, target_found, error)) {// שולף את שם המשתמש של המשתמש שמוחק מהמסד הנתונים ומחזיר שגיאה אם לא נמצא
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "Failed to resolve user" : error}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה אם לא ניתן לשלוף את שם המשתמש
            return;
        }
        if (!target_found) {// אם המשתמש שמוחק לא נמצא במסד הנתונים, מחזירה סטטוס 404 Not Found ומסר שגיאה
            res.status = 404;
            res.set_content(json({{"detail", "Admin user not found"}}).dump(), "application/json");
            return;
        }
// בודקת אם המשתמש שמוחק הוא המשתמש הנוכחי ומחזירה שגיאה אם כן
        if (target_username == current_username) {
            res.status = 400;
            res.set_content(json({{"detail", "Cannot delete yourself"}}).dump(), "application/json");
            return;
        }
// בודקת אם המשתמש שמוחק הוא המשתמש האחרון במסד הנתונים ומחזירה שגיאה אם כן
        int admin_count = 0;
        if (!db_count_admin_users(admin_count, error)) {// שולף את מספר המשתמשים במסד הנתונים ומחזיר שגיאה אם לא ניתן לשלוף את המספר
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "Failed to count admin users" : error}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה אם לא ניתן לשלוף את מספר המשתמשים
            return;
        }
        if (admin_count <= traffic::constants::kMinAdminCount) {// אם מספר המשתמשים במסד הנתונים קטן או שווה למספר המינימלי של משתמשים, מחזירה סטטוס 400 Bad Request ומסר שגיאה
            res.status = 400;
            res.set_content(json({{"detail", "Cannot delete the last admin user"}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה אם לא ניתן למחוק את המשתמש האחרון
            return;
        }

        bool found = false;
        if (!db_delete_admin_user(user_id, found, error)) {// מנסה למחוק את המשתמש מהמסד הנתונים ומחזיר שגיאה אם לא ניתן למחוק
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "Failed to delete admin user" : error}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה אם לא ניתן למחוק את המשתמש
            return;
        }
        if (!found) {// אם המשתמש לא נמצא במסד הנתונים, מחזירה סטטוס 404 Not Found ומסר שגיאה
            res.status = 404;
            res.set_content(json({{"detail", "Admin user not found"}}).dump(), "application/json");// מחזירה סטטוס 404 Not Found ומסר שגיאה אם המשתמש לא נמצא במסד הנתונים
            return;
        }

        res.set_content(json({// מחזירה סטטוס 200 OK ומסר הצלחה אם המשתמש נמחק בהצלחה
            {"status", "success"},
            {"message", "Admin user deleted"},
            {"deleted_user_id", user_id},
        }).dump(), "application/json"); // מחזירה סטטוס 200 OK ומסר הצלחה אם המשתמש נמחק בהצלחה
    });

    // ── PUT /admin/users/{user_id}/password ─────────────────────────────────
    svr_->Put(R"(/admin/users/(\d+)/password)", [validate_password](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה PUT לנתיב /admin/users/{user_id}/password ומעדכנת את הסיסמה של המשתמש במסד הנתונים
        const int user_id = std::stoi(req.matches[1]);// מחלצת את מזהה המשתמש מהנתיב של הבקשה

        // Role check: regular_admin can only change their own password
        {
            const auto tok = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));// מחלצת את הטוקן מהכותרת Authorization של הבקשה
            if (!tok) {// אם אין טוקן, מחזירה סטטוס 401 Unauthorized ומסר שגיאה
                res.status = 401;
                res.set_content(json({{"detail", "Missing authorization token"}}).dump(), "application/json");
                return;
            }
            std::string cur_user;// משתנה לאחסון שם המשתמש שמזוהה מהטוקן
            std::string cur_role;// משתנה לאחסון התפקיד של המשתמש שמזוהה מהטוקן
            if (!jwt_auth::validate_admin_token_with_role(*tok, cur_user, cur_role)) {// אם הטוקן אינו תקין, מחזירה סטטוס 401 Unauthorized ומסר שגיאה
                res.status = 401;
                res.set_content(json({{"detail", "Invalid authorization token"}}).dump(), "application/json");// מחזירה סטטוס 401 Unauthorized ומסר שגיאה אם הטוקן אינו תקין
                return;
            }
            if (cur_role != "super_admin") {// אם המשתמש הנוכחי אינו super_admin, בודקת אם הוא מנסה לשנות את הסיסמה של משתמש אחר ומחזירה שגיאה אם כן
                std::string target_user;// משתנה לאחסון שם המשתמש של המשתמש שמנסים לשנות את הסיסמה שלו
                bool target_found = false;// משתנה לאחסון אם המשתמש נמצא במסד הנתונים
                std::string target_error;// משתנה לאחסון הודעת שגיאה אם יש
                db_get_admin_username_by_id(user_id, target_user, target_found, target_error);// שולף את שם המשתמש של המשתמש שמנסים לשנות את הסיסמה שלו מהמסד הנתונים ומחזיר שגיאה אם לא נמצא
                if (!target_found || target_user != cur_user) {// אם המשתמש שמנסים לשנות את הסיסמה שלו אינו המשתמש הנוכחי, מחזירה סטטוס 403 Forbidden ומסר שגיאה
                    res.status = 403;
                    res.set_content(json({{"detail", "Regular admin can only change their own password"}}).dump(), "application/json");
                    return;
                }
            }
        }
// מחלצת את הסיסמה החדשה מהגוף של הבקשה ומוודאת שהיא תקינה
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }
// מחלצת את הסיסמה מהגוף של הבקשה ומוודאת שהיא תקינה
        const std::string password = body.value("password", "");
        if (auto e = validate_password(password)) {// אם הסיסמה אינה תקינה, מחזירה סטטוס 422 Unprocessable Entity ומסר שגיאה
            res.status = 422;
            res.set_content(json({{"detail", *e}}).dump(), "application/json");
            return;
        }// בודקת אם יש מפתח "confirm_password" בגוף הבקשה ואם הוא תואם לסיסמה
        if (body.contains("confirm_password") && body["confirm_password"].is_string()) {
            if (body["confirm_password"].get<std::string>() != password) {// אם הסיסמה והסיסמה המאושרת אינם תואמים, מחזירה סטטוס 422 Unprocessable Entity ומסר שגיאה
                res.status = 422;
                res.set_content(json({{"detail", "password and confirm_password do not match"}}).dump(), "application/json");// מחזירה סטטוס 422 Unprocessable Entity ומסר שגיאה אם הסיסמה והסיסמה המאושרת אינם תואמים
                return;
            }
        }
// יוצר סולט חדש ומחשב את ההאש של הסיסמה עם הסולט
        const std::string salt = jwt_auth::generate_salt_hex(16);
        const std::string hash = jwt_auth::hash_password_with_salt(salt, password);

        bool found = false;
        std::string error;
        if (!db_update_admin_user_password(user_id, hash, salt, found, error)) {// מנסה לעדכן את הסיסמה של המשתמש במסד הנתונים ומחזיר שגיאה אם נכשל
            res.status = 400;
            res.set_content(json({{"detail", error.empty() ? "Failed to update password" : error}}).dump(), "application/json");
            return;
        }
        if (!found) {// אם המשתמש לא נמצא במסד הנתונים, מחזירה סטטוס 404 Not Found ומסר שגיאה
            res.status = 404;
            res.set_content(json({{"detail", "Admin user not found"}}).dump(), "application/json");
            return;
        }

        res.set_content(json({// מחזירה סטטוס 200 OK ומסר הצלחה אם הסיסמה עודכנה בהצלחה
            {"status", "success"},
            {"message", "Password updated"},
            {"user_id", user_id},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersections ─────
    //שליפת רשימת הצמתים מהמסד נתונים והחזרתם בפורמט JSON
    svr_->Get("/admin/intersections", [this](const httplib::Request& /*req*/, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /admin/intersections ומחזירה את רשימת הצמתים מהמסד נתונים בפורמט JSON
        auto rows = db_fetch_intersections();// שולף את רשימת הצמתים מהמסד נתונים
        json items = json::array();// יוצרת מערך JSON ריק

        std::lock_guard<std::mutex> lk(store_mutex_);// ננעל את המוטקס כדי למנוע גישה מקבילה ל-state_store_ ו-action_store_
        for (const auto& row : rows) {// עוברת על כל הצמתים ומוסיפה אותם למערך JSON
            auto state_it = state_store_.find(row.id);// מחפשת את המצב הנוכחי של הצומת ב-state_store_
            auto action_it = action_store_.find(row.id);// מחפשת את הפעולה הנוכחית של הצומת ב-action_store_

            items.push_back({// מוסיפה אובייקט JSON עם המידע של הצומת למערך JSON
                {"intersection", {// מוסיפה אובייקט JSON עם המידע של הצומת
                    {"id", row.id},// מוסיפה את מזהה הצומת ל-JSON
                    {"code", row.code},// מוסיפה את קוד הצומת ל-JSON
                    {"name", row.name},// מוסיפה את שם הצומת ל-JSON
                    {"city", row.city},// מוסיפה את שם העיר של הצומת ל-JSON
                }},
                {"current_state", state_it != state_store_.end() ? json::parse(state_it->second) : json(nullptr)},// מוסיפה את המצב הנוכחי של הצומת ל-JSON אם קיים, אחרת מוסיפה null
                {"current_action", action_it != action_store_.end() ? json::parse(action_it->second) : json(nullptr)},// מוסיפה את הפעולה הנוכחית של הצומת ל-JSON אם קיימת, אחרת מוסיפה null
            });
        }

        res.set_content(json({// מחזירה סטטוס 200 OK ומסר הצלחה עם רשימת הצמתים
            {"status", "success"},// מוסיפה את המצב הנוכחי של הצומת ל-JSON אם קיים, אחרת מוסיפה null
            {"count", static_cast<int>(items.size())},// מוסיפה את מספר הצמתים ל-JSON
            {"intersections", items},// מוסיפה את רשימת הצמתים ל-JSON
        }).dump(), "application/json");// מחזירה סטטוס 200 OK ומסר הצלחה עם רשימת הצמתים
    });
// מטפלת בבקשה POST לנתיב /admin/intersection ומוסיפה צומת חדש למסד הנתונים
    auto create_admin_intersection = [](const httplib::Request& req, httplib::Response& res) {
        json body;// יוצרת אובייקט JSON ריק
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }
// מחלצת את הקוד והשם של הצומת מהגוף של הבקשה ומוודאת שהם קיימים
        const std::string code = body.value("code", "");
        const std::string name = body.value("name", "");
        if (code.empty() || name.empty() || !body.contains("latitude") || !body.contains("longitude")) {
            res.status = 422;
            res.set_content(json({{"detail", "code, name, latitude, longitude are required"}}).dump(), "application/json");
            return;
        }
// בודקת אם הקוד של הצומת כבר קיים במסד הנתונים ומחזירה שגיאה אם כן
        for (const auto& existing : db_fetch_intersections()) {
            if (existing.code == code) {
                res.status = 409;
                res.set_content(json({{"detail", "Intersection code already exists"}}).dump(), "application/json");
                return;
            }
        }
// מוסיפה את הצומת החדש למסד הנתונים ומחזירה שגיאה אם נכשל
        std::string error;
        const bool ok = db_insert_intersection(
            code,
            name,
            body["latitude"].get<double>(),
            body["longitude"].get<double>(),
            body.value("num_cameras", 4),
            body.contains("city") && !body["city"].is_null() ? std::optional<std::string>(body["city"].get<std::string>()) : std::nullopt,
            body.contains("region") && !body["region"].is_null() ? std::optional<std::string>(body["region"].get<std::string>()) : std::nullopt,
            body.contains("description") && !body["description"].is_null() ? std::optional<std::string>(body["description"].get<std::string>()) : std::nullopt,
            error);
        if (!ok) {
            res.status = 400;
            res.set_content(json({{"detail", error}}).dump(), "application/json");
            return;
        }
// מחזירה סטטוס 201 Created ומסר הצלחה עם המידע של הצומת החדש
        res.set_content(json({
            {"status", "success"},
            {"message", "Intersection created successfully"},
            {"data", body},
        }).dump(), "application/json");
    };
    svr_->Post("/admin/intersection", create_admin_intersection);// מטפלת בבקשה POST לנתיב /admin/intersection ומוסיפה צומת חדש למסד הנתונים
    svr_->Post("/admin/intersection/create", create_admin_intersection);// מטפלת בבקשה POST לנתיב /admin/intersection/create ומוסיפה צומת חדש למסד הנתונים

    // ── PUT /admin/intersection/{id} ───
    svr_->Put(R"(/admin/intersection/(\d+))", [](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה PUT לנתיב /admin/intersection/{id} ומעדכנת את המידע של הצומת במסד הנתונים
        json body;// יוצרת אובייקט JSON ריק
        try {// מנסה לפרסר את גוף הבקשה ל-JSON
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }

        const int id = std::stoi(req.matches[1]);// מחלצת את מזהה הצומת מהנתיב של הבקשה
        const auto name = body.contains("name") && !body["name"].is_null() ? std::optional<std::string>(body["name"].get<std::string>()) : std::nullopt;// מחלצת את השם של הצומת מהגוף של הבקשה אם קיים
        const auto num_cameras = body.contains("num_cameras") && !body["num_cameras"].is_null() ? std::optional<int>(body["num_cameras"].get<int>()) : std::nullopt;// מחלצת את מספר המצלמות של הצומת מהגוף של הבקשה אם קיים
        const auto city = body.contains("city") && !body["city"].is_null() ? std::optional<std::string>(body["city"].get<std::string>()) : std::nullopt;// מחלצת את העיר של הצומת מהגוף של הבקשה אם קיים
        const auto region = body.contains("region") && !body["region"].is_null() ? std::optional<std::string>(body["region"].get<std::string>()) : std::nullopt;// מחלצת את האזור של הצומת מהגוף של הבקשה אם קיים
        const auto description = body.contains("description") && !body["description"].is_null() ? std::optional<std::string>(body["description"].get<std::string>()) : std::nullopt;// מחלצת את התיאור של הצומת מהגוף של הבקשה אם קיים

        bool found = false;
        std::string error;
        const bool ok = db_update_intersection(id, name, num_cameras, city, region, description, found, error);// מנסה לעדכן את המידע של הצומת במסד הנתונים ומחזיר שגיאה אם נכשל
        if (!ok && error == "No updatable fields provided") {// אם לא סופקו שדות לעדכון, מחזירה סטטוס 400 Bad Request ומסר שגיאה
            res.status = 400;
            res.set_content(json({{"detail", error}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה אם לא סופקו שדות לעדכון
            return;
        }
        if (!found) {// אם הצומת לא נמצא במסד הנתונים, מחזירה סטטוס 404 Not Found ומסר שגיאה
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");// מחזירה סטטוס 404 Not Found ומסר שגיאה אם הצומת לא נמצא במסד הנתונים
            return;
        }
        if (!ok) {// אם העדכון נכשל מסיבה אחרת, מחזירה סטטוס 400 Bad Request ומסר שגיאה
            res.status = 400;
            res.set_content(json({{"detail", error}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה אם העדכון נכשל מסיבה אחרת
            return;
        }

        json data = json::object();// יוצרת אובייקט JSON ריק
        if (name) data["name"] = *name;// מוסיפה את השם של הצומת ל-JSON אם קיים
        if (num_cameras) data["num_cameras"] = *num_cameras;// מוסיפה את מספר המצלמות של הצומת ל-JSON אם קיים
        if (city) data["city"] = *city;// מוסיפה את העיר של הצומת ל-JSON אם קיים
        if (region) data["region"] = *region;// מוסיפה את האזור של הצומת ל-JSON אם קיים
        if (description) data["description"] = *description;// מוסיפה את התיאור של הצומת ל-JSON אם קיים

        res.set_content(json({// מחזירה סטטוס 200 OK ומסר הצלחה עם המידע המעודכן של הצומת
            {"status", "success"},
            {"message", "Intersection updated successfully"},
            {"data", data},
        }).dump(), "application/json");
    });

    // ── POST /state ──────────────────────────────────────────────────────────
    // מקבלת JSON של מצב הצומת מהמודול הראייה של Python.
    // מאחסנת את המצב, ואז מחזירה את הפעולה האחרונה שנבחרה על ידי RL (שפורסמה על ידי
    // smart_traffic_controller דרך POST /intersection/{id}/action).
    // חוזרת ל-Phase0 רק אם לא התקבלה עדיין פעולה של RL.
    svr_->Post("/state", [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה POST לנתיב /state ומקבלת את המצב הנוכחי של הצומת מהמודול הראייה של Python
        json body;// יוצרת אובייקט JSON ריק
        try {
            body = json::parse(req.body);// מנסה לפרסר את גוף הבקשה ל-JSON
        } catch (const json::exception& e) {// אם הפרסר נכשל, מחזירה סטטוס 400 Bad Request ומסר שגיאה
            res.status = 400;
            res.set_content(
                json({{"detail", std::string("Invalid JSON: ") + e.what()}}).dump(),
                "application/json");
            return;
        }
// בודקת אם יש מפתח "intersection_id" בגוף הבקשה ואם הוא מספר שלם
        if (!body.contains("intersection_id") || !body["intersection_id"].is_number_integer()) {
            res.status = 422;
            res.set_content(
                json({{"detail", "intersection_id is required and must be an integer"}}).dump(),
                "application/json");
            return;
        }
// מחלצת את מזהה הצומת מהגוף של הבקשה
        int id = body["intersection_id"].get<int>();
// בודקת אם יש מפתח "emergency_signal" בגוף הבקשה ואם הוא אובייקט
        std::string emergency_validation_reason;
        if (!validate_emergency_signal(body, emergency_validation_reason)) {
            res.status = 401;
            res.set_content(
                json({{"detail", std::string("Invalid emergency signal: ") + emergency_validation_reason}}).dump(),
                "application/json");
            return;
        }

        // ── Normalize lane count to DB schema ─────────────────────────────────
        // DB is the single source of truth for lane count.
        // Pad with zeros if live state has fewer lanes; truncate if more.
        {
            const auto& schema = get_cached_lane_schema(id);// שולף את סכמת הנתיבים מהמטמון עבור הצומת עם המזהה הנתון
            if (!schema.empty()) {// אם הסכמה אינה ריקה, מבצע נירמול של מספר הנתיבים לפי הסכמה של מסד הנתונים
                const int db_lane_count = static_cast<int>(schema.size());

                // Build lookup keyed by Python's 0-based lane_id, which
                // equals camera_index (the positional slot in the schema).
                // Using camera_index avoids the DB PK mismatch where
                // Python sends 0,1,2,3 but row.lane_id is an auto-increment
                // PK (e.g. 5,6,7 for intersection 2).
                std::unordered_map<int, json> live_by_camera;// יוצר מפת חיפוש שמקשרת בין מזהה הנתיב של Python לבין הנתונים החיוניים של הנתיב
                if (body.contains("lanes") && body["lanes"].is_array()) {// אם יש מפתח "lanes" בגוף הבקשה והוא מערך, מבצע לולאה על כל הנתיבים ומוסיף אותם למפת החיפוש
                    for (const auto& lane : body["lanes"]) {// מבצע לולאה על כל הנתיבים בגוף הבקשה
                        if (lane.contains("lane_id") && lane["lane_id"].is_number_integer()) {// אם יש מפתח "lane_id" בנתיב והוא מספר שלם, מוסיף את הנתיב למפת החיפוש עם המפתח כ- lane_id
                            live_by_camera[lane["lane_id"].get<int>()] = lane;
                        }
                    }
                }

                // Rebuild lanes array from DB schema order.
                // schema is sorted by camera_index, so schema[i].camera_index == i.
                // Python lane_id == camera_index, so look up by i.
                // Lane IDs in output are always 0-based so React can index
                // directly into the lane_directions array from /layout.
                json normalized_lanes = json::array();// יוצר מערך JSON ריק לנירמול הנתיבים
                for (int i = 0; i < static_cast<int>(schema.size()); ++i) {// מבצע לולאה על כל הנתיבים בסכמה של מסד הנתונים
                    const auto& row = schema[i];// שולף את השורה הנוכחית מהסכמה של מסד הנתונים
                    // Primary key: camera_index position (== Python lane_id).
                    // Fallback: try row.camera_index explicitly, then DB PK.
                    auto it = live_by_camera.find(i);// מחפש את הנתיב במפת החיפוש לפי המפתח i (מזהה הנתיב של Python)
                    if (it == live_by_camera.end())// אם הנתיב לא נמצא במפת החיפוש לפי המפתח i, מחפש את הנתיב במפת החיפוש לפי המפתח camera_index
                        it = live_by_camera.find(row.camera_index);// אם הנתיב לא נמצא במפת החיפוש לפי המפתח camera_index, מחפש את הנתיב במפת החיפוש לפי המפתח lane_id
                    if (it == live_by_camera.end())// אם הנתיב לא נמצא במפת החיפוש לפי המפתח lane_id, מחפש את הנתיב במפת החיפוש לפי המפתח lane_id של השורה הנוכחית בסכמה של מסד הנתונים
                        it = live_by_camera.find(row.lane_id);// אם הנתיב לא נמצא במפת החיפוש לפי המפתח lane_id של השורה הנוכחית בסכמה של מסד הנתונים, מחפש את הנתיב במפת החיפוש לפי המפתח lane_id של השורה הנוכחית בסכמה של מסד הנתונים

                    if (it != live_by_camera.end()) {// אם הנתיב נמצא במפת החיפוש, מוסיף את הנתיב למערך normalized_lanes עם המזהה של הנתיב והכיוון מהסכמה של מסד הנתונים
                        // Use live metric values; always stamp canonical
                        // lane_id and direction from DB schema.
                        json lane = it->second;// מוסיף את הנתיב למערך normalized_lanes עם המזהה של הנתיב והכיוון מהסכמה של מסד הנתונים
                        lane["lane_id"]  = i;              // 0-based output
                        lane["direction"] = row.direction; // canonical DB direction
                        normalized_lanes.push_back(std::move(lane));
                    } else {
                        // Lane absent from live data — pad with zeros.
                        json zero_lane = {
                            {"lane_id",          i},
                            {"direction",        row.direction},
                            {"vehicle_count",    0},
                            {"queue_length",     0},
                            {"density_pct",      0.0},
                            {"waiting_time_sec", 0.0},
                            //{"pedestrian_count", 0},
                        };
                        normalized_lanes.push_back(std::move(zero_lane));// מוסיף את הנתיב למערך normalized_lanes עם המזהה של הנתיב והכיוון מהסכמה של מסד הנתונים ומאפיינים אפסיים
                    }
                }

                body["lanes"]     = std::move(normalized_lanes);
                body["num_lanes"] = db_lane_count;
            }
        }
        // ──────────

        //אות חירום: אם יש אות חירום פעיל, מעדכנת את המצב בהתאם
        if (body.contains("emergency_signal") && body["emergency_signal"].is_object()) {// אם יש מפתח "emergency_signal" בגוף הבקשה והוא אובייקט, בודקת אם האות חירום פעיל וטרי ומעדכנת את המצב בהתאם
            const auto& sig = body["emergency_signal"];// שולף את האות חירום מהגוף של הבקשה
            if (sig.value("active", false)) {
                latch_emergency(id, sig);// אם האות חירום פעיל, מעדכנת את המצב עם האות חירום
            }
        }

        // Store the (lane-normalised) state.
        {
            std::lock_guard<std::mutex> lk(store_mutex_);// ננעל את המוטקס כדי למנוע גישה מקבילה ל-state_store_
            state_store_[id] = body.dump();// מאחסנת את המצב הנוכחי של הצומת ב-state_store_ עם המזהה של הצומת כמפתח
        }

        // Determine the action to return, mirroring the Python decision flow:
        //   1) If emergency is currently latched → emergency_preempt overrides.
        //   2) Otherwise if a controller / manual action is "sticky", keep it.
        //   3) Otherwise compute a server-side greedy-with-aging fallback.
        constexpr double CPP_ACTION_STICKY_SEC    = traffic::constants::kCppActionStickySec;// מחזירה את הזמן המקסימלי שבו פעולה שנבחרה על ידי בקרת C++ או על ידי לוח הבקרה הידני נשארת "דביקה" (sticky) לפני שהיא מתעדכנת מחדש
        constexpr double MANUAL_ACTION_STICKY_SEC = traffic::constants::kManualActionStickySec;// מחזירה את הזמן המקסימלי שבו פעולה שנבחרה על ידי לוח הבקרה הידני נשארת "דביקה" (sticky) לפני שהיא מתעדכנת מחדש

        const double now_s = std::chrono::duration<double>(// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
            std::chrono::system_clock::now().time_since_epoch()).count();// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת

        json action;// יוצרת אובייקט JSON ריק לפעולה שתוחזר
        std::string chosen_source = "server_fallback";

        json active_emergency = active_emergency_signal(id);// שולף את האות חירום הפעיל עבור הצומת עם המזהה הנתון
        if (!active_emergency.is_null()) {// אם האות חירום פעיל, מחלצת את הנתיב של האות חירום ומחשבת את מזהה הפאזה המתאים ומעדכנת את הפעולה שתוחזר
            const int em_lane = active_emergency.value("lane_id", 0);// מחלצת את מזהה הנתיב של האות חירום מהאות חירום הפעיל
            const int phase_id = (em_lane % 2 == 0) ? 0 : 1;// מחשבת את מזהה הפאזה המתאים לפי מזהה הנתיב של האות חירום (אם הנתיב זוגי, הפאזה היא 0, אחרת הפאזה היא 1)
            std::cout << "[GreedyAging] Intersection " << id// מדפיסה הודעה ללוג עם מזהה הצומת, הנתיב של האות חירום, מזהה הרכב של האות חירום ומזהה הפאזה המתאים
                      << " EMERGENCY OVERRIDE: lane=" << em_lane// מדפיסה את מזהה הנתיב של האות חירום
                      << " vehicle_id=" << active_emergency.value("vehicle_id", std::string("?"))// מדפיסה את מזהה הרכב של האות חירום
                      << " => Phase" << phase_id << "\n";// מדפיסה את מזהה הפאזה המתאים
            action = {// מעדכנת את הפעולה שתוחזר עם המידע של האות חירום
                {"action",         std::string("Phase") + std::to_string(phase_id)},
                {"reason",         "emergency_preempt"},
                {"intersection_id", id},
                {"phase_id",       phase_id},
            };
            chosen_source = "emergency_preempt";
        } else {
            std::string prev_source;
            double      prev_updated_at = 0.0;
            json        prev_action;
            {// ננעל את המוטקס כדי למנוע גישה מקבילה ל-action_source_store_, action_updated_at_store_ ו-action_store_
                std::lock_guard<std::mutex> lk(store_mutex_);// ננעל את המוטקס כדי למנוע גישה מקבילה ל-action_source_store_, action_updated_at_store_ ו-action_store_
                auto src_it = action_source_store_.find(id);// מחפש את המקור של הפעולה הקודמת עבור הצומת עם המזהה הנתון ב-action_source_store_
                auto upd_it = action_updated_at_store_.find(id);// מחפש את הזמן שבו הפעולה הקודמת עודכנה עבור הצומת עם המזהה הנתון ב-action_updated_at_store_
                auto act_it = action_store_.find(id);// מחפש את הפעולה הקודמת עבור הצומת עם המזהה הנתון ב-action_store_
                if (src_it != action_source_store_.end()) prev_source = src_it->second;// אם נמצא המקור של הפעולה הקודמת, מעדכן את prev_source עם המקור של הפעולה הקודמת
                if (upd_it != action_updated_at_store_.end()) prev_updated_at = upd_it->second;// אם נמצא הזמן שבו הפעולה הקודמת עודכנה, מעדכן את prev_updated_at עם הזמן שבו הפעולה הקודמת עודכנה
                if (act_it != action_store_.end()) {// אם נמצאה הפעולה הקודמת, מנסה לפרסר את הפעולה הקודמת ל-JSON ומעדכן את prev_action עם הפעולה הקודמת
                    try { prev_action = json::parse(act_it->second); } catch (...) {}// אם הפרסר נכשל, מתעלם מהשגיאה וממשיך
                }
            }

            const double age = now_s - prev_updated_at;// מחשבת את הגיל של הפעולה הקודמת בשניות מאז שהפעולה הקודמת עודכנה
            const bool cpp_sticky    = (prev_source == "cpp_controller"// בודקת אם המקור של הפעולה הקודמת הוא "cpp_controller" ואם הפעולה הקודמת היא אובייקט JSON ואם הגיל של הפעולה הקודמת קטן או שווה ל-CPP_ACTION_STICKY_SEC
                                        && prev_action.is_object()// אם הפעולה הקודמת היא אובייקט JSON
                                        && age <= CPP_ACTION_STICKY_SEC);// אם הגיל של הפעולה הקודמת קטן או שווה ל-CPP_ACTION_STICKY_SEC
            const bool manual_sticky = (prev_source == "manual_dashboard"// בודקת אם המקור של הפעולה הקודמת הוא "manual_dashboard" ואם הפעולה הקודמת היא אובייקט JSON ואם הגיל של הפעולה הקודמת קטן או שווה ל-MANUAL_ACTION_STICKY_SEC
                                        && prev_action.is_object()// אם הפעולה הקודמת היא אובייקט JSON
                                        && age <= MANUAL_ACTION_STICKY_SEC);// אם הגיל של הפעולה הקודמת קטן או שווה ל-MANUAL_ACTION_STICKY_SEC

            if (cpp_sticky || manual_sticky) {// אם המקור של הפעולה הקודמת הוא "cpp_controller" או "manual_dashboard" ואם הפעולה הקודמת היא אובייקט JSON ואם הגיל של הפעולה הקודמת קטן או שווה ל-CPP_ACTION_STICKY_SEC או ל-MANUAL_ACTION_STICKY_SEC, מחזירה את הפעולה הקודמת
                std::cout << "[GreedyAging] Intersection " << id// מדפיסה הודעה ללוג עם מזהה הצומת, המקור של הפעולה הקודמת, הגיל של הפעולה הקודמת והפעולה הקודמת
                          << " STICKY action retained (source=" << prev_source// מדפיסה את המקור של הפעולה הקודמת
                          << " age=" << std::fixed << std::setprecision(1) << age << "s)"// מדפיסה את הגיל של הפעולה הקודמת עם דיוק של ספרה אחת אחרי הנקודה העשרונית
                          << " action=" << (prev_action.contains("action") ? prev_action["action"].get<std::string>() : "?")// מדפיסה את הפעולה הקודמת אם קיימת, אחרת מדפיסה "?"
                          << "\n";
                action = prev_action;
                chosen_source = prev_source;
            } else {// אם המקור של הפעולה הקודמת אינו "cpp_controller" או "manual_dashboard" או שהפעולה הקודמת אינה אובייקט JSON או שהגיל של הפעולה הקודמת גדול מ-CPP_ACTION_STICKY_SEC או ל-MANUAL_ACTION_STICKY_SEC, מחשבת את הפעולה שתוחזר על ידי קריאה לפונקציה decide_action_fallback עם המזהה של הצומת והמצב הנוכחי של הצומת
                action = decide_action_fallback(id, body);// מחשבת את הפעולה שתוחזר על ידי קריאה לפונקציה decide_action_fallback עם המזהה של הצומת והמצב הנוכחי של הצומת
                chosen_source = "server_fallback";// מעדכנת את המקור של הפעולה שתוחזר ל-"server_fallback"
            }
        }

        // Persist the chosen action with source / timestamp.
        {// ננעל את המוטקס כדי למנוע גישה מקבילה ל-action_store_, action_source_store_ ו-action_updated_at_store_
            std::lock_guard<std::mutex> lk(store_mutex_);
            action_store_[id]            = action.dump();
            action_source_store_[id]     = chosen_source;
            action_updated_at_store_[id] = now_s;
        }

        // Broadcast state_updated (per-intersection AND global subscribers).
        broadcast_event(id, "state_updated", json({// שולח אירוע לכל המנויים עם מזהה הצומת, סוג האירוע והמצב הנוכחי של הצומת, הפעולה שנבחרה והמקור של הפעולה
            {"state",  body},
            {"action", action},
            {"source", chosen_source},
        }));
// מחזירה את הפעולה שנבחרה למודול הראייה של Python
        res.set_content(action.dump(), "application/json");
    });

    // ── GET /intersection/{id}/packet ────────────────────────────────────────
    // Returns the last stored state + action for an intersection.
    svr_->Get(R"(/intersection/(\d+)/packet)",
        [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /intersection/{id}/packet ומחזירה את המצב הנוכחי של הצומת והפעולה שנבחרה עבור הצומת עם המזהה הנתון

        int id = std::stoi(req.matches[1]);// מחלצת את מזהה הצומת מהנתיב של הבקשה

        std::string state_str, action_str;// יוצרת מחרוזות ריקות למצב הנוכחי של הצומת והפעולה שנבחרה עבור הצומת עם המזהה הנתון
        {
            std::lock_guard<std::mutex> lk(store_mutex_);// ננעל את המוטקס כדי למנוע גישה מקבילה ל-state_store_ ו-action_store_
            auto si = state_store_.find(id);// מחפש את המצב הנוכחי של הצומת עם המזהה הנתון ב-state_store_
            auto ai = action_store_.find(id);// מחפש את הפעולה שנבחרה עבור הצומת עם המזהה הנתון ב-action_store_
            if (si == state_store_.end()) {// אם המצב הנוכחי של הצומת לא נמצא ב-state_store_, מחזירה סטטוס 404 Not Found ומסר שגיאה
                res.status = 404;// מחזירה סטטוס 404 Not Found ומסר שגיאה אם המצב הנוכחי של הצומת לא נמצא ב-state_store_
                res.set_content(// מחזירה סטטוס 404 Not Found ומסר שגיאה אם המצב הנוכחי של הצומת לא נמצא ב-state_store_
                    json({{"detail", "Intersection state not found"}}).dump(),// מחזירה סטטוס 404 Not Found ומסר שגיאה אם המצב הנוכחי של הצומת לא נמצא ב-state_store_
                    "application/json");
                return;
            }
            state_str  = si->second;// מעדכנת את המצב הנוכחי של הצומת עם המזהה הנתון מהמחרוזת שנמצאה ב-state_store_
            action_str = (ai != action_store_.end()) ? ai->second : "null";// מעדכנת את הפעולה שנבחרה עבור הצומת עם המזהה הנתון מהמחרוזת שנמצאה ב-action_store_, אם קיימת, אחרת מעדכנת ל-"null"
        }

        double ts = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת

        json packet = {// יוצרת אובייקט JSON עם המידע של הצומת עם המזהה הנתון
            {"intersection_id", id},
            {"timestamp",   ts},
            {"state",       json::parse(state_str)},
            {"last_action", json::parse(action_str)},
            {"neighbors",   build_neighbor_summaries(id)},
        };

        res.set_content(packet.dump(), "application/json");// מחזירה את המידע של הצומת עם המזהה הנתון בפורמט JSON
    });

    // ── GET /intersection/{id}/action ────────────────────────────────────────
    // Returns the last recorded action for an intersection.
    svr_->Get(R"(/intersection/(\d+)/action)",// מטפלת בבקשה GET לנתיב /intersection/{id}/action ומחזירה את הפעולה האחרונה שנבחרה עבור הצומת עם המזהה הנתון
        [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /intersection/{id}/action ומחזירה את הפעולה האחרונה שנבחרה עבור הצומת עם המזהה הנתון
// מחלצת את מזהה הצומת מהנתיב של הבקשה
        int id = std::stoi(req.matches[1]);

        // Emergency always overrides any stored action.
        json active_em = active_emergency_signal(id);// שולף את האות חירום הפעיל עבור הצומת עם המזהה הנתון
        if (!active_em.is_null()) {// אם יש חירום פעיל עבור הצומת עם המזהה הנתון, מחזירה את הפעולה המתאימה לחירום
            const int em_lane  = active_em.value("lane_id", 0);// מחלצת את מזהה הנתיב של האות חירום מהאות חירום הפעיל
            const int phase_id = (em_lane % 2 == 0) ? 0 : 1;// מחשבת את מזהה הפאזה המתאים לפי מזהה הנתיב של האות חירום (אם הנתיב זוגי, הפאזה היא 0, אחרת הפאזה היא 1)
            res.set_content(json({// מחזירה את הפעולה המתאימה לחירום בפורמט JSON
                {"action",          std::string("Phase") + std::to_string(phase_id)},
                {"reason",          "emergency_preempt"},
                {"intersection_id", id},
                {"phase_id",        phase_id},
            }).dump(), "application/json");
            return;
        }

        std::lock_guard<std::mutex> lk(store_mutex_);// ננעל את המוטקס כדי למנוע גישה מקבילה ל-state_store_ ו-action_store_

        auto si = state_store_.find(id);// מחפש את המצב הנוכחי של הצומת עם המזהה הנתון ב-state_store_
        if (si == state_store_.end()) {// אם המצב הנוכחי של הצומת לא נמצא ב-state_store_, מחזירה סטטוס 404 Not Found ומסר שגיאה
            res.status = 404;// מחזירה סטטוס 404 Not Found ומסר שגיאה אם המצב הנוכחי של הצומת לא נמצא ב-state_store_
            res.set_content(// מחזירה סטטוס 404 Not Found ומסר שגיאה אם המצב הנוכחי של הצומת לא נמצא ב-state_store_
                json({{"detail", "Intersection state not found"}}).dump(),// מחזירה סטטוס 404 Not Found ומסר שגיאה אם המצב הנוכחי של הצומת לא נמצא ב-state_store_
                "application/json");
            return;
        }
// מחפש את הפעולה שנבחרה עבור הצומת עם המזהה הנתון ב-action_store_
        auto ai = action_store_.find(id);
        if (ai != action_store_.end()) {
            res.set_content(ai->second, "application/json");
        } else {
            // No action yet — return a Phase0 fallback
            json fallback = {
                {"action",         "Phase0"},
                {"reason",         "server_fallback"},
                {"intersection_id", id},
                {"phase_id",       0},
            };
            res.set_content(fallback.dump(), "application/json");// מחזירה את הפעולה המתאימה לפאזה 0 בפורמט JSON אם לא נמצאה פעולה שנבחרה עבור הצומת עם המזהה הנתון
        }
    });

    // ── POST /intersection/{id}/action ───────────
    // C++ controller (or admin dashboard) reports the chosen action.
    // Body: { "action": "Phase0", "phase_id": 0, "reason": "..." }
    svr_->Post(R"(/intersection/(\d+)/action)",// מטפלת בבקשה POST לנתיב /intersection/{id}/action ומקבלת את הפעולה שנבחרה עבור הצומת עם המזהה הנתון
        [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה POST לנתיב /intersection/{id}/action ומקבלת את הפעולה שנבחרה עבור הצומת עם המזהה הנתון

        int id = std::stoi(req.matches[1]);// מחלצת את מזהה הצומת מהנתיב של הבקשה

        json body;// יוצרת אובייקט JSON ריק
        try {// מנסה לפרסר את גוף הבקשה ל-JSON
            body = json::parse(req.body);
        } catch (...) {// אם הפרסר נכשל, מחזירה סטטוס 400 Bad Request ומסר שגיאה
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }
// בודקת אם יש מפתח "action" בגוף הבקשה ואם הוא מחרוזת
        if (!body.contains("action") || !body["action"].is_string()) {
            res.status = 422;
            res.set_content(json({{"detail", "action field is required"}}).dump(), "application/json");
            return;
        }

        {// ננעל את המוטקס כדי למנוע גישה מקבילה ל-state_store_ ובודקת אם המצב הנוכחי של הצומת עם המזהה הנתון קיים ב-state_store_
            std::lock_guard<std::mutex> lk(store_mutex_);
            if (state_store_.find(id) == state_store_.end()) {
                res.status = 404;
                res.set_content(
                    json({{"detail", "Intersection state not found"}}).dump(),
                    "application/json");
                return;
            }
        }
// מחלצת את מזהה הפאזה מהגוף של הבקשה, אם לא קיים מחזירה 0
        int phase_id = body.value("phase_id", 0);
        std::string action_str = body["action"].get<std::string>();
        std::string reason     = body.value("reason", "selected_by_cpp_controller");

        // Emergency is active — reject controller action, return emergency phase instead.
        {
            json active_em = active_emergency_signal(id);// שולף את האות חירום הפעיל עבור הצומת עם המזהה הנתון
            if (!active_em.is_null()) {// אם יש חירום פעיל עבור הצומת עם המזהה הנתון, מחזירה את הפעולה המתאימה לחירום
                const int em_lane  = active_em.value("lane_id", 0);// מחלצת את מזהה הנתיב של האות חירום מהאות חירום הפעיל
                const int em_phase = (em_lane % 2 == 0) ? 0 : 1;// מחשבת את מזהה הפאזה המתאים לפי מזהה הנתיב של האות חירום (אם הנתיב זוגי, הפאזה היא 0, אחרת הפאזה היא 1)
                res.set_content(json({// מחזירה את הפעולה המתאימה לחירום בפורמט JSON
                    {"action",          std::string("Phase") + std::to_string(em_phase)},
                    {"reason",          "emergency_preempt"},
                    {"intersection_id", id},
                    {"phase_id",        em_phase},
                }).dump(), "application/json");
                return;
            }
        }

        // Classify source from the reason hint, mirroring Python:
        //   reasons containing "manual"/"dashboard" → manual_dashboard (20s sticky)
        //   everything else                         → cpp_controller   (5s sticky)
        std::string source = "cpp_controller";// מחלצת את המקור של הפעולה שנבחרה מהסיבה שניתנה בגוף הבקשה, אם הסיבה מכילה את המילים "manual" או "dashboard", מעדכנת את המקור ל-"manual_dashboard", אחרת מעדכנת ל-"cpp_controller"
        std::string lower_reason = reason;// מעתיקה את הסיבה שניתנה בגוף הבקשה למחרוזת חדשה
        for (auto& ch : lower_reason) ch = (char)std::tolower((unsigned char)ch);// ממירה את המחרוזת החדשה לאותיות קטנות
        if (lower_reason.find("manual") != std::string::npos ||
            lower_reason.find("dashboard") != std::string::npos) {// אם המחרוזת החדשה מכילה את המילים "manual" או "dashboard", מעדכנת את המקור ל-"manual_dashboard"
            source = "manual_dashboard";
        }
// יוצרת אובייקט JSON עם המידע של הפעולה שנבחרה עבור הצומת עם המזהה הנתון
        json action = {
            {"action",         action_str},
            {"reason",         reason},
            {"intersection_id", id},
            {"phase_id",       phase_id},
        };
// מאחסנת את הפעולה שנבחרה עבור הצומת עם המזהה הנתון ב-action_store_ עם המזהה של הצומת כמפתח
        const double now_s = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        {// ננעל את המוטקס כדי למנוע גישה מקבילה ל-action_store_, action_source_store_ ו-action_updated_at_store_
            std::lock_guard<std::mutex> lk(store_mutex_);
            action_store_[id]            = action.dump();
            action_source_store_[id]     = source;
            action_updated_at_store_[id] = now_s;
        }
// שולח אירוע לכל המנויים עם מזהה הצומת, סוג האירוע והפעולה שנבחרה עבור הצומת עם המזהה הנתון
        broadcast_event(id, "action_updated", json({
            {"action", action},
            {"source", source},
        }));
// מחזירה את הפעולה שנבחרה למודול הראייה של Python
        res.set_content(action.dump(), "application/json");
    });

    // ── GET /intersection/{id}/lanes ────
    // קוראת את הגדרות הנתיבים מהטבלה dbo.intersection_lanes.
    svr_->Get(R"(/intersection/(\d+)/lanes)",
        [](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /intersection/{id}/lanes ומחזירה את הגדרות הנתיבים עבור הצומת עם המזהה הנתון
// מחלצת את מזהה הצומת מהנתיב של הבקשה
        int id = std::stoi(req.matches[1]);
        auto rows = db_fetch_lanes(id);
// יוצרת מערך JSON ריק וממלאת אותו עם הנתונים של הנתיבים עבור הצומת עם המזהה הנתון
        json arr = json::array();
        for (const auto& r : rows) {
            json lane = {
                {"lane_id",      r.lane_id},
                {"camera_index", r.camera_index},
                {"direction",    r.direction},
            };
            if (!r.description.empty())// אם התיאור של הנתיב אינו ריק, מוסיפה את התיאור למערך JSON
                lane["description"] = r.description;
            else// אם התיאור של הנתיב ריק, מוסיפה את המפתח "description" עם ערך null למערך JSON
                lane["description"] = nullptr;
            arr.push_back(lane);
        }
        res.set_content(arr.dump(), "application/json");// מחזירה את המערך JSON עם הנתונים של הנתיבים עבור הצומת עם המזהה הנתון
    });

    // ── GET /intersection/{id}/conflicts ──
    // קוראת את הזוגות של הנתיבים המתנגשים מהטבלה dbo.lane_conflicts.
    // צורת התגובה תואמת בדיוק לשרת Python.
    svr_->Get(R"(/intersection/(\d+)/conflicts)",// מטפלת בבקשה GET לנתיב /intersection/{id}/conflicts ומחזירה את הזוגות של הנתיבים המתנגשים עבור הצומת עם המזהה הנתון
        [](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /intersection/{id}/conflicts ומחזירה את הזוגות של הנתיבים המתנגשים עבור הצומת עם המזהה הנתון
// מחלצת את מזהה הצומת מהנתיב של הבקשה
        int id = std::stoi(req.matches[1]);
        auto rows = db_fetch_conflicts(id);
// יוצרת מערך JSON ריק וממלאת אותו עם הזוגות של הנתיבים המתנגשים עבור הצומת עם המזהה הנתון
        json pair_list = json::array();
        json conflict_rows = json::array();
        for (const auto& r : rows) {// עבור כל זוג של נתיבים מתנגשים, מוסיפה את המזהים של הנתיבים למערך pair_list ומוסיפה את המידע של הזוג למערך conflict_rows
            pair_list.push_back({r.lane_id_1, r.lane_id_2});
            json row = {// מוסיפה את המידע של הזוג למערך conflict_rows
                {"conflict_id",   r.conflict_id},
                {"lane_id_1",     r.lane_id_1},
                {"lane_id_2",     r.lane_id_2},
                {"conflict_type", r.conflict_type},
            };
            if (!r.created_at.empty())// אם התאריך שבו נוצר הזוג אינו ריק, מוסיפה את התאריך למערך JSON
                row["created_at"] = r.created_at;
            else// אם התאריך שבו נוצר הזוג ריק, מוסיפה את המפתח "created_at" עם ערך null למערך JSON
                row["created_at"] = nullptr;
            conflict_rows.push_back(row);
        }
// יוצרת אובייקט JSON עם המידע של הזוגות של הנתיבים המתנגשים עבור הצומת עם המזהה הנתון
        json response = {
            {"status",         "success"},
            {"intersection_id", id},
            {"count",          (int)rows.size()},
            {"conflicts",      pair_list},
            {"conflict_rows",  conflict_rows},
        };
        res.set_content(response.dump(), "application/json");// מחזירה את המידע של הזוגות של הנתיבים המתנגשים עבור הצומת עם המזהה הנתון בפורמט JSON
    });

    // ── GET / ────
    svr_->Get("/", [this](const httplib::Request& /*req*/, httplib::Response& res) {// מטפלת בבקשה GET לנתיב / ומחזירה את המידע של השרת בפורמט JSON
        res.set_content(json({// יוצרת אובייקט JSON עם המידע של השרת
            {"status",  "running"},
            {"message", "Smart Traffic Server (C++) — hybrid backend"},
            {"docs",    "/health"},
            {"health",  "/health"},
            {"intersections", "/intersections"},
            {"security", {
                {"jwt",         "enabled"},
                {"cors_origin", "*"},
            }},
            {"configuration", {
                {"port",            port_},
                {"router_mode",     router_mode_},
                {"internal_port",   internal_http_port_},
            }},
            {"emergency_auth", {
                {"vehicle_key_count", static_cast<int>(emergency_keys_.size())},
                {"max_clock_skew_sec", emergency_max_clock_skew_sec_},
            }},
            {"hardware", json::object()},
        }).dump(), "application/json");
    });

    // ── GET /config ─────────
    svr_->Get("/config", [](const httplib::Request& /*req*/, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /config ומחזירה את המידע של הקונפיגורציה של השרת בפורמט JSON
        res.set_content(json({// יוצרת אובייקט JSON עם המידע של הקונפיגורציה של השרת
            {"status",        "ok"},
            {"configuration", json::object()},
            {"thresholds",    json::object()},
            {"network",       json::object()},
            {"emergency",     json::object()},
            {"rl_agent",      json::object()},
            {"hardware",      {{"real_mode", false}, {"manual_emergency_enabled", true}}},// יוצרת אובייקט JSON עם המידע של החומרה של השרת
        }).dump(), "application/json");
    });

    // ── GET /metrics/summary ────────────
    svr_->Get("/metrics/summary", [this](const httplib::Request& /*req*/, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /metrics/summary ומחזירה את המידע של המדדים של השרת בפורמט JSON
        const double ts = std::chrono::duration<double>(// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
            std::chrono::system_clock::now().time_since_epoch()).count();// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
// יוצרת מערך JSON ריק וממלאת אותו עם המידע של הצמתים
        json by_intersection = json::array();
        int  total_queue     = 0;
        double waiting_sum   = 0.0;
        int    lane_total    = 0;
// ננעל את המוטקס כדי למנוע גישה מקבילה ל-state_store_
        std::lock_guard<std::mutex> lk(store_mutex_);
        for (const auto& kv : state_store_) {// עבור כל צומת במאגר המצב, מחלצת את המזהה של הצומת ואת המצב הנוכחי של הצומת
            int id = kv.first;// מחלצת את המזהה של הצומת
            json state;
            try { state = json::parse(kv.second); } catch (...) { continue; }// מנסה לפרסר את המצב הנוכחי של הצומת ל-JSON, אם הפרסר נכשל, ממשיך לצומת הבא
// מחלצת את מספר הרכבים בתור, זמן ההמתנה הממוצע ומספר הנתיבים מהמצב הנוכחי של הצומת
            int  queue   = 0;
            double wait  = 0.0;
            int  lanes_n = 0;
            if (state.contains("lanes") && state["lanes"].is_array()) {// אם המצב הנוכחי של הצומת מכיל את המפתח "lanes" והוא מערך, מחלצת את מספר הרכבים בתור, זמן ההמתנה הממוצע ומספר הנתיבים מהמצב הנוכחי של הצומת
                for (const auto& l : state["lanes"]) {// עבור כל נתיב במערך "lanes", מחלצת את מספר הרכבים בתור וזמן ההמתנה הממוצע
                    if (l.contains("vehicle_count") && l["vehicle_count"].is_number()) {// אם הנתיב מכיל את המפתח "vehicle_count" והוא מספר, מוסיפה את מספר הרכבים בתור למשתנה queue
                        queue += std::max(0, (int)l["vehicle_count"].get<double>());// מוסיפה את מספר הרכבים בתור למשתנה queue
                    }
                    if (l.contains("waiting_time_sec") && l["waiting_time_sec"].is_number()) {// אם הנתיב מכיל את המפתח "waiting_time_sec" והוא מספר, מוסיפה את זמן ההמתנה למשתנה wait
                        wait += std::max(0.0, l["waiting_time_sec"].get<double>());// מוסיפה את זמן ההמתנה למשתנה wait
                    }
                    ++lanes_n;// מגדילה את מספר הנתיבים ב-1
                }
            }
            const double avg_wait = lanes_n > 0 ? wait / lanes_n : 0.0;// מחשבת את זמן ההמתנה הממוצע על ידי חלוקת זמן ההמתנה הכולל במספר הנתיבים, אם מספר הנתיבים גדול מ-0, אחרת מחזירה 0.0
            total_queue += queue;
            waiting_sum += avg_wait;
            ++lane_total;

            by_intersection.push_back({// מוסיפה את המידע של הצומת למערך by_intersection
                {"intersection_id", id},
                {"total_queue",     queue},
                {"avg_waiting_sec", avg_wait},
            });
        }
// יוצרת אובייקט JSON עם המידע של המדדים של השרת
        res.set_content(json({
            {"timestamp",                ts},
            {"intersection_count",       static_cast<int>(state_store_.size())},
            {"total_network_queue",      total_queue},
            {"avg_network_waiting_sec",  lane_total > 0 ? waiting_sum / lane_total : 0.0},
            {"intersections",            by_intersection},
            {"security",                 {{"jwt", "enabled"}}},
        }).dump(), "application/json");
    });

    // ── GET /intersection/{id} ──────────────
    svr_->Get(R"(/intersection/(\d+))",// מטפלת בבקשה GET לנתיב /intersection/{id} ומחזירה את המצב הנוכחי של הצומת עם המזהה הנתון
        [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /intersection/{id} ומחזירה את המצב הנוכחי של הצומת עם המזהה הנתון
        int id = std::stoi(req.matches[1]);

        // מנסה את המצב החי קודם.
        std::string state_str;
        bool found = false;// יוצרת משתנה בוליאני בשם found ומאתחלת אותו ל-false
        {
            std::lock_guard<std::mutex> lk(store_mutex_);// ננעל את המוטקס כדי למנוע גישה מקבילה ל-state_store_
            auto it = state_store_.find(id);// מחפש את המצב הנוכחי של הצומת עם המזהה הנתון ב-state_store_
            if (it != state_store_.end()) {// אם המצב הנוכחי של הצומת נמצא ב-state_store_, מעדכנת את המשתנה state_str למחרוזת שנמצאה ב-state_store_ ומעדכנת את המשתנה found ל-true
                state_str = it->second;// מעדכנת את המשתנה state_str למחרוזת שנמצאה ב-state_store_
                found = true;
            }
        }

        if (!found) {// אם המצב הנוכחי של הצומת לא נמצא ב-state_store_, מנסה ליצור מצב אפס מהסכמה של הנתיבים כדי שהגריד של React יטען מיד (כל האפסים, ללא טעינה)
            // No state posted yet — synthesize a zero-state from the DB lane
            // schema so the React grid renders immediately (all zeros, no loading).
            const auto& schema = get_cached_lane_schema(id);// שולף את הסכמה של הנתיבים עבור הצומת עם המזהה הנתון מהמטמון
            if (schema.empty()) {// אם הסכמה של הנתיבים עבור הצומת עם המזהה הנתון ריקה, מחזירה סטטוס 404 Not Found ומסר שגיאה
                res.status = 404;
                res.set_content(json({{"detail", "Intersection state not found"}}).dump(),
                                "application/json");
                return;
            }
            const double now_ts = std::chrono::duration<double>(// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
                std::chrono::system_clock::now().time_since_epoch()).count();// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
            json zero_state = {// יוצרת אובייקט JSON עם המידע של הצומת עם המזהה הנתון
                {"intersection_id",  id},
                {"timestamp",        now_ts},
                {"num_lanes",        static_cast<int>(schema.size())},
                {"total_vehicles",   0},
                {"emergency_signal", nullptr},
            };
            json lanes = json::array();// יוצרת מערך JSON ריק וממלאת אותו עם המידע של הנתיבים עבור הצומת עם המזהה הנתון
            for (int i = 0; i < static_cast<int>(schema.size()); ++i) {
                lanes.push_back({
                    {"lane_id",          i},
                    {"direction",        schema[i].direction},
                    {"vehicle_count",    0},
                    {"queue_length",     0},
                    {"density_pct",      0.0},
                    {"waiting_time_sec", 0.0},
                    //{"pedestrian_count", 0},
                });
            }
            zero_state["lanes"] = std::move(lanes);// מוסיפה את המערך JSON עם המידע של הנתיבים לאובייקט JSON עם המידע של הצומת עם המזהה הנתון
            state_str = zero_state.dump();
        }

        res.set_content(state_str, "application/json");// מחזירה את המצב הנוכחי של הצומת עם המזהה הנתון בפורמט JSON
    });

    // ── GET /intersection/{id}/layout ───
    svr_->Get(R"(/intersection/(\d+)/layout)",// מטפלת בבקשה GET לנתיב /intersection/{id}/layout ומחזירה את המידע של הסכמה של הנתיבים והצמתים השכנים עבור הצומת עם המזהה הנתון
        [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /intersection/{id}/layout ומחזירה את המידע של הסכמה של הנתיבים והצמתים השכנים עבור הצומת עם המזהה הנתון
        int id = std::stoi(req.matches[1]);// מחלצת את מזהה הצומת מהנתיב של הבקשה
        const auto& schema = get_cached_lane_schema(id);// שולף את הסכמה של הנתיבים עבור הצומת עם המזהה הנתון מהמטמון

        json directions = json::array();// יוצרת מערך JSON ריק וממלאת אותו עם הכיוונים של הנתיבים עבור הצומת עם המזהה הנתון
        for (const auto& row : schema) directions.push_back(row.direction);// מוסיפה את הכיוון של הנתיב למערך JSON

        json neighbors = json::array();// יוצרת מערך JSON ריק וממלאת אותו עם המידע של הצמתים השכנים עבור הצומת עם המזהה הנתון
        // Prefer the full topology (includes direction_from) for the React layout contract.
        auto it_full = neighbor_topology_full_.find(id);// מחפש את המידע של הצמתים השכנים עבור הצומת עם המזהה הנתון ב-neighbor_topology_full_
        if (it_full != neighbor_topology_full_.end()) {// אם המידע של הצמתים השכנים עבור הצומת עם המזהה הנתון נמצא ב-neighbor_topology_full_, ממלא את המערך JSON עם המידע של הצמתים השכנים
            for (const auto& n : it_full->second) {// עבור כל צומת שכן במידע של הצמתים השכנים עבור הצומת עם המזהה הנתון, מוסיפה את המידע של הצומת השכן למערך JSON
                json obj = {
                    {"adjacent_intersection_id", n.adjacent_intersection_id},
                    {"direction_from", n.direction_from.empty()
                        ? json(nullptr) : json(n.direction_from)},
                };
                if (n.distance_m > 0) obj["distance_m"] = n.distance_m;// אם המרחק בין הצומת לבין הצומת השכן גדול מ-0, מוסיפה את המרחק למערך JSON
                neighbors.push_back(std::move(obj));
            }
        } else {
            // Fallback: plain IDs without direction info.
            auto it = neighbor_topology_.find(id);// מחפש את המידע של הצמתים השכנים עבור הצומת עם המזהה הנתון ב-neighbor_topology_
            if (it != neighbor_topology_.end()) {// אם המידע של הצמתים השכנים עבור הצומת עם המזהה הנתון נמצא ב-neighbor_topology_, ממלא את המערך JSON עם המידע של הצמתים השכנים
                for (int nid : it->second) {// עבור כל מזהה של צומת שכן במידע של הצמתים השכנים עבור הצומת עם המזהה הנתון, מוסיפה את המידע של הצומת השכן למערך JSON
                    neighbors.push_back({// מוסיפה את המידע של הצומת השכן למערך JSON
                        {"adjacent_intersection_id", nid},
                        {"direction_from", nullptr},
                    });
                }
            }
        }
// יוצרת אובייקט JSON עם המידע של הסכמה של הנתיבים והצמתים השכנים עבור הצומת עם המזהה הנתון
        res.set_content(json({
            {"intersection_id", id},
            {"num_lanes",       static_cast<int>(schema.size())},
            {"lane_directions", directions},
            {"neighbors",       neighbors},
        }).dump(), "application/json");
    });

    // ── POST /intersection/{id}/simulate-emergency ─
    svr_->Post(R"(/intersection/(\d+)/simulate-emergency)",// מטפלת בבקשה POST לנתיב /intersection/{id}/simulate-emergency ומדמה חירום עבור הצומת עם המזהה הנתון
        [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה POST לנתיב /intersection/{id}/simulate-emergency ומדמה חירום עבור הצומת עם המזהה הנתון
        int id = std::stoi(req.matches[1]);// מחלצת את מזהה הצומת מהנתיב של הבקשה
        json body;
        try { body = json::parse(req.body); }// מנסה לפרסר את גוף הבקשה ל-JSON
        catch (...) {// אם הפרסר נכשל, מחזירה סטטוס 400 Bad Request ומסר שגיאה
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה אם הפרסר נכשל
            return;
        }

        const int lane_id  = body.value("lane_id", 0);// מחלצת את מזהה הנתיב מהגוף של הבקשה, אם לא קיים מחזירה 0
        const std::string vehicle_id = body.value("vehicle_id", std::string("dashboard-sim"));// מחלצת את מזהה הרכב מהגוף של הבקשה, אם לא קיים מחזירה "dashboard-sim"
        const double now_s = std::chrono::duration<double>(// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
            std::chrono::system_clock::now().time_since_epoch()).count();// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת

        json signal = {// יוצרת אובייקט JSON עם המידע של האות חירום עבור הצומת עם המזהה הנתון
            {"active",     true},
            {"lane_id",    lane_id},
            {"vehicle_id", vehicle_id},
            {"timestamp",  now_s},
            {"signature",  nullptr},
            {"simulated",  true},
        };
        latch_emergency(id, signal);// מוסיפה את האות חירום למאגר המצב עבור הצומת עם המזהה הנתון
// מחשבת את מזהה הפאזה המתאים לפי מזהה הנתיב של האות חירום (אם הנתיב זוגי, הפאזה היא 0, אחרת הפאזה היא 1)
        const int phase_id = (lane_id % 2 == 0) ? 0 : 1;
        json action = {
            {"action",          std::string("Phase") + std::to_string(phase_id)},
            {"reason",          "emergency_preempt"},
            {"intersection_id", id},
            {"phase_id",        phase_id},
        };
        {
            std::lock_guard<std::mutex> lk(store_mutex_);// נועל את mutex כדי להבטיח גישה בטוחה למאגר המצב
            action_store_[id]            = action.dump();// מאחסן את הפעולה שנבחרה עבור הצומת עם המזהה הנתון ב-action_store_ עם המזהה של הצומת כמפתח
            action_source_store_[id]     = "emergency_preempt";// מאחסן את מקור הפעולה ב-action_source_store_ עם המזהה של הצומת כמפתח
            action_updated_at_store_[id] = now_s;// מאחסן את הזמן שבו הפעולה עודכנה ב-action_updated_at_store_ עם המזהה של הצומת כמפתח
        }// שולח אירוע לכל המנויים עם מזהה הצומת, סוג האירוע והפעולה שנבחרה עבור הצומת עם המזהה הנתון
        broadcast_event(id, "action_updated", json({
            {"action",  action},
            {"source",  "emergency_preempt"},
            {"signal",  signal},
        }));
        res.set_content(action.dump(), "application/json");
    });

    // ── POST /intersection/{id}/clear-emergency ─────────────────────────────
    svr_->Post(R"(/intersection/(\d+)/clear-emergency)",// מטפלת בבקשה POST לנתיב /intersection/{id}/clear-emergency ומסירה את האות חירום עבור הצומת עם המזהה הנתון
        [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה POST לנתיב /intersection/{id}/clear-emergency ומסירה את האות חירום עבור הצומת עם המזהה הנתון
        int id = std::stoi(req.matches[1]);// מחלצת את מזהה הצומת מהנתיב של הבקשה
        clear_emergency_latch(id);// מסירה את האות חירום מהמאגר עבור הצומת עם המזהה הנתון

        // Reset action stickiness so the next POST /state recomputes a fallback.
        const double now_s = std::chrono::duration<double>(// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
            std::chrono::system_clock::now().time_since_epoch()).count();// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
        json action = {
            {"action",          "Phase0"},
            {"reason",          "emergency_cleared"},
            {"intersection_id", id},
            {"phase_id",        0},
        };
        {
            std::lock_guard<std::mutex> lk(store_mutex_);// נועל את mutex כדי להבטיח גישה בטוחה למאגר המצב
            action_store_[id]            = action.dump();// מאחסן את הפעולה שנבחרה עבור הצומת עם המזהה הנתון ב-action_store_ עם המזהה של הצומת כמפתח
            action_source_store_[id]     = "server_fallback";// מאחסן את מקור הפעולה ב-action_source_store_ עם המזהה של הצומת כמפתח
            action_updated_at_store_[id] = now_s;// מאחסן את הזמן שבו הפעולה עודכנה ב-action_updated_at_store_ עם המזהה של הצומת כמפתח
        }
        broadcast_event(id, "action_updated", json({// שולח אירוע לכל המנויים עם מזהה הצומת, סוג האירוע והפעולה שנבחרה עבור הצומת עם המזהה הנתון
            {"action",  action},
            {"source",  "emergency_cleared"},
        }));
        res.set_content(action.dump(), "application/json");
    });

    // ── POST /emergency/locate ───────────────────────────────────────────────
    // מקבל קואורדינטות GPS של רכב חירום, מחשב את הצומת הקרובה ביותר בטווח 500 מטר,
    // מחשב את כיוון ההגעה של הרכב (N/S/E/W...) ומפעיל חירום על הנתיב המתאים.
    // Body: { vehicle_id, latitude, longitude, timestamp, signature }
    svr_->Post("/emergency/locate", [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה POST לנתיב /emergency/locate — מקבלת קואורדינטות GPS ומפעילה חירום על הנתיב הקרוב
        json body;
        try { body = json::parse(req.body); }// מנסה לפרסר את גוף הבקשה ל-JSON
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail","Invalid JSON"}}).dump(), "application/json");// מחזירה שגיאה אם ה-JSON לא תקין
            return;
        }

        // בדיקת שדות חובה
        if (!body.contains("vehicle_id") || !body.contains("latitude") ||// בודקת שיש vehicle_id
            !body.contains("longitude") || !body.contains("timestamp") ||// בודקת שיש longitude ו-timestamp
            !body.contains("signature")) {// בודקת שיש חתימת HMAC
            res.status = 422;
            res.set_content(json({{"detail","vehicle_id, latitude, longitude, timestamp, signature are required"}}).dump(), "application/json");// מחזירה שגיאה אם שדות חסרים
            return;
        }

        const std::string vehicle_id  = body["vehicle_id"].get<std::string>();// מחלצת מזהה הרכב
        const double      vehicle_lat = body["latitude"].get<double>();// מחלצת קו הרוחב של הרכב
        const double      vehicle_lon = body["longitude"].get<double>();// מחלצת קו האורך של הרכב
        const double      ts          = body["timestamp"].get<double>();// מחלצת חותמת זמן Unix
        const std::string signature   = body["signature"].get<std::string>();// מחלצת חתימת HMAC

        // אימות HMAC — אותו פורמט כמו validate_emergency_signal
        {
            std::lock_guard<std::mutex> lk(emergency_mutex_);// נועל mutex לגישה בטוחה למפתחות
            auto key_it = emergency_keys_.find(vehicle_id);// מחפש מפתח HMAC לפי מזהה הרכב
            if (key_it == emergency_keys_.end())// אם לא נמצא מפתח ספציפי לרכב
                key_it = emergency_keys_.find("*");// מנסה מפתח wildcard "*"
            if (key_it == emergency_keys_.end()) {// אם לא נמצא כלל
                res.status = 401;
                res.set_content(json({{"detail","Unknown vehicle_id"}}).dump(), "application/json");// מחזירה 401 — רכב לא מוכר
                return;
            }
            // פורמט payload: "{vehicle_id}|{lat:.7f}|{lon:.7f}|{ts:.3f}"
            char payload_buf[256];// מאגר לבניית מחרוזת ה-payload
            std::snprintf(payload_buf, sizeof(payload_buf), "%s|%.7f|%.7f|%.3f",// בונה את ה-payload בפורמט המוסכם
                          vehicle_id.c_str(), vehicle_lat, vehicle_lon, ts);
            const std::string expected = to_hex(hmac_sha256_bytes(key_it->second, payload_buf));// מחשב את החתימה הצפויה
            if (to_lower_copy(expected) != to_lower_copy(signature)) {// משווה חתימות (case-insensitive)
                res.status = 401;
                res.set_content(json({{"detail","Invalid signature"}}).dump(), "application/json");// מחזירה 401 — חתימה לא תקינה
                return;
            }
            // בדיקת clock skew
            const double now_s = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();// קוראת את השעה הנוכחית
            if (std::abs(now_s - ts) > emergency_max_clock_skew_sec_) {// בודקת שהחותמת לא ישנה מדי
                res.status = 401;
                res.set_content(json({{"detail","Timestamp out of allowed range"}}).dump(), "application/json");// מחזירה 401 — clock skew גדול מדי
                return;
            }
        }

        // חישוב Haversine — מרחק בין שתי נקודות GPS במטרים
        auto haversine_m = [](double lat1, double lon1, double lat2, double lon2) -> double {// פונקציית lambda לחישוב מרחק בין שתי נקודות GPS
            constexpr double R    = 6371000.0;// רדיוס כדור הארץ במטרים
            constexpr double kPi  = 3.14159265358979323846;// הגדרת PI
            const double phi1 = lat1 * kPi / 180.0;// ממיר lat1 מדרגות לרדיאנים
            const double phi2 = lat2 * kPi / 180.0;// ממיר lat2 מדרגות לרדיאנים
            const double dphi = (lat2 - lat1) * kPi / 180.0;// הפרש קוי הרוחב ברדיאנים
            const double dlam = (lon2 - lon1) * kPi / 180.0;// הפרש קוי האורך ברדיאנים
            const double a = std::sin(dphi/2)*std::sin(dphi/2) +// נוסחת Haversine: חלק א'
                             std::cos(phi1)*std::cos(phi2)*std::sin(dlam/2)*std::sin(dlam/2);// נוסחת Haversine: חלק ב'
            return R * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));// מחשב ומחזיר את המרחק במטרים
        };

        // זווית ממרכז הצומת לרכב (מצפון, בכיוון השעון)
        auto bearing_deg = [](double lat1, double lon1, double lat2, double lon2) -> double {// פונקציית lambda לחישוב כיוון (bearing) בין שתי נקודות
            constexpr double kPi = 3.14159265358979323846;// הגדרת PI
            const double phi1 = lat1 * kPi / 180.0;// ממיר lat1 לרדיאנים
            const double phi2 = lat2 * kPi / 180.0;// ממיר lat2 לרדיאנים
            const double dlam = (lon2 - lon1) * kPi / 180.0;// הפרש קוי האורך ברדיאנים
            const double y    = std::sin(dlam) * std::cos(phi2);// רכיב Y של הכיוון
            const double x    = std::cos(phi1)*std::sin(phi2) - std::sin(phi1)*std::cos(phi2)*std::cos(dlam);// רכיב X של הכיוון
            return std::fmod(std::atan2(y, x) * 180.0 / kPi + 360.0, 360.0);// מחזיר זווית בדרגות (0–360, מצפון בכיוון השעון)
        };

        // המרת זווית לשם כיוון (8 כיוונים — מתאים ל-direction בטבלת intersection_lanes)
        auto bearing_to_dir = [](double b) -> std::string {// ממירה זווית (0–360) לשם כיוון בפורמט intersection_lanes
            if (b <  22.5 || b >= 337.5) return "N";// צפון
            if (b <  67.5)               return "NE";// צפון-מזרח
            if (b < 112.5)               return "E";// מזרח
            if (b < 157.5)               return "SE";// דרום-מזרח
            if (b < 202.5)               return "S";// דרום
            if (b < 247.5)               return "SW";// דרום-מערב
            if (b < 292.5)               return "W";// מערב
            return "NW";// צפון-מערב
        };

        // חיפוש הצומת הקרובה ביותר בתוך הטווח המוגדר
        constexpr double MAX_RANGE_M = 500.0; // טווח מקסימלי: 500 מטר — רכב מחוץ לטווח לא יפעיל חירום
        int    best_id   = -1;// מזהה הצומת הקרובה ביותר (−1 = לא נמצאה)
        double best_dist = MAX_RANGE_M + 1.0;// מרחק הצומת הקרובה ביותר (מאותחל מעל הטווח)
        double best_bear = 0.0;// זווית הגעה לצומת הקרובה ביותר

        for (const auto& [iid, latlon] : intersection_locations_) {// עוברת על כל הצמתות שנטענו
            const double dist = haversine_m(latlon.first, latlon.second, vehicle_lat, vehicle_lon);// מחשבת מרחק מהצומת לרכב
            if (dist < best_dist) {// אם הצומת הנוכחית קרובה יותר מהקודמת
                best_dist = dist;// מעדכנת את המרחק הקרוב ביותר
                best_id   = iid;// מעדכנת את מזהה הצומת הקרובה ביותר
                best_bear = bearing_deg(latlon.first, latlon.second, vehicle_lat, vehicle_lon);// מחשבת את כיוון ההגעה
            }
        }

        if (best_id < 0) {// אם לא נמצאה צומת בטווח
            res.status = 404;
            res.set_content(json({
                {"detail",     "No intersection found within range"},// הסבר — לא נמצאה צומת בטווח
                {"max_range_m", MAX_RANGE_M},// מציין את הטווח המקסימלי
            }).dump(), "application/json");
            return;
        }

        // מיפוי כיוון → lane_id לפי סכמת ה-DB של הצומת שנמצאה
        const std::string target_dir = bearing_to_dir(best_bear);// ממירה את הזווית לשם כיוון (N/S/E/W...)
        const auto& schema = get_cached_lane_schema(best_id);// שולפת את סכמת הנתיבים של הצומת שנמצאה
        int target_lane_id = 0; // ברירת מחדל אם לא נמצא נתיב מתאים
        for (const auto& row : schema) {// עוברת על כל הנתיבים בסכמה
            if (row.direction == target_dir) {// מוצאת נתיב שכיוונו תואם לכיוון ההגעה
                target_lane_id = row.camera_index; // 0-based (==Python lane_id) — camera_index הוא ה-lane_id של Python
                break;// עוצרת — נמצא הנתיב המתאים
            }
        }

        // הפעלת חירום על הנתיב שנקבע
        const double now_s = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();// קוראת את השעה הנוכחית לחותמת
        json signal = {// בונה אובייקט אות חירום ל-latch
            {"active",        true},// האות פעיל
            {"lane_id",       target_lane_id},// הנתיב שנבחר לפי כיוון GPS
            {"vehicle_id",    vehicle_id},// מזהה הרכב
            {"timestamp",     ts},// חותמת הזמן המקורית מהרכב
            {"signature",     signature},// חתימת HMAC המקורית
            {"locate_source", true},          // מסמן שהמקור הוא GPS locate (ולא simulate-emergency)
            {"distance_m",    std::round(best_dist)},// מרחק הרכב מהצומת במטרים
            {"bearing_deg",   std::round(best_bear)},// זווית ההגעה בדרגות
            {"direction",     target_dir},// שם הכיוון (N/S/E/W...)
        };
        latch_emergency(best_id, signal);// מפעילה את נעילת החירום על הצומת שנמצאה

        const int phase_id = (target_lane_id % 2 == 0) ? 0 : 1;// מחשבת פאזה: נתיב זוגי=Phase0, אי-זוגי=Phase1
        json action = {// בונה אובייקט פעולה לשמירה ושידור
            {"action",          std::string("Phase") + std::to_string(phase_id)},// שם הפאזה
            {"reason",          "emergency_preempt_gps"},// סיבה — חירום GPS
            {"intersection_id", best_id},// מזהה הצומת שנמצאה
            {"phase_id",        phase_id},// מזהה הפאזה
        };
        {
            std::lock_guard<std::mutex> lk(store_mutex_);// נועל mutex לגישה בטוחה למאגרי הפעולות
            action_store_[best_id]            = action.dump();// שומר את הפעולה
            action_source_store_[best_id]     = "emergency_preempt";// מסמן שהמקור הוא חירום
            action_updated_at_store_[best_id] = now_s;// מעדכן את זמן העדכון האחרון
        }
        broadcast_event(best_id, "action_updated", json({// שולחת אירוע WebSocket לכל הלקוחות המחוברים
            {"action", action},// הפעולה שנבחרה
            {"source", "emergency_preempt_gps"},// המקור — GPS
            {"signal", signal},// אות החירום המלא
        }));

        std::cout << "[GPS-Emergency] vehicle=" << vehicle_id// מדפיסה לוג: מזהה הרכב
                  << " lat=" << vehicle_lat << " lon=" << vehicle_lon// מדפיסה קואורדינטות הרכב
                  << " → intersection=" << best_id// מדפיסה את הצומת שנמצאה
                  << " direction=" << target_dir// מדפיסה את הכיוון שזוהה
                  << " lane=" << target_lane_id// מדפיסה את הנתיב שנפתח
                  << " dist=" << static_cast<int>(best_dist) << "m\n";// מדפיסה את המרחק במטרים

        res.set_content(json({// מחזירה תשובת JSON ללקוח
            {"status",          "emergency_triggered"},// סטטוס ההפעלה
            {"intersection_id", best_id},// מזהה הצומת שנמצאה
            {"lane_id",         target_lane_id},// הנתיב שנפתח
            {"direction",       target_dir},// הכיוון שזוהה
            {"distance_m",      static_cast<int>(best_dist)},// מרחק הרכב מהצומת
            {"bearing_deg",     static_cast<int>(best_bear)},// זווית ההגעה
            {"action",          action},// הפעולה שהופעלה
        }).dump(), "application/json");
    });
    svr_->Get("/admin/auth/verify", [](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה GET לנתיב /admin/auth/verify ומחזירה את המידע של המשתמש המורשה בפורמט JSON
        // pre_routing_handler already validated the token if we reach here.
        const auto token = jwt_auth::extract_bearer_token(req.get_header_value("Authorization"));// מחלצת את הטוקן מהכותרת Authorization של הבקשה
        std::string username;// יוצרת משתנה מחרוזת בשם username
        std::string role = "regular_admin";// יוצרת משתנה מחרוזת בשם role ומאתחלת אותו ל-"regular_admin"
        if (token) jwt_auth::validate_admin_token_with_role(*token, username, role);// אם הטוקן קיים, מאמתת את הטוקן ומחלצת את שם המשתמש ואת התפקיד של המשתמש
        res.set_content(json({// יוצרת אובייקט JSON עם המידע של המשתמש המורשה
            {"status",   "ok"},
            {"username", username},
            {"role",     role},
            {"message",  "Token is valid"},
        }).dump(), "application/json");
    });

    // ── POST /admin/manual-control ──────────────────────────────────────────
    svr_->Post("/admin/manual-control", [this](const httplib::Request& req, httplib::Response& res) {// מטפלת בבקשה POST לנתיב /admin/manual-control ומבצעת שליטה ידנית על הצומת
        json body;
        try { body = json::parse(req.body); }// מנסה לפרסר את גוף הבקשה ל-JSON
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");// מחזירה סטטוס 400 Bad Request ומסר שגיאה אם הפרסר נכשל
            return;
        }
        if (!body.contains("intersection_id") || !body["intersection_id"].is_number_integer() ||
            !body.contains("phase_id")        || !body["phase_id"].is_number_integer()) {// אם הגוף של הבקשה אינו מכיל את המפתחות intersection_id ו-phase_id או שהם אינם מספרים שלמים, מחזירה סטטוס 422 Unprocessable Entity ומסר שגיאה
            res.status = 422;
            res.set_content(json({{"detail", "intersection_id and phase_id are required"}}).dump(),
                            "application/json");
            return;
        }// מחלצת את המזהה של הצומת ואת מזהה הפאזה מהגוף של הבקשה
        const int id       = body["intersection_id"].get<int>();
        const int phase_id = body["phase_id"].get<int>();
        const std::string reason = body.value("reason", std::string("manual_dashboard"));

        json action = {// יוצרת אובייקט JSON עם המידע של הפעולה שנבחרה עבור הצומת עם המזהה הנתון
            {"action",          std::string("Phase") + std::to_string(phase_id)},
            {"reason",          reason + " (manual)"},
            {"intersection_id", id},
            {"phase_id",        phase_id},
        };
        const double now_s = std::chrono::duration<double>(// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
            std::chrono::system_clock::now().time_since_epoch()).count();// מחזירה את הזמן הנוכחי בשניות מאז תחילת התקופה (epoch) של המערכת
        {// נועל את mutex כדי להבטיח גישה בטוחה למאגר המצב
            std::lock_guard<std::mutex> lk(store_mutex_);
            action_store_[id]            = action.dump();
            action_source_store_[id]     = "manual_dashboard";
            action_updated_at_store_[id] = now_s;
        }
        broadcast_event(id, "action_updated", json({// שולח אירוע לכל המנויים עם מזהה הצומת, סוג האירוע והפעולה שנבחרה עבור הצומת עם המזהה הנתון
            {"action", action},
            {"source", "manual_dashboard"},
        }));
        res.set_content(json({// יוצרת אובייקט JSON עם המידע של הפעולה שנבחרה עבור הצומת עם המזהה הנתון
            {"status",  "success"},
            {"message", "Manual control accepted"},
            {"action",  action},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id} ────────────────────────────────────────
    svr_->Get(R"(/admin/intersection/(\d+))",// מטפלת בבקשה GET לנתיב /admin/intersection/{id} ומחזירה את המידע של הצומת עם המזהה הנתון בפורמט JSON
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);

        json intersection_obj = nullptr;
        for (const auto& r : db_fetch_intersections()) {
            if (r.id == id) {
                intersection_obj = {
                    {"id",   r.id},
                    {"code", r.code},
                    {"name", r.name},
                    {"city", r.city},
                };
                break;
            }
        }
        if (intersection_obj.is_null()) {
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");
            return;
        }

        const auto& lanes = get_cached_lane_schema(id);
        json lanes_arr = json::array();
        for (const auto& l : lanes) {
            lanes_arr.push_back({
                {"lane_id",      l.lane_id},
                {"camera_index", l.camera_index},
                {"direction",    l.direction},
                {"description",  l.description.empty() ? json(nullptr) : json(l.description)},
            });
        }

        json neighbors_arr = json::array();
        auto nit = neighbor_topology_.find(id);
        if (nit != neighbor_topology_.end()) {
            for (int n : nit->second) neighbors_arr.push_back(n);
        }

        json state_obj  = nullptr;
        json action_obj = nullptr;
        {
            std::lock_guard<std::mutex> lk(store_mutex_);
            auto si = state_store_.find(id);
            auto ai = action_store_.find(id);
            if (si != state_store_.end()) {
                try { state_obj = json::parse(si->second); } catch (...) {}
            }
            if (ai != action_store_.end()) {
                try { action_obj = json::parse(ai->second); } catch (...) {}
            }
        }

        res.set_content(json({
            {"status",         "success"},
            {"intersection",   intersection_obj},
            {"neighbors",      neighbors_arr},
            {"lanes",          lanes_arr},
            {"current_state",  state_obj},
            {"current_action", action_obj},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id}/neighbors ──────────────────────────────
    // מטפלת בבקשה GET לנתיב /admin/intersection/{id}/neighbors ומחזירה את המידע של הצמתים השכנים עבור הצומת עם המזהה הנתון בפורמט JSON
    svr_->Get(R"(/admin/intersection/(\d+)/neighbors)",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        json neighbors = json::array();
        auto it = neighbor_topology_.find(id);
        if (it != neighbor_topology_.end()) {
            std::lock_guard<std::mutex> lk(store_mutex_);
            for (int nid : it->second) {
                json state = nullptr, action = nullptr;
                auto si = state_store_.find(nid);
                auto ai = action_store_.find(nid);
                if (si != state_store_.end()) { try { state  = json::parse(si->second); } catch (...) {} }
                if (ai != action_store_.end()) { try { action = json::parse(ai->second); } catch (...) {} }
                neighbors.push_back({{"id", nid}, {"state", state}, {"action", action}});
            }
        }
        res.set_content(json({
            {"status",          "success"},
            {"intersection_id", id},
            {"neighbor_count",  static_cast<int>(neighbors.size())},
            {"neighbors",       neighbors},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id}/phase-options ──────────────────────────
    // מטפלת בבקשה GET לנתיב /admin/intersection/{id}/phase-options ומחזירה את המידע של אפשרויות הפאזות עבור הצומת עם המזהה הנתון בפורמט JSON
    svr_->Get(R"(/admin/intersection/(\d+)/phase-options)",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        const auto& lanes = get_cached_lane_schema(id);

        // Even-lane phase (Phase0) and odd-lane phase (Phase1) — simple
        // 2-phase model matching the legacy Python heuristic. Real phase
        // computation lives in PhaseConfig / ConflictConfig on the controller
        // side; the dashboard only needs labels to populate its dropdown.
        json options = json::array();
        json even, odd;
        for (const auto& l : lanes) {
            if (l.lane_id % 2 == 0) even.push_back(l.lane_id);
            else                    odd.push_back(l.lane_id);
        }
        options.push_back({
            {"phase_id", 0}, {"label", "Phase 0 (even lanes)"}, {"green_lanes", even},
        });
        options.push_back({
            {"phase_id", 1}, {"label", "Phase 1 (odd lanes)"},  {"green_lanes", odd},
        });
        res.set_content(json({
            {"status",          "success"},
            {"intersection_id", id},
            {"count",           static_cast<int>(options.size())},
            {"phase_options",   options},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id}/lanes ─────────────────────────────────
    // מטפלת בבקשה GET לנתיב /admin/intersection/{id}/lanes ומחזירה את המידע של הנתיבים עבור הצומת עם המזהה הנתון בפורמט JSON
    svr_->Get(R"(/admin/intersection/(\d+)/lanes)",
        [this](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        const auto& lanes = get_cached_lane_schema(id);
        json arr = json::array();
        for (const auto& l : lanes) {
            arr.push_back({
                {"lane_id",      l.lane_id},
                {"camera_index", l.camera_index},
                {"direction",    l.direction},
                {"description",  l.description.empty() ? json(nullptr) : json(l.description)},
            });
        }
        res.set_content(json({
            {"status",          "success"},
            {"intersection_id", id},
            {"count",           static_cast<int>(arr.size())},
            {"lanes",           arr},
        }).dump(), "application/json");
    });

    // ── GET /admin/intersection/{id}/conflicts ──────────────────────────────
    // מטפלת בבקשה GET לנתיב /admin/intersection/{id}/conflicts ומחזירה את המידע של הקונפליקטים עבור הצומת עם המזהה הנתון בפורמט JSON
    svr_->Get(R"(/admin/intersection/(\d+)/conflicts)",
        [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        auto rows = db_fetch_conflicts(id);
        json arr = json::array();
        for (const auto& r : rows) {
            arr.push_back({
                {"conflict_id",   r.conflict_id},
                {"lane_id_1",     r.lane_id_1},
                {"lane_id_2",     r.lane_id_2},
                {"conflict_type", r.conflict_type},
                {"created_at",    r.created_at.empty() ? json(nullptr) : json(r.created_at)},
            });
        }
        res.set_content(json({
            {"status",          "success"},
            {"intersection_id", id},
            {"count",           static_cast<int>(arr.size())},
            {"conflicts",       arr},
        }).dump(), "application/json");
    });

    // ── helper lambdas reused by lane/conflict write routes ────────────────
    auto build_lane_array = [](int id) {
        auto rows = db_fetch_lanes(id);
        json arr = json::array();
        for (const auto& l : rows) {
            arr.push_back({
                {"lane_id",      l.lane_id},
                {"camera_index", l.camera_index},
                {"direction",    l.direction},
                {"description",  l.description.empty() ? json(nullptr) : json(l.description)},
            });
        }
        return arr;
    };
    auto build_conflict_array = [](int id) {
        auto rows = db_fetch_conflicts(id);
        json arr = json::array();
        for (const auto& r : rows) {
            arr.push_back({
                {"conflict_id",   r.conflict_id},
                {"intersection_id", id},
                {"lane_id_1",     r.lane_id_1},
                {"lane_id_2",     r.lane_id_2},
                {"conflict_type", r.conflict_type},
                {"created_at",    r.created_at.empty() ? json(nullptr) : json(r.created_at)},
            });
        }
        return arr;
    };
    auto normalize_direction = [](std::string s) -> std::string {
        // Trim + uppercase first letter; accept "N"/"S"/"E"/"W" or names.
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (s == "NORTH") s = "N";
        else if (s == "SOUTH") s = "S";
        else if (s == "EAST")  s = "E";
        else if (s == "WEST")  s = "W";
        return s;
    };

    // ── POST /admin/intersection/{id}/lanes ────────────────────────────────
    svr_->Post(R"(/admin/intersection/(\d+)/lanes)",
        [this, build_lane_array, normalize_direction]
        (const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        if (!db_intersection_exists(id)) {
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");
            return;
        }
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }
        if (!body.contains("camera_index") || !body["camera_index"].is_number_integer() ||
            !body.contains("direction")    || !body["direction"].is_string()) {
            res.status = 422;
            res.set_content(json({{"detail", "camera_index (int) and direction (string) are required"}}).dump(),
                            "application/json");
            return;
        }
        const int cam   = body["camera_index"].get<int>();
        const std::string dir = normalize_direction(body["direction"].get<std::string>());
        std::optional<std::string> desc;
        if (body.contains("description") && body["description"].is_string()) {
            desc = body["description"].get<std::string>();
        }

        std::string err;
        if (!db_insert_lane(id, cam, dir, desc, err)) {
            res.status = 400;
            res.set_content(json({{"detail", std::string("Failed creating lane: ") + err}}).dump(),
                            "application/json");
            return;
        }
        invalidate_lane_cache(id);
        res.set_content(json({
            {"status",  "success"},
            {"message", "Lane created successfully"},
            {"lanes",   build_lane_array(id)},
        }).dump(), "application/json");
    });

    // ── PUT /admin/intersection/{id}/lanes/{lane_id} ───────────────────────
    svr_->Put(R"(/admin/intersection/(\d+)/lanes/(\d+))",
        [this, build_lane_array, normalize_direction]
        (const httplib::Request& req, httplib::Response& res) {
        int id      = std::stoi(req.matches[1]);
        int lane_id = std::stoi(req.matches[2]);

        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }

        std::optional<std::string> dir;
        std::optional<std::string> desc;
        if (body.contains("direction") && body["direction"].is_string()) {
            dir = normalize_direction(body["direction"].get<std::string>());
        }
        if (body.contains("description") && body["description"].is_string()) {
            desc = body["description"].get<std::string>();
        }
        if (!dir && !desc) {
            res.status = 400;
            res.set_content(json({{"detail", "No fields to update"}}).dump(), "application/json");
            return;
        }

        bool found = false;
        std::string err;
        if (!db_update_lane(id, lane_id, dir, desc, found, err)) {
            res.status = 400;
            res.set_content(json({{"detail", std::string("Failed updating lane: ") + err}}).dump(),
                            "application/json");
            return;
        }
        if (!found) {
            res.status = 404;
            res.set_content(json({{"detail", "Lane not found"}}).dump(), "application/json");
            return;
        }
        invalidate_lane_cache(id);
        res.set_content(json({
            {"status",  "success"},
            {"message", "Lane updated successfully"},
            {"lanes",   build_lane_array(id)},
        }).dump(), "application/json");
    });

    // ── DELETE /admin/intersection/{id}/lanes/{lane_id} ────────────────────
    svr_->Delete(R"(/admin/intersection/(\d+)/lanes/(\d+))",
        [this, build_lane_array](const httplib::Request& req, httplib::Response& res) {
        int id      = std::stoi(req.matches[1]);
        int lane_id = std::stoi(req.matches[2]);

        bool found = false;
        std::string err;
        if (!db_delete_lane(id, lane_id, found, err)) {
            res.status = 400;
            res.set_content(json({{"detail", std::string("Failed deleting lane: ") + err}}).dump(),
                            "application/json");
            return;
        }
        if (!found) {
            res.status = 404;
            res.set_content(json({{"detail", "Lane not found"}}).dump(), "application/json");
            return;
        }
        invalidate_lane_cache(id);
        res.set_content(json({
            {"status",  "success"},
            {"message", "Lane deleted successfully"},
            {"lanes",   build_lane_array(id)},
        }).dump(), "application/json");
    });

    // ── POST /admin/intersection/{id}/conflicts ────────────────────────────
    svr_->Post(R"(/admin/intersection/(\d+)/conflicts)",
        [build_conflict_array](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        if (!db_intersection_exists(id)) {
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");
            return;
        }
        json body;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(json({{"detail", "Invalid JSON"}}).dump(), "application/json");
            return;
        }
        if (!body.contains("lane_id_1") || !body["lane_id_1"].is_number_integer() ||
            !body.contains("lane_id_2") || !body["lane_id_2"].is_number_integer()) {
            res.status = 422;
            res.set_content(json({{"detail", "lane_id_1 and lane_id_2 are required"}}).dump(),
                            "application/json");
            return;
        }
        const int a = body["lane_id_1"].get<int>();
        const int b = body["lane_id_2"].get<int>();
        if (a == b) {
            res.status = 400;
            res.set_content(json({{"detail", "lane_id_1 and lane_id_2 must differ"}}).dump(),
                            "application/json");
            return;
        }
        const int l1 = std::min(a, b);
        const int l2 = std::max(a, b);
        const std::string ctype = (body.contains("conflict_type") && body["conflict_type"].is_string())
            ? body["conflict_type"].get<std::string>() : std::string("crossing");

        bool missing = false, dup = false;
        std::string err;
        if (!db_insert_conflict(id, l1, l2, ctype, missing, dup, err)) {
            if (missing) {
                res.status = 404;
                res.set_content(json({{"detail", "Lane not found in intersection"}}).dump(),
                                "application/json");
            } else if (dup) {
                res.status = 400;
                res.set_content(json({{"detail", "This conflict pair already exists"}}).dump(),
                                "application/json");
            } else {
                res.status = 400;
                res.set_content(json({{"detail", std::string("Failed creating conflict: ") + err}}).dump(),
                                "application/json");
            }
            return;
        }
        json arr = build_conflict_array(id);
        res.set_content(json({
            {"status",          "success"},
            {"message",         "Lane conflict created successfully"},
            {"intersection_id", id},
            {"count",           static_cast<int>(arr.size())},
            {"conflicts",       arr},
        }).dump(), "application/json");
    });

    // ── DELETE /admin/intersection/{id}/conflicts/{conflict_id} ────────────
    svr_->Delete(R"(/admin/intersection/(\d+)/conflicts/(\d+))",
        [build_conflict_array](const httplib::Request& req, httplib::Response& res) {
        int id  = std::stoi(req.matches[1]);
        int cid = std::stoi(req.matches[2]);
        if (!db_intersection_exists(id)) {
            res.status = 404;
            res.set_content(json({{"detail", "Intersection not found"}}).dump(), "application/json");
            return;
        }
        bool found = false;
        std::string err;
        if (!db_delete_conflict(id, cid, found, err)) {
            res.status = 400;
            res.set_content(json({{"detail", std::string("Failed deleting conflict: ") + err}}).dump(),
                            "application/json");
            return;
        }
        if (!found) {
            res.status = 404;
            res.set_content(json({{"detail", "Conflict not found"}}).dump(), "application/json");
            return;
        }
        json arr = build_conflict_array(id);
        res.set_content(json({
            {"status",          "success"},
            {"message",         "Lane conflict deleted successfully"},
            {"intersection_id", id},
            {"count",           static_cast<int>(arr.size())},
            {"conflicts",       arr},
        }).dump(), "application/json");
    });
}

// ── get_cached_lane_schema ────────────────────────────────────────────────────
// Fetches lane schema from dbo.intersection_lanes on first call per intersection
// and caches it for the lifetime of the server.  Thread-safe.
const std::vector<LaneRow>& TrafficServer::get_cached_lane_schema(int intersection_id)
{// פונקציה שמחזירה את סכמת הנתיבים עבור צומת נתון — שולפת מה-DB בפעם הראשונה ומשמרת במטמון
    // Fast path: already cached — avoid DB query under the lock.
    {
        std::lock_guard<std::mutex> lk(lane_cache_mutex_);// נועלת את mutex של המטמון כדי למנוע גישה מקבילה
        if (lane_cache_loaded_.count(intersection_id)) {// בודקת אם הסכמה כבר נטענה למטמון עבור הצומת הנתון
            return lane_schema_cache_[intersection_id];// מחזירה את הסכמה מהמטמון ללא שאילתת DB
        }
    }

    // Slow path: query DB outside the lock so parallel threads don't queue up.
    auto rows = db_fetch_lanes(intersection_id);// שולפת את שורות הנתיבים מה-DB עבור הצומת הנתון

    std::lock_guard<std::mutex> lk(lane_cache_mutex_);// נועלת שוב את mutex לצורך כתיבה למטמון
    // Check again — another thread may have populated the cache while we queried.
    if (!lane_cache_loaded_.count(intersection_id)) {// בודקת שוב אם thread אחר לא מלא את המטמון בינתיים
        lane_schema_cache_[intersection_id] = std::move(rows);// מעביר את השורות שנשלפו למטמון
        lane_cache_loaded_[intersection_id] = true;// מסמנת שהמטמון נטען עבור הצומת הנתון
        std::cout << "[LaneCache] intersection_id=" << intersection_id
                  << " loaded " << lane_schema_cache_[intersection_id].size() << " lane(s) from DB.\n";// מדפיסה הודעת אתחול למטמון
    }

    return lane_schema_cache_[intersection_id];// מחזירה את הסכמה שנטענה למטמון
}

void TrafficServer::invalidate_lane_cache(int intersection_id)
{// פונקציה שמנקה את מטמון הנתיבים עבור צומת נתון — נקראת לאחר הוספה/עדכון/מחיקה של נתיב
    std::lock_guard<std::mutex> lk(lane_cache_mutex_);// נועלת את mutex של המטמון לצורך גישה בטוחה
    lane_schema_cache_.erase(intersection_id);// מוחקת את הסכמה של הצומת הנתון מהמטמון
    lane_cache_loaded_.erase(intersection_id);// מסמנת שהמטמון של הצומת הנתון אינו תקף יותר
}

void TrafficServer::load_neighbor_topology()
{// פונקציה שטוענת את טופולוגיית השכנים — נסיון ראשון מה-DB, אחר כך מ-JSON, ולבסוף ברירת מחדל
    auto db_topology      = db_fetch_neighbor_topology();// שולפת את הטופולוגיה הבסיסית (מזהים בלבד) מה-DB
    auto db_topology_full = db_fetch_neighbor_full_topology();// שולפת את הטופולוגיה המלאה (כולל כיוונים ומרחקים) מה-DB
    if (!db_topology.empty()) {// אם הטופולוגיה נמצאה ב-DB, משתמשת בה
        neighbor_topology_      = std::move(db_topology);// מעביר את הטופולוגיה הבסיסית למשתנה הפנימי
        neighbor_topology_full_ = std::move(db_topology_full);// מעביר את הטופולוגיה המלאה למשתנה הפנימי
        std::cout << "[TrafficServer] Loaded neighbor topology from DB ("
                  << neighbor_topology_.size() << " intersections)\n";// מדפיסה כמה צמתים נטענו מה-DB
        return;// יוצאת מהפונקציה — אין צורך לחפש בקבצי JSON
    }

    const std::vector<std::filesystem::path> candidates = {// רשימת נתיבים אפשריים לקובץ JSON של הטופולוגיה
        std::filesystem::path("python/server/neighbor_topology.json"),// נתיב ראשון לחיפוש
        std::filesystem::path("../python/server/neighbor_topology.json"),// נתיב שני לחיפוש
        std::filesystem::path("../../python/server/neighbor_topology.json"),// נתיב שלישי לחיפוש
        std::filesystem::path("neighbor_topology.json"),// נתיב רביעי לחיפוש
    };

    for (const auto& path : candidates) {// עוברת על כל הנתיבים האפשריים ומנסה לטעון מהם
        auto loaded = load_neighbor_topology_from_json_file(path);// מנסה לטעון טופולוגיה מקובץ JSON בנתיב הנוכחי
        if (loaded.has_value() && !loaded->empty()) {// אם הטעינה הצליחה והטופולוגיה אינה ריקה
            neighbor_topology_ = std::move(*loaded);// מעביר את הטופולוגיה שנטענה למשתנה הפנימי
            std::cout << "[TrafficServer] Loaded neighbor topology from " << path.string() << "\n";// מדפיסה מאיזה קובץ נטענה הטופולוגיה
            return;// יוצאת מהפונקציה — נמצא קובץ תקין
        }
    }

    neighbor_topology_ = DEFAULT_NEIGHBOR_TOPOLOGY;// משתמשת בטופולוגיית ברירת המחדל אם לא נמצא קובץ תקין
    std::cout << "[TrafficServer] Loaded fallback neighbor topology defaults\n";// מדפיסה שנטענה ברירת המחדל
}

// ---------------------------------------------------------------------------
// load_intersection_locations — GPS coordinates from DB (for /emergency/locate)
// ---------------------------------------------------------------------------
void TrafficServer::load_intersection_locations()// פונקציה שטוענת קואורדינטות GPS של כל הצמתות מה-DB לזיכרון
{
    const auto rows = db_fetch_all_intersection_locations();// שולפת את כל המיקומים מה-DB
    if (!rows.empty()) {// אם ה-DB החזיר שורות
        for (const auto& r : rows)// עוברת על כל שורה שהוחזרה
            intersection_locations_[r.id] = {r.latitude, r.longitude};// מאחסנת את הקואורדינטות במפה הפנימית לפי מזהה הצומת
        std::cout << "[TrafficServer] Loaded GPS locations for "
                  << intersection_locations_.size() << " intersections\n";// מדפיסה כמה צמתות נטענו
    } else {
        // ברירת מחדל — מתאים לנתוני ה-DB המובנים (database_schema_sqlserver.sql)
        intersection_locations_ = {
            {1, {32.0853000, 34.7817680}},// צומת 1: תל אביב מרכזי
            {2, {32.0773780, 34.7871110}},// צומת 2: צומת בית ספר
            {3, {32.0674200, 34.7635300}},// צומת 3: צומת תעשייה
            {4, {32.0000000, 34.8830000}},// צומת 4: שדה תעופה
        };
        std::cout << "[TrafficServer] Using default GPS locations (DB unavailable)\n";// מדפיסה שנטענו ערכי ברירת מחדל
    }
}

void TrafficServer::load_emergency_auth_config()
{// פונקציה שטוענת את מפתחות האימות של רכבי חירום — מנסה קבצי JSON לפי סדר עדיפות ואחר כך ברירת מחדל
    const std::vector<std::filesystem::path> candidates = {// רשימת נתיבים אפשריים לקובץ מפתחות החירום
        std::filesystem::path("python/server/emergency_keys.json"),// נתיב ראשון לחיפוש
        std::filesystem::path("../python/server/emergency_keys.json"),// נתיב שני לחיפוש
        std::filesystem::path("../../python/server/emergency_keys.json"),// נתיב שלישי לחיפוש
        std::filesystem::path("emergency_keys.json"),// נתיב רביעי לחיפוש
    };

    for (const auto& path : candidates) {// עוברת על כל הנתיבים האפשריים ומנסה לטעון מהם
        const auto loaded = load_emergency_auth_from_json_file(path);// מנסה לטעון את מפתחות החירום מקובץ JSON בנתיב הנוכחי
        if (!loaded.has_value()) {// אם הטעינה נכשלה (קובץ לא קיים או לא תקין)
            continue;// עובר לנתיב הבא
        }

        emergency_keys_ = loaded->vehicle_keys;// מאחסן את מפתחות הרכבים שנטענו
        emergency_max_clock_skew_sec_ = loaded->max_clock_skew_sec;// מאחסן את ערך ה-clock skew המקסימלי המותר
        std::cout << "[TrafficServer] Loaded emergency auth from " << loaded->source
                  << " (keys=" << emergency_keys_.size()
                  << ", skew=" << emergency_max_clock_skew_sec_ << "s)\n";// מדפיסה מאיזה קובץ נטענו המפתחות וכמה מפתחות יש
        return;// יוצאת מהפונקציה — נמצא קובץ תקין
    }

    emergency_keys_.clear();// מנקה את מפה המפתחות לפני הוספת ברירות מחדל
    emergency_keys_["AMB001"] = "demo-emergency-key-001";// מוסיפה מפתח דמו לאמבולנס AMB001
    emergency_keys_["POL001"] = "demo-emergency-key-002";// מוסיפה מפתח דמו לרכב משטרה POL001
    emergency_max_clock_skew_sec_ = 30.0;// מגדירה clock skew מקסימלי של 30 שניות כברירת מחדל
    std::cout << "[TrafficServer] Loaded fallback emergency auth defaults\n";// מדפיסה שנטענו ברירות המחדל
}

bool TrafficServer::validate_emergency_signal(const json& state_body, std::string& reason_out)
{// פונקציה שמאמתת את חוקיות אות חירום — בודקת חתימה HMAC-SHA256, clock skew, ומניעת replay attacks
    reason_out = "ok";// מאתחלת את סיבת השגיאה ל-"ok" (ללא שגיאה כרגע)

    if (!state_body.contains("emergency_signal") || state_body["emergency_signal"].is_null()) {// אם אין שדה emergency_signal בגוף הבקשה או שהוא null
        return true;// אין אות חירום — תקין, מחזירה true
    }
    if (!state_body["emergency_signal"].is_object()) {// אם שדה emergency_signal אינו אובייקט JSON
        reason_out = "invalid_emergency_object";// מגדירה סיבת שגיאה
        return false;// מחזירה false — אות לא תקין
    }

    const json& signal = state_body["emergency_signal"];// מחלצת את אובייקט האות מגוף הבקשה
    const bool active = signal.value("active", false);// קוראת את שדה active (ברירת מחדל false)
    if (!active) {// אם האות אינו פעיל
        return true;// אות לא פעיל — תקין, אין צורך לאמת
    }

    if (!signal.contains("lane_id") || !signal["lane_id"].is_number_integer()) {// בודקת שיש שדה lane_id ושהוא מספר שלם
        reason_out = "invalid_lane_id";// מגדירה סיבת שגיאה
        return false;// מחזירה false — חסר lane_id תקין
    }
    const int lane_id = signal["lane_id"].get<int>();// מחלצת את מזהה הנתיב
    if (lane_id < 0) {// בודקת שמזהה הנתיב אינו שלילי
        reason_out = "invalid_lane_id";// מגדירה סיבת שגיאה
        return false;// מחזירה false — מזהה נתיב שלילי אינו תקין
    }

    if (!signal.contains("vehicle_id") || !signal["vehicle_id"].is_string()) {// בודקת שיש שדה vehicle_id ושהוא מחרוזת
        reason_out = "missing_vehicle_id";// מגדירה סיבת שגיאה
        return false;// מחזירה false — חסר vehicle_id
    }
    const std::string vehicle_id = signal["vehicle_id"].get<std::string>();// מחלצת את מזהה הרכב
    if (vehicle_id.empty()) {// בודקת שמזהה הרכב אינו מחרוזת ריקה
        reason_out = "missing_vehicle_id";// מגדירה סיבת שגיאה
        return false;// מחזירה false — מזהה רכב ריק אינו תקין
    }

    if (!signal.contains("timestamp") || !signal["timestamp"].is_number()) {// בודקת שיש שדה timestamp ושהוא מספר
        reason_out = "missing_timestamp";// מגדירה סיבת שגיאה
        return false;// מחזירה false — חסר timestamp
    }
    const double timestamp = signal["timestamp"].get<double>();// מחלצת את חותמת הזמן של האות

    if (!signal.contains("signature") || !signal["signature"].is_string()) {// בודקת שיש שדה signature ושהוא מחרוזת
        reason_out = "missing_signature";// מגדירה סיבת שגיאה
        return false;// מחזירה false — חסרה חתימה
    }
    const std::string signature = signal["signature"].get<std::string>();// מחלצת את החתימה מהאות
    if (signature.empty()) {// בודקת שהחתימה אינה מחרוזת ריקה
        reason_out = "missing_signature";// מגדירה סיבת שגיאה
        return false;// מחזירה false — חתימה ריקה אינה תקינה
    }

    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();// מחשבת את הזמן הנוכחי בשניות
    if (std::fabs(now - timestamp) > emergency_max_clock_skew_sec_) {// בודקת שהפרש הזמן בין הזמן הנוכחי לחותמת האות אינו עולה על המותר
        reason_out = "timestamp_out_of_window";// מגדירה סיבת שגיאה
        return false;// מחזירה false — חותמת הזמן מחוץ לחלון המותר (clock skew)
    }

    std::string secret;// משתנה לאחסון מפתח הסוד של הרכב
    {
        std::lock_guard<std::mutex> lk(emergency_mutex_);// נועלת את mutex של מפתחות החירום
        auto vehicle_it = emergency_keys_.find(vehicle_id);// מחפשת מפתח ספציפי לרכב זה
        if (vehicle_it != emergency_keys_.end()) {// אם נמצא מפתח ספציפי לרכב
            secret = vehicle_it->second;// משתמשת במפתח הספציפי לרכב
        } else {
            auto wildcard_it = emergency_keys_.find("*");// מחפשת מפתח wildcard שמאפשר לכל רכב
            if (wildcard_it != emergency_keys_.end()) {// אם נמצא מפתח wildcard
                secret = wildcard_it->second;// משתמשת במפתח ה-wildcard
            }
        }

        if (secret.empty()) {// אם לא נמצא מפתח (לא ספציפי ולא wildcard)
            reason_out = "unknown_vehicle_id";// מגדירה סיבת שגיאה
            return false;// מחזירה false — הרכב אינו מורשה
        }

        auto replay_it = emergency_last_timestamp_by_vehicle_.find(vehicle_id);// מחפשת את חותמת הזמן האחרונה של הרכב הזה
        if (replay_it != emergency_last_timestamp_by_vehicle_.end() && timestamp <= replay_it->second) {// בודקת שחותמת הזמן חדשה יותר מהאחרונה (מניעת replay attack)
            reason_out = "replay_detected";// מגדירה סיבת שגיאה
            return false;// מחזירה false — זוהה replay attack
        }
    }

    std::ostringstream payload;// יוצרת מחרוזת payload לחישוב החתימה הצפויה
    payload << vehicle_id << "|" << lane_id << "|"
            << std::fixed << std::setprecision(3) << timestamp;// בונה את ה-payload בפורמט: vehicle_id|lane_id|timestamp

    const std::string expected_signature = to_hex(hmac_sha256_bytes(secret, payload.str()));// מחשבת את החתימה הצפויה באמצעות HMAC-SHA256
    if (expected_signature.empty()) {// אם חישוב החתימה נכשל
        reason_out = "signature_verification_failed";// מגדירה סיבת שגיאה
        return false;// מחזירה false — לא ניתן לאמת חתימה
    }

    if (to_lower_copy(expected_signature) != to_lower_copy(signature)) {// משווה את החתימה הצפויה לחתימה שהתקבלה (case-insensitive)
        reason_out = "invalid_signature";// מגדירה סיבת שגיאה
        return false;// מחזירה false — החתימה אינה תואמת
    }

    {
        std::lock_guard<std::mutex> lk(emergency_mutex_);// נועלת שוב את mutex לצורך עדכון חותמת הזמן
        emergency_last_timestamp_by_vehicle_[vehicle_id] = timestamp;// שומרת את חותמת הזמן הנוכחית למניעת replay attacks עתידיים
    }

    return true;// האות תקין — מחזירה true
}

json TrafficServer::build_neighbor_summaries(int intersection_id)
{// פונקציה שבונה סיכום חתום של מצב הצמתות השכנות — משמשת ל-/packet ולתיאום בין צמתים
    json summaries = json::array();// יוצרת מערך JSON ריק לאחסון הסיכומים

    const auto topo_it = neighbor_topology_.find(intersection_id);// מחפשת את רשימת השכנים של הצומת הנתון
    if (topo_it == neighbor_topology_.end() || topo_it->second.empty()) {// אם אין שכנים מוגדרים לצומת זה
        return summaries;// מחזירה מערך ריק
    }

    const std::string neighbor_key = env_or("NEIGHBOR_MESSAGE_KEY", "demo-neighbor-message-key");// קוראת את מפתח הסוד לחתימת הודעות שכן ממשתנה סביבה או ברירת מחדל

    for (int neighbor_id : topo_it->second) {// עוברת על כל מזהי הצמתות השכנות
        std::string neighbor_state_str;// משתנה לאחסון המצב הנוכחי של הצומת השכן כ-JSON string
        std::string neighbor_action_str;// משתנה לאחסון הפעולה הנוכחית של הצומת השכן כ-JSON string
        {
            std::lock_guard<std::mutex> lk(store_mutex_);// נועלת את mutex המאגר לגישה בטוחה
            auto si = state_store_.find(neighbor_id);// מחפשת את המצב הנוכחי של הצומת השכן
            if (si == state_store_.end()) {// אם אין מצב שמור לצומת השכן
                continue;// עוברת לשכן הבא — אין נתונים לסכם
            }
            neighbor_state_str = si->second;// שומרת את מחרוזת המצב של השכן

            auto ai = action_store_.find(neighbor_id);// מחפשת את הפעולה הנוכחית של הצומת השכן
            if (ai != action_store_.end()) {// אם יש פעולה שמורה לצומת השכן
                neighbor_action_str = ai->second;// שומרת את מחרוזת הפעולה של השכן
            }
        }

        json neighbor_state;// משתנה לאחסון המצב הנוכחי של השכן כ-JSON object
        try {
            neighbor_state = json::parse(neighbor_state_str);// מנסה לפרסר את מחרוזת המצב ל-JSON
        } catch (...) {
            continue;// אם הפרסור נכשל — עוברת לשכן הבא
        }

        json neighbor_action;// משתנה לאחסון הפעולה הנוכחית של השכן כ-JSON object
        if (!neighbor_action_str.empty()) {// אם יש מחרוזת פעולה
            try {
                neighbor_action = json::parse(neighbor_action_str);// מנסה לפרסר את מחרוזת הפעולה ל-JSON
            } catch (...) {
                neighbor_action = json::object();// אם הפרסור נכשל — משתמשת באובייקט ריק
            }
        } else {
            neighbor_action = {// אין פעולה שמורה — משתמשת בברירת מחדל Phase0
                {"action", "Phase0"},// פעולת ברירת מחדל
                {"phase_id", 0},// מזהה פאזה 0 כברירת מחדל
            };
        }

        int total_queue = 0;// מונה כולל של רכבים בתור בכל נתיבי השכן
        double waiting_sum = 0.0;// סכום זמני המתנה בכל הנתיבים
        int lane_count = 0;// מונה נתיבים לחישוב ממוצע

        if (neighbor_state.contains("lanes") && neighbor_state["lanes"].is_array()) {// אם המצב מכיל מערך נתיבים
            for (const auto& lane : neighbor_state["lanes"]) {// עוברת על כל הנתיבים
                if (!lane.is_object()) continue;// מדלגת על ערכים שאינם אובייקטים

                int vehicle_count = 0;// מספר רכבים בנתיב זה
                if (lane.contains("vehicle_count") && lane["vehicle_count"].is_number()) {// בודקת שיש שדה vehicle_count ושהוא מספר
                    vehicle_count = std::max(0, static_cast<int>(lane["vehicle_count"].get<double>()));// מחלצת את מספר הרכבים (לא שלילי)
                }
                total_queue += vehicle_count;// מוסיפה לסכום הכולל

                double waiting = 0.0;// זמן המתנה בנתיב זה
                if (lane.contains("waiting_time_sec") && lane["waiting_time_sec"].is_number()) {// בודקת שיש שדה waiting_time_sec ושהוא מספר
                    waiting = std::max(0.0, lane["waiting_time_sec"].get<double>());// מחלצת את זמן ההמתנה (לא שלילי)
                }
                waiting_sum += waiting;// מוסיפה לסכום זמני ההמתנה
                ++lane_count;// מקדמת את מונה הנתיבים
            }
        }

        const double avg_waiting_sec = lane_count > 0 ? (waiting_sum / lane_count) : 0.0;// מחשבת ממוצע זמן המתנה — 0 אם אין נתיבים
        const bool emergency_active =
            neighbor_state.contains("emergency_signal") &&
            neighbor_state["emergency_signal"].is_object() &&
            neighbor_state["emergency_signal"].value("active", false);// בודקת אם יש חירום פעיל בצומת השכן

        int phase_id = -1;// מאתחלת מזהה פאזה ל-1- (לא ידוע)
        if (neighbor_action.contains("phase_id") && neighbor_action["phase_id"].is_number_integer()) {// בודקת שיש שדה phase_id ושהוא מספר שלם
            phase_id = neighbor_action["phase_id"].get<int>();// מחלצת את מזהה הפאזה הנוכחי
        }

        const double signed_at = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();// מחשבת את חותמת הזמן הנוכחית לחתימה

        std::ostringstream payload;// בונה את ה-payload לחתימת ההודעה
        payload << neighbor_id << "|"
                << phase_id << "|"
                << total_queue << "|"
                << std::fixed << std::setprecision(3) << avg_waiting_sec << "|"
                << (emergency_active ? 1 : 0) << "|"
                << std::fixed << std::setprecision(3) << signed_at;// פורמט: neighbor_id|phase|queue|wait|emergency|timestamp

        const std::string signature = to_hex(hmac_sha256_bytes(neighbor_key, payload.str()));// מחשבת חתימת HMAC-SHA256 על ה-payload

        json summary = {// בונה אובייקט JSON עם הסיכום החתום של הצומת השכן
            {"intersection_id", neighbor_id},// מזהה הצומת השכן
            {"action", neighbor_action.value("action", "Phase0")},// הפאזה הנוכחית של השכן
            {"phase_id", phase_id >= 0 ? json(phase_id) : json(nullptr)},// מזהה הפאזה או null
            {"total_queue", total_queue},// סך הרכבים בתור בצומת השכן
            {"avg_waiting_sec", avg_waiting_sec},// ממוצע זמן המתנה בצומת השכן
            {"emergency_active", emergency_active},// האם יש חירום פעיל בשכן
            {"signed_at", signed_at},// חותמת הזמן של יצירת ה-summary
            {"signature", signature.empty() ? json(nullptr) : json(signature)},// חתימת HMAC על ה-summary
        };
        summaries.push_back(summary);// מוסיפה את הסיכום למערך
    }

    return summaries;// מחזירה את מערך הסיכומים עם כל הצמתות השכנות
}

// (helpers appended below)

// ── broadcast_event ──────────────────────────────────────────────────────────
// Wraps a payload in the canonical envelope and ships it to BOTH
// /ws/intersection/{id} subscribers and /ws/updates global subscribers.
void TrafficServer::broadcast_event(int intersection_id,
                                    const std::string& event_name,
                                    const nlohmann::json& payload)
{// פונקציה ששולחת אירוע WebSocket לכל המנויים של הצומת הנתון — עוטפת ב-envelope סטנדרטי
    const double ts = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();// מחשבת את חותמת הזמן הנוכחית לאירוע
    const nlohmann::json envelope = {// בונה את מעטפת האירוע הסטנדרטית
        {"event",           event_name},// שם האירוע (למשל: state_updated, action_updated)
        {"intersection_id", intersection_id},// מזהה הצומת שהאירוע שייך לו
        {"timestamp",       ts},// חותמת הזמן של האירוע
        {"payload",         payload},// תוכן האירוע (המצב/הפעולה)
    };
    if (hub_) hub_->broadcast_intersection(intersection_id, envelope.dump());// שולחת את המעטפת לכל מנויי הצומת דרך ה-WebSocket hub
}

// ── latch_emergency / clear_emergency_latch / active_emergency_signal ────────
void TrafficServer::latch_emergency(int intersection_id, const nlohmann::json& signal)
{// פונקציה שנועלת אות חירום פעיל עבור צומת — שומרת אותו למשך kEmergencyLatchSec שניות
    constexpr double EMERGENCY_LATCH_SEC = traffic::constants::kEmergencyLatchSec;// מספר השניות שהחירום נשאר פעיל לאחר קבלת האות
    const double now_s = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();// מחשבת את הזמן הנוכחי בשניות
    std::lock_guard<std::mutex> lk(emergency_latch_mutex_);// נועלת את mutex של מנגנון ה-latch
    emergency_latch_[intersection_id]        = signal.dump();// שומרת את אות החירום כ-JSON string
    emergency_latch_expiry_[intersection_id] = now_s + EMERGENCY_LATCH_SEC;// מגדירה את זמן תפוגת החירום
}

void TrafficServer::clear_emergency_latch(int intersection_id)
{// פונקציה שמנקה את אות החירום עבור צומת — נקראת ממשק הניהול או עם תפוגה
    std::lock_guard<std::mutex> lk(emergency_latch_mutex_);// נועלת את mutex של מנגנון ה-latch
    emergency_latch_.erase(intersection_id);// מוחקת את אות החירום השמור
    emergency_latch_expiry_.erase(intersection_id);// מוחקת את זמן תפוגת החירום
}

nlohmann::json TrafficServer::active_emergency_signal(int intersection_id)
{// פונקציה שמחזירה את אות החירום הפעיל עבור צומת — null אם אין חירום או אם פג תוקפו
    const double now_s = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();// מחשבת את הזמן הנוכחי בשניות
    std::lock_guard<std::mutex> lk(emergency_latch_mutex_);// נועלת את mutex של מנגנון ה-latch
    auto exp_it = emergency_latch_expiry_.find(intersection_id);// מחפשת את זמן התפוגה של החירום לצומת הנתון
    if (exp_it == emergency_latch_expiry_.end() || exp_it->second < now_s) {// אם אין חירום שמור או שפג תוקפו
        emergency_latch_.erase(intersection_id);// מנקה את האות הפג
        emergency_latch_expiry_.erase(intersection_id);// מנקה את זמן התפוגה
        return nullptr;// מחזירה null — אין חירום פעיל
    }
    auto sig_it = emergency_latch_.find(intersection_id);// מחפשת את האות השמור לצומת הנתון
    if (sig_it == emergency_latch_.end()) return nullptr;// אם לא נמצא אות — מחזירה null
    try { return nlohmann::json::parse(sig_it->second); }// מנסה לפרסר ולהחזיר את האות כ-JSON object
    catch (...) { return nullptr; }// אם הפרסור נכשל — מחזירה null
}

// ── decide_action_fallback ───────────────────────────────────────────────────
// Server-side greedy-with-aging fallback used when no controller / manual /
// emergency action is in force. Scores each lane by
//     vehicle_count * 1.5 + density_pct * 0.5 + waiting_time_sec * 0.2
// then maps the winner's lane index to phase 0 (even) or phase 1 (odd).
// Matches Python decide_action() so the React UI sees the same labels.
nlohmann::json TrafficServer::decide_action_fallback(int intersection_id,
                                                    const nlohmann::json& state_body)
{// פונקציה שמחשבת פעולה כ-fallback כשאין בקר RL פעיל — אלגוריתם Greedy Aging לפי ציון לכל נתיב
    int    best_lane  = 0;// מזהה הנתיב עם הציון הגבוה ביותר (ברירת מחדל: נתיב 0)
    double best_score = -1.0;// הציון הגבוה ביותר שנמצא עד כה (מאותחל לשלילי)
    bool   any_lane   = false;// דגל שמציין אם נסרקו נתיבים בכלל

    std::cout << "[GreedyAging] Intersection " << intersection_id
              << " — scoring lanes:\n";// מדפיסה כותרת לוג לתחילת חישוב הציונים

    if (state_body.contains("lanes") && state_body["lanes"].is_array()) {// בודקת שיש מערך נתיבים בגוף המצב
        for (const auto& lane : state_body["lanes"]) {// עוברת על כל הנתיבים
            if (!lane.is_object()) continue;// מדלגת על ערכים שאינם אובייקטים
            const int    lid   = lane.value("lane_id", 0);// מחלצת מזהה נתיב
            const std::string dir = lane.value("direction", std::string(""));// מחלצת כיוון נתיב
            const double vc    = lane.contains("vehicle_count")    && lane["vehicle_count"].is_number()    ? lane["vehicle_count"].get<double>()    : 0.0;// מספר רכבים בנתיב
            const double dens  = lane.contains("density_pct")      && lane["density_pct"].is_number()      ? lane["density_pct"].get<double>()      : 0.0;// צפיפות תנועה בנתיב באחוזים
            const double wait  = lane.contains("waiting_time_sec") && lane["waiting_time_sec"].is_number() ? lane["waiting_time_sec"].get<double>() : 0.0;// זמן המתנה בנתיב בשניות
            const double score = vc * traffic::constants::kGreedyWeightVehicleCount
                               + dens * traffic::constants::kGreedyWeightDensityPct
                               + wait * traffic::constants::kGreedyWeightWaitingSec;// מחשבת ציון משוקלל: vehicles*1.5 + density*0.5 + wait*0.2

            std::cout << "  Lane " << lid
                      << " (" << (dir.empty() ? "?" : dir) << ")"
                      << "  vc=" << vc
                      << "  dens=" << dens << "%"
                      << "  wait=" << wait << "s"
                      << "  => score=" << std::fixed << std::setprecision(2) << score
                      << (score > best_score ? "  <-- best so far" : "")
                      << "\n";// מדפיסה לוג מפורט עבור הנתיב הנוכחי

            if (score > best_score) { best_score = score; best_lane = lid; }// מעדכנת את הנתיב הזוכה אם הציון גבוה יותר
            any_lane = true;// מסמנת שנסרק לפחות נתיב אחד
        }
    }

    const int phase_id = (best_lane % 2 == 0) ? 0 : 1;// ממפה את הנתיב הזוכה לפאזה: נתיב זוגי → Phase0, אי-זוגי → Phase1

    if (any_lane) {
        std::cout << "[GreedyAging] Winner: lane " << best_lane
                  << "  score=" << std::fixed << std::setprecision(2) << best_score
                  << "  => Phase" << phase_id
                  << "  (reason: highest_score)\n";// מדפיסה את הנתיב הזוכה והפאזה שנבחרה
    } else {
        std::cout << "[GreedyAging] No lane data — defaulting to Phase0\n";// מדפיסה שאין נתונים ונבחרה ברירת מחדל
    }

    return nlohmann::json({// מחזירה את הפעולה שנבחרה בפורמט JSON
        {"action",          std::string("Phase") + std::to_string(phase_id)},// שם הפאזה שנבחרה
        {"reason",          std::string("server_fallback_greedy_aging lane=") + std::to_string(best_lane)},// סיבת הבחירה כולל מזהה הנתיב הזוכה
        {"intersection_id", intersection_id},// מזהה הצומת
        {"phase_id",        phase_id},// מזהה הפאזה המספרי
    });
}

