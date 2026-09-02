#pragma once

// ספריות סטנדרטיות לתמיכה בריבוי threads, מנעולים ומבני נתונים
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// winsock לא נכלל כאן — נכלל רק ב-.cpp כדי למנוע התנגשויות עם windows.h
// משתמשים ב-uintptr_t כ-alias ל-SOCKET שעובד גם על 32-bit וגם על 64-bit
#include <cstdint>

// מייצג לקוח WebSocket מחובר יחיד
struct WSClient {
    uintptr_t         sock;            // מזהה ה-socket של הלקוח (SOCKET cast ל-uintptr_t)
    int               intersection_id; // צומת עליו רשום הלקוח; -1 = גלובלי (/ws/updates)
    std::atomic<bool> alive;           // האם החיבור עדיין פעיל
    std::mutex        send_mutex;      // מונע שליחות מקבילות לאותו socket

    WSClient(uintptr_t s, int iid) : sock(s), intersection_id(iid), alive(true) {}
};

// ---------------------------------------------------------------------------
// WebSocketHub — שרת WebSocket פנימי לפרוטוקול RFC-6455
//
// מקשיב על פורט TCP ייעודי, מבצע HTTP-Upgrade handshake,
// ושומר כל חיבור חי ב-thread נפרד.
//
// נתיבים נתמכים:
//   /ws/updates            → מנוי גלובלי (intersection_id = -1)
//   /ws/intersection/{id}  → מנוי לצומת ספציפי
//
// בטיחות threads: כל המתודות הציבוריות בטוחות לקריאה מכל thread.
// ---------------------------------------------------------------------------
class WebSocketHub {
public:
    explicit WebSocketHub(int port = 9001);
    ~WebSocketHub();

    void start();   // מפעיל את לולאת ה-accept ב-thread רקע על הפורט
    void stop();    // סוגר את כל החיבורים; בטוח לקריאה מרובה

    // קולט socket שכבר קיבל accept חיצוני (מ-Router).
    // מבצע RFC-6455 handshake ומפעיל לולאת קריאה ב-thread נפרד.
    // משמש כאשר ה-Router מאחד פורטים ומעביר חיבורי WS ל-hub.
    void adopt_socket(uintptr_t raw_sock);

    // משבית את ה-listener הפנימי — לשימוש כאשר Router חיצוני מזין sockets דרך adopt_socket().
    // חייב להיקרא לפני start(), או שאפשר לוותר לחלוטין על start().
    void disable_own_listener() { own_listener_enabled_ = false; }

    // שידור הודעת JSON לכל הלקוחות המחוברים
    void broadcast_all(const std::string& json_msg);

    // שידור הודעת JSON רק ללקוחות הרשומים לצומת מסוים
    void broadcast_intersection(int intersection_id, const std::string& json_msg);

    int  port() const { return port_; }

private:
    int               port_;                          // הפורט שעליו ה-hub מקשיב
    uintptr_t         listen_sock_{~uintptr_t(0)};    // socket ה-listen (INVALID_SOCKET בהתחלה)
    std::atomic<bool> running_{false};                // האם ה-hub פעיל
    std::atomic<bool> own_listener_enabled_{true};    // האם להפעיל listener פנימי
    std::thread       accept_thread_;                 // thread לולאת ה-accept

    std::mutex                              clients_mutex_; // מגן על רשימת הלקוחות
    std::vector<std::shared_ptr<WSClient>> clients_;        // רשימת כל הלקוחות המחוברים

    void accept_loop();                                          // לולאת קבלת חיבורים חדשים
    void handle_client(uintptr_t sock);                          // טיפול בלקוח בודד (פועל ב-thread נפרד)
    bool perform_handshake(uintptr_t sock, int& out_intersection_id); // HTTP→WS upgrade
    void broadcast_impl(int filter_iid, const std::string& msg); // מימוש השידור הפנימי
};
