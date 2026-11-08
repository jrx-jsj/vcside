// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "gfx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>

#include "i18n.h"

using json = nlohmann::json;

namespace vcs::gfx {

// ---------------------------------------------------------------------------
// Colours

Tv tvFromProject(const std::string& tv) {
  if (tv.rfind("PAL", 0) == 0 && tv != "PAL-M") return Tv::PAL;
  if (tv.rfind("SECAM", 0) == 0) return Tv::SECAM;
  return Tv::NTSC;
}

// Luminance in bits 1-3, hue in bits 4-7. NTSC turns the hue around the colour wheel
// in steps of about 26 degrees from gold; PAL alternates two arcs (warm on even hues,
// cool on odd ones) and leaves hues 0, 1, 14 and 15 grey; SECAM ignores the hue and
// gives one of eight fixed colours per luminance.
static uint32_t yiq(double y, double angleDeg, double sat) {
  double a = angleDeg * 3.14159265358979 / 180.0;
  double i = sat * std::cos(a), q = sat * std::sin(a);
  double c[3] = {y + 0.956 * i + 0.621 * q, y - 0.272 * i - 0.647 * q, y - 1.106 * i + 1.703 * q};
  uint32_t out = 0;
  for (double v : c) out = out << 8 | (uint32_t)std::lround(std::clamp(v, 0.0, 1.0) * 255);
  return out;
}

uint32_t rgb(Tv tv, int colour) {
  int hue = (colour >> 4) & 15, lum = (colour >> 1) & 7;
  if (tv == Tv::SECAM) {
    static const uint32_t secam[8] = {0x000000, 0x2121FF, 0xF03C79, 0xFF50FF, 0x7FFF00, 0x7FFFFF, 0xFFFF3F, 0xFFFFFF};
    return secam[lum];
  }
  double y = 0.06 + lum / 7.0 * 0.88;
  if (tv == Tv::NTSC) return hue == 0 ? yiq(y, 0, 0) : yiq(y, -40 + (hue - 1) * 25.7, 0.30);
  if (hue < 2 || hue > 13) return yiq(y, 0, 0);
  if (hue % 2 == 0) return yiq(y, -40 + (hue - 2) / 2 * 30.0, 0.28);
  return yiq(y, -95 - (hue - 3) / 2 * 25.0, 0.28);
}

std::string hexColour(int colour) {
  char b[8];
  std::snprintf(b, sizeof b, "$%02X", colour & 0xFE);
  return b;
}

// ---------------------------------------------------------------------------
// Sprites

const char* objName(Obj o) {
  static const char* n[kObjects] = {"P0", "P1", "M0", "M1", "BL", "P2", "P3", "P4", "P5", "P6",
                                    "P7", "P8", "P9", "P10", "P11", "P12", "P13", "P14", "P15", "P16"};
  return n[(int)o];
}
bool isPlayer(Obj o) { return o == Obj::P0 || o == Obj::P1 || (int)o >= (int)Obj::P2; }
int playerNumber(Obj o) { return o == Obj::P0 ? 0 : o == Obj::P1 ? 1 : (int)o >= (int)Obj::P2 ? (int)o - (int)Obj::P2 + 2 : -1; }
Obj playerObj(int n) { return n == 0 ? Obj::P0 : n == 1 ? Obj::P1 : (Obj)((int)Obj::P2 + n - 2); }
const char* colourRegister(Obj o) {
  return o == Obj::P0 || o == Obj::M0 ? "COLUP0" : o == Obj::BL ? "COLUPF" : "COLUP1";
}
static int objIndex(Obj o) { return (int)o; }

bool Layer::pixel(int frame, int row, int x) const {
  if (frame < 0 || frame >= (int)bits.size() || row < 0 || row >= (int)bits[frame].size() || x < 0 || x >= width()) return false;
  uint8_t v = bits[frame][row];
  if (!isPlayer(obj)) return v != 0;
  int b = x / size;
  if (reflect) b = 7 - b;
  return (v >> (7 - b)) & 1;
}

int Layer::colour(int frame, int row) const {
  if (frame < 0 || frame >= (int)colours.size()) return 0x0E;
  for (int r = std::min(row, (int)colours[frame].size() - 1); r >= 0; r--)
    if (colours[frame][r] >= 0) return colours[frame][r];
  return 0x0E;
}

void Layer::resize(int frames, int h) {
  height = std::max(1, h);
  bits.resize(frames);
  colours.resize(frames);
  for (auto& f : bits) f.resize(height, 0);
  for (auto& f : colours) f.resize(height, -1);
}

int Sprite::phases() const {
  int n = 1;
  for (auto& l : layers) n = std::max(n, l.phase + 1);
  return n;
}

void Sprite::setFrames(int n) {
  frames = std::max(1, n);
  for (auto& l : layers) l.resize(frames, l.height);
}

void Sprite::insertFrame(int at, int copyOf) {
  at = std::clamp(at, 0, frames);
  for (auto& l : layers) {
    l.resize(frames, l.height);
    std::vector<uint8_t> b(l.height, 0);
    std::vector<int> c(l.height, -1);
    if (copyOf >= 0 && copyOf < frames) b = l.bits[copyOf], c = l.colours[copyOf];
    l.bits.insert(l.bits.begin() + at, b);
    l.colours.insert(l.colours.begin() + at, c);
  }
  for (auto& an : animations)
    for (auto& st : an.steps)
      if (st.frame >= at) st.frame++;
  frames++;
}

int Animation::length() const {
  int n = 0;
  for (auto& st : steps) n += std::max(1, st.ticks);
  return n;
}

int Sprite::frameAt(const Animation& a, int tick) const {
  if (a.steps.empty()) return 0;
  int len = a.length();
  tick = a.loop ? tick % len : std::min(tick, len - 1);
  for (auto& st : a.steps) {
    if (tick < std::max(1, st.ticks)) return std::clamp(st.frame, 0, frames - 1);
    tick -= std::max(1, st.ticks);
  }
  return std::clamp(a.steps.back().frame, 0, frames - 1);
}

void Sprite::swapFrames(int a, int b) {
  if (a < 0 || b < 0 || a >= frames || b >= frames || a == b) return;
  for (auto& l : layers) std::swap(l.bits[a], l.bits[b]), std::swap(l.colours[a], l.colours[b]);
  for (auto& an : animations)
    for (auto& st : an.steps) st.frame = st.frame == a ? b : st.frame == b ? a : st.frame;
}

void Sprite::removeFrame(int at) {
  if (frames <= 1 || at < 0 || at >= frames) return;
  for (auto& an : animations) {
    an.steps.erase(std::remove_if(an.steps.begin(), an.steps.end(), [&](const AnimStep& st) { return st.frame == at; }), an.steps.end());
    for (auto& st : an.steps)
      if (st.frame > at) st.frame--;
  }
  for (auto& l : layers) {
    l.resize(frames, l.height);
    l.bits.erase(l.bits.begin() + at);
    l.colours.erase(l.colours.begin() + at);
  }
  frames--;
}

// ---------------------------------------------------------------------------
// Playfields

void pfBit(int j, int* reg, int* bit) {
  if (j < 4) *reg = 0, *bit = 4 + j;            // PF0: bits 4-7, left to right
  else if (j < 12) *reg = 1, *bit = 7 - (j - 4); // PF1: bits 7-0
  else *reg = 2, *bit = j - 12;                  // PF2: bits 0-7
}

bool Playfield::pixel(int row, int col) const {
  if (row < 0 || row >= (int)rows.size() || col < 0 || col >= 40) return false;
  const PfRow& r = rows[row];
  if (col >= 20 && !r.asymmetric) col = r.mode == PfMode::Mirror ? 39 - col : col - 20;
  return (r.bits >> col) & 1;
}

void Playfield::setAsymmetric(int row, bool on) {
  if (row < 0 || row >= (int)rows.size() || rows[row].asymmetric == on) return;
  PfRow& r = rows[row];
  if (on) {
    uint64_t right = 0;
    for (int c = 20; c < 40; c++)
      if (pixel(row, c)) right |= 1ull << c;
    r.bits = (r.bits & 0xFFFFFull) | right;
  } else {
    r.bits &= 0xFFFFFull;
  }
  r.asymmetric = on;
}

void Playfield::setPixel(int row, int col, bool on) {
  if (row < 0 || row >= (int)rows.size() || col < 0 || col >= 40) return;
  if (col >= 20) setAsymmetric(row, true);
  uint64_t m = 1ull << col;
  rows[row].bits = on ? rows[row].bits | m : rows[row].bits & ~m;
}

int Playfield::pfAt(int row) const {
  for (int r = std::min(row, (int)rows.size() - 1); r >= 0; r--)
    if (rows[r].pf >= 0) return rows[r].pf;
  return pfColour;
}
int Playfield::bkAt(int row) const {
  for (int r = std::min(row, (int)rows.size() - 1); r >= 0; r--)
    if (rows[r].bk >= 0) return rows[r].bk;
  return bkColour;
}

void Playfield::registers(int row, uint8_t left[3], uint8_t right[3]) const {
  for (int i = 0; i < 3; i++) left[i] = right[i] = 0;
  int reg, bit;
  for (int j = 0; j < 20; j++)
    if (pixel(row, j)) pfBit(j, &reg, &bit), left[reg] |= 1 << bit;
  const PfRow& r = rows[row];
  for (int c = 20; c < 40; c++) {
    int j = r.mode == PfMode::Mirror ? 39 - c : c - 20;
    if (pixel(row, c)) pfBit(j, &reg, &bit), right[reg] |= 1 << bit;
  }
}

// ---------------------------------------------------------------------------
// JSON. Rows are "X" and "." so the file reads (and diffs) as a picture.

static std::string rowString(uint64_t bits, int n, bool msbFirst) {
  std::string s;
  for (int i = 0; i < n; i++) s += ((bits >> (msbFirst ? n - 1 - i : i)) & 1) ? 'X' : '.';
  return s;
}
static uint64_t rowBits(const std::string& s, bool msbFirst) {
  uint64_t v = 0;
  int n = (int)s.size();
  for (int i = 0; i < n; i++)
    if (s[i] == 'X' || s[i] == 'x' || s[i] == '1' || s[i] == '#') v |= 1ull << (msbFirst ? n - 1 - i : i);
  return v;
}

std::string toJson(const Asset& a) {
  json j;
  j["vcsgfx"] = 1;
  if (!a.kernel.empty()) j["kernel"] = a.kernel;
  j["sprites"] = json::array();
  for (auto& s : a.sprites) {
    json js = {{"name", s.name}, {"frames", s.frames}, {"line_height", s.lineHeight}, {"frame_ticks", s.frameTicks}};
    js["layers"] = json::array();
    for (auto& l : s.layers) {
      json jl = {{"object", objName(l.obj)}, {"name", l.name}, {"dx", l.dx},     {"dy", l.dy},
                 {"size", l.size},           {"reflect", l.reflect}, {"phase", l.phase}, {"height", l.height}};
      jl["frames"] = json::array();
      for (int f = 0; f < (int)l.bits.size(); f++) {
        json rows = json::array();
        for (auto b : l.bits[f]) rows.push_back(rowString(b, isPlayer(l.obj) ? 8 : 1, true));
        jl["frames"].push_back({{"rows", rows}, {"colours", f < (int)l.colours.size() ? l.colours[f] : std::vector<int>{}}});
      }
      js["layers"].push_back(jl);
    }
    if (!s.animations.empty()) {
      js["animations"] = json::array();
      for (auto& an : s.animations) {
        json ja = {{"name", an.name}, {"loop", an.loop}};
        ja["steps"] = json::array();
        for (auto& st : an.steps) ja["steps"].push_back({st.frame, st.ticks});
        if (!an.basicTimer.empty() || !an.basicStep.empty()) ja["basic_vars"] = {an.basicTimer, an.basicStep};
        js["animations"].push_back(ja);
      }
    }
    j["sprites"].push_back(js);
  }
  j["playfields"] = json::array();
  for (auto& p : a.playfields) {
    json jp = {{"name", p.name}, {"line_height", p.lineHeight}, {"pf", p.pfColour}, {"bk", p.bkColour}};
    jp["rows"] = json::array();
    for (auto& r : p.rows) {
      json jr = {{"bits", rowString(r.bits, r.asymmetric ? 40 : 20, false)}, {"mode", r.mode == PfMode::Mirror ? "mirror" : "repeat"}};
      if (r.pf >= 0) jr["pf"] = r.pf;
      if (r.bk >= 0) jr["bk"] = r.bk;
      jp["rows"].push_back(jr);
    }
    j["playfields"].push_back(jp);
  }
  return j.dump(1, ' ') + "\n";
}

bool fromJson(const std::string& text, Asset* a, std::string* error) {
  try {
    json j = json::parse(text);
    Asset out;
    out.kernel = j.value("kernel", "");
    for (auto& js : j.value("sprites", json::array())) {
      Sprite s;
      s.name = js.value("name", s.name);
      s.frames = std::max(1, js.value("frames", 1));
      s.lineHeight = std::clamp(js.value("line_height", 2), 1, 8);
      s.frameTicks = std::clamp(js.value("frame_ticks", 8), 1, 60);
      for (auto& jl : js.value("layers", json::array())) {
        Layer l;
        std::string o = jl.value("object", "P0");
        for (int k = 0; k < kObjects; k++)
          if (o == objName((Obj)k)) l.obj = (Obj)k;
        l.name = jl.value("name", "");
        l.dx = jl.value("dx", 0);
        l.dy = jl.value("dy", 0);
        l.size = jl.value("size", 1);
        l.reflect = jl.value("reflect", false);
        l.phase = std::clamp(jl.value("phase", 0), 0, 3);
        l.resize(s.frames, std::clamp(jl.value("height", 8), 1, 255));
        int f = 0;
        for (auto& jf : jl.value("frames", json::array())) {
          if (f >= s.frames) break;
          int r = 0;
          for (auto& row : jf.value("rows", json::array()))
            if (r < l.height) l.bits[f][r++] = (uint8_t)rowBits(row.get<std::string>(), true);
          r = 0;
          for (auto& c : jf.value("colours", json::array()))
            if (r < l.height) l.colours[f][r++] = c.get<int>();
          f++;
        }
        s.layers.push_back(l);
      }
      for (auto& ja : js.value("animations", json::array())) {
        Animation an;
        an.name = ja.value("name", an.name);
        an.loop = ja.value("loop", true);
        for (auto& st : ja.value("steps", json::array()))
          if (st.is_array() && st.size() == 2)
            an.steps.push_back({std::clamp(st[0].get<int>(), 0, s.frames - 1), std::clamp(st[1].get<int>(), 1, 255)});
        auto vars = ja.value("basic_vars", json::array());
        if (vars.size() == 2) an.basicTimer = vars[0].get<std::string>(), an.basicStep = vars[1].get<std::string>();
        s.animations.push_back(an);
      }
      out.sprites.push_back(s);
    }
    for (auto& jp : j.value("playfields", json::array())) {
      Playfield p;
      p.name = jp.value("name", p.name);
      p.lineHeight = std::clamp(jp.value("line_height", 8), 1, 64);
      p.pfColour = jp.value("pf", p.pfColour);
      p.bkColour = jp.value("bk", p.bkColour);
      p.rows.clear();
      for (auto& jr : jp.value("rows", json::array())) {
        PfRow r;
        std::string b = jr.value("bits", "");
        r.bits = rowBits(b, false);
        r.asymmetric = b.size() > 20;
        r.mode = jr.value("mode", "mirror") == "repeat" ? PfMode::Repeat : PfMode::Mirror;
        r.pf = jr.value("pf", -1);
        r.bk = jr.value("bk", -1);
        p.rows.push_back(r);
      }
      if (p.rows.empty()) p.rows.resize(1);
      out.playfields.push_back(p);
    }
    *a = out;
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

// ---------------------------------------------------------------------------
// Kernels. What each batari Basic kernel shows was checked with test programs built
// with batari Basic 1.9 and run in Stella 7.0 (row order, columns, colours), and in the
// kernels' sources (includes/*.asm, *.h).

const char* kernelId(Kernel k) {
  static const char* n[] = {"dasm", "standard", "multisprite", "dpc+", "pxe"};
  return n[(int)k];
}
const char* kernelName(Kernel k) {
  switch (k) {
    case Kernel::Dasm: return T("assembly (a kernel of one's own)");
    case Kernel::Standard: return T("batari Basic, standard kernel");
    case Kernel::Multisprite: return T("batari Basic, multisprite kernel");
    case Kernel::DPCPlus: return T("batari Basic, DPC+ kernel");
    case Kernel::PXE: return T("batari Basic, PXE kernel");
  }
  return "";
}
bool kernelFromId(const std::string& id, Kernel* k) {
  for (int i = 0; i < 5; i++)
    if (id == kernelId((Kernel)i)) return *k = (Kernel)i, true;
  return false;
}

std::string KernelOptions::text() const {
  std::string s;
  auto add = [&](bool on, const char* n) {
    if (on) s += (s.empty() ? "" : " ") + std::string(n);
  };
  add(player1colors, "player1colors");
  add(playercolors, "playercolors");
  add(pfcolors, "pfcolors");
  add(pfheights, "pfheights");
  add(background, "background");
  add(noBlankLines, "no_blank_lines");
  add(readpaddle, "readpaddle");
  if (pfres) s += (s.empty() ? "" : ", ") + std::string("pfres ") + std::to_string(pfres);
  return s;
}

int Profile::players() const {
  switch (kernel) {
    case Kernel::Multisprite: return 6;
    case Kernel::DPCPlus: return 10;
    case Kernel::PXE: return 17;
    default: return 2;
  }
}

std::string Profile::missing(Obj o) const {
  int n = playerNumber(o);
  if (n >= 0) return n < players() ? "" : F("{0} draws {1} players (P0-P{2}).", {kernelName(kernel), std::to_string(players()), std::to_string(players() - 1)});
  if (kernel != Kernel::Standard) return "";
  // the standard kernel spends the missiles' time on other things
  if (o == Obj::M1 && (options.player1colors || options.playercolors)) return T("The kernel option player1colors (or playercolors) uses missile 1's time.");
  if (o == Obj::M0 && options.playercolors) return T("The kernel option playercolors uses both missiles' time.");
  if (o == Obj::M0 && (options.noBlankLines || options.readpaddle)) return T("The kernel options no_blank_lines and readpaddle use missile 0's time.");
  return "";
}
bool Profile::has(Obj o) const { return missing(o).empty(); }

bool Profile::rowColours(Obj o) const {
  switch (kernel) {
    case Kernel::Dasm: return true;
    case Kernel::Standard:
      if (o == Obj::P0) return options.playercolors;
      if (o == Obj::P1) return options.player1colors || options.playercolors;
      return false;
    case Kernel::Multisprite: return false;
    default: return isPlayer(o);  // DPC+ and PXE: playerNcolor:
  }
}
bool Profile::shapedMissiles() const { return kernel == Kernel::Dasm; }
bool Profile::topFirst() const { return kernel == Kernel::Dasm || kernel == Kernel::DPCPlus || kernel == Kernel::PXE; }
int Profile::pfFirst() const { return kernel == Kernel::Dasm ? 0 : 4; }
int Profile::pfColumns() const { return kernel == Kernel::Dasm ? 40 : kernel == Kernel::Multisprite ? 16 : 32; }
bool Profile::pfMirrorOnly() const { return kernel == Kernel::Multisprite; }
bool Profile::pfAsymmetric() const { return kernel != Kernel::Multisprite; }
bool Profile::pfRowModes() const { return kernel == Kernel::Dasm; }
bool Profile::pfRowColours() const {
  if (kernel == Kernel::Standard) return options.pfcolors && !options.background;
  return kernel != Kernel::Multisprite;
}
bool Profile::bkRowColours() const {
  if (kernel == Kernel::Standard) return options.pfcolors && options.background;  // the same table, to COLUBK
  return kernel == Kernel::Dasm || kernel == Kernel::DPCPlus || kernel == Kernel::PXE;
}
int Profile::pfRowsDefault() const {
  if (kernel == Kernel::Standard) return options.pfres ? options.pfres : 11;
  if (kernel == Kernel::Multisprite) return 11;
  return 0;
}
int Profile::pfRowsMax() const {
  switch (kernel) {
    case Kernel::Standard: return 32;  // pfres above 12 needs cartridge RAM
    case Kernel::Multisprite: return 44;
    case Kernel::DPCPlus:
    case Kernel::PXE: return 176;
    default: return 192;
  }
}
std::string Profile::describe() const {
  std::string o = kernel == Kernel::Standard ? options.text() : "";
  return o.empty() ? kernelName(kernel) : std::string(kernelName(kernel)) + " (" + o + ")";
}

Profile profileFromBasic(const std::string& source) {
  Profile p;
  p.kernel = Kernel::Standard;
  std::istringstream in(source);
  for (std::string line; std::getline(in, line);) {
    std::string l;
    for (char c : line.substr(0, line.find(';'))) l += (char)std::tolower((unsigned char)c);
    std::istringstream w(l);
    std::string a, b;
    w >> a >> b;
    if (a == "rem") continue;
    if (a == "set" && b == "kernel") {
      std::string k;
      w >> k;
      if (k == "multisprite") p.kernel = Kernel::Multisprite;
      else if (k == "dpc+") p.kernel = Kernel::DPCPlus;
      else if (k == "pxe") p.kernel = Kernel::PXE;
    } else if (a == "set" && b == "kernel_options") {
      for (std::string o; w >> o;) {
        p.options.player1colors |= o == "player1colors";
        p.options.playercolors |= o == "playercolors";
        p.options.pfcolors |= o == "pfcolors";
        p.options.pfheights |= o == "pfheights";
        p.options.background |= o == "background" || o == "backgroundchange";
        p.options.noBlankLines |= o == "no_blank_lines";
        p.options.readpaddle |= o == "readpaddle";
      }
    } else if (a == "const" && b.rfind("pfres", 0) == 0) {
      size_t eq = l.find('=');
      if (eq != std::string::npos) p.options.pfres = std::atoi(l.c_str() + eq + 1);
    }
  }
  return p;
}

// ---------------------------------------------------------------------------
// Warnings

static std::string rowList(const std::vector<int>& rows) {
  std::string s;
  for (size_t i = 0; i < rows.size() && i < 8; i++) s += (i ? ", " : "") + std::to_string(rows[i]);
  if (rows.size() > 8) s += ", …";
  return s;
}

static bool rowHasPixels(const Layer& l, int frame, int row) {
  return frame < (int)l.bits.size() && row >= 0 && row < (int)l.bits[frame].size() && l.bits[frame][row] != 0;
}

static bool coloursVary(const Layer& l) {
  for (int f = 0; f < (int)l.bits.size(); f++)
    for (int r = 1; r < l.height; r++)
      if (l.colour(f, r) != l.colour(f, 0)) return true;
  return false;
}

std::vector<Issue> check(const Sprite& s, const Profile& k) {
  std::vector<Issue> out;
  int phases = s.phases();
  // objects the kernel does not draw
  std::set<int> told;
  for (auto& l : s.layers)
    if (!k.has(l.obj) && told.insert((int)l.obj).second)
      out.push_back({Issue::Error, F("{0} is not available: {1}", {objName(l.obj), k.missing(l.obj)})});
  // one object per phase
  for (int p = 0; p < phases; p++) {
    int used[kObjects] = {};
    for (auto& l : s.layers)
      if (l.phase == p) used[objIndex(l.obj)]++;
    for (int o = 0; o < kObjects; o++)
      if (used[o] > 1)
        out.push_back({Issue::Error, F("Phase {0} uses {1} in {2} layers: the TIA has only one {1}. Move one of them to another phase.",
                                       {std::to_string(p + 1), objName((Obj)o), std::to_string(used[o])})});
  }
  if (phases > 1) {
    std::vector<int> empty;
    for (int p = 0; p < phases; p++)
      if (std::none_of(s.layers.begin(), s.layers.end(), [&](const Layer& l) { return l.phase == p; })) empty.push_back(p + 1);
    out.push_back({Issue::Warning, F("{0} flicker phases: each layer shows one frame in {0} ({1} Hz on NTSC, {2} Hz on PAL) and flickers.",
                                     {std::to_string(phases), std::to_string(60 / phases), std::to_string(50 / phases)})});
    if (!empty.empty()) out.push_back({Issue::Warning, F("Phases without layers: {0}. The other layers flicker more than they need to.", {rowList(empty)})});
  }
  // animations: names become labels
  std::set<std::string> animNames;
  for (auto& an : s.animations) {
    if (an.steps.empty()) out.push_back({Issue::Warning, F("The animation {0} has no steps.", {an.name})});
    if (!animNames.insert(identifier(an.name)).second)
      out.push_back({Issue::Error, F("Two animations are called {0}: their labels would clash.", {an.name})});
  }
  // the kernels with more players draw them all with the TIA's player 1
  if (k.players() > 2) {
    bool shared = false;
    for (size_t a = 0; a < s.layers.size() && !shared; a++)
      for (size_t b = a + 1; b < s.layers.size() && !shared; b++) {
        const Layer &la = s.layers[a], &lb = s.layers[b];
        shared = la.phase == lb.phase && playerNumber(la.obj) >= 1 && playerNumber(lb.obj) >= 1 && la.obj != lb.obj &&
                 la.dy < lb.dy + lb.height && lb.dy < la.dy + la.height;
      }
    if (shared)
      out.push_back({Issue::Warning, F("P1 to P{0} are all drawn with the TIA's player 1: where two of them share lines the kernel shows them on alternate frames (they flicker). Put them in different rows, or combine P0 with one of them.",
                                       {std::to_string(k.players() - 1)})});
  }
  // a missile shares its player's colour register
  for (auto& m : s.layers) {
    if (m.obj != Obj::M0 && m.obj != Obj::M1) continue;
    Obj pl = m.obj == Obj::M0 ? Obj::P0 : Obj::P1;
    for (auto& p : s.layers) {
      if (p.obj != pl || p.phase != m.phase) continue;
      std::vector<int> bad;
      for (int f = 0; f < s.frames; f++)
        for (int r = 0; r < m.height; r++) {
          int pr = r + m.dy - p.dy;
          if (rowHasPixels(m, f, r) && pr >= 0 && pr < p.height && m.colour(f, r) != p.colour(f, pr) &&
              std::find(bad.begin(), bad.end(), r + m.dy + 1) == bad.end())
            bad.push_back(r + m.dy + 1);
        }
      if (!bad.empty())
        out.push_back({Issue::Warning, F("{0} takes the colour of {1} ({2}). On rows {3} they differ: {0} will show {1}'s.",
                                         {objName(m.obj), objName(pl), colourRegister(pl), rowList(bad)})});
    }
  }
  for (auto& b : s.layers)
    if (b.obj == Obj::BL)
      out.push_back({Issue::Info, coloursVary(b) ? T("The ball's colour is the playfield's (COLUPF): changing it per row recolours the playfield on those lines.")
                                                 : T("The ball's colour is the playfield's (COLUPF).")});
  // overlaps: the TIA's priority decides, not the layer order
  for (int p = 0; p < phases; p++) {
    bool overlap = false;
    for (size_t a = 0; a < s.layers.size() && !overlap; a++)
      for (size_t b = a + 1; b < s.layers.size() && !overlap; b++) {
        const Layer &la = s.layers[a], &lb = s.layers[b];
        if (la.phase != p || lb.phase != p || la.obj == lb.obj) continue;
        for (int f = 0; f < s.frames && !overlap; f++)
          for (int r = 0; r < la.height && !overlap; r++)
            for (int x = 0; x < la.width() && !overlap; x++)
              overlap = la.pixel(f, r, x) && lb.pixel(f, r + la.dy - lb.dy, x + la.dx - lb.dx);
      }
    if (overlap) {
      out.push_back({Issue::Info, T("Some layers overlap: the TIA draws P0 and M0 over P1 and M1, and those over the ball and playfield (the preview does the same).")});
      break;
    }
  }
  if (!k.basic()) {
    // what the kernel has to write per line
    for (int p = 0; p < phases; p++) {
      int writes = 0;
      std::set<std::string> regs;
      for (auto& l : s.layers) {
        if (l.phase != p) continue;
        writes++;
        if (coloursVary(l) && regs.insert(colourRegister(l.obj)).second) writes++;
      }
      if (writes > 4)
        out.push_back({Issue::Warning, F("Phase {0} changes {1} registers on the same lines (graphics and colours): about {2} cycles of loads and stores of the 76 a line has. It may need a two-line kernel.",
                                         {std::to_string(p + 1), std::to_string(writes), std::to_string(writes * 8)})});
    }
    return out;
  }
  for (auto& l : s.layers) {
    if (isPlayer(l.obj) || !k.has(l.obj)) continue;
    if (k.kernel == Kernel::Multisprite && l.obj != Obj::BL) {
      out.push_back({Issue::Warning, F("The multisprite kernel draws {0} one line high: only its position is kept.", {objName(l.obj)})});
      continue;
    }
    // batari Basic's missiles and ball are a block: height and width
    bool block = true;
    for (int f = 0; f < s.frames && block; f++) {
      int first = -1, last = -1;
      for (int r = 0; r < l.height; r++)
        if (rowHasPixels(l, f, r)) {
          if (first < 0) first = r;
          if (last >= 0 && last != r - 1) block = false;
          last = r;
        }
      if (f > 0 && l.bits[f] != l.bits[0]) block = false;
    }
    if (!block)
      out.push_back({Issue::Warning, F("batari Basic draws {0} as a solid block (a height and a width): its shape here is not kept, only its height.", {objName(l.obj)})});
  }
  if (k.kernel == Kernel::Standard && k.options.playercolors && !k.options.player1colors)
    out.push_back({Issue::Warning, T("The kernel option playercolors works together with player1colors: `set kernel_options player1colors playercolors`.")});
  if (phases > 1)
    out.push_back({Issue::Warning, T("batari Basic does not alternate the phases by itself: each one is exported on its own, and the program has to switch them every frame.")});
  for (auto& l : s.layers) {
    if (!isPlayer(l.obj) || !k.has(l.obj) || !coloursVary(l) || k.rowColours(l.obj)) continue;
    if (k.kernel == Kernel::Standard)
      out.push_back({Issue::Warning, F("{0} changes colour per row, and the program does not set `set kernel_options {1}`: only the first row's colour is kept.",
                                       {objName(l.obj), l.obj == Obj::P0 ? "playercolors" : "player1colors"})});
    else
      out.push_back({Issue::Warning, F("{0} changes colour per row, but this kernel gives each player one colour: only the first row's is kept.", {objName(l.obj)})});
  }
  if (s.layers.size() > 1) out.push_back({Issue::Info, T("The layers' positions are exported as comments: the program places each object relative to the first one.")});
  for (auto& an : s.animations)
    if (an.basicTimer.empty() || an.basicStep.empty())
      out.push_back({Issue::Warning, F("The animation {0} needs two batari Basic variables (its counter and its step): choose them in the animation.", {an.name})});
  return out;
}

std::vector<Issue> check(const Playfield& p, const Profile& k) {
  std::vector<Issue> out;
  std::vector<int> asym, modeChange, pfChange, bkChange;
  for (int r = 0; r < (int)p.rows.size(); r++) {
    if (p.rows[r].asymmetric) asym.push_back(r + 1);
    if (r > 0 && p.rows[r].mode != p.rows[r - 1].mode) modeChange.push_back(r + 1);
    if (r > 0 && p.pfAt(r) != p.pfAt(r - 1)) pfChange.push_back(r + 1);
    if (r > 0 && p.bkAt(r) != p.bkAt(r - 1)) bkChange.push_back(r + 1);
  }
  if (!k.basic()) {
    if (!asym.empty()) {
      out.push_back({Issue::Info, F("Asymmetric rows: {0}. On every line of them PF0, PF1 and PF2 are written again in the middle of the line, each one in its own window of cycles.", {rowList(asym)})});
      bool mirrorAsym = std::any_of(p.rows.begin(), p.rows.end(), [](const PfRow& r) { return r.asymmetric && r.mode == PfMode::Mirror; });
      if (mirrorAsym)
        out.push_back({Issue::Warning, T("An asymmetric row in mirror mode shows PF2 twice back to back in the middle of the line: its write has almost no margin. Repeat mode is easier.")});
    }
    if (!modeChange.empty())
      out.push_back({Issue::Info, F("Repeat/mirror changes on rows {0}: the kernel writes CTRLPF there, and CTRLPF also holds the ball's size and the playfield priority.", {rowList(modeChange)})});
    int writes = 3 + (asym.empty() ? 0 : 3) + (pfChange.empty() ? 0 : 1) + (bkChange.empty() ? 0 : 1);
    if (writes >= 7)
      out.push_back({Issue::Warning, F("Some lines change {0} registers (playfield and colours): together with sprites it may not fit in one line.", {std::to_string(writes)})});
    return out;
  }
  // columns the kernel does not show
  int first = k.pfFirst(), last = k.pfMirrorOnly() ? 19 : 39 - first;
  std::vector<int> lost, notMirror;
  for (int r = 0; r < (int)p.rows.size(); r++) {
    bool l = false, m = false;
    for (int c = 0; c < 40; c++) {
      bool shown = c >= first && c <= last;
      if (k.pfMirrorOnly() && c >= 20) shown = 39 - c >= first, m |= p.pixel(r, c) != p.pixel(r, 39 - c);
      l |= !shown && p.pixel(r, c);
    }
    if (l) lost.push_back(r + 1);
    if (m) notMirror.push_back(r + 1);
  }
  if (!lost.empty())
    out.push_back({Issue::Warning, F("This kernel shows columns {0} to {1} only: what is outside them is lost (rows {2}).",
                                     {std::to_string(first + 1), std::to_string(40 - first), rowList(lost)})});
  if (!notMirror.empty())
    out.push_back({Issue::Warning, F("The multisprite kernel's playfield is mirrored (the right half is the left one reversed); these rows are not: {0}.", {rowList(notMirror)})});
  int rows = (int)p.rows.size();
  if (rows > k.pfRowsMax())
    out.push_back({Issue::Error, F("{0} rows: this kernel shows {1} at most.", {std::to_string(rows), std::to_string(k.pfRowsMax())})});
  else if (k.pfRowsDefault() && rows != k.pfRowsDefault()) {
    if (k.kernel == Kernel::Standard)
      out.push_back({Issue::Warning, F("{0} rows, and the program shows {1}: it needs `const pfres = {0}` (only some values are allowed, and more rows take more RAM).",
                                       {std::to_string(rows), std::to_string(k.pfRowsDefault())})});
    else
      out.push_back({Issue::Warning, F("{0} rows: this kernel shows {1}.", {std::to_string(rows), std::to_string(k.pfRowsDefault())})});
  }
  out.push_back({Issue::Info, T("batari Basic sets the height of the rows itself; the line height here is only for the preview.")});
  bool pfVary = !pfChange.empty(), bkVary = !bkChange.empty();
  if (pfVary && !k.pfRowColours()) {
    if (k.kernel != Kernel::Standard) out.push_back({Issue::Warning, T("This kernel gives the playfield one colour: only the first row's is kept.")});
    else if (k.options.background) out.push_back({Issue::Warning, T("With the kernel option background the table of colours goes to the background: the playfield keeps one colour.")});
    else out.push_back({Issue::Warning, T("The rows have their own colours, and the program does not set `set kernel_options pfcolors`: only the first row's is kept.")});
  }
  if (bkVary && !k.bkRowColours()) {
    if (k.kernel == Kernel::Standard)
      out.push_back({Issue::Warning, k.options.pfcolors ? T("The background changes per row: the kernel options would have to be pfcolors and background, and then the playfield keeps one colour.")
                                                        : T("The background changes per row: it needs `set kernel_options pfcolors background` (and then the playfield keeps one colour).")});
    else out.push_back({Issue::Warning, T("This kernel does not change the background per row: only the first row's is kept.")});
  }
  return out;
}

// ---------------------------------------------------------------------------
// Export

std::string identifier(const std::string& name) {
  std::string s;
  for (char c : name) s += std::isalnum((unsigned char)c) ? c : '_';
  if (s.empty() || std::isdigit((unsigned char)s[0])) s = "g" + s;
  return s;
}

// "P0", "P0_helmet", "P0b" (a second P0 layer, in another phase)
static std::string layerId(const Layer& l, const std::vector<Layer>& all) {
  int same = 0, mine = 0;
  for (auto& o : all)
    if (o.obj == l.obj) {
      if (&o == &l) mine = same;
      same++;
    }
  std::string id = objName(l.obj);
  if (same > 1 && mine > 0) id += (char)('a' + std::min(mine, 25));
  if (!l.name.empty()) id += "_" + identifier(l.name);
  return id;
}

static std::string bin8(int v) {
  std::string s = "%";
  for (int b = 7; b >= 0; b--) s += (v >> b) & 1 ? '1' : '0';
  return s;
}

std::string exportDasm(const Sprite& s, const ExportOptions& o) {
  std::ostringstream out;
  std::string n = identifier(s.name);
  out << "; " << s.name << ": " << s.layers.size() << " layer(s), " << s.frames << " frame(s), " << s.phases()
      << " phase(s). Rows: " << (o.upsideDown ? "bottom first" : "top first") << ".\n";
  for (auto& l : s.layers) {
    std::string id = n + "_" + layerId(l, s.layers);
    out << "\n; " << objName(l.obj) << (l.name.empty() ? "" : " (" + l.name + ")") << ", phase " << l.phase + 1 << ", "
        << colourRegister(l.obj) << "\n";
    out << id << "_DX = " << l.dx << "\n" << id << "_DY = " << l.dy << "\n" << id << "_HEIGHT = " << l.height << "\n";
    out << id << "_PHASE = " << l.phase << "\n";
    if (isPlayer(l.obj)) {
      out << id << "_NUSIZ = " << (l.size == 4 ? "$07" : l.size == 2 ? "$05" : "$00") << "\n";
      if (l.reflect) out << id << "_REFP = $08\n";
    } else {
      int bits = l.size >= 8 ? 3 : l.size >= 4 ? 2 : l.size >= 2 ? 1 : 0;
      out << id << "_SIZE = $" << bits << "0" << (l.obj == Obj::BL ? "  ; CTRLPF bits 4-5\n" : "  ; NUSIZ bits 4-5\n");
    }
    for (int f = 0; f < s.frames; f++) {
      out << id << "_Frame" << f << "\n";
      for (int i = 0; i < l.height; i++) {
        int r = o.upsideDown ? l.height - 1 - i : i;
        int v = f < (int)l.bits.size() ? l.bits[f][r] : 0;
        if (isPlayer(l.obj)) out << "        .byte #" << bin8(v) << ";" << hexColour(l.colour(f, r)) << "\n";
        else out << "        .byte #" << bin8(v ? 2 : 0) << "\n";  // ENAMx/ENABL: bit 1
      }
    }
    if (l.obj != Obj::M0 && l.obj != Obj::M1)
      for (int f = 0; f < s.frames; f++) {
        out << id << "_Colour" << f << "\n";
        for (int i = 0; i < l.height; i++) out << "        .byte #" << hexColour(l.colour(f, o.upsideDown ? l.height - 1 - i : i)) << "\n";
      }
    if (s.frames > 1) {
      // the frames by number: lda Ptr,x
      auto pointers = [&](const char* table, const char* label) {
        for (const char* half : {"Lo", "Hi"}) {
          out << id << "_" << table << "Ptr" << half << "\n        .byte ";
          for (int f = 0; f < s.frames; f++) out << (f ? "," : "") << (half[0] == 'L' ? "<" : ">") << id << "_" << label << f;
          out << "\n";
        }
      };
      pointers("Frame", "Frame");
      if (l.obj != Obj::M0 && l.obj != Obj::M1) pointers("Colour", "Colour");
    }
  }
  out << "\n" << n << "_FRAMES = " << s.frames << "\n";
  for (auto& an : s.animations) {
    // steps: the frame and how many video frames it stays
    std::string a = n + "_" + identifier(an.name);
    out << "\n; animation " << an.name << ": " << an.steps.size() << " steps, " << an.length() << " video frames"
        << (an.loop ? ", loops" : ", stops on the last step") << "\n";
    out << a << "_STEPS = " << an.steps.size() << "\n" << a << "_LOOP = " << (an.loop ? 1 : 0) << "\n";
    out << a << "_FRAME\n        .byte ";
    for (size_t i = 0; i < an.steps.size(); i++) out << (i ? "," : "") << an.steps[i].frame;
    out << "\n" << a << "_TICKS\n        .byte ";
    for (size_t i = 0; i < an.steps.size(); i++) out << (i ? "," : "") << an.steps[i].ticks;
    out << "\n";
  }
  return out.str();
}

std::string exportDasm(const Playfield& p, const ExportOptions& o) {
  std::ostringstream out;
  std::string n = identifier(p.name);
  int rows = (int)p.rows.size();
  bool anyAsym = std::any_of(p.rows.begin(), p.rows.end(), [](const PfRow& r) { return r.asymmetric; });
  bool modes = std::any_of(p.rows.begin(), p.rows.end(), [&](const PfRow& r) { return r.mode != p.rows[0].mode; });
  bool pfVary = false, bkVary = false;
  for (int r = 1; r < rows; r++) pfVary |= p.pfAt(r) != p.pfAt(0), bkVary |= p.bkAt(r) != p.bkAt(0);
  out << "; " << p.name << ": " << rows << " rows of " << p.lineHeight << " lines"
      << (anyAsym ? ", asymmetric rows" : "") << ". Rows: " << (o.upsideDown ? "bottom first" : "top first") << ".\n";
  out << n << "_ROWS = " << rows << "\n" << n << "_LINES = " << p.lineHeight << "\n";
  out << n << "_CTRLPF = " << (p.rows[0].mode == PfMode::Mirror ? "$01" : "$00") << "  ; first row\n";
  out << n << "_COLUPF = " << hexColour(p.pfAt(0)) << "\n" << n << "_COLUBK = " << hexColour(p.bkAt(0)) << "\n";
  auto table = [&](const std::string& label, auto value, bool picture) {
    out << n << "_" << label << "\n";
    for (int i = 0; i < rows; i++) {
      int r = o.upsideDown ? rows - 1 - i : i;
      out << "        .byte " << value(r);
      if (picture) {
        out << "  ; |";
        for (int c = 0; c < 40; c++) out << (p.pixel(r, c) ? 'X' : '.');
        out << "|";
      }
      out << "\n";
    }
  };
  const char* reg[3] = {"PF0", "PF1", "PF2"};
  for (int k = 0; k < 3; k++)
    table(reg[k], [&](int r) { uint8_t L[3], R[3]; p.registers(r, L, R); return bin8(L[k]); }, k == 0);
  if (anyAsym)
    for (int k = 0; k < 3; k++)
      table(std::string(reg[k]) + "R", [&](int r) { uint8_t L[3], R[3]; p.registers(r, L, R); return bin8(R[k]); }, false);
  if (modes) table("CTRLPF_ROWS", [&](int r) { return std::string(p.rows[r].mode == PfMode::Mirror ? "$01" : "$00"); }, false);
  if (pfVary) table("COLUPF_ROWS", [&](int r) { return hexColour(p.pfAt(r)); }, false);
  if (bkVary) table("COLUBK_ROWS", [&](int r) { return hexColour(p.bkAt(r)); }, false);
  return out.str();
}

// The profile to use when the asset is for a kernel of one's own and still copied as
// batari Basic: the standard kernel with the options the graphics need.
static Profile basicFor(const Sprite& s, const Profile& k) {
  if (k.basic()) return k;
  Profile b;
  b.kernel = Kernel::Standard;
  for (auto& l : s.layers)
    if (coloursVary(l)) (l.obj == Obj::P0 ? b.options.playercolors : b.options.player1colors) = true;
  return b;
}
static Profile basicFor(const Playfield& p, const Profile& k) {
  if (k.basic()) return k;
  Profile b;
  b.kernel = Kernel::Standard;
  for (int r = 1; r < (int)p.rows.size(); r++) b.options.pfcolors |= p.pfAt(r) != p.pfAt(0);
  b.options.pfcolors |= p.rows[0].pf >= 0;
  return b;
}

static std::string basicName(Obj o) {
  int n = playerNumber(o);
  if (n >= 0) return "player" + std::to_string(n);
  return o == Obj::M0 ? "missile0" : o == Obj::M1 ? "missile1" : "ball";
}

// The variable for a player's colour or size: COLUP0, _COLUP1 in the kernels with more
// players (their COLUP1 is the TIA's, rewritten down the screen), COLUP2...
static std::string playerVar(const char* reg, int n, const Profile& k) {
  if (n == 1 && k.players() > 2) return "_" + std::string(reg) + "1";
  return reg + std::to_string(n);
}

std::string exportBasic(const Sprite& whole, const Profile& in, int frame) {
  Sprite s = whole;
  if (frame >= 0 && frame < whole.frames) {
    // just that frame
    for (auto& l : s.layers) {
      l.bits = {l.bits[frame]};
      l.colours = {l.colours[frame]};
    }
    s.frames = 1;
  }
  Profile k = basicFor(s, in);
  std::ostringstream out;
  bool one = frame >= 0 && frame < whole.frames;
  if (one) out << "  rem " << s.name << ", frame " << frame + 1 << "; " << k.describe() << "\n";
  else out << "  rem " << s.name << ": " << s.frames << " frame(s), " << s.phases() << " phase(s); " << k.describe() << "\n";
  const Layer* first = nullptr;
  for (auto& l : s.layers)
    if (isPlayer(l.obj) && k.has(l.obj) && !first) first = &l;
  for (int ph = 0; ph < s.phases(); ph++)
    for (int f = 0; f < s.frames; f++) {
      if (!one || s.phases() > 1) out << "  rem frame " << (one ? frame : f) + 1 << (s.phases() > 1 ? ", phase " + std::to_string(ph + 1) : "") << "\n";
      for (auto& l : s.layers) {
        if (l.phase != ph || !isPlayer(l.obj) || !k.has(l.obj)) continue;
        std::string pn = basicName(l.obj);
        auto row = [&](int i) { return k.topFirst() ? i : l.height - 1 - i; };
        out << "  " << pn << ":\n";
        for (int i = 0; i < l.height; i++) out << "  " << bin8(l.bits[f][row(i)]) << "\n";
        out << "end\n";
        if (k.rowColours(l.obj)) {
          out << "  " << pn << "color:\n";
          for (int i = 0; i < l.height; i++) out << "  " << hexColour(l.colour(f, row(i))) << "\n";
          out << "end\n";
        } else {
          // the kernel and the score reload the colour registers: set them before every drawscreen
          out << "  " << playerVar("COLUP", playerNumber(l.obj), k) << " = " << hexColour(l.colour(f, 0)) << " : rem every frame\n";
        }
      }
    }
  for (auto& l : s.layers) {
    std::string o = basicName(l.obj);
    if (!k.has(l.obj)) {
      out << "  rem " << objName(l.obj) << ": not drawn by this kernel\n";
      continue;
    }
    int n = playerNumber(l.obj);
    if (n >= 0) {
      int nusiz = (l.size == 4 ? 7 : l.size == 2 ? 5 : 0);
      bool refpInNusiz = k.kernel == Kernel::DPCPlus || k.kernel == Kernel::PXE;  // their NUSIZ bit 3 reflects
      if (refpInNusiz && l.reflect) nusiz |= 8;
      if (nusiz) out << "  " << playerVar("NUSIZ", n, k) << " = $" << std::hex << std::uppercase << nusiz << std::dec << "\n";
      if (l.reflect && !refpInNusiz) {
        if (n <= 1 && k.kernel == Kernel::Standard) out << "  REFP" << n << " = 8\n";
        else out << "  rem " << o << ": reflected\n";
      }
    } else if (k.kernel != Kernel::Multisprite || l.obj == Obj::BL) {
      int h = 0;
      for (int r = 0; r < l.height; r++) h += rowHasPixels(l, 0, r);
      out << "  " << o << "height = " << h << "\n";
    }
    if (first && &l != first)
      out << "  rem " << o << "x = " << basicName(first->obj) << "x + " << l.dx - first->dx << " : " << o << "y = "
          << basicName(first->obj) << "y + " << l.dy - first->dy << "\n";
  }
  return out.str();
}

std::string exportBasicAnimation(const Sprite& s, const Animation& an, const Profile& in) {
  Profile k = basicFor(s, in);
  std::ostringstream out;
  std::string a = identifier(s.name) + "_" + identifier(an.name), t = a + "_t", st = a + "_s";
  int n = (int)an.steps.size();
  out << "  rem " << s.name << ", animation " << an.name << ": " << n << " steps" << (an.loop ? ", loops" : "")
      << ". gosub " << a << " once per frame; gosub " << a << "_reset restarts it\n";
  if (n == 0) return out.str() + "  rem (no steps)\n";
  if (!an.basicTimer.empty() && !an.basicStep.empty())
    out << "  dim " << t << " = " << an.basicTimer << "\n  dim " << st << " = " << an.basicStep << "\n";
  else
    out << "  rem dim " << t << " and " << st << " to two free variables (the animation's batari Basic variables)\n";
  out << "  data " << a << "_ticks\n  ";
  for (int i = 0; i < n; i++) out << (i ? ", " : "") << std::clamp(an.steps[i].ticks, 1, 255);
  out << "\nend\n  goto " << a << "_end\n";
  out << a << "_reset\n  " << t << " = 0 : " << st << " = 0\n  return\n";
  out << a << "\n  " << t << " = " << t << " + 1\n  temp1 = " << a << "_ticks[" << st << "]\n";
  out << "  if " << t << " < temp1 then goto " << a << "_show\n  " << t << " = 0\n  " << st << " = " << st << " + 1\n";
  out << "  if " << st << " >= " << n << " then " << st << " = " << (an.loop ? 0 : n - 1) << "\n";
  out << a << "_show\n  on " << st << " goto";
  for (auto& step : an.steps) out << " " << a << "_f" << step.frame;
  out << "\n";
  std::set<int> frames;
  for (auto& step : an.steps) frames.insert(std::clamp(step.frame, 0, s.frames - 1));
  for (int f : frames) out << a << "_f" << f << "\n" << exportBasic(s, k, f) << "  return\n";
  out << a << "_end\n";
  return out.str();
}

std::string exportBasic(const Playfield& p, const Profile& in) {
  Profile k = basicFor(p, in);
  std::ostringstream out;
  int rows = (int)p.rows.size();
  out << "  rem " << p.name << "; " << k.describe() << "\n  playfield:\n";
  int c0 = k.pfFirst(), c1 = c0 + k.pfColumns();
  for (int r = 0; r < rows; r++) {
    out << "  ";
    for (int c = c0; c < c1; c++) out << (p.pixel(r, c) ? 'X' : '.');
    out << "\n";
  }
  out << "end\n";
  auto table = [&](const char* block, auto colour) {
    out << "  " << block << ":\n";
    for (int r = 0; r < rows; r++) out << "  " << hexColour(colour(r)) << "\n";
    out << "end\n";
  };
  bool bkVary = false;
  for (int r = 1; r < rows; r++) bkVary |= p.bkAt(r) != p.bkAt(0);
  if (k.pfRowColours()) table("pfcolors", [&](int r) { return p.pfAt(r); });
  else out << "  COLUPF = " << hexColour(p.pfAt(0)) << "\n";
  if (k.kernel == Kernel::Standard && k.bkRowColours()) table("pfcolors", [&](int r) { return p.bkAt(r); });  // option background: to COLUBK
  else if (k.bkRowColours() && bkVary) table("bkcolors", [&](int r) { return p.bkAt(r); });
  else out << "  COLUBK = " << hexColour(p.bkAt(0)) << "\n";
  if (k.kernel == Kernel::DPCPlus || k.kernel == Kernel::PXE) {
    // the height of the rows: the data fetchers advance FRACINC/256 rows a line, over 176 lines
    int inc = std::clamp((int)std::lround(rows * 256.0 / 176), 1, 255);
    std::string n = std::to_string(inc);
    out << "  rem row height (" << rows << " rows), before every drawscreen:\n  DF0FRACINC = " << n << " : DF1FRACINC = " << n
        << " : DF2FRACINC = " << n << " : DF3FRACINC = " << n << " : DF4FRACINC = " << n << " : DF6FRACINC = " << n << "\n";
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// Regions of a batari Basic program kept up to date

static std::string trimmed(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string lowered(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// "rem vcsgfx art.vcsgfx hero frame 2" -> file, item, frame (-1); "rem vcsgfx end" -> end
struct Marker {
  bool is = false, end = false;
  std::string file, item, animation;
  int frame = -1;
};
static Marker marker(const std::string& line) {
  Marker m;
  std::istringstream in(trimmed(line));
  std::string rem, tag;
  in >> rem >> tag;
  if (lowered(rem) != "rem" || lowered(tag) != "vcsgfx") return m;
  m.is = true;
  std::vector<std::string> w;
  for (std::string t; in >> t;) w.push_back(t);
  if (w.size() == 1 && lowered(w[0]) == "end") return m.end = true, m;
  if (w.empty()) return m;
  m.file = w[0];
  size_t n = w.size();
  if (n >= 3 && lowered(w[n - 2]) == "frame") m.frame = std::atoi(w[n - 1].c_str()) - 1, n -= 2;
  for (size_t i = 2; i < n; i++)
    if (lowered(w[i]) == "animation") {
      for (size_t j = i + 1; j < n; j++) m.animation += (j > i + 1 ? " " : "") + w[j];
      n = i;
      break;
    }
  for (size_t i = 1; i < n; i++) m.item += (i > 1 ? " " : "") + w[i];
  return m;
}

static std::string regionBody(const Asset& a, const std::string& item, int frame, const std::string& animation, const Profile& k,
                              bool* found) {
  *found = true;
  for (auto& s : a.sprites)
    if (s.name == item) {
      if (animation.empty()) return exportBasic(s, k, frame);
      for (auto& an : s.animations)
        if (an.name == animation) return exportBasicAnimation(s, an, k);
      *found = false;
      return "";
    }
  for (auto& p : a.playfields)
    if (p.name == item) return exportBasic(p, k);
  *found = false;
  return "";
}

std::string basicRegion(const std::string& file, const Asset& a, int item, const Profile& k) {
  std::string out;
  bool found;
  if (item < (int)a.sprites.size()) {
    const Sprite& s = a.sprites[item];
    if (!s.animations.empty()) {
      for (size_t i = 0; i < s.animations.size(); i++)
        out += (i ? "\n" : "") + std::string("  rem vcsgfx ") + file + " " + s.name + " animation " + s.animations[i].name + "\n" +
               exportBasicAnimation(s, s.animations[i], k) + "  rem vcsgfx end\n";
      return out;
    }
    for (int f = 0; f < s.frames; f++) {
      if (f) out += "\n";
      std::string frame = s.frames > 1 ? " frame " + std::to_string(f + 1) : "";
      out += "  rem vcsgfx " + file + " " + s.name + frame + "\n" + regionBody(a, s.name, s.frames > 1 ? f : -1, "", k, &found) + "  rem vcsgfx end\n";
    }
  } else if (item - a.sprites.size() < a.playfields.size()) {
    const Playfield& p = a.playfields[item - a.sprites.size()];
    out = "  rem vcsgfx " + file + " " + p.name + "\n" + regionBody(a, p.name, -1, "", k, &found) + "  rem vcsgfx end\n";
  }
  return out;
}

std::string updateBasicRegions(const std::string& source, const std::string& file, const Asset& a, const Profile& k,
                               int* updated, std::vector<std::string>* problems) {
  if (updated) *updated = 0;
  bool crlf = source.find("\r\n") != std::string::npos;
  std::vector<std::string> lines;
  {
    std::string cur;
    for (char c : source) {
      if (c == '\n') lines.push_back(cur), cur.clear();
      else cur += c;
    }
    lines.push_back(cur);  // what follows the last newline ("" if the text ends with one)
  }
  std::vector<std::string> out;
  for (size_t i = 0; i < lines.size(); i++) {
    out.push_back(lines[i]);
    Marker m = marker(lines[i]);
    if (!m.is || m.end || lowered(m.file) != lowered(file)) continue;
    size_t end = i + 1;
    while (end < lines.size() && !marker(lines[end]).end) end++;
    if (end >= lines.size()) {
      if (problems) problems->push_back(F("Line {0}: `rem vcsgfx {1}` has no `rem vcsgfx end`.", {std::to_string(i + 1), m.file + " " + m.item}));
      continue;
    }
    auto sp = std::find_if(a.sprites.begin(), a.sprites.end(), [&](const Sprite& s) { return s.name == m.item; });
    if (sp != a.sprites.end() && m.frame >= sp->frames) {
      if (problems) problems->push_back(F("Line {0}: {1} has {2} frames.", {std::to_string(i + 1), m.item, std::to_string(sp->frames)}));
      continue;
    }
    bool found;
    std::string body = regionBody(a, m.item, m.frame, m.animation, k, &found);
    if (!found) {
      if (problems)
        problems->push_back(m.animation.empty() || sp == a.sprites.end()
                                ? F("Line {0}: {1} has no sprite or playfield called {2}.", {std::to_string(i + 1), file, m.item})
                                : F("Line {0}: {1} has no animation called {2}.", {std::to_string(i + 1), m.item, m.animation}));
      continue;
    }
    std::istringstream b(body);
    for (std::string l; std::getline(b, l);) out.push_back(crlf ? l + "\r" : l);
    i = end - 1;  // the end marker is copied on the next turn
    if (updated) ++*updated;
  }
  std::string text;
  for (size_t i = 0; i < out.size(); i++) text += out[i] + (i + 1 < out.size() ? "\n" : "");
  return text;
}

// ---------------------------------------------------------------------------
// Import

namespace {
struct Block {
  std::string label, kind;     // kind: a batari Basic block ("player0", "pfcolors"...) or "" for assembly
  std::vector<int> values;     // bytes; playfield rows as -1 with the row in text
  std::vector<std::string> rows;
  std::vector<int> comments;   // ";$xx" after a value, -1 if none
};

int parseValue(const std::string& t) {
  std::string v = t;
  while (!v.empty() && (v[0] == '#' || std::isspace((unsigned char)v[0]))) v.erase(0, 1);
  try {
    if (!v.empty() && v[0] == '%') return std::stoi(v.substr(1), nullptr, 2);
    if (!v.empty() && v[0] == '$') return std::stoi(v.substr(1), nullptr, 16);
    if (v.rfind("0x", 0) == 0) return std::stoi(v.substr(2), nullptr, 16);
    if (!v.empty() && std::isdigit((unsigned char)v[0])) return std::stoi(v);
  } catch (...) {}
  return -1;
}
}  // namespace

bool importText(const std::string& text, Asset* out, std::string* what, const Profile& hint) {
  // a whole program says its kernel itself
  Profile kernel = hint;
  if (std::regex_search(text, std::regex(R"(\bset\s+kernel\s)", std::regex::icase))) kernel = profileFromBasic(text);
  static const std::regex bbStart(R"(^\s*(?:if\b.*\bthen\s+)?(player(?:1[0-6]|[0-9])(?:-\d+)?(?:color)?|playfield|pfcolors|bkcolors)\s*:\s*$)", std::regex::icase);
  static const std::regex asmData(R"(^\s+(?:\.byte|dc\.b|\.db|byte|\.BYTE|DC\.B)\s+(.*)$)", std::regex::icase);
  static const std::regex label(R"(^([A-Za-z_.][\w.]*):?\s*(?:;.*)?$)");
  static const std::regex colourComment(R"(;\s*\$([0-9A-Fa-f]{2}))");
  std::vector<Block> blocks;
  bool topFirst = false, sawRowsNote = false;
  std::istringstream in(text);
  std::string line;
  Block* bb = nullptr;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch m;
    if (line.find("Rows: top first") != std::string::npos) topFirst = true, sawRowsNote = true;
    if (line.find("Rows: bottom first") != std::string::npos) topFirst = false, sawRowsNote = true;
    if (bb) {
      std::string t = line;
      t.erase(0, t.find_first_not_of(" \t"));
      if (t.rfind("end", 0) == 0 && (t.size() == 3 || std::isspace((unsigned char)t[3]))) { bb = nullptr; continue; }
      if (t.empty()) continue;
      if (bb->kind == "playfield") bb->rows.push_back(t.substr(0, t.find_first_of(" \t")));
      else if (int v = parseValue(t.substr(0, t.find_first_of(" \t;"))); v >= 0) bb->values.push_back(v), bb->comments.push_back(-1);
      continue;
    }
    if (std::regex_match(line, m, bbStart)) {
      Block b;
      b.kind = m[1].str();
      for (auto& c : b.kind) c = (char)std::tolower((unsigned char)c);
      blocks.push_back(b);
      bb = &blocks.back();
      continue;
    }
    if (std::regex_match(line, m, asmData)) {
      if (blocks.empty() || !blocks.back().kind.empty()) blocks.push_back(Block{});
      Block& b = blocks.back();
      std::string data = m[1].str();
      std::smatch c;
      int comment = std::regex_search(data, c, colourComment) ? std::stoi(c[1].str(), nullptr, 16) : -1;
      data = data.substr(0, data.find(';'));
      std::istringstream vs(data);
      for (std::string v; std::getline(vs, v, ',');)
        if (int x = parseValue(v); x >= 0) b.values.push_back(x), b.comments.push_back(comment);
      continue;
    }
    if (std::regex_match(line, m, label) && line.find('=') == std::string::npos) {
      Block b;
      b.label = m[1].str();
      blocks.push_back(b);
    }
  }
  blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](const Block& b) { return b.values.empty() && b.rows.empty(); }), blocks.end());
  if (blocks.empty()) {
    if (what) what->clear();
    return false;
  }
  auto lower = [](std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
  };
  auto endsWith = [](const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; };
  std::vector<std::string> found;

  // batari Basic playfield
  for (auto& b : blocks)
    if (b.kind == "playfield") {
      Playfield p;
      p.rows.assign(b.rows.size(), PfRow{});
      for (size_t r = 0; r < b.rows.size(); r++) {
        if (b.rows[r].size() <= 16) {  // the multisprite kernel: the left half from column 4, mirrored
          p.rows[r].bits = (rowBits(b.rows[r], false) & 0xFFFF) << 4;
          p.rows[r].mode = PfMode::Mirror;
          continue;
        }
        p.rows[r].asymmetric = true;
        p.rows[r].bits = rowBits(b.rows[r], false) << 4;  // bB columns are the screen's 4-35
      }
      for (auto& c : blocks) {
        if (c.kind != "pfcolors" && c.kind != "bkcolors") continue;
        for (size_t r = 0; r < c.values.size() && r < p.rows.size(); r++) (c.kind == "pfcolors" ? p.rows[r].pf : p.rows[r].bk) = c.values[r];
      }
      // rows whose right half is what a mirror or a repeat would give are symmetric again
      for (int r = 0; r < (int)p.rows.size(); r++)
        for (PfMode mode : {PfMode::Mirror, PfMode::Repeat}) {
          Playfield t = p;
          t.rows[r].asymmetric = false;
          t.rows[r].mode = mode;
          t.rows[r].bits &= 0xFFFFFull;
          bool same = true;
          for (int c = 20; c < 40; c++) same &= t.pixel(r, c) == p.pixel(r, c);
          if (same) { p.rows[r] = t.rows[r]; break; }
        }
      p.name = "playfield";
      out->playfields.push_back(p);
      found.push_back(F("a playfield of {0} rows", {std::to_string(p.rows.size())}));
      break;
    }

  // batari Basic players: each block is a frame, colours pair in order. The standard and
  // multisprite kernels read the rows bottom first, DPC+ and PXE top first.
  {
    Sprite s;
    s.name = "sprite";
    bool top = kernel.topFirst() && kernel.basic();
    for (int pn = 0; pn < 17; pn++) {
      Obj o = playerObj(pn);
      std::string k = "player" + std::to_string(pn);
      std::vector<const Block*> g, c;
      for (auto& b : blocks) {
        std::string kind = b.kind;
        if (size_t d = kind.find('-'); d != std::string::npos) kind.erase(d, kind.find_first_not_of("0123456789", d + 1) - d);  // player1-3: the first
        if (kind == k) g.push_back(&b);
        if (kind == k + "color") c.push_back(&b);
      }
      if (g.empty()) continue;
      Layer l;
      l.obj = o;
      int h = 0;
      for (auto* b : g) h = std::max(h, (int)b->values.size());
      s.frames = std::max(s.frames, (int)g.size());
      l.resize((int)g.size(), h);
      for (size_t f = 0; f < g.size(); f++) {
        int n = (int)g[f]->values.size();
        for (int i = 0; i < n; i++) l.bits[f][top ? i : h - 1 - i] = (uint8_t)g[f]->values[i];
        if (f < c.size())
          for (int i = 0; i < (int)c[f]->values.size() && i < h; i++) l.colours[f][top ? i : h - 1 - i] = c[f]->values[i];
      }
      s.layers.push_back(l);
    }
    if (!s.layers.empty()) {
      s.setFrames(s.frames);
      out->sprites.push_back(s);
      found.push_back(F("{0} player(s), {1} frame(s)", {std::to_string(s.layers.size()), std::to_string(s.frames)}));
    }
  }

  // assembly playfield tables: labels ending in PF0, PF1, PF2 (and PF0R... for the right half)
  {
    std::map<std::string, const Block*> pf;
    for (auto& b : blocks) {
      if (!b.kind.empty()) continue;
      std::string l = lower(b.label);
      for (const char* k : {"pf0r", "pf1r", "pf2r", "pf0", "pf1", "pf2", "colupf_rows", "colubk_rows", "ctrlpf_rows"})
        if (endsWith(l, k) && !pf.count(k)) { pf[k] = &b; break; }
    }
    if (pf.count("pf0") && pf.count("pf1") && pf.count("pf2")) {
      Playfield p;
      int rows = (int)std::min({pf["pf0"]->values.size(), pf["pf1"]->values.size(), pf["pf2"]->values.size()});
      p.rows.assign(rows, PfRow{});
      bool flip = sawRowsNote && !topFirst;
      for (int i = 0; i < rows; i++) {
        int r = flip ? rows - 1 - i : i;
        PfRow& row = p.rows[r];
        if (pf.count("ctrlpf_rows") && i < (int)pf["ctrlpf_rows"]->values.size())
          row.mode = pf["ctrlpf_rows"]->values[i] & 1 ? PfMode::Mirror : PfMode::Repeat;
        int L[3] = {pf["pf0"]->values[i], pf["pf1"]->values[i], pf["pf2"]->values[i]}, reg, bit;
        for (int j = 0; j < 20; j++)
          if (pfBit(j, &reg, &bit), (L[reg] >> bit) & 1) row.bits |= 1ull << j;
        if (pf.count("pf0r") && pf.count("pf1r") && pf.count("pf2r") && i < (int)pf["pf0r"]->values.size()) {
          int R[3] = {pf["pf0r"]->values[i], pf["pf1r"]->values[i], pf["pf2r"]->values[i]};
          if (R[0] != L[0] || R[1] != L[1] || R[2] != L[2]) {
            row.asymmetric = true;
            for (int c = 20; c < 40; c++) {
              int j = row.mode == PfMode::Mirror ? 39 - c : c - 20;
              if (pfBit(j, &reg, &bit), (R[reg] >> bit) & 1) row.bits |= 1ull << c;
            }
          }
        }
        if (pf.count("colupf_rows") && i < (int)pf["colupf_rows"]->values.size()) row.pf = pf["colupf_rows"]->values[i];
        if (pf.count("colubk_rows") && i < (int)pf["colubk_rows"]->values.size()) row.bk = pf["colubk_rows"]->values[i];
      }
      std::string name = pf["pf0"]->label;
      name = name.substr(0, name.size() - 3);
      while (!name.empty() && (name.back() == '_' || name.back() == '.')) name.pop_back();
      p.name = name.empty() ? "playfield" : name;
      out->playfields.push_back(p);
      found.push_back(F("a playfield of {0} rows", {std::to_string(rows)}));
    }
  }

  // assembly sprite tables: every other labelled table of bytes is a frame; tables whose
  // label says colour are the colours of the frames in order. A ";$xx" after a row is its colour.
  {
    // VCSIDE's own labels: <sprite>_<object>[n][_<layer name>]_Frame<f> / _Colour<f>
    std::map<std::string, std::vector<const Block*>> frames, colours;  // by label prefix
    std::map<std::string, const Block*> animFrames, animTicks;          // <sprite>_<animation>
    std::vector<std::string> order;
    static const std::regex ours(R"(^(.*)_(?:Frame|Colour)\d+$)");
    static const std::regex colourLabel("colou?r", std::regex::icase);
    for (auto& b : blocks) {
      if (!b.kind.empty()) continue;
      std::string l = lower(b.label);
      bool isPf = false;
      for (const char* k : {"pf0r", "pf1r", "pf2r", "pf0", "pf1", "pf2", "colupf_rows", "colubk_rows", "ctrlpf_rows"}) isPf |= endsWith(l, k);
      if (isPf) continue;
      // frame pointers are not frames; animation tables are read below
      static const std::regex pointers(R"(_(?:Frame|Colour)Ptr(?:Lo|Hi)$)");
      static const std::regex animTable(R"(^(.*)_(FRAME|TICKS)$)");
      std::smatch am;
      if (std::regex_search(b.label, pointers)) continue;
      if (std::regex_match(b.label, am, animTable)) {
        (am[2] == "FRAME" ? animFrames : animTicks)[am[1].str()] = &b;
        continue;
      }
      std::smatch m;
      std::string key = std::regex_match(b.label, m, ours) && m[1].str().find('_') != std::string::npos ? m[1].str() : "";
      bool colour = std::regex_search(b.label, colourLabel);
      if (std::find(order.begin(), order.end(), key) == order.end()) order.push_back(key);
      (colour ? colours : frames)[key].push_back(&b);
    }
    Sprite s;
    s.name.clear();
    for (auto& key : order) {
      auto& g = frames[key];
      if (g.empty()) continue;
      Layer l;
      std::smatch m;
      static const std::regex obj(R"(^(.*?)_(P1[0-6]|P[0-9]|M0|M1|BL)[b-z]?(?:_(\w+))?$)");
      if (std::regex_match(key, m, obj)) {
        for (int k = 0; k < kObjects; k++)
          if (m[2].str() == objName((Obj)k)) l.obj = (Obj)k;
        l.name = m[3].str();
        if (s.name.empty()) s.name = m[1].str();
      }
      int h = 0;
      for (auto* b : g) h = std::max(h, (int)b->values.size());
      l.resize((int)g.size(), h);
      bool flip = !sawRowsNote || !topFirst;  // assembly tables are bottom first unless they say otherwise
      for (size_t f = 0; f < g.size(); f++) {
        int n = (int)g[f]->values.size();
        for (int i = 0; i < n; i++) {
          int r = flip ? h - 1 - i : i;
          int v = g[f]->values[i];
          l.bits[f][r] = (uint8_t)(isPlayer(l.obj) ? v : (v & 2) != 0);
          if (g[f]->comments[i] >= 0) l.colours[f][r] = g[f]->comments[i];
        }
        auto& c = colours[key];
        if (f < c.size())
          for (int i = 0; i < (int)c[f]->values.size() && i < h; i++) l.colours[f][flip ? h - 1 - i : i] = c[f]->values[i];
      }
      // the equates of an exported sprite give the position, size and phase back
      if (!key.empty()) {
        std::istringstream again(text);
        for (std::string t; std::getline(again, t);) {
          if (t.rfind(key + "_", 0) != 0) continue;
          std::string rest = t.substr(key.size() + 1);
          std::smatch e;
          static const std::regex eq(R"(^(DX|DY|PHASE)\s*=\s*(-?\d+))");
          if (std::regex_search(rest, e, eq)) {
            int v = std::stoi(e[2]);
            (e[1] == "DX" ? l.dx : e[1] == "DY" ? l.dy : l.phase) = e[1] == "PHASE" ? std::clamp(v, 0, 3) : v;
          }
          if (rest.rfind("NUSIZ", 0) == 0) l.size = rest.find("$07") != std::string::npos ? 4 : rest.find("$05") != std::string::npos ? 2 : 1;
          if (rest.rfind("REFP", 0) == 0) l.reflect = true;
          if (rest.rfind("SIZE", 0) == 0) l.size = rest.find("$30") != std::string::npos ? 8 : rest.find("$20") != std::string::npos ? 4 : rest.find("$10") != std::string::npos ? 2 : 1;
        }
      }
      s.frames = std::max(s.frames, (int)g.size());
      s.layers.push_back(l);
    }
    if (!s.layers.empty()) {
      if (s.name.empty()) s.name = "sprite";
      s.setFrames(s.frames);
      for (auto& [prefix, fb] : animFrames) {
        if (prefix.rfind(s.name + "_", 0) != 0) continue;
        Animation an;
        an.name = prefix.substr(s.name.size() + 1);
        const Block* tb = animTicks.count(prefix) ? animTicks[prefix] : nullptr;
        for (size_t i = 0; i < fb->values.size(); i++)
          an.steps.push_back({std::clamp(fb->values[i], 0, s.frames - 1), tb && i < tb->values.size() ? std::clamp(tb->values[i], 1, 255) : 8});
        an.loop = text.find(prefix + "_LOOP = 0") == std::string::npos;
        s.animations.push_back(an);
      }
      out->sprites.push_back(s);
      found.push_back(F("{0} layer(s), {1} frame(s)", {std::to_string(s.layers.size()), std::to_string(s.frames)}));
    }
  }
  if (what) {
    what->clear();
    for (auto& f : found) *what += (what->empty() ? "" : "; ") + f;
  }
  return !found.empty();
}

}  // namespace vcs::gfx
