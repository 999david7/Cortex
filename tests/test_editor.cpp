// Editor tests: drive the real TextEditor through ImGui frames with simulated
// keys and mouse, then check the text and the carets. No window or GPU is
// involved, so they run wherever the compiler does:
//
//   make test
//
// Each check prints the expression, what it got and what it expected.
#include <cfloat>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "texteditor.h"
#include "ui.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        ++g_checks;                                                                        \
        auto _va = (a);                                                                    \
        auto _vb = (b);                                                                    \
        if (!(_va == _vb)) {                                                               \
            ++g_failures;                                                                  \
            std::printf("FAIL %s:%d\n  %s\n  got:      [%s]\n  expected: [%s]\n", __FILE__, \
                        __LINE__, #a, show(_va).c_str(), show(_vb).c_str());               \
        }                                                                                  \
    } while (0)

static std::string show(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}
static std::string show(int v) { return std::to_string(v); }
static std::string show(bool v) { return v ? "true" : "false"; }
static std::string show(ed::Coord c) { return std::to_string(c.line) + ":" + std::to_string(c.column); }

static ed::TextEditor* E = nullptr;

static void frame(float dt = 1.0f / 60.0f) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1200, 800);
    io.DeltaTime = dt;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1200, 800));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("host", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::PopStyleVar();
    E->render("##code", ImVec2(1000, 700));
    ImGui::End();
    ImGui::Render();
}

static void press(ImGuiKey key, bool ctrl = false, bool shift = false, bool alt = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(ImGuiMod_Alt, alt);
    io.AddKeyEvent(key, true);
    frame();
    io.AddKeyEvent(key, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    frame();
}

static void type(const char* s) {
    for (const char* p = s; *p; ++p) {
        if (*p == '\n') { press(ImGuiKey_Enter); continue; }
        ImGui::GetIO().AddInputCharacter((unsigned)*p);
        frame();
    }
}

static void setup(const std::string& text, ed::Language lang = ed::Language::Cpp) {
    static ed::TextEditor* editor = nullptr;
    delete editor;
    editor = new ed::TextEditor();
    E = editor;
    E->set_text(text);
    E->set_language(lang);
    E->auto_close_brackets = true;
    E->show_minimap = true;
    E->request_focus();
    frame();
    frame();
}

// Text origin of the editor in this harness: no host padding, so the child is
// at (0,0); the gutter is digits + 26 px wide, then 10 px of padding.
static ImVec2 point_at(int line, int col) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", E->line_count());
    ImFont* f = ui::F.mono;
    const float digits = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, buf).x;
    const float cw = f->GetCharAdvance(' ');
    const float lh = std::floor(f->FontSize * 1.55f);
    const float gutter = std::floor(digits + 26.0f);
    return ImVec2(gutter + 10.0f + col * cw + 1.0f, 8.0f + line * lh + lh * 0.5f);
}

static void click(int line, int col, bool shift = false, bool alt = false, int times = 1) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 p = point_at(line, col);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(ImGuiMod_Alt, alt);
    io.AddMousePosEvent(p.x, p.y);
    frame(0.6f);
    for (int i = 0; i < times; ++i) {
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
    }
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    frame();
}

static void drag(int l0, int c0, int l1, int c1, bool shift = false, bool alt = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(ImGuiMod_Alt, alt);
    ImVec2 p = point_at(l0, c0);
    io.AddMousePosEvent(p.x, p.y);
    frame(0.6f);
    io.AddMouseButtonEvent(0, true);
    frame();
    ImVec2 q = point_at(l1, c1);
    for (int i = 1; i <= 4; ++i) {
        io.AddMousePosEvent(p.x + (q.x - p.x) * i / 4.0f, p.y + (q.y - p.y) * i / 4.0f);
        frame();
    }
    io.AddMouseButtonEvent(0, false);
    frame();
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    frame();
}

static std::string carets_str() {
    std::string s;
    for (const ed::Caret& c : E->carets()) {
        if (!s.empty()) s += " ";
        s += show(c.anchor);
        if (c.has_selection()) s += "-" + show(c.pos);
    }
    return s;
}

int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // The app runs with keyboard navigation on; so does the harness, or keys
    // ImGui's navigation takes would pass here and fail there.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ui::apply_style(1.0f);
    ui::load_fonts(1.0f);
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    // ---- typing, undo coalescing ----
    setup("");
    type("hello");
    CHECK_EQ(E->text(), std::string("hello"));
    press(ImGuiKey_Z, true);
    CHECK_EQ(E->text(), std::string(""));
    press(ImGuiKey_Y, true);
    CHECK_EQ(E->text(), std::string("hello"));
    CHECK_EQ(E->cursor(), ed::Coord(0, 5));

    // ---- auto-close and wrap ----
    setup("x");
    press(ImGuiKey_End);
    type("(");
    CHECK_EQ(E->text(), std::string("x()"));
    type("a)");
    CHECK_EQ(E->text(), std::string("x(a)"));
    setup("word");
    press(ImGuiKey_A, true);
    type("[");
    CHECK_EQ(E->text(), std::string("[word]"));
    CHECK_EQ(carets_str(), std::string("0:1-0:5"));
    // backspace inside an empty pair removes both
    setup("");
    type("(");
    press(ImGuiKey_Backspace);
    CHECK_EQ(E->text(), std::string(""));

    // ---- Ctrl+D: add next occurrence ----
    setup("foo bar\nfoo\nfoobar foo");
    press(ImGuiKey_D, true); // selects the word
    CHECK_EQ(carets_str(), std::string("0:0-0:3"));
    press(ImGuiKey_D, true);
    press(ImGuiKey_D, true);
    // whole-word: "foobar" is skipped
    CHECK_EQ(carets_str(), std::string("0:0-0:3 1:0-1:3 2:7-2:10"));
    type("baz");
    CHECK_EQ(E->text(), std::string("baz bar\nbaz\nfoobar baz"));
    press(ImGuiKey_Z, true);
    CHECK_EQ(E->text(), std::string("foo bar\nfoo\nfoobar foo"));
    CHECK_EQ(carets_str(), std::string("0:0-0:3 1:0-1:3 2:7-2:10"));
    press(ImGuiKey_Escape);
    CHECK_EQ(E->caret_count(), 1);

    // ---- Ctrl+U cursor undo ----
    setup("a a a");
    press(ImGuiKey_D, true);
    press(ImGuiKey_D, true);
    press(ImGuiKey_D, true);
    CHECK_EQ(E->caret_count(), 3);
    press(ImGuiKey_U, true);
    CHECK_EQ(E->caret_count(), 2);

    // ---- Ctrl+Shift+L: all occurrences ----
    setup("x = x + x1 + x;");
    press(ImGuiKey_L, true, true);
    CHECK_EQ(E->caret_count(), 3); // whole words only: x1 is not an x
    type("y");
    CHECK_EQ(E->text(), std::string("y = y + x1 + y;"));

    // ---- Ctrl+Alt+Down: column of carets ----
    setup("abc\nabc\nabc\nab");
    press(ImGuiKey_RightArrow);
    press(ImGuiKey_DownArrow, true, false, true);
    press(ImGuiKey_DownArrow, true, false, true);
    CHECK_EQ(E->caret_count(), 3);
    type("X");
    CHECK_EQ(E->text(), std::string("aXbc\naXbc\naXbc\nab"));
    press(ImGuiKey_Backspace);
    CHECK_EQ(E->text(), std::string("abc\nabc\nabc\nab"));
    press(ImGuiKey_End);
    type(";");
    CHECK_EQ(E->text(), std::string("abc;\nabc;\nabc;\nab"));
    press(ImGuiKey_Enter);
    CHECK_EQ(E->text(), std::string("abc;\n\nabc;\n\nabc;\n\nab"));
    press(ImGuiKey_Z, true);
    CHECK_EQ(E->text(), std::string("abc;\nabc;\nabc;\nab"));

    // ---- multi-caret paste: one line per caret ----
    setup("a\nb\nc");
    press(ImGuiKey_DownArrow, true, false, true);
    press(ImGuiKey_DownArrow, true, false, true);
    press(ImGuiKey_End);
    ImGui::SetClipboardText("1\n2\n3");
    press(ImGuiKey_V, true);
    CHECK_EQ(E->text(), std::string("a1\nb2\nc3"));

    // ---- copy without selection copies the line, paste puts it above ----
    setup("one\ntwo");
    press(ImGuiKey_DownArrow);
    press(ImGuiKey_C, true);
    press(ImGuiKey_UpArrow);
    press(ImGuiKey_V, true);
    CHECK_EQ(E->text(), std::string("two\none\ntwo"));

    // ---- move / copy / delete lines ----
    setup("1\n2\n3\n4");
    press(ImGuiKey_DownArrow, false, false, true); // Alt+Down
    CHECK_EQ(E->text(), std::string("2\n1\n3\n4"));
    CHECK_EQ(E->cursor(), ed::Coord(1, 0));
    press(ImGuiKey_DownArrow, false, true, true); // Shift+Alt+Down copies
    CHECK_EQ(E->text(), std::string("2\n1\n1\n3\n4"));
    CHECK_EQ(E->cursor(), ed::Coord(2, 0));
    press(ImGuiKey_K, true, true);
    CHECK_EQ(E->text(), std::string("2\n1\n3\n4"));
    // two carets, delete both lines
    setup("a\nb\nc\nd");
    press(ImGuiKey_DownArrow, true, false, true);
    press(ImGuiKey_DownArrow);
    press(ImGuiKey_DownArrow); // carets on lines 2 and 3 now
    press(ImGuiKey_K, true, true);
    CHECK_EQ(E->text(), std::string("a\nb"));
    // move a selected block up
    setup("a\nb\nc\nd");
    press(ImGuiKey_DownArrow);
    press(ImGuiKey_DownArrow, false, true);
    press(ImGuiKey_End, false, true);
    press(ImGuiKey_UpArrow, false, false, true);
    CHECK_EQ(E->text(), std::string("b\nc\na\nd"));
    CHECK_EQ(carets_str(), std::string("0:0-1:1"));
    press(ImGuiKey_Z, true);
    CHECK_EQ(E->text(), std::string("a\nb\nc\nd"));

    // ---- insert line below / above ----
    setup("    foo\nbar");
    press(ImGuiKey_Enter, true);
    type("x");
    CHECK_EQ(E->text(), std::string("    foo\n    x\nbar"));
    press(ImGuiKey_Enter, true, true);
    type("y");
    CHECK_EQ(E->text(), std::string("    foo\n    y\n    x\nbar"));

    // ---- comment toggling across carets ----
    setup("int a;\n  int b;\nint c;");
    press(ImGuiKey_A, true);
    press(ImGuiKey_Slash, true);
    CHECK_EQ(E->text(), std::string("// int a;\n//   int b;\n// int c;"));
    press(ImGuiKey_Slash, true);
    CHECK_EQ(E->text(), std::string("int a;\n  int b;\nint c;"));
    setup("    a;\n    b;");
    press(ImGuiKey_A, true);
    press(ImGuiKey_Slash, true);
    CHECK_EQ(E->text(), std::string("    // a;\n    // b;"));
    // block comment
    setup("x + y");
    press(ImGuiKey_A, true);
    press(ImGuiKey_A, false, true, true);
    CHECK_EQ(E->text(), std::string("/* x + y */"));
    press(ImGuiKey_A, false, true, true);
    CHECK_EQ(E->text(), std::string("x + y"));

    // ---- indent / outdent ----
    setup("a\nb\n\nc");
    press(ImGuiKey_A, true);
    press(ImGuiKey_Tab);
    CHECK_EQ(E->text(), std::string("    a\n    b\n\n    c"));
    CHECK_EQ(carets_str(), std::string("0:0-3:5"));
    press(ImGuiKey_Tab, false, true);
    CHECK_EQ(E->text(), std::string("a\nb\n\nc"));
    press(ImGuiKey_RightBracket, true);
    CHECK_EQ(E->text(), std::string("    a\n    b\n\n    c"));
    press(ImGuiKey_LeftBracket, true);
    CHECK_EQ(E->text(), std::string("a\nb\n\nc"));

    // ---- delete word ----
    setup("hello world");
    press(ImGuiKey_End);
    press(ImGuiKey_Backspace, true);
    CHECK_EQ(E->text(), std::string("hello "));

    // ---- expand / shrink selection ----
    setup("f(a, [bb, c]);");
    for (int i = 0; i < 7; ++i) press(ImGuiKey_RightArrow);
    press(ImGuiKey_RightArrow, false, true, true);
    CHECK_EQ(carets_str(), std::string("0:6-0:8")); // bb
    press(ImGuiKey_RightArrow, false, true, true);
    CHECK_EQ(carets_str(), std::string("0:6-0:11")); // bb, c
    press(ImGuiKey_RightArrow, false, true, true);
    CHECK_EQ(carets_str(), std::string("0:5-0:12")); // [bb, c]
    press(ImGuiKey_RightArrow, false, true, true);
    CHECK_EQ(carets_str(), std::string("0:2-0:12")); // a, [bb, c]
    press(ImGuiKey_LeftArrow, false, true, true);
    CHECK_EQ(carets_str(), std::string("0:5-0:12"));
    press(ImGuiKey_LeftArrow, false, true, true);
    CHECK_EQ(carets_str(), std::string("0:6-0:11"));

    // ---- brackets: strings and comments do not count ----
    setup("{ \"}\" // }\n  x;\n}");
    press(ImGuiKey_Backslash, true, true);
    CHECK_EQ(E->cursor(), ed::Coord(2, 0));
    press(ImGuiKey_Backslash, true, true);
    CHECK_EQ(E->cursor(), ed::Coord(0, 0));

    // ---- Shift+Alt+I ----
    setup("aa\nbbb\nc");
    press(ImGuiKey_A, true);
    press(ImGuiKey_I, false, true, true);
    CHECK_EQ(carets_str(), std::string("0:2 1:3 2:1"));
    type("!");
    CHECK_EQ(E->text(), std::string("aa!\nbbb!\nc!"));

    // ---- transforms (driven through the public API) ----
    setup("helloWorld HTTPServer");
    E->select_all();
    E->transform_case(ed::CaseKind::Snake);
    CHECK_EQ(E->text(), std::string("hello_world http_server"));
    E->transform_case(ed::CaseKind::Pascal);
    CHECK_EQ(E->text(), std::string("HelloWorld HttpServer"));
    E->transform_case(ed::CaseKind::Camel);
    CHECK_EQ(E->text(), std::string("helloWorld httpServer"));
    E->transform_case(ed::CaseKind::Kebab);
    CHECK_EQ(E->text(), std::string("hello-world http-server"));
    E->transform_case(ed::CaseKind::Upper);
    CHECK_EQ(E->text(), std::string("HELLO-WORLD HTTP-SERVER"));
    E->transform_case(ed::CaseKind::Title);
    CHECK_EQ(E->text(), std::string("Hello-World Http-Server"));
    setup("someName");
    E->transform_case(ed::CaseKind::Upper); // the word under the caret
    CHECK_EQ(E->text(), std::string("SOMENAME"));

    setup("pear\nApple\nbanana\napple");
    E->sort_lines(false);
    CHECK_EQ(E->text(), std::string("Apple\napple\nbanana\npear"));
    E->sort_lines(true);
    CHECK_EQ(E->text(), std::string("pear\nbanana\napple\nApple"));
    setup("a\nb\na\nc\nb");
    E->delete_duplicate_lines();
    CHECK_EQ(E->text(), std::string("a\nb\nc"));
    E->reverse_lines();
    CHECK_EQ(E->text(), std::string("c\nb\na"));
    setup("a  \n b\t\nc");
    E->trim_trailing_whitespace();
    CHECK_EQ(E->text(), std::string("a\n b\nc"));
    setup("        x\n    y");
    E->convert_indentation(true);
    CHECK_EQ(E->text(), std::string("\t\tx\n\ty"));
    E->convert_indentation(false);
    CHECK_EQ(E->text(), std::string("        x\n    y"));
    setup("a\n   b\nc");
    E->join_lines();
    CHECK_EQ(E->text(), std::string("a b\nc"));
    setup("ab");
    press(ImGuiKey_End);
    E->transpose_chars();
    CHECK_EQ(E->text(), std::string("ba"));
    setup("x");
    press(ImGuiKey_A, true);
    E->duplicate_selection();
    CHECK_EQ(E->text(), std::string("xx"));

    // ---- find / replace ----
    setup("a@1 b@2 c@3");
    E->find_regex = true;
    E->set_find_text("(\\w)@(\\d)");
    E->set_replace_text("$2-$1");
    CHECK_EQ(E->match_count(), 3);
    CHECK_EQ(E->replace_all(), 3);
    CHECK_EQ(E->text(), std::string("1-a 2-b 3-c"));
    press(ImGuiKey_Z, true);
    CHECK_EQ(E->text(), std::string("a@1 b@2 c@3"));
    setup("cat Cat catalog cat");
    E->find_regex = false;
    E->find_whole_word = true;
    E->find_case_sensitive = false;
    E->set_find_text("cat");
    CHECK_EQ(E->match_count(), 3);
    E->find_case_sensitive = true;
    CHECK_EQ(E->match_count(), 2);
    E->set_replace_text("dog");
    E->find_next();
    CHECK_EQ(carets_str(), std::string("0:0-0:3"));
    E->replace_one();
    CHECK_EQ(E->text(), std::string("dog Cat catalog cat"));
    CHECK_EQ(carets_str(), std::string("0:16-0:19"));
    E->find_previous();
    CHECK_EQ(carets_str(), std::string("0:16-0:19"));
    setup("bad");
    E->find_regex = true;
    E->set_find_text("(");
    CHECK_EQ(E->match_count(), 0);

    // ---- mouse: click, shift-click, double / triple click, alt-click, box ----
    setup("alpha beta\ngamma delta\nepsilon\nzeta eta");
    click(1, 2);
    CHECK_EQ(carets_str(), std::string("1:2"));
    click(2, 3, true);
    CHECK_EQ(carets_str(), std::string("1:2-2:3"));
    click(0, 7, false, false, 2);
    CHECK_EQ(carets_str(), std::string("0:6-0:10"));
    click(1, 1, false, false, 3);
    CHECK_EQ(carets_str(), std::string("1:0-2:0"));
    click(0, 0);
    click(2, 0, false, true);
    click(3, 0, false, true);
    CHECK_EQ(E->caret_count(), 3);
    type(">");
    CHECK_EQ(E->text(), std::string(">alpha beta\ngamma delta\n>epsilon\n>zeta eta"));
    click(0, 1, false, true); // alt-click an existing caret removes it
    CHECK_EQ(E->caret_count(), 2);
    setup("abcdef\nabcdef\nab\nabcdef");
    click(0, 1);
    drag(0, 1, 3, 4, true, true); // shift+alt drag = column selection
    CHECK_EQ(carets_str(), std::string("0:1-0:4 1:1-1:4 2:1-2:2 3:1-3:4"));
    type("-");
    CHECK_EQ(E->text(), std::string("a-ef\na-ef\na-\na-ef"));
    // drag-select
    setup("hello world");
    drag(0, 0, 0, 5);
    CHECK_EQ(carets_str(), std::string("0:0-0:5"));

    // ---- overtype ----
    setup("abc");
    press(ImGuiKey_Insert);
    type("XY");
    CHECK_EQ(E->text(), std::string("XYc"));
    press(ImGuiKey_Insert);
    type("Z");
    CHECK_EQ(E->text(), std::string("XYZc"));

    // ---- cursor styles render without trouble ----
    setup("abc\ndef");
    for (int s = 0; s < (int)ed::CursorStyle::Count; ++s) {
        E->cursor_style = (ed::CursorStyle)s;
        for (int b = 0; b < (int)ed::CursorBlink::Count; ++b) {
            E->cursor_blink = (ed::CursorBlink)b;
            E->smooth_caret = b % 2 == 0;
            frame();
        }
    }
    for (int wsp = 0; wsp < (int)ed::Whitespace::Count; ++wsp) {
        E->render_whitespace = (ed::Whitespace)wsp;
        E->bracket_pair_colors = true;
        E->rulers = {2, 80};
        frame();
    }

    // ---- find bar: typing goes into it, Enter jumps, Escape hands focus back ----
    setup("one two\nthree two\ntwo");
    press(ImGuiKey_F, true);
    frame();
    // the word at the caret seeds the find field
    CHECK_EQ(E->find_text(), std::string("one"));
    press(ImGuiKey_A, true); // select all inside the field
    type("two");
    frame();
    CHECK_EQ(E->find_text(), std::string("two"));
    CHECK_EQ(E->text(), std::string("one two\nthree two\ntwo")); // the editor did not get the keys
    press(ImGuiKey_Enter);
    CHECK_EQ(carets_str(), std::string("0:4-0:7"));
    press(ImGuiKey_Enter);
    CHECK_EQ(carets_str(), std::string("1:6-1:9"));
    press(ImGuiKey_Enter, false, true);
    CHECK_EQ(carets_str(), std::string("0:4-0:7"));
    press(ImGuiKey_Escape);
    frame();
    CHECK_EQ(E->find_open(), false);
    type("2");
    CHECK_EQ(E->text(), std::string("one 2\nthree two\ntwo"));
    // Ctrl+H opens the replace row; replace through the API
    press(ImGuiKey_H, true);
    frame();
    E->set_find_text("two");
    E->set_replace_text("2");
    CHECK_EQ(E->replace_all(), 2);
    CHECK_EQ(E->text(), std::string("one 2\nthree 2\n2"));
    E->request_focus();
    frame();
    press(ImGuiKey_Escape);

    // ---- gutter click selects the line, drag selects more ----
    setup("a\nb\nc\nd");
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 p = point_at(1, 0);
        io.AddMousePosEvent(4.0f, p.y);
        frame(0.6f);
        io.AddMouseButtonEvent(0, true);
        frame();
        CHECK_EQ(carets_str(), std::string("1:0-2:0"));
        ImVec2 q = point_at(2, 0);
        io.AddMousePosEvent(4.0f, q.y);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        CHECK_EQ(carets_str(), std::string("1:0-3:0"));
    }

    // ---- minimap: a click scrolls, it does not move the caret ----
    {
        std::string big;
        for (int i = 0; i < 400; ++i) big += "line " + std::to_string(i) + "\n";
        setup(big);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(1000.0f - 20.0f, 650.0f);
        frame(0.6f);
        io.AddMouseButtonEvent(0, true);
        frame();
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        CHECK_EQ(carets_str(), std::string("0:0"));
    }

    // ---- Ctrl+wheel asks for a zoom ----
    {
        setup("x");
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(300.0f, 100.0f);
        frame();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddMouseWheelEvent(0.0f, 1.0f);
        frame();
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
        CHECK_EQ(E->take_zoom_request(), 1);
        CHECK_EQ(E->take_zoom_request(), 0);
    }

    // ---- line endings ----
    setup("a\r\nb");
    CHECK_EQ(E->use_crlf(), true);
    CHECK_EQ(E->modified(), false);
    E->set_use_crlf(false);
    CHECK_EQ(E->modified(), true);
    CHECK_EQ(E->text(), std::string("a\nb"));

    // ---- saved marker survives coalescing ----
    setup("");
    type("ab");
    E->mark_saved();
    CHECK_EQ(E->modified(), false);
    type("c");
    CHECK_EQ(E->modified(), true);
    press(ImGuiKey_Z, true);
    CHECK_EQ(E->modified(), false);
    CHECK_EQ(E->text(), std::string("ab"));

    // ---- stress: thousands of carets, a large file ----
    {
        std::string big;
        for (int i = 0; i < 3000; ++i) big += "item_" + std::to_string(i) + " = value;\n";
        setup(big);
        press(ImGuiKey_A, true);
        press(ImGuiKey_I, false, true, true);
        CHECK_EQ(E->caret_count(), 3000);
        const auto t0 = std::chrono::steady_clock::now();
        type("!");
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("typing one character at 3000 carets: %.1f ms\n", ms);
        CHECK_EQ(E->line(2999), std::string("item_2999 = value;!"));
        press(ImGuiKey_Z, true);
        CHECK_EQ(E->line(0), std::string("item_0 = value;"));
        E->render_whitespace = ed::Whitespace::Selection;
        press(ImGuiKey_A, true);
        frame();
        setup(big);
        for (int i = 0; i < 4; ++i) press(ImGuiKey_RightArrow, false, true); // select "item"
        E->select_all_occurrences();
        CHECK_EQ(E->caret_count(), 3000);
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    ImGui::DestroyContext();
    return g_failures ? 1 : 0;
}
