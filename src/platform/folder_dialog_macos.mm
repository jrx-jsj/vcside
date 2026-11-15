// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The folder panel on macOS, with its New Folder button (canCreateDirectories), and the
// project panel, which takes a .vcsproj or a folder that has one.
#include "folder_dialog.h"

#include <string>

#import <AppKit/AppKit.h>

#include "../i18n.h"
#include "../project.h"

// Shows folders (to go into) and .vcsproj files only; a folder is accepted only if it has a project.
@interface VcsideProjectPanelDelegate : NSObject <NSOpenSavePanelDelegate>
@end

@implementation VcsideProjectPanelDelegate
- (BOOL)panel:(id)sender shouldEnableURL:(NSURL*)url {
  NSNumber* isDirectory = nil;
  [url getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
  return isDirectory.boolValue || [url.pathExtension isEqualToString:@"vcsproj"];
}

- (BOOL)panel:(id)sender validateURL:(NSURL*)url error:(NSError**)outError {
  if (!vcs::Project::projectFileAt(url.path.UTF8String).empty()) return YES;
  if (outError) {
    NSString* message = [NSString stringWithUTF8String:vcs::T("This folder has no VCSIDE project (.vcsproj). Choose a project file, or a folder that has one.")];
    *outError = [NSError errorWithDomain:@"VCSIDE" code:1 userInfo:@{NSLocalizedDescriptionKey : message}];
  }
  return NO;
}
@end

namespace vcs {
static NSWindow* parentWindow(SDL_Window* window) {
  return window ? (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr) : nil;
}

static void runPanel(NSOpenPanel* panel, SDL_DialogFileCallback callback, void* userdata, SDL_Window* window, id keepAlive) {
  auto finish = ^(NSModalResponse response) {
    (void)keepAlive;  // the panel's delegate is weak: the block holds it until the panel closes
    if (response == NSModalResponseOK && panel.URL) {
      const char* chosen[] = {panel.URL.path.UTF8String, nullptr};
      callback(userdata, chosen, -1);
    } else {
      const char* none[] = {nullptr};
      callback(userdata, none, -1);
    }
  };
  NSWindow* parent = parentWindow(window);
  if (parent) [panel beginSheetModalForWindow:parent completionHandler:finish];
  else [panel beginWithCompletionHandler:finish];
}

void showProjectDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window) {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseDirectories = YES;
  panel.canChooseFiles = YES;
  panel.allowsMultipleSelection = NO;
  VcsideProjectPanelDelegate* delegate = [VcsideProjectPanelDelegate new];
  panel.delegate = delegate;
  runPanel(panel, callback, userdata, window, delegate);
}

void showFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window, const char* defaultLocation) {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseDirectories = YES;
  panel.canChooseFiles = NO;
  panel.allowsMultipleSelection = NO;
  panel.canCreateDirectories = YES;
  if (defaultLocation && *defaultLocation) panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:defaultLocation] isDirectory:YES];
  runPanel(panel, callback, userdata, window, nil);
}
}  // namespace vcs
