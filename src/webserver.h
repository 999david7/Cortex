// A local static-file server with live reload: the IDE's own Live Server.
//
// It serves the open folder over 127.0.0.1, injects a small polling script into
// every HTML page it hands out, and bumps a token whenever anything under the
// root changes on disk — which is what makes the browser tab reload by itself.
// Everything runs on its own threads; nothing here blocks the UI.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace web {

// Paths stay in the platform's own form inside the server: UTF-16 on Windows,
// so no file name is lost in a round trip, and plain UTF-8 everywhere else.
#ifdef _WIN32
using NativePath = std::wstring;
#else
using NativePath = std::string;
#endif

class LiveServer {
public:
    LiveServer() = default;
    ~LiveServer();

    LiveServer(const LiveServer&) = delete;
    LiveServer& operator=(const LiveServer&) = delete;

    // Binds 127.0.0.1:port. When that port is taken it walks up to port + 20 and
    // reports the one it settled on through port(). Returns false and fills err
    // if the folder does not exist or nothing could be bound.
    bool start(const std::string& root, int port, std::string& err);
    void stop();

    bool running() const { return running_.load(); }
    int port() const { return port_.load(); }
    std::string root() const;
    std::string origin() const; // "http://127.0.0.1:5500", empty when stopped

    // Address of a file inside the root. A path outside it — or an empty one —
    // yields the site root.
    std::string url_for(const std::string& absolute_path) const;

    void set_live_reload(bool on) { live_reload_.store(on); }
    bool live_reload() const { return live_reload_.load(); }

    // Reloads every page that is open. The watcher does this on its own; the
    // editor also calls it on save so the refresh happens immediately.
    void notify_changed() { token_.fetch_add(1); }

    long long served() const { return served_.load(); }

    // Hands over the lines logged since the last call (starts, stops, misses).
    std::vector<std::string> take_log();

private:
    void accept_loop();
    void worker_loop();
    void watch_loop();
    void serve(uintptr_t sock);
    void log(const std::string& line);

    // Maps a decoded URL path onto a real file, refusing anything that resolves
    // outside the root. Returns false when the path escapes or does not exist.
    bool resolve(const std::string& url_path, NativePath& out_file, bool& is_dir) const;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<int> port_{0};
    std::atomic<long long> token_{1};
    std::atomic<long long> served_{0};
    std::atomic<bool> live_reload_{true};

    std::string root_;         // as opened, for display
    NativePath root_native_;   // canonical, with a trailing separator

    // A SOCKET (or a file descriptor), kept as an integer so this header stays
    // free of <winsock2.h>.
    std::atomic<uintptr_t> listen_{(uintptr_t)-1};
#ifdef _WIN32
    void* stop_event_ = nullptr; // wakes the directory watcher
    bool wsa_started_ = false;
#endif
    std::mutex watch_mu_;        // wakes the polling watcher (macOS, Linux)
    std::condition_variable watch_cv_;

    std::thread acceptor_;
    std::thread watcher_;
    std::vector<std::thread> workers_;

    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<uintptr_t> queue_;

    mutable std::mutex log_mu_;
    std::vector<std::string> log_;
};

} // namespace web
