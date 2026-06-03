#include "Database.h"

#include <windows.h>
#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Hardcoded fallback (mirrors Python DEFAULT_INTERSECTIONS)
// ---------------------------------------------------------------------------
static const std::vector<IntersectionRow> DEFAULTS = {
    {1, "J-001", "\xd7\xa6\xd7\x95\xd7\x9e\xd7\xaa \xd7\x9e\xd7\xa8\xd7\x9b\xd7\x96\xd7\x99",          "\xd7\x91\xd7\x90\xd7\xa8 \xd7\xa9\xd7\x91\xd7\xa2"},
    {2, "J-002", "\xd7\xa6\xd7\x95\xd7\x9e\xd7\xaa \xd7\x94\xd7\x90\xd7\x95\xd7\xa0\xd7\x99\xd7\x91\xd7\xa8\xd7\xa1\xd7\x99\xd7\x98\xd7\x94", "\xd7\x91\xd7\x90\xd7\xa8 \xd7\xa9\xd7\x91\xd7\xa2"},
    {3, "J-003", "\xd7\xa6\xd7\x95\xd7\x9e\xd7\xaa \xd7\x91\xd7\x99\xd7\xaa \xd7\x97\xd7\x95\xd7\x9c\xd7\x99\xd7\x9d",   "\xd7\x91\xd7\x90\xd7\xa8 \xd7\xa9\xd7\x91\xd7\xa2"},
    {4, "J-004", "\xd7\xa6\xd7\x95\xd7\x9e\xd7\xaa \xd7\x93\xd7\xa8\xd7\x95\xd7\x9e\xd7\x99",           "\xd7\x91\xd7\x90\xd7\xa8 \xd7\xa9\xd7\x91\xd7\xa2"},
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string env_or(const char* name, const char* def)
{
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : def;
}

// Convert a null-terminated wide string to UTF-8.
static std::string wstr_to_utf8(const WCHAR* wstr, SQLLEN ind)
{
    if (ind == SQL_NULL_DATA || wstr == nullptr || wstr[0] == L'\0')
        return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return "";
    std::string out(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &out[0], sz, nullptr, nullptr);
    return out;
}

static std::string build_conn_str()
{
    std::string server = env_or("SQL_SERVER",   R"(.\SQLEXPRESS)");
    std::string db     = env_or("SQL_DATABASE", "smart_traffic");
    std::string user   = env_or("SQL_USER",     "");
    std::string pass   = env_or("SQL_PASSWORD", "");

    std::string s = "DRIVER={ODBC Driver 17 for SQL Server};"
                    "SERVER=" + server + ";"
                    "DATABASE=" + db + ";";
    if (!user.empty())
        s += "UID=" + user + ";PWD=" + pass + ";";
    else
        s += "Trusted_Connection=yes;";
    return s;
}

static bool open_sql_connection(SQLHENV& env, SQLHDBC& dbc, std::string* error_out = nullptr)
{
    env = SQL_NULL_HENV;
    dbc = SQL_NULL_HDBC;

    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) != SQL_SUCCESS) {
        if (error_out) *error_out = "SQLAllocHandle(SQL_HANDLE_ENV) failed";
        return false;
    }
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

    if (SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) {
        if (error_out) *error_out = "SQLAllocHandle(SQL_HANDLE_DBC) failed";
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        env = SQL_NULL_HENV;
        return false;
    }

    std::string cs = build_conn_str();
    SQLRETURN rc = SQLDriverConnectA(
        dbc, NULL,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(cs.c_str())),
        static_cast<SQLSMALLINT>(cs.size()),
        NULL, 0, NULL,
        SQL_DRIVER_NOPROMPT
    );
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        if (error_out) *error_out = "SQLDriverConnect failed";
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        env = SQL_NULL_HENV;
        dbc = SQL_NULL_HDBC;
        return false;
    }

    return true;
}

static void close_sql_connection(SQLHENV env, SQLHDBC dbc)
{
    if (dbc != SQL_NULL_HDBC) {
        SQLDisconnect(dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    }
    if (env != SQL_NULL_HENV) {
        SQLFreeHandle(SQL_HANDLE_ENV, env);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<IntersectionRow> db_fetch_intersections()
{
    SQLHENV env  = SQL_NULL_HENV;
    SQLHDBC dbc  = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    // Allocate environment
    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) != SQL_SUCCESS)
        return DEFAULTS;
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

    // Allocate connection
    if (SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) {
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return DEFAULTS;
    }

    // Connect
    std::string cs = build_conn_str();
    SQLRETURN rc = SQLDriverConnectA(
        dbc, NULL,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(cs.c_str())),
        static_cast<SQLSMALLINT>(cs.size()),
        NULL, 0, NULL,
        SQL_DRIVER_NOPROMPT
    );
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        std::cerr << "[DB] Connection to SQL Server failed — using defaults\n";
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return DEFAULTS;
    }

    // Allocate statement
    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        SQLDisconnect(dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return DEFAULTS;
    }

    // Execute query
    const char* sql =
        "SELECT intersection_id, intersection_code, name, city "
        "FROM dbo.intersections "
        "ORDER BY intersection_id";

    if (SQLExecDirectA(stmt,
            reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),
            SQL_NTS) != SQL_SUCCESS) {
        std::cerr << "[DB] Query failed — using defaults\n";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLDisconnect(dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return DEFAULTS;
    }

    // Fetch rows
    std::vector<IntersectionRow> results;
    while (SQLFetch(stmt) == SQL_SUCCESS) {
        IntersectionRow row;

        // Column 1: intersection_id (integer)
        SQLINTEGER id_val = 0;
        SQLLEN     id_ind = 0;
        SQLGetData(stmt, 1, SQL_C_LONG, &id_val, 0, &id_ind);
        row.id = static_cast<int>(id_val);

        // Columns 2-4: wide strings -> UTF-8
        WCHAR  buf[512] = {};
        SQLLEN ind      = 0;

        SQLGetData(stmt, 2, SQL_C_WCHAR, buf, sizeof(buf), &ind);
        row.code = (ind != SQL_NULL_DATA && buf[0]) ? wstr_to_utf8(buf, ind)
                                                    : ("J-" + std::to_string(id_val));

        SQLGetData(stmt, 3, SQL_C_WCHAR, buf, sizeof(buf), &ind);
        row.name = wstr_to_utf8(buf, ind);

        SQLGetData(stmt, 4, SQL_C_WCHAR, buf, sizeof(buf), &ind);
        row.city = wstr_to_utf8(buf, ind);

        results.push_back(row);
    }

    // Cleanup
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLDisconnect(dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);

    return results.empty() ? DEFAULTS : results;
}

// ---------------------------------------------------------------------------
// db_fetch_lanes
// ---------------------------------------------------------------------------

std::vector<LaneRow> db_fetch_lanes(int intersection_id)
{
    SQLHENV env   = SQL_NULL_HENV;
    SQLHDBC dbc   = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) != SQL_SUCCESS)
        return {};
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) {
        SQLFreeHandle(SQL_HANDLE_ENV, env); return {};
    }

    std::string cs = build_conn_str();
    SQLRETURN rc = SQLDriverConnectA(dbc, NULL,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(cs.c_str())),
        static_cast<SQLSMALLINT>(cs.size()), NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        SQLFreeHandle(SQL_HANDLE_DBC, dbc); SQLFreeHandle(SQL_HANDLE_ENV, env);
        return {};
    }

    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env); return {};
    }

    // Parameterised query
    const char* sql =
        "SELECT lane_id, camera_index, direction, description "
        "FROM dbo.intersection_lanes "
        "WHERE intersection_id = ? "
        "ORDER BY camera_index, lane_id";

    SQLPrepareA(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
    SQLINTEGER param = static_cast<SQLINTEGER>(intersection_id);
    SQLLEN     param_ind = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &param, 0, &param_ind);

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
    SQLDisconnect(dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
    return results;
}

// ---------------------------------------------------------------------------
// db_fetch_conflicts
// ---------------------------------------------------------------------------

std::vector<ConflictRow> db_fetch_conflicts(int intersection_id)
{
    SQLHENV env   = SQL_NULL_HENV;
    SQLHDBC dbc   = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;

    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) != SQL_SUCCESS)
        return {};
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) {
        SQLFreeHandle(SQL_HANDLE_ENV, env); return {};
    }

    std::string cs = build_conn_str();
    SQLRETURN rc = SQLDriverConnectA(dbc, NULL,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(cs.c_str())),
        static_cast<SQLSMALLINT>(cs.size()), NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        SQLFreeHandle(SQL_HANDLE_DBC, dbc); SQLFreeHandle(SQL_HANDLE_ENV, env);
        return {};
    }

    if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
        SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env); return {};
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
    SQLDisconnect(dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
    return results;
}

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

bool db_insert_intersection(
    const std::string& code,
    const std::string& name,
    double latitude,
    double longitude,
    int num_cameras,
    const std::optional<std::string>& city,
    const std::optional<std::string>& region,
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
