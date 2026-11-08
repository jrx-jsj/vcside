// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
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
      {"stella", "Stella", N_("emulator"), "", "", "https://stella-emu.github.io/downloads.html",
       N_("Install it normally; VCSIDE looks in the usual application folders.")},
      {"gopher2600", "Gopher2600", N_("emulator"), "", "", "https://github.com/JetSetIlly/Gopher2600/releases",
       N_("ELF cartridges need a recent build (nightly from 2026-11 or later).")},
      {"bb", "batari Basic", N_("compiler"), "", "", "https://github.com/batari-Basic/batari-Basic/releases",
       N_("Unpack the release zip into the bb folder of VCSIDE's tools folder, or choose its 2600basic.sh (2600bas.bat on Windows). It brings its own DASM; version 1.9 also needs wasmtime.")},
      {"wasmtime", "wasmtime", N_("runtime"), "", "", "https://wasmtime.dev/",
       N_("batari Basic 1.9 runs its compiler on it (WebAssembly). macOS: brew install wasmtime; Windows: winget install BytecodeAlliance.Wasmtime.Portable.")},
      {"arm-gcc", "Arm GNU toolchain (ELF-VCS)", N_("compiler"), "", "",
       "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads",
       N_("arm-none-eabi-gcc, for C games on ELF cartridges with vcsLib.")},
      {"make", "GNU Make", N_("build tool"), "", "", "https://www.gnu.org/software/make/",
       N_("ELF projects build with their Makefile. macOS: xcode-select --install. Linux: your package manager (build-essential). Windows: MSYS2 (pacman -S make) or another GNU Make build.")},
      {"cc65", "cc65 / ca65", N_("assembler"), "", "", "https://cc65.github.io/", N_("Optional alternative to DASM.")},
  };
}

std::string Toolchain::toolsDir() {
  char* p = SDL_GetPrefPath("vcside", "VCSIDE");
  std::string s = p ? p : "";
  SDL_free(p);
  return s + "tools";
}

std::vector<std::string> Toolchain::candidates(const Tool& t) const {
  std::vector<std::string> c;
  const std::string own = toolsDir();
  const char* home = std::getenv("HOME");
  std::string h = home ? home : "";
#ifdef _WIN32
  std::string pf = std::getenv("ProgramFiles") ? std::getenv("ProgramFiles") : "C:\\Program Files";
  std::string lad = std::getenv("LOCALAPPDATA") ? std::getenv("LOCALAPPDATA") : "";
#endif
  auto add = [&](const std::string& s) { c.push_back(s); };
  if (t.id == "dasm") {
    for (auto n : {"dasm"}) {
      add(findInPath(n));
      add(own + "/dasm/dasm");
#ifdef _WIN32
      add(own + "\\dasm\\dasm.exe");
#endif
    }
    add("/opt/homebrew/bin/dasm");
    add("/usr/local/bin/dasm");
  } else if (t.id == "stella") {
    add(findInPath("stella"));
#if defined(__APPLE__)
    add("/Applications/Stella.app/Contents/MacOS/Stella");
    add(h + "/Applications/Stella.app/Contents/MacOS/Stella");
#elif defined(_WIN32)
    add(pf + "\\Stella\\Stella.exe");
    add(lad + "\\Programs\\Stella\\Stella.exe");
#else
    add("/usr/games/stella");
    add("/usr/bin/stella");
    add("/usr/local/bin/stella");
#endif
    add(own + "/stella/stella");
  } else if (t.id == "gopher2600") {
    add(findInPath("gopher2600"));
#if defined(__APPLE__)
    for (auto bin : {"gopher2600_darwin_arm64", "gopher2600_darwin_amd64", "gopher2600"}) {
      add(std::string("/Applications/Gopher2600.app/Contents/MacOS/") + bin);
      add(h + "/Applications/Gopher2600.app/Contents/MacOS/" + bin);
    }
#elif defined(_WIN32)
    add(own + "\\gopher2600\\gopher2600.exe");
    add(own + "\\gopher2600\\gopher2600_windows_amd64.exe");
#else
    add(own + "/gopher2600/gopher2600_linux_amd64");
#endif
    add(own + "/gopher2600/gopher2600");
  } else if (t.id == "wasmtime") {
    add(findInPath("wasmtime"));
#ifdef _WIN32
    std::string up = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "";
    add(up + "\\.wasmtime\\bin\\wasmtime.exe");
    add(lad + "\\Microsoft\\WinGet\\Links\\wasmtime.exe");
#else
    add(h + "/.wasmtime/bin/wasmtime");  // its install script
    add("/opt/homebrew/bin/wasmtime");
    add("/usr/local/bin/wasmtime");
#endif
  } else if (t.id == "bb") {
#ifdef _WIN32
    add(findInPath("2600bas.bat"));
    add(own + "\\bb\\2600bas.bat");
#else
    add(findInPath("2600basic.sh"));
    add(own + "/bb/2600basic.sh");
    if (const char* bb = std::getenv("bB")) add(std::string(bb) + "/2600basic.sh");
#endif
  } else if (t.id == "arm-gcc") {
    add(findInPath("arm-none-eabi-gcc"));
    add("/opt/homebrew/bin/arm-none-eabi-gcc");
    add("/usr/local/bin/arm-none-eabi-gcc");
    add("/usr/bin/arm-none-eabi-gcc");
    // Arm's own installers: <folder>/<version>/... (the newest version first)
    auto versions = [&](const std::string& root, const std::string& rest) {
      std::error_code ec;
      std::vector<std::string> found;
      for (auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory()) found.push_back((e.path() / rest).string());
      std::sort(found.rbegin(), found.rend());
      for (auto& f : found) add(f);
    };
#if defined(__APPLE__)
    versions("/Applications/ArmGNUToolchain", "arm-none-eabi/bin/arm-none-eabi-gcc");
#elif defined(_WIN32)
    std::string pf86 = std::getenv("ProgramFiles(x86)") ? std::getenv("ProgramFiles(x86)") : "C:\\Program Files (x86)";
    versions(pf86 + "\\Arm GNU Toolchain arm-none-eabi", "bin\\arm-none-eabi-gcc.exe");
    versions(pf + "\\Arm GNU Toolchain arm-none-eabi", "bin\\arm-none-eabi-gcc.exe");
#endif
  } else if (t.id == "make") {
    add(findInPath("make"));
    add("/usr/bin/make");
    add("/opt/homebrew/bin/gmake");
#ifdef _WIN32
    add(findInPath("mingw32-make"));
    add("C:\\msys64\\usr\\bin\\make.exe");
#endif
  } else if (t.id == "cc65") {
    add(findInPath("cl65"));
    add("/opt/homebrew/bin/cl65");
  }
  return c;
}

void Toolchain::detect() {
  for (auto& t : tools_) {
    t.path.clear();
    if (fileExists(t.userPath)) {
      t.path = t.userPath;
      continue;
    }
    for (auto& c : candidates(t))
      if (fileExists(c)) {
        t.path = c;
        break;
      }
  }
}

Tool* Toolchain::get(const std::string& id) {
  for (auto& t : tools_)
    if (t.id == id) return &t;
  return nullptr;
}

void Toolchain::loadSettings(const std::map<std::string, std::string>& userPaths) {
  for (auto& t : tools_) {
    auto it = userPaths.find(t.id);
    t.userPath = it == userPaths.end() ? "" : it->second;
  }
}

std::map<std::string, std::string> Toolchain::userPaths() const {
  std::map<std::string, std::string> m;
  for (auto& t : tools_)
    if (!t.userPath.empty()) m[t.id] = t.userPath;
  return m;
}

}  // namespace vcs
