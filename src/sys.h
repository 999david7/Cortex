// Thin platform layer: files, folders, dialogs and child processes.
// sys.cpp is the Windows side, sys_posix.cpp (plus sys_mac.mm) macOS and Linux.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace sys {

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

// The separator join_path writes: '\\' on Windows, '/' everywhere else. Both
// are accepted on input.
extern const char kPathSep;

// Compares paths the way the platform's file system does: ignoring ASCII case
// on Windows and macOS (NTFS, and APFS/HFS+ as macOS formats them by
// default), exactly on Linux.
int path_cmp(const char* a, const char* b);
int path_ncmp(const char* a, const char* b, size_t n);

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
// Created on demand: %APPDATA%\AI IDE on Windows, ~/Library/Application
// Support/AI IDE on macOS, $XDG_CONFIG_HOME/ai-ide (~/.config/ai-ide) on Linux.
std::string app_data_dir();

// ------------------------------------------------------------------ shell
// Shows the file in Explorer / Finder / the desktop's file manager.
void open_in_explorer(const std::string& path);
// "Explorer", "Finder" or "File Manager", for menu labels.
const char* file_manager_name();
void open_url(const std::string& url);

// --------------------------------------------------------------- processes
// Runs a command through cmd.exe (Windows) or the user's login shell (macOS,
// Linux) with stdout+stderr merged into a pipe.
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

#ifdef _WIN32
    void* process_ = nullptr;
    void* thread_ = nullptr;
    void* read_pipe_ = nullptr;
    void* write_pipe_ = nullptr;
#else
    int pid_ = -1;
    int read_fd_ = -1;
#endif
    std::string command_;
    mutable int exit_code_ = -1;
    mutable bool finished_ = false;
};

} // namespace sys
