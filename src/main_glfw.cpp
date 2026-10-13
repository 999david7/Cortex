// Entry point for Linux: a GLFW window, an OpenGL 3 context and the ImGui
// loop. main.cpp is the Win32/Direct3D 11 version, main_mac.mm the macOS
// GLFW/Metal one.
#if !defined(_WIN32) && !defined(__APPLE__)

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app.h"
#include "bmp.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_opengl3_loader.h"
#include "sys.h"
#include "ui.h"

namespace {

app::App* g_app = nullptr;
bool g_fonts_dirty = false;
std::string g_ini_path;

// X11 measures windows in pixels, so a high-density screen's content scale
// enlarges the layout itself (macOS works in points instead; see main_mac.mm).
void font_metrics(GLFWwindow* window, float& layout_scale, float& density) {
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window, &sx, &sy);
    layout_scale = sx > 0.0f ? sx : 1.0f;
    density = 1.0f;
}

void load_fonts_for(GLFWwindow* window) {
    float layout = 1.0f, density = 1.0f;
    font_metrics(window, layout, density);
    ui::apply_style(layout);
    ui::set_font_density(density);
    ui::load_fonts(layout);
}

// The same 24-bit BMP the Windows build writes for --screenshot, read back
// from the OpenGL framebuffer.
bool save_framebuffer_bmp(const std::string& path, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    std::vector<unsigned char> rgba((size_t)w * h * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return bmp::save(path, w, h, rgba.data(), false, false); // GL rows run bottom-up
}

void on_glfw_error(int code, const char* message) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, message ? message : "");
}

} // namespace

int main(int argc, char** argv) {
    // A browser that hangs up on the live server must not take the IDE down.
    std::signal(SIGPIPE, SIG_IGN);

    glfwSetErrorCallback(on_glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "Could not start the window system.\n");
        return 1;
    }

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(1440, 900, "Cortex", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Could not open a window with OpenGL 3.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigInputTextCursorBlink = true;

    g_ini_path = sys::join_path(sys::app_data_dir(), "layout.ini");
    io.IniFilename = g_ini_path.c_str();

    load_fonts_for(window);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    app::App application;
    g_app = &application;
    glfwSetWindowUserPointer(window, &application);

    // The system draws the window frame here, title bar and buttons included,
    // so the app's own title bar leaves those out.
    app::WindowHooks hooks;
    hooks.native_frame = true;
    hooks.minimize = [window] { glfwIconifyWindow(window); };
    hooks.toggle_maximize = [window] {
        if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) glfwRestoreWindow(window);
        else glfwMaximizeWindow(window);
    };
    hooks.close = [window] { glfwSetWindowShouldClose(window, GLFW_TRUE); };
    hooks.is_maximized = [window] { return glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0; };
    float layout_scale = 1.0f, density = 1.0f;
    font_metrics(window, layout_scale, density);
    application.init(window, hooks, layout_scale);

    glfwSetDropCallback(window, [](GLFWwindow*, int count, const char** paths) {
        if (!g_app) return;
        std::vector<std::string> list;
        for (int i = 0; i < count; ++i) list.push_back(paths[i]);
        g_app->handle_dropped_files(list);
    });
    // Moving the window to a screen with another density rebuilds the fonts.
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow*, float, float) { g_fonts_dirty = true; });

    // Command line: any plain argument is a file or folder to open.
    // --screenshot <path> [frames] renders that many frames, saves the
    // framebuffer and quits.
    std::string shot_path;
    int shot_after = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            shot_path = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') shot_after = std::atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            application.open_path(argv[i]);
        }
    }
    int frame_no = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.1); // minimized: nothing to draw
            continue;
        }

        if (ui::take_font_rebuild()) g_fonts_dirty = true; // editor zoom
        if (g_fonts_dirty) {
            g_fonts_dirty = false;
            load_fonts_for(window);
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        application.draw();

        ImGui::Render();
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (!shot_path.empty() && ++frame_no >= shot_after) {
            if (!save_framebuffer_bmp(shot_path, fb_w, fb_h))
                std::fprintf(stderr, "could not write %s\n", shot_path.c_str());
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        glfwSwapBuffers(window);
        if (application.should_exit()) glfwSetWindowShouldClose(window, GLFW_TRUE);
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

#endif // Linux
