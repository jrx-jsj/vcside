// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// VCSIDE entry point: SDL3 window + OpenGL + Dear ImGui.
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "menu.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "paths.h"
#include "window_icon.h"

namespace fs = std::filesystem;

// Keeps SDL text input on all the time: the code editor is not an ImGui
// InputText, so ImGui would otherwise switch text events off.
static void keepTextInput(ImGuiContext*, ImGuiViewport* vp, ImGuiPlatformImeData* data) {
  SDL_Window* w = SDL_GetWindowFromID((SDL_WindowID)(intptr_t)vp->PlatformHandle);
  if (!w) return;
  SDL_Rect r = {(int)data->InputPos.x, (int)data->InputPos.y, 1, (int)data->InputLineHeight};
  SDL_SetTextInputArea(w, &r, 0);
  if (!SDL_TextInputActive(w)) SDL_StartTextInput(w);
}

// --input FILE for test runs: one action per line, played one per frame after the window
// has settled. Coordinates are window points (what ImGui uses).
//   move X Y | down [right] | up [right] | click X Y [right] | drag X1 Y1 X2 Y2 [right]
//   key NAME [shift|ctrl|alt...] (NAME: a-z, z, y, enter, escape, tab, delete...)
//   hold shift|ctrl|alt | release shift|ctrl|alt | text WORDS | wait N | shot FILE.bmp
//   scroll DY (wheel steps; negative = down)
struct InputScript {
  std::vector<std::string> lines;
  size_t next = 0;
  int waitFrames = 0;
  float x = 0, y = 0;
  std::vector<std::pair<float, float>> path;  // a drag in progress, one point a frame
  int dragButton = 0;
  std::string shot;  // set by "shot": saved after this frame is drawn
  bool done() const { return next >= lines.size() && path.empty() && waitFrames == 0; }

  static ImGuiKey keyNamed(const std::string& n) {
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return (ImGuiKey)(ImGuiKey_A + (n[0] - 'a'));
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return (ImGuiKey)(ImGuiKey_0 + (n[0] - '0'));
    if (n == "enter") return ImGuiKey_Enter;
    if (n == "escape") return ImGuiKey_Escape;
    if (n == "tab") return ImGuiKey_Tab;
    if (n == "delete") return ImGuiKey_Delete;
    if (n == "backspace") return ImGuiKey_Backspace;
    if (n == "up") return ImGuiKey_UpArrow;
    if (n == "down") return ImGuiKey_DownArrow;
    if (n == "left") return ImGuiKey_LeftArrow;
    if (n == "right") return ImGuiKey_RightArrow;
    return ImGuiKey_None;
  }
  static ImGuiKey modNamed(const std::string& n) {
    return n == "shift" ? ImGuiMod_Shift : n == "ctrl" ? ImGuiMod_Ctrl : n == "alt" ? ImGuiMod_Alt : n == "cmd" ? ImGuiMod_Super : ImGuiKey_None;
  }

  // Queues this frame's input; call between the backend's NewFrame and ImGui::NewFrame,
  // so it wins over the real mouse.
  void step(ImGuiIO& io) {
    if (!path.empty()) {
      x = path.front().first, y = path.front().second;
      path.erase(path.begin());
      io.AddMousePosEvent(x, y);
      if (path.empty()) io.AddMouseButtonEvent(dragButton, false);
      return;
    }
    io.AddMousePosEvent(x, y);
    if (waitFrames > 0) {
      waitFrames--;
      return;
    }
    if (next >= lines.size()) return;
    std::istringstream in(lines[next++]);
    std::string op;
    in >> op;
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    auto num = [&](size_t i) { return i < a.size() ? std::stof(a[i]) : 0.0f; };
    int button = !a.empty() && a.back() == "right" ? 1 : 0;
    if (op == "move") x = num(0), y = num(1), io.AddMousePosEvent(x, y);
    else if (op == "down") io.AddMouseButtonEvent(button, true);
    else if (op == "up") io.AddMouseButtonEvent(button, false);
    else if (op == "click") {
      x = num(0), y = num(1);
      io.AddMousePosEvent(x, y);
      io.AddMouseButtonEvent(button, true);
      path = {{x, y}};  // released on the next frame
      dragButton = button;
    } else if (op == "drag") {
      x = num(0), y = num(1);
      io.AddMousePosEvent(x, y);
      io.AddMouseButtonEvent(button, true);
      float x2 = num(2), y2 = num(3);
      int steps = 12;
      for (int i = 1; i <= steps; i++) path.push_back({x + (x2 - x) * i / steps, y + (y2 - y) * i / steps});
      path.push_back({x2, y2});
      dragButton = button;
    } else if (op == "hold" || op == "release") {
      io.AddKeyEvent(modNamed(a.empty() ? "" : a[0]), op == "hold");
    } else if (op == "key") {
      ImGuiKey k = keyNamed(a.empty() ? "" : a[0]);
      for (size_t i = 1; i < a.size(); i++) io.AddKeyEvent(modNamed(a[i]), true);
      io.AddKeyEvent(k, true);
      io.AddKeyEvent(k, false);
      for (size_t i = 1; i < a.size(); i++) io.AddKeyEvent(modNamed(a[i]), false);
    } else if (op == "scan" && !a.empty()) {  // a physical key, as the keyboard sends it: "scan 56 cmd"
      SDL_Event e{};
      e.type = SDL_EVENT_KEY_DOWN;
      e.key.scancode = (SDL_Scancode)std::stoi(a[0]);
      for (size_t i = 1; i < a.size(); i++)
        e.key.mod |= a[i] == "cmd" ? SDL_KMOD_GUI : a[i] == "ctrl" ? SDL_KMOD_CTRL : a[i] == "shift" ? SDL_KMOD_SHIFT : 0;
      e.key.key = SDL_GetKeyFromScancode(e.key.scancode, SDL_KMOD_NONE, true);
      int nw = 0;
      if (SDL_Window** ws = SDL_GetWindows(&nw)) {
        if (nw > 0) e.key.windowID = SDL_GetWindowID(ws[0]);
        SDL_free(ws);
      }
      e.key.down = true;
      SDL_PushEvent(&e);
      SDL_Event up = e;
      up.type = SDL_EVENT_KEY_UP, up.key.down = false;
      SDL_PushEvent(&up);
    } else if (op == "char" && !a.empty()) {  // a key by the character it types, as SDL sends it: "char + cmd"
      SDL_Event e{};
      e.type = SDL_EVENT_KEY_DOWN;
      e.key.key = (SDL_Keycode)(unsigned char)a[0][0];
      for (size_t i = 1; i < a.size(); i++)
        e.key.mod |= a[i] == "cmd" ? SDL_KMOD_GUI : a[i] == "ctrl" ? SDL_KMOD_CTRL : a[i] == "shift" ? SDL_KMOD_SHIFT : 0;
      e.key.down = true;
      SDL_PushEvent(&e);
    } else if (op == "text") {
      std::string t = lines[next - 1].substr(lines[next - 1].find("text") + 5);
      io.AddInputCharactersUTF8(t.c_str());
    } else if (op == "scroll") {
      io.AddMouseWheelEvent(0, num(0));
    } else if (op == "wait") {
      waitFrames = (int)num(0);
    } else if (op == "shot" && !a.empty()) {
      shot = a[0];
    }
  }
};

// Shortcuts by character, whatever the keyboard layout. ImGui's SDL backend reads
// punctuation by its place on a US keyboard, so on a Spanish one Cmd+"-" would be
// Cmd+"/" and "/" (Shift+7) could not be reached. Before the backend sees a digit or
// punctuation key, the event is rewritten as the US key that types the same character;
// a character with no such key reaches ImGui as no key at all. Text typing comes in
// separate events and is not touched.
static void keyByCharacter(SDL_Event& e) {
  if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP) return;
  SDL_Scancode sc = e.key.scancode;
  bool digitOrPunct = (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) || (sc >= SDL_SCANCODE_MINUS && sc <= SDL_SCANCODE_SLASH) ||
                      sc == SDL_SCANCODE_NONUSBACKSLASH;
  if (!digitOrPunct) return;
  SDL_Keycode typed = SDL_GetKeyFromScancode(sc, e.key.mod & SDL_KMOD_SHIFT, false);  // false: with Shift applied
  if (typed >= '0' && typed <= '9') return;  // digits already go by character
  struct Map { SDL_Keycode ch; SDL_Scancode us; SDL_Keycode key; };
  static const Map map[] = {{'-', SDL_SCANCODE_MINUS, SDLK_MINUS},     {'=', SDL_SCANCODE_EQUALS, SDLK_EQUALS},
                            {'+', SDL_SCANCODE_EQUALS, SDLK_EQUALS},   {'[', SDL_SCANCODE_LEFTBRACKET, SDLK_LEFTBRACKET},
                            {']', SDL_SCANCODE_RIGHTBRACKET, SDLK_RIGHTBRACKET}, {'\\', SDL_SCANCODE_BACKSLASH, SDLK_BACKSLASH},
                            {';', SDL_SCANCODE_SEMICOLON, SDLK_SEMICOLON}, {'\'', SDL_SCANCODE_APOSTROPHE, SDLK_APOSTROPHE},
                            {'`', SDL_SCANCODE_GRAVE, SDLK_GRAVE},     {',', SDL_SCANCODE_COMMA, SDLK_COMMA},
                            {'.', SDL_SCANCODE_PERIOD, SDLK_PERIOD},   {'/', SDL_SCANCODE_SLASH, SDLK_SLASH}};
  e.key.scancode = SDL_SCANCODE_UNKNOWN;
  e.key.key = SDLK_UNKNOWN;
  for (auto& m : map)
    if (m.ch == typed) e.key.scancode = m.us, e.key.key = m.key;
}

static void saveShot(ImGuiIO& io, const std::string& file) {
  int w = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x), h = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y);
  SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
  SDL_FlipSurface(s, SDL_FLIP_VERTICAL);
  SDL_SaveBMP(s, file.c_str());
  SDL_DestroySurface(s);
}

static ImFont* loadFont(ImGuiIO& io, std::initializer_list<const char*> paths, float size, float density) {
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x206F, 0x2190, 0x21FF, 0};
  for (const char* p : paths) {
    std::error_code ec;
    if (!fs::exists(p, ec)) continue;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    if (ImFont* f = io.Fonts->AddFontFromFileTTF(p, size * density, &cfg, ranges)) return f;
  }
  ImFontConfig cfg;
  cfg.SizePixels = size * 13 / 15 * density;
  return io.Fonts->AddFontDefault(&cfg);
}

static void applyStyle() {
  ImGui::StyleColorsDark();
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0;
  s.FrameRounding = 3;
  s.TabRounding = 3;
  s.ScrollbarRounding = 3;
  s.WindowBorderSize = 1;
  s.FramePadding = ImVec2(6, 4);
  s.ItemSpacing = ImVec2(8, 5);
  s.WindowMenuButtonPosition = ImGuiDir_None;  // no tab-bar menu: its "hide" had no way back
  // the colours come from the theme (App::useTheme)
}

static const std::initializer_list<const char*> kUiFonts = {
    "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "C:/Windows/Fonts/segoeui.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
static const std::initializer_list<const char*> kMonoFonts = {
    "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf", "C:/Windows/Fonts/consola.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf"};

// One window of the IDE: its SDL window, its own ImGui context (fonts, layout, style)
// and its own App (project, files, panels). All of them draw with one OpenGL context.
struct IdeWindow {
  SDL_Window* sdl = nullptr;
  ImGuiContext* ctx = nullptr;
  std::unique_ptr<vcs::App> app;
  float density = 1, uiScale = 1;
  bool alive = true;

  // Fonts at the zoom, rasterised at the real pixel density so they stay sharp.
  ImFont* buildFonts(float zoom) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.FontDefault = loadFont(io, kUiFonts, 15 * uiScale * zoom, density);
    ImFont* mono = loadFont(io, kMonoFonts, 14 * uiScale * zoom, density);
    io.Fonts->Build();
    io.FontGlobalScale = 1.0f / density;
    return mono;
  }
};

static SDL_GLContext gGl = nullptr;
static const char* gGlsl = nullptr;
static bool gImguiMenu = false;

static SDL_Window* newSdlWindow(SDL_Window* after) {
  SDL_Window* w = SDL_CreateWindow("VCSIDE", 1400, 860,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
  if (!w) return nullptr;
  vcs::setWindowIcon(w);
  if (after) {  // a little below and to the right of the window it came from
    int x = 0, y = 0;
    SDL_GetWindowPosition(after, &x, &y);
    SDL_SetWindowPosition(w, x + 28, y + 28);
  }
  return w;
}

// The OpenGL context, made once with the first window: 3.0 (3.2 core on macOS, the
// only modern one it has), then the others for drivers and layers that only give one
// of them (Wine on macOS: 3.2 core or 2.1).
static bool makeGl(SDL_Window* window) {
  struct GlTry {
    int profile, major, minor, flags;
    const char* glsl;
  };
  const GlTry tries[] = {
#if defined(__APPLE__)
      {SDL_GL_CONTEXT_PROFILE_CORE, 3, 2, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG, "#version 150"},
#else
      {SDL_GL_CONTEXT_PROFILE_CORE, 3, 0, 0, "#version 130"},
      {SDL_GL_CONTEXT_PROFILE_CORE, 3, 2, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG, "#version 150"},
#endif
      {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, 0, "#version 120"},
  };
  std::string glErrors;
  for (auto& t : tries) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, t.profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, t.major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, t.minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, t.flags);
    gGl = SDL_GL_CreateContext(window);
    if (gGl) {
      gGlsl = t.glsl;
      break;
    }
    glErrors += "\nOpenGL " + std::to_string(t.major) + "." + std::to_string(t.minor) + ": " + SDL_GetError();
  }
  if (!gGl) {
    SDL_Log("No OpenGL context:%s", glErrors.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCSIDE",
                             ("VCSIDE needs OpenGL 2.1 or later, and this system did not give it any:" + glErrors +
                              "\n\nUpdate the graphics driver (on Windows, the one from the card's maker).")
                                 .c_str(),
                             window);
    return false;
  }
  SDL_GL_MakeCurrent(window, gGl);
  // No vsync: on macOS a swap with vsync blocks while the window is not visible
  // (another Space, behind other windows, launched in the background), which froze
  // the whole program. The loop below paces itself instead.
  SDL_GL_SetSwapInterval(0);
  return true;
}

// Sets up ImGui and the App of a window already made.
static std::unique_ptr<IdeWindow> openIdeWindow(SDL_Window* sdl) {
  auto w = std::make_unique<IdeWindow>();
  w->sdl = sdl;
  SDL_GL_MakeCurrent(sdl, gGl);
  SDL_ShowWindow(sdl);
  w->ctx = ImGui::CreateContext();
  ImGui::SetCurrentContext(w->ctx);
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  static std::string iniPath = vcs::configDir() + "layout.ini";
  io.IniFilename = iniPath.c_str();
  applyStyle();
  w->density = std::max(1.0f, SDL_GetWindowPixelDensity(sdl));
  w->uiScale = std::max(1.0f, SDL_GetWindowDisplayScale(sdl) / w->density);
  ImFont* mono = w->buildFonts(1.0f);
  ImGui_ImplSDL3_InitForOpenGL(sdl, gGl);
  if (!ImGui_ImplOpenGL3_Init(gGlsl)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCSIDE",
                             "The OpenGL driver does not give the functions VCSIDE needs. Update the graphics driver.", sdl);
    return nullptr;
  }
  ImGui::GetPlatformIO().Platform_SetImeDataFn = keepTextInput;
  ImGui::GetPlatformIO().Platform_OpenInShellFn = [](ImGuiContext*, const char* url) { return SDL_OpenURL(url); };
  SDL_StartTextInput(sdl);
  w->app = std::make_unique<vcs::App>();
  w->app->init(sdl, mono, gImguiMenu);
  return w;
}

static void closeIdeWindow(IdeWindow& w) {
  ImGui::SetCurrentContext(w.ctx);
  SDL_GL_MakeCurrent(w.sdl, gGl);
  w.app->shutdown();
  w.app.reset();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext(w.ctx);
  SDL_DestroyWindow(w.sdl);
}

int main(int argc, char** argv) {
  vcs::nativeEarlyInit();
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_Window* first = newSdlWindow(nullptr);
  if (!first) {
    SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCSIDE", (std::string("Cannot open the window: ") + SDL_GetError()).c_str(), nullptr);
    return 1;
  }
  if (!makeGl(first)) return 1;
  IMGUI_CHECKVERSION();
  for (int i = 1; i < argc; i++) gImguiMenu |= std::string(argv[i]) == "--imgui-menu";

  std::vector<std::unique_ptr<IdeWindow>> windows;
  {
    auto w = openIdeWindow(first);
    if (!w) return 1;
    windows.push_back(std::move(w));
  }
  IdeWindow* active = windows[0].get();  // the focused one: the system menu acts on it
  bool newWindowWanted = false, quitting = false;
  auto activeApp = [&]() -> vcs::App* { return active ? active->app.get() : windows[0]->app.get(); };
  vcs::App::hooks.newWindow = [&] { newWindowWanted = true; };
  vcs::App::hooks.quitAll = [&] {
    quitting = true;
    for (auto& w : windows) {
      ImGui::SetCurrentContext(w->ctx);
      w->app->requestQuit();
    }
    if (active) ImGui::SetCurrentContext(active->ctx);
  };
  vcs::App::hooks.raiseIfOpen = [&](const std::string& folder, vcs::App* from) {
    std::error_code ec;
    for (auto& w : windows)
      if (w->app.get() != from && !w->app->folder().empty() && fs::equivalent(w->app->folder(), folder, ec)) {
        SDL_RaiseWindow(w->sdl);
        return true;
      }
    return false;
  };
  if (vcs::nativeMenuAvailable() && !gImguiMenu)
    vcs::nativeMenuInstall({[&] { return activeApp()->menuBarForSystem(); }, [&] { vcs::App::hooks.quitAll(); },
                            [&] { activeApp()->showAbout(); }});

  // Options for automatic tests (the first window): --shot file.bmp saves the window and
  // quits; --build builds first, --select A-B selects lines, --doc WORD shows its
  // documentation, --find/--replace TEXT opens the find bar, --definition WORD jumps to
  // it, --lang CODE switches the interface language, --focus PANEL (output, problems,
  // space, docs...) brings a panel to the front before the capture.
  // --imgui-menu draws the menus with ImGui on macOS too; --menu "File/Save" chooses a
  // system menu item; --menu-dump prints the system menu bar; --goto-address TEXT
  // opens "Go to address" with that text. --input FILE plays mouse and key actions
  // (see InputScript) before the capture.
  vcs::App& app = *windows[0]->app;
  ImGui::SetCurrentContext(windows[0]->ctx);
  std::string shot, select, doc, find, def, focus, menu, gotoAddr;
  bool menuDump = false;
  bool findReplace = false;
  bool doBuild = false;
  std::string complete;
  InputScript input;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    else if (a == "--select" && i + 1 < argc) select = argv[++i];
    else if (a == "--doc" && i + 1 < argc) doc = argv[++i];
    else if (a == "--definition" && i + 1 < argc) def = argv[++i];
    else if ((a == "--find" || a == "--replace") && i + 1 < argc) findReplace = a == "--replace", find = argv[++i];
    else if (a == "--build") doBuild = true;
    else if (a == "--focus" && i + 1 < argc) focus = argv[++i];
    else if (a == "--lang" && i + 1 < argc) vcs::setLanguage(argv[++i]), app.refreshMenus();
    else if (a == "--menu" && i + 1 < argc) menu = argv[++i];
    else if (a == "--menu-dump") menuDump = true;
    else if (a == "--complete" && i + 1 < argc) complete = argv[++i];
    else if (a == "--goto-address" && i + 1 < argc) gotoAddr = argv[++i];
    else if (a == "--input" && i + 1 < argc) {
      std::ifstream f(argv[++i]);
      for (std::string l; std::getline(f, l);)
        if (!l.empty() && l[0] != '#') input.lines.push_back(l);
    } else if (a == "--imgui-menu") continue;
    else app.openPath(a);
  }
  int frame = 0, shotFrame = -1;
  bool done = false;

  while (!done) {
    if (!shot.empty()) {
      ImGui::SetCurrentContext(windows[0]->ctx);
      if (frame == 5 && doBuild) app.testBuild();
      if (frame == 8 && !select.empty()) app.testSelect(std::stoi(select), std::stoi(select.substr(select.find('-') + 1)));
      if (frame == 8 && !doc.empty()) app.testDoc(doc);
      if (frame == 9 && !find.empty()) app.testFind(find, findReplace);
      if (frame == 9 && !def.empty()) app.testDefinition(def);
      if (frame == 9 && !gotoAddr.empty()) app.testGoToAddress(gotoAddr);
      if (frame == 9 && !menu.empty() && !app.testMenu(menu)) SDL_Log("menu item not found or disabled: %s", menu.c_str());
      if (frame == 10 && !complete.empty()) std::fputs(app.testComplete(complete).c_str(), stdout), std::fflush(stdout);
      if (frame == 12 && menuDump) std::fputs(app.testMenuDump().c_str(), stdout), std::fflush(stdout), menuDump = false;
      if (frame > 10 && shotFrame < 0 && !app.testBusy() && (frame > 20 || input.lines.empty()) && input.done()) shotFrame = frame + 5;
      if (frame == shotFrame - 3 && !focus.empty()) app.testFocus(focus);
    }
    SDL_Event e;
    bool any = SDL_WaitEventTimeout(&e, shot.empty() ? 50 : 10);  // idle when nothing happens (a build polls every 50 ms)
    while (any) {
      // the window the event belongs to; none (quit, displays): every window
      SDL_Window* target = SDL_GetWindowFromEvent(&e);
      for (auto& w : windows) {
        if (target && target != w->sdl) continue;
        ImGui::SetCurrentContext(w->ctx);
        vcs::App& a = *w->app;
        // Zoom by the character of the key: Cmd on macOS, Ctrl elsewhere.
        if (e.type == SDL_EVENT_KEY_DOWN) {
#ifdef __APPLE__
          const SDL_Keymod cmd = SDL_KMOD_GUI;
#else
          const SDL_Keymod cmd = SDL_KMOD_CTRL;
#endif
          SDL_Keycode k = e.key.key;
          if ((e.key.mod & cmd) && !(e.key.mod & SDL_KMOD_ALT)) {
            if (k == SDLK_PLUS || k == SDLK_EQUALS || k == SDLK_KP_PLUS) a.zoomEditor(1);
            else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) a.zoomEditor(-1);
            else if ((k == SDLK_0 || k == SDLK_KP_0) && !e.key.repeat) a.zoomEditor(0);
          }
        }
        SDL_Event copy = e;
        keyByCharacter(copy);
        ImGui_ImplSDL3_ProcessEvent(&copy);
        if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) active = w.get();
        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) a.requestQuit();
        if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) a.openPath(e.drop.data);
      }
      if (e.type == SDL_EVENT_QUIT) vcs::App::hooks.quitAll();
      any = SDL_PollEvent(&e);
    }
    Uint64 loopStart = SDL_GetTicksNS();
    if (newWindowWanted) {
      newWindowWanted = false;
      if (SDL_Window* sdl = newSdlWindow(active ? active->sdl : nullptr))
        if (auto w = openIdeWindow(sdl)) active = w.get(), windows.push_back(std::move(w));
    }

    for (auto& wp : windows) {
      IdeWindow& w = *wp;
      ImGui::SetCurrentContext(w.ctx);
      w.app->setActive(&w == active);
      // Nothing to draw while the window cannot be seen (a test shot still renders).
      if (shot.empty() && (SDL_GetWindowFlags(w.sdl) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED))) continue;
      SDL_GL_MakeCurrent(w.sdl, gGl);
      ImGuiIO& io = ImGui::GetIO();
      // zoom: every font made again at the new size, and the style's sizes scaled
      if (float z = w.app->zoomWanted()) {
        ImFont* m = w.buildFonts(z);
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
        ImGuiStyle& st = ImGui::GetStyle();
        ImVec4 colours[ImGuiCol_COUNT];
        std::copy(std::begin(st.Colors), std::end(st.Colors), colours);
        applyStyle();
        st.ScaleAllSizes(z);
        std::copy(std::begin(colours), std::end(colours), st.Colors);
        w.app->setFonts(m, z);
      }
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      bool first = &w == windows[0].get();
      if (first && !shot.empty() && frame > 20) input.step(io);
      ImGui::NewFrame();
      w.app->frame();
      ImGui::Render();
      glViewport(0, 0, (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x), (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y));
      glClearColor(0.1f, 0.1f, 0.11f, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      if (first && !input.shot.empty()) saveShot(io, input.shot), input.shot.clear();
      if (first && frame == shotFrame) {
        saveShot(io, shot);
        if (std::getenv("VCSIDE_TEST_WINDOWS"))  // what each window holds, for tests
          for (auto& o : windows) SDL_Log("window: %s", o->app->folder().empty() ? "(welcome)" : o->app->folder().c_str());
        done = true;
      }
      SDL_GL_SwapWindow(w.sdl);
    }
    frame++;

    // Windows that are done: closed, unless it is the last one with a project open,
    // which goes back to the welcome page; the last one on the welcome page quits.
    for (auto& wp : windows) {
      IdeWindow& w = *wp;
      if (!w.app->wantsQuit()) continue;
      ImGui::SetCurrentContext(w.ctx);
      bool last = windows.size() == 1;
      if (last && !quitting && w.app->hasProject()) {
        w.app->closeProject();
        w.app->cancelQuit();
      } else {
        w.alive = false;
      }
    }
    for (size_t i = 0; i < windows.size();) {
      if (windows[i]->alive) {
        i++;
        continue;
      }
      if (active == windows[i].get()) active = nullptr;
      closeIdeWindow(*windows[i]);
      windows.erase(windows.begin() + i);
    }
    if (windows.empty()) break;
    if (!active) active = windows.back().get(), SDL_RaiseWindow(active->sdl);
    // At most ~60 frames per second while events keep coming (typing, dragging).
    const Uint64 frameNs = 1000000000ull / 60;
    Uint64 spent = SDL_GetTicksNS() - loopStart;
    if (spent < frameNs) SDL_DelayNS(frameNs - spent);
  }

  while (!windows.empty()) {
    closeIdeWindow(*windows.back());
    windows.pop_back();
  }
  SDL_GL_DestroyContext(gGl);
  SDL_Quit();
  return 0;
}
