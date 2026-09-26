// Entry point on Windows: frameless Win32 window, Direct3D 11 device and the
// ImGui loop. main_glfw.cpp is the macOS/Linux one.
#ifdef _WIN32

#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "sys.h"
#include "ui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapchain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_swapchain_occluded = false;
UINT g_resize_width = 0, g_resize_height = 0;

app::App* g_app = nullptr;
float g_dpi_scale = 1.0f;
bool g_fonts_dirty = false;
std::string g_ini_path;

// Win11 window attributes; defined here so older SDK headers still build.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

const int kResizeBorder = 6;

// Copies the current back buffer to a 24-bit BMP. Used by --screenshot, which
// exists because the window renders through Direct3D: nothing outside the
// process (PrintWindow, the clipboard, a screen grabber) can read what is on it.
bool save_backbuffer_bmp(const std::string& path) {
    if (!g_swapchain || !g_device || !g_context) return false;

    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    back->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    ID3D11Texture2D* copy = nullptr;
    if (FAILED(g_device->CreateTexture2D(&staging, nullptr, &copy)) || !copy) {
        back->Release();
        return false;
    }
    g_context->CopyResource(copy, back);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_context->Map(copy, 0, D3D11_MAP_READ, 0, &mapped))) {
        copy->Release();
        back->Release();
        return false;
    }

    const int w = (int)desc.Width, h = (int)desc.Height;
    const int row = w * 3;
    const int pad = (4 - (row % 4)) % 4;
    const int image_bytes = (row + pad) * h;

    std::vector<unsigned char> pixels((size_t)image_bytes, 0);
    const unsigned char* src = (const unsigned char*)mapped.pData;
    for (int y = 0; y < h; ++y) {
        // BMP rows run bottom-up.
        const unsigned char* s = src + (size_t)(h - 1 - y) * mapped.RowPitch;
        unsigned char* d = pixels.data() + (size_t)y * (row + pad);
        for (int x = 0; x < w; ++x) {
            // Back buffer is BGRA or RGBA depending on the swap-chain format.
            const unsigned char b0 = s[x * 4 + 0], b1 = s[x * 4 + 1], b2 = s[x * 4 + 2];
            if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                d[x * 3 + 0] = b2; d[x * 3 + 1] = b1; d[x * 3 + 2] = b0;
            } else {
                d[x * 3 + 0] = b0; d[x * 3 + 1] = b1; d[x * 3 + 2] = b2;
            }
        }
    }
    g_context->Unmap(copy, 0);
    copy->Release();
    back->Release();

    unsigned char header[54] = {0};
    const int file_size = 54 + image_bytes;
    header[0] = 'B'; header[1] = 'M';
    header[2] = (unsigned char)(file_size);        header[3] = (unsigned char)(file_size >> 8);
    header[4] = (unsigned char)(file_size >> 16);  header[5] = (unsigned char)(file_size >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = (unsigned char)(w);        header[19] = (unsigned char)(w >> 8);
    header[20] = (unsigned char)(w >> 16);  header[21] = (unsigned char)(w >> 24);
    header[22] = (unsigned char)(h);        header[23] = (unsigned char)(h >> 8);
    header[24] = (unsigned char)(h >> 16);  header[25] = (unsigned char)(h >> 24);
    header[26] = 1;
    header[28] = 24;
    header[34] = (unsigned char)(image_bytes);       header[35] = (unsigned char)(image_bytes >> 8);
    header[36] = (unsigned char)(image_bytes >> 16); header[37] = (unsigned char)(image_bytes >> 24);

    std::string blob((const char*)header, sizeof(header));
    blob.append((const char*)pixels.data(), pixels.size());
    std::string err;
    return sys::write_file(path, blob, err);
}

void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
        back_buffer->Release();
    }
}

void cleanup_render_target() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = 0;
    desc.BufferDesc.Height = 0;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc,
        &g_swapchain, &g_device, &obtained, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) // fall back to the WARP software renderer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &desc, &g_swapchain, &g_device,
                                           &obtained, &g_context);
    if (FAILED(hr)) return false;

    create_render_target();
    return true;
}

void cleanup_device() {
    cleanup_render_target();
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

bool is_maximized(HWND hwnd) {
    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd, &placement)) return false;
    return placement.showCmd == SW_SHOWMAXIMIZED;
}

LRESULT hit_test(HWND hwnd, LPARAM lparam) {
    POINT cursor = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    RECT window;
    GetWindowRect(hwnd, &window);

    const int border = (int)(kResizeBorder * g_dpi_scale);
    const bool maximized = is_maximized(hwnd);

    if (!maximized) {
        const bool left = cursor.x < window.left + border;
        const bool right = cursor.x >= window.right - border;
        const bool top = cursor.y < window.top + border;
        const bool bottom = cursor.y >= window.bottom - border;

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }

    // The app tells us whether the pointer sits on empty title-bar space.
    if (g_app && g_app->caption_drag_zone()) {
        const float bar_h = g_app->title_bar_height();
        if (cursor.y < window.top + (LONG)bar_h) return HTCAPTION;
    }
    return HTCLIENT;
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;

    switch (msg) {
        case WM_NCCALCSIZE:
            if (wparam == TRUE) {
                // Remove the standard frame but keep the resize behaviour.
                NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)lparam;
                if (is_maximized(hwnd)) {
                    // A maximized frameless window would cover the taskbar without this.
                    const int cx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    const int cy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    params->rgrc[0].left += cx;
                    params->rgrc[0].right -= cx;
                    params->rgrc[0].top += cy;
                    params->rgrc[0].bottom -= cy;
                }
                return 0;
            }
            break;

        case WM_NCHITTEST:
            return hit_test(hwnd, lparam);

        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) return 0;
            g_resize_width = (UINT)LOWORD(lparam);
            g_resize_height = (UINT)HIWORD(lparam);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* info = (MINMAXINFO*)lparam;
            info->ptMinTrackSize.x = (LONG)(760 * g_dpi_scale);
            info->ptMinTrackSize.y = (LONG)(480 * g_dpi_scale);
            return 0;
        }

        case WM_DPICHANGED: {
            g_dpi_scale = (float)HIWORD(wparam) / 96.0f;
            g_fonts_dirty = true;
            const RECT* suggested = (const RECT*)lparam;
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_DROPFILES: {
            HDROP drop = (HDROP)wparam;
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> paths;
            for (UINT i = 0; i < count; ++i) {
                wchar_t buffer[MAX_PATH * 2];
                if (DragQueryFileW(drop, i, buffer, MAX_PATH * 2))
                    paths.push_back(sys::narrow(buffer));
            }
            DragFinish(drop);
            if (g_app) g_app->handle_dropped_files(paths);
            SetForegroundWindow(hwnd);
            return 0;
        }

        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_KEYMENU) return 0; // no menu bar to activate
            break;

        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0x0D, 0x0F, 0x14));
    wc.lpszClassName = L"AIIDEWindowClass";
    // Icon 1 is packaging/windows/app.ico, linked in through app.rc.
    wc.hIcon = LoadIcon(instance, MAKEINTRESOURCE(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCE(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MAXIMIZEBOX |
                        WS_MINIMIZEBOX | WS_CLIPCHILDREN;

    // Size to the monitor's work area so the window never opens off-screen.
    RECT work = {0, 0, 1480, 940};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    const int win_w = (work_w * 88) / 100 < 1480 ? (work_w * 88) / 100 : 1480;
    const int win_h = (work_h * 90) / 100 < 940 ? (work_h * 90) / 100 : 940;
    const int win_x = work.left + (work_w - win_w) / 2;
    const int win_y = work.top + (work_h - win_h) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"AI IDE", style, win_x, win_y, win_w, win_h,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    // Win11 niceties: dark mode hints and rounded corners.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = RGB(0x23, 0x28, 0x34);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));

    if (!create_device(hwnd)) {
        cleanup_device();
        MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"AI IDE", MB_ICONERROR);
        return 1;
    }

    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    // Force a frame recalculation so WM_NCCALCSIZE takes effect immediately.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigInputTextCursorBlink = true;

    g_ini_path = sys::join_path(sys::app_data_dir(), "layout.ini");
    io.IniFilename = g_ini_path.c_str();

    g_dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ui::apply_style(g_dpi_scale);
    ui::load_fonts(g_dpi_scale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    app::App application;
    g_app = &application;

    app::WindowHooks hooks;
    hooks.minimize = [hwnd] { ShowWindow(hwnd, SW_MINIMIZE); };
    hooks.toggle_maximize = [hwnd] {
        ShowWindow(hwnd, is_maximized(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    };
    hooks.close = [hwnd] { PostMessageW(hwnd, WM_CLOSE, 0, 0); };
    hooks.is_maximized = [hwnd] { return is_maximized(hwnd); };
    application.init(hwnd, hooks, g_dpi_scale);

    // Command line: any plain argument is a file or folder to open, which is
    // what a shell or a "open with" association passes. --screenshot <path>
    // [frames] renders a few frames so the layout settles, saves the back buffer
    // and quits; the window is Direct3D, so nothing outside the process can see
    // what it looks like otherwise.
    std::string shot_path;
    int shot_after = 60;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            if (wcscmp(argv[i], L"--screenshot") == 0 && i + 1 < argc) {
                shot_path = sys::narrow(argv[++i]);
                if (i + 1 < argc && iswdigit(argv[i + 1][0])) shot_after = _wtoi(argv[++i]);
            } else if (argv[i][0] != L'-') {
                application.open_path(sys::narrow(argv[i]));
            }
        }
        if (argv) LocalFree(argv);
    }
    int frame_no = 0;

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        // Idle cheaply when the window is hidden or fully occluded.
        if (g_swapchain_occluded && g_swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(16);
            continue;
        }
        g_swapchain_occluded = false;

        if (g_resize_width != 0 && g_resize_height != 0) {
            cleanup_render_target();
            g_swapchain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            create_render_target();
        }

        if (g_fonts_dirty) {
            g_fonts_dirty = false;
            ui::apply_style(g_dpi_scale);
            ui::load_fonts(g_dpi_scale);
            ImGui_ImplDX11_InvalidateDeviceObjects();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        application.draw();

        ImGui::Render();
        const float clear[4] = {0.051f, 0.059f, 0.078f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (!shot_path.empty() && ++frame_no >= shot_after) {
            save_backbuffer_bmp(shot_path);
            running = false;
        }

        HRESULT present = g_swapchain->Present(1, 0); // vsync
        g_swapchain_occluded = (present == DXGI_STATUS_OCCLUDED);

        if (application.should_exit()) running = false;
    }

    application.shutdown();
    g_app = nullptr;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}

#endif // _WIN32
