// The command palette and quick open: one overlay under the title bar for
// files (Ctrl+P), commands (Ctrl+Shift+P), a line (Ctrl+G), a symbol in the
// open file (Ctrl+Shift+O) and the small pickers behind the status bar.
#include "app.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <regex>

#include "imgui.h"
#include "imgui_internal.h"
#include "ui.h"

namespace app {
namespace {

int lower(char c) { return std::tolower((unsigned char)c); }

// Case-insensitive subsequence match; -1 when a character of the pattern is
// missing. Runs of consecutive characters and word starts score higher, so
// "tedit" ranks texteditor.cpp above a path that merely contains the letters.
int fuzzy_score(const std::string& pattern, const std::string& text) {
    int score = 0;
    size_t ti = 0;
    int prev = -2;
    bool any = false;
    for (char pc : pattern) {
        if (pc == ' ') continue;
        any = true;
        const int want = lower(pc);
        bool found = false;
        while (ti < text.size()) {
            if (lower(text[ti]) == want) {
                int bonus = 1;
                if ((int)ti == prev + 1) bonus += 5;
                const char before = ti > 0 ? text[ti - 1] : ' ';
                if (ti == 0 || std::strchr(" /\\_-.:", before) ||
                    (std::isupper((unsigned char)text[ti]) && std::islower((unsigned char)before)))
                    bonus += 8;
                score += bonus;
                prev = (int)ti;
                ++ti;
                found = true;
                break;
            }
            ++ti;
        }
        if (!found) return -1;
    }
    if (!any) return 0;
    return score * 4 - (int)text.size() / 6;
}

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::string last_component(const std::string& rel) {
    const size_t slash = rel.find_last_of("/\\");
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

bool c_like(lang::Id id) {
    switch (id) {
        case lang::Id::C: case lang::Id::Cpp: case lang::Id::ObjC: case lang::Id::CSharp:
        case lang::Id::Java: case lang::Id::Cuda: case lang::Id::Glsl: case lang::Id::D:
        case lang::Id::Vala: case lang::Id::Dart: case lang::Id::Groovy:
            return true;
        default:
            return false;
    }
}

bool is_control_word(const std::string& w) {
    static const char* words[] = {"if", "for", "while", "switch", "return", "else", "catch",
                                  "sizeof", "do", "case", "new", "delete", "throw", "using",
                                  "typedef", "decltype", "static_assert", "defined", nullptr};
    for (const char** p = words; *p; ++p)
        if (w == *p) return true;
    return false;
}

} // namespace

// ------------------------------------------------------------ opening

void App::open_palette(const std::string& prefix) {
    if (commands_.empty()) build_commands();
    picker_active_ = false;
    std::snprintf(palette_buffer_, sizeof(palette_buffer_), "%s", prefix.c_str());
    palette_open_ = true;
    palette_focus_ = true;
    palette_cursor_end_ = true;
    palette_frames_ = 0;
    palette_selected_ = 0;
    palette_view_query_ = "\x01"; // force a refresh
    scan_workspace_files();
}

void App::open_picker(const std::string& title, std::vector<PaletteItem> items, int selected) {
    picker_active_ = true;
    picker_title_ = title;
    picker_items_ = std::move(items);
    palette_buffer_[0] = 0;
    palette_open_ = true;
    palette_focus_ = true;
    palette_cursor_end_ = true;
    palette_frames_ = 0;
    palette_selected_ = selected;
    palette_view_query_ = "\x01";
}

// ------------------------------------------------------------ sources

void App::scan_files_in(const std::string& dir, const std::string& rel, int depth, int& budget) {
    if (budget <= 0 || depth > 24) return;
    std::vector<sys::DirEntry> entries;
    if (!sys::list_directory(dir, entries)) return;
    for (const sys::DirEntry& e : entries) {
        if (budget <= 0) {
            workspace_files_truncated_ = true;
            return;
        }
        const std::string child = rel.empty() ? e.name : rel + "/" + e.name;
        if (e.is_dir) {
            scan_files_in(e.path, child, depth + 1, budget);
        } else {
            workspace_files_.push_back(child);
            --budget;
        }
    }
}

void App::scan_workspace_files() {
    const double now = ImGui::GetTime();
    // A recent scan of the same folder is reused; the tree rarely changes
    // between two Ctrl+P presses a few seconds apart.
    if (workspace_files_root_ == workspace_ && now - workspace_files_time_ < 8.0) return;
    workspace_files_.clear();
    workspace_files_truncated_ = false;
    workspace_files_root_ = workspace_;
    workspace_files_time_ = now;
    if (workspace_.empty()) return;
    int budget = 40000;
    scan_files_in(workspace_, std::string(), 0, budget);
}

void App::collect_symbols(std::vector<PaletteItem>& out) {
    out.clear();
    Document* doc = active_document();
    if (!doc) return;
    const ed::TextEditor& e = doc->editor;
    if (symbol_cache_doc_ == (const void*)doc && symbol_cache_version_ == e.version() &&
        !symbol_cache_.empty()) {
        out = symbol_cache_;
        return;
    }

    // Declarations introduced by a keyword, across most languages.
    static const std::regex keyword_re(
        R"(^\s*(?:(?:export|default|public|private|protected|internal|static|pub(?:\([^)]*\))?|async|abstract|final|sealed|open|override|inline|extern|virtual|unsafe|partial|data|local)\s+)*(def|class|function|func|fn|struct|enum|interface|trait|impl|module|namespace|type|union|record|object|protocol|extension|actor|sub|proc|macro|package|mod)\s+([A-Za-z_$][\w$]*))");
    // C-family function definitions: a name and an argument list at the start
    // of an unindented line that is not a statement.
    static const std::regex c_function_re(R"(^[A-Za-z_][\w:<>,\*&\s~]*?\b([A-Za-z_~][\w:~]*)\s*\([^;]*$)");
    static const std::regex heading_re(R"(^(#{1,6})\s+(.+?)\s*#*\s*$)");
    static const std::regex section_re(R"(^\s*\[([^\]]+)\]\s*$)");
    static const std::regex yaml_key_re(R"(^([A-Za-z_][\w.-]*)\s*:)");
    static const std::regex css_rule_re(R"(^([^\s{}@/][^{}]*?)\s*\{\s*$)");

    const lang::Id language = e.language();
    const bool markdown = language == lang::Id::Markdown;
    const bool sections = language == lang::Id::Toml || language == lang::Id::Ini;
    const bool yaml = language == lang::Id::Yaml;
    const bool css = language == lang::Id::Css || language == lang::Id::Scss || language == lang::Id::Less;
    const bool cfam = c_like(language);

    const int n = e.line_count();
    for (int i = 0; i < n && out.size() < 5000; ++i) {
        const std::string& line = e.line(i);
        if (line.empty() || line.size() > 400) continue;
        std::smatch m;
        std::string name, kind;
        int column = 0;
        if (markdown) {
            if (line[0] != '#' || !std::regex_search(line, m, heading_re)) continue;
            name = m[2].str();
            kind = "h" + std::to_string(m[1].length());
            column = (int)m.position(2);
        } else if (sections && std::regex_search(line, m, section_re)) {
            name = m[1].str();
            kind = "section";
            column = (int)m.position(1);
        } else if (yaml && std::regex_search(line, m, yaml_key_re)) {
            name = m[1].str();
            kind = "key";
        } else if (css && std::regex_search(line, m, css_rule_re)) {
            name = trimmed(m[1].str());
            kind = "rule";
        } else if (std::regex_search(line, m, keyword_re)) {
            name = m[2].str();
            kind = m[1].str();
            column = (int)m.position(2);
        } else if (cfam && !std::isspace((unsigned char)line[0]) && line[0] != '#' &&
                   line.find('(') != std::string::npos && std::regex_search(line, m, c_function_re)) {
            name = m[1].str();
            const std::string first_word = line.substr(0, line.find_first_of(" \t(:<*&"));
            if (is_control_word(first_word) || is_control_word(name)) continue;
            kind = "function";
            column = (int)m.position(1);
        } else {
            continue;
        }
        if (name.empty()) continue;
        PaletteItem item;
        item.label = name;
        item.detail = kind;
        char where[32];
        std::snprintf(where, sizeof(where), "%d", i + 1);
        item.keys = where;
        const int line_index = i;
        const int col = column;
        const int len = (int)name.size();
        item.run = [this, line_index, col, len]() {
            editor_command([line_index, col, len](ed::TextEditor& ed) {
                ed.select_range(ed::Coord(line_index, col), ed::Coord(line_index, col + len));
                ed.center_on_cursor();
            });
        };
        out.push_back(std::move(item));
    }
    symbol_cache_ = out;
    symbol_cache_doc_ = (const void*)doc;
    symbol_cache_version_ = e.version();
}

void App::refresh_palette_view() {
    const std::string query = palette_buffer_;
    if (query == palette_view_query_) return;
    const bool first = palette_view_query_ == "\x01";
    palette_view_query_ = query;
    palette_view_.clear();
    const int keep = first ? palette_selected_ : 0;

    auto ranked = [&](std::vector<PaletteItem>& items, const std::string& q, bool sort) {
        for (PaletteItem& it : items) {
            const int score = fuzzy_score(q, it.label);
            if (score < 0) continue;
            it.score = score;
            palette_view_.push_back(it);
        }
        if (sort && !trimmed(q).empty())
            std::stable_sort(palette_view_.begin(), palette_view_.end(),
                             [](const PaletteItem& a, const PaletteItem& b) { return a.score > b.score; });
    };

    if (picker_active_) {
        ranked(picker_items_, query, true);
    } else if (!query.empty() && query[0] == '>') {
        const std::string q = trimmed(query.substr(1));
        if (q.empty()) {
            // Recently used commands first, the way VS Code lists them.
            for (const std::string& label : recent_commands_)
                for (const PaletteItem& c : commands_)
                    if (c.label == label) {
                        PaletteItem r = c;
                        r.detail = "recently used";
                        palette_view_.push_back(r);
                    }
            for (const PaletteItem& c : commands_)
                if (std::find(recent_commands_.begin(), recent_commands_.end(), c.label) ==
                    recent_commands_.end())
                    palette_view_.push_back(c);
        } else {
            ranked(commands_, q, true);
        }
    } else if (!query.empty() && query[0] == ':') {
        Document* doc = active_document();
        PaletteItem item;
        if (!doc) {
            item.label = "Open a file to go to a line";
        } else {
            const ed::TextEditor& e = doc->editor;
            int line = 0, col = 0;
            const char* p = query.c_str() + 1;
            while (*p == ' ') ++p;
            while (*p >= '0' && *p <= '9') line = line * 10 + (*p++ - '0');
            if (*p == ':' || *p == ',') {
                ++p;
                while (*p >= '0' && *p <= '9') col = col * 10 + (*p++ - '0');
            }
            if (line <= 0) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "Current line: %d, column %d. Type a line number between 1 and %d.",
                              e.cursor().line + 1, e.cursor().column + 1, e.line_count());
                item.label = buf;
            } else {
                line = ImMin(line, e.line_count());
                char buf[96];
                if (col > 0) std::snprintf(buf, sizeof(buf), "Go to line %d, column %d", line, col);
                else std::snprintf(buf, sizeof(buf), "Go to line %d", line);
                item.label = buf;
                item.run = [this, line, col]() {
                    editor_command([line, col](ed::TextEditor& ed) { ed.goto_line(line - 1, ImMax(0, col - 1)); });
                };
            }
        }
        palette_view_.push_back(item);
    } else if (!query.empty() && query[0] == '@') {
        std::vector<PaletteItem> symbols;
        collect_symbols(symbols);
        ranked(symbols, trimmed(query.substr(1)), true);
        if (palette_view_.empty()) {
            PaletteItem none;
            none.label = active_document() ? "No symbols found in this file" : "Open a file to list its symbols";
            palette_view_.push_back(none);
        }
    } else if (!query.empty() && query[0] == '?') {
        struct Help { const char* prefix; const char* what; };
        const Help helps[] = {{"", "Go to a file in the open folder"},
                              {">", "Run a command"},
                              {":", "Go to a line (line:column works too)"},
                              {"@", "Go to a symbol in the open file"}};
        for (const Help& h : helps) {
            PaletteItem item;
            item.label = std::string(h.prefix[0] ? h.prefix : "…") + "   " + h.what;
            const std::string prefix = h.prefix;
            item.run = [this, prefix]() { open_palette(prefix); };
            palette_view_.push_back(item);
        }
    } else {
        // Files. Open tabs come first when nothing has been typed. The list
        // was read when the palette opened; typing never walks the disk.
        if (workspace_files_root_ != workspace_) scan_workspace_files();
        const std::string q = trimmed(query);
        if (q.empty()) {
            for (int i = (int)documents_.size() - 1; i >= 0; --i) {
                const Document* d = documents_[i].get();
                PaletteItem item;
                item.label = d->display_name;
                item.detail = d->path.empty() ? "unsaved" : "open";
                item.file_lang = d->editor.language();
                const int index = i;
                item.run = [this, index]() {
                    if (index >= (int)documents_.size()) return;
                    active_doc_ = index;
                    documents_[index]->scroll_to_tab = true;
                    documents_[index]->editor.request_focus();
                };
                palette_view_.push_back(item);
            }
        }
        if (workspace_.empty()) {
            if (palette_view_.empty()) {
                PaletteItem item;
                item.label = "Open a folder to search its files";
                item.keys = "Ctrl+K";
                item.run = [this]() {
                    std::string folder = sys::pick_folder(hwnd_, "Open Folder");
                    if (!folder.empty()) set_workspace(folder);
                };
                palette_view_.push_back(item);
            }
        } else {
            std::vector<PaletteItem> files;
            for (const std::string& rel : workspace_files_) {
                int score = 0;
                if (!q.empty()) {
                    const int whole = fuzzy_score(q, rel);
                    if (whole < 0) continue;
                    // A match inside the file name counts for more than one
                    // spread across the folders.
                    const int name = fuzzy_score(q, last_component(rel));
                    score = ImMax(whole, name >= 0 ? name + 40 : -1);
                }
                PaletteItem item;
                item.label = last_component(rel);
                const size_t slash = rel.find_last_of('/');
                item.detail = slash == std::string::npos ? std::string() : rel.substr(0, slash);
                item.file_lang = lang::from_path(rel);
                item.score = score;
                const std::string absolute = sys::join_path(workspace_, rel);
                item.run = [this, absolute]() {
                    open_file(absolute);
                    if (Document* d = active_document()) d->editor.request_focus();
                };
                files.push_back(std::move(item));
            }
            if (!q.empty())
                std::stable_sort(files.begin(), files.end(),
                                 [](const PaletteItem& a, const PaletteItem& b) { return a.score > b.score; });
            if (files.size() > 300) files.resize(300);
            for (PaletteItem& f : files) palette_view_.push_back(std::move(f));
        }
    }
    palette_selected_ = palette_view_.empty() ? 0 : ImClamp(keep, 0, (int)palette_view_.size() - 1);
    palette_scroll_ = true;
}

// ------------------------------------------------------------ drawing

namespace {

struct PaletteInputState {
    int* selected;
    int count;
    bool* scroll;
    bool* cursor_end;
};

int palette_input_callback(ImGuiInputTextCallbackData* data) {
    PaletteInputState* st = (PaletteInputState*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && st->count > 0) {
        if (data->EventKey == ImGuiKey_UpArrow)
            *st->selected = (*st->selected - 1 + st->count) % st->count;
        else if (data->EventKey == ImGuiKey_DownArrow)
            *st->selected = (*st->selected + 1) % st->count;
        *st->scroll = true;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && *st->cursor_end) {
        // Opening with ">" leaves the caret after it, ready for the command.
        data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
        *st->cursor_end = false;
    }
    return 0;
}

} // namespace

void App::draw_palette() {
    if (!palette_open_) return;
    refresh_palette_view();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = ImMin(ui::px(680), vp->WorkSize.x - ui::px(40));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + std::floor((vp->WorkSize.x - w) * 0.5f),
                                   vp->WorkPos.y + title_bar_height_ + ui::px(6)));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    if (palette_frames_ == 0) ImGui::SetNextWindowFocus();

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(ui::C.bg_panel));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(ui::C.ink));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(8), ui::px(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(6), ui::px(6)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::Begin("##palette", nullptr, flags);

    std::function<void()> to_run;
    bool close = false;

    if (picker_active_) {
        ImFont* micro = ui::F.ui_micro ? ui::F.ui_micro : ui::F.ui_small;
        ui::caps_text(picker_title_.c_str(), ui::C.text_dim, micro);
    }

    ImGui::PushFont(ui::F.ui);
    if (palette_focus_) {
        ImGui::SetKeyboardFocusHere();
        palette_focus_ = false;
    }
    const char* hint = picker_active_ ? "type to filter"
                                      : "file name · > command · : line · @ symbol · ? help";
    PaletteInputState st{&palette_selected_, (int)palette_view_.size(), &palette_scroll_, &palette_cursor_end_};
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputTextWithHint(
        "##palette_input", hint, palette_buffer_, sizeof(palette_buffer_),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackAlways,
        palette_input_callback, &st);
    ImGui::PopFont();

    // The list follows edits immediately, not a frame late.
    refresh_palette_view();
    const int count = (int)palette_view_.size();
    if (count > 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) {
            palette_selected_ = ImMin(count - 1, palette_selected_ + 10);
            palette_scroll_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) {
            palette_selected_ = ImMax(0, palette_selected_ - 10);
            palette_scroll_ = true;
        }
    }
    if (enter && count > 0 && palette_selected_ < count) {
        to_run = palette_view_[palette_selected_].run;
        if (!picker_active_ && !palette_view_query_.empty() && palette_view_query_[0] == '>') {
            const std::string label = palette_view_[palette_selected_].label;
            recent_commands_.erase(std::remove(recent_commands_.begin(), recent_commands_.end(), label),
                                   recent_commands_.end());
            recent_commands_.insert(recent_commands_.begin(), label);
            if (recent_commands_.size() > 8) recent_commands_.resize(8);
        }
        close = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;

    // --- the list ---
    ImFont* label_font = ui::F.ui;
    ImFont* small_font = ui::F.ui_small;
    ImFont* key_font = ui::F.mono_small ? ui::F.mono_small : ui::F.ui_small;
    const float row_h = std::floor(label_font->FontSize + ui::px(12));
    const int visible = ImMin(count, 14);
    if (count > 0) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::BeginChild("##palette_list", ImVec2(0, visible * row_h), ImGuiChildFlags_None);
        const float list_w = ImGui::GetContentRegionAvail().x;
        if (palette_scroll_) {
            const float top = palette_selected_ * row_h;
            const float sy = ImGui::GetScrollY();
            const float vh = ImGui::GetWindowHeight();
            if (top < sy) ImGui::SetScrollY(top);
            else if (top + row_h > sy + vh) ImGui::SetScrollY(top + row_h - vh);
            palette_scroll_ = false;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool mouse_moved = ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f;
        ImGuiListClipper clipper;
        clipper.Begin(count, row_h);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const PaletteItem& item = palette_view_[i];
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushID(i);
                const bool clicked = ImGui::InvisibleButton("##row", ImVec2(list_w, row_h));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hovered && mouse_moved) palette_selected_ = i;
                if (clicked) {
                    to_run = item.run;
                    close = true;
                }
                const bool selected = i == palette_selected_;
                if (selected) dl->AddRectFilled(p, ImVec2(p.x + list_w, p.y + row_h), ui::C.ink);
                else if (hovered) dl->AddRectFilled(p, ImVec2(p.x + list_w, p.y + row_h), ui::C.bg_surface_hi);

                float x = p.x + ui::px(8);
                if (item.file_lang != lang::Id::Count) {
                    const float icon = std::floor(row_h * 0.62f);
                    ui::draw_lang_icon(dl, item.file_lang, ImVec2(x, p.y + (row_h - icon) * 0.5f), icon);
                    x += icon + ui::px(8);
                }
                const ImU32 label_col = selected ? ui::C.on_ink : ui::C.text;
                const ImU32 dim_col = selected ? IM_COL32(0xB8, 0xB8, 0xB4, 0xFF) : ui::C.text_dim;

                // keys on the right
                float right = p.x + list_w - ui::px(8);
                if (!item.keys.empty()) {
                    const ImVec2 ks = key_font->CalcTextSizeA(key_font->FontSize, FLT_MAX, 0.0f, item.keys.c_str());
                    right -= ks.x;
                    dl->AddText(key_font, key_font->FontSize, ImVec2(right, p.y + (row_h - ks.y) * 0.5f),
                                dim_col, item.keys.c_str());
                    right -= ui::px(12);
                }
                dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(right, p.y + row_h), true);
                const ImVec2 ls = label_font->CalcTextSizeA(label_font->FontSize, FLT_MAX, 0.0f, item.label.c_str());
                dl->AddText(label_font, label_font->FontSize, ImVec2(x, p.y + (row_h - ls.y) * 0.5f), label_col,
                            item.label.c_str());
                if (!item.detail.empty()) {
                    const ImVec2 ds = small_font->CalcTextSizeA(small_font->FontSize, FLT_MAX, 0.0f, item.detail.c_str());
                    dl->AddText(small_font, small_font->FontSize,
                                ImVec2(x + ls.x + ui::px(10), p.y + (row_h - ds.y) * 0.5f), dim_col,
                                item.detail.c_str());
                }
                dl->PopClipRect();
            }
        }
        clipper.End();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    } else {
        ImGui::PushFont(ui::F.ui_small);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_faint), "%s",
                           picker_active_ ? "No matching choices"
                                          : (palette_buffer_[0] == '>' ? "No matching commands"
                                                                       : "No matching files"));
        ImGui::PopFont();
    }
    if (workspace_files_truncated_ && !picker_active_ && palette_buffer_[0] != '>' && palette_buffer_[0] != ':' &&
        palette_buffer_[0] != '@') {
        ImGui::PushFont(ui::F.ui_small);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.warning),
                           "Large folder: only the first 40000 files are listed");
        ImGui::PopFont();
    }

    // Clicking anywhere else dismisses it, as VS Code's does.
    if (palette_frames_ > 1 && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) close = true;

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ++palette_frames_;

    if (close) {
        palette_open_ = false;
        picker_active_ = false;
        if (to_run) {
            to_run();
        } else if (Document* d = active_document()) {
            d->editor.request_focus();
        }
    }
}

// ------------------------------------------------------------ pickers

void App::pick_language() {
    Document* doc = active_document();
    if (!doc) return;
    std::vector<PaletteItem> items;
    for (int k = 0; k < (int)lang::Id::Count; ++k) {
        const lang::Id id = (lang::Id)k;
        PaletteItem item;
        item.label = lang::name(id);
        item.detail = id == doc->editor.language() ? "current" : std::string(lang::tag(id));
        item.file_lang = id;
        item.run = [this, id]() {
            if (Document* d = active_document()) {
                d->editor.set_language(id);
                d->editor.request_focus();
            }
        };
        items.push_back(std::move(item));
    }
    std::stable_sort(items.begin(), items.end(), [](const PaletteItem& a, const PaletteItem& b) {
        std::string x = a.label, y = b.label;
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return x < y;
    });
    int selected = 0;
    for (int i = 0; i < (int)items.size(); ++i)
        if (items[i].detail == "current") selected = i;
    open_picker("select language mode", std::move(items), selected);
}

void App::pick_cursor_style(bool overtype) {
    std::vector<PaletteItem> items;
    int* target = overtype ? &settings_.overtype_cursor_style : &settings_.cursor_style;
    for (int k = 0; k < (int)ed::CursorStyle::Count; ++k) {
        PaletteItem item;
        item.label = ed::cursor_style_name((ed::CursorStyle)k);
        if (*target == k) item.detail = "current";
        item.run = [this, target, k]() {
            *target = k;
            settings_.save();
            if (Document* d = active_document()) d->editor.request_focus();
        };
        items.push_back(std::move(item));
    }
    open_picker(overtype ? "overtype cursor style" : "cursor style", std::move(items), *target);
}

void App::pick_cursor_blinking() {
    std::vector<PaletteItem> items;
    for (int k = 0; k < (int)ed::CursorBlink::Count; ++k) {
        PaletteItem item;
        item.label = ed::cursor_blink_name((ed::CursorBlink)k);
        if (settings_.cursor_blinking == k) item.detail = "current";
        item.run = [this, k]() {
            settings_.cursor_blinking = k;
            settings_.save();
            if (Document* d = active_document()) d->editor.request_focus();
        };
        items.push_back(std::move(item));
    }
    open_picker("cursor blinking", std::move(items), settings_.cursor_blinking);
}

void App::pick_whitespace() {
    std::vector<PaletteItem> items;
    for (int k = 0; k < (int)ed::Whitespace::Count; ++k) {
        PaletteItem item;
        item.label = ed::whitespace_name((ed::Whitespace)k);
        if (settings_.render_whitespace == k) item.detail = "current";
        item.run = [this, k]() {
            settings_.render_whitespace = k;
            settings_.save();
            if (Document* d = active_document()) d->editor.request_focus();
        };
        items.push_back(std::move(item));
    }
    open_picker("render whitespace", std::move(items), settings_.render_whitespace);
}

void App::pick_indentation() {
    std::vector<PaletteItem> items;
    auto add = [&](const std::string& label, const std::string& detail, std::function<void()> fn) {
        PaletteItem item;
        item.label = label;
        item.detail = detail;
        item.run = [this, fn]() {
            fn();
            settings_.save();
            if (Document* d = active_document()) d->editor.request_focus();
        };
        items.push_back(std::move(item));
    };
    add("Indent Using Spaces", settings_.insert_spaces ? "current" : "", [this]() { settings_.insert_spaces = true; });
    add("Indent Using Tabs", settings_.insert_spaces ? "" : "current", [this]() { settings_.insert_spaces = false; });
    for (int size : {2, 4, 8}) {
        add("Tab Size: " + std::to_string(size), settings_.tab_size == size ? "current" : "",
            [this, size]() { settings_.tab_size = size; });
    }
    add("Convert Indentation to Spaces", "", [this]() {
        if (Document* d = active_document()) d->editor.convert_indentation(false);
    });
    add("Convert Indentation to Tabs", "", [this]() {
        if (Document* d = active_document()) d->editor.convert_indentation(true);
    });
    open_picker("indentation", std::move(items), settings_.insert_spaces ? 0 : 1);
}

void App::pick_line_endings() {
    Document* doc = active_document();
    if (!doc) return;
    std::vector<PaletteItem> items;
    for (int k = 0; k < 2; ++k) {
        const bool crlf = k == 1;
        PaletteItem item;
        item.label = crlf ? "CRLF" : "LF";
        item.detail = crlf ? "Windows line endings" : "Unix line endings";
        if (doc->editor.use_crlf() == crlf) item.detail += " · current";
        item.run = [this, crlf]() {
            if (Document* d = active_document()) {
                d->editor.set_use_crlf(crlf);
                d->editor.request_focus();
            }
        };
        items.push_back(std::move(item));
    }
    open_picker("end of line sequence", std::move(items), doc->editor.use_crlf() ? 1 : 0);
}

// ------------------------------------------------------------ commands

void App::build_commands() {
    commands_.clear();
    auto add = [&](const char* label, const char* keys, std::function<void()> fn) {
        PaletteItem item;
        item.label = label;
        item.keys = keys ? keys : "";
        item.run = std::move(fn);
        commands_.push_back(std::move(item));
    };
    // Editor commands say so when there is no file to work on.
    auto edit = [&](const char* label, const char* keys, std::function<void(ed::TextEditor&)> fn) {
        add(label, keys, [this, fn]() {
            if (!active_document()) {
                toast("Open a file first", true);
                return;
            }
            editor_command(fn);
        });
    };
    auto toggle = [&](const char* label, bool* value) {
        add(label, nullptr, [this, value, label]() {
            *value = !*value;
            settings_.save();
            toast(std::string(label).substr(std::string(label).find(':') + 2) + (*value ? ": on" : ": off"));
            if (Document* d = active_document()) d->editor.request_focus();
        });
    };

    // File
    add("File: New File", "Ctrl+N", [this]() { new_document(); });
    add("File: Open File...", "Ctrl+O", [this]() {
        std::string path = sys::pick_open_file(hwnd_, workspace_dir_for_dialogs());
        if (!path.empty()) open_file(path);
    });
    add("File: Open Folder...", "Ctrl+K", [this]() {
        std::string folder = sys::pick_folder(hwnd_, "Open Folder");
        if (!folder.empty()) set_workspace(folder);
    });
    add("File: Save", "Ctrl+S", [this]() { save_document(active_document(), false); });
    add("File: Save As...", "Ctrl+Shift+S", [this]() { save_document(active_document(), true); });
    add("File: Save All", nullptr, [this]() { save_all(); });
    add("File: Revert File", nullptr, [this]() { revert_document(active_document()); });
    add("View: Close Editor", "Ctrl+W", [this]() { close_document(active_doc_); });
    add("View: Close All Editors", nullptr, [this]() { close_documents(CloseMode::All, active_doc_); });
    add("View: Close Other Editors", nullptr, [this]() { close_documents(CloseMode::Others, active_doc_); });
    add("View: Close Saved Editors", nullptr, [this]() { close_documents(CloseMode::Saved, active_doc_); });
    add("View: Reopen Closed Editor", "Ctrl+Shift+T", [this]() { reopen_closed_document(); });
    add("View: Next Editor", "Ctrl+Tab", [this]() { cycle_document(1); });
    add("View: Previous Editor", "Ctrl+Shift+Tab", [this]() { cycle_document(-1); });
    add("File: Copy Path of Active File", nullptr, [this]() {
        if (Document* d = active_document(); d && !d->path.empty()) {
            ImGui::SetClipboardText(d->path.c_str());
            toast("Copied " + d->path);
        }
    });
    add("File: Reveal Active File in Explorer", nullptr, [this]() {
        if (Document* d = active_document(); d && !d->path.empty()) sys::open_in_explorer(d->path);
    });

    // Edit
    edit("Undo", "Ctrl+Z", [](ed::TextEditor& e) { e.undo(); });
    edit("Redo", "Ctrl+Y", [](ed::TextEditor& e) { e.redo(); });
    edit("Edit: Cut", "Ctrl+X", [](ed::TextEditor& e) { e.cut(); });
    edit("Edit: Copy", "Ctrl+C", [](ed::TextEditor& e) { e.copy(); });
    edit("Edit: Paste", "Ctrl+V", [](ed::TextEditor& e) { e.paste(); });
    edit("Edit: Find", "Ctrl+F", [](ed::TextEditor& e) { e.open_find(); });
    edit("Edit: Replace", "Ctrl+H", [](ed::TextEditor& e) { e.open_replace(); });
    add("Search: Find in Files", "Ctrl+Shift+F", [this]() { open_search_view(); });
    edit("Edit: Find Next", "F3", [](ed::TextEditor& e) { e.find_next(); });
    edit("Edit: Find Previous", "Shift+F3", [](ed::TextEditor& e) { e.find_previous(); });
    edit("Toggle Line Comment", "Ctrl+/", [](ed::TextEditor& e) { e.toggle_line_comment(); });
    edit("Toggle Block Comment", "Shift+Alt+A", [](ed::TextEditor& e) { e.toggle_block_comment(); });
    edit("Delete Line", "Ctrl+Shift+K", [](ed::TextEditor& e) { e.delete_lines(); });
    edit("Insert Line Below", "Ctrl+Enter", [](ed::TextEditor& e) { e.insert_line_below(); });
    edit("Insert Line Above", "Ctrl+Shift+Enter", [](ed::TextEditor& e) { e.insert_line_above(); });
    edit("Indent Line", "Ctrl+]", [](ed::TextEditor& e) { e.indent_lines(); });
    edit("Outdent Line", "Ctrl+[", [](ed::TextEditor& e) { e.outdent_lines(); });
    edit("Join Lines", nullptr, [](ed::TextEditor& e) { e.join_lines(); });
    edit("Transpose Characters", nullptr, [](ed::TextEditor& e) { e.transpose_chars(); });
    edit("Sort Lines Ascending", nullptr, [](ed::TextEditor& e) { e.sort_lines(false); });
    edit("Sort Lines Descending", nullptr, [](ed::TextEditor& e) { e.sort_lines(true); });
    edit("Reverse Lines", nullptr, [](ed::TextEditor& e) { e.reverse_lines(); });
    edit("Delete Duplicate Lines", nullptr, [](ed::TextEditor& e) { e.delete_duplicate_lines(); });
    edit("Trim Trailing Whitespace", nullptr, [](ed::TextEditor& e) { e.trim_trailing_whitespace(); });
    edit("Delete All Left", "Ctrl+Shift+Backspace", [](ed::TextEditor& e) { e.delete_to_line_edge(-1); });
    edit("Delete All Right", "Ctrl+Shift+Delete", [](ed::TextEditor& e) { e.delete_to_line_edge(1); });
    edit("Toggle Overtype/Insert Mode", "Insert", [](ed::TextEditor& e) { e.toggle_overtype(); });
    for (int k = 0; k < (int)ed::CaseKind::Count; ++k) {
        const ed::CaseKind kind = (ed::CaseKind)k;
        static std::string labels[(int)ed::CaseKind::Count];
        labels[k] = std::string("Transform to ") + ed::case_name(kind);
        edit(labels[k].c_str(), nullptr, [kind](ed::TextEditor& e) { e.transform_case(kind); });
    }
    edit("Convert Indentation to Spaces", nullptr, [](ed::TextEditor& e) { e.convert_indentation(false); });
    edit("Convert Indentation to Tabs", nullptr, [](ed::TextEditor& e) { e.convert_indentation(true); });
    add("Indent Using Spaces", nullptr, [this]() { settings_.insert_spaces = true; });
    add("Indent Using Tabs", nullptr, [this]() { settings_.insert_spaces = false; });
    add("Change Indentation...", nullptr, [this]() { pick_indentation(); });
    add("Change End of Line Sequence...", nullptr, [this]() { pick_line_endings(); });
    add("Change Language Mode...", nullptr, [this]() { pick_language(); });

    // Selection & cursors
    edit("Select All", "Ctrl+A", [](ed::TextEditor& e) { e.select_all(); });
    edit("Expand Selection", "Shift+Alt+Right", [](ed::TextEditor& e) { e.expand_selection(); });
    edit("Shrink Selection", "Shift+Alt+Left", [](ed::TextEditor& e) { e.shrink_selection(); });
    edit("Expand Line Selection", nullptr, [](ed::TextEditor& e) { e.select_line(); });
    edit("Select to Bracket", nullptr, [](ed::TextEditor& e) { e.select_to_bracket(); });
    edit("Go to Bracket", "Ctrl+Shift+\\", [](ed::TextEditor& e) { e.goto_matching_bracket(); });
    edit("Copy Line Up", "Shift+Alt+Up", [](ed::TextEditor& e) { e.copy_lines(-1); });
    edit("Copy Line Down", "Shift+Alt+Down", [](ed::TextEditor& e) { e.copy_lines(1); });
    edit("Move Line Up", "Alt+Up", [](ed::TextEditor& e) { e.move_lines(-1); });
    edit("Move Line Down", "Alt+Down", [](ed::TextEditor& e) { e.move_lines(1); });
    edit("Duplicate Selection", nullptr, [](ed::TextEditor& e) { e.duplicate_selection(); });
    edit("Add Cursor Above", "Ctrl+Alt+Up", [](ed::TextEditor& e) { e.add_caret_vertical(-1); });
    edit("Add Cursor Below", "Ctrl+Alt+Down", [](ed::TextEditor& e) { e.add_caret_vertical(1); });
    edit("Add Cursors to Line Ends", "Shift+Alt+I", [](ed::TextEditor& e) { e.carets_to_line_ends(); });
    edit("Add Selection to Next Find Match", "Ctrl+D", [](ed::TextEditor& e) { e.add_next_occurrence(); });
    edit("Select All Occurrences of Find Match", "Ctrl+Shift+L", [](ed::TextEditor& e) { e.select_all_occurrences(); });
    edit("Cursor Undo", "Ctrl+U", [](ed::TextEditor& e) { e.cursor_undo(); });
    edit("Remove Secondary Cursors", "Escape", [](ed::TextEditor& e) { e.collapse_carets(); });

    // Go
    add("Go to File...", "Ctrl+P", [this]() { open_palette(""); });
    add("Go to Line/Column...", "Ctrl+G", [this]() { open_palette(":"); });
    add("Go to Symbol in Editor...", "Ctrl+Shift+O", [this]() { open_palette("@"); });

    // View & preferences
    add("Preferences: Open Settings", "Ctrl+,", [this]() { show_settings_ = true; });
    add("Preferences: Keyboard Shortcuts", nullptr, [this]() { show_shortcuts_ = true; });
    add("Preferences: Cursor Style...", nullptr, [this]() { pick_cursor_style(false); });
    add("Preferences: Overtype Cursor Style...", nullptr, [this]() { pick_cursor_style(true); });
    add("Preferences: Cursor Blinking...", nullptr, [this]() { pick_cursor_blinking(); });
    toggle("Preferences: Smooth Caret Animation", &settings_.smooth_caret);
    add("View: Render Whitespace...", nullptr, [this]() { pick_whitespace(); });
    toggle("View: Toggle Minimap", &settings_.minimap);
    toggle("View: Toggle Bracket Pair Colorization", &settings_.bracket_pair_colors);
    toggle("View: Toggle Matching Bracket Highlight", &settings_.match_brackets);
    toggle("View: Toggle Occurrence Highlight", &settings_.highlight_occurrences);
    toggle("View: Toggle Line Numbers", &settings_.show_line_numbers);
    toggle("View: Toggle Indent Guides", &settings_.indent_guides);
    toggle("View: Toggle Current Line Highlight", &settings_.highlight_current_line);
    toggle("Preferences: Toggle Auto Closing Brackets", &settings_.auto_close_brackets);
    add("View: Zoom In", "Ctrl+=", [this]() { zoom_editor(1); });
    add("View: Zoom Out", "Ctrl+-", [this]() { zoom_editor(-1); });
    add("View: Reset Zoom", "Ctrl+0", [this]() { zoom_editor(0); });
    add("View: Reset Layout", nullptr, [this]() {
        layout_initialized_ = false;
        toast("Layout reset");
    });
    add("View: Show Explorer", nullptr, [this]() {
        explorer_search_ = false;
        ImGui::SetWindowFocus("Explorer###explorer");
    });
    add("Live Server: Go Live", "Alt+L", [this]() { open_live_preview(); });
    add("Live Server: Stop", nullptr, [this]() { stop_live_server(); });
    add("Terminal: Run Default Command", nullptr, [this]() { run_command(terminal_command_); });

    // AI
    add("AI: Complete at Caret", "Ctrl+Space", [this]() { request_inline_completion(); });
    add("AI: Explain", "Ctrl+L", [this]() {
        quick_action("Explain", "Explain what this code does, step by step.", true);
    });
    add("AI: Fix Bugs", nullptr, [this]() {
        quick_action("Fix", "Find bugs or edge cases in this code and fix them.", true);
    });
    add("AI: Write Tests", nullptr, [this]() {
        quick_action("Tests", "Write focused unit tests for this code.", true);
    });
    add("AI: New Chat", nullptr, [this]() {
        stop_chat();
        chat_.clear();
        agent_history_.clear();
        agent_steps_ = 0;
    });
    add("AI: Reload Models", nullptr, [this]() { refresh_models(true); });
    add("Help: About", nullptr, [this]() { show_about_ = true; });
}

} // namespace app
