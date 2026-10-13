// Visual language for the IDE: palette, fonts, icons and small shared widgets.
#pragma once

#include <string>

#include "imgui.h"
#include "lang.h"

namespace ui {

// ------------------------------------------------------------------ palette
struct Palette {
    ImU32 bg_deep;        // window background behind everything
    ImU32 bg_panel;       // docked panel background
    ImU32 bg_surface;     // cards, inputs, elevated rows
    ImU32 bg_surface_hi;  // hovered surface
    ImU32 bg_titlebar;
    ImU32 border;
    ImU32 border_strong;

    ImU32 text;
    ImU32 text_dim;
    ImU32 text_faint;

    ImU32 ink;     // the black that fills an active thing
    ImU32 on_ink;  // paper, for text sitting on that black

    ImU32 accent;
    ImU32 accent_hi;
    ImU32 accent_dim;   // translucent accent for fills
    ImU32 accent_soft;  // very translucent, for selections
    ImU32 cyan;

    ImU32 success;
    ImU32 warning;
    ImU32 error;

    // editor
    ImU32 code_bg;
    ImU32 line_number;
    ImU32 line_number_cur;
    ImU32 current_line;
    ImU32 selection;
    ImU32 cursor;
    ImU32 indent_guide;
    ImU32 match_highlight;
    ImU32 ghost_text;

    // syntax
    ImU32 syn_default;
    ImU32 syn_keyword;
    ImU32 syn_type;
    ImU32 syn_string;
    ImU32 syn_number;
    ImU32 syn_comment;
    ImU32 syn_preproc;
    ImU32 syn_function;
    ImU32 syn_punct;
};

extern Palette C;

struct FontSet {
    ImFont* ui = nullptr;        // default UI text
    ImFont* ui_bold = nullptr;   // semibold, for headers and buttons
    ImFont* ui_small = nullptr;  // captions, status bar
    ImFont* ui_micro = nullptr;  // tracked uppercase labels, index numbers
    ImFont* ui_large = nullptr;  // titles
    ImFont* mono = nullptr;      // code editor
    ImFont* mono_small = nullptr;
    ImFont* mono_bold = nullptr; // language badges, emphasis in code blocks
};

extern FontSet F;

// Applies colors, rounding and spacing. scale is the DPI factor.
void apply_style(float scale);
// Loads Segoe UI / Consolas from the system, falling back to the built-in font.
void load_fonts(float scale);

float scale();
void set_scale(float s);

// The code font's size before DPI scaling. The editor zooms by changing it and
// asking for the font atlas to be rebuilt, which the frame loop does between
// frames (fonts cannot change while one is being built).
float code_font_size();
void set_code_font_size(float size);
void request_font_rebuild();
// True once after request_font_rebuild(); the frame loop calls it every frame.
bool take_font_rebuild();
inline float px(float v) { return v * scale(); }

// ---------------------------------------------------------------- lettering
// Every label in the chrome is set in wide-tracked uppercase; that spacing is
// most of what the look is. ImGui has no letter-spacing, so these lay the
// glyphs out one at a time. A negative `track` means "the house tracking".
float track_for(float font_size);
float caps_width(ImFont* font, float size, const char* text, float track = -1.0f);
// Draws `text` uppercased and tracked; returns the width it painted.
float draw_caps(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color,
                const char* text, float track = -1.0f);
// The same as an ImGui item, so it can sit in a normal layout flow.
void caps_text(const char* text, ImU32 color = 0, ImFont* font = nullptr);

// A full-width hairline, the only divider this design uses.
void rule(float pad_x = 0.0f);

// "Ø1", "Ø2", … — the slashed-zero index the reference numbers everything with.
// The returned pointer is valid until the next call.
const char* index_label(int n);

// -------------------------------------------------------------------- icons
enum class Icon {
    File, Folder, FolderOpen, ChevronRight, ChevronDown, Search, Settings,
    Sparkle, Send, Stop, Save, Close, Plus, Refresh, Terminal, Copy, Insert,
    Trash, Play, Menu, Warning, Check, Robot, User, Code, Wand, Sidebar,
    Minimize, Maximize, Restore, Globe
};

// Draws an icon inside a size x size box whose top-left corner is at pos.
void draw_icon(ImDrawList* dl, Icon icon, ImVec2 pos, float size, ImU32 color);

// Draws the file mark for a language: a rounded chip in that language's color
// carrying its short badge ("PY", "TS", "C++"). Every language in lang.h gets
// one, so the file tree never falls back to a generic document icon.
void draw_lang_icon(ImDrawList* dl, lang::Id id, ImVec2 pos, float size);

// Same chip, drawn as an ImGui item so it can sit inline in a row of widgets.
void lang_chip(lang::Id id, float size = 0.0f);

// ------------------------------------------------------------------ widgets
// Square icon button. Returns true when clicked.
bool icon_button(const char* id, Icon icon, const char* tooltip = nullptr,
                 ImU32 color = 0, float box = 0.0f);

// Filled accent button.
bool primary_button(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true);
// Outlined / ghost button.
bool ghost_button(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true);
// Small pill-shaped button used for chat actions and code-block tools.
bool chip_button(const char* label, Icon icon, const char* tooltip = nullptr);

// Small colored label, e.g. a language badge.
void badge(const char* text, ImU32 color);

// Section header inside a panel.
void panel_header(const char* label, Icon icon);

// A left-aligned selectable row with an icon, used by the file tree.
// Pass file_lang to draw that language's badge instead of the generic icon;
// lang::Id::Count means "use icon".
bool tree_row(const char* id, Icon icon, const char* label, bool selected,
              int depth, bool* toggled = nullptr, bool has_children = false,
              bool expanded = false, ImU32 icon_color = 0,
              lang::Id file_lang = lang::Id::Count);

// Animated three-square "thinking" indicator.
void typing_indicator(float time);

// A numbered cell from the reference's tab rail: the index in micro type over
// a tracked uppercase label, hairline-boxed, filled with ink when active.
struct TabCell {
    bool clicked = false;
    bool close_clicked = false;
    bool hovered = false;
    float width = 0.0f;
};
TabCell numbered_tab(const char* id, int index, const char* label, bool active,
                     bool modified = false, lang::Id file_lang = lang::Id::Count,
                     bool closable = true);

// ------------------------------------------------------------ bar furniture

// A status-bar cell: tracked uppercase text, closed by a hairline rule rather
// than a wedge. Pass bg = 0 to leave it on the paper, or ink to fill it.
// `x` is the left edge; the returned value is where the next cell starts.
float segment(ImDrawList* dl, float x, float top, float height, const char* text,
              ImU32 bg, ImU32 fg);

// The same, laid out right-to-left from `right`. Returns the new right edge.
float segment_r(ImDrawList* dl, float right, float top, float height, const char* text,
                ImU32 bg, ImU32 fg);

// Draws the hairline frame around the window currently being built; it goes to
// ink while that pane holds focus. Call it last, inside the window, so it
// paints over the content edges.
void pane_frame();

// A pane's title strip: "Ø1  NAME" on the left, optional detail on the right,
// closed by a hairline. Returns the height it consumed.
float pane_title(int index, const char* name, const char* detail = nullptr);

// Tooltip helper honouring the app style.
void tooltip(const char* text);

// Wraps ImGui::Spacing() with an explicit pixel height.
void vspace(float h);

} // namespace ui
