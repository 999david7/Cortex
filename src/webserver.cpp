#include "webserver.h"

#ifdef _WIN32
// Winsock has to come before windows.h, or windows.h pulls in the 1.1 headers.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cwchar>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sys.h"

#ifndef _WIN32
// The few Winsock names the server is written against, on BSD sockets.
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
inline int closesocket(SOCKET s) { return close(s); }
#endif

namespace web {

namespace {

#ifdef _WIN32
constexpr wchar_t kSep = L'\\';
#else
constexpr wchar_t kSep = L'/';
#endif

constexpr int kWorkerThreads = 6;
constexpr size_t kMaxRequestBytes = 32 * 1024;
constexpr size_t kStreamChunk = 64 * 1024;

// ------------------------------------------------------------------- strings

std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string extension_of(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return to_lower(path.substr(dot + 1));
}

// Everything a static site actually serves. Unknown types are handed over as
// a byte stream rather than guessed at, which keeps the browser from running
// something it should have downloaded.
const char* mime_for(const std::string& path) {
    const std::string ext = extension_of(path);
    struct Row { const char* ext; const char* type; };
    static const Row kTypes[] = {
        {"html", "text/html; charset=utf-8"},   {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},     {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"json", "application/json; charset=utf-8"},
        {"map", "application/json; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"},   {"md", "text/plain; charset=utf-8"},
        {"csv", "text/csv; charset=utf-8"},     {"xml", "application/xml; charset=utf-8"},
        {"svg", "image/svg+xml"},               {"png", "image/png"},
        {"jpg", "image/jpeg"},                  {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},                   {"webp", "image/webp"},
        {"avif", "image/avif"},                 {"bmp", "image/bmp"},
        {"ico", "image/x-icon"},                {"woff", "font/woff"},
        {"woff2", "font/woff2"},                {"ttf", "font/ttf"},
        {"otf", "font/otf"},                    {"eot", "application/vnd.ms-fontobject"},
        {"mp4", "video/mp4"},                   {"webm", "video/webm"},
        {"ogv", "video/ogg"},                   {"mp3", "audio/mpeg"},
        {"wav", "audio/wav"},                   {"ogg", "audio/ogg"},
        {"m4a", "audio/mp4"},                   {"flac", "audio/flac"},
        {"wasm", "application/wasm"},           {"pdf", "application/pdf"},
        {"zip", "application/zip"},
    };
    for (const Row& r : kTypes)
        if (ext == r.ext) return r.type;
    return "application/octet-stream";
}

bool is_html(const std::string& path) {
    const std::string ext = extension_of(path);
    return ext == "html" || ext == "htm";
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_value(s[i + 1]), lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string url_encode_path(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                          c == '~' || c == '/';
        if (safe) {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

std::string html_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// --------------------------------------------------------------------- net

bool send_all(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int chunk = (int)std::min<size_t>(len - sent, 1 << 20);
        const int n = ::send(s, data + sent, chunk, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

bool send_all(SOCKET s, const std::string& data) {
    return send_all(s, data.data(), data.size());
}

const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 416: return "Range Not Satisfiable";
        default: return "Internal Server Error";
    }
}

std::string header_block(int code, const char* content_type, long long length,
                         const std::string& extra) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "HTTP/1.1 %d %s\r\n"
                  "Content-Type: %s\r\n"
                  "Content-Length: %lld\r\n"
                  // A preview must never be served from the browser cache, or
                  // the reload shows the page you just replaced.
                  "Cache-Control: no-store, must-revalidate\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Connection: close\r\n",
                  code, status_text(code), content_type, length);
    return std::string(buf) + extra + "\r\n";
}

void send_simple(SOCKET s, int code, const char* content_type, const std::string& body,
                 bool head_only, const std::string& extra = std::string()) {
    if (!send_all(s, header_block(code, content_type, (long long)body.size(), extra))) return;
    if (!head_only) send_all(s, body);
}

// The IDE's own visual language, so a directory listing or an error page looks
// like it came from this program rather than from a 1996 web server.
std::string page(const std::string& title, const std::string& body) {
    return "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>" + html_escape(title) + "</title><style>"
           "*{box-sizing:border-box}"
           "body{margin:0;padding:56px 40px;background:#fff;color:#141413;"
           "font:13px/1.7 ui-monospace,'Cascadia Mono',Consolas,monospace}"
           "h1{margin:0 0 4px;font-size:13px;font-weight:400;letter-spacing:.18em;"
           "text-transform:uppercase}"
           ".sub{color:#b6b6b1;letter-spacing:.14em;text-transform:uppercase;font-size:11px}"
           "hr{border:0;border-top:1px solid #e4e4e0;margin:22px 0}"
           "ul{list-style:none;margin:0;padding:0}"
           "li{border-bottom:1px solid #f0f0ee}"
           "a{display:flex;gap:16px;padding:9px 2px;color:#141413;text-decoration:none}"
           "a:hover{background:#f6f6f4}"
           ".k{color:#b6b6b1;letter-spacing:.14em;text-transform:uppercase;font-size:10px;"
           "min-width:52px}"
           "</style></head><body>" + body + "</body></html>";
}

[[maybe_unused]] bool ends_with_w(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

[[maybe_unused]] std::wstring lower_w(std::wstring s) {
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return s;
}

// Paths stay wide end to end. Windows and macOS file systems ignore case, so
// the "is this inside the root" checks do too; Linux compares exactly.
std::wstring fold(const std::wstring& s) {
#if defined(_WIN32) || defined(__APPLE__)
    return lower_w(s);
#else
    return s;
#endif
}

bool full_path(const std::wstring& in, std::wstring& out) {
#ifdef _WIN32
    wchar_t full[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(in.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (n == 0 || n >= sizeof(full) / sizeof(full[0])) return false;
    out = full;
    return true;
#else
    if (in.empty()) return false;
    out = sys::widen(sys::normalize(sys::narrow(in)));
    return true;
#endif
}

// True when the path exists; is_dir says whether it is a folder.
bool path_info(const std::wstring& path, bool& is_dir) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
#else
    struct stat st {};
    if (stat(sys::narrow(path).c_str(), &st) != 0) return false;
    is_dir = S_ISDIR(st.st_mode);
    return true;
#endif
}

// A file opened for streaming: size, seek and read, on either system.
class InFile {
public:
    ~InFile() { close(); }
    bool open(const std::wstring& path) {
#ifdef _WIN32
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h_, &size)) return false;
        size_ = size.QuadPart;
        return true;
#else
        f_ = std::fopen(sys::narrow(path).c_str(), "rb");
        if (!f_) return false;
        struct stat st {};
        if (fstat(fileno(f_), &st) != 0) return false;
        size_ = (long long)st.st_size;
        return true;
#endif
    }
    long long size() const { return size_; }
    bool seek(long long at) {
#ifdef _WIN32
        LARGE_INTEGER pos{};
        pos.QuadPart = at;
        return SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) != 0;
#else
        return fseeko(f_, (off_t)at, SEEK_SET) == 0;
#endif
    }
    // Returns the bytes read; 0 at the end or on an error.
    size_t read(char* buf, size_t want) {
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(h_, buf, (DWORD)want, &got, nullptr)) return 0;
        return got;
#else
        return std::fread(buf, 1, want, f_);
#endif
    }
    void close() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
#else
        if (f_) std::fclose(f_);
        f_ = nullptr;
#endif
    }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    FILE* f_ = nullptr;
#endif
    long long size_ = 0;
};

bool read_whole_file(const std::wstring& path, std::string& out) {
    InFile f;
    if (!f.open(path) || f.size() > (long long)64 * 1024 * 1024) return false;
    out.resize((size_t)f.size());
    size_t done = 0;
    while (done < out.size()) {
        const size_t got = f.read(&out[done], std::min<size_t>(out.size() - done, 1 << 20));
        if (got == 0) return false;
        done += got;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------- lifecycle

LiveServer::~LiveServer() { stop(); }

std::string LiveServer::root() const { return root_; }

std::string LiveServer::origin() const {
    if (!running_.load()) return std::string();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "http://127.0.0.1:%d", port_.load());
    return buf;
}

std::string LiveServer::url_for(const std::string& absolute_path) const {
    const std::string base = origin();
    if (base.empty()) return base;
    if (absolute_path.empty()) return base + "/";

    // The file has to sit inside the root for the server to be able to reach it.
    std::wstring file;
    if (!full_path(sys::widen(absolute_path), file)) return base + "/";
    const std::wstring lf = fold(file), lr = fold(root_w_);
    if (lf.size() < lr.size() || lf.compare(0, lr.size(), lr) != 0) return base + "/";

    std::string rel = sys::narrow(file.substr(lr.size()));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return base + "/" + url_encode_path(rel);
}

void LiveServer::log(const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mu_);
    // The log is drained every frame; the cap only matters if the UI is busy.
    if (log_.size() < 200) log_.push_back(line);
}

std::vector<std::string> LiveServer::take_log() {
    std::lock_guard<std::mutex> lock(log_mu_);
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

bool LiveServer::start(const std::string& root, int port, std::string& err) {
    stop();

    if (root.empty()) {
        err = "Open a folder first — the server needs something to serve.";
        return false;
    }
    std::wstring full;
    if (!full_path(sys::widen(root), full)) {
        err = "That folder path could not be resolved.";
        return false;
    }
    bool root_is_dir = false;
    if (!path_info(full, root_is_dir) || !root_is_dir) {
        err = "That folder does not exist.";
        return false;
    }

    root_w_ = full;
    if (root_w_.empty() || root_w_.back() != kSep) root_w_ += kSep;
    root_ = sys::narrow(root_w_.size() > 1 ? root_w_.substr(0, root_w_.size() - 1) : root_w_);

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "Windows sockets could not be started.";
        return false;
    }
    wsa_started_ = true;
#endif

    // Loopback only: this is a preview for the machine it runs on, not a way to
    // put the folder on the network.
    SOCKET listener = INVALID_SOCKET;
    int chosen = 0;
    if (port < 1 || port > 65535 - 20) port = 5500;
    for (int p = port; p <= port + 20; ++p) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) break;
#ifndef _WIN32
        // Here SO_REUSEADDR only lets a restarted server take its port back
        // from TIME_WAIT; it never lets two listeners share one.
        int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        fcntl(listener, F_SETFD, FD_CLOEXEC);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)p);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // SO_REUSEADDR is deliberately not set: on Windows it lets a second
        // program bind a port that is already in use, and silently sharing the
        // port with something else is worse than moving to the next one.
        if (bind(listener, (const sockaddr*)&addr, sizeof(addr)) == 0 &&
            listen(listener, SOMAXCONN) == 0) {
            chosen = p;
            break;
        }
        closesocket(listener);
        listener = INVALID_SOCKET;
    }
    if (listener == INVALID_SOCKET || chosen == 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Ports %d-%d are all in use.", port, port + 20);
        err = buf;
#ifdef _WIN32
        WSACleanup();
#endif
        wsa_started_ = false;
        return false;
    }

    listen_.store((uintptr_t)listener);
    port_.store(chosen);
    stopping_.store(false);
    running_.store(true);
    served_.store(0);
    token_.fetch_add(1);

#ifdef _WIN32
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif

    workers_.reserve(kWorkerThreads);
    for (int i = 0; i < kWorkerThreads; ++i)
        workers_.emplace_back([this] { worker_loop(); });
    acceptor_ = std::thread([this] { accept_loop(); });
    watcher_ = std::thread([this] { watch_loop(); });

    char msg[320];
    std::snprintf(msg, sizeof(msg), "serving %s at http://127.0.0.1:%d", root_.c_str(), chosen);
    log(msg);
    return true;
}

void LiveServer::stop() {
    if (!running_.load() && !stopping_.load() && !acceptor_.joinable()) {
        // Never started, or already fully stopped.
#ifdef _WIN32
        if (wsa_started_) WSACleanup();
#endif
        wsa_started_ = false;
        return;
    }

    stopping_.store(true);

    // Closing the listening socket is what makes the blocked accept() return.
    const uintptr_t listener = listen_.exchange((uintptr_t)-1);
    if (listener != (uintptr_t)-1) closesocket((SOCKET)listener);
#ifdef _WIN32
    if (stop_event_) SetEvent((HANDLE)stop_event_);
#endif
    queue_cv_.notify_all();

    if (acceptor_.joinable()) acceptor_.join();
    for (std::thread& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
    if (watcher_.joinable()) watcher_.join();

    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        while (!queue_.empty()) {
            closesocket((SOCKET)queue_.front());
            queue_.pop_front();
        }
    }

#ifdef _WIN32
    if (stop_event_) CloseHandle((HANDLE)stop_event_);
    if (wsa_started_) WSACleanup();
#endif
    stop_event_ = nullptr;
    wsa_started_ = false;

    const bool was_running = running_.exchange(false);
    stopping_.store(false);
    port_.store(0);
    if (was_running) log("stopped");
}

// ------------------------------------------------------------------ threads

void LiveServer::accept_loop() {
    while (!stopping_.load()) {
        const uintptr_t listener = listen_.load();
        if (listener == (uintptr_t)-1) break;

#ifndef _WIN32
        // Closing a socket does not wake a thread blocked in accept() here the
        // way it does on Windows, so wait in short slices and check for stop.
        pollfd pfd{};
        pfd.fd = (SOCKET)listener;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 250);
        if (ready <= 0) continue;
#endif

        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        const SOCKET client = accept((SOCKET)listener, (sockaddr*)&from, &from_len);
        if (client == INVALID_SOCKET) {
            if (stopping_.load()) break;
            // stop() closes the listener, which is the only fatal case; a client
            // that vanished mid-handshake just means "try the next one".
#ifdef _WIN32
            const int e = WSAGetLastError();
            if (e == WSAECONNRESET || e == WSAEINTR || e == WSAEMFILE || e == WSAENOBUFS) continue;
#else
            const int e = errno;
            if (e == ECONNABORTED || e == EINTR || e == EMFILE || e == ENFILE || e == ENOBUFS ||
                e == EAGAIN || e == EWOULDBLOCK)
                continue;
#endif
            break;
        }
#ifndef _WIN32
        fcntl(client, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int one = 1; // a browser that hangs up must not kill the IDE with SIGPIPE
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif

        {
            std::lock_guard<std::mutex> lock(queue_mu_);
            // A backlog this deep means the browser is hammering us; dropping is
            // better than growing without bound.
            if (queue_.size() > 64) {
                closesocket(client);
                continue;
            }
            queue_.push_back((uintptr_t)client);
        }
        queue_cv_.notify_one();
    }
}

void LiveServer::worker_loop() {
    for (;;) {
        uintptr_t sock = (uintptr_t)-1;
        {
            std::unique_lock<std::mutex> lock(queue_mu_);
            queue_cv_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_.load()) return;
                continue;
            }
            sock = queue_.front();
            queue_.pop_front();
        }
        serve(sock);
        closesocket((SOCKET)sock);
    }
}

#ifdef _WIN32
void LiveServer::watch_loop() {
    HANDLE dir = CreateFileW(root_w_.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                             nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        log("could not watch the folder for changes; reload is manual");
        return;
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<char> buf(64 * 1024);

    while (!stopping_.load()) {
        ResetEvent(ov.hEvent);
        if (!ReadDirectoryChangesW(dir, buf.data(), (DWORD)buf.size(), TRUE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                       FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                   nullptr, &ov, nullptr))
            break;

        HANDLE waits[2] = {ov.hEvent, (HANDLE)stop_event_};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) break;

        DWORD got = 0;
        if (!GetOverlappedResult(dir, &ov, &got, FALSE)) break;
        if (got == 0) {
            // The buffer overflowed, so the detail is gone but something moved.
            token_.fetch_add(1);
            continue;
        }

        bool relevant = false;
        for (size_t offset = 0; offset + sizeof(FILE_NOTIFY_INFORMATION) <= got;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf.data() + offset);
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            const std::wstring lower = lower_w(name);

            // Editors write temporaries next to the real file, and a package
            // manager churning through node_modules would reload the page all
            // day. Neither is a change to the site.
            const bool noisy = lower.find(L".git\\") != std::wstring::npos ||
                               lower.find(L"node_modules") != std::wstring::npos ||
                               lower.find(L'~') != std::wstring::npos ||
                               ends_with_w(lower, L".tmp") || ends_with_w(lower, L".swp") ||
                               ends_with_w(lower, L".crswap");
            if (!noisy) relevant = true;

            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }
        if (relevant) token_.fetch_add(1);
    }

    CancelIo(dir);
    // Let the pending read finish reporting before the handle goes away.
    DWORD ignored = 0;
    GetOverlappedResult(dir, &ov, &ignored, TRUE);
    CloseHandle(ov.hEvent);
    CloseHandle(dir);
}
#else
namespace {

// A fingerprint of the tree: every file's path, size and modification time
// folded into one number. Two scans that differ mean something changed.
void fingerprint(const std::string& dir, int depth, unsigned long long& hash, int& budget) {
    if (depth > 24 || budget <= 0) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (--budget <= 0) break;
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        // The same noise the Windows watcher ignores: version control,
        // dependencies, and editors' temporaries.
        const bool temp = name.size() > 4 && (name.compare(name.size() - 4, 4, ".tmp") == 0 ||
                                              name.compare(name.size() - 4, 4, ".swp") == 0);
        if (name == ".git" || name == "node_modules" || name == ".DS_Store" ||
            name.find('~') != std::string::npos || temp)
            continue;
        const std::string path = dir + "/" + name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) continue;
        for (char c : path) hash = hash * 1099511628211ull ^ (unsigned char)c;
#ifdef __APPLE__
        const long long mtime_ns = (long long)st.st_mtimespec.tv_sec * 1000000000ll + st.st_mtimespec.tv_nsec;
#else
        const long long mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
        hash = hash * 1099511628211ull ^ (unsigned long long)mtime_ns;
        hash = hash * 1099511628211ull ^ (unsigned long long)st.st_size;
        if (S_ISDIR(st.st_mode)) fingerprint(path, depth + 1, hash, budget);
    }
    closedir(d);
}

} // namespace

// Without ReadDirectoryChangesW, the folder is scanned twice a second and the
// page reloads when the fingerprint moves. A site folder is small enough for
// that to cost nothing; the budget keeps a huge one from costing much.
void LiveServer::watch_loop() {
    const std::string root = root_;
    auto scan = [&root] {
        unsigned long long hash = 1469598103934665603ull;
        int budget = 20000;
        fingerprint(root, 0, hash, budget);
        return hash;
    };
    unsigned long long last = scan();
    while (!stopping_.load()) {
        for (int i = 0; i < 5 && !stopping_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stopping_.load()) break;
        const unsigned long long now = scan();
        if (now != last) {
            last = now;
            token_.fetch_add(1);
        }
    }
}
#endif

// ------------------------------------------------------------------ serving

bool LiveServer::resolve(const std::string& url_path, std::wstring& out_file, bool& is_dir) const {
    std::string rel = url_decode(url_path);
    if (rel.empty() || rel[0] != '/') return false;
    rel.erase(0, 1);

    // Anything that could climb out of the root, name a device or open an
    // alternate stream is refused before it reaches the file system.
    if (rel.find('\0') != std::string::npos) return false;
    if (rel.find(':') != std::string::npos) return false;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t slash = rel.find('/', start);
        const std::string part = rel.substr(start, slash == std::string::npos ? std::string::npos
                                                                              : slash - start);
        if (part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    std::wstring candidate = root_w_ + sys::widen(rel);
    std::replace(candidate.begin(), candidate.end(), L'/', kSep);

    std::wstring resolved;
    if (!full_path(candidate, resolved)) return false;

    // The canonical path must still start with the canonical root: this is the
    // check that actually keeps the server inside the folder.
    const std::wstring lr = fold(root_w_), lf = fold(resolved);
    const std::wstring root_no_slash = lr.substr(0, lr.size() - 1);
    if (lf != root_no_slash && (lf.size() < lr.size() || lf.compare(0, lr.size(), lr) != 0))
        return false;

    if (!path_info(resolved, is_dir)) return false;
    out_file = resolved;
    return true;
}

void LiveServer::serve(uintptr_t raw) {
    const SOCKET s = (SOCKET)raw;

#ifdef _WIN32
    DWORD timeout = 5000;
#else
    timeval timeout{};
    timeout.tv_sec = 5;
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    // --- read the request head ---
    std::string head;
    size_t head_end = std::string::npos;
    char chunk[4096];
    while (head.size() < kMaxRequestBytes) {
        const int n = recv(s, chunk, (int)sizeof(chunk), 0);
        if (n <= 0) break;
        head.append(chunk, (size_t)n);
        head_end = head.find("\r\n\r\n");
        if (head_end != std::string::npos) break;
    }
    if (head_end == std::string::npos) return;

    const size_t line_end = head.find("\r\n");
    const std::string request_line = head.substr(0, line_end);
    const size_t sp1 = request_line.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        send_simple(s, 400, "text/plain; charset=utf-8", "Bad Request", false);
        return;
    }

    const std::string method = request_line.substr(0, sp1);
    std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    const bool head_only = method == "HEAD";
    if (method != "GET" && !head_only) {
        send_simple(s, 405, "text/plain; charset=utf-8", "Only GET and HEAD are served.",
                    false, "Allow: GET, HEAD\r\n");
        return;
    }

    if (const size_t q = target.find_first_of("?#"); q != std::string::npos) target.erase(q);
    if (target.empty()) target = "/";

    // --- the live-reload endpoint ---
    if (target == "/__livereload") {
        char body[32];
        std::snprintf(body, sizeof(body), "%lld", token_.load());
        send_simple(s, 200, "text/plain; charset=utf-8", body, head_only);
        return;
    }

    served_.fetch_add(1);

    // --- map onto a file ---
    std::wstring file;
    bool is_dir = false;
    if (!resolve(target, file, is_dir)) {
        log("404  " + target);
        send_simple(s, 404, "text/html; charset=utf-8",
                    page("404", "<h1>Not found</h1><div class=\"sub\">" +
                                    html_escape(target) + "</div><hr>"),
                    head_only);
        return;
    }

    if (is_dir) {
        // A directory URL without its trailing slash breaks every relative link
        // on the page it is about to serve, so send the browser to the right one.
        if (target.back() != '/') {
            const std::string location = url_encode_path(target) + "/";
            send_simple(s, 301, "text/plain; charset=utf-8", "", head_only,
                        "Location: " + location + "\r\n");
            return;
        }

        std::wstring index;
        bool index_is_dir = false;
        if (resolve(target + "index.html", index, index_is_dir) && !index_is_dir) {
            file = index;
            is_dir = false;
        } else if (resolve(target + "index.htm", index, index_is_dir) && !index_is_dir) {
            file = index;
            is_dir = false;
        }
    }

    if (is_dir) {
        // No index: list what is there, which is far more useful than a 403.
        std::vector<sys::DirEntry> entries;
        sys::list_directory(sys::narrow(file), entries);

        // Landing on a listing instead of a site almost always means the pages
        // are one folder further in, so say which folder rather than making the
        // reader guess.
        std::string elsewhere;
        for (const sys::DirEntry& e : entries) {
            if (!e.is_dir) continue;
            bool sub_is_dir = false;
            const std::wstring candidate = file + kSep + sys::widen(e.name) + kSep + L"index.html";
            if (path_info(candidate, sub_is_dir) && !sub_is_dir) {
                if (!elsewhere.empty()) elsewhere += ", ";
                elsewhere += "<a href=\"" + url_encode_path(e.name) + "/\">" +
                             html_escape(e.name) + "/</a>";
            }
        }

        std::string body = "<h1>" + html_escape(target) + "</h1><div class=\"sub\">" +
                           std::to_string(entries.size()) + " entries · no index.html</div>";
        if (!elsewhere.empty())
            body += "<div class=\"sub\" style=\"margin-top:10px;color:#7c6214\">"
                    "there is an index.html in " + elsewhere + "</div>";
        body += "<hr><ul>";
        if (target != "/")
            body += "<li><a href=\"..\"><span class=\"k\">up</span><span>..</span></a></li>";
        for (const sys::DirEntry& e : entries) {
            const std::string href = url_encode_path(e.name) + (e.is_dir ? "/" : "");
            body += "<li><a href=\"" + href + "\"><span class=\"k\">" +
                    (e.is_dir ? "dir" : extension_of(e.name)) + "</span><span>" +
                    html_escape(e.name) + "</span></a></li>";
        }
        body += "</ul>";
        send_simple(s, 200, "text/html; charset=utf-8", page(target, body), head_only);
        return;
    }

    const std::string path_utf8 = sys::narrow(file);

    // --- HTML: read it whole so the reload script can be woven in ---
    if (is_html(path_utf8)) {
        std::string body;
        if (!read_whole_file(file, body)) {
            send_simple(s, 403, "text/plain; charset=utf-8", "That file could not be read.",
                        head_only);
            return;
        }
        if (live_reload_.load()) {
            char script[900];
            std::snprintf(script, sizeof(script),
                          "\n<script>/* injected by AI IDE live server */\n"
                          "(function(){var seen=\"%lld\";function poll(){"
                          "fetch(\"/__livereload\",{cache:\"no-store\"})"
                          ".then(function(r){return r.text();})"
                          ".then(function(v){if(v!==seen){location.reload();return;}"
                          "setTimeout(poll,400);})"
                          ".catch(function(){setTimeout(poll,2000);});}"
                          "poll();})();</script>\n",
                          token_.load());

            // Before </body> when there is one, otherwise before </html>, and
            // at the end for a fragment that has neither.
            const std::string lower = to_lower(body);
            size_t at = lower.rfind("</body>");
            if (at == std::string::npos) at = lower.rfind("</html>");
            if (at == std::string::npos) body += script;
            else body.insert(at, script);
        }
        send_simple(s, 200, "text/html; charset=utf-8", body, head_only);
        return;
    }

    // --- everything else is streamed, so a large video costs no memory ---
    InFile in;
    if (!in.open(file)) {
        send_simple(s, 403, "text/plain; charset=utf-8", "That file could not be read.", head_only);
        return;
    }
    const long long file_size = in.size();

    long long first = 0, last = file_size - 1;
    bool partial = false;

    // Range requests are what let a <video> seek instead of restarting.
    const std::string lower_head = to_lower(head.substr(0, head_end));
    if (const size_t r = lower_head.find("\r\nrange:"); r != std::string::npos && file_size > 0) {
        const size_t value = lower_head.find("bytes=", r);
        const size_t eol = lower_head.find("\r\n", r + 2);
        if (value != std::string::npos && value < eol) {
            const std::string spec = lower_head.substr(value + 6, eol - value - 6);
            const size_t dash = spec.find('-');
            if (dash != std::string::npos && spec.find(',') == std::string::npos) {
                const std::string a = spec.substr(0, dash);
                const std::string b = spec.substr(dash + 1);
                bool ok = true;
                if (a.empty() && !b.empty()) { // the last N bytes
                    const long long len = atoll(b.c_str());
                    if (len <= 0) ok = false;
                    else first = std::max<long long>(0, file_size - len);
                } else if (!a.empty()) {
                    first = atoll(a.c_str());
                    if (!b.empty()) last = atoll(b.c_str());
                } else {
                    ok = false;
                }
                if (ok && first <= last && first < file_size) {
                    last = std::min<long long>(last, file_size - 1);
                    partial = true;
                } else {
                    char extra[96];
                    std::snprintf(extra, sizeof(extra), "Content-Range: bytes */%lld\r\n", file_size);
                    send_simple(s, 416, "text/plain; charset=utf-8", "", head_only, extra);
                    return;
                }
            }
        }
    }

    const long long length = last - first + 1;
    std::string extra;
    if (partial) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Content-Range: bytes %lld-%lld/%lld\r\n", first, last,
                      file_size);
        extra = buf;
    }
    if (!send_all(s, header_block(partial ? 206 : 200, mime_for(path_utf8), length, extra)) ||
        head_only)
        return;
    if (!in.seek(first)) return;

    std::vector<char> chunk_buf(kStreamChunk);
    long long remaining = length;
    while (remaining > 0) {
        const size_t want = (size_t)std::min<long long>(remaining, (long long)chunk_buf.size());
        const size_t got = in.read(chunk_buf.data(), want);
        if (got == 0) break;
        if (!send_all(s, chunk_buf.data(), got)) break;
        remaining -= (long long)got;
    }
}

} // namespace web
