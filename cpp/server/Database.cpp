#include "Database.h"

// ─── ספריות Windows ו-ODBC לגישה ל-SQL Server ───────────────────────────────
#include <windows.h>  // ממשק Windows API (WideCharToMultiByte וכו')
#include <sql.h>      // סוגי נתונים ופונקציות ODBC בסיסיות
#include <sqlext.h>   // הרחבות ODBC (SQLDriverConnect, SQLBindParameter וכו')

// ─── ספריות סטנדרטיות של C++ ────────────────────────────────────────────────
#include <algorithm>  // std::find לחיפוש ברשימות
#include <cstdlib>    // std::getenv לקריאת משתני סביבה
#include <iostream>   // std::cerr להדפסת שגיאות
#include <string>     // std::string
#include <vector>     // std::vector לרשימות דינמיות

// ---------------------------------------------------------------------------
// נתוני ברירת מחדל — נטענים כשאין חיבור ל-SQL Server
// מראה את 4 הצמתות הקבועות (זהה ל-DEFAULT_INTERSECTIONS בפייתון)
// ---------------------------------------------------------------------------
static const std::vector<IntersectionRow> DEFAULTS = {
    {1, "J-001", "\xd7\xa6\xd7\x95\xd7\x9e\xd7\xaa \xd7\x9e\xd7\xa8\xd7\x9b\xd7\x96\xd7\x99",          "\xd7\x91\xd7\x90\xd7\xa8 \xd7\xa9\xd7\x91\xd7\xa2"},
    {2, "J-002", "\xd7\xa6\xd7\x95\xd7\x9e\xd7\xaa \xd7\x94\xd7\x90\xd7\x95\xd7\xa0\xd7\x99\xd7\x91\xd7\xa8\xd7\xa1\xd7\x99\xd7\x98\xd7\x94", "\xd7\x91\xd7\x90\xd7\xa8 \xd7\xa9\xd7\x91\xd7\xa2"},
    {3, "J-003", "\xd7\xa6\xd7\x95\xd7\x9e\xd7\xaa \xd7\x91\xd7\x99\xd7\xaa \xd7\x97\xd7\x95\xd7\x9c\xd7\x99\xd7\x9d",   "\xd7\x91\xd7\x90\xd7\xa8 \xd7\xa9\xd7\x91\xd7\xa2"},
    {4, "J-004", "\xd7\xa6\xd7\x95\xd7\x9e\xd7\xaa \xd7\x93\xd7\xa8\xd7\x95\xd7\x9e\xd7\x99",           "\xd7\x91\xd7\x90\xd7\xa8 \xd7\xa9\xd7\x91\xd7\xa2"},
};

// ---------------------------------------------------------------------------
// פונקציות עזר פנימיות — אינן נחשפות מחוץ לקובץ זה
// ---------------------------------------------------------------------------

// קורא משתנה סביבה לפי שם; אם לא קיים — מחזיר את ערך ברירת המחדל
static std::string env_or(const char* name, const char* def)
{
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : def;
}

// מבנה המכיל את פרטי החיבור ל-SQL Server
struct DbConnConfig {
    std::string driver;   // שם ה-ODBC driver
    std::string server;   // שם / כתובת השרת (ברירת מחדל: .\SQLEXPRESS)
    std::string database; // שם מסד הנתונים
    std::string user;     // שם משתמש SQL (ריק = Windows Authentication)
    std::string password; // סיסמת SQL
};

// בונה את פרטי החיבור ממשתני סביבה (SQL_SERVER, SQL_DATABASE, SQL_USER, SQL_PASSWORD)
// חייב להיות זהה לקוד ב-python/db_intersections.py
static DbConnConfig resolve_db_conn_config()
{
    DbConnConfig cfg;
    cfg.driver   = "ODBC Driver 17 for SQL Server";
    cfg.server   = env_or("SQL_SERVER",   R"(.\SQLEXPRESS)");  // ברירת מחדל: SQL Server Express מקומי
    cfg.database = env_or("SQL_DATABASE", "smart_traffic");    // שם ה-DB
    cfg.user     = env_or("SQL_USER",     "");                 // ריק = Windows Auth
    cfg.password = env_or("SQL_PASSWORD", "");
    return cfg;
}

// ממיר מחרוזת Unicode רחבה (WCHAR*) ל-UTF-8 רגיל
// ind == SQL_NULL_DATA פירושו שהשדה ריק ב-DB
static std::string wstr_to_utf8(const WCHAR* wstr, SQLLEN ind)
{
    if (ind == SQL_NULL_DATA || wstr == nullptr || wstr[0] == L'\0')
        return "";  // שדה NULL או ריק — מחזיר מחרוזת ריקה
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);  // מחשב גודל מחרוזת UTF-8
    if (sz <= 1) return "";
    std::string out(sz - 1, '\0');  // מקצה בафер בגודל הנכון
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &out[0], sz, nullptr, nullptr);  // ממיר בפועל
    return out;
}

// בונה את מחרוזת החיבור ODBC המלאה שתועבר ל-SQLDriverConnect
static std::string build_conn_str()
{
    const DbConnConfig cfg = resolve_db_conn_config();

    std::string s = "DRIVER={" + cfg.driver + "};"
                    "SERVER=" + cfg.server + ";"
                    "DATABASE=" + cfg.database + ";";
    if (!cfg.user.empty())
        s += "UID=" + cfg.user + ";PWD=" + cfg.password + ";";
    else
        s += "Trusted_Connection=yes;";  // אימות Windows — לא צריך שם משתמש/סיסמה
    return s;
}

// פותח חיבור ODBC ל-SQL Server
// מחזיר true אם החיבור הצליח; false + error_out עם תיאור השגיאה אם נכשל
static bool open_sql_connection(SQLHENV& env, SQLHDBC& dbc, std::string* error_out = nullptr)
{
    env = SQL_NULL_HENV;  // איפוס מאגר סביבה
    dbc = SQL_NULL_HDBC;  // איפוס מאגר חיבור

    // שלב 1: הקצאת מאגר סביבת ODBC
    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) != SQL_SUCCESS) {
        if (error_out) *error_out = "SQLAllocHandle(SQL_HANDLE_ENV) failed";
        return false;
    }
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);  // הגדרת גרסת ODBC 3

    // שלב 2: הקצאת מאגר חיבור
    if (SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) {
        if (error_out) *error_out = "SQLAllocHandle(SQL_HANDLE_DBC) failed";
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        env = SQL_NULL_HENV;
        return false;
    }

    // שלב 3: פתיחת החיבור בפועל עם מחרוזת ה-ODBC
    std::string cs = build_conn_str();
    SQLRETURN rc = SQLDriverConnectA(
        dbc, NULL,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(cs.c_str())),
        static_cast<SQLSMALLINT>(cs.size()),
        NULL, 0, NULL,
        SQL_DRIVER_NOPROMPT  // לא להציג חלון dialog — עבודה בשקט
    );
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        // החיבור נכשל — בונה הודעת שגיאה מפורטת
        const DbConnConfig cfg = resolve_db_conn_config();
        const char* auth = cfg.user.empty() ? "Trusted_Connection=yes" : "UID/PWD";
        if (error_out) {
            *error_out = "SQLDriverConnect failed (driver='" + cfg.driver +
                         "', server='" + cfg.server +
                         "', database='" + cfg.database +
                         "', auth='" + std::string(auth) + "')";
        }
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        env = SQL_NULL_HENV;
        dbc = SQL_NULL_HDBC;
        return false;
    }

    return true;  // החיבור נפתח בהצלחה
}

// סוגר חיבור ODBC ומשחרר את כל המשאבים שהוקצו
static void close_sql_connection(SQLHENV env, SQLHDBC dbc)
{
    if (dbc != SQL_NULL_HDBC) {
        SQLDisconnect(dbc);              // ניתוק מה-DB
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);  // שחרור מאגר החיבור
    }
    if (env != SQL_NULL_HENV) {
        SQLFreeHandle(SQL_HANDLE_ENV, env);  // שחרור מאגר הסביבה
    }
}

// ---------------------------------------------------------------------------
// API ציבורי — הפונקציות האלה נקראות מ-TrafficServer.cpp
// ---------------------------------------------------------------------------

// שולפת את כל הצמתות מ-dbo.intersections
// אם אין חיבור ל-DB — מחזירה את DEFAULTS (4 צמתות קבועות)
std::vector<IntersectionRow> db_fetch_intersections()
{
    SQLHENV env  = SQL_NULL_HENV;
    SQLHDBC dbc  = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    std::string db_error;

    if (!open_sql_connection(env, dbc, &db_error)) {
        std::cerr << "[DB] Connection to SQL Server failed (using Python-compatible SQL_* vars): "
                  << db_error << " — using defaults\n";
        return DEFAULTS;  // חיבור נכשל — מחזיר נתוני ברירת מחדל
    }

    // הקצאת statement לביצוע השאילתה
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        close_sql_connection(env, dbc);
        return DEFAULTS;
    }

    // שאילתת SQL — שולפת מזהה, קוד, שם ועיר של כל הצמתות לפי סדר
    const char* sql =
        "SELECT intersection_id, intersection_code, name, city "
        "FROM dbo.intersections "
        "ORDER BY intersection_id";

    if (SQLExecDirectA(stmt,
            reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),
            SQL_NTS) != SQL_SUCCESS) {
        std::cerr << "[DB] Query failed — using defaults\n";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return DEFAULTS;
    }

    // עוברים על כל שורה שהוחזרה ובונים אובייקט IntersectionRow לכל אחת
    std::vector<IntersectionRow> results;
    while (SQLFetch(stmt) == SQL_SUCCESS) {
        IntersectionRow row;

        // עמודה 1: מזהה מספרי של הצומת
        SQLINTEGER id_val = 0;
        SQLLEN     id_ind = 0;
        SQLGetData(stmt, 1, SQL_C_LONG, &id_val, 0, &id_ind);
        row.id = static_cast<int>(id_val);

        // עמודות 2-4: מחרוזות Unicode — ממירים ל-UTF-8
        WCHAR  buf[512] = {};
        SQLLEN ind      = 0;

        SQLGetData(stmt, 2, SQL_C_WCHAR, buf, sizeof(buf), &ind);
        row.code = (ind != SQL_NULL_DATA && buf[0]) ? wstr_to_utf8(buf, ind)
                                                    : ("J-" + std::to_string(id_val));  // קוד NULL — יוצרים קוד אוטומטי

        SQLGetData(stmt, 3, SQL_C_WCHAR, buf, sizeof(buf), &ind);
        row.name = wstr_to_utf8(buf, ind);  // שם הצומת בעברית

        SQLGetData(stmt, 4, SQL_C_WCHAR, buf, sizeof(buf), &ind);
        row.city = wstr_to_utf8(buf, ind);  // שם העיר

        results.push_back(row);
    }

    // ניקוי — שחרור statement וסגירת חיבור
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);

    return results.empty() ? DEFAULTS : results;  // אם לא הוחזרו שורות — ברירת מחדל
}

// ---------------------------------------------------------------------------
// שליפת נתיבי נסיעה (lanes) של צומת מסוים
// ---------------------------------------------------------------------------

// מחזירה את כל הנתיבים של צומת לפי intersection_id
// כל נתיב מכיל: lane_id, camera_index, כיוון (N/S/E/W) ותיאור אופציונלי
std::vector<LaneRow> db_fetch_lanes(int intersection_id)
{
    SQLHENV env   = SQL_NULL_HENV;
    SQLHDBC dbc   = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    std::string db_error;

    if (!open_sql_connection(env, dbc, &db_error)) {
        std::cerr << "[DB] db_fetch_lanes connection failed: " << db_error << "\n";
        return {};  // חיבור נכשל — מחרוזת ריקה
    }

    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        close_sql_connection(env, dbc); return {};
    }

    // שאילתה מוכנה מראש עם פרמטר (? = intersection_id) — מונעת SQL Injection
    const char* sql =
        "SELECT lane_id, camera_index, direction, description "
        "FROM dbo.intersection_lanes "
        "WHERE intersection_id = ? "
        "ORDER BY camera_index, lane_id";

    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    SQLINTEGER param = static_cast<SQLINTEGER>(intersection_id);
    SQLLEN     param_ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &param, 0, &param_ind);  // קושר את intersection_id לפרמטר ?

    std::vector<LaneRow> results;
    if (SQLExecute(stmt) == SQL_SUCCESS) {
        SQLINTEGER iv  = 0; SQLLEN il = 0;
        WCHAR      wbuf[512] = {};

        while (SQLFetch(stmt) == SQL_SUCCESS) {
            LaneRow row;

            SQLGetData(stmt, 1, SQL_C_LONG, &iv, 0, &il);
            row.lane_id = static_cast<int>(iv);

            SQLGetData(stmt, 2, SQL_C_LONG, &iv, 0, &il);
            row.camera_index = static_cast<int>(iv);

            SQLGetData(stmt, 3, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
            row.direction = wstr_to_utf8(wbuf, il);

            SQLGetData(stmt, 4, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
            row.description = (il != SQL_NULL_DATA) ? wstr_to_utf8(wbuf, il) : "";

            results.push_back(row);
        }
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return results;
}

// ---------------------------------------------------------------------------
// שליפת קונפליקטים (נתיבים שאסור להם לפעול במקביל) של צומת מסוים
// ---------------------------------------------------------------------------

// מחזירה רשימת זוגות נתיבים מתנגשים — לפי סדר יורד של תאריך יצירה
std::vector<ConflictRow> db_fetch_conflicts(int intersection_id)
{
    SQLHENV env   = SQL_NULL_HENV;
    SQLHDBC dbc   = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    std::string db_error;

    if (!open_sql_connection(env, dbc, &db_error)) {
        std::cerr << "[DB] db_fetch_conflicts connection failed: " << db_error << "\n";
        return {};
    }

    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        close_sql_connection(env, dbc); return {};
    }

    const char* sql =
        "SELECT conflict_id, lane_id_1, lane_id_2, conflict_type, created_at "
        "FROM dbo.lane_conflicts "
        "WHERE intersection_id = ? "
        "ORDER BY created_at DESC, conflict_id DESC";

    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    SQLINTEGER param = static_cast<SQLINTEGER>(intersection_id);
    SQLLEN     param_ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &param, 0, &param_ind);

    std::vector<ConflictRow> results;
    if (SQLExecute(stmt) == SQL_SUCCESS) {
        SQLINTEGER iv  = 0; SQLLEN il = 0;
        WCHAR      wbuf[512] = {};

        while (SQLFetch(stmt) == SQL_SUCCESS) {
            ConflictRow row;

            SQLGetData(stmt, 1, SQL_C_LONG, &iv, 0, &il);
            row.conflict_id = static_cast<int>(iv);

            SQLGetData(stmt, 2, SQL_C_LONG, &iv, 0, &il);
            row.lane_id_1 = static_cast<int>(iv);

            SQLGetData(stmt, 3, SQL_C_LONG, &iv, 0, &il);
            row.lane_id_2 = static_cast<int>(iv);

            SQLGetData(stmt, 4, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
            row.conflict_type = (il != SQL_NULL_DATA && wbuf[0]) ? wstr_to_utf8(wbuf, il) : "crossing";

            SQLGetData(stmt, 5, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
            row.created_at = (il != SQL_NULL_DATA) ? wstr_to_utf8(wbuf, il) : "";

            results.push_back(row);
        }
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return results;
}

// בודק אם צומת עם המזהה הנתון קיים בטבלת dbo.intersections
// משמש לפני UPDATE/DELETE כדי לדעת להחזיר 404 או 200
bool db_intersection_exists(int intersection_id)
{
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    if (!open_sql_connection(env, dbc)) return false;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql = "SELECT 1 FROM dbo.intersections WHERE intersection_id = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    SQLINTEGER param = static_cast<SQLINTEGER>(intersection_id);
    SQLLEN param_ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &param, 0, &param_ind);

    bool found = false;
    if (SQLExecute(stmt) == SQL_SUCCESS) {
        found = (SQLFetch(stmt) == SQL_SUCCESS);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return found;
}

// מוסיף צומת חדש לטבלת dbo.intersections
// מחזיר true אם ההוספה הצליחה; false + error_out עם תיאור השגיאה אם נכשל
bool db_insert_intersection(
    const std::string& code,
    const std::string& name,
    double latitude,
    double longitude,
    int num_cameras,
    const std::optional<std::string>& city,      // אופציונלי — עיר
    const std::optional<std::string>& region,    // אופציונלי — אזור
    const std::optional<std::string>& description,  // אופציונלי — תיאור חופשי
    std::string& error_out)
{
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    if (!open_sql_connection(env, dbc, &error_out)) return false;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql =
        "INSERT INTO dbo.intersections "
        "(intersection_code, name, latitude, longitude, num_cameras, city, region, description) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    SQLLEN code_ind = SQL_NTS;
    SQLLEN name_ind = SQL_NTS;
    SQLLEN lat_ind = 0;
    SQLLEN lon_ind = 0;
    SQLLEN cams_ind = 0;
    SQLLEN city_ind = city ? SQL_NTS : SQL_NULL_DATA;
    SQLLEN region_ind = region ? SQL_NTS : SQL_NULL_DATA;
    SQLLEN desc_ind = description ? SQL_NTS : SQL_NULL_DATA;

    SQLDOUBLE lat = latitude;
    SQLDOUBLE lon = longitude;
    SQLINTEGER cams = num_cameras;
    std::string city_val = city.value_or("");
    std::string region_val = region.value_or("");
    std::string desc_val = description.value_or("");

    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 50, 0, (SQLPOINTER)code.c_str(), 0, &code_ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 255, 0, (SQLPOINTER)name.c_str(), 0, &name_ind);
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &lat, 0, &lat_ind);
    SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &lon, 0, &lon_ind);
    SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &cams, 0, &cams_ind);
    SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 255, 0, (SQLPOINTER)city_val.c_str(), 0, &city_ind);
    SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 255, 0, (SQLPOINTER)region_val.c_str(), 0, &region_ind);
    SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 1000, 0, (SQLPOINTER)desc_val.c_str(), 0, &desc_ind);

    SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "INSERT failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// מעדכן שדות של צומת קיים — רק השדות שהועברו (optional שאינו ריק)
// found_out = false אם הצומת לא קיים (יגרום להחזרת HTTP 404)
bool db_update_intersection(
    int intersection_id,
    const std::optional<std::string>& name,
    const std::optional<int>& num_cameras,
    const std::optional<std::string>& city,
    const std::optional<std::string>& region,
    const std::optional<std::string>& description,
    bool& found_out,
    std::string& error_out)
{
    found_out = false;
    if (!name && !num_cameras && !city && !region && !description) {
        error_out = "No updatable fields provided";
        return false;
    }

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (!open_sql_connection(env, dbc, &error_out)) return false;

    if (!db_intersection_exists(intersection_id)) {
        close_sql_connection(env, dbc);
        found_out = false;
        return true;
    }
    found_out = true;

    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    std::vector<std::string> set_parts;
    if (name) set_parts.push_back("name = ?");
    if (num_cameras) set_parts.push_back("num_cameras = ?");
    if (city) set_parts.push_back("city = ?");
    if (region) set_parts.push_back("region = ?");
    if (description) set_parts.push_back("description = ?");

    std::string sql = "UPDATE dbo.intersections SET ";
    for (size_t i = 0; i < set_parts.size(); ++i) {
        if (i) sql += ", ";
        sql += set_parts[i];
    }
    sql += " WHERE intersection_id = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(sql.data()), SQL_NTS);

    std::string name_val = name.value_or("");
    SQLINTEGER cams_val = num_cameras.value_or(0);
    std::string city_val = city.value_or("");
    std::string region_val = region.value_or("");
    std::string desc_val = description.value_or("");

    SQLLEN name_ind = name ? SQL_NTS : SQL_NULL_DATA;
    SQLLEN cams_ind = num_cameras ? 0 : SQL_NULL_DATA;
    SQLLEN city_ind = city ? SQL_NTS : SQL_NULL_DATA;
    SQLLEN region_ind = region ? SQL_NTS : SQL_NULL_DATA;
    SQLLEN desc_ind = description ? SQL_NTS : SQL_NULL_DATA;
    SQLLEN id_ind = 0;
    SQLINTEGER id_val = intersection_id;

    SQLUSMALLINT idx = 1;
    if (name) SQLBindParameter(stmt, idx++, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 255, 0, (SQLPOINTER)name_val.c_str(), 0, &name_ind);
    if (num_cameras) SQLBindParameter(stmt, idx++, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &cams_val, 0, &cams_ind);
    if (city) SQLBindParameter(stmt, idx++, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 255, 0, (SQLPOINTER)city_val.c_str(), 0, &city_ind);
    if (region) SQLBindParameter(stmt, idx++, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 255, 0, (SQLPOINTER)region_val.c_str(), 0, &region_ind);
    if (description) SQLBindParameter(stmt, idx++, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 1000, 0, (SQLPOINTER)desc_val.c_str(), 0, &desc_ind);
    SQLBindParameter(stmt, idx, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &id_val, 0, &id_ind);

    SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "UPDATE failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// שולפת את טופולוגיית השכנויות כ-map פשוט: intersection_id → רשימת שכנים
// משמשת לקואורדינציה בין צמתות סמוכים
std::unordered_map<int, std::vector<int>> db_fetch_neighbor_topology()
{
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    if (!open_sql_connection(env, dbc)) return {};
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        close_sql_connection(env, dbc);
        return {};
    }

    const char* sql =
        "SELECT intersection_id, adjacent_intersection_id "
        "FROM dbo.intersection_neighbors "
        "ORDER BY intersection_id, adjacent_intersection_id";

    std::unordered_map<int, std::vector<int>> topology;
    if (SQLExecDirectA(stmt,
            reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),
            SQL_NTS) == SQL_SUCCESS) {
        SQLINTEGER iv = 0;
        SQLLEN il = 0;
        while (SQLFetch(stmt) == SQL_SUCCESS) {
            SQLGetData(stmt, 1, SQL_C_LONG, &iv, 0, &il);
            const int intersection_id = static_cast<int>(iv);

            SQLGetData(stmt, 2, SQL_C_LONG, &iv, 0, &il);
            const int neighbor_id = static_cast<int>(iv);

            auto& neighbors = topology[intersection_id];
            if (std::find(neighbors.begin(), neighbors.end(), neighbor_id) == neighbors.end()) {
                neighbors.push_back(neighbor_id);
            }
        }
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return topology;
}

// שולפת טופולוגיית שכנויות מלאה — כולל כיוון ומרחק לכל שכן
// משמשת לחישוב כיוון קירוב של רכב חירום יחסית לצומת
std::unordered_map<int, std::vector<NeighborRow>> db_fetch_neighbor_full_topology()
{
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    if (!open_sql_connection(env, dbc)) return {};
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        close_sql_connection(env, dbc);
        return {};
    }

    const char* sql =
        "SELECT intersection_id, adjacent_intersection_id, direction_from, distance_m "
        "FROM dbo.intersection_neighbors "
        "ORDER BY intersection_id, adjacent_intersection_id";

    std::unordered_map<int, std::vector<NeighborRow>> topology;
    if (SQLExecDirectA(stmt,
            reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),
            SQL_NTS) == SQL_SUCCESS) {
        SQLINTEGER iv = 0;
        SQLLEN il = 0;
        WCHAR wbuf[32] = {};
        while (SQLFetch(stmt) == SQL_SUCCESS) {
            SQLGetData(stmt, 1, SQL_C_LONG, &iv, 0, &il);
            const int intersection_id = static_cast<int>(iv);

            NeighborRow row;
            SQLGetData(stmt, 2, SQL_C_LONG, &iv, 0, &il);
            row.adjacent_intersection_id = static_cast<int>(iv);

            SQLGetData(stmt, 3, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
            row.direction_from = (il != SQL_NULL_DATA && wbuf[0])
                ? wstr_to_utf8(wbuf, il) : "";

            SQLGetData(stmt, 4, SQL_C_LONG, &iv, 0, &il);
            row.distance_m = (il != SQL_NULL_DATA) ? static_cast<int>(iv) : 0;

            topology[intersection_id].push_back(row);
        }
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return topology;
}

// (CRUD appended below)

// ---------------------------------------------------------------------------
// Intersection GPS locations
// ---------------------------------------------------------------------------

std::vector<IntersectionLocation> db_fetch_all_intersection_locations()// שולפת קואורדינטות GPS של כל הצמתות מ-dbo.intersections
{
    SQLHENV  env  = SQL_NULL_HENV;  // מאגר סביבת ODBC
    SQLHDBC  dbc  = SQL_NULL_HDBC;  // מאגר חיבור ODBC
    SQLHSTMT stmt = SQL_NULL_HSTMT; // מאגר שאילתה ODBC

    if (!open_sql_connection(env, dbc)) return {};// אם החיבור נכשל — מחזירה וקטור ריק
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {// אם הקצאת מאגר השאילתה נכשלה
        close_sql_connection(env, dbc);// סוגרת את החיבור
        return {};// מחזירה וקטור ריק
    }

    const char* sql =// שאילתת SQL לשליפת מזהה, קו רוחב וקו אורך של כל הצמתות
        "SELECT intersection_id, latitude, longitude "
        "FROM dbo.intersections "
        "ORDER BY intersection_id";

    if (SQLExecDirectA(stmt,// מריצה את השאילתה ישירות על ה-statement
            reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),
            SQL_NTS) != SQL_SUCCESS) {// אם השאילתה נכשלה
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);// משחררת את מאגר השאילתה
        close_sql_connection(env, dbc);// סוגרת את החיבור
        return {};// מחזירה וקטור ריק
    }

    std::vector<IntersectionLocation> results;// וקטור לאחסון תוצאות השליפה
    while (SQLFetch(stmt) == SQL_SUCCESS) {// עוברת על כל שורה שהוחזרה מה-DB
        IntersectionLocation loc;// אובייקט זמני לאחסון נתוני שורה אחת
        SQLINTEGER id_val = 0;  // ערך עמודת intersection_id
        SQLDOUBLE  lat = 0.0, lon = 0.0;// ערכי עמודות latitude ו-longitude
        SQLLEN     ind = 0;     // אינדיקטור — מצביע על NULL או אורך הנתון

        SQLGetData(stmt, 1, SQL_C_LONG,   &id_val, 0,       &ind); loc.id        = static_cast<int>(id_val);// שולפת את מזהה הצומת ממשתנה id_val ושומרת ב-loc.id
        SQLGetData(stmt, 2, SQL_C_DOUBLE, &lat,    sizeof(lat), &ind); loc.latitude  = lat;// שולפת קו רוחב ושומרת ב-loc.latitude
        SQLGetData(stmt, 3, SQL_C_DOUBLE, &lon,    sizeof(lon), &ind); loc.longitude = lon;// שולפת קו אורך ושומרת ב-loc.longitude
        results.push_back(loc);// מוסיפה את האובייקט לוקטור התוצאות
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);// משחררת את מאגר השאילתה
    close_sql_connection(env, dbc);// סוגרת את חיבור ה-ODBC
    return results;// מחזירה את רשימת המיקומים שנשלפו
}

// ---------------------------------------------------------------------------
// CRUD לנתיבים — הוספה, עדכון, מחיקה של שורות ב-dbo.intersection_lanes
// ---------------------------------------------------------------------------

// מוסיף נתיב חדש לצומת — camera_index הוא מספר המצלמה, direction הוא N/S/E/W
bool db_insert_lane(
    int intersection_id,
    int camera_index,
    const std::string& direction,
    const std::optional<std::string>& description,
    std::string& error_out)
{
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (!open_sql_connection(env, dbc, &error_out)) return false;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql =
        "INSERT INTO dbo.intersection_lanes "
        "(intersection_id, camera_index, direction, description) "
        "VALUES (?, ?, ?, ?)";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    SQLINTEGER iid = intersection_id;
    SQLINTEGER cam = camera_index;
    SQLLEN     iid_ind = 0, cam_ind = 0;
    SQLLEN     dir_ind = SQL_NTS;
    SQLLEN     desc_ind = description ? SQL_NTS : SQL_NULL_DATA;
    std::string desc_val = description.value_or("");

    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG,   SQL_INTEGER, 0,   0, &iid,                                       0, &iid_ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG,   SQL_INTEGER, 0,   0, &cam,                                       0, &cam_ind);
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR,   SQL_VARCHAR, 32,  0, (SQLPOINTER)direction.c_str(),              0, &dir_ind);
    SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR,   SQL_VARCHAR, 1000,0, (SQLPOINTER)desc_val.c_str(),               0, &desc_ind);

    SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "INSERT into dbo.intersection_lanes failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// עזר פנימי — בודק אם נתיב מסוים שייך לצומת מסוים
// משמש לפני UPDATE/DELETE כדי לוודא שהנתיב קיים
static bool lane_exists_in_intersection(SQLHDBC dbc, int intersection_id, int lane_id)
{
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) return false;

    const char* sql =
        "SELECT 1 FROM dbo.intersection_lanes WHERE lane_id = ? AND intersection_id = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    SQLINTEGER lid = lane_id, iid = intersection_id;
    SQLLEN     lid_ind = 0, iid_ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &lid, 0, &lid_ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &iid, 0, &iid_ind);

    bool found = false;
    if (SQLExecute(stmt) == SQL_SUCCESS) {
        found = (SQLFetch(stmt) == SQL_SUCCESS);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return found;
}

// מעדכן כיוון ו/או תיאור של נתיב קיים
// found_out = false אם הנתיב לא קיים בצומת זה
bool db_update_lane(
    int intersection_id,
    int lane_id,
    const std::optional<std::string>& direction,
    const std::optional<std::string>& description,
    bool& found_out,
    std::string& error_out)
{
    found_out = false;
    if (!direction && !description) {
        error_out = "No fields to update";
        return false;
    }

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    if (!open_sql_connection(env, dbc, &error_out)) return false;

    if (!lane_exists_in_intersection(dbc, intersection_id, lane_id)) {
        close_sql_connection(env, dbc);
        return true; // not an error; route maps to 404 via found_out=false
    }
    found_out = true;

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    std::vector<std::string> parts;
    if (direction)   parts.push_back("direction = ?");
    if (description) parts.push_back("description = ?");
    std::string sql = "UPDATE dbo.intersection_lanes SET ";
    for (size_t i = 0; i < parts.size(); ++i) { if (i) sql += ", "; sql += parts[i]; }
    sql += " WHERE lane_id = ? AND intersection_id = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(sql.data()), SQL_NTS);

    std::string dir_val  = direction.value_or("");
    std::string desc_val = description.value_or("");
    SQLLEN dir_ind  = direction   ? SQL_NTS : SQL_NULL_DATA;
    SQLLEN desc_ind = description ? SQL_NTS : SQL_NULL_DATA;
    SQLINTEGER lid = lane_id, iid = intersection_id;
    SQLLEN lid_ind = 0, iid_ind = 0;

    SQLUSMALLINT idx = 1;
    if (direction)   SQLBindParameter(stmt, idx++, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 32,   0, (SQLPOINTER)dir_val.c_str(),  0, &dir_ind);
    if (description) SQLBindParameter(stmt, idx++, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 1000, 0, (SQLPOINTER)desc_val.c_str(), 0, &desc_ind);
    SQLBindParameter(stmt, idx++, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &lid, 0, &lid_ind);
    SQLBindParameter(stmt, idx,   SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &iid, 0, &iid_ind);

    SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "UPDATE dbo.intersection_lanes failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// מוחק נתיב מהצומת — אם לא קיים, found_out = false (HTTP 404)
bool db_delete_lane(
    int intersection_id,
    int lane_id,
    bool& found_out,
    std::string& error_out)
{
    found_out = false;
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    if (!open_sql_connection(env, dbc, &error_out)) return false;

    if (!lane_exists_in_intersection(dbc, intersection_id, lane_id)) {
        close_sql_connection(env, dbc);
        return true;
    }
    found_out = true;

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql =
        "DELETE FROM dbo.intersection_lanes WHERE lane_id = ? AND intersection_id = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    SQLINTEGER lid = lane_id, iid = intersection_id;
    SQLLEN lid_ind = 0, iid_ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &lid, 0, &lid_ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &iid, 0, &iid_ind);

    SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "DELETE FROM dbo.intersection_lanes failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// ---------------------------------------------------------------------------
// CRUD לקונפליקטים — ניהול זוגות נתיבים שאסורים להם לפעול במקביל
// ---------------------------------------------------------------------------

// מוסיף קונפליקט חדש בין שני נתיבים
// בודק שהנתיבים קיימים ושאין כפילות לפני ההכנסה
bool db_insert_conflict(
    int intersection_id,
    int lane_id_1,
    int lane_id_2,
    const std::string& conflict_type,
    bool& missing_lane_out,
    bool& duplicate_out,
    std::string& error_out)
{
    missing_lane_out = false;
    duplicate_out    = false;

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    if (!open_sql_connection(env, dbc, &error_out)) return false;

    if (!lane_exists_in_intersection(dbc, intersection_id, lane_id_1) ||
        !lane_exists_in_intersection(dbc, intersection_id, lane_id_2))
    {
        missing_lane_out = true;
        close_sql_connection(env, dbc);
        return false;
    }

    // Duplicate check (pair is already ordered by caller).
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) == SQL_SUCCESS) {
            const char* sql =
                "SELECT conflict_id FROM dbo.lane_conflicts "
                "WHERE intersection_id = ? AND lane_id_1 = ? AND lane_id_2 = ?";
            SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
            SQLINTEGER iid = intersection_id, l1 = lane_id_1, l2 = lane_id_2;
            SQLLEN ind = 0;
            SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &iid, 0, &ind);
            SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &l1,  0, &ind);
            SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &l2,  0, &ind);
            if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
                duplicate_out = true;
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                close_sql_connection(env, dbc);
                return false;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        }
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql =
        "INSERT INTO dbo.lane_conflicts (intersection_id, lane_id_1, lane_id_2, conflict_type) "
        "VALUES (?, ?, ?, ?)";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    SQLINTEGER iid = intersection_id, l1 = lane_id_1, l2 = lane_id_2;
    SQLLEN ind = 0, ctype_ind = SQL_NTS;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0,  0, &iid, 0, &ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0,  0, &l1,  0, &ind);
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0,  0, &l2,  0, &ind);
    SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0,
                     (SQLPOINTER)conflict_type.c_str(), 0, &ctype_ind);

    SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "INSERT into dbo.lane_conflicts failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// מוחק קונפליקט לפי conflict_id ו-intersection_id
// found_out = false אם הקונפליקט לא קיים
bool db_delete_conflict(
    int intersection_id,
    int conflict_id,
    bool& found_out,
    std::string& error_out)
{
    found_out = false;
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    if (!open_sql_connection(env, dbc, &error_out)) return false;

    // existence check
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) == SQL_SUCCESS) {
            const char* sql =
                "SELECT 1 FROM dbo.lane_conflicts "
                "WHERE conflict_id = ? AND intersection_id = ?";
            SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
            SQLINTEGER cid = conflict_id, iid = intersection_id;
            SQLLEN ind = 0;
            SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &cid, 0, &ind);
            SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &iid, 0, &ind);
            if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
                found_out = true;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        }
    }
    if (!found_out) {
        close_sql_connection(env, dbc);
        return true;
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }
    const char* sql =
        "DELETE FROM dbo.lane_conflicts WHERE conflict_id = ? AND intersection_id = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    SQLINTEGER cid = conflict_id, iid = intersection_id;
    SQLLEN ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &cid, 0, &ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &iid, 0, &ind);

    SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "DELETE FROM dbo.lane_conflicts failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// ---------------------------------------------------------------------------
// ניהול משתמשי אדמין — CRUD על טבלת dbo.admin_users
// כל המשתמשים מאוחסנים עם password_hash + salt (לא סיסמה בטקסט ברור)
// ---------------------------------------------------------------------------

// מחזירה רשימת כל משתמשי האדמין (ללא סיסמאות — רק user_id, username, role, תאריכים)
std::vector<AdminUserRow> db_list_admin_users()
{
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    std::string db_error;

    if (!open_sql_connection(env, dbc, &db_error)) {
        std::cerr << "[DB] db_list_admin_users connection failed: " << db_error << "\n";
        return {};
    }
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        close_sql_connection(env, dbc);
        return {};
    }

    const char* sql =
        "SELECT user_id, username, ISNULL(role, 'regular_admin') AS role, created_at, last_login "
        "FROM dbo.admin_users "
        "ORDER BY user_id";

    std::vector<AdminUserRow> rows;
    if (SQLExecDirectA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS) == SQL_SUCCESS) {
        SQLINTEGER iv = 0;
        SQLLEN il = 0;
        WCHAR wbuf[512] = {};

        while (SQLFetch(stmt) == SQL_SUCCESS) {
            AdminUserRow row;

            SQLGetData(stmt, 1, SQL_C_LONG, &iv, 0, &il);
            row.user_id = static_cast<int>(iv);

            SQLGetData(stmt, 2, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
            row.username = (il != SQL_NULL_DATA) ? wstr_to_utf8(wbuf, il) : "";

            SQLGetData(stmt, 3, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
            row.role = (il != SQL_NULL_DATA) ? wstr_to_utf8(wbuf, il) : "regular_admin";

            SQLGetData(stmt, 4, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
            row.created_at = (il != SQL_NULL_DATA) ? wstr_to_utf8(wbuf, il) : "";

            SQLGetData(stmt, 5, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
            row.last_login = (il != SQL_NULL_DATA) ? wstr_to_utf8(wbuf, il) : "";

            rows.push_back(row);
        }
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return rows;
}

// שולפת את hash הסיסמה וה-salt של משתמש לפי שם משתמש
// משמשת בלוגין — מחשבים SHA-256(salt + password) ומשווים ל-hash
bool db_get_admin_credentials(
    const std::string& username,
    std::string& password_hash_out,
    std::string& salt_out)
{
    password_hash_out.clear();
    salt_out.clear();

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    if (!open_sql_connection(env, dbc)) return false;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql =
        "SELECT password_hash, salt "
        "FROM dbo.admin_users "
        "WHERE username = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    SQLLEN username_ind = SQL_NTS;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 50, 0,
                     (SQLPOINTER)username.c_str(), 0, &username_ind);

    bool found = false;
    if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
        WCHAR wbuf[256] = {};
        SQLLEN il = 0;

        SQLGetData(stmt, 1, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
        password_hash_out = (il != SQL_NULL_DATA) ? wstr_to_utf8(wbuf, il) : "";

        SQLGetData(stmt, 2, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
        salt_out = (il != SQL_NULL_DATA) ? wstr_to_utf8(wbuf, il) : "";

        found = !password_hash_out.empty() && !salt_out.empty();
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return found;
}

// מעדכן את שדה last_login ל-SYSUTCDATETIME() (זמן UTC נוכחי)
// נקרא מיד אחרי אימות מוצלח של משתמש
bool db_touch_admin_last_login(const std::string& username)
{
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    std::string error_out;

    if (!open_sql_connection(env, dbc, &error_out)) return false;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql =
        "UPDATE dbo.admin_users "
        "SET last_login = SYSUTCDATETIME() "
        "WHERE username = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    SQLLEN username_ind = SQL_NTS;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 50, 0,
                     (SQLPOINTER)username.c_str(), 0, &username_ind);

    const SQLRETURN rc = SQLExecute(stmt);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        close_sql_connection(env, dbc);
        return true;
    }

    close_sql_connection(env, dbc);
    return false;
}

// מוסיף משתמש אדמין חדש לטבלה
// בודק כפילות username לפני ההכנסה
// role מוגבל ל-'super_admin' או 'regular_admin' בלבד (הגנה מ-injection בשדה role)
bool db_insert_admin_user(
    const std::string& username,
    const std::string& password_hash,
    const std::string& salt,
    int& user_id_out,
    bool& duplicate_username_out,
    std::string& error_out,
    const std::string& role)
{
    user_id_out = 0;
    duplicate_username_out = false;

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    if (!open_sql_connection(env, dbc, &error_out)) return false;

    // Duplicate check by username.
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
            error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
            close_sql_connection(env, dbc);
            return false;
        }

        const char* sql = "SELECT user_id FROM dbo.admin_users WHERE username = ?";
        SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

        SQLLEN username_ind = SQL_NTS;
        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 50, 0,
                         (SQLPOINTER)username.c_str(), 0, &username_ind);

        if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
            duplicate_username_out = true;
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            close_sql_connection(env, dbc);
            return false;
        }
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const std::string safe_role = (role == "super_admin") ? "super_admin" : "regular_admin";
    const char* sql =
        "INSERT INTO dbo.admin_users (username, password_hash, salt, role) "
        "VALUES (?, ?, ?, ?)";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    SQLLEN username_ind = SQL_NTS;
    SQLLEN hash_ind = SQL_NTS;
    SQLLEN salt_ind = SQL_NTS;
    SQLLEN role_ind = SQL_NTS;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 50, 0,
                     (SQLPOINTER)username.c_str(), 0, &username_ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0,
                     (SQLPOINTER)password_hash.c_str(), 0, &hash_ind);
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 32, 0,
                     (SQLPOINTER)salt.c_str(), 0, &salt_ind);
    SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 20, 0,
                     (SQLPOINTER)safe_role.c_str(), 0, &role_ind);

    SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "INSERT into dbo.admin_users failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql_get = "SELECT user_id FROM dbo.admin_users WHERE username = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql_get)), SQL_NTS);
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 50, 0,
                     (SQLPOINTER)username.c_str(), 0, &username_ind);

    bool got_id = false;
    if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
        SQLINTEGER uid = 0;
        SQLLEN uid_ind = 0;
        SQLGetData(stmt, 1, SQL_C_LONG, &uid, 0, &uid_ind);
        user_id_out = static_cast<int>(uid);
        got_id = true;
    }

    if (!got_id) {
        error_out = "Failed to read inserted admin user id";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// מחזירה את ה-role של משתמש (super_admin / regular_admin)
// משמשת לאחר הלוגין לבדיקת הרשאות
bool db_get_admin_role(
    const std::string& username,
    std::string& role_out,
    std::string& error_out)
{
    role_out = "regular_admin";

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (!open_sql_connection(env, dbc, &error_out)) return false;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql =
        "SELECT ISNULL(role, 'regular_admin') "
        "FROM dbo.admin_users WHERE username = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    SQLLEN username_ind = SQL_NTS;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 50, 0,
                     (SQLPOINTER)username.c_str(), 0, &username_ind);

    if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
        WCHAR wbuf[64] = {};
        SQLLEN il = 0;
        SQLGetData(stmt, 1, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
        if (il != SQL_NULL_DATA) {
            role_out = wstr_to_utf8(wbuf, il);
        }
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// מוחק משתמש אדמין לפי user_id
// found_out = false אם המשתמש לא קיים
bool db_delete_admin_user(
    int user_id,
    bool& found_out,
    std::string& error_out)
{
    found_out = false;

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    if (!open_sql_connection(env, dbc, &error_out)) return false;

    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) == SQL_SUCCESS) {
            const char* sql = "SELECT 1 FROM dbo.admin_users WHERE user_id = ?";
            SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
            SQLINTEGER uid = user_id;
            SQLLEN uid_ind = 0;
            SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &uid, 0, &uid_ind);
            if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
                found_out = true;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        }
    }

    if (!found_out) {
        close_sql_connection(env, dbc);
        return true;
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql = "DELETE FROM dbo.admin_users WHERE user_id = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    SQLINTEGER uid = user_id;
    SQLLEN uid_ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &uid, 0, &uid_ind);

    const SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "DELETE FROM dbo.admin_users failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// מעדכן סיסמה של משתמש — שומר hash + salt חדשים (לא הסיסמה עצמה)
bool db_update_admin_user_password(
    int user_id,
    const std::string& password_hash,
    const std::string& salt,
    bool& found_out,
    std::string& error_out)
{
    found_out = false;

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    if (!open_sql_connection(env, dbc, &error_out)) return false;

    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) == SQL_SUCCESS) {
            const char* sql = "SELECT 1 FROM dbo.admin_users WHERE user_id = ?";
            SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
            SQLINTEGER uid = user_id;
            SQLLEN uid_ind = 0;
            SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &uid, 0, &uid_ind);
            if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
                found_out = true;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        }
    }

    if (!found_out) {
        close_sql_connection(env, dbc);
        return true;
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql =
        "UPDATE dbo.admin_users "
        "SET password_hash = ?, salt = ? "
        "WHERE user_id = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    SQLLEN hash_ind = SQL_NTS;
    SQLLEN salt_ind = SQL_NTS;
    SQLINTEGER uid = user_id;
    SQLLEN uid_ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0,
                     (SQLPOINTER)password_hash.c_str(), 0, &hash_ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 32, 0,
                     (SQLPOINTER)salt.c_str(), 0, &salt_ind);
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0,
                     &uid, 0, &uid_ind);

    const SQLRETURN rc = SQLExecute(stmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        error_out = "UPDATE dbo.admin_users password failed";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        close_sql_connection(env, dbc);
        return false;
    }

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// מחפש username לפי user_id — משמש להצגת שם בממשק
bool db_get_admin_username_by_id(
    int user_id,
    std::string& username_out,
    bool& found_out,
    std::string& error_out)
{
    username_out.clear();
    found_out = false;

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (!open_sql_connection(env, dbc, &error_out)) return false;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql = "SELECT username FROM dbo.admin_users WHERE user_id = ?";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    SQLINTEGER uid = user_id;
    SQLLEN uid_ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &uid, 0, &uid_ind);

    if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
        WCHAR wbuf[256] = {};
        SQLLEN il = 0;
        SQLGetData(stmt, 1, SQL_C_WCHAR, wbuf, sizeof(wbuf), &il);
        username_out = (il != SQL_NULL_DATA) ? wstr_to_utf8(wbuf, il) : "";
        found_out = !username_out.empty();
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}

// סופר כמה משתמשי אדמין קיימים במערכת
// משמש לבדיקה אם צריך ליצור משתמש ראשון (bootstrap)
bool db_count_admin_users(int& count_out, std::string& error_out)
{
    count_out = 0;

    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (!open_sql_connection(env, dbc, &error_out)) return false;
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        error_out = "SQLAllocHandle(SQL_HANDLE_STMT) failed";
        close_sql_connection(env, dbc);
        return false;
    }

    const char* sql = "SELECT COUNT(*) FROM dbo.admin_users";
    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);

    if (SQLExecute(stmt) == SQL_SUCCESS && SQLFetch(stmt) == SQL_SUCCESS) {
        SQLINTEGER count_value = 0;
        SQLLEN count_ind = 0;
        SQLGetData(stmt, 1, SQL_C_LONG, &count_value, 0, &count_ind);
        count_out = static_cast<int>(count_value);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    close_sql_connection(env, dbc);
    return true;
}
