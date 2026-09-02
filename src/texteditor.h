// A syntax-highlighting code editor widget drawn with ImGui draw lists.
#pragma once

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

    void set_use_crlf(bool crlf) { use_crlf_ = crlf; }
    bool use_crlf() const { return use_crlf_; }

    // ---- cursor & selection ----
    Coord cursor() const { return cursor_; }
    void set_cursor(Coord c, bool scroll_into_view = true);
    bool has_selection() const { return sel_start_ != sel_end_; }
    std::string selected_text() const;
    void select_all();
    void select_range(Coord a, Coord b);
    void clear_selection();
    void goto_line(int line_index);

    // Text around the caret, clipped to a byte budget. Used to build AI prompts.
    std::string context_before(int max_bytes) const;
    std::string context_after(int max_bytes) const;

    // ---- editing ----
    void insert_text(const std::string& text);      // replaces the selection
    void replace_selection(const std::string& text);
    void replace_all_text(const std::string& text); // undoable whole-buffer swap
    void undo();
    void redo();
    bool can_undo() const { return undo_index_ > 0; }
    bool can_redo() const { return undo_index_ < (int)undo_.size(); }

    // ---- AI inline suggestion (ghost text at the caret) ----
    void set_ghost(const std::string& text) { ghost_ = text; }
    void clear_ghost() { ghost_.clear(); }
    bool has_ghost() const { return !ghost_.empty(); }
    const std::string& ghost() const { return ghost_; }
    void accept_ghost();

    // ---- find ----
    void open_find();
    bool find_open() const { return find_open_; }

    // ---- options ----
    int tab_size = 4;
    bool insert_spaces = true;
    bool show_line_numbers = true;
    bool show_indent_guides = true;
    bool auto_close_brackets = true;
    bool highlight_current_line = true;

    // Draws the editor. Returns true if the buffer changed this frame.
    bool render(const char* id, const ImVec2& size);

private:
    // --- text primitives ---
    Coord sanitize(Coord c) const;
    std::string range_text(Coord a, Coord b) const;
    void raw_delete(Coord a, Coord b);
    Coord raw_insert(Coord c, const std::string& text);
    void apply_edit(Coord a, Coord b, const std::string& text, bool coalesce = false);

    // --- navigation helpers ---
    Coord next_char(Coord c) const;
    Coord prev_char(Coord c) const;
    Coord word_start(Coord c) const;
    Coord word_end(Coord c) const;
    Coord line_first_non_space(int line) const;
    Coord doc_end() const;

    // --- measurement ---
    float column_to_x(const std::string& text, int column) const;
    int x_to_column(const std::string& text, float x) const;
    float run_width(const char* begin, const char* end) const;
    float text_width(const std::string& s) const { return run_width(s.data(), s.data() + s.size()); }

    // --- interaction ---
    void handle_keyboard();
    void handle_mouse(const ImVec2& text_origin);
    void type_char(unsigned int codepoint);
    void insert_newline();
    void indent_selection(bool outdent);
    void toggle_comment();
    void duplicate_line();
    void move_lines(int delta);
    void delete_selection_or(int direction); // backspace / delete
    void copy_to_clipboard(bool cut);
    void paste_from_clipboard();
    void ensure_cursor_visible();
    void scroll_cursor_into_view();

    // --- syntax ---
    void rebuild_line_states(int from_line = 0);
    void refresh_metrics();

    // --- find ---
    void update_matches();
    void jump_to_match(int delta);
    void draw_find_bar(const ImVec2& area_min, float area_width);

    struct UndoRecord {
        Coord start;
        std::string removed;
        std::string added;
        Coord cursor_before;
        Coord cursor_after;
        Coord sel_start_before;
        Coord sel_end_before;
    };

    std::vector<std::string> lines_{std::string()};
    std::vector<unsigned char> line_state_; // syntax state entering each line
    Language language_ = Language::PlainText;

    Coord cursor_;
    Coord sel_start_;
    Coord sel_end_;
    Coord drag_anchor_;
    int preferred_column_x_ = -1; // sticky column for up/down movement

    std::vector<UndoRecord> undo_;
    int undo_index_ = 0;
    int saved_undo_index_ = 0; // -1 once the saved state is unreachable
    double last_edit_time_ = 0.0;

    std::string ghost_;

    bool find_open_ = false;
    bool find_focus_ = false;
    char find_buffer_[256] = {0};
    bool find_case_sensitive_ = false;
    std::vector<Coord> matches_;
    int current_match_ = -1;
    std::string matches_for_;

    bool read_only_ = false;
    bool use_crlf_ = false;
    unsigned version_ = 0;

    // per-frame cached metrics
    float char_width_ = 8.0f;
    float line_height_ = 16.0f;
    float space_width_ = 8.0f;
    float longest_line_px_ = 0.0f;
    bool scroll_to_cursor_ = false;
    bool focused_ = false;
    double blink_reset_ = 0.0;
};

} // namespace ed
