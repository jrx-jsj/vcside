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
static void keepTextInput(ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
  SDL_Window* window = SDL_GetWindowFromID((SDL_WindowID)(intptr_t)viewport->PlatformHandle);
  if (!window) return;
  SDL_Rect rect = {(int)data->InputPos.x, (int)data->InputPos.y, 1, (int)data->InputLineHeight};
  SDL_SetTextInputArea(window, &rect, 0);
  if (!SDL_TextInputActive(window)) SDL_StartTextInput(window);
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

  static ImGuiKey keyNamed(const std::string& name) {
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return (ImGuiKey)(ImGuiKey_A + (name[0] - 'a'));
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') return (ImGuiKey)(ImGuiKey_0 + (name[0] - '0'));
    if (name == "enter") return ImGuiKey_Enter;
    if (name == "escape") return ImGuiKey_Escape;
    if (name == "tab") return ImGuiKey_Tab;
    if (name == "delete") return ImGuiKey_Delete;
    if (name == "backspace") return ImGuiKey_Backspace;
    if (name == "up") return ImGuiKey_UpArrow;
    if (name == "down") return ImGuiKey_DownArrow;
    if (name == "left") return ImGuiKey_LeftArrow;
    if (name == "right") return ImGuiKey_RightArrow;
    return ImGuiKey_None;
  }
  static ImGuiKey modNamed(const std::string& name) {
    return name == "shift" ? ImGuiMod_Shift : name == "ctrl" ? ImGuiMod_Ctrl : name == "alt" ? ImGuiMod_Alt : name == "cmd" ? ImGuiMod_Super : ImGuiKey_None;
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
    std::istringstream lineStream(lines[next++]);
    std::string op;
    lineStream >> op;
    std::vector<std::string> args;
    for (std::string word; lineStream >> word;) args.push_back(word);
    auto num = [&](size_t i) { return i < args.size() ? std::stof(args[i]) : 0.0f; };
    int button = !args.empty() && args.back() == "right" ? 1 : 0;
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
      io.AddKeyEvent(modNamed(args.empty() ? "" : args[0]), op == "hold");
    } else if (op == "key") {
      ImGuiKey key = keyNamed(args.empty() ? "" : args[0]);
      for (size_t i = 1; i < args.size(); i++) io.AddKeyEvent(modNamed(args[i]), true);
      io.AddKeyEvent(key, true);
      io.AddKeyEvent(key, false);
      for (size_t i = 1; i < args.size(); i++) io.AddKeyEvent(modNamed(args[i]), false);
    } else if (op == "scan" && !args.empty()) {  // a physical key, as the keyboard sends it: "scan 56 cmd"
      SDL_Event event{};
      event.type = SDL_EVENT_KEY_DOWN;
      event.key.scancode = (SDL_Scancode)std::stoi(args[0]);
      for (size_t i = 1; i < args.size(); i++)
        event.key.mod |= args[i] == "cmd" ? SDL_KMOD_GUI : args[i] == "ctrl" ? SDL_KMOD_CTRL : args[i] == "shift" ? SDL_KMOD_SHIFT : 0;
      event.key.key = SDL_GetKeyFromScancode(event.key.scancode, SDL_KMOD_NONE, true);
      int windowCount = 0;
      if (SDL_Window** windows = SDL_GetWindows(&windowCount)) {
        if (windowCount > 0) event.key.windowID = SDL_GetWindowID(windows[0]);
        SDL_free(windows);
      }
      event.key.down = true;
      SDL_PushEvent(&event);
      SDL_Event up = event;
      up.type = SDL_EVENT_KEY_UP, up.key.down = false;
      SDL_PushEvent(&up);
    } else if (op == "char" && !args.empty()) {  // a key by the character it types, as SDL sends it: "char + cmd"
      SDL_Event event{};
      event.type = SDL_EVENT_KEY_DOWN;
      event.key.key = (SDL_Keycode)(unsigned char)args[0][0];
      for (size_t i = 1; i < args.size(); i++)
        event.key.mod |= args[i] == "cmd" ? SDL_KMOD_GUI : args[i] == "ctrl" ? SDL_KMOD_CTRL : args[i] == "shift" ? SDL_KMOD_SHIFT : 0;
      event.key.down = true;
      SDL_PushEvent(&event);
    } else if (op == "text") {
      std::string text = lines[next - 1].substr(lines[next - 1].find("text") + 5);
      io.AddInputCharactersUTF8(text.c_str());
    } else if (op == "scroll") {
      io.AddMouseWheelEvent(0, num(0));
    } else if (op == "wait") {
      waitFrames = (int)num(0);
    } else if (op == "shot" && !args.empty()) {
      shot = args[0];
    }
  }
};

// Shortcuts by character, whatever the keyboard layout. ImGui's SDL backend reads
// punctuation by its place on a US keyboard, so on a Spanish one Cmd+"-" would be
// Cmd+"/" and "/" (Shift+7) could not be reached. Before the backend sees a digit or
// punctuation key, the event is rewritten as the US key that types the same character;
// a character with no such key reaches ImGui as no key at all. Text typing comes in
// separate events and is not touched.
static void keyByCharacter(SDL_Event& event) {
  if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) return;
  SDL_Scancode scancode = event.key.scancode;
  bool digitOrPunct = (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_0) || (scancode >= SDL_SCANCODE_MINUS && scancode <= SDL_SCANCODE_SLASH) ||
                      scancode == SDL_SCANCODE_NONUSBACKSLASH;
  if (!digitOrPunct) return;
  SDL_Keycode typed = SDL_GetKeyFromScancode(scancode, event.key.mod & SDL_KMOD_SHIFT, false);  // false: with Shift applied
  if (typed >= '0' && typed <= '9') return;  // digits already go by character
  struct Map { SDL_Keycode character; SDL_Scancode usScancode; SDL_Keycode key; };
  static const Map map[] = {{'-', SDL_SCANCODE_MINUS, SDLK_MINUS},     {'=', SDL_SCANCODE_EQUALS, SDLK_EQUALS},
                            {'+', SDL_SCANCODE_EQUALS, SDLK_EQUALS},   {'[', SDL_SCANCODE_LEFTBRACKET, SDLK_LEFTBRACKET},
                            {']', SDL_SCANCODE_RIGHTBRACKET, SDLK_RIGHTBRACKET}, {'\\', SDL_SCANCODE_BACKSLASH, SDLK_BACKSLASH},
                            {';', SDL_SCANCODE_SEMICOLON, SDLK_SEMICOLON}, {'\'', SDL_SCANCODE_APOSTROPHE, SDLK_APOSTROPHE},
                            {'`', SDL_SCANCODE_GRAVE, SDLK_GRAVE},     {',', SDL_SCANCODE_COMMA, SDLK_COMMA},
                            {'.', SDL_SCANCODE_PERIOD, SDLK_PERIOD},   {'/', SDL_SCANCODE_SLASH, SDLK_SLASH}};
  event.key.scancode = SDL_SCANCODE_UNKNOWN;
  event.key.key = SDLK_UNKNOWN;
  for (auto& mapping : map)
    if (mapping.character == typed) event.key.scancode = mapping.usScancode, event.key.key = mapping.key;
}

static void saveShot(ImGuiIO& io, const std::string& file) {
  int width = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x), height = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y);
  SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ABGR8888);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, surface->pixels);
  SDL_FlipSurface(surface, SDL_FLIP_VERTICAL);
  SDL_SaveBMP(surface, file.c_str());
  SDL_DestroySurface(surface);
}

static ImFont* loadFont(ImGuiIO& io, std::initializer_list<const char*> paths, float size, float density) {
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x206F, 0x2190, 0x21FF, 0};
  for (const char* path : paths) {
    std::error_code ec;
    if (!fs::exists(path, ec)) continue;
    ImFontConfig config;
    config.OversampleH = 2;
    if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, size * density, &config, ranges)) return font;
  }
  ImFontConfig config;
  config.SizePixels = size * 13 / 15 * density;
  return io.Fonts->AddFontDefault(&config);
}

static void applyStyle() {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0;
  style.FrameRounding = 3;
  style.TabRounding = 3;
  style.ScrollbarRounding = 3;
  style.WindowBorderSize = 1;
  style.FramePadding = ImVec2(6, 4);
  style.ItemSpacing = ImVec2(8, 5);
  style.WindowMenuButtonPosition = ImGuiDir_None;  // no tab-bar menu: its "hide" had no way back
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
  SDL_Window* sdlWindow = nullptr;
  ImGuiContext* imguiContext = nullptr;
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
  SDL_Window* window = SDL_CreateWindow("VCSIDE", 1400, 860,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
  if (!window) return nullptr;
  vcs::setWindowIcon(window);
  if (after) {  // a little below and to the right of the window it came from
    int x = 0, y = 0;
    SDL_GetWindowPosition(after, &x, &y);
    SDL_SetWindowPosition(window, x + 28, y + 28);
  }
  return window;
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
  for (auto& attempt : tries) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, attempt.profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, attempt.major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, attempt.minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, attempt.flags);
    gGl = SDL_GL_CreateContext(window);
    if (gGl) {
      gGlsl = attempt.glsl;
      break;
    }
    glErrors += "\nOpenGL " + std::to_string(attempt.major) + "." + std::to_string(attempt.minor) + ": " + SDL_GetError();
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
static std::unique_ptr<IdeWindow> openIdeWindow(SDL_Window* sdlWindow) {
  auto ideWindow = std::make_unique<IdeWindow>();
  ideWindow->sdlWindow = sdlWindow;
  SDL_GL_MakeCurrent(sdlWindow, gGl);
  SDL_ShowWindow(sdlWindow);
  ideWindow->imguiContext = ImGui::CreateContext();
  ImGui::SetCurrentContext(ideWindow->imguiContext);
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  static std::string iniPath = vcs::configDir() + "layout.ini";
  io.IniFilename = iniPath.c_str();
  applyStyle();
  ideWindow->density = std::max(1.0f, SDL_GetWindowPixelDensity(sdlWindow));
  ideWindow->uiScale = std::max(1.0f, SDL_GetWindowDisplayScale(sdlWindow) / ideWindow->density);
  ImFont* mono = ideWindow->buildFonts(1.0f);
  ImGui_ImplSDL3_InitForOpenGL(sdlWindow, gGl);
  if (!ImGui_ImplOpenGL3_Init(gGlsl)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCSIDE",
                             "The OpenGL driver does not give the functions VCSIDE needs. Update the graphics driver.", sdlWindow);
    return nullptr;
  }
  ImGui::GetPlatformIO().Platform_SetImeDataFn = keepTextInput;
  ImGui::GetPlatformIO().Platform_OpenInShellFn = [](ImGuiContext*, const char* url) { return SDL_OpenURL(url); };
  SDL_StartTextInput(sdlWindow);
  ideWindow->app = std::make_unique<vcs::App>();
  ideWindow->app->init(sdlWindow, mono, gImguiMenu);
  return ideWindow;
}

static void closeIdeWindow(IdeWindow& ideWindow) {
  ImGui::SetCurrentContext(ideWindow.imguiContext);
  SDL_GL_MakeCurrent(ideWindow.sdlWindow, gGl);
  ideWindow.app->shutdown();
  ideWindow.app.reset();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext(ideWindow.imguiContext);
  SDL_DestroyWindow(ideWindow.sdlWindow);
}

int main(int argc, char** argv) {
  vcs::nativeEarlyInit();
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_Window* firstSdlWindow = newSdlWindow(nullptr);
  if (!firstSdlWindow) {
    SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCSIDE", (std::string("Cannot open the window: ") + SDL_GetError()).c_str(), nullptr);
    return 1;
  }
  if (!makeGl(firstSdlWindow)) return 1;
  IMGUI_CHECKVERSION();
  for (int i = 1; i < argc; i++) gImguiMenu |= std::string(argv[i]) == "--imgui-menu";

  std::vector<std::unique_ptr<IdeWindow>> windows;
  {
    auto ideWindow = openIdeWindow(firstSdlWindow);
    if (!ideWindow) return 1;
    windows.push_back(std::move(ideWindow));
  }
  IdeWindow* active = windows[0].get();  // the focused one: the system menu acts on it
  bool newWindowWanted = false, quitting = false;
  auto activeApp = [&]() -> vcs::App* { return active ? active->app.get() : windows[0]->app.get(); };
  vcs::App::hooks.newWindow = [&] { newWindowWanted = true; };
  vcs::App::hooks.quitAll = [&] {
    quitting = true;
    for (auto& ideWindow : windows) {
      ImGui::SetCurrentContext(ideWindow->imguiContext);
      ideWindow->app->requestQuit();
    }
    if (active) ImGui::SetCurrentContext(active->imguiContext);
  };
  vcs::App::hooks.raiseIfOpen = [&](const std::string& folder, vcs::App* from) {
    std::error_code ec;
    for (auto& ideWindow : windows)
      if (ideWindow->app.get() != from && !ideWindow->app->folder().empty() && fs::equivalent(ideWindow->app->folder(), folder, ec)) {
        SDL_RaiseWindow(ideWindow->sdlWindow);
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
  ImGui::SetCurrentContext(windows[0]->imguiContext);
  std::string shot, select, doc, find, def, focus, menu, gotoAddr;
  bool menuDump = false;
  bool findReplace = false;
  bool doBuild = false;
  std::string complete;
  InputScript input;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--shot" && i + 1 < argc) shot = argv[++i];
    else if (arg == "--select" && i + 1 < argc) select = argv[++i];
    else if (arg == "--doc" && i + 1 < argc) doc = argv[++i];
    else if (arg == "--definition" && i + 1 < argc) def = argv[++i];
    else if ((arg == "--find" || arg == "--replace") && i + 1 < argc) findReplace = arg == "--replace", find = argv[++i];
    else if (arg == "--build") doBuild = true;
    else if (arg == "--focus" && i + 1 < argc) focus = argv[++i];
    else if (arg == "--lang" && i + 1 < argc) vcs::setLanguage(argv[++i]), app.refreshMenus();
    else if (arg == "--menu" && i + 1 < argc) menu = argv[++i];
    else if (arg == "--menu-dump") menuDump = true;
    else if (arg == "--complete" && i + 1 < argc) complete = argv[++i];
    else if (arg == "--goto-address" && i + 1 < argc) gotoAddr = argv[++i];
    else if (arg == "--input" && i + 1 < argc) {
      std::ifstream file(argv[++i]);
      for (std::string line; std::getline(file, line);)
        if (!line.empty() && line[0] != '#') input.lines.push_back(line);
    } else if (arg == "--imgui-menu") continue;
    else app.openPath(arg);
  }
  int frame = 0, shotFrame = -1;
  bool done = false;

  while (!done) {
    if (!shot.empty()) {
      ImGui::SetCurrentContext(windows[0]->imguiContext);
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
    SDL_Event event;
    bool any = SDL_WaitEventTimeout(&event, shot.empty() ? 50 : 10);  // idle when nothing happens (a build polls every 50 ms)
    while (any) {
      // the window the event belongs to; none (quit, displays): every window
      SDL_Window* target = SDL_GetWindowFromEvent(&event);
      for (auto& ideWindow : windows) {
        if (target && target != ideWindow->sdlWindow) continue;
        ImGui::SetCurrentContext(ideWindow->imguiContext);
        vcs::App& ideApp = *ideWindow->app;
        // Zoom by the character of the key: Cmd on macOS, Ctrl elsewhere.
        if (event.type == SDL_EVENT_KEY_DOWN) {
#ifdef __APPLE__
          const SDL_Keymod cmd = SDL_KMOD_GUI;
#else
          const SDL_Keymod cmd = SDL_KMOD_CTRL;
#endif
          SDL_Keycode key = event.key.key;
          if ((event.key.mod & cmd) && !(event.key.mod & SDL_KMOD_ALT)) {
            if (key == SDLK_PLUS || key == SDLK_EQUALS || key == SDLK_KP_PLUS) ideApp.zoomEditor(1);
            else if (key == SDLK_MINUS || key == SDLK_KP_MINUS) ideApp.zoomEditor(-1);
            else if ((key == SDLK_0 || key == SDLK_KP_0) && !event.key.repeat) ideApp.zoomEditor(0);
          }
        }
        SDL_Event copy = event;
        keyByCharacter(copy);
        ImGui_ImplSDL3_ProcessEvent(&copy);
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) active = ideWindow.get();
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) ideApp.requestQuit();
        if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) ideApp.openPath(event.drop.data);
      }
      if (event.type == SDL_EVENT_QUIT) vcs::App::hooks.quitAll();
      any = SDL_PollEvent(&event);
    }
    Uint64 loopStart = SDL_GetTicksNS();
    if (newWindowWanted) {
      newWindowWanted = false;
      if (SDL_Window* sdlWindow = newSdlWindow(active ? active->sdlWindow : nullptr))
        if (auto ideWindow = openIdeWindow(sdlWindow)) active = ideWindow.get(), windows.push_back(std::move(ideWindow));
    }

    for (auto& windowSlot : windows) {
      IdeWindow& ideWindow = *windowSlot;
      ImGui::SetCurrentContext(ideWindow.imguiContext);
      ideWindow.app->setActive(&ideWindow == active);
      // Nothing to draw while the window cannot be seen (a test shot still renders).
      if (shot.empty() && (SDL_GetWindowFlags(ideWindow.sdlWindow) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED))) continue;
      SDL_GL_MakeCurrent(ideWindow.sdlWindow, gGl);
      ImGuiIO& io = ImGui::GetIO();
      // zoom: every font made again at the new size, and the style's sizes scaled
      if (float zoom = ideWindow.app->zoomWanted()) {
        ImFont* monoFont = ideWindow.buildFonts(zoom);
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4 colours[ImGuiCol_COUNT];
        std::copy(std::begin(style.Colors), std::end(style.Colors), colours);
        applyStyle();
        style.ScaleAllSizes(zoom);
        std::copy(std::begin(colours), std::end(colours), style.Colors);
        ideWindow.app->setFonts(monoFont, zoom);
      }
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      bool isFirstWindow = &ideWindow == windows[0].get();
      if (isFirstWindow && !shot.empty() && frame > 20) input.step(io);
      ImGui::NewFrame();
      ideWindow.app->frame();
      ImGui::Render();
      glViewport(0, 0, (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x), (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y));
      glClearColor(0.1f, 0.1f, 0.11f, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      if (isFirstWindow && !input.shot.empty()) saveShot(io, input.shot), input.shot.clear();
      if (isFirstWindow && frame == shotFrame) {
        saveShot(io, shot);
        if (std::getenv("VCSIDE_TEST_WINDOWS"))  // what each window holds, for tests
          for (auto& other : windows) SDL_Log("window: %s", other->app->folder().empty() ? "(welcome)" : other->app->folder().c_str());
        done = true;
      }
      SDL_GL_SwapWindow(ideWindow.sdlWindow);
    }
    frame++;

    // Windows that are done: closed, unless it is the last one with a project open,
    // which goes back to the welcome page; the last one on the welcome page quits.
    for (auto& windowSlot : windows) {
      IdeWindow& ideWindow = *windowSlot;
      if (!ideWindow.app->wantsQuit()) continue;
      ImGui::SetCurrentContext(ideWindow.imguiContext);
      bool last = windows.size() == 1;
      if (last && !quitting && ideWindow.app->hasProject()) {
        ideWindow.app->closeProject();
        ideWindow.app->cancelQuit();
      } else {
        ideWindow.alive = false;
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
    if (!active) active = windows.back().get(), SDL_RaiseWindow(active->sdlWindow);
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
