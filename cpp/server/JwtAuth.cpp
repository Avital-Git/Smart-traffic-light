#include "JwtAuth.h"   // הכרזות הפונקציות הציבוריות של המודול
#include "Database.h"  // גישה ל-db_get_admin_credentials ו-db_touch_admin_last_login
#include "../TrafficConstants.h"  // קבועי מערכת — kAdminJwtExpirationMinutes

// ── ספריות Windows ───────────────────────────────────────────────────────────
#include <windows.h>   // Windows API בסיסי
#include <bcrypt.h>    // Windows CNG — ספריית הצפנה מובנית (SHA-256, HMAC)

// ── ספריות C++ סטנדרטיות ────────────────────────────────────────────────────
#include <algorithm>         // std::equal — להשוואת מחרוזות
#include <chrono>            // std::chrono — זמן נוכחי (epoch)
#include <cctype>            // std::tolower — המרה לאותיות קטנות
#include <cstdlib>           // std::getenv — קריאת משתני סביבה
#include <iomanip>           // std::setw, std::setfill — עיצוב hex
#include <nlohmann/json.hpp> // ספריית JSON לבניית ופרסור payload של JWT
#include <optional>          // std::optional — לערכי החזרה אופציונליים
#include <random>            // std::random_device, mt19937 — אקראיות לsalt
#include <sstream>           // std::ostringstream — בניית מחרוזת hex
#include <string>            // std::string
#include <vector>            // std::vector

#pragma comment(lib, "bcrypt.lib")  // קישור אוטומטי לספריית CNG של Windows

using json = nlohmann::json;  // קיצור נוחות

namespace jwt_auth {
namespace {  // anonymous namespace — כל מה שכאן פרטי לקובץ זה בלבד

// ── קבועים ────────────────────────────────────────────────────────────────────
// תוקף JWT — מוגדר ב-TrafficConstants.h כ-kAdminJwtExpirationMinutes
const char* DEFAULT_ADMIN_SECRET = "super-secret-admin-key-change-in-production";  // מפתח חתימה ברירת מחדל — חובה לשנות בייצור!

// קורא משתנה סביבה; אם לא קיים — מחזיר את fallback
std::string get_env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return (value && value[0]) ? value : fallback;  // value[0] בודק שאינו מחרוזת ריקה
}

// ── SHA-256 (ללא מפתח) ────────────────────────────────────────────────────────
// מחשב SHA-256 של input ומחזיר את הבייטים הגולמיים
// משמש להצפנת סיסמאות: SHA-256(salt + password)
std::vector<unsigned char> sha256_bytes(const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;   // ידית אלגוריתם CNG
    BCRYPT_HASH_HANDLE hash = nullptr; // ידית אובייקט ה-hash
    DWORD object_len = 0;  // גודל בפר הפנימי של ה-hash
    DWORD data_len = 0;    // משמש כפרמטר פלט ב-BCryptGetProperty
    DWORD hash_len = 0;    // גודל תוצאת ה-hash (32 בייטים ל-SHA-256)
    std::vector<unsigned char> hash_object;  // בפר פנימי לאובייקט ה-hash
    std::vector<unsigned char> hash_value;   // תוצאת ה-hash הסופית

    // שלב 1: פתיחת ספק אלגוריתם SHA-256
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};  // נכשל — מחזיר וקטור ריק
    }
    // שלב 2: שאילת גודל הבפר הפנימי הנדרש
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    // שלב 3: שאילת גודל תוצאת ה-hash
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    hash_object.resize(object_len);  // הקצאת הבפר הפנימי
    hash_value.resize(hash_len);     // הקצאת בפר התוצאה

    // שלב 4: יצירת אובייקט ה-hash (ללא מפתח — SHA רגיל)
    if (BCryptCreateHash(alg, &hash, hash_object.data(), object_len, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    // שלב 5: הזנת הנתונים לחישוב
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    // שלב 6: חישוב ושמירת התוצאה הסופית
    if (BCryptFinishHash(hash, hash_value.data(), hash_len, 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    // ניקוי משאבים
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash_value;  // מחזיר 32 בייטים
}

// ── HMAC-SHA256 (עם מפתח) ─────────────────────────────────────────────────────
// מחשב HMAC-SHA256(key, input) — חתימה עם מפתח סודי
// משמש לחתימת טוקני JWT: sign(header.payload)
std::vector<unsigned char> hmac_sha256_bytes(const std::string& key, const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_len = 0;
    DWORD data_len = 0;
    DWORD hash_len = 0;
    std::vector<unsigned char> hash_object;
    std::vector<unsigned char> hash_value;

    // פותח ספק SHA-256 עם דגל HMAC — ההבדל מ-sha256_bytes
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &data_len, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    hash_object.resize(object_len);
    hash_value.resize(hash_len);

    // יצירת hash עם המפתח הסודי (JWT secret)
    if (BCryptCreateHash(
            alg,
            &hash,
            hash_object.data(),
            object_len,
            reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),  // המפתח הסודי
            static_cast<ULONG>(key.size()),
            0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    // הזנת הנתונים (header.payload של JWT)
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    // קבלת החתימה הסופית
    if (BCryptFinishHash(hash, hash_value.data(), hash_len, 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash_value;
}

// ── עזר: המרת בייטים ל-hex string ────────────────────────────────────────────
// למשל: {0xDE, 0xAD} → "dead"
std::string to_hex(const std::vector<unsigned char>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');  // פורמט hex עם אפסים מובילים
    for (unsigned char value : bytes) {
        out << std::setw(2) << static_cast<int>(value);  // כל בייט = 2 תווי hex
    }
    return out.str();
}

// ── Base64URL ─────────────────────────────────────────────────────────────────
// טבלת base64url — כמו base64 רגיל אבל + הוחלף ב-- ו-/ הוחלף ב-_
// URL-safe ולא צריך padding
const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// מקודד בייטים גולמיים ל-base64url (ללא '=' padding)
// משמש לקידוד header ו-payload של JWT
std::string base64url_encode(const std::vector<unsigned char>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);  // הקצאה מראש לגודל המשוער
    for (size_t i = 0; i < data.size(); i += 3) {  // מעבד 3 בייטים בכל פעם → 4 תווי base64
        uint32_t block = static_cast<uint32_t>(data[i]) << 16;  // בייט ראשון
        if (i + 1 < data.size()) block |= static_cast<uint32_t>(data[i + 1]) << 8;   // בייט שני (אם קיים)
        if (i + 2 < data.size()) block |= static_cast<uint32_t>(data[i + 2]);         // בייט שלישי (אם קיים)

        out.push_back(B64URL[(block >> 18) & 0x3f]);  // ביטים 23-18
        out.push_back(B64URL[(block >> 12) & 0x3f]);  // ביטים 17-12
        if (i + 1 < data.size()) out.push_back(B64URL[(block >> 6) & 0x3f]);  // ביטים 11-6
        if (i + 2 < data.size()) out.push_back(B64URL[block & 0x3f]);          // ביטים 5-0
    }
    return out;
}

// מפענח base64url חזרה לבייטים גולמיים
// מחזיר nullopt אם המחרוזת מכילה תווים לא חוקיים
std::optional<std::vector<unsigned char>> base64url_decode(const std::string& input) {
    std::vector<int> table(256, -1);  // מיפוי תו → ערך (0-63), -1 = לא חוקי
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(B64URL[i])] = i;  // בניית טבלת lookup

    std::vector<unsigned char> out;
    int val = 0;   // אוגר ביטים זמני
    int bits = -8; // מונה ביטים שנצברו
    for (unsigned char c : input) {
        if (table[c] == -1) return std::nullopt;  // תו לא חוקי — מפסיקים
        val = (val << 6) + table[c];  // הוספת 6 ביטים לאוגר
        bits += 6;
        if (bits >= 0) {  // יש בייט שלם — שומרים אותו
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// ── חתימת JWT (HS256) ─────────────────────────────────────────────────────────
// חותם את data (= "header.payload") עם המפתח הסודי
// מחזיר base64url של ה-HMAC-SHA256
std::string sign_hs256(const std::string& data) {
    std::string secret = get_env_or("ADMIN_JWT_SECRET", DEFAULT_ADMIN_SECRET);  // קורא מפתח ממשתנה סביבה
    return base64url_encode(hmac_sha256_bytes(secret, data));  // חישוב + קידוד
}

// מחזיר את הזמן הנוכחי בשניות מאז epoch (1.1.1970 UTC)
long long now_epoch_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace — סוף ה-anonymous namespace

// ── פונקציות ציבוריות ─────────────────────────────────────────────────────────

// בודק אם הסיסמה שהוזנה תואמת ל-hash שב-DB
// תהליך: שולף hash + salt מה-DB, מחשב SHA-256(salt + password), משווה
bool verify_admin_password(const std::string& username, const std::string& password) {
    std::string hash;
    std::string salt;
    if (!db_get_admin_credentials(username, hash, salt)) {
        return false;  // משתמש לא קיים או שגיאת DB
    }

    const std::string expected = to_hex(sha256_bytes(salt + password));  // מחשב hash צפוי
    const bool ok = (expected == hash);  // השוואה בטוחה (לא נגד timing attack, אבל hash מונע חשיפה)
    if (ok) {
        db_touch_admin_last_login(username);  // מעדכן last_login ב-DB
    }
    return ok;
}

// יוצר טוקן JWT חדש בפורמט: base64url(header).base64url(payload).base64url(signature)
// payload מכיל: sub (username), role, iat (issued at), exp (expiration)
AdminTokenResponse create_admin_token(const std::string& username, const std::string& role) {
    // header של JWT — מגדיר אלגוריתם חתימה
    json header = {
        {"alg", "HS256"},  // אלגוריתם: HMAC-SHA256
        {"typ", "JWT"},    // סוג: JWT
    };
    long long iat = now_epoch_seconds();  // זמן יצירה
    long long exp = iat + static_cast<long long>(traffic::constants::kAdminJwtExpirationMinutes) * 60LL;  // זמן תפוגה
    const std::string safe_role = (role == "super_admin") ? "super_admin" : "regular_admin";  // וולידציה של role
    // payload — המידע המוטבע בתוך הטוקן
    json payload = {
        {"sub", username},   // subject — שם המשתמש
        {"role", safe_role}, // הרשאה
        {"iat", iat},        // issued at
        {"exp", exp},        // expiration
    };

    // קידוד header ל-base64url
    const std::string header_dump = header.dump();  // המרה ל-JSON string
    const std::string header_part = base64url_encode(std::vector<unsigned char>(header_dump.begin(), header_dump.end()));
    // קידוד payload ל-base64url
    const std::string payload_dump = payload.dump();
    const std::string payload_part = base64url_encode(std::vector<unsigned char>(payload_dump.begin(), payload_dump.end()));
    // המחרוזת שנחתמת: "header.payload"
    const std::string signing_input = header_part + "." + payload_part;
    // חישוב החתימה
    const std::string sig_part = sign_hs256(signing_input);

    AdminTokenResponse response;
    response.access_token = signing_input + "." + sig_part;  // טוקן מלא: header.payload.signature
    response.expires_in = traffic::constants::kAdminJwtExpirationMinutes * 60;  // זמן תוקף בשניות
    return response;
}

// מאמת טוקן JWT — בודק חתימה ותאריך תפוגה
// מחזיר true ומעביר username אם הטוקן תקין
bool validate_admin_token(const std::string& token, std::string& username_out) {
    // פיצול הטוקן לשלושה חלקים לפי נקודות
    const size_t first_dot = token.find('.');
    const size_t second_dot = token.find('.', first_dot == std::string::npos ? first_dot : first_dot + 1);
    if (first_dot == std::string::npos || second_dot == std::string::npos) return false;  // פורמט לא תקין

    const std::string header_part  = token.substr(0, first_dot);  // החלק הראשון
    const std::string payload_part = token.substr(first_dot + 1, second_dot - first_dot - 1);  // החלק השני
    const std::string sig_part     = token.substr(second_dot + 1);  // החתימה

    // בדיקת חתימה — מחשבים חתימה חדשה ומשווים
    if (sign_hs256(header_part + "." + payload_part) != sig_part) return false;  // חתימה לא תואמת — טוקן מזויף

    // פענוח ה-payload
    auto payload_bytes = base64url_decode(payload_part);
    if (!payload_bytes) return false;  // פענוח base64 נכשל

    json payload;
    try {
        payload = json::parse(std::string(payload_bytes->begin(), payload_bytes->end()));  // פרסור JSON
    } catch (...) {
        return false;  // JSON לא תקין
    }

    // בדיקות תקינות של שדות ה-payload
    if (!payload.contains("sub") || !payload["sub"].is_string()) return false;  // חסר שם משתמש
    if (!payload.contains("exp") || !payload["exp"].is_number()) return false;  // חסר תאריך תפוגה
    if (payload["exp"].get<long long>() < now_epoch_seconds()) return false;    // הטוקן פג תוקף

    username_out = payload["sub"].get<std::string>();  // מחלץ שם משתמש
    return true;
}

// גרסה מורחבת — מחזירה גם את ה-role מה-payload
// משמש לאכיפת הרשאות (super_admin בלבד לפעולות כמו מחיקת משתמשים)
bool validate_admin_token_with_role(const std::string& token, std::string& username_out, std::string& role_out) {
    // זהה ל-validate_admin_token עם תוספת חילוץ role
    const size_t first_dot = token.find('.');
    const size_t second_dot = token.find('.', first_dot == std::string::npos ? first_dot : first_dot + 1);
    if (first_dot == std::string::npos || second_dot == std::string::npos) return false;

    const std::string header_part  = token.substr(0, first_dot);
    const std::string payload_part = token.substr(first_dot + 1, second_dot - first_dot - 1);
    const std::string sig_part     = token.substr(second_dot + 1);

    if (sign_hs256(header_part + "." + payload_part) != sig_part) return false;  // חתימה לא תואמת

    auto payload_bytes = base64url_decode(payload_part);
    if (!payload_bytes) return false;

    json payload;
    try {
        payload = json::parse(std::string(payload_bytes->begin(), payload_bytes->end()));
    } catch (...) {
        return false;
    }

    if (!payload.contains("sub") || !payload["sub"].is_string()) return false;
    if (!payload.contains("exp") || !payload["exp"].is_number()) return false;
    if (payload["exp"].get<long long>() < now_epoch_seconds()) return false;  // תפוגה

    username_out = payload["sub"].get<std::string>();               // שם משתמש
    role_out = payload.value("role", "regular_admin");              // role — ברירת מחדל regular_admin
    return true;
}

// מחלץ את הטוקן מ-header "Authorization: Bearer <token>"
// מאפשר גם אותיות גדולות/קטנות ("bearer", "Bearer", "BEARER")
std::optional<std::string> extract_bearer_token(const std::string& authorization_header) {
    const std::string prefix = "Bearer ";
    if (authorization_header.size() < prefix.size()) return std::nullopt;  // קצר מדי
    // השוואה case-insensitive לקידומת "Bearer "
    if (!std::equal(prefix.begin(), prefix.end(), authorization_header.begin(), authorization_header.begin() + prefix.size(),
                    [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
        return std::nullopt;  // לא מתחיל ב-Bearer
    }
    std::string token = authorization_header.substr(prefix.size());  // חותך את "Bearer "
    if (token.empty()) return std::nullopt;  // אין טוקן אחרי הקידומת
    return token;
}

// מחזיר כמה שניות טוקן תקף (קבוע * 60)
int admin_expiration_seconds() {
    return traffic::constants::kAdminJwtExpirationMinutes * 60;  // 480 דקות × 60 = 28800 שניות
}

// מחשב SHA-256(salt + password) ומחזיר hex string
// זה הפורמט שנשמר ב-DB ומשמש לאימות בכניסה
std::string hash_password_with_salt(const std::string& salt, const std::string& password) {
    return to_hex(sha256_bytes(salt + password));  // שרשור salt + סיסמה → hash
}

// מייצר salt אקראי — num_random_bytes בייטים אקראיים, מוחזרים כ-hex
// ה-salt מבטיח שאפילו שתי סיסמאות זהות יקבלו hash שונה
std::string generate_salt_hex(size_t num_random_bytes) {
    if (num_random_bytes == 0) {
        return "";  // בקשה ל-0 בייטים — salt ריק
    }

    std::random_device rd;       // מקור אנטרופיה של מערכת ההפעלה
    std::mt19937 gen(rd());      // מחולל מספרים פסאודו-אקראיים (מוזן מ-random_device)
    std::uniform_int_distribution<int> dist(0, 255);  // טווח: בייט שלם 0-255
    std::vector<unsigned char> bytes(num_random_bytes);
    for (size_t i = 0; i < num_random_bytes; ++i) {
        bytes[i] = static_cast<unsigned char>(dist(gen));  // בייט אקראי
    }
    return to_hex(bytes);  // מחזיר כ-hex (ברירת מחדל: 32 תווים = 16 בייטים)
}

} // namespace jwt_auth
