// Entry point on macOS and Linux: a GLFW window, an OpenGL 3 context and the
// ImGui loop. main.cpp is the Win32 + Direct3D 11 one.
#ifndef _WIN32

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION // OpenGL is deprecated on macOS, not gone
#endif
#include <GLFW/glfw3.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"
#include "sys.h"
#include "ui.h"

namespace {

app::App* g_app = nullptr;
bool g_fonts_dirty = false;
std::string g_ini_path;

// Two numbers describe the display, and they differ by platform:
//   ui scale  - how much larger than the 96-dpi design every pixel measurement
//               in the layout has to be, in the window's own coordinates;
//   density   - framebuffer pixels per window coordinate, which is what fonts
//               are rasterised at so they stay sharp.
// macOS lays windows out in points, so a Retina display is ui 1, density 2.
// X11 and most Wayland setups lay out in pixels: ui 1.5, density 1.
struct Scale {
    float ui = 1.0f;
    float density = 1.0f;
};

Scale window_scale(GLFWwindow* window) {
    Scale s;
    float cx = 1.0f, cy = 1.0f;
    glfwGetWindowContentScale(window, &cx, &cy);
    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    s.density = (ww > 0 && fw > 0) ? (float)fw / (float)ww : cx;
    if (s.density <= 0.0f) s.density = 1.0f;
    s.ui = cx / s.density;
    if (s.ui < 1.0f) s.ui = 1.0f;
    return s;
}

// Reads the framebuffer into a 24-bit BMP. Used by --screenshot, so the look
// can be checked without a screen grabber.
bool save_framebuffer_bmp(GLFWwindow* window, const std::string& path) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    if (w <= 0 || h <= 0) return false;

    std::vector<unsigned char> rgba((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    const int row = w * 3;
    const int pad = (4 - (row % 4)) % 4;
    const int image_bytes = (row + pad) * h;
    std::vector<unsigned char> pixels((size_t)image_bytes, 0);
    // OpenGL rows run bottom-up, exactly as a BMP wants them.
    for (int y = 0; y < h; ++y) {
        const unsigned char* s = rgba.data() + (size_t)y * w * 4;
        unsigned char* d = pixels.data() + (size_t)y * (row + pad);
        for (int x = 0; x < w; ++x) {
            d[x * 3 + 0] = s[x * 4 + 2];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 0];
        }
    }

    unsigned char header[54] = {0};
    const int file_size = 54 + image_bytes;
    auto put32 = [&](int at, int v) {
        header[at] = (unsigned char)v;
        header[at + 1] = (unsigned char)(v >> 8);
        header[at + 2] = (unsigned char)(v >> 16);
        header[at + 3] = (unsigned char)(v >> 24);
    };
    header[0] = 'B';
    header[1] = 'M';
    put32(2, file_size);
    header[10] = 54;
    header[14] = 40;
    put32(18, w);
    put32(22, h);
    header[26] = 1;
    header[28] = 24;
    put32(34, image_bytes);

    std::string blob((const char*)header, sizeof(header));
    blob.append((const char*)pixels.data(), pixels.size());
    std::string err;
    return sys::write_file(path, blob, err);
}

void on_glfw_error(int code, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, description);
}

void on_drop(GLFWwindow*, int count, const char** paths) {
    std::vector<std::string> list;
    for (int i = 0; i < count; ++i) list.emplace_back(paths[i]);
    if (g_app) g_app->handle_dropped_files(list);
}

void on_content_scale(GLFWwindow*, float, float) { g_fonts_dirty = true; }

bool is_maximized(GLFWwindow* window) {
    return glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
}

} // namespace

int main(int argc, char** argv) {
    // A browser that closes a live-preview tab mid-transfer must not take the
    // whole editor down with it.
    std::signal(SIGPIPE, SIG_IGN);

    glfwSetErrorCallback(on_glfw_error);
#ifdef __APPLE__
    // Stay in the folder the program was started from, even inside a bundle.
    glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);
#endif
    if (!glfwInit()) {
        std::fprintf(stderr, "AI IDE: could not initialise GLFW (is a display available?)\n");
        return 1;
    }

#ifdef __APPLE__
    // The newest context macOS offers is 3.2+ core, and only forward-compatible.
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GLFW_TRUE); // let a laptop stay on the iGPU
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);          // shown once it has been sized

    // Size to the monitor's work area so the window never opens off-screen.
    int work_x = 0, work_y = 0, work_w = 1480, work_h = 940;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
        glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_w, &work_h);
    const int win_w = (work_w * 88) / 100 < 1480 ? (work_w * 88) / 100 : 1480;
    const int win_h = (work_h * 90) / 100 < 940 ? (work_h * 90) / 100 : 940;

    GLFWwindow* window = glfwCreateWindow(win_w > 0 ? win_w : 1280, win_h > 0 ? win_h : 800,
                                          "AI IDE", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "AI IDE: could not create an OpenGL 3 window\n");
        glfwTerminate();
        return 1;
    }
    int actual_w = 0, actual_h = 0;
    glfwGetWindowSize(window, &actual_w, &actual_h);
    glfwSetWindowPos(window, work_x + (work_w - actual_w) / 2, work_y + (work_h - actual_h) / 2);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    Scale scale = window_scale(window);
    glfwSetWindowSizeLimits(window, (int)(760 * scale.ui), (int)(480 * scale.ui), GLFW_DONT_CARE,
                            GLFW_DONT_CARE);
    glfwShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigInputTextCursorBlink = true;

    g_ini_path = sys::join_path(sys::app_data_dir(), "layout.ini");
    io.IniFilename = g_ini_path.c_str();

    ui::apply_style(scale.ui);
    ui::load_fonts(scale.ui, scale.density);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    glfwSetDropCallback(window, on_drop);
    glfwSetWindowContentScaleCallback(window, on_content_scale);

    app::App application;
    g_app = &application;

    // The OS draws the frame, the title and the window buttons here, so the
    // app's own title bar keeps only its menus and status modules.
    app::WindowHooks hooks;
    hooks.native_frame = true;
    hooks.minimize = [window] { glfwIconifyWindow(window); };
    hooks.toggle_maximize = [window] {
        if (is_maximized(window)) glfwRestoreWindow(window);
        else glfwMaximizeWindow(window);
    };
    hooks.close = [window] { glfwSetWindowShouldClose(window, GLFW_TRUE); };
    hooks.is_maximized = [window] { return is_maximized(window); };
    application.init(window, hooks, scale.ui);

    // Command line: any plain argument is a file or folder to open.
    // --screenshot <path> [frames] renders a few frames so the layout settles,
    // saves the framebuffer and quits.
    std::string shot_path;
    int shot_after = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            shot_path = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                shot_after = std::atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            application.open_path(sys::normalize(argv[i]));
        }
    }
    int frame_no = 0;

    bool running = true;
    while (running) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window)) break;

        // Idle cheaply while minimised.
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.1);
            continue;
        }

        // Moving to a monitor with another scale changes one or both numbers.
        const Scale now = window_scale(window);
        if (now.ui != scale.ui || now.density != scale.density) g_fonts_dirty = true;
        if (g_fonts_dirty) {
            g_fonts_dirty = false;
            scale = now;
            ui::apply_style(scale.ui);
            ui::load_fonts(scale.ui, scale.density);
            ImGui_ImplOpenGL3_DestroyFontsTexture(); // rebuilt by the next NewFrame
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        application.draw();

        ImGui::Render();
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.051f, 0.059f, 0.078f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (!shot_path.empty() && ++frame_no >= shot_after) {
            save_framebuffer_bmp(window, shot_path);
            running = false;
        }

        glfwSwapBuffers(window);

        if (application.should_exit()) running = false;
    }

    application.shutdown();
    g_app = nullptr;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

#endif // !_WIN32
