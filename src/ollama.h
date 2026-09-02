// Ollama API client. Requests run on background threads; the UI polls Stream.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ollama {

struct Model {
    std::string name;
    std::string family;
    std::string parameter_size;
    std::string quantization;
    long long size_bytes = 0;
};

// A function call requested by the model.
struct ToolCall {
    std::string name;
    std::string arguments; // JSON object, serialized
    std::string id;
};

struct Message {
    std::string role;    // "system" | "user" | "assistant" | "tool"
    std::string content;
    std::string tool_name;             // set when role == "tool"
    std::vector<ToolCall> tool_calls;  // set when the assistant requested calls
};

// Declaration of a function the model may call. Only string parameters are
// needed here, which keeps the schema simple and portable across models.
struct ToolParam {
    std::string name;
    std::string description;
    bool required = true;
};

struct ToolDef {
    std::string name;
    std::string description;
    std::vector<ToolParam> params;
};

struct Options {
    float temperature = 0.7f;
    float top_p = 0.9f;
    int num_predict = -1;    // -1 = model default
    int num_ctx = 0;         // 0 = model default
};

// Shared state for one in-flight request. Every accessor takes the lock,
// so the UI thread can poll freely while the worker appends tokens.
class Stream {
public:
    // Snapshot of everything the UI needs for one frame.
    struct Snapshot {
        std::string text;
        std::string thinking;
        std::string error;
        bool done = false;
        bool tools_unsupported = false; // model rejected the tools parameter
        int eval_count = 0;
        double tokens_per_second = 0.0;
        std::vector<ToolCall> tool_calls;
    };

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot s;
        s.text = text_;
        s.thinking = thinking_;
        s.error = error_;
        s.done = done_;
        s.tools_unsupported = tools_unsupported_;
        s.eval_count = eval_count_;
        s.tokens_per_second = tokens_per_second_;
        s.tool_calls = tool_calls_;
        return s;
    }

    bool done() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return done_;
    }

    // Text accumulated so far, without the other snapshot fields.
    std::string text() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }

    void cancel() { cancel_.store(true); }
    bool cancelled() const { return cancel_.load(); }

    // --- worker side ---
    void append(const std::string& chunk) {
        std::lock_guard<std::mutex> lock(mutex_);
        text_ += chunk;
    }
    void append_thinking(const std::string& chunk) {
        std::lock_guard<std::mutex> lock(mutex_);
        thinking_ += chunk;
    }
    void set_stats(int eval_count, double tps) {
        std::lock_guard<std::mutex> lock(mutex_);
        eval_count_ = eval_count;
        tokens_per_second_ = tps;
    }
    void add_tool_call(ToolCall call) {
        std::lock_guard<std::mutex> lock(mutex_);
        tool_calls_.push_back(std::move(call));
    }
    void set_tools_unsupported() {
        std::lock_guard<std::mutex> lock(mutex_);
        tools_unsupported_ = true;
    }
    void finish(const std::string& error = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error.empty() && error_.empty()) error_ = error;
        done_ = true;
    }

private:
    mutable std::mutex mutex_;
    std::string text_;
    std::string thinking_;
    std::string error_;
    bool done_ = false;
    bool tools_unsupported_ = false;
    int eval_count_ = 0;
    double tokens_per_second_ = 0.0;
    std::vector<ToolCall> tool_calls_;
    std::atomic<bool> cancel_{false};
};

using StreamPtr = std::shared_ptr<Stream>;

class Client {
public:
    void set_host(const std::string& host) { host_ = normalize(host); }
    const std::string& host() const { return host_; }

    // Blocking; typically called from a worker thread or at startup.
    bool list_models(std::vector<Model>& out, std::string& err) const;
    bool ping(std::string& version, std::string& err) const;

    // Non-blocking: spawns a detached worker and returns immediately.
    // When tools is non-empty the model may answer with tool calls instead of
    // text; models that do not support tools report it via the snapshot.
    StreamPtr chat(const std::string& model, const std::vector<Message>& messages,
                   const Options& opts, const std::vector<ToolDef>& tools = {}) const;
    StreamPtr complete(const std::string& model, const std::string& prompt,
                       const std::string& suffix, const Options& opts) const;

private:
    static std::string normalize(const std::string& host);
    std::string host_ = "http://127.0.0.1:11434";
};

} // namespace ollama
