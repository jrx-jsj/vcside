// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "toolchain.h"

#include <algorithm>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>

#include "i18n.h"
#include "child_process.h"

namespace fs = std::filesystem;

namespace vcs {

Toolchain::Toolchain() {
  tools_ = {
      {"dasm", "DASM", N_("assembler"), "", "", "https://dasm-assembler.github.io/",
       N_("macOS: brew install dasm. Linux: your package manager or the release binaries. Windows: dasm.exe from the releases page.")},
      {"gopher2600", "Gopher2600", N_("emulator"), "", "", "https://github.com/JetSetIlly/Gopher2600/releases",
       N_("ELF cartridges need a recent build (nightly from 2026-11 or later).")},
      {"stella", "Stella", N_("emulator"), "", "", "https://stella-emu.github.io/downloads.html",
       N_("Install it normally; VCSIDE looks in the usual application folders.")},
      {"bb", "batari Basic", N_("compiler"), "", "", "https://github.com/batari-Basic/batari-Basic/releases",
       N_("Unpack the release zip into the bb folder of VCSIDE's tools folder, or choose its 2600basic.sh (2600bas.bat on Windows). It brings its own DASM; version 1.9 also needs wasmtime.")},
      {"bb-pxe", "batari Basic for PXE (beta)", N_("compiler"), "", "", "https://github.com/ZacharyScolaro/batari-Basic/releases",
       N_("PXE projects build with the PXE beta of batari Basic (ZackAttack's), whose kernel runs programs that 1.9's does not. Unpack its release (bB-1.9-PXE-beta-...-wasm) into the bb-pxe folder of VCSIDE's tools folder, or choose its 2600basic.sh (2600bas.bat on Windows). It needs wasmtime too.")},
      {"wasmtime", "wasmtime", N_("runtime"), "", "", "https://wasmtime.dev/",
       N_("batari Basic 1.9 runs its compiler on it (WebAssembly). macOS: brew install wasmtime; Windows: winget install BytecodeAlliance.Wasmtime.Portable.")},
      {"arm-gcc", "Arm GNU toolchain (ELF-VCS)", N_("compiler"), "", "",
       "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads",
       N_("arm-none-eabi-gcc, for C games on ELF cartridges with vcsLib.")},
      {"make", "GNU Make", N_("build tool"), "", "", "https://www.gnu.org/software/make/",
       N_("ELF projects build with their Makefile. macOS: xcode-select --install. Linux: your package manager (build-essential). Windows: MSYS2 (pacman -S make) or another GNU Make build.")},
  };
}

const std::vector<ToolGroup>& Toolchain::groups() {
  static const std::vector<ToolGroup> group = {
      {N_("Compilers and assemblers"), {"assembler", "compiler"}},
      {N_("Emulators"), {"emulator"}},
      {N_("Build helpers"), {"runtime", "build tool"}},
  };
  return group;
}

std::vector<Tool*> Toolchain::inGroup(const ToolGroup& group) {
  std::vector<Tool*> groupTools;
  for (auto& tool : tools_)
    for (auto& role : group.roles)
      if (tool.role == role) groupTools.push_back(&tool);
  return groupTools;
}

std::string Toolchain::toolsDir() {
  char* prefPath = SDL_GetPrefPath("vcside", "VCSIDE");
  std::string folder = prefPath ? prefPath : "";
  SDL_free(prefPath);
  return folder + "tools";
}

std::vector<std::string> Toolchain::candidates(const Tool& tool) const {
  std::vector<std::string> paths;
  const std::string toolsFolder = toolsDir();
  const char* home = std::getenv("HOME");
  std::string homeDir = home ? home : "";
#ifdef _WIN32
  std::string programFiles = std::getenv("ProgramFiles") ? std::getenv("ProgramFiles") : "C:\\Program Files";
  std::string localAppData = std::getenv("LOCALAPPDATA") ? std::getenv("LOCALAPPDATA") : "";
#endif
  auto add = [&](const std::string& path) { paths.push_back(path); };
  if (tool.id == "dasm") {
    for (auto name : {"dasm"}) {
      add(findInPath(name));
      add(toolsFolder + "/dasm/dasm");
#ifdef _WIN32
      add(toolsFolder + "\\dasm\\dasm.exe");
#endif
    }
    add("/opt/homebrew/bin/dasm");
    add("/usr/local/bin/dasm");
  } else if (tool.id == "stella") {
    add(findInPath("stella"));
#if defined(__APPLE__)
    add("/Applications/Stella.app/Contents/MacOS/Stella");
    add(homeDir + "/Applications/Stella.app/Contents/MacOS/Stella");
#elif defined(_WIN32)
    add(programFiles + "\\Stella\\Stella.exe");
    add(localAppData + "\\Programs\\Stella\\Stella.exe");
#else
    add("/usr/games/stella");
    add("/usr/bin/stella");
    add("/usr/local/bin/stella");
#endif
    add(toolsFolder + "/stella/stella");
  } else if (tool.id == "gopher2600") {
    add(findInPath("gopher2600"));
#if defined(__APPLE__)
    for (auto bin : {"gopher2600_darwin_arm64", "gopher2600_darwin_amd64", "gopher2600"}) {
      add(std::string("/Applications/Gopher2600.app/Contents/MacOS/") + bin);
      add(homeDir + "/Applications/Gopher2600.app/Contents/MacOS/" + bin);
    }
#elif defined(_WIN32)
    add(toolsFolder + "\\gopher2600\\gopher2600.exe");
    add(toolsFolder + "\\gopher2600\\gopher2600_windows_amd64.exe");
#else
    add(toolsFolder + "/gopher2600/gopher2600_linux_amd64");
#endif
    add(toolsFolder + "/gopher2600/gopher2600");
  } else if (tool.id == "wasmtime") {
    add(findInPath("wasmtime"));
#ifdef _WIN32
    std::string userProfile = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "";
    add(userProfile + "\\.wasmtime\\bin\\wasmtime.exe");
    add(localAppData + "\\Microsoft\\WinGet\\Links\\wasmtime.exe");
#else
    add(homeDir + "/.wasmtime/bin/wasmtime");  // its install script
    add("/opt/homebrew/bin/wasmtime");
    add("/usr/local/bin/wasmtime");
#endif
  } else if (tool.id == "bb") {
#ifdef _WIN32
    add(findInPath("2600bas.bat"));
    add(toolsFolder + "\\bb\\2600bas.bat");
#else
    add(findInPath("2600basic.sh"));
    add(toolsFolder + "/bb/2600basic.sh");
    if (const char* bb = std::getenv("bB")) add(std::string(bb) + "/2600basic.sh");
#endif
  } else if (tool.id == "bb-pxe") {
#ifdef _WIN32
    add(toolsFolder + "\\bb-pxe\\2600bas.bat");
#else
    add(toolsFolder + "/bb-pxe/2600basic.sh");
#endif
  } else if (tool.id == "arm-gcc") {
    add(findInPath("arm-none-eabi-gcc"));
    add("/opt/homebrew/bin/arm-none-eabi-gcc");
    add("/usr/local/bin/arm-none-eabi-gcc");
    add("/usr/bin/arm-none-eabi-gcc");
    // Arm's own installers: <folder>/<version>/... (the newest version first)
    auto versions = [&](const std::string& root, const std::string& rest) {
      std::error_code errorCode;
      std::vector<std::string> found;
      for (auto& entry : fs::directory_iterator(root, errorCode))
        if (entry.is_directory()) found.push_back((entry.path() / rest).string());
      std::sort(found.rbegin(), found.rend());
      for (auto& candidate : found) add(candidate);
    };
#if defined(__APPLE__)
    versions("/Applications/ArmGNUToolchain", "arm-none-eabi/bin/arm-none-eabi-gcc");
#elif defined(_WIN32)
    std::string pf86 = std::getenv("ProgramFiles(x86)") ? std::getenv("ProgramFiles(x86)") : "C:\\Program Files (x86)";
    versions(pf86 + "\\Arm GNU Toolchain arm-none-eabi", "bin\\arm-none-eabi-gcc.exe");
    versions(programFiles + "\\Arm GNU Toolchain arm-none-eabi", "bin\\arm-none-eabi-gcc.exe");
#endif
  } else if (tool.id == "make") {
    add(findInPath("make"));
    add("/usr/bin/make");
    add("/opt/homebrew/bin/gmake");
#ifdef _WIN32
    add(findInPath("mingw32-make"));
    add("C:\\msys64\\usr\\bin\\make.exe");
#endif
  }
  return paths;
}

void Toolchain::detect() {
  for (auto& tool : tools_) {
    tool.path.clear();
    if (fileExists(tool.userPath)) {
      tool.path = tool.userPath;
      continue;
    }
    for (auto& candidatePath : candidates(tool))
      if (fileExists(candidatePath)) {
        tool.path = candidatePath;
        break;
      }
  }
}

Tool* Toolchain::get(const std::string& id) {
  for (auto& tool : tools_)
    if (tool.id == id) return &tool;
  return nullptr;
}

void Toolchain::loadSettings(const std::map<std::string, std::string>& userPaths) {
  for (auto& tool : tools_) {
    auto found = userPaths.find(tool.id);
    tool.userPath = found == userPaths.end() ? "" : found->second;
  }
}

std::map<std::string, std::string> Toolchain::userPaths() const {
  std::map<std::string, std::string> paths;
  for (auto& tool : tools_)
    if (!tool.userPath.empty()) paths[tool.id] = tool.userPath;
  return paths;
}

}  // namespace vcs
