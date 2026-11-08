// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Themes: every colour of the IDE is a named role ("ui.background", "syntax.comment"...).
// A theme is a .vcstheme file (JSON) that gives roles their colours:
//
//   {
//     "vcstheme": 1,
//     "name": "My theme",
//     "base": "light",                       // roles it does not give come from this theme
//     "palette": {"paper": "#FDF6E3"},       // named colours, used as "$paper"
//     "colours": {
//       "ui.background": "$paper",
//       "ui.accent": "#268BD2",
//       "editor.background": "@ui.background" // another role's colour
//     },
//     "indent": ["#B5890020", "#85990020"],   // editor: one colour per indentation level
//     "imgui": {"PlotLines": "#808080"}       // any ImGui colour, after the roles
//   }
//
// Colours are #RRGGBB or #RRGGBBAA; "$name/40" or "@role/40" is that colour with alpha $40. References are resolved after the base is merged, so a
// theme that only changes the palette (or ui.accent) changes everything that refers to it.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#ifndef N_
#define N_(s) s  // marks a string for translation (i18n.h has the same)
#endif

namespace vcs::theme {

// X(enum name, id, group, label)
#define VCS_THEME_ROLES(X)                                                               \
  X(UiBackground, "ui.background", N_("Interface"), N_("Window background"))                    \
  X(UiPanel, "ui.panel", N_("Interface"), N_("Menu bar and empty dock space"))                  \
  X(UiPopup, "ui.popup", N_("Interface"), N_("Menus, popups and tooltips"))                     \
  X(UiText, "ui.text", N_("Interface"), N_("Text"))                                             \
  X(UiTextDim, "ui.textDim", N_("Interface"), N_("Secondary text"))                             \
  X(UiBorder, "ui.border", N_("Interface"), N_("Borders and separators"))                       \
  X(UiFrame, "ui.frame", N_("Interface"), N_("Fields (text, numbers, combos)"))                 \
  X(UiFrameHover, "ui.frameHover", N_("Interface"), N_("Field under the mouse"))                \
  X(UiFrameActive, "ui.frameActive", N_("Interface"), N_("Field being edited"))                 \
  X(UiTitle, "ui.title", N_("Interface"), N_("Window title"))                                   \
  X(UiTitleActive, "ui.titleActive", N_("Interface"), N_("Title of the focused window"))        \
  X(UiButton, "ui.button", N_("Interface"), N_("Button"))                                       \
  X(UiButtonHover, "ui.buttonHover", N_("Interface"), N_("Button under the mouse"))             \
  X(UiButtonActive, "ui.buttonActive", N_("Interface"), N_("Button being pressed"))             \
  X(UiHeader, "ui.header", N_("Interface"), N_("Selected item in lists and trees"))             \
  X(UiHeaderHover, "ui.headerHover", N_("Interface"), N_("List item under the mouse"))          \
  X(UiHeaderActive, "ui.headerActive", N_("Interface"), N_("List item being clicked"))          \
  X(UiTab, "ui.tab", N_("Interface"), N_("Tab"))                                                \
  X(UiTabSelected, "ui.tabSelected", N_("Interface"), N_("Selected tab"))                       \
  X(UiTabHover, "ui.tabHover", N_("Interface"), N_("Tab under the mouse"))                      \
  X(UiAccent, "ui.accent", N_("Interface"), N_("Accent (check marks, sliders, tab line)"))      \
  X(UiAccentHover, "ui.accentHover", N_("Interface"), N_("Accent under the mouse"))             \
  X(UiScrollbar, "ui.scrollbar", N_("Interface"), N_("Scroll bar"))                             \
  X(UiScrollbarGrab, "ui.scrollbarGrab", N_("Interface"), N_("Scroll bar handle"))              \
  X(UiScrollbarGrabHover, "ui.scrollbarGrabHover", N_("Interface"), N_("Scroll bar handle under the mouse")) \
  X(UiTableHeader, "ui.tableHeader", N_("Interface"), N_("Table header"))                       \
  X(UiTableRowAlt, "ui.tableRowAlt", N_("Interface"), N_("Alternate table rows"))               \
  X(UiLink, "ui.link", N_("Interface"), N_("Links"))                                            \
  X(UiSelection, "ui.selection", N_("Interface"), N_("Selected text in fields"))                \
  X(UiDockPreview, "ui.dockPreview", N_("Interface"), N_("Where a dragged panel will dock"))     \
  X(UiModalDim, "ui.modalDim", N_("Interface"), N_("Shade behind dialogs"))                     \
  X(DlgTitle, "dialog.title", N_("Dialogs"), N_("Title band"))                                 \
  X(DlgTitleText, "dialog.titleText", N_("Dialogs"), N_("Title text"))                          \
  X(DlgBackground, "dialog.background", N_("Dialogs"), N_("Background"))                       \
  X(DlgBorder, "dialog.border", N_("Dialogs"), N_("Border"))                                    \
  X(DlgStripe1, "dialog.stripe1", N_("Dialogs"), N_("Stripes (window top and dialog titles): top"))             \
  X(DlgStripe2, "dialog.stripe2", N_("Dialogs"), N_("Stripes: middle"))          \
  X(DlgStripe3, "dialog.stripe3", N_("Dialogs"), N_("Stripes: bottom"))          \
  X(DlgPrimary, "dialog.primary", N_("Dialogs"), N_("Main button (Create, Save...)"))           \
  X(DlgPrimaryHover, "dialog.primaryHover", N_("Dialogs"), N_("Main button under the mouse"))   \
  X(DlgPrimaryText, "dialog.primaryText", N_("Dialogs"), N_("Main button text"))                \
  X(EdBackground, "editor.background", N_("Code editor"), N_("Background"))                   \
  X(EdText, "editor.text", N_("Code editor"), N_("Plain text"))                                 \
  X(EdCurrentLine, "editor.currentLine", N_("Code editor"), N_("Line of the cursor"))           \
  X(EdSelection, "editor.selection", N_("Code editor"), N_("Selection"))                        \
  X(EdCursor, "editor.cursor", N_("Code editor"), N_("Cursor"))                                 \
  X(EdMatch, "editor.match", N_("Code editor"), N_("Search results"))                           \
  X(EdMatchCurrent, "editor.matchCurrent", N_("Code editor"), N_("Current search result"))      \
  X(EdErrorLine, "editor.errorLine", N_("Code editor"), N_("Line with an error"))                \
  X(EdWarningLine, "editor.warningLine", N_("Code editor"), N_("Line with a warning"))          \
  X(EdGutter, "editor.gutter", N_("Code editor"), N_("Margin"))                                 \
  X(EdGutterBorder, "editor.gutterBorder", N_("Code editor"), N_("Margin border"))              \
  X(EdLineNumber, "editor.lineNumber", N_("Code editor"), N_("Line numbers"))                   \
  X(EdLineNumberCurrent, "editor.lineNumberCurrent", N_("Code editor"), N_("Number of the cursor's line")) \
  X(EdErrorNumber, "editor.errorNumber", N_("Code editor"), N_("Number of a line with an error")) \
  X(EdWarningNumber, "editor.warningNumber", N_("Code editor"), N_("Number of a line with a warning")) \
  X(EdBreakpoint, "editor.breakpoint", N_("Code editor"), N_("Breakpoint"))                     \
  X(EdCycles, "editor.cycles", N_("Code editor"), N_("Cycle counts"))                           \
  X(EdIndentGuide, "editor.indentGuide", N_("Code editor"), N_("Indentation guides"))           \
  X(EdLink, "editor.link", N_("Code editor"), N_("Symbol under Ctrl/Cmd (click to go to it)"))     \
  X(SynComment, "syntax.comment", N_("Syntax"), N_("Comments"))                                 \
  X(SynString, "syntax.string", N_("Syntax"), N_("Strings"))                                    \
  X(SynNumber, "syntax.number", N_("Syntax"), N_("Numbers"))                                    \
  X(SynMnemonic, "syntax.mnemonic", N_("Syntax"), N_("Instructions and keywords"))              \
  X(SynIllegal, "syntax.illegal", N_("Syntax"), N_("Stable illegal opcodes"))                   \
  X(SynUnstable, "syntax.unstable", N_("Syntax"), N_("Unstable illegal opcodes"))               \
  X(SynRegister, "syntax.register", N_("Syntax"), N_("Hardware registers"))                     \
  X(SynDirective, "syntax.directive", N_("Syntax"), N_("Directives"))                           \
  X(SynLabel, "syntax.label", N_("Syntax"), N_("Labels"))                                       \
  X(SynSymbol, "syntax.symbol", N_("Syntax"), N_("Other symbols"))                              \
  X(MsgError, "messages.error", N_("Messages"), N_("Errors"))                                   \
  X(MsgWarning, "messages.warning", N_("Messages"), N_("Warnings"))                             \
  X(MsgInfo, "messages.info", N_("Messages"), N_("Notes"))                                      \
  X(MsgOk, "messages.ok", N_("Messages"), N_("Found, done"))                                    \
  X(MsgHighlight, "messages.highlight", N_("Messages"), N_("Highlighted values"))               \
  X(MsgName, "messages.name", N_("Messages"), N_("Names in the documentation"))                 \
  X(SpaceOk, "space.ok", N_("Space report"), N_("Room left"))                                   \
  X(SpaceWarning, "space.warning", N_("Space report"), N_("Nearly full"))                       \
  X(SpaceFull, "space.full", N_("Space report"), N_("Full"))                                    \
  X(GfxGrid, "graphics.grid", N_("Graphics editor"), N_("Grid (over the 2600's picture)"))                                \
  X(GfxGridMajor, "graphics.gridMajor", N_("Graphics editor"), N_("Grid every 8 pixels and register limits (over the picture)")) \
  X(GfxOutline, "graphics.outline", N_("Graphics editor"), N_("Selected layer, half of the screen (over the picture)")) \
  X(GfxThumbFrame, "graphics.thumbFrame", N_("Graphics editor"), N_("Frame thumbnails"))        \
  X(GfxThumbSelected, "graphics.thumbSelected", N_("Graphics editor"), N_("Selected frame"))    \
  X(GfxStepSelected, "graphics.stepSelected", N_("Graphics editor"), N_("Selected animation step")) \
  X(GfxLocked, "graphics.locked", N_("Graphics editor"), N_("Columns the kernel does not show (over the picture)")) \
  X(GfxLockedHatch, "graphics.lockedHatch", N_("Graphics editor"), N_("Hatching of those columns")) \
  X(GfxAsymmetric, "graphics.asymmetric", N_("Graphics editor"), N_("Asymmetric row button"))   \
  X(GfxP0, "graphics.p0", N_("Graphics editor"), N_("Layer names: P0"))                         \
  X(GfxP1, "graphics.p1", N_("Graphics editor"), N_("Layer names: P1 and the extra players"))   \
  X(GfxM0, "graphics.m0", N_("Graphics editor"), N_("Layer names: M0"))                         \
  X(GfxM1, "graphics.m1", N_("Graphics editor"), N_("Layer names: M1"))                         \
  X(GfxBL, "graphics.bl", N_("Graphics editor"), N_("Layer names: BL"))                         \
  X(FilesRowHover, "files.rowHover", N_("Project files"), N_("Row under the mouse")) \
  X(FilesRowSelected, "files.rowSelected", N_("Project files"), N_("File being edited")) \
  X(FilesGuide, "files.guide", N_("Project files"), N_("Tree guides")) \
  X(FilesFolder, "files.folder", N_("Project files"), N_("Folders")) \
  X(FilesProject, "files.project", N_("Project files"), N_("Projects (.vcsproj)")) \
  X(FilesAsm, "files.asm", N_("Project files"), N_("Assembly (.asm, .s)")) \
  X(FilesC, "files.c", N_("Project files"), N_("C (.c)")) \
  X(FilesHeader, "files.header", N_("Project files"), N_("Headers and includes (.h, .inc)")) \
  X(FilesBasic, "files.basic", N_("Project files"), N_("batari Basic (.bas)")) \
  X(FilesGraphics, "files.graphics", N_("Project files"), N_("Graphics (.vcsgfx)")) \
  X(FilesRom, "files.rom", N_("Project files"), N_("ROMs (.bin, .a26, .elf)")) \
  X(FilesImage, "files.image", N_("Project files"), N_("Images (.png, .bmp)")) \
  X(FilesListing, "files.listing", N_("Project files"), N_("Listings and symbols (.lst, .sym)")) \
  X(FilesText, "files.text", N_("Project files"), N_("Text and other files"))

enum class Role {
#define X(e, id, group, label) e,
  VCS_THEME_ROLES(X)
#undef X
  Count
};
constexpr int kRoles = (int)Role::Count;

struct RoleInfo {
  const char* id;
  const char* group;  // English; translated where shown
  const char* label;
};
const RoleInfo& info(Role r);
bool roleFromId(const std::string& id, Role* r);

// 0xRRGGBBAA
using Rgba = uint32_t;
bool parseColour(const std::string& text, Rgba* out);  // "#RRGGBB" or "#RRGGBBAA"
std::string colourText(Rgba c);                        // "#RRGGBB", or "#RRGGBBAA" if not opaque

// A theme as written: what its file says, before the base and the references.
struct Theme {
  std::string id;      // file name without .vcstheme ("dark", "solarized-light")
  std::string name;    // for people
  std::string base;    // id of the theme it builds on ("" = none)
  std::string path;    // where it was read from ("" = not saved yet)
  bool builtIn = false;
  std::map<std::string, std::string> palette;   // name -> colour
  std::map<std::string, std::string> colours;   // role id -> "#..", "$palette" or "@role"
  std::vector<std::string> indent;              // colours, "$palette" or "@role"
  bool hasIndent = false;                       // false: the base's
  std::map<std::string, std::string> imgui;     // ImGui colour name (without ImGuiCol_) -> colour
};

std::string toJson(const Theme& t);
bool fromJson(const std::string& text, Theme* t, std::string* error);

// Every colour, ready to use.
struct Resolved {
  Rgba colour[kRoles] = {};
  std::vector<Rgba> indent;
  std::map<std::string, Rgba> imgui;
  // lines that could not be resolved (unknown role, palette entry, theme or a loop)
  std::vector<std::string> problems;
  std::vector<std::string> notes;  // not wrong, but worth knowing
};

// The themes VCSIDE knows: the built-in ones (data/themes) and the user's (configDir()/themes).
class Library {
 public:
  void load(const std::string& builtInDir, const std::string& userDir);
  const std::vector<Theme>& themes() const { return themes_; }
  const Theme* find(const std::string& id) const;
  Theme* find(const std::string& id);
  // Follows the bases (the theme's last), then resolves palette names and references.
  Resolved resolve(const std::string& id) const;
  // A colour as a theme would read it ("$blue/30", "@ui.accent"...), with that theme's palette and roles.
  bool resolveSpec(const std::string& id, const std::string& spec, Rgba* out) const;
  // The palette and colours a theme ends up with after its bases (unresolved text).
  void merged(const std::string& id, std::map<std::string, std::string>* palette, std::map<std::string, std::string>* colours) const;
  // Saves a user theme (id taken from its name if empty) in the user folder.
  bool save(Theme& t, std::string* error);
  bool remove(const std::string& id, std::string* error);
  // Copies a .vcstheme from anywhere (or its text, from the clipboard) into the user folder;
  // *id gets its id. Nothing is overwritten: a theme whose id exists gets a new one.
  bool import(const std::string& path, std::string* id, std::string* error);
  bool importText(const std::string& text, const std::string& fallbackName, std::string* id, std::string* error);
  // A theme ready to give to someone: bases that are not built in are merged into it, so
  // it only needs what every VCSIDE has.
  std::string exportJson(const std::string& id) const;
  std::string uniqueId(const std::string& name) const;  // "My theme" -> "my-theme", "my-theme-2"...
  const std::string& userDir() const { return userDir_; }

 private:
  std::vector<Theme> themes_;
  std::string userDir_;
};

// The colours in use (set by the app when the theme changes).
void setCurrent(const Resolved& r);
const Resolved& current();
Rgba colour(Role r);

}  // namespace vcs::theme
