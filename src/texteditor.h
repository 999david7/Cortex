// A syntax-highlighting code editor widget drawn with ImGui draw lists.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "lang.h"

namespace ed {

// The editor speaks the same language ids as the rest of the IDE; the registry
// in lang.h owns the list, the highlighting rules and the icon metadata.
using Language = lang::Id;

Language language_from_path(const std::string& path);
// Resolves a markdown fence tag ("py", "c++", "yml") to a language.
Language language_from_tag(const std::string& tag);
const char* language_name(Language id);
// Fence tag used when quoting this language to the model ("cpp", "python", ...).
const char* language_tag(Language id);

// A colored run of a single line, for highlighting code outside the editor
// (chat code blocks). state carries block-comment/string context between lines.
struct Span {
    int start;
    int end;
    ImU32 color;
};
unsigned char highlight_line(Language lang, const std::string& line, unsigned char state,
                             std::vector<Span>& out);

struct Coord {
    int line = 0;
    int column = 0; // byte offset inside the line

    Coord() = default;
    Coord(int l, int c) : line(l), column(c) {}

    bool operator==(const Coord& o) const { return line == o.line && column == o.column; }
    bool operator!=(const Coord& o) const { return !(*this == o); }
    bool operator<(const Coord& o) const {
        return line != o.line ? line < o.line : column < o.column;
    }
    bool operator<=(const Coord& o) const { return *this < o || *this == o; }
    bool operator>(const Coord& o) const { return o < *this; }
    bool operator>=(const Coord& o) const { return o <= *this; }
};

// One caret and the selection it drags behind it. `pos` is where the caret is
// drawn; `anchor` is the end that stays put while the selection is extended.
struct Caret {
    Coord pos;
    Coord anchor;
    float preferred_x = -1.0f; // sticky x for up/down movement, -1 when unset

    Caret() = default;
    Caret(Coord p) : pos(p), anchor(p) {}
    Caret(Coord a, Coord p) : pos(p), anchor(a) {}

    bool has_selection() const { return pos != anchor; }
    Coord start() const { return pos < anchor ? pos : anchor; }
    Coord end() const { return pos < anchor ? anchor : pos; }
    bool same_place(const Caret& o) const { return pos == o.pos && anchor == o.anchor; }
};

// The caret shapes VS Code offers (editor.cursorStyle).
enum class CursorStyle { Line, Block, Underline, LineThin, BlockOutline, UnderlineThin, Count };
// And its blink animations (editor.cursorBlinking).
enum class CursorBlink { Blink, Smooth, Phase, Expand, Solid, Count };
// What the editor draws for spaces and tabs (editor.renderWhitespace).
enum class Whitespace { None, Boundary, Selection, Trailing, All, Count };
// Case conversions for the Transform commands.
enum class CaseKind { Upper, Lower, Title, Snake, Camel, Pascal, Kebab, Count };

const char* cursor_style_name(CursorStyle s);
// Draws a caret of the given shape in the character cell whose top-left corner
// is `pos`. Shared by the editor and the settings preview.
void draw_caret_shape(ImDrawList* dl, CursorStyle style, ImVec2 pos, float cell_width,
                      float line_height, float line_width, ImU32 color, float height_scale = 1.0f);
// Opacity and height (0..1) of a caret `t` seconds after it last moved.
void caret_blink_state(CursorBlink blink, double t, float& alpha, float& height_scale);
const char* cursor_blink_name(CursorBlink b);
const char* whitespace_name(Whitespace w);
const char* case_name(CaseKind k);

class TextEditor {
public:
    TextEditor();

    // ---- content ----
    void set_text(const std::string& text);
    std::string text() const;
    bool empty() const { return lines_.size() == 1 && lines_[0].empty(); }
    int line_count() const { return (int)lines_.size(); }
    const std::string& line(int index) const;

    // Bumped on every content change; owners use it to detect redraws.
    unsigned version() const { return version_; }
    // Dirty state follows the undo position, so undoing back to the saved
    // point clears the modified marker.
    void mark_saved() { saved_undo_index_ = undo_index_; }
    bool modified() const { return undo_index_ != saved_undo_index_; }

    void set_language(Language lang);
    Language language() const { return language_; }

    void set_read_only(bool ro) { read_only_ = ro; }
    bool read_only() const { return read_only_; }

    // Line endings used when the text is saved. Changing it is an edit as far
    // as the dirty marker is concerned, so a save writes the new endings.
    void set_use_crlf(bool crlf);
    bool use_crlf() const { return use_crlf_; }

    // ---- carets & selection ----
    // The primary caret: the one the view follows and the status bar reports.
    Coord cursor() const { return carets_[primary_].pos; }
    const std::vector<Caret>& carets() const { return carets_; }
    int caret_count() const { return (int)carets_.size(); }
    // Collapses to a single caret.
    void set_cursor(Coord c, bool scroll_into_view = true);
    // True when the primary caret has a selection.
    bool has_selection() const { return carets_[primary_].has_selection(); }
    // Every selection, in document order, joined by newlines.
    std::string selected_text() const;
    // Total number of selected characters across all carets.
    int selected_chars() const;
    void select_all();
    void select_range(Coord a, Coord b);
    void clear_selection();
    // Moves to a line (and optionally a 0-based column) and centres it.
    void goto_line(int line_index, int column = 0);
    // Scrolls so the primary caret sits in the middle of the view.
    void center_on_cursor() { center_cursor_ = true; }
    // Asks for keyboard focus on the next frame.
    void request_focus() { focus_request_ = true; }

    // Text around the caret, clipped to a byte budget. Used to build AI prompts.
    std::string context_before(int max_bytes) const;
    std::string context_after(int max_bytes) const;

    // ---- editing ----
    void insert_text(const std::string& text);      // at every caret, replacing selections
    void replace_selection(const std::string& text);
    void replace_all_text(const std::string& text); // undoable whole-buffer swap
    void undo();
    void redo();
    bool can_undo() const { return undo_index_ > 0; }
    bool can_redo() const { return undo_index_ < (int)undo_.size(); }

    void cut();
    void copy();
    void paste();

    // ---- multiple carets (the VS Code set) ----
    void add_caret_vertical(int direction);    // Ctrl+Alt+Up / Down
    void add_next_occurrence();                // Ctrl+D
    void select_all_occurrences();             // Ctrl+Shift+L
    void carets_to_line_ends();                // Shift+Alt+I
    void cursor_undo();                        // Ctrl+U
    void collapse_carets();                    // Escape

    // ---- selection ----
    void expand_selection();                   // Shift+Alt+Right
    void shrink_selection();                   // Shift+Alt+Left
    void select_line();                        // expand the selection by whole lines
    void goto_matching_bracket();              // Ctrl+Shift+Backslash
    void select_to_bracket();

    // ---- line commands ----
    void copy_lines(int direction);            // Shift+Alt+Up / Down
    void move_lines(int direction);            // Alt+Up / Down
    void delete_lines();                       // Ctrl+Shift+K
    void insert_line_below();                  // Ctrl+Enter
    void insert_line_above();                  // Ctrl+Shift+Enter
    void indent_lines();                       // Ctrl+]
    void outdent_lines();                      // Ctrl+[
    void toggle_line_comment();                // Ctrl+/
    void toggle_block_comment();               // Shift+Alt+A
    void join_lines();
    void transpose_chars();
    void transform_case(CaseKind kind);
    void sort_lines(bool descending);
    void delete_duplicate_lines();
    void reverse_lines();
    void trim_trailing_whitespace();
    void convert_indentation(bool to_tabs);
    void delete_word(int direction);           // Ctrl+Backspace / Ctrl+Delete
    void delete_to_line_edge(int direction);   // Ctrl+Shift+Backspace / Delete
    void duplicate_selection();

    // Insert toggles overtype: typed characters replace the ones after the
    // caret, and the caret takes overtype_cursor_style so the mode is visible.
    bool overtype() const { return overtype_; }
    void toggle_overtype() { overtype_ = !overtype_; blink_reset_ = ImGui::GetTime(); }

    // ---- AI inline suggestion (ghost text at the caret) ----
    void set_ghost(const std::string& text) { ghost_ = text; }
    void clear_ghost() { ghost_.clear(); }
    bool has_ghost() const { return !ghost_.empty(); }
    const std::string& ghost() const { return ghost_; }
    void accept_ghost();

    // ---- find / replace ----
    void open_find();
    void open_replace();
    bool find_open() const { return find_open_; }
    void find_next() { jump_to_match(1); }
    void find_previous() { jump_to_match(-1); }
    void replace_one();
    int replace_all(); // returns how many matches were replaced
    void set_find_text(const std::string& text);
    void set_replace_text(const std::string& text);
    std::string find_text() const { return find_buffer_; }
    int match_count() { update_matches(); return (int)matches_.size(); }
    bool find_case_sensitive = false;
    bool find_whole_word = false;
    bool find_regex = false;

    // ---- options ----
    int tab_size = 4;
    bool insert_spaces = true;
    bool show_line_numbers = true;
    bool show_indent_guides = true;
    bool auto_close_brackets = true;
    bool highlight_current_line = true;
    CursorStyle cursor_style = CursorStyle::Line;
    CursorStyle overtype_cursor_style = CursorStyle::Block;
    CursorBlink cursor_blink = CursorBlink::Blink;
    int cursor_width = 2;              // pixels, for the Line style
    bool smooth_caret = false;         // animate the caret between positions
    Whitespace render_whitespace = Whitespace::Selection;
    bool show_minimap = true;
    bool bracket_pair_colors = false;
    bool match_brackets = true;
    bool highlight_occurrences = true;
    std::vector<int> rulers;           // columns that get a vertical guide

    // Ctrl+mouse wheel over the editor asks the owner to change the font size;
    // this hands the request over (+1, -1 or 0) and clears it.
    int take_zoom_request() { int z = zoom_request_; zoom_request_ = 0; return z; }

    // Draws the editor. Returns true if the buffer changed this frame.
    bool render(const char* id, const ImVec2& size);

private:
    // --- one primitive change, as recorded for undo ---
    struct Edit {
        Coord start;
        std::string removed;
        std::string added;
    };
    // One undo step: every primitive change a command made, plus the carets
    // on either side of it so undo puts them back where they were.
    struct UndoStep {
        std::vector<Edit> edits;
        std::vector<Caret> before;
        std::vector<Caret> after;
        int primary_before = 0;
        int primary_after = 0;
        int kind = 0;      // EditKind; typing and deleting runs coalesce
        double time = 0.0;
    };
    // A saved set of carets, for cursor undo and for shrinking a selection.
    struct CaretSet {
        std::vector<Caret> carets;
        int primary = 0;
    };
    enum EditKind { kEditOther = 0, kEditTyping, kEditDeleteBack, kEditDeleteForward };

    // --- text primitives ---
    Coord sanitize(Coord c) const;
    std::string range_text(Coord a, Coord b) const;
    void raw_delete(Coord a, Coord b);
    Coord raw_insert(Coord c, const std::string& text);
    static Coord end_of_insert(Coord start, const std::string& text);

    // Every change goes through a transaction: begin_edit snapshots the carets,
    // apply_edit records and performs one primitive change and shifts every
    // caret past it, end_edit closes the undo step and refreshes the caches.
    void begin_edit(int kind = kEditOther);
    void end_edit();
    // Shared tail of undo and redo.
    void after_history_move(int from_line);
    // Returns the end of the inserted text.
    Coord apply_edit(Coord a, Coord b, const std::string& text);
    void shift_carets(Coord a, Coord b, Coord end);

    // --- carets ---
    static bool same_carets(const std::vector<Caret>& a, const std::vector<Caret>& b);
    void normalize_carets();
    void push_caret_history();
    void set_single_caret(Caret c);
    void caret_moved();
    // Line ranges covered by the carets, merged where they touch. Each block
    // lists the carets inside it so line commands can put them back.
    struct LineBlock {
        int first;
        int last;
        std::vector<int> carets;
    };
    std::vector<LineBlock> line_blocks(bool merge_adjacent) const;
    // The word under or just before a position; false when there is none.
    bool word_at(Coord c, Coord& a, Coord& b) const;
    // Characters between two positions, newlines counted once.
    int count_chars(Coord a, Coord b) const;
    // Runs `fn` over the lines each line command targets (the selected lines,
    // or the whole document for a lone caret) and writes back what changed.
    void rewrite_line_ranges(const std::function<void(std::vector<std::string>&)>& fn);

    // --- navigation helpers ---
    Coord next_char(Coord c) const;
    Coord prev_char(Coord c) const;
    Coord word_start(Coord c) const;
    Coord word_end(Coord c) const;
    Coord line_first_non_space(int line) const;
    Coord doc_end() const;
    void move_carets(int how, bool extend);

    // --- brackets ---
    // Bracket characters on a line that are code, not inside a string or comment.
    void line_brackets(int line, std::vector<std::pair<int, char>>& out) const;
    bool is_code_bracket(Coord c) const;
    bool match_bracket(Coord at, Coord& match, int limit = 20000) const;
    bool find_enclosing_brackets(Coord start, Coord end, Coord& open, Coord& close) const;
    bool string_token_at(Coord start, Coord end, Coord& a, Coord& b) const;

    // --- measurement ---
    float column_to_x(const std::string& text, int column) const;
    int x_to_column(const std::string& text, float x) const;
    float run_width(const char* begin, const char* end) const;
    float text_width(const std::string& s) const { return run_width(s.data(), s.data() + s.size()); }

    // --- interaction ---
    void handle_keyboard();
    void handle_mouse(const ImVec2& text_origin, float gutter_right, float text_right);
    void type_char(unsigned int codepoint);
    void insert_newline();
    void tab_key(bool shift);
    void backspace_or_delete(int direction);
    void scroll_cursor_into_view();
    void box_select(int line_a, float x_a, int line_b, float x_b);

    // --- syntax ---
    void rebuild_line_states(int from_line = 0);
    void refresh_metrics();

    // --- find ---
    struct Match {
        Coord a;
        Coord b;
    };
    void update_matches();
    void jump_to_match(int delta);
    // The replacement for one match: literal, or the regex template expanded.
    std::string replacement_for(const Match& m) const;
    void draw_find_bar(const ImVec2& area_min, float area_width);
    bool find_in_line(const std::string& line, int from, const std::string& needle, bool whole_word,
                      bool case_sensitive, int& at) const;

    // --- drawing ---
    void draw_caret(ImDrawList* dl, const ImVec2& text_origin, const Caret& c, float alpha,
                    float height_scale, bool primary);
    void draw_minimap(ImDrawList* dl, const ImVec2& win_pos, float x, float width, float height);
    float minimap_scroll(float height) const;

    std::vector<std::string> lines_{std::string()};
    std::vector<unsigned char> line_state_; // syntax state entering each line
    std::vector<int> line_depth_;           // bracket nesting entering each line
    Language language_ = Language::PlainText;

    std::vector<Caret> carets_{Caret()};
    int primary_ = 0;
    std::vector<CaretSet> caret_history_; // for cursor_undo
    std::vector<CaretSet> expand_stack_;  // for shrink_selection
    std::vector<Caret> expanded_to_;      // what the last expand produced

    std::vector<UndoStep> undo_;
    int undo_index_ = 0;
    int saved_undo_index_ = 0; // -1 once the saved state is unreachable
    int edit_depth_ = 0;
    UndoStep pending_;
    int dirty_from_ = -1;
    // Set by undo, redo and set_text so the next edit starts a fresh step
    // instead of merging into whatever came before.
    bool break_merge_ = false;
    // The text a caret-only copy put on the clipboard; pasting exactly that
    // back inserts it as whole lines above the caret, as VS Code does.
    std::string full_line_clip_;
    bool overtype_ = false;

    std::string ghost_;

    bool find_open_ = false;
    bool find_focus_ = false;
    bool replace_open_ = false;
    bool replace_focus_ = false;
    char find_buffer_[256] = {0};
    char replace_buffer_[256] = {0};
    std::vector<Match> matches_;
    int current_match_ = -1;
    std::string matches_for_;
    bool find_error_ = false;
    // Ctrl+D searches for whole words with matching case when it started from
    // an empty selection, the way VS Code does.
    bool occurrence_whole_word_ = false;

    bool read_only_ = false;
    bool use_crlf_ = false;
    unsigned version_ = 0;

    // per-frame cached metrics
    float char_width_ = 8.0f;
    float line_height_ = 16.0f;
    float space_width_ = 8.0f;
    float longest_line_px_ = 0.0f;
    float metrics_font_size_ = 0.0f;
    float view_height_ = 0.0f;
    bool scroll_to_cursor_ = false;
    bool center_cursor_ = false;
    bool focus_request_ = false;
    bool focused_ = false;
    double blink_reset_ = 0.0;
    int zoom_request_ = 0;
    int scroll_lines_request_ = 0;

    // mouse state
    enum class Drag { None, Text, Word, Line, Box, Gutter, Minimap };
    Drag drag_ = Drag::None;
    Coord drag_anchor_;
    Coord drag_anchor_end_;
    float box_anchor_x_ = 0.0f;
    int box_anchor_line_ = 0;
    int drag_caret_ = 0; // which caret a drag extends
    ImVec2 find_bar_min_ = ImVec2(0, 0);
    ImVec2 find_bar_max_ = ImVec2(0, 0);
    float minimap_x_ = 0.0f;
    float minimap_w_ = 0.0f;

    // smooth caret animation, in text-space pixels
    ImVec2 caret_anim_ = ImVec2(-1.0f, -1.0f);

    // bracket-match cache
    unsigned bracket_cache_version_ = ~0u;
    Coord bracket_cache_caret_ = Coord(-1, -1);
    bool bracket_cache_found_ = false;
    Coord bracket_cache_a_;
    Coord bracket_cache_b_;
};

} // namespace ed
