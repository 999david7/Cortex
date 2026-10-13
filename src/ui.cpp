#include "ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui_internal.h"

namespace ui {

Palette C;
FontSet F;
static float g_scale = 1.0f;

static float g_code_font_size = 14.0f;
static bool g_font_rebuild = false;

float scale() { return g_scale; }
void set_scale(float s) { g_scale = s; }

float code_font_size() { return g_code_font_size; }
void set_code_font_size(float size) { g_code_font_size = ImClamp(size, 6.0f, 48.0f); }
void request_font_rebuild() { g_font_rebuild = true; }
bool take_font_rebuild() {
    const bool r = g_font_rebuild;
    g_font_rebuild = false;
    return r;
}

static ImU32 rgb(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

static void init_palette() {
    // Paper white, ink black, hairline rules. Nothing is filled unless it is
    // *active* — active means solid black with the type knocked out white — and
    // the only two hues left in the whole interface are the ochre and the brick
    // that mark data: a warning and an error, nothing else.
    C.bg_deep       = rgb(0xFF, 0xFF, 0xFF); // the paper the panes are laid on
    C.bg_panel      = rgb(0xFF, 0xFF, 0xFF);
    C.bg_surface    = rgb(0xFA, 0xFA, 0xF9); // inputs and cards, a shade off white
    C.bg_surface_hi = rgb(0xF0, 0xF0, 0xEE); // hover
    C.bg_titlebar   = rgb(0xFF, 0xFF, 0xFF);
    C.border        = rgb(0xE4, 0xE4, 0xE0); // the hairline
    C.border_strong = rgb(0xC6, 0xC6, 0xC1);

    C.text       = rgb(0x14, 0x14, 0x13);
    C.text_dim   = rgb(0x8C, 0x8C, 0x87); // inactive labels in the rails
    C.text_faint = rgb(0xB6, 0xB6, 0xB1); // index numbers, hints

    C.ink    = rgb(0x0E, 0x0E, 0x0D);
    C.on_ink = rgb(0xFC, 0xFC, 0xFA);

    // "Accent" is not a hue here: emphasis is carried by black against white.
    C.accent      = rgb(0x0E, 0x0E, 0x0D);
    C.accent_hi   = rgb(0x00, 0x00, 0x00);
    C.accent_dim  = rgb(0x0E, 0x0E, 0x0D, 0x1C);
    C.accent_soft = rgb(0x0E, 0x0E, 0x0D, 0x0B);
    C.cyan        = rgb(0x4A, 0x5A, 0x62);

    C.success = rgb(0x4E, 0x7A, 0x48);
    C.warning = rgb(0xB0, 0x86, 0x1C); // ochre
    C.error   = rgb(0xB2, 0x3B, 0x2D); // brick

    C.code_bg         = rgb(0xFF, 0xFF, 0xFF);
    C.line_number     = rgb(0xC6, 0xC6, 0xC1);
    C.line_number_cur = rgb(0x0E, 0x0E, 0x0D);
    C.current_line    = rgb(0x0E, 0x0E, 0x0D, 0x09);
    C.selection       = rgb(0x0E, 0x0E, 0x0D, 0x24);
    C.cursor          = rgb(0x0E, 0x0E, 0x0D);
    C.indent_guide    = rgb(0xEE, 0xEE, 0xEB);
    C.match_highlight = rgb(0xB0, 0x86, 0x1C, 0x38);
    C.ghost_text      = rgb(0xB6, 0xB6, 0xB1);

    // Source colors for paper: ink for what you wrote, the two data hues for
    // literals and keywords, and everything structural in grey.
    C.syn_default  = rgb(0x23, 0x26, 0x2A);
    C.syn_keyword  = rgb(0xA3, 0x3A, 0x2A); // brick
    C.syn_type     = rgb(0x1F, 0x5C, 0x68); // deep teal
    C.syn_string   = rgb(0x7C, 0x62, 0x14); // ochre, darkened for white paper
    C.syn_number   = rgb(0x96, 0x59, 0x1C);
    C.syn_comment  = rgb(0xA8, 0xA8, 0xA3);
    C.syn_preproc  = rgb(0x5B, 0x65, 0x70);
    C.syn_function = rgb(0x11, 0x13, 0x17); // the darkest ink on the page
    C.syn_punct    = rgb(0x8C, 0x91, 0x96);
}

static ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void apply_style(float s) {
    init_palette();
    g_scale = s;

    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();

    // Airier than a desktop IDE. The reference gets its calm from white space
    // around small type, so items are given room rather than packed.
    st.WindowPadding     = ImVec2(14, 12);
    st.FramePadding      = ImVec2(10, 6);
    st.CellPadding       = ImVec2(8, 6);
    st.ItemSpacing       = ImVec2(8, 8);
    st.ItemInnerSpacing  = ImVec2(6, 5);
    st.IndentSpacing     = 16.0f;
    st.ScrollbarSize     = 8.0f;
    st.GrabMinSize       = 10.0f;

    // Every pane is a hairline rectangle. Nothing floats, nothing is rounded.
    st.WindowBorderSize  = 1.0f;
    st.ChildBorderSize   = 1.0f;
    st.PopupBorderSize   = 1.0f;
    st.FrameBorderSize   = 1.0f;
    st.TabBarBorderSize  = 1.0f;

    st.WindowRounding    = 0.0f;
    st.ChildRounding     = 0.0f;
    st.FrameRounding     = 0.0f;
    st.PopupRounding     = 0.0f;
    st.ScrollbarRounding = 0.0f;
    st.GrabRounding      = 0.0f;
    st.TabRounding       = 0.0f;

    st.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    st.WindowMenuButtonPosition = ImGuiDir_None;
    st.ColorButtonPosition = ImGuiDir_Left;
    st.SeparatorTextBorderSize = 1.0f;
    st.SeparatorTextPadding = ImVec2(12, 3);
    // Panes are separated by paper, not by a gutter: a thin run of white with
    // each pane's own hairline on either side of it reads as a ruled grid.
    st.DockingSeparatorSize = 4.0f;

    ImVec4* col = st.Colors;
    col[ImGuiCol_Text]                  = v4(C.text);
    col[ImGuiCol_TextDisabled]          = v4(C.text_faint);
    col[ImGuiCol_WindowBg]              = v4(C.bg_panel);
    col[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_PopupBg]               = v4(rgb(0xFF, 0xFF, 0xFF, 0xFC));
    col[ImGuiCol_Border]                = v4(C.border);
    col[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_FrameBg]               = v4(C.bg_surface);
    col[ImGuiCol_FrameBgHovered]        = v4(C.bg_surface_hi);
    col[ImGuiCol_FrameBgActive]         = v4(C.bg_surface_hi);
    col[ImGuiCol_TitleBg]               = v4(C.bg_titlebar);
    col[ImGuiCol_TitleBgActive]         = v4(C.bg_titlebar);
    col[ImGuiCol_TitleBgCollapsed]      = v4(C.bg_titlebar);
    col[ImGuiCol_MenuBarBg]             = v4(C.bg_titlebar);
    col[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ScrollbarGrab]         = v4(rgb(0xDC, 0xDC, 0xD8));
    col[ImGuiCol_ScrollbarGrabHovered]  = v4(rgb(0xB6, 0xB6, 0xB1));
    col[ImGuiCol_ScrollbarGrabActive]   = v4(C.ink);
    col[ImGuiCol_CheckMark]             = v4(C.accent_hi);
    col[ImGuiCol_SliderGrab]            = v4(C.accent);
    col[ImGuiCol_SliderGrabActive]      = v4(C.accent_hi);
    col[ImGuiCol_Button]                = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ButtonHovered]         = v4(C.bg_surface_hi);
    col[ImGuiCol_ButtonActive]          = v4(C.accent_dim);
    col[ImGuiCol_Header]                = v4(C.accent_soft);
    col[ImGuiCol_HeaderHovered]         = v4(C.bg_surface_hi);
    col[ImGuiCol_HeaderActive]          = v4(C.accent_dim);
    // The runs between docked panes are paper: each pane draws its own hairline,
    // so a white gutter between two of them reads as a single ruled grid.
    col[ImGuiCol_Separator]             = v4(C.bg_deep);
    col[ImGuiCol_SeparatorHovered]      = v4(C.border_strong);
    col[ImGuiCol_SeparatorActive]       = v4(C.ink);
    col[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ResizeGripHovered]     = v4(C.border_strong);
    col[ImGuiCol_ResizeGripActive]      = v4(C.ink);
    // The modals' tab bars follow the same rule as the rail: the selected tab is
    // marked by a rule of ink over it, not by a coloured shape.
    col[ImGuiCol_Tab]                   = v4(C.bg_titlebar);
    col[ImGuiCol_TabHovered]            = v4(C.bg_surface_hi);
    col[ImGuiCol_TabSelected]           = v4(C.bg_panel);
    col[ImGuiCol_TabSelectedOverline]   = v4(C.ink);
    col[ImGuiCol_TabDimmed]             = v4(C.bg_titlebar);
    col[ImGuiCol_TabDimmedSelected]     = v4(C.bg_panel);
    col[ImGuiCol_TabDimmedSelectedOverline] = v4(C.border_strong);
    col[ImGuiCol_DockingPreview]        = v4(C.accent_dim);
    col[ImGuiCol_DockingEmptyBg]        = v4(C.bg_deep);
    col[ImGuiCol_TableHeaderBg]         = v4(C.bg_surface);
    col[ImGuiCol_TableBorderStrong]     = v4(C.border);
    col[ImGuiCol_TableBorderLight]      = v4(C.border);
    col[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_TableRowBgAlt]         = v4(rgb(0x0E, 0x0E, 0x0D, 0x05));
    col[ImGuiCol_TextSelectedBg]        = v4(C.selection);
    col[ImGuiCol_NavHighlight]          = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ModalWindowDimBg]      = v4(rgb(0xFF, 0xFF, 0xFF, 0xB4));

    st.ScaleAllSizes(s);
}

// ------------------------------------------------------------------- fonts

// Latin-1 plus the punctuation and symbols the UI and source files actually use.
// Without this, characters like the ellipsis render as a fallback box.
static const ImWchar* build_glyph_ranges() {
    static ImVector<ImWchar> ranges;
    if (!ranges.empty()) return ranges.Data;

    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault()); // ASCII + Latin-1
    for (ImWchar c = 0x0100; c <= 0x017F; ++c) builder.AddChar(c);    // Latin Extended-A
    for (ImWchar c = 0x2010; c <= 0x2027; ++c) builder.AddChar(c);    // dashes, quotes, ellipsis
    const ImWchar extra[] = {0x20AC, 0x2192, 0x2190, 0x2191, 0x2193, 0x2022,
                             0x2713, 0x2717, 0x00D7, 0x221E, 0};
    for (int i = 0; extra[i]; ++i) builder.AddChar(extra[i]);
    builder.BuildRanges(&ranges);
    return ranges.Data;
}

// Picks the first font that exists on this machine. The lists are ordered
// best-first, so a user who installs a nicer mono gets it automatically.
static ImFont* load_first(const char* const* candidates, float size,
                          const ImFontConfig* cfg) {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* const* p = candidates; *p; ++p) {
        // AddFontFromFileTTF asserts on a missing file in debug builds, so
        // check readability ourselves before handing the path to ImGui.
        if (FILE* f = fopen(*p, "rb")) {
            fclose(f);
            if (ImFont* font = io.Fonts->AddFontFromFileTTF(*p, size, cfg)) return font;
        }
    }
    return nullptr;
}

// Xcode uses SF Mono, which is Apple-licensed and never present on Windows.
// These are the close substitutes, in descending order of similarity; Consolas
// is the guaranteed fallback because it ships with Windows.
static const char* kMonoFonts[] = {
    "C:/Windows/Fonts/SFMono-Regular.otf",
    "C:/Windows/Fonts/SF-Mono-Regular.otf",
    "C:/Windows/Fonts/CascadiaMono.ttf",
    "C:/Windows/Fonts/CascadiaCode.ttf",
    "C:/Windows/Fonts/JetBrainsMono-Regular.ttf",
    "C:/Windows/Fonts/FiraCode-Regular.ttf",
    "C:/Windows/Fonts/IBMPlexMono-Regular.ttf",
    "C:/Windows/Fonts/consola.ttf",
    "C:/Windows/Fonts/lucon.ttf",
    nullptr};

static const char* kMonoBoldFonts[] = {
    "C:/Windows/Fonts/SFMono-Bold.otf",
    "C:/Windows/Fonts/CascadiaMono-Bold.ttf",
    "C:/Windows/Fonts/JetBrainsMono-Bold.ttf",
    "C:/Windows/Fonts/consolab.ttf",
    nullptr};

static const char* kUiFonts[] = {
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/tahoma.ttf",
    nullptr};

static const char* kUiSemiboldFonts[] = {
    "C:/Windows/Fonts/seguisb.ttf",
    "C:/Windows/Fonts/segoeuib.ttf",
    "C:/Windows/Fonts/tahomabd.ttf",
    nullptr};

void load_fonts(float s) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = false;
    cfg.GlyphRanges = build_glyph_ranges();

    // Mono everywhere, chrome included. A proportional UI font is the single
    // thing that would stop this reading as a terminal-native tool, and the
    // fixed grid is what makes the panes and status segments line up.
    ImFontConfig mcfg = cfg;
    mcfg.PixelSnapH = true; // keeps the character grid crisp at any zoom

    F.ui       = load_first(kMonoFonts, std::floor(14.0f * s), &mcfg);
    F.ui_bold  = load_first(kMonoBoldFonts, std::floor(14.0f * s), &mcfg);
    F.ui_small = load_first(kMonoFonts, std::floor(12.0f * s), &mcfg);
    // The labels in the chrome are tiny and widely tracked; at this size the
    // tracking, not the letterforms, is what carries the look.
    F.ui_micro = load_first(kMonoFonts, std::floor(10.0f * s), &mcfg);
    F.ui_large = load_first(kMonoBoldFonts, std::floor(17.0f * s), &mcfg);

    // Only the editor face follows the zoom; the chrome stays put.
    F.mono       = load_first(kMonoFonts, std::floor(g_code_font_size * s), &mcfg);
    F.mono_small = load_first(kMonoFonts, std::floor(12.0f * s), &mcfg);
    F.mono_bold  = load_first(kMonoBoldFonts, std::floor(14.0f * s), &mcfg);

    // Proportional faces are the fallback rather than the default: if no mono
    // is installed at all, legible text still beats the built-in bitmap font.
    if (!F.ui) F.ui = load_first(kUiFonts, std::floor(15.0f * s), &cfg);
    if (!F.ui) F.ui = io.Fonts->AddFontDefault();
    if (!F.ui_bold) F.ui_bold = load_first(kUiSemiboldFonts, std::floor(15.0f * s), &cfg);
    if (!F.ui_bold) F.ui_bold = F.ui;
    if (!F.ui_small) F.ui_small = F.ui;
    if (!F.ui_micro) F.ui_micro = F.ui_small;
    if (!F.ui_large) F.ui_large = F.ui_bold;
    if (!F.mono) F.mono = F.ui;
    if (!F.mono_small) F.mono_small = F.mono;
    if (!F.mono_bold) F.mono_bold = F.mono;

    io.FontDefault = F.ui;
    io.Fonts->Build();
}

// --------------------------------------------------------------- lettering

// The house tracking: about a ninth of the em. Small uppercase set solid reads
// as a word; opened up this far it reads as a label, which is the whole trick.
float track_for(float font_size) { return ImMax(1.0f, std::floor(font_size * 0.11f)); }

namespace {

// Uppercases ASCII only. Anything above it — Ø in the index numbers, accented
// letters in a file name — is already the form we want.
unsigned int upper_cp(unsigned int c) {
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

} // namespace

float caps_width(ImFont* font, float size, const char* text, float track) {
    if (!font || !text || !*text) return 0.0f;
    if (track < 0.0f) track = track_for(size);
    const float k = size / font->FontSize;
    float w = 0.0f;
    int n = 0;
    for (const char* p = text; *p;) {
        unsigned int cp = 0;
        const int len = ImTextCharFromUtf8(&cp, p, nullptr);
        if (len <= 0 || cp == 0) break;
        p += len;
        w += font->GetCharAdvance((ImWchar)upper_cp(cp)) * k;
        ++n;
    }
    return w + (n > 1 ? (float)(n - 1) * track : 0.0f);
}

float draw_caps(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color,
                const char* text, float track) {
    if (!dl || !font || !text || !*text) return 0.0f;
    if (track < 0.0f) track = track_for(size);
    const float k = size / font->FontSize;
    float x = pos.x;
    for (const char* p = text; *p;) {
        unsigned int cp = 0;
        const int len = ImTextCharFromUtf8(&cp, p, nullptr);
        if (len <= 0 || cp == 0) break;
        p += len;
        cp = upper_cp(cp);
        char buf[5];
        ImTextCharToUtf8(buf, cp);
        dl->AddText(font, size, ImVec2(std::floor(x), pos.y), color, buf);
        x += font->GetCharAdvance((ImWchar)cp) * k + track;
    }
    return ImMax(0.0f, x - pos.x - track);
}

void caps_text(const char* text, ImU32 color, ImFont* font) {
    ImFont* f = font ? font : (F.ui_micro ? F.ui_micro : F.ui_small);
    if (!f) return;
    const float size = f->FontSize;
    const float w = caps_width(f, size, text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    draw_caps(ImGui::GetWindowDrawList(), f, size, pos, color ? color : C.text_dim, text);
    ImGui::Dummy(ImVec2(w, size));
}

void rule(float pad_x) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float y = std::floor(p.y) + 0.5f;
    dl->AddLine(ImVec2(p.x + pad_x, y), ImVec2(p.x + w - pad_x, y), C.border, 1.0f);
    ImGui::Dummy(ImVec2(w, 1.0f));
}

const char* index_label(int n) {
    // Ø rather than 0, the way the reference numbers its rails.
    static char buf[8];
    if (n >= 0 && n < 10) std::snprintf(buf, sizeof(buf), "\xC3\x98%d", n);
    else std::snprintf(buf, sizeof(buf), "%d", n);
    return buf;
}

// ------------------------------------------------------------------- icons

namespace {

struct IconGeom {
    ImDrawList* dl;
    ImVec2 o;     // origin (top-left)
    float s;      // box size
    ImU32 col;
    float th;     // stroke thickness

    ImVec2 p(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
    void line(float x1, float y1, float x2, float y2) const {
        dl->AddLine(p(x1, y1), p(x2, y2), col, th);
    }
    void rect(float x1, float y1, float x2, float y2, float r = 0.12f) const {
        dl->AddRect(p(x1, y1), p(x2, y2), col, r * s, 0, th);
    }
    void rect_filled(float x1, float y1, float x2, float y2, float r = 0.12f) const {
        dl->AddRectFilled(p(x1, y1), p(x2, y2), col, r * s);
    }
    void circle(float cx, float cy, float r) const {
        dl->AddCircle(p(cx, cy), r * s, col, 0, th);
    }
    void circle_filled(float cx, float cy, float r) const {
        dl->AddCircleFilled(p(cx, cy), r * s, col, 0);
    }
    void poly(const float* pts, int n, bool filled, bool closed = true) const {
        ImVec2 v[16];
        for (int i = 0; i < n && i < 16; ++i) v[i] = p(pts[i * 2], pts[i * 2 + 1]);
        if (filled) dl->AddConvexPolyFilled(v, n, col);
        else dl->AddPolyline(v, n, col, closed ? ImDrawFlags_Closed : 0, th);
    }
};

void icon_star(const IconGeom& g, float cx, float cy, float r) {
    // Four-pointed sparkle drawn as a concave diamond.
    const float k = r * 0.34f;
    ImVec2 pts[8] = {
        g.p(cx, cy - r),      g.p(cx + k, cy - k),
        g.p(cx + r, cy),      g.p(cx + k, cy + k),
        g.p(cx, cy + r),      g.p(cx - k, cy + k),
        g.p(cx - r, cy),      g.p(cx - k, cy - k)};
    g.dl->AddConcavePolyFilled(pts, 8, g.col);
}

} // namespace

void draw_icon(ImDrawList* dl, Icon icon, ImVec2 pos, float size, ImU32 color) {
    IconGeom g{dl, pos, size, color, ImMax(1.3f, size * 0.085f)};

    switch (icon) {
        case Icon::File: {
            const float pts[] = {0.24f, 0.10f, 0.62f, 0.10f, 0.78f, 0.28f,
                                 0.78f, 0.90f, 0.24f, 0.90f};
            g.poly(pts, 5, false);
            g.line(0.60f, 0.11f, 0.60f, 0.29f);
            g.line(0.60f, 0.28f, 0.77f, 0.28f);
            break;
        }
        case Icon::Folder: {
            const float pts[] = {0.12f, 0.80f, 0.12f, 0.22f, 0.42f, 0.22f,
                                 0.50f, 0.34f, 0.88f, 0.34f, 0.88f, 0.80f};
            g.poly(pts, 6, false);
            break;
        }
        case Icon::FolderOpen: {
            const float back[] = {0.12f, 0.80f, 0.12f, 0.22f, 0.42f, 0.22f,
                                  0.50f, 0.34f, 0.82f, 0.34f, 0.82f, 0.46f};
            g.poly(back, 6, false, false);
            const float front[] = {0.12f, 0.80f, 0.26f, 0.46f, 0.96f, 0.46f,
                                   0.82f, 0.80f};
            g.poly(front, 4, false);
            break;
        }
        case Icon::ChevronRight:
            g.line(0.40f, 0.24f, 0.66f, 0.50f);
            g.line(0.66f, 0.50f, 0.40f, 0.76f);
            break;
        case Icon::ChevronDown:
            g.line(0.24f, 0.40f, 0.50f, 0.66f);
            g.line(0.50f, 0.66f, 0.76f, 0.40f);
            break;
        case Icon::Search:
            g.circle(0.44f, 0.44f, 0.26f);
            g.line(0.63f, 0.63f, 0.84f, 0.84f);
            break;
        case Icon::Settings: {
            g.circle(0.5f, 0.5f, 0.18f);
            for (int i = 0; i < 6; ++i) {
                float a = (float)i * 3.14159265f / 3.0f;
                float ca = std::cos(a), sa = std::sin(a);
                g.line(0.5f + ca * 0.29f, 0.5f + sa * 0.29f,
                       0.5f + ca * 0.42f, 0.5f + sa * 0.42f);
            }
            break;
        }
        case Icon::Sparkle:
            icon_star(g, 0.44f, 0.44f, 0.36f);
            icon_star(g, 0.79f, 0.79f, 0.17f);
            break;
        case Icon::Wand:
            g.line(0.18f, 0.84f, 0.68f, 0.34f);
            icon_star(g, 0.79f, 0.23f, 0.19f);
            break;
        case Icon::Send: {
            const float pts[] = {0.14f, 0.18f, 0.88f, 0.50f, 0.14f, 0.82f, 0.30f, 0.50f};
            g.poly(pts, 4, true);
            break;
        }
        case Icon::Stop:
            g.rect_filled(0.28f, 0.28f, 0.72f, 0.72f, 0.16f);
            break;
        case Icon::Play: {
            const float pts[] = {0.30f, 0.20f, 0.82f, 0.50f, 0.30f, 0.80f};
            g.poly(pts, 3, true);
            break;
        }
        case Icon::Save: {
            g.rect(0.16f, 0.16f, 0.84f, 0.84f, 0.14f);
            g.rect_filled(0.34f, 0.16f, 0.66f, 0.40f, 0.05f);
            g.rect(0.32f, 0.56f, 0.68f, 0.84f, 0.05f);
            break;
        }
        case Icon::Close:
            g.line(0.28f, 0.28f, 0.72f, 0.72f);
            g.line(0.72f, 0.28f, 0.28f, 0.72f);
            break;
        case Icon::Plus:
            g.line(0.50f, 0.24f, 0.50f, 0.76f);
            g.line(0.24f, 0.50f, 0.76f, 0.50f);
            break;
        case Icon::Refresh: {
            dl->PathArcTo(g.p(0.5f, 0.5f), 0.30f * size, -0.6f, 4.2f, 20);
            dl->PathStroke(color, 0, g.th);
            const float head[] = {0.68f, 0.10f, 0.86f, 0.30f, 0.60f, 0.34f};
            g.poly(head, 3, true);
            break;
        }
        case Icon::Terminal:
            g.line(0.20f, 0.28f, 0.44f, 0.50f);
            g.line(0.44f, 0.50f, 0.20f, 0.72f);
            g.line(0.52f, 0.74f, 0.82f, 0.74f);
            break;
        case Icon::Copy:
            g.rect(0.14f, 0.14f, 0.62f, 0.62f, 0.14f);
            g.rect(0.38f, 0.38f, 0.86f, 0.86f, 0.14f);
            break;
        case Icon::Insert:
            g.line(0.50f, 0.16f, 0.50f, 0.70f);
            g.line(0.30f, 0.50f, 0.50f, 0.70f);
            g.line(0.70f, 0.50f, 0.50f, 0.70f);
            g.line(0.20f, 0.86f, 0.80f, 0.86f);
            break;
        case Icon::Trash:
            g.line(0.18f, 0.28f, 0.82f, 0.28f);
            g.line(0.38f, 0.28f, 0.38f, 0.16f);
            g.line(0.62f, 0.28f, 0.62f, 0.16f);
            g.line(0.38f, 0.16f, 0.62f, 0.16f);
            {
                const float body[] = {0.28f, 0.28f, 0.72f, 0.28f, 0.66f, 0.86f, 0.34f, 0.86f};
                g.poly(body, 4, false);
            }
            break;
        case Icon::Menu:
            g.line(0.20f, 0.32f, 0.80f, 0.32f);
            g.line(0.20f, 0.50f, 0.80f, 0.50f);
            g.line(0.20f, 0.68f, 0.80f, 0.68f);
            break;
        case Icon::Warning: {
            const float tri[] = {0.50f, 0.14f, 0.92f, 0.84f, 0.08f, 0.84f};
            g.poly(tri, 3, false);
            g.line(0.50f, 0.40f, 0.50f, 0.62f);
            g.circle_filled(0.50f, 0.73f, 0.045f);
            break;
        }
        case Icon::Check:
            g.line(0.22f, 0.52f, 0.42f, 0.72f);
            g.line(0.42f, 0.72f, 0.80f, 0.28f);
            break;
        case Icon::Robot:
            g.rect(0.18f, 0.32f, 0.82f, 0.82f, 0.20f);
            g.line(0.50f, 0.32f, 0.50f, 0.18f);
            g.circle_filled(0.50f, 0.14f, 0.055f);
            g.circle_filled(0.37f, 0.52f, 0.055f);
            g.circle_filled(0.63f, 0.52f, 0.055f);
            g.line(0.38f, 0.68f, 0.62f, 0.68f);
            break;
        case Icon::User:
            g.circle(0.50f, 0.34f, 0.18f);
            dl->PathArcTo(g.p(0.5f, 0.94f), 0.34f * size, 3.34159f, 6.0832f, 16);
            dl->PathStroke(color, 0, g.th);
            break;
        case Icon::Code:
            g.line(0.34f, 0.30f, 0.14f, 0.50f);
            g.line(0.14f, 0.50f, 0.34f, 0.70f);
            g.line(0.66f, 0.30f, 0.86f, 0.50f);
            g.line(0.86f, 0.50f, 0.66f, 0.70f);
            g.line(0.56f, 0.20f, 0.44f, 0.80f);
            break;
        case Icon::Sidebar:
            g.rect(0.12f, 0.20f, 0.88f, 0.80f, 0.14f);
            g.line(0.40f, 0.20f, 0.40f, 0.80f);
            break;
        case Icon::Minimize:
            dl->AddLine(g.p(0.18f, 0.52f), g.p(0.82f, 0.52f), color, 1.0f * ImMax(1.0f, g_scale));
            break;
        case Icon::Maximize:
            dl->AddRect(g.p(0.20f, 0.22f), g.p(0.80f, 0.78f), color, 0.0f, 0,
                        1.0f * ImMax(1.0f, g_scale));
            break;
        case Icon::Restore:
            dl->AddRect(g.p(0.14f, 0.30f), g.p(0.68f, 0.84f), color, 0.0f, 0,
                        1.0f * ImMax(1.0f, g_scale));
            dl->AddLine(g.p(0.32f, 0.30f), g.p(0.32f, 0.16f), color, 1.0f);
            dl->AddLine(g.p(0.32f, 0.16f), g.p(0.86f, 0.16f), color, 1.0f);
            dl->AddLine(g.p(0.86f, 0.16f), g.p(0.86f, 0.70f), color, 1.0f);
            dl->AddLine(g.p(0.86f, 0.70f), g.p(0.72f, 0.70f), color, 1.0f);
            break;

        case Icon::Globe: {
            // A wireframe globe: the outline, the equator with two latitudes,
            // and one meridian ellipse. It is the mark in the top-left corner.
            g.circle(0.50f, 0.50f, 0.42f);
            g.line(0.08f, 0.50f, 0.92f, 0.50f);
            g.line(0.12f, 0.32f, 0.88f, 0.32f);
            g.line(0.12f, 0.68f, 0.88f, 0.68f);
            ImVec2 meridian[25];
            for (int i = 0; i < 25; ++i) {
                const float a = (float)i / 24.0f * 6.2831853f;
                meridian[i] = g.p(0.50f + std::sin(a) * 0.19f, 0.50f - std::cos(a) * 0.42f);
            }
            dl->AddPolyline(meridian, 25, color, 0, g.th);
            break;
        }
    }
}

// ----------------------------------------------------------------- widgets

bool icon_button(const char* id, Icon icon, const char* tip, ImU32 color, float box) {
    if (box <= 0.0f) box = ImGui::GetFrameHeight();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(box, box));
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 hi(pos.x + box, pos.y + box);
    if (held) {
        dl->AddRectFilled(pos, hi, C.ink);
    } else if (hovered) {
        dl->AddRectFilled(pos, hi, C.bg_surface_hi);
        dl->AddRect(pos, hi, C.border_strong);
    }

    ImU32 c = color ? color : (held ? C.on_ink : (hovered ? C.text : C.text_dim));
    float glyph = std::floor(box * 0.54f);
    ImVec2 origin(pos.x + (box - glyph) * 0.5f, pos.y + (box - glyph) * 0.5f);
    draw_icon(dl, icon, origin, glyph, c);

    if (tip && hovered) tooltip(tip);
    return clicked;
}

// An ImGui label carries its id after "##"; only the part in front of it is
// drawn. ImGui::Button does this for us — the drawn widgets have to do it here.
static const char* visible(const char* label, char* buf, size_t n) {
    const char* hash = strstr(label, "##");
    if (!hash) return label;
    size_t len = (size_t)(hash - label);
    if (len > n - 1) len = n - 1;
    memcpy(buf, label, len);
    buf[len] = '\0';
    return buf;
}

// Buttons are drawn rather than pushed through ImGui::Button so their labels
// can be set in the tracked uppercase the rest of the chrome uses.
static bool button_impl(const char* label, ImVec2 size, bool enabled, bool filled) {
    ImFont* font = F.ui_small ? F.ui_small : F.ui;
    if (!font) return false;
    char vbuf[128];
    const char* text = visible(label, vbuf, sizeof(vbuf));
    const float fs = font->FontSize;
    const float tw = caps_width(font, fs, text);

    float h = size.y > 0.0f ? size.y : std::floor(fs + px(16.0f));
    float w;
    if (size.x > 0.0f) w = size.x;
    else if (size.x < 0.0f) w = ImMax(px(24.0f), ImGui::GetContentRegionAvail().x + size.x);
    else w = tw + px(28.0f);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h)) && enabled;
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 lo = pos, hi(pos.x + w, pos.y + h);

    ImU32 fg;
    if (!enabled) {
        dl->AddRect(lo, hi, C.border);
        fg = C.text_faint;
    } else if (filled) {
        // Solid ink with the label knocked out of it — the one filled shape in
        // the interface, kept for the single primary action on a surface.
        dl->AddRectFilled(lo, hi, held ? C.accent_hi : (hovered ? rgb(0x2E, 0x2E, 0x2C) : C.ink));
        fg = C.on_ink;
    } else {
        if (hovered) dl->AddRectFilled(lo, hi, C.bg_surface_hi);
        dl->AddRect(lo, hi, hovered ? C.ink : C.border_strong);
        fg = hovered ? C.text : C.text_dim;
    }

    draw_caps(dl, font, fs, ImVec2(pos.x + (w - tw) * 0.5f, pos.y + (h - fs) * 0.5f), fg, text);
    return clicked;
}

bool primary_button(const char* label, ImVec2 size, bool enabled) {
    return button_impl(label, size, enabled, true);
}

bool ghost_button(const char* label, ImVec2 size, bool enabled) {
    return button_impl(label, size, enabled, false);
}

bool chip_button(const char* label, Icon icon, const char* tip) {
    ImFont* font = F.ui_micro ? F.ui_micro : F.ui_small;
    if (!font) return false;
    char vbuf[128];
    const char* text = visible(label, vbuf, sizeof(vbuf));
    const float fs = font->FontSize;
    const float pad = px(10.0f);
    const float glyph = std::floor(fs * 1.05f);
    const float tw = caps_width(font, fs, text);
    const float h = std::floor(fs + px(13.0f));
    const float w = pad * 2.0f + glyph + px(6.0f) + tw;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 hi(pos.x + w, pos.y + h);
    if (held) dl->AddRectFilled(pos, hi, C.ink);
    else if (hovered) dl->AddRectFilled(pos, hi, C.bg_surface_hi);
    dl->AddRect(pos, hi, hovered || held ? C.ink : C.border);

    const ImU32 c = held ? C.on_ink : (hovered ? C.text : C.text_dim);
    draw_icon(dl, icon, ImVec2(pos.x + pad, pos.y + (h - glyph) * 0.5f), glyph, c);
    draw_caps(dl, font, fs, ImVec2(pos.x + pad + glyph + px(6.0f), pos.y + (h - fs) * 0.5f), c,
              text);

    if (tip && hovered) tooltip(tip);
    return clicked;
}

void badge(const char* text, ImU32 color) {
    ImFont* font = F.ui_micro ? F.ui_micro : F.ui_small;
    if (!font) return;
    const float fs = font->FontSize;
    const float padx = px(7.0f), pady = px(3.0f);
    const float tw = caps_width(font, fs, text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(tw + padx * 2, fs + pady * 2);

    // Outlined, never filled: a colored block would be the loudest thing on the
    // page, and the colour here is only meant to name a kind.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), C.border);
    draw_caps(dl, font, fs, ImVec2(pos.x + padx, pos.y + pady), color, text);
    ImGui::Dummy(size);
}

void panel_header(const char* label, Icon icon) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = F.ui_micro ? F.ui_micro : F.ui_small;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float fs = font ? font->FontSize : ImGui::GetFontSize();
    const float glyph = std::floor(fs * 1.2f);

    draw_icon(dl, icon, ImVec2(pos.x, pos.y + (fs - glyph) * 0.5f), glyph, C.text_dim);
    draw_caps(dl, font, fs, ImVec2(pos.x + glyph + px(8.0f), pos.y), C.text, label);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, fs));
}

bool tree_row(const char* id, Icon icon, const char* label, bool selected, int depth,
              bool* toggled, bool has_children, bool expanded, ImU32 icon_color,
              lang::Id file_lang) {
    const float h = ImGui::GetTextLineHeight() + px(7.0f);
    const float indent = px(10.0f) + depth * px(13.0f);
    const float avail = ImGui::GetContentRegionAvail().x;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(ImMax(avail, px(40.0f)), h));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(pos.x + avail, pos.y + h);
    if (selected) {
        dl->AddRectFilled(pos, p1, C.accent_soft);
        // The selected row is marked the way the reference marks a chosen
        // filter: a small filled square set in the margin, not a coloured bar.
        const float m = px(3.0f);
        const float my = pos.y + (h - m) * 0.5f;
        dl->AddRectFilled(ImVec2(pos.x + px(3.0f), my), ImVec2(pos.x + px(3.0f) + m, my + m), C.ink);
    } else if (hovered) {
        dl->AddRectFilled(pos, p1, C.bg_surface_hi);
    }

    float x = pos.x + indent;
    float glyph = px(14.0f);
    float gy = pos.y + (h - glyph) * 0.5f;

    if (has_children) {
        ImVec2 chev(x, gy);
        draw_icon(dl, expanded ? Icon::ChevronDown : Icon::ChevronRight, chev, glyph,
                  hovered || selected ? C.text : C.text_faint);
        x += glyph + px(3.0f);
    } else {
        x += px(4.0f);
    }

    if (file_lang != lang::Id::Count) {
        draw_lang_icon(dl, file_lang, ImVec2(x, gy), glyph);
    } else {
        ImU32 ic = icon_color ? icon_color : (selected ? C.ink : C.text_dim);
        draw_icon(dl, icon, ImVec2(x, gy), glyph, ic);
    }
    x += glyph + px(8.0f);

    ImU32 tc = selected ? C.text : (hovered ? C.text : C.text_dim);
    ImVec4 clip(pos.x, pos.y, pos.x + avail - px(6.0f), p1.y);
    dl->AddText(nullptr, 0.0f, ImVec2(x, pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f), tc,
                label, nullptr, 0.0f, &clip);

    if (toggled) *toggled = false;
    return clicked;
}

void typing_indicator(float time) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const float s = px(4.0f);
    const float h = ImGui::GetTextLineHeight();
    // Three squares, not dots: the same mark the selected filter uses, pulsing.
    for (int i = 0; i < 3; ++i) {
        const float phase = time * 3.0f - (float)i * 0.55f;
        const float on = 0.25f + 0.75f * ImMax(0.0f, std::sin(phase));
        const ImU32 c = (C.ink & 0x00FFFFFF) | ((ImU32)(on * 255.0f) << IM_COL32_A_SHIFT);
        const float x = pos.x + (float)i * s * 2.2f;
        const float y = pos.y + (h - s) * 0.5f;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + s, y + s), c);
    }
    ImGui::Dummy(ImVec2(s * 7.0f, h));
}

TabCell numbered_tab(const char* id, int index, const char* label, bool active, bool modified,
                     lang::Id file_lang, bool closable) {
    TabCell out;
    ImFont* micro = F.ui_micro ? F.ui_micro : F.ui_small;
    ImFont* name_font = F.ui_small ? F.ui_small : F.ui;
    if (!micro || !name_font) return out;

    const float mfs = micro->FontSize;
    const float nfs = name_font->FontSize;
    const float pad = px(12.0f);
    const float gap = px(4.0f);

    // Two lines, as on the reference's rail: the index (and the language, which
    // is this editor's equivalent of its little category tag) sits above the
    // name in micro type, the name below it in tracked uppercase.
    const char* idx = index_label(index);
    char top[64];
    if (file_lang != lang::Id::Count)
        std::snprintf(top, sizeof(top), "%s  %s", idx, lang::badge(file_lang));
    else
        std::snprintf(top, sizeof(top), "%s", idx);

    const float top_w = caps_width(micro, mfs, top);
    const float name_w = caps_width(name_font, nfs, label);
    const float body = ImMax(top_w, name_w) + (modified ? px(10.0f) : 0.0f);
    const float w = std::floor(pad * 2.0f + ImMax(px(56.0f), body));
    const float h = std::floor(mfs + gap + nfs + px(20.0f));

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 hi(pos.x + w, pos.y + h);

    ImGui::SetNextItemAllowOverlap();
    ImGui::PushID(id);
    out.clicked = ImGui::InvisibleButton("##cell", ImVec2(w, h));
    // The close button is submitted over this one, so once the pointer is on it
    // the cell itself stops reporting hover — testing the rect as well keeps the
    // × from blinking out from under the pointer on an inactive cell.
    out.hovered = ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(pos, hi);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) dl->AddRectFilled(pos, hi, C.ink);
    else if (out.hovered) dl->AddRectFilled(pos, hi, C.bg_surface_hi);
    dl->AddRect(pos, hi, active ? C.ink : C.border);

    const ImU32 idx_col = active ? rgb(0x8E, 0x8E, 0x8A) : C.text_faint;
    const ImU32 name_col = active ? C.on_ink : (out.hovered ? C.text : C.text_dim);

    const float ty = pos.y + px(9.0f);
    draw_caps(dl, micro, mfs, ImVec2(pos.x + pad, ty), idx_col, top);
    draw_caps(dl, name_font, nfs, ImVec2(pos.x + pad, ty + mfs + gap), name_col, label);

    if (modified) {
        // An unsaved file is marked with a small square in the ochre the data
        // marks use, at the end of the name.
        const float m = px(4.0f);
        const float mx = pos.x + pad + name_w + px(5.0f);
        const float my = ty + mfs + gap + (nfs - m) * 0.5f;
        dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + m, my + m), C.warning);
    }

    if (closable && (out.hovered || active)) {
        const float box = px(15.0f);
        ImGui::SetCursorScreenPos(ImVec2(hi.x - box - px(4.0f), pos.y + px(4.0f)));
        const bool close_hot = ImGui::InvisibleButton("##x", ImVec2(box, box));
        const bool over = ImGui::IsItemHovered();
        out.close_clicked = close_hot;
        if (over) out.clicked = false;
        const float glyph = std::floor(box * 0.5f);
        draw_icon(dl, Icon::Close,
                  ImVec2(hi.x - box - px(4.0f) + (box - glyph) * 0.5f,
                         pos.y + px(4.0f) + (box - glyph) * 0.5f),
                  glyph, over ? (active ? C.on_ink : C.error) : (active ? rgb(0x8E, 0x8E, 0x8A)
                                                                       : C.text_faint));
    }
    ImGui::PopID();

    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(w, h));
    out.width = w;
    return out;
}

// ------------------------------------------------- window-manager furniture

float segment(ImDrawList* dl, float x, float top, float height, const char* text, ImU32 bg,
              ImU32 fg) {
    ImFont* font = F.ui_micro ? F.ui_micro : F.ui_small;
    if (!font) return x;
    const float fs = font->FontSize;
    const float pad = px(12.0f);
    const float body = caps_width(font, fs, text) + pad * 2.0f;

    if (bg >> IM_COL32_A_SHIFT) dl->AddRectFilled(ImVec2(x, top), ImVec2(x + body, top + height), bg);
    draw_caps(dl, font, fs, ImVec2(x + pad, top + (height - fs) * 0.5f), fg, text);
    // Cells are closed by a hairline rather than a wedge; the rule is the only
    // divider anywhere in this interface.
    dl->AddLine(ImVec2(std::floor(x + body) + 0.5f, top + px(4.0f)),
                ImVec2(std::floor(x + body) + 0.5f, top + height - px(4.0f)), C.border, 1.0f);
    return x + body;
}

float segment_r(ImDrawList* dl, float right, float top, float height, const char* text, ImU32 bg,
                ImU32 fg) {
    ImFont* font = F.ui_micro ? F.ui_micro : F.ui_small;
    if (!font) return right;
    const float fs = font->FontSize;
    const float pad = px(12.0f);
    const float body = caps_width(font, fs, text) + pad * 2.0f;
    const float left = right - body;

    if (bg >> IM_COL32_A_SHIFT) dl->AddRectFilled(ImVec2(left, top), ImVec2(right, top + height), bg);
    draw_caps(dl, font, fs, ImVec2(left + pad, top + (height - fs) * 0.5f), fg, text);
    dl->AddLine(ImVec2(std::floor(left) + 0.5f, top + px(4.0f)),
                ImVec2(std::floor(left) + 0.5f, top + height - px(4.0f)), C.border, 1.0f);
    return left;
}

void pane_frame() {
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 lo = ImGui::GetWindowPos();
    const ImVec2 hi = ImVec2(lo.x + ImGui::GetWindowSize().x, lo.y + ImGui::GetWindowSize().y);

    // A window's clip rect stops at its content, so a border drawn on the window
    // bounds is clipped away entirely. Widen the clip first, then inset by half
    // a pixel so the stroke lands on the frame instead of straddling it.
    // The frame stays a hairline whether or not the pane has focus — focus is
    // shown by the line going to ink, not by the line getting fatter.
    dl->PushClipRect(lo, hi, false);
    dl->AddRect(ImVec2(lo.x + 0.5f, lo.y + 0.5f), ImVec2(hi.x - 0.5f, hi.y - 0.5f),
                focused ? C.ink : C.border, 0.0f, 0, 1.0f);
    dl->PopClipRect();
}

float pane_title(int index, const char* name, const char* detail) {
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = F.ui_micro ? F.ui_micro : F.ui_small;
    if (!font) return 0.0f;

    const float fs = font->FontSize;
    const float h = std::floor(fs + px(18.0f));

    // The strip spans the pane edge to edge, so it is drawn against the window
    // rect rather than the padded content region; the cursor is then moved
    // below it by hand.
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float ww = ImGui::GetWindowSize().x;
    const ImVec2 lo(wpos.x + 1.0f, wpos.y + 1.0f);
    const ImVec2 hi(wpos.x + ww - 1.0f, lo.y + h);
    const float ty = std::floor(lo.y + (h - fs) * 0.5f);

    dl->AddRectFilled(lo, hi, C.bg_titlebar);
    dl->AddLine(ImVec2(lo.x, hi.y - 0.5f), ImVec2(hi.x, hi.y - 0.5f), C.border, 1.0f);

    float x = lo.x + px(12.0f);
    if (focused) {
        // The focused pane carries the same filled square that marks the chosen
        // item everywhere else.
        const float m = px(4.0f);
        dl->AddRectFilled(ImVec2(x, ty + (fs - m) * 0.5f), ImVec2(x + m, ty + (fs + m) * 0.5f),
                          C.ink);
        x += m + px(7.0f);
    } else {
        x += px(4.0f) + px(7.0f);
    }

    x += draw_caps(dl, font, fs, ImVec2(x, ty), focused ? C.text_dim : C.text_faint,
                   index_label(index)) +
         px(10.0f);
    const float name_w = draw_caps(dl, font, fs, ImVec2(x, ty), focused ? C.text : C.text_dim, name);

    if (detail && *detail) {
        // Paths keep their own case: they are values, not labels.
        const float dw = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, detail).x;
        const float dx = hi.x - dw - px(12.0f);
        if (dx > x + name_w + px(14.0f))
            dl->AddText(font, fs, ImVec2(dx, ty), C.text_faint, detail);
    }

    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + pad.x, hi.y + pad.y));
    return h;
}

void tooltip(const char* text) {
    ImGui::PushFont(F.ui_small);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(9.0f), px(6.0f)));
    ImGui::SetTooltip("%s", text);
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

void vspace(float h) { ImGui::Dummy(ImVec2(0.0f, px(h))); }

// -------------------------------------------------------- language badges

namespace {

// Perceptual luminance, used to decide whether the badge text on a colored
// chip should be black or white.
float luminance(ImU32 c) {
    const float r = (float)((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
    const float g = (float)((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
    const float b = (float)((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

} // namespace

void draw_lang_icon(ImDrawList* dl, lang::Id id, ImVec2 pos, float size) {
    const char* text = lang::badge(id);
    const ImU32 base = lang::color(id);

    // Plain text and other non-code files get a quiet outlined document mark
    // instead of a colored chip, so real source files stand out in the tree.
    if (!lang::is_code(id) && (id == lang::Id::PlainText || id == lang::Id::Text)) {
        draw_icon(dl, Icon::File, pos, size, C.text_faint);
        return;
    }

    // The mark is a hairline square with the language's short code set in ink,
    // and the language's own colour survives only as a rule along the bottom
    // edge. That keeps the tree monochrome at a glance while a file's kind is
    // still identifiable by colour when you look for it.
    const ImVec2 a = pos;
    const ImVec2 b = ImVec2(pos.x + size, pos.y + size);
    dl->AddRect(a, b, C.border_strong);
    const float bar = ImMax(1.0f, std::floor(size * 0.14f));
    dl->AddRectFilled(ImVec2(a.x + 1.0f, b.y - bar), ImVec2(b.x - 1.0f, b.y - 1.0f), base);

    ImFont* font = F.mono_bold ? F.mono_bold : F.mono;
    if (!font) return;

    const int len = (int)strlen(text);
    if (len == 0) return;

    // Shrink the label until it fits the chip with a little breathing room.
    const float avail = size * 0.78f;
    float fsize = size * 0.52f;
    ImVec2 measured = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, text);
    if (measured.x > avail) {
        fsize *= avail / measured.x;
        fsize = ImMax(fsize, size * 0.24f);
        measured = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, text);
    }

    // Very dark language colours would vanish against the rule below them, so
    // the code itself is always ink.
    const ImU32 mark = luminance(base) < 0.06f ? C.text_dim : C.text;
    const ImVec2 at(pos.x + (size - measured.x) * 0.5f,
                    pos.y + (size - measured.y - bar) * 0.5f);
    dl->AddText(font, fsize, at, mark, text);
}

void lang_chip(lang::Id id, float size) {
    const float box = size > 0.0f ? size : ImGui::GetFontSize() * 1.05f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    draw_lang_icon(ImGui::GetWindowDrawList(), id, pos, box);
    ImGui::Dummy(ImVec2(box, box));
    if (ImGui::IsItemHovered()) tooltip(lang::name(id));
}

} // namespace ui
