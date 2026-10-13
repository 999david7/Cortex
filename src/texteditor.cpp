#include "texteditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <climits>
#include <functional>
#include <map>
#include <regex>
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

} // namespace

namespace {

bool is_open_bracket(char c) { return c == '(' || c == '[' || c == '{'; }
bool is_close_bracket(char c) { return c == ')' || c == ']' || c == '}'; }
int bracket_type(char c) {
    switch (c) {
        case '(': case ')': return 0;
        case '[': case ']': return 1;
        default: return 2;
    }
}
char partner_of(char c) {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        default: return 0;
    }
}

// Net bracket nesting a tokenized line adds, counting only code brackets.
int bracket_delta(const std::vector<Token>& tokens, const std::string& line, int depth) {
    for (const Token& t : tokens) {
        if (t.kind != Tok::Punct) continue;
        for (int i = t.start; i < t.end; ++i) {
            if (is_open_bracket(line[i])) ++depth;
            else if (is_close_bracket(line[i]) && depth > 0) --depth;
        }
    }
    return depth;
}

// Splits an identifier-like run into words: at '_' and '-', at a lower-to-
// upper step ("fooBar") and before the last capital of an acronym ("HTTPServer").
std::vector<std::string> split_words(const std::string& run) {
    std::vector<std::string> words;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) words.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < run.size(); ++i) {
        const char c = run[i];
        if (c == '_' || c == '-' || c == ' ') { flush(); continue; }
        const bool up = std::isupper((unsigned char)c) != 0;
        if (up && !cur.empty()) {
            const char prev = cur.back();
            const bool prev_lower = std::islower((unsigned char)prev) || std::isdigit((unsigned char)prev);
            const bool next_lower = i + 1 < run.size() && std::islower((unsigned char)run[i + 1]);
            if (prev_lower || (std::isupper((unsigned char)prev) && next_lower)) flush();
        }
        cur += c;
    }
    flush();
    return words;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string upper_ascii(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
std::string capitalized(const std::string& w) {
    std::string out = lower_ascii(w);
    if (!out.empty()) out[0] = (char)std::toupper((unsigned char)out[0]);
    return out;
}

// Converts one identifier-like run, keeping any leading and trailing
// separators ("_private" stays private-looking).
std::string convert_run(const std::string& run, CaseKind kind) {
    size_t lead = 0;
    while (lead < run.size() && (run[lead] == '_' || run[lead] == '-')) ++lead;
    size_t trail = run.size();
    while (trail > lead && (run[trail - 1] == '_' || run[trail - 1] == '-')) --trail;
    if (lead >= trail) return run;
    const std::vector<std::string> words = split_words(run.substr(lead, trail - lead));
    std::string body;
    for (size_t i = 0; i < words.size(); ++i) {
        switch (kind) {
            case CaseKind::Snake:  body += (i ? "_" : "") + lower_ascii(words[i]); break;
            case CaseKind::Kebab:  body += (i ? "-" : "") + lower_ascii(words[i]); break;
            case CaseKind::Camel:  body += i ? capitalized(words[i]) : lower_ascii(words[i]); break;
            case CaseKind::Pascal: body += capitalized(words[i]); break;
            default: break;
        }
    }
    return run.substr(0, lead) + body + run.substr(trail);
}

std::string convert_case(const std::string& text, CaseKind kind) {
    switch (kind) {
        case CaseKind::Upper: return upper_ascii(text);
        case CaseKind::Lower: return lower_ascii(text);
        case CaseKind::Title: {
            std::string out = lower_ascii(text);
            bool start = true;
            for (char& c : out) {
                const bool letter = std::isalnum((unsigned char)c) || c == '\'' || (unsigned char)c >= 0x80;
                if (letter && start && std::isalpha((unsigned char)c))
                    c = (char)std::toupper((unsigned char)c);
                start = !letter;
            }
            return out;
        }
        default: break;
    }
    // The identifier styles work run by run, so the spaces and punctuation
    // between identifiers in a selection are left where they are.
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (is_ident(c) || c == '-') {
            size_t j = i;
            while (j < text.size() && (is_ident(text[j]) || text[j] == '-')) ++j;
            out += convert_run(text.substr(i, j - i), kind);
            i = j;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

bool icase_less(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int ca = std::tolower((unsigned char)a[i]);
        const int cb = std::tolower((unsigned char)b[i]);
        if (ca != cb) return ca < cb;
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
}

// Every edit moves every caret, so a command that would create more carets
// than this stops there; typing stays responsive at the limit.
constexpr int kMaxCarets = 10000;

ImU32 with_alpha(ImU32 c, float a) {
    const int base = (int)((c >> IM_COL32_A_SHIFT) & 0xFF);
    const int alpha = ImClamp((int)(base * a + 0.5f), 0, 255);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
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

const char* cursor_style_name(CursorStyle s) {
    switch (s) {
        case CursorStyle::Line:          return "Line";
        case CursorStyle::Block:         return "Block";
        case CursorStyle::Underline:     return "Underline";
        case CursorStyle::LineThin:      return "Line (thin)";
        case CursorStyle::BlockOutline:  return "Block (outline)";
        case CursorStyle::UnderlineThin: return "Underline (thin)";
        default:                         return "";
    }
}

void draw_caret_shape(ImDrawList* dl, CursorStyle style, ImVec2 pos, float cell, float line_height,
                      float line_width, ImU32 col, float height_scale) {
    const float x = std::floor(pos.x);
    float top = pos.y + ui::px(1.0f);
    float bottom = pos.y + line_height - ui::px(1.0f);
    if (height_scale < 1.0f) {
        const float mid = (top + bottom) * 0.5f;
        const float half = (bottom - top) * 0.5f * height_scale;
        top = mid - half;
        bottom = mid + half;
    }
    if (bottom - top < 0.5f) return;
    switch (style) {
        case CursorStyle::Line:
            dl->AddRectFilled(ImVec2(x, top), ImVec2(x + ImMax(1.0f, line_width), bottom), col);
            break;
        case CursorStyle::LineThin:
            dl->AddRectFilled(ImVec2(x, top), ImVec2(x + 1.0f, bottom), col);
            break;
        case CursorStyle::Block:
            dl->AddRectFilled(ImVec2(x, top), ImVec2(x + cell, bottom), col);
            break;
        case CursorStyle::BlockOutline:
            dl->AddRect(ImVec2(x + 0.5f, top + 0.5f), ImVec2(x + cell - 0.5f, bottom - 0.5f), col, 0.0f, 0, 1.0f);
            break;
        case CursorStyle::Underline: {
            const float h = ImMax(2.0f, std::floor(ui::px(2.0f)));
            dl->AddRectFilled(ImVec2(x, bottom - h), ImVec2(x + cell, bottom), col);
            break;
        }
        case CursorStyle::UnderlineThin:
            dl->AddRectFilled(ImVec2(x, bottom - 1.0f), ImVec2(x + cell, bottom), col);
            break;
        default:
            break;
    }
}

void caret_blink_state(CursorBlink blink, double t, float& alpha, float& height_scale) {
    alpha = 1.0f;
    height_scale = 1.0f;
    // Every mode holds the caret solid for a moment after it moves, so it is
    // never invisible right where the user is looking.
    if (t <= 0.5) return;
    const double tt = t - 0.5;
    const float pi2 = 6.2831853f;
    const float wave = std::cos((float)std::fmod(tt, 1.2) / 1.2f * pi2);
    switch (blink) {
        case CursorBlink::Blink:  alpha = std::fmod(tt, 1.06) < 0.53 ? 0.0f : 1.0f; break;
        case CursorBlink::Smooth: alpha = 0.5f + 0.5f * wave; break;
        case CursorBlink::Phase:  alpha = ImSaturate(0.5f + wave * 0.9f); break;
        case CursorBlink::Expand: height_scale = ImSaturate(0.5f + wave * 0.9f); break;
        default: break;
    }
}

const char* cursor_blink_name(CursorBlink b) {
    switch (b) {
        case CursorBlink::Blink:  return "Blink";
        case CursorBlink::Smooth: return "Smooth";
        case CursorBlink::Phase:  return "Phase";
        case CursorBlink::Expand: return "Expand";
        case CursorBlink::Solid:  return "Solid";
        default:                  return "";
    }
}

const char* whitespace_name(Whitespace w) {
    switch (w) {
        case Whitespace::None:      return "None";
        case Whitespace::Boundary:  return "Boundary";
        case Whitespace::Selection: return "Selection";
        case Whitespace::Trailing:  return "Trailing";
        case Whitespace::All:       return "All";
        default:                    return "";
    }
}

const char* case_name(CaseKind k) {
    switch (k) {
        case CaseKind::Upper:  return "Upper Case";
        case CaseKind::Lower:  return "Lower Case";
        case CaseKind::Title:  return "Title Case";
        case CaseKind::Snake:  return "Snake Case";
        case CaseKind::Camel:  return "Camel Case";
        case CaseKind::Pascal: return "Pascal Case";
        case CaseKind::Kebab:  return "Kebab Case";
        default:               return "";
    }
}

TextEditor::TextEditor() {
    lines_.assign(1, std::string());
    line_state_.assign(1, 0);
    line_depth_.assign(1, 0);
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

    carets_.assign(1, Caret());
    primary_ = 0;
    caret_history_.clear();
    expand_stack_.clear();
    expanded_to_.clear();
    undo_.clear();
    undo_index_ = 0;
    saved_undo_index_ = 0;
    edit_depth_ = 0;
    pending_ = UndoStep();
    break_merge_ = true;
    version_ = 0;
    ghost_.clear();
    matches_.clear();
    matches_for_.clear();
    caret_anim_ = ImVec2(-1.0f, -1.0f);
    bracket_cache_version_ = ~0u;
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
    if (lang == language_ && !line_state_.empty()) return;
    language_ = lang;
    bracket_cache_version_ = ~0u;
    rebuild_line_states();
}

void TextEditor::set_use_crlf(bool crlf) {
    if (crlf == use_crlf_) return;
    use_crlf_ = crlf;
    // The buffer is unchanged but the file on disk would not be, so the tab
    // shows as modified until the next save.
    saved_undo_index_ = -1;
    ++version_;
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

int TextEditor::count_chars(Coord a, Coord b) const {
    a = sanitize(a);
    b = sanitize(b);
    if (b < a) std::swap(a, b);
    if (a.line == b.line) return b.column - a.column;
    int n = (int)lines_[a.line].size() - a.column + 1;
    for (int l = a.line + 1; l < b.line; ++l) n += (int)lines_[l].size() + 1;
    return n + b.column;
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

Coord TextEditor::end_of_insert(Coord start, const std::string& text) {
    int line = start.line;
    int col = start.column;
    for (char ch : text) {
        if (ch == '\r') continue;
        if (ch == '\n') { ++line; col = 0; }
        else ++col;
    }
    return Coord(line, col);
}

// ---------------------------------------------------------- transactions

void TextEditor::begin_edit(int kind) {
    if (edit_depth_++ > 0) return;
    pending_ = UndoStep();
    pending_.before = carets_;
    pending_.primary_before = primary_;
    pending_.kind = kind;
    dirty_from_ = -1;
}

void TextEditor::end_edit() {
    if (edit_depth_ <= 0) return;
    if (--edit_depth_ > 0) return;

    normalize_carets();
    if (pending_.edits.empty()) {
        pending_ = UndoStep();
        return;
    }
    pending_.after = carets_;
    pending_.primary_after = primary_;
    const double now = ImGui::GetTime();
    pending_.time = now;

    undo_.resize(undo_index_);
    // Editing after an undo discards the redo branch; if the saved state lived
    // in that branch it can never be reached again.
    if (saved_undo_index_ > (int)undo_.size()) saved_undo_index_ = -1;

    // A run of typing (or of deleting) is one undo step, the way it is in every
    // editor: same kind, under a second apart, and nothing moved the carets in
    // between. Never across the saved point, or the dirty marker would lie.
    bool merged = false;
    if (pending_.kind != kEditOther && !break_merge_ && !undo_.empty() &&
        saved_undo_index_ != (int)undo_.size()) {
        UndoStep& prev = undo_.back();
        if (prev.kind == pending_.kind && now - prev.time < 1.0 &&
            same_carets(prev.after, pending_.before)) {
            for (Edit& e : pending_.edits) prev.edits.push_back(std::move(e));
            prev.after = pending_.after;
            prev.primary_after = pending_.primary_after;
            prev.time = now;
            merged = true;
        }
    }
    if (!merged) undo_.push_back(std::move(pending_));
    pending_ = UndoStep();
    undo_index_ = (int)undo_.size();
    break_merge_ = false;

    ++version_;
    ghost_.clear();
    matches_for_.clear();
    expand_stack_.clear();
    rebuild_line_states(dirty_from_ < 0 ? 0 : dirty_from_);
    refresh_metrics();
    scroll_to_cursor_ = true;
    blink_reset_ = now;
}

Coord TextEditor::apply_edit(Coord a, Coord b, const std::string& text_in) {
    if (read_only_) return sanitize(a);
    const bool own = edit_depth_ == 0;
    if (own) begin_edit();

    a = sanitize(a);
    b = sanitize(b);
    if (b < a) std::swap(a, b);

    std::string text;
    text.reserve(text_in.size());
    for (char ch : text_in)
        if (ch != '\r') text += ch;

    Coord end = a;
    if (a != b || !text.empty()) {
        Edit e;
        e.start = a;
        e.removed = range_text(a, b);
        e.added = text;
        raw_delete(a, b);
        end = raw_insert(a, text);
        shift_carets(a, b, end);
        pending_.edits.push_back(std::move(e));
        if (dirty_from_ < 0 || a.line < dirty_from_) dirty_from_ = a.line;
    }
    if (own) end_edit();
    return end;
}

// Moves every caret the way the text around it moved: positions before the
// change stay, positions inside the removed range fall to its start, and
// positions after it (including one sitting exactly where text was inserted)
// travel with the text that follows.
void TextEditor::shift_carets(Coord a, Coord b, Coord end) {
    auto shift = [&](Coord p) {
        if (p < a) return p;
        if (p < b) return a;
        if (p.line == b.line) return Coord(end.line, end.column + (p.column - b.column));
        return Coord(p.line + (end.line - b.line), p.column);
    };
    for (Caret& c : carets_) {
        c.pos = shift(c.pos);
        c.anchor = shift(c.anchor);
    }
}

void TextEditor::undo() {
    if (!can_undo() || edit_depth_ > 0) return;
    const UndoStep& step = undo_[--undo_index_];
    int from = INT_MAX;
    for (auto it = step.edits.rbegin(); it != step.edits.rend(); ++it) {
        raw_delete(it->start, end_of_insert(it->start, it->added));
        raw_insert(it->start, it->removed);
        from = ImMin(from, it->start.line);
    }
    carets_ = step.before;
    primary_ = step.primary_before;
    after_history_move(from);
}

void TextEditor::redo() {
    if (!can_redo() || edit_depth_ > 0) return;
    const UndoStep& step = undo_[undo_index_++];
    int from = INT_MAX;
    for (const Edit& e : step.edits) {
        raw_delete(e.start, end_of_insert(e.start, e.removed));
        raw_insert(e.start, e.added);
        from = ImMin(from, e.start.line);
    }
    carets_ = step.after;
    primary_ = step.primary_after;
    after_history_move(from);
}

void TextEditor::after_history_move(int from_line) {
    if (carets_.empty()) carets_.push_back(Caret());
    normalize_carets();
    ++version_;
    ghost_.clear();
    matches_for_.clear();
    expand_stack_.clear();
    break_merge_ = true;
    rebuild_line_states(from_line == INT_MAX ? 0 : ImClamp(from_line, 0, (int)lines_.size() - 1));
    refresh_metrics();
    scroll_to_cursor_ = true;
    blink_reset_ = ImGui::GetTime();
}

// ----------------------------------------------------------------- carets

bool TextEditor::same_carets(const std::vector<Caret>& a, const std::vector<Caret>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!a[i].same_place(b[i])) return false;
    return true;
}

// Keeps the caret list in document order with no two carets overlapping, the
// invariant every command relies on. Merging keeps track of which caret was
// the primary one.
void TextEditor::normalize_carets() {
    if (carets_.empty()) {
        carets_.push_back(Caret());
        primary_ = 0;
        return;
    }
    primary_ = ImClamp(primary_, 0, (int)carets_.size() - 1);
    for (Caret& c : carets_) {
        c.pos = sanitize(c.pos);
        c.anchor = sanitize(c.anchor);
    }
    if (carets_.size() == 1) {
        primary_ = 0;
        return;
    }

    struct Item {
        Caret c;
        bool primary;
    };
    std::vector<Item> items;
    items.reserve(carets_.size());
    for (size_t i = 0; i < carets_.size(); ++i) items.push_back({carets_[i], (int)i == primary_});
    std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) {
        if (x.c.start() != y.c.start()) return x.c.start() < y.c.start();
        return x.c.end() < y.c.end();
    });

    std::vector<Item> out;
    out.reserve(items.size());
    for (const Item& it : items) {
        if (!out.empty()) {
            Item& last = out.back();
            const Coord le = last.c.end();
            const Coord s = it.c.start();
            const bool touching = s == le && (!it.c.has_selection() || !last.c.has_selection());
            if (s < le || touching) {
                const Coord ns = last.c.start();
                const Coord ne = ImMax(le, it.c.end());
                const Caret& dir = last.c.has_selection() ? last.c : it.c;
                const bool backward = dir.pos < dir.anchor;
                last.c = backward ? Caret(ne, ns) : Caret(ns, ne);
                last.primary = last.primary || it.primary;
                continue;
            }
        }
        out.push_back(it);
    }

    carets_.clear();
    primary_ = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        carets_.push_back(out[i].c);
        if (out[i].primary) primary_ = (int)i;
    }
}

void TextEditor::push_caret_history() {
    if (!caret_history_.empty() && same_carets(caret_history_.back().carets, carets_)) return;
    caret_history_.push_back({carets_, primary_});
    if (caret_history_.size() > 64) caret_history_.erase(caret_history_.begin());
}

void TextEditor::set_single_caret(Caret c) {
    carets_.assign(1, c);
    primary_ = 0;
    normalize_carets();
    caret_moved();
}

void TextEditor::caret_moved() {
    scroll_to_cursor_ = true;
    blink_reset_ = ImGui::GetTime();
    ghost_.clear();
    occurrence_whole_word_ = false;
}

std::vector<TextEditor::LineBlock> TextEditor::line_blocks(bool merge_adjacent) const {
    std::vector<LineBlock> blocks;
    for (int i = 0; i < (int)carets_.size(); ++i) {
        const Caret& c = carets_[i];
        int first = c.start().line;
        int last = c.end().line;
        // A selection that ends at the start of a line does not take that line.
        if (c.has_selection() && c.end().column == 0 && last > first) --last;
        if (!blocks.empty() &&
            (first <= blocks.back().last || (merge_adjacent && first == blocks.back().last + 1))) {
            blocks.back().last = ImMax(blocks.back().last, last);
            blocks.back().carets.push_back(i);
            continue;
        }
        blocks.push_back({first, last, {i}});
    }
    return blocks;
}

bool TextEditor::word_at(Coord c, Coord& a, Coord& b) const {
    c = sanitize(c);
    const std::string& l = lines_[c.line];
    int s = c.column;
    int e = c.column;
    while (s > 0 && is_ident(l[s - 1])) --s;
    while (e < (int)l.size() && is_ident(l[e])) ++e;
    if (s == e) return false;
    a = Coord(c.line, s);
    b = Coord(c.line, e);
    return true;
}

// ---------------------------------------------------------- selection API

std::string TextEditor::selected_text() const {
    std::string out;
    bool first = true;
    for (const Caret& c : carets_) {
        if (!c.has_selection()) continue;
        if (!first) out += '\n';
        out += range_text(c.start(), c.end());
        first = false;
    }
    return out;
}

int TextEditor::selected_chars() const {
    int n = 0;
    for (const Caret& c : carets_)
        if (c.has_selection()) n += count_chars(c.start(), c.end());
    return n;
}

void TextEditor::select_all() {
    push_caret_history();
    carets_.assign(1, Caret(Coord(0, 0), doc_end()));
    primary_ = 0;
    blink_reset_ = ImGui::GetTime();
}

void TextEditor::select_range(Coord a, Coord b) {
    a = sanitize(a);
    b = sanitize(b);
    if (b < a) std::swap(a, b);
    carets_.assign(1, Caret(a, b));
    primary_ = 0;
    scroll_to_cursor_ = true;
    blink_reset_ = ImGui::GetTime();
}

void TextEditor::clear_selection() {
    for (Caret& c : carets_) c.anchor = c.pos;
    normalize_carets();
}

void TextEditor::set_cursor(Coord c, bool scroll) {
    carets_.assign(1, Caret(sanitize(c)));
    primary_ = 0;
    if (scroll) scroll_to_cursor_ = true;
    blink_reset_ = ImGui::GetTime();
}

void TextEditor::goto_line(int line_index, int column) {
    push_caret_history();
    const int l = ImClamp(line_index, 0, (int)lines_.size() - 1);
    set_cursor(Coord(l, column));
    center_cursor_ = true;
}

std::string TextEditor::context_before(int max_bytes) const {
    std::string out;
    Coord c = cursor();
    out = lines_[c.line].substr(0, c.column);
    for (int l = c.line - 1; l >= 0 && (int)out.size() < max_bytes; --l)
        out = lines_[l] + "\n" + out;
    if ((int)out.size() > max_bytes) out = out.substr(out.size() - max_bytes);
    return out;
}

std::string TextEditor::context_after(int max_bytes) const {
    const Coord c = cursor();
    std::string out = lines_[c.line].substr(c.column);
    for (int l = c.line + 1; l < (int)lines_.size() && (int)out.size() < max_bytes; ++l) {
        out += "\n";
        out += lines_[l];
    }
    if ((int)out.size() > max_bytes) out = out.substr(0, max_bytes);
    return out;
}

// --------------------------------------------------------------- editing

void TextEditor::insert_text(const std::string& text) {
    if (read_only_) return;
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        c = Caret(apply_edit(c.start(), c.end(), text));
    }
    end_edit();
}

void TextEditor::replace_selection(const std::string& text) { insert_text(text); }

void TextEditor::replace_all_text(const std::string& text) {
    if (read_only_) return;
    begin_edit();
    apply_edit(Coord(0, 0), doc_end(), text);
    carets_.assign(1, Caret());
    primary_ = 0;
    end_edit();
    scroll_to_cursor_ = true;
}

void TextEditor::accept_ghost() {
    if (ghost_.empty() || read_only_) return;
    std::string text = ghost_;
    ghost_.clear();
    begin_edit();
    Caret& p = carets_[primary_];
    const Coord e = apply_edit(p.pos, p.pos, text);
    carets_.assign(1, Caret(e));
    primary_ = 0;
    end_edit();
}

void TextEditor::copy() {
    std::string text;
    bool any_selection = false;
    for (const Caret& c : carets_) any_selection = any_selection || c.has_selection();
    if (any_selection) {
        text = selected_text();
        full_line_clip_.clear();
    } else {
        // Nothing selected copies the caret lines whole, like VS Code.
        int last_line = -1;
        for (const Caret& c : carets_) {
            if (c.pos.line == last_line) continue;
            last_line = c.pos.line;
            text += lines_[c.pos.line] + "\n";
        }
        full_line_clip_ = text;
    }
    ImGui::SetClipboardText(text.c_str());
}

void TextEditor::cut() {
    copy();
    if (read_only_) return;
    bool any_selection = false;
    for (const Caret& c : carets_) any_selection = any_selection || c.has_selection();
    if (!any_selection) {
        delete_lines();
        return;
    }
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        if (c.has_selection()) c = Caret(apply_edit(c.start(), c.end(), ""));
    }
    end_edit();
}

void TextEditor::paste() {
    if (read_only_) return;
    const char* raw = ImGui::GetClipboardText();
    if (!raw || !*raw) return;
    std::string text;
    for (const char* p = raw; *p; ++p)
        if (*p != '\r') text += *p;

    bool any_selection = false;
    for (const Caret& c : carets_) any_selection = any_selection || c.has_selection();

    begin_edit();
    if (!any_selection && !full_line_clip_.empty() && text == full_line_clip_) {
        // Whole lines copied without a selection go back in as whole lines,
        // above the caret, rather than splitting the line the caret is on.
        for (size_t i = 0; i < carets_.size(); ++i) {
            const Coord at(carets_[i].pos.line, 0);
            apply_edit(at, at, text);
        }
        end_edit();
        return;
    }

    // As many lines on the clipboard as there are carets: one line each.
    std::vector<std::string> parts;
    if (carets_.size() > 1) {
        std::string body = text;
        if (!body.empty() && body.back() == '\n') body.pop_back();
        size_t start = 0;
        while (true) {
            size_t nl = body.find('\n', start);
            parts.push_back(body.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        if (parts.size() != carets_.size()) parts.clear();
    }
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        c = Caret(apply_edit(c.start(), c.end(), parts.empty() ? text : parts[i]));
    }
    end_edit();
}

void TextEditor::type_char(unsigned int cp) {
    if (read_only_) return;

    char utf8[5] = {0};
    int len = ImTextCharToUtf8(utf8, cp) ? (int)std::strlen(utf8) : 0;
    if (len == 0) return;
    const std::string s(utf8, utf8 + len);

    const char* open = "([{\"'";
    const char* close = ")]}\"'";

    begin_edit(kEditTyping);
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        const std::string& cur = lines_[c.pos.line];
        const char* found = cp < 128 ? std::strchr(open, (int)cp) : nullptr;

        if (auto_close_brackets && cp < 128) {
            // Wrap the selection instead of replacing it.
            if (found && c.has_selection()) {
                const Coord s0 = c.start();
                const Coord e0 = c.end();
                const bool backward = c.pos < c.anchor;
                apply_edit(e0, e0, std::string(1, close[found - open]));
                const Coord inner_start = apply_edit(s0, s0, s);
                const Coord inner_end =
                    e0.line == s0.line ? Coord(e0.line, e0.column + len) : e0;
                c = backward ? Caret(inner_end, inner_start) : Caret(inner_start, inner_end);
                continue;
            }
            // Type over a closing character that is already there.
            if (!c.has_selection() && c.pos.column < (int)cur.size() &&
                std::strchr(close, (int)cp) && cur[c.pos.column] == (char)cp) {
                c = Caret(Coord(c.pos.line, c.pos.column + 1));
                continue;
            }
            if (found && !c.has_selection()) {
                const bool at_word = c.pos.column < (int)cur.size() && is_ident(cur[c.pos.column]);
                // Quotes only auto-pair when not adjacent to a word.
                const bool quote = (cp == '"' || cp == '\'');
                if (!at_word && !(quote && c.pos.column > 0 && is_ident(cur[c.pos.column - 1]))) {
                    const Coord e = apply_edit(c.pos, c.pos, s + std::string(1, close[found - open]));
                    c = Caret(Coord(e.line, e.column - 1));
                    continue;
                }
            }
            // Outdent a closing brace typed at the start of a line.
            if (cp == '}' && !c.has_selection()) {
                bool only_space = true;
                for (int k = 0; k < c.pos.column; ++k)
                    if (!is_space(cur[k])) { only_space = false; break; }
                if (only_space && c.pos.column > 0) {
                    const int width = insert_spaces ? tab_size : 1;
                    const int remove = ImMin(width, c.pos.column);
                    c = Caret(apply_edit(Coord(c.pos.line, c.pos.column - remove), c.pos, s));
                    continue;
                }
            }
        }

        if (overtype_ && !c.has_selection() && c.pos.column < (int)cur.size()) {
            c = Caret(apply_edit(c.pos, next_char(c.pos), s));
            continue;
        }
        c = Caret(apply_edit(c.start(), c.end(), s));
    }
    end_edit();
}

void TextEditor::insert_newline() {
    if (read_only_) return;
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        const Coord s0 = c.start();
        const Coord e0 = c.end();
        const std::string& l = lines_[s0.line];

        std::string indent;
        for (size_t k = 0; k < l.size() && is_space(l[k]); ++k) indent += l[k];
        if ((int)indent.size() > s0.column) indent.resize((size_t)s0.column);

        const std::string before = l.substr(0, s0.column);
        const std::string after = lines_[e0.line].substr(e0.column);

        std::string extra;
        char last = 0;
        for (int k = (int)before.size() - 1; k >= 0; --k) {
            if (!is_space(before[k])) { last = before[k]; break; }
        }
        if (last == '{' || last == '[' || last == '(' || last == ':')
            extra = insert_spaces ? std::string((size_t)tab_size, ' ') : std::string("\t");

        char next = 0;
        for (size_t k = 0; k < after.size(); ++k) {
            if (!is_space(after[k])) { next = after[k]; break; }
        }

        if (!extra.empty() && (next == '}' || next == ']' || next == ')')) {
            // Opening brace with the matching closer right after: expand both lines.
            apply_edit(s0, e0, "\n" + indent + extra + "\n" + indent);
            c = Caret(Coord(s0.line + 1, (int)(indent.size() + extra.size())));
            continue;
        }
        c = Caret(apply_edit(s0, e0, "\n" + indent + extra));
    }
    end_edit();
}

void TextEditor::tab_key(bool shift) {
    if (read_only_) return;
    if (shift) {
        outdent_lines();
        return;
    }
    for (const Caret& c : carets_) {
        if (c.has_selection() && c.start().line != c.end().line) {
            indent_lines();
            return;
        }
    }
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        std::string unit = "\t";
        if (insert_spaces) {
            const Coord s0 = c.start();
            const int col = (int)(column_to_x(lines_[s0.line], s0.column) / space_width_ + 0.5f);
            unit.assign((size_t)(tab_size - col % tab_size), ' ');
        }
        c = Caret(apply_edit(c.start(), c.end(), unit));
    }
    end_edit();
}

void TextEditor::backspace_or_delete(int direction) {
    if (read_only_) return;
    begin_edit(direction < 0 ? kEditDeleteBack : kEditDeleteForward);
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        if (c.has_selection()) {
            c = Caret(apply_edit(c.start(), c.end(), ""));
            continue;
        }
        const Coord p = c.pos;
        const std::string& l = lines_[p.line];
        if (direction < 0) {
            // Backspace over a full indent step when only whitespace precedes.
            if (insert_spaces && p.column > 0) {
                bool only_space = true;
                for (int k = 0; k < p.column; ++k)
                    if (l[k] != ' ') { only_space = false; break; }
                if (only_space) {
                    int steps = p.column % tab_size;
                    if (steps == 0) steps = tab_size;
                    steps = ImMin(steps, p.column);
                    c = Caret(apply_edit(Coord(p.line, p.column - steps), p, ""));
                    continue;
                }
            }
            // Between an empty auto-closed pair: remove both halves.
            if (auto_close_brackets && p.column > 0 && p.column < (int)l.size()) {
                const char a = l[p.column - 1];
                const char b = l[p.column];
                if ((a == '(' && b == ')') || (a == '[' && b == ']') || (a == '{' && b == '}') ||
                    (a == '"' && b == '"') || (a == '\'' && b == '\'')) {
                    c = Caret(apply_edit(Coord(p.line, p.column - 1), Coord(p.line, p.column + 1), ""));
                    continue;
                }
            }
            const Coord prev = prev_char(p);
            if (prev != p) c = Caret(apply_edit(prev, p, ""));
        } else {
            const Coord next = next_char(p);
            if (next != p) c = Caret(apply_edit(p, next, ""));
        }
    }
    end_edit();
}

void TextEditor::delete_word(int direction) {
    if (read_only_) return;
    begin_edit(direction < 0 ? kEditDeleteBack : kEditDeleteForward);
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        if (c.has_selection()) {
            c = Caret(apply_edit(c.start(), c.end(), ""));
            continue;
        }
        const Coord other = direction < 0 ? word_start(c.pos) : word_end(c.pos);
        c = Caret(apply_edit(ImMin(other, c.pos), ImMax(other, c.pos), ""));
    }
    end_edit();
}

void TextEditor::delete_to_line_edge(int direction) {
    if (read_only_) return;
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        if (c.has_selection()) {
            c = Caret(apply_edit(c.start(), c.end(), ""));
            continue;
        }
        const Coord edge = direction < 0 ? Coord(c.pos.line, 0)
                                         : Coord(c.pos.line, (int)lines_[c.pos.line].size());
        c = Caret(apply_edit(ImMin(edge, c.pos), ImMax(edge, c.pos), ""));
    }
    end_edit();
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

namespace {
enum Move {
    kLeft, kRight, kWordLeft, kWordRight, kUp, kDown, kHome, kEnd, kDocStart, kDocEnd,
    kPageUp, kPageDown
};
} // namespace

void TextEditor::move_carets(int how, bool extend) {
    const int page = ImMax(1, (int)(view_height_ / line_height_) - 2);
    for (Caret& c : carets_) {
        Coord target = c.pos;
        bool vertical = false;
        switch (how) {
            case kLeft:
                target = (!extend && c.has_selection()) ? c.start() : prev_char(c.pos);
                break;
            case kRight:
                target = (!extend && c.has_selection()) ? c.end() : next_char(c.pos);
                break;
            case kWordLeft:  target = word_start(c.pos); break;
            case kWordRight: target = word_end(c.pos); break;
            case kUp: case kDown: case kPageUp: case kPageDown: {
                vertical = true;
                const int step = how == kUp ? -1 : how == kDown ? 1 : how == kPageUp ? -page : page;
                if (c.preferred_x < 0.0f) c.preferred_x = column_to_x(lines_[c.pos.line], c.pos.column);
                const int line = c.pos.line + step;
                // Up on the first line goes to its start, down on the last to its end.
                if (line < 0) target = Coord(0, 0);
                else if (line >= (int)lines_.size()) target = doc_end();
                else target = Coord(line, x_to_column(lines_[line], c.preferred_x));
                break;
            }
            case kHome: {
                const Coord first = line_first_non_space(c.pos.line);
                target = c.pos == first ? Coord(c.pos.line, 0) : first;
                break;
            }
            case kEnd:      target = Coord(c.pos.line, (int)lines_[c.pos.line].size()); break;
            case kDocStart: target = Coord(0, 0); break;
            case kDocEnd:   target = doc_end(); break;
            default: break;
        }
        const float keep_x = vertical ? c.preferred_x : -1.0f;
        if (extend) c.pos = sanitize(target);
        else c = Caret(sanitize(target));
        c.preferred_x = keep_x;
    }
    normalize_carets();
    caret_moved();
}

// ---------------------------------------------------------- multi-caret

void TextEditor::add_caret_vertical(int direction) {
    push_caret_history();
    // Grow the column from the caret furthest in that direction, so pressing
    // the key again keeps adding lines.
    const int idx = direction < 0 ? 0 : (int)carets_.size() - 1;
    Caret& src = carets_[idx];
    const int line = src.pos.line + direction;
    if (line < 0 || line >= (int)lines_.size()) return;
    if (src.preferred_x < 0.0f) src.preferred_x = column_to_x(lines_[src.pos.line], src.pos.column);
    Caret added(Coord(line, x_to_column(lines_[line], src.preferred_x)));
    added.preferred_x = src.preferred_x;
    carets_.push_back(added);
    primary_ = (int)carets_.size() - 1;
    normalize_carets();
    caret_moved();
}

bool TextEditor::find_in_line(const std::string& line, int from, const std::string& needle,
                              bool whole_word, bool case_sensitive, int& at) const {
    if (needle.empty()) return false;
    const std::string hay = case_sensitive ? line : lower_ascii(line);
    const std::string pin = case_sensitive ? needle : lower_ascii(needle);
    size_t pos = (size_t)ImMax(0, from);
    while ((pos = hay.find(pin, pos)) != std::string::npos) {
        const size_t end = pos + pin.size();
        const bool left_ok = pos == 0 || !is_ident(line[pos - 1]) || !is_ident(line[pos]);
        const bool right_ok = end >= line.size() || !is_ident(line[end]) || !is_ident(line[end - 1]);
        if (!whole_word || (left_ok && right_ok)) {
            at = (int)pos;
            return true;
        }
        ++pos;
    }
    return false;
}

void TextEditor::add_next_occurrence() {
    Caret& p = carets_[primary_];
    if (!p.has_selection()) {
        // The first press selects the word under every caret.
        push_caret_history();
        bool any = false;
        for (Caret& c : carets_) {
            Coord a, b;
            if (!c.has_selection() && word_at(c.pos, a, b)) {
                c = Caret(a, b);
                any = true;
            }
        }
        if (!any) return;
        normalize_carets();
        caret_moved();
        occurrence_whole_word_ = true;
        return;
    }
    const Coord ps = p.start();
    const Coord pe = p.end();
    if (ps.line != pe.line) return;
    const std::string needle = range_text(ps, pe);
    const bool whole = occurrence_whole_word_;
    const bool cs = occurrence_whole_word_ || find_case_sensitive;
    const int n = (int)lines_.size();

    // Search forward from the newest selection, wrapping once around the file.
    for (int step = 0; step <= n; ++step) {
        const int l = (pe.line + step) % n;
        int from = step == 0 ? pe.column : 0;
        int at = 0;
        while (find_in_line(lines_[l], from, needle, whole, cs, at)) {
            const Coord a(l, at);
            const Coord b(l, at + (int)needle.size());
            if (step == n && !(a < ps)) return; // wrapped back to where we started
            bool taken = false;
            for (const Caret& c : carets_)
                if (c.start() == a && c.end() == b) { taken = true; break; }
            if (!taken) {
                push_caret_history();
                carets_.push_back(Caret(a, b));
                primary_ = (int)carets_.size() - 1;
                normalize_carets();
                caret_moved();
                occurrence_whole_word_ = whole;
                return;
            }
            from = at + 1;
        }
    }
}

void TextEditor::select_all_occurrences() {
    Caret p = carets_[primary_];
    bool whole = false;
    if (!p.has_selection()) {
        Coord a, b;
        if (!word_at(p.pos, a, b)) return;
        p = Caret(a, b);
        whole = true;
    }
    const Coord ps = p.start();
    const Coord pe = p.end();
    if (ps.line != pe.line) return;
    const std::string needle = range_text(ps, pe);
    const bool cs = whole || find_case_sensitive;

    std::vector<Caret> found;
    int primary = 0;
    for (int l = 0; l < (int)lines_.size(); ++l) {
        int from = 0, at = 0;
        while (find_in_line(lines_[l], from, needle, whole, cs, at)) {
            const Coord a(l, at);
            if (a == ps) primary = (int)found.size();
            found.push_back(Caret(a, Coord(l, at + (int)needle.size())));
            from = at + (int)needle.size();
            if ((int)found.size() >= kMaxCarets) break;
        }
        if ((int)found.size() >= kMaxCarets) break;
    }
    if (found.empty()) return;
    push_caret_history();
    carets_ = found;
    primary_ = primary;
    normalize_carets();
    caret_moved();
}

void TextEditor::carets_to_line_ends() {
    push_caret_history();
    std::vector<Caret> out;
    int primary = 0;
    for (int i = 0; i < (int)carets_.size(); ++i) {
        const Caret& c = carets_[i];
        if (i == primary_) primary = (int)out.size();
        if (!c.has_selection()) {
            out.push_back(Caret(Coord(c.pos.line, (int)lines_[c.pos.line].size())));
            continue;
        }
        const Coord s = c.start();
        const Coord e = c.end();
        for (int l = s.line; l <= e.line && (int)out.size() < kMaxCarets; ++l) {
            if (l == e.line && e.column == 0 && l > s.line) break;
            out.push_back(Caret(Coord(l, l == e.line ? e.column : (int)lines_[l].size())));
        }
    }
    carets_ = out;
    primary_ = ImClamp(primary, 0, (int)out.size() - 1);
    normalize_carets();
    caret_moved();
}

void TextEditor::cursor_undo() {
    if (caret_history_.empty()) return;
    CaretSet s = caret_history_.back();
    caret_history_.pop_back();
    carets_ = s.carets;
    primary_ = s.primary;
    normalize_carets();
    caret_moved();
}

void TextEditor::collapse_carets() {
    if (carets_.size() > 1) {
        push_caret_history();
        const Caret keep = carets_[primary_];
        carets_.assign(1, keep);
        primary_ = 0;
    } else {
        carets_[0].anchor = carets_[0].pos;
    }
    caret_moved();
}

void TextEditor::box_select(int line_a, float x_a, int line_b, float x_b) {
    line_a = ImClamp(line_a, 0, (int)lines_.size() - 1);
    line_b = ImClamp(line_b, 0, (int)lines_.size() - 1);
    const int step = line_a <= line_b ? 1 : -1;
    const float left = ImMin(x_a, x_b);
    std::vector<Caret> out;
    for (int l = line_a;; l += step) {
        const std::string& text = lines_[l];
        // Lines that end before a box with width are skipped, as in VS Code;
        // a zero-width box puts a caret on every line.
        const bool short_line = x_a != x_b && left > text_width(text) + space_width_ * 0.5f;
        if (!short_line || l == line_b) {
            Caret c(Coord(l, x_to_column(text, x_a)), Coord(l, x_to_column(text, x_b)));
            c.preferred_x = x_b;
            out.push_back(c);
        }
        if (l == line_b || (int)out.size() >= kMaxCarets) break;
    }
    carets_ = out;
    primary_ = (int)out.size() - 1;
    normalize_carets();
    blink_reset_ = ImGui::GetTime();
}

// -------------------------------------------------------------- brackets

void TextEditor::line_brackets(int line, std::vector<std::pair<int, char>>& out) const {
    out.clear();
    if (line < 0 || line >= (int)lines_.size()) return;
    const std::string& l = lines_[line];
    if (l.find_first_of("()[]{}") == std::string::npos) return;
    std::vector<Token> tokens;
    tokenize(language_, l, line < (int)line_state_.size() ? line_state_[line] : 0, tokens);
    for (const Token& t : tokens) {
        if (t.kind != Tok::Punct) continue;
        for (int i = t.start; i < t.end; ++i)
            if (is_open_bracket(l[i]) || is_close_bracket(l[i])) out.push_back({i, l[i]});
    }
}

bool TextEditor::is_code_bracket(Coord c) const {
    if (c.line < 0 || c.line >= (int)lines_.size()) return false;
    const std::string& l = lines_[c.line];
    if (c.column < 0 || c.column >= (int)l.size()) return false;
    if (!is_open_bracket(l[c.column]) && !is_close_bracket(l[c.column])) return false;
    std::vector<std::pair<int, char>> br;
    line_brackets(c.line, br);
    for (const auto& b : br)
        if (b.first == c.column) return true;
    return false;
}

bool TextEditor::match_bracket(Coord at, Coord& match, int limit) const {
    if (!is_code_bracket(at)) return false;
    const char ch = lines_[at.line][at.column];
    const char want = partner_of(ch);
    const bool forward = is_open_bracket(ch);
    std::vector<std::pair<int, char>> br;
    int depth = 0;
    if (forward) {
        for (int l = at.line; l < (int)lines_.size() && l - at.line < limit; ++l) {
            line_brackets(l, br);
            for (const auto& b : br) {
                if (l == at.line && b.first <= at.column) continue;
                if (b.second == ch) ++depth;
                else if (b.second == want) {
                    if (depth == 0) { match = Coord(l, b.first); return true; }
                    --depth;
                }
            }
        }
    } else {
        for (int l = at.line; l >= 0 && at.line - l < limit; --l) {
            line_brackets(l, br);
            for (auto it = br.rbegin(); it != br.rend(); ++it) {
                if (l == at.line && it->first >= at.column) continue;
                if (it->second == ch) ++depth;
                else if (it->second == want) {
                    if (depth == 0) { match = Coord(l, it->first); return true; }
                    --depth;
                }
            }
        }
    }
    return false;
}

// The nearest bracket pair that opens before `start` and closes at or after
// `end`.
bool TextEditor::find_enclosing_brackets(Coord start, Coord end, Coord& open, Coord& close) const {
    int depth[3] = {0, 0, 0};
    const int limit = 20000;
    std::vector<std::pair<int, char>> br;
    for (int l = start.line; l >= 0 && start.line - l < limit; --l) {
        line_brackets(l, br);
        for (auto it = br.rbegin(); it != br.rend(); ++it) {
            if (l == start.line && it->first >= start.column) continue;
            const int t = bracket_type(it->second);
            if (is_close_bracket(it->second)) { ++depth[t]; continue; }
            if (depth[t] > 0) { --depth[t]; continue; }
            Coord m;
            const Coord o(l, it->first);
            if (match_bracket(o, m) && !(m < end)) {
                open = o;
                close = m;
                return true;
            }
        }
    }
    return false;
}

bool TextEditor::string_token_at(Coord start, Coord end, Coord& a, Coord& b) const {
    if (start.line != end.line) return false;
    const std::string& l = lines_[start.line];
    std::vector<Token> tokens;
    tokenize(language_, l, start.line < (int)line_state_.size() ? line_state_[start.line] : 0, tokens);
    for (const Token& t : tokens) {
        if (t.kind != Tok::String) continue;
        if (t.start <= start.column && end.column <= t.end) {
            a = Coord(start.line, t.start);
            b = Coord(start.line, t.end);
            return true;
        }
    }
    return false;
}

void TextEditor::goto_matching_bracket() {
    push_caret_history();
    for (Caret& c : carets_) {
        const Coord p = c.pos;
        Coord m, open, close;
        if (match_bracket(p, m)) c = Caret(m);
        else if (p.column > 0 && match_bracket(Coord(p.line, p.column - 1), m)) c = Caret(m);
        else if (find_enclosing_brackets(p, p, open, close)) c = Caret(close);
    }
    normalize_carets();
    caret_moved();
}

void TextEditor::select_to_bracket() {
    push_caret_history();
    for (Caret& c : carets_) {
        Coord open, close;
        if (find_enclosing_brackets(c.start(), c.end(), open, close))
            c = Caret(open, Coord(close.line, close.column + 1));
    }
    normalize_carets();
    caret_moved();
}

void TextEditor::expand_selection() {
    if (!same_carets(carets_, expanded_to_)) expand_stack_.clear();
    std::vector<Caret> next = carets_;
    bool changed = false;
    for (Caret& c : next) {
        const Coord s = c.start();
        const Coord e = c.end();
        const int current = count_chars(s, e);
        Coord best_a = s, best_b = e;
        int best = INT_MAX;
        auto consider = [&](Coord a, Coord b) {
            a = sanitize(a);
            b = sanitize(b);
            if (b < a) std::swap(a, b);
            if (!(a <= s && e <= b) || (a == s && b == e)) return;
            const int size = count_chars(a, b);
            if (size <= current || size >= best) return;
            best = size;
            best_a = a;
            best_b = b;
        };
        Coord a, b;
        if (s.line == e.line && word_at(s, a, b)) consider(a, b);
        if (string_token_at(s, e, a, b)) {
            consider(a, b);
            if (b.column - a.column >= 2) consider(Coord(a.line, a.column + 1), Coord(b.line, b.column - 1));
        }
        Coord open, close;
        if (find_enclosing_brackets(s, e, open, close)) {
            consider(Coord(open.line, open.column + 1), close);
            consider(open, Coord(close.line, close.column + 1));
        }
        consider(line_first_non_space(s.line), Coord(e.line, (int)lines_[e.line].size()));
        consider(Coord(s.line, 0), e.line + 1 < (int)lines_.size() ? Coord(e.line + 1, 0)
                                                                   : Coord(e.line, (int)lines_[e.line].size()));
        consider(Coord(0, 0), doc_end());
        if (best != INT_MAX) {
            c = Caret(best_a, best_b);
            changed = true;
        }
    }
    if (!changed) return;
    expand_stack_.push_back({carets_, primary_});
    carets_ = next;
    normalize_carets();
    expanded_to_ = carets_;
    scroll_to_cursor_ = true;
    blink_reset_ = ImGui::GetTime();
}

void TextEditor::shrink_selection() {
    if (expand_stack_.empty() || !same_carets(carets_, expanded_to_)) return;
    CaretSet s = expand_stack_.back();
    expand_stack_.pop_back();
    carets_ = s.carets;
    primary_ = s.primary;
    normalize_carets();
    expanded_to_ = carets_;
    scroll_to_cursor_ = true;
    blink_reset_ = ImGui::GetTime();
}

void TextEditor::select_line() {
    for (Caret& c : carets_) {
        const Coord s = c.start();
        Coord e = c.end();
        // Already whole lines: take one more.
        int last = e.line;
        if (c.has_selection() && s.column == 0 && e.column == 0) last = e.line;
        else last = e.line + 1;
        const Coord b = last < (int)lines_.size() ? Coord(last, 0) : doc_end();
        c = Caret(Coord(s.line, 0), b);
    }
    normalize_carets();
    caret_moved();
}

// ---------------------------------------------------------- line commands

void TextEditor::copy_lines(int direction) {
    if (read_only_) return;
    const std::vector<LineBlock> blocks = line_blocks(false);
    begin_edit();
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        std::string text;
        for (int l = it->first; l <= it->last; ++l) text += lines_[l] + "\n";
        std::vector<Caret> saved;
        for (int idx : it->carets) saved.push_back(carets_[idx]);
        apply_edit(Coord(it->first, 0), Coord(it->first, 0), text);
        // The copy went in above, so the original moved down: carets follow it
        // when copying down and stay on the (identical) upper copy otherwise.
        const int n = it->last - it->first + 1;
        for (size_t k = 0; k < saved.size(); ++k) {
            Caret c = saved[k];
            if (direction > 0) {
                c.pos.line += n;
                c.anchor.line += n;
            }
            carets_[it->carets[k]] = c;
        }
    }
    end_edit();
}

void TextEditor::duplicate_selection() {
    bool any = false;
    for (const Caret& c : carets_) any = any || c.has_selection();
    if (!any) {
        copy_lines(1);
        return;
    }
    if (read_only_) return;
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        if (!c.has_selection()) continue;
        const Coord e0 = c.end();
        const std::string t = range_text(c.start(), e0);
        const Coord e1 = apply_edit(e0, e0, t);
        c = Caret(e0, e1);
    }
    end_edit();
}

void TextEditor::move_lines(int direction) {
    if (read_only_) return;
    const std::vector<LineBlock> blocks = line_blocks(true);
    if (blocks.empty()) return;
    if (direction < 0 && blocks.front().first == 0) return;
    if (direction > 0 && blocks.back().last >= (int)lines_.size() - 1) return;

    begin_edit();
    for (const LineBlock& b : blocks) {
        const int a = direction < 0 ? b.first - 1 : b.first;
        const int z = direction < 0 ? b.last : b.last + 1;
        std::vector<std::string> chunk(lines_.begin() + a, lines_.begin() + z + 1);
        if (direction < 0) std::rotate(chunk.begin(), chunk.begin() + 1, chunk.end());
        else std::rotate(chunk.rbegin(), chunk.rbegin() + 1, chunk.rend());
        std::string replacement;
        for (size_t i = 0; i < chunk.size(); ++i) {
            if (i) replacement += "\n";
            replacement += chunk[i];
        }
        std::vector<Caret> saved;
        for (int idx : b.carets) saved.push_back(carets_[idx]);
        apply_edit(Coord(a, 0), Coord(z, (int)lines_[z].size()), replacement);
        for (size_t k = 0; k < saved.size(); ++k) {
            Caret c = saved[k];
            c.pos.line += direction;
            c.anchor.line += direction;
            carets_[b.carets[k]] = c;
        }
    }
    end_edit();
}

void TextEditor::delete_lines() {
    if (read_only_) return;
    const std::vector<LineBlock> blocks = line_blocks(false);
    begin_edit();
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        std::vector<int> cols;
        for (int idx : it->carets) cols.push_back(carets_[idx].pos.column);
        const int count = (int)lines_.size();
        Coord a, b;
        int target;
        if (it->last < count - 1) {
            a = Coord(it->first, 0);
            b = Coord(it->last + 1, 0);
            target = it->first;
        } else if (it->first > 0) {
            a = Coord(it->first - 1, (int)lines_[it->first - 1].size());
            b = Coord(it->last, (int)lines_[it->last].size());
            target = it->first - 1;
        } else {
            a = Coord(0, 0);
            b = Coord(it->last, (int)lines_[it->last].size());
            target = 0;
        }
        apply_edit(a, b, "");
        for (size_t k = 0; k < it->carets.size(); ++k)
            carets_[it->carets[k]] = Caret(sanitize(Coord(target, cols[k])));
    }
    end_edit();
}

void TextEditor::insert_line_below() {
    if (read_only_) return;
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        const int l = c.end().line;
        const Coord at(l, (int)lines_[l].size());
        const std::string indent = lines_[l].substr(0, (size_t)line_first_non_space(l).column);
        c = Caret(apply_edit(at, at, "\n" + indent));
    }
    end_edit();
}

void TextEditor::insert_line_above() {
    if (read_only_) return;
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        const int l = c.start().line;
        const std::string indent = lines_[l].substr(0, (size_t)line_first_non_space(l).column);
        apply_edit(Coord(l, 0), Coord(l, 0), indent + "\n");
        c = Caret(Coord(l, (int)indent.size()));
    }
    end_edit();
}

void TextEditor::indent_lines() {
    if (read_only_) return;
    const std::string unit = insert_spaces ? std::string((size_t)tab_size, ' ') : std::string("\t");
    const std::vector<LineBlock> blocks = line_blocks(false);
    // A selection that starts at column 0 keeps starting there, so whole-line
    // selections stay whole lines after the indent.
    std::vector<char> from_col0(carets_.size());
    for (size_t i = 0; i < carets_.size(); ++i)
        from_col0[i] = carets_[i].has_selection() && carets_[i].start().column == 0;
    begin_edit();
    for (const LineBlock& b : blocks)
        for (int l = b.first; l <= b.last; ++l)
            if (!lines_[l].empty() || b.first == b.last) apply_edit(Coord(l, 0), Coord(l, 0), unit);
    for (size_t i = 0; i < carets_.size(); ++i) {
        if (!from_col0[i]) continue;
        Caret& c = carets_[i];
        if (c.anchor < c.pos) c.anchor.column = 0;
        else c.pos.column = 0;
    }
    end_edit();
}

void TextEditor::outdent_lines() {
    if (read_only_) return;
    const std::vector<LineBlock> blocks = line_blocks(false);
    begin_edit();
    for (const LineBlock& b : blocks) {
        for (int l = b.first; l <= b.last; ++l) {
            const std::string& t = lines_[l];
            int k = 0;
            if (!t.empty() && t[0] == '\t') k = 1;
            else
                while (k < tab_size && k < (int)t.size() && t[k] == ' ') ++k;
            if (k > 0) apply_edit(Coord(l, 0), Coord(l, k), "");
        }
    }
    end_edit();
}

void TextEditor::toggle_line_comment() {
    if (read_only_ || language_ == Language::PlainText) return;
    const lang::Spec& spec = lang::spec(language_);
    const std::string marker = spec.line_comment ? spec.line_comment : "";
    if (marker.empty()) {
        // HTML, CSS and friends only have block comments: each block of lines
        // is wrapped (or unwrapped) as a whole.
        if (!spec.block_open || !*spec.block_open) return;
        const std::vector<LineBlock> blocks = line_blocks(false);
        carets_.clear();
        for (const LineBlock& b : blocks)
            carets_.push_back(Caret(line_first_non_space(b.first),
                                    Coord(b.last, (int)lines_[b.last].size())));
        primary_ = 0;
        toggle_block_comment();
        return;
    }

    const std::vector<LineBlock> blocks = line_blocks(false);
    bool all_commented = true;
    bool any_text = false;
    for (const LineBlock& b : blocks) {
        for (int l = b.first; l <= b.last; ++l) {
            const std::string& t = lines_[l];
            const size_t p = t.find_first_not_of(" \t");
            if (p == std::string::npos) continue;
            any_text = true;
            if (t.compare(p, marker.size(), marker) != 0) all_commented = false;
        }
    }
    if (!any_text) all_commented = false;

    std::vector<char> from_col0(carets_.size());
    for (size_t i = 0; i < carets_.size(); ++i)
        from_col0[i] = carets_[i].has_selection() && carets_[i].start().column == 0;

    begin_edit();
    for (const LineBlock& b : blocks) {
        // Markers line up at the block's shallowest indent, as in VS Code.
        size_t min_indent = std::string::npos;
        for (int l = b.first; l <= b.last; ++l) {
            const size_t p = lines_[l].find_first_not_of(" \t");
            if (p != std::string::npos) min_indent = std::min(min_indent, p);
        }
        for (int l = b.first; l <= b.last; ++l) {
            const std::string& t = lines_[l];
            const size_t p = t.find_first_not_of(" \t");
            if (all_commented) {
                if (p == std::string::npos || t.compare(p, marker.size(), marker) != 0) continue;
                size_t len = marker.size();
                if (p + len < t.size() && t[p + len] == ' ') ++len;
                apply_edit(Coord(l, (int)p), Coord(l, (int)(p + len)), "");
            } else {
                if (p == std::string::npos && b.first != b.last) continue; // blank line in a block
                const int at = p == std::string::npos ? (int)t.size() : (int)min_indent;
                apply_edit(Coord(l, at), Coord(l, at), marker + " ");
            }
        }
    }
    for (size_t i = 0; i < carets_.size(); ++i) {
        if (!from_col0[i]) continue;
        Caret& c = carets_[i];
        if (c.anchor < c.pos) c.anchor.column = 0;
        else c.pos.column = 0;
    }
    end_edit();
}

void TextEditor::toggle_block_comment() {
    if (read_only_ || language_ == Language::PlainText) return;
    const lang::Spec& spec = lang::spec(language_);
    const std::string open = spec.block_open ? spec.block_open : "";
    const std::string close = spec.block_close ? spec.block_close : "";
    if (open.empty() || close.empty()) {
        toggle_line_comment();
        return;
    }
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        const Coord s = c.start();
        const Coord e = c.end();
        if (!c.has_selection()) {
            apply_edit(s, s, open + "  " + close);
            c = Caret(Coord(s.line, s.column + (int)open.size() + 1));
            continue;
        }
        const std::string t = range_text(s, e);
        const size_t ts = t.find_first_not_of(" \t\n");
        const size_t te = t.find_last_not_of(" \t\n");
        const bool wrapped = ts != std::string::npos && te + 1 >= ts + open.size() + close.size() &&
                             t.compare(ts, open.size(), open) == 0 &&
                             t.compare(te + 1 - close.size(), close.size(), close) == 0;
        std::string replacement;
        if (wrapped) {
            std::string inner = t.substr(ts + open.size(), te + 1 - close.size() - (ts + open.size()));
            if (!inner.empty() && inner.front() == ' ') inner.erase(0, 1);
            if (!inner.empty() && inner.back() == ' ') inner.pop_back();
            replacement = t.substr(0, ts) + inner + t.substr(te + 1);
        } else {
            replacement = open + " " + t + " " + close;
        }
        const bool backward = c.pos < c.anchor;
        const Coord ne = apply_edit(s, e, replacement);
        c = backward ? Caret(ne, s) : Caret(s, ne);
    }
    end_edit();
}

void TextEditor::join_lines() {
    if (read_only_) return;
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        const int first = c.start().line;
        int last = c.end().line;
        if (last == first) last = first + 1;
        if (last >= (int)lines_.size()) continue;
        const bool had = c.has_selection();
        const Coord sel_start = c.start();
        Coord join_point;
        for (int l = last; l > first; --l) {
            const std::string& prev = lines_[l - 1];
            int pe = (int)prev.size();
            while (pe > 0 && is_space(prev[pe - 1])) --pe;
            const int ns = line_first_non_space(l).column;
            const bool empty_next = ns >= (int)lines_[l].size();
            const std::string sep = (pe == 0 || empty_next) ? "" : " ";
            apply_edit(Coord(l - 1, pe), Coord(l, ns), sep);
            if (l == first + 1) join_point = Coord(first, pe);
        }
        c = had ? Caret(sel_start, Coord(first, (int)lines_[first].size())) : Caret(join_point);
    }
    end_edit();
}

void TextEditor::transpose_chars() {
    if (read_only_) return;
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        if (c.has_selection()) continue;
        const Coord p = c.pos;
        const std::string& l = lines_[p.line];
        if (l.size() < 2 || p.column == 0) continue;
        Coord a, m, b;
        if (p.column >= (int)l.size()) {
            m = prev_char(p);
            a = prev_char(m);
            b = p;
        } else {
            a = prev_char(p);
            m = p;
            b = next_char(p);
        }
        if (a.line != p.line || b.line != p.line) continue;
        const std::string left = range_text(a, m);
        const std::string right = range_text(m, b);
        apply_edit(a, b, right + left);
        c = Caret(b);
    }
    end_edit();
}

void TextEditor::transform_case(CaseKind kind) {
    if (read_only_) return;
    begin_edit();
    for (size_t i = 0; i < carets_.size(); ++i) {
        Caret& c = carets_[i];
        const bool had = c.has_selection();
        Coord s = c.start();
        Coord e = c.end();
        // With nothing selected the word under the caret is converted.
        if (!had && !word_at(c.pos, s, e)) continue;
        const int offset = c.pos.column - s.column;
        const std::string t = range_text(s, e);
        const std::string converted = convert_case(t, kind);
        if (converted == t) continue;
        const bool backward = c.pos < c.anchor;
        const Coord ne = apply_edit(s, e, converted);
        if (had) c = backward ? Caret(ne, s) : Caret(s, ne);
        else c = Caret(Coord(s.line, s.column + ImMin(offset, (int)converted.size())));
    }
    end_edit();
}

void TextEditor::rewrite_line_ranges(const std::function<void(std::vector<std::string>&)>& fn) {
    if (read_only_) return;
    std::vector<std::pair<int, int>> ranges;
    bool multi_line = false;
    for (const Caret& c : carets_)
        if (c.has_selection() && c.start().line != c.end().line) multi_line = true;
    if (!multi_line && carets_.size() == 1) {
        ranges.push_back({0, (int)lines_.size() - 1});
    } else {
        for (const LineBlock& b : line_blocks(false))
            if (b.last > b.first) ranges.push_back({b.first, b.last});
    }
    if (ranges.empty()) return;

    const std::vector<Caret> saved = carets_;
    begin_edit();
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        std::vector<std::string> chunk(lines_.begin() + it->first, lines_.begin() + it->second + 1);
        const std::vector<std::string> before = chunk;
        fn(chunk);
        if (chunk == before) continue;
        std::string joined;
        for (size_t i = 0; i < chunk.size(); ++i) {
            if (i) joined += "\n";
            joined += chunk[i];
        }
        apply_edit(Coord(it->first, 0), Coord(it->second, (int)lines_[it->second].size()), joined);
    }
    // The lines were rewritten in place; the carets keep their coordinates.
    carets_ = saved;
    end_edit();
}

void TextEditor::sort_lines(bool descending) {
    rewrite_line_ranges([descending](std::vector<std::string>& v) {
        std::stable_sort(v.begin(), v.end(), [descending](const std::string& a, const std::string& b) {
            return descending ? icase_less(b, a) : icase_less(a, b);
        });
    });
}

void TextEditor::delete_duplicate_lines() {
    rewrite_line_ranges([](std::vector<std::string>& v) {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        for (const std::string& s : v)
            if (seen.insert(s).second) out.push_back(s);
        v.swap(out);
    });
}

void TextEditor::reverse_lines() {
    rewrite_line_ranges([](std::vector<std::string>& v) { std::reverse(v.begin(), v.end()); });
}

void TextEditor::trim_trailing_whitespace() {
    if (read_only_) return;
    begin_edit();
    for (int l = 0; l < (int)lines_.size(); ++l) {
        const std::string& t = lines_[l];
        int e = (int)t.size();
        while (e > 0 && is_space(t[e - 1])) --e;
        if (e < (int)t.size()) apply_edit(Coord(l, e), Coord(l, (int)t.size()), "");
    }
    end_edit();
}

void TextEditor::convert_indentation(bool to_tabs) {
    if (read_only_) return;
    begin_edit();
    for (int l = 0; l < (int)lines_.size(); ++l) {
        const std::string& t = lines_[l];
        int lead = 0;
        int width = 0;
        while (lead < (int)t.size() && is_space(t[lead])) {
            width = t[lead] == '\t' ? (width / tab_size + 1) * tab_size : width + 1;
            ++lead;
        }
        if (lead == 0) continue;
        std::string indent = to_tabs ? std::string((size_t)(width / tab_size), '\t') +
                                           std::string((size_t)(width % tab_size), ' ')
                                     : std::string((size_t)width, ' ');
        if (indent != t.substr(0, (size_t)lead)) apply_edit(Coord(l, 0), Coord(l, lead), indent);
    }
    end_edit();
}

// ----------------------------------------------------------- measurement

float TextEditor::run_width(const char* begin, const char* end) const {
    ImFont* font = ui::F.mono;
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
    const int n = (int)lines_.size();
    line_state_.resize(n);
    line_depth_.resize(n);
    from_line = ImClamp(from_line, 0, n - 1);
    unsigned char state = 0;
    int depth = 0;
    std::vector<Token> tokens;
    if (from_line > 0) {
        // recompute the state produced by the previous line
        state = tokenize(language_, lines_[from_line - 1], line_state_[from_line - 1], tokens);
        depth = bracket_delta(tokens, lines_[from_line - 1], line_depth_[from_line - 1]);
    }
    for (int i = from_line; i < n; ++i) {
        line_state_[i] = state;
        line_depth_[i] = depth;
        state = tokenize(language_, lines_[i], state, tokens);
        depth = bracket_delta(tokens, lines_[i], depth);
    }
}

// ---------------------------------------------------------------- search

void TextEditor::open_find() {
    find_open_ = true;
    find_focus_ = true;
    const Caret& p = carets_[primary_];
    std::string seed;
    if (p.has_selection() && p.start().line == p.end().line) seed = range_text(p.start(), p.end());
    else {
        Coord a, b;
        if (!p.has_selection() && word_at(p.pos, a, b)) seed = range_text(a, b);
    }
    if (!seed.empty() && seed.size() < sizeof(find_buffer_) - 1) {
        std::memcpy(find_buffer_, seed.c_str(), seed.size() + 1);
        matches_for_.clear();
    }
}

void TextEditor::set_find_text(const std::string& text) {
    std::snprintf(find_buffer_, sizeof(find_buffer_), "%s", text.c_str());
    matches_for_.clear();
}

void TextEditor::set_replace_text(const std::string& text) {
    std::snprintf(replace_buffer_, sizeof(replace_buffer_), "%s", text.c_str());
}

void TextEditor::open_replace() {
    open_find();
    replace_open_ = true;
}

void TextEditor::update_matches() {
    const std::string needle = find_buffer_;
    std::string key = needle;
    key += find_case_sensitive ? "\x01" : "\x02";
    key += find_whole_word ? "\x01" : "\x02";
    key += find_regex ? "\x01" : "\x02";
    if (key == matches_for_) return;
    matches_for_ = key;
    matches_.clear();
    current_match_ = -1;
    find_error_ = false;
    if (needle.empty()) return;

    const size_t cap = 100000;
    if (find_regex) {
        try {
            auto flags = std::regex::ECMAScript;
            if (!find_case_sensitive) flags |= std::regex::icase;
            const std::regex re(needle, flags);
            for (int i = 0; i < (int)lines_.size() && matches_.size() < cap; ++i) {
                const std::string& l = lines_[i];
                for (auto it = std::sregex_iterator(l.begin(), l.end(), re); it != std::sregex_iterator();
                     ++it) {
                    const int at = (int)it->position(0);
                    const int len = (int)it->length(0);
                    if (len == 0) continue; // an empty match selects nothing
                    if (find_whole_word) {
                        const bool left_ok = at == 0 || !is_ident(l[at - 1]);
                        const bool right_ok = at + len >= (int)l.size() || !is_ident(l[at + len]);
                        if (!left_ok || !right_ok) continue;
                    }
                    matches_.push_back({Coord(i, at), Coord(i, at + len)});
                }
            }
        } catch (const std::regex_error&) {
            find_error_ = true;
            matches_.clear();
        }
    } else {
        for (int i = 0; i < (int)lines_.size() && matches_.size() < cap; ++i) {
            int from = 0, at = 0;
            while (find_in_line(lines_[i], from, needle, find_whole_word, find_case_sensitive, at)) {
                matches_.push_back({Coord(i, at), Coord(i, at + (int)needle.size())});
                from = at + (int)needle.size();
            }
        }
    }
    // The match the caret is on or nearest after it is "current".
    const Coord ref = carets_[primary_].start();
    for (int i = 0; i < (int)matches_.size(); ++i) {
        if (!(matches_[i].a < ref)) { current_match_ = i; break; }
    }
    if (current_match_ < 0 && !matches_.empty()) current_match_ = 0;
}

void TextEditor::jump_to_match(int delta) {
    update_matches();
    if (matches_.empty()) return;
    const Caret& p = carets_[primary_];
    int idx = -1;
    if (delta > 0) {
        const Coord ref = p.end();
        for (int i = 0; i < (int)matches_.size(); ++i)
            if (!(matches_[i].a < ref) && !(matches_[i].a == p.start() && matches_[i].b == p.end())) {
                idx = i;
                break;
            }
        if (idx < 0) idx = 0;
    } else {
        const Coord ref = p.start();
        for (int i = (int)matches_.size() - 1; i >= 0; --i)
            if (matches_[i].a < ref) { idx = i; break; }
        if (idx < 0) idx = (int)matches_.size() - 1;
    }
    current_match_ = idx;
    select_range(matches_[idx].a, matches_[idx].b);
}

std::string TextEditor::replacement_for(const Match& m) const {
    std::string rep = replace_buffer_;
    if (!find_regex) return rep;
    // Regex mode understands \n and \t in the replacement, and $1-style groups.
    std::string unescaped;
    for (size_t i = 0; i < rep.size(); ++i) {
        if (rep[i] == '\\' && i + 1 < rep.size()) {
            const char n = rep[i + 1];
            if (n == 'n') { unescaped += '\n'; ++i; continue; }
            if (n == 't') { unescaped += '\t'; ++i; continue; }
            if (n == '\\') { unescaped += '\\'; ++i; continue; }
        }
        unescaped += rep[i];
    }
    try {
        auto flags = std::regex::ECMAScript;
        if (!find_case_sensitive) flags |= std::regex::icase;
        const std::regex re(find_buffer_, flags);
        const std::string& l = lines_[m.a.line];
        std::smatch sm;
        auto mflags = std::regex_constants::match_continuous;
        if (m.a.column > 0) mflags |= std::regex_constants::match_prev_avail;
        if (std::regex_search(l.begin() + m.a.column, l.end(), sm, re, mflags))
            return sm.format(unescaped);
    } catch (const std::regex_error&) {
    }
    return unescaped;
}

void TextEditor::replace_one() {
    if (read_only_) return;
    update_matches();
    if (matches_.empty()) return;
    const Caret& p = carets_[primary_];
    for (const Match& m : matches_) {
        if (m.a == p.start() && m.b == p.end()) {
            const std::string rep = replacement_for(m);
            begin_edit();
            const Coord e = apply_edit(m.a, m.b, rep);
            carets_.assign(1, Caret(e));
            primary_ = 0;
            end_edit();
            break;
        }
    }
    jump_to_match(1);
}

int TextEditor::replace_all() {
    if (read_only_) return 0;
    update_matches();
    if (matches_.empty()) return 0;
    const std::vector<Match> todo = matches_;
    std::vector<std::string> reps;
    reps.reserve(todo.size());
    for (const Match& m : todo) reps.push_back(replacement_for(m));
    begin_edit();
    for (int i = (int)todo.size() - 1; i >= 0; --i) apply_edit(todo[i].a, todo[i].b, reps[i]);
    const Coord keep = carets_[primary_].pos;
    carets_.assign(1, Caret(keep));
    primary_ = 0;
    end_edit();
    return (int)todo.size();
}

// ------------------------------------------------------------- input

void TextEditor::handle_keyboard() {
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;
    const bool alt = io.KeyAlt;

    // Exact-modifier matching, so Ctrl+Shift+K never also fires Ctrl+K.
    auto key = [&](ImGuiKey k, bool c, bool s, bool a, bool repeat = true) {
        return ctrl == c && shift == s && alt == a && ImGui::IsKeyPressed(k, repeat);
    };

    // --- history / clipboard ---
    if (key(ImGuiKey_Z, true, false, false)) { undo(); return; }
    if (key(ImGuiKey_Y, true, false, false) || key(ImGuiKey_Z, true, true, false)) { redo(); return; }
    if (key(ImGuiKey_C, true, false, false, false) || key(ImGuiKey_Insert, true, false, false, false)) {
        copy();
        return;
    }
    if (key(ImGuiKey_X, true, false, false, false) || key(ImGuiKey_Delete, false, true, false, false)) {
        cut();
        return;
    }
    if (key(ImGuiKey_V, true, false, false) || key(ImGuiKey_Insert, false, true, false)) { paste(); return; }
    if (key(ImGuiKey_A, true, false, false, false)) { select_all(); return; }

    // --- multiple carets ---
    if (key(ImGuiKey_D, true, false, false)) { add_next_occurrence(); return; }
    if (key(ImGuiKey_L, true, true, false, false) || key(ImGuiKey_F2, true, false, false, false)) {
        select_all_occurrences();
        return;
    }
    if (key(ImGuiKey_U, true, false, false)) { cursor_undo(); return; }
    if (key(ImGuiKey_I, false, true, true, false)) { carets_to_line_ends(); return; }
    if (key(ImGuiKey_UpArrow, true, false, true)) { add_caret_vertical(-1); return; }
    if (key(ImGuiKey_DownArrow, true, false, true)) { add_caret_vertical(1); return; }

    // --- selection ---
    if (key(ImGuiKey_RightArrow, false, true, true)) { expand_selection(); return; }
    if (key(ImGuiKey_LeftArrow, false, true, true)) { shrink_selection(); return; }
    if (key(ImGuiKey_Backslash, true, true, false, false)) { goto_matching_bracket(); return; }

    // --- find ---
    if (key(ImGuiKey_F, true, false, false, false)) { open_find(); return; }
    if (key(ImGuiKey_H, true, false, false, false)) { open_replace(); return; }
    if (key(ImGuiKey_F3, false, false, false)) { jump_to_match(1); return; }
    if (key(ImGuiKey_F3, false, true, false)) { jump_to_match(-1); return; }

    // --- view ---
    if (key(ImGuiKey_UpArrow, true, false, false)) { scroll_lines_request_ -= 1; return; }
    if (key(ImGuiKey_DownArrow, true, false, false)) { scroll_lines_request_ += 1; return; }

    // --- ghost text ---
    if (has_ghost()) {
        if (key(ImGuiKey_Tab, false, false, false, false)) { accept_ghost(); return; }
        if (key(ImGuiKey_Escape, false, false, false, false)) { clear_ghost(); return; }
    }

    if (key(ImGuiKey_Escape, false, false, false, false)) {
        if (find_open_) find_open_ = false;
        else collapse_carets();
        return;
    }
    if (key(ImGuiKey_Insert, false, false, false, false)) { toggle_overtype(); return; }

    // --- navigation ---
    if (!alt && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))) {
        const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
        move_carets(ctrl ? (left ? kWordLeft : kWordRight) : (left ? kLeft : kRight), shift);
        return;
    }
    if (!alt && !ctrl && (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))) {
        move_carets(ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ? kUp : kDown, shift);
        return;
    }
    if (!alt && ImGui::IsKeyPressed(ImGuiKey_Home, true)) { move_carets(ctrl ? kDocStart : kHome, shift); return; }
    if (!alt && ImGui::IsKeyPressed(ImGuiKey_End, true)) { move_carets(ctrl ? kDocEnd : kEnd, shift); return; }
    // Ctrl+PageUp/PageDown switch tabs; the owner handles those.
    if (!alt && !ctrl && (ImGui::IsKeyPressed(ImGuiKey_PageUp, true) || ImGui::IsKeyPressed(ImGuiKey_PageDown, true))) {
        move_carets(ImGui::IsKeyPressed(ImGuiKey_PageUp, true) ? kPageUp : kPageDown, shift);
        return;
    }

    if (read_only_) return;

    // --- line commands ---
    if (key(ImGuiKey_UpArrow, false, false, true)) { move_lines(-1); return; }
    if (key(ImGuiKey_DownArrow, false, false, true)) { move_lines(1); return; }
    if (key(ImGuiKey_UpArrow, false, true, true)) { copy_lines(-1); return; }
    if (key(ImGuiKey_DownArrow, false, true, true)) { copy_lines(1); return; }
    if (key(ImGuiKey_K, true, true, false)) { delete_lines(); return; }
    if (key(ImGuiKey_Slash, true, false, false, false)) { toggle_line_comment(); return; }
    if (key(ImGuiKey_A, false, true, true, false)) { toggle_block_comment(); return; }
    if (key(ImGuiKey_RightBracket, true, false, false)) { indent_lines(); return; }
    if (key(ImGuiKey_LeftBracket, true, false, false)) { outdent_lines(); return; }

    // --- editing keys ---
    if (key(ImGuiKey_Enter, true, false, false) || key(ImGuiKey_KeypadEnter, true, false, false)) {
        insert_line_below();
        return;
    }
    if (key(ImGuiKey_Enter, true, true, false) || key(ImGuiKey_KeypadEnter, true, true, false)) {
        insert_line_above();
        return;
    }
    if (!ctrl && !alt && (ImGui::IsKeyPressed(ImGuiKey_Enter, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true))) {
        insert_newline();
        return;
    }
    if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Tab, true)) { tab_key(shift); return; }
    if (key(ImGuiKey_Backspace, true, false, false)) { delete_word(-1); return; }
    if (key(ImGuiKey_Delete, true, false, false)) { delete_word(1); return; }
    if (key(ImGuiKey_Backspace, true, true, false)) { delete_to_line_edge(-1); return; }
    if (key(ImGuiKey_Delete, true, true, false)) { delete_to_line_edge(1); return; }
    if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) { backspace_or_delete(-1); return; }
    if (!ctrl && !alt && !shift && ImGui::IsKeyPressed(ImGuiKey_Delete, true)) { backspace_or_delete(1); return; }

    // --- text input ---
    if (!io.InputQueueCharacters.empty()) {
        // Ctrl+<key> is a shortcut, not text. AltGr (Ctrl+Alt) still types.
        const bool shortcut_modifier = ctrl && !alt;
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            unsigned int cp = (unsigned int)io.InputQueueCharacters[i];
            if (cp == 0 || cp == '\t' || cp == '\r' || cp == '\n' || cp == 127) continue;
            if (cp < 32) continue;
            if (shortcut_modifier) continue;
            type_char(cp);
        }
        io.InputQueueCharacters.resize(0);
    }
}

void TextEditor::handle_mouse(const ImVec2& text_origin, float gutter_right, float text_right) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 mouse = io.MousePos;

    auto coord_at = [&](ImVec2 m) {
        int line_index = (int)std::floor((m.y - text_origin.y) / line_height_);
        line_index = ImClamp(line_index, 0, (int)lines_.size() - 1);
        int col = x_to_column(lines_[line_index], m.x - text_origin.x);
        return Coord(line_index, col);
    };
    auto line_at = [&](ImVec2 m) {
        return ImClamp((int)std::floor((m.y - text_origin.y) / line_height_), 0, (int)lines_.size() - 1);
    };
    auto line_span = [&](int a, int b) {
        // Whole lines a..b, in the direction the drag went.
        const int lo = ImMin(a, b), hi = ImMax(a, b);
        const Coord s(lo, 0);
        const Coord e = hi + 1 < (int)lines_.size() ? Coord(hi + 1, 0) : Coord(hi, (int)lines_[hi].size());
        return b < a ? Caret(e, s) : Caret(s, e);
    };

    // Only the editor surface itself: not its scrollbars and not the find bar
    // that floats over it.
    const ImRect inner = window->InnerRect;
    const bool over_find = find_open_ && mouse.x >= find_bar_min_.x && mouse.x < find_bar_max_.x &&
                           mouse.y >= find_bar_min_.y && mouse.y < find_bar_max_.y;
    const bool in_area = ImGui::IsWindowHovered() && !over_find && inner.Contains(mouse);
    const bool in_minimap = in_area && show_minimap && mouse.x >= minimap_x_;
    const bool in_gutter = in_area && !in_minimap && mouse.x < gutter_right;
    const bool in_text = in_area && !in_minimap && !in_gutter && mouse.x < text_right;

    if ((in_text && drag_ == Drag::None) || drag_ == Drag::Text || drag_ == Drag::Word ||
        drag_ == Drag::Line || drag_ == Drag::Box)
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && in_area) {
        ImGui::SetWindowFocus();
        ghost_.clear();
        blink_reset_ = ImGui::GetTime();
        const int clicks = io.MouseClickedCount[ImGuiMouseButton_Left];

        if (in_minimap) {
            drag_ = Drag::Minimap;
        } else if (in_gutter) {
            // A click on a line number selects the line; dragging takes more.
            const int l = line_at(mouse);
            if (io.KeyShift) {
                box_anchor_line_ = carets_[primary_].anchor.line;
            } else {
                box_anchor_line_ = l;
            }
            set_single_caret(line_span(box_anchor_line_, l));
            drag_ = Drag::Gutter;
        } else if (in_text) {
            const Coord c = coord_at(mouse);
            if (io.KeyAlt && io.KeyShift) {
                // Column (box) selection from the primary caret to here.
                push_caret_history();
                const Caret& p = carets_[primary_];
                box_anchor_line_ = p.pos.line;
                box_anchor_x_ = column_to_x(lines_[p.pos.line], p.pos.column);
                box_select(box_anchor_line_, box_anchor_x_, c.line, mouse.x - text_origin.x);
                drag_ = Drag::Box;
            } else if (io.KeyAlt) {
                push_caret_history();
                if (clicks >= 2) {
                    // Alt+double-click adds a word selection.
                    Coord a, b;
                    if (word_at(c, a, b)) carets_[primary_] = Caret(a, b);
                    normalize_carets();
                    drag_ = Drag::None;
                } else {
                    // Alt+click toggles a caret: removes one that is there, adds one if not.
                    int existing = -1;
                    for (int i = 0; i < (int)carets_.size(); ++i)
                        if (carets_[i].pos == c && !carets_[i].has_selection()) existing = i;
                    if (existing >= 0 && carets_.size() > 1) {
                        carets_.erase(carets_.begin() + existing);
                        primary_ = (int)carets_.size() - 1;
                        normalize_carets();
                        drag_ = Drag::None;
                    } else {
                        carets_.push_back(Caret(c));
                        primary_ = (int)carets_.size() - 1;
                        normalize_carets();
                        drag_ = Drag::Text;
                    }
                }
            } else if (io.KeyShift) {
                Caret p = carets_[primary_];
                p.pos = c;
                p.preferred_x = -1.0f;
                set_single_caret(p);
                drag_ = Drag::Text;
            } else if (clicks == 2) {
                Coord a, b;
                if (!word_at(c, a, b)) { a = c; b = next_char(c); if (b.line != c.line) b = c; }
                set_single_caret(Caret(a, b));
                drag_anchor_ = a;
                drag_anchor_end_ = b;
                drag_ = Drag::Word;
            } else if (clicks >= 3) {
                set_single_caret(line_span(c.line, c.line));
                box_anchor_line_ = c.line;
                drag_ = Drag::Line;
            } else {
                set_single_caret(Caret(c));
                drag_ = Drag::Text;
            }
            occurrence_whole_word_ = false;
        }
    }

    if (drag_ != Drag::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = Drag::None;
    if (drag_ == Drag::None || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

    if (drag_ == Drag::Minimap) {
        const float h = window->InnerRect.GetHeight();
        const float lh = ImMax(1.0f, std::floor(ui::px(2.0f)));
        const float total = (float)lines_.size() * lh;
        const float area = ImMin(total, h);
        const float slider = ImMin(area, (h / line_height_) * lh);
        const float span = area - slider;
        const float ratio = span > 0.0f ? ImSaturate((mouse.y - window->InnerRect.Min.y - slider * 0.5f) / span) : 0.0f;
        ImGui::SetScrollY(ratio * ImGui::GetScrollMaxY());
        return;
    }

    const Coord c = coord_at(mouse);
    switch (drag_) {
        case Drag::Text:
            carets_[primary_].pos = c;
            normalize_carets();
            break;
        case Drag::Word: {
            Coord a, b;
            if (c < drag_anchor_) {
                if (!word_at(c, a, b)) a = c;
                carets_.assign(1, Caret(drag_anchor_end_, a));
            } else {
                if (!word_at(c, a, b)) b = c;
                carets_.assign(1, Caret(drag_anchor_, ImMax(b, drag_anchor_end_)));
            }
            primary_ = 0;
            break;
        }
        case Drag::Line:
        case Drag::Gutter:
            carets_.assign(1, line_span(box_anchor_line_, c.line));
            primary_ = 0;
            break;
        case Drag::Box:
            box_select(box_anchor_line_, box_anchor_x_, c.line, mouse.x - text_origin.x);
            break;
        default:
            break;
    }
    blink_reset_ = ImGui::GetTime();

    // auto-scroll while dragging past the edges
    const ImVec2 win_min = inner.Min;
    const ImVec2 win_max = inner.Max;
    const float margin = line_height_;
    if (mouse.y < win_min.y + margin)
        ImGui::SetScrollY(ImGui::GetScrollY() - line_height_ * 0.6f);
    else if (mouse.y > win_max.y - margin)
        ImGui::SetScrollY(ImGui::GetScrollY() + line_height_ * 0.6f);
    if (mouse.x < gutter_right + margin && ImGui::GetScrollX() > 0.0f)
        ImGui::SetScrollX(ImGui::GetScrollX() - char_width_ * 2.0f);
    else if (mouse.x > text_right - margin)
        ImGui::SetScrollX(ImGui::GetScrollX() + char_width_ * 2.0f);
}

void TextEditor::scroll_cursor_into_view() {
    const Coord cur = cursor();
    const float scroll_y = ImGui::GetScrollY();
    const float height = view_height_;
    const float top = (float)cur.line * line_height_;
    const float bottom = top + line_height_;
    const float margin = line_height_ * 2.0f;

    if (center_cursor_) {
        ImGui::SetScrollY(ImMax(0.0f, top - height * 0.5f + line_height_));
    } else if (top < scroll_y + margin) {
        ImGui::SetScrollY(ImMax(0.0f, top - margin));
    } else if (bottom > scroll_y + height - margin) {
        ImGui::SetScrollY(bottom - height + margin);
    }

    const float x = column_to_x(lines_[cur.line], cur.column);
    const float scroll_x = ImGui::GetScrollX();
    const float width = ImGui::GetWindowWidth() - minimap_w_;
    if (x < scroll_x + char_width_ * 4.0f)
        ImGui::SetScrollX(ImMax(0.0f, x - char_width_ * 8.0f));
    else if (x > scroll_x + width - char_width_ * 12.0f)
        ImGui::SetScrollX(x - width + char_width_ * 14.0f);
}

// --------------------------------------------------------------- render

void TextEditor::draw_find_bar(const ImVec2& area_min, float area_width) {
    ImGuiIO& io = ImGui::GetIO();
    const float bar_w = ui::px(470.0f);
    const float pad = ui::px(8.0f);
    ImVec2 pos(area_min.x + area_width - bar_w - pad * 2.0f - minimap_w_, area_min.y + pad);
    pos.x = ImMax(pos.x, area_min.x + pad);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(bar_w, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(ui::C.bg_surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(8), ui::px(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(4), ui::px(6)));
    ImGui::BeginChild("##find", ImVec2(bar_w, 0),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    find_bar_min_ = ImGui::GetWindowPos();
    find_bar_max_ = ImVec2(find_bar_min_.x + ImGui::GetWindowWidth(), find_bar_min_.y + ImGui::GetWindowHeight());

    ImGui::PushFont(ui::F.ui);
    const bool bar_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // A small square toggle in the house style: filled with ink when on.
    auto toggle = [&](const char* id, const char* label, bool* value, const char* tip) {
        ImFont* f = ui::F.mono_small ? ui::F.mono_small : ui::F.ui_small;
        const float h = ImGui::GetFrameHeight();
        const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label);
        const float w = ImMax(h, ts.x + ui::px(10));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        if (clicked) {
            *value = !*value;
            matches_for_.clear();
        }
        ImDrawList* d = ImGui::GetWindowDrawList();
        if (*value) d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::C.ink);
        else if (hovered) d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::C.bg_surface_hi);
        d->AddRect(p, ImVec2(p.x + w, p.y + h), *value ? ui::C.ink : ui::C.border);
        d->AddText(f, f->FontSize, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                   *value ? ui::C.on_ink : ui::C.text_dim, label);
        if (hovered) ui::tooltip(tip);
    };

    // --- row 1: find ---
    const float right_x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (ui::icon_button("##togglereplace", replace_open_ ? ui::Icon::ChevronDown : ui::Icon::ChevronRight,
                        "Toggle replace (Ctrl+H)"))
        replace_open_ = !replace_open_;
    ImGui::SameLine();
    if (find_focus_) {
        ImGui::SetKeyboardFocusHere();
        find_focus_ = false;
    }
    const float tools_w = ui::px(232.0f);
    ImGui::SetNextItemWidth(ImMax(ui::px(80), ImGui::GetContentRegionAvail().x - tools_w));
    if (find_error_) ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(ui::C.error));
    if (ImGui::InputTextWithHint("##findtext", "Find", find_buffer_, sizeof(find_buffer_),
                                 ImGuiInputTextFlags_AutoSelectAll))
        matches_for_.clear();
    if (find_error_) ImGui::PopStyleColor();
    const bool find_active = ImGui::IsItemFocused();
    const bool enter = find_active && (ImGui::IsKeyPressed(ImGuiKey_Enter, true) ||
                                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true));

    ImGui::SameLine();
    toggle("##case", "Aa", &find_case_sensitive, "Match case (Alt+C)");
    ImGui::SameLine();
    toggle("##word", "ab", &find_whole_word, "Match whole word (Alt+W)");
    ImGui::SameLine();
    toggle("##regex", ".*", &find_regex, "Use regular expression (Alt+R)");

    ImGui::SameLine();
    ImGui::PushFont(ui::F.ui_small);
    char counter[48];
    if (find_error_)
        std::snprintf(counter, sizeof(counter), "bad regex");
    else if (matches_.empty())
        std::snprintf(counter, sizeof(counter), "%s", find_buffer_[0] ? "no results" : "");
    else
        std::snprintf(counter, sizeof(counter), "%d of %d", current_match_ + 1, (int)matches_.size());
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(find_error_ || (matches_.empty() && find_buffer_[0])
                                                          ? ui::C.error
                                                          : ui::C.text_dim),
                       "%s", counter);
    ImGui::PopFont();

    const float btn = ImGui::GetFrameHeight();
    ImGui::SameLine(ImMax(ImGui::GetCursorPosX(), right_x - btn * 3.0f - ui::px(4)));
    if (ui::icon_button("##prev", ui::Icon::ChevronDown, "Previous match (Shift+Enter)")) jump_to_match(-1);
    ImGui::SameLine(0, ui::px(2));
    if (ui::icon_button("##next", ui::Icon::ChevronRight, "Next match (Enter)")) jump_to_match(1);
    ImGui::SameLine(0, ui::px(2));
    bool close = ui::icon_button("##closefind", ui::Icon::Close, "Close (Esc)");

    // --- row 2: replace ---
    bool replace_enter = false;
    if (replace_open_) {
        ImGui::Dummy(ImVec2(btn, btn));
        ImGui::SameLine();
        if (replace_focus_) {
            ImGui::SetKeyboardFocusHere();
            replace_focus_ = false;
        }
        ImGui::SetNextItemWidth(ImMax(ui::px(80), ImGui::GetContentRegionAvail().x - tools_w));
        ImGui::InputTextWithHint("##replacetext", find_regex ? "Replace ($1, \\n)" : "Replace",
                                 replace_buffer_, sizeof(replace_buffer_));
        replace_enter = ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter, true) ||
                                                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true));
        ImGui::SameLine();
        if (ui::ghost_button("Replace", ImVec2(0, 0), !read_only_)) replace_one();
        ImGui::SameLine();
        if (ui::ghost_button("All", ImVec2(0, 0), !read_only_)) replace_all();
    }

    if (bar_focused && io.KeyAlt && !io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) { find_case_sensitive = !find_case_sensitive; matches_for_.clear(); }
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) { find_whole_word = !find_whole_word; matches_for_.clear(); }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) { find_regex = !find_regex; matches_for_.clear(); }
    }
    if (enter) jump_to_match(io.KeyShift ? -1 : 1);
    if (replace_enter) {
        if (io.KeyCtrl && io.KeyAlt) replace_all();
        else replace_one();
    }
    if (bar_focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    if (close) {
        find_open_ = false;
        focus_request_ = true;
    }

    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void TextEditor::draw_caret(ImDrawList* dl, const ImVec2& origin, const Caret& c, float alpha,
                            float height_scale, bool primary) {
    const std::string& text = lines_[c.pos.line];
    float x = origin.x + column_to_x(text, c.pos.column);
    float y = origin.y + c.pos.line * line_height_;
    bool settled = true;
    if (primary && smooth_caret && carets_.size() == 1) {
        const ImVec2 target(x - origin.x, y - origin.y);
        if (caret_anim_.x < 0.0f) caret_anim_ = target;
        const float k = 1.0f - std::exp(-ImGui::GetIO().DeltaTime * 22.0f);
        caret_anim_.x += (target.x - caret_anim_.x) * k;
        caret_anim_.y += (target.y - caret_anim_.y) * k;
        if (std::fabs(target.x - caret_anim_.x) < 0.5f && std::fabs(target.y - caret_anim_.y) < 0.5f)
            caret_anim_ = target;
        settled = caret_anim_.x == target.x && caret_anim_.y == target.y;
        x = origin.x + caret_anim_.x;
        y = origin.y + caret_anim_.y;
    } else if (primary) {
        caret_anim_ = ImVec2(x - origin.x, y - origin.y);
    }
    x = std::floor(x);

    // The cell under the caret: the character there, or a space at line end.
    const Coord next = next_char(c.pos);
    float cell = space_width_;
    if (next.line == c.pos.line && next.column > c.pos.column) {
        cell = column_to_x(text, next.column) - column_to_x(text, c.pos.column);
        if (cell <= 0.0f) cell = space_width_;
    }

    const ImU32 col = with_alpha(ui::C.cursor, alpha);
    const CursorStyle style = overtype_ ? overtype_cursor_style : cursor_style;
    const float line_w = std::floor(ui::px((float)cursor_width) + 0.5f);
    draw_caret_shape(dl, style, ImVec2(x, y), cell, line_height_, line_w, col, height_scale);

    // The character under a block caret is knocked out in paper white, the way
    // every filled thing in this interface carries its type.
    if (style == CursorStyle::Block && height_scale >= 1.0f && settled && alpha > 0.5f &&
        next.line == c.pos.line && next.column > c.pos.column && !is_space(text[c.pos.column])) {
        const float text_y = y + std::floor((line_height_ - ImGui::GetFontSize()) * 0.5f);
        dl->AddText(ui::F.mono, ImGui::GetFontSize(), ImVec2(x, text_y), ui::C.on_ink,
                    text.data() + c.pos.column, text.data() + next.column);
    }
}

float TextEditor::minimap_scroll(float height) const {
    const float lh = ImMax(1.0f, std::floor(ui::px(2.0f)));
    const float total = (float)lines_.size() * lh;
    if (total <= height) return 0.0f;
    const float max_scroll = ImGui::GetScrollMaxY();
    const float ratio = max_scroll > 0.0f ? ImSaturate(ImGui::GetScrollY() / max_scroll) : 0.0f;
    return ratio * (total - height);
}

// A zoomed-out picture of the whole file at the right edge, the way VS Code
// draws it: one thin bar per token, the visible region as a translucent
// slider, find matches as ochre ticks. Click or drag it to scroll.
void TextEditor::draw_minimap(ImDrawList* dl, const ImVec2& win_pos, float x, float width, float height) {
    const float lh = ImMax(1.0f, std::floor(ui::px(2.0f)));
    const float cw = ImMax(0.5f, ui::px(1.0f));
    const float pad = ui::px(6.0f);
    const float mscroll = minimap_scroll(height);

    dl->AddRectFilled(ImVec2(x, win_pos.y), ImVec2(x + width, win_pos.y + height), ui::C.code_bg);
    dl->AddLine(ImVec2(x + 0.5f, win_pos.y), ImVec2(x + 0.5f, win_pos.y + height), ui::C.border, 1.0f);
    dl->PushClipRect(ImVec2(x + 1.0f, win_pos.y), ImVec2(x + width, win_pos.y + height), true);

    const int first = ImMax(0, (int)std::floor(mscroll / lh));
    const int last = ImMin((int)lines_.size() - 1, first + (int)std::ceil(height / lh) + 1);
    std::vector<Token> tokens;
    const int max_cols = (int)((width - pad * 1.5f) / cw);
    for (int i = first; i <= last; ++i) {
        const std::string& t = lines_[i];
        if (t.empty()) continue;
        const float y = win_pos.y + i * lh - mscroll;
        tokenize(language_, t, i < (int)line_state_.size() ? line_state_[i] : 0, tokens);
        int col = 0;
        int k = 0;
        for (const Token& tk : tokens) {
            // advance to the token start, expanding tabs
            for (; k < tk.start; ++k) col = t[k] == '\t' ? (col / tab_size + 1) * tab_size : col + 1;
            int start_col = col;
            for (; k < tk.end; ++k) col = t[k] == '\t' ? (col / tab_size + 1) * tab_size : col + 1;
            if (start_col >= max_cols) break;
            // skip pure whitespace runs
            bool blank = true;
            for (int q = tk.start; q < tk.end; ++q)
                if (!is_space(t[q])) { blank = false; break; }
            if (blank) continue;
            const int end_col = ImMin(col, max_cols);
            dl->AddRectFilled(ImVec2(x + pad + start_col * cw, y),
                              ImVec2(x + pad + end_col * cw, y + ImMax(1.0f, lh - (lh >= 2.0f ? 0.5f : 0.0f))),
                              with_alpha(color_for(tk.kind), 0.55f));
        }
    }

    // find matches and caret lines as marks along the right edge
    if (find_open_)
        for (const Match& m : matches_) {
            if (m.a.line < first || m.a.line > last) continue;
            const float y = win_pos.y + m.a.line * lh - mscroll;
            dl->AddRectFilled(ImVec2(x + width - ui::px(5), y), ImVec2(x + width - 1.0f, y + ImMax(2.0f, lh)),
                              ui::C.warning);
        }
    for (const Caret& c : carets_) {
        if (c.pos.line < first || c.pos.line > last) continue;
        const float y = win_pos.y + c.pos.line * lh - mscroll;
        dl->AddRectFilled(ImVec2(x + 1.0f, y), ImVec2(x + width, y + ImMax(1.0f, lh)),
                          with_alpha(ui::C.ink, 0.12f));
    }

    // the visible region
    const float total = (float)lines_.size() * lh;
    const float area = ImMin(total, height);
    const float slider = ImMin(area, (height / line_height_) * lh);
    const float max_scroll = ImGui::GetScrollMaxY();
    const float ratio = max_scroll > 0.0f ? ImSaturate(ImGui::GetScrollY() / max_scroll) : 0.0f;
    const float top = win_pos.y + ratio * (area - slider);
    const bool hot = drag_ == Drag::Minimap ||
                     ImGui::IsMouseHoveringRect(ImVec2(x, win_pos.y), ImVec2(x + width, win_pos.y + height));
    dl->AddRectFilled(ImVec2(x + 1.0f, top), ImVec2(x + width, top + slider),
                      with_alpha(ui::C.ink, hot ? 0.10f : 0.05f));
    dl->PopClipRect();
}

bool TextEditor::render(const char* id, const ImVec2& size) {
    const unsigned version_before = version_;
    ImGuiIO& io = ImGui::GetIO();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(ui::C.code_bg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushFont(ui::F.mono);

    // Ctrl+wheel zooms instead of scrolling.
    ImGui::BeginChild(id, size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav |
                          (io.KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : 0));
    if (focus_request_) {
        ImGui::SetWindowFocus();
        focus_request_ = false;
    }

    ImFont* font = ui::F.mono;
    space_width_ = font->GetCharAdvance((ImWchar)' ');
    char_width_ = space_width_;
    line_height_ = std::floor(ImGui::GetFontSize() * 1.55f);
    if (font->FontSize != metrics_font_size_) {
        // The font was rebuilt (zoom or DPI change): every width is stale.
        metrics_font_size_ = font->FontSize;
        refresh_metrics();
    }

    focused_ = ImGui::IsWindowFocused();
    // With keyboard navigation on, ImGui reads Escape as "leave this child
    // window" and takes the focus away before the editor sees the key. Owning
    // the key while focused keeps it for the editor: collapse the carets,
    // dismiss a suggestion, close the find bar.
    if (focused_) ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##editor_escape"));

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const float win_h = ImGui::GetWindowHeight();
    const float inner_right = window->InnerRect.Max.x;
    view_height_ = window->InnerRect.GetHeight();

    if (io.KeyCtrl && io.MouseWheel != 0.0f && ImGui::IsWindowHovered())
        zoom_request_ = io.MouseWheel > 0.0f ? 1 : -1;

    // gutter
    char last_no[16];
    std::snprintf(last_no, sizeof(last_no), "%d", (int)lines_.size());
    const float digits_w = ImGui::CalcTextSize(last_no).x;
    const float gutter_w = show_line_numbers ? std::floor(digits_w + ui::px(26.0f)) : ui::px(10.0f);
    const float pad_x = ui::px(10.0f);
    const float pad_y = ui::px(8.0f);

    minimap_w_ = show_minimap ? std::floor(ui::px(88.0f)) : 0.0f;
    minimap_x_ = inner_right - minimap_w_;

    const float content_w = gutter_w + pad_x + longest_line_px_ + ui::px(80.0f) + minimap_w_;
    const float content_h = lines_.size() * line_height_ + pad_y * 2.0f + win_h * 0.4f;
    ImGui::Dummy(ImVec2(content_w, content_h));

    const float scroll_x = ImGui::GetScrollX();
    const float scroll_y = ImGui::GetScrollY();
    const ImVec2 text_origin(win_pos.x + gutter_w + pad_x - scroll_x, win_pos.y + pad_y - scroll_y);

    if (focused_ && !ImGui::IsAnyItemActive()) handle_keyboard();
    handle_mouse(text_origin, win_pos.x + gutter_w, minimap_x_);

    if (scroll_lines_request_ != 0) {
        ImGui::SetScrollY(ImClamp(scroll_y + scroll_lines_request_ * line_height_, 0.0f, ImGui::GetScrollMaxY()));
        scroll_lines_request_ = 0;
    }
    if (scroll_to_cursor_ || center_cursor_) {
        scroll_cursor_into_view();
        scroll_to_cursor_ = false;
        center_cursor_ = false;
    }
    if (find_open_) update_matches();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Clamped both ways: a view can be scrolled past the end of a buffer that
    // just got shorter, for a frame, before ImGui clamps the scroll.
    const int line_max = (int)lines_.size() - 1;
    const int first_line = ImClamp((int)std::floor((scroll_y - pad_y) / line_height_), 0, line_max);
    const int last_line = ImClamp(first_line + (int)std::ceil(win_h / line_height_) + 1, first_line, line_max);
    const Caret& primary = carets_[primary_];

    // Which visible lines carry a caret: 1 = a caret, 2 = a caret with no
    // selection (those get the current-line band).
    std::vector<char> caret_line((size_t)(last_line - first_line + 1), 0);
    for (const Caret& c : carets_) {
        if (c.pos.line < first_line || c.pos.line > last_line) continue;
        char& m = caret_line[(size_t)(c.pos.line - first_line)];
        m = (char)ImMax((int)m, c.has_selection() ? 1 : 2);
    }

    // ---- text area (clipped so long lines do not spill into the gutter) ----
    const ImVec2 clip_min(win_pos.x + gutter_w, win_pos.y);
    const ImVec2 clip_max(minimap_x_, win_pos.y + win_h);
    dl->PushClipRect(clip_min, clip_max, true);

    const float text_dy = std::floor((line_height_ - ImGui::GetFontSize()) * 0.5f);

    // current-line bands
    if (highlight_current_line && focused_) {
        for (int i = first_line; i <= last_line; ++i) {
            if (caret_line[(size_t)(i - first_line)] != 2) continue;
            const float y = text_origin.y + i * line_height_;
            dl->AddRectFilled(ImVec2(clip_min.x, y), ImVec2(clip_max.x, y + line_height_), ui::C.current_line);
        }
    }

    // selections
    for (const Caret& c : carets_) {
        if (!c.has_selection()) continue;
        const Coord a = c.start();
        const Coord b = c.end();
        if (b.line < first_line || a.line > last_line) continue;
        for (int i = ImMax(a.line, first_line); i <= ImMin(b.line, last_line); ++i) {
            const std::string& text = lines_[i];
            const float y = text_origin.y + i * line_height_;
            const int col_a = i == a.line ? a.column : 0;
            const int col_b = i == b.line ? b.column : (int)text.size();
            const float x1 = text_origin.x + column_to_x(text, col_a);
            float x2 = text_origin.x + column_to_x(text, col_b);
            if (i < b.line) x2 += space_width_ * 0.6f; // show the wrapped newline
            dl->AddRectFilled(ImVec2(x1, y), ImVec2(ImMax(x2, x1 + 2.0f), y + line_height_),
                              ui::C.selection, ui::px(2.0f));
        }
    }

    // search matches (sorted by line, so start at the first visible one)
    if (find_open_ && !matches_.empty()) {
        auto it = std::lower_bound(matches_.begin(), matches_.end(), first_line,
                                   [](const Match& m, int line) { return m.a.line < line; });
        for (; it != matches_.end() && it->a.line <= last_line; ++it) {
            const std::string& text = lines_[it->a.line];
            const float y = text_origin.y + it->a.line * line_height_;
            const float x1 = text_origin.x + column_to_x(text, it->a.column);
            const float x2 = text_origin.x + column_to_x(text, it->b.column);
            const bool current = (int)(it - matches_.begin()) == current_match_;
            dl->AddRectFilled(ImVec2(x1, y), ImVec2(x2, y + line_height_),
                              current ? ui::C.accent_dim : ui::C.match_highlight, ui::px(2.0f));
            if (current)
                dl->AddRect(ImVec2(x1, y), ImVec2(x2, y + line_height_), ui::C.accent, ui::px(2.0f), 0, 1.0f);
        }
    }

    // other occurrences of the word under the caret, or of the selected text
    if (highlight_occurrences && carets_.size() == 1) {
        std::string needle;
        bool whole = false;
        Coord skip_a(-1, -1);
        if (primary.has_selection()) {
            if (primary.start().line == primary.end().line && count_chars(primary.start(), primary.end()) <= 200) {
                needle = range_text(primary.start(), primary.end());
                skip_a = primary.start();
                if (needle.find_first_not_of(" \t") == std::string::npos) needle.clear();
            }
        } else {
            Coord a, b;
            if (word_at(primary.pos, a, b)) {
                needle = range_text(a, b);
                whole = true;
            }
        }
        if (!needle.empty()) {
            for (int i = first_line; i <= last_line; ++i) {
                const std::string& text = lines_[i];
                int from = 0, at = 0;
                while (find_in_line(text, from, needle, whole, true, at)) {
                    from = at + (int)needle.size();
                    if (Coord(i, at) == skip_a) continue;
                    const float y = text_origin.y + i * line_height_;
                    const float x1 = text_origin.x + column_to_x(text, at);
                    const float x2 = text_origin.x + column_to_x(text, at + (int)needle.size());
                    dl->AddRectFilled(ImVec2(x1, y), ImVec2(x2, y + line_height_), ui::C.accent_soft, ui::px(2.0f));
                }
            }
        }
    }

    // matching bracket pair at the caret
    if (match_brackets && focused_) {
        if (bracket_cache_version_ != version_ || bracket_cache_caret_ != primary.pos) {
            bracket_cache_version_ = version_;
            bracket_cache_caret_ = primary.pos;
            bracket_cache_found_ = false;
            Coord m;
            const Coord p = primary.pos;
            // A shorter reach than the commands get: this runs whenever the
            // caret moves, and an unmatched bracket scans to the limit.
            if (match_bracket(p, m, 3000)) {
                bracket_cache_found_ = true;
                bracket_cache_a_ = p;
                bracket_cache_b_ = m;
            } else if (p.column > 0 && match_bracket(Coord(p.line, p.column - 1), m, 3000)) {
                bracket_cache_found_ = true;
                bracket_cache_a_ = Coord(p.line, p.column - 1);
                bracket_cache_b_ = m;
            }
        }
        if (bracket_cache_found_) {
            for (const Coord& b : {bracket_cache_a_, bracket_cache_b_}) {
                if (b.line < first_line || b.line > last_line) continue;
                const std::string& text = lines_[b.line];
                const float y = text_origin.y + b.line * line_height_;
                const float x1 = std::floor(text_origin.x + column_to_x(text, b.column));
                const float x2 = std::floor(text_origin.x + column_to_x(text, b.column + 1));
                dl->AddRectFilled(ImVec2(x1, y + 1.0f), ImVec2(x2, y + line_height_ - 1.0f), ui::C.accent_soft);
                dl->AddRect(ImVec2(x1 + 0.5f, y + 1.5f), ImVec2(x2 - 0.5f, y + line_height_ - 1.5f),
                            ui::C.border_strong, 0.0f, 0, 1.0f);
            }
        }
    }

    // rulers
    for (int col : rulers) {
        if (col <= 0) continue;
        const float rx = std::floor(text_origin.x + col * space_width_) + 0.5f;
        if (rx < clip_min.x || rx > clip_max.x) continue;
        dl->AddLine(ImVec2(rx, win_pos.y), ImVec2(rx, win_pos.y + win_h), ui::C.border, 1.0f);
    }

    // Three bracket-pair colours drawn from the syntax palette, so the extra
    // hues are ones the page already uses.
    const ImU32 pair_colors[3] = {ui::C.syn_keyword, ui::C.syn_type, ui::C.syn_number};

    // Selected column ranges per visible line, for "render whitespace in the
    // selection": built once here rather than by asking every caret per space.
    std::vector<std::vector<std::pair<int, int>>> selected_cols;
    if (render_whitespace == Whitespace::Selection) {
        selected_cols.resize((size_t)(last_line - first_line + 1));
        for (const Caret& c : carets_) {
            if (!c.has_selection()) continue;
            const Coord a = c.start();
            const Coord b = c.end();
            for (int i = ImMax(a.line, first_line); i <= ImMin(b.line, last_line); ++i)
                selected_cols[(size_t)(i - first_line)].push_back(
                    {i == a.line ? a.column : 0, i == b.line ? b.column : INT_MAX});
        }
    }

    std::vector<Token> tokens;
    for (int i = first_line; i <= last_line; ++i) {
        const std::string& text = lines_[i];
        const float y = text_origin.y + i * line_height_;

        // indent guides
        if (show_indent_guides) {
            int spaces = 0;
            for (size_t k = 0; k < text.size() && is_space(text[k]); ++k)
                spaces += text[k] == '\t' ? tab_size : 1;
            if (text.empty()) {
                // continue guides through blank lines using the next non-empty line
                for (int k = i + 1; k < (int)lines_.size() && k < i + 200; ++k) {
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

        if (text.empty()) continue;

        // whitespace markers
        if (render_whitespace != Whitespace::None) {
            int last_text = (int)text.size() - 1;
            while (last_text >= 0 && is_space(text[last_text])) --last_text;
            int first_text = 0;
            while (first_text < (int)text.size() && is_space(text[first_text])) ++first_text;
            float wx = 0.0f;
            for (int k = 0; k < (int)text.size();) {
                unsigned int cp = 0;
                const int adv = ImTextCharFromUtf8(&cp, text.data() + k, text.data() + text.size());
                if (adv <= 0) break;
                float step;
                if (cp == '\t') {
                    const float tab = space_width_ * (float)tab_size;
                    step = std::floor(wx / tab + 1.0f) * tab - wx;
                } else {
                    step = font->GetCharAdvance(cp <= 0xFFFF ? (ImWchar)cp : (ImWchar)'?');
                }
                if (cp == ' ' || cp == '\t') {
                    bool show = false;
                    switch (render_whitespace) {
                        case Whitespace::All: show = true; break;
                        case Whitespace::Trailing: show = k > last_text; break;
                        case Whitespace::Boundary:
                            show = cp == '\t' || k < first_text || k > last_text ||
                                   (k + 1 < (int)text.size() && is_space(text[k + 1])) ||
                                   (k > 0 && is_space(text[k - 1]));
                            break;
                        case Whitespace::Selection:
                            for (const auto& r : selected_cols[(size_t)(i - first_line)])
                                if (r.first <= k && k < r.second) { show = true; break; }
                            break;
                        default: break;
                    }
                    if (show) {
                        const float px0 = text_origin.x + wx;
                        if (cp == ' ') {
                            const float r = ImMax(1.0f, ui::px(1.1f));
                            dl->AddCircleFilled(ImVec2(px0 + step * 0.5f, y + line_height_ * 0.5f), r,
                                                ui::C.border_strong, 6);
                        } else {
                            const float my = std::floor(y + line_height_ * 0.5f) + 0.5f;
                            const float x0 = px0 + ui::px(2.0f);
                            const float x1 = px0 + ImMin(step, space_width_ * 2.0f) - ui::px(2.0f);
                            dl->AddLine(ImVec2(x0, my), ImVec2(x1, my), ui::C.border_strong, 1.0f);
                            dl->AddLine(ImVec2(x1 - ui::px(3), my - ui::px(3)), ImVec2(x1, my), ui::C.border_strong, 1.0f);
                            dl->AddLine(ImVec2(x1 - ui::px(3), my + ui::px(3)), ImVec2(x1, my), ui::C.border_strong, 1.0f);
                        }
                    }
                }
                wx += step;
                k += adv;
            }
        }

        // syntax-highlighted text
        unsigned char state = i < (int)line_state_.size() ? line_state_[i] : 0;
        tokenize(language_, text, state, tokens);
        int depth = i < (int)line_depth_.size() ? line_depth_[i] : 0;
        float x = text_origin.x;
        const float text_y = y + text_dy;
        for (const Token& tk : tokens) {
            const char* begin = text.data() + tk.start;
            const char* end = text.data() + tk.end;
            const ImU32 color = color_for(tk.kind);
            const bool pairs = bracket_pair_colors && tk.kind == Tok::Punct;
            // split on tabs (so alignment matches the measurement helpers) and,
            // when pairs are coloured, around each bracket
            const char* run = begin;
            for (const char* p = begin; p <= end; ++p) {
                const bool bracket = pairs && p < end && (is_open_bracket(*p) || is_close_bracket(*p));
                if (p == end || *p == '\t' || bracket) {
                    if (p > run) {
                        if (x < clip_max.x && x + (p - run) * char_width_ * 2.0f > clip_min.x)
                            dl->AddText(font, ImGui::GetFontSize(), ImVec2(x, text_y), color, run, p);
                        x += run_width(run, p);
                    }
                    if (p == end) break;
                    if (*p == '\t') {
                        float tab = space_width_ * (float)tab_size;
                        float local = x - text_origin.x;
                        x = text_origin.x + std::floor(local / tab + 1.0f) * tab;
                    } else {
                        if (is_close_bracket(*p) && depth > 0) --depth;
                        const ImU32 bc = pair_colors[depth % 3];
                        if (is_open_bracket(*p)) ++depth;
                        if (x < clip_max.x && x + char_width_ > clip_min.x)
                            dl->AddText(font, ImGui::GetFontSize(), ImVec2(x, text_y), bc, p, p + 1);
                        x += run_width(p, p + 1);
                    }
                    run = p + 1;
                }
            }
        }
    }

    // ghost (AI suggestion) text
    if (!ghost_.empty()) {
        float gx = text_origin.x + column_to_x(lines_[primary.pos.line], primary.pos.column);
        float gy = text_origin.y + primary.pos.line * line_height_;
        int line_offset = 0;
        size_t start = 0;
        while (start <= ghost_.size()) {
            size_t nl = ghost_.find('\n', start);
            std::string part = ghost_.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            float px_ = line_offset == 0 ? gx : text_origin.x;
            dl->AddText(font, ImGui::GetFontSize(),
                        ImVec2(px_, gy + line_offset * line_height_ + text_dy), ui::C.ghost_text, part.c_str());
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

    // carets
    if (focused_) {
        float alpha = 1.0f;
        float height_scale = 1.0f;
        caret_blink_state(cursor_blink, ImGui::GetTime() - blink_reset_, alpha, height_scale);
        if (alpha > 0.01f) {
            for (int k = 0; k < (int)carets_.size(); ++k) {
                const Caret& c = carets_[k];
                if (c.pos.line < first_line - 1 || c.pos.line > last_line + 1) continue;
                draw_caret(dl, text_origin, c, alpha, height_scale, k == primary_);
            }
        }
    } else {
        caret_anim_ = ImVec2(-1.0f, -1.0f);
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
            const float y = text_origin.y + i * line_height_ + text_dy;
            const float w = ImGui::CalcTextSize(buf).x;
            const bool current = caret_line[(size_t)(i - first_line)] != 0;
            dl->AddText(font, ImGui::GetFontSize(),
                        ImVec2(win_pos.x + gutter_w - ui::px(14.0f) - w, y),
                        current ? ui::C.line_number_cur : ui::C.line_number, buf);
        }
        dl->AddLine(ImVec2(win_pos.x + gutter_w - 0.5f, win_pos.y),
                    ImVec2(win_pos.x + gutter_w - 0.5f, win_pos.y + win_h), ui::C.border, 1.0f);
        dl->PopClipRect();
    }

    if (show_minimap) draw_minimap(dl, win_pos, minimap_x_, minimap_w_, view_height_);

    if (find_open_) draw_find_bar(win_pos, inner_right - win_pos.x + minimap_w_);
    else find_bar_min_ = find_bar_max_ = ImVec2(0, 0);

    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    return version_ != version_before;
}

} // namespace ed
