// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// VCSIDE entry point: SDL3 window + OpenGL + Dear ImGui.
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <filesystem>
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
  cfg.SizePixels = 13 * density;
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
  // the colours come from the theme (App::useTheme)
}

int main(int argc, char** argv) {
  vcs::nativeEarlyInit();
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_Window* window = SDL_CreateWindow("VCSIDE", 1400, 860,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
  if (!window) {
    SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCSIDE", (std::string("Cannot open the window: ") + SDL_GetError()).c_str(), nullptr);
    return 1;
  }
  vcs::setWindowIcon(window);
  // OpenGL: 3.0 (3.2 core on macOS, the only modern one it has), then the others for
  // drivers and layers that only give one of them (Wine on macOS: 3.2 core or 2.1).
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
  SDL_GLContext gl = nullptr;
  const char* glsl = nullptr;
  std::string glErrors;
  for (auto& t : tries) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, t.profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, t.major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, t.minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, t.flags);
    gl = SDL_GL_CreateContext(window);
    if (gl) {
      glsl = t.glsl;
      break;
    }
    glErrors += "\nOpenGL " + std::to_string(t.major) + "." + std::to_string(t.minor) + ": " + SDL_GetError();
  }
  if (!gl) {
    SDL_Log("No OpenGL context:%s", glErrors.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCSIDE",
                             ("VCSIDE needs OpenGL 2.1 or later, and this system did not give it any:" + glErrors +
                              "\n\nUpdate the graphics driver (on Windows, the one from the card's maker).")
                                 .c_str(),
                             window);
    return 1;
  }
  SDL_GL_MakeCurrent(window, gl);
  // No vsync: on macOS a swap with vsync blocks while the window is not visible
  // (another Space, behind other windows, launched in the background), which froze
  // the whole program. The loop below paces itself instead.
  SDL_GL_SetSwapInterval(0);
  SDL_ShowWindow(window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  static std::string iniPath = vcs::configDir() + "layout.ini";
  io.IniFilename = iniPath.c_str();
  applyStyle();

  // Fonts are rasterised at the real pixel density so they stay sharp on HiDPI screens.
  float density = SDL_GetWindowPixelDensity(window);
  if (density < 1) density = 1;
  float uiScale = SDL_GetWindowDisplayScale(window) / density;
  if (uiScale < 1) uiScale = 1;
  ImFont* ui = loadFont(io,
                        {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "C:/Windows/Fonts/segoeui.ttf",
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                         "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"},
                        15 * uiScale, density);
  const std::initializer_list<const char*> monoPaths = {
      "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf", "C:/Windows/Fonts/consola.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf"};
  ImFont* mono = loadFont(io, monoPaths, 14 * uiScale, density);
  io.FontDefault = ui;
  io.FontGlobalScale = 1.0f / density;

  ImGui_ImplSDL3_InitForOpenGL(window, gl);
  if (!ImGui_ImplOpenGL3_Init(glsl)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCSIDE",
                             "The OpenGL driver does not give the functions VCSIDE needs. Update the graphics driver.", window);
    return 1;
  }
  ImGui::GetPlatformIO().Platform_SetImeDataFn = keepTextInput;
  ImGui::GetPlatformIO().Platform_OpenInShellFn = [](ImGuiContext*, const char* url) { return SDL_OpenURL(url); };
  SDL_StartTextInput(window);

  bool imguiMenu = false;
  for (int i = 1; i < argc; i++) imguiMenu |= std::string(argv[i]) == "--imgui-menu";
  vcs::App app;
  app.init(window, mono, imguiMenu);
  // Options for automatic tests: --shot file.bmp saves the window and quits;
  // --build builds first, --select A-B selects lines, --doc WORD shows its documentation,
  // --find/--replace TEXT opens the find bar, --definition WORD jumps to it,
  // --lang CODE switches the interface language, --focus PANEL (output, problems,
  // space, docs...) brings a panel to the front before the capture.
  // --imgui-menu draws the menus with ImGui on macOS too; --menu "File/Save" chooses a
  // system menu item; --menu-dump prints the system menu bar; --goto-address TEXT
  // opens "Go to address" with that text. --input FILE plays mouse and key actions
  // (see InputScript) before the capture.
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

  while (!app.wantsQuit()) {
    if (!shot.empty()) {
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
      ImGui_ImplSDL3_ProcessEvent(&e);
      if (e.type == SDL_EVENT_QUIT) app.requestQuit();
      if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window)) app.requestQuit();
      if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) app.openPath(e.drop.data);
      any = SDL_PollEvent(&e);
    }
    // Nothing to draw while the window cannot be seen (a test shot still renders).
    if (shot.empty() && (SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED))) continue;
    Uint64 frameStart = SDL_GetTicksNS();

    // editor zoom: one more size in the font atlas, made between frames
    if (float pts = app.editorFontWanted()) {
      ImFont* f = loadFont(io, monoPaths, pts * uiScale, density);
      io.Fonts->Build();
      ImGui_ImplOpenGL3_DestroyFontsTexture();
      ImGui_ImplOpenGL3_CreateFontsTexture();
      app.setEditorFont(pts, f);
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    if (!shot.empty() && frame > 20) input.step(io);
    ImGui::NewFrame();
    app.frame();
    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x * io.DisplayFramebufferScale.x, (int)io.DisplaySize.y * io.DisplayFramebufferScale.y);
    glClearColor(0.1f, 0.1f, 0.11f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!input.shot.empty()) saveShot(io, input.shot), input.shot.clear();
    if (frame++ == shotFrame) {
      saveShot(io, shot);
      break;
    }
    SDL_GL_SwapWindow(window);
    // At most ~60 frames per second while events keep coming (typing, dragging).
    const Uint64 frameNs = 1000000000ull / 60;
    Uint64 spent = SDL_GetTicksNS() - frameStart;
    if (spent < frameNs) SDL_DelayNS(frameNs - spent);
  }

  app.shutdown();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DestroyContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
