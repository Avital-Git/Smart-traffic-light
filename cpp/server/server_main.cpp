// נקודת הכניסה הראשית של שרת התנועה החכם
// קובץ זה אחראי על אתחול כל רכיבי השרת ותיאום ביניהם:
//   - TrafficServer  — שרת HTTP פנימי (cpp-httplib) על loopback
//   - Router         — שכבת פרוקסי חיצונית שמאחדת HTTP ו-WebSocket לפורט אחד
//   - WebSocketHub   — ניהול חיבורי WebSocket עם לקוחות (דשבורד React)

#include "TrafficServer.h"
#include "Router.h"
#include "WebSocketHub.h"

// winsock2.h חייב להופיע לפני windows.h,
// ו-WSAStartup חייב להיקרא פעם אחת לפני כל קוד רשת
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>   // std::chrono::milliseconds
#include <csignal>  // signal, SIGINT, SIGTERM
#include <iostream> // std::cout, std::cerr
#include <string>   // std::stoi
#include <thread>   // std::thread

// קישור ספריית Winsock בזמן בנייה
#pragma comment(lib, "ws2_32.lib")

// מצביעים גלובליים לשימוש ב-signal handler — לא נגישים אחרת מתוך פונקציה סטטית
static TrafficServer* g_server = nullptr;
static Router*        g_router = nullptr;

// טיפול בסיגנלים (Ctrl+C / kill) — עוצר בצורה מסודרת את ה-Router ואת השרת
static void handle_signal(int /*sig*/)
{
    std::cout << "\n[TrafficServer] Shutting down...\n";
    if (g_router) g_router->stop(); // עצור קבלת חיבורים חדשים מבחוץ
    if (g_server) g_server->stop(); // עצור את שרת ה-HTTP הפנימי
}

int main(int argc, char* argv[])
{
    // פורט 8000 — כתובת ציבורית שאליה מתחבר הדשבורד ושאר לקוחות
    // פורט 18000 — פנימי בלבד (loopback), לא נחשף החוצה, רק Router מדבר איתו
    int public_port   = 8000;
    int internal_port = 18000;

    // אפשרות לעקוף את הפורטים דרך ארגומנטים בשורת הפקודה:
    //   traffic_server.exe [public_port] [internal_port]
    if (argc >= 2) { try { public_port   = std::stoi(argv[1]); } catch (...) {} }
    if (argc >= 3) { try { internal_port = std::stoi(argv[2]); } catch (...) {} }

    // רישום handlers לסיגנלים — מאפשר כיבוי מסודר בלחיצת Ctrl+C
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    // אתחול Winsock — חובה לפני כל פעולת רשת ב-Windows
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[TrafficServer] WSAStartup failed\n";
        return 1;
    }

    // יצירת שרת ה-HTTP הפנימי
    // ws_port=0 כי WebSocket מנוהל ישירות על ידי ה-Router ולא על ידי cpp-httplib
    TrafficServer server(internal_port, /*ws_port unused in router mode*/ 0);
    server.enable_router_mode(internal_port); // אמר לשרת לעבוד במצב router (לא לפתוח פורט ציבורי בעצמו)
    g_server = &server;

    // הפעלת שרת ה-HTTP על thread נפרד — כדי שה-Router יוכל לרוץ על ה-thread הראשי
    // server.run() חוסם עד ל-stop(), לכן חייב להיות ב-thread נפרד
    std::thread http_thread([&server]() { server.run(); });

    // המתנה קצרה כדי לתת ל-httplib לסיים את ה-bind לפני שה-Router מתחיל להעביר בקשות
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // יצירת ה-Router — מאחד HTTP ו-WebSocket על פורט ציבורי אחד
    // מעביר HTTP ל-internal_port, ו-WebSocket ישירות ל-hub
    Router router(public_port, internal_port, server.hub());
    g_router = &router;
    router.start(); // מתחיל accept loop ב-thread נפרד

    std::cout << "[TrafficServer] Ready. Public endpoint: http://127.0.0.1:"
              << public_port << "  (HTTP + WebSocket)\n";

    // המתנה לסיום thread ה-HTTP (יסתיים כשתיקרא server.stop())
    http_thread.join();

    // ניקוי סופי — עצירת ה-Router ושחרור Winsock
    router.stop();
    WSACleanup();
    return 0;
}
