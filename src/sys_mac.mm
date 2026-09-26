// macOS file dialogs: NSOpenPanel and NSSavePanel, run modally over the app.
#include <string>

#import <AppKit/AppKit.h>

namespace sys {

// mode: 0 = open file, 1 = save file, 2 = pick folder. Empty when cancelled.
std::string mac_file_dialog(int mode, const std::string& start_dir,
                            const std::string& suggested_name, const std::string& title) {
    std::string result;
    @autoreleasepool {
        NSSavePanel* panel = nil;
        if (mode == 1) {
            panel = [NSSavePanel savePanel];
            if (!suggested_name.empty())
                panel.nameFieldStringValue = [NSString stringWithUTF8String:suggested_name.c_str()];
        } else {
            NSOpenPanel* open = [NSOpenPanel openPanel];
            open.canChooseFiles = mode == 0 ? YES : NO;
            open.canChooseDirectories = mode == 2 ? YES : NO;
            open.allowsMultipleSelection = NO;
            open.resolvesAliases = YES;
            panel = open;
        }
        panel.canCreateDirectories = YES;
        panel.showsHiddenFiles = NO;
        if (!title.empty()) {
            NSString* t = [NSString stringWithUTF8String:title.c_str()];
            panel.title = t;
            panel.message = t; // the title bar is hidden on recent macOS; the message is not
        }
        if (!start_dir.empty())
            panel.directoryURL =
                [NSURL fileURLWithPath:[NSString stringWithUTF8String:start_dir.c_str()]
                           isDirectory:YES];

        if ([panel runModal] == NSModalResponseOK && panel.URL && panel.URL.path)
            result = panel.URL.path.UTF8String;
    }
    return result;
}

} // namespace sys
