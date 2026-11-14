// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "about.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

#include "i18n.h"
#include "imgui.h"
#include "kb.h"
#include "paths.h"
#include "toolchain.h"

#ifndef VCSIDE_VERSION
#define VCSIDE_VERSION "dev"
#endif

namespace vcs {

static std::string sdlVersion() {
  int version = SDL_GetVersion();
  return std::to_string(SDL_VERSIONNUM_MAJOR(version)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(version)) + "." +
         std::to_string(SDL_VERSIONNUM_MICRO(version));
}

AboutInfo aboutInfo(const KnowledgeBase& kb, Toolchain& tools) {
  AboutInfo info;
  info.version = VCSIDE_VERSION;
  info.tagline = T("A small native IDE for creating Atari 2600 games.");
  info.author = "Juan Sánchez Jurado";
  info.credit = T("Made with the help of Claude Code (Opus 5.5).");
  info.copyright = "Copyright © 2026 " + info.author + " · GPL-3.0-or-later";
  info.dataDir = dataDir();

  AboutSection ownSection{T("VCSIDE"), T("Games made with VCSIDE do not inherit any of these licences."), {}};
  ownSection.lines.push_back({T("Code and icon"), "GPL-3.0-or-later", "https://www.gnu.org/licenses/gpl-3.0.html"});
  ownSection.lines.push_back({T("Knowledge base (data/kb)"), "CC BY-SA 4.0", "https://creativecommons.org/licenses/by-sa/4.0/"});
  ownSection.lines.push_back({T("Project templates (data/templates)"), T("CC0 1.0 (public domain)"), "https://creativecommons.org/publicdomain/zero/1.0/"});
  info.sections.push_back(ownSection);

  AboutSection libs{T("Built with"), T("Their licences are in the data/licenses folder."), {}};
  libs.lines.push_back({"SDL " + sdlVersion(), T("zlib licence. Sam Lantinga and contributors."), "https://libsdl.org"});
  libs.lines.push_back({std::string("Dear ImGui ") + IMGUI_VERSION + " (docking)", T("MIT licence. Omar Cornut and contributors."),
                        "https://github.com/ocornut/imgui"});
  libs.lines.push_back({"nlohmann/json " + std::to_string(NLOHMANN_JSON_VERSION_MAJOR) + "." + std::to_string(NLOHMANN_JSON_VERSION_MINOR) +
                            "." + std::to_string(NLOHMANN_JSON_VERSION_PATCH),
                        T("MIT licence. Niels Lohmann."), "https://github.com/nlohmann/json"});
#if !SDL_VERSION_ATLEAST(3, 4, 0)
  libs.lines.push_back({"stb_image 2.30", T("MIT or public domain. Sean Barrett."), "https://github.com/nothings/stb"});
#endif
  info.sections.push_back(libs);

  // The versions VCSIDE was tried with, and each tool's licence. None is included.
  static const std::map<std::string, std::pair<std::string, std::string>> tested = {
      {"dasm", {"2.20.14.1", "GPL-2.0-or-later"}},
      {"stella", {"7.0", "GPL-2.0-or-later"}},
      {"gopher2600", {"0.56.1, nightly 07d8de5", "GPL-3.0"}},
      {"bb", {"1.9", "GPL-2.0; generated assembly CC0"}},
      {"bb-pxe", {"1.9 PXE beta 42", "GPL-2.0; generated assembly CC0"}},
      {"wasmtime", {"41.0.0", "Apache-2.0 WITH LLVM-exception"}},
      {"arm-gcc", {"GCC 16.2.0", "GPL-3.0 with the GCC runtime exception"}},
  };
  // grouped like the Tools window; none is included, so the first says so
  for (auto& group : Toolchain::groups()) {
    AboutSection toolSection{T(group.title), "", {}};
    if (info.sections.size() == 2) toolSection.note = T("Not included: VCSIDE runs the ones installed on this computer.");
    for (Tool* tool : tools.inGroup(group)) {
      auto found = tested.find(tool->id);
      std::string detail;
      if (found != tested.end()) detail = F("Tested with {0}.", {found->second.first}) + " " + found->second.second + ".";
      toolSection.lines.push_back({tool->name, detail, tool->homepage});
    }
    info.sections.push_back(toolSection);
  }

  AboutSection sourcesSection{T("Sources of the knowledge base"),
                   T("Thanks to every person and team listed for their work for the community. Facts are cited, not copied: the texts are VCSIDE's own. Details in docs/sources.md."), {}};
  std::vector<const Source*> list;
  for (auto& [id, source] : kb.sources()) list.push_back(&source);
  std::sort(list.begin(), list.end(), [](auto* left, auto* right) { return naturalLess(left->title, right->title); });
  for (auto* source : list) sourcesSection.lines.push_back({source->title, source->author, source->url});
  info.sections.push_back(sourcesSection);

  AboutSection themes{T("Themes"), "", {}};
  themes.lines.push_back({"Solarized", T("The palette of the Solarized themes. MIT licence. Ethan Schoonover."), "https://ethanschoonover.com/solarized/"});
  info.sections.push_back(themes);

  AboutSection fonts{T("Fonts"), "", {}};
  fonts.lines.push_back({T("The system's own"), T("SF and Menlo (macOS), Segoe UI and Consolas (Windows), DejaVu (Linux). Not included."), ""});
  info.sections.push_back(fonts);
  return info;
}

#ifndef __APPLE__
bool nativeShowAbout(const AboutInfo&) { return false; }
#endif

}  // namespace vcs
