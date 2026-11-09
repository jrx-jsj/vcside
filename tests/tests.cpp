// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Tests without GUI: knowledge base, cycle counter, output parsers.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "builder_parse.h"
#include "editor/text_editor.h"
#include "editor/folding.h"
#include "editor/indentation.h"
#include <memory>
#include "source_links.h"
#include "cycles.h"
#include "kb.h"
#include "space.h"
#include "debugmap.h"
#include "basic_syntax.h"
#include "elf_space.h"
#include "emulator_launch.h"
#include "i18n.h"
#include "gfx.h"
#include "theme.h"
#include "child_process.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <tuple>
#include <fstream>
#include <set>
#include <sstream>
#include <regex>

static int failures = 0, checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    checks++;                                                           \
    if (!(cond)) {                                                      \
      failures++;                                                       \
      std::fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                   \
  } while (0)

using namespace vcs;

static void testKb(const KnowledgeBase& kb) {
  CHECK(kb.instructions().size() >= 56);
  const Opcode* opcode = kb.opcode("lda", "absy");
  CHECK(opcode && opcode->code == 0xB9 && opcode->cycles == 4 && opcode->pagePenalty);
  opcode = kb.opcode("STA", "absx");
  CHECK(opcode && opcode->cycles == 5 && !opcode->pagePenalty);
  opcode = kb.opcode("DCP", "zp");
  CHECK(opcode && opcode->illegal && opcode->cycles == 5);
  CHECK(kb.opcode("NOP", "imp") && !kb.opcode("NOP", "imp")->illegal);
  // DASM cannot assemble JAM; LAS is a stable illegal opcode; every entry cites known sources.
  CHECK(kb.instruction("JAM") && !kb.instruction("JAM")->opcodes.empty() && !kb.instruction("JAM")->opcodes[0]->dasm);
  CHECK(kb.opcode("LAS", "absy") && kb.opcode("LAS", "absy")->stable);
  CHECK(kb.reg("WSYNC") && !kb.reg("WSYNC")->sources.empty());
  for (auto& reg : kb.registers())
    for (auto& id : reg.sources) CHECK(kb.source(id) != nullptr);
  for (auto& format : kb.formats())
    for (auto& id : format.sources) CHECK(kb.source(id) != nullptr);
  for (auto& [mnemonic, instruction] : kb.instructions())
    for (auto& id : instruction.sources) CHECK(kb.source(id) != nullptr);
  auto completions = kb.complete("ST");
  bool sta = false;
  for (auto& completion : completions) sta |= completion.word == "STA";
  CHECK(sta);
}

static void testCycles(const KnowledgeBase& kb) {
  std::map<std::string, int> syms = {{"WSYNC", 0x02}, {"RESP0", 0x10}, {"Ptr", 0x80}, {"Table", 0xF100}, {"SWCHA", 0x280}};
  SymbolLookup symbolLookup = [&](const std::string& name) -> std::optional<int> {
    auto found = syms.find(name);
    if (found == syms.end()) return std::nullopt;
    return found->second;
  };
  auto line = [&](const char* source) { return countLine(kb, source, symbolLookup); };

  LineCycles lineCycles = line("\tsta WSYNC");
  CHECK(lineCycles.known && lineCycles.mode == "zp" && lineCycles.min == 3 && lineCycles.max == 3 && lineCycles.bytes == 2);
  lineCycles = line("\tsta.w RESP0   ; 4 cycles");
  CHECK(lineCycles.known && lineCycles.mode == "abs" && lineCycles.min == 4);
  lineCycles = line("Loop\tlda Table,y");
  CHECK(lineCycles.known && lineCycles.mode == "absy" && lineCycles.min == 4 && lineCycles.max == 5);
  lineCycles = line("\tlda (Ptr),y");
  CHECK(lineCycles.mode == "indy" && lineCycles.min == 5 && lineCycles.max == 6);
  lineCycles = line("\tlda SWCHA");
  CHECK(lineCycles.mode == "abs" && lineCycles.min == 4);
  lineCycles = line("\tbne Loop");
  CHECK(lineCycles.mode == "rel" && lineCycles.min == 2 && lineCycles.max == 4);
  lineCycles = line("\tasl");
  CHECK(lineCycles.mode == "acc" && lineCycles.min == 2);
  lineCycles = line("\tlsr A");
  CHECK(lineCycles.mode == "acc");
  lineCycles = line("\tldx Unknown,y");
  CHECK(lineCycles.guessed && lineCycles.mode == "absy");
  lineCycles = line("\tSLEEP 7");
  CHECK(lineCycles.known && lineCycles.min == 7 && lineCycles.max == 7);
  lineCycles = line("\tjmp (Ptr)");
  CHECK(lineCycles.mode == "ind" && lineCycles.min == 5);
  lineCycles = line("\tsta Ptr+1,x");
  CHECK(lineCycles.mode == "zpx" && lineCycles.min == 4);
  lineCycles = line("\tlda #<Table");
  CHECK(lineCycles.mode == "imm" && lineCycles.min == 2);
  lineCycles = line("\torg $F000");
  CHECK(!lineCycles.isCode);
  lineCycles = line("Label");
  CHECK(!lineCycles.isCode);
  lineCycles = line("\tdc.b \"a;b\" ; comment");
  CHECK(!lineCycles.isCode);

  BlockCycles blockCycles = countBlock(kb, {"\tsta WSYNC", "\tlda Table,y", "\tsta RESP0", "\tdey", "\tbne Loop"}, symbolLookup);
  CHECK(blockCycles.instructions == 5 && blockCycles.min == 3 + 4 + 3 + 2 + 2 && blockCycles.max == 3 + 5 + 3 + 2 + 4);

  CHECK(evalExpr("$10+2", symbolLookup) == 0x12);
  CHECK(evalExpr(">Table", symbolLookup) == 0xF1);
  CHECK(evalExpr("%1010", symbolLookup) == 10);
  CHECK(!evalExpr("Nope", symbolLookup));
}

static void testSourceSymbols() {
  auto syms = scanAsmSymbols({
      "    processor 6502",
      "PFCOLOR = $1E          ; constant",
      "SPEED equ 3",
      "TOP=PFCOLOR+2",
      "ROWS =12",
      "  MAC SLEEP2 ; two cycles",
      "    nop",
      "  ENDM",
      "Frame:  lda #0",
      ".loop   dex",
      "score   ds 1",
      "1$      rts",
      "; Comment: lda",
  });
  auto find = [&](const std::string& name) -> const SourceSymbol* {
    for (auto& symbol : syms)
      if (symbol.name == name) return &symbol;
    return nullptr;
  };
  CHECK(syms.size() == 8);
  CHECK(find("PFCOLOR") && find("PFCOLOR")->kind == SourceSymbol::Constant && find("PFCOLOR")->value == "$1E");
  CHECK(find("SPEED") && find("SPEED")->kind == SourceSymbol::Constant && find("SPEED")->value == "3");
  CHECK(find("TOP") && find("TOP")->kind == SourceSymbol::Constant && find("TOP")->value == "PFCOLOR+2");
  CHECK(find("ROWS") && find("ROWS")->value == "12");
  CHECK(find("SLEEP2") && find("SLEEP2")->kind == SourceSymbol::Macro && find("SLEEP2")->line == 5);
  CHECK(find("Frame") && find("Frame")->kind == SourceSymbol::Label && find("Frame")->line == 8);
  CHECK(find(".loop") && find(".loop")->kind == SourceSymbol::Label);
  CHECK(find("score") && find("score")->kind == SourceSymbol::Label);
  CHECK(!find("1$") && !find("processor"));
}

static void testParsers() {
  auto dasmDiagnostics = parseDasmOutput(
      "--- Unresolved Symbol List\nUnknown                  0000 ????         (R )\n--- 1 Unresolved Symbol \n\n"
      "t.asm (4): error: Unknown Mnemonic 'foo'.\n\nFatal assembly error: Source is not resolvable.\n");
  CHECK(dasmDiagnostics.size() == 3);
  CHECK(dasmDiagnostics.size() == 3 && dasmDiagnostics[1].file == "t.asm" && dasmDiagnostics[1].line == 4 && dasmDiagnostics[1].severity == Diagnostic::Error);
  CHECK(dasmDiagnostics.size() == 3 && dasmDiagnostics[0].message.find("Unknown") != std::string::npos);

  auto gccDiagnostics = parseGccOutput("src/main.c:12:5: warning: unused variable 'x'\nsrc/a.c:3: error: boom\n");
  CHECK(gccDiagnostics.size() == 2 && gccDiagnostics[0].line == 12 && gccDiagnostics[0].severity == Diagnostic::Warning && gccDiagnostics[1].file == "src/a.c");
  // the ELF template's checks, ld and make
  gccDiagnostics = parseGccOutput(
      "src/main.c: error: undefined symbol 'foo' (only vcsLib functions may be left undefined)\n"
      "arm-none-eabi-gcc: error: nothing.c: No such file or directory\n"
      "/opt/bin/arm-none-eabi-ld: warning: build/x-chameleon.elf has a LOAD segment with RWX permissions\n"
      "/opt/bin/arm-none-eabi-ld: build/elf/main.o: in function `elf_main':\n"
      "main.c:(.text+0x1c): undefined reference to `bar'\n"
      "/opt/bin/arm-none-eabi-ld: region `RAM' overflowed by 120 bytes\n"
      "make: *** [build/x.bin] Error 1\n"
      "make: *** No rule to make target 'src/y.c', needed by 'build/elf/y.o'.  Stop.\n");
  CHECK(gccDiagnostics.size() == 5);
  if (gccDiagnostics.size() == 5) {
    CHECK(gccDiagnostics[0].file == "src/main.c" && gccDiagnostics[0].line == 0 && gccDiagnostics[0].message.find("'foo'") != std::string::npos);
    CHECK(gccDiagnostics[1].file.empty() && gccDiagnostics[1].message.rfind("arm-none-eabi-gcc: ", 0) == 0);
    CHECK(gccDiagnostics[2].file == "main.c" && gccDiagnostics[2].message == "undefined reference to `bar' (in elf_main)");
    CHECK(gccDiagnostics[3].file.empty() && gccDiagnostics[3].message == "region `RAM' overflowed by 120 bytes");
    CHECK(gccDiagnostics[4].message.rfind("make: No rule", 0) == 0);
  }

  auto symbols = parseDasmSymbols("--- Symbol List (sorted by symbol)\nStart                    f000              (R )\nVAR                      0081              (R )\n--- End of Symbol List.\n");
  CHECK(symbols.size() == 2 && symbols["Start"] == 0xF000 && symbols["VAR"] == 0x81);
}

static void testEditor() {
  // search
  std::vector<std::string> lines = {"\tlda Frame", "\tsta frame+1 ; frame", "FrameLoop dex"};
  CHECK(findMatches(lines, "frame", {}).size() == 4);
  CHECK(findMatches(lines, "frame", {true, false}).size() == 2);
  CHECK(findMatches(lines, "Frame", {false, true}).size() == 3);  // not FrameLoop
  CHECK(findMatches(lines, "", {}).empty());

  TextEditor textEditor;
  textEditor.setText("\tlda #1\n\tsta COLUBK\n\tdex");
  textEditor.selectLines(0, 1);
  textEditor.toggleComment();
  CHECK(textEditor.lines()[0] == ";\tlda #1" && textEditor.lines()[1] == ";\tsta COLUBK" && textEditor.lines()[2] == "\tdex");
  textEditor.toggleComment();
  CHECK(textEditor.lines()[0] == "\tlda #1" && textEditor.lines()[1] == "\tsta COLUBK");
  textEditor.undo();
  CHECK(textEditor.lines()[0] == ";\tlda #1");
  textEditor.undo();
  CHECK(textEditor.lines()[0] == "\tlda #1");

  textEditor.goTo(2);
  textEditor.moveLines(-1);
  CHECK(textEditor.lines()[1] == "\tdex" && textEditor.lines()[2] == "\tsta COLUBK" && textEditor.cursor().line == 1);
  textEditor.duplicateLines();
  CHECK(textEditor.lines().size() == 4 && textEditor.lines()[2] == "\tdex" && textEditor.cursor().line == 2);
  textEditor.deleteLines();
  CHECK(textEditor.lines().size() == 3);

  TextEditor basicEditor;
  basicEditor.setCommentPrefix("rem");
  basicEditor.setText("  COLUBK = $84");
  basicEditor.toggleComment();
  CHECK(basicEditor.lines()[0] == "  rem COLUBK = $84");
  basicEditor.toggleComment();
  CHECK(basicEditor.lines()[0] == "  COLUBK = $84");

  TextEditor findEditor;
  findEditor.setText("a\n\tsta WSYNC\nb\n\tsta WSYNC");
  findEditor.openFind(false);
  findEditor.setFindQuery("WSYNC");
  findEditor.findNext();
  CHECK(findEditor.hasSelection() && findEditor.cursor().line == 1);
  findEditor.findNext();
  CHECK(findEditor.cursor().line == 3);
  findEditor.findNext();
  CHECK(findEditor.cursor().line == 1);  // wraps
  findEditor.findNext(true);
  CHECK(findEditor.cursor().line == 3);  // backwards wraps

  TextEditor replaceEditor;
  replaceEditor.setText("lda Frame\nsta Frame");
  replaceEditor.openFind(true);
  replaceEditor.setFindQuery("Frame");
  replaceEditor.setReplaceText("Tick");
  CHECK(replaceEditor.replaceAll() == 2 && replaceEditor.text() == "lda Tick\nsta Tick");
  replaceEditor.undo();
  CHECK(replaceEditor.text() == "lda Frame\nsta Frame");
}

static std::string ras(const std::vector<std::pair<long, long>>& blocks) {
  std::string bytes;
  for (auto [org, length] : blocks) {
    bytes += char(org & 0xFF), bytes += char(org >> 8), bytes += char(length & 0xFF), bytes += char(length >> 8);
    bytes += std::string(length, '\xEA');
  }
  return bytes;
}

static bool hasDiag(const SpaceReport& report, Diagnostic::Severity severity, const std::string& part) {
  for (auto& diagnostic : report.diagnostics)
    if (diagnostic.severity == severity && diagnostic.message.find(part) != std::string::npos) return true;
  return false;
}

static void testSpace(const KnowledgeBase& kb) {
  auto range = parseRange("$1FF8-$1FF9");
  CHECK(range && range->lo == 0x1FF8 && range->hi == 0x1FFA);
  CHECK(parseRange("$1FF0") && parseRange("$1FF0")->size() == 1);
  CHECK(!parseRange("1FF8"));
  auto blocks = parseDasmRas(ras({{0x0000, 104}, {0x0FFC, 9}, {0x1FFC, 4}}));
  CHECK(blocks.size() == 3 && blocks[1].lo == 0x0FFC && blocks[1].hi == 0x1005);

  // Every format with a "space" object is consistent.
  int withSpace = 0;
  for (auto& format : kb.formats()) {
    if (!format.space.known) continue;
    withSpace++;
    CHECK(format.space.bankSize > 0);
    for (long size : format.space.romSizes) CHECK(size % format.space.bankSize == 0);
    CHECK(format.space.ramWindows.empty() || format.space.ramBytes > 0);
  }
  CHECK(withSpace >= 17);
  const CartFormat* f8 = kb.format("F8");
  const CartFormat* f8sc = kb.format("F8SC");
  CHECK(f8 && f8sc && f8sc->space.ramBytes == 128);

  // The listing of a small F8 program with Superchip RAM (DASM 2.20).
  const std::string listing =
      "------- FILE f8.asm LEVEL 1 PASS 1\n"
      "      1  0000\t\t\t\t      processor\t6502\n"
      "      4 U0080\t\t       00 00 00    Var1       ds\t3\n"
      "      5 U0083\t\t       00 00 00 00*Var2       ds\tSIZE\n"
      "      8 U1000\t\t       00 00 00 00*ScW\t      ds\t20\n"
      "     12  0000\t\t       a9 01\t   B0\t      lda\t#1\n"
      "     14  0004\t\t       ea ea ea ea*\t      ds\t100, $EA\n"
      "------- FILE inc.h LEVEL 2 PASS 1\n"
      "      3  1ff6\t\t       a9 02\t   B1\t      lda\t#2\n";
  std::vector<std::string> sourceLines(14);
  sourceLines[3] = "Var1       ds 3";
  sourceLines[4] = "Var2       ds SIZE";
  sourceLines[7] = "ScW\tds 20";
  SymbolLookup symbolLookup = [](const std::string& name) -> std::optional<int> {
    if (name == "SIZE") return 10;
    return std::nullopt;
  };
  auto ram = parseDasmListingRam(listing, [&](const std::string& file) { return file == "f8.asm" ? sourceLines : std::vector<std::string>{}; }, symbolLookup);
  CHECK(ram.size() == 3);
  CHECK(ram.size() == 3 && ram[1].label == "Var2" && ram[1].size == 10 && ram[2].addr == 0x1000 && ram[2].size == 20);
  auto code = parseDasmListingCode(listing);
  CHECK(code.size() == 3 && code.back().file == "inc.h" && code.back().addr == 0x1FF6);

  SpaceOptions options;
  // F8, two banks: bank 0 with 104+4 bytes, bank 1 with the vectors and code at $1FF6 (hotspot $1FF8).
  auto report = analyzeSpace(parseDasmRas(ras({{0x0000, 104}, {0x0FFC, 4}, {0x1FF6, 10}})), 8192, ram, code, f8->space, options);
  CHECK(report.banks.size() == 2 && report.banks[0].used == 108 && report.banks[1].used == 10);
  CHECK(report.ram.measured && report.ram.used == 13 && report.stackRoom == 128 - 13);
  CHECK(!report.cartRam);
  CHECK(hasDiag(report, Diagnostic::Warning, "$1FF8"));
  bool pointed = false;
  for (auto& diagnostic : report.diagnostics) pointed |= diagnostic.file == "inc.h" && diagnostic.line == 3;
  CHECK(pointed);
  // F8SC: the 20 bytes declared at $1000 are cartridge RAM.
  report = analyzeSpace(parseDasmRas(ras({{0x0100, 50}, {0x1100, 50}})), 8192, ram, code, f8sc->space, options);
  CHECK(report.cartRam && report.cartRam->measured && report.cartRam->used == 20 && report.cartRam->size == 128);
  CHECK(report.diagnostics.empty());
  CHECK(code.size() == 3 && !code[0].fill && code[1].fill);
  // Padding over the Superchip ports is fine; code there is not.
  std::vector<CodeLine> pad = {{"m.asm", 5, 0x0000, true}, {"m.asm", 6, 0x0100, false}};
  report = analyzeSpace(parseDasmRas(ras({{0x0000, 300}})), 8192, {}, pad, f8sc->space, options);
  CHECK(report.diagnostics.empty());
  pad[0].fill = false;
  report = analyzeSpace(parseDasmRas(ras({{0x0000, 300}})), 8192, {}, pad, f8sc->space, options);
  CHECK(hasDiag(report, Diagnostic::Warning, "$1000") && report.diagnostics[0].line == 5);
  // Wrong size: 12K for F8 is an overflow; 4K is a warning.
  report = analyzeSpace(parseDasmRas(ras({{0x0000, 12288}})), 12288, {}, {}, f8->space, options);
  CHECK(hasDiag(report, Diagnostic::Error, "12288"));
  CHECK(!report.ram.measured);
  report = analyzeSpace(parseDasmRas(ras({{0x0000, 100}})), 4096, {}, {}, f8->space, options);
  CHECK(hasDiag(report, Diagnostic::Warning, "4096"));
  // A full bank and one at 95 %.
  report = analyzeSpace(parseDasmRas(ras({{0x0000, 0x0FF8}, {0x0FFA, 6}, {0x1000, 3900}, {0x1FFA, 6}})), 8192, {}, {}, f8->space, options);
  CHECK(report.banks[0].used == 4094 && report.banks[1].used == 3906);
  CHECK(hasDiag(report, Diagnostic::Warning, "Bank 0 is 100 %") && hasDiag(report, Diagnostic::Warning, "Bank 1 is 96 %"));
  // 2600 RAM: little room for the stack, and a variable past $FF.
  std::vector<RamDecl> big = {{"a.asm", 4, 0x80, 124, "Buf"}};
  report = analyzeSpace({}, 0, big, {}, f8->space, options);
  CHECK(report.stackRoom == 4 && hasDiag(report, Diagnostic::Warning, "stack"));
  big[0].size = 130;
  report = analyzeSpace({}, 0, big, {}, f8->space, options);
  CHECK(hasDiag(report, Diagnostic::Error, "Buf") && report.diagnostics[0].line == 4);
  // E7's RAM cannot be measured; CV's can.
  const CartFormat* e7 = kb.format("E7");
  CHECK(e7 && e7->space.ramBytes == 2048);
  report = analyzeSpace({}, 0, {}, {}, e7->space, options);
  CHECK(report.cartRam && !report.cartRam->measured);
  CHECK(!report.summary().empty() || report.banks.empty());
}

static void testDebugMap() {
  // F8: ORG $0000/$1000 with RORG $F000 (DASM 2.20 listing), a macro and an include.
  const std::string listing =
      "------- FILE main.asm LEVEL 1 PASS 1\n"
      "      9  0000 ????\t\t\t\t      SEG\tBank0\n"
      "     10  0000\t\t\t\t\t      ORG\t$0000\n"
      "     11  0000\t\t\t\t\t      RORG\t$F000\n"
      "     12  0000\t\t       a9 01\t   B0\t      lda\t#1\n"
      "     13  0002\t\t\t\t   Loop\t\n"
      "     14  0002\t\t       85 80\t\t      sta\t$80\n"
      "     15  0004\t\t       ea ea ea ea*\t      ds\t100, $EA\n"
      "     16  0000 ????\t\t\t\t      SEG\tBank1\n"
      "     17  1000\t\t\t\t\t      ORG\t$1000\n"
      "     18  1000\t\t\t\t\t      RORG\t$F000\n"
      "     19  1000\t\t       a9 02\t   B1\t      lda\t#2\n"
      "      0  1002\t\t\t\t\t      FOO\n"
      "      1  1002\t\t       ea\t\t      nop\n"
      "      2  1003\t\t       ea\t\t      nop\n"
      "     21  1004\t\t       ea\t\t      nop\n"
      "------- FILE inc.h LEVEL 2 PASS 1\n"
      "      0  1005\t\t\t\t\t      include\t\"inc.h\"\n"
      "      1  1005\t\t       85 81\t\t      sta\t$81\n"
      "------- FILE main.asm\n"
      "     23  1007\t\t       ea\t\t      nop\n";
  AddressMap map;
  map.load(listing, [](const std::string&) -> std::optional<int> { return std::nullopt; }, 4096);
  CHECK(map.banks() == 2);
  auto isCode = [](const ListedLine& listedLine) { return listedLine.source.find("ds") == std::string::npos; };
  auto breakpoint = map.breakpointAt("main.asm", 12, isCode);
  CHECK(breakpoint && breakpoint->cpu == 0xF000 && breakpoint->bank == 0);
  breakpoint = map.breakpointAt("main.asm", 13, isCode);  // label line: next instruction
  CHECK(breakpoint && breakpoint->cpu == 0xF002 && breakpoint->at->line == 14);
  breakpoint = map.breakpointAt("main.asm", 19, isCode);
  CHECK(breakpoint && breakpoint->cpu == 0xF000 && breakpoint->bank == 1);
  breakpoint = map.breakpointAt("main.asm", 20, isCode);  // macro call: its first instruction
  CHECK(breakpoint && breakpoint->cpu == 0xF002 && breakpoint->bank == 1);
  breakpoint = map.breakpointAt("main.asm", 21, isCode);
  CHECK(breakpoint && breakpoint->cpu == 0xF004);
  breakpoint = map.breakpointAt("inc.h", 1, isCode);
  CHECK(breakpoint && breakpoint->cpu == 0xF005 && breakpoint->bank == 1);
  breakpoint = map.breakpointAt("main.asm", 23, isCode);
  CHECK(breakpoint && breakpoint->cpu == 0xF007);
  CHECK(!map.breakpointAt("main.asm", 24, isCode));
  auto lines = map.linesAt(0xF002);
  CHECK(lines.size() == 2);  // bank 0 (line 14) and bank 1 (the macro)
  lines = map.linesAt(0x1002, 0);  // a mirror of $F002
  CHECK(lines.size() == 1 && lines[0]->line == 14);
  lines = map.linesAt(0xF003, 1);  // inside the second nop of the macro
  CHECK(lines.size() == 1 && lines[0]->line == 20);
  map.resolveFiles([](const std::string& path) { return "/p/" + path; });
  CHECK(map.breakpointAt("/p/inc.h", 1, isCode));
  CHECK(map.linesAt(0xF050, 0).size() == 1);  // inside the ds: its length comes from the next line

  // Macros: a blank line before the call (listed without a tab) and two calls in a row
  std::string macroListing =
      "------- FILE m.asm LEVEL 1 PASS 2\n"
      "     10  f000\t\t       ea\t\t      nop\n"
      "     11  f001\n"
      "      0  f001\t\t\t\t\t      TWO\n"
      "      1  f001\t\t       ea\t\t      nop\n"
      "      2  f002\t\t       ea\t\t      nop\n"
      "      0  f003\t\t\t\t\t      ONE\n"
      "      1  f003\t\t       e8\t\t      inx\n"
      "     14  f004\n"
      "     15  f004\t\t       c8\t\t      iny\n";
  AddressMap macroMap;
  macroMap.load(macroListing, [](const std::string&) -> std::optional<int> { return std::nullopt; }, 4096);
  auto macroLines = macroMap.linesIn("m.asm");
  CHECK(macroLines.size() == 5);
  if (macroLines.size() == 5)
    CHECK(macroLines[1]->line == 12 && macroLines[2]->line == 12 && macroLines[3]->line == 13 && macroLines[4]->line == 15);

  // 6502 page separators
  std::string pagesListing =
      "------- FILE pages.asm LEVEL 1 PASS 2\n"
      "      1  f0fc\t\t\t\t\t      ORG\t$F0FC\n"
      "      2  f0fc\t\t       a9 01\t\t      lda\t#1\n"
      "      3  f0fe\t\t\t\t   Table\t\n"
      "      4  f0fe\t\t       01 02 03 04\t      .byte\t1,2,3,4\n"
      "      5  f102\t\t       ea\t\t      nop\n"
      "      6  f103\t\t\t\t\t      ; comment\n"
      "      7  f103\t\t\t\t\t      ORG\t$F200\n"
      "      8  f200\t\t\t\t   Next\t\n"
      "      9  f200\t\t       ea\t\t      nop\n"
      "     10  f201\t\t       00 00 00 00*\t      ds\t300\n"
      "     11  f32d\t\t       ea\t\t      nop\n";
  AddressMap pagesMap;
  pagesMap.load(pagesListing, [](const std::string&) -> std::optional<int> { return std::nullopt; }, 4096);
  std::vector<std::string> pagesSource = {"  ORG $F0FC", "  lda #1", "Table", "  .byte 1,2,3,4", "  nop", "; comment",
                                          "  ORG $F200", "Next", "  nop", "  ds 300", "  nop"};
  auto breaks = pageBreaks(pagesMap, "pages.asm", pagesSource);
  CHECK(breaks.size() == 3);
  if (breaks.size() == 3) {
    CHECK(breaks[0].line == 4 && breaks[0].address == 0xF100 && breaks[0].inside);   // the table crosses into $F100
    CHECK(breaks[1].line == 7 && breaks[1].address == 0xF200 && !breaks[1].inside);  // above the label Next
    CHECK(breaks[2].line == 10 && breaks[2].address == 0xF300 && breaks[2].inside);  // the ds goes on into $F300
  }

  // Breakpoints follow the text.
  TextEditor textEditor;
  textEditor.setText("a\nb\nc\nd");
  textEditor.toggleBreakpoint(2);
  CHECK(textEditor.breakpoints() == std::set<int>{2});
  textEditor.goTo(0);
  textEditor.duplicateLines();  // a a b c d
  CHECK(textEditor.breakpoints() == std::set<int>{3});
  textEditor.goTo(3);
  textEditor.deleteLines();  // the line with the breakpoint goes
  CHECK(textEditor.breakpoints().empty());
  textEditor.undo();
  textEditor.toggleBreakpoint(4);
  textEditor.goTo(1);
  textEditor.moveLines(1);
  CHECK(textEditor.breakpoints() == std::set<int>{4});
  textEditor.toggleBreakpoint(4);
  CHECK(textEditor.breakpoints().empty() && textEditor.takeBreakpointsChanged() && !textEditor.takeBreakpointsChanged());
}

static void testEmulatorOptions(const KnowledgeBase& kb) {
  namespace fs = std::filesystem;
  const EmulatorInfo* stella = kb.emulator("stella");
  const EmulatorInfo* gopher = kb.emulator("gopher2600");
  CHECK(stella && gopher);
  if (!stella || !gopher) return;
  // every option is well formed and its sources exist
  for (auto* emulator : {stella, gopher}) {
    std::set<std::string> ids;
    for (auto& option : emulator->options) {
      CHECK(ids.insert(option.id).second);
      CHECK(!option.label.empty() && !option.description.empty() && !option.group.empty());
      CHECK(option.type == "bool" || option.type == "choice" || option.type == "int" || option.type == "text");
      CHECK((option.type != "bool" && option.type != "choice") || !option.values.empty());
      CHECK((emulator == stella) == !option.arg.empty());
      CHECK(emulator == stella || (!option.flag.empty()) != (!option.pref.empty()));
      CHECK(!option.developer || option.arg.rfind("-dev.", 0) == 0);
    }
  }
  CHECK(kb.source("stella-7.0-manual") && kb.source("gopher2600-0.56.1"));
  // translations fall back to English
  const EmuOption* debugColors = stella->option("dev.debugcolors");
  CHECK(debugColors && debugColors->labelText() == "Debug colours");
  setLanguage("es");
  CHECK(debugColors && debugColors->labelText() == "Colores de depuración");
  setLanguage("en");

  Project project;
  project.dir = "/tmp/proj";
  project.output = "build/game.bin";
  project.emulator = "stella";
  project.emulatorOptions["stella"] = {{"dev.debugcolors", "1"}, {"audio.enabled", "0"}, {"holdreset", "1"}, {"tv.filter", "2"}};
  auto arguments = emulatorOptionArgs(project, *stella);
  auto has = [&](const std::vector<std::string>& args, std::initializer_list<std::string> sequence) {
    std::vector<std::string> want(sequence);
    return std::search(args.begin(), args.end(), want.begin(), want.end()) != args.end();
  };
  CHECK(has(arguments, {"-holdreset"}) && !has(arguments, {"-holdreset", "1"}));
  CHECK(has(arguments, {"-audio.enabled", "0"}) && has(arguments, {"-tv.filter", "2"}) && has(arguments, {"-dev.debugcolors", "1"}));
  CHECK(has(arguments, {"-dev.settings", "1"}));  // a developer option turns the developer settings on
  CHECK(has(arguments, {"-snapsavedir", (fs::path("/tmp/proj/build").lexically_normal() / "").string()}));
  project.emulatorOptions["stella"]["dev.settings"] = "0";
  CHECK(!has(emulatorOptionArgs(project, *stella), {"-dev.settings", "1"}));

  project.emulatorOptions["gopher2600"] = {{"swap", "true"}, {"left", "PADDLE"}, {"randState", "true"}, {"audioMutePlay", "true"}};
  std::map<std::string, std::string> prefs;
  arguments = emulatorOptionArgs(project, *gopher, &prefs);
  CHECK(has(arguments, {"-swap=true"}) && has(arguments, {"-left", "PADDLE"}));
  CHECK(prefs.size() == 2 && prefs["hardware.randState"] == "true" && prefs["sdlimgui.playmode.audioMute"] == "true");
  std::string merged = mergeGopherPreferences("*** do not edit this file by hand ***\nhardware.randState :: false\nx.y :: 3\n", prefs);
  CHECK(merged == "*** do not edit this file by hand ***\nhardware.randState :: true\nx.y :: 3\nsdlimgui.playmode.audioMute :: true\n");
  CHECK(mergeGopherPreferences("", {{"a", "1"}}).rfind("***", 0) == 0);

  // launches with a private copy of the settings
  fs::path tempDir = fs::temp_directory_path() / "vcside-test-emu";
  std::error_code errorCode;
  fs::remove_all(tempDir, errorCode);
  fs::path home = tempDir / "home", root = tempDir / "profiles";
#if defined(_WIN32)
  fs::path goUser = home / "gopher2600";
#elif defined(__APPLE__)
  fs::path goUser = home / "Library" / "Application Support" / "gopher2600";
#else
  fs::path goUser = home / ".config" / "gopher2600";
#endif
  fs::create_directories(goUser);
  std::ofstream(goUser / "preferences") << "*** do not edit this file by hand ***\nmine.key :: 7\nhardware.randState :: false\n";
#ifdef _WIN32
  fs::path stUser = home / "Stella";
#else
  fs::path stUser = home / ".config" / "stella";
#endif
  fs::create_directories(stUser);
  std::ofstream(stUser / "stella.sqlite3") << "settings";
  EmulatorLaunch launch;
  launch.args = {"/usr/local/bin/stella", "-debug", "/tmp/proj/build/game.bin"};
  std::string error;
  CHECK(prepareEmulatorLaunch(project, kb, root.string(), launch, &error, home.string()));
  CHECK(launch.args.back() == "/tmp/proj/build/game.bin");
  CHECK(has(launch.args, {"-basedir", (root / "stella").string()}) && has(launch.args, {"-userdir"}) && launch.env.empty());
  CHECK(fs::exists(root / "stella" / "stella.sqlite3"));
  launch = {};
  launch.args = {"/Applications/Stella.app/Contents/MacOS/Stella", "/tmp/proj/build/game.bin"};
  fs::create_directories(home / "Library" / "Application Support" / "Stella");
  std::ofstream(home / "Library" / "Application Support" / "Stella" / "stella.sqlite3") << "app";
  CHECK(prepareEmulatorLaunch(project, kb, root.string(), launch, &error, home.string()));
  CHECK(!has(launch.args, {"-basedir"}) && launch.env.size() == 1 && launch.env[0].first == "HOME");
  CHECK(fs::exists(root / "stella-home" / "Library" / "Application Support" / "Stella" / "stella.sqlite3"));
  // the macOS app reads <HOME>/<rom name>.script, not the one in -userdir
  Project scriptProject = project;
  scriptProject.dir = (tempDir / "proj").string();
  fs::create_directories(tempDir / "proj" / "build");
  std::ofstream(tempDir / "proj" / "build" / "game.script") << "clearBreaks\nbreak $F000 0\n";
  launch = {};
  launch.args = {"/Applications/Stella.app/Contents/MacOS/Stella", "-debug", (tempDir / "proj" / "build" / "game.bin").string()};
  CHECK(prepareEmulatorLaunch(scriptProject, kb, root.string(), launch, &error, home.string()));
  std::ifstream copiedScript(root / "stella-home" / "game.script");
  std::string copiedText((std::istreambuf_iterator<char>(copiedScript)), std::istreambuf_iterator<char>());
  copiedScript.close();
  CHECK(copiedText == "clearBreaks\nbreak $F000 0\n");
  fs::remove(tempDir / "proj" / "build" / "game.script");
  launch = {};
  launch.args = {"/Applications/Stella.app/Contents/MacOS/Stella", "-debug", (tempDir / "proj" / "build" / "game.bin").string()};
  CHECK(prepareEmulatorLaunch(scriptProject, kb, root.string(), launch, &error, home.string()));
  CHECK(!fs::exists(root / "stella-home" / "game.script"));

  project.emulator = "gopher2600";
  project.tv = "SECAM60";
  launch = {};
  launch.args = {"/usr/local/bin/gopher2600", "RUN", "/tmp/proj/build/game.bin"};
  CHECK(prepareEmulatorLaunch(project, kb, root.string(), launch, &error, home.string()));
  CHECK(launch.workDir == (root / "gopher2600").string() && fs::exists(root / "gopher2600" / "portable.txt"));
  CHECK(launch.notes.size() == 1);  // Gopher2600 has no SECAM60
  std::ifstream prefsFile(root / "gopher2600" / "Gopher2600_UserData" / "preferences");
  std::string prefsText((std::istreambuf_iterator<char>(prefsFile)), {});
  CHECK(prefsText.find("hardware.randState :: true") != std::string::npos && prefsText.find("mine.key :: 7") != std::string::npos);
  project.emulatorOptions["gopher2600"] = {{"swap", "true"}};  // no preference: the user's own folder
  launch = {};
  launch.args = {"/usr/local/bin/gopher2600", "RUN", "/tmp/proj/build/game.bin"};
  CHECK(prepareEmulatorLaunch(project, kb, root.string(), launch, &error, home.string()) && launch.workDir == project.dir);
  fs::remove_all(tempDir, errorCode);
  CHECK(tvSupported("stella", "NTSC50") && !tvSupported("stella", "PAL-M") && tvSupported("gopher2600", "PAL-M"));
  CHECK(tvIs50Hz("NTSC50") && !tvIs50Hz("PAL60"));
}

// A minimal 32-bit little-endian ELF: sections (name, type, flags, size) and LOAD segments (vaddr, paddr, filesz, memsz).
static std::string makeElf(int type, const std::vector<std::tuple<std::string, uint32_t, uint32_t, uint32_t>>& sections,
                           const std::vector<std::array<uint32_t, 4>>& segments) {
  auto put = [](std::string& bytes, size_t offset, uint32_t value, int byteCount) {
    if (bytes.size() < offset + byteCount) bytes.resize(offset + byteCount);
    for (int i = 0; i < byteCount; i++) bytes[offset + i] = (char)(value >> (8 * i));
  };
  std::string names(1, '\0');
  std::vector<uint32_t> nameOffsets;
  for (auto& section : sections) nameOffsets.push_back((uint32_t)names.size()), names += std::get<0>(section) + '\0';
  uint32_t shstr = (uint32_t)names.size();
  names += ".shstrtab";
  names += '\0';
  std::string bytes(52, '\0');
  bytes[0] = 0x7f, bytes[1] = 'E', bytes[2] = 'L', bytes[3] = 'F', bytes[4] = 1, bytes[5] = 1, bytes[6] = 1;
  put(bytes, 16, type, 2);
  put(bytes, 18, 40, 2);  // ARM
  uint32_t phoff = 52, strAt = phoff + 32 * (uint32_t)segments.size(), shoff = (strAt + (uint32_t)names.size() + 3) & ~3u;
  put(bytes, 28, segments.empty() ? 0 : phoff, 4);
  put(bytes, 32, shoff, 4);
  put(bytes, 42, 32, 2);
  put(bytes, 44, (uint32_t)segments.size(), 2);
  put(bytes, 46, 40, 2);
  put(bytes, 48, (uint32_t)sections.size() + 2, 2);
  put(bytes, 50, (uint32_t)sections.size() + 1, 2);
  for (size_t i = 0; i < segments.size(); i++) {
    size_t tableOffset = phoff + 32 * i;
    put(bytes, tableOffset, 1, 4);
    put(bytes, tableOffset + 8, segments[i][0], 4);
    put(bytes, tableOffset + 12, segments[i][1], 4);
    put(bytes, tableOffset + 16, segments[i][2], 4);
    put(bytes, tableOffset + 20, segments[i][3], 4);
  }
  bytes.resize(strAt);
  bytes += names;
  bytes.resize(shoff + 40);  // section 0: null
  for (size_t i = 0; i < sections.size(); i++) {
    size_t tableOffset = shoff + 40 * (i + 1);
    put(bytes, tableOffset, nameOffsets[i], 4);
    put(bytes, tableOffset + 4, std::get<1>(sections[i]), 4);
    put(bytes, tableOffset + 8, std::get<2>(sections[i]), 4);
    put(bytes, tableOffset + 20, std::get<3>(sections[i]), 4);
  }
  size_t tableOffset = shoff + 40 * (sections.size() + 1);
  put(bytes, tableOffset, shstr, 4);
  put(bytes, tableOffset + 4, 3, 4);
  put(bytes, tableOffset + 16, strAt, 4);
  put(bytes, tableOffset + 20, (uint32_t)names.size(), 4);
  bytes.resize(tableOffset + 40);
  return bytes;
}

static void testElfSpace() {
  // PROGBITS 1, NOBITS 8; WRITE 1, ALLOC 2, EXEC 4
  std::string rom = makeElf(1, {{".text", 1, 6, 1000}, {".RamFunc", 1, 6, 300}, {".rodata.tab", 1, 2, 2000},
                                {".data.x", 1, 3, 40}, {".bss.y", 8, 3, 500}, {".comment", 1, 0, 19}}, {});
  ElfFile romElf;
  std::string error;
  CHECK(readElf(rom, romElf, &error) && romElf.type == 1 && romElf.machine == 40 && romElf.sections.size() == 8);
  CHECK(romElf.sections.size() == 8 && romElf.sections[2].name == ".RamFunc");
  ElfSpace elfSpace = analyzeElfSpace(romElf, (long)rom.size(), nullptr, 90);
  CHECK(elfSpace.code == 1000 && elfSpace.ramCode == 300 && elfSpace.constants == 2000 && elfSpace.data == 40 && elfSpace.bss == 500 && !elfSpace.exact);
  CHECK(elfSpace.flash.used == 3340 + kChameleonFirmwareFlash && elfSpace.ram.used == 840 + kChameleonFirmwareRam);
  CHECK(elfSpace.diagnostics.empty());
  // the Chameleon image: flash = what is stored at 0x0800_0000, RAM = what lives at 0x2000_0000
  std::string img = makeElf(2, {}, {{0x08000000, 0x08000000, 120000, 120000}, {0x20000000, 0x0801D4C0, 4000, 34000},
                                   {0x20008500, 0x0801E460, 0, 1100}});
  ElfFile imageElf;
  CHECK(readElf(img, imageElf, &error) && imageElf.type == 2 && imageElf.segments.size() == 3);
  elfSpace = analyzeElfSpace(romElf, (long)rom.size(), &imageElf, 90);
  CHECK(elfSpace.exact && elfSpace.flash.used == 124000 && elfSpace.ram.used == 35100);
  CHECK(elfSpace.diagnostics.size() == 2 && elfSpace.diagnostics[0].severity == Diagnostic::Warning);  // 94 % and 95 %
  CHECK(!readElf("not an elf", romElf, &error) && !error.empty());

  // a new ELF project from the template
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "vcside-test-elf";
  std::error_code errorCode;
  fs::remove_all(dir, errorCode);
  Project project;
  CHECK(Project::create(dir.string(), "rainbow", Language::ElfC, "NTSC", &project, &error));
  CHECK(project.buildCommand == "make" && project.format == "ELF" && project.output == "build/rainbow.bin" && project.main == "src/main.c");
  for (auto fileName : {"Makefile", "include/vcsLib.h", "src/main.c", "src/memcpy_long.S", "chameleon/support.c"}) CHECK(fs::exists(dir / fileName));
  CHECK(!fs::exists(dir / "LICENSE.md"));
  std::ifstream makefile(dir / "Makefile");
  std::string makefileText((std::istreambuf_iterator<char>(makefile)), {});
  CHECK(makefileText.find("NAME  = rainbow") != std::string::npos && makefileText.find("{{") == std::string::npos);
  CHECK(!Project::create((dir / "b").string(), "two words", Language::ElfC, "NTSC", nullptr, &error));
  CHECK(Project::create((dir / "bb").string(), "Square Run", Language::BatariBasic, "NTSC", &project, &error));
  CHECK(project.main == "main.bas" && project.output == "main.bas.bin" && project.format == "AUTO" && project.buildCommand.empty());
  std::ifstream basFile(dir / "bb" / "main.bas");
  std::string basicText((std::istreambuf_iterator<char>(basFile)), {});
  CHECK(basicText.find("rem Square Run - batari Basic") != std::string::npos && basicText.find("drawscreen") != std::string::npos);
  CHECK(basicText.find("set tv") == std::string::npos);  // NTSC: batari Basic's own timing
  CHECK(Project::create((dir / "bbpal").string(), "Pal Run", Language::BatariBasic, "PAL", &project, &error));
  std::ifstream palFile(dir / "bbpal" / "main.bas");
  std::string palText((std::istreambuf_iterator<char>(palFile)), {});
  CHECK(palText.find("  set romsize 4k\n  set tv pal\n") != std::string::npos);
  CHECK(tvStandardsFor(Language::BatariBasic) == std::vector<std::string>({"NTSC", "PAL", "PAL60"}));
  CHECK(tvStandardsFor(Language::Dasm).size() == 7);
  // a folder that builds with its own Makefile becomes a project; tools and flags of its own
  {
    fs::path madeDir = dir / "made";
    fs::create_directories(madeDir / "tools", errorCode);
    std::ofstream(madeDir / "Makefile") << "ROM = $(NAME).bin\nall:\n\tdasm game.asm -f3 -oout/game.bin\n";
    std::ofstream(madeDir / "game.asm") << "\tprocessor 6502\n";
    std::ofstream(madeDir / "notes.asm") << "; nothing\n";
    Project detected;
    CHECK(Project::detect(madeDir.string(), &detected));
    CHECK(detected.buildCommand == "make" && detected.main == "game.asm" && detected.output == "out/game.bin" && detected.language == Language::Dasm && detected.name == "made");
    CHECK(!Project::detect((dir / "bb").string(), &detected));
    fs::path scriptedDir = dir / "scripted";
    fs::create_directories(scriptedDir, errorCode);
    std::ofstream(scriptedDir / "build.sh") << "bb main.bas\n";
    std::ofstream(scriptedDir / "main.bas") << "  drawscreen\n";
    CHECK(Project::detect(scriptedDir.string(), &detected) && detected.buildCommand == "sh build.sh" && detected.language == Language::BatariBasic && detected.output == "main.bin");
    // no build of its own: a DASM source (with its processor line) or a batari Basic one, in the
    // folder or in src/, is built by VCSIDE; C needs a Makefile; a project file wins
    fs::path plainDir = dir / "plain";
    fs::create_directories(plainDir / "src", errorCode);
    fs::create_directories(plainDir / "include", errorCode);
    std::ofstream(plainDir / "src" / "kernel.asm") << "; included by game.asm\n";
    std::ofstream(plainDir / "src" / "game.asm") << "\tprocessor 6502\n\tinclude \"vcs.h\"\n";
    CHECK(Project::detect(plainDir.string(), &detected));
    CHECK(detected.buildCommand.empty() && detected.language == Language::Dasm && detected.main == "src/game.asm" && detected.output == "build/game.bin");
    CHECK(detected.format == "AUTO" && detected.includeDirs == std::vector<std::string>({"src", "include"}));
    fs::path basicDir = dir / "basic-only";
    fs::create_directories(basicDir, errorCode);
    std::ofstream(basicDir / "game.bas") << "  drawscreen\n";
    CHECK(Project::detect(basicDir.string(), &detected) && detected.language == Language::BatariBasic && detected.main == "game.bas" && detected.buildCommand.empty());
    fs::path cDir = dir / "c-only";
    fs::create_directories(cDir, errorCode);
    std::ofstream(cDir / "main.c") << "int main(void) { return 0; }\n";
    CHECK(!Project::detect(cDir.string(), &detected));
    std::ofstream(basicDir / "game.vcsproj") << "{}\n";
    CHECK(!Project::detect(basicDir.string(), &detected));

    Toolchain toolchain;
    Project own;
    own.dir = madeDir.string(), own.main = "game.asm";
    own.toolPaths["dasm"] = "tools/dasm";
    own.buildFlags = "-DPAL=1 -T1";
    std::string why;
    CHECK(own.buildArgs(toolchain, &why).empty() && why.find("tools/dasm") != std::string::npos);  // not there yet
    std::ofstream(madeDir / "tools" / "dasm") << "";
    auto args = own.buildArgs(toolchain, &why);
    CHECK(!args.empty() && args[0] == (madeDir / "tools" / "dasm").lexically_normal().string() && args.back() == "-T1" && args[args.size() - 2] == "-DPAL=1");
    CHECK(own.buildTools(toolchain).empty());  // its own DASM: nothing to look for
    auto rasArguments = own.rasArgs(toolchain);
    CHECK(!rasArguments.empty() && rasArguments.back() == "-DPAL=1");
    own.file = (madeDir / "own.vcsproj").string();
    CHECK(own.save(&why));
    Project back;
    CHECK(back.load(own.file, &why) && back.toolPaths.at("dasm") == "tools/dasm" && back.buildFlags == "-DPAL=1 -T1");
    // keys VCSIDE does not use stay in the file; text that does not read is refused
    Project kept;
    CHECK(kept.fromJson(R"({"name":"k","notes":{"by":"me"},"tv":"PAL"})", (madeDir / "k.vcsproj").string(), &why));
    CHECK(kept.tv == "PAL" && kept.otherKeys.count("notes") && kept.toJson().find("\"by\": \"me\"") != std::string::npos);
    CHECK(!kept.fromJson("{\"name\": ", (madeDir / "k.vcsproj").string(), &why) && kept.name == "k" && !why.empty());
    CHECK(!kept.fromJson("[1]", (madeDir / "k.vcsproj").string(), &why));
    own.buildCommand = "make clean && make";
    args = own.buildArgs(toolchain, &why);
#ifdef _WIN32
    CHECK(args.size() == 3 && args[0] == "cmd.exe" && args[2] == "make clean && make");
#else
    CHECK(args.size() == 3 && args[0] == "/bin/sh" && args[1] == "-c" && args[2] == "make clean && make");
#endif
  }

  // the DASM example: its graphics, the tables they give, and a program that assembles
  CHECK(Project::examples(Language::Dasm).size() == 1 && Project::examples(Language::BatariBasic).empty());
  for (const char* tvStandard : {"NTSC", "PAL", "AUTO"}) {
    fs::path animationDir = dir / (std::string("anim-") + tvStandard);
    CHECK(Project::create(animationDir.string(), "walker", Language::Dasm, tvStandard, &project, &error, "animation"));
    for (auto fileName : {"main.asm", "2600.h", "hero.vcsgfx", "hero.gfx.asm"}) CHECK(fs::exists(animationDir / fileName));
    std::ifstream includeFile(animationDir / "hero.gfx.asm");
    std::string includeText((std::istreambuf_iterator<char>(includeFile)), {});
    CHECK(includeText.find("hero_walk_FRAME") != std::string::npos && includeText.find("hero_P0_FramePtrLo") != std::string::npos);
    std::string dasm = findInPath("dasm");
    if (!dasm.empty()) {
      std::string command = "cd \"" + animationDir.string() + "\" && \"" + dasm + "\" main.asm -f3 -oexample.bin > dasm.log 2>&1";
      CHECK(std::system(command.c_str()) == 0 && fs::file_size(animationDir / "example.bin", errorCode) == 4096);
      if (std::string(tvStandard) == "AUTO") {  // both systems from one source: -DPAL=1 gives the 50 Hz one
        std::string pal = "cd \"" + animationDir.string() + "\" && \"" + dasm + "\" main.asm -f3 -DPAL=1 -opal.bin > dasm.log 2>&1";
        CHECK(std::system(pal.c_str()) == 0 && fs::file_size(animationDir / "pal.bin", errorCode) == 4096);
        std::ifstream exampleRomFile(animationDir / "example.bin", std::ios::binary), palRomFile(animationDir / "pal.bin", std::ios::binary);
        std::string exampleBytes((std::istreambuf_iterator<char>(exampleRomFile)), {}), palBytes((std::istreambuf_iterator<char>(palRomFile)), {});
        CHECK(exampleBytes != palBytes);
      }
    }
  }
  CHECK(!Project::create((dir / "none").string(), "x", Language::Dasm, "NTSC", &project, &error, "nope") && !error.empty());
  // the ELF example: the template with the example's main.c, and hero.gfx.h next to it
  CHECK(Project::examples(Language::ElfC).size() == 1);
  fs::path elfDir = dir / "anim-elf";
  CHECK(Project::create(elfDir.string(), "walker", Language::ElfC, "NTSC", &project, &error, "animation"));
  for (auto fileName : {"Makefile", "include/vcsLib.h", "src/main.c", "src/memcpy_long.S", "src/hero.vcsgfx", "src/hero.gfx.h"})
    CHECK(fs::exists(elfDir / fileName));
  std::ifstream mainFile(elfDir / "src" / "main.c"), headerFile(elfDir / "src" / "hero.gfx.h");
  std::string mainText((std::istreambuf_iterator<char>(mainFile)), {}), headerText((std::istreambuf_iterator<char>(headerFile)), {});
  CHECK(mainText.find("walker - made with VCSIDE") != std::string::npos && mainText.find("hero.gfx.h") != std::string::npos);
  CHECK(headerText.find("static const vcsgfx_animation hero_walk = {hero_walk_steps, 4, 1};") != std::string::npos);
  std::string gcc = findInPath("arm-none-eabi-gcc"), make = findInPath("make");
  if (!gcc.empty() && !make.empty()) {
    std::string command = "cd \"" + elfDir.string() + "\" && \"" + make + "\" > make.log 2>&1";
    CHECK(std::system(command.c_str()) == 0 && fs::exists(elfDir / "build" / "walker.bin"));
  }
  CHECK(!Project::create((dir / "none-elf").string(), "x", Language::ElfC, "NTSC", &project, &error, "nope") && !error.empty());
  if (!std::getenv("VCSIDE_TEST_KEEP")) fs::remove_all(dir, errorCode);
}

static void testFormatOrder(const KnowledgeBase& kb) {
  CHECK(naturalLess("Atari 8K (F8)", "Atari 16K (F6)") && !naturalLess("Atari 16K (F6)", "Atari 8K (F8)"));
  CHECK(naturalLess("Atari 8K (F8)", "Atari 8K with Superchip (F8SC)") && naturalLess("bus", "CDF") && naturalLess("x2", "x010"));
  CHECK(!naturalLess("a", "a") && naturalLess("a", "ab"));
  std::set<std::string> cats;
  for (auto& [id, title] : formatCategories()) cats.insert(id);
  for (auto& format : kb.formats()) {
    CHECK(cats.count(format.category));
    if (!cats.count(format.category)) std::fprintf(stderr, "  format %s: category '%s'\n", format.id.c_str(), format.category.c_str());
  }
}

static void testBasicSyntax(const KnowledgeBase& kb) {
  // keywords from data/kb/batari-basic.json, ranges expanded
  CHECK(kb.basic("drawscreen") && kb.basic("drawscreen")->kind == "statement");
  CHECK(kb.basic("joy0up") && kb.basic("joy3fire") && !kb.basic("joy4up"));
  CHECK(kb.basic("joy1left") && kb.basic("joy1left")->summary.find("joystick 1") != std::string::npos);
  CHECK(kb.basic("player2:") && kb.basic("player0color:") && kb.basic("temp7") && !kb.basic("temp8"));
  CHECK(kb.basic("player1x")->syntax == "player1x = 50");
  CHECK(!kb.basic("Drawscreen"));  // the compiler wants lowercase
  for (auto& [word, keyword] : kb.basicKeywords()) {
    CHECK(!keyword.summary.empty() && !keyword.syntax.empty() && word.find('N') == std::string::npos);
    if (keyword.summary.empty()) std::fprintf(stderr, "  %s\n", word.c_str());
  }
  CHECK(basicLabelOf("main") == "main" && basicLabelOf("10 goto 10") == "10" && basicLabelOf(" drawscreen").empty());
  CHECK(basicLabelOf("player0:").empty() && basicLabelOf("").empty());

  auto kinds = [&](const std::string& line) {
    std::vector<std::pair<std::string, TokenKind>> result;
    for (auto& token : tokenizeBasic(kb, line)) result.push_back({line.substr(token.start, token.end - token.start), token.kind});
    return result;
  };
  using K = TokenKind;
  auto tokens = kinds("loop if joy0fire then goto shoot");
  CHECK(tokens.size() == 6 && tokens[0].second == K::Label && tokens[1].second == K::Mnemonic && tokens[2].second == K::Mnemonic &&
        tokens[5].first == "shoot" && tokens[5].second == K::Label);
  tokens = kinds(" if a > 3 then a = 0 : COLUBK = $84 rem blue");
  CHECK(tokens.size() == 9 && tokens[1].second == K::Symbol && tokens[2].second == K::Number && tokens[4].first == "a" && tokens[4].second == K::Symbol &&
        tokens[6].second == K::Register && tokens[7].second == K::Number && tokens[8].first == "rem blue" && tokens[8].second == K::Comment);
  tokens = kinds(" if switchreset then reset_game");
  CHECK(tokens.back().first == "reset_game" && tokens.back().second == K::Label);
  tokens = kinds(" on level goto l0 l1 l2");
  CHECK(tokens.size() == 6 && tokens[3].second == K::Label && tokens[5].second == K::Label);
  tokens = kinds(" player0:");
  CHECK(tokens.size() == 1 && tokens[0].first == "player0:" && tokens[0].second == K::Directive);
  tokens = kinds(" player0x = player0x + 1");
  CHECK(tokens.size() == 3 && tokens[0].second == K::Register && tokens[1].second == K::Register);
  tokens = kinds(" set romsize 8kSC");
  CHECK(tokens.size() == 3 && tokens[0].second == K::Directive && tokens[1].second == K::Directive);
}

static void testBasic() {
  // batari Basic 1.9: its own error, then DASM's in the generated .asm
  auto diagnostics = parseBasicOutput("Starting build of main.bas\nbatari Basic v1.9 (c)2025\nline 4: Error: Unknown keyword: frobnicate\n\nCompilation failed.\n",
                            "main.bas", "");
  CHECK(diagnostics.size() == 1 && diagnostics[0].severity == Diagnostic::Error && diagnostics[0].file == "main.bas" && diagnostics[0].line == 4 &&
        diagnostics[0].message == "Unknown keyword: frobnicate");
  const std::string asmText =
      "; header\n"                       // 1
      " jsr drawscreen\n"                // 2
      ".L03 ;;line 5;;  a = a + 1\n"     // 3
      "\tLDA a\n"                        // 4
      ".L04 ;;line 6;;  goto nowhere\n"  // 5
      "\n"                                // 6
      " jmp .nowhere\n"                   // 7
      " if ECHOFIRST\n"                   // 8
      " echo \"x\"\n";                    // 9
  auto map = basicLineMap(asmText);
  CHECK(map.size() == 9 && map[0] == 0 && map[2] == 5 && map[3] == 5 && map[6] == 6 && map[7] == 0 && map[8] == 0);
  diagnostics = parseBasicOutput("2600 Basic compilation complete.\n\n      2859 bytes of ROM space left\n\n"
                       "main.bas.asm (7): error: Unknown Mnemonic 'jmp .nowhere'.\n\nFatal assembly error: Source is not resolvable.\n",
                       "main.bas", asmText);
  CHECK(diagnostics.size() == 1 && diagnostics[0].file == "main.bas" && diagnostics[0].line == 6 && diagnostics[0].message.find("nowhere") != std::string::npos &&
        diagnostics[0].message.find("Mnemonic") == std::string::npos);
  diagnostics = parseBasicOutput("main.bas.asm (9): error: something\n", "main.bas", asmText);  // in the includes: stays in the .asm
  CHECK(diagnostics.size() == 1 && diagnostics[0].file == "main.bas.asm" && diagnostics[0].line == 9);
  CHECK(parseBasicOutput("2600 Basic compilation complete.\n      148 bytes of ROM space left\n", "main.bas", "").empty());

  // space: one bank, or 4 KB banks counted from 1
  SpaceOptions options;
  auto space = parseBasicSpace("      148 bytes of ROM space left\n", 4096, options);
  CHECK(space.banks.size() == 1 && space.banks[0].used == 4096 - 148 && space.banks[0].size == 4096 && !space.ram.measured);
  CHECK(space.diagnostics.size() == 1 && space.diagnostics[0].severity == Diagnostic::Warning);
  space = parseBasicSpace("   1000 bytes of ROM space left\n", 2048, options);
  CHECK(space.banks.size() == 1 && space.banks[0].size == 2048 && space.banks[0].used == 1048 && space.diagnostics.empty());
  space = parseBasicSpace("      3251 bytes of ROM space left in bank 1\r\n      -20 bytes of ROM space left in bank 2\n"
                      "      2276 bytes of ROM space left\n", 8192, options);
  CHECK(space.banks.size() == 2 && space.banks[0].name == "Bank 0" && space.banks[0].used == 4096 - 3251 && space.banks[1].used == 4116);
  CHECK(space.diagnostics.size() == 1 && space.diagnostics[0].severity == Diagnostic::Error);
  CHECK(parseBasicSpace("nothing\n", 4096, options).banks.empty());

  // breakpoints: the listing of the .asm taken to the lines of the .bas
  const std::string listing =
      "------- FILE main.bas.asm LEVEL 1 PASS 3\n"
      "      2  f000\t\t       20 00 f8\t      jsr\tdrawscreen\n"
      "      3  f003\t\t\t\t   .L03\t\t;;line 5;;  a = a + 1\n"
      "      4  f003\t\t       a5 d6\t      LDA\ta\n"
      "      7  f005\t\t       4c 00 f0\t      jmp\t.L03\n";
  AddressMap addressMap;
  addressMap.load(listing, [](const std::string&) { return std::optional<int>(); }, 4096);
  addressMap.remap("main.bas.asm", map, "/p/main.bas");
  auto any = [](const ListedLine&) { return true; };
  auto breakpoint = addressMap.breakpointAt("/p/main.bas", 5, any);
  CHECK(breakpoint && breakpoint->cpu == 0xF003 && breakpoint->at->line == 5);
  breakpoint = addressMap.breakpointAt("/p/main.bas", 6, any);
  CHECK(breakpoint && breakpoint->cpu == 0xF005);
  CHECK(!addressMap.breakpointAt("/p/main.bas", 7, any));
  CHECK(addressMap.linesAt(0xF000).empty());  // the header has no line of the .bas
}

static bool hasIssue(const std::vector<gfx::Issue>& issues, gfx::Issue::Severity severity, const std::string& part) {
  for (auto& issue : issues)
    if (issue.severity == severity && issue.text.find(part) != std::string::npos) return true;
  return false;
}

static std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> result;
  std::string currentLine;
  for (char character : text) {
    if (character == '\n') result.push_back(currentLine), currentLine.clear();
    else currentLine += character;
  }
  result.push_back(currentLine);
  return result;
}

static bool hasRegion(const std::vector<FoldRegion>& regions, int first, int last) {
  for (auto& region : regions)
    if (region.first == first && region.last == last) return true;
  return false;
}

// Indentation: the file's own style, and what Enter, braces, Tab and Backspace do in C.
static void testIndentation() {
  IndentStyle tabs{true, 4}, fourSpaces{false, 4};
  CHECK(detectIndentStyle({"int f()", "{", "  a();", "  if (x) {", "    b();", "  }", "}"}, tabs) == (IndentStyle{false, 2}));
  CHECK(detectIndentStyle({"main", "\tlda #1", "\tsta COLUBK"}, fourSpaces) == tabs);
  CHECK(detectIndentStyle({"int x;"}, fourSpaces) == fourSpaces);  // nothing indented: the fallback
  CHECK(detectIndentStyle({"/*", " * a comment", " */", "int x;"}, fourSpaces) == fourSpaces);
  bool inComment = false;
  CHECK(cCode("if (c == '{') { // {", inComment) == "if (c == '') { ");
  CHECK(cCode("s = \"}\"; /* { ", inComment) == "s = \"\"; " && inComment);
  CHECK(cCode("} */ x();", inComment) == " x();" && !inComment);

  auto editor = [](const std::string& text, IndentStyle fallback = IndentStyle{false, 4}) {
    auto made = std::make_unique<TextEditor>();
    made->setFoldSyntax(FoldSyntax::C);
    made->setDefaultIndent(fallback);
    made->setText(text);
    return made;
  };
  // after "{" one level more, and the "}" that closes it, in the file's style (2 spaces)
  auto textEditor = editor("void f(void) {\n  int a;\n}\nvoid g(void) {");
  textEditor->goTo(3, 14);
  textEditor->newline();
  CHECK(textEditor->lines().size() == 6 && textEditor->lines()[4] == "  " && textEditor->lines()[5] == "}");
  CHECK(textEditor->cursor().line == 4 && textEditor->cursor().col == 2);
  textEditor->typeText("x();");
  textEditor->newline();
  CHECK(textEditor->lines()[5] == "  " && textEditor->lines()[6] == "}");  // no second "}"
  // a block already closed gets no other "}"
  textEditor = editor("int main()\n{\n    return 0;\n}");
  textEditor->goTo(1, 1);
  textEditor->newline();
  CHECK(textEditor->lines().size() == 5 && textEditor->lines()[2] == "    " && textEditor->lines()[3] == "    return 0;" && textEditor->lines()[4] == "}");
  // "{|}" opens up into three lines
  textEditor = editor("if (x) {}");
  textEditor->goTo(0, 8);
  textEditor->newline();
  CHECK(textEditor->lines().size() == 3 && textEditor->lines()[1] == "    " && textEditor->lines()[2] == "}" && textEditor->cursor().line == 1);
  // a line at column 0 that is not a label: no tab, as in assembly
  textEditor = editor("int x;");
  textEditor->goTo(0, 6);
  textEditor->newline();
  CHECK(textEditor->lines()[1].empty());
  // a braceless "if": one level for its statement, then back
  textEditor = editor("void f(void)\n{\n    if (x)");
  textEditor->goTo(2, 10);
  textEditor->newline();
  CHECK(textEditor->lines()[3] == "        ");
  textEditor->typeText("y();");
  textEditor->newline();
  CHECK(textEditor->lines()[4] == "    ");
  // "{" on its own line under the "if" (Allman), "}" under the line it closes
  textEditor = editor("void f(void)\n{\n    if (x)\n        ");
  textEditor->goTo(3, 8);
  textEditor->typeText("{");
  CHECK(textEditor->lines()[3] == "    {");
  textEditor->newline();
  CHECK(textEditor->lines()[4] == "        " && textEditor->lines()[5] == "    }");
  textEditor = editor("int f(void) {\n  if (x) {\n    y();\n      ");
  textEditor->goTo(3, 6);
  textEditor->typeText("}");
  CHECK(textEditor->lines()[3] == "  }");
  textEditor->undo();
  CHECK(textEditor->lines()[3] == "      ");
  // a block comment goes on with " * "
  textEditor = editor("/* The kernel");
  textEditor->goTo(0, 13);
  textEditor->newline();
  CHECK(textEditor->lines()[1] == " * " && textEditor->cursor().col == 3);
  textEditor->typeText("runs.");
  textEditor->newline();
  CHECK(textEditor->lines()[2] == " * ");
  // Enter in the indentation leaves the code where it was
  textEditor = editor("{\n    x();\n}");
  textEditor->goTo(1, 2);
  textEditor->newline();
  CHECK(textEditor->lines()[1].empty() && textEditor->lines()[2] == "    x();" && textEditor->cursor().line == 2 && textEditor->cursor().col == 4);
  // Tab, Shift+Tab and Backspace go by levels of spaces
  textEditor = editor("x();\n  y();");
  textEditor->goTo(0, 0);
  textEditor->indentKey(false);
  CHECK(textEditor->lines()[0] == "  x();");
  textEditor->indentKey(true);
  CHECK(textEditor->lines()[0] == "x();");
  textEditor = editor("void f(void) {\n    if (a) {\n        b();\n    }\n      x();\n}", tabs);  // levels of 4
  textEditor->goTo(4, 6);
  textEditor->backspace();
  CHECK(textEditor->lines()[4] == "    x();" && textEditor->cursor().col == 4);
  textEditor->selectLines(3, 4);
  textEditor->indentKey(false);
  CHECK(textEditor->lines()[3] == "        }" && textEditor->lines()[4] == "        x();");
  // assembly keeps its tabs, and the label rule
  TextEditor assembly;
  assembly.setFoldSyntax(FoldSyntax::Asm);
  assembly.setText("Start");
  assembly.goTo(0, 5);
  assembly.newline();
  CHECK(assembly.lines()[1] == "\t");
}

static void testFolding() {
  // DASM
  auto asmLines = splitLines(
      "; ----\n"            // 0  comment run 0-1
      "; header\n"          // 1
      "        processor 6502\n"
      "        SEG Code\n"  // 3  section 3-22
      "        ORG $F000\n"
      "Reset   sei\n"       // 5  label 5-10 (blank and comment trimmed)
      "        cld\n"
      "        MAC WAIT\n"  // 7  macro 7-9
      "        sta WSYNC\n"
      "        ENDM\n"
      "        IF PAL\n"    // 10 if 10-12
      "        nop\n"
      "        ENDIF\n"
      "\n"
      "; next\n"
      "Kernel  SUBROUTINE\n"  // 15
      "        REPEAT 4\n"    // 16 repeat 16-18
      "        nop\n"
      "        REPEND\n"
      ".loop   dex\n"
      "        bne .loop\n"
      "Value = 5\n"          // 21 not a block
      "        rts\n");
  auto asmRegions = foldRegions(asmLines, FoldSyntax::Asm);
  CHECK(hasRegion(asmRegions, 0, 1));
  CHECK(hasRegion(asmRegions, 3, 22));
  CHECK(hasRegion(asmRegions, 5, 12));
  CHECK(hasRegion(asmRegions, 7, 9));
  CHECK(hasRegion(asmRegions, 10, 12));
  CHECK(hasRegion(asmRegions, 15, 22));
  CHECK(hasRegion(asmRegions, 16, 18));
  for (auto& region : asmRegions) CHECK(region.first != 21 && region.first != 19);

  // batari Basic
  auto basicLines = splitLines(
      "  rem my game\n"      // 0 comment run 0-1
      "  rem by me\n"
      "  set kernel multisprite\n"
      "main\n"               // 3 label 3-13
      "  player0:\n"         // 4 block 4-7
      "  %00111100\n"
      "  %01000010\n"
      "end\n"
      "  data level\n"       // 8 data 8-10
      "  1, 2, 3\n"
      "  end\n"
      "  rem vcsgfx hero.vcsgfx hero\n"  // 11 vcsgfx 11-13
      "  player0:\n"
      "  rem vcsgfx end\n"
      "\n"
      "loop\n"               // 15 label 15-16
      "  goto loop\n");
  auto basicRegions = foldRegions(basicLines, FoldSyntax::Basic);
  CHECK(hasRegion(basicRegions, 0, 1));
  CHECK(hasRegion(basicRegions, 3, 13));
  CHECK(hasRegion(basicRegions, 4, 7));
  CHECK(hasRegion(basicRegions, 8, 10));
  CHECK(hasRegion(basicRegions, 11, 13));
  CHECK(hasRegion(basicRegions, 15, 16));

  // C
  auto cLines = splitLines(
      "/* licence\n"         // 0 block comment 0-1
      "   text */\n"
      "#ifdef DEBUG\n"       // 2 #if 2-4
      "int x;\n"
      "#endif\n"
      "void f(void)\n"       // 5 braces 5-9 (the lone brace joins the line above)
      "{\n"
      "  const char *s = \"}\";\n"
      "  if (x) { x = 0; }\n"
      "}\n"
      "// a\n"               // 10 comment run 10-11
      "// b\n");
  auto cRegions = foldRegions(cLines, FoldSyntax::C);
  CHECK(hasRegion(cRegions, 0, 1));
  CHECK(hasRegion(cRegions, 2, 4));
  CHECK(hasRegion(cRegions, 5, 9));
  CHECK(hasRegion(cRegions, 10, 11));
  CHECK(cRegions.size() == 4);

  // the editor: folding hides the lines; editing above moves the fold; the cursor unfolds
  TextEditor textEditor;
  textEditor.setFoldSyntax(FoldSyntax::Asm);
  textEditor.setText("Start   nop\n        nop\n        nop\nOther   nop\n        nop\n");
  CHECK(textEditor.foldAt(0) && textEditor.folded(0) && textEditor.hiddenLine(1) && textEditor.hiddenLine(2) && !textEditor.hiddenLine(3));
  textEditor.goTo(0);
  textEditor.replaceText("; new\nStart   nop\n        nop\n        nop\nOther   nop\n        nop\n");
  CHECK(textEditor.folded(1) && !textEditor.folded(0) && textEditor.hiddenLine(2));
  textEditor.goTo(2);
  CHECK(!textEditor.folded(1) && !textEditor.hiddenLine(2));
  textEditor.foldAll();
  CHECK(textEditor.folded(1) && textEditor.folded(4));
  textEditor.unfoldAll();
  CHECK(!textEditor.folded(1) && !textEditor.folded(4));
}

static void testSourceLinks() {
  std::string fileName;
  int startColumn = 0, endColumn = 0;
  auto matchesFile = [&](const std::string& line, const std::string& want, int start) {
    return fileNameInLine(line, &fileName, &startColumn, &endColumn) && fileName == want && startColumn == start && endColumn == start + (int)want.size();
  };
  CHECK(matchesFile("        include \"2600.h\"", "2600.h", 17));
  CHECK(matchesFile("        INCLUDE 2600.h  ; registers", "2600.h", 16));
  CHECK(matchesFile("Data    incbin \"gfx/title.bin\"", "gfx/title.bin", 16));
  CHECK(matchesFile("#include \"vcs.h\"", "vcs.h", 10));
  CHECK(matchesFile("# include <stdint.h>", "stdint.h", 11));
  CHECK(matchesFile("  include div_mul.asm", "div_mul.asm", 10));
  CHECK(matchesFile("  inline kernel.asm", "kernel.asm", 9));
  CHECK(matchesFile("  rem vcsgfx hero.vcsgfx hero frame 2", "hero.vcsgfx", 13));
  CHECK(!fileNameInLine("  rem vcsgfx end", &fileName, &startColumn, &endColumn));
  CHECK(!fileNameInLine("        lda #include", &fileName, &startColumn, &endColumn));
  CHECK(!fileNameInLine("        lda include \"x.h\"", &fileName, &startColumn, &endColumn));
  CHECK(!fileNameInLine("; include \"x.h\"", &fileName, &startColumn, &endColumn));
  CHECK(!fileNameInLine("        include", &fileName, &startColumn, &endColumn));
}

static void testGfx() {
  using namespace gfx;
  // palette: grey hue 0, SECAM by luminance only
  CHECK(rgb(Tv::NTSC, 0x00) == 0x0F0F0F || (rgb(Tv::NTSC, 0x00) >> 16) == (rgb(Tv::NTSC, 0x00) & 0xFF));
  CHECK(rgb(Tv::SECAM, 0x0E) == 0xFFFFFF && rgb(Tv::SECAM, 0x40) == 0x000000 && rgb(Tv::SECAM, 0x42) == rgb(Tv::SECAM, 0x92));
  CHECK(tvFromProject("PAL60") == Tv::PAL && tvFromProject("PAL-M") == Tv::NTSC && tvFromProject("SECAM60") == Tv::SECAM);
  CHECK(hexColour(0x1F) == "$1E");

  // playfield bits: PF0 4-7, PF1 7-0, PF2 0-7
  int registerIndex, bit;
  pfBit(0, &registerIndex, &bit);
  CHECK(registerIndex == 0 && bit == 4);
  pfBit(4, &registerIndex, &bit);
  CHECK(registerIndex == 1 && bit == 7);
  pfBit(19, &registerIndex, &bit);
  CHECK(registerIndex == 2 && bit == 7);
  Playfield playfield;
  playfield.rows.resize(3);
  playfield.setPixel(0, 0, true);
  playfield.rows[0].mode = PfMode::Mirror;
  CHECK(playfield.pixel(0, 39) && !playfield.pixel(0, 20) && !playfield.rows[0].asymmetric);
  playfield.rows[1].mode = PfMode::Repeat;
  playfield.setPixel(1, 4, true);
  CHECK(playfield.pixel(1, 24));
  playfield.setPixel(1, 30, true);  // the right half: the row becomes asymmetric, the old copy stays
  CHECK(playfield.rows[1].asymmetric && playfield.pixel(1, 24) && playfield.pixel(1, 30) && !playfield.pixel(1, 10));
  uint8_t leftRegisters[3], rightRegisters[3];
  playfield.registers(1, leftRegisters, rightRegisters);
  CHECK(leftRegisters[1] == 0x80 && rightRegisters[1] == 0x82 && rightRegisters[2] == 0);  // column 30 = right index 10 = PF1 bit 1
  playfield.setAsymmetric(1, false);
  CHECK(!playfield.pixel(1, 30) && playfield.pixel(1, 24));
  playfield.rows[2].pf = 0x44;
  CHECK(playfield.pfAt(1) == playfield.pfColour && playfield.pfAt(2) == 0x44);

  // sprites: layers, frames, colours
  Sprite sprite;
  sprite.name = "hero";
  Layer playerLayer;
  playerLayer.obj = Obj::P0;
  playerLayer.resize(1, 3);
  playerLayer.bits[0] = {0x81, 0x3C, 0xFF};
  playerLayer.colours[0] = {0x1E, -1, 0x44};
  Layer missileLayer;
  missileLayer.obj = Obj::M0;
  missileLayer.dx = 9;
  missileLayer.size = 2;
  missileLayer.resize(1, 3);
  missileLayer.bits[0] = {1, 1, 0};
  missileLayer.colours[0] = {0x1E, 0x88, -1};
  sprite.layers = {playerLayer, missileLayer};
  CHECK(playerLayer.colour(0, 1) == 0x1E && playerLayer.colour(0, 2) == 0x44 && playerLayer.width() == 8 && missileLayer.width() == 2);
  CHECK(playerLayer.pixel(0, 0, 0) && !playerLayer.pixel(0, 0, 1) && playerLayer.pixel(0, 0, 7));
  Layer big = playerLayer;
  big.size = 2;
  big.reflect = true;
  CHECK(big.width() == 16 && big.pixel(0, 1, 4) && big.pixel(0, 1, 11) && !big.pixel(0, 1, 3));
  Profile ownProfile, standardProfile;
  standardProfile.kernel = Kernel::Standard;
  auto issues = check(sprite, ownProfile);
  CHECK(hasIssue(issues, Issue::Warning, "COLUP0"));  // M0 row 2 is $88, P0's is $1E
  CHECK(!hasIssue(issues, Issue::Error, ""));
  sprite.layers.push_back(playerLayer);  // a second P0 in the same phase
  CHECK(hasIssue(check(sprite, ownProfile), Issue::Error, "P0"));
  sprite.layers.back().phase = 1;
  issues = check(sprite, ownProfile);
  CHECK(!hasIssue(issues, Issue::Error, "") && hasIssue(issues, Issue::Warning, "30 Hz"));
  CHECK(hasIssue(check(sprite, standardProfile), Issue::Warning, "alternate the phases"));
  sprite.layers.pop_back();
  sprite.insertFrame(1, 0);
  CHECK(sprite.frames == 2 && sprite.layers[0].bits[1] == sprite.layers[0].bits[0]);
  sprite.layers[0].bits[1][0] = 0x18;
  sprite.removeFrame(0);
  CHECK(sprite.frames == 1 && sprite.layers[0].bits[0][0] == 0x18);
  sprite.layers[0].bits[0][0] = 0x81;

  // JSON round trip
  Asset asset;
  sprite.background = 0x0A;  // the editor's backdrop travels with the sprite
  asset.sprites = {sprite};
  asset.playfields = {playfield};
  Asset decoded;
  std::string error;
  CHECK(fromJson(toJson(asset), &decoded, &error));
  CHECK(decoded.sprites.size() == 1 && decoded.sprites[0].layers.size() == 2 && decoded.sprites[0].layers[1].dx == 9 &&
        decoded.sprites[0].layers[0].bits[0] == sprite.layers[0].bits[0] && decoded.sprites[0].layers[0].colours[0] == sprite.layers[0].colours[0]);
  CHECK(decoded.playfields.size() == 1 && decoded.playfields[0].rows[2].pf == 0x44 && decoded.playfields[0].pixel(0, 39));
  CHECK(toJson(decoded) == toJson(asset) && decoded.sprites[0].background == 0x0A);
  CHECK(!fromJson("{nope", &decoded, &error) && !error.empty());

  // the generated tables: one file, a file per item, a file per group
  {
    Asset splitAsset = asset;
    splitAsset.sprites.push_back(sprite), splitAsset.sprites.back().name = "hero two";
    auto one = generatedFiles(splitAsset, "art.vcsgfx", false);
    CHECK(one.size() == 1 && one[0].name == "art.gfx.asm" && generatedFrom(one[0].text.substr(0, one[0].text.find('\n')), "art.vcsgfx"));
    splitAsset.split = Asset::Split::Each;
    auto each = generatedFiles(splitAsset, "art.vcsgfx", true);
    CHECK(each.size() == 3 && each[1].name == "art.hero_two.gfx.h" && each[1].text.find("ART_HERO_TWO_GFX_H") != std::string::npos);
    splitAsset.split = Asset::Split::Group;
    splitAsset.sprites[1].group = "people", splitAsset.playfields[0].group = "rooms";
    auto groups = generatedFiles(splitAsset, "art.vcsgfx", false);
    CHECK(groups.size() == 3 && groups[0].name == "art.gfx.asm" && groups[1].name == "art.people.gfx.asm" && groups[2].name == "art.rooms.gfx.asm");
    Asset splitDecoded;
    CHECK(fromJson(toJson(splitAsset), &splitDecoded, &error) && splitDecoded.split == Asset::Split::Group && splitDecoded.sprites[1].group == "people" &&
          splitDecoded.playfields[0].group == "rooms" && toJson(splitDecoded) == toJson(splitAsset));
    CHECK(!generatedFrom("; Generated by VCSIDE from other.vcsgfx: edit that file, not this one.", "art.vcsgfx"));
  }

  // DASM export, read back
  std::string dasmText = exportDasm(sprite);
  CHECK(dasmText.find("hero_P0_Frame0\n        .byte #%11111111;$44\n        .byte #%00111100;$1E\n        .byte #%10000001;$1E\n") != std::string::npos);
  CHECK(dasmText.find("hero_M0_DX = 9") != std::string::npos && dasmText.find("hero_M0_SIZE = $10") != std::string::npos);
  Asset importedAsset;
  std::string what;
  CHECK(importText(dasmText, &importedAsset, &what) && importedAsset.sprites.size() == 1);
  if (importedAsset.sprites.size() == 1) {
    auto& importedSprite = importedAsset.sprites[0];
    CHECK(importedSprite.name == "hero" && importedSprite.layers.size() == 2);
    CHECK(importedSprite.layers.size() == 2 && importedSprite.layers[0].bits[0] == sprite.layers[0].bits[0] && importedSprite.layers[0].colour(0, 2) == 0x44);
    CHECK(importedSprite.layers.size() == 2 && importedSprite.layers[1].obj == Obj::M0 && importedSprite.layers[1].dx == 9 && importedSprite.layers[1].size == 2 && importedSprite.layers[1].bits[0][1] == 1);
  }
  // PlayerPal-style tables: Frame/ColorFrame labels, bottom row first, colour comments
  importedAsset = Asset();
  CHECK(importText(";---Graphics Data---\nFrame0\n        .byte #%00011000;$46\n        .byte #%00111100;$1A\nFrame1\n        .byte #%00100100;$46\n"
                   "        .byte #%01111110;$1A\n;---Color Data---\nColorFrame0\n        .byte #$46;\n        .byte #$1A;\n",
                   &importedAsset, &what));
  CHECK(importedAsset.sprites.size() == 1 && importedAsset.sprites[0].frames == 2 && importedAsset.sprites[0].layers.size() == 1);
  if (importedAsset.sprites.size() == 1 && importedAsset.sprites[0].layers.size() == 1) {
    auto& importedLayer = importedAsset.sprites[0].layers[0];
    CHECK(importedLayer.bits[0][0] == 0x3C && importedLayer.bits[0][1] == 0x18 && importedLayer.colour(0, 0) == 0x1A && importedLayer.bits[1][0] == 0x7E && importedLayer.colour(1, 1) == 0x46);
  }
  // batari Basic export and import. The standard kernel without options: one colour per
  // player, rows bottom first, missiles as blocks.
  std::string basicText = exportBasic(sprite, standardProfile);
  CHECK(basicText.find("  player0:\n  %11111111\n  %00111100\n  %10000001\nend\n  COLUP0 = $1E : rem every frame\n") != std::string::npos);
  CHECK(basicText.find("missile0height = 2") != std::string::npos);
  CHECK(hasIssue(check(sprite, standardProfile), Issue::Warning, "playercolors"));
  // playercolors: a colour per row, at the cost of both missiles
  Profile playercolorsProfile = profileFromBasic(" rem set kernel PXE\n  set kernel_options playercolors pfcolors ; colours\n  const pfres = 12\n");
  CHECK(playercolorsProfile.kernel == Kernel::Standard && playercolorsProfile.options.playercolors && playercolorsProfile.options.pfcolors && !playercolorsProfile.options.player1colors && playercolorsProfile.options.pfres == 12);
  CHECK(playercolorsProfile.rowColours(Obj::P0) && !playercolorsProfile.has(Obj::M0) && !playercolorsProfile.has(Obj::M1) && playercolorsProfile.has(Obj::BL) && playercolorsProfile.pfRowColours() && playercolorsProfile.pfRowsDefault() == 12);
  CHECK(hasIssue(check(sprite, playercolorsProfile), Issue::Error, "M0"));
  basicText = exportBasic(sprite, playercolorsProfile);
  CHECK(basicText.find("  player0color:\n  $44\n  $1E\n  $1E\nend\n") != std::string::npos && basicText.find("rem M0: not drawn") != std::string::npos);
  importedAsset = Asset();
  CHECK(importText(basicText, &importedAsset, &what) && importedAsset.sprites.size() == 1 && importedAsset.sprites[0].layers[0].bits[0] == sprite.layers[0].bits[0] &&
        importedAsset.sprites[0].layers[0].colour(0, 2) == 0x44);
  Profile backgroundProfile = profileFromBasic("set kernel_options pfcolors backgroundchange\n");
  CHECK(!backgroundProfile.pfRowColours() && backgroundProfile.bkRowColours());
  CHECK(!profileFromBasic("set kernel_options player1colors\n").has(Obj::M1) && profileFromBasic("set kernel_options player1colors\n").has(Obj::M0));
  // DPC+: rows top first, a colour table always, ten players
  Profile dpcProfile = profileFromBasic("  set kernel DPC+\n");
  CHECK(dpcProfile.kernel == Kernel::DPCPlus && dpcProfile.players() == 10 && dpcProfile.has(Obj::P9) && !dpcProfile.has(Obj::P10) && dpcProfile.topFirst());
  basicText = exportBasic(sprite, dpcProfile);
  CHECK(basicText.find("  player0:\n  %10000001\n  %00111100\n  %11111111\nend\n  player0color:\n  $1E\n  $1E\n  $44\nend\n") != std::string::npos);
  importedAsset = Asset();
  CHECK(importText(basicText, &importedAsset, &what, dpcProfile) && importedAsset.sprites.size() == 1 && importedAsset.sprites[0].layers[0].bits[0] == sprite.layers[0].bits[0]);
  importedAsset = Asset();
  CHECK(importText("  set kernel DPC+\n" + basicText, &importedAsset, &what) && importedAsset.sprites.size() == 1 && importedAsset.sprites[0].layers[0].bits[0] == sprite.layers[0].bits[0]);
  // PXE: up to P16; the extra players exist only in the kernels that draw them
  Profile pxeProfile = profileFromBasic("set kernel PXE\n");
  Sprite many = sprite;
  many.layers = {playerLayer};
  many.layers[0].obj = Obj::P12;
  many.layers.push_back(playerLayer);
  many.layers[1].obj = Obj::P3;
  CHECK(!hasIssue(check(many, pxeProfile), Issue::Error, "") && hasIssue(check(many, pxeProfile), Issue::Warning, "player 1"));
  CHECK(hasIssue(check(many, standardProfile), Issue::Error, "P12") && hasIssue(check(many, dpcProfile), Issue::Error, "P12"));
  basicText = exportBasic(many, pxeProfile);
  CHECK(basicText.find("  player12:\n  %10000001\n") != std::string::npos && basicText.find("player12color:") != std::string::npos);
  importedAsset = Asset();
  CHECK(importText(basicText, &importedAsset, &what, pxeProfile) && importedAsset.sprites.size() == 1 && importedAsset.sprites[0].layers.size() == 2);
  if (importedAsset.sprites.size() == 1 && importedAsset.sprites[0].layers.size() == 2)
    CHECK(importedAsset.sprites[0].layers[0].obj == Obj::P3 && importedAsset.sprites[0].layers[1].obj == Obj::P12 && importedAsset.sprites[0].layers[1].bits[0] == playerLayer.bits[0]);
  // DASM labels: P12 is the player 12, P1b a second P1 layer
  many.layers[1].obj = Obj::P1;
  many.layers.push_back(many.layers[1]);
  many.layers[2].phase = 1;
  dasmText = exportDasm(many);
  CHECK(dasmText.find("hero_P12_Frame0") != std::string::npos && dasmText.find("hero_P1_Frame0") != std::string::npos && dasmText.find("hero_P1b_Frame0") != std::string::npos);
  importedAsset = Asset();
  CHECK(importText(dasmText, &importedAsset, &what) && importedAsset.sprites.size() == 1 && importedAsset.sprites[0].layers.size() == 3);
  if (importedAsset.sprites.size() == 1 && importedAsset.sprites[0].layers.size() == 3)
    CHECK(importedAsset.sprites[0].layers[0].obj == Obj::P12 && importedAsset.sprites[0].layers[1].obj == Obj::P1 && importedAsset.sprites[0].layers[2].obj == Obj::P1 &&
          importedAsset.sprites[0].layers[2].phase == 1);
  // multisprite: one colour per player, missiles one line high
  Profile multispriteProfile = profileFromBasic("set kernel multisprite\n");
  CHECK(multispriteProfile.players() == 6 && !multispriteProfile.rowColours(Obj::P1) && multispriteProfile.pfMirrorOnly() && multispriteProfile.pfColumns() == 16);
  many.layers = {playerLayer, playerLayer};
  many.layers[1].obj = Obj::P4;
  basicText = exportBasic(many, multispriteProfile);
  CHECK(basicText.find("  COLUP4 = ") != std::string::npos && basicText.find("player4color") == std::string::npos);
  many.layers[1].obj = Obj::P1;
  CHECK(exportBasic(many, multispriteProfile).find("  _COLUP1 = ") != std::string::npos);
  // playfields: DASM tables and batari Basic blocks both come back
  Playfield secondPlayfield;
  secondPlayfield.name = "level";
  secondPlayfield.rows.resize(2);
  secondPlayfield.rows[0].mode = PfMode::Repeat;
  secondPlayfield.setPixel(0, 5, true);
  secondPlayfield.setPixel(1, 17, true);
  secondPlayfield.setPixel(1, 33, true);
  secondPlayfield.rows[1].pf = 0x86;
  dasmText = exportDasm(secondPlayfield);
  CHECK(dasmText.find("level_PF0R") != std::string::npos && dasmText.find("level_COLUPF_ROWS") != std::string::npos);
  importedAsset = Asset();
  CHECK(importText(dasmText, &importedAsset, &what) && importedAsset.playfields.size() == 1);
  if (importedAsset.playfields.size() == 1) {
    auto& importedPlayfield = importedAsset.playfields[0];
    bool same = importedPlayfield.rows.size() == 2;
    for (int row = 0; row < 2 && same; row++)
      for (int column = 0; column < 40; column++) same &= importedPlayfield.pixel(row, column) == secondPlayfield.pixel(row, column);
    CHECK(same && importedPlayfield.name == "level" && importedPlayfield.pfAt(1) == 0x86 && importedPlayfield.rows[1].asymmetric && !importedPlayfield.rows[0].asymmetric);
  }
  basicText = exportBasic(secondPlayfield, ownProfile);  // a kernel of one's own copied as batari Basic: the standard kernel with what it needs
  CHECK(basicText.find("  playfield:\n  .X...................X..........\n") != std::string::npos && basicText.find("pfcolors:") != std::string::npos);
  importedAsset = Asset();
  CHECK(importText(basicText, &importedAsset, &what) && importedAsset.playfields.size() == 1 && importedAsset.playfields[0].pixel(0, 5) && importedAsset.playfields[0].pixel(1, 33) &&
        importedAsset.playfields[0].pfAt(1) == 0x86);
  auto basicIssues = check(secondPlayfield, standardProfile);
  CHECK(hasIssue(basicIssues, Issue::Warning, "pfres") && hasIssue(basicIssues, Issue::Warning, "pfcolors"));
  CHECK(!hasIssue(check(secondPlayfield, playercolorsProfile), Issue::Warning, "pfcolors") && hasIssue(check(secondPlayfield, playercolorsProfile), Issue::Warning, "pfres = 2"));
  CHECK(!hasIssue(check(secondPlayfield, dpcProfile), Issue::Warning, "rows"));
  secondPlayfield.setPixel(0, 1, true);
  CHECK(hasIssue(check(secondPlayfield, standardProfile), Issue::Warning, "columns"));
  CHECK(hasIssue(check(secondPlayfield, ownProfile), Issue::Info, "CTRLPF"));
  // multisprite playfields: 16 columns from column 5, mirrored
  Playfield multispritePlayfield;
  multispritePlayfield.rows.resize(2);
  multispritePlayfield.setPixel(0, 4, true);
  multispritePlayfield.setPixel(1, 19, true);
  CHECK(!hasIssue(check(multispritePlayfield, multispriteProfile), Issue::Warning, "mirrored") && !hasIssue(check(multispritePlayfield, multispriteProfile), Issue::Warning, "columns"));
  basicText = exportBasic(multispritePlayfield, multispriteProfile);
  CHECK(basicText.find("  playfield:\n  X...............\n  ...............X\nend\n  COLUPF = ") != std::string::npos);
  importedAsset = Asset();
  CHECK(importText(basicText, &importedAsset, &what) && importedAsset.playfields.size() == 1 && importedAsset.playfields[0].pixel(0, 4) && importedAsset.playfields[0].pixel(0, 35) &&
        importedAsset.playfields[0].pixel(1, 20) && !importedAsset.playfields[0].rows[0].asymmetric);
  multispritePlayfield.rows[0].mode = PfMode::Repeat;
  CHECK(hasIssue(check(multispritePlayfield, multispriteProfile), Issue::Warning, "mirrored"));

  // regions of a batari Basic program kept up to date
  {
    Asset art;
    Sprite walk = sprite;
    walk.name = "walk";
    walk.layers = {playerLayer};
    walk.insertFrame(1, 0);
    walk.layers[0].bits[1][0] = 0x18;
    art.sprites = {walk};
    multispritePlayfield.name = "level";
    art.playfields = {multispritePlayfield};
    std::string region = basicRegion("art.vcsgfx", art, 0, standardProfile);
    CHECK(region.find("  rem vcsgfx art.vcsgfx walk frame 1\n") == 0 && region.find("  rem vcsgfx art.vcsgfx walk frame 2\n") != std::string::npos);
    CHECK(region.find("%00011000") != std::string::npos);
    std::string program = " set kernel_options pfcolors\r\nmain\r\n  if a then goto two\r\n  rem vcsgfx art.vcsgfx walk frame 1\r\n  player0:\r\nold\r\nend\r\n"
                       "  rem vcsgfx end\r\n  goto draw\r\ntwo\r\n REM VCSGFX Art.vcsgfx walk frame 2\r\n rem vcsgfx end\r\n"
                       "  rem vcsgfx art.vcsgfx level\r\n  rem vcsgfx end\r\n  rem vcsgfx other.vcsgfx walk\r\nkeep\r\n  rem vcsgfx end\r\ndraw\r\n drawscreen\r\n";
    int updatedCount = 0;
    std::vector<std::string> problems;
    std::string updated = updateBasicRegions(program, "art.vcsgfx", art, standardProfile, &updatedCount, &problems);
    CHECK(updatedCount == 3 && problems.empty());
    CHECK(updated.find("old") == std::string::npos && updated.find("keep\r\n") != std::string::npos && updated.find("drawscreen\r\n") != std::string::npos);
    CHECK(updated.find("  rem vcsgfx art.vcsgfx walk frame 1\r\n  rem walk, frame 1;") != std::string::npos);
    CHECK(updated.find("  %00011000\r\nend\r\n") != std::string::npos);  // frame 2's top row, bottom first: last
    CHECK(updated.find("  playfield:\r\n") != std::string::npos && updated.find("\n\n") == std::string::npos);
    // a second pass changes nothing
    CHECK(updateBasicRegions(updated, "art.vcsgfx", art, standardProfile, &updatedCount, &problems) == updated && updatedCount == 3);
    // problems: an unknown item, a frame too many, no end
    problems.clear();
    std::string bad = "  rem vcsgfx art.vcsgfx nobody\n  rem vcsgfx end\n  rem vcsgfx art.vcsgfx walk frame 3\n  rem vcsgfx end\n  rem vcsgfx art.vcsgfx walk\nx\n";
    CHECK(updateBasicRegions(bad, "art.vcsgfx", art, standardProfile, &updatedCount, &problems) == bad && updatedCount == 0 && problems.size() == 3);
  }
  CHECK(!importText("lda #1\n", &importedAsset, &what) && what.empty());
  // another playfield editor's export: a "; mode:" header, then one .byte line per row
  // (in the first column) or one table per register with left and right halves in turn
  {
    const char* rowsText =
        "; mode: asymmetric mirrored line-height 8\n"
        ".byte $10,$80,$81,$00,$00,$00 ;|X   X       X      X||                    | ( 0)\n"
        ".byte $00,$00,$00,$20,$00,$00 ;|                    ||                  X | ( 1)\n"
        ".byte $00,$40,$00,$00,$02,$00 ;|     X              ||         X          | ( 2)\n";
    const char* regsText =
        "; mode: asymmetric mirrored line-height 8\n\nDataPF0:\n\t.byte $10,$00,$00,$20,$00,$00\n"
        "DataPF1:\n\t.byte $80,$00,$00,$00,$40,$02\nDataPF2:\n\t.byte $81,$00,$00,$00,$00,$00\n";
    for (const char* text : {rowsText, regsText}) {
      Asset imported;
      CHECK(importText(text, &imported, &what) && imported.playfields.size() == 1 && imported.sprites.empty());
      if (imported.playfields.size() != 1) continue;
      const Playfield& importedPlayfield = imported.playfields[0];
      CHECK(importedPlayfield.rows.size() == 3 && importedPlayfield.lineHeight == 8 && importedPlayfield.rows[0].mode == PfMode::Mirror);
      std::string got;
      for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 40; column++) got += importedPlayfield.pixel(row, column) ? 'X' : ' ';
        got += '|';
      }
      CHECK(got == "X   X       X      X                    |"
                   "                                      X |"
                   "     X                       X          |");
      CHECK(importedPlayfield.rows[1].asymmetric && importedPlayfield.rows[2].asymmetric);
    }
    Asset imported;
    CHECK(importText("; mode: symmetric repeat line-height 4\n.byte %00010000,%00000000,%00000001\n.byte $00,$00,$00\n", &imported, &what));
    CHECK(imported.playfields.size() == 1 && imported.playfields[0].rows.size() == 2 && imported.playfields[0].lineHeight == 4 &&
          imported.playfields[0].rows[0].mode == PfMode::Repeat && imported.playfields[0].pixel(0, 0) && imported.playfields[0].pixel(0, 20) &&
          imported.playfields[0].pixel(0, 12) && imported.playfields[0].pixel(0, 32) && !imported.playfields[0].rows[0].asymmetric);
  }

  // animations: steps of frames with their durations
  Sprite walker = sprite;
  walker.name = "walker";
  walker.insertFrame(1, 0);
  walker.insertFrame(2, -1);
  walker.layers[0].bits[1][0] = 0x18;
  walker.layers[0].bits[2][0] = 0x24;
  Animation walkAnimation, idle;
  walkAnimation.name = "walk";
  walkAnimation.steps = {{0, 8}, {1, 4}, {2, 8}, {1, 4}};
  idle.name = "idle";
  idle.loop = false;
  idle.steps = {{2, 10}, {0, 5}};
  walker.animations = {walkAnimation, idle};
  CHECK(walkAnimation.length() == 24 && walker.frameAt(walkAnimation, 0) == 0 && walker.frameAt(walkAnimation, 8) == 1 && walker.frameAt(walkAnimation, 12) == 2 &&
        walker.frameAt(walkAnimation, 23) == 1 && walker.frameAt(walkAnimation, 24) == 0);
  CHECK(walker.frameAt(idle, 100) == 0 && walker.frameAt(idle, 3) == 2);
  {
    Sprite walkerCopy = walker;
    walkerCopy.insertFrame(0, -1);
    CHECK(walkerCopy.frames == 4 && walkerCopy.animations[0].steps[0].frame == 1 && walkerCopy.animations[0].steps[1].frame == 2);
    walkerCopy.removeFrame(2);  // the old frame 1: its steps go
    CHECK(walkerCopy.animations[0].steps.size() == 2 && walkerCopy.animations[0].steps[0].frame == 1 && walkerCopy.animations[0].steps[1].frame == 2);
    walkerCopy = walker;
    walkerCopy.swapFrames(0, 2);
    CHECK(walkerCopy.animations[0].steps[0].frame == 2 && walkerCopy.animations[0].steps[2].frame == 0 && walkerCopy.layers[0].bits[0][0] == 0x24);
  }
  {
    Asset walkerAsset, decodedAsset;
    walkerAsset.sprites = {walker};
    walkerAsset.sprites[0].animations[0].basicTimer = "y";
    walkerAsset.sprites[0].animations[0].basicStep = "z";
    CHECK(fromJson(toJson(walkerAsset), &decodedAsset, &error) && decodedAsset.sprites.size() == 1 && decodedAsset.sprites[0].animations.size() == 2);
    CHECK(!decodedAsset.sprites[0].animations[1].loop && decodedAsset.sprites[0].animations[0].steps[1].ticks == 4 && decodedAsset.sprites[0].animations[0].basicStep == "z");
    CHECK(toJson(decodedAsset) == toJson(walkerAsset));
  }
  dasmText = exportDasm(walker);
  CHECK(dasmText.find("walker_P0_FramePtrLo\n        .byte <walker_P0_Frame0,<walker_P0_Frame1,<walker_P0_Frame2\n") != std::string::npos);
  CHECK(dasmText.find("walker_walk_FRAME\n        .byte 0,1,2,1\nwalker_walk_TICKS\n        .byte 8,4,8,4\n") != std::string::npos &&
        dasmText.find("walker_idle_LOOP = 0") != std::string::npos && dasmText.find("walker_FRAMES = 3") != std::string::npos);
  importedAsset = Asset();
  CHECK(importText(dasmText, &importedAsset, &what) && importedAsset.sprites.size() == 1);
  if (importedAsset.sprites.size() == 1) {
    auto& importedSprite = importedAsset.sprites[0];
    CHECK(importedSprite.frames == 3 && importedSprite.layers.size() == 2 && importedSprite.animations.size() == 2);
    for (auto& animation : importedSprite.animations) {
      if (animation.name == "walk") CHECK(animation.loop && animation.steps.size() == 4 && animation.steps[2].frame == 2 && animation.steps[3].ticks == 4);
      else CHECK(animation.name == "idle" && !animation.loop && animation.steps.size() == 2 && animation.steps[0].ticks == 10);
    }
  }
  // batari Basic: a subroutine that plays it
  CHECK(hasIssue(check(walker, standardProfile), Issue::Warning, "walk") && !hasIssue(check(walker, ownProfile), Issue::Warning, "variables"));
  walker.animations[0].basicTimer = "y";
  walker.animations[0].basicStep = "z";
  basicText = exportBasicAnimation(walker, walker.animations[0], standardProfile);
  CHECK(basicText.find("  dim walker_walk_t = y\n  dim walker_walk_s = z\n  data walker_walk_ticks\n  8, 4, 8, 4\nend\n") != std::string::npos);
  CHECK(basicText.find("  on walker_walk_s goto walker_walk_f0 walker_walk_f1 walker_walk_f2 walker_walk_f1\n") != std::string::npos);
  CHECK(basicText.find("\nwalker_walk_f2\n") != std::string::npos && basicText.find("  if walker_walk_s >= 4 then walker_walk_s = 0\n") != std::string::npos);
  CHECK(exportBasicAnimation(walker, walker.animations[1], standardProfile).find("then walker_idle_s = 1\n") != std::string::npos);
  {
    Asset walkerAsset;
    walkerAsset.sprites = {walker};
    std::string region = basicRegion("w.vcsgfx", walkerAsset, 0, standardProfile);
    CHECK(region.find("  rem vcsgfx w.vcsgfx walker animation walk\n") == 0 && region.find("  rem vcsgfx w.vcsgfx walker animation idle\n") != std::string::npos);
    int updatedCount = 0;
    std::vector<std::string> problems;
    std::string program = "  rem vcsgfx w.vcsgfx walker animation walk\n  rem vcsgfx end\n  rem vcsgfx w.vcsgfx walker animation run\n  rem vcsgfx end\n";
    std::string updated = updateBasicRegions(program, "w.vcsgfx", walkerAsset, standardProfile, &updatedCount, &problems);
    CHECK(updatedCount == 1 && problems.size() == 1 && updated.find("walker_walk_show") != std::string::npos && updated.find("animation run\n  rem vcsgfx end") != std::string::npos);
  }

  // C: arrays top row first, the animations as steps, one header per .vcsgfx
  Asset cAsset;
  cAsset.sprites = {sprite, walker};
  cAsset.playfields = {playfield, secondPlayfield};
  std::string cHeader = cInclude(cAsset, "my gfx.vcsgfx");
  CHECK(cHeader.find("from my gfx.vcsgfx") != std::string::npos && cHeader.find("#ifndef MY_GFX_VCSGFX_H") != std::string::npos);
  CHECK(cHeader.find("#define HERO_FRAMES") != std::string::npos && cHeader.find("static const uint8_t hero_P0_gfx[HERO_FRAMES][") != std::string::npos);
  CHECK(cHeader.find("static const uint8_t walker_P0_gfx[WALKER_FRAMES]") != std::string::npos);
  CHECK(cHeader.find("static const vcsgfx_step walker_walk_steps[] = {") != std::string::npos);
  CHECK(cHeader.find("static const uint8_t level_pf0r[LEVEL_ROWS]") != std::string::npos);  // asymmetric rows
  CHECK(exportC(walker).find("0x18,  /* ...XX... ") != std::string::npos);
  CHECK(exportC(sprite).find("vcsgfx_animate") == std::string::npos && cHeader.find("vcsgfx_animate") != std::string::npos);
  Profile elfProfile;
  elfProfile.usesC = true;
  CHECK(elfProfile.describe().find("C") == 0 && !elfProfile.basic());

  // the exported tables assemble (when DASM is installed); the C header compiles (with a C compiler)
  namespace fs = std::filesystem;
  std::string cCompiler = findInPath("cc");
  if (!cCompiler.empty()) {
    fs::path dir = fs::temp_directory_path() / "vcside-gfx-test";
    fs::create_directories(dir);
    Asset other;
    other.sprites = {walker};
    other.sprites[0].name = "other";
    std::ofstream(dir / "a.gfx.h") << cHeader;
    std::ofstream(dir / "b.gfx.h") << cInclude(other, "b.vcsgfx");
    std::ofstream(dir / "g.c") << "#include \"a.gfx.h\"\n#include \"a.gfx.h\"\n#include \"b.gfx.h\"\n"
                                  "int f(void) { vcsgfx_player p = {0}; vcsgfx_play(&p, &walker_walk);\n"
                                  "  return vcsgfx_animate(&p) + hero_P0_gfx[0][0] + level_pf1[0] + other_P0_gfx[0][0]; }\n";
    std::string command = "\"" + cCompiler + "\" -std=c11 -Wall -Wextra -Werror -fsyntax-only \"" + (dir / "g.c").string() + "\" > \"" +
                      (dir / "c.log").string() + "\" 2>&1";
    int returnCode = std::system(command.c_str());
    CHECK(returnCode == 0);
    if (returnCode != 0) {
      std::ifstream log(dir / "c.log");
      std::fputs(std::string(std::istreambuf_iterator<char>(log), {}).c_str(), stderr);
    }
  }
  std::string dasm = findInPath("dasm");
  if (!dasm.empty()) {
    fs::path dir = fs::temp_directory_path() / "vcside-gfx-test";
    fs::create_directories(dir);
    std::ofstream(dir / "g.asm") << "    processor 6502\n    org $F000\n" << exportDasm(sprite) << exportDasm(secondPlayfield) << exportDasm(playfield) << exportDasm(walker)
                                 << "    org $FFFC\n    .word $F000\n    .word $F000\n";
    std::string command = "\"" + dasm + "\" \"" + (dir / "g.asm").string() + "\" -f3 -o\"" + (dir / "g.bin").string() + "\" > \"" +
                      (dir / "g.log").string() + "\" 2>&1";
    int returnCode = std::system(command.c_str());
    CHECK(returnCode == 0);
    if (returnCode != 0) {
      std::ifstream log(dir / "g.log");
      std::fputs(std::string(std::istreambuf_iterator<char>(log), {}).c_str(), stderr);
    }
  }
}

// The template's 2600.h: the register names of the Stella Programmer's Guide, plus TIMINT
// for $285, which the guide leaves unnamed; and an empty DASM project assembles with it.
static void testHeader(const KnowledgeBase& kb, const std::string& data) {
  namespace fs = std::filesystem;
  std::ifstream headerFile(data + "/templates/dasm/2600.h");
  std::string text((std::istreambuf_iterator<char>(headerFile)), {});
  CHECK(!text.empty());
  std::set<std::string> header, guide;
  std::istringstream stream(text);
  for (std::string line; std::getline(stream, line);) {
    std::smatch match;
    static const std::regex label(R"(^([A-Z][A-Z0-9_]*)\s+ds\s+1\b)");
    if (std::regex_search(line, match, label)) header.insert(match[1].str());
  }
  for (auto& registerEntry : kb.registers())  // registers, not areas such as RAM ($80-$FF)
    if (std::find(registerEntry.sources.begin(), registerEntry.sources.end(), "stella-guide") != registerEntry.sources.end() && registerEntry.address.find('-') == std::string::npos)
      guide.insert(registerEntry.name);
  guide.insert("TIMINT");
  CHECK(header.size() == 69 && header == guide);
  for (auto& name : header)
    if (!guide.count(name)) std::fprintf(stderr, "2600.h: %s is not a Stella guide name\n", name.c_str());
  for (auto& name : guide)
    if (!header.count(name)) std::fprintf(stderr, "2600.h: %s is missing\n", name.c_str());
  CHECK(text.find("TIA_BASE") == std::string::npos);
  // a new DASM project
  fs::path dir = fs::temp_directory_path() / "vcside-test-2600h";
  std::error_code errorCode;
  fs::remove_all(dir, errorCode);
  Project project;
  std::string error;
  CHECK(Project::create(dir.string(), "plain", Language::Dasm, "NTSC", &project, &error));
  CHECK(fs::exists(dir / "2600.h") && !fs::exists(dir / "hardware.h"));
  std::string dasm = findInPath("dasm");
  if (!dasm.empty()) {
    std::string command = "cd \"" + dir.string() + "\" && \"" + dasm + "\" main.asm -f3 -oplain.bin > dasm.log 2>&1";
    CHECK(std::system(command.c_str()) == 0 && fs::file_size(dir / "plain.bin", errorCode) == 4096);
  }
  fs::remove_all(dir, errorCode);
}

static void testThemes(const std::string& data) {
  using namespace vcs::theme;
  namespace fs = std::filesystem;
  Rgba parsedColour = 0;
  CHECK(parseColour("#268BD2", &parsedColour) && parsedColour == 0x268BD2FF && parseColour("#268bd240", &parsedColour) && parsedColour == 0x268BD240);
  CHECK(!parseColour("268BD2", &parsedColour) && !parseColour("#12345", &parsedColour) && !parseColour("#GG0000", &parsedColour));
  CHECK(colourText(0x268BD2FF) == "#268BD2" && colourText(0x268BD240) == "#268BD240");
  Role role;
  CHECK(roleFromId("syntax.comment", &role) && role == Role::SynComment && !roleFromId("syntax.nope", &role));

  // the built-in themes: every colour, nothing that cannot be read
  fs::path user = fs::temp_directory_path() / "vcside-test-themes";
  std::error_code errorCode;
  fs::remove_all(user, errorCode);
  Library library;
  library.load(data + "/themes", user.string());
  for (const char* id : {"dark", "light", "solarized-light", "solarized-dark"}) {
    const Theme* theme = library.find(id);
    CHECK(theme && theme->builtIn && !theme->name.empty());
    Resolved resolved = library.resolve(id);
    for (auto& problem : resolved.problems) std::fprintf(stderr, "theme %s: %s\n", id, problem.c_str());
    CHECK(resolved.problems.empty() && !resolved.indent.empty());
  }
  // Solarized Dark is Solarized Light with the palette turned round
  Resolved solarizedLight = library.resolve("solarized-light"), solarizedDark = library.resolve("solarized-dark");
  CHECK(solarizedLight.colour[(int)Role::EdBackground] == 0xFDF6E3FF && solarizedDark.colour[(int)Role::EdBackground] == 0x002B36FF);
  CHECK(solarizedLight.colour[(int)Role::SynMnemonic] == solarizedDark.colour[(int)Role::SynMnemonic]);  // the accents stay
  CHECK(solarizedLight.colour[(int)Role::UiSelection] == 0x268BD240);                         // "$blue/40"
  CHECK(library.resolve("dark").colour[(int)Role::SynSymbol] == library.resolve("dark").colour[(int)Role::EdText]);  // "@editor.text"

  // a theme of one's own: a base, a palette entry, references, alpha, a loop
  Theme mine;
  std::string error;
  CHECK(fromJson(R"({"vcstheme":1,"name":"Mine","base":"solarized-light","palette":{"blue":"#FF0000","paper":"#FFFFFF"},
                      "colours":{"editor.background":"$paper","ui.accent":"@editor.background/80"},"indent":[]})", &mine, &error));
  CHECK(library.save(mine, &error) && mine.id == "mine" && fs::exists(user / "mine.vcstheme"));
  Resolved mineResolved = library.resolve("mine");
  CHECK(mineResolved.problems.empty() && mineResolved.colour[(int)Role::EdBackground] == 0xFFFFFFFF && mineResolved.colour[(int)Role::UiAccent] == 0xFFFFFF80);
  // A full copy with no base, missing colours added later: they come from Dark or Light.
  Theme old;
  CHECK(fromJson(R"({"vcstheme":1,"name":"Old","colours":{"ui.background":"#FAFAFA"}})", &old, &error) && library.save(old, &error));
  Resolved oldResolved = library.resolve(old.id), light = library.resolve("light");
  CHECK(oldResolved.problems.empty() && oldResolved.notes.size() == 1 && oldResolved.colour[(int)Role::FilesAsm] == light.colour[(int)Role::FilesAsm]);
  CHECK(library.remove(old.id, &error));
  for (const char* themeId : {"dark", "light", "solarized-light", "solarized-dark"}) CHECK(library.resolve(themeId).notes.empty() && library.resolve(themeId).problems.empty());
  CHECK(mineResolved.colour[(int)Role::SynMnemonic] == 0xFF0000FF);  // the base's "$blue" is now this palette's
  CHECK(mineResolved.indent.empty());                                // an empty list: no shading
  CHECK(library.resolveSpec("mine", "$blue/20", &parsedColour) && parsedColour == 0xFF000020 && !library.resolveSpec("mine", "$nope", &parsedColour));
  Theme loop;
  CHECK(fromJson(R"({"vcstheme":1,"name":"Loop","base":"mine","colours":{"ui.text":"@ui.textDim","ui.textDim":"@ui.text","editor.nope":"#000"}})", &loop, &error));
  CHECK(library.save(loop, &error));
  Resolved loopResolved = library.resolve("loop");
  CHECK(loopResolved.problems.size() == 3 && loopResolved.colour[(int)Role::UiText] == 0xFF00FFFF);  // two that refer to each other, one unknown
  CHECK(!fromJson("{\"name\":\"x\"}", &loop, &error) && !error.empty());

  // sharing: an exported theme carries its own bases up to a built-in one
  Theme child;
  CHECK(fromJson(R"({"vcstheme":1,"name":"Child","base":"mine","colours":{"ui.text":"#010203"}})", &child, &error) && library.save(child, &error));
  Theme exportedTheme;
  CHECK(fromJson(library.exportJson("child"), &exportedTheme, &error));
  CHECK(exportedTheme.base == "solarized-light" && exportedTheme.palette["blue"] == "#FF0000" && exportedTheme.colours["ui.text"] == "#010203" &&
        exportedTheme.colours["editor.background"] == "$paper" && exportedTheme.hasIndent && exportedTheme.indent.empty());
  // importing never overwrites: the same file twice gives two themes
  std::ofstream(user / "shared.vcstheme.tmp") << library.exportJson("child");
  fs::path file = fs::temp_directory_path() / "Child.vcstheme";
  fs::copy_file(user / "shared.vcstheme.tmp", file, fs::copy_options::overwrite_existing, errorCode);
  std::string id1, id2;
  CHECK(library.import(file.string(), &id1, &error) && library.import(file.string(), &id2, &error) && id1 != id2 && id1 != "child");
  Resolved imported = library.resolve(id1);
  CHECK(imported.problems.empty() && imported.colour[(int)Role::UiText] == 0x010203FF && imported.colour[(int)Role::EdBackground] == 0xFFFFFFFF);
  // a user theme with a built-in one's id is not possible through save, and built-in ones cannot be deleted
  CHECK(!library.remove("dark", &error) && library.remove(id2, &error) && !library.find(id2));
  CHECK(library.uniqueId("Solarized Light") == "solarized-light-2" && library.uniqueId("  ") == "theme");
  CHECK(library.uniqueId("Claro, mío") == "claro-mio" && library.uniqueId("Año ñu — 2") == "ano-nu-2");
  // what the user folder holds is read back
  Library again;
  again.load(data + "/themes", user.string());
  CHECK(again.find("mine") && !again.find("mine")->builtIn && again.find(id1));
  fs::remove_all(user, errorCode);
  fs::remove(file, errorCode);
}

int main() {
  KnowledgeBase kb;
  std::string error;
  // VCSIDE_TEST_DATA: the data folder when the tests run elsewhere (e.g. a Windows build in Wine)
  const char* env = std::getenv("VCSIDE_TEST_DATA");
  const std::string data = env && *env ? env : VCSIDE_SOURCE_DATA;
  kb.load(data + "/kb", &error);
  if (!error.empty()) std::fprintf(stderr, "knowledge base: %s", error.c_str());
  CHECK(error.empty());
  testKb(kb);
  testIndentation();
  testCycles(kb);
  testParsers();
  testSourceSymbols();
  testEditor();
  testSpace(kb);
  testDebugMap();
  loadLanguages(data + "/i18n");
  testEmulatorOptions(kb);
  testElfSpace();
  testBasic();
  testBasicSyntax(kb);
  testFormatOrder(kb);
  testGfx();
  testFolding();
  testSourceLinks();
  testThemes(data);
  testHeader(kb, data);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
