#include "ollama.h"

#include <cctype>
#include <thread>

#include "http.h"
#include "json.h"

namespace ollama {
namespace {

// Feeds newline-delimited JSON to a callback, buffering partial lines.
class NdjsonSplitter {
public:
    template <typename Fn>
    void feed(const char* data, size_t len, const Fn& on_line) {
        buffer_.append(data, len);
        size_t start = 0;
        while (true) {
            size_t nl = buffer_.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = buffer_.substr(start, nl - start);
            start = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) on_line(line);
        }
        if (start > 0) buffer_.erase(0, start);
    }

    template <typename Fn>
    void flush(const Fn& on_line) {
        if (!buffer_.empty()) {
            on_line(buffer_);
            buffer_.clear();
        }
    }

private:
    std::string buffer_;
};

js::Value make_options(const Options& o) {
    js::Value v = js::Value::object();
    v.set("temperature", js::Value((double)o.temperature));
    v.set("top_p", js::Value((double)o.top_p));
    if (o.num_predict > 0) v.set("num_predict", js::Value(o.num_predict));
    if (o.num_ctx > 0) v.set("num_ctx", js::Value(o.num_ctx));
    return v;
}

js::Value make_tools(const std::vector<ToolDef>& tools) {
    js::Value arr = js::Value::array();
    for (const ToolDef& t : tools) {
        js::Value props = js::Value::object();
        js::Value required = js::Value::array();
        for (const ToolParam& p : t.params) {
            js::Value schema = js::Value::object();
            schema.set("type", js::Value("string"));
            schema.set("description", js::Value(p.description));
            props.set(p.name, std::move(schema));
            if (p.required) required.push(js::Value(p.name));
        }
        js::Value params = js::Value::object();
        params.set("type", js::Value("object"));
        params.set("properties", std::move(props));
        params.set("required", std::move(required));

        js::Value fn = js::Value::object();
        fn.set("name", js::Value(t.name));
        fn.set("description", js::Value(t.description));
        fn.set("parameters", std::move(params));

        js::Value entry = js::Value::object();
        entry.set("type", js::Value("function"));
        entry.set("function", std::move(fn));
        arr.push(std::move(entry));
    }
    return arr;
}

js::Value make_messages(const std::vector<Message>& messages) {
    js::Value arr = js::Value::array();
    for (const Message& m : messages) {
        js::Value jm = js::Value::object();
        jm.set("role", js::Value(m.role));
        jm.set("content", js::Value(m.content));
        if (!m.tool_name.empty()) jm.set("tool_name", js::Value(m.tool_name));
        if (!m.tool_calls.empty()) {
            js::Value calls = js::Value::array();
            for (const ToolCall& c : m.tool_calls) {
                js::Value fn = js::Value::object();
                fn.set("name", js::Value(c.name));
                // Arguments travel as a JSON object, not as a string.
                js::Value args = js::parse(c.arguments);
                fn.set("arguments", args.is_object() ? args : js::Value::object());
                js::Value call = js::Value::object();
                if (!c.id.empty()) call.set("id", js::Value(c.id));
                call.set("type", js::Value("function"));
                call.set("function", std::move(fn));
                calls.push(std::move(call));
            }
            jm.set("tool_calls", std::move(calls));
        }
        arr.push(std::move(jm));
    }
    return arr;
}

// True when the server rejected the request because the model has no tool
// support, so the caller can retry without tools.
bool is_tools_unsupported(const std::string& message) {
    std::string lower;
    for (char c : message) lower += (char)std::tolower((unsigned char)c);
    return lower.find("does not support tools") != std::string::npos ||
           lower.find("doesn't support tools") != std::string::npos ||
           (lower.find("tools") != std::string::npos &&
            lower.find("not support") != std::string::npos);
}

// Shared worker body for /api/chat and /api/generate: both stream NDJSON and
// differ only in which field carries the token text.
void run_stream(const std::string& url, const std::string& payload,
                bool chat_shape, StreamPtr stream) {
    NdjsonSplitter splitter;
    std::string protocol_error;

    auto handle_line = [&](const std::string& line) {
        std::string parse_err;
        js::Value v = js::parse(line, &parse_err);
        if (!parse_err.empty()) return; // ignore anything that is not a JSON object
        if (v["error"].is_string()) {
            protocol_error = v["error"].as_string();
            return;
        }
        if (chat_shape) {
            const js::Value& msg = v["message"];
            if (msg["content"].is_string()) stream->append(msg["content"].as_string());
            if (msg["thinking"].is_string()) stream->append_thinking(msg["thinking"].as_string());
            const js::Value& calls = msg["tool_calls"];
            for (size_t i = 0; i < calls.size(); ++i) {
                const js::Value& fn = calls[i]["function"];
                if (!fn["name"].is_string()) continue;
                ToolCall call;
                call.name = fn["name"].as_string();
                call.id = calls[i]["id"].as_string();
                // Ollama sends an object; some models send a JSON string.
                const js::Value& args = fn["arguments"];
                call.arguments = args.is_string() ? args.as_string() : args.dump();
                stream->add_tool_call(std::move(call));
            }
        } else {
            if (v["response"].is_string()) stream->append(v["response"].as_string());
            if (v["thinking"].is_string()) stream->append_thinking(v["thinking"].as_string());
        }
        if (v["done"].as_bool(false)) {
            int eval_count = v["eval_count"].as_int(0);
            double eval_ns = v["eval_duration"].as_double(0.0);
            double tps = eval_ns > 0.0 ? (double)eval_count / (eval_ns / 1e9) : 0.0;
            stream->set_stats(eval_count, tps);
        }
    };

    std::atomic<bool> cancel{false};
    auto on_chunk = [&](const char* data, size_t len) -> bool {
        splitter.feed(data, len, handle_line);
        return !stream->cancelled();
    };

    http::Response resp = http::request("POST", url, payload, on_chunk, &cancel);
    splitter.flush(handle_line);

    if (!protocol_error.empty()) {
        if (is_tools_unsupported(protocol_error)) stream->set_tools_unsupported();
        stream->finish(protocol_error);
    } else if (stream->cancelled()) {
        stream->finish();
    } else if (!resp.ok) {
        std::string err = resp.error;
        // Ollama reports model errors in the body of a non-2xx response.
        if (!resp.body.empty()) {
            js::Value v = js::parse(resp.body);
            if (v["error"].is_string()) err = v["error"].as_string();
        }
        if (err.empty()) err = "request failed";
        if (is_tools_unsupported(err)) stream->set_tools_unsupported();
        stream->finish(err);
    } else {
        stream->finish();
    }
}

} // namespace

std::string Client::normalize(const std::string& host) {
    std::string h = host;
    while (!h.empty() && (h.back() == '/' || h.back() == ' ')) h.pop_back();
    if (h.empty()) return "http://127.0.0.1:11434";
    if (h.find("://") == std::string::npos) h = "http://" + h;
    return h;
}

bool Client::ping(std::string& version, std::string& err) const {
    http::Response r = http::get(host_ + "/api/version", 2500);
    if (!r.ok) {
        err = r.error.empty() ? "no response" : r.error;
        return false;
    }
    version = js::parse(r.body)["version"].as_string();
    err.clear();
    return true;
}

bool Client::list_models(std::vector<Model>& out, std::string& err) const {
    http::Response r = http::get(host_ + "/api/tags", 5000);
    if (!r.ok) {
        err = r.error.empty() ? "could not reach Ollama" : r.error;
        return false;
    }

    std::string parse_err;
    js::Value root = js::parse(r.body, &parse_err);
    if (!parse_err.empty()) {
        err = "bad response from Ollama: " + parse_err;
        return false;
    }

    out.clear();
    const js::Value& models = root["models"];
    for (size_t i = 0; i < models.size(); ++i) {
        const js::Value& m = models[i];
        Model model;
        model.name = m["name"].as_string();
        if (model.name.empty()) continue;
        model.size_bytes = (long long)m["size"].as_double(0.0);
        const js::Value& details = m["details"];
        model.family = details["family"].as_string();
        model.parameter_size = details["parameter_size"].as_string();
        model.quantization = details["quantization_level"].as_string();
        out.push_back(std::move(model));
    }
    err.clear();
    return true;
}

StreamPtr Client::chat(const std::string& model, const std::vector<Message>& messages,
                       const Options& opts, const std::vector<ToolDef>& tools) const {
    auto stream = std::make_shared<Stream>();

    js::Value body = js::Value::object();
    body.set("model", js::Value(model));
    body.set("stream", js::Value(true));
    body.set("messages", make_messages(messages));
    if (!tools.empty()) body.set("tools", make_tools(tools));
    body.set("options", make_options(opts));

    std::string url = host_ + "/api/chat";
    std::string payload = body.dump();
    std::thread([url, payload, stream] { run_stream(url, payload, true, stream); }).detach();
    return stream;
}

StreamPtr Client::complete(const std::string& model, const std::string& prompt,
                           const std::string& suffix, const Options& opts) const {
    auto stream = std::make_shared<Stream>();

    js::Value body = js::Value::object();
    body.set("model", js::Value(model));
    body.set("prompt", js::Value(prompt));
    if (!suffix.empty()) body.set("suffix", js::Value(suffix));
    body.set("stream", js::Value(true));
    body.set("options", make_options(opts));

    std::string url = host_ + "/api/generate";
    std::string payload = body.dump();
    std::thread([url, payload, stream] { run_stream(url, payload, false, stream); }).detach();
    return stream;
}

} // namespace ollama
