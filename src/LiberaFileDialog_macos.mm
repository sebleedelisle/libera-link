#include "LiberaFileDialog.h"

#import <AppKit/AppKit.h>

namespace libera::ui {

std::string OpenFileDialog(const char* title,
                           const std::vector<std::string>& extensions) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        if (title && *title) {
            [panel setMessage:[NSString stringWithUTF8String:title]];
        }
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setResolvesAliases:YES];

        if (!extensions.empty()) {
            NSMutableArray<NSString*>* allowedTypes = [NSMutableArray array];
            for (const auto& extension : extensions) {
                [allowedTypes addObject:[NSString stringWithUTF8String:extension.c_str()]];
            }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [panel setAllowedFileTypes:allowedTypes];
#pragma clang diagnostic pop
        }

        if ([panel runModal] != NSModalResponseOK) {
            return {};
        }

        NSURL* url = [[panel URLs] firstObject];
        if (!url) {
            return {};
        }
        return std::string([[url path] UTF8String]);
    }
}

} // namespace libera::ui
