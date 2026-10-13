// Entry point for macOS: a GLFW window drawn with Metal, and the ImGui loop.
// main.cpp is the Win32/Direct3D 11 version, main_glfw.cpp the Linux/OpenGL
// one. Metal rather than OpenGL because OpenGL is deprecated on macOS, and
// virtual Macs (the release build's own test machine among them) have no
// OpenGL renderer at all.
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app.h"
#include "bmp.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_metal.h"
#include "sys.h"
#include "ui.h"

namespace {

app::App* g_app = nullptr;
bool g_fonts_dirty = false;
std::string g_ini_path;

// The window is measured in points and a Retina screen has two pixels per
// point, so layout stays at 1x and only the glyphs are rasterized denser.
void load_fonts_for(GLFWwindow* window) {
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window, &sx, &sy);
    ui::apply_style(1.0f);
    ui::set_font_density(sx > 0.0f ? sx : 1.0f);
    ui::load_fonts(1.0f);
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

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        std::fprintf(stderr, "This Mac has no Metal device.\n");
        glfwTerminate();
        return 1;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1440, 900, "Cortex", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Could not open a window.\n");
        glfwTerminate();
        return 1;
    }

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

    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplMetal_Init(device);

    NSWindow* nswin = glfwGetCocoaWindow(window);
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO; // --screenshot copies the drawable back
    nswin.contentView.layer = layer;
    nswin.contentView.wantsLayer = YES;

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor new];

    app::App application;
    g_app = &application;

    // macOS draws the window frame, traffic lights included, so the app's own
    // title bar leaves those out.
    app::WindowHooks hooks;
    hooks.native_frame = true;
    hooks.minimize = [window] { glfwIconifyWindow(window); };
    hooks.toggle_maximize = [window] {
        if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) glfwRestoreWindow(window);
        else glfwMaximizeWindow(window);
    };
    hooks.close = [window] { glfwSetWindowShouldClose(window, GLFW_TRUE); };
    hooks.is_maximized = [window] { return glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0; };
    application.init(window, hooks, 1.0f);

    glfwSetDropCallback(window, [](GLFWwindow*, int count, const char** paths) {
        if (!g_app) return;
        std::vector<std::string> list;
        for (int i = 0; i < count; ++i) list.push_back(paths[i]);
        g_app->handle_dropped_files(list);
    });
    // Moving the window to a screen with another density rebuilds the fonts.
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow*, float, float) { g_fonts_dirty = true; });

    // Command line: any plain argument is a file or folder to open (Finder on
    // old systems adds a -psn_ argument; like every dash argument it is
    // skipped). --screenshot <path> [frames] renders that many frames, saves
    // the window's pixels and quits.
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
        @autoreleasepool {
            glfwPollEvents();
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                glfwWaitEventsTimeout(0.1); // minimized: nothing to draw
                continue;
            }

            if (ui::take_font_rebuild()) g_fonts_dirty = true; // editor zoom
            if (g_fonts_dirty) {
                g_fonts_dirty = false;
                load_fonts_for(window);
                ImGui_ImplMetal_DestroyFontsTexture();
                ImGui_ImplMetal_CreateFontsTexture(device);
            }

            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            if (width <= 0 || height <= 0) {
                glfwWaitEventsTimeout(0.1);
                continue;
            }
            layer.drawableSize = CGSizeMake(width, height);
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) continue;

            id<MTLCommandBuffer> commands = [queue commandBuffer];
            pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 1.0, 1.0, 1.0);
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];

            ImGui_ImplMetal_NewFrame(pass);
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            application.draw();

            ImGui::Render();
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commands, encoder);
            [encoder endEncoding];

            // On the screenshot frame the drawable is copied into a buffer the
            // CPU can read before it goes to the screen.
            id<MTLBuffer> readback = nil;
            const bool shoot = !shot_path.empty() && ++frame_no >= shot_after;
            if (shoot) {
                const NSUInteger row = (NSUInteger)width * 4;
                readback = [device newBufferWithLength:row * (NSUInteger)height
                                               options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                [blit copyFromTexture:drawable.texture
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(0, 0, 0)
                                  sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                                    toBuffer:readback
                           destinationOffset:0
                      destinationBytesPerRow:row
                    destinationBytesPerImage:row * (NSUInteger)height];
                [blit endEncoding];
            }

            [commands presentDrawable:drawable];
            [commands commit];

            if (shoot) {
                [commands waitUntilCompleted];
                if (!bmp::save(shot_path, width, height, (const unsigned char*)readback.contents, true, true))
                    std::fprintf(stderr, "could not write %s\n", shot_path.c_str());
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            if (application.should_exit()) glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }

    application.shutdown();
    g_app = nullptr;

    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
