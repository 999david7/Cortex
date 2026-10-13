// Tiny HTTP client with streaming support: WinHTTP on Windows (http.cpp),
// libcurl on macOS and Linux (http_curl.cpp).
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace http {

struct Response {
    bool ok = false;       // transport succeeded and status is 2xx
    int status = 0;
    std::string body;
    std::string error;
};

// Called for every chunk of the response body as it arrives.
// Return false to abort the transfer.
using ChunkFn = std::function<bool(const char* data, size_t len)>;

// Blocking request. If on_chunk is null the body is accumulated into the response.
// cancel (optional) is polled while streaming so long responses can be stopped.
Response request(const std::string& method,
                 const std::string& url,
                 const std::string& body,
                 const ChunkFn& on_chunk = nullptr,
                 std::atomic<bool>* cancel = nullptr,
                 int connect_timeout_ms = 4000,
                 int read_timeout_ms = 300000);

inline Response get(const std::string& url, int timeout_ms = 4000) {
    return request("GET", url, "", nullptr, nullptr, timeout_ms, timeout_ms);
}

} // namespace http
