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
#include "debugmap.h"
#include "emulator_launch.h"
#include "i18n.h"

#include <filesystem>
#include <fstream>
#include <set>

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
  CHECK(has(a, {"-snapsavedir", (fs::path("/tmp/proj/build") / "").string()}));
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
  testDebugMap();
  loadLanguages(VCSIDE_SOURCE_DATA "/i18n");
  testEmulatorOptions(kb);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
