// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// VCSIDE entry point: SDL3 window + OpenGL + Dear ImGui.
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <filesystem>
#include <string>

#include "app.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "paths.h"

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
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.13f, 1);
  c[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.12f, 0.13f, 1);
  c[ImGuiCol_MenuBarBg] = ImVec4(0.16f, 0.16f, 0.17f, 1);
  c[ImGuiCol_TitleBg] = ImVec4(0.14f, 0.14f, 0.15f, 1);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.17f, 0.17f, 0.19f, 1);
  c[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.17f, 1);
  c[ImGuiCol_TabSelected] = ImVec4(0.12f, 0.12f, 0.13f, 1);
  c[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.22f, 0.12f, 1);
  c[ImGuiCol_Header] = ImVec4(0.30f, 0.22f, 0.10f, 0.8f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.32f, 0.12f, 0.8f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.38f, 0.12f, 1);
  c[ImGuiCol_Button] = ImVec4(0.25f, 0.25f, 0.27f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.38f, 0.12f, 1);
  c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.65f, 0.2f, 1);
  c[ImGuiCol_DockingPreview] = ImVec4(0.95f, 0.65f, 0.2f, 0.5f);
  c[ImGuiCol_TabSelectedOverline] = ImVec4(0.95f, 0.65f, 0.2f, 1);
}

int main(int argc, char** argv) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    SDL_Log("SDL_Init: %s", SDL_GetError());
    return 1;
  }
#if defined(__APPLE__)
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  const char* glsl = "#version 150";
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  const char* glsl = "#version 130";
#endif
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_Window* window = SDL_CreateWindow("VCSIDE", 1400, 860,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
  if (!window) {
    SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  SDL_GL_MakeCurrent(window, gl);
  SDL_GL_SetSwapInterval(1);
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
  ImFont* mono = loadFont(io,
                          {"/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf", "C:/Windows/Fonts/consola.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                           "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf"},
                          14 * uiScale, density);
  io.FontDefault = ui;
  io.FontGlobalScale = 1.0f / density;

  ImGui_ImplSDL3_InitForOpenGL(window, gl);
  ImGui_ImplOpenGL3_Init(glsl);
  ImGui::GetPlatformIO().Platform_SetImeDataFn = keepTextInput;
  SDL_StartTextInput(window);

  vcs::App app;
  app.init(window, mono);
  // Options for automatic tests: --shot file.bmp saves the window and quits;
  // --build builds first, --select A-B selects lines, --doc WORD shows its documentation,
  // --lang CODE switches the interface language for this run.
  std::string shot, select, doc;
  bool doBuild = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    else if (a == "--select" && i + 1 < argc) select = argv[++i];
    else if (a == "--doc" && i + 1 < argc) doc = argv[++i];
    else if (a == "--build") doBuild = true;
    else if (a == "--lang" && i + 1 < argc) vcs::setLanguage(argv[++i]);
    else app.openPath(a);
  }
  int frame = 0, shotFrame = -1;
  // macOS pauses buffer swaps with vsync on while the window is hidden; a test
  // run must not depend on the window being visible.
  if (!shot.empty()) SDL_GL_SetSwapInterval(0);

  while (!app.wantsQuit()) {
    if (!shot.empty()) {
      if (frame == 5 && doBuild) app.testBuild();
      if (frame == 8 && !select.empty()) app.testSelect(std::stoi(select), std::stoi(select.substr(select.find('-') + 1)));
      if (frame == 8 && !doc.empty()) app.testDoc(doc);
      if (frame > 10 && shotFrame < 0 && !app.testBusy()) shotFrame = frame + 5;
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
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) continue;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    app.frame();
    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x * io.DisplayFramebufferScale.x, (int)io.DisplaySize.y * io.DisplayFramebufferScale.y);
    glClearColor(0.1f, 0.1f, 0.11f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (frame++ == shotFrame) {
      int w = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x), h = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y);
      SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
      SDL_FlipSurface(s, SDL_FLIP_VERTICAL);
      SDL_SaveBMP(s, shot.c_str());
      SDL_DestroySurface(s);
      break;
    }
    SDL_GL_SwapWindow(window);
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
