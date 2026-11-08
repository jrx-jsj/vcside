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
  static const char* n[] = {"P0", "P1", "M0", "M1", "BL"};
  return n[(int)o];
}
bool isPlayer(Obj o) { return o == Obj::P0 || o == Obj::P1; }
const char* colourRegister(Obj o) {
  return o == Obj::P0 || o == Obj::M0 ? "COLUP0" : o == Obj::P1 || o == Obj::M1 ? "COLUP1" : "COLUPF";
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
  frames++;
}

void Sprite::removeFrame(int at) {
  if (frames <= 1 || at < 0 || at >= frames) return;
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
    for (auto& js : j.value("sprites", json::array())) {
      Sprite s;
      s.name = js.value("name", s.name);
      s.frames = std::max(1, js.value("frames", 1));
      s.lineHeight = std::clamp(js.value("line_height", 2), 1, 8);
      s.frameTicks = std::clamp(js.value("frame_ticks", 8), 1, 60);
      for (auto& jl : js.value("layers", json::array())) {
        Layer l;
        std::string o = jl.value("object", "P0");
        for (int k = 0; k < 5; k++)
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

std::vector<Issue> check(const Sprite& s, Target t) {
  std::vector<Issue> out;
  int phases = s.phases();
  // one object per phase
  for (int p = 0; p < phases; p++) {
    int used[5] = {0, 0, 0, 0, 0};
    for (auto& l : s.layers)
      if (l.phase == p) used[objIndex(l.obj)]++;
    for (int k = 0; k < 5; k++)
      if (used[k] > 1)
        out.push_back({Issue::Error, F("Phase {0} uses {1} in {2} layers: the TIA has only one {1}. Move one of them to another phase.",
                                       {std::to_string(p + 1), objName((Obj)k), std::to_string(used[k])})});
  }
  if (phases > 1) {
    std::vector<int> empty;
    for (int p = 0; p < phases; p++)
      if (std::none_of(s.layers.begin(), s.layers.end(), [&](const Layer& l) { return l.phase == p; })) empty.push_back(p + 1);
    out.push_back({Issue::Warning, F("{0} flicker phases: each layer shows one frame in {0} ({1} Hz on NTSC, {2} Hz on PAL) and flickers.",
                                     {std::to_string(phases), std::to_string(60 / phases), std::to_string(50 / phases)})});
    if (!empty.empty()) out.push_back({Issue::Warning, F("Phases without layers: {0}. The other layers flicker more than they need to.", {rowList(empty)})});
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
  if (t == Target::Dasm) {
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
  } else {
    for (auto& l : s.layers) {
      if (isPlayer(l.obj)) continue;
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
    if (phases > 1)
      out.push_back({Issue::Warning, T("batari Basic does not flicker by itself: each phase is exported on its own, and the program has to alternate them every frame.")});
    for (auto& l : s.layers)
      if (isPlayer(l.obj) && coloursVary(l))
        out.push_back({Issue::Info, F("{0} changes colour per row: it needs `set kernel_options {1}`.",
                                      {objName(l.obj), l.obj == Obj::P0 ? "playercolors" : "player1colors"})});
    if (s.layers.size() > 1) out.push_back({Issue::Info, T("The layers' positions are exported as comments: the program places each object relative to the first one.")});
  }
  return out;
}

std::vector<Issue> check(const Playfield& p, Target t) {
  std::vector<Issue> out;
  std::vector<int> asym, modeChange, pfChange, bkChange;
  for (int r = 0; r < (int)p.rows.size(); r++) {
    if (p.rows[r].asymmetric) asym.push_back(r + 1);
    if (r > 0 && p.rows[r].mode != p.rows[r - 1].mode) modeChange.push_back(r + 1);
    if (r > 0 && p.pfAt(r) != p.pfAt(r - 1)) pfChange.push_back(r + 1);
    if (r > 0 && p.bkAt(r) != p.bkAt(r - 1)) bkChange.push_back(r + 1);
  }
  if (t == Target::Dasm) {
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
  } else {
    bool outer = false;
    for (int r = 0; r < (int)p.rows.size(); r++)
      for (int c : {0, 1, 2, 3, 36, 37, 38, 39}) outer |= p.pixel(r, c);
    if (outer)
      out.push_back({Issue::Warning, T("batari Basic's standard kernel shows columns 5 to 36 only (32 wide; PF0 is not used): the four outer columns of each side are lost.")});
    if (p.rows.size() != 11)
      out.push_back({Issue::Info, F("{0} rows: batari Basic shows 11 unless the program sets `const pfres = {0}` (only some values are allowed, and more rows take more RAM).", {std::to_string(p.rows.size())})});
    out.push_back({Issue::Info, T("batari Basic sets the height of the rows itself; the line height here is only for the preview.")});
    if (!pfChange.empty() || p.rows[0].pf >= 0) out.push_back({Issue::Info, T("The rows have their own colours: pfcolors: needs `set kernel_options pfcolors`.")});
    if (!bkChange.empty()) out.push_back({Issue::Info, T("The background changes per row: bkcolors: needs the kernel options pfcolors and backgroundchange.")});
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

// "P0", "P0_helmet", "P02" (a second P0 layer, in another phase)
static std::string layerId(const Layer& l, const std::vector<Layer>& all) {
  int same = 0, mine = 0;
  for (auto& o : all)
    if (o.obj == l.obj) {
      if (&o == &l) mine = same;
      same++;
    }
  std::string id = objName(l.obj);
  if (same > 1) id += std::to_string(mine + 1);
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

std::string exportBasic(const Sprite& s) {
  std::ostringstream out;
  out << "  rem " << s.name << ": " << s.frames << " frame(s), " << s.phases() << " phase(s)\n";
  const Layer* first = nullptr;
  for (auto& l : s.layers)
    if (isPlayer(l.obj) && !first) first = &l;
  for (int ph = 0; ph < s.phases(); ph++)
    for (int f = 0; f < s.frames; f++) {
      out << "  rem frame " << f + 1 << (s.phases() > 1 ? ", phase " + std::to_string(ph + 1) : "") << "\n";
      for (auto& l : s.layers) {
        if (l.phase != ph) continue;
        std::string pn = l.obj == Obj::P0 ? "player0" : l.obj == Obj::P1 ? "player1" : "";
        if (pn.empty()) continue;
        out << "  " << pn << ":\n";
        for (int r = l.height - 1; r >= 0; r--) out << "  " << bin8(l.bits[f][r]) << "\n";  // bottom row first
        out << "end\n";
        if (coloursVary(l)) {
          out << "  " << pn << "color:\n";
          for (int r = l.height - 1; r >= 0; r--) out << "  " << hexColour(l.colour(f, r)) << "\n";
          out << "end\n";
        } else {
          out << "  COLUP" << (l.obj == Obj::P0 ? "0" : "1") << " = " << hexColour(l.colour(f, 0)) << "\n";
        }
      }
    }
  for (auto& l : s.layers) {
    std::string o = l.obj == Obj::P0 ? "player0" : l.obj == Obj::P1 ? "player1" : l.obj == Obj::M0 ? "missile0" : l.obj == Obj::M1 ? "missile1" : "ball";
    if (isPlayer(l.obj)) {
      if (l.size > 1) out << "  NUSIZ" << (l.obj == Obj::P0 ? "0" : "1") << " = " << (l.size == 4 ? "$07" : "$05") << "\n";
      if (l.reflect) out << "  REFP" << (l.obj == Obj::P0 ? "0" : "1") << " = 8\n";
    } else {
      int h = 0;
      for (int r = 0; r < l.height; r++) h += rowHasPixels(l, 0, r);
      out << "  " << o << "height = " << h << "\n";
    }
    if (first && &l != first)
      out << "  rem " << o << "x = " << (first->obj == Obj::P0 ? "player0x" : "player1x") << " + " << l.dx - first->dx << " : "
          << o << "y = " << (first->obj == Obj::P0 ? "player0y" : "player1y") << " + " << l.dy - first->dy << "\n";
  }
  return out.str();
}

std::string exportBasic(const Playfield& p) {
  std::ostringstream out;
  out << "  rem " << p.name << "\n  playfield:\n";
  for (int r = 0; r < (int)p.rows.size(); r++) {
    out << "  ";
    for (int c = 4; c < 36; c++) out << (p.pixel(r, c) ? 'X' : '.');
    out << "\n";
  }
  out << "end\n";
  bool pfVary = p.rows[0].pf >= 0, bkVary = false;
  for (int r = 1; r < (int)p.rows.size(); r++) pfVary |= p.pfAt(r) != p.pfAt(0), bkVary |= p.bkAt(r) != p.bkAt(0);
  if (pfVary) {
    out << "  pfcolors:\n";
    for (int r = 0; r < (int)p.rows.size(); r++) out << "  " << hexColour(p.pfAt(r)) << "\n";
    out << "end\n";
  } else {
    out << "  COLUPF = " << hexColour(p.pfColour) << "\n";
  }
  if (bkVary) {
    out << "  bkcolors:\n";
    for (int r = 0; r < (int)p.rows.size(); r++) out << "  " << hexColour(p.bkAt(r)) << "\n";
    out << "end\n";
  } else {
    out << "  COLUBK = " << hexColour(p.bkColour) << "\n";
  }
  return out.str();
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

bool importText(const std::string& text, Asset* out, std::string* what) {
  static const std::regex bbStart(R"(^\s*(?:if\b.*\bthen\s+)?(player[01](?:color)?|playfield|pfcolors|bkcolors)\s*:\s*$)", std::regex::icase);
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

  // batari Basic players: each block is a frame (bottom row first), colours pair in order
  {
    Sprite s;
    s.name = "sprite";
    for (Obj o : {Obj::P0, Obj::P1}) {
      std::string k = o == Obj::P0 ? "player0" : "player1";
      std::vector<const Block*> g, c;
      for (auto& b : blocks) {
        if (b.kind == k) g.push_back(&b);
        if (b.kind == k + "color") c.push_back(&b);
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
        for (int i = 0; i < n; i++) l.bits[f][h - 1 - i] = (uint8_t)g[f]->values[i];
        if (f < c.size())
          for (int i = 0; i < (int)c[f]->values.size() && i < h; i++) l.colours[f][h - 1 - i] = c[f]->values[i];
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
    std::vector<std::string> order;
    static const std::regex ours(R"(^(.*)_(?:Frame|Colour)\d+$)");
    static const std::regex colourLabel("colou?r", std::regex::icase);
    for (auto& b : blocks) {
      if (!b.kind.empty()) continue;
      std::string l = lower(b.label);
      bool isPf = false;
      for (const char* k : {"pf0r", "pf1r", "pf2r", "pf0", "pf1", "pf2", "colupf_rows", "colubk_rows", "ctrlpf_rows"}) isPf |= endsWith(l, k);
      if (isPf) continue;
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
      static const std::regex obj(R"(^(.*?)_(P0|P1|M0|M1|BL)\d*(?:_(\w+))?$)");
      if (std::regex_match(key, m, obj)) {
        for (int k = 0; k < 5; k++)
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
