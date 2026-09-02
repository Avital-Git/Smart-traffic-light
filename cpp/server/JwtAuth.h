#pragma once  // מונע הכללה כפולה של הקובץ

#include <optional>  // std::optional — לערכי החזרה שיכולים להיות ריקים
#include <string>    // std::string

namespace jwt_auth {  // כל הפונקציות נמצאות תחת namespace jwt_auth כדי למנוע התנגשות שמות

// מבנה המייצג תשובת טוקן — מה שנשלח ללקוח לאחר כניסה מוצלחת
struct AdminTokenResponse {
    std::string access_token;        // הטוקן JWT המלא (header.payload.signature)
    std::string token_type = "bearer";  // סוג הטוקן — תמיד "bearer" לפי תקן OAuth2
    int expires_in = 0;              // כמה שניות הטוקן תקף מרגע יצירתו
};

// בודק סיסמה של משתמש אדמין מול ה-DB (hash + salt)
// מחזיר true אם הסיסמה נכונה, false אחרת
bool verify_admin_password(const std::string& username, const std::string& password);

// יוצר טוקן JWT חדש עבור משתמש אדמין מאומת
// role = 'super_admin' או 'regular_admin' — מוטבע בתוך הטוקן
AdminTokenResponse create_admin_token(const std::string& username, const std::string& role = "regular_admin");

// מאמת טוקן JWT — בודק חתימה + תאריך תפוגה
// אם תקין — ממלא username_out ומחזיר true
bool validate_admin_token(const std::string& token, std::string& username_out);

// גרסה מורחבת של validate_admin_token — מחזירה גם את ה-role
// משמשת לבדיקת הרשאות (super_admin בלבד לפעולות רגישות)
bool validate_admin_token_with_role(const std::string& token, std::string& username_out, std::string& role_out);

// מחלץ את הטוקן מה-header: "Authorization: Bearer <token>"
// מחזיר nullopt אם ה-header לא תקין
std::optional<std::string> extract_bearer_token(const std::string& authorization_header);

// מחזיר כמה שניות טוקן תקף (480 דקות = 8 שעות ברירת מחדל)
int admin_expiration_seconds();

// מחשב SHA-256(salt + password) ומחזיר hex string
// זה ה-hash שנשמר ב-DB ומשמש לאימות
std::string hash_password_with_salt(const std::string& salt, const std::string& password);

// מייצר salt אקראי בגודל num_random_bytes בייטים ומחזיר אותו כ-hex string
// ברירת מחדל: 16 בייטים = 32 תווי hex
std::string generate_salt_hex(size_t num_random_bytes = 16);

} // namespace jwt_auth
