// Recovering tool calls that a model wrote as text instead of emitting through
// the API's structured field.
//
// This is not an edge case. Plenty of local models — qwen2.5-coder among them —
// know the tool schema and produce a correct JSON call, but skip the wrapper
// tokens their chat template needs, so the server hands it back as ordinary
// message content with no tool_calls attached. Without this recovery step the
// IDE would show the user a blob of JSON and never touch a file.
#pragma once

#include <string>
#include <vector>

#include "json.h"
#include "ollama.h"

namespace agent {

// Reads a tool call out of one JSON value. Accepts the shapes models actually
// produce: {"name":…,"arguments":{…}}, {"function":{…}}, {"tool":…,
// "parameters":{…}} and flat objects whose arguments sit at the top level.
// Only names in `known` are accepted, so a JSON sample quoted in an answer is
// never mistaken for a call.
bool tool_call_from_json(const js::Value& value, const std::vector<std::string>& known,
                         ollama::ToolCall& out);

// Tries to read a call, or an array of them, out of one JSON snippet.
int calls_from_snippet(const std::string& snippet, const std::vector<std::string>& known,
                       std::vector<ollama::ToolCall>& out);

// Pulls every recognisable call out of `content`, appends them to `out`, and
// strips them from `content` so the user sees prose rather than JSON. Handles
// fenced blocks and JSON sitting inline in the prose. Returns how many it found.
int harvest_text_tool_calls(std::string& content, const std::vector<std::string>& known,
                            std::vector<ollama::ToolCall>& out);

// -------------------------------------------------- code blocks as files
//
// The other half of the same problem. A model that will not emit a call often
// still writes the file: it prints it as a fenced code block and leaves the
// user to copy it out. A block that says which file it is is a create_file in
// everything but form, so these functions find that name and the IDE performs
// the write instead of handing over something to copy.

struct FileBlock {
    std::string path;    // target file, relative to the project root; empty when unknown
    std::string content; // the block body, fences excluded
    std::string lang;    // the fence's info tag, e.g. "html"
    // True when the model named the file itself - in the fence info, in a label
    // above the block, or in a comment on its first line - rather than the name
    // being worked out from the other blocks around it.
    bool named = false;
    // True when the path came from the fence's own info line, which is the one
    // place the chat's markdown renderer also looks.
    bool fenced_path = false;
    size_t begin = 0;     // offset of the opening fence in the reply
    size_t end = 0;       // offset one past the closing fence's newline
    size_t info_end = 0;  // offset of the end of the info line, where a path= fits
};

// Splits a fence info line - "cpp path=src/foo.cpp", or a bare path - into a
// language tag and an optional target path.
void parse_fence_info(const std::string& info, std::string& lang, std::string& path);

// Reads a file name out of a line of prose: "**index.html**", "### src/app.js",
// "File: styles.css". `fence_tag` is the language of the block the line belongs
// to, and a name whose language contradicts it is rejected - which is what
// stops "run Node.js first" above a shell block from naming a file. Empty when
// the line does not name one.
std::string file_name_in_line(const std::string& line, const std::string& fence_tag);

// Collects every fenced code block in `text`, in order, and works out which
// file each one is meant to become. Blocks whose target cannot be established
// are returned too, with an empty path.
int collect_file_blocks(const std::string& text, std::vector<FileBlock>& out);

// A replacement of one byte range of the reply text.
struct Splice {
    size_t begin = 0;
    size_t end = 0;   // one past the last byte replaced
    std::string text; // what goes in its place; empty removes the range
};

// Applies `edits` to `source`. Overlapping edits are dropped rather than
// merged. Where a range is removed the seam is tidied up, so lifting a block
// out of a reply does not leave a stack of blank lines behind.
std::string splice(const std::string& source, std::vector<Splice> edits);

enum class Match {
    None,       // the snippet is not in the file
    Unique,     // found exactly once; begin/end delimit it
    Ambiguous,  // found in more than one place, so the edit is not well defined
};

// Locates `needle` inside `haystack` for edit_file.
//
// An exact hit wins. Failing that, the search retries ignoring trailing
// whitespace on each line and any difference in the number of blank lines
// between them, because that is what models get wrong — they reproduce the
// code correctly but not the exact spacing around it. The match must still be
// unique, so a tolerant hit is never a guess about which place was meant.
Match find_editable_region(const std::string& haystack, const std::string& needle,
                           size_t& begin, size_t& end);

} // namespace agent
