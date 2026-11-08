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

// The five objects that move freely, and the extra players of kernels that reuse the
// TIA's player 1 down the screen (batari Basic's multisprite, DPC+ and PXE): P2-P16.
enum class Obj { P0, P1, M0, M1, BL, P2, P3, P4, P5, P6, P7, P8, P9, P10, P11, P12, P13, P14, P15, P16 };
constexpr int kObjects = 20;
const char* objName(Obj o);
bool isPlayer(Obj o);
int playerNumber(Obj o);  // 0-16, -1 for missiles and ball
Obj playerObj(int n);
// The register a colour goes to: COLUP0 for P0 and M0, COLUP1 for P1, M1 and the extra
// players (the kernel rewrites it), COLUPF for BL.
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

// An animation: steps that each show a frame for some video frames (1/60 s on NTSC).
struct AnimStep {
  int frame = 0;
  int ticks = 8;
};
struct Animation {
  std::string name = "anim";
  std::vector<AnimStep> steps;
  bool loop = true;            // off: it stays on the last step
  std::string basicTimer, basicStep;  // batari Basic: the variables it uses ("" = to be chosen)
  int length() const;          // video frames of one pass
};

struct Sprite {
  std::string name = "sprite";
  int frames = 1;
  int lineHeight = 2;      // scanlines per row in the preview (1 = 1LK, 2 = 2LK...)
  int frameTicks = 8;      // animation speed in the preview: video frames per animation frame
  std::vector<Layer> layers;
  std::vector<Animation> animations;

  int phases() const;      // flicker phases in use (1 = no flicker)
  void setFrames(int n);
  void insertFrame(int at, int copyOf);  // copyOf -1 = blank; the animations' steps follow
  void removeFrame(int at);              // steps that showed it go
  void swapFrames(int a, int b);
  // The frame an animation shows at a video frame (counted from its start).
  int frameAt(const Animation& a, int tick) const;
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
  std::string kernel;  // "" = the project's; otherwise a kernelId()
};

std::string toJson(const Asset& a);
bool fromJson(const std::string& text, Asset* a, std::string* error);

// ---------------------------------------------------------------------------
// Kernels: what the program that draws the graphics can show.

enum class Kernel { Dasm, Standard, Multisprite, DPCPlus, PXE };
const char* kernelId(Kernel k);    // "dasm", "standard", "multisprite", "dpc+", "pxe"
const char* kernelName(Kernel k);  // for people
bool kernelFromId(const std::string& id, Kernel* k);

// batari Basic's `set kernel_options` and constants that change the standard kernel.
struct KernelOptions {
  bool player1colors = false, playercolors = false, pfcolors = false, pfheights = false;
  bool background = false, noBlankLines = false, readpaddle = false;
  int pfres = 0;  // const pfres, 0 = not set
  std::string text() const;  // "player1colors pfcolors, pfres 12"
};

struct Profile {
  Kernel kernel = Kernel::Dasm;
  KernelOptions options;

  bool basic() const { return kernel != Kernel::Dasm; }
  int players() const;                  // 2, 6, 10 or 17
  bool has(Obj o) const;                // the kernel draws it (with these options)
  std::string missing(Obj o) const;     // why not, if it does not
  bool rowColours(Obj o) const;         // a colour per row
  bool shapedMissiles() const;          // missiles and ball keep their shape (DASM) or are blocks
  bool topFirst() const;                // player data: top row first in the source
  int pfFirst() const;                  // first screen column the kernel shows (0 or 4)
  int pfColumns() const;                // 40, 32 or 16 (16 = left half, mirrored)
  bool pfMirrorOnly() const;
  bool pfAsymmetric() const;            // halves can differ
  bool pfRowModes() const;              // repeat/mirror per row (only a kernel of one's own)
  bool pfRowColours() const;
  bool bkRowColours() const;
  int pfRowsDefault() const;            // rows the kernel shows by default (0 = any)
  int pfRowsMax() const;
  std::string describe() const;         // "batari Basic, standard kernel (player1colors pfcolors)"
};
// The kernel and options a batari Basic program sets (`set kernel`, `set kernel_options`, `const pfres`).
Profile profileFromBasic(const std::string& source);

struct Issue {
  enum Severity { Info, Warning, Error } severity = Warning;
  std::string text;
};
std::vector<Issue> check(const Sprite& s, const Profile& k);
std::vector<Issue> check(const Playfield& p, const Profile& k);

struct ExportOptions {
  bool upsideDown = true;  // tables bottom row first, as most kernels read them (and PlayerPal)
};
// DASM: labelled .byte tables. A player row carries its colour as a ";$xx" comment,
// so the table can be read back (and pasted into PlayerPal).
std::string exportDasm(const Sprite& s, const ExportOptions& o = {});
std::string exportDasm(const Playfield& p, const ExportOptions& o = {});
// The file a program includes (<name>.gfx.asm): every item's tables, under a header that
// names the .vcsgfx they come from.
std::string dasmInclude(const Asset& a, const std::string& sourceName);
// batari Basic: playerN: blocks with their colours, playfield:, pfcolors:, bkcolors:, as the
// kernel of the profile reads them (row order, width, which blocks exist).
std::string exportBasic(const Sprite& s, const Profile& k, int frame = -1);  // frame -1: all of them
// A subroutine that plays an animation: `gosub <sprite>_<animation>` once per frame (before
// drawscreen) advances it and sets the frame's blocks. `<sprite>_<animation>_reset` restarts it.
std::string exportBasicAnimation(const Sprite& s, const Animation& a, const Profile& k);
std::string exportBasic(const Playfield& p, const Profile& k);
std::string identifier(const std::string& name);  // "my hero" -> "my_hero"

// A batari Basic program can hold graphics that VCSIDE keeps up to date: the lines between
//   rem vcsgfx <file>.vcsgfx <item name> [frame N | animation NAME]
//   rem vcsgfx end
// are the item's export (one animation frame, if given) for the asset's kernel.
// basicRegion: the region for an item (sprites first, then playfields), ready to paste;
// a sprite of several frames gives one region per frame, or one per animation if it has them.
std::string basicRegion(const std::string& file, const Asset& a, int item, const Profile& k);
// Rewrites the regions of `file` in a program. *updated counts them; *problems gets a
// line for each region that names an item the asset does not have or has no end.
std::string updateBasicRegions(const std::string& source, const std::string& file, const Asset& a, const Profile& k,
                               int* updated, std::vector<std::string>* problems);

// ---------------------------------------------------------------------------
// Import from text (assembly tables, batari Basic blocks). What was recognised goes
// into the asset; *what says it in a line (empty = nothing found). The kernel decides the
// row order of batari Basic blocks; a program that sets its kernel overrides it.
bool importText(const std::string& text, Asset* out, std::string* what, const Profile& kernel = {});

}  // namespace vcs::gfx
