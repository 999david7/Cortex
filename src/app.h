// The IDE: window layout, editor tabs, file explorer, terminal and AI chat.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "ollama.h"
#include "sys.h"
#include "texteditor.h"
#include "webserver.h"

namespace app {

struct Settings {
    std::string ollama_host = "http://127.0.0.1:11434";
    std::string chat_model;
    std::string completion_model;
    // Deliberately says nothing about fenced code blocks: this text is followed
    // by the tool instructions, and a model told to "wrap code in a fenced
    // block" here will do exactly that instead of writing the file.
    std::string system_prompt =
        "You are a coding assistant embedded in a local IDE, working on the "
        "user's open project. Answer concisely. When something needs to change "
        "in the project, change it in the files rather than printing the code "
        "in the chat.";
    float temperature = 0.7f;
    // A turn that calls tools is following instructions, not writing prose, and
    // the sampling that makes an answer readable is the same sampling that makes
    // a small model wander off the rules it was given. Tool turns get their own,
    // much colder setting.
    float agent_temperature = 0.2f;
    float top_p = 0.9f;
    // Agent turns carry a project map, file contents and tool results, so the
    // window has to be generous. It is also capped rather than left at the
    // model default: some models advertise a 256k context, and asking a local
    // runtime to allocate that much KV cache fails before a token is produced.
    int num_ctx = 8192;
    bool attach_open_file = true;
    bool attach_selection = true;
    int max_context_bytes = 12000;

    // The model may create files and folders inside the open workspace.
    bool enable_file_tools = true;
    // Apply new files and targeted edits without asking. Replacing a whole file
    // that already exists always asks, whatever this is set to.
    bool auto_approve_writes = true;

    int tab_size = 4;
    bool insert_spaces = true;
    bool show_line_numbers = true;
    bool indent_guides = true;
    bool auto_close_brackets = true;
    bool highlight_current_line = true;

    // The caret and the editor surface. The names in brackets are the VS Code
    // settings each one mirrors.
    int cursor_style = 0;          // ed::CursorStyle  (editor.cursorStyle)
    int cursor_blinking = 0;       // ed::CursorBlink  (editor.cursorBlinking)
    int cursor_width = 2;          // line caret width (editor.cursorWidth)
    bool smooth_caret = false;     // (editor.cursorSmoothCaretAnimation)
    int overtype_cursor_style = 1; // caret while Insert is toggled (editor.overtypeCursorStyle)
    int render_whitespace = 2;     // ed::Whitespace   (editor.renderWhitespace)
    bool minimap = true;
    bool bracket_pair_colors = false;
    bool match_brackets = true;
    bool highlight_occurrences = true;
    std::string rulers;            // comma-separated columns, e.g. "80, 120"
    float editor_font_size = 14.0f;
    std::vector<int> ruler_columns() const;

    // Live server: the port to try first, whether a page reloads by itself when
    // a file under the root changes, and whether starting it opens the browser.
    int live_port = 5500;
    bool live_reload = true;

    float ui_scale = 0.0f; // 0 = follow system DPI
    std::string last_folder;
    std::string run_command;
    std::vector<std::string> recent_folders;

    bool load();
    bool save() const;
    static std::string path();
};

struct Document {
    std::string path;          // empty for untitled buffers
    std::string display_name;
    ed::TextEditor editor;
    bool wants_close = false;
    bool scroll_to_tab = false;
};

// One file-system operation the model asked for. Writes wait for approval
// unless the user turned auto-apply on; reads run immediately.
struct ToolAction {
    // create_file | edit_file | create_directory | read_file | list_directory |
    // search_files
    std::string tool;
    std::string path;      // as requested, relative to the workspace
    std::string content;   // whole file body for create_file
    std::string find_text; // edit_file: the snippet to replace
    std::string replace_text; // edit_file: what to put in its place
    std::string query;     // search_files: the text to look for
    std::string result;    // text handed back to the model
    int replacements = 0;  // edit_file: how many occurrences were rewritten
    bool read_only = false;
    bool resolved = false; // executed or rejected
    bool ok = false;
    bool rejected = false;
    // The write went through but left something broken behind - a page that
    // links to a file which is not there. Worth seeing at a glance, because the
    // model does not always fix it on the next step.
    bool warned = false;
    bool overwrite = false; // target already exists
    // Target is open in a tab with unsaved edits; writing would strand them.
    bool touches_unsaved = false;
    bool expanded = false;  // content preview open
};

struct ChatMessage {
    std::string role; // "user" | "assistant" | "system"
    std::string content;
    std::string thinking;
    std::string attachment_note; // e.g. "main.cpp (selection)"
    std::string error;
    bool streaming = false;
    bool show_thinking = false;
    int eval_count = 0;
    double tokens_per_second = 0.0;
    std::vector<ToolAction> actions;
    bool actions_reported = false; // results already sent back to the model
    // True when the calls were recovered from the reply text rather than from
    // Ollama's structured field. Shown in the UI; results go back through the
    // tool protocol either way.
    bool actions_from_text = false;
    // False when the reply that produced these actions could not have called a
    // tool at all, because the model has none. The results are still recorded,
    // but there is no point asking the model to carry on.
    bool expect_followup = true;
};

// Callbacks into the Win32 layer for the custom title bar.
struct WindowHooks {
    std::function<void()> minimize;
    std::function<void()> toggle_maximize;
    std::function<void()> close;
    std::function<bool()> is_maximized;
};

class App {
public:
    App();
    ~App();

    void init(void* hwnd, WindowHooks hooks, float dpi_scale);
    void draw();
    void shutdown();

    bool should_exit() const { return should_exit_; }
    // True when the pointer is over an empty part of the title bar, so the
    // Win32 layer can report HTCAPTION and let the OS drag the window.
    bool caption_drag_zone() const { return caption_drag_zone_; }
    float title_bar_height() const { return title_bar_height_; }

    void open_path(const std::string& path);
    void handle_dropped_files(const std::vector<std::string>& paths);

private:
    // --- layout ---
    void draw_title_bar();
    void draw_menus();
    void draw_status_bar();
    void draw_explorer();
    void draw_editor_panel();
    void draw_chat_panel();
    void draw_terminal_panel();
    void draw_settings_modal();
    void draw_editor_settings();
    void draw_shortcuts_modal();
    void draw_about_modal();
    void draw_welcome(const ImVec2& size);
    void draw_toasts();

    // --- documents ---
    Document* active_document();
    void new_document();
    void open_file(const std::string& path);
    void save_document(Document* doc, bool save_as);
    void close_document(int index);
    int find_document(const std::string& path) const;
    void apply_editor_settings(Document* doc);
    void save_all();
    void revert_document(Document* doc);
    // Closes every tab, every tab but the active one, or every saved tab. Tabs
    // with unsaved edits are kept open rather than thrown away.
    enum class CloseMode { All, Others, Saved, ToTheRight };
    void close_documents(CloseMode mode, int pivot);
    void reopen_closed_document();
    void cycle_document(int direction);
    void zoom_editor(int direction); // +1, -1, or 0 to reset
    // Runs `fn` on the active editor and hands the keyboard back to it.
    void editor_command(const std::function<void(ed::TextEditor&)>& fn);

    // --- explorer ---
    struct TreeNode {
        std::string name;
        std::string path;
        bool is_dir = false;
        bool expanded = false;
        bool loaded = false;
        long long size = 0;
        std::vector<TreeNode> children;
    };
    void set_workspace(const std::string& folder);
    void load_children(TreeNode& node);
    void draw_tree_node(TreeNode& node, int depth);
    // Re-reads the tree from disk while keeping folders expanded.
    void refresh_tree();
    void collect_expanded(const TreeNode& node, std::vector<std::string>& out) const;
    void restore_expanded(TreeNode& node, const std::vector<std::string>& paths);

    // --- ai ---
    void refresh_models(bool notify);
    bool model_installed(const std::string& name) const;
    // The model to fall back on: a code-oriented one when there is one.
    std::string best_installed_model() const;
    void send_chat(const std::string& user_text, const std::string& attachment,
                   const std::string& note);
    void stop_chat();
    void update_streams();

    // --- ai file tools ---
    std::vector<ollama::ToolDef> file_tools() const;
    std::string effective_system_prompt(bool tools_available) const;
    bool tools_available() const;
    void start_agent_turn();
    // Keeps the transcript inside the model's context window. Nothing else
    // bounds it, and a turn that builds a whole site sends every file it wrote
    // back with every request.
    void compact_agent_history();
    size_t agent_history_bytes() const;
    void collect_tool_calls(ChatMessage& msg, const std::vector<ollama::ToolCall>& calls);
    // A model that will not call a tool still writes the files - it prints them
    // as fenced code blocks and leaves the user to copy them out. Every block
    // that says which file it is becomes a real write; a block whose target can
    // only be guessed at is labelled instead, so it carries a one-click button.
    // `text` is the reply, and the blocks that were taken on are lifted out of
    // it. Returns true when anything was adopted.
    bool adopt_code_blocks(ChatMessage& msg, std::string& text, bool tools_turn);
    // Fills in what a write needs to know about its target before it can be
    // judged: whether it exists, and whether a tab has unsaved edits on it.
    void prepare_write_action(ToolAction& action);
    // Performs everything in the message that does not need to be approved.
    void apply_ready_actions(ChatMessage& msg);
    // Files the model has been shown during this turn. Windows paths differ in
    // case for the same file, so the set is keyed on a folded copy.
    void mark_read(const std::string& absolute);
    bool was_read(const std::string& absolute) const;
    // True when the user's message asks for the project to change, rather than
    // for something to be explained. Only then is a reply that printed code
    // instead of writing it worth a second attempt.
    static bool asks_for_changes(const std::string& text);
    void execute_action(ToolAction& action);
    // Reflects an agent write into the editor tabs.
    void sync_open_document(const std::string& absolute, const std::string& content);
    void maybe_continue_agent();
    // A compact recursive listing of the workspace, injected into the system
    // prompt so the model knows what exists without having to go looking.
    std::string project_map() const;
    void build_project_map(const std::string& dir, const std::string& prefix, int depth,
                           int& budget, std::string& out) const;
    // Recursive text search used by the search_files tool.
    void search_workspace(const std::string& query, std::string& out) const;
    void search_dir(const std::string& dir, const std::string& query, int& budget,
                    std::string& out) const;
    // Maps a model-supplied path into the workspace, refusing anything outside it.
    bool resolve_in_workspace(const std::string& requested, std::string& absolute,
                              std::string& err) const;
    void draw_tool_action(ToolAction& action, int id, float width);
    // Writes a code block from a reply straight to disk (text-protocol path).
    void write_block_to_file(const std::string& rel_path, const std::string& content);
    // The same, for when no folder is open: asks where the block should go,
    // writes it and opens it, so a reply is never only something to copy.
    void save_block_as(const std::string& suggested_path, const std::string& lang_tag,
                       const std::string& content);
    // Names the files a page links to that are not there. Handed back with the
    // tool result so the model repairs the page in the same turn rather than
    // leaving the user with broken images and dead links.
    std::string missing_reference_note(const std::string& absolute_html,
                                       const std::string& html) const;
    // A page can reference nothing but files that exist and still show a blank
    // screen: framework code a browser cannot run, a root element nothing
    // renders into, "//" used as a comment in CSS. These are the failures that
    // can be recognised without running anything.
    std::string runtime_problem_note(const std::string& absolute,
                                     const std::string& content) const;
    // True when the project actually builds its sources (package.json, a
    // bundler config, node_modules) - then module syntax is not a defect.
    bool project_has_bundler() const;

    // Re-checks every page written in this batch once the batch has finished, so
    // a page is not reported broken because of a file created two calls later.
    void refresh_reference_warnings(ChatMessage& msg);
    // Lists pages written this turn that still point at files which are not
    // there. Empty when the turn left the site in one piece.
    std::string dead_reference_report() const;
    // Looks for a file with this name anywhere in the project. Used to turn "the
    // file you linked is missing" into "it is here, and this is how to write the
    // path from where you are".
    bool find_by_name(const std::string& file_name, std::string& absolute) const;
    void find_by_name_in(const std::string& dir, const std::string& lower_name, int depth,
                         int& budget, std::string& found) const;
    // A small model reads "this link is dead" and asks the user to fix it. One
    // automatic repair turn, with the dead links named, is what closes that gap.
    void maybe_repair_broken_pages();
    // The model answered a "build this" with code in the chat and called
    // nothing, and nothing in the reply could be placed. One more turn, told
    // plainly that a code block is not a file, is what a small model needs.
    // Returns true when that turn was started.
    bool nudge_towards_tools(ChatMessage& msg);
    // `agent_instruction`, when given, is used instead in the case where the
    // model can actually reach the file: the change is then made in the project
    // rather than printed back for the user to paste.
    void quick_action(const char* label, const char* instruction, bool needs_code,
                      const char* agent_instruction = nullptr);
    void request_inline_completion();
    std::string build_file_context() const;
    void insert_code_into_editor(const std::string& code, bool replace_selection);

    // --- chat rendering ---
    void draw_message(ChatMessage& msg, int index);
    void draw_markdown(const std::string& text, float wrap_width, int message_index);
    // target_path (optional) turns the block into a one-click file write.
    void draw_code_block(const std::string& code, const std::string& lang_tag, int block_id,
                         const std::string& target_path = std::string());

    // --- terminal ---
    void run_command(const std::string& command);
    void update_process();

    // --- live server ---
    // The folder to serve: the workspace, or the open file's own folder when no
    // workspace is open.
    std::string live_root() const;
    bool start_live_server();
    void stop_live_server();
    // Starts the server if it is not up, then opens the browser on the current
    // file — or on the site root when the current file is not a page.
    void open_live_preview();

    // --- command palette / quick open ---
    // One row of the palette: a command, a file, a line, a symbol or a choice
    // in a picker. `run` does whatever choosing it means.
    struct PaletteItem {
        std::string label;
        std::string detail;
        std::string keys;
        std::function<void()> run;
        lang::Id file_lang = lang::Id::Count;
        int score = 0;
    };
    // Opens the palette with a prefix: "" files, ">" commands, ":" go to line,
    // "@" symbols in the open file, "?" help.
    void open_palette(const std::string& prefix);
    // Opens the palette as a list of choices (cursor style, language, ...).
    void open_picker(const std::string& title, std::vector<PaletteItem> items, int selected = 0);
    void draw_palette();
    void build_commands();
    void refresh_palette_view();
    void scan_workspace_files();
    void scan_files_in(const std::string& dir, const std::string& rel, int depth, int& budget);
    void collect_symbols(std::vector<PaletteItem>& out);
    void pick_language();
    void pick_cursor_style(bool overtype);
    void pick_cursor_blinking();
    void pick_whitespace();
    void pick_indentation();
    void pick_line_endings();

    // --- find in files ---
    struct SearchHit {
        std::string path;    // absolute
        std::string rel;     // relative to the workspace, for display
        int line = 0;        // 0-based
        int column = 0;      // byte offset of the match
        int length = 0;
        std::string preview; // the line, trimmed for display
        int preview_offset = 0; // where the match starts inside `preview`
    };
    // Runs on a worker thread; the UI copies hits out under the mutex.
    struct SearchJob {
        std::mutex mutex;
        std::vector<SearchHit> hits;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        std::atomic<int> files{0};
        bool truncated = false;
        std::string error;
    };
    void open_search_view();
    void start_search();
    static void run_search(std::shared_ptr<SearchJob> job, std::string root, std::string query,
                           bool case_sensitive, bool whole_word, bool regex);
    void update_search();
    void draw_search_view();
    void open_search_hit(const SearchHit& hit);

    // --- misc ---
    void toast(const std::string& text, bool error = false);
    void handle_shortcuts();
    std::string workspace_dir_for_dialogs() const;

    void* hwnd_ = nullptr;
    WindowHooks hooks_;
    Settings settings_;
    bool should_exit_ = false;
    bool caption_drag_zone_ = false;
    float title_bar_height_ = 38.0f;
    float dpi_scale_ = 1.0f;
    bool layout_initialized_ = false;
    // The editor takes focus on the first frame so typing works straight
    // away, and so one pane is visibly focused rather than none.
    bool initial_focus_done_ = false;

    // documents
    std::vector<std::unique_ptr<Document>> documents_;
    int active_doc_ = -1;
    int untitled_counter_ = 0;
    std::vector<std::string> closed_paths_; // most recent last, for Ctrl+Shift+T
    int tab_menu_doc_ = -1;

    // explorer
    std::string workspace_;
    TreeNode tree_root_;
    mutable std::string project_map_cache_;
    mutable bool project_map_dirty_ = true;
    std::string selected_path_;
    char filter_buffer_[128] = {0};

    // ai
    ollama::Client client_;
    std::vector<ollama::Model> models_;
    std::string models_error_;
    bool models_loading_ = false;
    std::string ollama_version_;
    bool ollama_online_ = false;
    double last_ping_ = -100.0;

    std::vector<ChatMessage> chat_;
    ollama::StreamPtr chat_stream_;
    std::string chat_input_;
    std::string pending_attachment_;      // file added from the explorer context menu
    std::string pending_attachment_note_; // its name, for the chip on the message
    std::string pending_attachment_path_; // and its full path, so it counts as read

    // Full transcript sent to the model, including tool results.
    // Files the model has actually read during this turn. create_file is only
    // allowed to clobber an existing file that appears here, which stops a model
    // from "adding a function" by replacing the whole file with just that
    // function - the single most destructive thing a weak model does.
    std::set<std::string> read_paths_;

    std::vector<ollama::Message> agent_history_;
    int agent_steps_ = 0;
    // Pages written during this turn, checked again when the turn ends.
    std::vector<std::string> pages_written_;
    int repair_attempts_ = 0;
    // How many times this turn the model has been told that printing code in
    // the chat is not the same as writing a file. Once is enough; twice is a
    // model that cannot do it, and the code blocks stay as they are.
    int block_nudges_ = 0;
    // Whether this turn was asked to change the project. "Explain this file"
    // was not, so a code block in the answer is an illustration, not a file.
    bool turn_wants_files_ = false;
    bool tools_used_this_turn_ = false;
    std::vector<std::string> models_without_tools_; // discovered at runtime
    bool chat_scroll_to_bottom_ = false;
    bool focus_chat_input_ = false;

    ollama::StreamPtr completion_stream_;
    int completion_doc_ = -1;
    ed::Coord completion_cursor_;

    // live server
    web::LiveServer live_;

    // terminal
    sys::Process process_;
    std::string terminal_output_;
    std::string terminal_command_;
    bool terminal_scroll_ = false;

    // command palette
    std::vector<PaletteItem> commands_;
    std::vector<std::string> recent_commands_; // labels, most recent first
    std::vector<std::string> workspace_files_; // relative paths
    bool workspace_files_truncated_ = false;
    std::string workspace_files_root_;
    double workspace_files_time_ = -100.0;
    std::vector<PaletteItem> symbol_cache_;
    const void* symbol_cache_doc_ = nullptr;
    unsigned symbol_cache_version_ = 0;
    bool palette_cursor_end_ = false;
    bool palette_open_ = false;
    bool palette_focus_ = false;
    int palette_frames_ = 0;
    char palette_buffer_[256] = {0};
    std::string palette_view_query_ = "\x01";
    std::vector<PaletteItem> palette_view_;
    int palette_selected_ = 0;
    bool palette_scroll_ = false;
    bool picker_active_ = false;
    std::string picker_title_;
    std::vector<PaletteItem> picker_items_;

    // find in files
    bool explorer_search_ = false;
    bool search_focus_ = false;
    char search_buffer_[256] = {0};
    bool search_case_ = false;
    bool search_word_ = false;
    bool search_regex_ = false;
    std::string search_ran_for_;
    double search_edit_time_ = 0.0;
    bool search_pending_ = false;
    int search_files_ = 0;
    std::shared_ptr<SearchJob> search_job_;
    std::vector<SearchHit> search_hits_;
    bool search_truncated_ = false;
    std::string search_error_;
    std::set<std::string> search_collapsed_; // files folded in the results

    // modals
    bool show_settings_ = false;
    bool show_shortcuts_ = false;
    bool show_about_ = false;
    bool settings_ping_pending_ = false;
    std::string settings_ping_result_;

    struct Toast {
        std::string text;
        bool error;
        double created;
    };
    std::vector<Toast> toasts_;
};

} // namespace app
