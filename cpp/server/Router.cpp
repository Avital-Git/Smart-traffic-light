// Router.cpp — איחוד HTTP ו-WebSocket על אותו פורט.
// כל חיבור נכנס לפורט 8000; הקוד מציץ בשקיפה ומנתב ל-WebSocket Hub או ל-HTTP proxy.

// ── ספריות Winsock ─────────────────────────────────────────────────────
#include <winsock2.h>   // ספריית socket ראשית של Windows — חייב להיות ראשון!
#include <ws2tcpip.h>   // פונקציות TCP/IP מתקדמות (inet_pton וכו')
#include <windows.h>    // Windows API כללי

#include "Router.h"       // הכרזות המחלקה
#include "WebSocketHub.h" // ל-adopt_socket()

// ── ספריות C++ סטנדרטיות ───────────────────────────────────────────────
#include <algorithm>  // std::transform — המרה לאותיות קטנות
#include <atomic>     // std::atomic — done flag בין תילים
#include <chrono>     // מדידת זמן ל-deadline ב-peek
#include <cstring>    // std::strlen — אורך מחרוזת סטינה
#include <iostream>   // std::cerr — הדפסת שגיאות
#include <string>     // std::string
#include <thread>     // std::thread — תילת pipe לכל חיבור
#include <vector>     // std::vector — בפרי קריאה/כתיבה

namespace {

// ממיר SOCKET ל-uintptr_t ובחזרה לצרכי דיור במשתני גנריים
static inline SOCKET to_sock(uintptr_t v) { return (SOCKET)v; }
static inline uintptr_t to_uint(SOCKET s) { return (uintptr_t)s; }
static const uintptr_t INVALID_U = to_uint(INVALID_SOCKET);  // ערך socket לא חוקי

// בודק אם הבאפר מסתיים ב-\r\n\r\n (סוף ה-headers של HTTP)
bool ends_with_blankline(const std::string& buf) {
    return buf.size() >= 4 &&
        buf[buf.size()-4] == '\r' && buf[buf.size()-3] == '\n' &&
        buf[buf.size()-2] == '\r' && buf[buf.size()-1] == '\n';
}

// צוצף (ללא הסרה מה-socket) עד \r\n\r\n של HTTP.
// משתמש ב-MSG_PEEK כדי שה-HTTP server הפנימי עדיין יקבל את הבקשה המלאה.
// timeout: 2 שניות מקסימום לחיבור מותנה אחרי half-open.
std::string peek_http_preface(SOCKET sock) {
    constexpr int kMax = 8192;  // מקסימום בייטים ל-headers
    std::vector<char> buf(kMax);
    int total = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);  // דדליין 2 שניות

    while (total < kMax) {
        int n = ::recv(sock, buf.data(), kMax, MSG_PEEK);  // ציצוף בלבד — לא מסירים מה-socket
        if (n <= 0) {
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {  // אין נתונים עדיין
                if (std::chrono::steady_clock::now() > deadline) break;  // חרגנו הזמן
                std::this_thread::sleep_for(std::chrono::milliseconds(5));  // מתינה קצרה
                continue;
            }
            break;
        }
        total = n;
        std::string preface(buf.data(), buf.data() + n);
        if (ends_with_blankline(preface)) {
            return preface;  // קיבלנו את כל ה-headers
        }
        if (std::chrono::steady_clock::now() > deadline) break;  // timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::string(buf.data(), buf.data() + total);  // מה שהצלחנו לקבל
}

// מחלץ ערך של header לפי שמו (השוואה case-insensitive)
// למשל: header_value(preface, "upgrade") יחזיר "websocket"
std::string header_value(const std::string& preface, const std::string& name_lower) {
    std::string lower(preface.size(), '\0');
    std::transform(preface.begin(), preface.end(), lower.begin(),
                   [](char c){ return (char)std::tolower((unsigned char)c); });  // המר לאותיות קטנות
    const std::string needle = "\r\n" + name_lower + ":";
    auto pos = lower.find(needle);
    if (pos == std::string::npos) return "";  // header לא קיים
    pos += needle.size();
    while (pos < preface.size() && (preface[pos] == ' ' || preface[pos] == '\t')) ++pos;  // דילוג רווחות
    auto end = preface.find("\r\n", pos);
    if (end == std::string::npos) end = preface.size();
    return preface.substr(pos, end - pos);  // מחרוזת הערך
}

// מחלץ את ה-path משורת הבקשה הראשונה של HTTP ("GET /path HTTP/1.1")
std::string request_path(const std::string& preface) {
    auto p1 = preface.find(' ');  // סוף המתודה (GET/POST)
    if (p1 == std::string::npos) return "";
    auto p2 = preface.find(' ', p1 + 1);  // סוף ה-path
    if (p2 == std::string::npos) p2 = preface.find('\r', p1 + 1);
    if (p2 == std::string::npos) return "";
    return preface.substr(p1 + 1, p2 - p1 - 1);  // ה-path עצמו
}

// בודק אם הבקשה היא WebSocket Upgrade ל-path שמתחיל ב-/ws/
// מחזיר true אם שני התנאים מתקיימים
bool is_websocket_upgrade(const std::string& preface) {
    std::string upgrade = header_value(preface, "upgrade");  // קרא את header ה-Upgrade
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(),
                   [](char c){ return (char)std::tolower((unsigned char)c); });  // המר ל-lowercase
    if (upgrade.find("websocket") == std::string::npos) return false;  // אין WebSocket
    const std::string path = request_path(preface);
    return path.rfind("/ws/", 0) == 0;  // בדוק שה-path מתחיל ב-/ws/
}

// פותח חיבור TCP ל-HTTP server הפנימי על 127.0.0.1:port
// מחזיר INVALID_SOCKET אם החיבור נכשל
SOCKET connect_internal(int port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);  // יוצר socket TCP
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);  // פורט בפורמט network
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);  // איפה לאוקייננט
    if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;  // החיבור נכשל
    }
    return s;  // חיבור פתוח
}

// מעביר ביטים בין שני סוקטים בלולאה עד שאחד מהם נסגר
// done הוא דגל משותף בין שתי התילים (client->upstream + upstream->client)
void pipe_bytes(SOCKET from, SOCKET to, std::atomic<bool>& done) {
    constexpr int kBuf = 8192;  // גודל בפר העברה
    std::vector<char> buf(kBuf);
    while (!done) {
        int n = ::recv(from, buf.data(), kBuf, 0);  // קרא מהמקור
        if (n <= 0) break;  // החיבור נסגר או שגיאה
        int sent = 0;
        while (sent < n) {
            int m = ::send(to, buf.data() + sent, n - sent, 0);  // שלח ליעד
            if (m <= 0) { done = true; return; }  // שליחה נכשלה
            sent += m;
        }
    }
    done = true;          // סיימנו — מעדכן done כדי שהתילה השנייה גם תעצור
    shutdown(to, SD_SEND); // סוגר צד השליחה של ה-socket היעד
}

} // namespace

// ---------------------------------------------------------------------------

// בונה את ה-Router — שומר פרמטרים ומצביע ל-hub
Router::Router(int public_port, int internal_http_port, WebSocketHub* hub)
    : public_port_(public_port), internal_port_(internal_http_port), hub_(hub) {}

// השמדאות: קורא ל-stop() לפני הרסת האובייקט
Router::~Router() { stop(); }

// מפעיל את תילת ה-accept loop ברקע
void Router::start() {
    running_ = true;
    accept_thread_ = std::thread(&Router::accept_loop, this);  // תילה חדשה
}

// עוצר את ה-accept loop: סוגר את ה-socket ומחכה לתילה
void Router::stop() {
    running_ = false;
    if (listen_sock_ != INVALID_U) {
        closesocket(to_sock(listen_sock_));  // סגירת ה-socket גורמת ל-accept() להחזיר שגיאה
        listen_sock_ = INVALID_U;
    }
    if (accept_thread_.joinable()) accept_thread_.join();  // מחכה שהתילה תסיים
}

// לולאת האזנה הראשית: פותח socket מאזין, קושר, מאזין
void Router::accept_loop() {
    SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);  // יוצר socket מאזין
    if (srv == INVALID_SOCKET) {
        std::cerr << "[Router] socket() failed\n";
        return;
    }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));  // אפשר שימוש חוזר בפורט מידית אחרי סגירה

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // הקשב לכל הנדדות
    addr.sin_port        = htons((uint16_t)public_port_);  // פורט בנוסח רשת (big endian)

    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        ::listen(srv, SOMAXCONN) == SOCKET_ERROR) {  // SOMAXCONN = תור המתנה מקסימלי
        std::cerr << "[Router] bind/listen failed on port " << public_port_ << "\n";
        closesocket(srv);
        return;
    }
    listen_sock_ = to_uint(srv);  // שמירת ידית ה-socket
    std::cout << "[Router] public port: " << public_port_
              << "  (HTTP -> 127.0.0.1:" << internal_port_
              << ", WebSocket -> in-process hub)\n";

    // לולאת קבלה: פותחת תילה חדשה לכל חיבור נכנס
    while (running_) {
        SOCKET client = ::accept(srv, nullptr, nullptr);  // חוסם לחיבור חדש
        if (client == INVALID_SOCKET) break;  // ה-socket נסגר או שגיאה
        std::thread(&Router::handle_connection, this, to_uint(client)).detach();  // תילה נפרדת
    }
    closesocket(srv);
    listen_sock_ = INVALID_U;
}

// מטפל בחיבור בודד: צוצף את הבקשה, מחליט WebSocket או HTTP, ומנתב
void Router::handle_connection(uintptr_t raw_sock) {
    SOCKET sock = to_sock(raw_sock);

    // הגדרת timeout קצר ל-recv כדי שה-peek לא יתקע בחיבור half-open.
    // בקשות WS/HTTP רגילות משלימות את הבקשה במהירות.
    DWORD tv = 2000;  // 2 שניות timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

    std::string preface = peek_http_preface(sock);  // ציצוף ה-headers
    if (preface.empty()) {
        closesocket(sock);  // אפס נתונים — סגור החיבור
        return;
    }

    if (is_websocket_upgrade(preface) && hub_) {
        // שחזור ל-blocking רגיל — ה-hub מנהל את ה-timeout בעצמו
        DWORD tv0 = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv0, sizeof(tv0));
        hub_->adopt_socket(raw_sock);  // מסירים בעלות של ה-socket ל-hub
        return; // ה-hub בעל ה-socket עכשיו
    }

    proxy_http(raw_sock, preface);  // לא WebSocket — העבר כ-HTTP
}

// העברת HTTP: חיבור לסרבר הפנימי + pipe דוסטרסי (client <-> upstream)
void Router::proxy_http(uintptr_t raw_sock, const std::string& /*preface*/) {
    SOCKET client_sock = to_sock(raw_sock);

    SOCKET upstream = connect_internal(internal_port_);  // חיבור ל-httplib
    if (upstream == INVALID_SOCKET) {
        // הסרבר הפנימי לא ענה — מחזיר  502 Bad Gateway
        const char* resp =
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        ::send(client_sock, resp, (int)std::strlen(resp), 0);
        closesocket(client_sock);
        return;
    }

    // שחזור ל-blocking רגיל לשני הסוקטים
    DWORD tv0 = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv0, sizeof(tv0));

    // שתי תילות pipe במקביל:
    //   t1: client -> upstream
    //   התילה הנוכחית: upstream -> client
    std::atomic<bool> done{false};
    std::thread t1(pipe_bytes, client_sock, upstream, std::ref(done));
    pipe_bytes(upstream, client_sock, done);  // תילה נוכחית עושה upstream->client
    if (t1.joinable()) t1.join();  // מחכה שתילת client->upstream תסיים

    closesocket(upstream);    // סגירת השני הסוקטים
    closesocket(client_sock);
}
