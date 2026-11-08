// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// Tests without GUI: knowledge base, cycle counter, output parsers.
#include <cstdio>
#include <string>

#include "builder_parse.h"
#include "editor/text_editor.h"
#include "cycles.h"
#include "kb.h"
#include "space.h"

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

static void testParsers() {
  auto d = parseDasmOutput(
      "--- Unresolved Symbol List\nUnknown                  0000 ????         (R )\n--- 1 Unresolved Symbol \n\n"
      "t.asm (4): error: Unknown Mnemonic 'foo'.\n\nFatal assembly error: Source is not resolvable.\n");
  CHECK(d.size() == 3);
  CHECK(d.size() == 3 && d[1].file == "t.asm" && d[1].line == 4 && d[1].severity == Diagnostic::Error);
  CHECK(d.size() == 3 && d[0].message.find("Unknown") != std::string::npos);

  auto g = parseGccOutput("src/main.c:12:5: warning: unused variable 'x'\nsrc/a.c:3: error: boom\n");
  CHECK(g.size() == 2 && g[0].line == 12 && g[0].severity == Diagnostic::Warning && g[1].file == "src/a.c");

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

int main() {
  KnowledgeBase kb;
  std::string err;
  kb.load(VCSIDE_SOURCE_DATA "/kb", &err);
  if (!err.empty()) std::fprintf(stderr, "knowledge base: %s", err.c_str());
  CHECK(err.empty());
  testKb(kb);
  testCycles(kb);
  testParsers();
  testEditor();
  testSpace(kb);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
