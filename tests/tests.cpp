// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Tests without GUI: knowledge base, cycle counter, output parsers.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "builder_parse.h"
#include "editor/text_editor.h"
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
  const Opcode* o = kb.opcode("lda", "absy");
  CHECK(o && o->code == 0xB9 && o->cycles == 4 && o->pagePenalty);
  o = kb.opcode("STA", "absx");
  CHECK(o && o->cycles == 5 && !o->pagePenalty);
  o = kb.opcode("DCP", "zp");
  CHECK(o && o->illegal && o->cycles == 5);
  CHECK(kb.opcode("NOP", "imp") && !kb.opcode("NOP", "imp")->illegal);
  // DASM cannot assemble JAM; LAS is a stable illegal opcode; every entry cites known sources.
  CHECK(kb.instruction("JAM") && !kb.instruction("JAM")->opcodes.empty() && !kb.instruction("JAM")->opcodes[0]->dasm);
  CHECK(kb.opcode("LAS", "absy") && kb.opcode("LAS", "absy")->stable);
  CHECK(kb.reg("WSYNC") && !kb.reg("WSYNC")->sources.empty());
  for (auto& r : kb.registers())
    for (auto& id : r.sources) CHECK(kb.source(id) != nullptr);
  for (auto& f : kb.formats())
    for (auto& id : f.sources) CHECK(kb.source(id) != nullptr);
  for (auto& [mn, in] : kb.instructions())
    for (auto& id : in.sources) CHECK(kb.source(id) != nullptr);
  auto c = kb.complete("ST");
  bool sta = false;
  for (auto& e : c) sta |= e.word == "STA";
  CHECK(sta);
}

static void testCycles(const KnowledgeBase& kb) {
  std::map<std::string, int> syms = {{"WSYNC", 0x02}, {"RESP0", 0x10}, {"Ptr", 0x80}, {"Table", 0xF100}, {"SWCHA", 0x280}};
  SymbolLookup look = [&](const std::string& n) -> std::optional<int> {
    auto it = syms.find(n);
    if (it == syms.end()) return std::nullopt;
    return it->second;
  };
  auto line = [&](const char* s) { return countLine(kb, s, look); };

  LineCycles l = line("\tsta WSYNC");
  CHECK(l.known && l.mode == "zp" && l.min == 3 && l.max == 3 && l.bytes == 2);
  l = line("\tsta.w RESP0   ; 4 cycles");
  CHECK(l.known && l.mode == "abs" && l.min == 4);
  l = line("Loop\tlda Table,y");
  CHECK(l.known && l.mode == "absy" && l.min == 4 && l.max == 5);
  l = line("\tlda (Ptr),y");
  CHECK(l.mode == "indy" && l.min == 5 && l.max == 6);
  l = line("\tlda SWCHA");
  CHECK(l.mode == "abs" && l.min == 4);
  l = line("\tbne Loop");
  CHECK(l.mode == "rel" && l.min == 2 && l.max == 4);
  l = line("\tasl");
  CHECK(l.mode == "acc" && l.min == 2);
  l = line("\tlsr A");
  CHECK(l.mode == "acc");
  l = line("\tldx Unknown,y");
  CHECK(l.guessed && l.mode == "absy");
  l = line("\tSLEEP 7");
  CHECK(l.known && l.min == 7 && l.max == 7);
  l = line("\tjmp (Ptr)");
  CHECK(l.mode == "ind" && l.min == 5);
  l = line("\tsta Ptr+1,x");
  CHECK(l.mode == "zpx" && l.min == 4);
  l = line("\tlda #<Table");
  CHECK(l.mode == "imm" && l.min == 2);
  l = line("\torg $F000");
  CHECK(!l.isCode);
  l = line("Label");
  CHECK(!l.isCode);
  l = line("\tdc.b \"a;b\" ; comment");
  CHECK(!l.isCode);

  BlockCycles b = countBlock(kb, {"\tsta WSYNC", "\tlda Table,y", "\tsta RESP0", "\tdey", "\tbne Loop"}, look);
  CHECK(b.instructions == 5 && b.min == 3 + 4 + 3 + 2 + 2 && b.max == 3 + 5 + 3 + 2 + 4);

  CHECK(evalExpr("$10+2", look) == 0x12);
  CHECK(evalExpr(">Table", look) == 0xF1);
  CHECK(evalExpr("%1010", look) == 10);
  CHECK(!evalExpr("Nope", look));
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
  auto find = [&](const std::string& n) -> const SourceSymbol* {
    for (auto& x : syms)
      if (x.name == n) return &x;
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
  auto d = parseDasmOutput(
      "--- Unresolved Symbol List\nUnknown                  0000 ????         (R )\n--- 1 Unresolved Symbol \n\n"
      "t.asm (4): error: Unknown Mnemonic 'foo'.\n\nFatal assembly error: Source is not resolvable.\n");
  CHECK(d.size() == 3);
  CHECK(d.size() == 3 && d[1].file == "t.asm" && d[1].line == 4 && d[1].severity == Diagnostic::Error);
  CHECK(d.size() == 3 && d[0].message.find("Unknown") != std::string::npos);

  auto g = parseGccOutput("src/main.c:12:5: warning: unused variable 'x'\nsrc/a.c:3: error: boom\n");
  CHECK(g.size() == 2 && g[0].line == 12 && g[0].severity == Diagnostic::Warning && g[1].file == "src/a.c");
  // the ELF template's checks, ld and make
  g = parseGccOutput(
      "src/main.c: error: undefined symbol 'foo' (only vcsLib functions may be left undefined)\n"
      "arm-none-eabi-gcc: error: nothing.c: No such file or directory\n"
      "/opt/bin/arm-none-eabi-ld: warning: build/x-chameleon.elf has a LOAD segment with RWX permissions\n"
      "/opt/bin/arm-none-eabi-ld: build/elf/main.o: in function `elf_main':\n"
      "main.c:(.text+0x1c): undefined reference to `bar'\n"
      "/opt/bin/arm-none-eabi-ld: region `RAM' overflowed by 120 bytes\n"
      "make: *** [build/x.bin] Error 1\n"
      "make: *** No rule to make target 'src/y.c', needed by 'build/elf/y.o'.  Stop.\n");
  CHECK(g.size() == 5);
  if (g.size() == 5) {
    CHECK(g[0].file == "src/main.c" && g[0].line == 0 && g[0].message.find("'foo'") != std::string::npos);
    CHECK(g[1].file.empty() && g[1].message.rfind("arm-none-eabi-gcc: ", 0) == 0);
    CHECK(g[2].file == "main.c" && g[2].message == "undefined reference to `bar' (in elf_main)");
    CHECK(g[3].file.empty() && g[3].message == "region `RAM' overflowed by 120 bytes");
    CHECK(g[4].message.rfind("make: No rule", 0) == 0);
  }

  auto s = parseDasmSymbols("--- Symbol List (sorted by symbol)\nStart                    f000              (R )\nVAR                      0081              (R )\n--- End of Symbol List.\n");
  CHECK(s.size() == 2 && s["Start"] == 0xF000 && s["VAR"] == 0x81);
}

static void testEditor() {
  // search
  std::vector<std::string> lines = {"\tlda Frame", "\tsta frame+1 ; frame", "FrameLoop dex"};
  CHECK(findMatches(lines, "frame", {}).size() == 4);
  CHECK(findMatches(lines, "frame", {true, false}).size() == 2);
  CHECK(findMatches(lines, "Frame", {false, true}).size() == 3);  // not FrameLoop
  CHECK(findMatches(lines, "", {}).empty());

  TextEditor ed;
  ed.setText("\tlda #1\n\tsta COLUBK\n\tdex");
  ed.selectLines(0, 1);
  ed.toggleComment();
  CHECK(ed.lines()[0] == ";\tlda #1" && ed.lines()[1] == ";\tsta COLUBK" && ed.lines()[2] == "\tdex");
  ed.toggleComment();
  CHECK(ed.lines()[0] == "\tlda #1" && ed.lines()[1] == "\tsta COLUBK");
  ed.undo();
  CHECK(ed.lines()[0] == ";\tlda #1");
  ed.undo();
  CHECK(ed.lines()[0] == "\tlda #1");

  ed.goTo(2);
  ed.moveLines(-1);
  CHECK(ed.lines()[1] == "\tdex" && ed.lines()[2] == "\tsta COLUBK" && ed.cursor().line == 1);
  ed.duplicateLines();
  CHECK(ed.lines().size() == 4 && ed.lines()[2] == "\tdex" && ed.cursor().line == 2);
  ed.deleteLines();
  CHECK(ed.lines().size() == 3);

  TextEditor bas;
  bas.setCommentPrefix("rem");
  bas.setText("  COLUBK = $84");
  bas.toggleComment();
  CHECK(bas.lines()[0] == "  rem COLUBK = $84");
  bas.toggleComment();
  CHECK(bas.lines()[0] == "  COLUBK = $84");

  TextEditor fn;
  fn.setText("a\n\tsta WSYNC\nb\n\tsta WSYNC");
  fn.openFind(false);
  fn.setFindQuery("WSYNC");
  fn.findNext();
  CHECK(fn.hasSelection() && fn.cursor().line == 1);
  fn.findNext();
  CHECK(fn.cursor().line == 3);
  fn.findNext();
  CHECK(fn.cursor().line == 1);  // wraps
  fn.findNext(true);
  CHECK(fn.cursor().line == 3);  // backwards wraps

  TextEditor r;
  r.setText("lda Frame\nsta Frame");
  r.openFind(true);
  r.setFindQuery("Frame");
  r.setReplaceText("Tick");
  CHECK(r.replaceAll() == 2 && r.text() == "lda Tick\nsta Tick");
  r.undo();
  CHECK(r.text() == "lda Frame\nsta Frame");
}

static std::string ras(const std::vector<std::pair<long, long>>& blocks) {
  std::string d;
  for (auto [org, len] : blocks) {
    d += char(org & 0xFF), d += char(org >> 8), d += char(len & 0xFF), d += char(len >> 8);
    d += std::string(len, '\xEA');
  }
  return d;
}

static bool hasDiag(const SpaceReport& r, Diagnostic::Severity s, const std::string& part) {
  for (auto& d : r.diagnostics)
    if (d.severity == s && d.message.find(part) != std::string::npos) return true;
  return false;
}

static void testSpace(const KnowledgeBase& kb) {
  auto rr = parseRange("$1FF8-$1FF9");
  CHECK(rr && rr->lo == 0x1FF8 && rr->hi == 0x1FFA);
  CHECK(parseRange("$1FF0") && parseRange("$1FF0")->size() == 1);
  CHECK(!parseRange("1FF8"));
  auto blocks = parseDasmRas(ras({{0x0000, 104}, {0x0FFC, 9}, {0x1FFC, 4}}));
  CHECK(blocks.size() == 3 && blocks[1].lo == 0x0FFC && blocks[1].hi == 0x1005);

  // Every format with a "space" object is consistent.
  int withSpace = 0;
  for (auto& f : kb.formats()) {
    if (!f.space.known) continue;
    withSpace++;
    CHECK(f.space.bankSize > 0);
    for (long s : f.space.romSizes) CHECK(s % f.space.bankSize == 0);
    CHECK(f.space.ramWindows.empty() || f.space.ramBytes > 0);
  }
  CHECK(withSpace >= 17);
  const CartFormat* f8 = kb.format("F8");
  const CartFormat* f8sc = kb.format("F8SC");
  CHECK(f8 && f8sc && f8sc->space.ramBytes == 128);

  // The listing of a small F8 program with Superchip RAM (DASM 2.20).
  const std::string lst =
      "------- FILE f8.asm LEVEL 1 PASS 1\n"
      "      1  0000\t\t\t\t      processor\t6502\n"
      "      4 U0080\t\t       00 00 00    Var1       ds\t3\n"
      "      5 U0083\t\t       00 00 00 00*Var2       ds\tSIZE\n"
      "      8 U1000\t\t       00 00 00 00*ScW\t      ds\t20\n"
      "     12  0000\t\t       a9 01\t   B0\t      lda\t#1\n"
      "     14  0004\t\t       ea ea ea ea*\t      ds\t100, $EA\n"
      "------- FILE inc.h LEVEL 2 PASS 1\n"
      "      3  1ff6\t\t       a9 02\t   B1\t      lda\t#2\n";
  std::vector<std::string> src(14);
  src[3] = "Var1       ds 3";
  src[4] = "Var2       ds SIZE";
  src[7] = "ScW\tds 20";
  SymbolLookup sym = [](const std::string& s) -> std::optional<int> {
    if (s == "SIZE") return 10;
    return std::nullopt;
  };
  auto ram = parseDasmListingRam(lst, [&](const std::string& f) { return f == "f8.asm" ? src : std::vector<std::string>{}; }, sym);
  CHECK(ram.size() == 3);
  CHECK(ram.size() == 3 && ram[1].label == "Var2" && ram[1].size == 10 && ram[2].addr == 0x1000 && ram[2].size == 20);
  auto code = parseDasmListingCode(lst);
  CHECK(code.size() == 3 && code.back().file == "inc.h" && code.back().addr == 0x1FF6);

  SpaceOptions opt;
  // F8, two banks: bank 0 with 104+4 bytes, bank 1 with the vectors and code at $1FF6 (hotspot $1FF8).
  auto rep = analyzeSpace(parseDasmRas(ras({{0x0000, 104}, {0x0FFC, 4}, {0x1FF6, 10}})), 8192, ram, code, f8->space, opt);
  CHECK(rep.banks.size() == 2 && rep.banks[0].used == 108 && rep.banks[1].used == 10);
  CHECK(rep.ram.measured && rep.ram.used == 13 && rep.stackRoom == 128 - 13);
  CHECK(!rep.cartRam);
  CHECK(hasDiag(rep, Diagnostic::Warning, "$1FF8"));
  bool pointed = false;
  for (auto& d : rep.diagnostics) pointed |= d.file == "inc.h" && d.line == 3;
  CHECK(pointed);
  // F8SC: the 20 bytes declared at $1000 are cartridge RAM.
  rep = analyzeSpace(parseDasmRas(ras({{0x0100, 50}, {0x1100, 50}})), 8192, ram, code, f8sc->space, opt);
  CHECK(rep.cartRam && rep.cartRam->measured && rep.cartRam->used == 20 && rep.cartRam->size == 128);
  CHECK(rep.diagnostics.empty());
  CHECK(code.size() == 3 && !code[0].fill && code[1].fill);
  // Padding over the Superchip ports is fine; code there is not.
  std::vector<CodeLine> pad = {{"m.asm", 5, 0x0000, true}, {"m.asm", 6, 0x0100, false}};
  rep = analyzeSpace(parseDasmRas(ras({{0x0000, 300}})), 8192, {}, pad, f8sc->space, opt);
  CHECK(rep.diagnostics.empty());
  pad[0].fill = false;
  rep = analyzeSpace(parseDasmRas(ras({{0x0000, 300}})), 8192, {}, pad, f8sc->space, opt);
  CHECK(hasDiag(rep, Diagnostic::Warning, "$1000") && rep.diagnostics[0].line == 5);
  // Wrong size: 12K for F8 is an overflow; 4K is a warning.
  rep = analyzeSpace(parseDasmRas(ras({{0x0000, 12288}})), 12288, {}, {}, f8->space, opt);
  CHECK(hasDiag(rep, Diagnostic::Error, "12288"));
  CHECK(!rep.ram.measured);
  rep = analyzeSpace(parseDasmRas(ras({{0x0000, 100}})), 4096, {}, {}, f8->space, opt);
  CHECK(hasDiag(rep, Diagnostic::Warning, "4096"));
  // A full bank and one at 95 %.
  rep = analyzeSpace(parseDasmRas(ras({{0x0000, 0x0FF8}, {0x0FFA, 6}, {0x1000, 3900}, {0x1FFA, 6}})), 8192, {}, {}, f8->space, opt);
  CHECK(rep.banks[0].used == 4094 && rep.banks[1].used == 3906);
  CHECK(hasDiag(rep, Diagnostic::Warning, "Bank 0 is 100 %") && hasDiag(rep, Diagnostic::Warning, "Bank 1 is 96 %"));
  // 2600 RAM: little room for the stack, and a variable past $FF.
  std::vector<RamDecl> big = {{"a.asm", 4, 0x80, 124, "Buf"}};
  rep = analyzeSpace({}, 0, big, {}, f8->space, opt);
  CHECK(rep.stackRoom == 4 && hasDiag(rep, Diagnostic::Warning, "stack"));
  big[0].size = 130;
  rep = analyzeSpace({}, 0, big, {}, f8->space, opt);
  CHECK(hasDiag(rep, Diagnostic::Error, "Buf") && rep.diagnostics[0].line == 4);
  // E7's RAM cannot be measured; CV's can.
  const CartFormat* e7 = kb.format("E7");
  CHECK(e7 && e7->space.ramBytes == 2048);
  rep = analyzeSpace({}, 0, {}, {}, e7->space, opt);
  CHECK(rep.cartRam && !rep.cartRam->measured);
  CHECK(!rep.summary().empty() || rep.banks.empty());
}

static void testDebugMap() {
  // F8: ORG $0000/$1000 with RORG $F000 (DASM 2.20 listing), a macro and an include.
  const std::string lst =
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
  map.load(lst, [](const std::string&) -> std::optional<int> { return std::nullopt; }, 4096);
  CHECK(map.banks() == 2);
  auto code = [](const ListedLine& l) { return l.source.find("ds") == std::string::npos; };
  auto bp = map.breakpointAt("main.asm", 12, code);
  CHECK(bp && bp->cpu == 0xF000 && bp->bank == 0);
  bp = map.breakpointAt("main.asm", 13, code);  // label line: next instruction
  CHECK(bp && bp->cpu == 0xF002 && bp->at->line == 14);
  bp = map.breakpointAt("main.asm", 19, code);
  CHECK(bp && bp->cpu == 0xF000 && bp->bank == 1);
  bp = map.breakpointAt("main.asm", 20, code);  // macro call: its first instruction
  CHECK(bp && bp->cpu == 0xF002 && bp->bank == 1);
  bp = map.breakpointAt("main.asm", 21, code);
  CHECK(bp && bp->cpu == 0xF004);
  bp = map.breakpointAt("inc.h", 1, code);
  CHECK(bp && bp->cpu == 0xF005 && bp->bank == 1);
  bp = map.breakpointAt("main.asm", 23, code);
  CHECK(bp && bp->cpu == 0xF007);
  CHECK(!map.breakpointAt("main.asm", 24, code));
  auto at = map.linesAt(0xF002);
  CHECK(at.size() == 2);  // bank 0 (line 14) and bank 1 (the macro)
  at = map.linesAt(0x1002, 0);  // a mirror of $F002
  CHECK(at.size() == 1 && at[0]->line == 14);
  at = map.linesAt(0xF003, 1);  // inside the second nop of the macro
  CHECK(at.size() == 1 && at[0]->line == 20);
  map.resolveFiles([](const std::string& f) { return "/p/" + f; });
  CHECK(map.breakpointAt("/p/inc.h", 1, code));

  // Breakpoints follow the text.
  TextEditor e;
  e.setText("a\nb\nc\nd");
  e.toggleBreakpoint(2);
  CHECK(e.breakpoints() == std::set<int>{2});
  e.goTo(0);
  e.duplicateLines();  // a a b c d
  CHECK(e.breakpoints() == std::set<int>{3});
  e.goTo(3);
  e.deleteLines();  // the line with the breakpoint goes
  CHECK(e.breakpoints().empty());
  e.undo();
  e.toggleBreakpoint(4);
  e.goTo(1);
  e.moveLines(1);
  CHECK(e.breakpoints() == std::set<int>{4});
  e.toggleBreakpoint(4);
  CHECK(e.breakpoints().empty() && e.takeBreakpointsChanged() && !e.takeBreakpointsChanged());
}

static void testEmulatorOptions(const KnowledgeBase& kb) {
  namespace fs = std::filesystem;
  const EmulatorInfo* st = kb.emulator("stella");
  const EmulatorInfo* go = kb.emulator("gopher2600");
  CHECK(st && go);
  if (!st || !go) return;
  // every option is well formed and its sources exist
  for (auto* e : {st, go}) {
    std::set<std::string> ids;
    for (auto& o : e->options) {
      CHECK(ids.insert(o.id).second);
      CHECK(!o.label.empty() && !o.description.empty() && !o.group.empty());
      CHECK(o.type == "bool" || o.type == "choice" || o.type == "int" || o.type == "text");
      CHECK((o.type != "bool" && o.type != "choice") || !o.values.empty());
      CHECK((e == st) == !o.arg.empty());
      CHECK(e == st || (!o.flag.empty()) != (!o.pref.empty()));
      CHECK(!o.developer || o.arg.rfind("-dev.", 0) == 0);
    }
  }
  CHECK(kb.source("stella-7.0-manual") && kb.source("gopher2600-0.56.1"));
  // translations fall back to English
  const EmuOption* dc = st->option("dev.debugcolors");
  CHECK(dc && dc->labelText() == "Debug colours");
  setLanguage("es");
  CHECK(dc && dc->labelText() == "Colores de depuración");
  setLanguage("en");

  Project p;
  p.dir = "/tmp/proj";
  p.output = "build/game.bin";
  p.emulator = "stella";
  p.emulatorOptions["stella"] = {{"dev.debugcolors", "1"}, {"audio.enabled", "0"}, {"holdreset", "1"}, {"tv.filter", "2"}};
  auto a = emulatorOptionArgs(p, *st);
  auto has = [&](const std::vector<std::string>& v, std::initializer_list<std::string> seq) {
    std::vector<std::string> want(seq);
    return std::search(v.begin(), v.end(), want.begin(), want.end()) != v.end();
  };
  CHECK(has(a, {"-holdreset"}) && !has(a, {"-holdreset", "1"}));
  CHECK(has(a, {"-audio.enabled", "0"}) && has(a, {"-tv.filter", "2"}) && has(a, {"-dev.debugcolors", "1"}));
  CHECK(has(a, {"-dev.settings", "1"}));  // a developer option turns the developer settings on
  CHECK(has(a, {"-snapsavedir", (fs::path("/tmp/proj/build").lexically_normal() / "").string()}));
  p.emulatorOptions["stella"]["dev.settings"] = "0";
  CHECK(!has(emulatorOptionArgs(p, *st), {"-dev.settings", "1"}));

  p.emulatorOptions["gopher2600"] = {{"swap", "true"}, {"left", "PADDLE"}, {"randState", "true"}, {"audioMutePlay", "true"}};
  std::map<std::string, std::string> prefs;
  a = emulatorOptionArgs(p, *go, &prefs);
  CHECK(has(a, {"-swap=true"}) && has(a, {"-left", "PADDLE"}));
  CHECK(prefs.size() == 2 && prefs["hardware.randState"] == "true" && prefs["sdlimgui.playmode.audioMute"] == "true");
  std::string merged = mergeGopherPreferences("*** do not edit this file by hand ***\nhardware.randState :: false\nx.y :: 3\n", prefs);
  CHECK(merged == "*** do not edit this file by hand ***\nhardware.randState :: true\nx.y :: 3\nsdlimgui.playmode.audioMute :: true\n");
  CHECK(mergeGopherPreferences("", {{"a", "1"}}).rfind("***", 0) == 0);

  // launches with a private copy of the settings
  fs::path tmp = fs::temp_directory_path() / "vcside-test-emu";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::path home = tmp / "home", root = tmp / "profiles";
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
  EmulatorLaunch l;
  l.args = {"/usr/local/bin/stella", "-debug", "/tmp/proj/build/game.bin"};
  std::string err;
  CHECK(prepareEmulatorLaunch(p, kb, root.string(), l, &err, home.string()));
  CHECK(l.args.back() == "/tmp/proj/build/game.bin");
  CHECK(has(l.args, {"-basedir", (root / "stella").string()}) && has(l.args, {"-userdir"}) && l.env.empty());
  CHECK(fs::exists(root / "stella" / "stella.sqlite3"));
  l = {};
  l.args = {"/Applications/Stella.app/Contents/MacOS/Stella", "/tmp/proj/build/game.bin"};
  fs::create_directories(home / "Library" / "Application Support" / "Stella");
  std::ofstream(home / "Library" / "Application Support" / "Stella" / "stella.sqlite3") << "app";
  CHECK(prepareEmulatorLaunch(p, kb, root.string(), l, &err, home.string()));
  CHECK(!has(l.args, {"-basedir"}) && l.env.size() == 1 && l.env[0].first == "HOME");
  CHECK(fs::exists(root / "stella-home" / "Library" / "Application Support" / "Stella" / "stella.sqlite3"));

  p.emulator = "gopher2600";
  p.tv = "SECAM60";
  l = {};
  l.args = {"/usr/local/bin/gopher2600", "RUN", "/tmp/proj/build/game.bin"};
  CHECK(prepareEmulatorLaunch(p, kb, root.string(), l, &err, home.string()));
  CHECK(l.workDir == (root / "gopher2600").string() && fs::exists(root / "gopher2600" / "portable.txt"));
  CHECK(l.notes.size() == 1);  // Gopher2600 has no SECAM60
  std::ifstream pf(root / "gopher2600" / "Gopher2600_UserData" / "preferences");
  std::string pt((std::istreambuf_iterator<char>(pf)), {});
  CHECK(pt.find("hardware.randState :: true") != std::string::npos && pt.find("mine.key :: 7") != std::string::npos);
  p.emulatorOptions["gopher2600"] = {{"swap", "true"}};  // no preference: the user's own folder
  l = {};
  l.args = {"/usr/local/bin/gopher2600", "RUN", "/tmp/proj/build/game.bin"};
  CHECK(prepareEmulatorLaunch(p, kb, root.string(), l, &err, home.string()) && l.workDir == p.dir);
  fs::remove_all(tmp, ec);
  CHECK(tvSupported("stella", "NTSC50") && !tvSupported("stella", "PAL-M") && tvSupported("gopher2600", "PAL-M"));
  CHECK(tvIs50Hz("NTSC50") && !tvIs50Hz("PAL60"));
}

// A minimal 32-bit little-endian ELF: sections (name, type, flags, size) and LOAD segments (vaddr, paddr, filesz, memsz).
static std::string makeElf(int type, const std::vector<std::tuple<std::string, uint32_t, uint32_t, uint32_t>>& secs,
                           const std::vector<std::array<uint32_t, 4>>& segs) {
  auto put = [](std::string& d, size_t at, uint32_t v, int n) {
    if (d.size() < at + n) d.resize(at + n);
    for (int i = 0; i < n; i++) d[at + i] = (char)(v >> (8 * i));
  };
  std::string names(1, '\0');
  std::vector<uint32_t> off;
  for (auto& s : secs) off.push_back((uint32_t)names.size()), names += std::get<0>(s) + '\0';
  uint32_t shstr = (uint32_t)names.size();
  names += ".shstrtab";
  names += '\0';
  std::string d(52, '\0');
  d[0] = 0x7f, d[1] = 'E', d[2] = 'L', d[3] = 'F', d[4] = 1, d[5] = 1, d[6] = 1;
  put(d, 16, type, 2);
  put(d, 18, 40, 2);  // ARM
  uint32_t phoff = 52, strAt = phoff + 32 * (uint32_t)segs.size(), shoff = (strAt + (uint32_t)names.size() + 3) & ~3u;
  put(d, 28, segs.empty() ? 0 : phoff, 4);
  put(d, 32, shoff, 4);
  put(d, 42, 32, 2);
  put(d, 44, (uint32_t)segs.size(), 2);
  put(d, 46, 40, 2);
  put(d, 48, (uint32_t)secs.size() + 2, 2);
  put(d, 50, (uint32_t)secs.size() + 1, 2);
  for (size_t i = 0; i < segs.size(); i++) {
    size_t p = phoff + 32 * i;
    put(d, p, 1, 4);
    put(d, p + 8, segs[i][0], 4);
    put(d, p + 12, segs[i][1], 4);
    put(d, p + 16, segs[i][2], 4);
    put(d, p + 20, segs[i][3], 4);
  }
  d.resize(strAt);
  d += names;
  d.resize(shoff + 40);  // section 0: null
  for (size_t i = 0; i < secs.size(); i++) {
    size_t p = shoff + 40 * (i + 1);
    put(d, p, off[i], 4);
    put(d, p + 4, std::get<1>(secs[i]), 4);
    put(d, p + 8, std::get<2>(secs[i]), 4);
    put(d, p + 20, std::get<3>(secs[i]), 4);
  }
  size_t p = shoff + 40 * (secs.size() + 1);
  put(d, p, shstr, 4);
  put(d, p + 4, 3, 4);
  put(d, p + 16, strAt, 4);
  put(d, p + 20, (uint32_t)names.size(), 4);
  d.resize(p + 40);
  return d;
}

static void testElfSpace() {
  // PROGBITS 1, NOBITS 8; WRITE 1, ALLOC 2, EXEC 4
  std::string rom = makeElf(1, {{".text", 1, 6, 1000}, {".RamFunc", 1, 6, 300}, {".rodata.tab", 1, 2, 2000},
                                {".data.x", 1, 3, 40}, {".bss.y", 8, 3, 500}, {".comment", 1, 0, 19}}, {});
  ElfFile f;
  std::string err;
  CHECK(readElf(rom, f, &err) && f.type == 1 && f.machine == 40 && f.sections.size() == 8);
  CHECK(f.sections.size() == 8 && f.sections[2].name == ".RamFunc");
  ElfSpace e = analyzeElfSpace(f, (long)rom.size(), nullptr, 90);
  CHECK(e.code == 1000 && e.ramCode == 300 && e.constants == 2000 && e.data == 40 && e.bss == 500 && !e.exact);
  CHECK(e.flash.used == 3340 + kChameleonFirmwareFlash && e.ram.used == 840 + kChameleonFirmwareRam);
  CHECK(e.diagnostics.empty());
  // the Chameleon image: flash = what is stored at 0x0800_0000, RAM = what lives at 0x2000_0000
  std::string img = makeElf(2, {}, {{0x08000000, 0x08000000, 120000, 120000}, {0x20000000, 0x0801D4C0, 4000, 34000},
                                   {0x20008500, 0x0801E460, 0, 1100}});
  ElfFile c;
  CHECK(readElf(img, c, &err) && c.type == 2 && c.segments.size() == 3);
  e = analyzeElfSpace(f, (long)rom.size(), &c, 90);
  CHECK(e.exact && e.flash.used == 124000 && e.ram.used == 35100);
  CHECK(e.diagnostics.size() == 2 && e.diagnostics[0].severity == Diagnostic::Warning);  // 94 % and 95 %
  CHECK(!readElf("not an elf", f, &err) && !err.empty());

  // a new ELF project from the template
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "vcside-test-elf";
  std::error_code ec;
  fs::remove_all(dir, ec);
  Project p;
  CHECK(Project::create(dir.string(), "rainbow", Language::ElfC, "NTSC", &p, &err));
  CHECK(p.buildCommand == "make" && p.format == "ELF" && p.output == "build/rainbow.bin" && p.main == "src/main.c");
  for (auto f : {"Makefile", "include/vcs.h", "src/main.c", "src/trampolines.S", "chameleon/support.c"}) CHECK(fs::exists(dir / f));
  CHECK(!fs::exists(dir / "LICENSE.md"));
  std::ifstream mk(dir / "Makefile");
  std::string mtext((std::istreambuf_iterator<char>(mk)), {});
  CHECK(mtext.find("NAME  = rainbow") != std::string::npos && mtext.find("{{") == std::string::npos);
  CHECK(!Project::create((dir / "b").string(), "two words", Language::ElfC, "NTSC", nullptr, &err));
  CHECK(Project::create((dir / "bb").string(), "Square Run", Language::BatariBasic, "NTSC", &p, &err));
  CHECK(p.main == "main.bas" && p.output == "main.bas.bin" && p.format == "AUTO" && p.buildCommand.empty());
  std::ifstream bas(dir / "bb" / "main.bas");
  std::string btext((std::istreambuf_iterator<char>(bas)), {});
  CHECK(btext.find("rem Square Run - batari Basic") != std::string::npos && btext.find("drawscreen") != std::string::npos);
  // the DASM example: its graphics, the tables they give, and a program that assembles
  CHECK(Project::examples(Language::Dasm).size() == 1 && Project::examples(Language::BatariBasic).empty());
  for (const char* tv : {"NTSC", "PAL"}) {
    fs::path ad = dir / (std::string("anim-") + tv);
    CHECK(Project::create(ad.string(), "walker", Language::Dasm, tv, &p, &err, "animation"));
    for (auto f : {"main.asm", "2600.h", "hero.vcsgfx", "hero.gfx.asm"}) CHECK(fs::exists(ad / f));
    std::ifstream inc(ad / "hero.gfx.asm");
    std::string itext((std::istreambuf_iterator<char>(inc)), {});
    CHECK(itext.find("hero_walk_FRAME") != std::string::npos && itext.find("hero_P0_FramePtrLo") != std::string::npos);
    std::string dasm = findInPath("dasm");
    if (!dasm.empty()) {
      std::string cmd = "cd \"" + ad.string() + "\" && \"" + dasm + "\" main.asm -f3 -oexample.bin > dasm.log 2>&1";
      CHECK(std::system(cmd.c_str()) == 0 && fs::file_size(ad / "example.bin", ec) == 4096);
    }
  }
  CHECK(!Project::create((dir / "none").string(), "x", Language::Dasm, "NTSC", &p, &err, "nope") && !err.empty());
  // the ELF example: the template with the example's main.c, and hero.gfx.h next to it
  CHECK(Project::examples(Language::ElfC).size() == 1);
  fs::path ed = dir / "anim-elf";
  CHECK(Project::create(ed.string(), "walker", Language::ElfC, "NTSC", &p, &err, "animation"));
  for (auto f : {"Makefile", "include/vcs.h", "src/main.c", "src/trampolines.S", "src/hero.vcsgfx", "src/hero.gfx.h"})
    CHECK(fs::exists(ed / f));
  std::ifstream emain(ed / "src" / "main.c"), einc(ed / "src" / "hero.gfx.h");
  std::string emtext((std::istreambuf_iterator<char>(emain)), {}), eitext((std::istreambuf_iterator<char>(einc)), {});
  CHECK(emtext.find("walker - made with VCSIDE") != std::string::npos && emtext.find("hero.gfx.h") != std::string::npos);
  CHECK(eitext.find("static const vcsgfx_animation hero_walk = {hero_walk_steps, 4, 1};") != std::string::npos);
  std::string gcc = findInPath("arm-none-eabi-gcc"), make = findInPath("make");
  if (!gcc.empty() && !make.empty()) {
    std::string cmd = "cd \"" + ed.string() + "\" && \"" + make + "\" > make.log 2>&1";
    CHECK(std::system(cmd.c_str()) == 0 && fs::exists(ed / "build" / "walker.bin"));
  }
  CHECK(!Project::create((dir / "none-elf").string(), "x", Language::ElfC, "NTSC", &p, &err, "nope") && !err.empty());
  if (!std::getenv("VCSIDE_TEST_KEEP")) fs::remove_all(dir, ec);
}

static void testFormatOrder(const KnowledgeBase& kb) {
  CHECK(naturalLess("Atari 8K (F8)", "Atari 16K (F6)") && !naturalLess("Atari 16K (F6)", "Atari 8K (F8)"));
  CHECK(naturalLess("Atari 8K (F8)", "Atari 8K with Superchip (F8SC)") && naturalLess("bus", "CDF") && naturalLess("x2", "x010"));
  CHECK(!naturalLess("a", "a") && naturalLess("a", "ab"));
  std::set<std::string> cats;
  for (auto& [id, title] : formatCategories()) cats.insert(id);
  for (auto& f : kb.formats()) {
    CHECK(cats.count(f.category));
    if (!cats.count(f.category)) std::fprintf(stderr, "  format %s: category '%s'\n", f.id.c_str(), f.category.c_str());
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
  for (auto& [w, b] : kb.basicKeywords()) {
    CHECK(!b.summary.empty() && !b.syntax.empty() && w.find('N') == std::string::npos);
    if (b.summary.empty()) std::fprintf(stderr, "  %s\n", w.c_str());
  }
  CHECK(basicLabelOf("main") == "main" && basicLabelOf("10 goto 10") == "10" && basicLabelOf(" drawscreen").empty());
  CHECK(basicLabelOf("player0:").empty() && basicLabelOf("").empty());

  auto kinds = [&](const std::string& l) {
    std::vector<std::pair<std::string, TokenKind>> out;
    for (auto& t : tokenizeBasic(kb, l)) out.push_back({l.substr(t.start, t.end - t.start), t.kind});
    return out;
  };
  using K = TokenKind;
  auto v = kinds("loop if joy0fire then goto shoot");
  CHECK(v.size() == 6 && v[0].second == K::Label && v[1].second == K::Mnemonic && v[2].second == K::Mnemonic &&
        v[5].first == "shoot" && v[5].second == K::Label);
  v = kinds(" if a > 3 then a = 0 : COLUBK = $84 rem blue");
  CHECK(v.size() == 9 && v[1].second == K::Symbol && v[2].second == K::Number && v[4].first == "a" && v[4].second == K::Symbol &&
        v[6].second == K::Register && v[7].second == K::Number && v[8].first == "rem blue" && v[8].second == K::Comment);
  v = kinds(" if switchreset then reset_game");
  CHECK(v.back().first == "reset_game" && v.back().second == K::Label);
  v = kinds(" on level goto l0 l1 l2");
  CHECK(v.size() == 6 && v[3].second == K::Label && v[5].second == K::Label);
  v = kinds(" player0:");
  CHECK(v.size() == 1 && v[0].first == "player0:" && v[0].second == K::Directive);
  v = kinds(" player0x = player0x + 1");
  CHECK(v.size() == 3 && v[0].second == K::Register && v[1].second == K::Register);
  v = kinds(" set romsize 8kSC");
  CHECK(v.size() == 3 && v[0].second == K::Directive && v[1].second == K::Directive);
}

static void testBasic() {
  // batari Basic 1.9: its own error, then DASM's in the generated .asm
  auto d = parseBasicOutput("Starting build of main.bas\nbatari Basic v1.9 (c)2025\nline 4: Error: Unknown keyword: frobnicate\n\nCompilation failed.\n",
                            "main.bas", "");
  CHECK(d.size() == 1 && d[0].severity == Diagnostic::Error && d[0].file == "main.bas" && d[0].line == 4 &&
        d[0].message == "Unknown keyword: frobnicate");
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
  d = parseBasicOutput("2600 Basic compilation complete.\n\n      2859 bytes of ROM space left\n\n"
                       "main.bas.asm (7): error: Unknown Mnemonic 'jmp .nowhere'.\n\nFatal assembly error: Source is not resolvable.\n",
                       "main.bas", asmText);
  CHECK(d.size() == 1 && d[0].file == "main.bas" && d[0].line == 6 && d[0].message.find("nowhere") != std::string::npos &&
        d[0].message.find("Mnemonic") == std::string::npos);
  d = parseBasicOutput("main.bas.asm (9): error: something\n", "main.bas", asmText);  // in the includes: stays in the .asm
  CHECK(d.size() == 1 && d[0].file == "main.bas.asm" && d[0].line == 9);
  CHECK(parseBasicOutput("2600 Basic compilation complete.\n      148 bytes of ROM space left\n", "main.bas", "").empty());

  // space: one bank, or 4 KB banks counted from 1
  SpaceOptions opt;
  auto s = parseBasicSpace("      148 bytes of ROM space left\n", 4096, opt);
  CHECK(s.banks.size() == 1 && s.banks[0].used == 4096 - 148 && s.banks[0].size == 4096 && !s.ram.measured);
  CHECK(s.diagnostics.size() == 1 && s.diagnostics[0].severity == Diagnostic::Warning);
  s = parseBasicSpace("   1000 bytes of ROM space left\n", 2048, opt);
  CHECK(s.banks.size() == 1 && s.banks[0].size == 2048 && s.banks[0].used == 1048 && s.diagnostics.empty());
  s = parseBasicSpace("      3251 bytes of ROM space left in bank 1\r\n      -20 bytes of ROM space left in bank 2\n"
                      "      2276 bytes of ROM space left\n", 8192, opt);
  CHECK(s.banks.size() == 2 && s.banks[0].name == "Bank 0" && s.banks[0].used == 4096 - 3251 && s.banks[1].used == 4116);
  CHECK(s.diagnostics.size() == 1 && s.diagnostics[0].severity == Diagnostic::Error);
  CHECK(parseBasicSpace("nothing\n", 4096, opt).banks.empty());

  // breakpoints: the listing of the .asm taken to the lines of the .bas
  const std::string lst =
      "------- FILE main.bas.asm LEVEL 1 PASS 3\n"
      "      2  f000\t\t       20 00 f8\t      jsr\tdrawscreen\n"
      "      3  f003\t\t\t\t   .L03\t\t;;line 5;;  a = a + 1\n"
      "      4  f003\t\t       a5 d6\t      LDA\ta\n"
      "      7  f005\t\t       4c 00 f0\t      jmp\t.L03\n";
  AddressMap am;
  am.load(lst, [](const std::string&) { return std::optional<int>(); }, 4096);
  am.remap("main.bas.asm", map, "/p/main.bas");
  auto any = [](const ListedLine&) { return true; };
  auto bp = am.breakpointAt("/p/main.bas", 5, any);
  CHECK(bp && bp->cpu == 0xF003 && bp->at->line == 5);
  bp = am.breakpointAt("/p/main.bas", 6, any);
  CHECK(bp && bp->cpu == 0xF005);
  CHECK(!am.breakpointAt("/p/main.bas", 7, any));
  CHECK(am.linesAt(0xF000).empty());  // the header has no line of the .bas
}

static bool hasIssue(const std::vector<gfx::Issue>& v, gfx::Issue::Severity sev, const std::string& part) {
  for (auto& i : v)
    if (i.severity == sev && i.text.find(part) != std::string::npos) return true;
  return false;
}

static void testGfx() {
  using namespace gfx;
  // palette: grey hue 0, SECAM by luminance only
  CHECK(rgb(Tv::NTSC, 0x00) == 0x0F0F0F || (rgb(Tv::NTSC, 0x00) >> 16) == (rgb(Tv::NTSC, 0x00) & 0xFF));
  CHECK(rgb(Tv::SECAM, 0x0E) == 0xFFFFFF && rgb(Tv::SECAM, 0x40) == 0x000000 && rgb(Tv::SECAM, 0x42) == rgb(Tv::SECAM, 0x92));
  CHECK(tvFromProject("PAL60") == Tv::PAL && tvFromProject("PAL-M") == Tv::NTSC && tvFromProject("SECAM60") == Tv::SECAM);
  CHECK(hexColour(0x1F) == "$1E");

  // playfield bits: PF0 4-7, PF1 7-0, PF2 0-7
  int reg, bit;
  pfBit(0, &reg, &bit);
  CHECK(reg == 0 && bit == 4);
  pfBit(4, &reg, &bit);
  CHECK(reg == 1 && bit == 7);
  pfBit(19, &reg, &bit);
  CHECK(reg == 2 && bit == 7);
  Playfield pf;
  pf.rows.resize(3);
  pf.setPixel(0, 0, true);
  pf.rows[0].mode = PfMode::Mirror;
  CHECK(pf.pixel(0, 39) && !pf.pixel(0, 20) && !pf.rows[0].asymmetric);
  pf.rows[1].mode = PfMode::Repeat;
  pf.setPixel(1, 4, true);
  CHECK(pf.pixel(1, 24));
  pf.setPixel(1, 30, true);  // the right half: the row becomes asymmetric, the old copy stays
  CHECK(pf.rows[1].asymmetric && pf.pixel(1, 24) && pf.pixel(1, 30) && !pf.pixel(1, 10));
  uint8_t L[3], R[3];
  pf.registers(1, L, R);
  CHECK(L[1] == 0x80 && R[1] == 0x82 && R[2] == 0);  // column 30 = right index 10 = PF1 bit 1
  pf.setAsymmetric(1, false);
  CHECK(!pf.pixel(1, 30) && pf.pixel(1, 24));
  pf.rows[2].pf = 0x44;
  CHECK(pf.pfAt(1) == pf.pfColour && pf.pfAt(2) == 0x44);

  // sprites: layers, frames, colours
  Sprite sp;
  sp.name = "hero";
  Layer a;
  a.obj = Obj::P0;
  a.resize(1, 3);
  a.bits[0] = {0x81, 0x3C, 0xFF};
  a.colours[0] = {0x1E, -1, 0x44};
  Layer m;
  m.obj = Obj::M0;
  m.dx = 9;
  m.size = 2;
  m.resize(1, 3);
  m.bits[0] = {1, 1, 0};
  m.colours[0] = {0x1E, 0x88, -1};
  sp.layers = {a, m};
  CHECK(a.colour(0, 1) == 0x1E && a.colour(0, 2) == 0x44 && a.width() == 8 && m.width() == 2);
  CHECK(a.pixel(0, 0, 0) && !a.pixel(0, 0, 1) && a.pixel(0, 0, 7));
  Layer big = a;
  big.size = 2;
  big.reflect = true;
  CHECK(big.width() == 16 && big.pixel(0, 1, 4) && big.pixel(0, 1, 11) && !big.pixel(0, 1, 3));
  Profile own, std;
  std.kernel = Kernel::Standard;
  auto iss = check(sp, own);
  CHECK(hasIssue(iss, Issue::Warning, "COLUP0"));  // M0 row 2 is $88, P0's is $1E
  CHECK(!hasIssue(iss, Issue::Error, ""));
  sp.layers.push_back(a);  // a second P0 in the same phase
  CHECK(hasIssue(check(sp, own), Issue::Error, "P0"));
  sp.layers.back().phase = 1;
  iss = check(sp, own);
  CHECK(!hasIssue(iss, Issue::Error, "") && hasIssue(iss, Issue::Warning, "30 Hz"));
  CHECK(hasIssue(check(sp, std), Issue::Warning, "alternate the phases"));
  sp.layers.pop_back();
  sp.insertFrame(1, 0);
  CHECK(sp.frames == 2 && sp.layers[0].bits[1] == sp.layers[0].bits[0]);
  sp.layers[0].bits[1][0] = 0x18;
  sp.removeFrame(0);
  CHECK(sp.frames == 1 && sp.layers[0].bits[0][0] == 0x18);
  sp.layers[0].bits[0][0] = 0x81;

  // JSON round trip
  Asset as;
  sp.background = 0x0A;  // the editor's backdrop travels with the sprite
  as.sprites = {sp};
  as.playfields = {pf};
  Asset back;
  std::string err;
  CHECK(fromJson(toJson(as), &back, &err));
  CHECK(back.sprites.size() == 1 && back.sprites[0].layers.size() == 2 && back.sprites[0].layers[1].dx == 9 &&
        back.sprites[0].layers[0].bits[0] == sp.layers[0].bits[0] && back.sprites[0].layers[0].colours[0] == sp.layers[0].colours[0]);
  CHECK(back.playfields.size() == 1 && back.playfields[0].rows[2].pf == 0x44 && back.playfields[0].pixel(0, 39));
  CHECK(toJson(back) == toJson(as) && back.sprites[0].background == 0x0A);
  CHECK(!fromJson("{nope", &back, &err) && !err.empty());

  // DASM export, read back
  std::string d = exportDasm(sp);
  CHECK(d.find("hero_P0_Frame0\n        .byte #%11111111;$44\n        .byte #%00111100;$1E\n        .byte #%10000001;$1E\n") != std::string::npos);
  CHECK(d.find("hero_M0_DX = 9") != std::string::npos && d.find("hero_M0_SIZE = $10") != std::string::npos);
  Asset imp;
  std::string what;
  CHECK(importText(d, &imp, &what) && imp.sprites.size() == 1);
  if (imp.sprites.size() == 1) {
    auto& s2 = imp.sprites[0];
    CHECK(s2.name == "hero" && s2.layers.size() == 2);
    CHECK(s2.layers.size() == 2 && s2.layers[0].bits[0] == sp.layers[0].bits[0] && s2.layers[0].colour(0, 2) == 0x44);
    CHECK(s2.layers.size() == 2 && s2.layers[1].obj == Obj::M0 && s2.layers[1].dx == 9 && s2.layers[1].size == 2 && s2.layers[1].bits[0][1] == 1);
  }
  // PlayerPal-style tables: Frame/ColorFrame labels, bottom row first, colour comments
  imp = Asset();
  CHECK(importText(";---Graphics Data---\nFrame0\n        .byte #%00011000;$46\n        .byte #%00111100;$1A\nFrame1\n        .byte #%00100100;$46\n"
                   "        .byte #%01111110;$1A\n;---Color Data---\nColorFrame0\n        .byte #$46;\n        .byte #$1A;\n",
                   &imp, &what));
  CHECK(imp.sprites.size() == 1 && imp.sprites[0].frames == 2 && imp.sprites[0].layers.size() == 1);
  if (imp.sprites.size() == 1 && imp.sprites[0].layers.size() == 1) {
    auto& l = imp.sprites[0].layers[0];
    CHECK(l.bits[0][0] == 0x3C && l.bits[0][1] == 0x18 && l.colour(0, 0) == 0x1A && l.bits[1][0] == 0x7E && l.colour(1, 1) == 0x46);
  }
  // batari Basic export and import. The standard kernel without options: one colour per
  // player, rows bottom first, missiles as blocks.
  std::string b = exportBasic(sp, std);
  CHECK(b.find("  player0:\n  %11111111\n  %00111100\n  %10000001\nend\n  COLUP0 = $1E : rem every frame\n") != std::string::npos);
  CHECK(b.find("missile0height = 2") != std::string::npos);
  CHECK(hasIssue(check(sp, std), Issue::Warning, "playercolors"));
  // playercolors: a colour per row, at the cost of both missiles
  Profile pc = profileFromBasic(" rem set kernel PXE\n  set kernel_options playercolors pfcolors ; colours\n  const pfres = 12\n");
  CHECK(pc.kernel == Kernel::Standard && pc.options.playercolors && pc.options.pfcolors && !pc.options.player1colors && pc.options.pfres == 12);
  CHECK(pc.rowColours(Obj::P0) && !pc.has(Obj::M0) && !pc.has(Obj::M1) && pc.has(Obj::BL) && pc.pfRowColours() && pc.pfRowsDefault() == 12);
  CHECK(hasIssue(check(sp, pc), Issue::Error, "M0"));
  b = exportBasic(sp, pc);
  CHECK(b.find("  player0color:\n  $44\n  $1E\n  $1E\nend\n") != std::string::npos && b.find("rem M0: not drawn") != std::string::npos);
  imp = Asset();
  CHECK(importText(b, &imp, &what) && imp.sprites.size() == 1 && imp.sprites[0].layers[0].bits[0] == sp.layers[0].bits[0] &&
        imp.sprites[0].layers[0].colour(0, 2) == 0x44);
  Profile bg = profileFromBasic("set kernel_options pfcolors backgroundchange\n");
  CHECK(!bg.pfRowColours() && bg.bkRowColours());
  CHECK(!profileFromBasic("set kernel_options player1colors\n").has(Obj::M1) && profileFromBasic("set kernel_options player1colors\n").has(Obj::M0));
  // DPC+: rows top first, a colour table always, ten players
  Profile dpc = profileFromBasic("  set kernel DPC+\n");
  CHECK(dpc.kernel == Kernel::DPCPlus && dpc.players() == 10 && dpc.has(Obj::P9) && !dpc.has(Obj::P10) && dpc.topFirst());
  b = exportBasic(sp, dpc);
  CHECK(b.find("  player0:\n  %10000001\n  %00111100\n  %11111111\nend\n  player0color:\n  $1E\n  $1E\n  $44\nend\n") != std::string::npos);
  imp = Asset();
  CHECK(importText(b, &imp, &what, dpc) && imp.sprites.size() == 1 && imp.sprites[0].layers[0].bits[0] == sp.layers[0].bits[0]);
  imp = Asset();
  CHECK(importText("  set kernel DPC+\n" + b, &imp, &what) && imp.sprites.size() == 1 && imp.sprites[0].layers[0].bits[0] == sp.layers[0].bits[0]);
  // PXE: up to P16; the extra players exist only in the kernels that draw them
  Profile pxe = profileFromBasic("set kernel PXE\n");
  Sprite many = sp;
  many.layers = {a};
  many.layers[0].obj = Obj::P12;
  many.layers.push_back(a);
  many.layers[1].obj = Obj::P3;
  CHECK(!hasIssue(check(many, pxe), Issue::Error, "") && hasIssue(check(many, pxe), Issue::Warning, "player 1"));
  CHECK(hasIssue(check(many, std), Issue::Error, "P12") && hasIssue(check(many, dpc), Issue::Error, "P12"));
  b = exportBasic(many, pxe);
  CHECK(b.find("  player12:\n  %10000001\n") != std::string::npos && b.find("player12color:") != std::string::npos);
  imp = Asset();
  CHECK(importText(b, &imp, &what, pxe) && imp.sprites.size() == 1 && imp.sprites[0].layers.size() == 2);
  if (imp.sprites.size() == 1 && imp.sprites[0].layers.size() == 2)
    CHECK(imp.sprites[0].layers[0].obj == Obj::P3 && imp.sprites[0].layers[1].obj == Obj::P12 && imp.sprites[0].layers[1].bits[0] == a.bits[0]);
  // DASM labels: P12 is the player 12, P1b a second P1 layer
  many.layers[1].obj = Obj::P1;
  many.layers.push_back(many.layers[1]);
  many.layers[2].phase = 1;
  d = exportDasm(many);
  CHECK(d.find("hero_P12_Frame0") != std::string::npos && d.find("hero_P1_Frame0") != std::string::npos && d.find("hero_P1b_Frame0") != std::string::npos);
  imp = Asset();
  CHECK(importText(d, &imp, &what) && imp.sprites.size() == 1 && imp.sprites[0].layers.size() == 3);
  if (imp.sprites.size() == 1 && imp.sprites[0].layers.size() == 3)
    CHECK(imp.sprites[0].layers[0].obj == Obj::P12 && imp.sprites[0].layers[1].obj == Obj::P1 && imp.sprites[0].layers[2].obj == Obj::P1 &&
          imp.sprites[0].layers[2].phase == 1);
  // multisprite: one colour per player, missiles one line high
  Profile ms = profileFromBasic("set kernel multisprite\n");
  CHECK(ms.players() == 6 && !ms.rowColours(Obj::P1) && ms.pfMirrorOnly() && ms.pfColumns() == 16);
  many.layers = {a, a};
  many.layers[1].obj = Obj::P4;
  b = exportBasic(many, ms);
  CHECK(b.find("  COLUP4 = ") != std::string::npos && b.find("player4color") == std::string::npos);
  many.layers[1].obj = Obj::P1;
  CHECK(exportBasic(many, ms).find("  _COLUP1 = ") != std::string::npos);
  // playfields: DASM tables and batari Basic blocks both come back
  Playfield p2;
  p2.name = "level";
  p2.rows.resize(2);
  p2.rows[0].mode = PfMode::Repeat;
  p2.setPixel(0, 5, true);
  p2.setPixel(1, 17, true);
  p2.setPixel(1, 33, true);
  p2.rows[1].pf = 0x86;
  d = exportDasm(p2);
  CHECK(d.find("level_PF0R") != std::string::npos && d.find("level_COLUPF_ROWS") != std::string::npos);
  imp = Asset();
  CHECK(importText(d, &imp, &what) && imp.playfields.size() == 1);
  if (imp.playfields.size() == 1) {
    auto& q = imp.playfields[0];
    bool same = q.rows.size() == 2;
    for (int r = 0; r < 2 && same; r++)
      for (int c = 0; c < 40; c++) same &= q.pixel(r, c) == p2.pixel(r, c);
    CHECK(same && q.name == "level" && q.pfAt(1) == 0x86 && q.rows[1].asymmetric && !q.rows[0].asymmetric);
  }
  b = exportBasic(p2, own);  // a kernel of one's own copied as batari Basic: the standard kernel with what it needs
  CHECK(b.find("  playfield:\n  .X...................X..........\n") != std::string::npos && b.find("pfcolors:") != std::string::npos);
  imp = Asset();
  CHECK(importText(b, &imp, &what) && imp.playfields.size() == 1 && imp.playfields[0].pixel(0, 5) && imp.playfields[0].pixel(1, 33) &&
        imp.playfields[0].pfAt(1) == 0x86);
  auto bi = check(p2, std);
  CHECK(hasIssue(bi, Issue::Warning, "pfres") && hasIssue(bi, Issue::Warning, "pfcolors"));
  CHECK(!hasIssue(check(p2, pc), Issue::Warning, "pfcolors") && hasIssue(check(p2, pc), Issue::Warning, "pfres = 2"));
  CHECK(!hasIssue(check(p2, dpc), Issue::Warning, "rows"));
  p2.setPixel(0, 1, true);
  CHECK(hasIssue(check(p2, std), Issue::Warning, "columns"));
  CHECK(hasIssue(check(p2, own), Issue::Info, "CTRLPF"));
  // multisprite playfields: 16 columns from column 5, mirrored
  Playfield mp;
  mp.rows.resize(2);
  mp.setPixel(0, 4, true);
  mp.setPixel(1, 19, true);
  CHECK(!hasIssue(check(mp, ms), Issue::Warning, "mirrored") && !hasIssue(check(mp, ms), Issue::Warning, "columns"));
  b = exportBasic(mp, ms);
  CHECK(b.find("  playfield:\n  X...............\n  ...............X\nend\n  COLUPF = ") != std::string::npos);
  imp = Asset();
  CHECK(importText(b, &imp, &what) && imp.playfields.size() == 1 && imp.playfields[0].pixel(0, 4) && imp.playfields[0].pixel(0, 35) &&
        imp.playfields[0].pixel(1, 20) && !imp.playfields[0].rows[0].asymmetric);
  mp.rows[0].mode = PfMode::Repeat;
  CHECK(hasIssue(check(mp, ms), Issue::Warning, "mirrored"));

  // regions of a batari Basic program kept up to date
  {
    Asset art;
    Sprite walk = sp;
    walk.name = "walk";
    walk.layers = {a};
    walk.insertFrame(1, 0);
    walk.layers[0].bits[1][0] = 0x18;
    art.sprites = {walk};
    mp.name = "level";
    art.playfields = {mp};
    std::string reg = basicRegion("art.vcsgfx", art, 0, std);
    CHECK(reg.find("  rem vcsgfx art.vcsgfx walk frame 1\n") == 0 && reg.find("  rem vcsgfx art.vcsgfx walk frame 2\n") != std::string::npos);
    CHECK(reg.find("%00011000") != std::string::npos);
    std::string prog = " set kernel_options pfcolors\r\nmain\r\n  if a then goto two\r\n  rem vcsgfx art.vcsgfx walk frame 1\r\n  player0:\r\nold\r\nend\r\n"
                       "  rem vcsgfx end\r\n  goto draw\r\ntwo\r\n REM VCSGFX Art.vcsgfx walk frame 2\r\n rem vcsgfx end\r\n"
                       "  rem vcsgfx art.vcsgfx level\r\n  rem vcsgfx end\r\n  rem vcsgfx other.vcsgfx walk\r\nkeep\r\n  rem vcsgfx end\r\ndraw\r\n drawscreen\r\n";
    int n = 0;
    std::vector<std::string> probs;
    std::string up = updateBasicRegions(prog, "art.vcsgfx", art, std, &n, &probs);
    CHECK(n == 3 && probs.empty());
    CHECK(up.find("old") == std::string::npos && up.find("keep\r\n") != std::string::npos && up.find("drawscreen\r\n") != std::string::npos);
    CHECK(up.find("  rem vcsgfx art.vcsgfx walk frame 1\r\n  rem walk, frame 1;") != std::string::npos);
    CHECK(up.find("  %00011000\r\nend\r\n") != std::string::npos);  // frame 2's top row, bottom first: last
    CHECK(up.find("  playfield:\r\n") != std::string::npos && up.find("\n\n") == std::string::npos);
    // a second pass changes nothing
    CHECK(updateBasicRegions(up, "art.vcsgfx", art, std, &n, &probs) == up && n == 3);
    // problems: an unknown item, a frame too many, no end
    probs.clear();
    std::string bad = "  rem vcsgfx art.vcsgfx nobody\n  rem vcsgfx end\n  rem vcsgfx art.vcsgfx walk frame 3\n  rem vcsgfx end\n  rem vcsgfx art.vcsgfx walk\nx\n";
    CHECK(updateBasicRegions(bad, "art.vcsgfx", art, std, &n, &probs) == bad && n == 0 && probs.size() == 3);
  }
  CHECK(!importText("lda #1\n", &imp, &what) && what.empty());
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
    for (const char* t : {rowsText, regsText}) {
      Asset mw;
      CHECK(importText(t, &mw, &what) && mw.playfields.size() == 1 && mw.sprites.empty());
      if (mw.playfields.size() != 1) continue;
      const Playfield& q = mw.playfields[0];
      CHECK(q.rows.size() == 3 && q.lineHeight == 8 && q.rows[0].mode == PfMode::Mirror);
      std::string got;
      for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 40; c++) got += q.pixel(r, c) ? 'X' : ' ';
        got += '|';
      }
      CHECK(got == "X   X       X      X                    |"
                   "                                      X |"
                   "     X                       X          |");
      CHECK(q.rows[1].asymmetric && q.rows[2].asymmetric);
    }
    Asset mw;
    CHECK(importText("; mode: symmetric repeat line-height 4\n.byte %00010000,%00000000,%00000001\n.byte $00,$00,$00\n", &mw, &what));
    CHECK(mw.playfields.size() == 1 && mw.playfields[0].rows.size() == 2 && mw.playfields[0].lineHeight == 4 &&
          mw.playfields[0].rows[0].mode == PfMode::Repeat && mw.playfields[0].pixel(0, 0) && mw.playfields[0].pixel(0, 20) &&
          mw.playfields[0].pixel(0, 12) && mw.playfields[0].pixel(0, 32) && !mw.playfields[0].rows[0].asymmetric);
  }

  // animations: steps of frames with their durations
  Sprite walker = sp;
  walker.name = "walker";
  walker.insertFrame(1, 0);
  walker.insertFrame(2, -1);
  walker.layers[0].bits[1][0] = 0x18;
  walker.layers[0].bits[2][0] = 0x24;
  Animation wa, idle;
  wa.name = "walk";
  wa.steps = {{0, 8}, {1, 4}, {2, 8}, {1, 4}};
  idle.name = "idle";
  idle.loop = false;
  idle.steps = {{2, 10}, {0, 5}};
  walker.animations = {wa, idle};
  CHECK(wa.length() == 24 && walker.frameAt(wa, 0) == 0 && walker.frameAt(wa, 8) == 1 && walker.frameAt(wa, 12) == 2 &&
        walker.frameAt(wa, 23) == 1 && walker.frameAt(wa, 24) == 0);
  CHECK(walker.frameAt(idle, 100) == 0 && walker.frameAt(idle, 3) == 2);
  {
    Sprite w2 = walker;
    w2.insertFrame(0, -1);
    CHECK(w2.frames == 4 && w2.animations[0].steps[0].frame == 1 && w2.animations[0].steps[1].frame == 2);
    w2.removeFrame(2);  // the old frame 1: its steps go
    CHECK(w2.animations[0].steps.size() == 2 && w2.animations[0].steps[0].frame == 1 && w2.animations[0].steps[1].frame == 2);
    w2 = walker;
    w2.swapFrames(0, 2);
    CHECK(w2.animations[0].steps[0].frame == 2 && w2.animations[0].steps[2].frame == 0 && w2.layers[0].bits[0][0] == 0x24);
  }
  {
    Asset aa, bb;
    aa.sprites = {walker};
    aa.sprites[0].animations[0].basicTimer = "y";
    aa.sprites[0].animations[0].basicStep = "z";
    CHECK(fromJson(toJson(aa), &bb, &err) && bb.sprites.size() == 1 && bb.sprites[0].animations.size() == 2);
    CHECK(!bb.sprites[0].animations[1].loop && bb.sprites[0].animations[0].steps[1].ticks == 4 && bb.sprites[0].animations[0].basicStep == "z");
    CHECK(toJson(bb) == toJson(aa));
  }
  d = exportDasm(walker);
  CHECK(d.find("walker_P0_FramePtrLo\n        .byte <walker_P0_Frame0,<walker_P0_Frame1,<walker_P0_Frame2\n") != std::string::npos);
  CHECK(d.find("walker_walk_FRAME\n        .byte 0,1,2,1\nwalker_walk_TICKS\n        .byte 8,4,8,4\n") != std::string::npos &&
        d.find("walker_idle_LOOP = 0") != std::string::npos && d.find("walker_FRAMES = 3") != std::string::npos);
  imp = Asset();
  CHECK(importText(d, &imp, &what) && imp.sprites.size() == 1);
  if (imp.sprites.size() == 1) {
    auto& w = imp.sprites[0];
    CHECK(w.frames == 3 && w.layers.size() == 2 && w.animations.size() == 2);
    for (auto& an : w.animations) {
      if (an.name == "walk") CHECK(an.loop && an.steps.size() == 4 && an.steps[2].frame == 2 && an.steps[3].ticks == 4);
      else CHECK(an.name == "idle" && !an.loop && an.steps.size() == 2 && an.steps[0].ticks == 10);
    }
  }
  // batari Basic: a subroutine that plays it
  CHECK(hasIssue(check(walker, std), Issue::Warning, "walk") && !hasIssue(check(walker, own), Issue::Warning, "variables"));
  walker.animations[0].basicTimer = "y";
  walker.animations[0].basicStep = "z";
  b = exportBasicAnimation(walker, walker.animations[0], std);
  CHECK(b.find("  dim walker_walk_t = y\n  dim walker_walk_s = z\n  data walker_walk_ticks\n  8, 4, 8, 4\nend\n") != std::string::npos);
  CHECK(b.find("  on walker_walk_s goto walker_walk_f0 walker_walk_f1 walker_walk_f2 walker_walk_f1\n") != std::string::npos);
  CHECK(b.find("\nwalker_walk_f2\n") != std::string::npos && b.find("  if walker_walk_s >= 4 then walker_walk_s = 0\n") != std::string::npos);
  CHECK(exportBasicAnimation(walker, walker.animations[1], std).find("then walker_idle_s = 1\n") != std::string::npos);
  {
    Asset aa;
    aa.sprites = {walker};
    std::string reg = basicRegion("w.vcsgfx", aa, 0, std);
    CHECK(reg.find("  rem vcsgfx w.vcsgfx walker animation walk\n") == 0 && reg.find("  rem vcsgfx w.vcsgfx walker animation idle\n") != std::string::npos);
    int n = 0;
    std::vector<std::string> probs;
    std::string prog = "  rem vcsgfx w.vcsgfx walker animation walk\n  rem vcsgfx end\n  rem vcsgfx w.vcsgfx walker animation run\n  rem vcsgfx end\n";
    std::string up = updateBasicRegions(prog, "w.vcsgfx", aa, std, &n, &probs);
    CHECK(n == 1 && probs.size() == 1 && up.find("walker_walk_show") != std::string::npos && up.find("animation run\n  rem vcsgfx end") != std::string::npos);
  }

  // C: arrays top row first, the animations as steps, one header per .vcsgfx
  Asset ca;
  ca.sprites = {sp, walker};
  ca.playfields = {pf, p2};
  std::string c = cInclude(ca, "my gfx.vcsgfx");
  CHECK(c.find("from my gfx.vcsgfx") != std::string::npos && c.find("#ifndef MY_GFX_VCSGFX_H") != std::string::npos);
  CHECK(c.find("#define HERO_FRAMES") != std::string::npos && c.find("static const uint8_t hero_P0_gfx[HERO_FRAMES][") != std::string::npos);
  CHECK(c.find("static const uint8_t walker_P0_gfx[WALKER_FRAMES]") != std::string::npos);
  CHECK(c.find("static const vcsgfx_step walker_walk_steps[] = {") != std::string::npos);
  CHECK(c.find("static const uint8_t level_pf0r[LEVEL_ROWS]") != std::string::npos);  // asymmetric rows
  CHECK(exportC(walker).find("0x18,  /* ...XX... ") != std::string::npos);
  CHECK(exportC(sp).find("vcsgfx_animate") == std::string::npos && c.find("vcsgfx_animate") != std::string::npos);
  Profile elf;
  elf.c = true;
  CHECK(elf.describe().find("C") == 0 && !elf.basic());

  // the exported tables assemble (when DASM is installed); the C header compiles (with a C compiler)
  namespace fs = std::filesystem;
  std::string cc = findInPath("cc");
  if (!cc.empty()) {
    fs::path dir = fs::temp_directory_path() / "vcside-gfx-test";
    fs::create_directories(dir);
    Asset other;
    other.sprites = {walker};
    other.sprites[0].name = "other";
    std::ofstream(dir / "a.gfx.h") << c;
    std::ofstream(dir / "b.gfx.h") << cInclude(other, "b.vcsgfx");
    std::ofstream(dir / "g.c") << "#include \"a.gfx.h\"\n#include \"a.gfx.h\"\n#include \"b.gfx.h\"\n"
                                  "int f(void) { vcsgfx_player p = {0}; vcsgfx_play(&p, &walker_walk);\n"
                                  "  return vcsgfx_animate(&p) + hero_P0_gfx[0][0] + level_pf1[0] + other_P0_gfx[0][0]; }\n";
    std::string cmd = "\"" + cc + "\" -std=c11 -Wall -Wextra -Werror -fsyntax-only \"" + (dir / "g.c").string() + "\" > \"" +
                      (dir / "c.log").string() + "\" 2>&1";
    int rc = std::system(cmd.c_str());
    CHECK(rc == 0);
    if (rc != 0) {
      std::ifstream log(dir / "c.log");
      std::fputs(std::string(std::istreambuf_iterator<char>(log), {}).c_str(), stderr);
    }
  }
  std::string dasm = findInPath("dasm");
  if (!dasm.empty()) {
    fs::path dir = fs::temp_directory_path() / "vcside-gfx-test";
    fs::create_directories(dir);
    std::ofstream(dir / "g.asm") << "    processor 6502\n    org $F000\n" << exportDasm(sp) << exportDasm(p2) << exportDasm(pf) << exportDasm(walker)
                                 << "    org $FFFC\n    .word $F000\n    .word $F000\n";
    std::string cmd = "\"" + dasm + "\" \"" + (dir / "g.asm").string() + "\" -f3 -o\"" + (dir / "g.bin").string() + "\" > \"" +
                      (dir / "g.log").string() + "\" 2>&1";
    int rc = std::system(cmd.c_str());
    CHECK(rc == 0);
    if (rc != 0) {
      std::ifstream log(dir / "g.log");
      std::fputs(std::string(std::istreambuf_iterator<char>(log), {}).c_str(), stderr);
    }
  }
}

// The template's 2600.h: the register names of the Stella Programmer's Guide and no others,
// and an empty DASM project assembles with it.
static void testHeader(const KnowledgeBase& kb, const std::string& data) {
  namespace fs = std::filesystem;
  std::ifstream h(data + "/templates/dasm/2600.h");
  std::string text((std::istreambuf_iterator<char>(h)), {});
  CHECK(!text.empty());
  std::set<std::string> header, guide;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    std::smatch m;
    static const std::regex label(R"(^([A-Z][A-Z0-9_]*)\s+ds\s+1\b)");
    if (std::regex_search(line, m, label)) header.insert(m[1].str());
  }
  for (auto& r : kb.registers())  // registers, not areas such as RAM ($80-$FF)
    if (std::find(r.sources.begin(), r.sources.end(), "stella-guide") != r.sources.end() && r.address.find('-') == std::string::npos)
      guide.insert(r.name);
  CHECK(header.size() == 68 && header == guide);
  for (auto& n : header)
    if (!guide.count(n)) std::fprintf(stderr, "2600.h: %s is not a Stella guide name\n", n.c_str());
  for (auto& n : guide)
    if (!header.count(n)) std::fprintf(stderr, "2600.h: %s is missing\n", n.c_str());
  CHECK(text.find("TIMINT") == std::string::npos && text.find("TIA_BASE") == std::string::npos);
  // a new DASM project
  fs::path dir = fs::temp_directory_path() / "vcside-test-2600h";
  std::error_code ec;
  fs::remove_all(dir, ec);
  Project p;
  std::string err;
  CHECK(Project::create(dir.string(), "plain", Language::Dasm, "NTSC", &p, &err));
  CHECK(fs::exists(dir / "2600.h") && !fs::exists(dir / "hardware.h"));
  std::string dasm = findInPath("dasm");
  if (!dasm.empty()) {
    std::string cmd = "cd \"" + dir.string() + "\" && \"" + dasm + "\" main.asm -f3 -oplain.bin > dasm.log 2>&1";
    CHECK(std::system(cmd.c_str()) == 0 && fs::file_size(dir / "plain.bin", ec) == 4096);
  }
  fs::remove_all(dir, ec);
}

static void testThemes(const std::string& data) {
  using namespace vcs::theme;
  namespace fs = std::filesystem;
  Rgba c = 0;
  CHECK(parseColour("#268BD2", &c) && c == 0x268BD2FF && parseColour("#268bd240", &c) && c == 0x268BD240);
  CHECK(!parseColour("268BD2", &c) && !parseColour("#12345", &c) && !parseColour("#GG0000", &c));
  CHECK(colourText(0x268BD2FF) == "#268BD2" && colourText(0x268BD240) == "#268BD240");
  Role r;
  CHECK(roleFromId("syntax.comment", &r) && r == Role::SynComment && !roleFromId("syntax.nope", &r));

  // the built-in themes: every colour, nothing that cannot be read
  fs::path user = fs::temp_directory_path() / "vcside-test-themes";
  std::error_code ec;
  fs::remove_all(user, ec);
  Library lib;
  lib.load(data + "/themes", user.string());
  for (const char* id : {"dark", "light", "solarized-light", "solarized-dark"}) {
    const Theme* t = lib.find(id);
    CHECK(t && t->builtIn && !t->name.empty());
    Resolved res = lib.resolve(id);
    for (auto& p : res.problems) std::fprintf(stderr, "theme %s: %s\n", id, p.c_str());
    CHECK(res.problems.empty() && !res.indent.empty());
  }
  // Solarized Dark is Solarized Light with the palette turned round
  Resolved sl = lib.resolve("solarized-light"), sd = lib.resolve("solarized-dark");
  CHECK(sl.colour[(int)Role::EdBackground] == 0xFDF6E3FF && sd.colour[(int)Role::EdBackground] == 0x002B36FF);
  CHECK(sl.colour[(int)Role::SynMnemonic] == sd.colour[(int)Role::SynMnemonic]);  // the accents stay
  CHECK(sl.colour[(int)Role::UiSelection] == 0x268BD240);                         // "$blue/40"
  CHECK(lib.resolve("dark").colour[(int)Role::SynSymbol] == lib.resolve("dark").colour[(int)Role::EdText]);  // "@editor.text"

  // a theme of one's own: a base, a palette entry, references, alpha, a loop
  Theme mine;
  std::string err;
  CHECK(fromJson(R"({"vcstheme":1,"name":"Mine","base":"solarized-light","palette":{"blue":"#FF0000","paper":"#FFFFFF"},
                      "colours":{"editor.background":"$paper","ui.accent":"@editor.background/80"},"indent":[]})", &mine, &err));
  CHECK(lib.save(mine, &err) && mine.id == "mine" && fs::exists(user / "mine.vcstheme"));
  Resolved m = lib.resolve("mine");
  CHECK(m.problems.empty() && m.colour[(int)Role::EdBackground] == 0xFFFFFFFF && m.colour[(int)Role::UiAccent] == 0xFFFFFF80);
  // A full copy with no base, missing colours added later: they come from Dark or Light.
  Theme old;
  CHECK(fromJson(R"({"vcstheme":1,"name":"Old","colours":{"ui.background":"#FAFAFA"}})", &old, &err) && lib.save(old, &err));
  Resolved o = lib.resolve(old.id), light = lib.resolve("light");
  CHECK(o.problems.empty() && o.notes.size() == 1 && o.colour[(int)Role::FilesAsm] == light.colour[(int)Role::FilesAsm]);
  CHECK(lib.remove(old.id, &err));
  for (const char* b : {"dark", "light", "solarized-light", "solarized-dark"}) CHECK(lib.resolve(b).notes.empty() && lib.resolve(b).problems.empty());
  CHECK(m.colour[(int)Role::SynMnemonic] == 0xFF0000FF);  // the base's "$blue" is now this palette's
  CHECK(m.indent.empty());                                // an empty list: no shading
  CHECK(lib.resolveSpec("mine", "$blue/20", &c) && c == 0xFF000020 && !lib.resolveSpec("mine", "$nope", &c));
  Theme loop;
  CHECK(fromJson(R"({"vcstheme":1,"name":"Loop","base":"mine","colours":{"ui.text":"@ui.textDim","ui.textDim":"@ui.text","editor.nope":"#000"}})", &loop, &err));
  CHECK(lib.save(loop, &err));
  Resolved lr = lib.resolve("loop");
  CHECK(lr.problems.size() == 3 && lr.colour[(int)Role::UiText] == 0xFF00FFFF);  // two that refer to each other, one unknown
  CHECK(!fromJson("{\"name\":\"x\"}", &loop, &err) && !err.empty());

  // sharing: an exported theme carries its own bases up to a built-in one
  Theme child;
  CHECK(fromJson(R"({"vcstheme":1,"name":"Child","base":"mine","colours":{"ui.text":"#010203"}})", &child, &err) && lib.save(child, &err));
  Theme out;
  CHECK(fromJson(lib.exportJson("child"), &out, &err));
  CHECK(out.base == "solarized-light" && out.palette["blue"] == "#FF0000" && out.colours["ui.text"] == "#010203" &&
        out.colours["editor.background"] == "$paper" && out.hasIndent && out.indent.empty());
  // importing never overwrites: the same file twice gives two themes
  std::ofstream(user / "shared.vcstheme.tmp") << lib.exportJson("child");
  fs::path file = fs::temp_directory_path() / "Child.vcstheme";
  fs::copy_file(user / "shared.vcstheme.tmp", file, fs::copy_options::overwrite_existing, ec);
  std::string id1, id2;
  CHECK(lib.import(file.string(), &id1, &err) && lib.import(file.string(), &id2, &err) && id1 != id2 && id1 != "child");
  Resolved imported = lib.resolve(id1);
  CHECK(imported.problems.empty() && imported.colour[(int)Role::UiText] == 0x010203FF && imported.colour[(int)Role::EdBackground] == 0xFFFFFFFF);
  // a user theme with a built-in one's id is not possible through save, and built-in ones cannot be deleted
  CHECK(!lib.remove("dark", &err) && lib.remove(id2, &err) && !lib.find(id2));
  CHECK(lib.uniqueId("Solarized Light") == "solarized-light-2" && lib.uniqueId("  ") == "theme");
  CHECK(lib.uniqueId("Claro, mío") == "claro-mio" && lib.uniqueId("Año ñu — 2") == "ano-nu-2");
  // what the user folder holds is read back
  Library again;
  again.load(data + "/themes", user.string());
  CHECK(again.find("mine") && !again.find("mine")->builtIn && again.find(id1));
  fs::remove_all(user, ec);
  fs::remove(file, ec);
}

int main() {
  KnowledgeBase kb;
  std::string err;
  // VCSIDE_TEST_DATA: the data folder when the tests run elsewhere (e.g. a Windows build in Wine)
  const char* env = std::getenv("VCSIDE_TEST_DATA");
  const std::string data = env && *env ? env : VCSIDE_SOURCE_DATA;
  kb.load(data + "/kb", &err);
  if (!err.empty()) std::fprintf(stderr, "knowledge base: %s", err.c_str());
  CHECK(err.empty());
  testKb(kb);
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
  testThemes(data);
  testHeader(kb, data);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
