#include "agenttext.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include "lang.h"

namespace agent {
namespace {

// Local copy of the whitespace trim used while tidying stripped prose.
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

// Recognises a JSON object that describes a call to one of our tools. Accepts
// the shapes local models actually produce: {"name":…,"arguments":{…}},
// {"function":{…}}, {"tool":…,"parameters":{…}} and flat argument objects.
bool tool_call_from_json(const js::Value& value, const std::vector<std::string>& known,
                         ollama::ToolCall& out) {
    if (!value.is_object()) return false;
    const js::Value& obj = value["function"].is_object() ? value["function"] : value;

    std::string name = obj["name"].as_string();
    if (name.empty()) name = obj["tool"].as_string();
    if (name.empty()) name = obj["tool_name"].as_string();
    if (name.empty()) return false;
    // Only known tool names are accepted, so ordinary JSON samples in a reply
    // are never mistaken for calls.
    if (std::find(known.begin(), known.end(), name) == known.end()) return false;

    const js::Value* args = &obj["arguments"];
    if (!args->is_object()) args = &obj["parameters"];
    if (!args->is_object()) args = &obj["args"];
    // Flat form: the arguments sit directly on the call object. "path" covers
    // the file tools, "query" covers search_files.
    if (!args->is_object() && (obj["path"].is_string() || obj["query"].is_string()))
        args = &obj;

    out.name = name;
    out.arguments = args->is_object() ? args->dump() : "{}";
    return true;
}

// Tries to read a tool call (or an array of them) out of one JSON snippet.
int calls_from_snippet(const std::string& snippet, const std::vector<std::string>& known,
                       std::vector<ollama::ToolCall>& out) {
    js::Value parsed = js::parse(snippet);
    int found = 0;
    if (parsed.is_array()) {
        for (size_t k = 0; k < parsed.size(); ++k) {
            ollama::ToolCall call;
            if (tool_call_from_json(parsed[k], known, call)) {
                out.push_back(std::move(call));
                ++found;
            }
        }
        return found;
    }
    ollama::ToolCall call;
    if (tool_call_from_json(parsed, known, call)) {
        out.push_back(std::move(call));
        ++found;
    }
    // A wrapper such as {"tool_calls":[…]}
    const js::Value& nested = parsed["tool_calls"];
    for (size_t k = 0; k < nested.size(); ++k) {
        ollama::ToolCall inner;
        if (tool_call_from_json(nested[k], known, inner)) {
            out.push_back(std::move(inner));
            ++found;
        }
    }
    return found;
}

// Pulls tool calls out of the reply text and strips them from what the user
// sees. Handles both fenced blocks and JSON written inline in prose, since
// models differ in how they present a call they cannot make natively.
int harvest_text_tool_calls(std::string& content, const std::vector<std::string>& known,
                            std::vector<ollama::ToolCall>& out) {
    int found = 0;

    // Pass 1: whole fenced blocks.
    {
        std::string kept;
        size_t i = 0;
        while (i < content.size()) {
            size_t fence = content.find("```", i);
            if (fence == std::string::npos) { kept += content.substr(i); break; }
            size_t info_end = content.find('\n', fence);
            if (info_end == std::string::npos) { kept += content.substr(i); break; }
            size_t close = content.find("```", info_end);
            if (close == std::string::npos) { kept += content.substr(i); break; }

            std::vector<ollama::ToolCall> calls;
            int n = calls_from_snippet(content.substr(info_end + 1, close - info_end - 1),
                                       known, calls);
            kept += content.substr(i, fence - i);
            if (n == 0) {
                kept += content.substr(fence, close + 3 - fence);
            } else {
                for (ollama::ToolCall& call : calls) out.push_back(std::move(call));
                found += n;
            }
            i = close + 3;
        }
        content = kept;
    }

    // Pass 2: balanced top-level JSON objects sitting in the prose. One scan
    // finds every candidate; anything that is not a known call is left alone.
    {
        std::string kept;
        size_t i = 0;
        while (i < content.size()) {
            if (content[i] != '{') {
                kept += content[i];
                ++i;
                continue;
            }
            // Walk to the matching brace, skipping over string literals.
            int depth = 0;
            bool in_string = false;
            size_t j = i;
            for (; j < content.size(); ++j) {
                const char c = content[j];
                if (in_string) {
                    if (c == '\\') ++j;
                    else if (c == '"') in_string = false;
                    continue;
                }
                if (c == '"') in_string = true;
                else if (c == '{') ++depth;
                else if (c == '}' && --depth == 0) break;
            }
            if (j >= content.size()) {
                // The object never closed. Models routinely drop the last brace
                // or two off an otherwise correct call, so try to repair it
                // before giving up — but only accept the result if it parses
                // into a real call, so ordinary truncated prose is left alone.
                std::string repaired = content.substr(i);
                if (in_string) repaired += '"';
                if (depth > 0) repaired.append((size_t)depth, '}');

                std::vector<ollama::ToolCall> calls;
                if (calls_from_snippet(repaired, known, calls) > 0) {
                    for (ollama::ToolCall& call : calls) out.push_back(std::move(call));
                    found += (int)calls.size();
                } else {
                    kept += content.substr(i);
                }
                break;
            }

            std::vector<ollama::ToolCall> calls;
            int n = calls_from_snippet(content.substr(i, j - i + 1), known, calls);
            if (n == 0) {
                kept += content.substr(i, j - i + 1);
            } else {
                for (ollama::ToolCall& call : calls) out.push_back(std::move(call));
                found += n;
            }
            i = j + 1;
        }
        content = kept;
    }

    if (found > 0) {
        // Tidy up separators left behind by the removed JSON.
        std::string cleaned;
        for (size_t i = 0; i < content.size(); ++i) {
            const char c = content[i];
            if ((c == ',' || c == ')' || c == '(' || c == ']' || c == '[') &&
                trim(cleaned).empty())
                continue;
            cleaned += c;
        }
        content = trim(cleaned);
    }
    return found;
}

// -------------------------------------------------- code blocks as files

namespace {

// Markdown decoration that can sit around a file name written in prose:
// "**index.html**", "`src/app.js`:", "### styles.css".
std::string undecorate(const std::string& s) {
    static const char* kLead = "#*`_~-<>|[({\"' \t";
    static const char* kTail = "*`_~:;,.!?>)]}\"' \t";
    const size_t a = s.find_first_not_of(kLead);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(kTail);
    if (b == std::string::npos || b < a) return std::string();
    return s.substr(a, b - a + 1);
}

void split_tokens(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    std::string current;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) out.push_back(current);
}

// True when `s` reads as the name of a file the IDE could write. The language
// registry is the test: it knows the extensions and the extensionless names
// (Makefile, Dockerfile, .env), and it rejects everything else - which is what
// keeps "version 18.2" and "e.g." out.
bool file_token(const std::string& s, lang::Id& id) {
    if (s.empty() || s.size() > 200) return false;
    if (s.find_first_of(" \t<>|\"*?=") != std::string::npos) return false;
    if (s.find("://") != std::string::npos) return false;
    if (s.back() == '/' || s.back() == '\\') return false;

    std::string leaf = s;
    const size_t slash = leaf.find_last_of("/\\");
    if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
    if (leaf.empty()) return false;

    id = lang::from_path(leaf);
    return id != lang::Id::PlainText;
}

// Languages close enough that a name in one and a fence tag in the other are
// still the same file: .js in a ```jsx block, .scss in a ```css block.
int language_family(lang::Id id) {
    switch (id) {
        case lang::Id::JavaScript:
        case lang::Id::TypeScript:
        case lang::Id::Jsx:
        case lang::Id::Tsx:
            return 1;
        case lang::Id::Css:
        case lang::Id::Scss:
        case lang::Id::Less:
            return 2;
        case lang::Id::Html:
        case lang::Id::Xml:
        case lang::Id::Svelte:
        case lang::Id::Vue:
        case lang::Id::Astro:
        case lang::Id::Handlebars:
            return 3;
        case lang::Id::C:
        case lang::Id::Cpp:
        case lang::Id::ObjC:
            return 4;
        case lang::Id::Shell:
        case lang::Id::Batch:
        case lang::Id::PowerShell:
            return 5;
        default:
            return 0;
    }
}

// A name found in prose is only believed when it does not contradict the block
// it labels. Unknown or absent tags agree with everything.
bool languages_agree(lang::Id a, lang::Id b) {
    if (a == b) return true;
    if (a == lang::Id::PlainText || b == lang::Id::PlainText) return true;
    if (a == lang::Id::Text || b == lang::Id::Text) return true;
    const int fa = language_family(a);
    return fa != 0 && fa == language_family(b);
}

// The file name a comment on the block's first line gives it, if it gives one:
// "// index.html", "# app.py", "<!-- about.html -->", "/* styles.css */".
std::string file_name_in_first_comment(const std::string& body, const std::string& fence_tag) {
    const size_t nl = body.find('\n');
    std::string first = trim(body.substr(0, nl == std::string::npos ? body.size() : nl));
    if (first.empty()) return std::string();

    static const char* kOpeners[] = {"<!--", "#!", "//", "/*", "--", ";;", "rem ", "REM ",
                                     "#", ";", "%", "'"};
    const char* opener = nullptr;
    for (const char* candidate : kOpeners) {
        const size_t n = std::strlen(candidate);
        if (first.size() > n && first.compare(0, n, candidate) == 0) {
            opener = candidate;
            first = first.substr(n);
            break;
        }
    }
    if (!opener) return std::string();

    // Whatever closes the comment again.
    for (const char* closer : {"-->", "*/"}) {
        const size_t n = std::strlen(closer);
        if (first.size() >= n && first.compare(first.size() - n, n, closer) == 0)
            first = first.substr(0, first.size() - n);
    }

    // Exactly one word, or this is a sentence rather than a file name.
    std::vector<std::string> tokens;
    split_tokens(trim(first), tokens);
    if (tokens.size() != 1) return std::string();

    const std::string candidate = undecorate(tokens[0]);
    lang::Id id = lang::Id::PlainText;
    if (!file_token(candidate, id)) return std::string();
    if (!languages_agree(id, lang::from_tag(fence_tag))) return std::string();
    return candidate;
}

// A fence inside a numbered list is indented, and so is everything in it.
// Written out as-is that indentation becomes part of the file, so it comes off
// again - but only where every line really carries it.
std::string unindent(const std::string& body, size_t width) {
    if (width == 0 || body.empty()) return body;

    size_t line_start = 0;
    while (line_start < body.size()) {
        const size_t nl = body.find('\n', line_start);
        const size_t line_end = (nl == std::string::npos) ? body.size() : nl;
        if (line_end > line_start) { // blank lines carry nothing and prove nothing
            size_t k = line_start;
            while (k < line_end && k - line_start < width &&
                   (body[k] == ' ' || body[k] == '\t'))
                ++k;
            if (k - line_start < width && k < line_end) return body;
        }
        if (nl == std::string::npos) break;
        line_start = nl + 1;
    }

    std::string out;
    line_start = 0;
    while (line_start < body.size()) {
        const size_t nl = body.find('\n', line_start);
        const size_t line_end = (nl == std::string::npos) ? body.size() : nl;
        size_t k = line_start;
        while (k < line_end && k - line_start < width && (body[k] == ' ' || body[k] == '\t')) ++k;
        out += body.substr(k, line_end - k);
        if (nl == std::string::npos) break;
        out += '\n';
        line_start = nl + 1;
    }
    return out;
}

} // namespace

void parse_fence_info(const std::string& info, std::string& lang_tag, std::string& path) {
    lang_tag.clear();
    path.clear();

    std::vector<std::string> tokens;
    split_tokens(info, tokens);

    auto unquote = [](std::string s) {
        if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
            s = s.substr(1, s.size() - 2);
        return s;
    };
    auto looks_like_path = [](const std::string& s) {
        if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return true;
        const size_t dot = s.find_last_of('.');
        return dot != std::string::npos && dot > 0 && dot + 1 < s.size();
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        if (token.rfind("path=", 0) == 0) { path = unquote(token.substr(5)); continue; }
        if (token.rfind("file=", 0) == 0) { path = unquote(token.substr(5)); continue; }
        if (token.rfind("filename=", 0) == 0) { path = unquote(token.substr(9)); continue; }
        if (token.rfind("title=", 0) == 0) { path = unquote(token.substr(6)); continue; }
        if (i == 0) {
            // "```src/main.cpp" - the tag itself is the path
            if (looks_like_path(token)) path = unquote(token);
            else lang_tag = token;
            continue;
        }
        if (path.empty() && looks_like_path(token)) path = unquote(token);
    }

    // A path that is not a file name the IDE recognises is not a path.
    if (!path.empty()) {
        lang::Id id = lang::Id::PlainText;
        if (!file_token(path, id)) path.clear();
    }
    if (lang_tag.empty() && !path.empty()) lang_tag = lang::tag(lang::from_path(path));
}

std::string file_name_in_line(const std::string& line, const std::string& fence_tag) {
    if (line.empty() || line.size() > 300) return std::string();

    std::vector<std::string> tokens;
    split_tokens(line, tokens);
    const lang::Id fence = lang::from_tag(fence_tag);

    for (const std::string& token : tokens) {
        const std::string candidate = undecorate(token);
        lang::Id id = lang::Id::PlainText;
        if (!file_token(candidate, id)) continue;
        if (!languages_agree(id, fence)) continue;
        return candidate;
    }
    return std::string();
}

int collect_file_blocks(const std::string& text, std::vector<FileBlock>& out) {
    const size_t before = out.size();

    // The last non-blank line seen outside a block: models put the file name
    // right above it ("**index.html**", "### src/app.js", "Now styles.css:").
    std::string label;

    size_t i = 0;
    while (i < text.size()) {
        const size_t nl = text.find('\n', i);
        const size_t line_end = (nl == std::string::npos) ? text.size() : nl;
        const std::string line = trim(text.substr(i, line_end - i));

        if (line.rfind("```", 0) != 0) {
            if (!line.empty()) label = line;
            i = (nl == std::string::npos) ? text.size() : nl + 1;
            continue;
        }

        FileBlock block;
        block.begin = i;
        block.info_end = line_end;

        // Walk to the closing fence, which has to sit at the start of a line.
        const size_t body_start = (nl == std::string::npos) ? text.size() : nl + 1;
        size_t body_end = text.size();
        size_t block_end = text.size();
        size_t j = body_start;
        while (j < text.size()) {
            const size_t jnl = text.find('\n', j);
            const size_t jend = (jnl == std::string::npos) ? text.size() : jnl;
            if (trim(text.substr(j, jend - j)).rfind("```", 0) == 0) {
                body_end = j;
                block_end = (jnl == std::string::npos) ? text.size() : jnl + 1;
                break;
            }
            j = (jnl == std::string::npos) ? text.size() : jnl + 1;
        }
        size_t indent = 0;
        while (i + indent < line_end && (text[i + indent] == ' ' || text[i + indent] == '\t'))
            ++indent;
        block.content = unindent(text.substr(body_start, body_end - body_start), indent);
        block.end = block_end;
        parse_fence_info(trim(line.substr(3)), block.lang, block.path);
        if (!block.path.empty()) {
            block.named = true;
            block.fenced_path = true;
        }

        if (block.path.empty()) {
            const std::string labelled = file_name_in_line(label, block.lang);
            if (!labelled.empty()) {
                block.path = labelled;
                block.named = true;
            }
        }
        if (block.path.empty()) {
            const std::string commented = file_name_in_first_comment(block.content, block.lang);
            if (!commented.empty()) {
                block.path = commented;
                block.named = true;
            }
        }

        out.push_back(std::move(block));
        label.clear();
        i = block_end;
    }

    // One last inference, and the only one made from the language alone: a
    // single unnamed page in a reply is the site's entry point. It is what a
    // model writes when it is asked for a website and names nothing, and
    // without it the one file that matters has nowhere to go.
    int unnamed_pages = 0;
    size_t page_at = 0;
    bool index_taken = false;
    for (size_t k = before; k < out.size(); ++k) {
        FileBlock& block = out[k];
        if (!block.path.empty()) {
            std::string leaf = block.path;
            const size_t slash = leaf.find_last_of("/\\");
            if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
            for (char& c : leaf) c = (char)std::tolower((unsigned char)c);
            if (leaf == "index.html" || leaf == "index.htm") index_taken = true;
            continue;
        }
        const bool html_tag = lang::from_tag(block.lang) == lang::Id::Html;
        std::string head = block.content.substr(0, 200);
        for (char& c : head) c = (char)std::tolower((unsigned char)c);
        const bool html_body = block.lang.empty() && (head.find("<!doctype html") != std::string::npos ||
                                                      head.find("<html") != std::string::npos);
        if (!html_tag && !html_body) continue;
        ++unnamed_pages;
        page_at = k;
    }
    if (unnamed_pages == 1 && !index_taken) out[page_at].path = "index.html";

    return (int)(out.size() - before);
}

std::string splice(const std::string& source, std::vector<Splice> edits) {
    std::sort(edits.begin(), edits.end(),
              [](const Splice& a, const Splice& b) { return a.begin < b.begin; });

    std::string out;
    size_t at = 0;
    for (const Splice& edit : edits) {
        if (edit.end < edit.begin || edit.end > source.size() || edit.begin < at) continue;
        size_t begin = edit.begin;
        size_t end = edit.end;
        if (edit.text.empty()) {
            // Take the whole lines the range sits on, so lifting a block out
            // does not leave its indentation and its newline behind.
            while (begin > at && (source[begin - 1] == ' ' || source[begin - 1] == '\t')) --begin;
            while (end < source.size() &&
                   (source[end] == ' ' || source[end] == '\t' || source[end] == '\r'))
                ++end;
            if (end < source.size() && source[end] == '\n') ++end;
        }
        out += source.substr(at, begin - at);
        out += edit.text;
        at = end;
        // Two paragraphs that were separated by a block are now separated by
        // its blank lines; keep one.
        while (edit.text.empty() && out.size() >= 2 &&
               out.compare(out.size() - 2, 2, "\n\n") == 0 && at < source.size() &&
               source[at] == '\n')
            ++at;
    }
    out += source.substr(at);
    return out;
}

// ------------------------------------------------------- tolerant matching

namespace {

struct Line {
    size_t begin;   // offset of the first character
    size_t end;     // offset one past the last character, excluding the newline
    std::string trimmed; // content with trailing whitespace removed
    bool blank;
};

// Splits text into lines, remembering where each one sits in the original so a
// match can be mapped back to byte offsets.
std::vector<Line> split_lines(const std::string& text) {
    std::vector<Line> lines;
    size_t i = 0;
    while (i <= text.size()) {
        size_t nl = text.find('\n', i);
        const size_t stop = (nl == std::string::npos) ? text.size() : nl;
        Line line;
        line.begin = i;
        line.end = stop;
        size_t last = stop;
        while (last > i && (text[last - 1] == ' ' || text[last - 1] == '\t' ||
                            text[last - 1] == '\r'))
            --last;
        line.trimmed = text.substr(i, last - i);
        line.blank = line.trimmed.empty();
        lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    return lines;
}

} // namespace

Match find_editable_region(const std::string& haystack, const std::string& needle,
                           size_t& begin, size_t& end) {
    if (needle.empty()) return Match::None;

    // Exact match first: if the model reproduced the snippet byte for byte,
    // honour that and nothing else.
    const size_t at = haystack.find(needle);
    if (at != std::string::npos) {
        if (haystack.find(needle, at + needle.size()) != std::string::npos)
            return Match::Ambiguous;
        begin = at;
        end = at + needle.size();
        return Match::Unique;
    }

    // Tolerant pass: compare only the non-blank lines, with trailing whitespace
    // ignored. Blank lines in either side are skipped, so a snippet that differs
    // only in how many empty lines separate two functions still matches.
    const std::vector<Line> hay = split_lines(haystack);
    const std::vector<Line> need = split_lines(needle);

    std::vector<size_t> wanted; // indices into `need` of the meaningful lines
    for (size_t i = 0; i < need.size(); ++i)
        if (!need[i].blank) wanted.push_back(i);
    if (wanted.empty()) return Match::None;

    size_t found_begin = 0, found_end = 0;
    int hits = 0;

    for (size_t start = 0; start < hay.size(); ++start) {
        if (hay[start].blank || hay[start].trimmed != need[wanted[0]].trimmed) continue;

        size_t h = start;
        size_t w = 0;
        size_t last_matched = start;
        bool ok = true;

        while (w < wanted.size()) {
            while (h < hay.size() && hay[h].blank) ++h; // skip blank file lines
            if (h >= hay.size() || hay[h].trimmed != need[wanted[w]].trimmed) {
                ok = false;
                break;
            }
            last_matched = h;
            ++h;
            ++w;
        }

        if (!ok) continue;
        ++hits;
        if (hits > 1) return Match::Ambiguous;
        found_begin = hay[start].begin;
        found_end = hay[last_matched].end;
    }

    if (hits != 1) return Match::None;
    begin = found_begin;
    end = found_end;
    return Match::Unique;
}

} // namespace agent
