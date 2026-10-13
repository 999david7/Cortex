// The platform layer for macOS and Linux: files, folders, dialogs, the shell
// and child processes. sys.cpp is the Windows side of the same interface.
#ifndef _WIN32

#include "sys.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern char** environ;

namespace sys {

#ifdef __APPLE__
// Native panels, in sys_mac.mm.
std::string mac_pick_folder(const std::string& title);
std::string mac_pick_open_file(const std::string& start_dir);
std::string mac_pick_save_file(const std::string& suggested_name, const std::string& start_dir);
#endif

// wchar_t is 32 bits here, so a wide string is UTF-32.
std::wstring widen(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        unsigned int cp = c;
        int len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        if (i + (size_t)len > s.size()) { out += (wchar_t)0xFFFD; break; }
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out += (wchar_t)cp;
        i += (size_t)len;
    }
    return out;
}

std::string narrow(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t wc : s) {
        const unsigned int cp = (unsigned int)wc;
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// ------------------------------------------------------------------- files

bool read_file(const std::string& path, std::string& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open file";
        return false;
    }
    struct stat st {};
    if (fstat(fileno(f), &st) != 0 || S_ISDIR(st.st_mode)) {
        std::fclose(f);
        err = "cannot read file size";
        return false;
    }
    if (st.st_size > (off_t)64 * 1024 * 1024) {
        std::fclose(f);
        err = "file is larger than 64 MB";
        return false;
    }
    out.resize((size_t)st.st_size);
    const size_t got = out.empty() ? 0 : std::fread(&out[0], 1, out.size(), f);
    out.resize(got);
    std::fclose(f);

    // Strip a UTF-8 BOM so it does not show up as a stray glyph.
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);

    err.clear();
    return true;
}

bool write_file(const std::string& path, const std::string& content, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        err = errno == EACCES ? "access denied" : "cannot write file";
        return false;
    }
    const size_t put = content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), f);
    const bool closed = std::fclose(f) == 0;
    if (put != content.size() || !closed) {
        err = "write failed";
        return false;
    }
    err.clear();
    return true;
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool create_directories(const std::string& path, std::string& err) {
    if (path.empty()) {
        err = "empty path";
        return false;
    }
    if (is_directory(path)) return true;
    const std::string normalized = normalize(path);
    for (size_t pos = normalized.find('/', 1); pos != std::string::npos; pos = normalized.find('/', pos + 1)) {
        const std::string part = normalized.substr(0, pos);
        if (!is_directory(part)) mkdir(part.c_str(), 0755);
    }
    if (mkdir(normalized.c_str(), 0755) != 0 && errno != EEXIST) {
        err = errno == EACCES ? "access denied" : "could not create the folder";
        return false;
    }
    err.clear();
    return true;
}

long long file_size(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return -1;
    return (long long)st.st_size;
}

std::string file_name(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string parent_path(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t s = p.find_last_of("/\\");
    if (s == std::string::npos) return std::string();
    if (s == 0) return "/";
    return p.substr(0, s);
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string out = a;
    if (out.back() != '/') out += '/';
    out += b[0] == '/' ? b.substr(1) : b;
    return out;
}

namespace {

std::vector<std::string> components(const std::string& path) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) out.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return out;
}

std::string home_dir() {
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) : std::string("/");
}

} // namespace

// Absolute, with "." and ".." folded away and no symlinks resolved — the
// same contract as GetFullPathName on the Windows side. Backslashes are read
// as separators, since paths written for Windows still turn up (a model's tool
// call, a link in a page).
std::string normalize(const std::string& path) {
    if (path.empty()) return path;
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p[0] == '~' && (p.size() == 1 || p[1] == '/')) p = home_dir() + p.substr(1);
    if (p[0] != '/') {
        char cwd[4096];
        p = std::string(getcwd(cwd, sizeof(cwd)) ? cwd : "/") + "/" + p;
    }
    std::vector<std::string> out;
    for (const std::string& part : components(p)) {
        if (part == ".") continue;
        if (part == "..") {
            if (!out.empty()) out.pop_back();
            continue;
        }
        out.push_back(part);
    }
    std::string result;
    for (const std::string& part : out) result += "/" + part;
    return result.empty() ? "/" : result;
}

std::string relative_path(const std::string& from_dir, const std::string& target) {
    const std::vector<std::string> from = components(normalize(from_dir));
    const std::vector<std::string> to = components(normalize(target));
    size_t common = 0;
    while (common < from.size() && common < to.size() && from[common] == to[common]) ++common;
    std::string rel;
    for (size_t i = common; i < from.size(); ++i) rel += "../";
    for (size_t i = common; i < to.size(); ++i) {
        rel += to[i];
        if (i + 1 < to.size()) rel += "/";
    }
    if (rel.empty()) return ".";
    if (rel.back() == '/') rel.pop_back();
    return rel;
}

namespace {

bool should_skip(const std::string& name, bool is_dir) {
    if (name == "." || name == "..") return true;
    if (!name.empty() && name[0] == '.') {
        // keep useful dotfiles, hide VCS noise and Finder's bookkeeping
        if (name == ".git" || name == ".svn" || name == ".hg" || name == ".vs" ||
            name == ".idea" || name == ".cache" || name == ".DS_Store" || name == ".Trash")
            return true;
        return false;
    }
    if (is_dir) {
        if (name == "node_modules" || name == "__pycache__" || name == "target" ||
            name == "Debug" || name == "Release")
            return true;
    }
    return false;
}

} // namespace

bool list_directory(const std::string& path, std::vector<DirEntry>& out) {
    out.clear();
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    while (dirent* d = readdir(dir)) {
        const std::string name = d->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = join_path(path, name);
        struct stat st {};
        if (stat(full.c_str(), &st) != 0) continue; // a dangling link
        const bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !S_ISREG(st.st_mode)) continue; // sockets, fifos, devices
        if (should_skip(name, is_dir)) continue;
        DirEntry e;
        e.name = name;
        e.path = full;
        e.is_dir = is_dir;
        e.size = is_dir ? 0 : (long long)st.st_size;
        out.push_back(std::move(e));
    }
    closedir(dir);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

// ----------------------------------------------------------------- dialogs

namespace {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

#ifndef __APPLE__
// Linux has no single native dialog; zenity is the common one, and without it
// the dialog simply does not appear.
std::string run_zenity(const std::string& args) {
    const std::string cmd = "zenity --file-selection " + args + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return std::string();
    std::string out;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
#endif

// Starts a program with arguments and lets it run on its own; a thread reaps
// it so it never lingers as a zombie.
void launch(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) == 0)
        std::thread([pid] {
            int status = 0;
            waitpid(pid, &status, 0);
        }).detach();
}

} // namespace

std::string pick_folder(void*, const std::string& title) {
#ifdef __APPLE__
    return mac_pick_folder(title.empty() ? "Open Folder" : title);
#else
    return run_zenity("--directory --title=" + shell_quote(title.empty() ? "Open Folder" : title));
#endif
}

std::string pick_open_file(void*, const std::string& start_dir) {
#ifdef __APPLE__
    return mac_pick_open_file(start_dir);
#else
    std::string args = "--title='Open File'";
    if (!start_dir.empty()) args += " --filename=" + shell_quote(join_path(start_dir, ""));
    return run_zenity(args);
#endif
}

std::string pick_save_file(void*, const std::string& suggested_name, const std::string& start_dir) {
#ifdef __APPLE__
    return mac_pick_save_file(suggested_name, start_dir);
#else
    std::string args = "--save --confirm-overwrite --title='Save File As'";
    const std::string where = start_dir.empty() ? suggested_name : join_path(start_dir, suggested_name);
    if (!where.empty()) args += " --filename=" + shell_quote(where);
    return run_zenity(args);
#endif
}

// -------------------------------------------------------------- app config

std::string app_data_dir() {
#ifdef __APPLE__
    const std::string dir = join_path(home_dir(), "Library/Application Support/AI IDE");
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string base = xdg && *xdg ? std::string(xdg) : join_path(home_dir(), ".config");
    const std::string dir = join_path(base, "ai-ide");
#endif
    std::string err;
    create_directories(dir, err);
    return dir;
}

// ------------------------------------------------------------------ shell

void open_in_explorer(const std::string& path) {
#ifdef __APPLE__
    if (is_directory(path)) launch({"open", path});
    else launch({"open", "-R", path}); // reveal the file in Finder
#else
    launch({"xdg-open", is_directory(path) ? path : parent_path(path)});
#endif
}

void open_url(const std::string& url) {
#ifdef __APPLE__
    launch({"open", url});
#else
    launch({"xdg-open", url});
#endif
}

// --------------------------------------------------------------- processes
//
// The handles the header keeps as void* hold, on this side, the child's pid
// (process_) and the read end of its output pipe plus one (read_pipe_), so
// that zero still means "none".

namespace {
pid_t pid_of(void* p) { return (pid_t)(intptr_t)p; }
int fd_of(void* p) { return (int)(intptr_t)p - 1; }
} // namespace

Process::~Process() {
    terminate();
    close_handles();
}

void Process::close_handles() {
    if (read_pipe_) {
        close(fd_of(read_pipe_));
        read_pipe_ = nullptr;
    }
    process_ = nullptr;
}

bool Process::start(const std::string& command_line, const std::string& working_dir, std::string& err) {
    terminate();
    close_handles();
    finished_ = false;
    exit_code_ = -1;
    command_ = command_line;

    int fds[2];
    if (pipe(fds) != 0) {
        err = "could not create pipe";
        return false;
    }

    // A login shell, so the PATH is the one the user's terminal has: an app
    // started from the Dock gets a bare environment otherwise, and npm,
    // python or a Homebrew compiler would not be found.
    const char* env_shell = std::getenv("SHELL");
#ifdef __APPLE__
    const std::string shell = env_shell && *env_shell ? env_shell : "/bin/zsh";
#else
    const std::string shell = env_shell && *env_shell ? env_shell : "/bin/sh";
#endif
    const long max_fd = sysconf(_SC_OPEN_MAX) > 0 ? sysconf(_SC_OPEN_MAX) : 1024;
    const char* dir = working_dir.empty() ? nullptr : working_dir.c_str();

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        err = "could not start process";
        return false;
    }
    if (pid == 0) {
        // Child: only async-signal-safe calls until exec.
        setpgid(0, 0); // its own group, so stopping it stops what it started
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) dup2(null_fd, STDIN_FILENO);
        // Nothing of the IDE's — the live server's socket above all — may
        // outlive it inside a long-running child.
        for (long fd = 3; fd < max_fd && fd < 65536; ++fd) close((int)fd);
        if (dir && chdir(dir) != 0) _exit(126);
        execl(shell.c_str(), shell.c_str(), "-lc", command_line.c_str(), (char*)nullptr);
        _exit(127);
    }

    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    read_pipe_ = (void*)(intptr_t)(fds[0] + 1);
    process_ = (void*)(intptr_t)pid;
    err.clear();
    return true;
}

bool Process::poll(std::string& out) {
    if (!read_pipe_) return false;
    bool got = false;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd_of(read_pipe_), buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, (size_t)n);
            got = true;
            continue;
        }
        break; // 0: the child closed the pipe; -1 with EAGAIN: nothing yet
    }
    running(); // reaps the child once it has exited
    return got;
}

bool Process::running() const {
    if (!process_ || finished_) return false;
    int status = 0;
    const pid_t r = waitpid(pid_of(process_), &status, WNOHANG);
    if (r == 0) return true;
    if (r == pid_of(process_)) {
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    finished_ = true;
    return false;
}

void Process::terminate() {
    if (!running()) return;
    const pid_t pid = pid_of(process_);
    kill(-pid, SIGTERM);
    for (int i = 0; i < 100 && running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (running()) {
        kill(-pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
    }
    exit_code_ = -1;
    finished_ = true;
}

} // namespace sys

#endif // !_WIN32
