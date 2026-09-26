// libcurl client for macOS and Linux. http.cpp is the WinHTTP one for Windows.
// libcurl ships with macOS (headers in the SDK), so this adds no install step.
#include "http.h"

#ifndef _WIN32

#include <curl/curl.h>

#include <cstdio>
#include <mutex>

namespace http {
namespace {

void global_init() {
    // curl_global_init is not thread-safe, and requests start on worker threads.
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Transfer {
    Response* resp = nullptr;
    const ChunkFn* on_chunk = nullptr;
    std::atomic<bool>* cancel = nullptr;
    CURL* curl = nullptr;
    bool status_known = false;
    bool stream = false;
    bool aborted = false;
};

size_t on_write(char* data, size_t size, size_t count, void* user) {
    Transfer* t = (Transfer*)user;
    const size_t len = size * count;
    if (!t->status_known) {
        long status = 0;
        curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
        t->resp->status = (int)status;
        // Error bodies are always buffered so the caller can report the reason.
        t->stream = t->on_chunk && *t->on_chunk && status >= 200 && status < 300;
        t->status_known = true;
    }
    if (t->cancel && t->cancel->load()) {
        t->aborted = true;
        return 0; // makes curl fail the transfer with CURLE_WRITE_ERROR
    }
    if (t->stream) {
        if (!(*t->on_chunk)(data, len)) {
            t->aborted = true;
            return 0;
        }
    } else {
        t->resp->body.append(data, len);
    }
    return len;
}

// Lets a cancel stop a request that is waiting for its first byte, which for
// a large model loading into memory can be a long time.
int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    Transfer* t = (Transfer*)user;
    if (t->cancel && t->cancel->load()) {
        t->aborted = true;
        return 1;
    }
    return 0;
}

std::string describe(CURLcode code) {
    char buf[256];
    switch (code) {
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            std::snprintf(buf, sizeof(buf), "cannot reach server (connect)");
            break;
        case CURLE_OPERATION_TIMEDOUT:
            std::snprintf(buf, sizeof(buf), "request timed out");
            break;
        case CURLE_URL_MALFORMAT:
        case CURLE_UNSUPPORTED_PROTOCOL:
            std::snprintf(buf, sizeof(buf), "invalid URL");
            break;
        default:
            std::snprintf(buf, sizeof(buf), "request failed (%s)", curl_easy_strerror(code));
            break;
    }
    return buf;
}

} // namespace

Response request(const std::string& method,
                 const std::string& url,
                 const std::string& body,
                 const ChunkFn& on_chunk,
                 std::atomic<bool>* cancel,
                 int connect_timeout_ms,
                 int read_timeout_ms) {
    Response resp;
    global_init();

    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "could not start a request";
        return resp;
    }

    Transfer t;
    t.resp = &resp;
    t.on_chunk = &on_chunk;
    t.cancel = cancel;
    t.curl = curl;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AI-IDE/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // required on worker threads
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)connect_timeout_ms);
    // WinHTTP's read timeout is per receive, not for the whole transfer: a
    // streamed reply may take minutes as long as tokens keep coming. The
    // closest curl has is "abort if under 1 byte/s for that long".
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)((read_timeout_ms + 999) / 1000));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    // curl honours http_proxy from the environment; a local Ollama must never
    // be routed through it.
    if (url.find("://localhost") != std::string::npos || url.find("://127.") != std::string::npos ||
        url.find("://[::1]") != std::string::npos)
        curl_easy_setopt(curl, CURLOPT_PROXY, "");

    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else {
        if (method != "POST") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
    }

    const CURLcode code = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp.status = (int)status;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (t.aborted) {
        resp.error = "cancelled";
        return resp;
    }
    if (code != CURLE_OK) {
        resp.error = describe(code);
        return resp;
    }

    resp.ok = status >= 200 && status < 300;
    if (!resp.ok && resp.error.empty())
        resp.error = "HTTP " + std::to_string(resp.status);
    return resp;
}

} // namespace http

#endif // !_WIN32
