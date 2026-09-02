#pragma once

// Router.h
// מאזן אחדודי ל-HTTP ו-WebSocket על אותו פורט
//
// מקבל כל חיבור נכנס בפורט הציבורי (8000 ברירת מחדל),
// מציץ בשקיפה את שורת הבקשה וה-headers, ומנתב כך:
//   - בקשה עם "Upgrade: websocket" ו-path שמתחיל ב-/ws/  -> WebSocketHub::adopt_socket()
//   - כל השאר                                          -> מועבר ל-cpp-httplib הפנימי
//
// רקע: cpp-httplib לא חושף את ה-socket הגולמי מתוך handler,
// ולכן לקוח React אין אפשרות לעשות Upgrade ל-WebSocket באותו פורט בצורה ישירה.
// בעלות שקע ההאזנה כאן היא הדרך הנקייה ביותר לשמור על
// ה-URL הקבוע: ws://127.0.0.1:8000

#include <atomic>    // std::atomic — דגל running_ בטוח לבין תילים
#include <cstdint>   // uintptr_t — שמירת ידית socket כמספר
#include <string>    // std::string
#include <thread>    // std::thread — תילת ה-accept loop

class WebSocketHub;  // הכרזה מקדימה — הגדרה מלאה ב-WebSocketHub.h

class Router {
public:
    // בונה: public_port = פורט לקוח, internal_http_port = פורט פנימי של cpp-httplib
    Router(int public_port, int internal_http_port, WebSocketHub* hub);
    ~Router();  // קורא ל-stop() אוטומטית

    void start();  // מפעיל תילת accept loop ברקע
    void stop();   // עוצר בטוחה — אפשר לקרוא מספר פעמים

    int public_port()   const { return public_port_; }   // פורט הציבורי (8000)
    int internal_port() const { return internal_port_; } // פורט פנימי של cpp-httplib

private:
    void accept_loop();                                      // לולאת קבלת חיבורים
    void handle_connection(uintptr_t raw_sock);              // בודק אם WebSocket או HTTP ומנתב
    void proxy_http(uintptr_t raw_sock, const std::string& preface);  // מעביר HTTP לסרבר הפנימי

    int               public_port_;          // פורט שנשמע ללקוח
    int               internal_port_;        // פורט של cpp-httplib (לא נחשף)
    WebSocketHub*     hub_;                  // מצביע ל-WS hub — אינו בבעלות
    uintptr_t         listen_sock_{~uintptr_t(0)};  // ידית socket המאזין
    std::atomic<bool> running_{false};       // דגל הפעלה — false מעצור את הלולא
    std::thread       accept_thread_;        // תילת ה-accept loop
};
