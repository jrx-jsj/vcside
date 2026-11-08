// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "space.h"

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>

#include "i18n.h"

namespace vcs {

std::optional<Range> parseRange(const std::string& s) {
  static const std::regex re(R"(^\s*\$([0-9A-Fa-f]+)\s*(?:-\s*\$([0-9A-Fa-f]+))?\s*$)");
  std::smatch m;
  if (!std::regex_match(s, m, re)) return std::nullopt;
  long lo = std::stol(m[1], nullptr, 16);
  long hi = m[2].matched ? std::stol(m[2], nullptr, 16) : lo;
  if (hi < lo) return std::nullopt;
  return Range{lo, hi + 1};
}

std::vector<Range> parseDasmRas(const std::string& d) {
  std::vector<Range> out;
  size_t i = 0;
  while (i + 4 <= d.size()) {
    long org = (unsigned char)d[i] | ((unsigned char)d[i + 1] << 8);
    long len = (unsigned char)d[i + 2] | ((unsigned char)d[i + 3] << 8);
    i += 4;
    if (i + len > d.size()) break;  // truncated file
    if (len > 0) out.push_back({org, org + len});
    i += len;
  }
  return out;
}

namespace {
// "     12  f000\t\t       a9 01\t   B0 ..." / "      4 U0080\t\t       00 00 00    Var1 ..."
const std::regex kLine(R"(^\s*(\d+)\s([U ])([0-9a-fA-F]{4,8})(?: \?\?\?\?)?\t\t {7}([0-9a-f]{2}(?: [0-9a-f]{2})*)(\*)?)");
const std::regex kFile(R"(^------- FILE (\S+))");
const std::regex kFill(R"(^[^;]*\s(\.?ds(\.[bwl])?|align)\s)", std::regex::icase);

struct ListingVisitor {
  std::string file;
  template <class F>
  void run(const std::string& listing, F&& onLine) {
    std::istringstream in(listing);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::smatch m;
      if (std::regex_search(line, m, kFile)) {
        file = m[1];
        continue;
      }
      if (std::regex_search(line, m, kLine)) onLine(m);
    }
  }
};

long countBytes(const std::string& hex) { return (long)(hex.size() + 1) / 3; }
}  // namespace

std::vector<RamDecl> parseDasmListingRam(const std::string& listing,
                                         const std::function<std::vector<std::string>(const std::string&)>& readSource,
                                         const SymbolLookup& lookup) {
  std::vector<RamDecl> out;
  std::map<std::string, std::vector<std::string>> cache;
  ListingVisitor v;
  v.run(listing, [&](const std::smatch& m) {
    if (m[2] != "U") return;
    RamDecl d;
    d.file = v.file;
    d.line = std::stoi(m[1]);
    d.addr = std::stol(m[3], nullptr, 16);
    long shown = countBytes(m[4]);
    d.size = shown;
    if (readSource) {
      auto it = cache.find(d.file);
      if (it == cache.end()) it = cache.emplace(d.file, readSource(d.file)).first;
      if (d.line >= 1 && d.line <= (int)it->second.size()) {
        AsmLine a = splitAsmLine(it->second[d.line - 1]);
        d.label = a.label;
        std::string mn = upper(a.mnemonic);
        if (mn == "DS" || mn == "DS.B" || mn == ".DS" || mn == "DS.W") {
          std::string count = a.operand.substr(0, a.operand.find(','));
          if (auto n = evalExpr(count, lookup)) d.size = *n * (mn == "DS.W" ? 2 : 1);
        }  // otherwise (.byte lists in SEG.U...): the bytes shown, at least

      }
    }
    if (d.size > 0) out.push_back(d);
  });
  return out;
}

std::vector<CodeLine> parseDasmListingCode(const std::string& listing) {
  std::vector<CodeLine> out;
  ListingVisitor v;
  v.run(listing, [&](const std::smatch& m) {
    if (m[2] == "U") return;
    std::string rest = m.suffix().str() + " ";
    out.push_back({v.file, std::stoi(m[1]), std::stol(m[3], nullptr, 16), std::regex_search(rest, kFill)});
  });
  std::stable_sort(out.begin(), out.end(), [](const CodeLine& a, const CodeLine& b) { return a.addr < b.addr; });
  return out;
}

static long overlap(const Range& a, const Range& b) { return std::max(0L, std::min(a.hi, b.hi) - std::max(a.lo, b.lo)); }

static std::string hex(long v, int digits = 4) {
  char b[16];
  snprintf(b, sizeof b, "$%0*lX", digits, v);
  return b;
}

static int percent(long used, long size) { return size > 0 ? (int)((used * 100 + size - 1) / size) : 0; }

std::string SpaceReport::summary() const {
  std::string s;
  if (!banks.empty()) {
    long used = 0, size = 0;
    int worst = 0;
    for (auto& b : banks) used += b.used, size += b.size, worst = std::max(worst, percent(b.used, b.size));
    s = F("ROM {0}/{1}", {std::to_string(used), std::to_string(size)});
    if (banks.size() > 1) s += " " + F("(fullest bank {0} %)", {std::to_string(worst)});
  }
  if (ram.measured) s += (s.empty() ? "" : "  ·  ") + F("RAM {0}/{1}", {std::to_string(ram.used), std::to_string(ram.size)});
  if (cartRam && cartRam->measured)
    s += (s.empty() ? "" : "  ·  ") + F("cart RAM {0}/{1}", {std::to_string(cartRam->used), std::to_string(cartRam->size)});
  return s;
}

SpaceReport analyzeSpace(const std::vector<Range>& rom, long romFileSize, const std::vector<RamDecl>& ram,
                         const std::vector<CodeLine>& code, const FormatSpace& fmt, const SpaceOptions& opt) {
  SpaceReport r;
  r.romFileSize = romFileSize;
  auto lineAt = [&](long addr) -> const CodeLine* {
    // last listing line at or before addr
    auto it = std::upper_bound(code.begin(), code.end(), addr, [](long a, const CodeLine& c) { return a < c.addr; });
    return it == code.begin() ? nullptr : &*std::prev(it);
  };
  auto at = [&](long addr) -> std::pair<std::string, int> {
    const CodeLine* c = lineAt(addr);
    return c ? std::make_pair(c->file, c->line) : std::make_pair(std::string(), 0);
  };
  auto diag = [&](Diagnostic::Severity s, const std::string& msg, const std::string& file = "", int line = 0) {
    r.diagnostics.push_back({s, file, line, msg});
  };

  // ---- ROM ----
  long bank = fmt.bankSize > 0 ? fmt.bankSize : 4096;
  if (!rom.empty()) {
    long base = rom.front().lo;
    for (auto& x : rom) base = std::min(base, x.lo);
    base -= base % bank;  // images start on a bank boundary
    long end = 0;
    for (auto& x : rom) end = std::max(end, x.hi - base);
    long total = std::max(romFileSize, end);
    long nBanks = std::max(1L, (total + bank - 1) / bank);
    for (long i = 0; i < nBanks; i++) {
      SpaceReport::Area a;
      a.name = nBanks > 1 ? F("Bank {0}", {std::to_string(i)}) : T("ROM");
      a.size = bank;
      Range b{base + i * bank, base + (i + 1) * bank};
      for (auto& x : rom) a.used += overlap(x, b);
      r.banks.push_back(a);
      int pct = percent(a.used, a.size);
      if (a.used >= a.size) diag(Diagnostic::Warning, F("{0} is full ({1} bytes).", {a.name, std::to_string(a.size)}));
      else if (pct >= opt.warnPercent)
        diag(Diagnostic::Warning, F("{0} is {1} % full: {2} bytes free.", {a.name, std::to_string(pct), std::to_string(a.size - a.used)}));
    }
    if (!fmt.romSizes.empty() && romFileSize > 0 &&
        std::find(fmt.romSizes.begin(), fmt.romSizes.end(), romFileSize) == fmt.romSizes.end()) {
      long maxSize = *std::max_element(fmt.romSizes.begin(), fmt.romSizes.end());
      std::string sizes;
      for (long s : fmt.romSizes) sizes += (sizes.empty() ? "" : ", ") + std::to_string(s);
      if (romFileSize > maxSize) {
        auto [f, l] = at(base + maxSize);
        diag(Diagnostic::Error, F("The ROM is {0} bytes, more than the format allows ({1}): the code overflows.",
                                  {std::to_string(romFileSize), sizes}), f, l);
      } else {
        diag(Diagnostic::Warning, F("The ROM is {0} bytes; this format expects {1}. Emulators may not detect it.",
                                    {std::to_string(romFileSize), sizes}));
      }
    }
    // Hotspots and RAM ports, inside each bank's window.
    if (!fmt.reserved.empty() && (bank == 4096 || bank == 2048)) {
      long windowBase = 0x2000 - bank;
      std::set<std::string> reported;
      for (auto& x : rom) {
        for (long i = (x.lo - base) / bank; i * bank < x.hi - base; i++) {
          Range piece{std::max(x.lo - base, i * bank), std::min(x.hi - base, (i + 1) * bank)};
          if (piece.size() <= 0) continue;
          Range w{windowBase + piece.lo % bank, windowBase + piece.lo % bank + piece.size()};
          for (auto& res : fmt.reserved) {
            long o = overlap(w, res);
            if (!o) continue;
            // first address of the overlap that is not ds/align padding
            long first = -1;
            const CodeLine* cl = nullptr;
            for (long a = std::max(w.lo, res.lo); a < std::min(w.hi, res.hi); a++) {
              cl = lineAt(base + i * bank + (a - windowBase));
              if (!cl || !cl->fill) {
                first = a;
                break;
              }
            }
            if (first < 0) continue;
            std::string key = std::to_string(i) + hex(res.lo);
            if (!reported.insert(key).second) continue;
            std::string f = cl ? cl->file : "";
            int l = cl ? cl->line : 0;
            diag(Diagnostic::Warning,
                 F("Bank {0}: code or data at {1}, a reserved address of the format (bank-switch hotspot or RAM port).",
                   {std::to_string(i), hex(first)}),
                 f, l);
          }
        }
      }
    }
  }

  // ---- 2600 RAM ----
  r.ram.name = T("2600 RAM");
  r.ram.size = 128;
  std::set<long> used;
  const RamDecl* highest = nullptr;
  for (auto& d : ram) {
    if (d.addr + d.size <= 0x80 || d.addr >= 0x100) continue;
    for (long a = std::max(0x80L, d.addr); a < d.addr + d.size && a < 0x100; a++) used.insert(a);
    if (!highest || d.addr + d.size > highest->addr + highest->size) highest = &d;
    if (d.addr + d.size > 0x100)
      diag(Diagnostic::Error, F("{0} goes past the end of RAM ($FF).", {d.label.empty() ? hex(d.addr) : d.label}), d.file, d.line);
  }
  if (!highest) {
    r.ram.measured = false;
    r.ram.note = T("Not measured: declare the variables with SEG.U and ds so VCSIDE can count them.");
  } else {
    r.ram.used = (long)used.size();
    long top = highest->addr + highest->size;
    r.stackRoom = std::max(0L, 0x100 - top);
    if (top <= 0x100 && r.stackRoom < opt.stackReserve)
      diag(Diagnostic::Warning,
           F("Only {0} bytes left for the stack above {1} (reserve: {2}).",
             {std::to_string(r.stackRoom), highest->label.empty() ? hex(highest->addr) : highest->label, std::to_string(opt.stackReserve)}),
           highest->file, highest->line);
    else if (percent(r.ram.used, 128) >= opt.warnPercent)
      diag(Diagnostic::Warning, F("{0} is {1} % full: {2} bytes free.", {r.ram.name, std::to_string(percent(r.ram.used, 128)), std::to_string(128 - r.ram.used)}));
  }

  // ---- cartridge RAM ----
  if (fmt.ramBytes > 0) {
    SpaceReport::Area a;
    a.name = T("Cartridge RAM");
    a.size = fmt.ramBytes;
    if (fmt.ramWindows.empty()) {
      a.measured = false;
      a.note = T("This format's RAM cannot be measured from the DASM output.");
    } else {
      std::set<long> offs;
      for (auto& d : ram) {
        if (d.addr < 0x1000) continue;  // TIA, 2600 RAM, RIOT
        long w = 0x1000 | (d.addr & 0x0FFF);
        for (auto& win : fmt.ramWindows) {
          if (w < win.lo || w >= win.hi) continue;
          long off = w - win.lo;
          for (long k = 0; k < d.size; k++) offs.insert(off + k);
          if (off + d.size > fmt.ramBytes)
            diag(Diagnostic::Error, F("{0} goes past the end of the cartridge RAM ({1} bytes).",
                                      {d.label.empty() ? hex(d.addr) : d.label, std::to_string(fmt.ramBytes)}),
                 d.file, d.line);
          break;
        }
      }
      a.used = (long)offs.size();
      int pct = percent(a.used, a.size);
      if (a.used > 0 && pct >= opt.warnPercent && a.used <= a.size)
        diag(Diagnostic::Warning, F("{0} is {1} % full: {2} bytes free.", {a.name, std::to_string(pct), std::to_string(a.size - a.used)}));
    }
    r.cartRam = a;
  }
  return r;
}

}  // namespace vcs
