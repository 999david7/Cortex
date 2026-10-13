// Thin platform layer: files, folders, dialogs and child processes.
// sys.cpp implements it for Windows, sys_posix.cpp for macOS and Linux.
#pragma once

#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
// The case-insensitive comparisons the rest of the code calls by their MSVC
// names.
#include <strings.h>
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
#endif

namespace sys {

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

// ------------------------------------------------------------------- files
bool read_file(const std::string& path, std::string& out, std::string& err);
bool write_file(const std::string& path, const std::string& content, std::string& err);
bool file_exists(const std::string& path);
bool is_directory(const std::string& path);
// Creates every missing component of the path. Succeeds if it already exists.
bool create_directories(const std::string& path, std::string& err);
long long file_size(const std::string& path);

std::string file_name(const std::string& path);
std::string parent_path(const std::string& path);
std::string join_path(const std::string& a, const std::string& b);
// How `target` has to be written from inside `from_dir`, with forward slashes:
// exactly what an href or src on a page in that folder needs to say. Empty when
// the two are not on the same drive.
std::string relative_path(const std::string& from_dir, const std::string& target);
std::string normalize(const std::string& path);

struct DirEntry {
    std::string name;
    std::string path;
    bool is_dir = false;
    long long size = 0;
};

// Sorted: directories first, then files, both alphabetically.
// Hidden entries and heavy build folders are skipped.
bool list_directory(const std::string& path, std::vector<DirEntry>& out);

// ----------------------------------------------------------------- dialogs
// Return an empty string when the user cancels.
std::string pick_folder(void* owner_hwnd, const std::string& title);
std::string pick_open_file(void* owner_hwnd, const std::string& start_dir);
std::string pick_save_file(void* owner_hwnd, const std::string& suggested_name,
                           const std::string& start_dir);

// -------------------------------------------------------------- app config
std::string app_data_dir(); // %APPDATA%\AI IDE, created on demand

// ------------------------------------------------------------------ shell
void open_in_explorer(const std::string& path);
void open_url(const std::string& url);

// --------------------------------------------------------------- processes
// Runs a command through the shell (cmd.exe on Windows, the user's login
// shell elsewhere) with stdout+stderr merged into a pipe.
class Process {
public:
    ~Process();

    bool start(const std::string& command_line, const std::string& working_dir, std::string& err);
    // Non-blocking: appends whatever is available, returns true if anything was read.
    bool poll(std::string& out);
    bool running() const;
    void terminate();
    int exit_code() const { return exit_code_; }
    const std::string& command() const { return command_; }

private:
    void close_handles();

    void* process_ = nullptr;
    void* thread_ = nullptr;
    void* read_pipe_ = nullptr;
    void* write_pipe_ = nullptr;
    std::string command_;
    mutable int exit_code_ = -1;
    mutable bool finished_ = false;
};

} // namespace sys
