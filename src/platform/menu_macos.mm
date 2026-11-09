// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// macOS: the menus go in the system menu bar. Each menu is rebuilt from the
// MenuBar description when it opens (menuNeedsUpdate:), so labels, ticks and
// enabled states are always current. Shortcuts are only displayed: the main
// menu refuses key equivalents for our menus, so the keys reach SDL and ImGui
// handles them as on the other systems (one place for every shortcut). The
// application and Window menus keep their usual keys (Cmd+Q, Cmd+H, Cmd+M...).
#import <Cocoa/Cocoa.h>

#include <map>

#include "../about.h"
#include "../i18n.h"
#include "../menu.h"

using vcs::MenuItem;

static vcs::NativeMenuCallbacks gCallbacks;
static std::vector<std::function<void()>> gPending;

@interface VCSMainMenu : NSMenu
@property(nonatomic, strong) NSMutableSet<NSMenu*>* systemMenus;  // menus whose keys work natively
@end

@implementation VCSMainMenu
- (BOOL)performKeyEquivalent:(NSEvent*)event {
  for (NSMenu* systemMenu in self.systemMenus)
    if ([systemMenu performKeyEquivalent:event]) return YES;
  return NO;  // our menus: let the key go to the window (SDL -> ImGui)
}
@end

@interface VCSMenuTarget : NSObject <NSMenuDelegate> {
 @public
  std::map<NSInteger, std::vector<std::function<void()>>> actions;  // per top-level menu
}
@end

static VCSMenuTarget* gTarget = nil;

static NSString* ns(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()]; }

// "Ctrl+Shift+S" -> key equivalent + modifier mask.
static void applyShortcut(NSMenuItem* item, const std::string& shortcut) {
  if (shortcut.empty()) return;
  NSEventModifierFlags mask = 0;
  std::string key;
  size_t start = 0;
  while (start <= shortcut.size()) {
    size_t plus = shortcut.find('+', start);
    if (plus == start) plus = shortcut.find('+', start + 1);  // "Ctrl++"
    std::string token = shortcut.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
    if (token == "Ctrl") mask |= NSEventModifierFlagCommand;
    else if (token == "Shift") mask |= NSEventModifierFlagShift;
    else if (token == "Alt") mask |= NSEventModifierFlagOption;
    else key = token;
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  NSString* keyText = nil;
  if (key.size() == 1) {
    keyText = [ns(key) lowercaseString];
  } else if (key.size() >= 2 && key[0] == 'F' && isdigit((unsigned char)key[1])) {
    unichar keyChar = (unichar)(NSF1FunctionKey + std::stoi(key.substr(1)) - 1);
    keyText = [NSString stringWithCharacters:&keyChar length:1];
  } else if (key == "Up" || key == "Down" || key == "Left" || key == "Right") {
    unichar keyChar = key == "Up" ? NSUpArrowFunctionKey : key == "Down" ? NSDownArrowFunctionKey
                    : key == "Left" ? NSLeftArrowFunctionKey : NSRightArrowFunctionKey;
    keyText = [NSString stringWithCharacters:&keyChar length:1];
  }
  if (!keyText) return;
  item.keyEquivalent = keyText;
  item.keyEquivalentModifierMask = mask;
}

static void fill(NSMenu* menu, const std::vector<MenuItem>& items, NSInteger top, bool parentEnabled) {
  auto& menuActions = gTarget->actions[top];
  for (auto& item : items) {
    if (item.separator) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    std::string label = item.label + (item.hint.empty() ? "" : "  (" + item.hint + ")");
    NSMenuItem* menuItem = [[NSMenuItem alloc] initWithTitle:ns(label) action:nil keyEquivalent:@""];
    menuItem.enabled = parentEnabled && item.enabled;
    if (item.checked) menuItem.state = NSControlStateValueOn;
    if (!item.children.empty()) {
      NSMenu* sub = [[NSMenu alloc] initWithTitle:ns(item.label)];
      sub.autoenablesItems = NO;
      fill(sub, item.children, top, menuItem.enabled);
      menuItem.submenu = sub;
    } else {
      applyShortcut(menuItem, item.shortcut);
      if (item.action) {
        menuItem.target = gTarget;
        menuItem.action = @selector(chosen:);
        menuItem.tag = top * 100000 + (NSInteger)menuActions.size();
        menuActions.push_back(item.action);
      }
    }
    [menu addItem:menuItem];
  }
}

static NSMenuItem* systemItem(NSString* title, SEL action, NSString* key, NSEventModifierFlags mask = NSEventModifierFlagCommand) {
  NSMenuItem* menuItem = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
  menuItem.keyEquivalentModifierMask = mask;
  return menuItem;
}

static NSString* appName() {
  NSString* name = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
  return name.length ? name : @"VCSIDE";
}

static void fillAppMenu(NSMenu* menu, const MenuItem& own) {
  [menu removeAllItems];
  gTarget->actions[0].clear();
  NSMenuItem* about = [[NSMenuItem alloc] initWithTitle:ns(vcs::F("About {0}", {appName().UTF8String}))
                                                 action:@selector(about:) keyEquivalent:@""];
  about.target = gTarget;
  [menu addItem:about];
  [menu addItem:[NSMenuItem separatorItem]];
  fill(menu, own.children, 0, true);  // Settings..., Language
  [menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem* services = [[NSMenuItem alloc] initWithTitle:ns(vcs::T("Services")) action:nil keyEquivalent:@""];
  NSMenu* servicesMenu = [[NSMenu alloc] initWithTitle:@""];
  services.submenu = servicesMenu;
  NSApp.servicesMenu = servicesMenu;
  [menu addItem:services];
  [menu addItem:[NSMenuItem separatorItem]];
  [menu addItem:systemItem(ns(vcs::F("Hide {0}", {appName().UTF8String})), @selector(hide:), @"h")];
  [menu addItem:systemItem(ns(vcs::T("Hide Others")), @selector(hideOtherApplications:), @"h",
                           NSEventModifierFlagCommand | NSEventModifierFlagOption)];
  [menu addItem:systemItem(ns(vcs::T("Show All")), @selector(unhideAllApplications:), @"")];
  [menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem* quit = systemItem(ns(vcs::F("Quit {0}", {appName().UTF8String})), @selector(quit:), @"q");
  quit.target = gTarget;
  [menu addItem:quit];
}

static void fillWindowMenu(NSMenu* menu) {
  [menu removeAllItems];
  [menu addItem:systemItem(ns(vcs::T("Minimize")), @selector(performMiniaturize:), @"m")];
  [menu addItem:systemItem(ns(vcs::T("Zoom")), @selector(performZoom:), @"")];
  [menu addItem:[NSMenuItem separatorItem]];
  [menu addItem:systemItem(ns(vcs::T("Bring All to Front")), @selector(arrangeInFront:), @"")];
}

@implementation VCSMenuTarget
- (void)chosen:(NSMenuItem*)item {
  NSInteger top = item.tag / 100000, i = item.tag % 100000;
  auto found = actions.find(top);
  if (found != actions.end() && i < (NSInteger)found->second.size()) gPending.push_back(found->second[i]);
}
- (void)about:(id)sender {
  if (gCallbacks.about) gPending.push_back(gCallbacks.about);
}
- (void)quit:(id)sender {
  if (gCallbacks.quit) gPending.push_back(gCallbacks.quit);
}
- (void)menuNeedsUpdate:(NSMenu*)menu {
  if (!gCallbacks.build) return;
  vcs::MenuBar bar = gCallbacks.build();
  NSInteger index = [NSApp.mainMenu indexOfItemWithSubmenu:menu];
  if (index < 0) return;
  // index 0 is the application menu (bar[0]); the Window menu is last and not ours
  if (index == 0) {
    if (!bar.empty()) fillAppMenu(menu, bar[0]);
    return;
  }
  NSInteger top = index;
  if (top >= (NSInteger)bar.size()) return;
  [menu removeAllItems];
  actions[top].clear();
  fill(menu, bar[top].children, top, bar[top].enabled);
}
@end

namespace vcs {

bool nativeMenuAvailable() { return true; }

void nativeEarlyInit() {
  [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"ApplePersistenceIgnoreState" : @YES,
                                                            @"NSQuitAlwaysKeepsWindows" : @NO}];
}

void nativeMenuRefresh() {
  if (!gTarget || !gCallbacks.build) return;
  MenuBar bar = gCallbacks.build();
  VCSMainMenu* main = [[VCSMainMenu alloc] initWithTitle:@""];
  main.systemMenus = [NSMutableSet set];
  for (size_t i = 0; i < bar.size(); i++) {
    NSString* title = i == 0 ? appName() : ns(bar[i].label);
    NSMenuItem* topItem = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    NSMenu* sub = [[NSMenu alloc] initWithTitle:title];
    sub.autoenablesItems = NO;
    sub.delegate = gTarget;
    topItem.submenu = sub;
    [main addItem:topItem];
    if (i == 0) {
      fillAppMenu(sub, bar[0]);
      sub.autoenablesItems = YES;  // Hide, Show All... are enabled by AppKit
      [main.systemMenus addObject:sub];
    } else {
      [sub removeAllItems];
      gTarget->actions[(NSInteger)i].clear();
      fill(sub, bar[i].children, (NSInteger)i, bar[i].enabled);
    }
  }
  NSMenuItem* windowItem = [[NSMenuItem alloc] initWithTitle:ns(T("Window")) action:nil keyEquivalent:@""];
  NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:ns(T("Window"))];
  fillWindowMenu(windowMenu);
  windowItem.submenu = windowMenu;
  // Window goes before Help, as on macOS
  NSInteger helpIndex = main.numberOfItems - 1;
  [main insertItem:windowItem atIndex:helpIndex > 0 ? helpIndex : main.numberOfItems];
  [main.systemMenus addObject:windowMenu];
  NSApp.mainMenu = main;
  NSApp.windowsMenu = windowMenu;
  NSApp.helpMenu = [main itemAtIndex:main.numberOfItems - 1].submenu;
}

void nativeMenuInstall(const NativeMenuCallbacks& callbacks) {
  gCallbacks = callbacks;
  if (!gTarget) gTarget = [[VCSMenuTarget alloc] init];
  nativeMenuRefresh();
}

std::vector<std::function<void()>> nativeMenuTakeActions() {
  std::vector<std::function<void()>> out;
  out.swap(gPending);
  return out;
}

static void dumpMenu(NSMenu* menu, int depth, std::string& out) {
  if (menu.delegate == gTarget) [gTarget menuNeedsUpdate:menu];
  for (NSMenuItem* menuItem in menu.itemArray) {
    out += std::string(depth * 2, ' ');
    if (menuItem.isSeparatorItem) {
      out += "----\n";
      continue;
    }
    out += menuItem.title.UTF8String;
    if (menuItem.keyEquivalent.length) {
      std::string mods;
      NSEventModifierFlags flags = menuItem.keyEquivalentModifierMask;
      if (flags & NSEventModifierFlagCommand) mods += "Cmd+";
      if (flags & NSEventModifierFlagOption) mods += "Opt+";
      if (flags & NSEventModifierFlagShift) mods += "Shift+";
      unichar keyChar = [menuItem.keyEquivalent characterAtIndex:0];
      std::string key = keyChar >= NSF1FunctionKey && keyChar <= NSF35FunctionKey ? "F" + std::to_string(keyChar - NSF1FunctionKey + 1)
                        : keyChar == NSUpArrowFunctionKey ? "Up" : keyChar == NSDownArrowFunctionKey ? "Down"
                        : menuItem.keyEquivalent.uppercaseString.UTF8String;
      out += "  [" + mods + key + "]";
    }
    if (menuItem.state == NSControlStateValueOn) out += "  (checked)";
    if (!menuItem.isEnabled && !menuItem.hasSubmenu) out += "  (disabled)";
    out += "\n";
    if (menuItem.hasSubmenu) dumpMenu(menuItem.submenu, depth + 1, out);
  }
}

std::string nativeMenuDump() {
  std::string out;
  for (NSMenuItem* top in NSApp.mainMenu.itemArray) {
    out += std::string("[") + top.title.UTF8String + "]\n";
    dumpMenu(top.submenu, 1, out);
  }
  return out;
}

bool nativeMenuPerform(const std::string& path) {
  NSMenu* menu = NSApp.mainMenu;
  NSMenuItem* found = nil;
  size_t start = 0;
  while (menu) {
    size_t slash = path.find('/', start);
    std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (menu.delegate == gTarget) [gTarget menuNeedsUpdate:menu];
    found = nil;
    for (NSMenuItem* menuItem in menu.itemArray)
      if (!menuItem.isSeparatorItem && std::string(menuItem.title.UTF8String).rfind(part, 0) == 0) {
        found = menuItem;
        break;
      }
    if (!found) return false;
    if (slash == std::string::npos) break;
    menu = found.submenu;
    start = slash + 1;
  }
  if (!found || !found.isEnabled || !found.action) return false;
  NSInteger index = [found.menu indexOfItem:found];
  [found.menu performActionForItemAtIndex:index];
  return true;
}

}  // namespace vcs

@interface VCSAboutTarget : NSObject {
 @public
  NSPanel* __weak panel;
}
@end
@implementation VCSAboutTarget
- (void)close:(id)sender {
  [panel close];
}
@end

// The About panel: application icon, name, version and copyright, and the
// credits (scrollable) built from the same sections as the ImGui window.
bool vcs::nativeShowAbout(const vcs::AboutInfo& info) {
  @autoreleasepool {
    NSMutableAttributedString* credits = [[NSMutableAttributedString alloc] init];
    NSMutableParagraphStyle* para = [[NSMutableParagraphStyle alloc] init];
    para.paragraphSpacing = 2;
    NSDictionary* body = @{NSFontAttributeName : [NSFont systemFontOfSize:11],
                           NSForegroundColorAttributeName : NSColor.labelColor,
                           NSParagraphStyleAttributeName : para};
    NSDictionary* dim = @{NSFontAttributeName : [NSFont systemFontOfSize:10],
                          NSForegroundColorAttributeName : NSColor.secondaryLabelColor,
                          NSParagraphStyleAttributeName : para};
    NSDictionary* head = @{NSFontAttributeName : [NSFont boldSystemFontOfSize:12],
                           NSForegroundColorAttributeName : NSColor.labelColor};
    auto add = [&](const std::string& text, NSDictionary* attrs) {
      [credits appendAttributedString:[[NSAttributedString alloc] initWithString:ns(text) attributes:attrs]];
    };
    add(info.tagline + "\n", body);
    add(info.credit + "\n", dim);
    for (auto& section : info.sections) {
      add("\n" + section.title + "\n", head);
      if (!section.note.empty()) add(section.note + "\n", dim);
      for (auto& line : section.lines) {
        if (!line.url.empty()) {
          NSMutableDictionary* link = [body mutableCopy];
          link[NSLinkAttributeName] = [NSURL URLWithString:ns(line.url)];
          [credits appendAttributedString:[[NSAttributedString alloc] initWithString:ns(line.name) attributes:link]];
        } else {
          add(line.name, body);
        }
        add(line.detail.empty() ? "\n" : " — " + line.detail + "\n", line.detail.empty() ? body : dim);
      }
    }
    add("\n" + vcs::F("Data: {0}", {info.dataDir}) + "\n", dim);
    // A panel of our own rather than the standard one, which has no button to close it.
    static NSPanel* panel = nil;
    static VCSAboutTarget* target = nil;
    if (!target) target = [[VCSAboutTarget alloc] init];
    if (panel) [panel close];
    const CGFloat panelWidth = 440, margin = 24, inner = panelWidth - 2 * margin;
    panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, panelWidth, 600)
                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
    panel.title = ns(vcs::T("About VCSIDE"));
    panel.releasedWhenClosed = NO;
    target->panel = panel;

    auto label = [&](NSString* text, NSFont* font, NSColor* color) {
      NSTextField* field = [NSTextField wrappingLabelWithString:text];
      field.font = font;
      field.textColor = color;
      field.alignment = NSTextAlignmentCenter;
      field.preferredMaxLayoutWidth = inner;
      return field;
    };
    NSImageView* icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
    [icon.widthAnchor constraintEqualToConstant:112].active = YES;
    [icon.heightAnchor constraintEqualToConstant:112].active = YES;
    NSTextField* name = label(@"VCSIDE", [NSFont boldSystemFontOfSize:22], NSColor.labelColor);
    NSTextField* version = label(ns(vcs::F("Version {0}", {info.version})), [NSFont systemFontOfSize:12], NSColor.secondaryLabelColor);

    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, inner, 280)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSLineBorder;
    scroll.drawsBackground = NO;
    NSTextView* text = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, inner, 280)];
    text.editable = NO;
    text.selectable = YES;
    text.drawsBackground = NO;
    text.textContainerInset = NSMakeSize(6, 8);
    text.verticallyResizable = YES;
    text.autoresizingMask = NSViewWidthSizable;
    text.textContainer.widthTracksTextView = YES;
    [text.textStorage setAttributedString:credits];
    scroll.documentView = text;
    [scroll.widthAnchor constraintEqualToConstant:inner].active = YES;
    [scroll.heightAnchor constraintEqualToConstant:280].active = YES;

    NSTextField* copyright = label(ns(info.copyright), [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    NSButton* close = [NSButton buttonWithTitle:ns(vcs::T("Close")) target:target action:@selector(close:)];
    close.keyEquivalent = @"\r";  // Return; Esc closes the panel too (cancelOperation)
    close.bezelStyle = NSBezelStyleRounded;

    NSStackView* stack = [NSStackView stackViewWithViews:@[ icon, name, version, scroll, copyright, close ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 8;
    [stack setCustomSpacing:14 afterView:version];
    [stack setCustomSpacing:14 afterView:scroll];
    stack.edgeInsets = NSEdgeInsetsMake(margin, margin, margin, margin);
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    NSView* content = panel.contentView;
    [content addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
      [stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
      [stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
      [stack.topAnchor constraintEqualToAnchor:content.topAnchor],
      [stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
    [panel layoutIfNeeded];
    [panel center];
    [panel makeKeyAndOrderFront:nil];
    [panel makeFirstResponder:close];
    [NSApp activateIgnoringOtherApps:YES];
  }
  return true;
}

