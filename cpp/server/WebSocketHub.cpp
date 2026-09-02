// WebSocketHub.cpp
// שרת WebSocket מינימלי ל-Windows לפי פרוטוקול RFC-6455, באמצעות WinSock גולמי + WinCrypt.
// אין צורך בספריות חיצוניות מעבר ל-ws2_32 ו-advapi32 שכבר מקושרות.

// winsock2.h חייב להופיע לפני windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include "WebSocketHub.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ── המרות עזר בין SOCKET ל-uintptr_t ───────────────────────────────────────
// נדרש כי WSClient מאחסן socket כ-uintptr_t כדי להימנע מ-winsock2.h בהדר
static inline SOCKET to_sock(uintptr_t v) { return (SOCKET)v; }
static inline uintptr_t to_uint(SOCKET s) { return (uintptr_t)s; }
static const uintptr_t INVALID = to_uint(INVALID_SOCKET);

// ── Base64 — קידוד לצורך RFC-6455 handshake ────────────────────────────────
// ממיר bytes גולמיים לייצוג Base64 סטנדרטי (לא base64url)
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* d, size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b = (uint32_t)d[i] << 16;
        if (i+1 < n) b |= (uint32_t)d[i+1] << 8;
        if (i+2 < n) b |= d[i+2];
        out += B64[(b >> 18) & 63];
        out += B64[(b >> 12) & 63];
        out += (i+1 < n) ? B64[(b >>  6) & 63] : '=';
        out += (i+2 < n) ? B64[ b        & 63] : '=';
    }
    return out;
}

// ── SHA-1 דרך WinCrypt (advapi32) ───────────────────────────────────────────
// נדרש לחישוב Sec-WebSocket-Accept לפי RFC-6455:
// SHA1(client_key + GUID) → Base64
static std::vector<uint8_t> sha1(const std::string& data) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::vector<uint8_t> result(20, 0);
    if (!CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return result;
    if (CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptHashData(hHash, (const BYTE*)data.data(), (DWORD)data.size(), 0);
        DWORD len = 20;
        CryptGetHashParam(hHash, HP_HASHVAL, result.data(), &len, 0);
        CryptDestroyHash(hHash);
    }
    CryptReleaseContext(hProv, 0);
    return result;
}

// מחשב את ה-Sec-WebSocket-Accept: key + GUID קבוע → SHA1 → Base64
static std::string ws_accept_key(const std::string& key) {
    auto h = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64_encode(h.data(), h.size());
}

// ── עזרי TCP ────────────────────────────────────────────────────────────────
// קורא בדיוק 'len' bytes — ממשיך עד שקיבל הכל או עד סגירת החיבור
// מחזיר >0 בהצלחה, ≤0 בשגיאה/סגירה
static int recv_exact(SOCKET sock, void* buf, int len) {
    int done = 0;
    while (done < len) {
        int n = ::recv(sock, (char*)buf + done, len - done, 0);
        if (n <= 0) return n == 0 ? 0 : -1;
        done += n;
    }
    return done;
}

// קורא את בקשת ה-HTTP עד \r\n\r\n (סוף ה-headers) או עד מילוי הבאפר
static std::string read_http_request(SOCKET sock) {
    std::string buf;
    buf.reserve(2048);
    char c;
    while (buf.size() < 8192) {
        if (::recv(sock, &c, 1, 0) <= 0) break;
        buf += c;
        if (buf.size() >= 4 &&
            buf[buf.size()-4] == '\r' && buf[buf.size()-3] == '\n' &&
            buf[buf.size()-2] == '\r' && buf[buf.size()-1] == '\n')
            break;
    }
    return buf;
}

// ── עזרי HTTP headers ────────────────────────────────────────────────────────
// שולף ערך header לפי שם (חיפוש case-insensitive)
static std::string req_header(const std::string& req, const std::string& name) {
    std::string lower_req = req;
    std::transform(lower_req.begin(), lower_req.end(), lower_req.begin(), ::tolower);
    std::string target = "\r\n" + name + ":";
    std::transform(target.begin(), target.end(), target.begin(), ::tolower);

    auto pos = lower_req.find(target);
    if (pos == std::string::npos) return "";
    pos += target.size();
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) ++pos;
    auto end = req.find("\r\n", pos);
    if (end == std::string::npos) end = req.size();
    return req.substr(pos, end - pos);
}

// מחלץ את הנתיב מ-request line: "GET /path HTTP/1.1" → "/path"
static std::string req_path(const std::string& req) {
    auto p1 = req.find(' ');
    if (p1 == std::string::npos) return "";
    auto p2 = req.find(' ', p1 + 1);
    if (p2 == std::string::npos) p2 = req.find('\r', p1 + 1);
    if (p2 == std::string::npos) return "";
    return req.substr(p1 + 1, p2 - p1 - 1);
}

// ── עזרי WebSocket frames ────────────────────────────────────────────────────
// שולח frame טקסט ללא masking (שרת→לקוח תמיד ללא mask לפי RFC-6455)
static bool ws_send(SOCKET sock, const std::string& text) {
    size_t len = text.size();
    std::vector<uint8_t> frame;
    frame.reserve(len + 4);
    frame.push_back(0x81);           // FIN=1 + opcode=1 (text frame)
    if (len < 126) {
        frame.push_back((uint8_t)len); // אורך קטן — מוטמע ישירות
    } else {
        frame.push_back(0x7e);       // אורך מורחב 16-bit (126..65535 bytes)
        frame.push_back((uint8_t)(len >> 8));
        frame.push_back((uint8_t)(len & 0xff));
    }
    frame.insert(frame.end(), (const uint8_t*)text.data(),
                              (const uint8_t*)text.data() + len);
    int r = ::send(sock, (const char*)frame.data(), (int)frame.size(), 0);
    return r == (int)frame.size();
}

// שולח close frame ללקוח (FIN + opcode=8, ללא payload)
static void ws_close_frame(SOCKET sock) {
    uint8_t f[2] = {0x88, 0x00};
    ::send(sock, (const char*)f, 2, 0);
}

// ── WebSocketHub — מימוש ─────────────────────────────────────────────────────

WebSocketHub::WebSocketHub(int port) : port_(port) {}

WebSocketHub::~WebSocketHub() { stop(); }

void WebSocketHub::start() {
    running_ = true;
    if (!own_listener_enabled_) {
        // מצב adopt-only: ה-Router החיצוני מחזיק את הפורט ומזין sockets דרך adopt_socket()
        std::cout << "[WSHub] running in adopt-only mode (external Router owns the port)\n";
        return;
    }
    // מצב רגיל: פותח listener עצמאי על הפורט
    accept_thread_ = std::thread(&WebSocketHub::accept_loop, this);
}

void WebSocketHub::stop() {
    running_ = false;

    // סגירת socket ה-listen — מוציא את accept() מהחסימה
    if (listen_sock_ != INVALID) {
        closesocket(to_sock(listen_sock_));
        listen_sock_ = INVALID;
    }

    // סגירת כל sockets הלקוחות — threads מנותקים יתעוררו ויצאו מהלולאה
    std::vector<SOCKET> to_close;
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        for (auto& c : clients_) {
            if (c->alive.exchange(false))
                to_close.push_back(to_sock(c->sock));
        }
        clients_.clear();
    }
    for (SOCKET s : to_close) closesocket(s);
    // thread ה-accept יצא מעצמו לאחר סגירת listen_sock_
}

// ── לולאת קבלת חיבורים ──────────────────────────────────────────────────────
// פותחת socket, מבצעת bind+listen, ולכל חיבור חדש מפעילה handle_client ב-thread נפרד
void WebSocketHub::accept_loop() {
    SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        std::cerr << "[WSHub] socket() failed\n";
        return;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port_);

    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        ::listen(srv, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[WSHub] bind/listen failed on port " << port_ << "\n";
        closesocket(srv);
        return;
    }

    listen_sock_ = to_uint(srv);
    std::cout << "[WSHub] WebSocket listening on ws://0.0.0.0:" << port_ << "\n";
    std::cout << "[WSHub]   /ws/updates              (global)\n";
    std::cout << "[WSHub]   /ws/intersection/{id}    (per-intersection)\n";

    while (running_) {
        SOCKET client = ::accept(srv, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;   // stop() סגר את listen_sock_

        // כל לקוח מטופל ב-thread נפרד מנותק
        std::thread(&WebSocketHub::handle_client, this, to_uint(client)).detach();
    }

    closesocket(srv);
    listen_sock_ = INVALID;
}

// ── טיפול בלקוח בודד ────────────────────────────────────────────────────────
// פועל ב-thread נפרד: handshake → welcome → לולאת קריאה → ניקוי
void WebSocketHub::handle_client(uintptr_t raw_sock) {
    SOCKET sock = to_sock(raw_sock);

    // timeout של 1 שניה — מאפשר ללולאת הקריאה לבדוק את running_ מדי שניה
    DWORD tv = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

    int iid = -1;
    if (!perform_handshake(sock, iid)) {
        closesocket(sock);
        return;
    }

    auto client = std::make_shared<WSClient>(raw_sock, iid);

    // שליחת הודעת ברוכים הבאים (תואמת את פורמט שרת ה-Python)
    double ts = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    json welcome;
    if (iid < 0) {
        welcome = {
            {"event",           "welcome"},
            {"intersection_id", nullptr},
            {"timestamp",       ts},
            {"payload",         {{"message", "connected_to_global_updates"}}},
        };
    } else {
        welcome = {
            {"event",           "welcome"},
            {"intersection_id", iid},
            {"timestamp",       ts},
            {"payload",         {
                {"message",   "connected_to_intersection_updates"},
                {"has_state", false},
            }},
        };
    }
    {
        std::lock_guard<std::mutex> lk(client->send_mutex);
        ws_send(sock, welcome.dump());
    }

    // רישום הלקוח ברשימה הגלובלית של ה-hub
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        clients_.push_back(client);
    }

    // ── לולאת קריאה — מרוקנת frames עד ניתוק ───────────────────────────────
    while (client->alive && running_) {
        uint8_t hdr[2];
        int n = recv_exact(sock, hdr, 2);
        if (n == 0) break;          // סגירה מסודרת מצד הלקוח
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;  // timeout רגיל של 1 שניה — המשך
            break;                  // שגיאת socket אמיתית
        }

        uint8_t  opcode = hdr[0] & 0x0f;
        bool     masked = (hdr[1] & 0x80) != 0;
        uint64_t plen   = hdr[1] & 0x7f;

        if (plen == 126) {
            uint8_t ext[2];
            if (recv_exact(sock, ext, 2) <= 0) break;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8];
            if (recv_exact(sock, ext, 8) <= 0) break;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
        }

        // הגבלת בטיחות: frame בודד לא יעלה על 64 KiB (לקוחות דפדפן שולחים רק pings)
        if (plen > 65536) break;

        uint8_t mask[4] = {};
        if (masked) {
            if (recv_exact(sock, mask, 4) <= 0) break;
        }

        if (plen > 0) {
            std::vector<uint8_t> payload((size_t)plen);
            if (recv_exact(sock, payload.data(), (int)plen) <= 0) break;
            // (Payload content ignored — clients only send keep-alive pings)
        }

        if (opcode == 0x8) {        // close frame — לקוח יזם סגירה
            ws_close_frame(sock);   // מגיב עם close frame
            break;
        }
        // opcode 0x9 = ping מהדפדפן — מטופל על ידי ה-recv timeout, אין צורך בתגובה מיידית
    }

    // ── ניקוי לאחר ניתוק ────────────────────────────────────────────────────
    if (client->alive.exchange(false))
        closesocket(sock);

    // הרשומה הישנה תוסר ברשימה בשידור הבא (broadcast מנקה רשומות מתות)
}

// ── HTTP → WebSocket upgrade handshake ───────────────────────────────────────
// שלבים: קריאת request → אימות Upgrade header → חילוץ Sec-WebSocket-Key
//         → קביעת סוג מנוי לפי הנתיב → שליחת 101 Switching Protocols
bool WebSocketHub::perform_handshake(uintptr_t raw_sock, int& out_iid) {
    SOCKET sock = to_sock(raw_sock);
    std::string req = read_http_request(sock);
    if (req.empty()) return false;

    // חייב להכיל "Upgrade: websocket"
    std::string upgrade = req_header(req, "Upgrade");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
    if (upgrade.find("websocket") == std::string::npos) return false;

    std::string ws_key = req_header(req, "Sec-WebSocket-Key");
    if (ws_key.empty()) return false;

    // קביעת סוג המנוי לפי הנתיב
    std::string path = req_path(req);
    out_iid = -1;  // ברירת מחדל: גלובלי (/ws/updates)

    const std::string iid_prefix = "/ws/intersection/";
    if (path.rfind(iid_prefix, 0) == 0) {
        try { out_iid = std::stoi(path.substr(iid_prefix.size())); }
        catch (...) { out_iid = -1; }
    }
    // /ws/updates → נשאר -1

    // בניית תגובת RFC-6455 101 Switching Protocols
    std::string accept = ws_accept_key(ws_key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "\r\n";

    int r = ::send(sock, resp.data(), (int)resp.size(), 0);
    return r == (int)resp.size();
}

// ── שידור הודעות ─────────────────────────────────────────────────────────────

// שידור לכל הלקוחות ללא סינון
void WebSocketHub::broadcast_all(const std::string& msg) {
    broadcast_impl(-1, msg);
}

void WebSocketHub::adopt_socket(uintptr_t raw_sock) {
    // מפעיל handle_client ב-thread מנותק כדי שה-Router יוכל לחזור מיד לקבל חיבורים
    running_ = true; // בטוח גם אם start() טרם נקרא
    std::thread(&WebSocketHub::handle_client, this, raw_sock).detach();
}

// שידור רק ללקוחות הרשומים לצומת מסוים
void WebSocketHub::broadcast_intersection(int iid, const std::string& msg) {
    broadcast_impl(iid, msg);
}

void WebSocketHub::broadcast_impl(int filter_iid, const std::string& msg) {
    // לוקח snapshot מהרשימה תחת lock (קצר), ואז שולח מחוץ ל-lock כדי למנוע deadlock
    std::vector<std::shared_ptr<WSClient>> snapshot;
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        // ניקוי רשומות מתות תוך כדי שיש לנו את ה-lock
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [](const auto& c){ return !c->alive; }),
            clients_.end());
        snapshot = clients_;
    }

    for (auto& c : snapshot) {
        if (!c->alive) continue;
        // filter_iid < 0  → broadcast_all  → שולח לכולם
        // filter_iid >= 0 → לצומת ספציפי → שולח למנויים של אותו צומת
        //                   וגם למנויים גלובליים (intersection_id == -1, /ws/updates)
        //                   — תואם את ה-LiveUpdateHub של Python
        if (filter_iid >= 0 &&
            c->intersection_id != filter_iid &&
            c->intersection_id != -1) continue;

        std::lock_guard<std::mutex> lk(c->send_mutex);
        if (!ws_send(to_sock(c->sock), msg)) {
            // שגיאת שליחה — מסמן את הלקוח כמת וסוגר את ה-socket
            if (c->alive.exchange(false))
                closesocket(to_sock(c->sock));
        }
    }
}
