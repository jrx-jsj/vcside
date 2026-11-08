// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Graphics assets (.vcsgfx): sprites built from the TIA's movable objects and
// playfields that may change from one band of lines to the next. The model knows
// what the hardware does with them, so it can say what a kernel will have to do
// and what a target (DASM with its own kernel, batari Basic) cannot show.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vcs::gfx {

// ---------------------------------------------------------------------------
// Colours

enum class Tv { NTSC, PAL, SECAM };
Tv tvFromProject(const std::string& tv);  // "PAL60" -> PAL, "SECAM60" -> SECAM...
// RGB (0xRRGGBB) of a TIA colour value (bit 0 ignored). The tables are computed
// from the hue/luminance layout of each standard: close, not measured.
uint32_t rgb(Tv tv, int colour);
std::string hexColour(int colour);  // "$1E"

// ---------------------------------------------------------------------------
// Sprites

// The five objects that move freely.
enum class Obj { P0, P1, M0, M1, BL };
const char* objName(Obj o);
bool isPlayer(Obj o);
// The register a colour goes to: COLUP0 for P0 and M0, COLUP1 for P1 and M1, COLUPF for BL.
const char* colourRegister(Obj o);

struct Layer {
  Obj obj = Obj::P0;
  std::string name;        // free text ("helmet"); the export uses the object if empty
  int dx = 0, dy = 0;      // position in the sprite: colour clocks, rows
  int size = 1;            // players: 1, 2, 4 (NUSIZ stretch); missiles and ball: width 1, 2, 4, 8
  bool reflect = false;    // players: REFPx
  int phase = 0;           // flicker: drawn on the frames where frame % phases == phase
  int height = 8;
  // per animation frame, per row (top first): players 8 bits (bit 7 = left); missiles and ball 0/1
  std::vector<std::vector<uint8_t>> bits;
  // per animation frame, per row: TIA colour, -1 = the row above's (the first row: white)
  std::vector<std::vector<int>> colours;

  int width() const { return isPlayer(obj) ? 8 * size : size; }  // in colour clocks
  bool pixel(int frame, int row, int x) const;                     // x in colour clocks from dx
  int colour(int frame, int row) const;                            // resolved, never -1
  void resize(int frames, int height);
};

struct Sprite {
  std::string name = "sprite";
  int frames = 1;
  int lineHeight = 2;      // scanlines per row in the preview (1 = 1LK, 2 = 2LK...)
  int frameTicks = 8;      // animation speed in the preview: video frames per animation frame
  std::vector<Layer> layers;

  int phases() const;      // flicker phases in use (1 = no flicker)
  void setFrames(int n);
  void insertFrame(int at, int copyOf);  // copyOf -1 = blank
  void removeFrame(int at);
};

// ---------------------------------------------------------------------------
// Playfields

enum class PfMode { Repeat, Mirror };  // the right half repeats the left, or mirrors it (CTRLPF bit 0)

struct PfRow {
  uint64_t bits = 0;       // 40 columns, bit 0 = leftmost
  PfMode mode = PfMode::Mirror;
  bool asymmetric = false; // the right half has its own pattern: PF0-PF2 rewritten mid-line
  int pf = -1, bk = -1;    // COLUPF, COLUBK of this row; -1 = the row above's (the first: the defaults)
};

struct Playfield {
  std::string name = "playfield";
  int lineHeight = 8;      // scanlines per row
  int pfColour = 0x0E, bkColour = 0x00;
  std::vector<PfRow> rows = std::vector<PfRow>(24);

  bool pixel(int row, int col) const;    // what the screen shows (symmetric rows: from the left half)
  void setPixel(int row, int col, bool on);  // in a symmetric row the other half follows; editing the
                                             // right half of one makes it asymmetric
  void setAsymmetric(int row, bool on);  // off: the right half goes back to the left's copy
  int pfAt(int row) const;
  int bkAt(int row) const;
  // PF0, PF1, PF2 for the left half and, for asymmetric rows, the right half
  void registers(int row, uint8_t left[3], uint8_t right[3]) const;
};
// The playfield bit behind screen column 0-19 (left half): register 0-2 and bit 0-7.
void pfBit(int col, int* reg, int* bit);

// ---------------------------------------------------------------------------
// The asset: one file holds sprites and playfields.

struct Asset {
  std::vector<Sprite> sprites;
  std::vector<Playfield> playfields;
};

std::string toJson(const Asset& a);
bool fromJson(const std::string& text, Asset* a, std::string* error);

// ---------------------------------------------------------------------------
// Targets, warnings and export

enum class Target { Dasm, Basic };

struct Issue {
  enum Severity { Info, Warning, Error } severity = Warning;
  std::string text;
};
std::vector<Issue> check(const Sprite& s, Target t);
std::vector<Issue> check(const Playfield& p, Target t);

struct ExportOptions {
  bool upsideDown = true;  // tables bottom row first, as most kernels read them (and PlayerPal)
};
// DASM: labelled .byte tables. A player row carries its colour as a ";$xx" comment,
// so the table can be read back (and pasted into PlayerPal).
std::string exportDasm(const Sprite& s, const ExportOptions& o = {});
std::string exportDasm(const Playfield& p, const ExportOptions& o = {});
// batari Basic: player0:/player1: blocks with their colours, playfield:, pfcolors:, bkcolors:.
std::string exportBasic(const Sprite& s);
std::string exportBasic(const Playfield& p);
std::string identifier(const std::string& name);  // "my hero" -> "my_hero"

// ---------------------------------------------------------------------------
// Import from text (assembly tables, batari Basic blocks). What was recognised goes
// into the asset; *what says it in a line (empty = nothing found).
bool importText(const std::string& text, Asset* out, std::string* what);

}  // namespace vcs::gfx
