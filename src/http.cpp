// WinHTTP client for Windows. http_curl.cpp is the macOS/Linux one.
#include "http.h"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace http {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

struct Url {
    bool secure = false;
    std::wstring host;
    int port = 80;
    std::wstring path = L"/";
};

bool parse_url(const std::string& url, Url& out) {
    std::string rest = url;
    if (rest.rfind("https://", 0) == 0) {
        out.secure = true;
        out.port = 443;
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        out.secure = false;
        out.port = 80;
        rest = rest.substr(7);
    } else if (rest.find("://") != std::string::npos) {
        return false; // unsupported scheme
    }

    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (authority.empty()) return false;

    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.port = std::atoi(authority.c_str() + colon + 1);
        if (out.port <= 0) return false;
        authority = authority.substr(0, colon);
    }

    out.host = widen(authority);
    out.path = widen(path);
    return !out.host.empty();
}

std::string last_error(const char* stage) {
    DWORD code = GetLastError();
    char buf[256];
    switch (code) {
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            std::snprintf(buf, sizeof(buf), "cannot reach server (%s)", stage);
            break;
        case ERROR_WINHTTP_TIMEOUT:
            std::snprintf(buf, sizeof(buf), "request timed out (%s)", stage);
            break;
        default:
            std::snprintf(buf, sizeof(buf), "%s failed (win32 error %lu)", stage, (unsigned long)code);
            break;
    }
    return buf;
}

// RAII for WinHTTP handles.
struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HINTERNET() const { return h; }
};

} // namespace

Response request(const std::string& method,
                 const std::string& url,
                 const std::string& body,
                 const ChunkFn& on_chunk,
                 std::atomic<bool>* cancel,
                 int connect_timeout_ms,
                 int read_timeout_ms) {
    Response resp;

    Url u;
    if (!parse_url(url, u)) {
        resp.error = "invalid URL: " + url;
        return resp;
    }

    Handle session(WinHttpOpen(L"Cortex/1.0",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        resp.error = last_error("session open");
        return resp;
    }
    WinHttpSetTimeouts(session, connect_timeout_ms, connect_timeout_ms, read_timeout_ms, read_timeout_ms);

    Handle connect(WinHttpConnect(session, u.host.c_str(), (INTERNET_PORT)u.port, 0));
    if (!connect) {
        resp.error = last_error("connect");
        return resp;
    }

    DWORD flags = WINHTTP_FLAG_REFRESH | (u.secure ? WINHTTP_FLAG_SECURE : 0);
    Handle req(WinHttpOpenRequest(connect, widen(method).c_str(), u.path.c_str(),
                                  nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req) {
        resp.error = last_error("open request");
        return resp;
    }

    const wchar_t* headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    BOOL sent = WinHttpSendRequest(req, headers, (DWORD)-1,
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                   (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent) {
        resp.error = last_error("send");
        return resp;
    }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        resp.error = last_error("receive");
        return resp;
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    resp.status = (int)status;

    // Error bodies are always buffered so the caller can report the reason.
    const bool stream = on_chunk && status >= 200 && status < 300;
    std::vector<char> buf(16 * 1024);
    while (true) {
        if (cancel && cancel->load()) {
            resp.error = "cancelled";
            return resp;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            resp.error = last_error("read");
            return resp;
        }
        if (avail == 0) break;
        if (avail > buf.size()) buf.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &read)) {
            resp.error = last_error("read");
            return resp;
        }
        if (read == 0) break;
        if (stream) {
            if (!on_chunk(buf.data(), (size_t)read)) {
                resp.error = "cancelled";
                return resp;
            }
        } else {
            resp.body.append(buf.data(), read);
        }
    }

    resp.ok = status >= 200 && status < 300;
    if (!resp.ok && resp.error.empty())
        resp.error = "HTTP " + std::to_string(resp.status);
    return resp;
}

} // namespace http

#endif // _WIN32
