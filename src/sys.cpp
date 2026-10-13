// Windows only; macOS and Linux use sys_posix.cpp.
#ifdef _WIN32

#include "sys.h"

#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>

namespace sys {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// ------------------------------------------------------------------- files

bool read_file(const std::string& path, std::string& out, std::string& err) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "cannot open file";
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        err = "cannot read file size";
        return false;
    }
    if (size.QuadPart > (LONGLONG)64 * 1024 * 1024) {
        CloseHandle(h);
        err = "file is larger than 64 MB";
        return false;
    }
    out.resize((size_t)size.QuadPart);
    DWORD total = 0;
    while (total < (DWORD)size.QuadPart) {
        DWORD read = 0;
        if (!ReadFile(h, &out[total], (DWORD)size.QuadPart - total, &read, nullptr) || read == 0) break;
        total += read;
    }
    out.resize(total);
    CloseHandle(h);

    // Strip a UTF-8 BOM so it does not show up as a stray glyph.
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);

    err.clear();
    return true;
}

bool write_file(const std::string& path, const std::string& content, std::string& err) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "cannot write file";
        return false;
    }
    DWORD written = 0;
    bool ok = content.empty() ||
              WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr) != 0;
    CloseHandle(h);
    if (!ok || (!content.empty() && written != content.size())) {
        err = "write failed";
        return false;
    }
    err.clear();
    return true;
}

bool file_exists(const std::string& path) {
    DWORD attr = GetFileAttributesW(widen(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool is_directory(const std::string& path) {
    DWORD attr = GetFileAttributesW(widen(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool create_directories(const std::string& path, std::string& err) {
    if (path.empty()) {
        err = "empty path";
        return false;
    }
    if (is_directory(path)) return true;

    std::string normalized = normalize(path);
    // Walk down from the root, creating each missing level.
    size_t pos = normalized.find_first_of("\\/");
    if (pos != std::string::npos && pos + 1 < normalized.size() && normalized[pos + 1] == '\\')
        pos = normalized.find_first_of("\\/", pos + 2); // UNC prefix

    while (pos != std::string::npos) {
        std::string part = normalized.substr(0, pos);
        if (!part.empty() && part.back() != ':' && !is_directory(part))
            CreateDirectoryW(widen(part).c_str(), nullptr);
        pos = normalized.find_first_of("\\/", pos + 1);
    }
    if (!CreateDirectoryW(widen(normalized).c_str(), nullptr)) {
        DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS) {
            err = code == ERROR_ACCESS_DENIED ? "access denied" : "could not create the folder";
            return false;
        }
    }
    err.clear();
    return true;
}

long long file_size(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &data)) return -1;
    return ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

std::string file_name(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string parent_path(const std::string& path) {
    std::string p = path;
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t s = p.find_last_of("/\\");
    if (s == std::string::npos) return std::string();
    return p.substr(0, s);
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string out = a;
    if (out.back() != '\\' && out.back() != '/') out += '\\';
    out += b;
    return out;
}

std::string relative_path(const std::string& from_dir, const std::string& target) {
    wchar_t out[MAX_PATH * 2] = {0};
    const std::wstring from = widen(normalize(from_dir));
    const std::wstring to = widen(normalize(target));
    if (from.empty() || to.empty()) return std::string();
    if (!PathRelativePathToW(out, from.c_str(), FILE_ATTRIBUTE_DIRECTORY, to.c_str(), 0))
        return std::string();

    std::string rel = narrow(out);
    for (char& c : rel)
        if (c == '\\') c = '/';
    // PathRelativePathTo always prefixes "./", which no one writes by hand.
    if (rel.compare(0, 2, "./") == 0) rel.erase(0, 2);
    return rel;
}

std::string normalize(const std::string& path) {
    std::wstring w = widen(path);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(w.c_str(), MAX_PATH * 2, buf, nullptr);
    if (n == 0 || n >= MAX_PATH * 2) return path;
    return narrow(std::wstring(buf, buf + n));
}

namespace {

bool should_skip(const std::string& name, bool is_dir) {
    if (name == "." || name == "..") return true;
    if (!name.empty() && name[0] == '.') {
        // keep useful dotfiles, hide VCS noise
        if (name == ".git" || name == ".svn" || name == ".hg" || name == ".vs" ||
            name == ".idea" || name == ".cache")
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
    std::wstring pattern = widen(join_path(path, "*"));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    do {
        std::string name = narrow(fd.cFileName);
        bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (should_skip(name, dir)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;

        DirEntry e;
        e.name = name;
        e.path = join_path(path, name);
        e.is_dir = dir;
        e.size = dir ? 0 : (((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

// ----------------------------------------------------------------- dialogs

namespace {

// Shared IFileDialog driver. mode: 0 = open file, 1 = save file, 2 = pick folder.
std::string run_file_dialog(void* owner, int mode, const std::string& start_dir,
                            const std::string& suggested_name, const std::string& title) {
    std::string result;
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool need_uninit = SUCCEEDED(init);

    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(mode == 1 ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr) && dialog) {
        DWORD flags = 0;
        dialog->GetOptions(&flags);
        flags |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        if (mode == 2) flags |= FOS_PICKFOLDERS;
        dialog->SetOptions(flags);

        if (!title.empty()) dialog->SetTitle(widen(title).c_str());
        if (!suggested_name.empty()) dialog->SetFileName(widen(suggested_name).c_str());

        if (!start_dir.empty()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(widen(start_dir).c_str(), nullptr,
                                                      IID_PPV_ARGS(&folder))) && folder) {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }

        if (SUCCEEDED(dialog->Show((HWND)owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR wpath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath)) && wpath) {
                    result = narrow(wpath);
                    CoTaskMemFree(wpath);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    if (need_uninit) CoUninitialize();
    return result;
}

} // namespace

std::string pick_folder(void* owner, const std::string& title) {
    return run_file_dialog(owner, 2, "", "", title.empty() ? "Open Folder" : title);
}

std::string pick_open_file(void* owner, const std::string& start_dir) {
    return run_file_dialog(owner, 0, start_dir, "", "Open File");
}

std::string pick_save_file(void* owner, const std::string& suggested_name,
                           const std::string& start_dir) {
    return run_file_dialog(owner, 1, start_dir, suggested_name, "Save File As");
}

// -------------------------------------------------------------- app config

std::string app_data_dir() {
    PWSTR path = nullptr;
    std::string base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)) && path) {
        base = narrow(path);
        CoTaskMemFree(path);
    }
    if (base.empty()) {
        char buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
        base = n ? std::string(buf, buf + n) : std::string(".");
    }
    std::string dir = join_path(base, "AI IDE");
    CreateDirectoryW(widen(dir).c_str(), nullptr);
    return dir;
}

// ------------------------------------------------------------------ shell

void open_in_explorer(const std::string& path) {
    std::wstring w = widen(path);
    if (is_directory(path)) {
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        std::wstring args = L"/select,\"" + w + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void open_url(const std::string& url) {
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --------------------------------------------------------------- processes

Process::~Process() {
    terminate();
    close_handles();
}

void Process::close_handles() {
    if (read_pipe_) { CloseHandle((HANDLE)read_pipe_); read_pipe_ = nullptr; }
    if (write_pipe_) { CloseHandle((HANDLE)write_pipe_); write_pipe_ = nullptr; }
    if (thread_) { CloseHandle((HANDLE)thread_); thread_ = nullptr; }
    if (process_) { CloseHandle((HANDLE)process_); process_ = nullptr; }
}

bool Process::start(const std::string& command_line, const std::string& working_dir, std::string& err) {
    terminate();
    close_handles();
    finished_ = false;
    exit_code_ = -1;
    command_ = command_line;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 1 << 16)) {
        err = "could not create pipe";
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"cmd.exe /d /s /c \"" + widen(command_line) + L"\"";
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    std::wstring cwd = widen(working_dir);
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(write_end);
    if (!ok) {
        CloseHandle(read_end);
        err = "could not start process";
        return false;
    }

    read_pipe_ = read_end;
    process_ = pi.hProcess;
    thread_ = pi.hThread;
    err.clear();
    return true;
}

bool Process::poll(std::string& out) {
    if (!read_pipe_) return false;
    bool got = false;
    char buf[4096];
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe((HANDLE)read_pipe_, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0) break;
        DWORD to_read = available < sizeof(buf) ? available : (DWORD)sizeof(buf);
        DWORD read = 0;
        if (!ReadFile((HANDLE)read_pipe_, buf, to_read, &read, nullptr) || read == 0) break;
        out.append(buf, read);
        got = true;
    }
    // Once the child is gone and the pipe is drained, capture the exit code.
    if (process_ && !running() && !finished_) {
        DWORD code = 0;
        GetExitCodeProcess((HANDLE)process_, &code);
        exit_code_ = (int)code;
        finished_ = true;
    }
    return got;
}

bool Process::running() const {
    if (!process_) return false;
    return WaitForSingleObject((HANDLE)process_, 0) == WAIT_TIMEOUT;
}

void Process::terminate() {
    if (process_ && running()) {
        TerminateProcess((HANDLE)process_, 1);
        WaitForSingleObject((HANDLE)process_, 2000);
        exit_code_ = -1;
        finished_ = true;
    }
}

} // namespace sys

#endif // _WIN32
