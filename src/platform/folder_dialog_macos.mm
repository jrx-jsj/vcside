// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The folder panel on macOS, with its New Folder button (canCreateDirectories).
#include "folder_dialog.h"

#import <AppKit/AppKit.h>

namespace vcs {
void showFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window, const char* defaultLocation) {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseDirectories = YES;
  panel.canChooseFiles = NO;
  panel.allowsMultipleSelection = NO;
  panel.canCreateDirectories = YES;
  if (defaultLocation && *defaultLocation) panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:defaultLocation] isDirectory:YES];
  auto finish = ^(NSModalResponse response) {
    if (response == NSModalResponseOK && panel.URL) {
      const char* chosen[] = {panel.URL.path.UTF8String, nullptr};
      callback(userdata, chosen, -1);
    } else {
      const char* none[] = {nullptr};
      callback(userdata, none, -1);
    }
  };
  NSWindow* parent = window ? (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr) : nil;
  if (parent) [panel beginSheetModalForWindow:parent completionHandler:finish];
  else [panel beginWithCompletionHandler:finish];
}
}  // namespace vcs
