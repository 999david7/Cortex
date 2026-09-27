// macOS and Linux side of the platform layer. sys.cpp is the Windows one; the
// native file dialogs on macOS live in sys_mac.mm.
#include "sys.h"

#ifndef _WIN32

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sys {

#ifdef __APPLE__
// sys_mac.mm: NSOpenPanel / NSSavePanel. mode: 0 = open file, 1 = save file,
// 2 = pick folder.
std::string mac_file_dialog(int mode, const std::string& start_dir,
                            const std::string& suggested_name, const std::string& title);
#endif

const char kPathSep = '/';

#ifdef __APPLE__
int path_cmp(const char* a, const char* b) { return strcasecmp(a, b); }
int path_ncmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
#else
int path_cmp(const char* a, const char* b) { return std::strcmp(a, b); }
int path_ncmp(const char* a, const char* b, size_t n) { return std::strncmp(a, b, n); }
#endif

// Strings are UTF-8 end to end here; the wide forms only exist so shared code
// that still speaks them compiles. wchar_t is UTF-32 on these platforms.
std::wstring widen(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        unsigned int cp = c;
        int extra = 0;
        if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i)
            cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        out += (wchar_t)cp;
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
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = "cannot open file";
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        err = "cannot read file size";
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        ::close(fd);
        err = "cannot open file";
        return false;
    }
    if (st.st_size > (off_t)64 * 1024 * 1024) {
        ::close(fd);
        err = "file is larger than 64 MB";
        return false;
    }
    out.resize((size_t)st.st_size);
    size_t total = 0;
    while (total < out.size()) {
        const ssize_t n = ::read(fd, &out[total], out.size() - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        total += (size_t)n;
    }
    out.resize(total);
    ::close(fd);

    // Strip a UTF-8 BOM so it does not show up as a stray glyph.
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);

    err.clear();
    return true;
}

bool write_file(const std::string& path, const std::string& content, std::string& err) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        err = errno == EACCES ? "permission denied" : "cannot write file";
        return false;
    }
    size_t written = 0;
    bool ok = true;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        written += (size_t)n;
    }
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        err = "write failed";
        return false;
    }
    err.clear();
    return true;
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool create_directories(const std::string& path, std::string& err) {
    if (path.empty()) {
        err = "empty path";
        return false;
    }
    if (is_directory(path)) return true;

    const std::string normalized = normalize(path);
    // Walk down from the root, creating each missing level.
    size_t pos = normalized.find('/', 1);
    while (pos != std::string::npos) {
        const std::string part = normalized.substr(0, pos);
        if (!is_directory(part)) ::mkdir(part.c_str(), 0755);
        pos = normalized.find('/', pos + 1);
    }
    if (::mkdir(normalized.c_str(), 0755) != 0 && errno != EEXIST) {
        err = errno == EACCES ? "access denied" : "could not create the folder";
        return false;
    }
    if (!is_directory(normalized)) {
        err = "a file with that name already exists";
        return false;
    }
    err.clear();
    return true;
}

long long file_size(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return -1;
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
    if (out.back() != '\\' && out.back() != '/') out += '/';
    out += b;
    return out;
}

namespace {

std::vector<std::string> split_components(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        if (end > start) parts.push_back(path.substr(start, end - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return parts;
}

bool same_component(const std::string& a, const std::string& b) {
    return path_cmp(a.c_str(), b.c_str()) == 0;
}

} // namespace

std::string relative_path(const std::string& from_dir, const std::string& target) {
    if (from_dir.empty() || target.empty()) return std::string();
    const std::vector<std::string> from = split_components(normalize(from_dir));
    const std::vector<std::string> to = split_components(normalize(target));

    size_t common = 0;
    while (common < from.size() && common < to.size() && same_component(from[common], to[common]))
        ++common;

    std::string rel;
    for (size_t i = common; i < from.size(); ++i) rel += "../";
    for (size_t i = common; i < to.size(); ++i) {
        rel += to[i];
        if (i + 1 < to.size()) rel += '/';
    }
    if (!rel.empty() && rel.back() == '/') rel.pop_back();
    return rel.empty() ? std::string(".") : rel;
}

// Absolute, with "." and ".." folded away and no trailing slash. Purely
// lexical, like GetFullPathName on Windows: symlinks are left as they are and
// the path does not have to exist.
std::string normalize(const std::string& path) {
    if (path.empty()) return path;
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');

    if (p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        if (const char* home = getenv("HOME")) p = std::string(home) + p.substr(1);
    }
    if (p[0] != '/') {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) p = std::string(cwd) + "/" + p;
    }

    std::vector<std::string> out;
    for (std::string& part : split_components(p)) {
        if (part == ".") continue;
        if (part == "..") {
            if (!out.empty()) out.pop_back();
            continue;
        }
        out.push_back(std::move(part));
    }
    std::string result;
    for (const std::string& part : out) result += "/" + part;
    return result.empty() ? std::string("/") : result;
}

namespace {

bool should_skip(const std::string& name, bool is_dir) {
    if (name == "." || name == "..") return true;
    if (!name.empty() && name[0] == '.') {
        // keep useful dotfiles, hide VCS noise and Finder metadata
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
    DIR* dir = ::opendir(path.c_str());
    if (!dir) return false;

    while (dirent* ent = ::readdir(dir)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        const std::string full = join_path(path, name);
        struct stat st {};
        // stat, not lstat: a symlinked folder is shown and opened as a folder.
        if (::stat(full.c_str(), &st) != 0) continue;
        const bool dir_entry = S_ISDIR(st.st_mode);
        if (should_skip(name, dir_entry)) continue;
        if (!dir_entry && !S_ISREG(st.st_mode)) continue; // sockets, fifos, devices

        DirEntry e;
        e.name = name;
        e.path = full;
        e.is_dir = dir_entry;
        e.size = dir_entry ? 0 : (long long)st.st_size;
        out.push_back(std::move(e));
    }
    ::closedir(dir);

    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

// ----------------------------------------------------------------- helpers

namespace {

#ifndef __APPLE__
// Single-quoted for /bin/sh, so nothing in the string is interpreted.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}
#endif

// Starts a program without waiting for it and without a shell in between.
void spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return;
    const pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        // Double fork so the helper is reparented and never left as a zombie.
        if (fork() != 0) _exit(0);
        setsid();
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        std::vector<char*> args;
        for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

#ifndef __APPLE__
// Whether a helper is on PATH.
bool have_program(const char* name) {
    const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

// Runs a helper and returns its first line of output; empty on cancel.
std::string capture_first_line(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return std::string();
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    const int status = pclose(pipe);
    if (status != 0) return std::string();
    const size_t nl = out.find('\n');
    if (nl != std::string::npos) out.erase(nl);
    return out;
}

// zenity (GNOME and most others) or kdialog (KDE), whichever is installed.
std::string linux_file_dialog(int mode, const std::string& start_dir,
                              const std::string& suggested_name, const std::string& title) {
    std::string start = start_dir.empty() ? std::string(getenv("HOME") ? getenv("HOME") : ".")
                                          : start_dir;
    if (have_program("zenity")) {
        std::string cmd = "zenity --file-selection 2>/dev/null --title=" + shell_quote(title);
        if (mode == 1) {
            cmd += " --save --confirm-overwrite";
            cmd += " --filename=" + shell_quote(join_path(start, suggested_name));
        } else {
            if (mode == 2) cmd += " --directory";
            cmd += " --filename=" + shell_quote(start + "/");
        }
        return capture_first_line(cmd);
    }
    if (have_program("kdialog")) {
        std::string cmd = "kdialog 2>/dev/null --title " + shell_quote(title);
        if (mode == 0) cmd += " --getopenfilename " + shell_quote(start);
        else if (mode == 1) cmd += " --getsavefilename " + shell_quote(join_path(start, suggested_name));
        else cmd += " --getexistingdirectory " + shell_quote(start);
        return capture_first_line(cmd);
    }
    fprintf(stderr, "Cortex: install zenity or kdialog for file dialogs, or pass a path "
                    "on the command line.\n");
    return std::string();
}
#endif

std::string file_dialog(int mode, const std::string& start_dir, const std::string& suggested_name,
                        const std::string& title) {
    std::string result;
#ifdef __APPLE__
    result = mac_file_dialog(mode, start_dir, suggested_name, title);
#else
    result = linux_file_dialog(mode, start_dir, suggested_name, title);
#endif
    while (result.size() > 1 && result.back() == '/') result.pop_back();
    return result;
}

} // namespace

// ----------------------------------------------------------------- dialogs

std::string pick_folder(void*, const std::string& title) {
    return file_dialog(2, "", "", title.empty() ? "Open Folder" : title);
}

std::string pick_open_file(void*, const std::string& start_dir) {
    return file_dialog(0, start_dir, "", "Open File");
}

std::string pick_save_file(void*, const std::string& suggested_name, const std::string& start_dir) {
    return file_dialog(1, start_dir, suggested_name, "Save File As");
}

// -------------------------------------------------------------- app config

std::string app_data_dir() {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
#ifdef __APPLE__
    const std::string base = home + "/Library/Application Support";
    const std::string dir = base + "/Cortex";
    const std::string old_dir = base + "/AI IDE";
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const std::string base = (xdg && *xdg) ? std::string(xdg) : home + "/.config";
    const std::string dir = base + "/cortex";
    const std::string old_dir = base + "/ai-ide";
#endif
    // The app used to be called AI IDE; carry its settings and layout over.
    if (!is_directory(dir) && is_directory(old_dir)) ::rename(old_dir.c_str(), dir.c_str());
    std::string err;
    create_directories(dir, err);
    return dir;
}

// ------------------------------------------------------------------ shell

void open_in_explorer(const std::string& path) {
#ifdef __APPLE__
    if (is_directory(path)) spawn_detached({"open", path});
    else spawn_detached({"open", "-R", path}); // reveal and select in Finder
#else
    // There is no portable "select this file"; open the folder that holds it.
    spawn_detached({"xdg-open", is_directory(path) ? path : parent_path(path)});
#endif
}

const char* file_manager_name() {
#ifdef __APPLE__
    return "Finder";
#else
    return "File Manager";
#endif
}

void open_url(const std::string& url) {
#ifdef __APPLE__
    spawn_detached({"open", url});
#else
    spawn_detached({"xdg-open", url});
#endif
}

// --------------------------------------------------------------- processes

Process::~Process() {
    terminate();
    close_handles();
}

void Process::close_handles() {
    if (read_fd_ >= 0) {
        ::close(read_fd_);
        read_fd_ = -1;
    }
}

bool Process::start(const std::string& command_line, const std::string& working_dir, std::string& err) {
    terminate();
    close_handles();
    finished_ = false;
    exit_code_ = -1;
    pid_ = -1;
    command_ = command_line;

    int fds[2];
    if (pipe(fds) != 0) {
        err = "could not create pipe";
        return false;
    }

    // A login shell, so the command sees the PATH the user set up in their
    // profile. An app started from Finder or the Dock inherits only
    // /usr/bin:/bin:/usr/sbin:/sbin, which has neither Homebrew nor npm on it.
    const char* env_shell = getenv("SHELL");
    const std::string shell = (env_shell && *env_shell && access(env_shell, X_OK) == 0)
                                  ? env_shell
                                  : "/bin/sh";

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        err = "could not start process";
        return false;
    }
    if (pid == 0) {
        // Own process group, so terminate() stops everything the command started.
        setpgid(0, 0);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        ::close(fds[0]);
        ::close(fds[1]);
        if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) {
            fprintf(stderr, "cannot enter %s\n", working_dir.c_str());
            _exit(127);
        }
        execl(shell.c_str(), shell.c_str(), "-l", "-c", command_line.c_str(), (char*)nullptr);
        _exit(127);
    }
    setpgid(pid, pid); // also from the parent, so there is no race with terminate()

    ::close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);

    read_fd_ = fds[0];
    pid_ = pid;
    err.clear();
    return true;
}

bool Process::poll(std::string& out) {
    if (read_fd_ < 0) return false;
    bool got = false;
    char buf[4096];
    bool eof = false;
    while (true) {
        const ssize_t n = ::read(read_fd_, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, (size_t)n);
            got = true;
            continue;
        }
        if (n == 0) eof = true; // every writer has closed its end
        else if (errno == EINTR) continue;
        break; // EAGAIN: nothing more right now
    }
    // Once the child is gone and the pipe is drained, capture the exit code.
    if (pid_ > 0 && !running() && !finished_) finished_ = true;
    if (eof && finished_) close_handles();
    return got;
}

bool Process::running() const {
    if (pid_ <= 0 || finished_) return false;
    int status = 0;
    const pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == 0) return true;
    if (r == pid_) {
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                       : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    }
    // Reaped (or lost, which is the same thing to the caller). pid_ has to
    // stay set so poll() can tell a finished process from none at all.
    finished_ = true;
    return false;
}

void Process::terminate() {
    if (pid_ > 0 && running()) {
        kill(-pid_, SIGTERM);
        // Give it up to two seconds, as the Windows side does, then insist.
        for (int i = 0; i < 200; ++i) {
            const pid_t r = waitpid(pid_, nullptr, WNOHANG);
            if (r == pid_ || (r < 0 && errno == ECHILD)) break;
            if (i == 199) {
                kill(-pid_, SIGKILL);
                waitpid(pid_, nullptr, 0);
            }
            usleep(10 * 1000);
        }
        exit_code_ = -1;
        finished_ = true;
    }
}

} // namespace sys

#endif // !_WIN32
