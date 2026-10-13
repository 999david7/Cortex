// Find in files (Ctrl+Shift+F): the search view in the explorer pane and the
// worker thread that walks the open folder. Hits stream into the view while
// the walk is still going, the way VS Code fills its results.
#include "app.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <regex>
#include <thread>

#include "imgui.h"
#include "imgui_internal.h"
#include "ui.h"

namespace app {
namespace {

constexpr size_t kMaxHits = 5000;
constexpr long long kMaxFileBytes = 2 * 1024 * 1024;

bool word_char(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

std::string lowered(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A file is treated as binary when its first few kilobytes hold a NUL.
bool looks_binary(const std::string& content) {
    const size_t n = std::min<size_t>(content.size(), 8192);
    return std::memchr(content.data(), '\0', n) != nullptr;
}

bool chip(const char* id, const char* label, bool* value, const char* tip) {
    ImFont* f = ui::F.mono_small ? ui::F.mono_small : ui::F.ui_small;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label);
    const float w = ImMax(h, ts.x + ui::px(10));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) *value = !*value;
    ImDrawList* d = ImGui::GetWindowDrawList();
    if (*value) d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::C.ink);
    else if (hovered) d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::C.bg_surface_hi);
    d->AddRect(p, ImVec2(p.x + w, p.y + h), *value ? ui::C.ink : ui::C.border);
    d->AddText(f, f->FontSize, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
               *value ? ui::C.on_ink : ui::C.text_dim, label);
    if (hovered && tip) ui::tooltip(tip);
    return clicked;
}

} // namespace

void App::run_search(std::shared_ptr<SearchJob> job, std::string root, std::string query,
                     bool case_sensitive, bool whole_word, bool regex) {
    std::unique_ptr<std::regex> re;
    if (regex) {
        try {
            auto flags = std::regex::ECMAScript;
            if (!case_sensitive) flags |= std::regex::icase;
            re.reset(new std::regex(query, flags));
        } catch (const std::regex_error&) {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->error = "Not a valid regular expression";
            job->done = true;
            return;
        }
    }
    const std::string needle = case_sensitive ? query : lowered(query);
    size_t total = 0;

    auto rel_of = [&](const std::string& path) {
        std::string rel = path.size() > root.size() + 1 ? path.substr(root.size() + 1) : path;
        for (char& c : rel)
            if (c == '\\') c = '/';
        return rel;
    };

    // Depth-first, files of a folder before its subfolders, so results come
    // out in the order the tree shows them.
    std::vector<std::string> stack{root};
    while (!stack.empty() && !job->cancel) {
        const std::string dir = stack.back();
        stack.pop_back();
        std::vector<sys::DirEntry> entries;
        if (!sys::list_directory(dir, entries)) continue;
        std::vector<std::string> subdirs;
        for (const sys::DirEntry& e : entries) {
            if (job->cancel) break;
            if (e.is_dir) {
                subdirs.push_back(e.path);
                continue;
            }
            if (e.size > kMaxFileBytes) continue;
            std::string content, err;
            if (!sys::read_file(e.path, content, err) || looks_binary(content)) continue;
            job->files++;

            std::vector<SearchHit> found;
            const std::string rel = rel_of(e.path);
            int line_no = 0;
            size_t start = 0;
            while (start <= content.size() && total + found.size() < kMaxHits) {
                size_t end = content.find('\n', start);
                if (end == std::string::npos) end = content.size();
                std::string line = content.substr(start, end - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();

                auto add = [&](int at, int len) {
                    if (whole_word) {
                        const bool left = at == 0 || !word_char(line[at - 1]);
                        const bool right = at + len >= (int)line.size() || !word_char(line[at + len]);
                        if (!left || !right) return;
                    }
                    SearchHit hit;
                    hit.path = e.path;
                    hit.rel = rel;
                    hit.line = line_no;
                    hit.column = at;
                    hit.length = len;
                    // Keep the preview short and the match in sight in a
                    // narrow pane: indentation dropped, and a long run-up
                    // cut to a few characters behind an ellipsis.
                    size_t from = line.find_first_not_of(" \t");
                    if (from == std::string::npos || (int)from > at) from = (size_t)at;
                    std::string lead;
                    if (at - (int)from > 14) {
                        from = (size_t)(at - 10);
                        while (from > 0 && ((unsigned char)line[from] & 0xC0) == 0x80) --from;
                        lead = "\xE2\x80\xA6"; // …
                    }
                    hit.preview = lead + line.substr(from, 200);
                    hit.preview_offset = (int)lead.size() + at - (int)from;
                    found.push_back(std::move(hit));
                };

                if (re) {
                    for (auto it = std::sregex_iterator(line.begin(), line.end(), *re);
                         it != std::sregex_iterator() && total + found.size() < kMaxHits; ++it) {
                        if (it->length(0) == 0) continue;
                        add((int)it->position(0), (int)it->length(0));
                    }
                } else {
                    const std::string hay = case_sensitive ? line : lowered(line);
                    size_t pos = 0;
                    while ((pos = hay.find(needle, pos)) != std::string::npos && total + found.size() < kMaxHits) {
                        add((int)pos, (int)needle.size());
                        pos += needle.size();
                    }
                }
                if (end == content.size()) break;
                start = end + 1;
                ++line_no;
            }
            if (!found.empty()) {
                total += found.size();
                std::lock_guard<std::mutex> lock(job->mutex);
                for (SearchHit& h : found) job->hits.push_back(std::move(h));
            }
            if (total >= kMaxHits) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->truncated = true;
                job->cancel = true;
                break;
            }
        }
        for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(*it);
    }
    job->done = true;
}

void App::open_search_view() {
    if (workspace_.empty()) {
        toast("Open a folder to search in it", true);
        return;
    }
    explorer_search_ = true;
    search_focus_ = true;
    // Seed the query with the selection, or the word at the caret.
    if (Document* d = active_document()) {
        std::string seed = d->editor.has_selection() ? d->editor.selected_text() : std::string();
        if (!seed.empty() && seed.find('\n') == std::string::npos && seed.size() < sizeof(search_buffer_) - 1) {
            std::snprintf(search_buffer_, sizeof(search_buffer_), "%s", seed.c_str());
            search_pending_ = true;
            search_edit_time_ = -100.0;
        }
    }
    ImGui::SetWindowFocus("Explorer###explorer");
}

void App::start_search() {
    search_pending_ = false;
    if (search_job_) search_job_->cancel = true;
    search_job_.reset();
    search_hits_.clear();
    search_error_.clear();
    search_truncated_ = false;
    search_files_ = 0;
    search_collapsed_.clear();
    const std::string query = search_buffer_;
    search_ran_for_ = query;
    if (query.empty() || workspace_.empty()) return;

    auto job = std::make_shared<SearchJob>();
    search_job_ = job;
    std::thread(run_search, job, workspace_, query, search_case_, search_word_, search_regex_).detach();
}

void App::update_search() {
    if (search_pending_ && ImGui::GetTime() - search_edit_time_ > 0.3) start_search();
    if (!search_job_) return;
    {
        std::lock_guard<std::mutex> lock(search_job_->mutex);
        if (search_job_->hits.size() != search_hits_.size()) search_hits_ = search_job_->hits;
        search_truncated_ = search_job_->truncated;
        search_error_ = search_job_->error;
    }
    search_files_ = search_job_->files;
    if (search_job_->done) {
        std::lock_guard<std::mutex> lock(search_job_->mutex);
        search_hits_ = search_job_->hits;
        search_job_.reset();
    }
}

void App::open_search_hit(const SearchHit& hit) {
    open_file(hit.path);
    Document* doc = active_document();
    if (!doc) return;
    doc->editor.select_range(ed::Coord(hit.line, hit.column), ed::Coord(hit.line, hit.column + hit.length));
    doc->editor.center_on_cursor();
    doc->editor.request_focus();
}

void App::draw_search_view() {
    // --- query row ---
    ImGui::PushFont(ui::F.ui_small);
    if (search_focus_) {
        ImGui::SetKeyboardFocusHere();
        search_focus_ = false;
    }
    const float chips_w = ImGui::GetFrameHeight() * 3.0f + ui::px(20);
    ImGui::SetNextItemWidth(ImMax(ui::px(60), ImGui::GetContentRegionAvail().x - chips_w));
    const bool enter = ImGui::InputTextWithHint("##searchq", "search", search_buffer_, sizeof(search_buffer_),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) {
        search_pending_ = true;
        search_edit_time_ = ImGui::GetTime();
    }
    ImGui::PopFont();
    bool rerun = enter;
    ImGui::SameLine(0, ui::px(3));
    rerun |= chip("##s_case", "Aa", &search_case_, "Match case");
    ImGui::SameLine(0, ui::px(2));
    rerun |= chip("##s_word", "ab", &search_word_, "Match whole word");
    ImGui::SameLine(0, ui::px(2));
    rerun |= chip("##s_regex", ".*", &search_regex_, "Use regular expression");
    if (rerun) start_search();

    // --- status ---
    ImGui::PushFont(ui::F.ui_small);
    {
        int files_with_hits = 0;
        const std::string* last = nullptr;
        for (const SearchHit& h : search_hits_)
            if (!last || *last != h.path) {
                ++files_with_hits;
                last = &h.path;
            }
        char status[160];
        ImU32 col = ui::C.text_faint;
        if (!search_error_.empty()) {
            std::snprintf(status, sizeof(status), "%s", search_error_.c_str());
            col = ui::C.error;
        } else if (search_buffer_[0] == 0) {
            std::snprintf(status, sizeof(status), "Type to search every file in the folder.");
        } else if (search_job_) {
            std::snprintf(status, sizeof(status), "Searching… %d results, %d files read", (int)search_hits_.size(),
                          search_files_);
        } else if (search_hits_.empty()) {
            std::snprintf(status, sizeof(status), "No results in %d files.", search_files_);
        } else {
            std::snprintf(status, sizeof(status), "%d result%s in %d file%s%s", (int)search_hits_.size(),
                          search_hits_.size() == 1 ? "" : "s", files_with_hits, files_with_hits == 1 ? "" : "s",
                          search_truncated_ ? " (capped)" : "");
            if (search_truncated_) col = ui::C.warning;
        }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", status);
    }
    ImGui::PopFont();
    ui::vspace(2);

    // --- results: a file row, then its hits, folded on click ---
    struct Row {
        int hit;      // first hit of the file for a header, or the hit itself
        bool header;
        int count;    // hits in the file, for a header
    };
    std::vector<Row> rows;
    for (int i = 0; i < (int)search_hits_.size();) {
        int j = i;
        while (j < (int)search_hits_.size() && search_hits_[j].path == search_hits_[i].path) ++j;
        rows.push_back({i, true, j - i});
        if (!search_collapsed_.count(search_hits_[i].path))
            for (int k = i; k < j; ++k) rows.push_back({k, false, 0});
        i = j;
    }

    ImGui::BeginChild("##searchresults", ImVec2(0, 0), ImGuiChildFlags_None);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ui::F.ui_small;
    ImFont* mono = ui::F.mono_small ? ui::F.mono_small : font;
    const float row_h = std::floor(font->FontSize + ui::px(8));
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGuiListClipper clipper;
    clipper.Begin((int)rows.size(), row_h);
    int clicked_hit = -1;
    std::string toggle_path;
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const Row& row = rows[r];
            const SearchHit& hit = search_hits_[row.hit];
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(r);
            const bool clicked = ImGui::InvisibleButton("##sr", ImVec2(width, row_h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hovered) dl->AddRectFilled(p, ImVec2(p.x + width, p.y + row_h), ui::C.bg_surface_hi);
            if (row.header) {
                if (clicked) toggle_path = hit.path;
                const bool folded = search_collapsed_.count(hit.path) > 0;
                const float chev = ui::px(9);
                ui::draw_icon(dl, folded ? ui::Icon::ChevronRight : ui::Icon::ChevronDown,
                              ImVec2(p.x + ui::px(2), p.y + (row_h - chev) * 0.5f), chev, ui::C.text_dim);
                const float icon = std::floor(row_h * 0.7f);
                ui::draw_lang_icon(dl, lang::from_path(hit.rel), ImVec2(p.x + ui::px(14), p.y + (row_h - icon) * 0.5f),
                                   icon);
                char count[16];
                std::snprintf(count, sizeof(count), "%d", row.count);
                const ImVec2 cs = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, count);
                const float right = p.x + width - cs.x - ui::px(6);
                dl->AddText(font, font->FontSize, ImVec2(right, p.y + (row_h - cs.y) * 0.5f), ui::C.text_dim, count);
                dl->PushClipRect(p, ImVec2(right - ui::px(6), p.y + row_h), true);
                dl->AddText(font, font->FontSize, ImVec2(p.x + ui::px(20) + icon, p.y + (row_h - font->FontSize) * 0.5f),
                            ui::C.text, hit.rel.c_str());
                dl->PopClipRect();
                if (hovered) ui::tooltip(hit.path.c_str());
            } else {
                if (clicked) clicked_hit = row.hit;
                char num[16];
                std::snprintf(num, sizeof(num), "%d", hit.line + 1);
                const float num_w = mono->CalcTextSizeA(mono->FontSize, FLT_MAX, 0.0f, "00000").x;
                const ImVec2 ns = mono->CalcTextSizeA(mono->FontSize, FLT_MAX, 0.0f, num);
                const float ty = p.y + (row_h - mono->FontSize) * 0.5f;
                dl->AddText(mono, mono->FontSize, ImVec2(p.x + ui::px(14) + num_w - ns.x, ty), ui::C.text_faint, num);
                const float tx = p.x + ui::px(22) + num_w;
                dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + width, p.y + row_h), true);
                // the match itself, marked in the ochre the find bar uses
                const char* text = hit.preview.c_str();
                const int off = ImClamp(hit.preview_offset, 0, (int)hit.preview.size());
                const int end = ImClamp(off + hit.length, off, (int)hit.preview.size());
                const float x0 = tx + mono->CalcTextSizeA(mono->FontSize, FLT_MAX, 0.0f, text, text + off).x;
                const float x1 = tx + mono->CalcTextSizeA(mono->FontSize, FLT_MAX, 0.0f, text, text + end).x;
                dl->AddRectFilled(ImVec2(x0, p.y + ui::px(2)), ImVec2(x1, p.y + row_h - ui::px(2)), ui::C.match_highlight);
                dl->AddText(mono, mono->FontSize, ImVec2(tx, ty), ui::C.syn_default, text);
                dl->PopClipRect();
            }
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
    ImGui::EndChild();

    if (!toggle_path.empty()) {
        if (search_collapsed_.count(toggle_path)) search_collapsed_.erase(toggle_path);
        else search_collapsed_.insert(toggle_path);
    }
    if (clicked_hit >= 0 && clicked_hit < (int)search_hits_.size()) {
        const SearchHit hit = search_hits_[clicked_hit];
        open_search_hit(hit);
    }
}

} // namespace app
