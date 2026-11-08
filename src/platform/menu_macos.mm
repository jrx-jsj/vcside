// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
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
  for (NSMenu* m in self.systemMenus)
    if ([m performKeyEquivalent:event]) return YES;
  return NO;  // our menus: let the key go to the window (SDL -> ImGui)
}
@end

@interface VCSMenuTarget : NSObject <NSMenuDelegate> {
 @public
  std::map<NSInteger, std::vector<std::function<void()>>> actions;  // per top-level menu
}
@end

static VCSMenuTarget* gTarget = nil;

static NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

// "Ctrl+Shift+S" -> key equivalent + modifier mask.
static void applyShortcut(NSMenuItem* item, const std::string& sc) {
  if (sc.empty()) return;
  NSEventModifierFlags mask = 0;
  std::string key;
  size_t start = 0;
  while (start <= sc.size()) {
    size_t plus = sc.find('+', start);
    if (plus == start) plus = sc.find('+', start + 1);  // "Ctrl++"
    std::string tok = sc.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
    if (tok == "Ctrl") mask |= NSEventModifierFlagCommand;
    else if (tok == "Shift") mask |= NSEventModifierFlagShift;
    else if (tok == "Alt") mask |= NSEventModifierFlagOption;
    else key = tok;
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  NSString* k = nil;
  if (key.size() == 1) {
    k = [ns(key) lowercaseString];
  } else if (key.size() >= 2 && key[0] == 'F' && isdigit((unsigned char)key[1])) {
    unichar c = (unichar)(NSF1FunctionKey + std::stoi(key.substr(1)) - 1);
    k = [NSString stringWithCharacters:&c length:1];
  } else if (key == "Up" || key == "Down" || key == "Left" || key == "Right") {
    unichar c = key == "Up" ? NSUpArrowFunctionKey : key == "Down" ? NSDownArrowFunctionKey
              : key == "Left" ? NSLeftArrowFunctionKey : NSRightArrowFunctionKey;
    k = [NSString stringWithCharacters:&c length:1];
  }
  if (!k) return;
  item.keyEquivalent = k;
  item.keyEquivalentModifierMask = mask;
}

static void fill(NSMenu* menu, const std::vector<MenuItem>& items, NSInteger top, bool parentEnabled) {
  auto& acts = gTarget->actions[top];
  for (auto& m : items) {
    if (m.separator) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    std::string label = m.label + (m.hint.empty() ? "" : "  (" + m.hint + ")");
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:ns(label) action:nil keyEquivalent:@""];
    it.enabled = parentEnabled && m.enabled;
    if (m.checked) it.state = NSControlStateValueOn;
    if (!m.children.empty()) {
      NSMenu* sub = [[NSMenu alloc] initWithTitle:ns(m.label)];
      sub.autoenablesItems = NO;
      fill(sub, m.children, top, it.enabled);
      it.submenu = sub;
    } else {
      applyShortcut(it, m.shortcut);
      if (m.action) {
        it.target = gTarget;
        it.action = @selector(chosen:);
        it.tag = top * 100000 + (NSInteger)acts.size();
        acts.push_back(m.action);
      }
    }
    [menu addItem:it];
  }
}

static NSMenuItem* systemItem(NSString* title, SEL action, NSString* key, NSEventModifierFlags mask = NSEventModifierFlagCommand) {
  NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
  it.keyEquivalentModifierMask = mask;
  return it;
}

static NSString* appName() {
  NSString* n = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
  return n.length ? n : @"VCSIDE";
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
  auto it = actions.find(top);
  if (it != actions.end() && i < (NSInteger)it->second.size()) gPending.push_back(it->second[i]);
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

void nativeMenuInstall(const NativeMenuCallbacks& cb) {
  gCallbacks = cb;
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
  for (NSMenuItem* it in menu.itemArray) {
    out += std::string(depth * 2, ' ');
    if (it.isSeparatorItem) {
      out += "----\n";
      continue;
    }
    out += it.title.UTF8String;
    if (it.keyEquivalent.length) {
      std::string mods;
      NSEventModifierFlags f = it.keyEquivalentModifierMask;
      if (f & NSEventModifierFlagCommand) mods += "Cmd+";
      if (f & NSEventModifierFlagOption) mods += "Opt+";
      if (f & NSEventModifierFlagShift) mods += "Shift+";
      unichar c = [it.keyEquivalent characterAtIndex:0];
      std::string key = c >= NSF1FunctionKey && c <= NSF35FunctionKey ? "F" + std::to_string(c - NSF1FunctionKey + 1)
                        : c == NSUpArrowFunctionKey ? "Up" : c == NSDownArrowFunctionKey ? "Down"
                        : it.keyEquivalent.uppercaseString.UTF8String;
      out += "  [" + mods + key + "]";
    }
    if (it.state == NSControlStateValueOn) out += "  (checked)";
    if (!it.isEnabled && !it.hasSubmenu) out += "  (disabled)";
    out += "\n";
    if (it.hasSubmenu) dumpMenu(it.submenu, depth + 1, out);
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
    for (NSMenuItem* it in menu.itemArray)
      if (!it.isSeparatorItem && std::string(it.title.UTF8String).rfind(part, 0) == 0) {
        found = it;
        break;
      }
    if (!found) return false;
    if (slash == std::string::npos) break;
    menu = found.submenu;
    start = slash + 1;
  }
  if (!found || !found.isEnabled || !found.action) return false;
  NSInteger idx = [found.menu indexOfItem:found];
  [found.menu performActionForItemAtIndex:idx];
  return true;
}

}  // namespace vcs

// The system's About panel: application icon, name, version and copyright, and the
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
    for (auto& sec : info.sections) {
      add("\n" + sec.title + "\n", head);
      if (!sec.note.empty()) add(sec.note + "\n", dim);
      for (auto& l : sec.lines) {
        if (!l.url.empty()) {
          NSMutableDictionary* link = [body mutableCopy];
          link[NSLinkAttributeName] = [NSURL URLWithString:ns(l.url)];
          [credits appendAttributedString:[[NSAttributedString alloc] initWithString:ns(l.name) attributes:link]];
        } else {
          add(l.name, body);
        }
        add(l.detail.empty() ? "\n" : " — " + l.detail + "\n", l.detail.empty() ? body : dim);
      }
    }
    add("\n" + vcs::F("Data: {0}", {info.dataDir}) + "\n", dim);
    NSDictionary* opts = @{
      NSAboutPanelOptionApplicationName : @"VCSIDE",
      NSAboutPanelOptionApplicationVersion : ns(info.version),
      NSAboutPanelOptionVersion : @"",
      NSAboutPanelOptionCredits : credits,
      @"Copyright" : ns(info.copyright),
    };
    [NSApp orderFrontStandardAboutPanelWithOptions:opts];
    [NSApp activateIgnoringOtherApps:YES];
  }
  return true;
}

