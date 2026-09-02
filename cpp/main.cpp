/**
 * main.cpp
 * --------
 * Unified controller entrypoint (single RL path):
 * - traffic::Junction
 * - traffic::RLAgent
 */

#include "http_client.h"
#include "Junction.h"
#include "RLAgent.h"
#include "Simulation.h"
#include "ConflictConfig.h"
#include "GreedyAgingController.h"
#include "NeighborCoordConfig.h"
#include "PhaseConfig.h"
#include "SelfTests.h"
#include "ThresholdConfig.h"
#include "TrafficConstants.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <cctype>
#include <cstdint>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace {

struct ParsedLane {// מידע על נתיב בצומת
    int laneId = -1;// מזהה הנתיב
    int vehicleCount = 0;// מספר הרכבים בנתיב
    double densityPct = 0.0;// אחוז הצפיפות בנתיב
    double waitingSec = 0.0;// זמן ההמתנה בנתיב
};

struct ParsedPacketState {// מידע על מצב החבילה שנשלחה מהצומת
    int intersectionId = 1;// מזהה הצומת
    double timestamp = 0.0;// חותמת זמן של המצב
    bool emergencyActive = false;// מציין אם יש מצב חירום פעיל בצומת
    std::optional<int> emergencyLaneId;// מזהה הנתיב שבו יש מצב חירום, אם קיים
    std::vector<ParsedLane> lanes;// רשימת הנתיבים בצומת
    std::vector<traffic::NeighborSignal> neighbors;// רשימת האותות מהשכנים
};

struct NeighborAuthConfig {// קונפיגורציה לאימות הודעות מהשכנים
    std::string sharedKey = "demo-neighbor-message-key";// מפתח משותף לאימות הודעות מהשכנים
    double maxSignatureSkewSec = traffic::constants::kDefaultNeighborSignatureSkewSec;// ההפרש המקסימלי המותר בין חותמות הזמן של ההודעות
    std::string source = "defaults";// מקור הקונפיגורציה
};

std::optional<std::string> read_text_file(const std::string& path) {// פונקציה שקוראת קובץ טקסט ומחזירה את תוכנו
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

NeighborAuthConfig load_neighbor_auth_config() {// פונקציה שמטענת את קונפיגורציית האימות של הודעות השכנים
    const std::vector<std::string> candidates = {// רשימת נתיבים אפשריים לקובץ הקונפיגורציה של האימות
        "neighbor_message_auth.json",
        "cpp/neighbor_message_auth.json",
        "../neighbor_message_auth.json",
        "../../neighbor_message_auth.json",
        "python/server/neighbor_message_auth.json",
    };

    NeighborAuthConfig cfg;// אובייקט קונפיגורציה עם ערכי ברירת מחדל
    for (const auto& path : candidates) {// לולאה שעוברת על כל הנתיבים האפשריים
        const auto text = read_text_file(path);// קוראת את תוכן הקובץ אם הוא קיים
        if (!text.has_value()) continue;

        std::smatch m;// משתנה לאחסון תוצאות החיפוש של regex
        std::regex keyRe("\\\"shared_hmac_key\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");// regex שמחפש את המפתח המשותף לאימות ההודעות מהשכנים
        if (!std::regex_search(*text, m, keyRe)) {// אם לא נמצא המפתח המשותף, ממשיכה לנתיב הבא
            continue;
        }
        cfg.sharedKey = m[1].str();// מעדכנת את המפתח המשותף בקונפיגורציה

        std::regex skewRe("\\\"max_signature_skew_sec\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");// regex שמחפש את ההפרש המקסימלי המותר בין חותמות הזמן של ההודעות
        if (std::regex_search(*text, m, skewRe)) {
            try {
                cfg.maxSignatureSkewSec = std::max(1.0, std::stod(m[1].str()));// מעדכנת את ההפרש המקסימלי המותר בין חותמות הזמן של ההודעות
            } catch (...) {
                cfg.maxSignatureSkewSec = traffic::constants::kDefaultNeighborSignatureSkewSec;// במקרה של שגיאה, משתמשת בערך ברירת מחדל
            }
        }

        cfg.source = path;// מעדכנת את מקור הקונפיגורציה
        return cfg;// מחזירה את הקונפיגורציה שנמצאה
    }

    return cfg;
}

std::string to_hex_lower(const std::vector<unsigned char>& data) {// פונקציה שממירה מערך בתים למחרוזת הקסדצימלית באותיות קטנות
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char b : data) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string to_lower_copy(std::string s) {// פונקציה שממירה מחרוזת לאותיות קטנות
    for (auto& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}
// פונקציה שמחשבת את ה-HMAC-SHA256 של מחרוזת ומחזירה את התוצאה כמחרוזת הקסדצימלית באותיות קטנות
std::optional<std::string> hmac_sha256_hex(const std::string& key, const std::string& payload) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) return std::nullopt;

    DWORD objLen = 0;
    DWORD hashLen = 0;
    DWORD cbResult = 0;
    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cbResult, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::nullopt;
    }

    status = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cbResult, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::nullopt;
    }

    std::vector<unsigned char> obj(objLen);
    std::vector<unsigned char> hashBytes(hashLen);

    status = BCryptCreateHash(
        alg,
        &hash,
        obj.data(),
        objLen,
        reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
        static_cast<ULONG>(key.size()),
        0
    );
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::nullopt;
    }

    status = BCryptHashData(
        hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(payload.data())),
        static_cast<ULONG>(payload.size()),
        0
    );
    if (status >= 0) {
        status = BCryptFinishHash(hash, hashBytes.data(), hashLen, 0);
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (status < 0) return std::nullopt;
    return to_hex_lower(hashBytes);
#else
    (void)key;
    (void)payload;
    return std::nullopt;
#endif
}

bool signature_equals(std::string a, std::string b) {// פונקציה שמבצעת השוואה בין שתי מחרוזות בצורה שאינה תלויה באותיות גדולות או קטנות
    a = to_lower_copy(std::move(a));// ממירה את המחרוזת הראשונה לאותיות קטנות
    b = to_lower_copy(std::move(b));//  ממירה את המחרוזת השנייה לאותיות קטנות
    return a == b;
}

std::string canonical_neighbor_payload(const traffic::NeighborSignal& n) {// פונקציה שמייצרת מחרוזת ייחודית שמייצגת את המידע על השכן, לשם אימות החתימה הדיגיטלית
    std::ostringstream oss;// יוצר סטרינג סטרים לאגירת המידע על השכן
    oss.setf(std::ios::fixed);// הגדרת פורמט המספרים הממשיים לכתיבה בסטרינג סטרים
    oss.precision(3);// הגדרת דיוק המספרים הממשיים לכתיבה בסטרינג סטרים
    oss << n.intersectionId// מזהה הצומת של השכן
        << "|" << n.phaseId// מזהה השלב של השכן
        << "|" << n.totalQueue// מספר הרכבים בתור הכולל של השכן
        << "|" << n.avgWaitingSec// זמן ההמתנה הממוצע של השכן
        << "|" << (n.emergencyActive ? 1 : 0)// האם מצב חירום פעיל אצל השכן
        << "|" << n.signedAtSec;// זמן החתימה של השכן
    return oss.str();// מחזירה את המחרוזת הייחודית שמייצגת את המידע על השכן
}

bool verify_neighbor_signature(// פונקציה שמבצעת אימות של החתימה הדיגיטלית של השכן
    const traffic::NeighborSignal& neighbor,// מידע על השכן
    const std::string& signature,// החתימה הדיגיטלית שנשלחה מהשכן
    const NeighborAuthConfig& auth// קונפיגורציה לאימות הודעות מהשכנים
) {
    const double now = std::chrono::duration<double>(// פונקציה שמחזירה את הזמן הנוכחי בשניות מאז תחילת האפוק
        std::chrono::system_clock::now().time_since_epoch()// החזרת הזמן הנוכחי מאז תחילת האפוק
    ).count();// החזרת הזמן הנוכחי בשניות מאז תחילת האפוק

    if (std::abs(now - neighbor.signedAtSec) > auth.maxSignatureSkewSec) {// אם ההפרש בין הזמן הנוכחי לבין זמן החתימה של השכן גדול מההפרש המקסימלי המותר, מחזירה false
        return false;// מחזירה false אם ההפרש בין הזמן הנוכחי לבין זמן החתימה של השכן גדול מההפרש המקסימלי המותר
    }

    const auto expected = hmac_sha256_hex(auth.sharedKey, canonical_neighbor_payload(neighbor));// מחשבת את החתימה הצפויה של השכן על פי המפתח המשותף והמידע על השכן
    if (!expected.has_value()) {// אם לא ניתן לחשב את החתימה הצפויה, מחזירה false
        return false;// מחזירה false אם לא ניתן לחשב את החתימה הצפויה
    }

    return signature_equals(*expected, signature);// מחזירה true אם החתימה הצפויה תואמת לחתימה שנשלחה מהשכן, אחרת מחזירה false
}

std::optional<std::string> extract_json_object(const std::string& json, const std::string& key) {// פונקציה שמחזירה אובייקט JSON לפי מפתח
    const std::string token = "\"" + key + "\"";// יוצר את המחרוזת שמייצגת את המפתח ב-JSON
    const std::size_t kpos = json.find(token);// מחפש את המיקום של המפתח ב-JSON
    if (kpos == std::string::npos) return std::nullopt;// אם המפתח לא נמצא, מחזירה nullopt

    std::size_t pos = json.find('{', kpos);// מחפש את המיקום של הסוגר הפותח של האובייקט JSON
    if (pos == std::string::npos) return std::nullopt;// אם הסוגר הפותח לא נמצא, מחזירה nullopt

    int depth = 0;// משתנה שמייצג את עומק הסוגריים של האובייקט JSON
    const std::size_t start = pos;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '{') depth++;
        else if (json[pos] == '}') {
            depth--;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);// מחזירה את האובייקט JSON שמצאנו
            }
        }
    }
    return std::nullopt;// אם לא מצאנו את הסוגר הסוגר של האובייקט JSON, מחזירה nullopt
}

std::optional<std::string> extract_json_array(const std::string& json, const std::string& key) {// פונקציה שמחזירה מערך JSON לפי מפתח
    const std::string token = "\"" + key + "\"";// יוצר את המחרוזת שמייצגת את המפתח ב-JSON
    const std::size_t kpos = json.find(token);// מחפש את המיקום של המפתח ב-JSON
    if (kpos == std::string::npos) return std::nullopt;// אם המפתח לא נמצא, מחזירה nullopt

    std::size_t pos = json.find('[', kpos);// מחפש את המיקום של הסוגר הפותח של המערך JSON
    if (pos == std::string::npos) return std::nullopt;// אם הסוגר הפותח לא נמצא, מחזירה nullopt

    int depth = 0;// משתנה שמייצג את עומק הסוגריים של המערך JSON
    const std::size_t start = pos;// שמירת המיקום של הסוגר הפותח של המערך JSON
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '[') depth++;
        else if (json[pos] == ']') {
            depth--;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);// מחזירה את המערך JSON שמצאנו
            }
        }
    }
    return std::nullopt;// אם לא מצאנו את הסוגר הסוגר של המערך JSON, מחזירה nullopt
}

std::optional<int> extract_int_field(const std::string& json, const std::string& key) {// פונקציה שמחזירה ערך מספרי שלם לפי מפתח
    std::regex re("\\\"" + key + "\\\"\\s*:\\s*(-?\\d+)");// regex שמחפש את המפתח ואת הערך המספרי שלם שלו ב-JSON
    std::smatch m;
    if (std::regex_search(json, m, re)) return std::stoi(m[1].str());// אם מצאנו את המפתח ואת הערך המספרי שלם שלו, מחזירה את הערך המספרי שלם
    return std::nullopt;// אם לא מצאנו את המפתח או את הערך המספרי שלם שלו, מחזירה nullopt
}

std::optional<double> extract_double_field(const std::string& json, const std::string& key) {// פונקציה שמחזירה ערך מספרי ממשי לפי מפתח
    std::regex re("\\\"" + key + "\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");// regex שמחפש את המפתח ואת הערך המספרי הממשי שלו ב-JSON
    std::smatch m;
    if (std::regex_search(json, m, re)) return std::stod(m[1].str());// אם מצאנו את המפתח ואת הערך המספרי הממשי שלו, מחזירה את הערך המספרי הממשי
    return std::nullopt;// אם לא מצאנו את המפתח או את הערך המספרי הממשי שלו, מחזירה nullopt
}

std::optional<bool> extract_bool_field(const std::string& json, const std::string& key) {// פונקציה שמחזירה ערך בוליאני לפי מפתח
    std::regex re("\\\"" + key + "\\\"\\s*:\\s*(true|false)");// regex שמחפש את המפתח ואת הערך הבוליאני שלו ב-JSON
    std::smatch m;
    if (std::regex_search(json, m, re)) return m[1].str() == "true";// אם מצאנו את המפתח ואת הערך הבוליאני שלו, מחזירה את הערך הבוליאני
    return std::nullopt;// אם לא מצאנו את המפתח או את הערך הבוליאני שלו, מחזירה nullopt
}

bool parse_packet_state(const std::string& packetJson, ParsedPacketState& out, const NeighborAuthConfig& neighborAuth) {// פונקציה שמפרקת את המידע על מצב החבילה שנשלחה מהצומת ומאמתת את החתימות של השכנים
    const auto stateObj = extract_json_object(packetJson, "state");// פונקציה שמחזירה את האובייקט JSON של המצב שנשלח מהצומת
    if (!stateObj.has_value()) return false;// אם לא מצאנו את האובייקט JSON של המצב שנשלח מהצומת, מחזירה false

    out.intersectionId = extract_int_field(*stateObj, "intersection_id").value_or(1);// אם לא מצאנו את המזהה של הצומת, מחזירה את המזהה של הצומת כברירת מחדל
    out.timestamp = extract_double_field(*stateObj, "timestamp").value_or(0.0);// אם לא מצאנו את החותמת הזמן, מחזירה את החותמת הזמן כברירת מחדל
    out.lanes.clear();// מנקה את רשימת הנתיבים הקודמת
    out.neighbors.clear();// מנקה את רשימת השכנים הקודמת

    const auto lanesArray = extract_json_array(*stateObj, "lanes");// פונקציה שמחזירה את המערך JSON של הנתיבים שנשלחו מהצומת
    if (lanesArray.has_value()) {// אם מצאנו את המערך JSON של הנתיבים שנשלחו מהצומת, מפרקת את המידע על כל נתיב ומוסיפה אותו לרשימת הנתיבים
        std::regex laneRe(
            "\\{[^\\}]*\\\"lane_id\\\"\\s*:\\s*(\\d+)"
            "[^\\}]*\\\"vehicle_count\\\"\\s*:\\s*(-?\\d+)"
            "[^\\}]*\\\"density_pct\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)"
            "[^\\}]*\\\"waiting_time_sec\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)"
            "[^\\}]*\\}");

        auto begin = std::sregex_iterator(lanesArray->begin(), lanesArray->end(), laneRe);// יוצר איטרטור שמתחיל מהאובייקט הראשון במערך הנתיבים שנשלחו מהצומת
        auto end = std::sregex_iterator();// יוצר איטרטור שמסמן את סוף המערך הנתיבים שנשלחו מהצומת
        for (auto it = begin; it != end; ++it) {// לולאה שעוברת על כל האובייקטים במערך הנתיבים שנשלחו מהצומת
            ParsedLane lane;// יוצר אובייקט שמייצג את המידע על הנתיב
            lane.laneId = std::stoi((*it)[1].str());// מעדכנת את מזהה הנתיב במידע על הנתיב
            lane.vehicleCount = std::stoi((*it)[2].str());// מעדכנת את מספר הרכבים במידע על הנתיב
            lane.densityPct = std::stod((*it)[3].str());// מעדכנת את אחוז הצפיפות במידע על הנתיב
            lane.waitingSec = std::stod((*it)[4].str());// מעדכנת את זמן ההמתנה במידע על הנתיב
            out.lanes.push_back(lane);// מוסיפה את המידע על הנתיב לרשימת הנתיבים
        }
    }

    out.emergencyActive = false;// מאתחלת את מצב החירום כלא פעיל
    out.emergencyLaneId = std::nullopt;// מאתחלת את מזהה הנתיב שבו יש מצב חירום כלא קיים
    if (stateObj->find("\"emergency_signal\": null") == std::string::npos) {// אם לא מצאנו את המחרוזת שמציינת שאין מצב חירום, בודקת אם יש אובייקט JSON שמייצג את מצב החירום
        if (const auto emObj = extract_json_object(*stateObj, "emergency_signal"); emObj.has_value()) {// אם מצאנו את האובייקט JSON שמייצג את מצב החירום, מפרקת את המידע על מצב החירום ומעדכנת את המידע במבנה ParsedPacketState
            out.emergencyActive = extract_bool_field(*emObj, "active").value_or(false);// מעדכנת את מצב החירום במבנה ParsedPacketState
            const auto lane = extract_int_field(*emObj, "lane_id");// מעדכנת את מזהה הנתיב שבו יש מצב חירום במבנה ParsedPacketState
            if (lane.has_value() && *lane >= 0) {// אם מצאנו את מזהה הנתיב שבו יש מצב חירום והוא חוקי, מעדכנת את המידע במבנה ParsedPacketState
                out.emergencyLaneId = *lane;// מעדכנת את מזהה הנתיב שבו יש מצב חירום במבנה ParsedPacketState
            }
        }
    }

    if (const auto neighborsArray = extract_json_array(packetJson, "neighbors"); neighborsArray.has_value()) {// אם מצאנו את המערך JSON של השכנים שנשלחו מהצומת, מפרקת את המידע על כל שכן ומוסיפה אותו לרשימת השכנים
        std::regex neighborRe(
            "\\{[^\\}]*\\\"intersection_id\\\"\\s*:\\s*(\\d+)"
            "[^\\}]*\\\"action\\\"\\s*:\\s*\\\"([^\\\"]*)\\\""
            "[^\\}]*\\\"phase_id\\\"\\s*:\\s*(null|-?\\d+)"
            "[^\\}]*\\\"total_queue\\\"\\s*:\\s*(-?\\d+)"
            "[^\\}]*\\\"avg_waiting_sec\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)"
            "[^\\}]*\\\"emergency_active\\\"\\s*:\\s*(true|false)"
            "[^\\}]*\\\"signed_at\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)"
            "[^\\}]*\\\"signature\\\"\\s*:\\s*\\\"([0-9a-fA-F]+)\\\""
            "[^\\}]*\\}");

        auto begin = std::sregex_iterator(neighborsArray->begin(), neighborsArray->end(), neighborRe);// יוצר איטרטור שמתחיל מהאובייקט הראשון במערך השכנים שנשלחו מהצומת
        auto end = std::sregex_iterator();// יוצר איטרטור שמסמן את סוף המערך השכנים שנשלחו מהצומת
        for (auto it = begin; it != end; ++it) {// לולאה שעוברת על כל האובייקטים במערך השכנים שנשלחו מהצומת
            traffic::NeighborSignal neighbor;// יוצר אובייקט שמייצג את המידע על השכן
            neighbor.intersectionId = std::stoi((*it)[1].str());// מעדכנת את מזהה הצומת של השכן במידע על השכן
            const std::string phaseIdText = (*it)[3].str();// מעדכנת את מזהה השלב של השכן במידע על השכן
            neighbor.phaseId = (phaseIdText == "null") ? -1 : std::stoi(phaseIdText);// אם מזהה השלב של השכן הוא "null", מעדכנת את מזהה השלב של השכן ל-1, אחרת מעדכנת את מזהה השלב של השכן למספר השלב שנמצא במידע על השכן
            neighbor.totalQueue = std::max(0, std::stoi((*it)[4].str()));// מעדכנת את מספר הרכבים בתור הכולל של השכן במידע על השכן
            neighbor.avgWaitingSec = std::max(0.0, std::stod((*it)[5].str()));// מעדכנת את זמן ההמתנה הממוצע של השכן במידע על השכן
            neighbor.emergencyActive = ((*it)[6].str() == "true");// מעדכנת את מצב החירום של השכן במידע על השכן
            neighbor.signedAtSec = std::stod((*it)[7].str());// מעדכנת את זמן החתימה של השכן במידע על השכן
            const std::string signature = (*it)[8].str();// מעדכנת את החתימה של השכן במידע על השכן
            neighbor.signatureVerified = verify_neighbor_signature(neighbor, signature, neighborAuth);// מאמתת את החתימה של השכן במידע על השכן
            if (neighbor.signatureVerified) {// אם החתימה של השכן מאומתת, מוסיפה את המידע על השכן לרשימת השכנים
                out.neighbors.push_back(neighbor);// מוסיפה את המידע על השכן לרשימת השכנים
            }
        }
    }

    return !out.lanes.empty();// מחזירה true אם מצאנו לפחות נתיב אחד במידע על מצב החבילה שנשלחה מהצומת, אחרת מחזירה false
}

traffic::JunctionState with_neighbor_signals(traffic::JunctionState state, const std::vector<traffic::NeighborSignal>& neighbors) {// פונקציה שמחזירה את מצב הצומת עם האותות מהשכנים
    state.neighborSignals = neighbors;// מעדכנת את רשימת האותות מהשכנים במצב הצומת
    return state;
}

bool has_busy_synced_neighbor(// פונקציה שבודקת אם יש שכן מסונכרן עסוק
    const traffic::JunctionState& state,// מצב הצומת
    int selectedPhase,// מזהה השלב הנבחר
    const traffic::TrafficThresholdConfig& thresholds// קונפיגורציה של סף התנועה
) {
    const double localLaneCount = std::max<std::size_t>(1, state.laneIds.size());// מספר הנתיבים בצומת הנוכחי, לפחות 1
    for (const auto& neighbor : state.neighborSignals) {// לולאה שעוברת על כל השכנים של הצומת הנוכחי
        const double normalizedQueue = static_cast<double>(neighbor.totalQueue) / static_cast<double>(localLaneCount);// מחשבת את מספר הרכבים בתור של השכן ביחס למספר הנתיבים בצומת הנוכחי
        if (neighbor.phaseId == selectedPhase && normalizedQueue > thresholds.vehicleCount.lowMax) {// אם השכן מסונכרן עם השלב הנבחר ויש לו יותר מדי רכבים בתור, מחזירה true
            return true;
        }
    }
    return false;
}

bool has_busy_opposing_neighbor(// פונקציה שבודקת אם יש שכן מסונכרן עסוק
    const traffic::JunctionState& state,// מצב הצומת
    int selectedPhase,// מזהה השלב הנבחר
    const traffic::TrafficThresholdConfig& thresholds// קונפיגורציה של סף התנועה
) {
    const double localLaneCount = std::max<std::size_t>(1, state.laneIds.size());// מספר הנתיבים בצומת הנוכחי, לפחות 1
    for (const auto& neighbor : state.neighborSignals) {// לולאה שעוברת על כל השכנים של הצומת הנוכחי
        const double normalizedQueue = static_cast<double>(neighbor.totalQueue) / static_cast<double>(localLaneCount);// מחשבת את מספר הרכבים בתור של השכן ביחס למספר הנתיבים בצומת הנוכחי
        if (neighbor.phaseId >= 0 && neighbor.phaseId != selectedPhase && normalizedQueue > thresholds.vehicleCount.mediumMax) {// אם השכן מסונכרן עם שלב שונה ויש לו יותר מדי רכבים בתור, מחזירה true
            return true;
        }
    }
    return false;
}

bool phase_contains_lane(const std::vector<traffic::Action>& validActions, int phaseId, int laneId) {// פונקציה שבודקת אם שלב מסוים מכיל נתיב מסוים
    for (const auto& phase : validActions) {// לולאה שעוברת על כל השלבים התקפים
        if (phase.phaseId != phaseId) continue;// אם מזהה השלב שונה מזה של השלב הנבדק, ממשיכה לשלב הבא
        return std::find(phase.greenLanes.begin(), phase.greenLanes.end(), laneId) != phase.greenLanes.end();// מחזירה true אם הנתיב נמצא ברשימת הנתיבים הירוקים של השלב
    }
    return false;
}

std::vector<traffic::Action> build_phases_from_lanes(const std::vector<ParsedLane>& lanes) {// פונקציה שמבנה את השלבים התקפים מהנתיבים הקיימים בצומת
    std::vector<int> even;// רשימה של מזהי הנתיבים הזוגיים
    std::vector<int> odd;// רשימה של מזהי הנתיבים האי-זוגיים
    for (const auto& lane : lanes) {// לולאה שעוברת על כל הנתיבים הקיימים בצומת
        if (lane.laneId % 2 == 0) {// אם מזהה הנתיב זוגי, מוסיפה אותו לרשימת הנתיבים הזוגיים
            even.push_back(lane.laneId);// מוסיפה את מזהה הנתיב הזוגי לרשימת הנתיבים הזוגיים
        } else {
            odd.push_back(lane.laneId);// מוסיפה את מזהה הנתיב האי-זוגי לרשימת הנתיבים האי-זוגיים
        }
    }

    std::vector<traffic::Action> phases;// רשימה של השלבים התקפים
    if (!even.empty()) phases.push_back({0, even});// אם ישנם נתיבים זוגיים, מוסיפה שלב עם מזהה 0 ורשימת הנתיבים הזוגיים
    if (!odd.empty()) phases.push_back({1, odd});// אם ישנם נתיבים אי-זוגיים, מוסיפה שלב עם מזהה 1 ורשימת הנתיבים האי-זוגיים

    if (phases.empty()) {// אם אין שלבים תקפים, מוסיפה שלב לכל נתיב בנפרד
        for (std::size_t i = 0; i < lanes.size(); ++i) {// לולאה שעוברת על כל הנתיבים הקיימים בצומת
            phases.push_back({static_cast<int>(i), {lanes[i].laneId}});// מוסיפה שלב עם מזהה השווה למיקום הנתיב ברשימה ורשימת הנתיבים שמכילה רק את הנתיב הנוכחי
        }
    }

    return phases;
}

std::vector<int> lane_ids_from_lanes(const std::vector<ParsedLane>& lanes) {// פונקציה שמחזירה את מזהי הנתיבים מרשימת הנתיבים
    std::vector<int> ids;// רשימה של מזהי הנתיבים
    ids.reserve(lanes.size());// שמורה מקום ברשימה בהתאם למספר הנתיבים
    for (const auto& lane : lanes) {// לולאה שעוברת על כל הנתיבים
        ids.push_back(lane.laneId);// מוסיפה את מזהה הנתיב לרשימה
    }
    return ids;
}

traffic::LaneConflictConfig load_lane_conflicts_from_api(// פונקציה שמטענת את הקונפיגורציה של קונפליקטים בין נתיבים מה-API
    const smart_traffic::HttpClient& client,// אובייקט שמייצג את הלקוח שמבצע את הבקשה ל-API
    int intersectionId// מזהה הצומת
) {
    traffic::LaneConflictConfig cfg;// אובייקט קונפיגורציה עם ערכי ברירת מחדל
    cfg.source = "/intersection/" + std::to_string(intersectionId) + "/conflicts";// מחרוזת שמייצגת את הנתיב ל-API שמחזיר את הקונפליקטים בין הנתיבים בצומת

    const std::string payload = client.get(cfg.source);// מבצע בקשה ל-API ומחזיר את התגובה במחרוזת
    if (payload.empty()) {// אם התגובה ריקה, מעדכנת את מקור הקונפיגורציה ומחזירה את הקונפיגורציה
        cfg.source += " (empty response)";// מעדכנת את מקור הקונפיגורציה במחרוזת שמציינת שהתגובה ריקה
        return cfg;// מחזירה את הקונפיגורציה עם מקור הקונפיגורציה המעודכן
    }

    const auto conflictsArray = extract_json_array(payload, "conflicts");// פונקציה שמחזירה את המערך JSON של הקונפליקטים בין הנתיבים בצומת
    if (!conflictsArray.has_value()) {// אם לא מצאנו את המערך JSON של הקונפליקטים בין הנתיבים בצומת, מעדכנת את מקור הקונפיגורציה ומחזירה את הקונפיגורציה
        cfg.source += " (missing conflicts array)";// מעדכנת את מקור הקונפיגורציה במחרוזת שמציינת שהמערך JSON של הקונפליקטים בין הנתיבים בצומת חסר
        return cfg;
    }

    std::regex pairRe("\\[\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*\\]");// regex שמחפש זוגות של מספרים שלמים במערך JSON של הקונפליקטים בין הנתיבים בצומת
    auto begin = std::sregex_iterator(conflictsArray->begin(), conflictsArray->end(), pairRe);// יוצר איטרטור שמתחיל מהאובייקט הראשון במערך הקונפליקטים בין הנתיבים בצומת
    auto end = std::sregex_iterator();// יוצר איטרטור שמסמן את סוף המערך הקונפליקטים בין הנתיבים בצומת
    for (auto it = begin; it != end; ++it) {// לולאה שעוברת על כל האובייקטים במערך הקונפליקטים בין הנתיבים בצומת
        int a = -1;// משתנה שמייצג את מזהה הנתיב הראשון בקונפליקט
        int b = -1;// משתנה שמייצג את מזהה הנתיב השני בקונפליקט
        try {
            a = std::stoi((*it)[1].str());// מעדכנת את מזהה הנתיב הראשון בקונפליקט
            b = std::stoi((*it)[2].str());// מעדכנת את מזהה הנתיב השני בקונפליקט
        } catch (...) {
            continue;// אם לא ניתן להמיר את המחרוזת למספר שלם, ממשיכה לאובייקט הבא במערך הקונפליקטים בין הנתיבים בצומת
        }

        if (a < 0 || b < 0 || a == b) continue;// אם אחד המזהים של הנתיבים בקונפליקט הוא שלילי או ששני המזהים זהים, ממשיכה לאובייקט הבא במערך הקונפליקטים בין הנתיבים בצומת
        if (a > b) std::swap(a, b);// אם מזהה הנתיב הראשון גדול מזהה הנתיב השני, מחליפה ביניהם כדי לשמור על סדר עולה
        cfg.conflictPairs.emplace_back(a, b);// מוסיפה את זוג המזהים של הנתיבים בקונפליקט לרשימת הקונפליקטים בין הנתיבים בצומת
    }

    std::sort(cfg.conflictPairs.begin(), cfg.conflictPairs.end());// ממיינת את רשימת הקונפליקטים בין הנתיבים בצומת לפי סדר עולה של זוגות המזהים
    cfg.conflictPairs.erase(std::unique(cfg.conflictPairs.begin(), cfg.conflictPairs.end()), cfg.conflictPairs.end());// מסירה כפילויות מרשימת הקונפליקטים בין הנתיבים בצומת
    return cfg;
}

void print_loaded_conflicts(int intersectionId, const traffic::LaneConflictConfig& cfg) {// פונקציה שמדפיסה את הקונפליקטים בין הנתיבים בצומת
    std::cout << "Loaded conflict pairs for intersection " << intersectionId// מדפיסה את המזהה של הצומת
              << " from " << cfg.source
              << " (pairs=" << cfg.conflictPairs.size() << ")\n";
    if (cfg.conflictPairs.empty()) {// אם אין קונפליקטים בין הנתיבים בצומת, מדפיסה הודעה מתאימה
        std::cout << "  - No conflict pairs returned by API\n";// מדפיסה הודעה מתאימה
        return;
    }

    for (const auto& p : cfg.conflictPairs) {// לולאה שעוברת על כל הקונפליקטים בין הנתיבים בצומת ומדפיסה את זוגות המזהים של הנתיבים בקונפליקט
        std::cout << "  - conflict: lane " << p.first << " <-> lane " << p.second << "\n";// מדפיסה את זוג המזהים של הנתיבים בקונפליקט
    }
}

void run_conflict_enforcement_smoke_test(// פונקציה שמריצה בדיקה מהירה של אכיפת הקונפליקטים בין הנתיבים בצומת
    int intersectionId,// מזהה הצומת
    const traffic::LaneConflictConfig& cfg// קונפיגורציה של הקונפליקטים בין הנתיבים בצומת
) {
    if (cfg.conflictPairs.empty()) {// אם אין קונפליקטים בין הנתיבים בצומת, מדפיסה הודעה מתאימה ומחזירה
        std::cout << "[Conflict smoke test] intersection " << intersectionId// מדפיסה את המזהה של הצומת
                  << ": skipped (no conflicts from API)\n";
        return;
    }

    const int a = cfg.conflictPairs.front().first;// משתנה שמייצג את מזהה הנתיב הראשון בקונפליקט הראשון ברשימת הקונפליקטים בין הנתיבים בצומת    
    const int b = cfg.conflictPairs.front().second;// משתנה שמייצג את מזהה הנתיב השני בקונפליקט הראשון ברשימת הקונפליקטים בין הנתיבים בצומת

    std::vector<traffic::Lane> lanes = {// רשימה של הנתיבים בצומת
        {a, 0, 0.0, 0.0, false},
        {b, 0, 0.0, 0.0, false},
    };

    std::vector<traffic::Action> phases = {// רשימה של השלבים התקפים בצומת
        {900, {a, b}}, // must be rejected
        {901, {a}},    // should be valid
    };

    traffic::Junction testJunction(// יוצר אובייקט Junction שמייצג את הצומת
        intersectionId,
        lanes,
        phases,
        0.0,
        10.0,
        cfg.conflictPairs
    );

    const bool blocked = !testJunction.applyPhase(900, 0.0);// בודק אם השלב 900 נחסם על ידי הקונפליקטים בין הנתיבים בצומת
    const bool allowed = testJunction.applyPhase(901, 1.0);// בודק אם השלב 901 מותר על ידי הקונפליקטים בין הנתיבים בצומת

    std::cout << "[Conflict smoke test] pair(" << a << "," << b << ")"// מדפיסה את התוצאה של הבדיקה המהירה של אכיפת הקונפליקטים בין הנתיבים בצומת
              << " | phase{900:[" << a << "," << b << "]} => " << (blocked ? "BLOCKED" : "ALLOWED")// מדפיסה את התוצאה של השלב 900
              << " | phase{901:[" << a << "]} => " << (allowed ? "ALLOWED" : "BLOCKED")// מדפיסה את התוצאה של השלב 901
              << "\n";
}

std::string lane_topology_key(const ParsedPacketState& s) {// פונקציה שמחזירה מחרוזת שמייצגת את המפתח של הטופולוגיה של הנתיבים בצומת
    std::ostringstream oss;// יוצר אובייקט שמייצג את המחרוזת שמכילה את המפתח של הטופולוגיה של הנתיבים בצומת
    oss << s.intersectionId << "|";// מוסיפה את מזהה הצומת למחרוזת שמכילה את המפתח של הטופולוגיה של הנתיבים בצומת
    for (const auto& lane : s.lanes) {// לולאה שעוברת על כל הנתיבים בצומת ומוסיפה את מזהי הנתיבים למחרוזת שמכילה את המפתח של הטופולוגיה של הנתיבים בצומת
        oss << lane.laneId << ",";
    }
    return oss.str();// מחזירה את המחרוזת שמכילה את המפתח של הטופולוגיה של הנתיבים בצומת
}

void run_with_server(const std::string& host, int port, bool useGreedyController = false) {// פונקציה שמריצה את התוכנית עם חיבור לשרת
    std::cout << "\n=== CONNECTED MODE (" << (useGreedyController ? "Greedy-Aging" : "Unified RL") << " Path) ===\n";// מדפיסה הודעה שמציינת שהחיבור לשרת פעיל ושמציינת את סוג הבקרת התנועה שנבחרה
    std::cout << "Server: " << host << ":" << port << "\n\n";

    smart_traffic::HttpClient client(host, port);// יוצר אובייקט שמייצג את הלקוח שמבצע את הבקשות לשרת
    const NeighborAuthConfig neighborAuth = load_neighbor_auth_config();// טוען את קונפיגורציית האימות של השכנים
    const std::string health = client.get("/health");// מבצע בקשה לשרת ומחזיר את התגובה במחרוזת
    if (health.empty()) {// אם התגובה ריקה, מדפיסה הודעת שגיאה ומחזירה
        std::cerr << "Cannot connect to server. Run FastAPI first.\n";
        return;
    }
    std::cout << "Health: " << health << "\n\n";// מדפיסה את התגובה של הבקשה לשרת
    std::cout << "Neighbor auth source: " << neighborAuth.source// מדפיסה את מקור קונפיגורציית האימות של השכנים
              << " | skew=" << neighborAuth.maxSignatureSkewSec << "s\n";// מדפיסה את ההפרש המקסימלי של החתימות של השכנים

    traffic::TrafficThresholdConfig thresholds = traffic::loadTrafficThresholdConfig();// טוען את קונפיגורציית סף התנועה
    std::cout << "Threshold config: " << thresholds.source << "\n";
    const traffic::NeighborCoordConfig neighborConfig = traffic::loadNeighborCoordConfig();// טוען את קונפיגורציית הקואורדינטות של השכנים
    std::cout << "Neighbor tuning: " << neighborConfig.profileName// מדפיסה את שם הפרופיל של קונפיגורציית הקואורדינטות של השכנים
              << " (" << neighborConfig.source << ")\n";
    traffic::LaneConflictConfig laneConflicts;// יוצר אובייקט שמייצג את קונפיגורציית הקונפליקטים בין הנתיבים בצומת
    laneConflicts.source = "api (pending intersection)";// מחרוזת שמציינת שהקונפיגורציה של הקונפליקטים בין הנתיבים בצומת תטען מה-API עבור הצומת הפעילה
    traffic::PhaseConfig phaseConfig = traffic::loadPhaseConfig();// טוען את קונפיגורציית השלבים התקפים
    std::cout << "Phase config: " << phaseConfig.source
              << " (intersections=" << phaseConfig.phasesByIntersection.size() << ")\n";
    traffic::RLAgent agent({}, thresholds);// יוצר אובייקט שמייצג את הסוכן הלמידה עם קונפיגורציית סף התנועה
    traffic::GreedyAgingController greedy;// יוצר אובייקט שמייצג את הבקרת התנועה Greedy-Aging
    int activeThresholdIntersection = -1;// משתנה שמייצג את מזהה הצומת הפעילה עבור קונפיגורציית סף התנועה
    int activeConflictIntersection = -1;// משתנה שמייצג את מזהה הצומת הפעילה עבור קונפיגורציית הקונפליקטים בין הנתיבים בצומת
    const std::string qTablePath = "qtable.tsv";
    const bool loaded = agent.loadQTable(qTablePath);// טוען את טבלת Q מהקובץ
    std::cout << "Q-table load: " << (loaded ? "ok" : "new") << " (" << qTablePath << ")\n";
    std::unique_ptr<traffic::Junction> junction;// מצביע חכם לאובייקט Junction שמייצג את הצומת הפעילה
    std::vector<traffic::Action> phases;// רשימה של השלבים התקפים בצומת הפעילה

    std::string currentTopology;// מחרוזת שמייצגת את המפתח של הטופולוגיה של הנתיבים בצומת הפעילה
    double nowSec = 0.0;// משתנה שמייצג את הזמן הנוכחי בשניות
    constexpr double kStepSec = traffic::constants::kControllerStepSec;// משתנה שמייצג את משך הזמן של כל צעד של הבקרת התנועה בשניות

    while (true) {
        const std::string packet = client.get("/intersection/1/packet");//קבלת מידע על צומת מהשרת
        if (packet.empty()) {//אם אין מידע
            std::cerr << "Packet request failed\n";//הזפסת שגיאה
            std::this_thread::sleep_for(std::chrono::milliseconds(500));//המתנה לחצי שנייה
            continue;//חזרה לתחילת הוויל
        }

        ParsedPacketState parsed;//אובייקט לייצוג מידע הצומת
        if (!parse_packet_state(packet, parsed, neighborAuth)) {//אם לא הצלחנו לנתח את המידע
            std::cerr << "Failed to parse /packet response\n";//שגיאה
            std::this_thread::sleep_for(std::chrono::milliseconds(500));//המתנה לחצי שנייה
            continue;//חזרה לתחילת הוויל
        }

        if (parsed.intersectionId != activeThresholdIntersection) {//אם הצומת שפעילה כרגע שונה מהצומת שהתקבלה מהשרת
            thresholds = traffic::loadTrafficThresholdConfigForIntersection(parsed.intersectionId);//טוענים את ספי התנועה עבור הצומת החדש
            agent.setThresholdConfig(thresholds);//עדכון ספי התנועה בסוכן הלמידה
            activeThresholdIntersection = parsed.intersectionId;//עדכון הצומת הפעילה לספי התנועה
            std::cout << "Threshold config for intersection " << parsed.intersectionId// הדפסת מקור ספי התנועה עבור הצומת החדש
                      << ": " << thresholds.source << "\n";
        }

        if (parsed.intersectionId != activeConflictIntersection) {//אם הצומת שפעילה כרגע שונה מהצומת שהתקבלה מהשרת
            laneConflicts = load_lane_conflicts_from_api(client, parsed.intersectionId);//טוענים את קונפליקטים של הנתיבים עבור הצומת החדש מהשרת
            activeConflictIntersection = parsed.intersectionId;//עדכון הצומת הפעילה לקונפליקטים של הנתיבים
            std::cout << "Lane conflicts API for intersection " << parsed.intersectionId// הדפסת מקור קונפליקטים של הנתיבים עבור הצומת החדש
                      << ": " << laneConflicts.source
                      << " (pairs=" << laneConflicts.conflictPairs.size() << ")\n";//הדפסת מספר זוגות הקונפליקט שהתקבלו מהשרת
            print_loaded_conflicts(parsed.intersectionId, laneConflicts);//הדפסת זוגות הקונפליקט שהתקבלו מהשרת
            run_conflict_enforcement_smoke_test(parsed.intersectionId, laneConflicts);// הרצת בדיקת עשן בסיסית לאכיפת הקונפליקטים של הנתיבים עבור הצומת החדש
        }

        const std::string topology = lane_topology_key(parsed);//ייצוג טופולוגיית הנתיבים בצומת כמחרוזת ייחודית
        if (!junction || topology != currentTopology) {//אם אין צומת פעילה כרגע או שהטופולוגיה של הנתיבים שונה מהטופולוגיה הנוכחית
            std::vector<traffic::Lane> lanes;//וקטור לייצוג הנתיבים בצומת
            lanes.reserve(parsed.lanes.size());//שמירת מקום לנתיבים
            for (const auto& l : parsed.lanes) {//עבור כל נתיב שהתקבל מהשרת
                lanes.push_back({//הוספת נתיב לוקטור הנתיבים עם המידע שהתקבל מהשרת
                    l.laneId,//מזהה הנתיב
                    std::max(0, l.vehicleCount),//מספר הרכבים בנתיב (לא יכול להיות שלילי)
                    std::clamp(l.densityPct, 0.0, 100.0),//אחוז הצפיפות בנתיב (מוגבל בין 0 ל-100)
                    std::max(0.0, l.waitingSec),//זמן ההמתנה הממוצע ברכבים בנתיב (לא יכול להיות שלילי)
                    parsed.emergencyActive && parsed.emergencyLaneId.has_value() && *parsed.emergencyLaneId == l.laneId//האם יש רכב חירום פעיל בנתיב הזה
                });
            }

            phases = traffic::resolveConfiguredPhases(//נסיון לקבל את שלבי האור הירוק מהקונפיגורציה עבור הצומת הנוכחית
                parsed.intersectionId,//מזהה הצומת
                lane_ids_from_lanes(parsed.lanes),//רשימת מזהי הנתיבים בצומת
                phaseConfig
            );
            if (phases.empty()) {//אם לא הצלחנו לקבל שלבי אור ירוק מהקונפיגורציה עבור הצומת הנוכחית
                phases = build_phases_from_lanes(parsed.lanes);//בניית שלבי אור ירוק אוטומטית על בסיס הנתיבים בצומת (למשל, כל הנתיבים הזוגיים יחד וכל הנתיבים האי-זוגיים יחד)
                std::cout << "Phase config fallback for intersection " << parsed.intersectionId// הדפסת הודעת גיבוי לבניית שלבי אור ירוק אוטומטית עבור הצומת הנוכחית
                          << ": using auto phase builder\n";
            }
            if (phases.empty()) {//אם עדיין אין שלבי אור ירוק תקפים לאחר ניסיון לקבל מהקונפיגורציה ובניית גיבוי אוטומטית
                std::cerr << "No valid phases from packet lanes\n";//הדפסת שגיאה שאין שלבי אור ירוק תקפים מהנתיבים שהתקבלו מהשרת
                std::this_thread::sleep_for(std::chrono::milliseconds(500));//המתנה לחצי שנייה
                continue;//חזרה לתחילת הוויל
            }

            junction = std::make_unique<traffic::Junction>(//יצירת אובייקט צומת חדש עם המידע שהתקבל מהשרת והקונפיגורציות השונות
                parsed.intersectionId,//מזהה הצומת
                lanes,//רשימת הנתיבים בצומת
                phases,//רשימת הפאזות החוקית
                3.0,   // minGreenSec: 3 seconds - enables fast switching on empty lanes
                45.0,  // maxGreenSec: 45 seconds - prevents starvation
                laneConflicts.conflictPairs//זוגות הנתיבים בקונפליקט שהתקבלו מהשרת
            );
            currentTopology = topology;//עדכון הטופולוגיה הנוכחית
            nowSec = 0.0;//איפוס הזמן
        }

        for (const auto& l : parsed.lanes) {//עבור כל נתיב בצומת 
            const bool emergencyOnLane = parsed.emergencyActive && parsed.emergencyLaneId.has_value() && *parsed.emergencyLaneId == l.laneId;//האם יש רכב חירום פעיל בנתיב הזה
            junction->updateLaneObservation(l.laneId, l.vehicleCount, emergencyOnLane, l.densityPct);//עדכון המידע על הנתיב 
        }
        junction->setEmergencySignal(parsed.emergencyActive, parsed.emergencyLaneId);//עדכון המידע על רכב החירום בצומת

        const traffic::JunctionState prevState = with_neighbor_signals(junction->currentState(), parsed.neighbors);//קבלת המצב הנוכחי של הצומת עם מידע על השכנים
        const auto emergencyPhase = junction->resolveEmergencyPhase();//קבלת הפאזת החירום אם קיימת

        int selectedPhase = useGreedyController// בחירת הפאזה לפעולה: אם משתמשים בבקר החמדן אז לבחור לפי הבקר החמדן, אחרת לבחור לפי סוכן הלמידה
            ? greedy.selectAction(prevState, junction->validPhases(), emergencyPhase)
            : agent.selectAction(prevState, junction->validPhases(), emergencyPhase);
        if (!junction->applyPhase(selectedPhase, nowSec)) {//אם לא הצליח להתחיל את הפאזה שנבחרה
            selectedPhase = junction->activePhaseId();//לקבל את הפאזה הפעילה הנוכחית
            if (selectedPhase < 0 && !junction->validPhases().empty()) {//אם אין פאזה פעילה אבל יש פאזה חוקית זמינה
                selectedPhase = junction->validPhases().front().phaseId;//לבחור את הפאזה החוקית הראשונה
                (void)junction->applyPhase(selectedPhase, nowSec);//להחיל את הפאזה החוקית הראשונה
            }
        }

        bool emergencyLaneGotGreen = false;//האם נתיב החירום קיבל אור ירוק
        if (prevState.emergencyVehicleActive && prevState.emergencyLaneId.has_value()) {//אם יש רכב חירום פעיל בנתיב מסוים במצב הקודם
            emergencyLaneGotGreen = phase_contains_lane(junction->validPhases(), selectedPhase, *prevState.emergencyLaneId);//בדיקה האם הפאזה שנבחרה כוללת את נתיב החירום
        }

        junction->tick(kStepSec);// עדכון מצב הצומת על בסיס הזמן שעבר
        nowSec += kStepSec;//עדכון הזמן הכולל
        const traffic::JunctionState nextState = with_neighbor_signals(junction->currentState(), parsed.neighbors);//קבלת המצב הבא של הצומת עם מידע על השכנים לאחר העדכון

        const bool greenSyncedWithNeighbor = has_busy_synced_neighbor(prevState, selectedPhase, agent.thresholdConfig());// האם יש שכן עם פאזה מסונכרנת שזוהתה כעמוסה לפי הספים של סוכן הלמידה
        const bool greenOppositeToNeighbor = has_busy_opposing_neighbor(prevState, selectedPhase, agent.thresholdConfig());// האם יש שכן עם פאזה מנוגדת שזוהתה כעמוסה לפי הספים של סוכן הלמידה

        const double reward = agent.computeReward(//חישוב תגמול
            prevState,//מצב קודם
            nextState,//מצב הבא
            selectedPhase,//פאזה שנבחרה
            emergencyLaneGotGreen,//האם החירום קיבל ירוק
            greenSyncedWithNeighbor,//האם היה סנכרון עם שכן עמוס
            greenOppositeToNeighbor,//האם היה ניגוד עם שכן עמוס
            kStepSec// משך הזמן שלקח לבצע את הפעולה
        );
        agent.update(prevState, selectedPhase, reward, nextState, junction->validPhases());//עדכון סוכן הלמידה עם המידע על המעבר שבוצע
        agent.decayExploration();//הפחתת חקירה- הורדת אפסילון

        const std::string actionText = (selectedPhase >= 0)//אם יש פאזה שנבחרה אז להציג את מזהה הפאזה, אחרת להציג "Hold"
            ? ("Phase" + std::to_string(selectedPhase))
            : "Hold";
//בניית גוף הבקשה לשליחת הפעולה שנבחרה בחזרה לשרת
        std::ostringstream body;
        body << "{\"action\":\"" << actionText << "\",";
        body << "\"phase_id\":" << selectedPhase << ",";
        body << "\"reason\":\"cpp_rl_unified\",";
        body << "\"timestamp\":" << parsed.timestamp << "}";

        const std::string postPath = "/intersection/" + std::to_string(parsed.intersectionId) + "/action";//שליחת הפעולה שנבחרה בחזרה לשרת
        const std::string postResp = client.post(postPath, body.str());//קבלת תגובה מהשרת לאחר שליחת הפעולה

        int totalVehicles = 0;//חישוב סך הרכבים בכל הנתיבים בצומת
        for (int c : prevState.vehicleCounts) totalVehicles += c;
//הדפסת המידע על הצומת
        std::cout << "Intersection " << parsed.intersectionId
                  << " | lanes=" << parsed.lanes.size()
                  << " | vehicles=" << totalVehicles
                  << " | phase=" << selectedPhase
                  << " | action=" << actionText
                  << " | reward=" << reward
                  << " | epsilon=" << agent.epsilon()
                  << " | post=" << (postResp.empty() ? "failed" : "ok")
                  << "\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(500));//המתנה לחצי שנייה לפני הבקשה הבאה
    }

    const bool saved = agent.saveQTable(qTablePath);//שמירת הטבלת קיו לקובץ לאחר סיום הריצה
    std::cout << "Q-table save: " << (saved ? "ok" : "failed") << " (" << qTablePath << ")\n";//הדפסת תוצאה של שמירת הטבלת קיו
}

void run_conflict_check_once(const std::string& host, int port, int intersectionId) {//הרצת בדיקת קונפליקט אחת בלבד עבור צומת מסוימת
    std::cout << "\n=== CONFLICT CHECK MODE (API) ===\n";
    std::cout << "Server: " << host << ":" << port << " | intersection=" << intersectionId << "\n\n";//הדפסת מידע על מצב הריצה

    smart_traffic::HttpClient client(host, port);//יצירת לקוח HTTP להתחברות לשרת
    const std::string health = client.get("/health");//בדיקת מצב השרת
    if (health.empty()) {//אם השרת לא מגיב
        std::cerr << "Cannot connect to server. Run FastAPI first.\n";//הדפסת שגיאה
        return;//חזרה מהפונקציה
    }

    traffic::LaneConflictConfig laneConflicts = load_lane_conflicts_from_api(client, intersectionId);//טעינת קונפליקטים של הנתיבים עבור הצומת הנתונה מהשרת
    print_loaded_conflicts(intersectionId, laneConflicts);// הדפסת הקונפליקטים של הנתיבים שהתקבלו מהשרת
    run_conflict_enforcement_smoke_test(intersectionId, laneConflicts);//הרצת בדיקת קונפליקט עבור הצומת הנתונה
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "  Smart Traffic Controller (C++)\n";
    std::cout << "  Unified RL Path: Junction + RLAgent\n";
    std::cout << "========================================\n";
//בדיקה איזה מצב ריצה נבחר על ידי המשתמש
    if (argc > 1 && std::string(argv[1]) == "--server") {
        std::string host = "127.0.0.1";
        int port = 8000;
        bool useGreedy = false;
        std::vector<std::string> positional;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--greedy") useGreedy = true;
            else                 positional.push_back(a);
        }
        if (positional.size() >= 1) host = positional[0];
        if (positional.size() >= 2) port = std::atoi(positional[1].c_str());
        run_with_server(host, port, useGreedy);
    } else if (argc > 1 && std::string(argv[1]) == "--conflict-check") {
        std::string host = argc > 2 ? argv[2] : "127.0.0.1";
        int port = argc > 3 ? std::atoi(argv[3]) : 8000;
        int intersectionId = argc > 4 ? std::atoi(argv[4]) : 1;
        if (intersectionId <= 0) intersectionId = 1;
        run_conflict_check_once(host, port, intersectionId);
    } else if (argc > 1 && std::string(argv[1]) == "--selftest") {
        const bool ok = traffic_tests::run_all_self_tests();
        return ok ? 0 : 1;
    } else if (argc > 1 && std::string(argv[1]) == "--simulate") {
        traffic_sim::run_simulation_comparison_verbose();
    } else {
        std::cout << "Usage:\n";
        std::cout << "  smart_traffic_controller.exe --simulate\n";
        std::cout << "  smart_traffic_controller.exe --server [host] [port] [--greedy]\n\n";
        std::cout << "  smart_traffic_controller.exe --conflict-check [host] [port] [intersection_id]\n\n";
        std::cout << "  smart_traffic_controller.exe --selftest\n\n";
        traffic_sim::run_simulation_comparison_verbose();
    }

    return 0;
}
