// The HTTP client for macOS and Linux, on libcurl (which both systems ship).
// http.cpp is the WinHTTP version of the same interface.
#ifndef _WIN32

#include "http.h"

#include <curl/curl.h>

#include <cstdio>
#include <mutex>

namespace http {
namespace {

struct Transfer {
    CURL* curl = nullptr;
    const ChunkFn* on_chunk = nullptr;
    std::atomic<bool>* cancel = nullptr;
    Response* resp = nullptr;
    bool cancelled = false;
};

size_t on_write(char* data, size_t size, size_t count, void* user) {
    Transfer* t = (Transfer*)user;
    const size_t len = size * count;
    if (t->cancel && t->cancel->load()) {
        t->cancelled = true;
        return 0; // a short write aborts the transfer
    }
    long status = 0;
    curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
    // Error bodies are always buffered so the caller can report the reason.
    const bool stream = t->on_chunk && *t->on_chunk && status >= 200 && status < 300;
    if (stream) {
        if (!(*t->on_chunk)(data, len)) {
            t->cancelled = true;
            return 0;
        }
    } else {
        t->resp->body.append(data, len);
    }
    return len;
}

// Polled by curl even while no data arrives, so Stop works on a silent stream.
int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    Transfer* t = (Transfer*)user;
    if (t->cancel && t->cancel->load()) {
        t->cancelled = true;
        return 1;
    }
    return 0;
}

} // namespace

Response request(const std::string& method,
                 const std::string& url,
                 const std::string& body,
                 const ChunkFn& on_chunk,
                 std::atomic<bool>* cancel,
                 int connect_timeout_ms,
                 int read_timeout_ms) {
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    Response resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "could not start a request";
        return resp;
    }

    Transfer t;
    t.curl = curl;
    t.on_chunk = &on_chunk;
    t.cancel = cancel;
    t.resp = &resp;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AI-IDE/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // requests run on worker threads
    // A local model server is never reached through a proxy, whatever the
    // environment says.
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "127.0.0.1,localhost,::1");
    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
        if (method != "POST") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)connect_timeout_ms);
    if (read_timeout_ms <= 60000) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)read_timeout_ms);
    } else {
        // A streamed reply can run for minutes; what matters is that it keeps
        // moving, the same as the read timeout on the Windows side.
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)(read_timeout_ms / 1000));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp.status = (int)status;

    if (rc != CURLE_OK) {
        if (t.cancelled) {
            resp.error = "cancelled";
        } else if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST) {
            resp.error = "cannot reach server (connect)";
        } else if (rc == CURLE_OPERATION_TIMEDOUT) {
            resp.error = "request timed out (read)";
        } else {
            resp.error = curl_easy_strerror(rc);
        }
    } else {
        resp.ok = status >= 200 && status < 300;
        if (!resp.ok) resp.error = "HTTP " + std::to_string(resp.status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

} // namespace http

#endif // !_WIN32
