// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "space.h"

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>

#include "i18n.h"

namespace vcs {

std::optional<Range> parseRange(const std::string& text) {
  static const std::regex rangePattern(R"(^\s*\$([0-9A-Fa-f]+)\s*(?:-\s*\$([0-9A-Fa-f]+))?\s*$)");
  std::smatch match;
  if (!std::regex_match(text, match, rangePattern)) return std::nullopt;
  long lo = std::stol(match[1], nullptr, 16);
  long hi = match[2].matched ? std::stol(match[2], nullptr, 16) : lo;
  if (hi < lo) return std::nullopt;
  return Range{lo, hi + 1};
}

std::vector<Range> parseDasmRas(const std::string& bytes) {
  std::vector<Range> ranges;
  size_t i = 0;
  while (i + 4 <= bytes.size()) {
    long org = (unsigned char)bytes[i] | ((unsigned char)bytes[i + 1] << 8);
    long len = (unsigned char)bytes[i + 2] | ((unsigned char)bytes[i + 3] << 8);
    i += 4;
    if (i + len > bytes.size()) break;  // truncated file
    if (len > 0) ranges.push_back({org, org + len});
    i += len;
  }
  return ranges;
}

namespace {
// "     12  f000\t\t       a9 01\t   B0 ..." / "      4 U0080\t\t       00 00 00    Var1 ..."
const std::regex kLine(R"(^\s*(\d+)\s([U ])([0-9a-fA-F]{4,8})(?: \?\?\?\?)?\t\t {7}([0-9a-f]{2}(?: [0-9a-f]{2})*)(\*)?)");
const std::regex kFile(R"(^------- FILE (\S+))");
const std::regex kFill(R"(^[^;]*\s(\.?ds(\.[bwl])?|align)\s)", std::regex::icase);

struct ListingVisitor {
  std::string file;
  template <class Callback>
  void run(const std::string& listing, Callback&& onLine) {
    std::istringstream stream(listing);
    std::string line;
    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::smatch match;
      if (std::regex_search(line, match, kFile)) {
        file = match[1];
        continue;
      }
      if (std::regex_search(line, match, kLine)) onLine(match);
    }
  }
};

long countBytes(const std::string& hex) { return (long)(hex.size() + 1) / 3; }
}  // namespace

std::vector<RamDecl> parseDasmListingRam(const std::string& listing,
                                         const std::function<std::vector<std::string>(const std::string&)>& readSource,
                                         const SymbolLookup& lookup) {
  std::vector<RamDecl> declarations;
  std::map<std::string, std::vector<std::string>> cache;
  ListingVisitor visitor;
  visitor.run(listing, [&](const std::smatch& match) {
    if (match[2] != "U") return;
    RamDecl decl;
    decl.file = visitor.file;
    decl.line = std::stoi(match[1]);
    decl.addr = std::stol(match[3], nullptr, 16);
    long shown = countBytes(match[4]);
    decl.size = shown;
    if (readSource) {
      auto found = cache.find(decl.file);
      if (found == cache.end()) found = cache.emplace(decl.file, readSource(decl.file)).first;
      if (decl.line >= 1 && decl.line <= (int)found->second.size()) {
        AsmLine asmLine = splitAsmLine(found->second[decl.line - 1]);
        decl.label = asmLine.label;
        std::string mnemonic = upper(asmLine.mnemonic);
        if (mnemonic == "DS" || mnemonic == "DS.B" || mnemonic == ".DS" || mnemonic == "DS.W") {
          std::string count = asmLine.operand.substr(0, asmLine.operand.find(','));
          if (auto value = evalExpr(count, lookup)) decl.size = *value * (mnemonic == "DS.W" ? 2 : 1);
        }  // otherwise (.byte lists in SEG.U...): the bytes shown, at least

      }
    }
    if (decl.size > 0) declarations.push_back(decl);
  });
  return declarations;
}

std::vector<CodeLine> parseDasmListingCode(const std::string& listing) {
  std::vector<CodeLine> lines;
  ListingVisitor visitor;
  visitor.run(listing, [&](const std::smatch& match) {
    if (match[2] == "U") return;
    std::string rest = match.suffix().str() + " ";
    lines.push_back({visitor.file, std::stoi(match[1]), std::stol(match[3], nullptr, 16), std::regex_search(rest, kFill)});
  });
  std::stable_sort(lines.begin(), lines.end(), [](const CodeLine& left, const CodeLine& right) { return left.addr < right.addr; });
  return lines;
}

static long overlap(const Range& left, const Range& right) { return std::max(0L, std::min(left.hi, right.hi) - std::max(left.lo, right.lo)); }

static std::string hex(long value, int digits = 4) {
  char buffer[24];
  snprintf(buffer, sizeof buffer, "$%0*lX", digits, value);
  return buffer;
}

static int percent(long used, long size) { return size > 0 ? (int)((used * 100 + size - 1) / size) : 0; }

std::string SpaceReport::summary() const {
  std::string text;
  if (!banks.empty()) {
    long used = 0, size = 0;
    int worst = 0;
    for (auto& bank : banks) used += bank.used, size += bank.size, worst = std::max(worst, percent(bank.used, bank.size));
    text = F("ROM {0}/{1}", {std::to_string(used), std::to_string(size)});
    if (banks.size() > 1) text += " " + F("(fullest bank {0} %)", {std::to_string(worst)});
  }
  if (ram.measured) text += (text.empty() ? "" : "  ·  ") + F("RAM {0}/{1}", {std::to_string(ram.used), std::to_string(ram.size)});
  if (cartRam && cartRam->measured)
    text += (text.empty() ? "" : "  ·  ") + F("cart RAM {0}/{1}", {std::to_string(cartRam->used), std::to_string(cartRam->size)});
  return text;
}

SpaceReport analyzeSpace(const std::vector<Range>& rom, long romFileSize, const std::vector<RamDecl>& ram,
                         const std::vector<CodeLine>& code, const FormatSpace& formatSpace, const SpaceOptions& options) {
  SpaceReport report;
  report.romFileSize = romFileSize;
  auto lineAt = [&](long addr) -> const CodeLine* {
    // last listing line at or before addr
    auto found = std::upper_bound(code.begin(), code.end(), addr, [](long address, const CodeLine& codeLine) { return address < codeLine.addr; });
    return found == code.begin() ? nullptr : &*std::prev(found);
  };
  auto fileAndLineAt = [&](long addr) -> std::pair<std::string, int> {
    const CodeLine* codeLine = lineAt(addr);
    return codeLine ? std::make_pair(codeLine->file, codeLine->line) : std::make_pair(std::string(), 0);
  };
  auto addDiagnostic = [&](Diagnostic::Severity severity, const std::string& message, const std::string& file = "", int line = 0) {
    report.diagnostics.push_back({severity, file, line, message});
  };

  // ---- ROM ----
  long bank = formatSpace.bankSize > 0 ? formatSpace.bankSize : 4096;
  if (!rom.empty()) {
    long base = rom.front().lo;
    for (auto& romRange : rom) base = std::min(base, romRange.lo);
    base -= base % bank;  // images start on a bank boundary
    long end = 0;
    for (auto& romRange : rom) end = std::max(end, romRange.hi - base);
    long total = std::max(romFileSize, end);
    long nBanks = std::max(1L, (total + bank - 1) / bank);
    for (long i = 0; i < nBanks; i++) {
      SpaceReport::Area bankArea;
      bankArea.name = nBanks > 1 ? F("Bank {0}", {std::to_string(i)}) : T("ROM");
      bankArea.size = bank;
      Range bankRange{base + i * bank, base + (i + 1) * bank};
      for (auto& romRange : rom) bankArea.used += overlap(romRange, bankRange);
      report.banks.push_back(bankArea);
      int usedPercent = percent(bankArea.used, bankArea.size);
      if (bankArea.used >= bankArea.size) addDiagnostic(Diagnostic::Warning, F("{0} is full ({1} bytes).", {bankArea.name, std::to_string(bankArea.size)}));
      else if (usedPercent >= options.warnPercent)
        addDiagnostic(Diagnostic::Warning, F("{0} is {1} % full: {2} bytes free.", {bankArea.name, std::to_string(usedPercent), std::to_string(bankArea.size - bankArea.used)}));
    }
    if (!formatSpace.romSizes.empty() && romFileSize > 0 &&
        std::find(formatSpace.romSizes.begin(), formatSpace.romSizes.end(), romFileSize) == formatSpace.romSizes.end()) {
      long maxSize = *std::max_element(formatSpace.romSizes.begin(), formatSpace.romSizes.end());
      std::string sizes;
      for (long size : formatSpace.romSizes) sizes += (sizes.empty() ? "" : ", ") + std::to_string(size);
      if (romFileSize > maxSize) {
        auto [file, line] = fileAndLineAt(base + maxSize);
        addDiagnostic(Diagnostic::Error, F("The ROM is {0} bytes, more than the format allows ({1}): the code overflows.",
                                  {std::to_string(romFileSize), sizes}), file, line);
      } else {
        addDiagnostic(Diagnostic::Warning, F("The ROM is {0} bytes; this format expects {1}. Emulators may not detect it.",
                                    {std::to_string(romFileSize), sizes}));
      }
    }
    // Hotspots and RAM ports, inside each bank's window.
    if (!formatSpace.reserved.empty() && (bank == 4096 || bank == 2048)) {
      long windowBase = 0x2000 - bank;
      std::set<std::string> reported;
      for (auto& romRange : rom) {
        for (long i = (romRange.lo - base) / bank; i * bank < romRange.hi - base; i++) {
          Range piece{std::max(romRange.lo - base, i * bank), std::min(romRange.hi - base, (i + 1) * bank)};
          if (piece.size() <= 0) continue;
          Range window{windowBase + piece.lo % bank, windowBase + piece.lo % bank + piece.size()};
          for (auto& reservedRange : formatSpace.reserved) {
            long overlapped = overlap(window, reservedRange);
            if (!overlapped) continue;
            // first address of the overlap that is not ds/align padding
            long first = -1;
            const CodeLine* codeLine = nullptr;
            for (long address = std::max(window.lo, reservedRange.lo); address < std::min(window.hi, reservedRange.hi); address++) {
              codeLine = lineAt(base + i * bank + (address - windowBase));
              if (!codeLine || !codeLine->fill) {
                first = address;
                break;
              }
            }
            if (first < 0) continue;
            std::string key = std::to_string(i) + hex(reservedRange.lo);
            if (!reported.insert(key).second) continue;
            std::string file = codeLine ? codeLine->file : "";
            int line = codeLine ? codeLine->line : 0;
            addDiagnostic(Diagnostic::Warning,
                 F("Bank {0}: code or data at {1}, a reserved address of the format (bank-switch hotspot or RAM port).",
                   {std::to_string(i), hex(first)}),
                 file, line);
          }
        }
      }
    }
  }

  // ---- 2600 RAM ----
  report.ram.name = T("2600 RAM");
  report.ram.size = 128;
  std::set<long> used;
  const RamDecl* highest = nullptr;
  for (auto& decl : ram) {
    if (decl.addr + decl.size <= 0x80 || decl.addr >= 0x100) continue;
    for (long address = std::max(0x80L, decl.addr); address < decl.addr + decl.size && address < 0x100; address++) used.insert(address);
    if (!highest || decl.addr + decl.size > highest->addr + highest->size) highest = &decl;
    if (decl.addr + decl.size > 0x100)
      addDiagnostic(Diagnostic::Error, F("{0} goes past the end of RAM ($FF).", {decl.label.empty() ? hex(decl.addr) : decl.label}), decl.file, decl.line);
  }
  if (!highest) {
    report.ram.measured = false;
    report.ram.note = T("Not measured: declare the variables with SEG.U and ds so VCSIDE can count them.");
  } else {
    report.ram.used = (long)used.size();
    long top = highest->addr + highest->size;
    report.stackRoom = std::max(0L, 0x100 - top);
    if (top <= 0x100 && report.stackRoom < options.stackReserve)
      addDiagnostic(Diagnostic::Warning,
           F("Only {0} bytes left for the stack above {1} (reserve: {2}).",
             {std::to_string(report.stackRoom), highest->label.empty() ? hex(highest->addr) : highest->label, std::to_string(options.stackReserve)}),
           highest->file, highest->line);
    else if (percent(report.ram.used, 128) >= options.warnPercent)
      addDiagnostic(Diagnostic::Warning, F("{0} is {1} % full: {2} bytes free.", {report.ram.name, std::to_string(percent(report.ram.used, 128)), std::to_string(128 - report.ram.used)}));
  }

  // ---- cartridge RAM ----
  if (formatSpace.ramBytes > 0) {
    SpaceReport::Area cartArea;
    cartArea.name = T("Cartridge RAM");
    cartArea.size = formatSpace.ramBytes;
    if (formatSpace.ramWindows.empty()) {
      cartArea.measured = false;
      cartArea.note = T("This format's RAM cannot be measured from the DASM output.");
    } else {
      std::set<long> offsets;
      for (auto& decl : ram) {
        if (decl.addr < 0x1000) continue;  // TIA, 2600 RAM, RIOT
        long address = 0x1000 | (decl.addr & 0x0FFF);
        for (auto& ramWindow : formatSpace.ramWindows) {
          if (address < ramWindow.lo || address >= ramWindow.hi) continue;
          long offset = address - ramWindow.lo;
          for (long byteIndex = 0; byteIndex < decl.size; byteIndex++) offsets.insert(offset + byteIndex);
          if (offset + decl.size > formatSpace.ramBytes)
            addDiagnostic(Diagnostic::Error, F("{0} goes past the end of the cartridge RAM ({1} bytes).",
                                      {decl.label.empty() ? hex(decl.addr) : decl.label, std::to_string(formatSpace.ramBytes)}),
                 decl.file, decl.line);
          break;
        }
      }
      cartArea.used = (long)offsets.size();
      int usedPercent = percent(cartArea.used, cartArea.size);
      if (cartArea.used > 0 && usedPercent >= options.warnPercent && cartArea.used <= cartArea.size)
        addDiagnostic(Diagnostic::Warning, F("{0} is {1} % full: {2} bytes free.", {cartArea.name, std::to_string(usedPercent), std::to_string(cartArea.size - cartArea.used)}));
    }
    report.cartRam = cartArea;
  }
  return report;
}

SpaceReport parseBasicSpace(const std::string& output, long romFileSize, const SpaceOptions& options) {
  SpaceReport report;
  report.romFileSize = romFileSize;
  static const std::regex pattern(R"(^\s*(-?\d+) bytes of ROM space left(?: in bank (\d+))?\s*$)");
  std::map<int, long> left;  // bank (from 0) -> bytes left
  long single = 0;
  bool hasSingle = false;
  std::istringstream stream(output);
  for (std::string line; std::getline(stream, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch match;
    if (!std::regex_match(line, match, pattern)) continue;
    if (match[2].matched) left[std::stoi(match[2]) - 1] = std::stol(match[1]);
    else single = std::stol(match[1]), hasSingle = true;  // also printed after the banks: the last one again
  }
  if (left.empty() && hasSingle) left[0] = single;
  long bankSize = left.size() > 1 ? 4096 : (romFileSize > 0 && romFileSize < 4096 ? romFileSize : 4096);
  if (options.basicRomSize > 0 && left.size() == 1) bankSize = options.basicRomSize;
  for (auto& [bank, free] : left) {
    SpaceReport::Area area;
    area.name = left.size() > 1 ? F("Bank {0}", {std::to_string(bank)}) : T("ROM");
    area.size = bankSize;
    area.used = bankSize - free;
    int usedPercent = percent(area.used, area.size);
    if (free < 0)
      report.diagnostics.push_back({Diagnostic::Error, "", 0, F("{0} does not fit: {1} bytes too many.", {area.name, std::to_string(-free)})});
    else if (free == 0)
      report.diagnostics.push_back({Diagnostic::Warning, "", 0, F("{0} is full ({1} bytes).", {area.name, std::to_string(area.size)})});
    else if (usedPercent >= options.warnPercent)
      report.diagnostics.push_back({Diagnostic::Warning, "", 0, F("{0} is {1} % full: {2} bytes free.", {area.name, std::to_string(usedPercent), std::to_string(free)})});
    report.banks.push_back(area);
  }
  report.ram.name = T("2600 RAM");
  report.ram.size = 128;
  report.ram.measured = false;
  report.ram.note = T("Not measured: batari Basic's variables are fixed (a-z and those its kernel reserves).");
  return report;
}

}  // namespace vcs
