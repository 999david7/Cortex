// Language registry: detection, highlighting rules, display names and icons.
//
// Everything the IDE knows about a programming language lives in one table in
// lang.cpp. Adding a language means adding one row there — the tokenizer, the
// file-tree icons, the chat code blocks and the status bar all read from it.
#pragma once

#include <string>

#include "imgui.h"

namespace lang {

enum class Id {
    PlainText = 0,
    // C family
    C, Cpp, ObjC, CSharp, Java, Kotlin, Scala, Groovy, Dart, Swift, Go, Rust,
    Zig, D, Vala, Haxe, Solidity, Verilog, Vhdl, Glsl, Cuda,
    // scripting
    Python, Ruby, Perl, Php, Lua, JavaScript, TypeScript, Shell, PowerShell,
    Batch, R, Julia, Tcl, Nim, Crystal, Vim, AppleScript,
    // functional
    Haskell, OCaml, FSharp, Elixir, Erlang, Clojure, Lisp, Scheme, Elm, Prolog,
    // web / markup
    Html, Xml, Css, Scss, Less, Svelte, Vue, Astro, Jsx, Tsx, Handlebars,
    // data / config
    Json, Yaml, Toml, Ini, Csv, Sql, GraphQl, Proto, Properties, Env,
    // build / infra
    Dockerfile, Makefile, Cmake, Meson, Gradle, Nix, Terraform, Bazel,
    // docs & misc
    Markdown, Rst, Latex, AsciiDoc, Diff, Log, Assembly, Fortran, Matlab,
    Pascal, Ada, Cobol, Text,
    Count
};

// Highlighting rules for one language. Consumed by the tokenizer.
struct Spec {
    const char* const* keywords;
    const char* const* types;
    const char* line_comment;
    const char* line_comment_alt;
    const char* block_open;
    const char* block_close;
    bool preproc_hash;      // C/C++ style #include
    bool char_literals;     // 'x' is a character literal, not a string
    bool triple_quotes;     // Python """ ... """
    bool backtick_strings;  // JS `...`, Go `...`
    bool case_insensitive;  // SQL, Pascal, Fortran
    bool nested_blocks;     // Rust /* /* */ */
};

// Everything needed to present a language in the UI.
struct Info {
    Id id;
    const char* name;   // "C++"           display name
    const char* tag;    // "cpp"           markdown fence tag
    const char* badge;  // "C++"           short label drawn inside the file icon
    ImU32 color;        //                 accent color for that icon
};

// Detection. Matches exact filenames first (Makefile, Dockerfile, .env), then
// the extension. Returns Id::PlainText when nothing matches.
Id from_path(const std::string& path);
// Resolves a markdown fence tag ("py", "c++", "yml") to a language.
Id from_tag(const std::string& tag);

const Info& info(Id id);
const Spec& spec(Id id);

inline const char* name(Id id) { return info(id).name; }
inline const char* tag(Id id) { return info(id).tag; }
inline const char* badge(Id id) { return info(id).badge; }
inline ImU32 color(Id id) { return info(id).color; }

// True for languages that are really prose or data rather than code; the chat
// uses this to decide whether "explain this code" style actions make sense.
bool is_code(Id id);

} // namespace lang
