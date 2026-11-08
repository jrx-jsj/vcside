// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// What the About window shows: VCSIDE's version and licences, the libraries built into
// it, the tools it works with and the sources of its knowledge base. The same text is
// drawn by the system's About panel (macOS) or by ImGui (Windows, Linux).
#pragma once
#include <string>
#include <vector>

namespace vcs {

class KnowledgeBase;
class Toolchain;

struct AboutLine {
  std::string name, detail, url;
};
struct AboutSection {
  std::string title, note;
  std::vector<AboutLine> lines;
};
struct AboutInfo {
  std::string version, tagline, author, credit, copyright, dataDir;
  std::vector<AboutSection> sections;
};

AboutInfo aboutInfo(const KnowledgeBase& kb, Toolchain& tools);

// macOS: the system's About panel with the icon and these credits. False elsewhere.
bool nativeShowAbout(const AboutInfo& info);

}  // namespace vcs
