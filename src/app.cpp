#include "app.h"

#include "agenttext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "imgui.h"
#include "imgui_internal.h"
#include "json.h"
#include "ui.h"

namespace app {
namespace {

// What the system file browser is called here.
#ifdef __APPLE__
const char* const kRevealLabel = "Reveal in Finder";
#elif defined(_WIN32)
const char* const kRevealLabel = "Reveal in Explorer";
#else
const char* const kRevealLabel = "Show in File Manager";
#endif

// How many tool calls one chat turn may chain together before the IDE stops
// and hands control back to the user.
constexpr int kMaxAgentSteps = 16;

// ------------------------------------------------------------------ helpers

struct InputTextUserData {
    std::string* str;
};

int input_text_resize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = ((InputTextUserData*)data->UserData)->str;
        str->resize((size_t)data->BufTextLen);
        data->Buf = (char*)str->c_str();
    }
    return 0;
}

bool input_text_multiline(const char* label, std::string* str, const ImVec2& size,
                          ImGuiInputTextFlags flags) {
    InputTextUserData ud{str};
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextMultiline(label, (char*)str->c_str(), str->capacity() + 1, size, flags,
                                     input_text_resize, &ud);
}

// A cell of a segmented switch in the house style: tracked capitals, filled
// with ink when it is the active one.
bool segment_toggle(const char* id, const char* label, bool active, float width) {
    ImFont* f = ui::F.ui_micro ? ui::F.ui_micro : ui::F.ui_small;
    const float fs = f->FontSize;
    const float h = std::floor(fs + ui::px(12));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), ui::C.ink);
    else if (hovered) dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), ui::C.bg_surface_hi);
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), active ? ui::C.ink : ui::C.border);
    const float tw = ui::caps_width(f, fs, label);
    ui::draw_caps(dl, f, fs, ImVec2(p.x + (width - tw) * 0.5f, p.y + (h - fs) * 0.5f),
                  active ? ui::C.on_ink : (hovered ? ui::C.text : ui::C.text_dim), label);
    return clicked;
}

// Two paths naming the same folder, allowing for case and a trailing separator.
bool same_path(std::string a, std::string b) {
    auto tidy = [](std::string& p) {
        for (char& c : p) {
            if (c == '/') c = '\\';
            c = (char)std::tolower((unsigned char)c);
        }
        while (p.size() > 1 && p.back() == '\\') p.pop_back();
    };
    tidy(a);
    tidy(b);
    return a == b;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string human_size(long long bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%lld B", bytes);
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    else std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

// Removes ANSI escape sequences from process output.
std::string strip_ansi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b') {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') {
                ++j;
                while (j < s.size() && !((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z')))
                    ++j;
                i = j;
                continue;
            }
        }
        if (s[i] == '\r') continue;
        out += s[i];
    }
    return out;
}

// Strips a leading/trailing markdown fence from a model reply.
std::string strip_code_fence(const std::string& s) {
    std::string t = trim(s);
    if (t.rfind("```", 0) != 0) return s;
    size_t first_nl = t.find('\n');
    if (first_nl == std::string::npos) return std::string();
    std::string body = t.substr(first_nl + 1);
    size_t close = body.rfind("```");
    if (close != std::string::npos) body = body.substr(0, close);
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
    return body;
}

// Ollama's failures are mostly operational, and the raw text rarely tells a
// user what to do about it. Map the common ones onto a concrete next step.
std::string error_hint(const std::string& error) {
    std::string lower;
    for (char c : error) lower += (char)std::tolower((unsigned char)c);

    if (lower.find("out of memory") != std::string::npos ||
        lower.find("out-of-memory") != std::string::npos ||
        lower.find("failed to allocate") != std::string::npos ||
        lower.find("kv cache") != std::string::npos) {
        return "The model did not fit in memory, so it never started. Pick a smaller "
               "model, or lower the context window in Settings.";
    }
    if (lower.find("no such model") != std::string::npos ||
        lower.find("not found, try pulling") != std::string::npos ||
        lower.find("file does not exist") != std::string::npos) {
        return "That model is not installed. Run  ollama pull <model>  and press "
               "Refresh in the model picker.";
    }
    if (lower.find("cannot reach") != std::string::npos ||
        lower.find("no response") != std::string::npos ||
        lower.find("connection") != std::string::npos) {
        return "Ollama is not answering on " + std::string("the configured host") +
               ". Start it, or check the host in Settings.";
    }
    if (lower.find("does not support tools") != std::string::npos ||
        lower.find("doesn't support tools") != std::string::npos) {
        return "This model cannot call tools, so it cannot edit files directly. "
               "The IDE will fall back to code blocks with a Create file button.";
    }
    if (lower.find("context") != std::string::npos &&
        lower.find("exceed") != std::string::npos) {
        return "The conversation outgrew the context window. Start a new chat, or "
               "raise the context size in Settings.";
    }
    return std::string();
}

// A path the way a person refers to it: relative to the open workspace when it
// is inside one, otherwise trimmed from the left so the filename — the part that
// identifies it — always survives. Bars and breadcrumbs have a fixed width, and
// an absolute Windows path will happily eat all of it.
// Extensions whose files are not text. A model that "creates" one through a
// text tool produces a file that looks right in the tree and is broken
// everywhere else - a 15-byte JPEG is the single most common way a generated
// site ends up full of broken images.
const char* binary_asset_kind(const std::string& path) {
    struct Row { const char* ext; const char* kind; };
    static const Row kRows[] = {
        {".jpg", "image"},  {".jpeg", "image"}, {".png", "image"},  {".gif", "image"},
        {".webp", "image"}, {".avif", "image"}, {".bmp", "image"},  {".ico", "icon"},
        {".mp3", "audio"},  {".wav", "audio"},  {".ogg", "audio"},  {".flac", "audio"},
        {".mp4", "video"},  {".webm", "video"}, {".mov", "video"},  {".avi", "video"},
        {".woff", "font"},  {".woff2", "font"}, {".ttf", "font"},   {".otf", "font"},
        {".eot", "font"},   {".pdf", "PDF"},    {".zip", "archive"},{".exe", "binary"},
        {".dll", "binary"}, {".wasm", "binary"},
    };
    std::string lower;
    for (char c : path) lower += (char)std::tolower((unsigned char)c);
    for (const Row& r : kRows) {
        const size_t n = std::strlen(r.ext);
        if (lower.size() > n && lower.compare(lower.size() - n, n, r.ext) == 0) return r.kind;
    }
    return nullptr;
}

// "//" is a comment in JavaScript and in C, and in CSS it is a syntax error
// that makes the browser throw away the rule behind it. Models write it out of
// habit, and the page silently loses its first block of styling.
bool css_has_slash_comment(const std::string& css) {
    size_t line_start = 0;
    while (line_start < css.size()) {
        size_t i = line_start;
        while (i < css.size() && (css[i] == ' ' || css[i] == '\t')) ++i;
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '/') return true;
        const size_t nl = css.find('\n', line_start);
        if (nl == std::string::npos) break;
        line_start = nl + 1;
    }
    return false;
}

// Script content a browser cannot execute on its own. Both signals are
// unambiguous: JSX is not JavaScript, and a bare module specifier has nothing
// to resolve against without a bundler or an import map.
bool script_needs_a_build_step(const std::string& js, std::string& why) {
    // import X from 'name' where name is not a path and not a URL
    size_t i = 0;
    while ((i = js.find("from", i)) != std::string::npos) {
        size_t q = i + 4;
        while (q < js.size() && (js[q] == ' ' || js[q] == '\t')) ++q;
        if (q < js.size() && (js[q] == '\'' || js[q] == '"')) {
            const char quote = js[q];
            const size_t end = js.find(quote, q + 1);
            if (end != std::string::npos) {
                const std::string spec = js.substr(q + 1, end - q - 1);
                const bool relative = !spec.empty() && (spec[0] == '.' || spec[0] == '/');
                const bool url = spec.find("://") != std::string::npos;
                if (!spec.empty() && !relative && !url) {
                    why = "it imports the module \"" + spec +
                          "\", which the browser has nothing to resolve against";
                    return true;
                }
            }
        }
        i += 4;
    }

    // JSX: a tag whose name starts with a capital letter, which HTML has no
    // such thing as and JavaScript cannot parse.
    for (size_t k = 0; k + 2 < js.size(); ++k) {
        if (js[k] != '<') continue;
        const char c = js[k + 1];
        if (c < 'A' || c > 'Z') continue;
        size_t e = k + 2;
        while (e < js.size() && (isalnum((unsigned char)js[e]) || js[e] == '.')) ++e;
        if (e < js.size() && (js[e] == ' ' || js[e] == '/' || js[e] == '>')) {
            why = "it contains JSX (" + js.substr(k, e - k) + " …), which is not JavaScript";
            return true;
        }
    }
    return false;
}

// The <script> tags of a page: what they load and whether they are modules.
struct ScriptRef {
    std::string src;
    bool is_module = false;
};

void collect_scripts(const std::string& html, std::vector<ScriptRef>& out) {
    size_t i = 0;
    while ((i = html.find("<script", i)) != std::string::npos) {
        const size_t end = html.find('>', i);
        if (end == std::string::npos) break;
        const std::string tag = html.substr(i, end - i);
        std::string lower;
        for (char c : tag) lower += (char)std::tolower((unsigned char)c);

        ScriptRef ref;
        ref.is_module = lower.find("type=\"module\"") != std::string::npos ||
                        lower.find("type=\'module\'") != std::string::npos;
        const size_t at = lower.find("src=");
        if (at != std::string::npos && at + 4 < tag.size()) {
            const char quote = tag[at + 4];
            if (quote == '"' || quote == '\'') {
                const size_t stop = tag.find(quote, at + 5);
                if (stop != std::string::npos) ref.src = tag.substr(at + 5, stop - at - 5);
            }
        }
        if (!ref.src.empty()) out.push_back(ref);
        i = end + 1;
    }
}

// Pulls the local files a page points at out of its markup. External URLs, data
// URIs and in-page anchors are not files, so they are left alone.
void collect_local_refs(const std::string& html, std::vector<std::string>& out) {
    static const char* kAttrs[] = {"src=", "href=", "poster="};
    for (const char* attr : kAttrs) {
        const size_t attr_len = std::strlen(attr);
        size_t i = 0;
        while ((i = html.find(attr, i)) != std::string::npos) {
            i += attr_len;
            if (i >= html.size()) break;
            const char quote = html[i];
            if (quote != '"' && quote != '\'') continue;
            const size_t end = html.find(quote, i + 1);
            if (end == std::string::npos) break;
            std::string ref = html.substr(i + 1, end - i - 1);
            i = end + 1;

            if (const size_t cut = ref.find_first_of("?#"); cut != std::string::npos)
                ref.erase(cut);
            if (ref.empty()) continue;
            std::string lower;
            for (char c : ref) lower += (char)std::tolower((unsigned char)c);
            if (lower.find("://") != std::string::npos || ref.compare(0, 2, "//") == 0 ||
                lower.compare(0, 5, "data:") == 0 || lower.compare(0, 7, "mailto:") == 0 ||
                lower.compare(0, 4, "tel:") == 0 || lower.compare(0, 11, "javascript:") == 0)
                continue;
            if (std::find(out.begin(), out.end(), ref) == out.end()) out.push_back(ref);
        }
    }
}

std::string display_path(const std::string& path, const std::string& workspace,
                         size_t max_chars) {
    if (path.empty()) return path;

    std::string out = path;
    const bool inside = !workspace.empty() && out.size() > workspace.size() + 1 &&
                        _strnicmp(out.c_str(), workspace.c_str(), workspace.size()) == 0 &&
                        (out[workspace.size()] == '\\' || out[workspace.size()] == '/');
    if (inside) out = out.substr(workspace.size() + 1);

    for (char& c : out)
        if (c == '\\') c = '/';

    if (out.size() <= max_chars) return out;

    // Drop leading components until it fits, keeping at least the file name.
    size_t slash = out.find('/');
    while (slash != std::string::npos && out.size() - slash - 1 > max_chars - 1) {
        out = out.substr(slash + 1);
        slash = out.find('/');
    }
    if (out.size() > max_chars) out = out.substr(out.size() - max_chars);
    return "…/" + out;
}

ImU32 with_alpha(ImU32 color, int alpha) {
    return (color & 0x00FFFFFF) | ((ImU32)alpha << 24);
}


// Background fetch of the Ollama model list, so the UI never blocks on HTTP.
struct ModelFetch {
    std::mutex mutex;
    bool done = false;
    bool ok = false;
    bool online = false;
    std::vector<ollama::Model> models;
    std::string version;
    std::string error;
};

std::shared_ptr<ModelFetch> g_fetch;

} // namespace

// ------------------------------------------------------------------ settings

std::string Settings::path() {
    return sys::join_path(sys::app_data_dir(), "settings.json");
}

bool Settings::load() {
    std::string content, err;
    if (!sys::read_file(path(), content, err)) return false;
    js::Value v = js::parse(content);
    if (!v.is_object()) return false;

    ollama_host = v["ollama_host"].as_string(ollama_host);
    chat_model = v["chat_model"].as_string(chat_model);
    completion_model = v["completion_model"].as_string(completion_model);
    {
        // The first system prompt this IDE shipped with told the model to wrap
        // code in a fenced block — which is precisely how a reply ends up as
        // something to copy rather than as a file on disk. A settings file that
        // still carries it verbatim was never edited by the user, so it is
        // moved on to the current default; anything else is left alone.
        static const char kSupersededPrompt[] =
            "You are a focused coding assistant embedded in a local IDE. "
            "Answer concisely. When you produce code, wrap it in a fenced block "
            "tagged with the language.";
        const std::string saved = v["system_prompt"].as_string(system_prompt);
        if (saved != kSupersededPrompt) system_prompt = saved;
    }
    temperature = (float)v["temperature"].as_double(temperature);
    agent_temperature = (float)v["agent_temperature"].as_double(agent_temperature);
    top_p = (float)v["top_p"].as_double(top_p);
    num_ctx = v["num_ctx"].as_int(num_ctx);

    // Settings written before the context window got a real default carry
    // num_ctx = 0, meaning "whatever the model advertises". For a model that
    // claims a 256k window that is an allocation failure before the first
    // token, so those files are migrated once to the bounded default. A 0 the
    // user chooses afterwards is kept, because the version has moved on.
    const int file_version = v["settings_version"].as_int(1);
    if (file_version < 2 && num_ctx == 0) num_ctx = 8192;
    attach_open_file = v["attach_open_file"].as_bool(attach_open_file);
    attach_selection = v["attach_selection"].as_bool(attach_selection);
    max_context_bytes = v["max_context_bytes"].as_int(max_context_bytes);
    enable_file_tools = v["enable_file_tools"].as_bool(enable_file_tools);
    auto_approve_writes = v["auto_approve_writes"].as_bool(auto_approve_writes);
    tab_size = v["tab_size"].as_int(tab_size);
    insert_spaces = v["insert_spaces"].as_bool(insert_spaces);
    show_line_numbers = v["show_line_numbers"].as_bool(show_line_numbers);
    indent_guides = v["indent_guides"].as_bool(indent_guides);
    auto_close_brackets = v["auto_close_brackets"].as_bool(auto_close_brackets);
    highlight_current_line = v["highlight_current_line"].as_bool(highlight_current_line);
    cursor_style = ImClamp(v["cursor_style"].as_int(cursor_style), 0, (int)ed::CursorStyle::Count - 1);
    cursor_blinking = ImClamp(v["cursor_blinking"].as_int(cursor_blinking), 0, (int)ed::CursorBlink::Count - 1);
    cursor_width = ImClamp(v["cursor_width"].as_int(cursor_width), 1, 8);
    smooth_caret = v["smooth_caret"].as_bool(smooth_caret);
    overtype_cursor_style = ImClamp(v["overtype_cursor_style"].as_int(overtype_cursor_style), 0,
                                    (int)ed::CursorStyle::Count - 1);
    render_whitespace = ImClamp(v["render_whitespace"].as_int(render_whitespace), 0, (int)ed::Whitespace::Count - 1);
    minimap = v["minimap"].as_bool(minimap);
    bracket_pair_colors = v["bracket_pair_colors"].as_bool(bracket_pair_colors);
    match_brackets = v["match_brackets"].as_bool(match_brackets);
    highlight_occurrences = v["highlight_occurrences"].as_bool(highlight_occurrences);
    rulers = v["rulers"].as_string(rulers);
    editor_font_size = ImClamp((float)v["editor_font_size"].as_double(editor_font_size), 8.0f, 36.0f);
    live_port = v["live_port"].as_int(live_port);
    live_reload = v["live_reload"].as_bool(live_reload);
    ui_scale = (float)v["ui_scale"].as_double(ui_scale);
    last_folder = v["last_folder"].as_string(last_folder);
    run_command = v["run_command"].as_string(run_command);

    recent_folders.clear();
    const js::Value& recents = v["recent_folders"];
    for (size_t i = 0; i < recents.size(); ++i) {
        std::string p = recents[i].as_string();
        if (!p.empty()) recent_folders.push_back(p);
    }
    return true;
}

bool Settings::save() const {
    js::Value v = js::Value::object();
    v.set("ollama_host", js::Value(ollama_host));
    v.set("chat_model", js::Value(chat_model));
    v.set("completion_model", js::Value(completion_model));
    v.set("system_prompt", js::Value(system_prompt));
    v.set("temperature", js::Value((double)temperature));
    v.set("agent_temperature", js::Value((double)agent_temperature));
    v.set("top_p", js::Value((double)top_p));
    v.set("settings_version", js::Value(2));
    v.set("num_ctx", js::Value(num_ctx));
    v.set("attach_open_file", js::Value(attach_open_file));
    v.set("attach_selection", js::Value(attach_selection));
    v.set("max_context_bytes", js::Value(max_context_bytes));
    v.set("enable_file_tools", js::Value(enable_file_tools));
    v.set("auto_approve_writes", js::Value(auto_approve_writes));
    v.set("tab_size", js::Value(tab_size));
    v.set("insert_spaces", js::Value(insert_spaces));
    v.set("show_line_numbers", js::Value(show_line_numbers));
    v.set("indent_guides", js::Value(indent_guides));
    v.set("auto_close_brackets", js::Value(auto_close_brackets));
    v.set("highlight_current_line", js::Value(highlight_current_line));
    v.set("cursor_style", js::Value(cursor_style));
    v.set("cursor_blinking", js::Value(cursor_blinking));
    v.set("cursor_width", js::Value(cursor_width));
    v.set("smooth_caret", js::Value(smooth_caret));
    v.set("overtype_cursor_style", js::Value(overtype_cursor_style));
    v.set("render_whitespace", js::Value(render_whitespace));
    v.set("minimap", js::Value(minimap));
    v.set("bracket_pair_colors", js::Value(bracket_pair_colors));
    v.set("match_brackets", js::Value(match_brackets));
    v.set("highlight_occurrences", js::Value(highlight_occurrences));
    v.set("rulers", js::Value(rulers));
    v.set("editor_font_size", js::Value((double)editor_font_size));
    v.set("live_port", js::Value(live_port));
    v.set("live_reload", js::Value(live_reload));
    v.set("ui_scale", js::Value((double)ui_scale));
    v.set("last_folder", js::Value(last_folder));
    v.set("run_command", js::Value(run_command));

    js::Value recents = js::Value::array();
    for (size_t i = 0; i < recent_folders.size() && i < 8; ++i)
        recents.push(js::Value(recent_folders[i]));
    v.set("recent_folders", std::move(recents));

    std::string err;
    return sys::write_file(path(), v.dump(2), err);
}

std::vector<int> Settings::ruler_columns() const {
    std::vector<int> out;
    int value = 0;
    bool in_number = false;
    for (char c : rulers + ",") {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            in_number = true;
            if (value > 1000) value = 1000;
        } else if (in_number) {
            if (value > 0) out.push_back(value);
            value = 0;
            in_number = false;
        }
    }
    return out;
}

// ---------------------------------------------------------------- lifecycle

App::App() = default;
App::~App() = default;

void App::init(void* hwnd, WindowHooks hooks, float dpi_scale) {
    hwnd_ = hwnd;
    hooks_ = std::move(hooks);
    dpi_scale_ = dpi_scale;

    settings_.load();
    client_.set_host(settings_.ollama_host);
    refresh_models(false);

    // Ctrl+Tab belongs to the editor tabs, as in VS Code, not to ImGui's own
    // window switcher.
    if (ImGuiContext* ctx = ImGui::GetCurrentContext()) {
        ctx->ConfigNavWindowingKeyNext = 0;
        ctx->ConfigNavWindowingKeyPrev = 0;
    }
    if (settings_.editor_font_size != ui::code_font_size()) {
        ui::set_code_font_size(settings_.editor_font_size);
        ui::request_font_rebuild();
    }

    if (!settings_.last_folder.empty() && sys::is_directory(settings_.last_folder))
        set_workspace(settings_.last_folder);

    terminal_command_ = settings_.run_command;
    terminal_output_ =
        "Ready. Commands run in the workspace folder.\n";
}

void App::shutdown() {
    stop_chat();
    process_.terminate();
    live_.stop();
    if (!workspace_.empty()) settings_.last_folder = workspace_;
    settings_.run_command = terminal_command_;
    settings_.save();
}

void App::toast(const std::string& text, bool error) {
    toasts_.push_back({text, error, ImGui::GetTime()});
    if (toasts_.size() > 5) toasts_.erase(toasts_.begin());
}

// ---------------------------------------------------------------- documents

Document* App::active_document() {
    if (active_doc_ < 0 || active_doc_ >= (int)documents_.size()) return nullptr;
    return documents_[active_doc_].get();
}

void App::apply_editor_settings(Document* doc) {
    if (!doc) return;
    doc->editor.tab_size = settings_.tab_size;
    doc->editor.insert_spaces = settings_.insert_spaces;
    doc->editor.show_line_numbers = settings_.show_line_numbers;
    doc->editor.show_indent_guides = settings_.indent_guides;
    doc->editor.auto_close_brackets = settings_.auto_close_brackets;
    doc->editor.highlight_current_line = settings_.highlight_current_line;
    doc->editor.cursor_style = (ed::CursorStyle)settings_.cursor_style;
    doc->editor.cursor_blink = (ed::CursorBlink)settings_.cursor_blinking;
    doc->editor.cursor_width = settings_.cursor_width;
    doc->editor.smooth_caret = settings_.smooth_caret;
    doc->editor.overtype_cursor_style = (ed::CursorStyle)settings_.overtype_cursor_style;
    doc->editor.render_whitespace = (ed::Whitespace)settings_.render_whitespace;
    doc->editor.show_minimap = settings_.minimap;
    doc->editor.bracket_pair_colors = settings_.bracket_pair_colors;
    doc->editor.match_brackets = settings_.match_brackets;
    doc->editor.highlight_occurrences = settings_.highlight_occurrences;
    doc->editor.rulers = settings_.ruler_columns();
}

void App::editor_command(const std::function<void(ed::TextEditor&)>& fn) {
    Document* doc = active_document();
    if (!doc) return;
    fn(doc->editor);
    doc->editor.request_focus();
}

int App::find_document(const std::string& path) const {
    for (size_t i = 0; i < documents_.size(); ++i)
        if (!documents_[i]->path.empty() &&
            _stricmp(documents_[i]->path.c_str(), path.c_str()) == 0)
            return (int)i;
    return -1;
}

void App::new_document() {
    auto doc = std::make_unique<Document>();
    doc->display_name = "untitled-" + std::to_string(++untitled_counter_);
    doc->editor.set_text("");
    doc->editor.set_language(ed::Language::PlainText);
    doc->scroll_to_tab = true;
    apply_editor_settings(doc.get());
    doc->editor.request_focus();
    documents_.push_back(std::move(doc));
    active_doc_ = (int)documents_.size() - 1;
}

void App::open_file(const std::string& path) {
    int existing = find_document(path);
    if (existing >= 0) {
        active_doc_ = existing;
        documents_[existing]->scroll_to_tab = true;
        return;
    }

    std::string content, err;
    if (!sys::read_file(path, content, err)) {
        toast("Could not open " + sys::file_name(path) + ": " + err, true);
        return;
    }

    auto doc = std::make_unique<Document>();
    doc->path = sys::normalize(path);
    doc->display_name = sys::file_name(path);
    doc->editor.set_text(content);
    doc->editor.set_language(ed::language_from_path(path));
    doc->scroll_to_tab = true;
    apply_editor_settings(doc.get());
    documents_.push_back(std::move(doc));
    active_doc_ = (int)documents_.size() - 1;
    selected_path_ = path;
}

void App::save_document(Document* doc, bool save_as) {
    if (!doc) return;
    std::string target = doc->path;
    if (target.empty() || save_as) {
        target = sys::pick_save_file(hwnd_, doc->display_name, workspace_dir_for_dialogs());
        if (target.empty()) return;
    }
    std::string err;
    if (!sys::write_file(target, doc->editor.text(), err)) {
        toast("Save failed: " + err, true);
        return;
    }
    bool was_new = doc->path != target;
    doc->path = sys::normalize(target);
    doc->display_name = sys::file_name(target);
    doc->editor.mark_saved();
    if (was_new) doc->editor.set_language(ed::language_from_path(target));
    toast("Saved " + doc->display_name);
    live_.notify_changed();
    if (was_new && !workspace_.empty()) set_workspace(workspace_); // refresh the tree
}

void App::close_document(int index) {
    if (index < 0 || index >= (int)documents_.size()) return;
    const std::string path = documents_[index]->path;
    if (!path.empty()) {
        closed_paths_.erase(std::remove(closed_paths_.begin(), closed_paths_.end(), path),
                            closed_paths_.end());
        closed_paths_.push_back(path);
        if (closed_paths_.size() > 30) closed_paths_.erase(closed_paths_.begin());
    }
    documents_.erase(documents_.begin() + index);
    if (index < active_doc_) --active_doc_;
    if (documents_.empty()) active_doc_ = -1;
    else active_doc_ = ImClamp(active_doc_, 0, (int)documents_.size() - 1);
}

void App::close_documents(CloseMode mode, int pivot) {
    int kept_unsaved = 0;
    for (int i = (int)documents_.size() - 1; i >= 0; --i) {
        const bool modified = documents_[i]->editor.modified();
        bool close = false;
        switch (mode) {
            case CloseMode::All:        close = true; break;
            case CloseMode::Others:     close = i != pivot; break;
            case CloseMode::Saved:      close = !modified; break;
            case CloseMode::ToTheRight: close = i > pivot; break;
        }
        if (!close) continue;
        if (modified) {
            ++kept_unsaved;
            continue;
        }
        close_document(i);
    }
    if (kept_unsaved > 0)
        toast(std::to_string(kept_unsaved) + (kept_unsaved == 1 ? " tab has" : " tabs have") +
              " unsaved changes and stayed open");
}

void App::reopen_closed_document() {
    while (!closed_paths_.empty()) {
        const std::string path = closed_paths_.back();
        closed_paths_.pop_back();
        if (sys::file_exists(path)) {
            open_file(path);
            return;
        }
    }
    toast("No recently closed files");
}

void App::cycle_document(int direction) {
    if (documents_.empty()) return;
    const int n = (int)documents_.size();
    active_doc_ = ((active_doc_ < 0 ? 0 : active_doc_) + direction + n) % n;
    documents_[active_doc_]->scroll_to_tab = true;
    documents_[active_doc_]->editor.request_focus();
}

void App::save_all() {
    int saved = 0;
    for (auto& d : documents_) {
        if (!d->editor.modified() || d->path.empty()) continue;
        std::string err;
        if (sys::write_file(d->path, d->editor.text(), err)) {
            d->editor.mark_saved();
            ++saved;
        } else {
            toast("Save failed for " + d->display_name + ": " + err, true);
        }
    }
    if (saved > 0) {
        live_.notify_changed();
        toast("Saved " + std::to_string(saved) + (saved == 1 ? " file" : " files"));
    } else {
        toast("Nothing to save");
    }
}

void App::revert_document(Document* doc) {
    if (!doc || doc->path.empty()) return;
    std::string content, err;
    if (!sys::read_file(doc->path, content, err)) {
        toast("Could not read " + doc->display_name + ": " + err, true);
        return;
    }
    // Undoable, so a revert by mistake costs one Ctrl+Z.
    doc->editor.replace_all_text(content);
    doc->editor.mark_saved();
    toast("Reverted " + doc->display_name);
}

void App::zoom_editor(int direction) {
    float size = direction == 0 ? 14.0f : settings_.editor_font_size + (float)direction;
    size = ImClamp(size, 8.0f, 36.0f);
    if (size == settings_.editor_font_size && direction != 0) return;
    settings_.editor_font_size = size;
    ui::set_code_font_size(size);
    ui::request_font_rebuild();
    char msg[48];
    std::snprintf(msg, sizeof(msg), "Editor font %d px", (int)size);
    toast(msg);
}

void App::open_path(const std::string& path) {
    if (path.empty()) return;
    if (sys::is_directory(path)) set_workspace(path);
    else open_file(path);
}

void App::handle_dropped_files(const std::vector<std::string>& paths) {
    for (const std::string& p : paths) open_path(p);
}

std::string App::workspace_dir_for_dialogs() const {
    if (!workspace_.empty()) return workspace_;
    if (active_doc_ >= 0 && active_doc_ < (int)documents_.size() &&
        !documents_[active_doc_]->path.empty())
        return sys::parent_path(documents_[active_doc_]->path);
    return std::string();
}

// ---------------------------------------------------------------- explorer

void App::set_workspace(const std::string& folder) {
    workspace_ = sys::normalize(folder);
    project_map_dirty_ = true;
    tree_root_ = TreeNode();
    tree_root_.name = sys::file_name(workspace_);
    if (tree_root_.name.empty()) tree_root_.name = workspace_;
    tree_root_.path = workspace_;
    tree_root_.is_dir = true;
    tree_root_.expanded = true;
    load_children(tree_root_);

    if (live_.running() && !same_path(live_.root(), workspace_)) {
        std::string err;
        if (live_.start(workspace_, settings_.live_port, err))
            toast("Live server now serving " + sys::file_name(workspace_));
        else
            toast(err, true);
    }

    settings_.last_folder = workspace_;
    auto& recents = settings_.recent_folders;
    recents.erase(std::remove(recents.begin(), recents.end(), workspace_), recents.end());
    recents.insert(recents.begin(), workspace_);
    if (recents.size() > 8) recents.resize(8);
    settings_.save();
}

void App::collect_expanded(const TreeNode& node, std::vector<std::string>& out) const {
    if (node.is_dir && node.expanded) out.push_back(node.path);
    for (const TreeNode& child : node.children) collect_expanded(child, out);
}

void App::restore_expanded(TreeNode& node, const std::vector<std::string>& paths) {
    if (!node.is_dir) return;
    if (std::find(paths.begin(), paths.end(), node.path) == paths.end()) return;
    node.expanded = true;
    if (!node.loaded) load_children(node);
    for (TreeNode& child : node.children) restore_expanded(child, paths);
}

void App::refresh_tree() {
    if (workspace_.empty()) return;
    project_map_dirty_ = true; // the map in the system prompt is now stale
    std::vector<std::string> expanded;
    collect_expanded(tree_root_, expanded);
    load_children(tree_root_);
    for (TreeNode& child : tree_root_.children) restore_expanded(child, expanded);
}

void App::load_children(TreeNode& node) {
    node.children.clear();
    node.loaded = true;
    std::vector<sys::DirEntry> entries;
    if (!sys::list_directory(node.path, entries)) return;
    node.children.reserve(entries.size());
    for (const sys::DirEntry& e : entries) {
        TreeNode child;
        child.name = e.name;
        child.path = e.path;
        child.is_dir = e.is_dir;
        child.size = e.size;
        node.children.push_back(std::move(child));
    }
}

void App::draw_tree_node(TreeNode& node, int depth) {
    const bool selected = !selected_path_.empty() && selected_path_ == node.path;
    const ui::Icon icon = node.expanded ? ui::Icon::FolderOpen : ui::Icon::Folder;
    // Folders keep the monochrome folder glyph; files get their language badge.
    const lang::Id file_lang = node.is_dir ? lang::Id::Count : lang::from_path(node.name);

    if (ui::tree_row(node.path.c_str(), icon, node.name.c_str(), selected, depth, nullptr,
                     node.is_dir, node.expanded, ui::C.text_dim, file_lang)) {
        if (node.is_dir) {
            node.expanded = !node.expanded;
            if (node.expanded && !node.loaded) load_children(node);
        } else {
            open_file(node.path);
        }
        selected_path_ = node.path;
    }

    if (ImGui::BeginPopupContextItem(node.path.c_str())) {
        ImGui::PushFont(ui::F.ui);
        if (!node.is_dir && ImGui::MenuItem("Open")) open_file(node.path);
        if (node.is_dir && ImGui::MenuItem("Open as workspace")) set_workspace(node.path);
        if (ImGui::MenuItem(kRevealLabel)) sys::open_in_explorer(node.path);
        if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(node.path.c_str());
        if (!node.is_dir && ImGui::MenuItem("Add to chat context")) {
            std::string content, err;
            if (sys::read_file(node.path, content, err)) {
                if ((int)content.size() > settings_.max_context_bytes)
                    content = content.substr(0, (size_t)settings_.max_context_bytes);
                chat_input_ += (chat_input_.empty() ? "" : "\n") + std::string("About `") +
                               node.name + "`:\n";
                focus_chat_input_ = true;
                std::string attach = "```" +
                                     std::string(ed::language_tag(ed::language_from_path(node.path))) +
                                     "\n" + content + "\n```";
                toast("Added " + node.name + " to the next message");
                chat_input_ += "";
                // stash the file content as an attachment for the next send
                pending_attachment_ = attach;
                pending_attachment_note_ = node.name;
                pending_attachment_path_ = node.path;
            }
        }
        if (node.is_dir && ImGui::MenuItem("Refresh")) load_children(node);
        ImGui::PopFont();
        ImGui::EndPopup();
    }

    if (node.is_dir && node.expanded) {
        for (TreeNode& child : node.children) draw_tree_node(child, depth + 1);
    }
}

void App::draw_explorer() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(8), ui::px(10)));
    ImGui::Begin("Explorer###explorer");
    ImGui::PopStyleVar();
    ui::pane_title(1, "explorer",
                   workspace_.empty() ? "" : sys::file_name(workspace_).c_str());

    if (workspace_.empty()) {
        ui::vspace(6);
        ImGui::PushFont(ui::F.ui_small);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::C.text_dim));
        ImGui::TextWrapped("No folder opened yet. Open one to browse and edit its files.");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ui::vspace(8);
        if (ui::primary_button("Open Folder", ImVec2(-1, ImGui::GetFrameHeight() + ui::px(4)))) {
            std::string folder = sys::pick_folder(hwnd_, "Open Folder");
            if (!folder.empty()) set_workspace(folder);
        }
        if (!settings_.recent_folders.empty()) {
            ui::vspace(10);
            ImGui::PushFont(ui::F.ui_small);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "RECENT");
            ImGui::PopFont();
            ui::vspace(2);
            for (const std::string& folder : settings_.recent_folders) {
                std::string name = sys::file_name(folder);
                if (ui::tree_row(folder.c_str(), ui::Icon::Folder, name.c_str(), false, 0, nullptr))
                    set_workspace(folder);
                if (ImGui::IsItemHovered()) ui::tooltip(folder.c_str());
            }
        }
        ui::pane_frame();
        ImGui::End();
        return;
    }

    // Files or search, the two views VS Code keeps in its sidebar.
    {
        const float half = std::floor((ImGui::GetContentRegionAvail().x - 1.0f) * 0.5f);
        if (segment_toggle("##view_files", "files", !explorer_search_, half)) explorer_search_ = false;
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 1.0f);
        if (segment_toggle("##view_search", "search", explorer_search_, half + 1.0f)) {
            explorer_search_ = true;
            search_focus_ = true;
        }
        if (ImGui::IsItemHovered()) ui::tooltip("Find in files (Ctrl+Shift+F)");
        ui::vspace(6);
    }
    if (explorer_search_) {
        draw_search_view();
        ui::pane_frame();
        ImGui::End();
        return;
    }

    // The pane title already carries the workspace name, so this row is just
    // the actions, sitting alongside the filter box.
    ImGui::PushFont(ui::F.ui_small);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui::px(80));
    ImGui::InputTextWithHint("##filter", "filter", filter_buffer_, sizeof(filter_buffer_));
    ImGui::PopFont();

    ImGui::SameLine(0, ui::px(4));
    if (ui::icon_button("##refresh", ui::Icon::Refresh, "Refresh", 0, ui::px(22)))
        set_workspace(workspace_);
    ImGui::SameLine(0, ui::px(2));
    if (ui::icon_button("##newfile", ui::Icon::Plus, "New file", 0, ui::px(22))) new_document();
    ImGui::SameLine(0, ui::px(2));
    if (ui::icon_button("##openfolder", ui::Icon::FolderOpen, "Open another folder", 0,
                        ui::px(22))) {
        std::string folder = sys::pick_folder(hwnd_, "Open Folder");
        if (!folder.empty()) set_workspace(folder);
    }
    ui::vspace(3);

    ImGui::BeginChild("##tree", ImVec2(0, 0), ImGuiChildFlags_None);
    if (filter_buffer_[0]) {
        // flat, filtered view
        std::string needle = filter_buffer_;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        int shown = 0;
        std::vector<TreeNode*> stack{&tree_root_};
        while (!stack.empty() && shown < 300) {
            TreeNode* node = stack.back();
            stack.pop_back();
            if (!node->loaded && node->is_dir) load_children(*node);
            for (TreeNode& child : node->children) {
                if (child.is_dir) {
                    stack.push_back(&child);
                    continue;
                }
                std::string lower = child.name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (lower.find(needle) == std::string::npos) continue;
                if (ui::tree_row(child.path.c_str(), ui::Icon::File, child.name.c_str(),
                                 selected_path_ == child.path, 0, nullptr, false, false,
                                 ui::C.text_dim, lang::from_path(child.name))) {
                    selected_path_ = child.path;
                    open_file(child.path);
                }
                ++shown;
                if (shown >= 300) break;
            }
        }
        if (shown == 0) {
            ImGui::PushFont(ui::F.ui_small);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "No matches");
            ImGui::PopFont();
        }
    } else {
        for (TreeNode& child : tree_root_.children) draw_tree_node(child, 0);
    }
    ImGui::EndChild();
    ui::pane_frame();
    ImGui::End();
}

// ------------------------------------------------------------------ editor

void App::draw_welcome(const ImVec2& size) {
    // The empty pane is a title page: the mark, the name in wide capitals, a
    // rule, and then the state of the machine set as a small table.
    const float card_w = ImMin(ui::px(560.0f), size.x - ui::px(60));
    // Sits a little above centre, but never so far down that the shortcut list
    // at the bottom falls off a short pane.
    const float top = ImClamp(size.y * 0.10f, ui::px(20.0f), ui::px(70.0f));
    ImGui::SetCursorPos(ImVec2((size.x - card_w) * 0.5f, top));
    // Every gap on this page is set by hand, so the automatic spacing between
    // items is taken out of the way first.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginGroup();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* micro = ui::F.ui_micro ? ui::F.ui_micro : ui::F.ui_small;
    const float mfs = micro ? micro->FontSize : ImGui::GetFontSize();

    // Masthead.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float logo = ui::px(30);
        ui::draw_icon(dl, ui::Icon::Globe, ImVec2(p.x, p.y), logo, ui::C.ink);
        ImFont* big = ui::F.ui_large ? ui::F.ui_large : ui::F.ui;
        const float bfs = big ? big->FontSize : mfs;
        ui::draw_caps(dl, big, bfs, ImVec2(p.x + logo + ui::px(14), p.y + (logo - bfs) * 0.5f),
                      ui::C.ink, "ai-ide", ui::px(7.0f));
        ImGui::Dummy(ImVec2(card_w, logo));
    }

    ui::vspace(14);
    ui::rule();
    ui::vspace(12);

    struct Row { const char* key; std::string value; };
    const Row info[] = {
        {"editor", "local, no account, no telemetry"},
        {"backend", settings_.ollama_host},
        {"model", settings_.chat_model.empty() ? std::string("none selected")
                                               : settings_.chat_model},
        {"status", ollama_online_ ? std::string("connected") : std::string("ollama not reachable")},
        {"workspace", workspace_.empty() ? std::string("none open")
                                         : display_path(workspace_, std::string(), 34)},
    };
    const float key_col = ui::px(110);
    for (const Row& r : info) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ui::draw_caps(dl, micro, mfs, p, ui::C.text_faint, r.key);
        const bool bad = r.key[0] == 's' && !ollama_online_;
        dl->AddText(micro, mfs, ImVec2(p.x + key_col, p.y), bad ? ui::C.error : ui::C.text,
                    r.value.c_str());
        ImGui::Dummy(ImVec2(card_w, mfs + ui::px(11)));
    }

    ui::vspace(16);
    const float btn_w = (card_w - ui::px(8)) * 0.5f;
    const float btn_h = std::floor(mfs + ui::px(20));
    if (ui::primary_button("open folder  ctrl+k", ImVec2(btn_w, btn_h))) {
        std::string folder = sys::pick_folder(hwnd_, "Open Folder");
        if (!folder.empty()) set_workspace(folder);
    }
    ImGui::SameLine(0, ui::px(8));
    if (ui::ghost_button("new file  ctrl+n", ImVec2(btn_w, btn_h))) new_document();

    ui::vspace(14);
    ui::rule();
    ui::vspace(10);

    struct Bind { const char* key; const char* what; };
    const Bind binds[] = {
        {"ctrl+o", "open file"},        {"ctrl+k", "open folder"},
        {"ctrl+s", "save"},             {"ctrl+f", "find in file"},
        {"ctrl+space", "ai completion at the caret"},
        {"ctrl+l", "ask the ai about the current file"},
        {"ctrl+/", "toggle comment"},   {"alt+up/down", "move lines"},
    };
    for (const Bind& b : binds) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ui::draw_caps(dl, micro, mfs, p, ui::C.text_dim, b.key);
        dl->AddText(micro, mfs, ImVec2(p.x + key_col, p.y), ui::C.text_faint, b.what);
        ImGui::Dummy(ImVec2(card_w, mfs + ui::px(8)));
    }

    ImGui::EndGroup();
    ImGui::PopStyleVar();
}

void App::draw_editor_panel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Editor###editor", nullptr, ImGuiWindowFlags_NoScrollbar |
                                                 ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();


    {
        Document* titled = active_document();
        std::string detail;
        if (titled)
            detail = titled->path.empty() ? titled->display_name
                                          : display_path(titled->path, workspace_, 40);
        ui::pane_title(2, "editor", detail.c_str());
    }

    if (documents_.empty()) {
        draw_welcome(ImGui::GetContentRegionAvail());
        ui::pane_frame();
        ImGui::End();
        return;
    }

    // --- tab rail ---
    // Numbered cells rather than tabs: each one carries its index and language
    // above the file name, and the open document is the single filled cell.
    int close_request = -1;
    {
        // Tall enough for a cell plus the thin scrollbar that appears once the
        // rail runs past the pane edge — without it, tabs off the right side
        // would only be reachable through the file tree.
        const float rail_h = std::floor(ui::px(60));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::BeginChild("##tabrail", ImVec2(0, rail_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::SetCursorPos(ImVec2(ui::px(10), ui::px(4)));

        for (int i = 0; i < (int)documents_.size(); ++i) {
            Document* doc = documents_[i].get();
            if (i) {
                // Cells sit shoulder to shoulder and share one hairline, so the
                // rail reads as a single ruled strip rather than a row of boxes.
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 1.0f);
            }

            char id[32];
            std::snprintf(id, sizeof(id), "##tab%d", i);
            const bool active = i == active_doc_;
            ui::TabCell cell =
                ui::numbered_tab(id, i + 1, doc->display_name.c_str(), active,
                                 doc->editor.modified(), doc->editor.language());

            if (doc->scroll_to_tab) {
                doc->scroll_to_tab = false;
                active_doc_ = i;
                ImGui::SetScrollHereX(0.5f);
            }
            if (cell.clicked) {
                active_doc_ = i;
                doc->editor.request_focus();
            }
            if (cell.close_clicked) close_request = i;
            if (cell.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) close_request = i;
            if (cell.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                tab_menu_doc_ = i;
                ImGui::OpenPopup("##tabmenu");
            }
            if (cell.hovered && !doc->path.empty()) ui::tooltip(doc->path.c_str());
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), ui::px(6)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(8), ui::px(8)));
        if (ImGui::BeginPopup("##tabmenu")) {
            ImGui::PushFont(ui::F.ui);
            const int t = tab_menu_doc_;
            const bool valid = t >= 0 && t < (int)documents_.size();
            const bool has_path = valid && !documents_[t]->path.empty();
            if (ImGui::MenuItem("Close", ui::keys("Ctrl+W"), false, valid)) close_request = t;
            if (ImGui::MenuItem("Close Others", nullptr, false, valid && documents_.size() > 1))
                close_documents(CloseMode::Others, t);
            if (ImGui::MenuItem("Close to the Right", nullptr, false, valid && t + 1 < (int)documents_.size()))
                close_documents(CloseMode::ToTheRight, t);
            if (ImGui::MenuItem("Close Saved")) close_documents(CloseMode::Saved, t);
            if (ImGui::MenuItem("Close All")) close_documents(CloseMode::All, t);
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Path", nullptr, false, has_path))
                ImGui::SetClipboardText(documents_[t]->path.c_str());
            if (ImGui::MenuItem("Copy Relative Path", nullptr, false, has_path))
                ImGui::SetClipboardText(display_path(documents_[t]->path, workspace_, 4096).c_str());
            if (ImGui::MenuItem(kRevealLabel, nullptr, false, has_path))
                sys::open_in_explorer(documents_[t]->path);
            ImGui::Separator();
            if (ImGui::MenuItem("Reopen Closed Editor", ui::keys("Ctrl+Shift+T"), false, !closed_paths_.empty()))
                reopen_closed_document();
            ImGui::PopFont();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        // The rail is closed by a full-width rule, the way the reference closes
        // every horizontal band.
        {
            ImDrawList* rdl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            rdl->AddLine(ImVec2(p.x, std::floor(p.y) + 0.5f),
                         ImVec2(p.x + w, std::floor(p.y) + 0.5f), ui::C.border, 1.0f);
        }
    }

    Document* doc = active_document();
    if (doc) {
        apply_editor_settings(doc);

        // --- action strip ---
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(8), ui::px(4)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(6), ui::px(4)));
        ImGui::Indent(ui::px(10));
        ui::vspace(3);

        // The pane title already shows the path, so this row is only the actions
        // and they get all of it — in a mono face they are wide enough that
        // sharing the row with a path pushed them off the edge.
        const bool completing = completion_stream_ != nullptr;
        if (ui::chip_button(completing ? "Completing…" : "Complete", ui::Icon::Sparkle,
                            "AI completion at the caret (Ctrl+Space)") &&
            !completing)
            request_inline_completion();
        ImGui::SameLine(0, ui::px(5));
        if (ui::chip_button("Explain", ui::Icon::Robot, "Explain the selection or file (Ctrl+L)"))
            quick_action("Explain", "Explain what this code does, step by step.", true);
        ImGui::SameLine(0, ui::px(5));
        if (ui::chip_button("Improve", ui::Icon::Wand, "Suggest a cleaner implementation"))
            quick_action("Improve",
                         "Refactor this code to be clearer and more idiomatic. "
                         "Return the full replacement code in one fenced block, then a short "
                         "list of what you changed.",
                         true,
                         "Refactor this code to be clearer and more idiomatic. Make the change "
                         "in the file itself with edit_file, one edit per place you change, "
                         "then list what you changed in a sentence or two.");
        ImGui::SameLine(0, ui::px(5));
        if (ui::chip_button("Fix", ui::Icon::Check, "Find and fix bugs"))
            quick_action("Fix",
                         "Find bugs or edge cases in this code and fix them. Show the corrected "
                         "code in a fenced block and explain each fix briefly.",
                         true,
                         "Find bugs or edge cases in this code and fix them in the file itself "
                         "with edit_file, one edit per fix. Then explain each fix briefly. If "
                         "you find nothing wrong, say so and change nothing.");
        ImGui::SameLine(0, ui::px(5));
        if (ui::chip_button("Tests", ui::Icon::Code, "Write tests for this code"))
            quick_action("Tests", "Write focused unit tests for this code.", true,
                         "Write focused unit tests for this code and put them in the project "
                         "with create_file, in the place and the naming style this project "
                         "already uses for its tests.");

        // The preview lives with the other actions on the open file: start the
        // local server if it is down, then open this page in the browser.
        ImGui::SameLine(0, ui::px(5));
        const bool live_up = live_.running();
        if (ui::chip_button(live_up ? "Preview" : "Go live", ui::Icon::Globe,
                            live_up ? "Open this page in the browser (Alt+L)"
                                    : "Serve this folder on localhost and open it (Alt+L)"))
            open_live_preview();
        if (live_up) {
            ImGui::SameLine(0, ui::px(5));
            if (ui::chip_button("Stop##live", ui::Icon::Stop, "Stop the live server"))
                stop_live_server();
        }

        ImGui::Unindent(ui::px(10));
        ImGui::PopStyleVar(2);
        ui::vspace(3);

        ImVec2 avail = ImGui::GetContentRegionAvail();
        // One view per document, so every tab keeps its own scroll position.
        char code_id[40];
        std::snprintf(code_id, sizeof(code_id), "##code%p", (void*)doc);
        doc->editor.render(code_id, avail);
        if (const int zoom = doc->editor.take_zoom_request()) zoom_editor(zoom);
    }

    ui::pane_frame();
    ImGui::End();

    if (close_request >= 0) close_document(close_request);
}

// -------------------------------------------------------------------- chat

void App::draw_code_block(const std::string& code, const std::string& lang_tag, int block_id,
                          const std::string& target_path) {
    // The registry resolves fence tags directly, including the aliases models
    // actually emit ("c++", "py3", "console"). Fall back to the target path
    // when a block carries one but no tag.
    ed::Language lang = ed::language_from_tag(lang_tag);
    if (lang == ed::Language::PlainText && !target_path.empty())
        lang = ed::language_from_path(target_path);

    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : code) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        lines.push_back(cur);
        while (lines.size() > 1 && trim(lines.back()).empty()) lines.pop_back();
    }

    ImGui::PushID(block_id);
    const float width = ImGui::GetContentRegionAvail().x;
    const float header_h = ImGui::GetFontSize() + ui::px(12);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    // header
    const bool can_write = !target_path.empty() && !workspace_.empty();
    ImFont* hfont = ui::F.ui_micro ? ui::F.ui_micro : ui::F.ui_small;
    const float hfs = hfont ? hfont->FontSize : ImGui::GetFontSize();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + header_h), ui::C.bg_surface);
    dl->AddLine(ImVec2(origin.x, origin.y + header_h - 0.5f),
                ImVec2(origin.x + width, origin.y + header_h - 0.5f), ui::C.border, 1.0f);
    const std::string header = target_path.empty() ? (lang_tag.empty() ? "code" : lang_tag)
                                                   : target_path;
    ui::draw_caps(dl, hfont, hfs, ImVec2(origin.x + ui::px(12), origin.y + (header_h - hfs) * 0.5f),
                  target_path.empty() ? ui::C.text_dim : ui::C.text, header.c_str());

    // With no folder open there is nowhere to write to, so the block offers to
    // become a file the user picks. Code in a chat window that can only be
    // copied out is the one outcome worth avoiding.
    const bool offer_save_as = !can_write && workspace_.empty();
    const float tools_w = can_write ? ui::px(280) : (offer_save_as ? ui::px(250) : ui::px(160));
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - tools_w, origin.y + ui::px(3)));
    if (offer_save_as) {
        if (ui::chip_button("Save as…", ui::Icon::Save, "Write this block to a file on disk"))
            save_block_as(target_path, lang_tag, code);
        ImGui::SameLine(0, ui::px(4));
    }
    if (can_write) {
        std::string absolute, err;
        const bool exists = resolve_in_workspace(target_path, absolute, err) &&
                            sys::file_exists(absolute);
        if (ui::chip_button(exists ? "Replace file" : "Create file", ui::Icon::Save,
                            exists ? "Overwrite this file in the project"
                                   : "Write this block to the project"))
            write_block_to_file(target_path, code);
        ImGui::SameLine(0, ui::px(4));
    }
    bool copy = ui::chip_button("Copy", ui::Icon::Copy, "Copy to clipboard");
    ImGui::SameLine(0, ui::px(4));
    bool insert = ui::chip_button("Insert", ui::Icon::Insert, "Insert at the caret in the editor");
    if (copy) {
        ImGui::SetClipboardText(code.c_str());
        toast("Copied to clipboard");
    }
    if (insert) insert_code_into_editor(code, true);

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + header_h));

    // body
    ImFont* mono = ui::F.mono_small;
    const float line_h = std::floor(mono->FontSize * 1.45f);
    const float body_h = ImMin(lines.size() * line_h + ui::px(14), ui::px(420.0f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(ui::C.code_bg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(12), ui::px(7)));
    ImGui::BeginChild("##codebody", ImVec2(width, body_h),
                      ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* body_dl = ImGui::GetWindowDrawList();
    ImVec2 text_pos = ImGui::GetCursorScreenPos();
    float max_w = 0.0f;
    unsigned char state = 0;
    std::vector<ed::Span> spans;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        state = ed::highlight_line(lang, line, state, spans);
        float x = text_pos.x;
        const float y = text_pos.y + i * line_h;
        for (const ed::Span& sp : spans) {
            const char* b = line.data() + sp.start;
            const char* e = line.data() + sp.end;
            body_dl->AddText(mono, mono->FontSize, ImVec2(x, y), sp.color, b, e);
            x += mono->CalcTextSizeA(mono->FontSize, FLT_MAX, 0.0f, b, e).x;
        }
        max_w = ImMax(max_w, x - text_pos.x);
    }
    ImGui::Dummy(ImVec2(max_w + ui::px(10), lines.size() * line_h));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImVec2 end = ImGui::GetCursorScreenPos();
    dl->AddRect(origin, ImVec2(origin.x + width, end.y - ui::px(1)), ui::C.border, 0.0f, 0, 1.0f);
    ImGui::PopID();
    ui::vspace(4);
}

namespace {

struct InlineRun {
    std::string text;
    bool code = false;
    bool bold = false;
};

void parse_inline(const std::string& s, std::vector<InlineRun>& out) {
    out.clear();
    std::string current;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '`') {
            size_t end = s.find('`', i + 1);
            if (end != std::string::npos) {
                if (!current.empty()) { out.push_back({current, false, false}); current.clear(); }
                InlineRun run;
                run.text = s.substr(i + 1, end - i - 1);
                run.code = true;
                out.push_back(run);
                i = end + 1;
                continue;
            }
        }
        if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') {
            size_t end = s.find("**", i + 2);
            if (end != std::string::npos) {
                if (!current.empty()) { out.push_back({current, false, false}); current.clear(); }
                InlineRun run;
                run.text = s.substr(i + 2, end - i - 2);
                run.bold = true;
                out.push_back(run);
                i = end + 2;
                continue;
            }
        }
        current += s[i];
        ++i;
    }
    if (!current.empty()) out.push_back({current, false, false});
}

// Word-wrapped inline layout with per-run fonts and inline code chips.
void draw_inline_runs(const std::vector<InlineRun>& runs, float wrap_width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float line_h = std::floor(ui::F.ui->FontSize * 1.45f);
    float x = start.x;
    float y = start.y;

    for (const InlineRun& run : runs) {
        ImFont* font = run.code ? ui::F.mono_small : (run.bold ? ui::F.ui_bold : ui::F.ui);
        const float size = font->FontSize;
        const ImU32 color = run.code ? ui::C.syn_keyword : ui::C.text;

        size_t i = 0;
        while (i < run.text.size()) {
            if (run.text[i] == '\n') {
                x = start.x;
                y += line_h;
                ++i;
                continue;
            }
            size_t end = i;
            while (end < run.text.size() && run.text[end] != ' ' && run.text[end] != '\n') ++end;
            while (end < run.text.size() && run.text[end] == ' ') ++end;

            const char* b = run.text.data() + i;
            const char* e = run.text.data() + end;
            float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, b, e).x;

            if (x + w > start.x + wrap_width && x > start.x) {
                x = start.x;
                y += line_h;
                while (b < e && *b == ' ') ++b; // drop the leading space after a wrap
                w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, b, e).x;
            }
            if (run.code) {
                dl->AddRectFilled(ImVec2(x - ui::px(3), y + ui::px(1)),
                                  ImVec2(x + w + ui::px(3), y + line_h - ui::px(2)),
                                  ui::C.bg_surface_hi);
            }
            dl->AddText(font, size, ImVec2(x, y + (line_h - size) * 0.5f), color, b, e);
            x += w;
            i = end;
        }
    }
    ImGui::Dummy(ImVec2(wrap_width, y + line_h - start.y));
}

} // namespace

void App::draw_markdown(const std::string& text, float wrap_width, int message_index) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        lines.push_back(cur);
    }

    std::vector<InlineRun> runs;
    std::string paragraph;
    int block_counter = 0;

    auto flush_paragraph = [&]() {
        if (trim(paragraph).empty()) { paragraph.clear(); return; }
        parse_inline(paragraph, runs);
        draw_inline_runs(runs, wrap_width);
        paragraph.clear();
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        std::string stripped = trim(line);

        if (stripped.rfind("```", 0) == 0) {
            flush_paragraph();
            std::string lang, path;
            agent::parse_fence_info(trim(stripped.substr(3)), lang, path);
            std::string code;
            ++i;
            for (; i < lines.size(); ++i) {
                if (trim(lines[i]).rfind("```", 0) == 0) break;
                code += lines[i];
                code += "\n";
            }
            if (!code.empty()) code.pop_back();
            draw_code_block(code, lang, message_index * 1000 + block_counter++, path);
            continue;
        }

        if (stripped.empty()) {
            flush_paragraph();
            ui::vspace(4);
            continue;
        }

        if (stripped[0] == '#') {
            flush_paragraph();
            size_t level = stripped.find_first_not_of('#');
            std::string title = level == std::string::npos ? "" : trim(stripped.substr(level));
            ui::vspace(6);
            ui::caps_text(title.c_str(), ui::C.text, ui::F.ui_small);
            ui::vspace(3);
            {
                ImDrawList* hdl = ImGui::GetWindowDrawList();
                const ImVec2 hp = ImGui::GetCursorScreenPos();
                hdl->AddLine(ImVec2(hp.x, std::floor(hp.y) + 0.5f),
                             ImVec2(hp.x + wrap_width, std::floor(hp.y) + 0.5f), ui::C.border,
                             1.0f);
            }
            ui::vspace(5);
            continue;
        }

        bool bullet = (stripped.size() > 1 && (stripped[0] == '-' || stripped[0] == '*') &&
                       stripped[1] == ' ');
        bool numbered = stripped.size() > 2 && std::isdigit((unsigned char)stripped[0]) &&
                        (stripped[1] == '.' || stripped[1] == ')') && stripped[2] == ' ';
        if (bullet || numbered) {
            flush_paragraph();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float line_h = std::floor(ui::F.ui->FontSize * 1.45f);
            std::string body;
            if (bullet) {
                const float bs = ui::px(4);
                dl->AddRectFilled(ImVec2(p.x + ui::px(3), p.y + (line_h - bs) * 0.5f),
                                  ImVec2(p.x + ui::px(3) + bs, p.y + (line_h + bs) * 0.5f),
                                  ui::C.ink);
                body = trim(stripped.substr(2));
            } else {
                std::string marker = stripped.substr(0, 2);
                dl->AddText(ui::F.ui_small, ui::F.ui_small->FontSize,
                            ImVec2(p.x, p.y + (line_h - ui::F.ui_small->FontSize) * 0.5f),
                            ui::C.text_dim, marker.c_str());
                body = trim(stripped.substr(3));
            }
            ImGui::Indent(ui::px(16));
            parse_inline(body, runs);
            draw_inline_runs(runs, wrap_width - ui::px(16));
            ImGui::Unindent(ui::px(16));
            continue;
        }

        if (!paragraph.empty()) paragraph += " ";
        paragraph += stripped;
    }
    flush_paragraph();
}

void App::draw_tool_action(ToolAction& action, int id, float width) {
    ImGui::PushID(id);

    const bool pending = !action.resolved;
    const ImU32 accent =
        pending ? ui::C.warning
                : (action.rejected ? ui::C.text_faint
                                   : (!action.ok ? ui::C.error
                                                 : (action.warned ? ui::C.warning
                                                                  : ui::C.success)));

    std::string title;
    ui::Icon icon = ui::Icon::File;
    if (action.tool == "create_file") {
        title = action.overwrite ? "Replace file" : "Create file";
        icon = ui::Icon::File;
    } else if (action.tool == "create_directory") {
        title = "Create folder";
        icon = ui::Icon::Folder;
    } else if (action.tool == "read_file") {
        title = "Read file";
        icon = ui::Icon::Search;
    } else if (action.tool == "list_directory") {
        title = "List folder";
        icon = ui::Icon::FolderOpen;
    } else if (action.tool == "edit_file") {
        title = "Edit file";
        icon = ui::Icon::Wand;
    } else if (action.tool == "search_files") {
        title = "Search";
        icon = ui::Icon::Search;
    } else {
        title = action.tool;
        icon = ui::Icon::Warning;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImGui::ColorConvertU32ToFloat4(with_alpha(ui::C.bg_surface, 0xC0)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(11), ui::px(9)));
    ImGui::BeginChild("##action", ImVec2(width, 0),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_Borders);

    // header row
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 head = ImGui::GetCursorScreenPos();
    const float glyph = ui::px(15);
    ui::draw_icon(dl, icon, ImVec2(head.x, head.y + ui::px(2)), glyph, accent);
    ImGui::SetCursorScreenPos(ImVec2(head.x + glyph + ui::px(8), head.y));

    ui::caps_text(title.c_str(), accent);
    ImGui::SameLine(0, ui::px(9));
    ImGui::PushFont(ui::F.mono_small);
    const std::string subject =
        action.tool == "search_files" ? ('"' + action.query + '"') : action.path;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text), "%s", subject.c_str());
    ImGui::PopFont();

    // status / result
    ImGui::PushFont(ui::F.ui_small);
    if (pending) {
        const char* note = "Waiting for your approval.";
        if (action.touches_unsaved) note = "This file is open with unsaved changes.";
        else if (action.tool == "edit_file") note = "This rewrites part of an existing file.";
        else if (action.overwrite) note = "This replaces a file that already exists.";
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_dim), "%s", note);
    } else if (action.rejected) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "Declined");
    } else {
        // Reads produce long output; show only the first line as a summary.
        std::string summary = action.result;
        size_t nl = summary.find('\n');
        if (action.read_only && nl != std::string::npos) summary = summary.substr(0, nl);
        const size_t summary_cap = action.warned ? 320 : 160;
        if (summary.size() > summary_cap) summary = summary.substr(0, summary_cap) + "…";
        ImGui::PushTextWrapPos(0.0f);
        const ImU32 summary_color = !action.ok ? ui::C.error
                                              : (action.warned ? ui::C.warning : ui::C.text_dim);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(summary_color), "%s", summary.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();

    const bool has_preview =
        (action.tool == "create_file" && !action.content.empty()) ||
        (action.tool == "edit_file" && !action.find_text.empty());
    if (has_preview || pending) {
        ui::vspace(4);
        if (has_preview) {
            if (ui::chip_button(action.expanded ? "Hide content" : "Show content",
                                action.expanded ? ui::Icon::ChevronDown : ui::Icon::ChevronRight))
                action.expanded = !action.expanded;
        }
        if (pending) {
            // The chat panel can be narrow, so split the row evenly instead of
            // using fixed widths that would overflow.
            const float row = ImGui::GetContentRegionAvail().x;
            const float button = ImMax(ui::px(70.0f), (row - ui::px(6)) * 0.5f);
            if (ui::primary_button("Approve", ImVec2(button, 0))) execute_action(action);
            ImGui::SameLine(0, ui::px(6));
            if (ui::ghost_button("Decline", ImVec2(button, 0))) {
                action.resolved = true;
                action.rejected = true;
                action.result = "Declined by the user.";
            }
        }
    }

    if (has_preview && action.expanded) {
        ui::vspace(4);
        const std::string tag = ed::language_tag(ed::language_from_path(action.path));
        if (action.tool == "edit_file") {
            ImGui::PushFont(ui::F.ui_small);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.error), "Replacing");
            ImGui::PopFont();
            draw_code_block(action.find_text, tag, 900000 + id);
            ImGui::PushFont(ui::F.ui_small);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.success), "With");
            ImGui::PopFont();
            draw_code_block(action.replace_text, tag, 910000 + id);
        } else {
            draw_code_block(action.content, tag, 900000 + id);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // A stripe down the left edge of the card carries its state.
    ImVec2 lo = ImGui::GetItemRectMin();
    ImVec2 hi = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(lo, ImVec2(lo.x + ui::px(2), hi.y), accent);
    ImGui::PopID();
    ui::vspace(5);
}

void App::draw_message(ChatMessage& msg, int index) {
    const bool user = msg.role == "user";
    const float avail = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(index);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // avatar + role. Yours is the filled square, the model's is outlined — the
    // same "active is ink, everything else is a hairline" rule as the chrome.
    ImVec2 head = ImGui::GetCursorScreenPos();
    const float dot = ui::px(20);
    const ImVec2 dot_hi(head.x + dot, head.y + dot);
    if (user) dl->AddRectFilled(head, dot_hi, ui::C.ink);
    else dl->AddRect(head, dot_hi, ui::C.border_strong);
    ui::draw_icon(dl, user ? ui::Icon::User : ui::Icon::Sparkle,
                  ImVec2(head.x + ui::px(4), head.y + ui::px(4)), dot - ui::px(8),
                  user ? ui::C.on_ink : ui::C.text);

    // The header row is drawn rather than laid out: a tracked name on the left,
    // the attachment note beside it, and the token count set flush right.
    {
        ImFont* micro = ui::F.ui_micro ? ui::F.ui_micro : ui::F.ui_small;
        const float mfs = micro ? micro->FontSize : ImGui::GetFontSize();
        const float ty = head.y + ui::px(5);
        float x = head.x + dot + ui::px(10);
        const char* who = user ? "you"
                               : (settings_.chat_model.empty() ? "assistant"
                                                               : settings_.chat_model.c_str());
        x += ui::draw_caps(dl, micro, mfs, ImVec2(x, ty), ui::C.text, who) + ui::px(10);

        if (!msg.attachment_note.empty())
            x += ui::draw_caps(dl, micro, mfs, ImVec2(x, ty), ui::C.text_faint,
                               msg.attachment_note.c_str()) +
                 ui::px(10);

        if (!msg.streaming && msg.tokens_per_second > 0.0) {
            char stats[64];
            std::snprintf(stats, sizeof(stats), "%d tok / %.0f tok s", msg.eval_count,
                          msg.tokens_per_second);
            const float w = ui::caps_width(micro, mfs, stats);
            const float sx = head.x + avail - w;
            if (sx > x) ui::draw_caps(dl, micro, mfs, ImVec2(sx, ty), ui::C.text_faint, stats);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(head.x + dot + ui::px(10), head.y + dot + ui::px(8)));
    ImGui::BeginGroup();
    const float body_width = avail - dot - ui::px(10);

    if (!msg.thinking.empty()) {
        ImGui::PushFont(ui::F.ui_small);
        std::string label = (msg.show_thinking ? "Hide reasoning" : "Show reasoning");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::C.text_faint));
        if (ImGui::SmallButton(label.c_str())) msg.show_thinking = !msg.show_thinking;
        ImGui::PopStyleColor();
        if (msg.show_thinking) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                  ImGui::ColorConvertU32ToFloat4(with_alpha(ui::C.bg_surface, 0x80)));
            ImGui::BeginChild("##think", ImVec2(body_width, ImMin(ui::px(220.0f),
                                  ImGui::CalcTextSize(msg.thinking.c_str(), nullptr, false,
                                                      body_width - ui::px(16)).y + ui::px(16))),
                              ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::C.text_faint));
            ImGui::PushTextWrapPos(body_width - ui::px(16));
            ImGui::TextUnformatted(msg.thinking.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        ImGui::PopFont();
    }

    if (user) {
        // What you asked is set as a quotation: a rule down the left margin and
        // the text indented off it, rather than a bubble.
        ImGui::PushFont(ui::F.ui);
        const float bar = ui::px(2);
        const float inset = ui::px(12);
        const float wrap = body_width - inset;
        const ImVec2 size = ImGui::CalcTextSize(msg.content.c_str(), nullptr, false, wrap);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + bar, p.y + size.y + ui::px(4)), ui::C.ink);
        ImGui::SetCursorScreenPos(ImVec2(p.x + inset, p.y));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextUnformatted(msg.content.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + size.y + ui::px(10)));
    } else if (msg.content.empty() && msg.streaming) {
        ui::typing_indicator((float)ImGui::GetTime());
    } else {
        draw_markdown(msg.content, body_width, index);
    }

    // file-system actions requested by the model
    if (!msg.actions.empty()) {
        ui::vspace(4);
        int pending = 0;
        for (const ToolAction& action : msg.actions)
            if (!action.resolved) ++pending;

        if (pending > 1) {
            char label[64];
            std::snprintf(label, sizeof(label), "Approve all %d", pending);
            const float row = body_width;
            const float button = ImMax(ui::px(80.0f), (row - ui::px(6)) * 0.5f);
            if (ui::primary_button(label, ImVec2(button, 0))) {
                for (ToolAction& action : msg.actions)
                    if (!action.resolved) execute_action(action);
            }
            ImGui::SameLine(0, ui::px(6));
            if (ui::ghost_button("Decline all", ImVec2(button, 0))) {
                for (ToolAction& action : msg.actions) {
                    if (action.resolved) continue;
                    action.resolved = true;
                    action.rejected = true;
                    action.result = "Declined by the user.";
                }
            }
            ui::vspace(5);
        }
        for (size_t i = 0; i < msg.actions.size(); ++i)
            draw_tool_action(msg.actions[i], index * 100 + (int)i, body_width);
    }

    if (!msg.error.empty()) {
        ImGui::PushFont(ui::F.ui_small);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + body_width);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::C.error));
        ImGui::Text("Error: %s", msg.error.c_str());
        ImGui::PopStyleColor();

        // A raw runtime error rarely says what to do next; add that when we can
        // recognise it.
        const std::string hint = error_hint(msg.error);
        if (!hint.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(ui::C.text_dim));
            ImGui::TextUnformatted(hint.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }

    // per-message actions
    if (!msg.streaming && !user && !msg.content.empty()) {
        ui::vspace(2);
        if (ui::chip_button("Copy##msg", ui::Icon::Copy, "Copy the whole reply")) {
            ImGui::SetClipboardText(msg.content.c_str());
            toast("Reply copied");
        }
    }

    ImGui::EndGroup();
    ImGui::PopID();
    ui::vspace(10);
}

void App::draw_chat_panel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(10), ui::px(10)));
    ImGui::Begin("AI Assistant###chat");
    ImGui::PopStyleVar();
    ui::pane_title(3, "assistant", settings_.chat_model.c_str());

    // --- header: model picker + status ---
    const float avail_w = ImGui::GetContentRegionAvail().x;
    // The picker is a field, not a control: hairline box, small type, and the
    // same height as the two icon buttons beside it.
    ImGui::PushFont(ui::F.ui_small);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(10), ui::px(7)));
    ImGui::SetNextItemWidth(avail_w - ui::px(64));
    std::string preview = settings_.chat_model.empty() ? "Select a model" : settings_.chat_model;
    if (ImGui::BeginCombo("##model", preview.c_str())) {
        if (models_.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "%s",
                               models_error_.empty() ? "No models found" : models_error_.c_str());
        }
        for (const ollama::Model& m : models_) {
            bool selected = m.name == settings_.chat_model;
            std::string label = m.name;
            if (ImGui::Selectable(label.c_str(), selected)) {
                settings_.chat_model = m.name;
                settings_.save();
            }
            if (!m.parameter_size.empty()) {
                ImGui::SameLine();
                ImGui::PushFont(ui::F.ui_small);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "  %s  %s",
                                   m.parameter_size.c_str(), human_size(m.size_bytes).c_str());
                ImGui::PopFont();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::SameLine(0, ui::px(4));
    if (ui::icon_button("##reloadmodels", ui::Icon::Refresh, "Reload the model list"))
        refresh_models(true);
    ImGui::SameLine(0, ui::px(2));
    if (ui::icon_button("##newchat", ui::Icon::Plus, "New chat")) {
        stop_chat();
        chat_.clear();
        agent_history_.clear();
        agent_steps_ = 0;
        toast("Started a new chat");
    }

    // --- status line ---
    ImGui::PushFont(ui::F.ui_small);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 sp = ImGui::GetCursorScreenPos();
    const ImU32 mark = ollama_online_ ? ui::C.success : ui::C.error;
    const float m = ui::px(5);
    const float my = sp.y + (ImGui::GetFontSize() - m) * 0.5f;
    dl->AddRectFilled(ImVec2(sp.x, my), ImVec2(sp.x + m, my + m), mark);
    ImGui::SetCursorScreenPos(ImVec2(sp.x + ui::px(14), sp.y));
    if (ollama_online_ && !settings_.chat_model.empty() && !models_.empty() &&
        !model_installed(settings_.chat_model))
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.error),
                           "%s is not installed — pick a model above",
                           settings_.chat_model.c_str());
    else if (ollama_online_)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "Ollama %s  ·  %d models",
                           ollama_version_.c_str(), (int)models_.size());
    else
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.error), "Ollama offline  ·  %s",
                           models_error_.empty() ? "not reachable" : models_error_.c_str());
    ImGui::PopFont();

    ui::vspace(4);

    // --- message list ---
    // Reserve room for the context chip, the input box and the send button —
    // plus the notice under them when the AI cannot reach the project, which
    // carries a button to put that right.
    const bool blocked_notice =
        !tools_available() && (workspace_.empty() || !settings_.enable_file_tools);
    const float input_h = ui::px(152) + (blocked_notice ? ui::px(32) : 0.0f);
    ImGui::BeginChild("##messages", ImVec2(0, -input_h), ImGuiChildFlags_None);
    if (chat_.empty()) {
        ui::vspace(20);
        ImGui::PushFont(ui::F.ui);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::C.text_dim));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(
            "Ask anything about the code you have open. The current file (or your selection) "
            "is sent along automatically.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ui::vspace(10);

        struct Starter { const char* label; const char* prompt; };
        const Starter starters[] = {
            {"Explain this file", "Explain what this file does and how it is structured."},
            {"Find bugs", "Review this code for bugs and edge cases."},
            {"Write tests", "Write focused unit tests for this code."},
            {"Add comments", "Add clear comments and doc-comments to this code."},
        };
        for (const Starter& s : starters) {
            if (ui::ghost_button(s.label, ImVec2(-1, ImGui::GetFrameHeight() + ui::px(2)))) {
                send_chat(s.prompt, build_file_context(), std::string());
            }
            ui::vspace(2);
        }
    }
    for (int i = 0; i < (int)chat_.size(); ++i) draw_message(chat_[i], i);

    if (chat_scroll_to_bottom_ ||
        (chat_stream_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ui::px(60))) {
        ImGui::SetScrollHereY(1.0f);
        chat_scroll_to_bottom_ = false;
    }
    ImGui::EndChild();

    // --- context chips ---
    Document* doc = active_document();
    ImGui::PushFont(ui::F.ui_small);
    if (doc) {
        bool has_sel = doc->editor.has_selection();
        char label[160];
        std::snprintf(label, sizeof(label), "%s%s", doc->display_name.c_str(),
                      has_sel ? "  (selection)" : "");
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFontSize() + ui::px(8);
        float w = ImGui::CalcTextSize(label).x + ui::px(30);
        cdl->AddRect(p, ImVec2(p.x + w, p.y + h), ui::C.border);
        ui::draw_icon(cdl, ui::Icon::File, ImVec2(p.x + ui::px(8), p.y + ui::px(4)),
                      h - ui::px(8), ui::C.text_dim);
        cdl->AddText(ImVec2(p.x + ui::px(24), p.y + ui::px(4)), ui::C.text, label);
        ImGui::Dummy(ImVec2(w, h));
        if (ImGui::IsItemHovered())
            ui::tooltip(settings_.attach_open_file
                            ? "This file is included with your next message"
                            : "File context is disabled in Settings");
    } else {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "No file open");
    }
    ImGui::PopFont();

    // Say plainly when the model cannot touch the project, since "it will not
    // create files" almost always comes down to one of these three reasons.
    if (!tools_available()) {
        const char* why = nullptr;
        if (workspace_.empty()) why = "No folder open — the AI has nowhere to write files";
        else if (!settings_.enable_file_tools) why = "File editing is turned off in Settings";
        else why = "This model cannot call tools — the IDE will write its code blocks instead";

        ImGui::PushFont(ui::F.ui_small);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.warning), "%s", why);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();

        // The one that can be put right from here, in one click, is worth a
        // button: it is the difference between the model working on the project
        // and the model describing it.
        if (workspace_.empty()) {
            ui::vspace(2);
            if (ui::ghost_button("Open folder…", ImVec2(ui::px(120), 0))) {
                std::string folder = sys::pick_folder(hwnd_, "Open Folder");
                if (!folder.empty()) set_workspace(folder);
            }
        } else if (!settings_.enable_file_tools) {
            ui::vspace(2);
            if (ui::ghost_button("Turn it on", ImVec2(ui::px(120), 0))) {
                settings_.enable_file_tools = true;
                settings_.save();
                toast("The AI can create and edit files in this folder");
            }
        }
    }

    // --- input ---
    const bool busy = chat_stream_ != nullptr;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(ui::C.bg_surface));
    if (focus_chat_input_) {
        ImGui::SetKeyboardFocusHere();
        focus_chat_input_ = false;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                ImGuiInputTextFlags_CtrlEnterForNewLine;
    bool submit = input_text_multiline("##chatinput", &chat_input_,
                                       ImVec2(-1, ui::px(64)), flags);
    ImGui::PopStyleColor();
    if (chat_input_.empty() && !ImGui::IsItemActive()) {
        ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(p.x + ui::px(10), p.y + ui::px(7)), ui::C.text_faint,
            "Ask about your code…");
    }

    ui::vspace(2);
    float btn_w = ui::px(96);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - btn_w);
    if (busy) {
        if (ui::ghost_button("Stop", ImVec2(btn_w, 0))) stop_chat();
    } else {
        bool can_send = !trim(chat_input_).empty() && !settings_.chat_model.empty();
        if (ui::primary_button("Send", ImVec2(btn_w, 0), can_send) || submit) {
            std::string text = trim(chat_input_);
            if (!text.empty() && !settings_.chat_model.empty()) {
                chat_input_.clear();
                send_chat(text, build_file_context(), std::string());
            } else if (settings_.chat_model.empty()) {
                toast("Pick a model first", true);
            }
        }
    }

    ui::pane_frame();
    ImGui::End();
}

// ---------------------------------------------------------------- terminal

void App::run_command(const std::string& command) {
    if (trim(command).empty()) return;
    std::string cwd = workspace_;
    if (cwd.empty() && active_document() && !active_document()->path.empty())
        cwd = sys::parent_path(active_document()->path);

    process_.terminate();
    std::string err;
    terminal_output_ += "\n> " + command + "\n";
    if (!process_.start(command, cwd, err)) {
        terminal_output_ += "[error] " + err + "\n";
        toast("Could not run the command: " + err, true);
    }
    terminal_scroll_ = true;
    settings_.run_command = command;
}

void App::update_process() {
    // The server logs from its own threads; this is where those lines reach the
    // terminal panel, which is the only console this program has.
    for (const std::string& line : live_.take_log()) {
        terminal_output_ += "[live] " + line + "\n";
        terminal_scroll_ = true;
    }

    std::string chunk;
    if (process_.poll(chunk)) {
        terminal_output_ += strip_ansi(chunk);
        terminal_scroll_ = true;
        // keep the buffer bounded
        if (terminal_output_.size() > 400000)
            terminal_output_.erase(0, terminal_output_.size() - 300000);
    }
}

void App::draw_terminal_panel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(10), ui::px(8)));
    ImGui::Begin("Terminal###terminal");
    ImGui::PopStyleVar();
    ui::pane_title(4, "terminal", terminal_command_.c_str());

    const bool running = process_.running();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui::px(150));
    ImGui::PushFont(ui::F.mono_small);
    InputTextUserData ud{&terminal_command_};
    bool submit = ImGui::InputTextWithHint(
        "##cmd", "Command to run in the workspace folder…", (char*)terminal_command_.c_str(),
        terminal_command_.capacity() + 1,
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
        input_text_resize, &ud);
    ImGui::PopFont();

    ImGui::SameLine(0, ui::px(6));
    if (running) {
        if (ui::ghost_button("Stop", ImVec2(ui::px(70), 0))) {
            process_.terminate();
            terminal_output_ += "\n[stopped]\n";
            terminal_scroll_ = true;
        }
    } else {
        if (ui::primary_button("Run", ImVec2(ui::px(70), 0), !trim(terminal_command_).empty()) || submit)
            run_command(terminal_command_);
    }
    ImGui::SameLine(0, ui::px(4));
    if (ui::icon_button("##clearterm", ui::Icon::Trash, "Clear output")) terminal_output_.clear();

    ui::vspace(4);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(ui::C.code_bg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(10), ui::px(8)));
    ImGui::BeginChild("##termout", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding |
                                                     ImGuiChildFlags_Borders);
    ImGui::PushFont(ui::F.mono_small);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::C.text));
    ImGui::TextUnformatted(terminal_output_.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (terminal_scroll_) {
        ImGui::SetScrollHereY(1.0f);
        terminal_scroll_ = false;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ui::pane_frame();
    ImGui::End();
}

// ------------------------------------------------------------- live server

namespace {

bool ends_with_ci(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const char a = (char)std::tolower((unsigned char)s[s.size() - n + i]);
        if (a != suffix[i]) return false;
    }
    return true;
}

bool is_page(const std::string& path) {
    return ends_with_ci(path, ".html") || ends_with_ci(path, ".htm");
}

} // namespace

std::string App::live_root() const {
    if (!workspace_.empty()) return workspace_;
    // No folder open: serve the folder the open file lives in, which is what
    // "preview this page" has to mean when a single .html was opened.
    if (active_doc_ >= 0 && active_doc_ < (int)documents_.size()) {
        const Document* doc = documents_[active_doc_].get();
        if (doc && !doc->path.empty()) return sys::parent_path(doc->path);
    }
    return std::string();
}

bool App::start_live_server() {
    const std::string root = live_root();
    if (root.empty()) {
        toast("Open a folder (Ctrl+K) or save the file first — nothing to serve", true);
        return false;
    }

    live_.set_live_reload(settings_.live_reload);
    std::string err;
    if (!live_.start(root, settings_.live_port, err)) {
        toast(err, true);
        return false;
    }

    if (live_.port() != settings_.live_port) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "Port %d was busy — serving on %d", settings_.live_port,
                      live_.port());
        toast(msg);
    } else {
        toast("Live server on " + live_.origin());
    }
    return true;
}

void App::stop_live_server() {
    if (!live_.running()) return;
    live_.stop();
    toast("Live server stopped");
}

void App::open_live_preview() {
    if (!live_.running() && !start_live_server()) return;

    // A page opens at its own address; anything else (a stylesheet, a script,
    // a .cpp) opens the site root, which is what you actually want to look at.
    std::string url = live_.origin() + "/";
    Document* doc = active_document();
    if (doc && !doc->path.empty() && is_page(doc->path)) url = live_.url_for(doc->path);
    sys::open_url(url);
}

// ---------------------------------------------------------------------- AI

bool App::model_installed(const std::string& name) const {
    for (const ollama::Model& m : models_)
        if (m.name == name) return true;
    return false;
}

std::string App::best_installed_model() const {
    if (models_.empty()) return std::string();
    for (const ollama::Model& m : models_)
        if (m.name.find("coder") != std::string::npos || m.name.find("code") != std::string::npos)
            return m.name;
    return models_[0].name;
}

void App::refresh_models(bool notify) {
    if (models_loading_) return;
    models_loading_ = true;
    models_error_.clear();

    auto fetch = std::make_shared<ModelFetch>();
    g_fetch = fetch;
    ollama::Client client;
    client.set_host(settings_.ollama_host);

    std::thread([fetch, client, notify]() {
        std::string version, err;
        bool online = client.ping(version, err);
        std::vector<ollama::Model> models;
        std::string list_err;
        bool ok = online && client.list_models(models, list_err);

        std::lock_guard<std::mutex> lock(fetch->mutex);
        fetch->done = true;
        fetch->ok = ok;
        fetch->online = online;
        fetch->version = version;
        fetch->models = std::move(models);
        fetch->error = online ? list_err : err;
    }).detach();
    (void)notify;
}

// --------------------------------------------------------------- file tools

std::vector<ollama::ToolDef> App::file_tools() const {
    std::vector<ollama::ToolDef> tools;

    ollama::ToolDef read_file;
    read_file.name = "read_file";
    read_file.description =
        "Read a file from the project. Do this before editing anything, so you are working "
        "from the file's real current content rather than from memory.";
    read_file.params.push_back({"path", "File path relative to the project root", true});
    tools.push_back(read_file);

    ollama::ToolDef list_directory;
    list_directory.name = "list_directory";
    list_directory.description =
        "List the files and folders in a project directory. Use \".\" for the project root.";
    list_directory.params.push_back({"path",
        "Folder path relative to the project root, or \".\" for the root", true});
    tools.push_back(list_directory);

    ollama::ToolDef search_files;
    search_files.name = "search_files";
    search_files.description =
        "Search every text file in the project for a piece of text and get back the matching "
        "lines with their file and line number. Use this to find where something is defined "
        "or used before you change it.";
    search_files.params.push_back({"query", "Literal text to search for, e.g. a function name",
                                   true});
    tools.push_back(search_files);

    ollama::ToolDef edit_file;
    edit_file.name = "edit_file";
    edit_file.description =
        "Change part of an existing file by replacing an exact snippet of it. This is the "
        "preferred way to modify a file: it leaves the rest of the file untouched. "
        "'old_text' must match the current file content character for character, including "
        "indentation, and must appear exactly once - include a few surrounding lines to make "
        "it unique. Read the file first so you know what to match.";
    edit_file.params.push_back({"path", "File path relative to the project root", true});
    edit_file.params.push_back({"old_text",
        "The exact snippet to replace, copied verbatim from the file", true});
    edit_file.params.push_back({"new_text", "The replacement text", true});
    tools.push_back(edit_file);

    ollama::ToolDef create_file;
    create_file.name = "create_file";
    create_file.description =
        "Create a new file, or completely replace an existing one, inside the project. "
        "Missing parent folders are created automatically. Always write the complete file "
        "content, never a fragment or a diff. To change part of a file that already exists, "
        "use edit_file instead.";
    create_file.params.push_back({"path",
        "File path relative to the project root, e.g. src/utils/parser.cpp", true});
    create_file.params.push_back({"content", "The full text content of the file", true});
    tools.push_back(create_file);

    ollama::ToolDef create_directory;
    create_directory.name = "create_directory";
    create_directory.description =
        "Create a folder (including any missing parent folders) inside the project.";
    create_directory.params.push_back({"path",
        "Folder path relative to the project root, e.g. src/utils", true});
    tools.push_back(create_directory);

    return tools;
}

bool App::tools_available() const {
    if (!settings_.enable_file_tools || workspace_.empty()) return false;
    return std::find(models_without_tools_.begin(), models_without_tools_.end(),
                     settings_.chat_model) == models_without_tools_.end();
}

std::string App::effective_system_prompt(bool with_tools) const {
    std::string prompt = settings_.system_prompt;
    if (workspace_.empty()) return prompt;

    prompt += "\n\nThe user's open project folder is: " + workspace_;

    const std::string map = project_map();
    if (!map.empty())
        prompt += "\n\nThese are the files currently in the project:\n" + map;

    if (with_tools) {
        prompt +=
            "\n\nYou are not just describing changes - you make them. You can call:\n"
            "  read_file(path)                        see a file's real content\n"
            "  list_directory(path)                   see what is in a folder\n"
            "  search_files(query)                    find text across the project\n"
            "  edit_file(path, old_text, new_text)    change part of an existing file\n"
            "  create_file(path, content)             write a whole new file\n"
            "  create_directory(path)                 add a folder\n"
            "\nA fenced code block in your reply is not a file. Nothing on the user's disk "
            "changes when you print one, and they did not ask for something to copy and "
            "paste - they asked for the work to be done. Every file you produce goes through "
            "create_file or edit_file, with its complete content, and you never ask the user "
            "to create, paste or save anything themselves.\n"
            "\nHow to work:\n"
            "1. If the request touches code that already exists, read_file or search_files "
            "first. Never guess at a file's contents or edit from memory.\n"
            "2. Use edit_file for changes to an existing file, and create_file only for a "
            "genuinely new file or a full rewrite. For edit_file, old_text must be copied "
            "exactly from what read_file returned, and must occur exactly once.\n"
            "3. Make the whole change, one call at a time, then stop and tell the user what "
            "you did in a sentence or two.\n"
            "4. If a call comes back with an error, fix the problem and make the next call "
            "yourself. Do not stop and explain the error to the user - they cannot act on "
            "it, and the tools are yours to use.\n"
            "\nWhen the request is to build something new rather than to change existing "
            "code:\n"
            "- A website's entry point is index.html in the project root, with its stylesheet "
            "beside it. The preview server serves the project root, so a page hidden in a "
            "src/ or public/ folder is a site the user cannot open. Do not invent a build "
            "layout for a plain static site.\n"
            "- You cannot create images, fonts, audio or video: these tools write text, so a "
            ".jpg or .png you 'create' is a broken file. Draw with inline <svg>, CSS "
            "gradients or styled blocks instead, and only link media that already exists in "
            "the project.\n"
            "- Every link in a navigation must point at a page you actually create, and every "
            "stylesheet or script you reference must be a file you write. Finish the whole "
            "thing - pages, stylesheet, and the links between them - in this one turn.\n"
            "- Write real content, not lorem ipsum or TODO placeholders.\n"
            "- What you write has to run in the browser exactly as it is. Unless the project "
            "already builds its sources (a package.json with a bundler), do not write React, "
            "JSX or bare `import` statements into a file a <script> tag loads: nothing "
            "compiles them, the browser reports a syntax error and the page stays blank. "
            "Write plain HTML with real markup, CSS, and DOM JavaScript.\n"
            "- In CSS, a comment is /* … */. // is a syntax error there and costs you the "
            "rule behind it.\n"
            "\nPaths are always relative to the project root. The IDE performs each call and "
            "reports the real result back to you, including failures - read that result and "
            "react to it. Never say a file was created or changed unless a call actually "
            "succeeded.\n"
            "If you cannot emit a real function call, write it as a JSON object like "
            "{\"name\": \"create_file\", \"arguments\": {\"path\": \"src/app.cpp\", "
            "\"content\": \"...\"}} - the IDE understands that form too.";
    } else {
        prompt +=
            "\n\nYou write files by putting the target path on the fence of a code block, "
            "like this:\n```cpp path=src/utils/parser.cpp\n// the complete file content\n```\n"
            "The IDE writes such a block into the project for the user, so give every file "
            "you produce a path, and always write the whole file in the block - never a "
            "fragment, a diff or an outline. A block with no path is left for the user to "
            "place by hand, so do not leave one unnamed.";
    }
    return prompt;
}

// ------------------------------------------------------- project awareness

std::string App::project_map() const {
    if (workspace_.empty()) return std::string();
    if (!project_map_dirty_) return project_map_cache_;

    std::string out;
    int budget = 400; // keep the prompt small even in a large repository
    build_project_map(workspace_, "", 0, budget, out);
    if (budget <= 0) out += "... (listing truncated)\n";

    project_map_cache_ = out;
    project_map_dirty_ = false;
    return project_map_cache_;
}

void App::build_project_map(const std::string& dir, const std::string& prefix, int depth,
                            int& budget, std::string& out) const {
    if (depth > 4 || budget <= 0) return;
    std::vector<sys::DirEntry> entries;
    if (!sys::list_directory(dir, entries)) return;

    for (const sys::DirEntry& e : entries) {
        if (budget <= 0) return;
        --budget;
        out += prefix + e.name;
        if (e.is_dir) {
            out += "/\n";
            build_project_map(e.path, prefix + "  ", depth + 1, budget, out);
        } else {
            out += "\n";
        }
    }
}

void App::search_workspace(const std::string& query, std::string& out) const {
    int budget = 60; // matching lines
    search_dir(workspace_, query, budget, out);
    if (out.empty()) out = "No matches for \"" + query + "\".";
    else if (budget <= 0) out += "... (more matches not shown)\n";
}

void App::search_dir(const std::string& dir, const std::string& query, int& budget,
                     std::string& out) const {
    if (budget <= 0) return;
    std::vector<sys::DirEntry> entries;
    if (!sys::list_directory(dir, entries)) return;

    for (const sys::DirEntry& e : entries) {
        if (budget <= 0) return;
        if (e.is_dir) {
            search_dir(e.path, query, budget, out);
            continue;
        }
        // Skip anything too large to be hand-written source.
        if (e.size > 2 * 1024 * 1024) continue;

        std::string content, err;
        if (!sys::read_file(e.path, content, err)) continue;
        if (content.find('\0') != std::string::npos) continue; // binary

        std::string rel = e.path;
        if (rel.size() > workspace_.size() + 1 &&
            _strnicmp(rel.c_str(), workspace_.c_str(), workspace_.size()) == 0)
            rel = rel.substr(workspace_.size() + 1);

        int line_no = 1;
        size_t line_start = 0;
        while (line_start <= content.size() && budget > 0) {
            size_t line_end = content.find('\n', line_start);
            if (line_end == std::string::npos) line_end = content.size();
            const std::string line = content.substr(line_start, line_end - line_start);
            if (line.find(query) != std::string::npos) {
                std::string shown = trim(line);
                if (shown.size() > 200) shown = shown.substr(0, 200) + "...";
                out += rel + ":" + std::to_string(line_no) + ": " + shown + "\n";
                --budget;
            }
            if (line_end == content.size()) break;
            line_start = line_end + 1;
            ++line_no;
        }
    }
}

bool App::resolve_in_workspace(const std::string& requested, std::string& absolute,
                               std::string& err) const {
    if (workspace_.empty()) {
        err = "no project folder is open in the IDE";
        return false;
    }
    std::string rel = trim(requested);
    if (rel.empty()) {
        err = "empty path";
        return false;
    }
    if (rel == "." || rel == "./" || rel == ".\\") {
        absolute = sys::normalize(workspace_);
        err.clear();
        return true;
    }

    // "/styles.css" is how a page refers to the site root, and the site root is
    // the project folder. Taken literally it would be the root of the drive and
    // refused, so it is read the way the browser reads it.
    const bool unc = rel.size() > 1 && (rel[0] == '/' || rel[0] == '\\') &&
                     (rel[1] == '/' || rel[1] == '\\');
    if (!unc && (rel[0] == '/' || rel[0] == '\\')) {
        rel.erase(0, 1);
        if (rel.empty()) {
            absolute = sys::normalize(workspace_);
            err.clear();
            return true;
        }
    }

    const bool is_absolute = (rel.size() > 1 && rel[1] == ':') || rel[0] == '/' || rel[0] == '\\';
    const std::string joined = is_absolute ? rel : sys::join_path(workspace_, rel);
    const std::string full = sys::normalize(joined);
    const std::string root = sys::normalize(workspace_);

    // Must stay inside the workspace, and match on a path boundary so that a
    // sibling folder with the same prefix cannot slip through.
    if (_strnicmp(full.c_str(), root.c_str(), root.size()) != 0 ||
        (full.size() > root.size() && full[root.size()] != '\\' && full[root.size()] != '/')) {
        err = "path is outside the open project folder";
        return false;
    }
    absolute = full;
    err.clear();
    return true;
}

// After the agent writes a file, show the result: open it, or refresh the tab
// if it is already open and has no unsaved edits of its own.
void App::sync_open_document(const std::string& absolute, const std::string& content) {
    const int existing = find_document(absolute);
    if (existing < 0) {
        open_file(absolute);
        return;
    }
    Document* doc = documents_[existing].get();
    if (doc->editor.modified()) {
        toast(doc->display_name + " changed on disk - your unsaved edits were kept", true);
    } else {
        doc->editor.set_text(content);
        doc->editor.mark_saved();
    }
    active_doc_ = existing;
    doc->scroll_to_tab = true;
}

// Windows hands the same file back under different capitalisation depending on
// who asked - the tree, a tool call, the tab bar - so the set of files the
// model has seen is keyed on a folded copy of the path.
static std::string fold_path(const std::string& path) {
    std::string folded;
    folded.reserve(path.size());
    for (char c : path) folded += (char)std::tolower((unsigned char)(c == '/' ? '\\' : c));
    return folded;
}

void App::mark_read(const std::string& absolute) { read_paths_.insert(fold_path(absolute)); }

bool App::was_read(const std::string& absolute) const {
    return read_paths_.count(fold_path(absolute)) > 0;
}

void App::execute_action(ToolAction& action) {
    action.resolved = true;
    action.ok = false;

    std::string absolute, err;
    if (!resolve_in_workspace(action.path, absolute, err)) {
        action.result = "Error: " + err;
        return;
    }

    if (action.tool == "create_file") {
        // These tools write text. Letting a model "create" a .jpg produces a
        // file that is broken everywhere it is used, so it is refused outright
        // and the model is told what to do instead.
        if (const char* kind = binary_asset_kind(action.path)) {
            action.result =
                "Error: " + action.path + " is a " + kind +
                " file, and these tools write text - this would create a broken file, not a "
                "real " + kind + ". Never create binary assets. Use an inline <svg>, a CSS "
                "gradient or a styled block instead, and only reference image, font or media "
                "files that are already in the project.";
            return;
        }

        // Refuse to replace a file the model has not looked at in this turn.
        // Left unchecked, a small model "adds a function" by rewriting the file
        // with only that function, silently deleting everything else.
        if (sys::file_exists(absolute) && !was_read(absolute)) {
            std::string existing, ignored;
            const int lines = sys::read_file(absolute, existing, ignored)
                                  ? 1 + (int)std::count(existing.begin(), existing.end(), '\n')
                                  : 0;
            action.result =
                "Error: " + action.path + " already exists (" + std::to_string(lines) +
                " lines), and replacing it would delete what is in it. Do not answer the "
                "user yet. Call read_file(\"" + action.path + "\") now, then call edit_file "
                "to make the change. Use create_file on this path only after reading it and "
                "only if you really mean to replace the whole file.";
            return;
        }
        std::string parent = sys::parent_path(absolute);
        if (!parent.empty() && !sys::is_directory(parent) &&
            !sys::create_directories(parent, err)) {
            action.result = "Error: could not create the folder for " + action.path + ": " + err;
            return;
        }
        if (!sys::write_file(absolute, action.content, err)) {
            action.result = "Error: could not write " + action.path + ": " + err;
            return;
        }
        action.ok = true;
        action.result = (action.overwrite ? "Replaced " : "Created ") + action.path + " (" +
                        std::to_string(action.content.size()) + " bytes)";
        {
            std::string note = missing_reference_note(absolute, action.content);
            note += runtime_problem_note(absolute, action.content);
            action.result += note;
            action.warned = !note.empty();
            if (is_page(action.path) &&
                std::find(pages_written_.begin(), pages_written_.end(), absolute) ==
                    pages_written_.end())
                pages_written_.push_back(absolute);
        }
        refresh_tree();
        sync_open_document(absolute, action.content);
        return;
    }

    if (action.tool == "edit_file") {
        std::string content;
        if (!sys::read_file(absolute, content, err)) {
            action.result = "Error: could not read " + action.path + ": " + err +
                            ". The file may not exist yet - use create_file to make it.";
            return;
        }
        if (action.find_text.empty()) {
            action.result = "Error: old_text was empty. Give the exact snippet to replace.";
            return;
        }

        size_t at = 0, stop = 0;
        const agent::Match match =
            agent::find_editable_region(content, action.find_text, at, stop);
        if (match == agent::Match::None) {
            action.result = "Error: old_text was not found in " + action.path + ".";
            // Telling a small model to go and read the file sends it round the
            // same loop: it reads, then guesses the snippet wrong again. For a
            // file small enough to quote, hand it the real content right here.
            if (content.size() <= 6000) {
                mark_read(absolute);
                action.result += " Here is what the file actually contains:\n" + content +
                                 "\nEither copy old_text from this exactly, or call "
                                 "create_file on this path with the whole corrected file.";
            } else {
                action.result += " Call read_file(\"" + action.path +
                                 "\") and copy the snippet from the result, then try again.";
            }
            return;
        }
        if (match == agent::Match::Ambiguous) {
            action.result =
                "Error: old_text matches more than one place in " + action.path +
                ". Include more surrounding lines so it identifies exactly one place.";
            return;
        }

        const int line_no =
            1 + (int)std::count(content.begin(), content.begin() + (ptrdiff_t)at, '\n');
        content.replace(at, stop - at, action.replace_text);
        if (!sys::write_file(absolute, content, err)) {
            action.result = "Error: could not write " + action.path + ": " + err;
            return;
        }
        action.ok = true;
        action.replacements = 1;
        action.result = "Edited " + action.path + " at line " + std::to_string(line_no) + ".";
        {
            std::string note = missing_reference_note(absolute, content);
            note += runtime_problem_note(absolute, content);
            action.result += note;
            action.warned = !note.empty();
            if (is_page(action.path) &&
                std::find(pages_written_.begin(), pages_written_.end(), absolute) ==
                    pages_written_.end())
                pages_written_.push_back(absolute);
        }
        refresh_tree();
        sync_open_document(absolute, content);
        return;
    }

    if (action.tool == "search_files") {
        if (action.query.empty()) {
            action.result = "Error: query was empty.";
            return;
        }
        std::string hits;
        search_workspace(action.query, hits);
        if ((int)hits.size() > settings_.max_context_bytes)
            hits = hits.substr(0, (size_t)settings_.max_context_bytes) + "\n... (truncated)";
        action.ok = true;
        action.result = hits;
        return;
    }

    if (action.tool == "create_directory") {
        if (!sys::create_directories(absolute, err)) {
            action.result = "Error: could not create " + action.path + ": " + err;
            return;
        }
        action.ok = true;
        action.result = "Created folder " + action.path;
        refresh_tree();
        return;
    }

    if (action.tool == "read_file") {
        std::string content;
        if (!sys::read_file(absolute, content, err)) {
            action.result = "Error: could not read " + action.path + ": " + err;
            return;
        }
        const int lines = 1 + (int)std::count(content.begin(), content.end(), '\n');
        bool truncated = false;
        if ((int)content.size() > settings_.max_context_bytes) {
            content = content.substr(0, (size_t)settings_.max_context_bytes);
            truncated = true;
        }
        action.ok = true;
        mark_read(absolute);
        action.result = action.path + " (" + std::to_string(lines) + " lines):\n" + content;
        if (truncated)
            action.result +=
                "\n… (truncated — only the start of the file is shown; use search_files "
                "to locate what you need further down)";
        return;
    }

    if (action.tool == "list_directory") {
        std::vector<sys::DirEntry> entries;
        if (!sys::list_directory(absolute, entries)) {
            action.result = "Error: could not list " + action.path;
            return;
        }
        std::string listing;
        for (const sys::DirEntry& e : entries)
            listing += (e.is_dir ? "[dir]  " : "       ") + e.name + "\n";
        if (listing.empty()) listing = "(empty)\n";
        action.ok = true;
        action.result = "Contents of " + action.path + ":\n" + listing;
        return;
    }

    action.result = "Error: unknown tool '" + action.tool + "'";
}

void App::refresh_reference_warnings(ChatMessage& msg) {
    // Models write a whole site in one batch, so a page routinely names a file
    // that only exists two calls later. Judging each page once the batch is
    // done is the difference between a real warning and noise.
    static const char* kMarker = " Warning: this page points at ";
    for (ToolAction& action : msg.actions) {
        if (!action.ok) continue;
        if (action.tool != "create_file" && action.tool != "edit_file") continue;
        if (!is_page(action.path) && !ends_with_ci(action.path, ".css") &&
            !ends_with_ci(action.path, ".js"))
            continue;

        std::string absolute, err;
        if (!resolve_in_workspace(action.path, absolute, err)) continue;
        std::string html;
        if (!sys::read_file(absolute, html, err)) continue;

        const size_t at = action.result.find(kMarker);
        const std::string base = at == std::string::npos ? action.result
                                                         : action.result.substr(0, at);
        std::string note = missing_reference_note(absolute, html);
        note += runtime_problem_note(absolute, html);
        action.result = base + note;
        action.warned = !note.empty();
    }
}

bool App::project_has_bundler() const {
    if (workspace_.empty()) return false;
    const char* kSigns[] = {"package.json",     "vite.config.js",  "vite.config.ts",
                            "webpack.config.js", "rollup.config.js", "next.config.js",
                            "node_modules"};
    for (const char* sign : kSigns) {
        const std::string p = sys::join_path(workspace_, sign);
        if (sys::file_exists(p) || sys::is_directory(p)) return true;
    }
    return false;
}

std::string App::runtime_problem_note(const std::string& absolute,
                                      const std::string& content) const {
    std::string lower_path;
    for (char c : absolute) lower_path += (char)std::tolower((unsigned char)c);
    const bool is_css = lower_path.size() > 4 && lower_path.compare(lower_path.size() - 4, 4, ".css") == 0;
    const bool is_js = lower_path.size() > 3 && lower_path.compare(lower_path.size() - 3, 3, ".js") == 0;

    if (is_css && css_has_slash_comment(content))
        return " Warning: this stylesheet uses // as a comment. CSS has no such comment - the "
               "browser discards the rule that follows it. Use /* … */ instead.";

    const bool builds = project_has_bundler();

    if (is_js && !builds) {
        std::string why;
        if (script_needs_a_build_step(content, why))
            return " Warning: this file cannot run in a browser as it is, because " + why +
                   ". There is no bundler in this project, so nothing compiles it. Write plain "
                   "DOM JavaScript instead, or load the framework from a CDN and use "
                   "React.createElement rather than JSX.";
        return std::string();
    }

    if (!is_page(absolute)) return std::string();

    // A page: judge it by the scripts it actually loads.
    std::vector<ScriptRef> scripts;
    collect_scripts(content, scripts);
    const std::string dir = sys::parent_path(absolute);

    bool renders = false;
    for (const ScriptRef& script : scripts) {
        if (script.src.find("://") != std::string::npos || script.src.compare(0, 2, "//") == 0)
            continue;
        const std::string target = script.src[0] == '/'
                                       ? sys::join_path(workspace_, script.src.substr(1))
                                       : sys::join_path(dir, script.src);
        std::string js, err;
        if (!sys::read_file(target, js, err)) continue;

        if (js.find("createRoot") != std::string::npos ||
            js.find("ReactDOM.render") != std::string::npos ||
            js.find("document.body") != std::string::npos ||
            js.find("getElementById") != std::string::npos)
            renders = true;

        if (!builds) {
            std::string why;
            if (script_needs_a_build_step(js, why))
                return " Warning: this page loads " + script.src + ", which cannot run in a "
                       "browser because " + why + ". There is no bundler in this project, so "
                       "nothing compiles it and the page stays blank. Rewrite the script as "
                       "plain DOM JavaScript, or put the markup straight into the HTML.";
            if (!script.is_module && js.find("import ") != std::string::npos)
                return " Warning: this page loads " + script.src + " with a plain <script> "
                       "tag, but that file uses import statements, which a classic script "
                       "cannot do - the browser reports a syntax error and nothing runs. Add "
                       "type=\"module\" to the tag, or drop the imports.";
        }
    }

    // An empty mount point with nothing to fill it is the classic blank page.
    const bool has_root = content.find("id=\"root\"") != std::string::npos ||
                          content.find("id=\'root\'") != std::string::npos;
    if (has_root && !renders) {
        // Only complain when the root really is the whole body: a page with real
        // markup beside it is not blank.
        const size_t body = content.find("<body");
        const std::string rest = body == std::string::npos ? content : content.substr(body);
        const bool has_other_markup = rest.find("<h1") != std::string::npos ||
                                      rest.find("<p") != std::string::npos ||
                                      rest.find("<main") != std::string::npos ||
                                      rest.find("<header") != std::string::npos ||
                                      rest.find("<section") != std::string::npos;
        if (!has_other_markup)
            return " Warning: the body of this page is an empty <div id=\"root\"> and nothing "
                   "in it ever renders anything into that element, so the browser shows a "
                   "blank page. Put the real markup in the HTML, or have a script fill the "
                   "element.";
    }
    return std::string();
}

std::string App::dead_reference_report() const {
    std::string report;
    for (const std::string& page : pages_written_) {
        std::string html, err;
        if (!sys::read_file(page, html, err)) continue;
        const std::string note = missing_reference_note(page, html);
        if (note.empty()) continue;

        // missing_reference_note is written for the model mid-turn; here only
        // the file names matter.
        const size_t start = note.find("points at ");
        const size_t stop = note.find(", which");
        if (start == std::string::npos || stop == std::string::npos || stop <= start) continue;
        report += "- " + display_path(page, workspace_, 60) + " links to " +
                  note.substr(start + 10, stop - start - 10) + "\n";
    }
    return report;
}

void App::maybe_repair_broken_pages() {
    if (chat_stream_ || pages_written_.empty()) return;
    if (!tools_used_this_turn_ || repair_attempts_ >= 1) {
        pages_written_.clear();
        return;
    }

    const std::string report = dead_reference_report();
    pages_written_.clear();
    if (report.empty()) return;

    // The model has just been told about the dead links and answered the user
    // instead of fixing them - which is what small models do. One more turn,
    // with the links named and the escape route spelled out, usually lands it.
    ++repair_attempts_;

    ollama::Message nudge;
    nudge.role = "user";
    nudge.content =
        "The site you just wrote is broken - these references point at files that do not "
        "exist:\n" + report +
        "\nFix this yourself, now, with tool calls. For each page: call read_file on it, "
        "then call create_file on the same path with the whole corrected page - that is "
        "easier to get right than an exact-snippet edit. If a path is simply wrong, correct "
        "it; if it points at an image or another binary file, replace that reference with an "
        "inline <svg> or a styled block, because you cannot create binary files. Do not "
        "print the files back to me and do not ask me to add anything.";
    agent_history_.push_back(std::move(nudge));

    ChatMessage next;
    next.role = "assistant";
    next.streaming = true;
    // Labelled, so the extra reply is visibly the IDE's doing rather than an
    // answer appearing out of nowhere.
    next.attachment_note = "automatic repair";
    chat_.push_back(next);
    start_agent_turn();
    chat_scroll_to_bottom_ = true;
    toast("Repairing broken links in the generated pages");
}

// Verbs that mean "change the project". A reply full of code is only worth a
// second attempt when this is what was asked for: an explanation is allowed to
// contain a code block, and rewriting the file because of one would be wrong.
bool App::asks_for_changes(const std::string& text) {
    std::string lower;
    for (char c : text) lower += (char)std::tolower((unsigned char)c);

    static const char* kVerbs[] = {
        "creat", "build", "make ", "write", "add ",  "implement", "generat", "set up",
        "fix",   "chang", "updat", "modif", "refactor", "rename", "replace", "remove",
        "delete", "convert", "migrat", "port ", "extend", "extract", "rewrite", "apply",
        "install", "scaffold", "turn ", "give me", "i want", "i need", "can you",
        // The IDE is used in German as often as in English on this machine.
        "erstell", "schreib", "bau ", "baue", "mach", "füge", "fuege", "ändere", "aendere",
        "änder", "aender", "lösch", "loesch", "korrigier", "repariere", "erzeug",
    };
    for (const char* verb : kVerbs)
        if (lower.find(verb) != std::string::npos) return true;
    return false;
}

bool App::nudge_towards_tools(ChatMessage& msg) {
    if (chat_stream_ || !tools_used_this_turn_ || !turn_wants_files_) return false;
    if (block_nudges_ >= 1 || agent_steps_ > kMaxAgentSteps) return false;

    // Only when the reply really is a delivery: code the user is expected to
    // put somewhere, not a two-line illustration inside an explanation.
    std::vector<agent::FileBlock> blocks;
    agent::collect_file_blocks(msg.content, blocks);
    bool substantial = false;
    for (const agent::FileBlock& block : blocks) {
        const size_t lines = 1 + (size_t)std::count(block.content.begin(), block.content.end(), '\n');
        if (lines >= 6) substantial = true;
    }
    if (!substantial) return false;

    ++block_nudges_;

    ollama::Message nudge;
    nudge.role = "user";
    nudge.content =
        "Stop. You printed that code in the chat instead of writing it, so nothing has "
        "changed on disk and I cannot use any of it. A fenced code block is not a file. "
        "Do it again as tool calls: create_file(path, content) for each new file with its "
        "complete content, or edit_file(path, old_text, new_text) to change a file that "
        "already exists - reading it first. Make the calls now, and do not repeat the code "
        "in your reply.";
    agent_history_.push_back(std::move(nudge));

    ChatMessage next;
    next.role = "assistant";
    next.streaming = true;
    next.attachment_note = "asked again for real file writes";
    chat_.push_back(next);
    start_agent_turn();
    chat_scroll_to_bottom_ = true;
    return true;
}

void App::collect_tool_calls(ChatMessage& msg, const std::vector<ollama::ToolCall>& calls) {
    for (const ollama::ToolCall& call : calls) {
        ToolAction action;
        action.tool = call.name;
        js::Value args = js::parse(call.arguments);
        action.path = args["path"].as_string();
        action.content = args["content"].as_string();
        action.find_text = args["old_text"].as_string();
        action.replace_text = args["new_text"].as_string();
        action.query = args["query"].as_string();
        action.read_only = (call.name == "read_file" || call.name == "list_directory" ||
                            call.name == "search_files");
        prepare_write_action(action);
        msg.actions.push_back(std::move(action));
    }

    apply_ready_actions(msg);
}

void App::prepare_write_action(ToolAction& action) {
    if (action.read_only) return;

    // An edit always targets a file that already exists.
    if (action.tool == "edit_file") action.overwrite = true;

    std::string absolute, err;
    if (!resolve_in_workspace(action.path, absolute, err)) return;
    if (action.tool == "create_file") action.overwrite = sys::file_exists(absolute);
    // Writing over a buffer the user has been editing would strand their work,
    // so that case always asks first.
    const int open_doc = find_document(absolute);
    action.touches_unsaved = open_doc >= 0 && documents_[open_doc]->editor.modified();
}

void App::apply_ready_actions(ChatMessage& msg) {
    // Reads are harmless and run straight away. Among the writes, only the two
    // that can destroy work outright still ask every time: replacing a whole
    // file that already exists, and any write over a file with unsaved edits.
    // A new file or a targeted edit_file goes through, which is what makes the
    // agent usable rather than a click-through gauntlet.
    for (ToolAction& action : msg.actions) {
        if (action.resolved) continue;
        const bool destructive = (action.tool == "create_file" && action.overwrite) ||
                                 action.touches_unsaved;
        const bool auto_ok = action.read_only ||
                             (settings_.auto_approve_writes && !destructive);
        if (auto_ok) execute_action(action);
    }

    refresh_reference_warnings(msg);
}

bool App::adopt_code_blocks(ChatMessage& msg, std::string& text, bool tools_turn) {
    // Without a folder there is nowhere to write, and with file tools switched
    // off the user has said not to.
    if (workspace_.empty() || !settings_.enable_file_tools) return false;

    std::vector<agent::FileBlock> blocks;
    if (agent::collect_file_blocks(text, blocks) == 0) return false;

    // A page names the files it loads, so an unnamed stylesheet or script in
    // the same reply usually does have a name - on the <link> or <script> tag
    // of the page written beside it.
    std::vector<std::string> refs;
    for (const agent::FileBlock& block : blocks) {
        const bool page = lang::from_tag(block.lang) == lang::Id::Html ||
                          (!block.path.empty() && lang::from_path(block.path) == lang::Id::Html);
        if (page) collect_local_refs(block.content, refs);
    }
    for (const agent::FileBlock& block : blocks) {
        if (block.path.empty()) continue;
        refs.erase(std::remove(refs.begin(), refs.end(), block.path), refs.end());
    }
    for (agent::FileBlock& block : blocks) {
        if (!block.path.empty()) continue;
        const lang::Id id = lang::from_tag(block.lang);
        if (id == lang::Id::PlainText) continue;

        std::string only;
        int matches = 0;
        for (const std::string& ref : refs)
            if (lang::from_path(ref) == id) {
                ++matches;
                only = ref;
            }
        // One candidate is a deduction; two would be a guess about which is
        // which, and a stylesheet written to the wrong name is worse than one
        // the user places themselves.
        if (matches != 1) continue;
        block.path = only;
        refs.erase(std::remove(refs.begin(), refs.end(), only), refs.end());
    }

    std::vector<agent::Splice> edits;
    const size_t before = msg.actions.size();

    for (const agent::FileBlock& block : blocks) {
        if (block.path.empty() || trim(block.content).empty()) continue;
        // These tools write text. A block "for" a binary asset would produce a
        // file that is broken everywhere it is used, so it is left alone here
        // exactly as it would be refused as a call.
        if (binary_asset_kind(block.path)) continue;

        std::string absolute, err;
        if (!resolve_in_workspace(block.path, absolute, err)) continue;

        const bool exists = sys::file_exists(absolute);
        // Replacing a whole file the model has not read is how a small model
        // deletes the other 200 lines of it. Through a tool call that comes
        // back as an error the model can act on; here there is no call to
        // answer, so unless this turn can still make one, the block is labelled
        // and left for the user to apply with one click instead.
        const bool blind = exists && !was_read(absolute);
        if ((exists && !block.named) || (blind && !tools_turn)) {
            if (!block.fenced_path)
                edits.push_back({block.info_end, block.info_end, " path=" + block.path});
            continue;
        }

        ToolAction action;
        action.tool = "create_file";
        action.path = block.path;
        action.content = block.content;
        if (!action.content.empty() && action.content.back() != '\n') action.content += '\n';
        prepare_write_action(action);
        msg.actions.push_back(std::move(action));
        // The block itself is lifted out of the reply: what happened to it is
        // now the action card's job, and the content lives there.
        edits.push_back({block.begin, block.end, std::string()});
    }

    if (edits.empty()) return false;
    text = trim(agent::splice(text, edits));
    if (msg.actions.size() == before) return false; // labelled only

    apply_ready_actions(msg);
    return true;
}

void App::maybe_continue_agent() {
    if (chat_stream_ || chat_.empty()) return;
    ChatMessage& msg = chat_.back();
    if (msg.streaming) return;
    // The turn is over: no calls to report back. Before letting it rest, check
    // that the model has not simply printed what it was asked to write, and
    // that what was written actually holds together.
    if (msg.actions.empty()) {
        if (!nudge_towards_tools(msg)) maybe_repair_broken_pages();
        return;
    }
    if (msg.actions_reported) return;
    for (const ToolAction& action : msg.actions)
        if (!action.resolved) return; // still waiting for the user

    msg.actions_reported = true;
    const char* declined_note =
        "The user declined this action. Do not retry it; ask what they would prefer instead.";

    // Results always go back through the tool protocol, even when the calls had
    // to be recovered from the reply text. Restating the same information as a
    // user message measurably degrades a local model: after a failed call it
    // apologises instead of retrying, and after a read it describes the edit it
    // was about to make and calls that done.
    for (const ToolAction& action : msg.actions) {
        ollama::Message result;
        result.role = "tool";
        result.tool_name = action.tool;
        result.content = action.rejected ? declined_note : action.result;
        agent_history_.push_back(std::move(result));
    }

    // The model has no tools to call, so the writes came out of its code
    // blocks. The results are on the record for the next thing the user asks;
    // handing the turn back to a model that cannot act on them is not useful.
    if (!msg.expect_followup) return;

    // A real change is often read → edit → read → edit across a few files, so
    // the budget has to allow a genuine multi-step turn without running away.
    if (++agent_steps_ > kMaxAgentSteps) {
        toast("Stopped after " + std::to_string(kMaxAgentSteps) +
                  " tool steps — ask again to continue",
              true);
        return;
    }

    ChatMessage next;
    next.role = "assistant";
    next.streaming = true;
    chat_.push_back(next);
    start_agent_turn();
    chat_scroll_to_bottom_ = true;
}

size_t App::agent_history_bytes() const {
    size_t total = 0;
    for (const ollama::Message& m : agent_history_) {
        total += m.content.size() + m.tool_name.size() + 24; // 24 ≈ the role wrapper
        for (const ollama::ToolCall& call : m.tool_calls)
            total += call.name.size() + call.arguments.size() + 24;
    }
    return total;
}

void App::compact_agent_history() {
    if (agent_history_.size() < 6) return;

    // Ollama drops the oldest messages when a prompt outgrows num_ctx, and it
    // does so silently: the turn keeps going, minus the instructions and the
    // results it was working from. Trimming here means we choose what to lose.
    // Roughly three characters per token for code and markup, less the room the
    // reply itself needs.
    const int ctx = settings_.num_ctx > 0 ? settings_.num_ctx : 8192;
    const size_t budget = (size_t)std::max(2000, ctx - 1200) * 3;
    if (agent_history_bytes() <= budget) return;

    // The system prompt and the last few exchanges stay whole; everything older
    // is summarised, oldest first, until the transcript fits.
    const size_t keep_recent = 6;
    int trimmed = 0;
    for (size_t i = 1; i + keep_recent < agent_history_.size(); ++i) {
        if (agent_history_bytes() <= budget) break;
        ollama::Message& m = agent_history_[i];

        if (m.role == "tool" && m.content.size() > 240) {
            m.content = m.content.substr(0, 200) +
                        "\n… (older tool result trimmed to fit the context window)";
            ++trimmed;
        }

        // The file body inside an older create_file is the largest single thing
        // in the transcript and the least useful to keep: the result line right
        // after it already records that the file was written.
        for (ollama::ToolCall& call : m.tool_calls) {
            if (call.arguments.size() <= 240) continue;
            js::Value args = js::parse(call.arguments);
            if (!args.is_object()) continue;
            js::Value slim = js::Value::object();
            bool shrank = false;
            for (const char* key : {"content", "new_text", "old_text"}) {
                if (args[key].is_string() && args[key].as_string().size() > 160) {
                    slim.set(key, js::Value("… (sent earlier, omitted to fit the context)"));
                    shrank = true;
                } else if (args[key].is_string()) {
                    slim.set(key, js::Value(args[key].as_string()));
                }
            }
            if (!shrank) continue;
            if (args["path"].is_string()) slim.set("path", js::Value(args["path"].as_string()));
            if (args["query"].is_string()) slim.set("query", js::Value(args["query"].as_string()));
            call.arguments = slim.dump();
            ++trimmed;
        }
    }

    if (trimmed > 0) {
        char note[96];
        std::snprintf(note, sizeof(note), "[live] trimmed %d older entries to fit the context",
                      trimmed);
        // The terminal is the only console this program has, and knowing the
        // transcript was compacted explains an answer that lost the thread.
        terminal_output_ += std::string(note) + "\n";
        terminal_scroll_ = true;
    }
}

void App::start_agent_turn() {
    compact_agent_history();

    ollama::Options opts;
    // Whether this turn can call tools is decided just below, but the sampling
    // has to be chosen first; tools_available() is what start_agent_turn is
    // about to use anyway.
    opts.temperature = tools_available() ? settings_.agent_temperature : settings_.temperature;
    opts.top_p = settings_.top_p;
    opts.num_ctx = settings_.num_ctx;

    tools_used_this_turn_ = tools_available();
    if (!agent_history_.empty() && agent_history_[0].role == "system")
        agent_history_[0].content = effective_system_prompt(tools_used_this_turn_);

    chat_stream_ = client_.chat(settings_.chat_model, agent_history_, opts,
                                tools_used_this_turn_ ? file_tools()
                                                      : std::vector<ollama::ToolDef>());
}

void App::find_by_name_in(const std::string& dir, const std::string& lower_name, int depth,
                          int& budget, std::string& found) const {
    if (budget <= 0 || depth > 6) return;
    std::vector<sys::DirEntry> entries;
    if (!sys::list_directory(dir, entries)) return;
    for (const sys::DirEntry& e : entries) {
        if (--budget <= 0) return;
        if (e.is_dir) {
            find_by_name_in(e.path, lower_name, depth + 1, budget, found);
            continue;
        }
        std::string lower;
        for (char c : e.name) lower += (char)std::tolower((unsigned char)c);
        if (lower != lower_name) continue;
        if (found.empty()) {
            found = e.path;
        } else if (found != e.path) {
            // A second file with the same name: which one was meant is now a
            // guess, and a confident wrong answer is worse than none.
            found = "?";
            budget = 0;
            return;
        }
    }
}

bool App::find_by_name(const std::string& file_name, std::string& absolute) const {
    if (workspace_.empty() || file_name.empty()) return false;
    std::string lower;
    for (char c : file_name) lower += (char)std::tolower((unsigned char)c);
    int budget = 4000;
    std::string found;
    find_by_name_in(workspace_, lower, 0, budget, found);
    if (found.empty() || found == "?") return false;
    absolute = found;
    return true;
}

std::string App::missing_reference_note(const std::string& absolute_html,
                                        const std::string& html) const {
    std::vector<std::string> refs;
    collect_local_refs(html, refs);
    if (refs.empty()) return std::string();

    const std::string dir = sys::parent_path(absolute_html);
    std::vector<std::string> missing;
    for (const std::string& ref : refs) {
        // A leading slash means the site root, which is the project folder the
        // preview serves; everything else is relative to the page itself.
        std::string candidate = ref[0] == '/' ? sys::join_path(workspace_, ref.substr(1))
                                              : sys::join_path(dir, ref);
        if (sys::file_exists(candidate) || sys::is_directory(candidate)) continue;
        if (missing.size() < 8) missing.push_back(ref);
    }
    if (missing.empty()) return std::string();

    std::string note = " Warning: this page points at ";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i) note += ", ";
        note += missing[i];
    }
    note += missing.size() == 1 ? ", which does not exist. " : ", which do not exist. ";

    // A wrong number of "../" is the usual cause, and it is something that can
    // be worked out rather than guessed at: if a file with that name is in the
    // project, say how to write the path from this page.
    std::string corrections;
    for (const std::string& ref : missing) {
        const std::string name = sys::file_name(ref);
        std::string elsewhere;
        if (name.empty() || !find_by_name(name, elsewhere)) continue;
        const std::string fixed = sys::relative_path(dir, elsewhere);
        if (fixed.empty() || fixed == ref) continue;
        if (corrections.empty()) corrections = "The file is in the project already: write ";
        else corrections += ", ";
        corrections += fixed + " instead of " + ref;
    }
    if (!corrections.empty()) {
        note += corrections +
                ". Fix the path with edit_file. ";
    }
    note += "Create the missing files now, or correct the paths - do not tell the user the "
            "site is finished while a link or an image is dead.";
    return note;
}

void App::save_block_as(const std::string& suggested_path, const std::string& lang_tag,
                        const std::string& content) {
    std::string name = sys::file_name(suggested_path);
    if (name.empty()) {
        const char* tag = lang::tag(lang::from_tag(lang_tag));
        name = std::string("untitled.") + (tag[0] ? tag : "txt");
    }

    const std::string target = sys::pick_save_file(hwnd_, name, workspace_dir_for_dialogs());
    if (target.empty()) return;

    std::string body = content;
    if (!body.empty() && body.back() != '\n') body += '\n';
    std::string err;
    if (!sys::write_file(target, body, err)) {
        toast("Could not write " + target + ": " + err, true);
        return;
    }
    toast("Wrote " + sys::file_name(target));
    open_file(target);
    refresh_tree();
}

void App::write_block_to_file(const std::string& rel_path, const std::string& content) {
    ToolAction action;
    action.tool = "create_file";
    action.path = rel_path;
    action.content = content;
    std::string absolute, err;
    if (resolve_in_workspace(rel_path, absolute, err))
        action.overwrite = sys::file_exists(absolute);
    execute_action(action);
    toast(action.result, !action.ok);
}

std::string App::build_file_context() const {
    if (!settings_.attach_open_file) return std::string();
    if (active_doc_ < 0 || active_doc_ >= (int)documents_.size()) return std::string();
    const Document* doc = documents_[active_doc_].get();

    std::string body;
    bool selection = settings_.attach_selection && doc->editor.has_selection();
    if (selection) body = doc->editor.selected_text();
    else body = doc->editor.text();

    if ((int)body.size() > settings_.max_context_bytes)
        body = body.substr(0, (size_t)settings_.max_context_bytes) + "\n… (truncated)";

    if (trim(body).empty()) return std::string();

    // Named the way a tool call has to name it: relative to the project root.
    std::string header = "File: " + (doc->path.empty() ? doc->display_name
                                                       : display_path(doc->path, workspace_, 200));
    if (selection) header += "  (selected lines only)";
    return header + "\n```" + ed::language_tag(doc->editor.language()) + "\n" + body + "\n```";
}

void App::send_chat(const std::string& user_text, const std::string& attachment,
                    const std::string& note) {
    if (settings_.chat_model.empty()) {
        toast("Pick a model first", true);
        return;
    }
    stop_chat();

    ChatMessage user;
    user.role = "user";
    user.content = user_text;
    Document* doc = active_document();
    if (!attachment.empty()) {
        user.attachment_note =
            !note.empty()
                ? note
                : (doc ? doc->display_name + (doc->editor.has_selection() ? " (selection)" : "")
                       : std::string());
    }
    if (!pending_attachment_.empty() && !pending_attachment_note_.empty()) {
        user.attachment_note = user.attachment_note.empty()
                                   ? pending_attachment_note_
                                   : user.attachment_note + " + " + pending_attachment_note_;
    }
    chat_.push_back(user);

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.streaming = true;
    chat_.push_back(assistant);

    // agent_history_ is the authoritative transcript: it also holds the tool
    // calls and their results, which the visible chat does not replay.
    if (agent_history_.empty()) {
        ollama::Message system;
        system.role = "system";
        system.content = effective_system_prompt(tools_available());
        agent_history_.push_back(std::move(system));
    }

    ollama::Message turn;
    turn.role = "user";
    turn.content = user_text;
    if (!attachment.empty()) turn.content += "\n\n" + attachment;
    // A file picked out of the explorer for this message goes with it. Stashing
    // it and then leaving it behind is how "I attached the file" turns into a
    // model answering about the wrong one.
    if (!pending_attachment_.empty()) {
        turn.content += "\n\nFile: " +
                        (pending_attachment_path_.empty()
                             ? pending_attachment_note_
                             : display_path(pending_attachment_path_, workspace_, 200)) +
                        "\n" + pending_attachment_;
    }
    agent_history_.push_back(std::move(turn));

    agent_steps_ = 0;
    repair_attempts_ = 0;
    block_nudges_ = 0;
    turn_wants_files_ = asks_for_changes(user_text);
    pages_written_.clear();
    // Freshness is per turn: a file read during an earlier exchange may well
    // have changed since, so it has to be read again before it can be replaced.
    read_paths_.clear();
    // The open file travels with the message, so the model has seen it just as
    // surely as if it had called read_file - and rewriting it whole is then a
    // decision rather than a shot in the dark. A selection is not the file, so
    // it does not count.
    if (!attachment.empty() && doc && !doc->path.empty() &&
        !(settings_.attach_selection && doc->editor.has_selection()))
        mark_read(sys::normalize(doc->path));
    if (!pending_attachment_.empty() && !pending_attachment_path_.empty())
        mark_read(sys::normalize(pending_attachment_path_));
    start_agent_turn();
    chat_scroll_to_bottom_ = true;
    pending_attachment_.clear();
    pending_attachment_note_.clear();
    pending_attachment_path_.clear();
}

void App::stop_chat() {
    if (chat_stream_) {
        chat_stream_->cancel();
        chat_stream_.reset();
        for (auto it = chat_.rbegin(); it != chat_.rend(); ++it) {
            if (it->streaming) {
                it->streaming = false;
                if (it->content.empty()) it->content = "_(stopped)_";
                break;
            }
        }
    }
}

void App::update_streams() {
    // model list
    if (g_fetch) {
        std::lock_guard<std::mutex> lock(g_fetch->mutex);
        if (g_fetch->done) {
            models_ = g_fetch->models;
            ollama_online_ = g_fetch->online;
            ollama_version_ = g_fetch->version;
            models_error_ = g_fetch->error;
            models_loading_ = false;
            if (g_fetch->ok && !models_.empty()) {
                if (settings_.chat_model.empty()) {
                    settings_.chat_model = best_installed_model();
                } else if (!model_installed(settings_.chat_model)) {
                    // The saved model is gone (removed, renamed, or never pulled
                    // on this machine). Keeping it means every message fails
                    // with a 404 the user has to decode, so move to one that is
                    // actually here and say so.
                    const std::string missing = settings_.chat_model;
                    settings_.chat_model = best_installed_model();
                    toast(missing + " is not installed — switched to " + settings_.chat_model,
                          true);
                    settings_.save();
                }
            }
            g_fetch.reset();
        }
    }
    // periodic health check
    if (ImGui::GetTime() - last_ping_ > 10.0 && !models_loading_) {
        last_ping_ = ImGui::GetTime();
        refresh_models(false);
    }

    // chat stream
    if (chat_stream_) {
        ollama::Stream::Snapshot snap = chat_stream_->snapshot();
        for (auto it = chat_.rbegin(); it != chat_.rend(); ++it) {
            if (it->role != "assistant" || !it->streaming) continue;
            it->content = snap.text;
            it->thinking = snap.thinking;
            if (!snap.done) break;

            // The model has no tool support: remember that and retry in text mode,
            // where files are offered as buttons on code blocks instead.
            if (snap.tools_unsupported && tools_used_this_turn_) {
                models_without_tools_.push_back(settings_.chat_model);
                chat_stream_.reset();
                toast(settings_.chat_model + " has no tool support — using code blocks instead");
                start_agent_turn();
                break;
            }

            it->streaming = false;
            it->error = snap.error;
            it->eval_count = snap.eval_count;
            it->tokens_per_second = snap.tokens_per_second;

            // Prefer Ollama's structured calls; otherwise recover calls the
            // model wrote as JSON in its reply.
            std::vector<ollama::ToolCall> calls = snap.tool_calls;
            std::string visible = snap.text;
            bool from_text = false;
            if (calls.empty() && tools_used_this_turn_) {
                std::vector<std::string> known;
                for (const ollama::ToolDef& tool : file_tools()) known.push_back(tool.name);
                if (agent::harvest_text_tool_calls(visible, known, calls) > 0) from_text = true;
            }

            // Still nothing, and yet the reply is full of code: the model wrote
            // the files as code blocks and left the user to copy them out. A
            // block that says which file it is gets written for real.
            bool adopted = false;
            if (calls.empty()) {
                adopted = adopt_code_blocks(*it, visible, tools_used_this_turn_);
                if (adopted) from_text = true;
            }
            it->content = visible;

            // Record the turn as a proper tool-calling one whether the calls
            // arrived through the API field, were recovered from the text, or
            // were the code blocks themselves, so the model sees a consistent
            // protocol on the next round.
            if (adopted) {
                for (const ToolAction& action : it->actions) {
                    js::Value args = js::Value::object();
                    args.set("path", js::Value(action.path));
                    args.set("content", js::Value(action.content));
                    ollama::ToolCall call;
                    call.name = action.tool;
                    call.arguments = args.dump();
                    calls.push_back(std::move(call));
                }
            }

            ollama::Message assistant;
            assistant.role = "assistant";
            assistant.content = visible;
            assistant.tool_calls = calls;
            agent_history_.push_back(std::move(assistant));

            it->actions_from_text = from_text;
            it->expect_followup = tools_used_this_turn_;
            if (!adopted && !calls.empty()) collect_tool_calls(*it, calls);
            if (it->content.empty() && it->error.empty() && it->actions.empty())
                it->content = "_(empty response)_";

            chat_stream_.reset();
            break;
        }
    }

    // Approvals granted since the last frame may unblock the agent loop.
    maybe_continue_agent();

    // inline completion stream
    if (completion_stream_) {
        ollama::Stream::Snapshot snap = completion_stream_->snapshot();
        if (snap.done) {
            completion_stream_.reset();
            if (!snap.error.empty()) {
                toast("Completion failed: " + snap.error, true);
            } else if (completion_doc_ >= 0 && completion_doc_ < (int)documents_.size()) {
                std::string text = strip_code_fence(snap.text);
                while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
                Document* doc = documents_[completion_doc_].get();
                if (doc->editor.cursor() == completion_cursor_ && !text.empty())
                    doc->editor.set_ghost(text);
                else if (text.empty())
                    toast("The model returned nothing to insert");
            }
            completion_doc_ = -1;
        }
    }
}

void App::quick_action(const char* label, const char* instruction, bool needs_code,
                       const char* agent_instruction) {
    Document* doc = active_document();
    if (needs_code && !doc) {
        toast("Open a file first", true);
        return;
    }
    std::string attachment;
    std::string note;
    bool on_disk = false;
    if (doc) {
        bool selection = doc->editor.has_selection();
        std::string body = selection ? doc->editor.selected_text() : doc->editor.text();
        if ((int)body.size() > settings_.max_context_bytes)
            body = body.substr(0, (size_t)settings_.max_context_bytes) + "\n… (truncated)";
        if (trim(body).empty()) {
            toast("Nothing to send — the file is empty", true);
            return;
        }
        std::string absolute, err;
        on_disk = !doc->path.empty() && resolve_in_workspace(doc->path, absolute, err);
        // The path as the model has to write it in a call: relative to the
        // project root. Handing it an absolute one is how a tool call comes
        // back "path is outside the open project folder".
        attachment = "File: " +
                     (doc->path.empty() ? doc->display_name
                                        : display_path(doc->path, workspace_, 200)) +
                     (selection ? "  (selected lines only)" : "") + "\n```" +
                     ed::language_tag(doc->editor.language()) + "\n" + body + "\n```";
        note = doc->display_name + (selection ? " (selection)" : "");
    }
    (void)label;
    // With the file within reach, these actions do the work rather than
    // describing it — which is the whole point of having the tools.
    const bool can_apply = agent_instruction && on_disk && tools_available();
    send_chat(can_apply ? agent_instruction : instruction, attachment, note);
    chat_scroll_to_bottom_ = true;
}

void App::request_inline_completion() {
    Document* doc = active_document();
    if (!doc) {
        toast("Open a file first", true);
        return;
    }
    if (completion_stream_) {
        toast("A completion is already running");
        return;
    }
    std::string model = settings_.completion_model.empty() ? settings_.chat_model
                                                           : settings_.completion_model;
    if (model.empty()) {
        toast("Pick a model first", true);
        return;
    }

    const std::string prefix = doc->editor.context_before(4000);
    const std::string suffix = doc->editor.context_after(2000);
    const char* lang = ed::language_tag(doc->editor.language());

    std::string system =
        "You are a code completion engine. Continue the code exactly where the marker "
        "<CURSOR> sits. Reply with the raw continuation only: no explanation, no markdown "
        "fences, no repetition of the code before the cursor. Keep it short — a few lines at "
        "most — and match the surrounding style and indentation.";

    std::string user = std::string("Language: ") + (lang[0] ? lang : "plain text") + "\n\n" +
                       prefix + "<CURSOR>" + suffix;

    ollama::Options opts;
    opts.temperature = 0.2f;
    opts.top_p = 0.9f;
    opts.num_predict = 160;
    opts.num_ctx = settings_.num_ctx;

    std::vector<ollama::Message> messages = {{"system", system}, {"user", user}};
    completion_stream_ = client_.chat(model, messages, opts);
    completion_doc_ = active_doc_;
    completion_cursor_ = doc->editor.cursor();
    toast("Asking " + model + " for a completion…");
}

void App::insert_code_into_editor(const std::string& code, bool replace_selection) {
    Document* doc = active_document();
    if (!doc) {
        new_document();
        doc = active_document();
    }
    if (!doc) return;
    if (replace_selection && doc->editor.has_selection()) doc->editor.replace_selection(code);
    else doc->editor.insert_text(code);
    toast("Inserted into " + doc->display_name);
}

// ------------------------------------------------------------------ chrome

void App::draw_title_bar() {
    const float h = title_bar_height_;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), ui::C.bg_titlebar);
    dl->AddLine(ImVec2(origin.x, origin.y + h - 0.5f), ImVec2(origin.x + width, origin.y + h - 0.5f),
                ui::C.border, 1.0f);

    // The mark and the wordmark, set the way a masthead is: a small wireframe
    // globe, then the name in wide-tracked capitals.
    const float logo = ui::px(18);
    const float logo_x = origin.x + ui::px(18);
    ui::draw_icon(dl, ui::Icon::Globe, ImVec2(logo_x, origin.y + (h - logo) * 0.5f), logo,
                  ui::C.ink);

    ImFont* wm_font = ui::F.ui_small ? ui::F.ui_small : ui::F.ui;
    const float wm_size = wm_font ? wm_font->FontSize : ImGui::GetFontSize();
    const float wm_x = logo_x + logo + ui::px(10);
    const float wm_w =
        ui::draw_caps(dl, wm_font, wm_size, ImVec2(wm_x, origin.y + (h - wm_size) * 0.5f),
                      ui::C.ink, "ai-ide", ui::px(4.0f));

    // Menus, laid out like a masthead's section list.
    ImGui::SetCursorScreenPos(
        ImVec2(wm_x + wm_w + ui::px(26), origin.y + (h - ImGui::GetFrameHeight()) * 0.5f));
    draw_menus();
    const float menus_right = ImGui::GetItemRectMax().x;

    // Window buttons, kept square and unrounded like the rest of the frame.
    // Where the system draws the frame it has its own, and these stay out.
    const float btn_w = ui::px(40);
    const float buttons_left = hooks_.native_frame ? origin.x + width : origin.x + width - btn_w * 3.0f;
    ImGui::SetCursorScreenPos(ImVec2(buttons_left, origin.y));

    struct WinBtn { ui::Icon icon; const char* id; };
    const bool maximized = hooks_.is_maximized && hooks_.is_maximized();
    WinBtn buttons[3] = {{ui::Icon::Minimize, "##min"},
                         {maximized ? ui::Icon::Restore : ui::Icon::Maximize, "##max"},
                         {ui::Icon::Close, "##close"}};
    for (int i = 0; i < (hooks_.native_frame ? 0 : 3); ++i) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(p);
        bool clicked = ImGui::InvisibleButton(buttons[i].id, ImVec2(btn_w, h));
        bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImU32 bg = i == 2 ? ui::C.error : ui::C.bg_surface_hi;
            dl->AddRectFilled(p, ImVec2(p.x + btn_w, p.y + h), bg);
        }
        float glyph = ui::px(10);
        ui::draw_icon(dl, buttons[i].icon,
                      ImVec2(p.x + (btn_w - glyph) * 0.5f, p.y + (h - glyph) * 0.5f), glyph,
                      hovered ? (i == 2 ? ui::C.on_ink : ui::C.text) : ui::C.text_dim);
        if (clicked) {
            if (i == 0 && hooks_.minimize) hooks_.minimize();
            if (i == 1 && hooks_.toggle_maximize) hooks_.toggle_maximize();
            if (i == 2) should_exit_ = true;
        }
        ImGui::SameLine(0, 0);
    }
    ImGui::NewLine();

    // Right-hand modules: workspace, connection state, then the model — the
    // selected model is the one filled cell, the way the reference fills only
    // the tab you are on.
    const float seg_h = ui::px(22);
    const float seg_top = origin.y + (h - seg_h) * 0.5f;
    float right = buttons_left - ui::px(14);

    const std::string model = settings_.chat_model.empty() ? "no model" : settings_.chat_model;
    right = ui::segment_r(dl, right, seg_top, seg_h, model.c_str(),
                          ollama_online_ ? ui::C.ink : 0,
                          ollama_online_ ? ui::C.on_ink : ui::C.text_faint);
    right = ui::segment_r(dl, right, seg_top, seg_h, ollama_online_ ? "online" : "offline", 0,
                          ollama_online_ ? ui::C.text_dim : ui::C.error);
    if (!workspace_.empty()) {
        const std::string ws = sys::file_name(workspace_);
        right = ui::segment_r(dl, right, seg_top, seg_h, ws.c_str(), 0, ui::C.text);
    }

    // The command centre, as in VS Code's title bar: a quiet box in the middle
    // that opens quick open, with the palette one keystroke away.
    {
        const float space_left = menus_right + ui::px(24);
        const float space_right = right - ui::px(24);
        const float box_w = ImMin(ui::px(340), space_right - space_left);
        if (box_w >= ui::px(150)) {
            const float box_h = ui::px(24);
            const float bx = std::floor(space_left + (space_right - space_left - box_w) * 0.5f);
            const float by = std::floor(origin.y + (h - box_h) * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            if (ImGui::InvisibleButton("##commandcenter", ImVec2(box_w, box_h))) open_palette("");
            const bool hot = ImGui::IsItemHovered();
            if (hot) ui::tooltip("Go to file (Ctrl+P) · commands (Ctrl+Shift+P)");
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + box_w, by + box_h),
                              hot ? ui::C.bg_surface_hi : ui::C.bg_surface);
            dl->AddRect(ImVec2(bx, by), ImVec2(bx + box_w, by + box_h), hot ? ui::C.border_strong : ui::C.border);
            const float icon = ui::px(11);
            ui::draw_icon(dl, ui::Icon::Search, ImVec2(bx + ui::px(9), by + (box_h - icon) * 0.5f), icon,
                          ui::C.text_dim);
            ImFont* cf = ui::F.ui_micro ? ui::F.ui_micro : ui::F.ui_small;
            const char* label = "go to file";
            ui::draw_caps(dl, cf, cf->FontSize, ImVec2(bx + ui::px(26), by + (box_h - cf->FontSize) * 0.5f),
                          ui::C.text_dim, label);
            const char* hint = ui::keys("ctrl+p");
            const float hw = ui::caps_width(cf, cf->FontSize, hint);
            ui::draw_caps(dl, cf, cf->FontSize,
                          ImVec2(bx + box_w - hw - ui::px(9), by + (box_h - cf->FontSize) * 0.5f),
                          ui::C.text_faint, hint);
        }
    }

    // caption drag zone: anywhere in the bar that is not a widget or a module
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool in_bar = mouse.y >= origin.y && mouse.y < origin.y + h && mouse.x >= origin.x &&
                        mouse.x < buttons_left;
    caption_drag_zone_ = !hooks_.native_frame && in_bar && mouse.x > menus_right && mouse.x < right &&
                         !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() &&
                         !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h));
}

void App::draw_menus() {
    struct MenuDef { const char* label; const char* id; };
    const MenuDef menus[] = {{"File", "##menu_file"}, {"Edit", "##menu_edit"},
                             {"Selection", "##menu_selection"}, {"View", "##menu_view"},
                             {"Go", "##menu_go"}, {"AI", "##menu_ai"}, {"Help", "##menu_help"}};
    const int menu_count = (int)(sizeof(menus) / sizeof(menus[0]));

    // Section names, not buttons: tracked capitals with nothing drawn around
    // them until the pointer is over one, and an underline on the open menu.
    ImFont* mf = ui::F.ui_micro ? ui::F.ui_micro : ui::F.ui_small;
    const float mfs = mf ? mf->FontSize : ImGui::GetFontSize();
    const float mh = std::floor(mfs + ui::px(14));
    const float mpad = ui::px(9);
    ImDrawList* mdl = ImGui::GetWindowDrawList();

    // Each menu drops down from under its own name.
    ImVec2 anchors[sizeof(menus) / sizeof(menus[0])];
    // Once one menu is open, moving across the bar opens its neighbours.
    bool any_open = false;
    for (int i = 0; i < menu_count; ++i) any_open = any_open || ImGui::IsPopupOpen(menus[i].id);

    for (int i = 0; i < menu_count; ++i) {
        if (i) ImGui::SameLine(0, ui::px(1));
        const float tw = ui::caps_width(mf, mfs, menus[i].label);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 sz(tw + mpad * 2, mh);
        if (ImGui::InvisibleButton(menus[i].id, sz)) ImGui::OpenPopup(menus[i].id);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
        if (any_open && hovered && !ImGui::IsPopupOpen(menus[i].id)) {
            ImGui::CloseCurrentPopup();
            ImGui::OpenPopup(menus[i].id);
        }
        const bool open = ImGui::IsPopupOpen(menus[i].id);
        if (hovered || open)
            mdl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), ui::C.bg_surface_hi);
        if (open)
            mdl->AddRectFilled(ImVec2(p.x, p.y + sz.y - ui::px(2)),
                               ImVec2(p.x + sz.x, p.y + sz.y), ui::C.ink);
        ui::draw_caps(mdl, mf, mfs, ImVec2(p.x + mpad, p.y + (sz.y - mfs) * 0.5f),
                      hovered || open ? ui::C.text : ui::C.text_dim, menus[i].label);
        anchors[i] = ImVec2(p.x, p.y + sz.y);
    }

    Document* doc = active_document();
    const bool has_doc = doc != nullptr;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), ui::px(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(8), ui::px(8)));

    // An editor command as a menu item: greyed out without a file, and the
    // keyboard goes back to the editor afterwards.
    auto edit_item = [&](const char* label, const char* keys,
                         const std::function<void(ed::TextEditor&)>& fn) {
        if (ImGui::MenuItem(label, ui::keys(keys), false, has_doc)) editor_command(fn);
    };

    ImGui::SetNextWindowPos(anchors[0]);
    if (ImGui::BeginPopup("##menu_file")) {
        if (ImGui::MenuItem("New File", ui::keys("Ctrl+N"))) new_document();
        if (ImGui::MenuItem("Open File…", ui::keys("Ctrl+O"))) {
            std::string path = sys::pick_open_file(hwnd_, workspace_dir_for_dialogs());
            if (!path.empty()) open_file(path);
        }
        if (ImGui::MenuItem("Open Folder…", ui::keys("Ctrl+K"))) {
            std::string folder = sys::pick_folder(hwnd_, "Open Folder");
            if (!folder.empty()) set_workspace(folder);
        }
        if (ImGui::BeginMenu("Recent Folders", !settings_.recent_folders.empty())) {
            for (const std::string& folder : settings_.recent_folders)
                if (ImGui::MenuItem(folder.c_str())) set_workspace(folder);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reopen Closed Editor", ui::keys("Ctrl+Shift+T"), false, !closed_paths_.empty()))
            reopen_closed_document();
        ImGui::Separator();
        if (ImGui::MenuItem("Save", ui::keys("Ctrl+S"), false, has_doc)) save_document(doc, false);
        if (ImGui::MenuItem("Save As…", ui::keys("Ctrl+Shift+S"), false, has_doc)) save_document(doc, true);
        if (ImGui::MenuItem("Save All", nullptr, false, has_doc)) save_all();
        if (ImGui::MenuItem("Revert File", nullptr, false, has_doc && !doc->path.empty()))
            revert_document(doc);
        ImGui::Separator();
        if (ImGui::MenuItem("Close Tab", ui::keys("Ctrl+W"), false, has_doc)) close_document(active_doc_);
        if (ImGui::MenuItem("Close Other Tabs", nullptr, false, documents_.size() > 1))
            close_documents(CloseMode::Others, active_doc_);
        if (ImGui::MenuItem("Close All Tabs", nullptr, false, has_doc))
            close_documents(CloseMode::All, active_doc_);
        ImGui::Separator();
        if (ImGui::MenuItem("Settings…", ui::keys("Ctrl+,"))) show_settings_ = true;
        if (ImGui::MenuItem("Exit", ui::keys("Alt+F4"))) should_exit_ = true;
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(anchors[1]);
    if (ImGui::BeginPopup("##menu_edit")) {
        if (ImGui::MenuItem("Undo", ui::keys("Ctrl+Z"), false, has_doc && doc->editor.can_undo()))
            editor_command([](ed::TextEditor& e) { e.undo(); });
        if (ImGui::MenuItem("Redo", ui::keys("Ctrl+Y"), false, has_doc && doc->editor.can_redo()))
            editor_command([](ed::TextEditor& e) { e.redo(); });
        ImGui::Separator();
        edit_item("Cut", "Ctrl+X", [](ed::TextEditor& e) { e.cut(); });
        edit_item("Copy", "Ctrl+C", [](ed::TextEditor& e) { e.copy(); });
        edit_item("Paste", "Ctrl+V", [](ed::TextEditor& e) { e.paste(); });
        ImGui::Separator();
        edit_item("Find", "Ctrl+F", [](ed::TextEditor& e) { e.open_find(); });
        edit_item("Replace", "Ctrl+H", [](ed::TextEditor& e) { e.open_replace(); });
        if (ImGui::MenuItem("Find in Files", ui::keys("Ctrl+Shift+F"), false, !workspace_.empty())) open_search_view();
        ImGui::Separator();
        edit_item("Toggle Line Comment", "Ctrl+/", [](ed::TextEditor& e) { e.toggle_line_comment(); });
        edit_item("Toggle Block Comment", "Shift+Alt+A", [](ed::TextEditor& e) { e.toggle_block_comment(); });
        if (ImGui::BeginMenu("Lines", has_doc)) {
            edit_item("Delete Line", "Ctrl+Shift+K", [](ed::TextEditor& e) { e.delete_lines(); });
            edit_item("Insert Line Below", "Ctrl+Enter", [](ed::TextEditor& e) { e.insert_line_below(); });
            edit_item("Insert Line Above", "Ctrl+Shift+Enter", [](ed::TextEditor& e) { e.insert_line_above(); });
            edit_item("Indent Line", "Ctrl+]", [](ed::TextEditor& e) { e.indent_lines(); });
            edit_item("Outdent Line", "Ctrl+[", [](ed::TextEditor& e) { e.outdent_lines(); });
            edit_item("Join Lines", nullptr, [](ed::TextEditor& e) { e.join_lines(); });
            ImGui::Separator();
            edit_item("Sort Lines Ascending", nullptr, [](ed::TextEditor& e) { e.sort_lines(false); });
            edit_item("Sort Lines Descending", nullptr, [](ed::TextEditor& e) { e.sort_lines(true); });
            edit_item("Reverse Lines", nullptr, [](ed::TextEditor& e) { e.reverse_lines(); });
            edit_item("Delete Duplicate Lines", nullptr, [](ed::TextEditor& e) { e.delete_duplicate_lines(); });
            edit_item("Trim Trailing Whitespace", nullptr, [](ed::TextEditor& e) { e.trim_trailing_whitespace(); });
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Transform", has_doc)) {
            for (int k = 0; k < (int)ed::CaseKind::Count; ++k) {
                const ed::CaseKind kind = (ed::CaseKind)k;
                std::string label = std::string("To ") + ed::case_name(kind);
                edit_item(label.c_str(), nullptr, [kind](ed::TextEditor& e) { e.transform_case(kind); });
            }
            ImGui::Separator();
            edit_item("Transpose Characters", nullptr, [](ed::TextEditor& e) { e.transpose_chars(); });
            edit_item("Convert Indentation to Spaces", nullptr,
                      [](ed::TextEditor& e) { e.convert_indentation(false); });
            edit_item("Convert Indentation to Tabs", nullptr,
                      [](ed::TextEditor& e) { e.convert_indentation(true); });
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Toggle Overtype", ui::keys("Insert"), has_doc && doc->editor.overtype(), has_doc))
            editor_command([](ed::TextEditor& e) { e.toggle_overtype(); });
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(anchors[2]);
    if (ImGui::BeginPopup("##menu_selection")) {
        edit_item("Select All", "Ctrl+A", [](ed::TextEditor& e) { e.select_all(); });
        edit_item("Expand Selection", "Shift+Alt+Right", [](ed::TextEditor& e) { e.expand_selection(); });
        edit_item("Shrink Selection", "Shift+Alt+Left", [](ed::TextEditor& e) { e.shrink_selection(); });
        edit_item("Expand Line Selection", nullptr, [](ed::TextEditor& e) { e.select_line(); });
        edit_item("Select to Bracket", nullptr, [](ed::TextEditor& e) { e.select_to_bracket(); });
        ImGui::Separator();
        edit_item("Copy Line Up", "Shift+Alt+Up", [](ed::TextEditor& e) { e.copy_lines(-1); });
        edit_item("Copy Line Down", "Shift+Alt+Down", [](ed::TextEditor& e) { e.copy_lines(1); });
        edit_item("Move Line Up", "Alt+Up", [](ed::TextEditor& e) { e.move_lines(-1); });
        edit_item("Move Line Down", "Alt+Down", [](ed::TextEditor& e) { e.move_lines(1); });
        edit_item("Duplicate Selection", nullptr, [](ed::TextEditor& e) { e.duplicate_selection(); });
        ImGui::Separator();
        edit_item("Add Cursor Above", "Ctrl+Alt+Up", [](ed::TextEditor& e) { e.add_caret_vertical(-1); });
        edit_item("Add Cursor Below", "Ctrl+Alt+Down", [](ed::TextEditor& e) { e.add_caret_vertical(1); });
        edit_item("Add Cursors to Line Ends", "Shift+Alt+I", [](ed::TextEditor& e) { e.carets_to_line_ends(); });
        edit_item("Add Next Occurrence", "Ctrl+D", [](ed::TextEditor& e) { e.add_next_occurrence(); });
        edit_item("Select All Occurrences", "Ctrl+Shift+L", [](ed::TextEditor& e) { e.select_all_occurrences(); });
        edit_item("Cursor Undo", "Ctrl+U", [](ed::TextEditor& e) { e.cursor_undo(); });
        ImGui::Separator();
        ImGui::PushFont(ui::F.ui_small);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "%s",
                           ui::keys("Alt+Click adds a cursor, Shift+Alt+Drag selects a column"));
        ImGui::PopFont();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(anchors[3]);
    if (ImGui::BeginPopup("##menu_view")) {
        if (ImGui::MenuItem("Command Palette…", ui::keys("Ctrl+Shift+P"))) open_palette(">");
        ImGui::Separator();
        if (ImGui::BeginMenu("Cursor")) {
            if (ImGui::BeginMenu("Style")) {
                for (int k = 0; k < (int)ed::CursorStyle::Count; ++k)
                    if (ImGui::MenuItem(ed::cursor_style_name((ed::CursorStyle)k), nullptr,
                                        settings_.cursor_style == k))
                        settings_.cursor_style = k;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Blinking")) {
                for (int k = 0; k < (int)ed::CursorBlink::Count; ++k)
                    if (ImGui::MenuItem(ed::cursor_blink_name((ed::CursorBlink)k), nullptr,
                                        settings_.cursor_blinking == k))
                        settings_.cursor_blinking = k;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Overtype Style")) {
                for (int k = 0; k < (int)ed::CursorStyle::Count; ++k)
                    if (ImGui::MenuItem(ed::cursor_style_name((ed::CursorStyle)k), nullptr,
                                        settings_.overtype_cursor_style == k))
                        settings_.overtype_cursor_style = k;
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Smooth Caret Animation", nullptr, settings_.smooth_caret))
                settings_.smooth_caret = !settings_.smooth_caret;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Render Whitespace")) {
            for (int k = 0; k < (int)ed::Whitespace::Count; ++k)
                if (ImGui::MenuItem(ed::whitespace_name((ed::Whitespace)k), nullptr, settings_.render_whitespace == k))
                    settings_.render_whitespace = k;
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Minimap", nullptr, settings_.minimap)) settings_.minimap = !settings_.minimap;
        if (ImGui::MenuItem("Bracket Pair Colors", nullptr, settings_.bracket_pair_colors))
            settings_.bracket_pair_colors = !settings_.bracket_pair_colors;
        if (ImGui::MenuItem("Matching Brackets", nullptr, settings_.match_brackets))
            settings_.match_brackets = !settings_.match_brackets;
        if (ImGui::MenuItem("Occurrence Highlight", nullptr, settings_.highlight_occurrences))
            settings_.highlight_occurrences = !settings_.highlight_occurrences;
        if (ImGui::MenuItem("Line Numbers", nullptr, settings_.show_line_numbers))
            settings_.show_line_numbers = !settings_.show_line_numbers;
        if (ImGui::MenuItem("Indent Guides", nullptr, settings_.indent_guides))
            settings_.indent_guides = !settings_.indent_guides;
        if (ImGui::MenuItem("Highlight Current Line", nullptr, settings_.highlight_current_line))
            settings_.highlight_current_line = !settings_.highlight_current_line;
        ImGui::Separator();
        if (ImGui::MenuItem("Zoom In", ui::keys("Ctrl+="))) zoom_editor(1);
        if (ImGui::MenuItem("Zoom Out", ui::keys("Ctrl+-"))) zoom_editor(-1);
        if (ImGui::MenuItem("Reset Zoom", ui::keys("Ctrl+0"))) zoom_editor(0);
        ImGui::Separator();
        if (live_.running()) {
            char label[64];
            std::snprintf(label, sizeof(label), "Stop Live Server (port %d)", live_.port());
            if (ImGui::MenuItem("Open Preview in Browser", ui::keys("Alt+L"))) open_live_preview();
            if (ImGui::MenuItem("Copy Server Address")) {
                ImGui::SetClipboardText(live_.origin().c_str());
                toast("Copied " + live_.origin());
            }
            if (ImGui::MenuItem(label)) stop_live_server();
        } else if (ImGui::MenuItem("Go Live (local web server)", ui::keys("Alt+L"))) {
            open_live_preview();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            layout_initialized_ = false;
            toast("Layout reset");
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(anchors[4]);
    if (ImGui::BeginPopup("##menu_go")) {
        if (ImGui::MenuItem("Go to File…", ui::keys("Ctrl+P"))) open_palette("");
        if (ImGui::MenuItem("Go to Line…", ui::keys("Ctrl+G"), false, has_doc)) open_palette(":");
        if (ImGui::MenuItem("Go to Symbol in File…", ui::keys("Ctrl+Shift+O"), false, has_doc)) open_palette("@");
        edit_item("Go to Bracket", "Ctrl+Shift+\\", [](ed::TextEditor& e) { e.goto_matching_bracket(); });
        ImGui::Separator();
        edit_item("Next Match", "F3", [](ed::TextEditor& e) { e.find_next(); });
        edit_item("Previous Match", "Shift+F3", [](ed::TextEditor& e) { e.find_previous(); });
        ImGui::Separator();
        if (ImGui::MenuItem("Next Tab", ui::keys("Ctrl+Tab"), false, documents_.size() > 1)) cycle_document(1);
        if (ImGui::MenuItem("Previous Tab", ui::keys("Ctrl+Shift+Tab"), false, documents_.size() > 1)) cycle_document(-1);
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(anchors[5]);
    if (ImGui::BeginPopup("##menu_ai")) {
        if (ImGui::MenuItem("Complete at Caret", ui::keys("Ctrl+Space"), false, has_doc))
            request_inline_completion();
        if (ImGui::MenuItem("Explain", ui::keys("Ctrl+L"), false, has_doc))
            quick_action("Explain", "Explain what this code does, step by step.", true);
        if (ImGui::MenuItem("Improve", nullptr, false, has_doc))
            quick_action("Improve",
                         "Refactor this code to be clearer and more idiomatic. Return the full "
                         "replacement code in one fenced block, then a short list of changes.",
                         true);
        if (ImGui::MenuItem("Fix Bugs", nullptr, false, has_doc))
            quick_action("Fix", "Find bugs or edge cases in this code and fix them.", true);
        if (ImGui::MenuItem("Write Tests", nullptr, false, has_doc))
            quick_action("Tests", "Write focused unit tests for this code.", true);
        ImGui::Separator();
        if (ImGui::MenuItem("New Chat")) {
            stop_chat();
            chat_.clear();
            agent_history_.clear();
            agent_steps_ = 0;
        }
        if (ImGui::MenuItem("Reload Models")) refresh_models(true);
        if (ImGui::MenuItem("AI Settings…")) show_settings_ = true;
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(anchors[6]);
    if (ImGui::BeginPopup("##menu_help")) {
        if (ImGui::MenuItem("Command Palette…", ui::keys("Ctrl+Shift+P"))) open_palette(">");
        if (ImGui::MenuItem("Keyboard Shortcuts")) show_shortcuts_ = true;
        if (ImGui::MenuItem("Ollama Website")) sys::open_url("https://ollama.com");
        if (ImGui::MenuItem("About")) show_about_ = true;
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
}

void App::draw_status_bar() {
    const float h = ui::px(26);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), ui::C.bg_titlebar);
    dl->AddLine(origin, ImVec2(origin.x + width, origin.y), ui::C.border, 1.0f);

    Document* doc = active_document();
    ImFont* font = ui::F.ui_micro ? ui::F.ui_micro : ui::F.ui_small;
    const float fs = font ? font->FontSize : ImGui::GetFontSize();

    // The right block is laid out first so the left block knows how much room it
    // actually has; otherwise a long path runs straight under it.
    float right = origin.x + width;
    right = ui::segment_r(dl, right, origin.y, h, ollama_online_ ? "ollama" : "offline", 0,
                          ollama_online_ ? ui::C.text_dim : ui::C.error);
    if (live_.running()) {
        // The one thing down here that is *running*, so it gets the filled cell.
        char live_cell[48];
        std::snprintf(live_cell, sizeof(live_cell), "live %d", live_.port());
        right = ui::segment_r(dl, right, origin.y, h, live_cell, ui::C.ink, ui::C.on_ink);
    }
    // A status cell that does something when clicked, the way VS Code's do:
    // a faint fill on hover, then the action.
    auto clickable = [&](float new_right, float old_right, const char* id, const char* tip) {
        const ImVec2 back = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(new_right, origin.y));
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(ImMax(1.0f, old_right - new_right), h));
        if (ImGui::IsItemHovered()) {
            dl->AddRectFilled(ImVec2(new_right, origin.y + 1.0f), ImVec2(old_right, origin.y + h),
                              ui::C.accent_soft);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ui::tooltip(tip);
        }
        ImGui::SetCursorScreenPos(back);
        return clicked;
    };

    if (doc) {
        const ed::TextEditor& e = doc->editor;
        float before = right;
        right = ui::segment_r(dl, right, origin.y, h, ed::language_name(e.language()), 0, ui::C.text_dim);
        if (clickable(right, before, "##sb_lang", "Select language mode")) pick_language();

        before = right;
        right = ui::segment_r(dl, right, origin.y, h, e.use_crlf() ? "crlf" : "lf", 0, ui::C.text_dim);
        if (clickable(right, before, "##sb_eol", "Select end of line sequence")) pick_line_endings();

        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %d", settings_.insert_spaces ? "spaces:" : "tab size:",
                      settings_.tab_size);
        before = right;
        right = ui::segment_r(dl, right, origin.y, h, buf, 0, ui::C.text_dim);
        if (clickable(right, before, "##sb_indent", "Select indentation")) pick_indentation();

        const ed::Coord c = e.cursor();
        std::snprintf(buf, sizeof(buf), "ln %d, col %d", c.line + 1, c.column + 1);
        before = right;
        right = ui::segment_r(dl, right, origin.y, h, buf, 0, ui::C.text_dim);
        if (clickable(right, before, "##sb_line", "Go to line (Ctrl+G)")) open_palette(":");

        const int selected = e.selected_chars();
        if (e.caret_count() > 1) {
            std::snprintf(buf, sizeof(buf), "%d cursors%s", e.caret_count(),
                          selected > 0 ? (" · " + std::to_string(selected) + " sel").c_str() : "");
            right = ui::segment_r(dl, right, origin.y, h, buf, ui::C.ink, ui::C.on_ink);
        } else if (selected > 0) {
            std::snprintf(buf, sizeof(buf), "%d selected", selected);
            right = ui::segment_r(dl, right, origin.y, h, buf, 0, ui::C.text_dim);
        }
        if (e.overtype()) right = ui::segment_r(dl, right, origin.y, h, "ovr", ui::C.ink, ui::C.on_ink);
    }

    // Left block: the mode, filled with ink — the one solid mark down here —
    // then the file being edited.
    float x = origin.x;
    const bool dirty = doc && doc->editor.modified();
    x = ui::segment(dl, x, origin.y, h, dirty ? "edit*" : "edit", ui::C.ink, ui::C.on_ink);

    std::string where;
    if (doc && !doc->path.empty()) where = display_path(doc->path, workspace_, 52);
    else if (doc) where = doc->display_name;
    else where = workspace_.empty() ? "~" : sys::file_name(workspace_);

    // Trim the path to whatever is left between the two blocks.
    const float budget = right - x - ui::px(30);
    while (!where.empty() && ui::caps_width(font, fs, where.c_str()) > budget)
        where.erase(where.begin());
    if (!where.empty()) ui::segment(dl, x, origin.y, h, where.c_str(), 0, ui::C.text);

    ImGui::Dummy(ImVec2(width, h));
}

void App::draw_toasts() {
    const double now = ImGui::GetTime();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [now](const Toast& t) { return now - t.created > 4.0; }),
                  toasts_.end());
    if (toasts_.empty()) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->Pos.y + vp->Size.y - ui::px(44);
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ImGui::PushFont(ui::F.ui_small);
    for (int i = (int)toasts_.size() - 1; i >= 0; --i) {
        const Toast& t = toasts_[i];
        float age = (float)(now - t.created);
        float alpha = age > 3.4f ? ImMax(0.0f, 1.0f - (age - 3.4f) / 0.6f) : 1.0f;
        if (age < 0.18f) alpha = age / 0.18f;

        ImVec2 ts = ImGui::CalcTextSize(t.text.c_str());
        float w = ts.x + ui::px(38);
        float bh = ts.y + ui::px(16);
        ImVec2 p(vp->Pos.x + vp->Size.x - w - ui::px(18), y - bh);

        // A square card on paper: white ground, hairline edge, and a state
        // colour only on the mark at its left.
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + bh),
                          with_alpha(ui::C.bg_panel, (int)(0xFA * alpha)));
        dl->AddRect(p, ImVec2(p.x + w, p.y + bh),
                    with_alpha(t.error ? ui::C.error : ui::C.ink, (int)(0xFF * alpha)), 0.0f, 0,
                    1.0f);
        ui::draw_icon(dl, t.error ? ui::Icon::Warning : ui::Icon::Check,
                      ImVec2(p.x + ui::px(11), p.y + (bh - ui::px(13)) * 0.5f), ui::px(13),
                      with_alpha(t.error ? ui::C.error : ui::C.ink, (int)(0xFF * alpha)));
        dl->AddText(ImVec2(p.x + ui::px(30), p.y + ui::px(8)),
                    with_alpha(ui::C.text, (int)(0xFF * alpha)), t.text.c_str());
        y -= bh + ui::px(8);
    }
    ImGui::PopFont();
}

// ------------------------------------------------------------------ modals

void App::draw_settings_modal() {
    if (show_settings_) {
        ImGui::OpenPopup("Settings###settings");
        show_settings_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui::px(620), ui::px(560)), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(18), ui::px(16)));
    if (!ImGui::BeginPopupModal("Settings###settings", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar();
        return;
    }

    if (ImGui::BeginTabBar("##settingtabs")) {
        if (ImGui::BeginTabItem("Model")) {
            ui::vspace(6);
            ImGui::TextUnformatted("Ollama server");
            static char host_buf[256];
            if (ImGui::IsWindowAppearing())
                std::snprintf(host_buf, sizeof(host_buf), "%s", settings_.ollama_host.c_str());
            ImGui::SetNextItemWidth(-ui::px(110));
            if (ImGui::InputText("##host", host_buf, sizeof(host_buf))) {
                settings_.ollama_host = host_buf;
                client_.set_host(settings_.ollama_host);
            }
            ImGui::SameLine();
            if (ui::ghost_button("Test", ImVec2(ui::px(100), 0))) {
                refresh_models(true);
                settings_ping_result_ = "Checking…";
            }
            if (!settings_ping_result_.empty()) {
                ImGui::PushFont(ui::F.ui_small);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(
                                       ollama_online_ ? ui::C.success : ui::C.text_dim),
                                   "%s", ollama_online_
                                             ? ("Connected to Ollama " + ollama_version_).c_str()
                                             : settings_ping_result_.c_str());
                ImGui::PopFont();
            }

            ui::vspace(10);
            ImGui::TextUnformatted("Chat model");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##chatmodel", settings_.chat_model.empty()
                                                     ? "None"
                                                     : settings_.chat_model.c_str())) {
                for (const ollama::Model& m : models_)
                    if (ImGui::Selectable(m.name.c_str(), m.name == settings_.chat_model))
                        settings_.chat_model = m.name;
                ImGui::EndCombo();
            }

            ui::vspace(6);
            ImGui::TextUnformatted("Completion model (used for Ctrl+Space)");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##compmodel", settings_.completion_model.empty()
                                                     ? "Same as chat model"
                                                     : settings_.completion_model.c_str())) {
                if (ImGui::Selectable("Same as chat model", settings_.completion_model.empty()))
                    settings_.completion_model.clear();
                for (const ollama::Model& m : models_)
                    if (ImGui::Selectable(m.name.c_str(), m.name == settings_.completion_model))
                        settings_.completion_model = m.name;
                ImGui::EndCombo();
            }

            ui::vspace(12);
            ImGui::SliderFloat("Temperature", &settings_.temperature, 0.0f, 1.5f, "%.2f");
            ImGui::SliderFloat("Temperature (file tasks)", &settings_.agent_temperature, 0.0f,
                               1.5f, "%.2f");
            ImGui::PushFont(ui::F.ui_small);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint),
                               "The second one is used whenever the model works on your files. "
                               "Low values keep a small model on the instructions it was given.");
            ImGui::PopFont();
            ImGui::SliderFloat("Top P", &settings_.top_p, 0.1f, 1.0f, "%.2f");
            ImGui::SliderInt("Context window", &settings_.num_ctx, 0, 32768,
                             settings_.num_ctx == 0 ? "model default" : "%d tokens");
            ImGui::SliderInt("Max code sent", &settings_.max_context_bytes, 1000, 60000, "%d bytes");

            ui::vspace(10);
            ImGui::Checkbox("Send the open file with each message", &settings_.attach_open_file);
            ImGui::Checkbox("Send only the selection when there is one", &settings_.attach_selection);

            ui::vspace(10);
            ImGui::SeparatorText("Project access");
            ImGui::Checkbox("Let the AI create files and folders", &settings_.enable_file_tools);
            ImGui::PushFont(ui::F.ui_small);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::C.text_faint));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(
                "Writes are limited to the folder you have open and are shown for approval "
                "first. Models without tool support fall back to code blocks with a "
                "'Create file' button.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::BeginDisabled(!settings_.enable_file_tools);
            ImGui::Checkbox("Apply new files and edits without asking",
                            &settings_.auto_approve_writes);
            ImGui::EndDisabled();
            ImGui::PushFont(ui::F.ui_small);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint),
                               "Replacing an existing file always asks.");
            ImGui::PopFont();

            ui::vspace(10);
            ImGui::TextUnformatted("System prompt");
            input_text_multiline("##sysprompt", &settings_.system_prompt,
                                 ImVec2(-1, ui::px(110)), ImGuiInputTextFlags_None);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Editor")) {
            draw_editor_settings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Preview")) {
            ui::vspace(6);
            ImGui::TextUnformatted("Local web server");
            ImGui::PushFont(ui::F.ui_small);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint),
                               "Serves the open folder on 127.0.0.1 so you can look at a site in "
                               "the browser. It is reachable from this machine only.");
            ImGui::PopTextWrapPos();
            ImGui::PopFont();

            ui::vspace(8);
            ImGui::SetNextItemWidth(ui::px(160));
            if (ImGui::InputInt("Port", &settings_.live_port)) {
                if (settings_.live_port < 1024) settings_.live_port = 1024;
                if (settings_.live_port > 65500) settings_.live_port = 65500;
            }
            ImGui::PushFont(ui::F.ui_small);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint),
                               "If the port is busy the next free one up to +20 is used.");
            ImGui::PopFont();

            ui::vspace(8);
            if (ImGui::Checkbox("Reload the page when a file changes", &settings_.live_reload))
                live_.set_live_reload(settings_.live_reload);

            ui::vspace(12);
            ImGui::Separator();
            ui::vspace(8);
            if (live_.running()) {
                char state[160];
                std::snprintf(state, sizeof(state), "Running on %s  ·  %lld requests",
                              live_.origin().c_str(), live_.served());
                ImGui::TextUnformatted(state);
                ImGui::PushFont(ui::F.ui_small);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "%s",
                                   live_.root().c_str());
                ImGui::PopFont();
                ui::vspace(8);
                if (ui::ghost_button("Stop", ImVec2(ui::px(130), 0))) stop_live_server();
                ImGui::SameLine();
                if (ui::ghost_button("Open in browser", ImVec2(ui::px(190), 0)))
                    open_live_preview();
            } else {
                ImGui::PushFont(ui::F.ui_small);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_dim), "Not running.");
                ImGui::PopFont();
                ui::vspace(8);
                if (ui::primary_button("Go live", ImVec2(ui::px(130), 0))) open_live_preview();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ui::vspace(8);
    ImGui::Separator();
    ui::vspace(6);
    if (ui::primary_button("Save", ImVec2(ui::px(110), 0))) {
        client_.set_host(settings_.ollama_host);
        settings_.save();
        for (auto& d : documents_) apply_editor_settings(d.get());
        refresh_models(false);
        toast("Settings saved");
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::ghost_button("Close", ImVec2(ui::px(110), 0))) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

void App::draw_editor_settings() {
    ImGui::BeginChild("##editorsettings", ImVec2(0, ImGui::GetContentRegionAvail().y - ui::px(46)));
    ui::vspace(6);

    auto combo = [](const char* label, int* value, int count, const char* (*name)(int)) {
        ImGui::SetNextItemWidth(ui::px(220));
        if (ImGui::BeginCombo(label, name(*value))) {
            for (int k = 0; k < count; ++k)
                if (ImGui::Selectable(name(k), *value == k)) *value = k;
            ImGui::EndCombo();
        }
    };
    auto style_name = [](int k) { return ed::cursor_style_name((ed::CursorStyle)k); };
    auto blink_name = [](int k) { return ed::cursor_blink_name((ed::CursorBlink)k); };
    auto ws_name = [](int k) { return ed::whitespace_name((ed::Whitespace)k); };

    ImGui::SeparatorText("Cursor");
    combo("Cursor style", &settings_.cursor_style, (int)ed::CursorStyle::Count, style_name);
    combo("Blinking", &settings_.cursor_blinking, (int)ed::CursorBlink::Count, blink_name);
    ImGui::BeginDisabled(settings_.cursor_style != (int)ed::CursorStyle::Line);
    ImGui::SetNextItemWidth(ui::px(220));
    ImGui::SliderInt("Line width", &settings_.cursor_width, 1, 8, "%d px");
    ImGui::EndDisabled();
    combo("Overtype style", &settings_.overtype_cursor_style, (int)ed::CursorStyle::Count, style_name);
    ImGui::Checkbox("Smooth caret animation", &settings_.smooth_caret);

    // A live sample of every caret shape, the chosen one marked, blinking the
    // way the editor will. Click one to choose it.
    {
        ui::vspace(4);
        ImFont* mono = ui::F.mono;
        ImFont* micro = ui::F.ui_micro ? ui::F.ui_micro : ui::F.ui_small;
        const float fs = mono->FontSize;
        const float lh = std::floor(fs * 1.55f);
        const float cw = mono->GetCharAdvance('x');
        const float pad = ui::px(10);
        const int n = (int)ed::CursorStyle::Count;
        float cell_w = cw * 6.0f;
        for (int k = 0; k < n; ++k)
            cell_w = ImMax(cell_w, ui::caps_width(micro, micro->FontSize, ed::cursor_style_name((ed::CursorStyle)k)));
        cell_w = std::floor(cell_w + pad * 2.0f);
        const float cell_h = std::floor(lh + micro->FontSize + ui::px(18));
        const float gap = ui::px(4);
        const int per_row = ImMax(1, (int)((ImGui::GetContentRegionAvail().x + gap) / (cell_w + gap)));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float alpha = 1.0f, height_scale = 1.0f;
        ed::caret_blink_state((ed::CursorBlink)settings_.cursor_blinking, std::fmod(ImGui::GetTime(), 4.0),
                              alpha, height_scale);
        for (int k = 0; k < n; ++k) {
            const ed::CursorStyle style = (ed::CursorStyle)k;
            const ImVec2 q(p0.x + (k % per_row) * (cell_w + gap), p0.y + (k / per_row) * (cell_h + gap));
            const bool chosen = k == settings_.cursor_style;
            ImGui::SetCursorScreenPos(q);
            ImGui::PushID(k);
            if (ImGui::InvisibleButton("##caretsample", ImVec2(cell_w, cell_h))) settings_.cursor_style = k;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddRect(q, ImVec2(q.x + cell_w, q.y + cell_h),
                        chosen ? ui::C.ink : (hovered ? ui::C.border_strong : ui::C.border));
            const ImVec2 tp(q.x + pad, q.y + ui::px(4));
            const float ty = tp.y + std::floor((lh - fs) * 0.5f);
            dl->AddText(mono, fs, ImVec2(tp.x, ty), ui::C.syn_default, "let x");
            const float a = chosen ? alpha : 1.0f;
            const ImVec2 at(tp.x + cw * 4.0f, tp.y);
            ed::draw_caret_shape(dl, style, at, cw, lh, std::floor(ui::px((float)settings_.cursor_width) + 0.5f),
                                 with_alpha(ui::C.cursor, (int)(255 * a)), chosen ? height_scale : 1.0f);
            if (style == ed::CursorStyle::Block && a > 0.5f && (!chosen || height_scale >= 1.0f))
                dl->AddText(mono, fs, ImVec2(at.x, ty), ui::C.on_ink, "x");
            ui::draw_caps(dl, micro, micro->FontSize, ImVec2(tp.x, q.y + lh + ui::px(10)),
                          chosen ? ui::C.text : ui::C.text_faint, ed::cursor_style_name(style));
        }
        const int rows = (n + per_row - 1) / per_row;
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rows * (cell_h + gap) + ui::px(4)));
        ImGui::Dummy(ImVec2(1, 1));
    }

    ImGui::SeparatorText("Display");
    ImGui::Checkbox("Minimap", &settings_.minimap);
    combo("Render whitespace", &settings_.render_whitespace, (int)ed::Whitespace::Count, ws_name);
    ImGui::Checkbox("Bracket pair colours", &settings_.bracket_pair_colors);
    ImGui::Checkbox("Highlight matching brackets", &settings_.match_brackets);
    ImGui::Checkbox("Highlight other occurrences of the word at the caret", &settings_.highlight_occurrences);
    ImGui::Checkbox("Show line numbers", &settings_.show_line_numbers);
    ImGui::Checkbox("Show indent guides", &settings_.indent_guides);
    ImGui::Checkbox("Highlight the current line", &settings_.highlight_current_line);
    {
        static char rulers_buf[64];
        if (ImGui::IsWindowAppearing())
            std::snprintf(rulers_buf, sizeof(rulers_buf), "%s", settings_.rulers.c_str());
        ImGui::SetNextItemWidth(ui::px(220));
        if (ImGui::InputTextWithHint("Rulers", "e.g. 80, 120", rulers_buf, sizeof(rulers_buf)))
            settings_.rulers = rulers_buf;
    }
    {
        int size = (int)settings_.editor_font_size;
        ImGui::SetNextItemWidth(ui::px(220));
        ImGui::SliderInt("Font size", &size, 8, 36, "%d px");
        if (size != (int)settings_.editor_font_size) settings_.editor_font_size = (float)size;
        // Rebuilding the font atlas is not free, so it waits for the release.
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ui::set_code_font_size(settings_.editor_font_size);
            ui::request_font_rebuild();
        }
        ImGui::PushFont(ui::F.ui_small);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "%s",
                           ui::keys("Ctrl+= / Ctrl+- / Ctrl+0, or Ctrl+wheel over the editor."));
        ImGui::PopFont();
    }

    ImGui::SeparatorText("Typing");
    ImGui::SetNextItemWidth(ui::px(220));
    ImGui::SliderInt("Tab size", &settings_.tab_size, 2, 8);
    ImGui::Checkbox("Insert spaces instead of tabs", &settings_.insert_spaces);
    ImGui::Checkbox("Auto-close brackets and quotes", &settings_.auto_close_brackets);

    ui::vspace(12);
    ImGui::TextUnformatted("Default run command");
    ImGui::SetNextItemWidth(-1);
    static char cmd_buf[512];
    if (ImGui::IsWindowAppearing())
        std::snprintf(cmd_buf, sizeof(cmd_buf), "%s", terminal_command_.c_str());
    if (ImGui::InputText("##runcmd", cmd_buf, sizeof(cmd_buf))) terminal_command_ = cmd_buf;
    ImGui::PushFont(ui::F.ui_small);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint),
                       "Runs in the workspace folder from the Terminal panel.");
    ImGui::PopFont();
    ImGui::EndChild();
}

void App::draw_shortcuts_modal() {
    if (show_shortcuts_) {
        ImGui::OpenPopup("Keyboard Shortcuts###shortcuts");
        show_shortcuts_ = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ui::px(660), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Keyboard Shortcuts###shortcuts", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    struct Row { const char* keys; const char* what; };
    const Row rows[] = {
        {"Ctrl+Shift+P / F1", "Command palette"},
        {"Ctrl+P", "Go to file (type > for commands, : for a line, @ for symbols)"},
        {"Ctrl+G / Ctrl+Shift+O", "Go to line / go to symbol in the file"},
        {"Ctrl+N / Ctrl+O / Ctrl+K", "New file / open file / open folder"},
        {"Ctrl+S / Ctrl+Shift+S", "Save / save as"},
        {"Ctrl+W / Ctrl+Shift+T", "Close the tab / reopen the last closed one"},
        {"Ctrl+Tab / Ctrl+PageDown", "Next tab (with Shift, or PageUp: previous)"},
        {"Ctrl+F / Ctrl+H", "Find / replace in the file (Alt+C, Alt+W, Alt+R toggle options)"},
        {"F3 / Shift+F3", "Next / previous match"},
        {"Ctrl+Shift+F", "Find in files"},
        {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
        {"Alt+Click", "Add or remove a cursor"},
        {"Shift+Alt+Drag", "Column (box) selection"},
        {"Ctrl+Alt+Up / Down", "Add a cursor above / below"},
        {"Ctrl+D", "Add the next occurrence to the selection"},
        {"Ctrl+Shift+L / Ctrl+F2", "Select every occurrence"},
        {"Shift+Alt+I", "Put a cursor at the end of each selected line"},
        {"Ctrl+U", "Undo the last cursor change"},
        {"Escape", "Back to a single cursor"},
        {"Shift+Alt+Right / Left", "Expand / shrink the selection"},
        {"Alt+Up / Alt+Down", "Move lines up or down"},
        {"Shift+Alt+Up / Down", "Copy lines up or down"},
        {"Ctrl+Shift+K", "Delete the line"},
        {"Ctrl+Enter / Ctrl+Shift+Enter", "Insert a line below / above"},
        {"Ctrl+] / Ctrl+[", "Indent / outdent the line"},
        {"Tab / Shift+Tab", "Indent / outdent the selection"},
        {"Ctrl+/ / Shift+Alt+A", "Toggle line comment / block comment"},
        {"Ctrl+Shift+\\", "Jump to the matching bracket"},
        {"Ctrl+Backspace / Ctrl+Delete", "Delete the word left / right"},
        {"Ctrl+Up / Ctrl+Down", "Scroll by a line without moving the cursor"},
        {"Insert", "Toggle overtype"},
        {"Ctrl+= / Ctrl+- / Ctrl+0", "Zoom the editor font in / out / reset"},
        {"Alt+L", "Go live: serve the folder and open the browser"},
        {"Ctrl+Space", "Ask the model to complete at the caret"},
        {"Tab (on a suggestion)", "Accept the AI suggestion"},
        {"Ctrl+L", "Send the file or selection to the chat"},
        {"Ctrl+,", "Settings"},
    };
    ImGui::BeginChild("##scrows", ImVec2(ui::px(620), ui::px(440)));
    ImGui::PushFont(ui::F.ui);
    if (ImGui::BeginTable("##sc", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const Row& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(ui::F.mono_small);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.accent_hi), "%s", ui::keys(r.keys));
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_dim), "%s", r.what);
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ui::vspace(10);
    if (ui::primary_button("Close", ImVec2(ui::px(110), 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::draw_about_modal() {
    if (show_about_) {
        ImGui::OpenPopup("About###about");
        show_about_ = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("About###about", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::PushFont(ui::F.ui_large);
    ImGui::TextUnformatted("AI IDE");
    ImGui::PopFont();
    ImGui::PushFont(ui::F.ui);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_dim),
                       "A local-first code editor in C++ that talks to your own models.");
    ui::vspace(6);
#if defined(_WIN32)
    ImGui::Text("Rendering: Direct3D 11 + Dear ImGui");
#elif defined(__APPLE__)
    ImGui::Text("Rendering: Metal + GLFW + Dear ImGui");
#else
    ImGui::Text("Rendering: OpenGL 3 + GLFW + Dear ImGui");
#endif
    ImGui::Text("Models: any Ollama-compatible server (%s)", settings_.ollama_host.c_str());
    ImGui::PopFont();
    ui::vspace(10);
    if (ui::primary_button("Close", ImVec2(ui::px(110), 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// --------------------------------------------------------------- shortcuts

void App::handle_shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;
    const bool alt = io.KeyAlt;
    Document* doc = active_document();

    // Exact modifiers, so the editor's Ctrl+Shift+K or Ctrl+Shift+L never also
    // fires the app's Ctrl+K or Ctrl+L.
    auto key = [&](ImGuiKey k, bool c, bool s, bool a, bool repeat = false) {
        return ctrl == c && shift == s && alt == a && ImGui::IsKeyPressed(k, repeat);
    };

    if (key(ImGuiKey_P, true, true, false) || key(ImGuiKey_F1, false, false, false)) open_palette(">");
    if (key(ImGuiKey_P, true, false, false)) open_palette("");
    if (key(ImGuiKey_G, true, false, false) && doc) open_palette(":");
    if (key(ImGuiKey_O, true, true, false) && doc) open_palette("@");
    if (key(ImGuiKey_F, true, true, false)) open_search_view();

    if (key(ImGuiKey_N, true, false, false)) new_document();
    if (key(ImGuiKey_O, true, false, false)) {
        std::string path = sys::pick_open_file(hwnd_, workspace_dir_for_dialogs());
        if (!path.empty()) open_file(path);
    }
    if (key(ImGuiKey_K, true, false, false)) {
        std::string folder = sys::pick_folder(hwnd_, "Open Folder");
        if (!folder.empty()) set_workspace(folder);
    }
    if (key(ImGuiKey_S, true, false, false)) save_document(doc, false);
    if (key(ImGuiKey_S, true, true, false)) save_document(doc, true);
    if (key(ImGuiKey_W, true, false, false) && doc) close_document(active_doc_);
    if (key(ImGuiKey_T, true, true, false)) reopen_closed_document();
    if (key(ImGuiKey_Tab, true, false, false, true) || key(ImGuiKey_PageDown, true, false, false, true))
        cycle_document(1);
    if (key(ImGuiKey_Tab, true, true, false, true) || key(ImGuiKey_PageUp, true, false, false, true))
        cycle_document(-1);

    if (ctrl && !alt && (ImGui::IsKeyPressed(ImGuiKey_Equal, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, true)))
        zoom_editor(1);
    if (ctrl && !alt && (ImGui::IsKeyPressed(ImGuiKey_Minus, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true)))
        zoom_editor(-1);
    if (key(ImGuiKey_0, true, false, false) || key(ImGuiKey_Keypad0, true, false, false)) zoom_editor(0);

    if (key(ImGuiKey_Space, true, false, false)) request_inline_completion();
    if (key(ImGuiKey_L, true, false, false) && doc) {
        focus_chat_input_ = true;
        quick_action("Explain", "Explain what this code does, step by step.", true);
    }
    if (key(ImGuiKey_Comma, true, false, false)) show_settings_ = true;
    if (key(ImGuiKey_L, false, false, true)) open_live_preview();
}

// ------------------------------------------------------------------- frame

void App::draw() {
    update_streams();
    update_process();
    update_search();
    handle_shortcuts();

    title_bar_height_ = std::floor(ui::px(46.0f));

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking |
                                  ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // The host is the desktop the panes are tiled on; its colour is what shows
    // through the gaps between them.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(ui::C.bg_deep));
    ImGui::Begin("##host", nullptr, host_flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    draw_title_bar();

    const float status_h = ui::px(28);
    ImVec2 dock_size = ImGui::GetContentRegionAvail();
    dock_size.y -= status_h;

    ImGuiID dock_id = ImGui::GetID("MainDockSpace");
    if (!layout_initialized_) {
        layout_initialized_ = true;
        if (ImGui::DockBuilderGetNode(dock_id) == nullptr ||
            ImGui::DockBuilderGetNode(dock_id)->IsEmpty()) {
            ImGui::DockBuilderRemoveNode(dock_id);
            ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock_id, dock_size);

            ImGuiID center = dock_id;
            ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.19f, nullptr, &center);
            ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.32f, nullptr, &center);
            ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, nullptr, &center);

            ImGui::DockBuilderDockWindow("Explorer###explorer", left);
            ImGui::DockBuilderDockWindow("AI Assistant###chat", right);
            ImGui::DockBuilderDockWindow("Terminal###terminal", bottom);
            ImGui::DockBuilderDockWindow("Editor###editor", center);
            ImGui::DockBuilderFinish(dock_id);
        }
    }
    // A pane that is alone in its node shows no tab bar, so the layout reads
    // as tiled windows rather than as a set of tabbed docks.
    ImGui::DockSpace(dock_id, dock_size, ImGuiDockNodeFlags_AutoHideTabBar);

    draw_status_bar();
    ImGui::End();

    draw_explorer();
    draw_editor_panel();
    draw_chat_panel();
    draw_terminal_panel();

    draw_settings_modal();
    draw_shortcuts_modal();
    draw_about_modal();
    draw_palette();
    draw_toasts();

    // Focus the editor once the layout has settled, so typing works immediately
    // and one pane is visibly focused. Done here rather than inside the editor
    // panel: a focus set while the later panes are still being created is undone
    // as they appear.
    if (!initial_focus_done_ && ImGui::GetFrameCount() > 3) {
        initial_focus_done_ = true;
        ImGui::SetWindowFocus("Editor###editor");
        if (Document* d = active_document()) d->editor.request_focus();
    }
}

} // namespace app
