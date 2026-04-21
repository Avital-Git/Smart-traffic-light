/**
 * http_client.h
 * -------------
 * HTTP Client פשוט לתקשורת עם שרת ה-FastAPI.
 * משתמש ב-WinHTTP (Windows) לשליחת/קבלת מצב צמתים.
 *
 * אביטל חדד | מכללת בנות בת שבע
 */

#pragma once

#include <string>
#include <sstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace smart_traffic {

class HttpClient {
public:
    HttpClient(const std::string& host = "127.0.0.1", int port = 8000)
        : host_(host), port_(port) {}

    /// שליחת GET request
    std::string get(const std::string& path) const {
#ifdef _WIN32
        return winhttp_request(L"GET", path, "");
#else
        std::cerr << "[HttpClient] Not implemented for this platform\n";
        return "";
#endif
    }

    /// שליחת POST request עם JSON body
    std::string post(const std::string& path, const std::string& json_body) const {
#ifdef _WIN32
        return winhttp_request(L"POST", path, json_body);
#else
        std::cerr << "[HttpClient] Not implemented for this platform\n";
        return "";
#endif
    }

private:
    std::string host_;
    int port_;

#ifdef _WIN32
    std::string winhttp_request(const wchar_t* method, const std::string& path,
                                 const std::string& body) const {
        std::wstring whost(host_.begin(), host_.end());
        std::wstring wpath(path.begin(), path.end());

        HINTERNET session = WinHttpOpen(L"SmartTraffic/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        if (!session) return "";

        HINTERNET connect = WinHttpConnect(session, whost.c_str(), (INTERNET_PORT)port_, 0);
        if (!connect) { WinHttpCloseHandle(session); return ""; }

        HINTERNET request = WinHttpOpenRequest(connect, method, wpath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!request) {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return "";
        }

        const wchar_t* headers = L"Content-Type: application/json\r\n";
        BOOL sent;
        if (body.empty()) {
            sent = WinHttpSendRequest(request, headers, -1,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        } else {
            sent = WinHttpSendRequest(request, headers, -1,
                (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
        }

        std::string result;
        if (sent && WinHttpReceiveResponse(request, nullptr)) {
            DWORD size = 0;
            do {
                WinHttpQueryDataAvailable(request, &size);
                if (size > 0) {
                    std::vector<char> buf(size + 1, 0);
                    DWORD read = 0;
                    WinHttpReadData(request, buf.data(), size, &read);
                    result.append(buf.data(), read);
                }
            } while (size > 0);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }
#endif
};

}  // namespace smart_traffic
