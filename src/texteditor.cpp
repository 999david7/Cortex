#include "texteditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_set>

#include "imgui_internal.h"
#include "ui.h"

namespace ed {
namespace {

// ------------------------------------------------------------- languages


const std::unordered_set<std::string>& word_set(const char* const* words) {
    static std::map<const void*, std::unordered_set<std::string>> cache;
    auto it = cache.find((const void*)words);
    if (it != cache.end()) return it->second;
    std::unordered_set<std::string> set;
    for (const char* const* p = words; *p; ++p) set.insert(*p);
    return cache.emplace((const void*)words, std::move(set)).first->second;
}

inline bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
           (unsigned char)c >= 0x80;
}
inline bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_space(char c) { return c == ' ' || c == '\t'; }

// ------------------------------------------------------------- tokenizer

enum class Tok : unsigned char {
    Default, Keyword, Type, String, Number, Comment, Preproc, Function, Punct
};

struct Token {
    int start;
    int end;
    Tok kind;
};

// state bits carried across lines
enum : unsigned char {
    kStateBlockComment = 1 << 0,
    kStateTripleSingle = 1 << 1,
    kStateTripleDouble = 1 << 2,
    kStateBacktick    = 1 << 3,
    kStateFence       = 1 << 4, // markdown code fence
};

void push_token(std::vector<Token>& out, int start, int end, Tok kind) {
    if (end <= start) return;
    if (!out.empty() && out.back().kind == kind && out.back().end == start) {
        out.back().end = end;
        return;
    }
    out.push_back({start, end, kind});
}

unsigned char tokenize_markdown(const std::string& s, unsigned char state, std::vector<Token>& out) {
    const int n = (int)s.size();
    int i = 0;
    while (i < n && is_space(s[i])) ++i;

    if (n - i >= 3 && s.compare(i, 3, "```") == 0) {
        push_token(out, 0, n, Tok::Preproc);
        return state ^ kStateFence;
    }
    if (state & kStateFence) {
        push_token(out, 0, n, Tok::Default);
        return state;
    }
    if (i < n && s[i] == '#') {
        push_token(out, 0, n, Tok::Keyword);
        return state;
    }
    if (i < n && (s[i] == '>' )) {
        push_token(out, 0, n, Tok::Comment);
        return state;
    }
    if (i < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && i + 1 < n && s[i + 1] == ' ') {
        push_token(out, i, i + 1, Tok::Number);
        i += 1;
    }

    while (i < n) {
        if (s[i] == '`') {
            int j = i + 1;
            while (j < n && s[j] != '`') ++j;
            push_token(out, i, std::min(j + 1, n), Tok::String);
            i = j + 1;
        } else if (s[i] == '*' && i + 1 < n && s[i + 1] == '*') {
            int j = (int)s.find("**", i + 2);
            if (j == (int)std::string::npos) { push_token(out, i, n, Tok::Default); break; }
            push_token(out, i, j + 2, Tok::Type);
            i = j + 2;
        } else if (s[i] == '[') {
            int j = (int)s.find(')', i);
            if (j == (int)std::string::npos) { push_token(out, i, i + 1, Tok::Default); ++i; continue; }
            push_token(out, i, j + 1, Tok::Function);
            i = j + 1;
        } else {
            push_token(out, i, i + 1, Tok::Default);
            ++i;
        }
    }
    return state;
}

unsigned char tokenize(Language id, const std::string& s, unsigned char state,
                       std::vector<Token>& out) {
    out.clear();
    if (id == Language::Markdown) return tokenize_markdown(s, state, out);

    const lang::Spec& spec = lang::spec(id);
    const auto& keywords = word_set(spec.keywords);
    const auto& types = word_set(spec.types);
    const int n = (int)s.size();
    int i = 0;

    // Continue a block comment started on a previous line.
    if (state & kStateBlockComment) {
        const char* close = spec.block_close;
        size_t pos = *close ? s.find(close) : std::string::npos;
        if (pos == std::string::npos) {
            push_token(out, 0, n, Tok::Comment);
            return state;
        }
        int end = (int)pos + (int)std::strlen(close);
        push_token(out, 0, end, Tok::Comment);
        i = end;
        state &= (unsigned char)~kStateBlockComment;
    }
    // Continue a Python triple-quoted string.
    if (state & (kStateTripleSingle | kStateTripleDouble)) {
        const char* close = (state & kStateTripleDouble) ? "\"\"\"" : "'''";
        size_t pos = s.find(close, (size_t)i);
        if (pos == std::string::npos) {
            push_token(out, i, n, Tok::String);
            return state;
        }
        int end = (int)pos + 3;
        push_token(out, i, end, Tok::String);
        i = end;
        state &= (unsigned char)~(kStateTripleSingle | kStateTripleDouble);
    }
    // Continue a JS template literal.
    if (state & kStateBacktick) {
        int j = i;
        while (j < n && s[j] != '`') { if (s[j] == '\\') ++j; ++j; }
        if (j >= n) { push_token(out, i, n, Tok::String); return state; }
        push_token(out, i, j + 1, Tok::String);
        i = j + 1;
        state &= (unsigned char)~kStateBacktick;
    }

    // Preprocessor directive (whole line, minus trailing comment).
    if (spec.preproc_hash) {
        int k = i;
        while (k < n && is_space(s[k])) ++k;
        if (k < n && s[k] == '#') {
            size_t c = std::string::npos;
            if (*spec.line_comment) c = s.find(spec.line_comment, (size_t)k);
            int end = c == std::string::npos ? n : (int)c;
            push_token(out, i, end, Tok::Preproc);
            i = end;
        }
    }

    while (i < n) {
        const char c = s[i];

        if (is_space(c)) {
            int j = i;
            while (j < n && is_space(s[j])) ++j;
            push_token(out, i, j, Tok::Default);
            i = j;
            continue;
        }

        // line comment
        if (*spec.line_comment && s.compare(i, std::strlen(spec.line_comment), spec.line_comment) == 0) {
            push_token(out, i, n, Tok::Comment);
            return state;
        }
        if (*spec.line_comment_alt &&
            s.compare(i, std::strlen(spec.line_comment_alt), spec.line_comment_alt) == 0) {
            push_token(out, i, n, Tok::Comment);
            return state;
        }
        // block comment
        if (*spec.block_open && s.compare(i, std::strlen(spec.block_open), spec.block_open) == 0) {
            size_t pos = s.find(spec.block_close, i + std::strlen(spec.block_open));
            if (pos == std::string::npos) {
                push_token(out, i, n, Tok::Comment);
                return (unsigned char)(state | kStateBlockComment);
            }
            int end = (int)pos + (int)std::strlen(spec.block_close);
            push_token(out, i, end, Tok::Comment);
            i = end;
            continue;
        }
        // triple-quoted strings
        if (spec.triple_quotes && (s.compare(i, 3, "\"\"\"") == 0 || s.compare(i, 3, "'''") == 0)) {
            bool dbl = s[i] == '"';
            const char* close = dbl ? "\"\"\"" : "'''";
            size_t pos = s.find(close, (size_t)i + 3);
            if (pos == std::string::npos) {
                push_token(out, i, n, Tok::String);
                return (unsigned char)(state | (dbl ? kStateTripleDouble : kStateTripleSingle));
            }
            int end = (int)pos + 3;
            push_token(out, i, end, Tok::String);
            i = end;
            continue;
        }
        // strings
        if (c == '"' || (c == '\'' && !spec.char_literals) ||
            (c == '\'' && spec.char_literals) ||
            (c == '`' && spec.backtick_strings)) {
            const char quote = c;
            int j = i + 1;
            bool closed = false;
            while (j < n) {
                if (s[j] == '\\') { j += 2; continue; }
                if (s[j] == quote) { closed = true; ++j; break; }
                ++j;
            }
            push_token(out, i, std::min(j, n), Tok::String);
            i = std::min(j, n);
            if (!closed && quote == '`' && spec.backtick_strings)
                return (unsigned char)(state | kStateBacktick);
            continue;
        }
        // numbers
        if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i + 1]))) {
            int j = i;
            while (j < n && (is_ident(s[j]) || s[j] == '.' ||
                             ((s[j] == '+' || s[j] == '-') && j > i &&
                              (s[j - 1] == 'e' || s[j - 1] == 'E'))))
                ++j;
            push_token(out, i, j, Tok::Number);
            i = j;
            continue;
        }
        // identifiers
        if (is_ident_start(c)) {
            int j = i;
            while (j < n && is_ident(s[j])) ++j;
            std::string word = s.substr(i, j - i);
            Tok kind = Tok::Default;
            if (keywords.count(word)) kind = Tok::Keyword;
            else if (types.count(word)) kind = Tok::Type;
            else {
                int k = j;
                while (k < n && is_space(s[k])) ++k;
                if (k < n && s[k] == '(') kind = Tok::Function;
                else if (id == Language::Json) {
                    int p = i - 1;
                    while (p >= 0 && is_space(s[p])) --p;
                    kind = Tok::Default;
                }
            }
            push_token(out, i, j, kind);
            i = j;
            continue;
        }
        // punctuation / operators
        push_token(out, i, i + 1, Tok::Punct);
        ++i;
    }
    return state;
}

ImU32 color_for(Tok kind) {
    switch (kind) {
        case Tok::Keyword:  return ui::C.syn_keyword;
        case Tok::Type:     return ui::C.syn_type;
        case Tok::String:   return ui::C.syn_string;
        case Tok::Number:   return ui::C.syn_number;
        case Tok::Comment:  return ui::C.syn_comment;
        case Tok::Preproc:  return ui::C.syn_preproc;
        case Tok::Function: return ui::C.syn_function;
        case Tok::Punct:    return ui::C.syn_punct;
        default:            return ui::C.syn_default;
    }
}

std::string lowercase(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

} // namespace

// ------------------------------------------------------------ public API

// Detection, display names and fence tags all live in the language registry
// (lang.cpp), so the tokenizer, the file tree and the chat all agree on them.
Language language_from_path(const std::string& path) { return lang::from_path(path); }
Language language_from_tag(const std::string& tag) { return lang::from_tag(tag); }
const char* language_name(Language id) { return lang::name(id); }
const char* language_tag(Language id) { return lang::tag(id); }

unsigned char highlight_line(Language id, const std::string& line, unsigned char state,
                             std::vector<Span>& out) {
    static std::vector<Token> tokens;
    unsigned char next = tokenize(id, line, state, tokens);
    out.clear();
    out.reserve(tokens.size());
    for (const Token& t : tokens) out.push_back({t.start, t.end, color_for(t.kind)});
    return next;
}

TextEditor::TextEditor() {
    lines_.assign(1, std::string());
    line_state_.assign(1, 0);
}

const std::string& TextEditor::line(int index) const {
    static const std::string empty;
    if (index < 0 || index >= (int)lines_.size()) return empty;
    return lines_[index];
}

void TextEditor::set_text(const std::string& text) {
    lines_.clear();
    use_crlf_ = text.find("\r\n") != std::string::npos;

    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            lines_.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    lines_.push_back(current);
    if (lines_.empty()) lines_.push_back(std::string());

    cursor_ = Coord();
    sel_start_ = sel_end_ = Coord();
    undo_.clear();
    undo_index_ = 0;
    saved_undo_index_ = 0;
    version_ = 0;
    ghost_.clear();
    matches_.clear();
    matches_for_.clear();
    rebuild_line_states();
    refresh_metrics();
}

std::string TextEditor::text() const {
    std::string out;
    size_t total = 0;
    for (const std::string& l : lines_) total += l.size() + 2;
    out.reserve(total);
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += use_crlf_ ? "\r\n" : "\n";
        out += lines_[i];
    }
    return out;
}

void TextEditor::set_language(Language lang) {
    language_ = lang;
    rebuild_line_states();
}

// ----------------------------------------------------------- primitives

Coord TextEditor::sanitize(Coord c) const {
    if (c.line < 0) return Coord(0, 0);
    if (c.line >= (int)lines_.size()) {
        int last = (int)lines_.size() - 1;
        return Coord(last, (int)lines_[last].size());
    }
    const std::string& l = lines_[c.line];
    if (c.column < 0) c.column = 0;
    if (c.column > (int)l.size()) c.column = (int)l.size();
    // never land in the middle of a UTF-8 sequence
    while (c.column > 0 && c.column < (int)l.size() &&
           ((unsigned char)l[c.column] & 0xC0) == 0x80)
        --c.column;
    return c;
}

Coord TextEditor::doc_end() const {
    int last = (int)lines_.size() - 1;
    return Coord(last, (int)lines_[last].size());
}

std::string TextEditor::range_text(Coord a, Coord b) const {
    a = sanitize(a);
    b = sanitize(b);
    if (b < a) std::swap(a, b);
    if (a == b) return std::string();
    if (a.line == b.line) return lines_[a.line].substr(a.column, b.column - a.column);

    std::string out = lines_[a.line].substr(a.column);
    for (int l = a.line + 1; l < b.line; ++l) {
        out += '\n';
        out += lines_[l];
    }
    out += '\n';
    out += lines_[b.line].substr(0, b.column);
    return out;
}

void TextEditor::raw_delete(Coord a, Coord b) {
    if (b < a) std::swap(a, b);
    if (a == b) return;
    if (a.line == b.line) {
        lines_[a.line].erase(a.column, b.column - a.column);
    } else {
        std::string head = lines_[a.line].substr(0, a.column);
        std::string tail = lines_[b.line].substr(b.column);
        lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
        lines_[a.line] = head + tail;
    }
}

Coord TextEditor::raw_insert(Coord c, const std::string& text) {
    if (text.empty()) return c;

    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == '\r') continue;
        if (ch == '\n') { parts.push_back(current); current.clear(); }
        else current += ch;
    }
    parts.push_back(current);

    std::string head = lines_[c.line].substr(0, c.column);
    std::string tail = lines_[c.line].substr(c.column);

    if (parts.size() == 1) {
        lines_[c.line] = head + parts[0] + tail;
        return Coord(c.line, (int)(head.size() + parts[0].size()));
    }

    lines_[c.line] = head + parts.front();
    std::vector<std::string> inserted(parts.begin() + 1, parts.end());
    inserted.back() += tail;
    lines_.insert(lines_.begin() + c.line + 1, inserted.begin(), inserted.end());
    return Coord(c.line + (int)parts.size() - 1, (int)parts.back().size());
}

void TextEditor::apply_edit(Coord a, Coord b, const std::string& text, bool coalesce) {
    if (read_only_) return;
    a = sanitize(a);
    b = sanitize(b);
    if (b < a) std::swap(a, b);

    UndoRecord rec;
    rec.start = a;
    rec.removed = range_text(a, b);
    rec.added = text;
    rec.cursor_before = cursor_;
    rec.sel_start_before = sel_start_;
    rec.sel_end_before = sel_end_;

    raw_delete(a, b);
    Coord end = raw_insert(a, text);

    cursor_ = end;
    sel_start_ = sel_end_ = end;
    rec.cursor_after = end;

    undo_.resize(undo_index_);
    // Editing after an undo discards the redo branch; if the saved state lived
    // in that branch it can never be reached again.
    if (saved_undo_index_ > (int)undo_.size()) saved_undo_index_ = -1;

    const double now = ImGui::GetTime();
    bool merged = false;
    if (coalesce && !undo_.empty() && now - last_edit_time_ < 1.0) {
        UndoRecord& prev = undo_.back();
        // merge consecutive single-character typing into one undo step
        if (prev.removed.empty() && rec.removed.empty() &&
            prev.added.find('\n') == std::string::npos &&
            rec.added.find('\n') == std::string::npos &&
            prev.start.line == a.line &&
            prev.start.column + (int)prev.added.size() == a.column) {
            prev.added += rec.added;
            prev.cursor_after = end;
            merged = true;
        }
    }
    if (!merged) undo_.push_back(rec);
    undo_index_ = (int)undo_.size();
    last_edit_time_ = now;

    ++version_;
    ghost_.clear();
    matches_for_.clear();
    rebuild_line_states(a.line);
    refresh_metrics();
    scroll_to_cursor_ = true;
    blink_reset_ = now;
}

void TextEditor::undo() {
    if (!can_undo()) return;
    const UndoRecord& rec = undo_[--undo_index_];

    Coord end = rec.start;
    if (!rec.added.empty()) {
        // walk to the end of the inserted text
        std::vector<std::string> parts;
        std::string cur;
        for (char ch : rec.added) {
            if (ch == '\n') { parts.push_back(cur); cur.clear(); }
            else if (ch != '\r') cur += ch;
        }
        parts.push_back(cur);
        if (parts.size() == 1) end = Coord(rec.start.line, rec.start.column + (int)parts[0].size());
        else end = Coord(rec.start.line + (int)parts.size() - 1, (int)parts.back().size());
    }
    raw_delete(rec.start, end);
    raw_insert(rec.start, rec.removed);

    cursor_ = sanitize(rec.cursor_before);
    sel_start_ = sanitize(rec.sel_start_before);
    sel_end_ = sanitize(rec.sel_end_before);
    ++version_;
    ghost_.clear();
    matches_for_.clear();
    rebuild_line_states(rec.start.line);
    refresh_metrics();
    scroll_to_cursor_ = true;
    last_edit_time_ = 0.0;
}

void TextEditor::redo() {
    if (!can_redo()) return;
    const UndoRecord& rec = undo_[undo_index_++];

    Coord end = rec.start;
    if (!rec.removed.empty()) {
        std::vector<std::string> parts;
        std::string cur;
        for (char ch : rec.removed) {
            if (ch == '\n') { parts.push_back(cur); cur.clear(); }
            else if (ch != '\r') cur += ch;
        }
        parts.push_back(cur);
        if (parts.size() == 1) end = Coord(rec.start.line, rec.start.column + (int)parts[0].size());
        else end = Coord(rec.start.line + (int)parts.size() - 1, (int)parts.back().size());
    }
    raw_delete(rec.start, end);
    Coord after = raw_insert(rec.start, rec.added);

    cursor_ = sanitize(rec.cursor_after.line >= 0 ? rec.cursor_after : after);
    sel_start_ = sel_end_ = cursor_;
    ++version_;
    ghost_.clear();
    matches_for_.clear();
    rebuild_line_states(rec.start.line);
    refresh_metrics();
    scroll_to_cursor_ = true;
    last_edit_time_ = 0.0;
}

// ---------------------------------------------------------- selection API

std::string TextEditor::selected_text() const {
    if (!has_selection()) return std::string();
    return range_text(sel_start_, sel_end_);
}

void TextEditor::select_all() {
    sel_start_ = Coord(0, 0);
    sel_end_ = doc_end();
    cursor_ = sel_end_;
}

void TextEditor::select_range(Coord a, Coord b) {
    a = sanitize(a);
    b = sanitize(b);
    if (b < a) std::swap(a, b);
    sel_start_ = a;
    sel_end_ = b;
    cursor_ = b;
    scroll_to_cursor_ = true;
}

void TextEditor::clear_selection() { sel_start_ = sel_end_ = cursor_; }

void TextEditor::set_cursor(Coord c, bool scroll) {
    cursor_ = sanitize(c);
    sel_start_ = sel_end_ = cursor_;
    if (scroll) scroll_to_cursor_ = true;
    blink_reset_ = ImGui::GetTime();
}

void TextEditor::goto_line(int line_index) {
    set_cursor(Coord(ImClamp(line_index, 0, (int)lines_.size() - 1), 0));
}

std::string TextEditor::context_before(int max_bytes) const {
    std::string out;
    Coord c = cursor_;
    out = lines_[c.line].substr(0, c.column);
    for (int l = c.line - 1; l >= 0 && (int)out.size() < max_bytes; --l)
        out = lines_[l] + "\n" + out;
    if ((int)out.size() > max_bytes) out = out.substr(out.size() - max_bytes);
    return out;
}

std::string TextEditor::context_after(int max_bytes) const {
    std::string out = lines_[cursor_.line].substr(cursor_.column);
    for (int l = cursor_.line + 1; l < (int)lines_.size() && (int)out.size() < max_bytes; ++l) {
        out += "\n";
        out += lines_[l];
    }
    if ((int)out.size() > max_bytes) out = out.substr(0, max_bytes);
    return out;
}

void TextEditor::insert_text(const std::string& text) {
    if (has_selection()) apply_edit(sel_start_, sel_end_, text);
    else apply_edit(cursor_, cursor_, text);
}

void TextEditor::replace_selection(const std::string& text) { insert_text(text); }

void TextEditor::replace_all_text(const std::string& text) {
    apply_edit(Coord(0, 0), doc_end(), text);
    cursor_ = sel_start_ = sel_end_ = Coord(0, 0);
    scroll_to_cursor_ = true;
}

void TextEditor::accept_ghost() {
    if (ghost_.empty()) return;
    std::string text = ghost_;
    ghost_.clear();
    apply_edit(cursor_, cursor_, text);
}

// ------------------------------------------------------------ navigation

Coord TextEditor::next_char(Coord c) const {
    c = sanitize(c);
    const std::string& l = lines_[c.line];
    if (c.column >= (int)l.size()) {
        if (c.line + 1 < (int)lines_.size()) return Coord(c.line + 1, 0);
        return c;
    }
    int len = 1;
    unsigned char b = (unsigned char)l[c.column];
    if (b >= 0xF0) len = 4; else if (b >= 0xE0) len = 3; else if (b >= 0xC0) len = 2;
    return sanitize(Coord(c.line, c.column + len));
}

Coord TextEditor::prev_char(Coord c) const {
    c = sanitize(c);
    if (c.column == 0) {
        if (c.line == 0) return c;
        return Coord(c.line - 1, (int)lines_[c.line - 1].size());
    }
    int col = c.column - 1;
    const std::string& l = lines_[c.line];
    while (col > 0 && ((unsigned char)l[col] & 0xC0) == 0x80) --col;
    return Coord(c.line, col);
}

Coord TextEditor::word_start(Coord c) const {
    c = sanitize(c);
    if (c.column == 0) return c.line > 0 ? Coord(c.line - 1, (int)lines_[c.line - 1].size()) : c;
    const std::string& l = lines_[c.line];
    int i = c.column;
    while (i > 0 && is_space(l[i - 1])) --i;
    if (i > 0 && is_ident(l[i - 1])) {
        while (i > 0 && is_ident(l[i - 1])) --i;
    } else {
        while (i > 0 && !is_ident(l[i - 1]) && !is_space(l[i - 1])) --i;
    }
    return Coord(c.line, i);
}

Coord TextEditor::word_end(Coord c) const {
    c = sanitize(c);
    const std::string& l = lines_[c.line];
    if (c.column >= (int)l.size())
        return c.line + 1 < (int)lines_.size() ? Coord(c.line + 1, 0) : c;
    int i = c.column;
    if (is_ident(l[i])) {
        while (i < (int)l.size() && is_ident(l[i])) ++i;
    } else if (!is_space(l[i])) {
        while (i < (int)l.size() && !is_ident(l[i]) && !is_space(l[i])) ++i;
    }
    while (i < (int)l.size() && is_space(l[i])) ++i;
    return Coord(c.line, i);
}

Coord TextEditor::line_first_non_space(int line_index) const {
    const std::string& l = lines_[line_index];
    int i = 0;
    while (i < (int)l.size() && is_space(l[i])) ++i;
    return Coord(line_index, i);
}

// ----------------------------------------------------------- measurement

float TextEditor::run_width(const char* begin, const char* end) const {
    ImFont* font = ui::F.mono;
    const float size = font->FontSize;
    float w = 0.0f;
    const char* p = begin;
    while (p < end) {
        unsigned int cp = 0;
        int adv = ImTextCharFromUtf8(&cp, p, end);
        if (adv == 0) break;
        p += adv;
        if (cp == '\t') {
            float tab = space_width_ * (float)tab_size;
            w = std::floor(w / tab + 1.0f) * tab;
        } else {
            w += font->GetCharAdvance(cp <= 0xFFFF ? (ImWchar)cp : (ImWchar)'?');
        }
    }
    (void)size;
    return w;
}

float TextEditor::column_to_x(const std::string& s, int column) const {
    column = ImClamp(column, 0, (int)s.size());
    return run_width(s.data(), s.data() + column);
}

int TextEditor::x_to_column(const std::string& s, float x) const {
    if (x <= 0.0f) return 0;
    ImFont* font = ui::F.mono;
    float w = 0.0f;
    const char* begin = s.data();
    const char* end = begin + s.size();
    const char* p = begin;
    while (p < end) {
        unsigned int cp = 0;
        int adv = ImTextCharFromUtf8(&cp, p, end);
        if (adv == 0) break;
        float step;
        if (cp == '\t') {
            float tab = space_width_ * (float)tab_size;
            step = std::floor(w / tab + 1.0f) * tab - w;
        } else {
            step = font->GetCharAdvance(cp <= 0xFFFF ? (ImWchar)cp : (ImWchar)'?');
        }
        if (x < w + step * 0.5f) return (int)(p - begin);
        w += step;
        p += adv;
    }
    return (int)s.size();
}

void TextEditor::refresh_metrics() {
    longest_line_px_ = 0.0f;
    // Measuring every line is fine for the file sizes an editor realistically
    // opens; the exact value only drives the horizontal scrollbar extent.
    for (const std::string& l : lines_)
        longest_line_px_ = ImMax(longest_line_px_, text_width(l));
}

void TextEditor::rebuild_line_states(int from_line) {
    line_state_.resize(lines_.size());
    unsigned char state = from_line > 0 && from_line <= (int)line_state_.size()
                              ? line_state_[from_line - 1]
                              : 0;
    if (from_line > 0 && from_line - 1 < (int)line_state_.size()) {
        // recompute the state produced by the previous line
        std::vector<Token> tmp;
        state = tokenize(language_, lines_[from_line - 1], line_state_[from_line - 1], tmp);
    }
    std::vector<Token> tokens;
    for (int i = ImMax(0, from_line); i < (int)lines_.size(); ++i) {
        line_state_[i] = state;
        state = tokenize(language_, lines_[i], state, tokens);
    }
}

// -------------------------------------------------------------- editing

void TextEditor::type_char(unsigned int cp) {
    if (read_only_) return;

    char utf8[5] = {0};
    int len = ImTextCharToUtf8(utf8, cp) ? (int)std::strlen(utf8) : 0;
    if (len == 0) return;
    std::string s(utf8, utf8 + len);

    if (auto_close_brackets) {
        const char* open = "([{\"'";
        const char* close = ")]}\"'";
        const char* found = std::strchr(open, (int)cp);
        // Wrap the selection instead of replacing it.
        if (found && has_selection() && cp < 128) {
            std::string sel = selected_text();
            char c = close[found - open];
            apply_edit(sel_start_, sel_end_, s + sel + std::string(1, c));
            return;
        }
        // Type over an auto-inserted closing character.
        const std::string& cur = lines_[cursor_.line];
        if (cursor_.column < (int)cur.size() && cp < 128 &&
            std::strchr(")]}\"'", (int)cp) && cur[cursor_.column] == (char)cp) {
            set_cursor(Coord(cursor_.line, cursor_.column + 1));
            return;
        }
        if (found && !has_selection() && cp < 128) {
            char c = close[found - open];
            const std::string& l = lines_[cursor_.line];
            bool at_word = cursor_.column < (int)l.size() && is_ident(l[cursor_.column]);
            // Quotes only auto-pair when not adjacent to a word.
            bool quote = (cp == '"' || cp == '\'');
            if (!at_word && !(quote && cursor_.column > 0 && is_ident(l[cursor_.column - 1]))) {
                apply_edit(cursor_, cursor_, s + std::string(1, c));
                set_cursor(Coord(cursor_.line, cursor_.column - 1));
                return;
            }
        }
        // Outdent a closing brace typed at the start of a line.
        if (cp == '}' && !has_selection()) {
            const std::string& l = lines_[cursor_.line];
            bool only_space = true;
            for (int i = 0; i < cursor_.column; ++i)
                if (!is_space(l[i])) { only_space = false; break; }
            if (only_space && cursor_.column > 0) {
                int width = insert_spaces ? tab_size : 1;
                int remove = ImMin(width, cursor_.column);
                apply_edit(Coord(cursor_.line, cursor_.column - remove), cursor_, s);
                return;
            }
        }
    }

    insert_text(s);
}

void TextEditor::insert_newline() {
    if (read_only_) return;

    const std::string& l = lines_[cursor_.line];
    std::string indent;
    for (size_t i = 0; i < l.size() && is_space(l[i]); ++i) indent += l[i];

    std::string before = l.substr(0, cursor_.column);
    std::string after = l.substr(cursor_.column);

    // Trim trailing spaces of the current indent when the line is empty.
    std::string extra;
    char last = 0;
    for (int i = (int)before.size() - 1; i >= 0; --i) {
        if (!is_space(before[i])) { last = before[i]; break; }
    }
    if (last == '{' || last == '[' || last == '(' || last == ':')
        extra = insert_spaces ? std::string((size_t)tab_size, ' ') : std::string("\t");

    char next = 0;
    for (size_t i = 0; i < after.size(); ++i) {
        if (!is_space(after[i])) { next = after[i]; break; }
    }

    if (!extra.empty() && (next == '}' || next == ']' || next == ')')) {
        // Opening brace with the matching closer right after: expand both lines.
        std::string text = "\n" + indent + extra + "\n" + indent;
        insert_text(text);
        set_cursor(Coord(cursor_.line - 1, (int)(indent.size() + extra.size())));
        return;
    }

    insert_text("\n" + indent + extra);
}

void TextEditor::delete_selection_or(int direction) {
    if (read_only_) return;
    if (has_selection()) {
        apply_edit(sel_start_, sel_end_, "");
        return;
    }
    if (direction < 0) {
        // Backspace over a full indent step when only whitespace precedes.
        const std::string& l = lines_[cursor_.line];
        if (insert_spaces && cursor_.column > 0) {
            bool only_space = true;
            for (int i = 0; i < cursor_.column; ++i)
                if (!is_space(l[i])) { only_space = false; break; }
            if (only_space) {
                int steps = cursor_.column % tab_size;
                if (steps == 0) steps = tab_size;
                steps = ImMin(steps, cursor_.column);
                apply_edit(Coord(cursor_.line, cursor_.column - steps), cursor_, "");
                return;
            }
        }
        Coord prev = prev_char(cursor_);
        if (prev != cursor_) apply_edit(prev, cursor_, "");
    } else {
        Coord next = next_char(cursor_);
        if (next != cursor_) apply_edit(cursor_, next, "");
    }
}

void TextEditor::indent_selection(bool outdent) {
    if (read_only_) return;

    int first = has_selection() ? sel_start_.line : cursor_.line;
    int last = has_selection() ? sel_end_.line : cursor_.line;
    if (has_selection() && sel_end_.column == 0 && last > first) --last;

    const std::string unit = insert_spaces ? std::string((size_t)tab_size, ' ') : std::string("\t");

    std::string replacement;
    for (int i = first; i <= last; ++i) {
        std::string l = lines_[i];
        if (outdent) {
            int removed = 0;
            while (removed < tab_size && !l.empty() && is_space(l[0])) {
                if (l[0] == '\t') { l.erase(0, 1); break; }
                l.erase(0, 1);
                ++removed;
            }
        } else if (!l.empty() || first == last) {
            l = unit + l;
        }
        if (i > first) replacement += "\n";
        replacement += l;
    }

    Coord a(first, 0);
    Coord b(last, (int)lines_[last].size());
    Coord old_cursor = cursor_;
    apply_edit(a, b, replacement);

    // Restore a sensible selection over the affected lines.
    sel_start_ = Coord(first, 0);
    sel_end_ = Coord(last, (int)lines_[ImMin(last, (int)lines_.size() - 1)].size());
    int shift = outdent ? -tab_size : tab_size;
    cursor_ = sanitize(Coord(old_cursor.line, ImMax(0, old_cursor.column + shift)));
}

void TextEditor::toggle_comment() {
    if (read_only_) return;
    const lang::Spec& spec = lang::spec(language_);
    std::string marker = *spec.line_comment ? spec.line_comment : "//";
    if (language_ == Language::PlainText) return;

    int first = has_selection() ? sel_start_.line : cursor_.line;
    int last = has_selection() ? sel_end_.line : cursor_.line;
    if (has_selection() && sel_end_.column == 0 && last > first) --last;

    bool all_commented = true;
    for (int i = first; i <= last; ++i) {
        const std::string& l = lines_[i];
        size_t p = l.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        if (l.compare(p, marker.size(), marker) != 0) { all_commented = false; break; }
    }

    std::string replacement;
    for (int i = first; i <= last; ++i) {
        std::string l = lines_[i];
        size_t p = l.find_first_not_of(" \t");
        if (p != std::string::npos) {
            if (all_commented) {
                l.erase(p, marker.size());
                if (p < l.size() && l[p] == ' ') l.erase(p, 1);
            } else {
                l.insert(p, marker + " ");
            }
        }
        if (i > first) replacement += "\n";
        replacement += l;
    }

    Coord old_cursor = cursor_;
    apply_edit(Coord(first, 0), Coord(last, (int)lines_[last].size()), replacement);
    cursor_ = sanitize(old_cursor);
    sel_start_ = sel_end_ = cursor_;
}

void TextEditor::duplicate_line() {
    if (read_only_) return;
    if (has_selection()) {
        std::string sel = selected_text();
        Coord end = sel_end_;
        apply_edit(end, end, sel);
        return;
    }
    const std::string& l = lines_[cursor_.line];
    Coord end(cursor_.line, (int)l.size());
    Coord old = cursor_;
    apply_edit(end, end, "\n" + l);
    cursor_ = sanitize(Coord(old.line + 1, old.column));
    sel_start_ = sel_end_ = cursor_;
}

void TextEditor::move_lines(int delta) {
    if (read_only_) return;
    int first = has_selection() ? sel_start_.line : cursor_.line;
    int last = has_selection() ? sel_end_.line : cursor_.line;
    if (has_selection() && sel_end_.column == 0 && last > first) --last;

    if (delta < 0 && first == 0) return;
    if (delta > 0 && last >= (int)lines_.size() - 1) return;

    int block_first = ImMin(first, last);
    int block_last = ImMax(first, last);
    int a = delta < 0 ? block_first - 1 : block_first;
    int b = delta < 0 ? block_last : block_last + 1;

    std::vector<std::string> block(lines_.begin() + a, lines_.begin() + b + 1);
    if (delta < 0) std::rotate(block.begin(), block.begin() + 1, block.end());
    else std::rotate(block.rbegin(), block.rbegin() + 1, block.rend());

    std::string replacement;
    for (size_t i = 0; i < block.size(); ++i) {
        if (i) replacement += "\n";
        replacement += block[i];
    }

    bool had_selection = has_selection();
    Coord old_cursor = cursor_;
    apply_edit(Coord(a, 0), Coord(b, (int)lines_[b].size()), replacement);

    cursor_ = sanitize(Coord(old_cursor.line + delta, old_cursor.column));
    if (had_selection) {
        sel_start_ = sanitize(Coord(block_first + delta, 0));
        sel_end_ = sanitize(Coord(block_last + delta, (int)lines_[ImClamp(block_last + delta, 0,
                                    (int)lines_.size() - 1)].size()));
    } else {
        sel_start_ = sel_end_ = cursor_;
    }
}

void TextEditor::copy_to_clipboard(bool cut) {
    std::string text;
    if (has_selection()) {
        text = selected_text();
    } else {
        text = lines_[cursor_.line] + "\n"; // whole line, like most editors
    }
    ImGui::SetClipboardText(text.c_str());
    if (cut && !read_only_) {
        if (has_selection()) apply_edit(sel_start_, sel_end_, "");
        else {
            Coord a(cursor_.line, 0);
            Coord b = cursor_.line + 1 < (int)lines_.size() ? Coord(cursor_.line + 1, 0)
                                                            : Coord(cursor_.line, (int)lines_[cursor_.line].size());
            apply_edit(a, b, "");
        }
    }
}

void TextEditor::paste_from_clipboard() {
    if (read_only_) return;
    const char* text = ImGui::GetClipboardText();
    if (text && *text) insert_text(text);
}

// ---------------------------------------------------------------- search

void TextEditor::open_find() {
    find_open_ = true;
    find_focus_ = true;
    if (has_selection() && sel_start_.line == sel_end_.line) {
        std::string sel = selected_text();
        if (!sel.empty() && sel.size() < sizeof(find_buffer_) - 1) {
            std::memcpy(find_buffer_, sel.c_str(), sel.size() + 1);
            matches_for_.clear();
        }
    }
}

void TextEditor::update_matches() {
    std::string needle = find_buffer_;
    std::string key = needle + (find_case_sensitive_ ? "\x01" : "\x02");
    if (key == matches_for_) return;
    matches_for_ = key;
    matches_.clear();
    current_match_ = -1;
    if (needle.empty()) return;

    std::string cmp_needle = find_case_sensitive_ ? needle : lowercase(needle);
    for (int i = 0; i < (int)lines_.size(); ++i) {
        const std::string& raw = lines_[i];
        std::string hay = find_case_sensitive_ ? raw : lowercase(raw);
        size_t pos = 0;
        while ((pos = hay.find(cmp_needle, pos)) != std::string::npos) {
            matches_.push_back(Coord(i, (int)pos));
            pos += cmp_needle.size();
        }
    }
    // Start from the match nearest the caret.
    for (int i = 0; i < (int)matches_.size(); ++i) {
        if (!(matches_[i] < cursor_)) { current_match_ = i; break; }
    }
    if (current_match_ < 0 && !matches_.empty()) current_match_ = 0;
}

void TextEditor::jump_to_match(int delta) {
    if (matches_.empty()) return;
    current_match_ = (current_match_ + delta + (int)matches_.size()) % (int)matches_.size();
    Coord a = matches_[current_match_];
    Coord b(a.line, a.column + (int)std::strlen(find_buffer_));
    select_range(a, b);
    scroll_to_cursor_ = true;
}

// ------------------------------------------------------------- input

void TextEditor::handle_keyboard() {
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;
    const bool alt = io.KeyAlt;

    auto move_to = [&](Coord target, bool extend) {
        target = sanitize(target);
        if (extend) {
            if (!has_selection()) { sel_start_ = sel_end_ = cursor_; }
            if (cursor_ == sel_start_) sel_start_ = target;
            else sel_end_ = target;
            if (sel_end_ < sel_start_) std::swap(sel_start_, sel_end_);
        } else {
            sel_start_ = sel_end_ = target;
        }
        cursor_ = target;
        scroll_to_cursor_ = true;
        blink_reset_ = ImGui::GetTime();
        ghost_.clear();
    };

    // --- clipboard / history ---
    if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Z, true)) { undo(); return; }
    if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, true)) ||
        (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_Z, true))) { redo(); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) { copy_to_clipboard(false); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) { copy_to_clipboard(true); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, true)) { paste_from_clipboard(); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) { select_all(); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) { duplicate_line(); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Slash, false)) { toggle_comment(); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) { open_find(); return; }
    if (alt && ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) { move_lines(-1); return; }
    if (alt && ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) { move_lines(1); return; }

    // --- ghost text ---
    if (has_ghost()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) { accept_ghost(); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { clear_ghost(); return; }
    }

    // --- navigation ---
#ifdef __APPLE__
    // The macOS conventions: Option moves by word, Command (reported as Ctrl)
    // jumps to the ends of the line, and vertically to the ends of the file.
    const bool word_mod = alt;
    const bool edge_mod = ctrl;
#else
    const bool word_mod = ctrl;
    const bool edge_mod = false;
#endif
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
        if (edge_mod) {
            const Coord first = line_first_non_space(cursor_.line);
            move_to(cursor_ == first ? Coord(cursor_.line, 0) : first, shift);
        } else {
            move_to(word_mod ? word_start(cursor_)
                             : (has_selection() && !shift ? sel_start_ : prev_char(cursor_)),
                    shift);
        }
        preferred_column_x_ = -1;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
        if (edge_mod)
            move_to(Coord(cursor_.line, (int)lines_[cursor_.line].size()), shift);
        else
            move_to(word_mod ? word_end(cursor_)
                             : (has_selection() && !shift ? sel_end_ : next_char(cursor_)),
                    shift);
        preferred_column_x_ = -1;
        return;
    }
    if (edge_mod && (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ||
                     ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))) {
        move_to(ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ? Coord(0, 0) : doc_end(), shift);
        preferred_column_x_ = -1;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
        int dir = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ? -1 : 1;
        if (preferred_column_x_ < 0)
            preferred_column_x_ = (int)column_to_x(lines_[cursor_.line], cursor_.column);
        int target_line = ImClamp(cursor_.line + dir, 0, (int)lines_.size() - 1);
        int col = x_to_column(lines_[target_line], (float)preferred_column_x_);
        move_to(Coord(target_line, col), shift);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, true)) {
        Coord first = line_first_non_space(cursor_.line);
        Coord target = ctrl ? Coord(0, 0) : (cursor_ == first ? Coord(cursor_.line, 0) : first);
        move_to(target, shift);
        preferred_column_x_ = -1;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End, true)) {
        Coord target = ctrl ? doc_end() : Coord(cursor_.line, (int)lines_[cursor_.line].size());
        move_to(target, shift);
        preferred_column_x_ = -1;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true) || ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) {
        int dir = ImGui::IsKeyPressed(ImGuiKey_PageUp, true) ? -1 : 1;
        int page = ImMax(1, (int)(ImGui::GetWindowHeight() / line_height_) - 2);
        if (preferred_column_x_ < 0)
            preferred_column_x_ = (int)column_to_x(lines_[cursor_.line], cursor_.column);
        int target_line = ImClamp(cursor_.line + dir * page, 0, (int)lines_.size() - 1);
        move_to(Coord(target_line, x_to_column(lines_[target_line], (float)preferred_column_x_)), shift);
        return;
    }

    if (read_only_) return;

    // --- editing keys ---
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true)) {
        insert_newline();
        preferred_column_x_ = -1;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, true)) {
        if (has_selection() && sel_start_.line != sel_end_.line) indent_selection(shift);
        else if (shift) indent_selection(true);
        else insert_text(insert_spaces ? std::string((size_t)tab_size -
                             (size_t)(column_to_x(lines_[cursor_.line], cursor_.column) /
                                      space_width_) % (size_t)tab_size, ' ')
                                       : std::string("\t"));
        preferred_column_x_ = -1;
        return;
    }
#ifdef __APPLE__
    // Option+Delete removes the word before the caret, Command+Delete the
    // rest of the line before it: select that span, then delete it.
    if ((alt || ctrl) && !has_selection() && ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
        const Coord from = ctrl ? Coord(cursor_.line, 0) : word_start(cursor_);
        if (from < cursor_) move_to(from, true);
        delete_selection_or(-1);
        preferred_column_x_ = -1;
        return;
    }
#endif
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) { delete_selection_or(-1); preferred_column_x_ = -1; return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, true)) { delete_selection_or(1); preferred_column_x_ = -1; return; }

    // --- text input ---
    if (!io.InputQueueCharacters.empty()) {
        // Ctrl+<key> is a shortcut, not text. AltGr (Ctrl+Alt) still types.
#ifdef __APPLE__
        // Command arrives as Ctrl and the Control key as Super; neither types.
        const bool shortcut_modifier = (ctrl || io.KeySuper) && !alt;
#else
        const bool shortcut_modifier = ctrl && !alt;
#endif
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            unsigned int cp = (unsigned int)io.InputQueueCharacters[i];
            if (cp == 0 || cp == '\t' || cp == '\r' || cp == '\n' || cp == 127) continue;
            if (cp < 32) continue;
            if (shortcut_modifier) continue;
            type_char(cp);
        }
        io.InputQueueCharacters.resize(0);
        preferred_column_x_ = -1;
    }
}

void TextEditor::handle_mouse(const ImVec2& text_origin) {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) return;

    auto coord_at = [&](ImVec2 mouse) {
        int line_index = (int)std::floor((mouse.y - text_origin.y) / line_height_);
        line_index = ImClamp(line_index, 0, (int)lines_.size() - 1);
        int col = x_to_column(lines_[line_index], mouse.x - text_origin.x);
        return Coord(line_index, col);
    };

    const ImVec2 mouse = io.MousePos;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetWindowFocus();
        Coord c = coord_at(mouse);
        if (io.KeyShift && (has_selection() || true)) {
            drag_anchor_ = cursor_;
            select_range(drag_anchor_, c);
            cursor_ = c;
        } else {
            drag_anchor_ = c;
            set_cursor(c, false);
        }
        preferred_column_x_ = -1;
        blink_reset_ = ImGui::GetTime();
        ghost_.clear();
    } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        Coord c = coord_at(mouse);
        Coord a = word_start(next_char(c));
        Coord b = word_end(c);
        if (a <= c && c <= b) select_range(a, b);
    } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ImGui::IsWindowFocused()) {
        Coord c = coord_at(mouse);
        select_range(drag_anchor_, c);
        cursor_ = c;

        // auto-scroll while dragging past the edges
        const ImVec2 win_min = ImGui::GetWindowPos();
        const ImVec2 win_max(win_min.x + ImGui::GetWindowWidth(), win_min.y + ImGui::GetWindowHeight());
        const float margin = line_height_;
        if (mouse.y < win_min.y + margin)
            ImGui::SetScrollY(ImGui::GetScrollY() - line_height_ * 0.6f);
        else if (mouse.y > win_max.y - margin)
            ImGui::SetScrollY(ImGui::GetScrollY() + line_height_ * 0.6f);
        if (mouse.x < win_min.x + margin)
            ImGui::SetScrollX(ImGui::GetScrollX() - char_width_ * 2.0f);
        else if (mouse.x > win_max.x - margin)
            ImGui::SetScrollX(ImGui::GetScrollX() + char_width_ * 2.0f);
    }
}

void TextEditor::scroll_cursor_into_view() {
    const float scroll_y = ImGui::GetScrollY();
    const float height = ImGui::GetWindowHeight();
    const float top = (float)cursor_.line * line_height_;
    const float bottom = top + line_height_;
    const float margin = line_height_ * 2.0f;

    if (top < scroll_y + margin)
        ImGui::SetScrollY(ImMax(0.0f, top - margin));
    else if (bottom > scroll_y + height - margin)
        ImGui::SetScrollY(bottom - height + margin);

    const float x = column_to_x(lines_[cursor_.line], cursor_.column);
    const float scroll_x = ImGui::GetScrollX();
    const float width = ImGui::GetWindowWidth();
    if (x < scroll_x + char_width_ * 4.0f)
        ImGui::SetScrollX(ImMax(0.0f, x - char_width_ * 8.0f));
    else if (x > scroll_x + width - char_width_ * 8.0f)
        ImGui::SetScrollX(x - width + char_width_ * 10.0f);
}

// --------------------------------------------------------------- render

void TextEditor::draw_find_bar(const ImVec2& area_min, float area_width) {
    ImGuiIO& io = ImGui::GetIO();
    const float bar_w = ui::px(340.0f);
    const float pad = ui::px(8.0f);
    ImVec2 pos(area_min.x + area_width - bar_w - pad * 2.0f, area_min.y + pad);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(bar_w, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(ui::C.bg_surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(8), ui::px(8)));
    ImGui::BeginChild("##find", ImVec2(bar_w, 0),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushFont(ui::F.ui);
    if (find_focus_) {
        ImGui::SetKeyboardFocusHere();
        find_focus_ = false;
    }
    ImGui::SetNextItemWidth(bar_w - ui::px(126.0f));
    if (ImGui::InputTextWithHint("##findtext", "Find", find_buffer_, sizeof(find_buffer_),
                                 ImGuiInputTextFlags_AutoSelectAll))
        matches_for_.clear();

    bool enter = ImGui::IsItemFocused() &&
                 (ImGui::IsKeyPressed(ImGuiKey_Enter, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true));

    ImGui::SameLine(0, ui::px(6));
    ImGui::PushFont(ui::F.ui_small);
    char counter[48];
    if (matches_.empty())
        std::snprintf(counter, sizeof(counter), find_buffer_[0] ? "0/0" : "");
    else
        std::snprintf(counter, sizeof(counter), "%d/%d", current_match_ + 1, (int)matches_.size());
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::C.text_dim), "%s", counter);
    ImGui::PopFont();

    ImGui::SameLine(0, ui::px(4));
    if (ui::icon_button("##prev", ui::Icon::ChevronDown, "Previous (Shift+Enter)")) jump_to_match(-1);
    ImGui::SameLine(0, ui::px(2));
    if (ui::icon_button("##next", ui::Icon::ChevronRight, "Next (Enter)")) jump_to_match(1);
    ImGui::SameLine(0, ui::px(2));
    if (ui::icon_button("##closefind", ui::Icon::Close, "Close (Esc)")) find_open_ = false;

    if (enter) jump_to_match(io.KeyShift ? -1 : 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) find_open_ = false;

    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

bool TextEditor::render(const char* id, const ImVec2& size) {
    const unsigned version_before = version_;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(ui::C.code_bg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushFont(ui::F.mono);

    ImGui::BeginChild(id, size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);

    ImFont* font = ui::F.mono;
    space_width_ = font->GetCharAdvance((ImWchar)' ');
    char_width_ = space_width_;
    line_height_ = std::floor(ImGui::GetFontSize() * 1.55f);

    focused_ = ImGui::IsWindowFocused();

    const ImVec2 win_pos = ImGui::GetWindowPos();
    const float win_w = ImGui::GetWindowWidth();
    const float win_h = ImGui::GetWindowHeight();

    // gutter
    char last_no[16];
    std::snprintf(last_no, sizeof(last_no), "%d", (int)lines_.size());
    const float digits_w = ImGui::CalcTextSize(last_no).x;
    const float gutter_w = show_line_numbers ? std::floor(digits_w + ui::px(26.0f)) : ui::px(10.0f);
    const float pad_x = ui::px(10.0f);
    const float pad_y = ui::px(8.0f);

    const float content_w = gutter_w + pad_x + longest_line_px_ + ui::px(80.0f);
    const float content_h = lines_.size() * line_height_ + pad_y * 2.0f + win_h * 0.4f;
    ImGui::Dummy(ImVec2(content_w, content_h));

    const float scroll_x = ImGui::GetScrollX();
    const float scroll_y = ImGui::GetScrollY();
    const ImVec2 text_origin(win_pos.x + gutter_w + pad_x - scroll_x, win_pos.y + pad_y - scroll_y);

    if (focused_ && !ImGui::IsAnyItemActive()) handle_keyboard();
    handle_mouse(text_origin);

    if (scroll_to_cursor_) {
        scroll_cursor_into_view();
        scroll_to_cursor_ = false;
    }
    if (find_open_) update_matches();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int first_line = ImMax(0, (int)std::floor((scroll_y - pad_y) / line_height_));
    const int last_line = ImMin((int)lines_.size() - 1,
                                first_line + (int)std::ceil(win_h / line_height_) + 1);

    // ---- text area (clipped so long lines do not spill into the gutter) ----
    const ImVec2 clip_min(win_pos.x + gutter_w, win_pos.y);
    const ImVec2 clip_max(win_pos.x + win_w, win_pos.y + win_h);
    dl->PushClipRect(clip_min, clip_max, true);

    const Coord sel_a = sel_start_;
    const Coord sel_b = sel_end_;
    const bool selecting = has_selection();

    std::vector<Token> tokens;
    for (int i = first_line; i <= last_line; ++i) {
        const std::string& text = lines_[i];
        const float y = text_origin.y + i * line_height_;

        // current line highlight
        if (highlight_current_line && i == cursor_.line && !selecting && focused_) {
            dl->AddRectFilled(ImVec2(clip_min.x, y), ImVec2(clip_max.x, y + line_height_),
                              ui::C.current_line);
        }

        // selection
        if (selecting && i >= sel_a.line && i <= sel_b.line) {
            int col_a = i == sel_a.line ? sel_a.column : 0;
            int col_b = i == sel_b.line ? sel_b.column : (int)text.size();
            float x1 = text_origin.x + column_to_x(text, col_a);
            float x2 = text_origin.x + column_to_x(text, col_b);
            if (i < sel_b.line) x2 += space_width_ * 0.6f; // show the wrapped newline
            dl->AddRectFilled(ImVec2(x1, y), ImVec2(ImMax(x2, x1 + 2.0f), y + line_height_),
                              ui::C.selection, ui::px(2.0f));
        }

        // search matches
        if (find_open_ && !matches_.empty()) {
            const int needle_len = (int)std::strlen(find_buffer_);
            for (size_t m = 0; m < matches_.size(); ++m) {
                if (matches_[m].line != i) continue;
                float x1 = text_origin.x + column_to_x(text, matches_[m].column);
                float x2 = text_origin.x + column_to_x(text, matches_[m].column + needle_len);
                bool current = (int)m == current_match_;
                dl->AddRectFilled(ImVec2(x1, y), ImVec2(x2, y + line_height_),
                                  current ? ui::C.accent_dim : ui::C.match_highlight, ui::px(2.0f));
                if (current)
                    dl->AddRect(ImVec2(x1, y), ImVec2(x2, y + line_height_), ui::C.accent,
                                ui::px(2.0f), 0, 1.0f);
            }
        }

        // indent guides
        if (show_indent_guides) {
            int spaces = 0;
            for (size_t k = 0; k < text.size() && is_space(text[k]); ++k)
                spaces += text[k] == '\t' ? tab_size : 1;
            if (text.empty()) {
                // continue guides through blank lines using the next non-empty line
                for (int k = i + 1; k < (int)lines_.size(); ++k) {
                    if (lines_[k].empty()) continue;
                    int s2 = 0;
                    for (size_t q = 0; q < lines_[k].size() && is_space(lines_[k][q]); ++q)
                        s2 += lines_[k][q] == '\t' ? tab_size : 1;
                    spaces = s2;
                    break;
                }
            }
            for (int level = tab_size; level < spaces; level += tab_size) {
                float gx = std::floor(text_origin.x + level * space_width_) + 0.5f;
                dl->AddLine(ImVec2(gx, y), ImVec2(gx, y + line_height_), ui::C.indent_guide, 1.0f);
            }
        }

        // syntax-highlighted text
        if (!text.empty()) {
            unsigned char state = i < (int)line_state_.size() ? line_state_[i] : 0;
            tokenize(language_, text, state, tokens);
            float x = text_origin.x;
            const float text_y = y + std::floor((line_height_ - ImGui::GetFontSize()) * 0.5f);
            for (const Token& tk : tokens) {
                const char* begin = text.data() + tk.start;
                const char* end = text.data() + tk.end;
                const ImU32 color = color_for(tk.kind);
                // split on tabs so alignment matches the measurement helpers
                const char* run = begin;
                for (const char* p = begin; p <= end; ++p) {
                    if (p == end || *p == '\t') {
                        if (p > run) {
                            if (x < clip_max.x && x + (p - run) * char_width_ * 2.0f > clip_min.x)
                                dl->AddText(font, ImGui::GetFontSize(), ImVec2(x, text_y), color, run, p);
                            x += run_width(run, p);
                        }
                        if (p != end) {
                            float tab = space_width_ * (float)tab_size;
                            float local = x - text_origin.x;
                            x = text_origin.x + std::floor(local / tab + 1.0f) * tab;
                            run = p + 1;
                        }
                    }
                }
            }
        }
    }

    // ghost (AI suggestion) text
    if (!ghost_.empty()) {
        float gx = text_origin.x + column_to_x(lines_[cursor_.line], cursor_.column);
        float gy = text_origin.y + cursor_.line * line_height_;
        int line_offset = 0;
        size_t start = 0;
        while (start <= ghost_.size()) {
            size_t nl = ghost_.find('\n', start);
            std::string part = ghost_.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            float px_ = line_offset == 0 ? gx : text_origin.x;
            dl->AddText(font, ImGui::GetFontSize(),
                        ImVec2(px_, gy + line_offset * line_height_ +
                                        std::floor((line_height_ - ImGui::GetFontSize()) * 0.5f)),
                        ui::C.ghost_text, part.c_str());
            if (nl == std::string::npos) break;
            start = nl + 1;
            ++line_offset;
            if (line_offset > 12) break;
        }
        // "Tab to accept" pill
        const char* hint = "Tab";
        ImGui::PushFont(ui::F.ui_small);
        ImVec2 ts = ImGui::CalcTextSize(hint);
        ImGui::PopFont();
        float hx = gx + ui::px(6.0f);
        float hy = gy + (line_offset + 1) * line_height_;
        dl->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + ts.x + ui::px(10), hy + ts.y + ui::px(4)),
                          ui::C.accent_dim, ui::px(4));
        dl->AddText(ui::F.ui_small, ui::F.ui_small->FontSize,
                    ImVec2(hx + ui::px(5), hy + ui::px(2)), ui::C.text, hint);
    }

    // caret
    if (focused_) {
        const double t = ImGui::GetTime() - blink_reset_;
        if (std::fmod(t, 1.06) < 0.63) {
            float cx = std::floor(text_origin.x + column_to_x(lines_[cursor_.line], cursor_.column));
            float cy = text_origin.y + cursor_.line * line_height_;
            dl->AddRectFilled(ImVec2(cx, cy + ui::px(1.0f)),
                              ImVec2(cx + ImMax(1.5f, ui::px(2.0f)), cy + line_height_ - ui::px(1.0f)),
                              ui::C.cursor, 1.0f);
        }
    }

    dl->PopClipRect();

    // ---- gutter (fixed while scrolling horizontally) ----
    if (show_line_numbers) {
        dl->PushClipRect(ImVec2(win_pos.x, win_pos.y), ImVec2(win_pos.x + gutter_w, win_pos.y + win_h), true);
        dl->AddRectFilled(ImVec2(win_pos.x, win_pos.y), ImVec2(win_pos.x + gutter_w, win_pos.y + win_h),
                          ui::C.code_bg);
        for (int i = first_line; i <= last_line; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", i + 1);
            const float y = text_origin.y + i * line_height_ +
                            std::floor((line_height_ - ImGui::GetFontSize()) * 0.5f);
            const float w = ImGui::CalcTextSize(buf).x;
            const bool current = i == cursor_.line;
            dl->AddText(font, ImGui::GetFontSize(),
                        ImVec2(win_pos.x + gutter_w - ui::px(14.0f) - w, y),
                        current ? ui::C.line_number_cur : ui::C.line_number, buf);
        }
        dl->AddLine(ImVec2(win_pos.x + gutter_w - 0.5f, win_pos.y),
                    ImVec2(win_pos.x + gutter_w - 0.5f, win_pos.y + win_h), ui::C.border, 1.0f);
        dl->PopClipRect();
    }

    if (find_open_) draw_find_bar(win_pos, win_w);

    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    return version_ != version_before;
}

} // namespace ed
