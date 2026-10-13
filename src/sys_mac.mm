// macOS file panels for the platform layer (sys_posix.cpp). Built on macOS only.
#import <Cocoa/Cocoa.h>

#include <string>

namespace sys {

namespace {

std::string run_panel(NSSavePanel* panel) {
    std::string result;
    // The panel is modal on the main thread, which is where the frame loop
    // calls from; the window gets its focus back when it closes.
    if ([panel runModal] == NSModalResponseOK && panel.URL)
        result = panel.URL.path.UTF8String ? panel.URL.path.UTF8String : "";
    return result;
}

NSURL* folder_url(const std::string& dir) {
    if (dir.empty()) return nil;
    return [NSURL fileURLWithPath:[NSString stringWithUTF8String:dir.c_str()] isDirectory:YES];
}

} // namespace

std::string mac_pick_folder(const std::string& title) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.canCreateDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.message = [NSString stringWithUTF8String:title.c_str()];
        panel.prompt = @"Open";
        return run_panel(panel);
    }
}

std::string mac_pick_open_file(const std::string& start_dir) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if (NSURL* dir = folder_url(start_dir)) panel.directoryURL = dir;
        return run_panel(panel);
    }
}

std::string mac_pick_save_file(const std::string& suggested_name, const std::string& start_dir) {
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.canCreateDirectories = YES;
        if (!suggested_name.empty())
            panel.nameFieldStringValue = [NSString stringWithUTF8String:suggested_name.c_str()];
        if (NSURL* dir = folder_url(start_dir)) panel.directoryURL = dir;
        return run_panel(panel);
    }
}

} // namespace sys
