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
#include "pluscart_server.h"
#include "emulator_launch.h"
#include "i18n.h"
#include "gfx.h"
#include "theme.h"
#include "child_process.h"
#include "music/tia_audio.h"
#include "music/tuning.h"
#include "music/song.h"
#include "music/player.h"
#include "music/export.h"
#include "music/import_tiatracker.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "cpu6502.h"
#include "assistant/engine.h"
#include "assistant/grounding.h"
#include "assistant/http.h"
#include "assistant/markdown.h"
#include "assistant/tools.h"
#include "assistant/chat_tabs.h"
#include "assistant/project_index.h"
#include "assistant/embeddings.h"
#include "assistant/context_items.h"
#include "assistant/edits.h"
#include "file_operations.h"
#include "assistant/domain_checks.h"
#include "assistant/validate.h"
#include "assistant/first_pass.h"
#include "assistant/mask.h"
#include "assistant/model_list.h"
#include "assistant/tool_schema.h"
#include <imgui.h>
#include "assistant/secrets.h"
#include "assistant/models.h"
#include "assistant/prompt.h"
#include "assistant/provider.h"
#include "doc_markdown.h"
#include "assistant/settings.h"
#include "assistant/sha256.h"
#include "assistant/sse.h"
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#endif
#include <random>

#include <algorithm>
#include <array>
#include <cmath>
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

// Where a new project keeps a file: programs in src/, .vcsgfx and .vcsmusic in resources/gfx
// and resources/music, what they export in generated/gfx and generated/music. A C project's
// "src/<file>" is taken the same way; other folders' files are where they are named.
static std::filesystem::path inProject(const std::filesystem::path& project, const std::string& name) {
  std::string base = name;
  if (name.rfind("src/", 0) == 0) base = name.substr(4);
  else if (name.find('/') != std::string::npos) return project / name;
  auto ends = [&](const std::string& suffix) { return base.size() >= suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0; };
  if (ends(".vcsgfx")) return project / "resources" / "gfx" / base;
  if (ends(".vcsmusic")) return project / "resources" / "music" / base;
  if (ends(".gfx.asm") || ends(".gfx.h")) return project / "generated" / "gfx" / base;
  if (base == "music.asm" || base == "music.h") return project / "generated" / "music" / base;
  for (const char* source : {".asm", ".h", ".c", ".S", ".bas"})
    if (ends(source)) return project / "src" / base;
  return project / name;
}

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
  const CartFormat* ace = kb.format("ACE");
  CHECK(ace && ace->category == "arm" && ace->stella == "none" && !ace->sources.empty());
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
  {  // RAM variables (DS in SEG.U, as DASM 2.20 lists them): their address and size, apart from
     // the ROM's lines; an equate in the segment reserves nothing
    const std::string ramListing =
        "------- FILE main.asm LEVEL 1 PASS 1\n"
        "      2 U0000 ????\t\t\t\t      SEG.U\tVars\n"
        "      3 U0080\t\t\t\t\t      ORG\t$80\n"
        "      4 U0080\t\t       00\t   System     ds\t1\n"
        "      5 U0081\t\t       00 00 00 00 Buf\t      ds\t4\n"
        "      6 U0085\t\t       00 00 00 00*Big\t      ds\t12\n"
        "      7 U0085\t\t       00 03\t   SIZE       =\t3\n"
        "      8 U0091\t\t       00 00 00    Tbl\t      ds\tSIZE\n"
        "      9 U0094\t\t\t\t   Shared\n"
        "     10 U0094\t\t       00 00\t   Title      ds\t2\n"
        "     11 U0096\t\t\t\t  -\t      ELSE\n"
        "     11 U0096\t\t\t\t  -Unused     ds\t1\n"
        "     11 U0096\t\t\t\t\t      ENDIF\n"
        "     12 U0094\t\t\t\t\t      ORG\tShared\n"
        "     13 U0094\t\t       00\t   Enemy      ds\t1\n"
        "     14  0000 ????\t\t\t\t      SEG\tCode\n"
        "     15  f000\t\t\t\t\t      ORG\t$F000\n"
        "     16  f000\t\t       a5 80\t\t      lda\tSystem\n";
    AddressMap ramMap;
    ramMap.load(ramListing, [](const std::string&) -> std::optional<int> { return std::nullopt; }, 4096);
    auto listedRam = ramMap.reservedIn("main.asm");
    std::vector<const ListedLine*> reserved, markers;  // variables; labels and ORGs (no bytes)
    for (auto* listed : listedRam) (listed->bytes > 0 ? reserved : markers).push_back(listed);
    CHECK(reserved.size() == 6 && ramMap.linesIn("main.asm").size() == 1);
    // ORG $80, the label Shared and ORG Shared, back over Title (ENDIF is no label; Unused, in the branch not taken, is skipped)
    CHECK(markers.size() == 3 && markers[0]->line == 3 && markers[0]->cpu == 0x80);
    CHECK(markers[1]->line == 9 && markers[1]->cpu == 0x94 && markers[2]->line == 12 && markers[2]->cpu == 0x94);
    CHECK(reserved[4]->cpu == 0x94 && reserved[4]->bytes == 2 && reserved[5]->line == 13 && reserved[5]->cpu == 0x94);
    CHECK(reserved[0]->line == 4 && reserved[0]->cpu == 0x80 && reserved[0]->bytes == 1);
    CHECK(reserved[1]->cpu == 0x81 && reserved[1]->bytes == 4 && reserved[2]->cpu == 0x85 && reserved[2]->bytes == 12);
    CHECK(reserved[3]->line == 8 && reserved[3]->cpu == 0x91 && reserved[3]->bytes == 3);  // SIZE unknown here: the bytes listed
  }
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
  {  // a second bank in the same file: a bank separator, not just a new page
    std::vector<std::string> source(23, "  nop");
    int bankBreaks = 0;
    for (const PageBreak& pageBreak : pageBreaks(map, "main.asm", source))
      if (pageBreak.newBank) {
        bankBreaks++;
        CHECK(pageBreak.bank == 1 && pageBreak.address == 0xF000);
      }
    CHECK(bankBreaks == 1);
  }
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
    CHECK(!breaks[0].newBank && !breaks[1].newBank && !breaks[2].newBank);  // one bank
    CHECK(breaks[1].line == 6 && breaks[1].address == 0xF200 && !breaks[1].inside);  // above the ORG and the label Next
    CHECK(breaks[2].line == 10 && breaks[2].address == 0xF300 && breaks[2].inside);  // the ds goes on into $F300
  }

  {  // the vectors: the rule goes above the ORG, across the blank line after it
    std::string vectorsListing =
        "------- FILE v.asm LEVEL 1 PASS 2\n"
        "      1  f000\t\t\t\t\t      ORG\t$F000\n"
        "      2  f000\t\t       ea\t\t      nop\n"
        "      3  f001\t\t\t\t\t      ; Interrupt vectors\n"
        "      4  f001\n"
        "      5  fffa\t\t\t\t\t      ORG\t$FFFA\n"
        "      6  fffa\n"
        "      7  fffa\t\t       00 f0\t\t      .word\tStart\n";
    AddressMap vectorsMap;
    vectorsMap.load(vectorsListing, [](const std::string&) -> std::optional<int> { return std::nullopt; }, 4096);
    std::vector<std::string> vectorsSource = {"  ORG $F000", "  nop", "; Interrupt vectors", "", "  ORG $FFFA", "", "  .word Start"};
    auto vectorBreaks = pageBreaks(vectorsMap, "v.asm", vectorsSource);
    CHECK(vectorBreaks.size() == 1 && vectorBreaks[0].line == 4 && vectorBreaks[0].address == 0xFF00);
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

// The PlusCart's requests, as its firmware writes them (esp8266.c): the menu, then a ROM in
// ranges on one keep-alive connection.
static void testPlusCartServer() {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "vcside-test-pluscart";
  std::error_code errorCode;
  fs::create_directories(dir, errorCode);
  std::string rom(40000, '\0');
  for (size_t index = 0; index < rom.size(); index++) rom[index] = (char)(index * 7);
  std::ofstream(dir / "my game.ace", std::ios::binary) << rom;
  std::vector<ServedRom> roms = {{"my game.ace", (dir / "my game.ace").string()}};
  CHECK(decodePlusStorePath("/my%20game.ace") == "/my game.ace" && decodePlusStorePath("/a+b%2B") == "/a b+");
  const std::string headers = " HTTP/1.0\r\nHost: 192.168.1.20\r\nPlusStore-ID: v2.3.32 0123456789abcdef01234567 PlusCart(+)\r\nClient-Conf: 5,0,0,1,1, 9\r\nConnection: keep-alive\r\n";
  PlusStoreRequest request = parsePlusStoreRequest("GET /api.php?p=" + headers + "\r\n");
  CHECK(request.valid && request.path.empty() && !request.hasRange);
  // the menu: status line, then type 2 (a ROM file), the size in 6 digits from index 2, the name from index 9
  std::string menu = plusStoreMenu(roms, "", "VCSIDE: game");
  CHECK(menu == "00VCSIDE: game\n2 040000 my game.ace\n");
  CHECK(plusStoreMenu(roms, "/Somewhere", "") == "00\n0 000000 ..\n");
  request = parsePlusStoreRequest("GET /api.php?p=/my%20game.ace" + headers + "Range: bytes=32768-39999\r\n\r\n");
  CHECK(request.valid && request.path == "/my game.ace" && request.hasRange && request.first == 32768 && request.last == 39999);
  std::string logLine;
  std::string response = plusStoreResponse(request, roms, "", &logLine);
  CHECK(response.find("206") != std::string::npos && response.find("Content-Length: 7232\r\n") != std::string::npos && logLine.empty());
  CHECK(response.substr(response.find("\r\n\r\n") + 4) == rom.substr(32768));
  request.first = 0, request.last = 32767;
  plusStoreResponse(request, roms, "", &logLine);
  CHECK(logLine.find("my game.ace") != std::string::npos);  // the first range: it is being downloaded
  request.path = "/other.bin";
  CHECK(plusStoreResponse(request, roms, "", nullptr).find("416") != std::string::npos);
  CHECK(!parsePlusStoreRequest("GET /index.html HTTP/1.0\r\n\r\n").valid);
#ifndef _WIN32
  // a real connection on the loopback, with the requests one after another
  PlusCartServer server;
  std::string error;
  CHECK(server.start(0, &error) && server.port() > 0);
  server.setRoms(roms, "VCSIDE: game");
  int client = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)server.port());
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  CHECK(connect(client, (sockaddr*)&address, sizeof address) == 0);
  std::string requests = "GET /api.php?p=" + headers + "\r\n";
  requests += "GET /api.php?p=/my%20game.ace" + headers + "Range: bytes=0-32767\r\n\r\n";
  requests += "GET /api.php?p=/my%20game.ace" + headers + "Range: bytes=32768-39999\r\n\r\n";
  CHECK(send(client, requests.data(), requests.size(), 0) == (ssize_t)requests.size());
  std::string received;
  char buffer[8192];
  auto complete = [&received] {  // three answers, each its header and Content-Length bytes
    size_t position = 0;
    for (int answer = 0; answer < 3; answer++) {
      size_t headerEnd = received.find("\r\n\r\n", position), lengthAt = received.find("Content-Length: ", position);
      if (headerEnd == std::string::npos || lengthAt == std::string::npos) return false;
      position = headerEnd + 4 + std::stoul(received.substr(lengthAt + 16));
      if (position > received.size()) return false;
    }
    return true;
  };
  for (int attempt = 0; attempt < 500 && !complete(); attempt++) {
    ssize_t count = recv(client, buffer, sizeof buffer, 0);
    if (count <= 0) break;
    received.append(buffer, (size_t)count);
  }
  close(client);
  CHECK(complete() && received.find("2 040000 my game.ace\n") != std::string::npos);
  size_t firstBody = received.find("\r\n\r\n", received.find("206")) + 4;
  CHECK(received.compare(firstBody, 32768, rom, 0, 32768) == 0);
  CHECK(received.size() >= 7232 && received.compare(received.size() - 7232, 7232, rom, 32768, 7232) == 0);
  server.stop();
  CHECK(!server.running());
  auto log = server.takeLog();
  CHECK(log.size() == 2);  // the menu, and the ROM once
#endif
  fs::remove_all(dir, errorCode);
}

static void testElfSpace() {
  // PROGBITS 1, NOBITS 8; WRITE 1, ALLOC 2, EXEC 4
  std::string rom = makeElf(1, {{".text", 1, 6, 1000}, {".RamFunc", 1, 6, 300}, {".rodata.tab", 1, 2, 2000},
                                {".data.x", 1, 3, 40}, {".bss.y", 8, 3, 500}, {".comment", 1, 0, 19}}, {});
  ElfFile romElf;
  std::string error;
  CHECK(readElf(rom, romElf, &error) && romElf.type == 1 && romElf.machine == 40 && romElf.sections.size() == 8);
  CHECK(romElf.sections.size() == 8 && romElf.sections[2].name == ".RamFunc");
  ElfLimits chameleon = elfLimits("chameleon", 0, 0);
  CHECK(chameleon.cartridge == ElfCartridge::Chameleon && chameleon.flash == kChameleonFlash && chameleon.ram == kChameleonRam);
  ElfSpace elfSpace = analyzeElfSpace(romElf, (long)rom.size(), nullptr, chameleon, 90);
  CHECK(elfSpace.code == 1000 && elfSpace.ramCode == 300 && elfSpace.constants == 2000 && elfSpace.data == 40 && elfSpace.bss == 500 && !elfSpace.exact);
  CHECK(elfSpace.flash.used == 3340 + kChameleonFirmwareFlash && elfSpace.ram.used == 840 + kChameleonFirmwareRam);
  CHECK(elfSpace.diagnostics.empty());
  // the Chameleon image: flash = what is stored at 0x0800_0000, RAM = what lives at 0x2000_0000
  std::string img = makeElf(2, {}, {{0x08000000, 0x08000000, 120000, 120000}, {0x20000000, 0x0801D4C0, 4000, 34000},
                                   {0x20008500, 0x0801E460, 0, 1100}});
  ElfFile imageElf;
  CHECK(readElf(img, imageElf, &error) && imageElf.type == 2 && imageElf.segments.size() == 3);
  elfSpace = analyzeElfSpace(romElf, (long)rom.size(), &imageElf, chameleon, 90);
  CHECK(elfSpace.exact && elfSpace.flash.used == 124000 && elfSpace.ram.used == 35100);
  CHECK(elfSpace.diagnostics.size() == 2 && elfSpace.diagnostics[0].severity == Diagnostic::Warning);  // 94 % and 95 %
  // the 512 KB model, set in the project: the same image fits easily
  ElfLimits bigChameleon = elfLimits("chameleon", 512, 144);
  CHECK(bigChameleon.flash == 512 * 1024 && bigChameleon.ram == 144 * 1024);
  elfSpace = analyzeElfSpace(romElf, (long)rom.size(), &imageElf, bigChameleon, 90);
  CHECK(elfSpace.diagnostics.empty() && elfSpace.ccm.size == 0);
  // PlusCart / UnoCart: a small file is read whole into the 96 KB buffer, the zeroed variables go to CCM RAM
  ElfLimits plusCart = elfLimits("pluscart", 0, 0);
  CHECK(plusCart.cartridge == ElfCartridge::PlusCart && plusCart.ram == kPlusCartBuffer && defaultElfFlashKb("pluscart") == 0);
  elfSpace = analyzeElfSpace(romElf, 5000, &imageElf, plusCart, 90);  // the Chameleon image is not used
  CHECK(elfSpace.wholeFile && elfSpace.ram.used == 5000 && elfSpace.ccm.used == 500 && elfSpace.ccm.size == kPlusCartCcm && elfSpace.flash.size == 0);
  CHECK(elfSpace.diagnostics.empty() && elfSpace.summary().find("PlusCart") != std::string::npos);
  // a bigger file goes to flash: code, .data and the constants with relocations are copied to the buffer
  std::string relocated = makeElf(1, {{".text", 1, 6, 1000}, {".RamFunc", 1, 6, 302}, {".rodata.tab", 1, 2, 2000}, {".rodata.font", 1, 2, 700},
                                      {".rel.rodata.tab", 9, 0x40, 16}, {".data.x", 1, 3, 40}, {".bss.y", 8, 3, 501}}, {});
  CHECK(readElf(relocated, romElf, &error));
  elfSpace = analyzeElfSpace(romElf, 200000, nullptr, plusCart, 90);
  CHECK(!elfSpace.wholeFile && elfSpace.relocatedConstants == 2000 && elfSpace.constants == 2700);
  CHECK(elfSpace.ram.used == 1000 + 304 + 2000 + 40 && elfSpace.ccm.used == 504);
  elfSpace = analyzeElfSpace(romElf, 200000, nullptr, elfLimits("pluscart", 0, 3), 90);  // a 3 KB buffer
  CHECK(elfSpace.diagnostics.size() == 1 && elfSpace.diagnostics[0].severity == Diagnostic::Error);
  CHECK(!readElf("not an elf", romElf, &error) && !error.empty());

  // ACE: the debug ELF's sections by address: the file's header and start code, then the
  // program in SRAM after the firmware's arguments (0x100), against the 96 KB buffer
  ElfFile aceElf;
  aceElf.type = 2;
  aceElf.sections = {{".ace_header", 1, 2, 0x08020000, 40}, {".ace_start", 1, 6, 0x08020028, 60}, {".text", 1, 6, 0x20000100, 3000},
                     {".rodata", 1, 2, 0x20000CB8, 800}, {".data", 1, 3, 0x20000FD8, 40}, {".bss", 8, 3, 0x20001000, 1200},
                     {".debug_info", 1, 0, 0, 9000}};
  ElfSpace aceSpace = analyzeAceSpace(aceElf, 3940, 90);
  CHECK(aceSpace.cartridge == ElfCartridge::Ace && aceSpace.startCode == 100 && aceSpace.code == 3000 && aceSpace.constants == 800);
  CHECK(aceSpace.data == 40 && aceSpace.bss == 1200 && aceSpace.ram.used == 0x1000 + 1200 && aceSpace.ram.size == kPlusCartBuffer);
  CHECK(aceSpace.flash.used == 3940 && aceSpace.flash.size == kAceFileLimit && aceSpace.emulatorRam.size == 128000);
  CHECK(aceSpace.diagnostics.empty() && aceSpace.summary().find("ACE") != std::string::npos);
  CHECK(gopherAceSram(63999) == 128000 && gopherAceSram(64000) == 256000 && gopherAceSram(447999) == 512000 && gopherAceSram(448000) == 64000);
  // a big file: Gopher2600 gives it 64,000 bytes of SRAM (a warning); 90 KB of SRAM is 93 % of the buffer
  aceElf.sections[5].size = 90 * 1024 - 0x1000;
  aceSpace = analyzeAceSpace(aceElf, 500000, 90);
  CHECK(aceSpace.diagnostics.size() == 2 && aceSpace.diagnostics[0].severity == Diagnostic::Warning && aceSpace.diagnostics[1].message.find("Gopher2600") != std::string::npos);
  aceSpace = analyzeAceSpace(aceElf, 1000 * 1024, 90);  // over the firmware's 896 KB
  CHECK(aceSpace.diagnostics[0].severity == Diagnostic::Error);

  // a new ELF project from the template
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "vcside-test-elf";
  std::error_code errorCode;
  fs::remove_all(dir, errorCode);
  Project project;
  CHECK(Project::create(dir.string(), "rainbow", Language::ElfC, "NTSC", &project, &error));
  CHECK(project.buildCommand == "make" && project.format == "ELF" && project.output == "build/rainbow.bin" && project.main == "src/main.c");
  CHECK(project.elfCartridge == "chameleon" && project.toJson().find("\"elf_cartridge\": \"chameleon\"") != std::string::npos);
  project.elfCartridge = "pluscart", project.elfFlashKb = 512, project.elfRamKb = 64;
  {
    Project reread;
    CHECK(reread.fromJson(project.toJson(), project.file, &error));
    CHECK(reread.elfCartridge == "pluscart" && reread.elfFlashKb == 0 && reread.elfRamKb == 64 && reread.otherKeys.empty());
  }
  {  // the project's folders: kept in the .vcsproj; exports go to generated/<kind>, with the
     // subfolder of resources/<kind> they come from; none set: next to the file
    CHECK(project.sourceDir == "src" && project.resourcesDir == "resources" && project.generatedDir == "generated" && project.buildDir == "build");
    Project reread;
    CHECK(reread.fromJson(project.toJson(), project.file, &error) && reread.generatedDir == "generated" && reread.buildDir == "build" && reread.otherKeys.empty());
    CHECK(fs::path(project.generatedFolder((dir / "resources" / "music" / "theme.vcsmusic").string(), "music")) == (dir / "generated" / "music").lexically_normal());
    CHECK(fs::path(project.generatedFolder((dir / "resources" / "gfx" / "level1" / "a.vcsgfx").string(), "gfx")) == (dir / "generated" / "gfx" / "level1").lexically_normal());
    CHECK(project.generatedFolder("/elsewhere/a.vcsgfx", "gfx") == "/elsewhere");
    reread.generatedDir.clear();
    CHECK(reread.generatedFolder((dir / "resources" / "gfx" / "a.vcsgfx").string(), "gfx") == (dir / "resources" / "gfx").string());
    CHECK(fs::exists(dir / ".gitignore"));
  }
  for (auto fileName : {"Makefile", "include/vcsLib.h", "src/main.c", "src/memcpy_long.S", "chameleon/support.c", "src/theme.vcsmusic"}) CHECK(fs::exists(inProject(dir, fileName)));
  {  // its music, exported as the C player (the editor writes it on every save)
    std::ifstream musicHeader(inProject(dir, "src/music.h"));
    std::string musicText((std::istreambuf_iterator<char>(musicHeader)), {});
    CHECK(musicText.find("#define MUSIC_EFFECT_FIRE 0") != std::string::npos && musicText.find("void music_frame(int lines)") != std::string::npos);
    CHECK(musicText.find("Tuned for NTSC") != std::string::npos);
  }
  CHECK(!fs::exists(dir / "LICENSE.md"));
  std::ifstream makefile(dir / "Makefile");
  std::string makefileText((std::istreambuf_iterator<char>(makefile)), {});
  CHECK(makefileText.find("NAME  = rainbow") != std::string::npos && makefileText.find("{{") == std::string::npos);
  CHECK(!Project::create((dir / "b").string(), "two words", Language::ElfC, "NTSC", nullptr, &error));
  CHECK(Project::create((dir / "bb").string(), "Square Run", Language::BatariBasic, "NTSC", &project, &error));
  CHECK(project.main == "src/main.bas" && project.output == "build/Square Run.bin" && project.format == "AUTO" && project.buildCommand.empty());
  std::ifstream basFile(inProject(dir / "bb", "main.bas"));
  std::string basicText((std::istreambuf_iterator<char>(basFile)), {});
  CHECK(basicText.find("rem Square Run - batari Basic") != std::string::npos && basicText.find("drawscreen") != std::string::npos);
  CHECK(basicText.find("set tv") == std::string::npos);  // NTSC: batari Basic's own timing
  CHECK(basicText.find("inline") == std::string::npos && !fs::exists(inProject(dir / "bb", "theme.vcsmusic")));  // empty: the loop alone
  {  // the square example's music: the song, and the player and tables main.bas inlines (h to z)
    CHECK(Project::create((dir / "bbsquare").string(), "Square Run", Language::BatariBasic, "NTSC", &project, &error, "square"));
    std::ifstream squareFile(inProject(dir / "bbsquare", "main.bas")), musicFile(inProject(dir / "bbsquare", "music.asm"));
    std::string squareText((std::istreambuf_iterator<char>(squareFile)), {}), musicText((std::istreambuf_iterator<char>(musicFile)), {});
    CHECK(fs::exists(inProject(dir / "bbsquare", "theme.vcsmusic")) && squareText.find("include \"generated/music/music.asm\"") != std::string::npos);
    CHECK(musicText.find("MUSIC_RAM = h") != std::string::npos && musicText.find("MUSIC_EFFECT_BLIP") != std::string::npos);
  }
  CHECK(Project::create((dir / "bbpal").string(), "Pal Run", Language::BatariBasic, "PAL", &project, &error));
  std::ifstream palFile(inProject(dir / "bbpal", "main.bas"));
  std::string palText((std::istreambuf_iterator<char>(palFile)), {});
  CHECK(palText.find("\tset romsize 4k\n\tset tv pal\n") != std::string::npos);
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
    // an ACE game: its Makefile names a .ace, so the format is ACE and the emulator Gopher2600;
    // the format is told from the header (ACE-UF00 and the rest) and Stella refuses it
    fs::path aceDir = dir / "ace-made";
    fs::create_directories(aceDir / "build", errorCode);
    std::ofstream(aceDir / "Makefile") << "all: build/game.ace\nbuild/game.ace: main.c\n\tarm-none-eabi-gcc main.c\n";
    std::ofstream(aceDir / "main.c") << "void ace_entry(void) {}\n";
    CHECK(Project::detect(aceDir.string(), &detected) && detected.output == "build/game.ace" && detected.format == "ACE" && detected.emulator == "gopher2600");
    std::ofstream(aceDir / "build" / "game.ace", std::ios::binary) << "ACE-UF00VCSIDE test" << std::string(21, '\0');
    std::ofstream(aceDir / "build" / "old.ace", std::ios::binary) << "ACE-2600" << std::string(32, '\0');
    std::ofstream(aceDir / "build" / "plain.bin", std::ios::binary) << std::string(4096, '\xEA');
    CHECK(romFormat((aceDir / "build" / "game.ace").string()) == "ACE" && romFormat((aceDir / "build" / "old.ace").string()) == "ACE");
    CHECK(romFormat((aceDir / "build" / "plain.bin").string()).empty() && romFormat((aceDir / "missing.ace").string()).empty());
    CHECK(!formatSupported("stella", "ACE") && formatSupported("gopher2600", "ACE") && formatSupported("stella", "ELF"));
    {
      Toolchain emulators;
      emulators.get("gopher2600")->path = "/usr/bin/gopher2600";
      emulators.get("stella")->path = "/usr/bin/stella";
      std::string runError;
      detected.dir = aceDir.string();
      auto gopherRun = detected.runArgs(emulators, false, &runError);
      CHECK(gopherRun.size() >= 4 && gopherRun[1] == "RUN" && gopherRun[2] == "-mapping" && gopherRun[3] == "ACE");
      detected.emulator = "stella";
      CHECK(detected.runArgs(emulators, false, &runError).empty() && runError.find("Gopher2600") != std::string::npos);
    }
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
  CHECK(Project::examples(Language::Dasm).size() == 8 && Project::examples(Language::Dasm)[0].first == "game" &&
        Project::examples(Language::BatariBasic).size() == 3 && Project::examples(Language::BatariBasic, "F4")[0].first == "game");
  // the cartridge of a new project: the examples made for it, and an empty template that
  // assembles to its size (2K and 4K, and F8, F6 and F4 with their banks)
  CHECK(Project::cartridgeFormats(Language::Dasm) == std::vector<std::string>({"2K", "4K", "F8", "F6", "F4", "DPC+", "CDFJ"}));
  CHECK(Project::cartridgeFormats(Language::BatariBasic) == std::vector<std::string>({"4K", "F8", "F6", "F4", "PXE"}));
  // PXE: an ELF project with its own empty program, which neither sets a ROM size nor the TV
  {
    fs::path pxeDir = dir / "pxe-empty";
    CHECK(Project::create(pxeDir.string(), "p", Language::BatariBasic, "PAL", &project, &error, "", "PXE"));
    std::ifstream programFile(inProject(pxeDir, "main.bas"));
    const std::string program((std::istreambuf_iterator<char>(programFile)), std::istreambuf_iterator<char>());
    CHECK(project.format == "ELF" && program.find("set kernel PXE") != std::string::npos &&
          program.find("romsize") == std::string::npos && program.find("set tv") == std::string::npos);
  }
  // PXE's game: pxe.c plays the TIA music from C, the second enemy is drawn "as player2",
  // and the project builds with the PXE beta of batari Basic, then links pxe.c
  {
    CHECK(Project::examples(Language::BatariBasic, "PXE").size() == 2 && Project::examples(Language::BatariBasic, "PXE")[0].first == "game");
    fs::path gameDir = dir / "pxe-game";
    CHECK(Project::create(gameDir.string(), "g", Language::BatariBasic, "NTSC", &project, &error, "game", "PXE"));
    std::ifstream programFile(inProject(gameDir, "main.bas")), musicFile(inProject(gameDir, "music.h"));
    const std::string program((std::istreambuf_iterator<char>(programFile)), std::istreambuf_iterator<char>());
    const std::string music((std::istreambuf_iterator<char>(musicFile)), std::istreambuf_iterator<char>());
    CHECK(fs::exists(inProject(gameDir, "pxe.c")) && music.find("#define MUSIC_TIA 1") != std::string::npos && music.find("music_audio") != std::string::npos);
    CHECK(program.find("set kernel PXE") != std::string::npos && program.find("player2:") != std::string::npos &&
          program.find("player2color:") != std::string::npos && program.find("{{") == std::string::npos);
    CHECK(project.pxe() && project.basicTool() == "bb-pxe" && !project.pxeSource().empty());
    project.toolPaths["bb"] = "/somewhere/2600basic.sh";  // a batari Basic of one's own is used for PXE too
    CHECK(project.basicTool() == "bb");
    Toolchain pxeTools;
    bool listed = false;
    for (auto& tool : pxeTools.tools()) listed = listed || tool.id == "bb-pxe";
    CHECK(listed);
    std::string gcc = findInPath("arm-none-eabi-gcc");
    if (!gcc.empty()) {  // pxe.c and the exported player compile for the cartridge's ARM
      std::string command = "cd \"" + gameDir.string() + "\" && \"" + gcc + "\" -mcpu=cortex-m0 -mthumb -O2 -std=c11 -Wall -Werror -ffreestanding -Igenerated/music -c src/pxe.c -o pxe.o";
      CHECK(std::system(command.c_str()) == 0);
    }
  }
  CHECK(Project::cartridgeFormats(Language::ElfC).empty());
  CHECK(Project::examples(Language::Dasm, "4K").size() == 3 && Project::examples(Language::Dasm, "F8").size() == 2 &&
        Project::examples(Language::Dasm, "2K").size() == 1 && Project::examples(Language::Dasm, "DPC+").size() == 2 &&
        Project::examples(Language::Dasm, "CDFJ").size() == 2 && Project::hasEmptyTemplate(Language::Dasm, "DPC+") &&
        Project::hasEmptyTemplate(Language::Dasm, "CDFJ"));
  CHECK(!Project::create((dir / "anim-f8").string(), "g", Language::Dasm, "NTSC", &project, &error, "animation", "F8") && !error.empty());
  CHECK(!Project::create((dir / "game-2k-4k").string(), "g", Language::Dasm, "NTSC", &project, &error, "game-2k", "4K"));
  // VCSIDE The Game's variants: the switches, the files they leave out, the cartridge's size
  for (auto [example, format, bytes, title] : {std::tuple{"game", "F8", 8192, true}, {"game-notitle", "4K", 4096, false},
                                               {"game-notitle", "F4", 32768, false}, {"game-2k", "2K", 2048, false}}) {
    fs::path variantDir = dir / (std::string("variant-") + example + "-" + format);
    CHECK(Project::create(variantDir.string(), "v", Language::Dasm, "PAL", &project, &error, example, format));
    CHECK(project.format == format && fs::exists(inProject(variantDir, "title.vcsgfx")) == title &&
          fs::exists(inProject(variantDir, "theme.vcsmusic")) == (std::string(example) != "game-2k"));
    std::ifstream sourceFile(inProject(variantDir, "main.asm"));
    std::string sourceText((std::istreambuf_iterator<char>(sourceFile)), {});
    CHECK(sourceText.find("{{") == std::string::npos && sourceText.find(title ? "TITLE_SCREEN   = 1" : "TITLE_SCREEN   = 0") != std::string::npos);
    std::string dasm = findInPath("dasm");
    if (!dasm.empty()) {
      std::string command = "cd \"" + variantDir.string() + "\" && \"" + dasm + "\" src/main.asm -Isrc -Igenerated/gfx -Igenerated/music -f3 -ov.bin > dasm.log 2>&1";
      CHECK(std::system(command.c_str()) == 0 && fs::file_size(variantDir / "v.bin", errorCode) == (uintmax_t)bytes);
    }
  }
  CHECK(!Project::create((dir / "bb-2k").string(), "g", Language::BatariBasic, "NTSC", &project, &error, "", "2K"));
  for (auto [format, bytes] : {std::pair{"2K", 2048}, {"4K", 4096}, {"F8", 8192}, {"F6", 16384}, {"F4", 32768}}) {
    fs::path cartDir = dir / (std::string("cart-") + format);
    CHECK(Project::create(cartDir.string(), "c", Language::Dasm, "PAL", &project, &error, "", format));
    CHECK(project.format == format);
    std::ifstream sourceFile(inProject(cartDir, "main.asm")), notesFile(cartDir / "VCSIDE.md");
    std::string sourceText((std::istreambuf_iterator<char>(sourceFile)), {}), notesText((std::istreambuf_iterator<char>(notesFile)), {});
    CHECK(sourceText.find("{{") == std::string::npos && notesText.find("{{") == std::string::npos);
    CHECK(notesText.find(std::to_string(bytes) + " bytes") != std::string::npos);
    std::string dasm = findInPath("dasm");
    if (!dasm.empty()) {
      std::string command = "cd \"" + cartDir.string() + "\" && \"" + dasm + "\" src/main.asm -Isrc -Igenerated/gfx -Igenerated/music -f3 -oc.bin > dasm.log 2>&1";
      CHECK(std::system(command.c_str()) == 0 && fs::file_size(cartDir / "c.bin", errorCode) == (uintmax_t)bytes);
    }
  }
  for (auto [format, romSize] : {std::pair{"4K", "4k"}, {"F8", "8k"}, {"F6", "16k"}, {"F4", "32k"}}) {
    fs::path cartDir = dir / (std::string("bbcart-") + format);
    CHECK(Project::create(cartDir.string(), "c", Language::BatariBasic, "PAL", &project, &error, "", format));
    std::ifstream basFile(inProject(cartDir, "main.bas")), notesFile(cartDir / "VCSIDE.md");
    std::string basText((std::istreambuf_iterator<char>(basFile)), {}), notesText((std::istreambuf_iterator<char>(notesFile)), {});
    CHECK(project.format == format && basText.find(std::string("\tset romsize ") + romSize + "\n\tset tv pal\n") != std::string::npos);
    CHECK(notesText.find(std::string("set romsize ") + romSize) != std::string::npos && notesText.find("{{") == std::string::npos);
  }
  // batari Basic's starts on every cartridge: the empty loop, VCSIDE The Game with and
  // without the title screen, and the square; the graphics regions filled, nothing left of
  // the template's markers. With $bB set (batari Basic's folder), each one compiles.
  {
    const char* bbFolder = std::getenv("bB");
    std::string bbScript = bbFolder ? (fs::path(bbFolder) / "2600basic.sh").string() : "";
    bool canCompile = !bbScript.empty() && fs::exists(bbScript) && !findInPath("wasmtime").empty();
    int startIndex = 0;
    for (const char* format : {"4K", "F8", "F6", "F4"})
      for (const char* example : {"", "game", "game-notitle", "square"})
        for (const char* tv : {"NTSC", "PAL"}) {
          fs::path startDir = dir / ("bbstart-" + std::to_string(startIndex++));
          CHECK(Project::create(startDir.string(), "s", Language::BatariBasic, tv, &project, &error, example, format));
          std::ifstream basFile(inProject(startDir, "main.bas"));
          std::string basText((std::istreambuf_iterator<char>(basFile)), {});
          const std::string exampleId = example;
          CHECK(project.format == format && basText.find("{{") == std::string::npos && basText.find("rem s - ") != std::string::npos);
          CHECK((basText.find("set tv pal") != std::string::npos) == (std::string(tv) == "PAL"));
          CHECK(basText.find("rem vcsgfx end\n  rem vcsgfx end") == std::string::npos);
          if (exampleId.rfind("game", 0) == 0) {
            const bool title = exampleId == "game";
            CHECK(fs::exists(inProject(startDir, "title.vcsgfx")) == title && (basText.find("goto title") != std::string::npos) == title);
            CHECK(fs::exists(inProject(startDir, "hero.vcsgfx")) && fs::exists(inProject(startDir, "enemy.vcsgfx")) && fs::exists(inProject(startDir, "room.vcsgfx")));
            // every region holds the export of its frame or playfield
            for (const char* filled : {"\trem vcsgfx hero.vcsgfx hero frame 4\n\trem hero, frame 4", "\trem vcsgfx enemy.vcsgfx enemy frame 2\n\trem enemy, frame 2",
                                       "\trem vcsgfx room.vcsgfx room\n\trem room;"})
              CHECK(basText.find(filled) != std::string::npos);
            if (title) CHECK(basText.find("\trem vcsgfx title.vcsgfx cart\n\trem cart:") != std::string::npos);
            // the graphics' NTSC colours, turned into PAL ones in a PAL project (the hero's top row,
            // $F4, last: batari Basic's tables start at the bottom), and the second enemy's blue ($86)
            const bool ntsc = std::string(tv) == "NTSC";
            CHECK((basText.find("\t$F4\nend\n") != std::string::npos) == ntsc);
            CHECK((basText.find("in enemy.vcsgfx.\n\tplayer1color:\n\t$86\n") != std::string::npos) == ntsc &&
                  basText.find("{{SECOND_ENEMY_COLOUR}}") == std::string::npos);
            std::ifstream musicFile(inProject(startDir, "music.asm"));
            std::string musicText((std::istreambuf_iterator<char>(musicFile)), {});
            CHECK(musicText.find("MUSIC_RAM = h") != std::string::npos && musicText.find("MUSIC_EFFECT_LOSE") != std::string::npos);
          }
          if (!canCompile) continue;
          std::string command = "cd \"" + startDir.string() + "\" && sh \"" + bbScript + "\" src/main.bas > bb.log 2>&1";
          CHECK(std::system(command.c_str()) == 0);
          std::ifstream logFile(startDir / "bb.log");
          std::string logText((std::istreambuf_iterator<char>(logFile)), {});
          CHECK(logText.find("compilation complete") != std::string::npos && logText.find("rror") == std::string::npos);
          int romBytes = std::string(format) == "4K" ? 4096 : std::string(format) == "F8" ? 8192 : std::string(format) == "F6" ? 16384 : 32768;
          CHECK(fs::file_size(startDir / "src" / "main.bas.bin", errorCode) == (uintmax_t)romBytes);
        }
  }
  // the DASM game: the shared graphics and song, the menu's first TV system from the
  // project's standard, and a 4K program that assembles
  for (const char* tvStandard : {"NTSC", "PAL", "PAL60", "AUTO"}) {
    fs::path gameDir = dir / (std::string("game-") + tvStandard);
    CHECK(Project::create(gameDir.string(), "my game", Language::Dasm, tvStandard, &project, &error, "game"));
    for (auto fileName : {"main.asm", "2600.h", "hero.vcsgfx", "enemy.vcsgfx", "room.vcsgfx", "title.vcsgfx", "hero.gfx.asm",
                          "enemy.gfx.asm", "room.gfx.asm", "title.gfx.asm", "theme.vcsmusic", "music.asm"})
      CHECK(fs::exists(inProject(gameDir, fileName)));
    std::ifstream sourceFile(inProject(gameDir, "main.asm"));
    std::string sourceText((std::istreambuf_iterator<char>(sourceFile)), {});
    const std::string expectedSystem = std::string(tvStandard) == "PAL60" ? "1" : std::string(tvStandard) == "PAL" ? "2" : "0";
    CHECK(sourceText.find("DEFAULT_SYSTEM = " + expectedSystem) != std::string::npos && sourceText.find("{{") == std::string::npos);
    std::ifstream musicFile(inProject(gameDir, "music.asm"));
    std::string musicText((std::istreambuf_iterator<char>(musicFile)), {});
    CHECK(musicText.find("MUSIC_EFFECT_SHOT") != std::string::npos && musicText.find("MUSIC_EFFECT_LOSE") != std::string::npos);
    // its effects all end (one that holds its volume keeps the channel for good)
    std::ifstream songFile(inProject(gameDir, "theme.vcsmusic"));
    vcs::music::Song gameSong;
    CHECK(vcs::music::fromJson(std::string((std::istreambuf_iterator<char>(songFile)), {}), &gameSong, &error));
    CHECK(vcs::music::exportSong(gameSong, vcs::music::audioClockHz("NTSC"), {}).problems.empty());
    std::string dasm = findInPath("dasm");
    if (!dasm.empty()) {
      std::string command = "cd \"" + gameDir.string() + "\" && \"" + dasm + "\" src/main.asm -Isrc -Igenerated/gfx -Igenerated/music -f3 -ogame.bin > dasm.log 2>&1";
      CHECK(std::system(command.c_str()) == 0 && fs::file_size(gameDir / "game.bin", errorCode) == 4096);
    }
  }
  for (const char* tvStandard : {"NTSC", "PAL", "AUTO"}) {
    fs::path animationDir = dir / (std::string("anim-") + tvStandard);
    CHECK(Project::create(animationDir.string(), "walker", Language::Dasm, tvStandard, &project, &error, "animation"));
    for (auto fileName : {"main.asm", "2600.h", "hero.vcsgfx", "hero.gfx.asm", "theme.vcsmusic", "music.asm"})
      CHECK(fs::exists(inProject(animationDir, fileName)));
    std::ifstream musicFile(inProject(animationDir, "music.asm"));
    std::string musicText((std::istreambuf_iterator<char>(musicFile)), {});
    CHECK(musicText.find("MUSIC_EFFECT_JUMP") != std::string::npos && musicText.find("MusicFrame") != std::string::npos);
    std::ifstream includeFile(inProject(animationDir, "hero.gfx.asm"));
    std::string includeText((std::istreambuf_iterator<char>(includeFile)), {});
    CHECK(includeText.find("hero_walk_FRAME") != std::string::npos && includeText.find("hero_P0_FramePtrLo") != std::string::npos);
    std::string dasm = findInPath("dasm");
    if (!dasm.empty()) {
      std::string command = "cd \"" + animationDir.string() + "\" && \"" + dasm + "\" src/main.asm -Isrc -Igenerated/gfx -Igenerated/music -f3 -oexample.bin > dasm.log 2>&1";
      CHECK(std::system(command.c_str()) == 0 && fs::file_size(animationDir / "example.bin", errorCode) == 4096);
      if (std::string(tvStandard) == "AUTO") {  // both systems from one source: -DPAL=1 gives the 50 Hz one
        std::string pal = "cd \"" + animationDir.string() + "\" && \"" + dasm + "\" src/main.asm -Isrc -Igenerated/gfx -Igenerated/music -f3 -DPAL=1 -opal.bin > dasm.log 2>&1";
        CHECK(std::system(pal.c_str()) == 0 && fs::file_size(animationDir / "pal.bin", errorCode) == 4096);
        std::ifstream exampleRomFile(animationDir / "example.bin", std::ios::binary), palRomFile(animationDir / "pal.bin", std::ios::binary);
        std::string exampleBytes((std::istreambuf_iterator<char>(exampleRomFile)), {}), palBytes((std::istreambuf_iterator<char>(palRomFile)), {});
        CHECK(exampleBytes != palBytes);
      }
    }
  }
  CHECK(!Project::create((dir / "none").string(), "x", Language::Dasm, "NTSC", &project, &error, "nope") && !error.empty());
  // every template gives VCSIDE.md, the notes for the assistant, with the project's name
  {
    const std::vector<std::pair<Language, std::string>> notedTemplates = {
        {Language::Dasm, ""},  {Language::Dasm, "animation"}, {Language::Dasm, "game"},   {Language::Dasm, "cdfj"},
        {Language::BatariBasic, ""}, {Language::BatariBasic, "game"}, {Language::BatariBasic, "square"}, {Language::ElfC, ""},   {Language::ElfC, "animation"},
        {Language::ElfC, "game"},    {Language::AceC, ""},        {Language::AceC, "animation"}, {Language::AceC, "vcslib"}, {Language::AceC, "game"}};
    int notesIndex = 0;
    for (auto& [language, example] : notedTemplates) {
      fs::path notedDir = dir / ("notes-" + std::to_string(notesIndex++));
      CHECK(Project::create(notedDir.string(), "noted", language, "NTSC", &project, &error, example));
      std::ifstream notesFile(notedDir / "VCSIDE.md");
      std::string notesText((std::istreambuf_iterator<char>(notesFile)), {});
      CHECK(notesText.find("# noted: notes for the assistant") == 0 && notesText.find("{{") == std::string::npos &&
            notesText.find("## Kernel rules") != std::string::npos && notesText.find("## Do not touch") != std::string::npos);
      CHECK(notesText.size() <= 4000);  // what the assistant reads of it
    }
  }
  // DPC+: the example, VCSIDE The Game and the empty template; the driver copied from
  // batari Basic (here a stand-in, through $bB), the song exported in three parts, a 32K image
  {
    fs::path fakeBb = dir / "fake-bb";
    fs::create_directories(fakeBb / "includes");
    std::ofstream(fakeBb / "includes" / "DPCplus.arm", std::ios::binary) << std::string(3072, '\0');
    const char* realBb = std::getenv("bB");
    std::string savedBb = realBb ? realBb : "";
    auto setBb = [](const std::string& value) {
#ifdef _WIN32
      _putenv_s("bB", value.c_str());
#else
      if (value.empty()) unsetenv("bB");
      else setenv("bB", value.c_str(), 1);
#endif
    };
    setBb(fakeBb.string());
    for (const char* tvStandard : {"NTSC", "PAL"})
      for (std::string example : {"dpcplus", "game-dpcplus", ""}) {
        fs::path dpcDir = dir / ("dpc-" + (example.empty() ? std::string("empty") : example) + "-" + tvStandard);
        CHECK(Project::create(dpcDir.string(), "voices", Language::Dasm, tvStandard, &project, &error, example, "DPC+"));
        CHECK(project.format == "DPC+");
        for (auto fileName : {"main.asm", "2600.h", "DPCplus.arm", "VCSIDE.md"}) CHECK(fs::exists(inProject(dpcDir, fileName)));
        std::ifstream sourceFile(inProject(dpcDir, "main.asm")), notesFile(dpcDir / "VCSIDE.md");
        std::string sourceText((std::istreambuf_iterator<char>(sourceFile)), {}), notesText((std::istreambuf_iterator<char>(notesFile)), {});
        CHECK(sourceText.find("{{") == std::string::npos && notesText.find("# voices: notes for the assistant") == 0 &&
              notesText.find("{{") == std::string::npos && notesText.find("## Kernel rules") != std::string::npos &&
              notesText.size() <= 4000);
        CHECK(fs::exists(inProject(dpcDir, "music.asm")) == !example.empty());
        if (!example.empty()) {
          std::ifstream musicFile(inProject(dpcDir, "music.asm"));
          std::string musicText((std::istreambuf_iterator<char>(musicFile)), {});
          CHECK(musicText.find("MUSIC_PART = 3") != std::string::npos);
          CHECK(musicText.find("MUSIC_TIA_EFFECTS = 1 ") != std::string::npos);  // "bump" or "hit", on the TIA's channel 1
          CHECK(musicText.find(example == "dpcplus" ? "MUSIC_EFFECT_FIRE" : "MUSIC_EFFECT_SHOT") != std::string::npos);
        }
        if (example == "game-dpcplus")
          for (auto fileName : {"hero.gfx.asm", "enemy.gfx.asm", "room.gfx.asm", "title.gfx.asm"}) CHECK(fs::exists(inProject(dpcDir, fileName)));
        std::string dasm = findInPath("dasm");
        if (!dasm.empty()) {
          std::string command = "cd \"" + dpcDir.string() + "\" && \"" + dasm + "\" src/main.asm -Isrc -Igenerated/gfx -Igenerated/music -f3 -oexample.bin > dasm.log 2>&1";
          CHECK(std::system(command.c_str()) == 0 && fs::file_size(dpcDir / "example.bin", errorCode) == 32768);
        }
      }
    setBb(savedBb);
  }
  // CDFJ: the example, VCSIDE The Game and the empty template; the C code, the Makefile and
  // the song exported for CDFJ; built with make when the tools are there, with a stand-in
  // for the driver (or the real one in VCSIDE_CDFJ_DRIVER; VCSIDE_CDFJ_OUT keeps the
  // example's project there, to run it)
  for (const char* tvStandard : {"NTSC", "PAL"})
  for (std::string example : {"cdfj", "game-cdfj", ""}) {
    const char* keepIn = std::getenv("VCSIDE_CDFJ_OUT");
    fs::path cdfjDir = keepIn && std::string(tvStandard) == "NTSC" && example == "cdfj"
                           ? fs::path(keepIn)
                           : dir / ("cdfj-" + (example.empty() ? std::string("empty") : example) + "-" + tvStandard);
    CHECK(!Project::create((dir / "cdfj spaces").string(), "my game", Language::Dasm, tvStandard, &project, &error, example, "CDFJ") && !error.empty());
    CHECK(Project::create(cdfjDir.string(), "voices", Language::Dasm, tvStandard, &project, &error, example, "CDFJ"));
    CHECK(project.format == "CDFJ" && project.buildCommand == "make" && project.output == "build/voices.bin");
    for (auto fileName : {"main.asm", "2600.h", "Makefile", "game.c", "divide.c", "cdfj.h", "layout.h", "start.S", "arm.ld", "VCSIDE.md"})
      CHECK(fs::exists(inProject(cdfjDir, fileName)));
    std::ifstream makefile(cdfjDir / "Makefile"), notesFile(cdfjDir / "VCSIDE.md");
    std::string makeText((std::istreambuf_iterator<char>(makefile)), {}), notesText((std::istreambuf_iterator<char>(notesFile)), {});
    CHECK(makeText.find("NAME      = voices") != std::string::npos);
    CHECK(notesText.find("# voices: notes for the assistant") == 0 && notesText.find("{{") == std::string::npos &&
          notesText.find("## Kernel rules") != std::string::npos && notesText.size() <= 4000);
    CHECK(fs::exists(inProject(cdfjDir, "music.h")) == !example.empty() && fs::exists(inProject(cdfjDir, "theme.vcsmusic")) == !example.empty());
    if (!example.empty()) {
      std::ifstream musicFile(inProject(cdfjDir, "music.h"));
      std::string musicText((std::istreambuf_iterator<char>(musicFile)), {});
      CHECK(musicText.find("#define MUSIC_CDFJ 1") != std::string::npos);
      CHECK(musicText.find("#define MUSIC_TIA_EFFECTS 1 ") != std::string::npos);
    }
    if (example == "game-cdfj")  // its graphics are the C code's
      for (auto fileName : {"hero.gfx.h", "enemy.gfx.h", "room.gfx.h", "title.gfx.h"}) CHECK(fs::exists(inProject(cdfjDir, fileName)));
    std::string make = findInPath("make"), dasm = findInPath("dasm"), gcc = findInPath("arm-none-eabi-gcc");
    if (make.empty() || dasm.empty() || gcc.empty()) {
      std::printf("  (no make, dasm or arm-none-eabi-gcc: the CDFJ example is not built)\n");
      continue;
    }
    std::string withoutDriver = "cd \"" + cdfjDir.string() + "\" && make > make.log 2>&1";
    CHECK(std::system(withoutDriver.c_str()) != 0);  // the driver is not there: make says so
    std::ifstream logFile(cdfjDir / "make.log");
    std::string logText((std::istreambuf_iterator<char>(logFile)), {});
    CHECK(logText.find("cdfj_driver.bin is missing") != std::string::npos);
    const char* realDriver = std::getenv("VCSIDE_CDFJ_DRIVER");
    if (realDriver) fs::copy_file(realDriver, cdfjDir / "cdfj_driver.bin", fs::copy_options::overwrite_existing, errorCode);
    else std::ofstream(cdfjDir / "cdfj_driver.bin", std::ios::binary) << std::string(2048, '\0');
    std::string build = "cd \"" + cdfjDir.string() + "\" && make > make.log 2>&1";
    CHECK(std::system(build.c_str()) == 0 && fs::file_size(cdfjDir / "build" / "voices.bin", errorCode) == 32768);
  }
  {
    // the user's own CDFJ driver: checked (size, signature, entry points), then copied. The
    // files are made here: zeros with only what is checked, not the driver
    fs::path drivers = dir / "cdfj-drivers";
    fs::create_directories(drivers, errorCode);
    std::string likeDriver(2048, '\0');
    likeDriver.replace(0x174, 12, "CDFJCDFJCDFJ");
    for (int entry = 0x750; entry < 0x760; entry += 4) likeDriver.replace(entry, 4, "\x25\x4c\x20\x47");
    auto write = [&](const char* name, const std::string& bytes) {
      std::ofstream(drivers / name, std::ios::binary) << bytes;
      return (drivers / name).string();
    };
    std::string driverError;
    CHECK(Project::checkCdfjDriver(write("good.bin", likeDriver), &driverError) && driverError.empty());
    CHECK(!Project::checkCdfjDriver(write("short.bin", likeDriver.substr(0, 2047)), &driverError) && driverError.find("2047") != std::string::npos);
    std::string cdfPlus = likeDriver;
    cdfPlus.replace(0x174, 12, "PLUSCDFJPLUS");
    CHECK(!Project::checkCdfjDriver(write("plus.bin", cdfPlus), &driverError) && driverError.find("signature") != std::string::npos);
    std::string moved = likeDriver;
    moved.replace(0x758, 4, std::string(4, '\0'));
    CHECK(!Project::checkCdfjDriver(write("moved.bin", moved), &driverError) && driverError.find("$0751") != std::string::npos);
    CHECK(!Project::checkCdfjDriver((drivers / "none.bin").string(), &driverError));
    fs::path into = drivers / "project";
    fs::create_directories(into, errorCode);
    CHECK(!Project::addCdfjDriver((drivers / "plus.bin").string(), into.string(), &driverError) && !fs::exists(into / Project::kCdfjDriverFile));
    CHECK(Project::addCdfjDriver((drivers / "good.bin").string(), into.string(), &driverError) && fs::file_size(into / Project::kCdfjDriverFile, errorCode) == 2048);
    CHECK(Project::addCdfjDriver((into / Project::kCdfjDriverFile).string(), into.string(), &driverError));  // already there
    if (const char* realDriver = std::getenv("VCSIDE_CDFJ_DRIVER")) CHECK(Project::checkCdfjDriver(realDriver, &driverError));
  }
  CHECK(stellaFormat("CDFJ") == "CDF" && stellaFormat("DPC+") == "DPCP");
  // the ELF example: the template with the example's main.c, and hero.gfx.h next to it
  CHECK(Project::examples(Language::ElfC).size() == 2 && Project::examples(Language::ElfC)[0].first == "game");
  fs::path elfDir = dir / "anim-elf";
  CHECK(Project::create(elfDir.string(), "walker", Language::ElfC, "NTSC", &project, &error, "animation"));
  for (auto fileName : {"Makefile", "include/vcsLib.h", "src/main.c", "src/memcpy_long.S", "src/hero.vcsgfx", "src/hero.gfx.h", "src/theme.vcsmusic", "src/music.h"})
    CHECK(fs::exists(inProject(elfDir, fileName)));
  std::ifstream mainFile(inProject(elfDir, "src/main.c")), headerFile(inProject(elfDir, "src/hero.gfx.h")), elfMusicFile(inProject(elfDir, "src/music.h"));
  std::string mainText((std::istreambuf_iterator<char>(mainFile)), {}), headerText((std::istreambuf_iterator<char>(headerFile)), {});
  std::string elfMusicText((std::istreambuf_iterator<char>(elfMusicFile)), {});
  CHECK(elfMusicText.find("#define MUSIC_TIA_EFFECTS 1 ") != std::string::npos && mainText.find("vcsWrite5(AUDV1, music_tia[2]);") != std::string::npos);
  CHECK(mainText.find("walker - made with VCSIDE") != std::string::npos && mainText.find("hero.gfx.h") != std::string::npos);
  CHECK(mainText.find("music_frame(lines + MUSIC_OVERBLANK_LINES);") != std::string::npos);
  CHECK(headerText.find("static const vcsgfx_animation hero_walk = {hero_walk_steps, 4, 1};") != std::string::npos);
  std::string gcc = findInPath("arm-none-eabi-gcc"), make = findInPath("make");
  if (!gcc.empty() && !make.empty()) {
    std::string command = "cd \"" + elfDir.string() + "\" && \"" + make + "\" > make.log 2>&1";
    CHECK(std::system(command.c_str()) == 0 && fs::exists(elfDir / "build" / "walker.bin"));
    command = "cd \"" + dir.string() + "\" && \"" + make + "\" > make.log 2>&1";  // the template without an example
    CHECK(std::system(command.c_str()) == 0 && fs::exists(dir / "build" / "rainbow.bin"));
    // make ace: the same games as ACE files, with vcsLib on the ACE bus library
    for (auto& [projectDir, aceName] : {std::pair{elfDir, "walker.ace"}, std::pair{dir, "rainbow.ace"}}) {
      command = "cd \"" + projectDir.string() + "\" && \"" + make + "\" ace > make-ace.log 2>&1";
      CHECK(std::system(command.c_str()) == 0 && romFormat((projectDir / "build" / aceName).string()) == "ACE");
    }
  }
  // VCSIDE The Game in C, on ELF: the shared rules, menu and screen, the shared graphics and
  // song (with the game's three effects), the menu's first system from the project's TV
  // standard; it builds as ELF and, with make ace, as ACE
  for (const char* tvStandard : {"NTSC", "PAL", "PAL60"}) {
    fs::path gameDir = dir / (std::string("elf-game-") + tvStandard);
    CHECK(Project::create(gameDir.string(), "game", Language::ElfC, tvStandard, &project, &error, "game"));
    CHECK(project.format == "ELF" && project.buildCommand == "make" && project.output == "build/game.bin");
    for (auto fileName : {"Makefile", "include/vcsLib.h", "src/main.c", "src/game.c", "src/game.h", "src/menu.c", "src/menu.h", "src/screen.c",
                          "src/screen.h", "src/hero.gfx.h", "src/enemy.gfx.h", "src/room.gfx.h", "src/title.gfx.h", "src/music.h",
                          "ace/vcslib_ace.c", "chameleon/support.c"})
      CHECK(fs::exists(inProject(gameDir, fileName)));
    {  // its effects all end (one that holds its volume keeps its voice for good)
      std::ifstream songFile(inProject(gameDir, "src/theme.vcsmusic"));
      vcs::music::Song gameSong;
      CHECK(vcs::music::fromJson(std::string((std::istreambuf_iterator<char>(songFile)), {}), &gameSong, &error));
      CHECK(vcs::music::exportSong(gameSong, vcs::music::audioClockHz("NTSC"), {vcs::music::ExportTarget::C, "", "NTSC"}).problems.empty());
    }
    std::ifstream gameMain(inProject(gameDir, "src/main.c")), gameMusic(inProject(gameDir, "src/music.h")), gameRules(inProject(gameDir, "src/game.c"));
    std::string gameMainText((std::istreambuf_iterator<char>(gameMain)), {}), gameMusicText((std::istreambuf_iterator<char>(gameMusic)), {});
    std::string gameRulesText((std::istreambuf_iterator<char>(gameRules)), {});
    const std::string expectedSystem = std::string(tvStandard) == "PAL" ? "SYSTEM_PAL" : std::string(tvStandard) == "PAL60" ? "SYSTEM_PAL60" : "SYSTEM_NTSC";
    CHECK(gameMainText.find("#define DEFAULT_SYSTEM " + expectedSystem + " ") != std::string::npos && gameMainText.find("{{") == std::string::npos);
    CHECK(gameRulesText.find("game - the rules") != std::string::npos && gameRulesText.find("{{") == std::string::npos);
    CHECK(gameMusicText.find("MUSIC_EFFECT_SHOT") != std::string::npos && gameMusicText.find("MUSIC_EFFECT_HIT") != std::string::npos &&
          gameMusicText.find("MUSIC_EFFECT_LOSE") != std::string::npos && gameMusicText.find("#define MUSIC_TIA_EFFECTS 1 ") != std::string::npos);
    if (!gcc.empty() && !make.empty() && std::string(tvStandard) == "NTSC") {
      std::string command = "cd \"" + gameDir.string() + "\" && \"" + make + "\" > make.log 2>&1 && \"" + make + "\" ace > make-ace.log 2>&1";
      CHECK(std::system(command.c_str()) == 0 && fs::exists(gameDir / "build" / "game.bin") && romFormat((gameDir / "build" / "game.ace").string()) == "ACE");
    }
  }
  // the ACE files an ELF project gets for make ace, and its TV system
  for (auto fileName : {"ace/ace.h", "ace/ace.c", "ace/start.S", "ace/memory.c", "ace/ace.ld", "ace/vcslib_ace.c", "ace/main_ace.c"})
    CHECK(fs::exists(inProject(elfDir, fileName)));
  {
    std::ifstream elfMakefile(elfDir / "Makefile"), elfStart(elfDir / "ace" / "start.S");
    std::string makefileText((std::istreambuf_iterator<char>(elfMakefile)), {}), startText((std::istreambuf_iterator<char>(elfStart)), {});
    CHECK(makefileText.find("ACE_SYSTEM ?= 0\n") != std::string::npos && startText.find("\"walker          \"") != std::string::npos);
    fs::path palElf = dir / "pal-elf";
    CHECK(Project::create(palElf.string(), "palgame", Language::ElfC, "PAL", &project, &error));
    std::ifstream palMakefile(palElf / "Makefile");
    std::string palText((std::istreambuf_iterator<char>(palMakefile)), {});
    CHECK(palText.find("ACE_SYSTEM ?= 1\n") != std::string::npos);
  }
  // the builds besides the ROM, chosen when the project is made: only those get their glue
  // folder, their Makefile target and their lines in VCSIDE.md
  for (auto [chameleonBuild, aceBuild] : {std::pair{false, false}, std::pair{true, false}, std::pair{false, true}}) {
    fs::path buildsDir = dir / ("elf-builds-" + std::to_string(chameleonBuild) + std::to_string(aceBuild));
    CHECK(Project::create(buildsDir.string(), "solo", Language::ElfC, "NTSC", &project, &error, "game", "", {chameleonBuild, aceBuild}));
    CHECK(fs::exists(buildsDir / "chameleon") == chameleonBuild && fs::exists(buildsDir / "ace") == aceBuild);
    std::ifstream makefileIn(buildsDir / "Makefile"), notesIn(buildsDir / "VCSIDE.md");
    std::string makefileText((std::istreambuf_iterator<char>(makefileIn)), {}), notesText((std::istreambuf_iterator<char>(notesIn)), {});
    CHECK(makefileText.find("{{") == std::string::npos && notesText.find("{{") == std::string::npos);
    CHECK((makefileText.find("\nchameleon: ") != std::string::npos) == chameleonBuild && (makefileText.find("\nace: ") != std::string::npos) == aceBuild);
    CHECK((notesText.find("make chameleon") != std::string::npos) == chameleonBuild && (notesText.find("make ace") != std::string::npos) == aceBuild);
    if (!gcc.empty() && !make.empty() && !chameleonBuild && !aceBuild) {
      std::string command = "cd \"" + buildsDir.string() + "\" && \"" + make + "\" > make.log 2>&1";
      CHECK(std::system(command.c_str()) == 0 && fs::exists(buildsDir / "build" / "solo.bin"));
    }
  }
  CHECK(!Project::create((dir / "none-elf").string(), "x", Language::ElfC, "NTSC", &project, &error, "nope") && !error.empty());
  // the ACE template: Gopher2600 at the STM32F407's clock, the header's 16-character name,
  // the visible lines for the TV standard, and a file that starts with the ACE header
  fs::path aceDir = dir / "ace";
  CHECK(Project::create(aceDir.string(), "beam", Language::AceC, "PAL", &project, &error));
  CHECK(project.format == "ACE" && project.output == "build/beam.ace" && project.emulator == "gopher2600" && project.buildCommand == "make");
  CHECK(project.emulatorOptions["gopher2600"]["armClock"] == "168" && languageFromId(languageId(Language::AceC)) == Language::AceC);
  for (auto fileName : {"Makefile", "ace.ld", "include/ace.h", "src/ace.c", "src/start.S", "src/main.c", "src/theme.vcsmusic", "src/music.h"})
    CHECK(fs::exists(inProject(aceDir, fileName)));
  std::ifstream aceStart(inProject(aceDir, "src/start.S")), aceMain(inProject(aceDir, "src/main.c"));
  std::string aceStartText((std::istreambuf_iterator<char>(aceStart)), {}), aceMainText((std::istreambuf_iterator<char>(aceMain)), {});
  CHECK(aceStartText.find("\"beam            \"") != std::string::npos && aceMainText.find("lines = 249;") != std::string::npos);
  CHECK(tvStandardsFor(Language::AceC) == tvStandardsFor(Language::ElfC) && isC(Language::AceC) && !isC(Language::Dasm));
  if (!gcc.empty() && !make.empty()) {
    std::string command = "cd \"" + aceDir.string() + "\" && \"" + make + "\" > make.log 2>&1";
    CHECK(std::system(command.c_str()) == 0 && romFormat((aceDir / "build" / "beam.ace").string()) == "ACE" && fs::exists(aceDir / "build" / "ACE_debugging.elf"));
    // its space: the header and start code in the file, code and constants apart, all of it well inside the buffer
    std::ifstream debugFile(aceDir / "build" / "ACE_debugging.elf", std::ios::binary);
    std::string debugBytes((std::istreambuf_iterator<char>(debugFile)), {});
    ElfFile builtElf;
    CHECK(readElf(debugBytes, builtElf, &error) && builtElf.type == 2);
    long aceFileSize = (long)fs::file_size(aceDir / "build" / "beam.ace", errorCode);
    ElfSpace builtSpace = analyzeAceSpace(builtElf, aceFileSize, 90);
    CHECK(builtSpace.startCode > 40 && builtSpace.code > 1000 && builtSpace.constants > 0 && builtSpace.bss > 0 && builtSpace.diagnostics.empty());
    CHECK(aceFileSize == builtSpace.startCode + builtSpace.code + builtSpace.constants + builtSpace.data);  // the file: all but .bss
  }
  // the ACE example: the animated sprite, its graphics' tables, NTSC lines
  CHECK(Project::examples(Language::AceC).size() == 3 && Project::examples(Language::AceC)[0].first == "game" &&
        Project::examples(Language::AceC)[1].first == "animation");
  fs::path aceAnimationDir = dir / "ace-animation";
  CHECK(Project::create(aceAnimationDir.string(), "walker", Language::AceC, "NTSC", &project, &error, "animation"));
  for (auto fileName : {"src/hero.vcsgfx", "src/hero.gfx.h", "src/music.h", "src/ace.c", "src/start.S"})
    CHECK(fs::exists(inProject(aceAnimationDir, fileName)));
  std::ifstream aceAnimationMain(inProject(aceAnimationDir, "src/main.c"));
  std::string aceAnimationText((std::istreambuf_iterator<char>(aceAnimationMain)), {});
  CHECK(aceAnimationText.find("hero.gfx.h") != std::string::npos && aceAnimationText.find("lines = 199;") != std::string::npos);
  if (!gcc.empty() && !make.empty()) {
    std::string command = "cd \"" + aceAnimationDir.string() + "\" && \"" + make + "\" > make.log 2>&1";
    CHECK(std::system(command.c_str()) == 0 && romFormat((aceAnimationDir / "build" / "walker.ace").string()) == "ACE");
  }
  // VCSIDE The Game on ace.h: the ELF game's shared files with ACE's own main.c, and the
  // project's TV standard as the menu's first system
  fs::path aceGameDir = dir / "ace-game";
  CHECK(Project::create(aceGameDir.string(), "game", Language::AceC, "PAL", &project, &error, "game"));
  CHECK(project.format == "ACE" && project.output == "build/game.ace" && project.buildCommand == "make" && project.emulator == "gopher2600");
  for (auto fileName : {"Makefile", "include/ace.h", "src/ace.c", "src/start.S", "src/main.c", "src/game.c", "src/menu.c", "src/screen.c",
                        "src/hero.gfx.h", "src/enemy.gfx.h", "src/room.gfx.h", "src/title.gfx.h", "src/music.h"})
    CHECK(fs::exists(inProject(aceGameDir, fileName)));
  CHECK(!fs::exists(aceGameDir / "include" / "vcsLib.h"));
  std::ifstream aceGameMain(inProject(aceGameDir, "src/main.c"));
  std::string aceGameText((std::istreambuf_iterator<char>(aceGameMain)), {});
  CHECK(aceGameText.find("#define DEFAULT_SYSTEM SYSTEM_PAL ") != std::string::npos && aceGameText.find("ace_overblank_end") != std::string::npos);
  if (!gcc.empty() && !make.empty()) {
    std::string command = "cd \"" + aceGameDir.string() + "\" && \"" + make + "\" > make.log 2>&1";
    CHECK(std::system(command.c_str()) == 0 && romFormat((aceGameDir / "build" / "game.ace").string()) == "ACE");
  }
  // ACE's vcsLib example: the ELF project, built with make ace (and still as ELF with make)
  fs::path aceVcsLibDir = dir / "ace-vcslib";
  CHECK(Project::create(aceVcsLibDir.string(), "both", Language::AceC, "PAL", &project, &error, "vcslib"));
  CHECK(project.format == "ACE" && project.output == "build/both.ace" && project.emulator == "gopher2600" && project.buildCommand == "make ace");
  for (auto fileName : {"Makefile", "include/vcsLib.h", "src/main.c", "src/music.h", "ace/vcslib_ace.c", "ace/main_ace.c", "ace/ace.c", "ace/start.S"})
    CHECK(fs::exists(inProject(aceVcsLibDir, fileName)));
  CHECK(!fs::exists(aceVcsLibDir / "include" / "ace.h") && !fs::exists(aceVcsLibDir / "shared.json") && !fs::exists(aceVcsLibDir / "chameleon"));
  if (!gcc.empty() && !make.empty()) {
    std::string command = "cd \"" + aceVcsLibDir.string() + "\" && \"" + make + "\" ace > make-ace.log 2>&1 && \"" + make + "\" > make.log 2>&1";
    CHECK(std::system(command.c_str()) == 0 && romFormat((aceVcsLibDir / "build" / "both.ace").string()) == "ACE" && fs::exists(aceVcsLibDir / "build" / "both.bin"));
  }
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
  // DASM with spaces: code 8 in, conditionals 2 in; the step is 8, not the smallest indentation
  CHECK(detectIndentStyle({"  IF MUSIC", "Start", "        lda #1", "        sta AUDV0", "Loop", "        jmp Loop", "  ENDIF"}, tabs) ==
        (IndentStyle{false, 8}));
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
  CHECK(dasmText.find("hero_P0_Frame0\n\t.byte #%11111111;$44\n\t.byte #%00111100;$1E\n\t.byte #%10000001;$1E\n") != std::string::npos);
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
  CHECK(basicText.find("\tplayer0:\n\t%11111111\n\t%00111100\n\t%10000001\nend\n\tCOLUP0 = $1E : rem every frame\n") != std::string::npos);
  CHECK(basicText.find("missile0height = 2") != std::string::npos);
  CHECK(hasIssue(check(sprite, standardProfile), Issue::Warning, "playercolors"));
  // playercolors: a colour per row, at the cost of both missiles
  Profile playercolorsProfile = profileFromBasic(" rem set kernel PXE\n  set kernel_options playercolors pfcolors ; colours\n  const pfres = 12\n");
  CHECK(playercolorsProfile.kernel == Kernel::Standard && playercolorsProfile.options.playercolors && playercolorsProfile.options.pfcolors && !playercolorsProfile.options.player1colors && playercolorsProfile.options.pfres == 12);
  CHECK(playercolorsProfile.rowColours(Obj::P0) && !playercolorsProfile.has(Obj::M0) && !playercolorsProfile.has(Obj::M1) && playercolorsProfile.has(Obj::BL) && playercolorsProfile.pfRowColours() && playercolorsProfile.pfRowsDefault() == 12);
  CHECK(hasIssue(check(sprite, playercolorsProfile), Issue::Error, "M0"));
  basicText = exportBasic(sprite, playercolorsProfile);
  CHECK(basicText.find("\tplayer0color:\n\t$44\n\t$1E\n\t$1E\nend\n") != std::string::npos && basicText.find("rem M0: not drawn") != std::string::npos);
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
  CHECK(basicText.find("\tplayer0:\n\t%10000001\n\t%00111100\n\t%11111111\nend\n\tplayer0color:\n\t$1E\n\t$1E\n\t$44\nend\n") != std::string::npos);
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
  CHECK(basicText.find("\tplayer12:\n\t%10000001\n") != std::string::npos && basicText.find("player12color:") != std::string::npos);
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
  CHECK(basicText.find("\tCOLUP4 = ") != std::string::npos && basicText.find("player4color") == std::string::npos);
  many.layers[1].obj = Obj::P1;
  CHECK(exportBasic(many, multispriteProfile).find("\t_COLUP1 = ") != std::string::npos);
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
  CHECK(basicText.find("\tplayfield:\n\t.X...................X..........\n") != std::string::npos && basicText.find("pfcolors:") != std::string::npos);
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
  CHECK(basicText.find("\tplayfield:\n\tX...............\n\t...............X\nend\n\tCOLUPF = ") != std::string::npos);
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
    CHECK(region.find("\trem vcsgfx art.vcsgfx walk frame 1\n") == 0 && region.find("\trem vcsgfx art.vcsgfx walk frame 2\n") != std::string::npos);
    CHECK(region.find("%00011000") != std::string::npos);
    std::string program = " set kernel_options pfcolors\r\nmain\r\n  if a then goto two\r\n  rem vcsgfx art.vcsgfx walk frame 1\r\n  player0:\r\nold\r\nend\r\n"
                       "  rem vcsgfx end\r\n  goto draw\r\ntwo\r\n REM VCSGFX Art.vcsgfx walk frame 2\r\n rem vcsgfx end\r\n"
                       "  rem vcsgfx art.vcsgfx level\r\n  rem vcsgfx end\r\n  rem vcsgfx other.vcsgfx walk\r\nkeep\r\n  rem vcsgfx end\r\ndraw\r\n drawscreen\r\n";
    int updatedCount = 0;
    std::vector<std::string> problems;
    std::string updated = updateBasicRegions(program, "art.vcsgfx", art, standardProfile, &updatedCount, &problems);
    CHECK(updatedCount == 3 && problems.empty());
    CHECK(updated.find("old") == std::string::npos && updated.find("keep\r\n") != std::string::npos && updated.find("drawscreen\r\n") != std::string::npos);
    CHECK(updated.find("  rem vcsgfx art.vcsgfx walk frame 1\r\n\trem walk, frame 1;") != std::string::npos);  // the marker stays as the program wrote it
    CHECK(updated.find("\t%00011000\r\nend\r\n") != std::string::npos);  // frame 2's top row, bottom first: last
    CHECK(updated.find("\tplayfield:\r\n") != std::string::npos && updated.find("\n\n") == std::string::npos);
    // a second pass changes nothing
    CHECK(updateBasicRegions(updated, "art.vcsgfx", art, standardProfile, &updatedCount, &problems) == updated && updatedCount == 3);
    // problems: an unknown item, a frame too many, no end
    problems.clear();
    std::string bad = "  rem vcsgfx art.vcsgfx nobody\n  rem vcsgfx end\n  rem vcsgfx art.vcsgfx walk frame 3\n  rem vcsgfx end\n  rem vcsgfx art.vcsgfx walk\nx\n";
    CHECK(updateBasicRegions(bad, "art.vcsgfx", art, standardProfile, &updatedCount, &problems) == bad && updatedCount == 0 && problems.size() == 3);
    // "as player2": the sprite drawn by another player (PXE and multisprite's enemies)
    problems.clear();
    std::string asOther = "  rem vcsgfx art.vcsgfx walk frame 2 as player2\n  rem vcsgfx end\n";
    std::string drawnAsOther = updateBasicRegions(asOther, "art.vcsgfx", art, multispriteProfile, &updatedCount, &problems);
    CHECK(updatedCount == 1 && problems.empty() && drawnAsOther.find("player2:") != std::string::npos &&
          drawnAsOther.find("player0:") == std::string::npos && drawnAsOther.find("%00011000") != std::string::npos);
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
  CHECK(dasmText.find("walker_P0_FramePtrLo\n\t.byte <walker_P0_Frame0,<walker_P0_Frame1,<walker_P0_Frame2\n") != std::string::npos);
  CHECK(dasmText.find("walker_walk_FRAME\n\t.byte 0,1,2,1\nwalker_walk_TICKS\n\t.byte 8,4,8,4\n") != std::string::npos &&
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
  CHECK(basicText.find("\tdim walker_walk_t = y\n\tdim walker_walk_s = z\n\tdata walker_walk_ticks\n\t8, 4, 8, 4\nend\n") != std::string::npos);
  CHECK(basicText.find("\ton walker_walk_s goto walker_walk_f0 walker_walk_f1 walker_walk_f2 walker_walk_f1\n") != std::string::npos);
  CHECK(basicText.find("\nwalker_walk_f2\n") != std::string::npos && basicText.find("\tif walker_walk_s >= 4 then walker_walk_s = 0\n") != std::string::npos);
  CHECK(exportBasicAnimation(walker, walker.animations[1], standardProfile).find("then walker_idle_s = 1\n") != std::string::npos);
  {
    Asset walkerAsset;
    walkerAsset.sprites = {walker};
    std::string region = basicRegion("w.vcsgfx", walkerAsset, 0, standardProfile);
    CHECK(region.find("\trem vcsgfx w.vcsgfx walker animation walk\n") == 0 && region.find("\trem vcsgfx w.vcsgfx walker animation idle\n") != std::string::npos);
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
  CHECK(fs::exists(inProject(dir, "2600.h")) && !fs::exists(inProject(dir, "hardware.h")));
  std::string dasm = findInPath("dasm");
  if (!dasm.empty()) {
    std::string command = "cd \"" + dir.string() + "\" && \"" + dasm + "\" src/main.asm -Isrc -Igenerated/gfx -Igenerated/music -f3 -oplain.bin > dasm.log 2>&1";
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

// The TIA's sound and the tuning maths of the music editor.
static void testMusic() {
  using namespace vcs::music;
  // Counter by counter, the same as Gopher2600 (Chris Brenner's model): channel 0's level
  // over 2048 audio clocks from reset, for every AUDC and some AUDF values, hashed.
  struct Reference {
    int control, frequency;
    uint32_t hash;
  };
  const Reference references[] = {
      {0, 0, 0xf40099e4u},
      {0, 1, 0x6731897cu},
      {0, 7, 0x78ec09a8u},
      {0, 31, 0xf2066498u},
      {1, 0, 0x28e3b9e6u},
      {1, 1, 0xd71642b4u},
      {1, 7, 0xc4fde218u},
      {1, 31, 0xc39fbe18u},
      {2, 0, 0x14f43130u},
      {2, 1, 0xa8c64c65u},
      {2, 7, 0xf6a3adadu},
      {2, 31, 0x6e678418u},
      {3, 0, 0xa5168840u},
      {3, 1, 0x835c770eu},
      {3, 7, 0xde43e510u},
      {3, 31, 0x6fbe50d8u},
      {4, 0, 0xef95cdc5u},
      {4, 1, 0x477dd9c5u},
      {4, 7, 0xdf2829c5u},
      {4, 31, 0xc9cf69c5u},
      {5, 0, 0xef95cdc5u},
      {5, 1, 0x477dd9c5u},
      {5, 7, 0xdf2829c5u},
      {5, 31, 0xc9cf69c5u},
      {6, 0, 0xd419c213u},
      {6, 1, 0x15dd6204u},
      {6, 7, 0x78ab2ae0u},
      {6, 31, 0x683c4a38u},
      {7, 0, 0x3320227bu},
      {7, 1, 0xcc2ce6f7u},
      {7, 7, 0x6b7d13d5u},
      {7, 31, 0x42b4a4b8u},
      {8, 0, 0x64aae5cdu},
      {8, 1, 0x848ede47u},
      {8, 7, 0x66fd82edu},
      {8, 31, 0x915dcb58u},
      {9, 0, 0xc8eece2du},
      {9, 1, 0x80c071fau},
      {9, 7, 0xe2cb7d75u},
      {9, 31, 0x9ce509c5u},
      {10, 0, 0xd419c213u},
      {10, 1, 0x15dd6204u},
      {10, 7, 0x78ab2ae0u},
      {10, 31, 0x683c4a38u},
      {11, 0, 0xc12c7bf5u},
      {11, 1, 0xac5beb12u},
      {11, 7, 0xb3136f40u},
      {11, 31, 0xe8d3e878u},
      {12, 0, 0x8679562fu},
      {12, 1, 0xf80db318u},
      {12, 7, 0x6f462558u},
      {12, 31, 0x84c50b58u},
      {13, 0, 0x8679562fu},
      {13, 1, 0xf80db318u},
      {13, 7, 0x6f462558u},
      {13, 31, 0x84c50b58u},
      {14, 0, 0x2876cb21u},
      {14, 1, 0x809b44cfu},
      {14, 7, 0x350c4c48u},
      {14, 31, 0x6e678418u},
      {15, 0, 0xe3c264bdu},
      {15, 1, 0x3c8619cbu},
      {15, 7, 0x6d8d3e18u},
      {15, 31, 0x645930d8u},
  };
  int matching = 0;
  for (auto& reference : references) {
    TiaAudioChannel channel;
    channel.control = (uint8_t)reference.control, channel.frequency = (uint8_t)reference.frequency, channel.volume = 15;
    uint32_t hash = 2166136261u;
    for (int clockIndex = 0; clockIndex < 2048; clockIndex++) {
      channel.clock();
      hash = (hash ^ channel.level()) * 16777619u;
    }
    if (hash == reference.hash) matching++;
    else std::fprintf(stderr, "TIA audio differs: AUDC %d AUDF %d\n", reference.control, reference.frequency);
  }
  CHECK(matching == (int)std::size(references));

  // The periods of the distortions table, found by playing them.
  auto periodOf = [](int control, int frequency) {
    TiaAudioChannel channel;
    channel.control = (uint8_t)control, channel.frequency = (uint8_t)frequency, channel.volume = 15;
    std::vector<int> bits;
    for (int clockIndex = 0; clockIndex < 40000; clockIndex++) {
      channel.clock();
      if (clockIndex >= 4000) bits.push_back(channel.high());
    }
    for (size_t period = 1; period < 20000; period++) {
      bool repeats = true;
      for (size_t index = 0; index + period < bits.size() && repeats; index++) repeats = bits[index] == bits[index + period];
      if (repeats) return (int)period;
    }
    return 0;
  };
  for (auto& distortion : distortions())
    for (int control : distortion.controls) {
      CHECK(periodOf(control, 0) == distortion.period);
      CHECK(periodOf(control, 3) == distortion.period * 4);
    }
  CHECK(distortionOf(0) == nullptr && distortionOf(11) == nullptr && distortionOf(8)->period == 511);
  // AUDC 0: the output settles high, so AUDV is a level (samples)
  {
    TiaAudioChannel channel;
    channel.volume = 9;
    for (int clockIndex = 0; clockIndex < 8; clockIndex++) channel.clock();
    bool steady = true;
    for (int clockIndex = 0; clockIndex < 500; clockIndex++) channel.clock(), steady = steady && channel.level() == 9;
    CHECK(steady);
  }
  // mixing: 15 + 15 is the top, one channel at 15 two thirds of it
  CHECK(mixLevels(0) == 0 && mixLevels(30) == 1.0f && std::fabs(mixLevels(15) - 2.0f / 3.0f) < 1e-6);
  {
    Resampler resampler(31399.5, 48000);
    std::vector<float> output;
    for (int index = 0; index < 31400; index++) resampler.push(0.5f, output);
    bool flat = true;
    for (float value : output) flat = flat && std::fabs(value - 0.5f) < 1e-5;
    CHECK(flat && std::abs((int)output.size() - 48000) <= 2);
  }

  // Pitches and tuning
  double ntsc = audioClockHz("NTSC"), pal = audioClockHz("PAL");
  CHECK(std::fabs(ntsc - 31399.5) < 0.1 && std::fabs(pal - 31113.1) < 0.1 && audioClockHz("PAL60") == pal);
  CHECK(std::fabs(pitchHz(ntsc, 2, 0) - 15699.75) < 0.1 && noteName(60) == "C4" && noteName(69) == "A4" && noteName(70) == "A#4");
  CHECK(std::fabs(noteNumber(440) - 69) < 1e-9 && std::fabs(noteNumber(440, 50) - 68.5) < 1e-9);
  int nearNotes = 0;
  for (int frequency = 0; frequency < 32; frequency++) {
    double note = noteNumber(pitchHz(ntsc, 6, frequency));
    if (std::fabs(note - std::round(note)) * 100 <= 10) nearNotes++;
  }
  CHECK(nearNotes == 5);  // ÷6 square on NTSC: 5 of 32 values within 10 cents of A440
  NotePitch pitch = closestPitch(69, ntsc, {4, 12});
  CHECK(pitch.playable && pitch.control == 12 && std::fabs(pitch.hz - pitchHz(ntsc, 6, pitch.frequency)) < 1e-9);
  CHECK(!closestPitch(20, ntsc, {4}).playable && !closestPitch(69, ntsc, {8}).playable);  // too low; noise has no pitch
  // a C major melody over two octaves with square and ÷6
  std::vector<SongNote> melody;
  for (int octave = 0; octave < 2; octave++)
    for (int step : {0, 2, 4, 5, 7, 9, 11}) melody.push_back({60 + octave * 12 + step, {4, 12}});
  melody.push_back({84, {4, 12}});
  SongTuning asWritten = measureSong(melody, ntsc);
  CHECK(asWritten.unplayable == 0 && std::lround(asWritten.worstCents) == 46 && std::lround(asWritten.averageCents) == 14);
  SongTuning tunedWorst = tuneSong(melody, ntsc, TuningGoal::WorstNote);
  SongTuning tunedAverage = tuneSong(melody, ntsc, TuningGoal::Average);
  CHECK(tunedWorst.worstCents < asWritten.worstCents && tunedWorst.unplayable == 0 && tunedWorst.pitches.size() == melody.size());
  CHECK(tunedAverage.averageCents <= asWritten.averageCents && tunedAverage.averageCents <= tunedWorst.averageCents);
}

static void testSong() {
  using namespace vcs::music;
  namespace fs = std::filesystem;
  double ntsc = audioClockHz("NTSC");
  // JSON round trip
  Song song = blankSong();
  song.name = "theme";
  song.speed = 5, song.swing = 7, song.loopTo = 1;
  song.instruments[0].volumes[0].loop = 2;
  song.instruments[0].volumes.push_back({{8, 4}, 0});
  song.instruments[0].releases.push_back({});
  song.instruments[0].pitch = {1, -1};
  song.instruments[0].pitchUnit = PitchUnit::Audf;
  song.instruments[0].byHand[64] = {12, {9, 10}};
  song.tuning.transpose = -3, song.tuning.referenceCents = -14, song.tuning.goal = TuningGoal::Average;
  Pattern second;
  second.name = "verse";
  second.resize(16);
  song.patterns.push_back(second);
  song.order = {0, 1, 1};
  song.patterns[0].cells[0][0] = {69, 0};
  song.patterns[0].cells[0][4] = {kNoteOff, -1};
  song.patterns[0].cells[1][2] = {61, 1};
  song.patterns[1].cells[1][15] = {119, 0, kEffectVolume, 1};
  song.patterns[0].cells[0][5] = {kNoNote, -1, kEffectRelease, 15};
  std::string json = toJson(song), error;
  CHECK(json.find("\"A-4 00 ..\"") != std::string::npos && json.find("\"=== .. ..\"") != std::string::npos && json.find("\"--- .. RF\"") != std::string::npos);
  Song back;
  CHECK(fromJson(json, &back, &error));
  CHECK(toJson(back) == json);
  CHECK(back.name == "theme" && back.speed == 5 && back.swing == 7 && back.loopTo == 1 && back.order == std::vector<int>({0, 1, 1}));
  const Instrument& leadBack = back.instruments[0];
  CHECK(back.instruments.size() == 2 && back.instruments[1].drum && leadBack.volumes.size() == 2 && leadBack.volumes[0].loop == 2 && leadBack.volumes[1].values == std::vector<int>({8, 4}));
  CHECK(leadBack.releases.size() == 2 && leadBack.releases[1].empty() && leadBack.pitch == std::vector<int>({1, -1}) && leadBack.pitchUnit == PitchUnit::Audf);
  CHECK(leadBack.byHand.size() == 1 && leadBack.byHand.at(64).control == 12 && leadBack.byHand.at(64).frequencies == std::vector<int>({9, 10}));
  CHECK(back.patterns[1].cells[1][15].effect == kEffectVolume && back.patterns[1].cells[1][15].effectValue == 1 && back.patterns[0].cells[0][5].effectValue == 15);
  CHECK(back.tuning.transpose == -3 && back.tuning.referenceCents == -14 && back.tuning.goal == TuningGoal::Average);
  CHECK(back.patterns[1].rows == 16 && back.patterns[1].cells[1][15].note == 119 && back.patterns[0].cells[0][4].note == kNoteOff);
  CHECK(!fromJson("{\"format\":\"other\"}", &back, &error) && !error.empty());
  CHECK(!fromJson("{\"patterns\":[{\"rows\":1,\"channels\":[[\"C-4 00 X1\"]]}]}", &back, &error));  // no such effect
  // version 1: one volume, loop and release; the pitch in AUDF steps; cells without an effect
  CHECK(fromJson("{\"format\":\"vcsmusic\",\"version\":1,\"instruments\":[{\"name\":\"old\",\"volume\":[9,8],\"loop\":1,\"release\":[2],"
                 "\"controls\":[4],\"pitch\":[0,2]}],\"patterns\":[{\"rows\":2,\"channels\":[[\"C-4 00\",\"=== ..\"],[]]}]}",
                 &back, &error));
  CHECK(back.instruments[0].volumes.size() == 1 && back.instruments[0].volumes[0].values == std::vector<int>({9, 8}) && back.instruments[0].volumes[0].loop == 1);
  CHECK(back.instruments[0].releases == std::vector<std::vector<int>>({{2}}) && back.instruments[0].pitchUnit == PitchUnit::Audf && back.patterns[0].cells[0][0].note == 60);
  CHECK(song.rowFrames(0) == 5 && song.rowFrames(1) == 7);

  // The player, frame by frame: a held note, its loop, a note off and its release
  Song tune = blankSong();
  tune.speed = 3;
  tune.loopTo = -1;
  tune.patterns[0].resize(4);
  Instrument& lead = tune.instruments[0];
  lead.volumes = {{{15, 12, 9, 6}, 2}};
  lead.releases = {{4, 2}};
  lead.pitch = {0, 1};
  lead.pitchUnit = PitchUnit::Audf;
  tune.patterns[0].cells[0][0] = {69, 0};
  tune.patterns[0].cells[0][2] = {kNoteOff, -1};
  tune.patterns[0].cells[1][1] = {60, 1};
  NotePitch a4 = tune.pitchOf(69, lead, ntsc);
  Player player;
  player.start(tune, ntsc);
  std::vector<FrameRegisters> frames;
  for (int frame = 0; frame < 14; frame++) frames.push_back(player.nextFrame());
  std::vector<int> volumes, drumFrequencies;
  for (auto& frame : frames) volumes.push_back(frame.volume[0]), drumFrequencies.push_back(frame.frequency[1]);
  // rows of 3 frames: the note at 0, its loop (9 6 9 6...), the note off at 6, the release 4 2, then silence
  CHECK(volumes == std::vector<int>({15, 12, 9, 6, 9, 6, 4, 2, 0, 0, 0, 0, 0, 0}));
  CHECK(frames[0].control[0] == a4.control && frames[0].frequency[0] == a4.frequency && frames[1].frequency[0] == a4.frequency + 1 && frames[5].frequency[0] == a4.frequency + 1);
  // the drum from row 1: AUDC 8, AUDF 2 4 6 6..., its volume 14 10 6 3 1 0
  CHECK(frames[3].control[1] == 8 && drumFrequencies[3] == 2 && drumFrequencies[4] == 4 && drumFrequencies[5] == 6 && drumFrequencies[8] == 6);
  CHECK(frames[3].volume[1] == 14 && frames[7].volume[1] == 1 && frames[8].volume[1] == 0 && frames[2].volume[1] == 0);
  CHECK(!player.playing() && !player.sounding());  // 4 rows of 3 frames, then it stops

  // swing and the order list: rows alternate 2 and 5 frames; the song loops to entry 1
  Song looped = blankSong();
  looped.speed = 2, looped.swing = 5;
  looped.patterns[0].resize(2);
  looped.patterns.push_back(looped.patterns[0]);
  looped.order = {0, 1};
  looped.loopTo = 1;
  Player loopPlayer;
  loopPlayer.start(looped, ntsc);
  std::vector<std::pair<int, int>> seen;
  for (int frame = 0; frame < 21; frame++) {
    loopPlayer.nextFrame();
    SongPosition position = loopPlayer.position();
    if (seen.empty() || seen.back() != std::make_pair(position.order, position.row)) seen.push_back({position.order, position.row});
  }
  // 0:0 (2 frames) 0:1 (5) 1:0 (2) 1:1 (5) 1:0 (2) 1:1 (5): 21 frames
  using RowPositions = std::vector<std::pair<int, int>>;
  CHECK((seen == RowPositions({{0, 0}, {0, 1}, {1, 0}, {1, 1}, {1, 0}, {1, 1}}) && loopPlayer.playing()));
  // a note no allowed AUDC reaches is not heard
  Song unplayable = blankSong();
  unplayable.instruments[0].controls = {4};
  unplayable.patterns[0].cells[0][0] = {20, 0};
  Player silentPlayer;
  silentPlayer.start(unplayable, ntsc);
  CHECK(silentPlayer.nextFrame().volume[0] == 0);
  // a preview note outside the song, held until it is let go
  Player previewPlayer;
  previewPlayer.noteOn(tune, ntsc, 1, 69, 0);
  CHECK(previewPlayer.nextFrame().volume[1] == 15 && previewPlayer.sounding() && !previewPlayer.playing());
  previewPlayer.noteOff(1);
  CHECK(previewPlayer.nextFrame().volume[1] == 4 && previewPlayer.nextFrame().volume[1] == 2);
  previewPlayer.nextFrame();
  CHECK(!previewPlayer.sounding());

  // the song as sound: its 12 frames and the one where it ends, 262 lines each at 48 kHz, and a WAV of it
  std::vector<float> samples = renderSong(tune, "NTSC", 48000, 1000);
  double expected = 13 * 262 * 2 * 48000.0 / ntsc;
  CHECK(std::fabs(samples.size() - expected) < 4);
  float loudest = 0;
  for (float sample : samples) loudest = std::max(loudest, std::fabs(sample));
  CHECK(loudest > 0.05f && loudest <= 1.0f);
  CHECK(renderSong(tune, "PAL", 48000, 5).size() > 5 * 262 * 2 * 48000.0 / audioClockHz("PAL"));  // PAL frames are longer
  fs::path wavPath = fs::temp_directory_path() / "vcside-test-song.wav";
  CHECK(writeWav(wavPath.string(), samples, 48000, &error));
  std::ifstream wav(wavPath, std::ios::binary);
  std::string header(44, '\0');
  wav.read(&header[0], 44);
  CHECK(header.compare(0, 4, "RIFF") == 0 && header.compare(8, 8, "WAVEfmt ") == 0 && header.compare(36, 4, "data") == 0);
  CHECK(fs::file_size(wavPath) == 44 + samples.size() * 2);
  wav.close();
  fs::remove(wavPath);

  // The pitch in cents: every frame gets the AUDF closest to the note moved by those cents,
  // on the note's own AUDC; in AUDF steps the same step is a different interval on each note.
  Instrument vibrato;
  vibrato.controls = {4};
  vibrato.pitch = {0, 100, -100};
  Song plain = blankSong();
  std::vector<PitchStep> steps = plain.pitchSequence(81, vibrato, ntsc);  // A5
  NotePitch a5 = closestPitch(81, ntsc, {4}), b5 = closestPitch(82, ntsc, {4});
  CHECK(steps.size() == 3 && steps[0].frequency == a5.frequency && steps[1].control == 4 && steps[1].frequency == b5.frequency && steps[2].frequency > a5.frequency);
  CHECK(std::fabs(steps[1].cents - b5.cents) < 1e-6);  // measured against the note it aims at
  vibrato.pitch = {-1};
  vibrato.pitchUnit = PitchUnit::Audf;
  vibrato.controls = {12};  // ÷6 reaches both C4 and C6
  double highStep = plain.pitchSequence(84, vibrato, ntsc)[0].cents - plain.pitchOf(84, vibrato, ntsc).cents;
  double lowStep = plain.pitchSequence(60, vibrato, ntsc)[0].cents - plain.pitchOf(60, vibrato, ntsc).cents;
  CHECK(highStep > 2 * lowStep && lowStep > 0);
  // a note tuned by hand: its own AUDC and AUDF list, by the note heard
  vibrato.byHand[72] = {12, {11, 12}};
  plain.tuning.transpose = 2;
  steps = plain.pitchSequence(70, vibrato, ntsc);
  CHECK(steps.size() == 2 && steps[0].control == 12 && steps[0].frequency == 11 && steps[1].frequency == 12 && plain.pitchOf(70, vibrato, ntsc).frequency == 11);
  CHECK(plain.pitchSequence(72, vibrato, ntsc)[0].frequency != 11 || plain.pitchSequence(72, vibrato, ntsc)[0].control != 12);

  // The effects: V picks the volume envelope, R the release; a new note goes back to 0
  Song effects = blankSong();
  effects.speed = 2, effects.loopTo = -1;
  effects.patterns[0].resize(6);
  Instrument& voiced = effects.instruments[0];
  voiced.volumes = {{{10}, -1}, {{5}, -1}};
  voiced.releases = {{1}, {7, 7}};
  auto& lane = effects.patterns[0].cells[0];
  lane[0] = {60, 0, kEffectVolume, 1};
  lane[1] = {kNoNote, -1, kEffectVolume, 0};   // the held note changes envelope
  lane[2] = {kNoteOff, -1, kEffectRelease, 1};
  lane[3] = {62, 0};                          // back to envelope 0 and release 0
  lane[4] = {kNoteOff, -1};
  Player effectPlayer;
  effectPlayer.start(effects, ntsc);
  std::vector<int> effectVolumes;
  for (int frame = 0; frame < 12; frame++) effectVolumes.push_back(effectPlayer.nextFrame().volume[0]);
  CHECK(effectVolumes == std::vector<int>({5, 5, 10, 10, 7, 7, 10, 10, 1, 0, 0, 0}));
  CHECK(effectPlayer.trace(0).active == false);
  // a variant the instrument lacks is its first one
  lane[0].effectValue = 9;
  effectPlayer.start(effects, ntsc);
  CHECK(effectPlayer.nextFrame().volume[0] == 10);
  // what the register view explains: note, instrument, envelope frame and pitch
  effectPlayer.start(effects, ntsc);
  effectPlayer.nextFrame();
  effectPlayer.nextFrame();
  effectPlayer.nextFrame();
  const VoiceTrace& trace = effectPlayer.trace(0);
  CHECK(trace.active && trace.note == 60 && trace.instrument == 0 && trace.frame == 0 && !trace.released && trace.pitch.playable && trace.pitch.control == effectPlayer.nextFrame().control[0]);
}

// The exported player against the editor's: songs assembled with DASM and run on a 6502,
// frame by frame, every AUDC, AUDF and AUDV write compared. Skipped without DASM.
static std::string findDasm() {
  if (const char* given = std::getenv("VCSIDE_DASM")) return given;
#ifndef _WIN32
  if (std::system("command -v dasm >/dev/null 2>&1") == 0) return "dasm";
#endif
  return "";
}

struct AssembledSong {
  bool ok = false;
  std::string error;
  Cpu6502 cpu;
  uint16_t init = 0, frame = 0, effect = 0, play = 0, stop = 0;
  int bytes = 0;
};

// dpc: the DPC+ export, its three parts at $1000 (player), $4000 (waveforms) and $6000
// (frequencies), with WAVEFORM0-2 at $E0-$E2 and NOTE0-2 at $E3-$E5 standing in for DPC+.
static void assembleSong(const std::string& dasm, const std::string& exportText, AssembledSong& out, bool dpc = false) {
  namespace fs = std::filesystem;
  fs::path folder = fs::temp_directory_path() / "vcside-test-music";
  fs::create_directories(folder);
  {
    std::ofstream source(folder / "song.asm");
    source << "    processor 6502\nAUDC0 = $15\nAUDC1 = $16\nAUDF0 = $17\nAUDF1 = $18\nAUDV0 = $19\nAUDV1 = $1A\nMUSIC_RAM = $80\n";
    if (!dpc)
      source << "    ORG $1000\n" << exportText;
    else
      source << "WAVEFORM0 = $E0\nWAVEFORM1 = $E1\nWAVEFORM2 = $E2\nNOTE0 = $E3\nNOTE1 = $E4\nNOTE2 = $E5\nAMPLITUDE = $E6\n"
             << "    ORG $1000\nMUSIC_PART SET 1\n" << exportText << "    ORG $4000\nMUSIC_PART SET 2\n" << exportText
             << "    ORG $6000\nMUSIC_PART SET 3\n" << exportText;
  }
  std::string command = "cd \"" + folder.string() + "\" && \"" + dasm + "\" song.asm -f3 -osong.bin -ssong.sym -lsong.lst > song.log 2>&1";
  if (std::system(command.c_str()) != 0) {
    std::ifstream log(folder / "song.log");
    std::stringstream text;
    text << log.rdbuf();
    out.error = text.str();
    return;
  }
  std::ifstream binary(folder / "song.bin", std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(binary)), std::istreambuf_iterator<char>());
  out.bytes = (int)bytes.size();
  for (size_t index = 0; index < bytes.size() && 0x1000 + index < 65536; index++) out.cpu.memory[0x1000 + index] = (uint8_t)bytes[index];
  std::ifstream symbols(folder / "song.sym");
  for (std::string line; std::getline(symbols, line);) {
    std::istringstream words(line);
    std::string name, value;
    words >> name >> value;
    uint16_t address = (uint16_t)std::strtol(value.c_str(), nullptr, 16);
    if (name == "MusicInit") out.init = address;
    if (name == "MusicFrame") out.frame = address;
    if (name == "MusicEffect") out.effect = address;
    if (name == "MusicPlay") out.play = address;
    if (name == "MusicStop") out.stop = address;
  }
  out.ok = out.init && out.frame && out.effect;
  if (!out.ok) out.error = "no player symbols";
}

// A song made of random choices, with every feature the player has.
static vcs::music::Song randomSong(unsigned seed) {
  using namespace vcs::music;
  std::mt19937 random(seed);
  auto pick = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); };
  Song song = blankSong();
  song.speed = pick(1, 5);
  song.swing = pick(0, 1) ? pick(1, 6) : 0;
  song.tuning.transpose = pick(-3, 3);
  song.instruments.clear();
  int instrumentCount = pick(2, 5);
  for (int index = 0; index < instrumentCount; index++) {
    Instrument instrument;
    instrument.name = "i" + std::to_string(index);
    instrument.drum = pick(0, 3) == 0;
    static const int tonal[] = {1, 4, 6, 7, 12, 14, 15};
    instrument.controls = {tonal[pick(0, 6)]};
    if (pick(0, 1)) instrument.controls.push_back(tonal[pick(0, 6)]);
    instrument.volumes.clear();
    for (int variantIndex = pick(1, 3); variantIndex > 0; variantIndex--) {
      Envelope envelope;
      for (int frame = pick(0, 6); frame > 0; frame--) envelope.values.push_back(pick(0, 15));
      envelope.loop = !envelope.values.empty() && pick(0, 1) ? pick(0, (int)envelope.values.size() - 1) : -1;
      instrument.volumes.push_back(envelope);
    }
    instrument.releases.clear();
    for (int variantIndex = pick(1, 2); variantIndex > 0; variantIndex--) {
      std::vector<int> release;
      for (int frame = pick(0, 4); frame > 0; frame--) release.push_back(pick(0, 15));
      instrument.releases.push_back(release);
    }
    instrument.pitchUnit = pick(0, 1) ? PitchUnit::Cents : PitchUnit::Audf;
    for (int frame = pick(0, 1) ? pick(1, 5) : 0; frame > 0; frame--) instrument.pitch.push_back(instrument.pitchUnit == PitchUnit::Cents ? pick(-150, 150) : pick(-3, 3));
    if (pick(0, 2) == 0) instrument.byHand[pick(55, 80)] = {tonal[pick(0, 6)], {pick(0, 31), pick(0, 31)}};
    instrument.drumControl = {pick(1, 15), pick(1, 15)};
    instrument.drumFrequency = {pick(0, 31)};
    song.instruments.push_back(instrument);
  }
  song.patterns.clear();
  for (int patternIndex = pick(1, 3); patternIndex > 0; patternIndex--) {
    Pattern pattern;
    pattern.resize(pick(2, 12));
    for (int channelIndex = 0; channelIndex < kMaxChannels; channelIndex++)
      for (auto& cell : pattern.cells[channelIndex]) {
        int kind = pick(0, 9);
        if (kind < 4) cell.note = pick(40, 90), cell.instrument = pick(0, instrumentCount);  // sometimes one that does not exist
        else if (kind == 4) cell.note = kNoteOff;
        if (pick(0, 4) == 0) cell.effect = pick(0, 1) ? kEffectVolume : kEffectRelease, cell.effectValue = pick(0, 3);
      }
    song.patterns.push_back(pattern);
  }
  {  // some patterns with a speed of their own (a separate generator: the rest stays as it was)
    std::mt19937 speeds(seed * 31 + 7);
    auto speedPick = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(speeds); };
    for (auto& pattern : song.patterns)
      if (speedPick(0, 2) == 0) pattern.speed = speedPick(1, 5), pattern.swing = speedPick(0, 1) ? speedPick(1, 6) : 0;
  }
  song.order.clear();
  for (int entry = pick(1, 4); entry > 0; entry--) song.order.push_back(pick(0, (int)song.patterns.size() - 1));
  song.loopTo = pick(0, 2) ? pick(0, (int)song.order.size() - 1) : -1;
  for (int index = pick(0, 2); index > 0; index--) song.effects.push_back({"fx" + std::to_string(index), pick(0, instrumentCount - 1), pick(50, 80), pick(0, 1)});
  return song;
}

static void testMusicExport() {
  using namespace vcs::music;
  double ntsc = audioClockHz("NTSC");
  // what the export says without running it
  Song song = blankSong();
  song.patterns[0].cells[0][0] = {69, 0};
  song.effects.push_back({"Jump up!", 1, 60, 1});
  ExportResult exported = exportSong(song, ntsc, {});
  CHECK(exported.ok && exported.problems.empty() && exported.sounds == 2 && exported.text.find("MUSIC_EFFECT_JUMP_UP = 0") != std::string::npos);
  CHECK(exported.text.find("IFNCONST MUSIC_RAM") != std::string::npos && exported.text.find("CC0") != std::string::npos);
  ExportResult basic = exportSong(song, ntsc, {ExportTarget::BatariBasic, "h", "NTSC"});
  CHECK(basic.ok && basic.text.find("MUSIC_RAM = h") != std::string::npos && basic.text.find("IFNCONST") == std::string::npos);
  CHECK(effectSymbol("  boom 2 ") == "MUSIC_EFFECT_BOOM_2");
  // an effect whose volume never reaches 0 is reported: nothing would let it go
  Song held = song;
  held.instruments[held.effects[0].instrument].volumes[0] = {{12, 8}, -1};
  CHECK(exportSong(held, ntsc, {}).problems.size() == 1);
  held.instruments[held.effects[0].instrument].volumes[0] = {{12, 8, 0}, 1};
  CHECK(exportSong(held, ntsc, {}).problems.size() == 1);
  // a pattern that plays differently depending on what came before is reported
  Song ambiguous = blankSong();
  ambiguous.patterns[0].resize(2);
  Pattern second = ambiguous.patterns[0];
  ambiguous.patterns[0].cells[0][0] = {60, 0};
  second.cells[0][0] = {60, 1};
  ambiguous.patterns.push_back(second);
  Pattern shared;
  shared.resize(1);
  shared.cells[0][0].note = 62;  // no instrument: the one before it
  ambiguous.patterns.push_back(shared);
  ambiguous.order = {0, 2, 1, 2};
  CHECK(exportSong(ambiguous, ntsc, {}).problems.size() == 1);

  std::string dasm = findDasm();
  if (dasm.empty()) {
    std::printf("  (no DASM: the exported player is not run)\n");
    return;
  }
  int compared = 0, skipped = 0, heardFrames = 0, effectsPlayed = 0;
  long mostInstructions = 0;
  for (unsigned seed = 1; seed <= 60; seed++) {
    Song random = seed == 1 ? blankSong() : randomSong(seed);
    if (seed == 1) {
      random.patterns[0].cells[0][0] = {69, 0}, random.patterns[0].cells[1][4] = {60, 1}, random.patterns[0].cells[0][8] = {kNoteOff, -1};
      random.effects.push_back({"blip", 0, 72, 0});
    }
    ExportResult result = exportSong(random, ntsc, {});
    bool ambiguousSong = false;
    for (auto& problem : result.problems) ambiguousSong = ambiguousSong || problem.find("depending on what came before") != std::string::npos;
    if (ambiguousSong) {
      skipped++;
      continue;
    }
    CHECK(result.ok);
    if (!result.ok) continue;
    AssembledSong assembled;
    assembleSong(dasm, result.text, assembled);
    if (!assembled.ok) {
      std::printf("  seed %u: %s\n", seed, assembled.error.c_str());
      CHECK(assembled.ok);
      break;
    }
    Player player;
    player.start(random, ntsc);
    Cpu6502& cpu = assembled.cpu;
    CHECK(cpu.call(assembled.init));
    std::mt19937 effects(seed);
    int validEffects = 0;
    for (auto& effect : random.effects) validEffects += effect.instrument < (int)random.instruments.size();
    bool same = true;
    for (int frame = 0; frame < 1500 && same; frame++) {
      if (validEffects && std::uniform_int_distribution<int>(0, 60)(effects) == 0) {
        int effect = std::uniform_int_distribution<int>(0, validEffects - 1)(effects);
        player.playEffect(random, ntsc, effect);
        effectsPlayed++;
        cpu.a = (uint8_t)effect;
        same = cpu.call(assembled.effect);
      }
      FrameRegisters expected = player.nextFrame();
      long before = cpu.instructions;
      same = same && cpu.call(assembled.frame);
      mostInstructions = std::max(mostInstructions, cpu.instructions - before);
      heardFrames += expected.volume[0] || expected.volume[1];
      for (int channel = 0; channel < kChannels && same; channel++)
        same = cpu.memory[0x15 + channel] == expected.control[channel] && cpu.memory[0x17 + channel] == expected.frequency[channel] &&
               cpu.memory[0x19 + channel] == expected.volume[channel];
      if (!same)
        std::printf("  seed %u, frame %d: 6502 %d %d %d / %d %d %d, editor %d %d %d / %d %d %d\n", seed, frame, cpu.memory[0x15], cpu.memory[0x17], cpu.memory[0x19],
                    cpu.memory[0x16], cpu.memory[0x18], cpu.memory[0x1a], expected.control[0], expected.frequency[0], expected.volume[0], expected.control[1],
                    expected.frequency[1], expected.volume[1]);
    }
    CHECK(same);
    compared++;
    if (!same) break;
  }
  CHECK(compared >= 40 && heardFrames > 10000 && effectsPlayed > 100);
  CHECK(mostInstructions < 260);  // MusicFrame stays well inside a vertical blank
  std::printf("  exported player: %d songs the same as the editor's (%d skipped as ambiguous; %d frames with sound, %d effects), at most %ld instructions a frame\n",
              compared, skipped, heardFrames, effectsPlayed, mostInstructions);
}

// Several songs exported together, with one player: each one played with MusicPlay
// against the editor's player for that song alone, effects included (their numbers go on
// from one song to the next); a song started over another one as it plays (the last
// song's voices go quiet), and MusicStop letting the notes go.
static void testMusicSongs() {
  using namespace vcs::music;
  double ntsc = audioClockHz("NTSC");
  {  // the constants, and an effect two songs share kept once
    Song first = blankSong(), second;
    for (auto& instrument : first.instruments) instrument.volumes[0] = {{12, 8, 0}, -1};  // effects must end
    second = first;
    first.patterns[0].cells[0][0] = {69, 0};
    second.patterns[0].cells[1][0] = {60, 1};
    first.effects.push_back({"blip", 0, 72, 0});
    second.effects = first.effects;
    second.effects.push_back({"boom", 1, 50, 1});
    ExportResult both = exportSongs({{"title theme", &first}, {"game", &second}}, ntsc, {});
    CHECK(both.ok && both.problems.empty() && both.text.find("MUSIC_SONG_TITLE_THEME = 0\nMUSIC_SONG_GAME = 1\n") != std::string::npos);
    CHECK(both.text.find("MUSIC_EFFECT_BLIP = 0\nMUSIC_EFFECT_BOOM = 1\n") != std::string::npos && both.text.find("MUSIC_SONGS = 2") != std::string::npos);
    second.effects[0].note = 74;  // the same name, another sound
    CHECK(exportSongs({{"a", &first}, {"b", &second}}, ntsc, {}).problems.size() == 1);
    // songs that cannot go together are left out, the rest is exported
    Song samples = blankSong();
    samples.target = "samples";
    ExportResult mixed = exportSongs({{"a", &first}, {"b", &samples}, {"a", &second}}, ntsc, {});
    CHECK(mixed.ok && mixed.problems.size() == 2 && mixed.text.find("MUSIC_SONGS = 1") != std::string::npos);
    // a folder: the songs exported the same way, by file name, the one being edited as it is
    namespace fs = std::filesystem;
    fs::path folder = fs::temp_directory_path() / "vcside-test-songs";
    std::error_code errorCode;
    fs::remove_all(folder, errorCode);
    fs::create_directories(folder);
    first.exportTo = second.exportTo = "dasm";
    second.effects[0].note = 72;
    Song quiet = blankSong();  // not exported: left alone
    std::ofstream(folder / "b.vcsmusic") << toJson(first);
    std::ofstream(folder / "a.vcsmusic") << toJson(second);
    std::ofstream(folder / "c.vcsmusic") << toJson(quiet);
    Song edited = first;
    edited.speed = 3;
    FolderExport folderExport = exportFolder(folder.string(), ExportTarget::Dasm, ntsc, {}, (folder / "b.vcsmusic").string(), &edited);
    CHECK(folderExport.result.ok && folderExport.songs == std::vector<std::string>({"a", "b"}) && folderExport.path == (folder / "music.asm").string());
    CHECK(folderExport.result.text.find("MUSIC_SONG_A = 0") != std::string::npos && folderExport.result.text.find("MUSIC_PATTERN_SPEEDS = 1") != std::string::npos);
    std::string error;
    CHECK(writeFolderExport(folderExport, &error) && fs::exists(folder / "music.asm"));
    fs::remove_all(folder, errorCode);
  }
  std::string dasm = findDasm();
  if (dasm.empty()) {
    std::printf("  (no DASM: the songs played together are not run)\n");
    return;
  }
  int compared = 0, skipped = 0, switched = 0, effectsPlayed = 0;
  for (unsigned seed = 1; seed <= 30; seed++) {
    std::vector<Song> songs;
    for (unsigned index = 0; index < 3; index++) {
      Song song = randomSong(seed * 7 + index);
      for (auto& effect : song.effects) effect.name = "song" + std::to_string(index) + effect.name;  // each song's own
      songs.push_back(song);
    }
    std::vector<NamedSong> named;
    for (size_t index = 0; index < songs.size(); index++) named.push_back({"s" + std::to_string(index), &songs[index]});
    ExportResult result = exportSongs(named, ntsc, {});
    bool ambiguousSong = false;
    for (auto& problem : result.problems) ambiguousSong = ambiguousSong || problem.find("depending on what came before") != std::string::npos;
    if (ambiguousSong) {
      skipped++;
      continue;
    }
    CHECK(result.ok);
    if (!result.ok) continue;
    AssembledSong assembled;
    assembleSong(dasm, result.text, assembled);
    CHECK(assembled.ok && assembled.play && assembled.stop);
    if (!assembled.ok || !assembled.play || !assembled.stop) {
      std::printf("  seed %u: %s\n", seed, assembled.error.c_str());
      break;
    }
    auto sameFrame = [&](const Cpu6502& cpu, const FrameRegisters& expected) {
      for (int channel = 0; channel < kChannels; channel++)
        if (cpu.memory[0x15 + channel] != expected.control[channel] || cpu.memory[0x17 + channel] != expected.frequency[channel] ||
            cpu.memory[0x19 + channel] != expected.volume[channel])
          return false;
      return true;
    };
    int firstEffect = 0;
    for (size_t index = 0; index < songs.size(); index++) {
      const Song& song = songs[index];
      // each song from its start, with its effects at their numbers in the export
      Cpu6502 cpu = assembled.cpu;
      CHECK(cpu.call(assembled.init));
      cpu.a = (uint8_t)index;
      CHECK(cpu.call(assembled.play));
      Player player;
      player.start(song, ntsc);
      std::mt19937 effects(seed * 3 + (unsigned)index);
      bool same = true;
      for (int frame = 0; frame < 800 && same; frame++) {
        if (!song.effects.empty() && std::uniform_int_distribution<int>(0, 60)(effects) == 0) {
          int effect = std::uniform_int_distribution<int>(0, (int)song.effects.size() - 1)(effects);
          player.playEffect(song, ntsc, effect);
          cpu.a = (uint8_t)(firstEffect + effect);
          same = cpu.call(assembled.effect);
          effectsPlayed++;
        }
        FrameRegisters expected = player.nextFrame();
        same = same && cpu.call(assembled.frame) && sameFrame(cpu, expected);
        if (!same) std::printf("  seed %u, song %zu, frame %d: not the editor's\n", seed, index, frame);
      }
      CHECK(same);
      compared += same;
      firstEffect += (int)song.effects.size();
      // the next song started while this one plays: from there on, as that song from its start
      size_t next = (index + 1) % songs.size();
      cpu = assembled.cpu;
      CHECK(cpu.call(assembled.init));
      cpu.a = (uint8_t)index;
      CHECK(cpu.call(assembled.play));
      for (int frame = 0; frame < 37; frame++) CHECK(cpu.call(assembled.frame));
      cpu.a = (uint8_t)next;
      CHECK(cpu.call(assembled.play));
      Player nextPlayer;
      nextPlayer.start(songs[next], ntsc);
      for (int frame = 0; frame < 400 && same; frame++) same = cpu.call(assembled.frame) && sameFrame(cpu, nextPlayer.nextFrame());
      CHECK(same);
      switched += same;
      // MusicStop: the notes are let go and the song stops; silence follows
      CHECK(cpu.call(assembled.stop));
      bool silent = false;
      for (int frame = 0; frame < 300 && !silent; frame++) {
        CHECK(cpu.call(assembled.frame));
        silent = cpu.memory[0x19] == 0 && cpu.memory[0x1a] == 0 && cpu.memory[0x80] == 0xFF;
      }
      CHECK(silent);
    }
  }
  CHECK(compared >= 40 && switched >= 40 && effectsPlayed > 200);
  std::printf("  songs played together: %d the same as the editor's, %d started over another one (%d seeds skipped as ambiguous, %d effects)\n", compared, switched,
              skipped, effectsPlayed);
}

// Every other sound effect of a song of samples goes to the TIA's channel 1, with a copy of
// its instrument whose volume ends (an effect there must end). Returns how many.
static int markTiaEffects(vcs::music::Song& song, double clock) {
  using namespace vcs::music;
  int marked = 0;
  for (size_t index = 0; index < song.effects.size(); index += 2) {
    SoundEffect& effect = song.effects[index];
    if (effect.instrument >= (int)song.instruments.size()) continue;
    Instrument sound = song.instruments[effect.instrument];
    Envelope& volume = sound.volumes[0];
    volume.loop = -1;
    if (volume.values.empty() || volume.values.back() != 0) volume.values.push_back(0);
    song.instruments.push_back(sound);
    effect.instrument = (int)song.instruments.size() - 1;
    effect.tia = true;
    bool endless = false;
    CHECK(!tiaEffectFrames(song, clock, (int)index, kMaxTiaEffectFrames, &endless).empty() && !endless);
    marked++;
  }
  return marked;
}

// The TIA's channel 1 as the exported player left it, against the editor's: the volume
// always, AUDC1 and AUDF1 while it sounds.
static bool sameTia(const vcs::music::FrameRegisters& expected, int control, int frequency, int volume, int* heard) {
  if (volume != expected.tiaVolume) return false;
  if (!volume) return true;
  (*heard)++;
  return control == expected.tiaControl && frequency == expected.tiaFrequency;
}

// The DPC+ export: songs played as samples, assembled and run on a 6502 frame by frame;
// each voice's WAVEFORM must point at its waveform scaled to its volume, and its NOTE at
// its frequency (the phase step at DPC+'s 20 kHz). Skipped without DASM.
static vcs::music::Song randomSampleSong(unsigned seed);
static void testDpcExport() {
  using namespace vcs::music;
  // VCSIDE_DPC_SONG=<.vcsmusic> VCSIDE_DPC_OUT=<.asm>: that song exported for DPC+ (for a test ROM)
  if (const char* songPath = std::getenv("VCSIDE_DPC_SONG")) {
    std::ifstream file(songPath);
    std::string text((std::istreambuf_iterator<char>(file)), {}), error;
    Song song;
    CHECK(fromJson(text, &song, &error));
    ExportResult result = exportSong(song, audioClockHz("NTSC"), {ExportTarget::DpcPlus, "", "NTSC"});
    for (auto& problem : result.problems) std::printf("  DPC+ export: %s\n", problem.c_str());
    if (result.ok && std::getenv("VCSIDE_DPC_OUT")) std::ofstream(std::getenv("VCSIDE_DPC_OUT")) << result.text;
  }
  std::string dasm = findDasm();
  if (dasm.empty()) {
    std::printf("  (no DASM: the DPC+ player is not run)\n");
    return;
  }
  Song four = randomSampleSong(3);
  four.voices = 4;
  CHECK(!exportSong(four, audioClockHz("NTSC"), {ExportTarget::DpcPlus, "", "NTSC"}).ok);
  CHECK(!exportSong(blankSong(), audioClockHz("NTSC"), {ExportTarget::DpcPlus, "", "NTSC"}).ok);  // a TIA song
  int compared = 0, skipped = 0, soundingFrames = 0, effectsPlayed = 0, tiaFrames = 0;
  for (unsigned seed = 1; seed <= 40; seed++) {
    Song random = randomSampleSong(seed);
    random.voices = std::min(random.voices, 3);
    std::string tv = seed % 3 == 0 ? "PAL" : "NTSC";
    double clock = audioClockHz(tv);
    markTiaEffects(random, clock);
    ExportResult result = exportSong(random, clock, {ExportTarget::DpcPlus, "", tv});
    bool ambiguousSong = false;
    for (auto& problem : result.problems) ambiguousSong = ambiguousSong || problem.find("depending on what came before") != std::string::npos;
    if (ambiguousSong) {
      skipped++;
      continue;
    }
    if (!result.ok) std::printf("  seed %u: %s\n", seed, result.problems.empty() ? "?" : result.problems[0].c_str());
    CHECK(result.ok);
    if (!result.ok) continue;
    AssembledSong assembled;
    assembleSong(dasm, result.text, assembled, true);
    if (!assembled.ok) {
      std::printf("  seed %u: %s\n", seed, assembled.error.c_str());
      CHECK(assembled.ok);
      break;
    }
    Cpu6502& cpu = assembled.cpu;
    CHECK(cpu.call(assembled.init));
    Player player;
    player.start(random, clock);
    int voices = random.channelCount();
    std::mt19937 effects(seed);
    int validEffects = 0;
    for (auto& effect : random.effects) validEffects += effect.instrument < (int)random.instruments.size();
    bool same = true;
    for (int frame = 0; frame < 1000 && same; frame++) {
      if (validEffects && std::uniform_int_distribution<int>(0, 60)(effects) == 0) {
        int effect = std::uniform_int_distribution<int>(0, validEffects - 1)(effects);
        player.playEffect(random, clock, effect);
        effectsPlayed++;
        cpu.a = (uint8_t)effect;
        same = cpu.call(assembled.effect);
      }
      FrameRegisters expected = player.nextFrame();
      same = same && cpu.call(assembled.frame);
      if (same && !sameTia(expected, cpu.memory[0x16], cpu.memory[0x18], cpu.memory[0x1A], &tiaFrames)) {
        same = false;
        std::printf("  seed %u, frame %d: TIA channel 1 %d %d %d, editor %d %d %d\n", seed, frame, cpu.memory[0x16], cpu.memory[0x18], cpu.memory[0x1A],
                    expected.tiaControl, expected.tiaFrequency, expected.tiaVolume);
      }
      for (int voice = 0; voice < voices && same; voice++) {
        // the 32 samples the voice's WAVEFORM points at
        const uint8_t* wave = &cpu.memory[0x4000 + 32 * (cpu.memory[0xE0 + voice] & 0x7F)];
        int level = expected.sounding[voice] ? dpcLevel(expected.volume[voice], voices, random.gain) : 0;
        std::array<uint8_t, 32> shape{};
        if (level) shape = dpcWave(random.instruments[expected.instrument[voice]]);
        for (int sample = 0; sample < 32 && same; sample++) same = wave[sample] == (int)std::lround(shape[sample] * level / 15.0);
        if (level) {
          int note = cpu.memory[0xE3 + voice];
          uint32_t frequency = cpu.memory[0x6000 + 4 * note] | cpu.memory[0x6001 + 4 * note] << 8 | cpu.memory[0x6002 + 4 * note] << 16 |
                               (uint32_t)cpu.memory[0x6003 + 4 * note] << 24;
          same = same && note > 0 && frequency == dpcFrequency(expected.phaseStep[voice], clock);
          soundingFrames++;
        }
        if (!same)
          std::printf("  seed %u, frame %d, voice %d: level %d, instrument %d, step %u; 6502 slot %d note %d\n", seed, frame, voice, level,
                      expected.instrument[voice], expected.phaseStep[voice], cpu.memory[0xE0 + voice], cpu.memory[0xE3 + voice]);
      }
    }
    CHECK(same);
    compared++;
    if (!same) break;
  }
  CHECK(compared >= 25 && soundingFrames > 10000 && effectsPlayed > 50 && tiaFrames > 100);
  std::printf("  exported DPC+ player: %d songs the same as the editor's (%d skipped as ambiguous; %d voice frames with sound, %d effects, %d frames on the TIA's channel 1)\n",
              compared, skipped, soundingFrames, effectsPlayed, tiaFrames);
}

// The CDFJ export: the C player compiled with the computer's C compiler, its two hooks
// writing down what a CDFJ cartridge would be told; each voice's samples and frequency
// compared, frame by frame, with the editor's player. Skipped without a C compiler.
static std::string findCompiler();
static void testCdfjExport() {
  using namespace vcs::music;
  namespace fs = std::filesystem;
  CHECK(!exportSong(blankSong(), audioClockHz("NTSC"), {ExportTarget::Cdfj, "", "NTSC"}).ok);  // a TIA song
  std::string compiler = findCompiler();
  if (compiler.empty()) {
    std::printf("  (no C compiler: the CDFJ player is not run)\n");
    return;
  }
  fs::path folder = fs::temp_directory_path() / "vcside-test-cdfj";
  fs::create_directories(folder);
  {
    std::ofstream driver(folder / "driver.c");
    driver << "#include <stdio.h>\n#include <stdint.h>\n"
              "static uint8_t given_wave[4][32];\nstatic uint32_t given_note[4];\nstatic int given_restart[4], given_sounding[4];\n"
              "#define MUSIC_CDFJ_WAVE(v, s) do { for (int k = 0; k < 32; k++) given_wave[v][k] = (s)[k]; } while (0)\n"
              "#define MUSIC_CDFJ_NOTE(v, f, r) (given_note[v] = (f), given_restart[v] = (r), given_sounding[v] = 1)\n"
              "#include \"song.music.h\"\n"
              "int main(void) {\n  int frames;\n  if (scanf(\"%d\", &frames) != 1) return 1;\n  music_init();\n"
              "  for (int f = 0; f < frames; f++) {\n    int effect;\n    if (scanf(\"%d\", &effect) != 1) return 1;\n"
              "    if (effect >= 0) music_effect(effect);\n    for (int v = 0; v < 4; v++) given_sounding[v] = 0;\n    music_frame(262);\n"
              "    for (int v = 0; v < MUSIC_VOICES; v++) {\n      for (int k = 0; k < 32; k++) printf(\"%d \", given_wave[v][k]);\n"
              "      printf(\"%d %u %d \", given_sounding[v], (unsigned)given_note[v], given_restart[v]);\n    }\n"
              "    printf(\"%d %d %d\", music_tia[0], music_tia[1], music_tia[2]);\n"
              "    printf(\"\\n\");\n  }\n  return 0;\n}\n";
  }
  int compared = 0, skipped = 0, soundingFrames = 0, restarts = 0, tiaFrames = 0;
  for (unsigned seed = 1; seed <= 24; seed++) {
    Song random = randomSampleSong(seed);
    random.voices = std::min(random.voices, 3);
    std::string tv = seed % 3 == 0 ? "PAL" : "NTSC";
    double clock = audioClockHz(tv);
    markTiaEffects(random, clock);
    ExportResult result = exportSong(random, clock, {ExportTarget::Cdfj, "", tv});
    bool ambiguousSong = false;
    for (auto& problem : result.problems) ambiguousSong = ambiguousSong || problem.find("depending on what came before") != std::string::npos;
    if (ambiguousSong) {
      skipped++;
      continue;
    }
    CHECK(result.ok);
    if (!result.ok) continue;
    std::ofstream(folder / "song.music.h") << result.text;
    std::string build = "cd \"" + folder.string() + "\" && \"" + compiler + "\" -std=c11 -O1 -Wall -Wextra -o driver driver.c > build.log 2>&1";
    bool built = std::system(build.c_str()) == 0;
    std::ifstream buildLog(folder / "build.log");
    std::string warnings((std::istreambuf_iterator<char>(buildLog)), {});
    if (!built || !warnings.empty()) std::printf("  seed %u: %s\n", seed, warnings.c_str());
    CHECK(built && warnings.empty());
    if (!built) break;
    const int frames = 600;
    std::mt19937 effects(seed);
    int validEffects = 0;
    for (auto& effect : random.effects) validEffects += effect.instrument < (int)random.instruments.size();
    std::vector<int> schedule;
    for (int frame = 0; frame < frames; frame++)
      schedule.push_back(validEffects && std::uniform_int_distribution<int>(0, 40)(effects) == 0 ? std::uniform_int_distribution<int>(0, validEffects - 1)(effects) : -1);
    {
      std::ofstream input(folder / "input.txt");
      input << frames << "\n";
      for (int effect : schedule) input << effect << "\n";
    }
    std::string run = "cd \"" + folder.string() + "\" && ./driver < input.txt > output.txt";
    CHECK(std::system(run.c_str()) == 0);
    std::ifstream output(folder / "output.txt");
    Player player;
    player.start(random, clock);
    int voices = random.channelCount();
    bool same = true;
    std::vector<bool> pendingRestart(voices, false);
    for (int frame = 0; frame < frames && same; frame++) {
      if (schedule[frame] >= 0) player.playEffect(random, clock, schedule[frame]);
      FrameRegisters expected = player.nextFrame();
      for (int voice = 0; voice < voices && same; voice++) {
        int wave[32], sounding = 0, restart = 0;
        unsigned frequency = 0;
        for (int& sample : wave) output >> sample;
        output >> sounding >> frequency >> restart;
        int level = expected.sounding[voice] ? dpcLevel(expected.volume[voice], voices, random.gain) : 0;
        std::array<uint8_t, 32> shape{};
        if (level) shape = dpcWave(random.instruments[expected.instrument[voice]]);
        for (int sample = 0; sample < 32 && same; sample++) same = wave[sample] == (int)std::lround(shape[sample] * level / 15.0);
        // a note that starts silent is restarted when it is first heard
        pendingRestart[voice] = pendingRestart[voice] || (expected.sounding[voice] && expected.restart[voice]);
        same = same && (sounding != 0) == (level != 0);
        if (level) {
          same = same && frequency == dpcFrequency(expected.phaseStep[voice], clock) && (restart != 0) == pendingRestart[voice];
          pendingRestart[voice] = false;
          soundingFrames++;
          restarts += restart != 0;
        }
        if (!same)
          std::printf("  seed %u, frame %d, voice %d: level %d, step %u, restart %d; C sounding %d frequency %u restart %d\n", seed, frame, voice, level,
                      expected.phaseStep[voice], (int)expected.restart[voice], sounding, frequency, restart);
      }
      int control = -1, frequency = -1, volume = -1;
      output >> control >> frequency >> volume;
      if (same && !sameTia(expected, control, frequency, volume, &tiaFrames)) {
        same = false;
        std::printf("  seed %u, frame %d: TIA channel 1 %d %d %d, editor %d %d %d\n", seed, frame, control, frequency, volume, expected.tiaControl,
                    expected.tiaFrequency, expected.tiaVolume);
      }
    }
    CHECK(same);
    compared++;
    if (!same) break;
  }
  CHECK(compared >= 15 && soundingFrames > 5000 && restarts > 100 && tiaFrames > 50);
  std::printf("  exported CDFJ player: %d songs the same as the editor's (%d skipped as ambiguous; %d voice frames with sound, %d notes started, %d frames on the TIA's channel 1)\n",
              compared, skipped, soundingFrames, restarts, tiaFrames);
}

// The exported C player against the editor's: songs played as samples, compiled with the
// computer's C compiler and run frame by frame; every line's level compared with
// SampleMixer's. Skipped without a C compiler.
static std::string findCompiler() {
  if (const char* given = std::getenv("VCSIDE_CC")) return given;
#ifndef _WIN32
  if (std::system("command -v cc >/dev/null 2>&1") == 0) return "cc";
#endif
  return "";
}

static vcs::music::Song randomSampleSong(unsigned seed) {
  using namespace vcs::music;
  std::mt19937 random(seed * 7919);
  auto pick = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); };
  Song song = randomSong(seed);
  song.target = "samples";
  song.voices = pick(1, 4);
  song.gain = pick(8, 40);
  for (auto& instrument : song.instruments) {
    instrument.wave = (Waveform)pick(0, 5);
    instrument.duty = pick(1, 31);
    static const int lengths[] = {4, 5, 9, 15};
    instrument.noiseBits = lengths[pick(0, 3)];
    instrument.drawn.clear();
    for (int step = 0; step < 32; step++) instrument.drawn.push_back(pick(0, 15));
  }
  for (auto& effect : song.effects) effect.channel = pick(0, song.voices - 1);
  return song;
}

// How many frames of a TIATracker song (its .ttt as JSON) VCSIDE's player plays differently
// after the import, against a small model of how TIATracker plays it: rows, envelopes with
// a held loop and a release, percussion played once, the speed of channel 0's pattern
// (even and odd rows), two channels with their own sequences and jumps.
static int tiaTrackerDifferences(const nlohmann::json& song, const vcs::music::Song& imported, int frameCount, bool print) {
  using namespace vcs::music;
  using nlohmann::json;
  const json &instruments = song["instruments"], &percussion = song["percussion"], &patterns = song["patterns"], &channels = song["channels"];
  struct Channel {
    int kind = 0;  // 0 silent, 1 instrument, 2 percussion
    int number = 0, value = 0, frame = 0;
    bool released = false;
    size_t entry = 0, row = 0;
  } model[2];
  model[0].entry = song.value("startpattern0", 0), model[1].entry = song.value("startpattern1", 0);
  auto controlOf = [](int waveform, int value) {
    switch (waveform) {
      case 16: return value >= 32 ? 12 : 4;
      case 5: return 4;
      case 9: return 7;
      case 10: return 6;
      case 13: return 12;
      default: return waveform;
    }
  };
  bool globalSpeed = song.value("globalspeed", true);
  Player player;
  player.start(imported, audioClockHz(song.value("tvmode", std::string("ntsc")) == "pal" ? "PAL" : "NTSC"));
  int differences = 0, timer = 0;
  for (int frame = 0; frame < frameCount; frame++) {
    // a channel with no more patterns stops the song
    if (timer == 0 && (model[0].entry >= channels[0]["sequence"].size() || model[1].entry >= channels[1]["sequence"].size())) break;
    if (timer == 0) {
      {  // channel 0's pattern (or the song) and its row give the speed
        const json& pattern = globalSpeed ? song : patterns[channels[0]["sequence"][model[0].entry]["patternindex"].get<int>()];
        timer = model[0].row % 2 == 0 ? pattern["oddspeed"].get<int>() : pattern["evenspeed"].get<int>();  // as TIATracker does
      }
      for (int channel = 0; channel < 2; channel++) {
        Channel& voice = model[channel];
        const json& sequence = channels[channel]["sequence"];
        const json& notes = patterns[sequence[voice.entry]["patternindex"].get<int>()]["notes"];
        const json& cell = notes[voice.row];
        int type = cell["type"], number = cell["number"], value = cell["value"];
        if (type == 1) voice = {1, number, value, 0, false, voice.entry, voice.row};
        else if (type == 3) voice = {2, number, 0, 0, false, voice.entry, voice.row};
        else if (type == 2 && voice.kind == 1 && !voice.released) voice.released = true, voice.frame = instruments[voice.number]["releaseStart"];
        if (++voice.row == notes.size()) {
          voice.row = 0;
          int target = sequence[voice.entry]["gototarget"];
          voice.entry = target >= 0 ? (size_t)target : voice.entry + 1;
        }
      }
    }
    timer--;
    FrameRegisters registers = player.nextFrame();
    for (int channel = 0; channel < 2; channel++) {
      Channel& voice = model[channel];
      int control = 0, frequency = 0, volume = 0;
      if (voice.kind == 1) {
        const json& source = instruments[voice.number];
        int length = source["envelopeLength"], sustain = source["sustainStart"], release = source["releaseStart"];
        if (voice.frame < length) {
          control = controlOf(source["waveform"], voice.value);
          frequency = std::clamp((voice.value & 31) + source["frequencies"][voice.frame].get<int>(), 0, 31);
          volume = source["volumes"][voice.frame];
          if (source["waveform"] == 11) volume = 0;
          voice.frame++;
          if (!voice.released && voice.frame == release) voice.frame = sustain;
        } else {
          voice.kind = 0;
        }
      } else if (voice.kind == 2) {
        const json& source = percussion[voice.number];
        if (voice.frame < (int)source["envelopeLength"]) {
          control = controlOf(source["waveforms"][voice.frame], 0), frequency = source["frequencies"][voice.frame], volume = source["volumes"][voice.frame];
          voice.frame++;
        } else {
          voice.kind = 0;
        }
      }
      bool same = registers.volume[channel] == volume && (volume == 0 || (registers.control[channel] == control && registers.frequency[channel] == frequency));
      if (!same && print && differences < 5)
        std::printf("  TIATracker import, frame %d channel %d: expected %d/%d/%d, plays %d/%d/%d\n", frame, channel, control, frequency, volume,
                    registers.control[channel], registers.frequency[channel], registers.volume[channel]);
      differences += !same;
    }
  }
  return differences;
}

// TIATracker import: a song of our own in the .ttt format, played by a small model of how
// TIATracker plays it (rows, envelopes with a held loop and a release, percussion played
// once, two channels with their own sequences and jumps) and by VCSIDE's player after the
// import: every frame must write the same.
static void testTiaTrackerImport() {
  using namespace vcs::music;
  using nlohmann::json;
  auto instrument = [](const char* name, int waveform, std::vector<int> frequencies, std::vector<int> volumes, int sustain, int release) {
    return json{{"name", name}, {"waveform", waveform}, {"envelopeLength", (int)volumes.size()}, {"sustainStart", sustain},
                {"releaseStart", release}, {"frequencies", frequencies}, {"volumes", volumes}};
  };
  json instruments = json::array();
  instruments.push_back(instrument("lead", 4, {2, 1, 0, 0, 0, 0, 0}, {15, 13, 11, 10, 9, 5, 2}, 3, 5));
  instruments.push_back(instrument("pure", 16, {0, 0, 0, 0}, {12, 10, 8, 3}, 1, 3));
  instruments.push_back(instrument("bass", 13, {0, 0, 0, 0, 0}, {14, 12, 12, 11, 4}, 2, 4));  // AUDC 13 sounds as 12
  instruments.push_back(instrument("wobble", 4, {0, 1, -1, 0}, {9, 9, 9, 0}, 0, 3));          // bends while held
  for (int unused = 4; unused < 7; unused++) instruments.push_back(instrument("unused", 4, {0, 0}, {0, 0}, 0, 1));
  json percussion = json::array();
  percussion.push_back(json{{"name", "kick"}, {"envelopeLength", 4}, {"overlay", false}, {"frequencies", {28, 30, 31, 31}}, {"volumes", {15, 12, 6, 2}}, {"waveforms", {15, 15, 14, 14}}});
  percussion.push_back(json{{"name", "hat"}, {"envelopeLength", 2}, {"overlay", false}, {"frequencies", {1, 1}}, {"volumes", {8, 0}}, {"waveforms", {8, 8}}});
  for (int unused = 2; unused < 15; unused++) percussion.push_back(json{{"name", "x"}, {"envelopeLength", 1}, {"overlay", false}, {"frequencies", {0}}, {"volumes", {0}}, {"waveforms", {8}}});
  auto note = [](int type, int number, int value) { return json{{"type", type}, {"number", number}, {"value", value}}; };
  enum { H = 0, I = 1, P = 2, D = 3 };
  json patterns = json::array();
  // channel 0: an intro pattern of 6 rows, then a loop of 8
  patterns.push_back(json{{"name", "intro"}, {"evenspeed", 3}, {"oddspeed", 3}, {"notes", {note(I, 0, 17), note(H, 0, 0), note(I, 0, 14), note(H, 0, 0), note(P, 0, 0), note(H, 0, 0)}}});
  patterns.push_back(json{{"name", "verse"}, {"evenspeed", 4}, {"oddspeed", 2},
                          {"notes", {note(I, 1, 10), note(H, 0, 0), note(I, 1, 40), note(H, 0, 0), note(I, 3, 20), note(H, 0, 0), note(H, 0, 0), note(P, 0, 0)}}});
  // channel 1: a loop of 5 rows (lengths 8 and 5 meet every 40 rows)
  patterns.push_back(json{{"name", "drums"}, {"evenspeed", 3}, {"oddspeed", 3}, {"notes", {note(D, 0, 0), note(D, 1, 0), note(I, 2, 25), note(H, 0, 0), note(P, 0, 0)}}});
  json channels = json::array();
  channels.push_back(json{{"sequence", {json{{"patternindex", 0}, {"gototarget", -1}}, json{{"patternindex", 1}, {"gototarget", 1}}}}});
  channels.push_back(json{{"sequence", {json{{"patternindex", 2}, {"gototarget", 0}}}}});
  json song = {{"version", 1}, {"tvmode", "ntsc"}, {"globalspeed", false}, {"evenspeed", 3}, {"oddspeed", 3}, {"rowsperbeat", 4},
               {"metaName", "test song"}, {"instruments", instruments}, {"percussion", percussion}, {"patterns", patterns}, {"channels", channels}};

  Song imported;
  ImportResult result = importTiaTracker(song.dump(), "fallback", &imported);
  CHECK(result.ok && imported.name == "test song" && imported.speed == 3 && imported.swing == 0);
  CHECK(imported.instruments.size() == 6);  // the 4 instruments and 2 percussion sounds used
  {  // the verse's speeds, alternating, also where channel 1 cuts it on an odd row
    bool even = false, odd = false;
    for (auto& pattern : imported.patterns) even |= pattern.speed == 2 && pattern.swing == 4, odd |= pattern.speed == 4 && pattern.swing == 2;
    CHECK(even && odd);
    Song reread;
    std::string error;
    CHECK(fromJson(toJson(imported), &reread, &error) && reread.patterns.size() == imported.patterns.size());
    for (size_t index = 0; index < reread.patterns.size(); index++)
      CHECK(reread.patterns[index].speed == imported.patterns[index].speed && reread.patterns[index].swing == imported.patterns[index].swing);
  }
  CHECK(!importTiaTracker("{\"x\": 1}", "x", &imported).ok);

  int frames = 700, differences = tiaTrackerDifferences(song, imported, frames, true);
  CHECK(differences == 0);

  // what could not be kept is said: slides, overlay percussion
  json changed = song;
  changed["patterns"][1]["notes"][5] = note(4, 0, 2);  // a slide
  changed["percussion"][1]["overlay"] = true;

  result = importTiaTracker(changed.dump(), "fallback", &imported);
  CHECK(result.ok && result.warnings.size() == 4);  // + the instrument that bends while held and in its release

  // the songs of a folder, if given (VCSIDE_TTT_DIR): each imported and exported for DASM
  if (const char* folder = std::getenv("VCSIDE_TTT_DIR")) {
    for (auto& entry : std::filesystem::directory_iterator(folder)) {
      if (entry.path().extension() != ".ttt") continue;
      std::ifstream file(entry.path());
      std::stringstream text;
      text << file.rdbuf();
      Song folderSong;
      ImportResult folderResult = importTiaTracker(text.str(), entry.path().stem().string(), &folderSong);
      ExportOptions options;
      ExportResult exported = folderResult.ok ? exportSong(folderSong, audioClockHz("NTSC"), options) : ExportResult{};
      // compared with TIATracker's way where the import keeps it exactly (no slides, no
      // overlay percussion)
      bool exact = folderResult.ok;
      for (auto& warning : folderResult.warnings) exact &= warning.find("Slides") == std::string::npos && warning.find("Overlay") == std::string::npos;
      if (std::getenv("VCSIDE_TTT_DUMP")) std::printf("%s\n", toJson(folderSong).c_str());
      int folderDifferences = -1;
      if (exact) folderDifferences = tiaTrackerDifferences(nlohmann::json::parse(text.str()), folderSong, 3000, std::getenv("VCSIDE_TTT_PRINT") != nullptr);
      std::printf("  %s: %s, %zu instruments, %zu patterns, order %zu, export %s, %s\n", entry.path().filename().string().c_str(),
                  folderResult.ok ? "imported" : folderResult.error.c_str(), folderSong.instruments.size(), folderSong.patterns.size(),
                  folderSong.order.size(), exported.ok ? "ok" : "fails",
                  folderDifferences < 0 ? "not compared" : (std::to_string(folderDifferences) + " of 6000 channel frames differ").c_str());
      for (auto& warning : folderResult.warnings) std::printf("      %s\n", warning.c_str());
      for (auto& problem : exported.problems) std::printf("      ! %s\n", problem.c_str());
    }
  }
  std::printf("  TIATracker import: %d frames the same as TIATracker plays them\n", frames);
}

static void testSampleExport() {
  using namespace vcs::music;
  namespace fs = std::filesystem;
  double ntsc = audioClockHz("NTSC");
  // the waveforms and the mix
  Instrument square;
  square.duty = 8;
  auto levels = waveLevels(square);
  CHECK(levels[0] == 15 && levels[7] == 15 && levels[8] == 0 && levels[31] == 0);
  square.wave = Waveform::Triangle;
  CHECK(waveLevels(square)[15] == 15 && waveLevels(square)[16] == 15 && waveLevels(square)[0] == 0);
  CHECK(levelTable()[0] == 0 && levelTable()[60] == 15 && levelTable()[63] == 15);
  for (int quarter = 1; quarter < 64; quarter++) CHECK(levelTable()[quarter] >= levelTable()[quarter - 1]);
  // exact pitch: A4 is 440 Hz whatever the TV
  Song song = blankSong();
  song.target = "samples";
  uint32_t step = song.phaseSteps(69, song.instruments[0], ntsc)[0];
  CHECK(std::fabs(step * (ntsc / 2) / 4294967296.0 - 440.0) < 0.01);
  CHECK(song.pitchOf(61, song.instruments[0], ntsc).playable && song.pitchOf(61, song.instruments[0], ntsc).cents == 0);
  // a samples song in JSON
  song.voices = 3, song.gain = 20;
  song.instruments[1].wave = Waveform::Noise, song.instruments[1].noiseBits = 9;
  song.instruments[0].wave = Waveform::Drawn, song.instruments[0].drawn = std::vector<int>(32, 7);
  song.patterns[0].cells[2][3] = {64, 0};
  Song read;
  std::string error;
  CHECK(fromJson(toJson(song), &read, &error) && read.samples() && read.channelCount() == 3 && read.gain == 20);
  CHECK(read.instruments[1].wave == Waveform::Noise && read.instruments[1].noiseBits == 9 && read.instruments[0].drawn[5] == 7 && read.patterns[0].cells[2][3].note == 64);
  // a sound effect on the TIA's channel 1: kept in JSON, its instrument a TIA one (the snare,
  // a drum: AUDC 8 frame by frame), played apart from the voices and ending with its envelope
  SoundEffect zap;
  zap.name = "zap", zap.instrument = 1, zap.tia = true;
  song.effects = {zap};
  CHECK(fromJson(toJson(song), &read, &error) && read.effects.size() == 1 && read.effects[0].tia && song.playsOnTia(1) && !song.playsOnTia(0));
  std::vector<TiaFrame> zapFrames = tiaEffectFrames(song, ntsc, 0, kMaxTiaEffectFrames);
  CHECK(zapFrames.size() == 6 && zapFrames[5].volume == 0 && zapFrames[0].control == 8 && zapFrames[0].frequency == 2 && zapFrames[0].volume == 14 && zapFrames[2].frequency == 6);
  Player zapPlayer;
  zapPlayer.playEffect(song, ntsc, 0);
  FrameRegisters zapRegisters = zapPlayer.nextFrame();
  CHECK(zapRegisters.tiaVolume == 14 && !zapRegisters.sounding[0] && !zapRegisters.sounding[1] && !zapRegisters.sounding[2]);
  song.effects.clear();
  // samples go to C; TIA songs to DASM, batari Basic, or C (a TIA player)
  CHECK(!exportSong(song, ntsc, {}).ok);
  ExportResult tiaC = exportSong(blankSong(), ntsc, {ExportTarget::C, "", "NTSC"});
  CHECK(tiaC.ok && tiaC.text.find("#define MUSIC_TIA 1") != std::string::npos && tiaC.text.find("music_audio") != std::string::npos);
  ExportResult c = exportSong(song, ntsc, {ExportTarget::C, "", "NTSC"});
  CHECK(c.ok && c.text.find("#define MUSIC_VOICES 3") != std::string::npos && c.text.find("CC0") != std::string::npos);

  std::string compiler = findCompiler();
  if (compiler.empty()) {
    std::printf("  (no C compiler: the exported C player is not run)\n");
    return;
  }
  fs::path folder = fs::temp_directory_path() / "vcside-test-cmusic";
  fs::create_directories(folder);
  {
    std::ofstream driver(folder / "driver.c");
    driver << "#include <stdio.h>\n#include \"song.music.h\"\n"
              "int main(void) {\n  int frames, lines, song;\n  if (scanf(\"%d %d %d\", &frames, &lines, &song) != 3) return 1;\n  music_init();\n  if (song) music_play(song);\n"
              "  for (int f = 0; f < frames; f++) {\n    int effect;\n    if (scanf(\"%d\", &effect) != 1) return 1;\n"
              "    if (effect >= 0) music_effect(effect);\n    music_frame(lines);\n"
              "    for (int i = 0; i < lines - MUSIC_OVERBLANK_LINES; i++) printf(\"%d \", music_level[i]);\n"
              "    for (int j = 0; j < 31; j++) printf(\"%d \", music_nibbles[j]);\n"
              "    printf(\"%d %d %d\", music_tia[0], music_tia[1], music_tia[2]);\n"
              "    printf(\"\\n\");\n  }\n  return 0;\n}\n";
  }
  int compared = 0, skipped = 0, effectsPlayed = 0, tiaFrames = 0;
  long loudLines = 0;
  for (unsigned seed = 1; seed <= 24; seed++) {
    Song random = randomSampleSong(seed);
    int lines = seed % 3 == 0 ? 312 : 262;
    std::string tv = lines == 312 ? "PAL" : "NTSC";
    double clock = audioClockHz(tv);
    markTiaEffects(random, clock);
    ExportResult result = exportSong(random, clock, {ExportTarget::C, "", tv});
    bool ambiguousSong = false;
    for (auto& problem : result.problems) ambiguousSong = ambiguousSong || problem.find("depending on what came before") != std::string::npos;
    if (ambiguousSong) {
      skipped++;
      continue;
    }
    CHECK(result.ok);
    if (!result.ok) continue;
    std::ofstream(folder / "song.music.h") << result.text;
    std::string build = "cd \"" + folder.string() + "\" && \"" + compiler + "\" -std=c11 -O1 -Wall -Wextra -o driver driver.c > build.log 2>&1";
    bool built = std::system(build.c_str()) == 0;
    std::ifstream buildLog(folder / "build.log");
    std::string warnings((std::istreambuf_iterator<char>(buildLog)), {});
    if (!built || !warnings.empty()) std::printf("  seed %u: %s\n", seed, warnings.c_str());
    CHECK(built && warnings.empty());
    if (!built) break;
    // the same frames on both sides, with the same effects
    const int frames = 400;
    std::mt19937 effects(seed);
    int validEffects = 0;
    for (auto& effect : random.effects) validEffects += effect.instrument < (int)random.instruments.size();
    std::vector<int> schedule;
    for (int frame = 0; frame < frames; frame++)
      schedule.push_back(validEffects && std::uniform_int_distribution<int>(0, 40)(effects) == 0 ? std::uniform_int_distribution<int>(0, validEffects - 1)(effects) : -1);
    {
      std::ofstream input(folder / "input.txt");
      input << frames << " " << lines << " 0\n";
      for (int effect : schedule) input << effect << "\n";
    }
    std::string run = "cd \"" + folder.string() + "\" && ./driver < input.txt > output.txt";
    CHECK(std::system(run.c_str()) == 0);
    std::ifstream output(folder / "output.txt");
    Player player;
    player.start(random, clock);
    SampleMixer mixer;
    bool same = true;
    for (int frame = 0; frame < frames && same; frame++) {
      if (schedule[frame] >= 0) player.playEffect(random, clock, schedule[frame]), effectsPlayed++;
      std::vector<uint8_t> expected;
      FrameRegisters registers = player.nextFrame();
      mixer.render(random, registers, lines, expected);
      int kernel = lines - 63;
      const uint8_t* overblank = expected.data() + kernel;
      std::vector<int> wanted(expected.begin(), expected.begin() + kernel);
      wanted.push_back(((overblank[0] + overblank[1] + 1) >> 1) | overblank[2] << 4);
      for (int j = 1; j < 31; j++) wanted.push_back(overblank[2 * j + 1] | overblank[2 * j + 2] << 4);
      for (size_t index = 0; index < wanted.size() && same; index++) {
        int got = -1;
        output >> got;
        same = got == wanted[index];
        if (!same) std::printf("  seed %u, frame %d, value %zu: C %d, editor %d\n", seed, frame, index, got, wanted[index]);
      }
      int control = -1, frequency = -1, volume = -1;
      output >> control >> frequency >> volume;
      if (same && !sameTia(registers, control, frequency, volume, &tiaFrames)) {
        same = false;
        std::printf("  seed %u, frame %d: TIA channel 1 %d %d %d, editor %d %d %d\n", seed, frame, control, frequency, volume, registers.tiaControl,
                    registers.tiaFrequency, registers.tiaVolume);
      }
      for (uint8_t level : expected) loudLines += level > 0;
    }
    CHECK(same);
    compared++;
    if (!same) break;
  }
  CHECK(compared >= 15 && loudLines > 100000 && effectsPlayed > 50 && tiaFrames > 20);
  // songs exported together (the same voices and loudness), each one started with music_play
  int songsCompared = 0;
  for (unsigned seed = 31; seed <= 54; seed++) {
    std::vector<Song> songs = {randomSampleSong(seed), randomSampleSong(seed + 100)};
    songs[1].voices = songs[0].voices, songs[1].gain = songs[0].gain;
    songs[1].effects.clear();
    std::vector<NamedSong> named = {{"first", &songs[0]}, {"second", &songs[1]}};
    ExportResult result = exportSongs(named, ntsc, {ExportTarget::C, "", "NTSC"});
    bool ambiguousSong = false;
    for (auto& problem : result.problems) ambiguousSong = ambiguousSong || problem.find("depending on what came before") != std::string::npos;
    if (ambiguousSong || !result.ok) continue;
    std::ofstream(folder / "song.music.h") << result.text;
    std::string build = "cd \"" + folder.string() + "\" && \"" + compiler + "\" -std=c11 -O1 -Wall -Wextra -o driver driver.c > build.log 2>&1";
    CHECK(std::system(build.c_str()) == 0);
    const int frames = 300, lines = 262;
    {
      std::ofstream input(folder / "input.txt");
      input << frames << " " << lines << " 1\n";
      for (int frame = 0; frame < frames; frame++) input << "-1\n";
    }
    std::string run = "cd \"" + folder.string() + "\" && ./driver < input.txt > output.txt";
    CHECK(std::system(run.c_str()) == 0);
    std::ifstream output(folder / "output.txt");
    Player player;
    player.start(songs[1], ntsc);
    SampleMixer mixer;
    bool same = true;
    for (int frame = 0; frame < frames && same; frame++) {
      std::vector<uint8_t> expected;
      mixer.render(songs[1], player.nextFrame(), lines, expected);
      for (int line = 0; line < lines - 63 && same; line++) {
        int got = -1;
        output >> got;
        same = got == expected[line];
      }
      std::string rest;
      std::getline(output, rest);  // the overblank's nibbles and the TIA's channel 1
    }
    CHECK(same);
    songsCompared += same;
  }
  CHECK(songsCompared >= 10);
  {  // the TIA track: kept in the file; an effect there takes it until it ends, and the next note gets it back
    Song track = blankSong();
    track.target = "samples", track.voices = 2, track.tiaTrack = true;
    track.instruments[0].volumes[0] = {{9}, -1};  // held
    track.instruments.push_back(track.instruments[0]);
    track.instruments[2].volumes[0] = {{14, 12, 11, 10, 9, 8, 7, 0}, -1};  // the effect's: 7 frames, then it ends
    track.patterns[0].resize(8);
    track.speed = 4;
    track.patterns[0].cells[kTiaTrack][0] = {69, 0};
    track.patterns[0].cells[kTiaTrack][1] = {69, 0};  // frame 4, while the effect sounds: skipped
    track.patterns[0].cells[kTiaTrack][4] = {64, 0};
    track.effects = {{"zap", 2, 60, 0, true}};
    Song read;
    std::string error;
    CHECK(fromJson(toJson(track), &read, &error) && read.hasTiaTrack() && read.trackCount() == 3 && read.patterns[0].cells[kTiaTrack][4].note == 64);
    CHECK(read.playsOnTia(0) && !read.playsOnTia(1));
    Player player;
    player.start(track, ntsc);
    FrameRegisters first = player.nextFrame();
    CHECK(first.tiaVolume == 9);
    player.playEffect(track, ntsc, 0);
    bool effectHeard = true;
    for (int volume : {14, 12, 11, 10, 9, 8, 7}) effectHeard = effectHeard && player.nextFrame().tiaVolume == volume;
    CHECK(effectHeard);
    bool quietUntilNote = true;
    for (int frame = 8; frame < 16; frame++) quietUntilNote = quietUntilNote && player.nextFrame().tiaVolume == 0;  // row 1's note was skipped
    CHECK(quietUntilNote && player.nextFrame().tiaVolume == 9);  // row 4
    ExportResult dpcTrack = exportSong(track, ntsc, {ExportTarget::DpcPlus, "", "NTSC"});
    bool leftOut = false;
    for (auto& problem : dpcTrack.problems) leftOut = leftOut || problem.find("TIA track") != std::string::npos;
    CHECK(dpcTrack.ok && leftOut);
  }
  // the TIA track: random notes on the TIA's channel 1, with effects there taking it
  int trackCompared = 0, trackFrames = 0, trackEffects = 0;
  for (unsigned seed = 61; seed <= 90; seed++) {
    Song random = randomSampleSong(seed);
    random.tiaTrack = true;
    std::mt19937 notes(seed);
    auto pick = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(notes); };
    for (auto& pattern : random.patterns)
      for (auto& cell : pattern.cells[kTiaTrack]) {
        int kind = pick(0, 9);
        if (kind < 4) cell.note = pick(40, 80), cell.instrument = pick(0, (int)random.instruments.size() - 1);
        else if (kind == 4) cell.note = kNoteOff;
        if (pick(0, 5) == 0) cell.effect = pick(0, 1) ? kEffectVolume : kEffectRelease, cell.effectValue = pick(0, 2);
      }
    markTiaEffects(random, ntsc);
    ExportResult result = exportSong(random, ntsc, {ExportTarget::C, "", "NTSC"});
    bool ambiguousSong = false;
    for (auto& problem : result.problems) ambiguousSong = ambiguousSong || problem.find("depending on what came before") != std::string::npos;
    if (ambiguousSong || !result.ok) continue;
    CHECK(result.text.find("#define MUSIC_TIA_TRACK 1") != std::string::npos);
    std::ofstream(folder / "song.music.h") << result.text;
    std::string build = "cd \"" + folder.string() + "\" && \"" + compiler + "\" -std=c11 -O1 -Wall -Wextra -o driver driver.c > build.log 2>&1";
    bool built = std::system(build.c_str()) == 0;
    std::ifstream buildLog(folder / "build.log");
    std::string warnings((std::istreambuf_iterator<char>(buildLog)), {});
    if (!built || !warnings.empty()) std::printf("  seed %u: %s\n", seed, warnings.c_str());
    CHECK(built && warnings.empty());
    if (!built) break;
    const int frames = 800, lines = 262;
    std::mt19937 effects(seed);
    std::vector<int> schedule;
    for (int frame = 0; frame < frames; frame++)
      schedule.push_back(!random.effects.empty() && std::uniform_int_distribution<int>(0, 30)(effects) == 0
                             ? std::uniform_int_distribution<int>(0, (int)random.effects.size() - 1)(effects)
                             : -1);
    {
      std::ofstream input(folder / "input.txt");
      input << frames << " " << lines << " 0\n";
      for (int effect : schedule) input << effect << "\n";
    }
    std::string run = "cd \"" + folder.string() + "\" && ./driver < input.txt > output.txt";
    CHECK(std::system(run.c_str()) == 0);
    std::ifstream output(folder / "output.txt");
    Player player;
    player.start(random, ntsc);
    SampleMixer mixer;
    bool same = true;
    for (int frame = 0; frame < frames && same; frame++) {
      if (schedule[frame] >= 0) player.playEffect(random, ntsc, schedule[frame]), trackEffects++;
      std::vector<uint8_t> expected;
      FrameRegisters registers = player.nextFrame();
      mixer.render(random, registers, lines, expected);
      for (int line = 0; line < lines - 63 + 31 && same; line++) {
        int got = -1;
        output >> got;
        if (line < lines - 63) same = got == expected[line];
      }
      int control = -1, frequency = -1, volume = -1;
      output >> control >> frequency >> volume;
      if (same && !sameTia(registers, control, frequency, volume, &trackFrames)) {
        same = false;
        std::printf("  seed %u, frame %d: TIA track %d %d %d, editor %d %d %d\n", seed, frame, control, frequency, volume, registers.tiaControl, registers.tiaFrequency,
                    registers.tiaVolume);
      }
    }
    CHECK(same);
    trackCompared += same;
  }
  CHECK(trackCompared >= 15 && trackFrames > 2000 && trackEffects > 100);
  std::printf("  exported C player: %d songs the same as the editor's, line by line (%d skipped as ambiguous; %ld lines with sound, %d effects, %d frames on the TIA's channel 1); %d second songs of two\n",
              compared, skipped, loudLines, effectsPlayed, tiaFrames, songsCompared);
  std::printf("  the TIA track: %d songs the same as the editor's (%d frames heard on the TIA's channel 1, %d effects there)\n", trackCompared, trackFrames, trackEffects);
}

// ---- assistant ---------------------------------------------------------------------
// ---- bundled model: checksum, catalogue, helper command line ---------------------------
static void testBundledModel() {
  using namespace vcs::assistant;
  std::printf("bundled model\n");
  // SHA-256 test vectors (FIPS 180-4) and a message that crosses the padding boundary
  {
    Sha256 empty;
    CHECK(empty.hexDigest() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Sha256 abc;
    abc.update("abc", 3);
    CHECK(abc.hexDigest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Sha256 split;
    const char* text = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    split.update(text, 10);
    split.update(text + 10, std::strlen(text) - 10);
    CHECK(split.hexDigest() == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 million;
    std::string block(1000, 'a');
    for (int i = 0; i < 1000; i++) million.update(block.data(), block.size());
    CHECK(million.hexDigest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    namespace fs = std::filesystem;
    fs::path file = fs::temp_directory_path() / ("vcside_sha_" + std::to_string(std::rand()));
    std::ofstream(file, std::ios::binary) << "abc";
    CHECK(sha256OfFile(file.string()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fs::remove(file);
    CHECK(sha256OfFile((fs::temp_directory_path() / "vcside_no_such_file").string()).empty());
  }
  // the shipped catalogue: every entry pinned, https, a plain file name
  {
    std::vector<CatalogModel> catalog = loadCatalog(std::string(VCSIDE_SOURCE_DATA) + "/assistant/models.json");
    CHECK(catalog.size() >= 2);
    for (auto& model : catalog) {
      CHECK(model.sha256.size() == 64 && model.url.rfind("https://", 0) == 0 && model.bytes > 0 && !model.license.empty());
      CHECK(model.url.size() > model.file.size() && model.url.compare(model.url.size() - model.file.size(), model.file.size(), model.file) == 0);
    }
    // the machine decides: GPU memory first, otherwise the CPU within 60% of the RAM and at most 8 GB
    auto pick = [&](int ramGb, std::vector<Device> devices) {
      Hardware hardware;
      hardware.ramGb = ramGb, hardware.devices = std::move(devices), hardware.probed = true;
      return recommend(catalog, hardware);
    };
    CHECK(pick(8, {}).model && pick(8, {}).model->memoryGb == 4 && !pick(8, {}).onGpu);
    CHECK(pick(16, {}).model && pick(16, {}).model->memoryGb == 8);   // 8 <= 60% of 16
    CHECK(pick(64, {}).model && pick(64, {}).model->memoryGb == 8);   // no GPU: never above 8 GB
    CHECK(pick(2, {}).model == nullptr);
    Device appleGpu{"MTL0", "Apple M1 Pro", 12124, 12123, true};
    Recommendation apple = pick(16, {appleGpu, Device{"BLAS", "Accelerate", 0, 0, false}});
    CHECK(apple.model && apple.model->memoryGb == 8 && apple.onGpu && apple.device == "Apple M1 Pro");  // 11.8 GB free: 8 fits, 12 does not
    Recommendation big = pick(32, {Device{"CUDA0", "NVIDIA RTX", 24576, 23000, true}});
    CHECK(big.model && big.model->memoryGb == 12 && big.onGpu);
    Recommendation small = pick(16, {Device{"Vulkan0", "Intel UHD", 2048, 1800, true}});
    CHECK(small.model && !small.onGpu && small.model->memoryGb == 8);   // the GPU is too small: the CPU path
    CHECK(pick(16, {Device{"BLAS", "Accelerate", 0, 0, false}}).onGpu == false);
    // the helper's device list
    std::vector<Device> listed = parseDeviceList("Available devices:\n  MTL0: Apple M1 Pro (12124 MiB, 12123 MiB free)\n  BLAS: Accelerate (0 MiB, 0 MiB free)\nnoise\n  Vulkan0: AMD Radeon RX 6600 (8176 MiB, 8000 MiB free)\n");
    CHECK(listed.size() == 3 && listed[0].gpu && listed[0].freeMiB == 12123 && !listed[1].gpu && listed[2].name == "AMD Radeon RX 6600");
    Hardware two;
    two.devices = listed;
    CHECK(two.bestGpu() && two.bestGpu()->id == "MTL0");
    CHECK(parseDeviceList("").empty() && parseDeviceList("garbage (1 MiB)").empty());
    CHECK(formatBytes(5027783488) == "4.7 GB" && formatBytes(500 * 1048576) == "500 MB");
  }
  // an entry without a hash, with a path in the file name or over plain http is never offered
  {
    namespace fs = std::filesystem;
    fs::path file = fs::temp_directory_path() / ("vcside_models_" + std::to_string(std::rand()) + ".json");
    std::string hash(64, 'a');
    std::ofstream(file) << "{\"models\":["
        "{\"id\":\"ok\",\"file\":\"a.gguf\",\"url\":\"https://h/a.gguf\",\"sha256\":\"" << hash << "\",\"bytes\":1},"
        "{\"id\":\"nohash\",\"file\":\"b.gguf\",\"url\":\"https://h/b.gguf\",\"bytes\":1},"
        "{\"id\":\"path\",\"file\":\"../c.gguf\",\"url\":\"https://h/c.gguf\",\"sha256\":\"" << hash << "\",\"bytes\":1},"
        "{\"id\":\"http\",\"file\":\"d.gguf\",\"url\":\"http://h/d.gguf\",\"sha256\":\"" << hash << "\",\"bytes\":1}]}";
    std::vector<CatalogModel> catalog = loadCatalog(file.string());
    CHECK(catalog.size() == 1 && catalog[0].id == "ok");
    fs::remove(file);
    CHECK(loadCatalog((fs::temp_directory_path() / "vcside_no_such_catalog").string()).empty());
  }
  // the helper only listens on this computer and runs one chat at a time
  {
    std::vector<std::string> arguments = helperArguments("/m/model.gguf", 4242, 8192);
    auto has = [&](const std::string& flag, const std::string& value) {
      for (size_t i = 0; i + 1 < arguments.size(); i++)
        if (arguments[i] == flag && arguments[i + 1] == value) return true;
      return false;
    };
    CHECK(has("--host", "127.0.0.1") && has("--port", "4242") && has("-m", "/m/model.gguf") && has("-c", "8192") && has("-np", "1"));
    int port = freeLoopbackPort();
    CHECK(port > 1023 && port < 65536);
  }
#ifndef _WIN32
  // Job::stop ends a long-running program and stopAndWait lets it start again
  {
    Job job;
    CHECK(job.start({"sleep", "30"}, ""));
    CHECK(job.running());
    job.stopAndWait();
    CHECK(!job.running() && job.finished());
    CHECK(job.start({"echo", "again"}, ""));
    job.stopAndWait();
  }
  // the engine reports a model that is not there instead of starting anything
  {
    Engine engine;
    setenv("VCSIDE_LLM_HELPER_PATH", "/bin/echo", 1);
    CHECK(engine.available());
    engine.start("/no/such/model.gguf", 4096);
    CHECK(engine.state() == Engine::State::Failed && engine.error().find("missing") != std::string::npos);
    unsetenv("VCSIDE_LLM_HELPER_PATH");
  }
#endif
}

// ---- Markdown in answers ----------------------------------------------------------------
static void testMarkdown() {
  using namespace vcs::assistant;
  std::printf("markdown\n");
  auto runs = parseInline("use **HMP0** and `RESP0`, not *this*; 2 * 3 stays; [docs](http://x) too");
  CHECK(runs.size() >= 6);
  CHECK(runs[1].bold && runs[1].text == "HMP0");
  bool codeRun = false, italicRun = false, star = false, link = false;
  for (auto& run : runs) {
    codeRun = codeRun || (run.code && run.text == "RESP0");
    italicRun = italicRun || (run.italic && run.text == "this");
    star = star || run.text.find("2 * 3") != std::string::npos;
    link = link || run.text.find("docs too") != std::string::npos || run.text.find("docs") != std::string::npos;
  }
  CHECK(codeRun && italicRun && star && link);
  // unfinished marks stay as typed (an answer that is still streaming)
  auto open = parseInline("half **bold");
  CHECK(open.size() == 1 && open[0].text == "half **bold" && !open[0].bold);
  auto snake = parseInline("the my__name__x identifier");
  CHECK(snake.size() == 1 && !snake[0].bold);
  {  // a closing * followed by a space still closes: "*Dirección:* el índice"
    auto closed = parseInline("*Direction:* the index");
    CHECK(closed.size() == 2 && closed[0].italic && closed[0].text == "Direction:" && !closed[1].italic && closed[1].text == " the index");
  }
  CHECK(parseInline("a\\*b").size() == 1 && parseInline("a\\*b")[0].text == "a*b");  // escaped

  auto blocks = parseMarkdown("### Title\n\nFirst line\nsecond line.\n\n- one\n- two **b**\n  - nested\n1. first\n2. second\n> quoted\n---\n| a | b |\n|---|---|\n| 1 | 2 |\n");
  CHECK(blocks.size() == 10);
  CHECK(blocks[0].kind == MdBlock::Heading && blocks[0].level == 3 && blocks[0].runs[0].text == "Title");
  CHECK(blocks[1].kind == MdBlock::Paragraph && blocks[1].runs[0].text == "First line second line.");  // wrapped lines join
  CHECK(blocks[2].kind == MdBlock::Bullet && blocks[3].kind == MdBlock::Bullet && blocks[3].runs.size() == 2 && blocks[3].runs[1].bold);
  CHECK(blocks[4].kind == MdBlock::Bullet && blocks[4].level == 1);
  CHECK(blocks[5].kind == MdBlock::Number && blocks[5].number == 1 && blocks[6].number == 2);
  CHECK(blocks[7].kind == MdBlock::Quote && blocks[8].kind == MdBlock::Rule);
  CHECK(blocks[9].kind == MdBlock::Table && blocks[9].rows.size() == 2 && blocks[9].rows[0].size() == 2 && blocks[9].rows[1][1][0].text == "2");  // the separator row is dropped
  CHECK(parseMarkdown("").empty() && parseMarkdown("\n\n").empty());
  // a number in a sentence is not a list, and neither is a hyphen inside a word
  auto plain = parseMarkdown("2026 was a year\nwell-known 3.5 mm");
  CHECK(plain.size() == 1 && plain[0].kind == MdBlock::Paragraph);
  // what the answer in the bug report looked like
  auto answer = parseMarkdown("Use **HMP0** ($04, W):\n\n- **HMP0** ($04, W): motion\n- **HMP1**: motion\n");
  CHECK(answer.size() == 3 && answer[1].kind == MdBlock::Bullet && answer[1].runs[0].bold);
}

#ifndef _WIN32
// A one-shot HTTP server on loopback: reads a request, answers with `reply`, closes.
struct MockServer {
  int listener = -1, port = 0;
  std::string request;
  std::thread thread;
  MockServer(const std::string& reply, size_t pieces = 1) {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, (sockaddr*)&address, sizeof address);
    socklen_t length = sizeof address;
    getsockname(listener, (sockaddr*)&address, &length);
    port = ntohs(address.sin_port);
    listen(listener, 1);
    thread = std::thread([this, reply, pieces] {
      int client = accept(listener, nullptr, nullptr);
      char buffer[8192];
      for (;;) {  // until the header and the whole body have arrived
        ssize_t n = recv(client, buffer, sizeof buffer, 0);
        if (n <= 0) break;
        request.append(buffer, (size_t)n);
        size_t end = request.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        size_t cl = request.find("Content-Length: ");
        size_t want = cl == std::string::npos ? 0 : (size_t)std::atoll(request.c_str() + cl + 16);
        if (request.size() >= end + 4 + want) break;
      }
      size_t step = std::max<size_t>(1, reply.size() / pieces);
      for (size_t i = 0; i < reply.size(); i += step) {
        send(client, reply.data() + i, std::min(step, reply.size() - i), 0);
        usleep(2000);
      }
      close(client);
    });
  }
  ~MockServer() {
    thread.join();
    close(listener);
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/v1"; }
};

static std::string chunk(const std::string& data) {
  char size[16];
  std::snprintf(size, sizeof size, "%zx", data.size());
  return std::string(size) + "\r\n" + data + "\r\n";
}
#endif

static void testAgentTools(const KnowledgeBase& kb) {
  namespace fs = std::filesystem;
  using namespace vcs::assistant;
  {  // checks of a proposed change, with no model
    auto knows = [](const std::string& word) { return word == "playercontrols" || word == "player_animation"; };
    std::string before = "PandaControls\n  lda SWCHA\n  rts\n";
    std::string viaPort = "PandaControls\n  lda SWCHA\n  and #%00000001  ; fire button\n  beq .Pressed\n  rts\n";
    auto wrong = proposalProblems("el oso debe saltar cuando se pulse el botón del joystick", before, viaPort, kb, knows, false);
    CHECK(wrong.find("INPT4") != std::string::npos && wrong.find("SWCHA") != std::string::npos);
    std::string viaFire = "PandaControls\n  lda SWCHA\n  lda INPT4\n  bmi .Skip\n.Skip\n  rts\n";
    CHECK(proposalProblems("el oso debe saltar cuando se pulse el botón del joystick", before, viaFire, kb, knows, false).empty());
    CHECK(proposalProblems("mueve el oso a la izquierda", before, viaPort, kb, knows, false).find("INPT4") == std::string::npos);  // not about the button
    std::string invented = "PandaControls\n  lda SWCHA\n  lda #JUMP_ANIMATION\n  sta PLAYER_ANIMATION\n  rts\n";
    auto unknown = proposalProblems("haz que salte", before, invented, kb, knows, false);
    CHECK(unknown.find("JUMP_ANIMATION") != std::string::npos && unknown.find("PLAYER_ANIMATION") == std::string::npos);
    std::string defined = "JUMP_ANIMATION EQU 3\nPandaControls\n  lda SWCHA\n  lda #JUMP_ANIMATION\n  rts\n";
    CHECK(proposalProblems("haz que salte", before, defined, kb, knows, false).empty());
    CHECK(addedLines("a\nb\n", "a\nc\nb\n") == "c\n");
    // an addition that throws away existing code, and labels that nothing defines (mixed case ones)
    std::string wide = "Animate\n  inc FRAME\n  lda ANIM\n  cmp #3\n  bne .Done\n  lda #1\n  sta ANIM\n.Done\n  rts\n";
    std::string thrown = "Animate\n  inc FRAME\n  lda INPT4\n  bpl .Pressed\n  jsr Jump\n.Pressed\n  lda FireWas\n.Done\n  rts\n";
    auto both = proposalProblems("el oso debe saltar cuando se pulse el botón", wide, thrown, kb, knows, false);
    CHECK(both.find("deletes 5 lines") != std::string::npos && both.find("Jump") != std::string::npos && both.find("FireWas") != std::string::npos && both.find(".Pressed") == std::string::npos);
    CHECK(proposalProblems("quita la animación", wide, thrown, kb, knows, false).find("deletes") == std::string::npos);  // asked to remove
    std::string fine = "FireWas DS 1\nAnimate\n  inc FRAME\n  lda ANIM\n  cmp #3\n  bne .Done\n  lda #1\n  sta ANIM\n  lda INPT4\n  bpl .Pressed\n  jsr Jump\n.Pressed\n  lda FireWas\n.Done\n  rts\nJump\n  rts\n";
    CHECK(proposalProblems("el oso debe saltar cuando se pulse el botón", wide, fine, kb, knows, false).empty());
    CHECK(proposalProblems("haz que salte", before, "PandaControls\n  lda SWCHA\n  bra .Skip\n.Skip\n  stz X\n  rts\n", kb, knows, false).find("no bra, stz instruction") != std::string::npos);
    std::string inverted = "PandaControls\n  lda SWCHA\n  lda INPT4\n  bmi .FireButtonPressed\n  jmp .NoJump\n.FireButtonPressed\n.NoJump\n  rts\n";
    CHECK(proposalProblems("el oso debe saltar al pulsar el botón", before, inverted, kb, knows, false).find("bmi branches when the button is NOT pressed") != std::string::npos);
    std::string right = "PandaControls\n  lda SWCHA\n  lda INPT4\n  bpl .FireButtonPressed\n  jmp .NoJump\n.FireButtonPressed\n.NoJump\n  rts\n";
    CHECK(proposalProblems("el oso debe saltar al pulsar el botón", before, right, kb, knows, false).empty());
    CHECK(proposalProblems("haz que salte", before, "PandaControls\n\tlda SWCHA\n   rts\n", kb, knows, false).find("changes nothing") != std::string::npos);  // only the spaces differ
  }
  fs::path root = fs::temp_directory_path() / "vcside_agent_test";
  fs::remove_all(root);
  fs::create_directories(root / "sub");
  fs::create_directories(root / ".vcsai");
  fs::create_directories(root.parent_path() / "vcside_agent_outside");
  auto write = [](const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; };
  write(root / "main.asm", "  processor 6502\nStart\n  sta WSYNC\n  sta WSYNC\n  jmp Start\n");
  write(root / "sub" / "util.asm", "Util\n  rts\n");
  write(root / ".env", "KEY=secret\n");
  write(root / "game.bin", std::string("\0\1\2", 3));
  write(root / "Makefile", "all:\n\ttrue\n");
  write(root.parent_path() / "vcside_agent_outside" / "x.txt", "outside");
  std::string dir = root.string();

  // the sandbox
  std::string absolute, error;
  CHECK(resolveInProject(dir, "main.asm", &absolute, &error));
  CHECK(!resolveInProject(dir, "../vcside_agent_outside/x.txt", &absolute, &error));
  CHECK(!resolveInProject(dir, "/etc/passwd", &absolute, &error));
  CHECK(!resolveInProject(dir, "sub/../../x", &absolute, &error));
  CHECK(!resolveInProject(dir, ".env", &absolute, &error));
  CHECK(!resolveInProject(dir, ".vcsai/chats/a.json", &absolute, &error));
  CHECK(!resolveInProject(dir, "", &absolute, &error));
  CHECK(!resolveInProject("", "main.asm", &absolute, &error));
  CHECK(isSecretFile("id_rsa") && isSecretFile("server.PEM") && isSecretFile(".env.local") && !isSecretFile("environment.asm"));
#ifndef _WIN32
  fs::create_directory_symlink(root.parent_path() / "vcside_agent_outside", root / "escape");
  CHECK(!resolveInProject(dir, "escape/x.txt", &absolute, &error));
#endif

  ToolEnv env;
  env.projectDir = dir;
  env.kb = &kb;
  env.projectInfo = "DASM, 4K, NTSC, main.asm";
  env.buildErrors = "main.asm:3: error: x\n";
  env.symbols = "Start main.asm:2\nUtil sub/util.asm:1\n";
  auto call = [&](const std::string& name, const nlohmann::json& args) { return runTool(env, {name, args}); };

  auto listing = call("list_files", {{"path", "."}});
  CHECK(!listing.error && listing.text.find("main.asm") != std::string::npos && listing.text.find("sub/util.asm") != std::string::npos);
  CHECK(listing.text.find(".env") == std::string::npos && listing.text.find(".vcsai") == std::string::npos);
  auto read = call("read_file", {{"path", "main.asm"}, {"from", 2}, {"to", 3}});
  CHECK(read.text == "2: Start\n3:   sta WSYNC\n[showing lines 2-3 of 5; ask for the next lines with from=4]\n");
  // reading a folder lists it; a small window gets small answers with the marker; very long lines are clipped
  CHECK(call("read_file", {{"path", "sub"}}).text.find("sub/util.asm") != std::string::npos);
  {
    ToolEnv small = env;
    small.readCapChars = 20;
    auto capped = runTool(small, {"read_file", {{"path", "main.asm"}}});
    CHECK(capped.text.find("1:   processor 6502\n") == 0 && capped.text.find("[showing lines 1-1 of 5; ask for the next lines with from=2]") != std::string::npos);
    small.searchLimit = 1;
    auto fewer = runTool(small, {"search", {{"text", "wsync"}}});
    CHECK(fewer.text.find("  3:") != std::string::npos && fewer.text.find("  4:") == std::string::npos && fewer.text.find("1 more lines not shown") != std::string::npos);
  }
  CHECK(toolNames(2, 2).size() == 13 && [] { auto names = toolNames(2, 2); return std::find(names.begin(), names.end(), "list_files") == names.end() && std::find(names.begin(), names.end(), "project_info") == names.end(); }() && toolNames(2, 1).size() == 6 && toolNames(0, 1).size() == 5 && toolNames(1, 1).size() == 5 && toolNames(2).size() == 15);
  CHECK(toolPrompt(2, false, 1).find("symbols") == std::string::npos && toolPrompt(2, false, false).find("symbols") != std::string::npos && toolPrompt(2, true).find("```tool") == std::string::npos);
  CHECK(call("read_file", {{"path", ".env"}}).error);
  CHECK(call("read_file", {{"path", "game.bin"}}).error);
  CHECK(call("read_file", {{"path", "../x"}}).error);
  auto found = call("search", {{"text", "wsync"}});
  CHECK(found.text.find("main.asm (") != std::string::npos && found.text.find("  3:") != std::string::npos && found.text.find("  4:") != std::string::npos && found.text.find(".env") == std::string::npos);
  // alternatives with |, and a place to look in
  auto either = call("search", {{"text", "nothing-here|WSYNC"}});
  CHECK(either.text.find("  3:") != std::string::npos);
  CHECK(call("search", {{"text", "wsync"}, {"path", "*.txt"}}).text == "no matches\n" && call("search", {{"text", "wsync"}, {"path", "main.asm"}}).text.find("  3:") != std::string::npos && call("search", {{"text", "wsync"}, {"path", "sub"}}).text == "no matches\n");
  CHECK(call("symbols", {{"name", "util"}}).text == "Util sub/util.asm:1\n");
  CHECK(call("build_errors", {}).text == env.buildErrors);
  CHECK(call("project_info", {}).text.find("DASM") != std::string::npos);
  CHECK(call("kb_lookup", {{"id", "RESP0"}}).text.find("RESP0") != std::string::npos);
  CHECK(call("kb_search", {{"query", "how do I make a tone"}}).text.find("AUDC") != std::string::npos);
  CHECK(call("nonsense", {}).error);
  // the same argument under other names, or under none when there is only one text
  CHECK(call("kb_search", {{"q", "how do I make a tone"}}).text.find("AUDC") != std::string::npos);
  CHECK(call("kb_search", {{"topic_words", "how do I make a tone"}}).text.find("AUDC") != std::string::npos);
  CHECK(call("kb_lookup", {{"register", "RESP0"}}).text.find("RESP0") != std::string::npos);
  CHECK(call("search", {{"keyword", "wsync"}}).text.find("  3:") != std::string::npos);
  CHECK(call("read_file", {{"file", "main.asm"}, {"from", 2}, {"to", 2}}).text.find("2: Start") != std::string::npos);
  CHECK(call("find_code", {}).error && call("find_code", {{"a", "x"}, {"b", "y"}}).error);  // nothing, or two texts and no way to tell: refused with a reason
  CHECK(call("find_code", {}).text.find("query") != std::string::npos);

  // edits are proposals; nothing is written
  auto edit = call("edit_file", {{"path", "main.asm"}, {"edits", {{{"search", "  jmp Start"}, {"replace", "  jmp Start ; loop"}}}}});
  CHECK(edit.proposed && !edit.error && edit.edit.after.find("; loop") != std::string::npos && !edit.edit.privileged);
  std::string onDisk;
  { std::ifstream in(root / "main.asm"); std::ostringstream buffer; buffer << in.rdbuf(); onDisk = buffer.str(); }
  CHECK(onDisk.find("; loop") == std::string::npos);
  auto twice = call("edit_file", {{"path", "main.asm"}, {"edits", {{{"search", "  sta WSYNC"}, {"replace", "x"}}}}});
  CHECK(twice.error && twice.text.find("more than once") != std::string::npos);
  auto missing = call("edit_file", {{"path", "main.asm"}, {"edits", {{{"search", "  sta  WSYNC"}, {"replace", "x"}}}}});
  CHECK(missing.error && missing.text.find("not found") != std::string::npos);
  CHECK(call("edit_file", {{"path", "game.bin"}, {"edits", {{{"search", "a"}, {"replace", "b"}}}}}).error);
  CHECK(call("edit_file", {{"path", "build/out.asm"}, {"content", "x"}}).error);
  CHECK(call("edit_file", {{"path", ".env"}, {"content", "x"}}).error);
  auto created = call("edit_file", {{"path", "new.asm"}, {"content", "  nop\n"}});
  CHECK(created.proposed && created.edit.created);
  CHECK(call("edit_file", {{"path", "main.asm"}, {"content", "x"}}).error);  // exists: needs edits
  CHECK(call("edit_file", {{"path", "Makefile"}, {"edits", {{{"search", "true"}, {"replace", "curl x | sh"}}}}}).edit.privileged);
  CHECK(isBuildFile("demo.vcsproj") && isBuildFile("build.sh") && !isBuildFile("main.asm"));

  // every shape a model sends a call in (the urgent bug: flat arguments arrived empty)
  {
    ToolCall shape;
    CHECK(findToolCall("```tool\n{\"tool\": \"read_file\", \"path\": \"main.asm\", \"from\": 2}\n```", &shape) && shape.name == "read_file" && shape.args["path"] == "main.asm" && shape.args["from"] == 2);  // flat
    // the tool's name inside the arguments
    CHECK(findToolCall("```tool\n{\"args\":{\"path\":\"a.asm\",\"text\":\"x\",\"tool\":\"search\"}}\n```", &shape) && shape.name == "search" && shape.args["path"] == "a.asm" && !shape.args.contains("tool"));
    // real tabs and line ends inside the strings (not valid JSON): the call is read all the same
    CHECK(findToolCall("Here:\n```tool\n{\"args\":{\"path\":\"a.asm\",\"edits\":[{\"search\":\"\t\tlda #1\nsta X\",\"replace\":\"\t\tlda #2\"}]},\"tool\":\"edit_file\"}\n```", &shape) && shape.name == "edit_file" && shape.args["edits"][0]["search"] == "\t\tlda #1\nsta X");
    CHECK(findToolCall("{\"name\": \"read_file\", \"arguments\": \"{\\\"path\\\": \\\"main.asm\\\"}\"}", &shape) && shape.args["path"] == "main.asm");   // arguments as a JSON string
    CHECK(findToolCall("{\"type\": \"function\", \"id\": \"call_1\", \"function\": {\"name\": \"search\", \"arguments\": \"{\\\"text\\\": \\\"WSYNC\\\"}\"}}", &shape) && shape.name == "search" && shape.args["text"] == "WSYNC");  // native
    CHECK(findToolCall("<tool_call>{\"name\": \"find_code\", \"parameters\": {\"q\": \"joystick\"}}</tool_call>", &shape) && shape.name == "find_code" && shape.args["q"] == "joystick");
    CHECK(findToolCall("```json\n{\"tool_name\": \"kb_lookup\", \"input\": {\"id\": \"INPT4\"}}\n```", &shape) && shape.args["id"] == "INPT4");
    CHECK(findToolCall("```tool\n{\"tool\": \"ReadFile\", \"file\": \"main.asm\"}\n```", &shape) && shape.name == "read_file");  // spelled differently
    CHECK(findToolCall("```\n```\ntool {\"args\":{\"text\":\"PLAYER_FIRE_PREV\"},\"tool\":\"search\"}\n```\n```", &shape) && shape.name == "search" && shape.args["text"] == "PLAYER_FIRE_PREV");  // between empty fences
    CHECK(findToolCall("Let me look.\ntool {\"tool\": \"search\", \"args\": {\"text\": \"WSYNC\"}}", &shape) && shape.name == "search");  // a bare "tool {...}" line
    CHECK(findToolCall("tool search {\"text\": \"WSYNC\"}", &shape) && shape.name == "search" && shape.args["text"] == "WSYNC");
    CHECK(!findToolCall("The tool is useful. tools {not a call}", &shape));
    CHECK(findToolCall("```tool\n{\"tool\": \"project_info\"}\n```", &shape) && shape.args.empty());
  }
  // run through the tools: repaired where that is certain, refused with the reason where it is not
  auto flat = call("read_file", {{"file", "main.asm"}, {"from", "2"}, {"to", 3.0}, {"unknown", "x"}, {"note", nullptr}});
  CHECK(!flat.error && flat.text.find("2: Start") != std::string::npos && flat.text.find("3:") != std::string::npos && flat.text.find("(Note:") == 0);
  CHECK(call("ReadFile", {{"path", "main.asm"}}).text.find("1: ") != std::string::npos);
  CHECK(call("functions.search", {{"text", "wsync"}}).text.find("  3:") != std::string::npos);
  auto stringArgs = runTool(env, {"read_file", "{\"path\": \"main.asm\"}"});
  CHECK(!stringArgs.error && stringArgs.text.find("1: ") != std::string::npos);
  auto missingPath = call("read_file", {{"from", 2}});
  CHECK(missingPath.error && missingPath.text.find("missing \"path\"") != std::string::npos && missingPath.text.find("Received:") != std::string::npos && missingPath.text.find("call the tool again") != std::string::npos);
  auto badNumber = call("read_file", {{"path", "main.asm"}, {"from", "first"}});
  CHECK(badNumber.error && badNumber.text.find("\"from\" must be a number") != std::string::npos);
  auto weirdTool = call("delete_everything", {});
  CHECK(weirdTool.error && weirdTool.text.find("no tool called") != std::string::npos);
  // edit_file: pairs under other names, one pair instead of a list, and the rule edits-or-content
  auto otherNames = call("edit_file", {{"path", "main.asm"}, {"changes", {{"old_text", "  jmp Start"}, {"new_text", "  jmp Start ; x"}}}});
  CHECK(otherNames.proposed && otherNames.edit.after.find("; x") != std::string::npos);
  auto neither = call("edit_file", {{"path", "main.asm"}});
  CHECK(neither.error && neither.text.find("\"edits\"") != std::string::npos && neither.text.find("\"content\"") != std::string::npos);
  // the schemas for native tool calls
  auto native = nativeTools({"read_file", "edit_file", "project_info"}, false);
  CHECK(native.size() == 3 && native[0]["type"] == "function" && native[0]["function"]["name"] == "read_file" && native[0]["function"]["parameters"]["required"][0] == "path");
  CHECK(native[1]["function"]["parameters"]["properties"]["edits"]["type"] == "array" && native[2]["function"]["parameters"]["properties"].empty());
  auto nativeAnthropic = nativeTools({"search"}, true);
  CHECK(nativeAnthropic[0]["name"] == "search" && nativeAnthropic[0]["input_schema"]["properties"]["text"]["type"] == "string" && !toolDescription("search").empty());
  for (auto& name : toolNames(2)) CHECK(resolveToolName(name) == name && !nativeTools({name}, false).empty() && !toolDescription(name).empty());  // every tool has its schema
  CHECK(resolveToolName("read-file") == "read_file" && resolveToolName("readfile") == "read_file" && resolveToolName("reed_file") == "read_file" && resolveToolName("rm") == "" && resolveToolName("") == "");

  // reading a call from an answer
  ToolCall parsed;
  std::string before;
  CHECK(findToolCall("Let me look.\n```tool\n{\"tool\": \"read_file\", \"args\": {\"path\": \"main.asm\"}}\n```", &parsed, &before));
  CHECK(parsed.name == "read_file" && parsed.args["path"] == "main.asm" && before == "Let me look.\n");
  CHECK(findToolCall("<tool_call>\n{\"name\": \"kb_lookup\", \"arguments\": {\"id\": \"WSYNC\"}}\n</tool_call>", &parsed));
  CHECK(parsed.name == "kb_lookup" && parsed.args["id"] == "WSYNC");
  CHECK(findToolCall("{\"tool\":\"build_errors\"}", &parsed) && parsed.name == "build_errors");
  CHECK(findToolCall("```json\n{\"tool\":\"search\",\"args\":{\"text\":\"}\"}}\n```", &parsed) && parsed.args["text"] == "}");
  CHECK(findToolCall("```tool music_describe {\"path\": \"a.vcsmusic\", \"pattern\": 0} ```", &parsed) && parsed.name == "music_describe" && parsed.args["pattern"] == 0);
  CHECK(findToolCall("```tool\nkb_lookup {\"id\": \"WSYNC\"}\n```", &parsed) && parsed.name == "kb_lookup");
  CHECK(!findToolCall("Use ```asm\nlda #0\n``` here", &parsed));
  CHECK(!findToolCall("```tool\n{\"tool\": \"rm_rf\"}\n```", &parsed));
  CHECK(!findToolCall("The JSON {\"tool\":\"read_file\"} is an example", &parsed));  // not at the start
  {  // a busy service says when to try again: the pause is that, not a guess
    HttpResult busy;
    busy.status = 429;
    busy.errorBody = R"([{"error": {"code": 429, "details": [{"@type": "x.RetryInfo", "retryDelay": "28s"}]}}])";
    CHECK(retryDelaySeconds(busy) == 28 && pauseBeforeRetry(busy, 0) == 29);
    busy.errorBody = R"({"error": {"message": "Rate limit reached. Please try again in 250ms."}})";
    CHECK(retryDelaySeconds(busy) == 0.25);
    busy.errorBody = "Please retry in 28.9s.";
    CHECK(retryDelaySeconds(busy) > 28.8 && retryDelaySeconds(busy) < 29);
    busy.errorBody = "", busy.retryAfterSeconds = 7;
    CHECK(retryDelaySeconds(busy) == 7);
    busy.retryAfterSeconds = 0;
    CHECK(pauseBeforeRetry(busy, 0) == 2 && pauseBeforeRetry(busy, 1) == 5 && pauseBeforeRetry(busy, 2) == 10 && pauseBeforeRetry(busy, 3) < 0);  // no hint: 2, 5, 10 s, then give up
    busy.retryAfterSeconds = 3600;
    CHECK(pauseBeforeRetry(busy, 0) < 0);  // a daily quota is not waited for
    HttpResult refused;
    refused.status = 400;
    CHECK(pauseBeforeRetry(refused, 0) < 0);
  }
  CHECK(toolNames(0).size() == 12 && toolNames(1).size() == 13 && toolNames(2).size() == 15);
  CHECK(toolPrompt(0).find("edit_file") == std::string::npos && toolPrompt(2).find("run_emulator") != std::string::npos);
  CHECK(estimateTokens(toolPrompt(2)) < 1100);  // the design budget for the tool descriptions

  // graphics and music files: described, and an edit that would not load is refused
  std::string data = VCSIDE_SOURCE_DATA;
  fs::copy_file(data + "/templates/shared/hero.vcsgfx", root / "hero.vcsgfx");
  fs::copy_file(data + "/templates/shared/theme-tia.vcsmusic", root / "theme.vcsmusic");
  auto gfxText = call("gfx_describe", {{"path", "hero.vcsgfx"}});
  CHECK(!gfxText.error && gfxText.text.find("sprite ") != std::string::npos && gfxText.text.find('#') != std::string::npos && gfxText.text.find("$") != std::string::npos);
  auto musicText = call("music_describe", {{"path", "theme.vcsmusic"}});
  CHECK(!musicText.error && musicText.text.find("instruments:") != std::string::npos && musicText.text.find("pattern 0") != std::string::npos);
  CHECK(call("gfx_describe", {{"path", "main.asm"}}).error && call("music_describe", {{"path", "../x.vcsmusic"}}).error);
  write(root / "broken.vcsgfx", "not json");
  CHECK(call("gfx_describe", {{"path", "broken.vcsgfx"}}).error);
  std::string heroText;
  { std::ifstream in(root / "hero.vcsgfx"); std::ostringstream buffer; buffer << in.rdbuf(); heroText = buffer.str(); }
  size_t cut = heroText.find_last_of('}');
  auto breaking = call("edit_file", {{"path", "hero.vcsgfx"}, {"edits", {{{"search", heroText.substr(cut > 30 ? cut - 30 : 0, 31)}, {"replace", "  ,,,"}}}}});
  CHECK(breaking.error && breaking.text.find("would not load") != std::string::npos);
  CHECK(call("edit_file", {{"path", "new.vcsmusic"}, {"content", "oops"}}).error);

  fs::remove_all(root);
  fs::remove_all(root.parent_path() / "vcside_agent_outside");
}

static void testBusyServiceMessages() {
  using namespace vcs::assistant;
  // Google answers an error as a list: [{"error": {"message": ...}}]
  std::string body = R"([{"error":{"code":400,"message":"API key not valid. Please pass a valid API key.","status":"INVALID_ARGUMENT"}}])";
  MockServer server("HTTP/1.1 400 Bad Request\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  Provider provider;
  provider.baseUrl = server.url(), provider.model = "m";
  ChatRequest request;
  request.start(provider, {{"user", "hi"}});
  std::string error;
  for (int i = 0; i < 300 && error.empty(); i++) {
    usleep(10000);
    for (auto& event : request.poll())
      if (event.kind == Event::Error) error = event.text;
  }
  CHECK(error == "HTTP 400: API key not valid. Please pass a valid API key.");
}

static void testNativeToolCalls() {
  using namespace vcs::assistant;
  // the definitions go in the body, per dialect
  Provider provider;
  provider.model = "m";
  auto tools = nativeTools({"read_file"}, false);
  auto body = nlohmann::json::parse(chatRequestBody(provider, {{"user", "hi"}}, &tools));
  CHECK(body["tools"].size() == 1 && body["tools"][0]["function"]["name"] == "read_file" && !nlohmann::json::parse(chatRequestBody(provider, {{"user", "hi"}})).contains("tools"));
  provider.dialect = "anthropic";
  auto anthropicTools = nativeTools({"read_file"}, true);
  CHECK(nlohmann::json::parse(chatRequestBody(provider, {{"user", "hi"}}, &anthropicTools))["tools"][0]["input_schema"]["required"][0] == "path");
  // Claude models get cache breakpoints (system text, last message); other models and services get none
  Provider claude;
  claude.dialect = "openai", claude.baseUrl = "https://openrouter.ai/api/v1", claude.model = "anthropic/claude-sonnet-5.5";
  auto cached = nlohmann::json::parse(chatRequestBody(claude, {{"system", "rules"}, {"user", "one"}, {"assistant", "two"}, {"user", "three"}}));
  CHECK(cached["messages"][0]["content"][0]["cache_control"]["type"] == "ephemeral" && cached["messages"][3]["content"][0]["text"] == "three" && cached["messages"][3]["content"][0].contains("cache_control") && cached["messages"][1]["content"].is_string());
  claude.model = "openai/gpt-5";
  CHECK(chatRequestBody(claude, {{"system", "rules"}, {"user", "three"}}).find("cache_control") == std::string::npos);
  claude.dialect = "anthropic", claude.baseUrl = "https://api.anthropic.com/v1", claude.model = "claude-sonnet-5-5";
  auto direct = nlohmann::json::parse(chatRequestBody(claude, {{"system", "rules"}, {"user", "one"}, {"assistant", "two"}, {"user", "three"}}));
  CHECK(direct["system"][0]["cache_control"]["type"] == "ephemeral" && direct["messages"].back()["content"].back().contains("cache_control"));

  // OpenAI shape: a name, then the arguments in pieces, then the end
  {
    ToolCallAssembler assembler;
    std::vector<Event> events;
    parseOpenAiStreamData(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"read_file","arguments":""}}]}}]})", events, &assembler);
    parseOpenAiStreamData(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"path\": \"ma"}}]}}]})", events, &assembler);
    parseOpenAiStreamData(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"in.asm\"}"}}]}}]})", events, &assembler);
    CHECK(events.empty());  // nothing until the call is whole
    parseOpenAiStreamData(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})", events, &assembler);
    parseOpenAiStreamData("[DONE]", events, &assembler);
    CHECK(events.size() == 2 && events[0].kind == Event::ToolCall && events[0].text == "read_file" && nlohmann::json::parse(events[0].arguments)["path"] == "main.asm" && events[1].kind == Event::Done);
  }
  // Google's layer: the whole call in one chunk, with no index, and two calls
  {
    ToolCallAssembler assembler;
    std::vector<Event> events;
    parseOpenAiStreamData(R"({"choices":[{"delta":{"role":"assistant","tool_calls":[{"id":"a","function":{"name":"search","arguments":"{\"text\":\"WSYNC\"}"},"type":"function"},{"id":"b","function":{"name":"find_code","arguments":"{\"query\":\"x\"}"},"type":"function"}]},"finish_reason":"stop"}]})", events, &assembler);
    CHECK(events.size() == 2 && events[0].text == "search" && events[1].text == "find_code" && nlohmann::json::parse(events[1].arguments)["query"] == "x");
  }
  // without the assembler (a plain chat) tool-call chunks are ignored and text still comes through
  {
    std::vector<Event> events;
    parseOpenAiStreamData(R"({"choices":[{"delta":{"content":"hi","tool_calls":[{"index":0,"function":{"name":"x"}}]}}]})", events);
    CHECK(events.size() == 1 && events[0].kind == Event::Text);
  }
  // Anthropic shape
  {
    ToolCallAssembler assembler;
    std::vector<Event> events;
    parseAnthropicStreamData(R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"t1","name":"read_file","input":{}}})", events, &assembler);
    parseAnthropicStreamData(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})", events, &assembler);
    parseAnthropicStreamData(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":" \"main.asm\"}"}})", events, &assembler);
    parseAnthropicStreamData(R"({"type":"content_block_stop","index":1})", events, &assembler);
    CHECK(events.size() == 1 && events[0].kind == Event::ToolCall && events[0].text == "read_file" && nlohmann::json::parse(events[0].arguments)["path"] == "main.asm");
    std::vector<Event> noArguments;
    ToolCallAssembler second;
    parseAnthropicStreamData(R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","name":"project_info"}})", noArguments, &second);
    parseAnthropicStreamData(R"({"type":"content_block_stop","index":0})", noArguments, &second);
    CHECK(noArguments.size() == 1 && noArguments[0].arguments == "{}");
  }
  // a call the model made natively reads like one it wrote as text
  ToolCall call;
  CHECK(findToolCall("```tool\n{\"tool\": \"read_file\", \"args\": {\"path\": \"main.asm\"}}\n```", &call) && call.name == "read_file");
}

static void testLengthStops() {
  using namespace vcs::assistant;
  std::vector<Event> events;
  parseOpenAiStreamData(R"({"choices":[{"delta":{"content":"x"},"finish_reason":null}]})", events);
  parseOpenAiStreamData(R"({"choices":[{"delta":{},"finish_reason":"length"}]})", events);
  parseOpenAiStreamData(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})", events);
  CHECK(events.size() == 2 && events[0].kind == Event::Text && events[1].kind == Event::Length);
  std::vector<Event> anthropic;
  parseAnthropicStreamData(R"({"type":"message_delta","delta":{"stop_reason":"max_tokens"},"usage":{"output_tokens":9}})", anthropic);
  CHECK(anthropic.size() == 2 && anthropic[0].kind == Event::Usage && anthropic[1].kind == Event::Length);
  std::vector<Event> normal;
  parseAnthropicStreamData(R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":9}})", normal);
  CHECK(normal.size() == 1);
}

static void testRemote() {
  using namespace vcs::assistant;
  // the Anthropic dialect: the system prompt is its own field
  Provider provider;
  provider.dialect = "anthropic", provider.model = "m", provider.baseUrl = "https://api.anthropic.com/v1";
  auto body = nlohmann::json::parse(anthropicRequestBody(provider, {{"system", "be brief"}, {"user", "hi"}, {"assistant", "hello"}, {"user", "more"}}));
  CHECK(body["system"] == "be brief" && body["messages"].size() == 3 && body["messages"][0]["role"] == "user" && body["stream"] == true && body["max_tokens"] > 0);
  CHECK(chatUrl(provider) == "https://api.anthropic.com/v1/messages");
  std::vector<Event> events;
  parseAnthropicStreamData(R"({"type":"message_start","message":{"usage":{"input_tokens":25,"output_tokens":1}}})", events);
  parseAnthropicStreamData(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hel"}})", events);
  parseAnthropicStreamData(R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"hm"}})", events);
  parseAnthropicStreamData(R"({"type":"ping"})", events);
  parseAnthropicStreamData(R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":15}})", events);
  parseAnthropicStreamData(R"({"type":"message_stop"})", events);
  parseAnthropicStreamData(R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})", events);
  CHECK(events.size() == 5);
  CHECK(events[0].kind == Event::Usage && events[0].promptTokens == 25);
  CHECK(events[1].kind == Event::Text && events[1].text == "Hel");
  CHECK(events[2].kind == Event::Usage && events[2].completionTokens == 15);
  CHECK(events[3].kind == Event::Done && events[4].kind == Event::Error && events[4].text == "Overloaded");

  // keys: headers per dialect, from the secret store, never in the body
  CHECK(saveSecret("TestProvider", "sk-test-1234567890abcd") && !secretsPersistent());
  provider.name = "TestProvider";
  bool hasKey = false, hasVersion = false;
  for (auto& header : chatHeaders(provider)) {
    if (header.first == "x-api-key" && header.second == "sk-test-1234567890abcd") hasKey = true;
    if (header.first == "anthropic-version") hasVersion = true;
  }
  CHECK(hasKey && hasVersion);
  Provider openai;
  openai.name = "TestProvider";
  bool bearer = false;
  for (auto& header : chatHeaders(openai)) bearer = bearer || (header.first == "Authorization" && header.second == "Bearer sk-test-1234567890abcd");
  CHECK(bearer);
  CHECK(chatRequestBody(provider, {{"user", "hi"}}).find("sk-test") == std::string::npos);
  CHECK(maskKey("sk-test-1234567890abcd") == "...abcd" && maskKey("short") == "...");
  CHECK(redactKey("error for sk-test-1234567890abcd twice sk-test-1234567890abcd", "sk-test-1234567890abcd") == "error for [key] twice [key]");
  CHECK(removeSecret("TestProvider"));
  std::string none;
  CHECK(!loadSecret("TestProvider", &none));
  {  // the file store: chosen by the user, 0600, keys moved across both ways
    namespace fs = std::filesystem;
    fs::path folder = fs::temp_directory_path() / ("vcside-keys-" + std::to_string(::getpid()));
    fs::remove_all(folder);
    setSecretFile((folder / "keys.json").string());
    CHECK(saveSecret("Moved", "sk-moved-1234567890") && !usingSecretFile());
    moveSecrets({"Moved", "Missing"}, true);
    useSecretFile(true);
    std::string moved;
    CHECK(usingSecretFile() && secretsPersistent() && std::string(secretStoreName()) == "a file only you can read");
    CHECK(loadSecret("Moved", &moved) && moved == "sk-moved-1234567890");
    CHECK(saveSecret("Second", "sk-second-123456789") && loadSecret("Second", &moved) && moved == "sk-second-123456789");
#ifndef _WIN32
    CHECK((fs::status(folder / "keys.json").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
    CHECK(removeSecret("Second") && !loadSecret("Second", &moved));
    useSecretFile(false);
    CHECK(!loadSecret("Moved", &moved));  // the move took it out of the other store
    moveSecrets({"Moved"}, false);
    CHECK(loadSecret("Moved", &moved) && moved == "sk-moved-1234567890" && removeSecret("Moved"));
    useSecretFile(true);
    CHECK(!loadSecret("Moved", &moved));
    useSecretFile(false), setSecretFile("");
    fs::remove_all(folder);
  }
  // an https address with no TLS backend is refused clearly, not tried in the clear
  HttpRequest request;
  request.url = "https://example.invalid/v1";
  std::atomic<bool> never{false};
  if (!httpsAvailable()) {
    HttpResult refused = httpRequest(request, [](const char*, size_t) { return true; }, never);
    CHECK(refused.status == 0 && refused.error.find("https") != std::string::npos);
  }
  // the budget survives a save
  Settings settings;
  settings.budgetTokens = 50000;
  CHECK(Settings::fromJson(settings.toJson()).budgetTokens == 50000);
}

// The tab bar's protocol on a headless ImGui: a click on "+" adds a tab and the new one stays selected.
namespace {
struct TabHarness {
  ImGuiContext* context;
  std::vector<vcs::assistant::TabView> tabs;
  int active = 0, want = -1;
  unsigned nextUid = 1;
  ImVec2 plus;
  std::vector<std::string> log;
  TabHarness() {
    context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    unsigned char* pixels;
    int width, height;
    io.Fonts->AddFontDefault();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    tabs.push_back({"first", nextUid++});
  }
  ~TabHarness() { ImGui::DestroyContext(context); }
  void frame() {
    using namespace vcs::assistant;
    ImGui::GetIO().DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(700, 300));
    ImGui::Begin("tabs", nullptr, ImGuiWindowFlags_NoSavedSettings);
    TabAction action = drawChatTabs(tabs, active, &want, false, "New chat", 150);
    plus = action.plusCentre;
    // the panel's model: "+" adds a tab and asks for it to be selected; a click on a tab switches
    if (action.kind == TabAction::New) tabs.push_back({"chat " + std::to_string(nextUid), nextUid}), nextUid++, active = (int)tabs.size() - 1, want = active, log.push_back("new");
    if (action.kind == TabAction::Switch) active = action.index, log.push_back("switch " + std::to_string(action.index));
    if (action.kind == TabAction::Close) {
      tabs.erase(tabs.begin() + action.index);
      if (active >= (int)tabs.size()) active = (int)tabs.size() - 1;
      want = active, log.push_back("close");
    }
    ImGui::End();
    ImGui::Render();
  }
  void click(ImVec2 at) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    frame(), frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame(), frame(), frame();
  }
};
}  // namespace

static void testChatTabs() {
  TabHarness harness;
  for (int i = 0; i < 4; i++) harness.frame();
  harness.click(harness.plus);
  CHECK(harness.tabs.size() == 2 && harness.active == 1);
  for (int i = 0; i < 5; i++) harness.frame();
  CHECK(harness.tabs.size() == 2 && harness.active == 1);  // it stays on the new tab: no switch back
  CHECK(harness.log.size() == 1 && harness.log[0] == "new");
  harness.click(harness.plus);
  CHECK(harness.tabs.size() == 3 && harness.active == 2 && harness.log.size() == 2);
  // the text cut keeps whole characters
  CHECK(vcs::assistant::ellipsizeText("short", 500) == "short");
  std::string cut = vcs::assistant::ellipsizeText(std::string("caf\xC3\xA9 caf\xC3\xA9 caf\xC3\xA9 caf\xC3\xA9"), 30);
  CHECK(cut.size() >= 3 && cut.substr(cut.size() - 3) == "..." && vcs::assistant::sanitizeUtf8(cut) == cut);
}

// Text cut in the middle of a character, or not UTF-8 at all (a Latin-1 source file), must never reach
// a JSON dump: that threw out of the chat store and ended the program.
static void testInvalidUtf8() {
  using namespace vcs::assistant;
  CHECK(sanitizeUtf8("plain") == "plain" && sanitizeUtf8("caf\xC3\xA9") == "caf\xC3\xA9");
  CHECK(sanitizeUtf8("caf\xC3") == "caf\xEF\xBF\xBD");                       // cut in the middle
  CHECK(sanitizeUtf8("a\xE9" "b") == "a\xEF\xBF\xBD" "b");                   // Latin-1
  CHECK(sanitizeUtf8("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");             // overlong
  CHECK(sanitizeUtf8("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");  // a surrogate
  CHECK(sanitizeUtf8("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80");             // an emoji is fine
  std::string spanish = "ñandú, ¿qué? ñandú, ¿qué? ñandú";
  for (size_t limit = 1; limit < spanish.size(); limit++) {
    CHECK(sanitizeUtf8(utf8Cut(spanish, limit)) == utf8Cut(spanish, limit) && utf8Cut(spanish, limit).size() <= limit);
    CHECK(sanitizeUtf8(utf8Tail(spanish, limit)) == utf8Tail(spanish, limit));
  }
  // the chat store saves such a chat instead of throwing, and what it wrote loads back
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() / "vcside_utf8_test";
  fs::remove_all(root);
  fs::create_directories(root);
  Chat chat;
  chat.id = "20260101-000000", chat.title = "title \xC3", chat.model = "m";
  chat.messages.push_back({"user", "caf\xC3", {}, ""});
  chat.messages.push_back({"assistant", "a\xE9" "b \xFF", {}, ""});
  ChatStore store(root.string());
  bool saved = false, threw = false;
  try { saved = store.save(chat); } catch (...) { threw = true; }
  CHECK(saved && !threw);
  Chat loaded;
  CHECK(store.load(chat.id, false, &loaded) && loaded.messages.size() == 2 && loaded.messages[0].content == "caf\xEF\xBF\xBD");
  // and so does a request
  Provider provider;
  provider.model = "m";
  bool bodyThrew = false;
  try { chatRequestBody(provider, {{"user", "caf\xC3"}}); provider.dialect = "anthropic"; chatRequestBody(provider, {{"user", "a\xFF"}}); } catch (...) { bodyThrew = true; }
  CHECK(!bodyThrew);
  // chats kept in the application's own data folder (not the project's), and the open tabs remembered
  {
    fs::path project = fs::temp_directory_path() / "vcside_chat_base_project", base = fs::temp_directory_path() / "vcside_chat_base";
    fs::remove_all(project), fs::remove_all(base);
    fs::create_directories(project);
    ChatStore store(project.string(), base.string());
    CHECK(store.loadOpenTabs().ids.empty() && store.list().empty());
    Chat fresh;
    fresh.id = "20260102-000000", fresh.title = "new one";
    fresh.messages.push_back({"user", "hi", {}});
    CHECK(store.save(fresh) && fs::exists(fs::path(store.chatsDir()) / "20260102-000000.json") && !fs::exists(project / ".vcsai"));
    CHECK(store.chatsDir().rfind(base.string(), 0) == 0 && ChatStore::folderKey(project.string()).rfind("vcside_chat_base_project-", 0) == 0);
    CHECK(ChatStore::folderKey("/a/proj") != ChatStore::folderKey("/b/proj"));
    ChatStore::OpenTabs open;
    open.ids = {"20260102-000000", "other"}, open.active = "20260102-000000";
    store.saveOpenTabs(open);
    auto back = ChatStore(project.string(), base.string()).loadOpenTabs();
    CHECK(back.ids == open.ids && back.active == open.active && store.list().size() == 1);  // open.tabs is not a chat
    fs::remove_all(project), fs::remove_all(base);
  }
  fs::remove_all(root);
}

static void testFileOperations() {
  namespace fs = std::filesystem;
  using namespace vcs::fileops;
  fs::path root = fs::temp_directory_path() / "vcside_fileops_test";
  fs::remove_all(root);
  fs::create_directories(root / "src");
  std::string created, error;
  CHECK(nameProblem("main.asm").empty() && !nameProblem("").empty() && !nameProblem("a/b").empty() && !nameProblem("..").empty() && !nameProblem(" x").empty() && !nameProblem("a:b").empty() && !nameProblem("x.").empty());
  CHECK(createFile(root.string(), "main.asm", &created, &error) && fs::exists(root / "main.asm") && created == (root / "main.asm").string());
  CHECK(!createFile(root.string(), "main.asm", &created, &error) && error.find("already exists") != std::string::npos);
  CHECK(createFolder(root.string(), "gfx", &created, &error) && fs::is_directory(root / "gfx"));
  CHECK(!createFile((root / "nope").string(), "x", &created, &error));
  std::ofstream(root / "main.asm") << "hello";
  std::string renamed;
  CHECK(rename((root / "main.asm").string(), "game.asm", &renamed, &error) && fs::exists(root / "game.asm") && !fs::exists(root / "main.asm") && renamed == (root / "game.asm").string());
  CHECK(!rename((root / "game.asm").string(), "gfx", &renamed, &error) && fs::exists(root / "game.asm"));  // the name is taken
  CHECK(rename((root / "game.asm").string(), "game.asm", &renamed, &error));                            // the same name: nothing to do
  std::string copy;
  CHECK(duplicate((root / "game.asm").string(), &copy, &error) && copy == (root / "game copy.asm").string() && fs::exists(copy));
  CHECK(duplicate((root / "game.asm").string(), &copy, &error) && copy == (root / "game copy 2.asm").string());
  std::ofstream(root / "gfx" / "a.png") << "x";
  CHECK(duplicate((root / "gfx").string(), &copy, &error) && copy == (root / "gfx copy").string() && fs::exists(root / "gfx copy" / "a.png"));
  CHECK(remove((root / "gfx").string(), &error) && !fs::exists(root / "gfx") && fs::exists(root / "gfx copy"));
  CHECK(!remove((root / "gfx").string(), &error) && !error.empty());
  fs::remove_all(root);
}

static void testProjectIndex() {
  namespace fs = std::filesystem;
  using vcs::assistant::ProjectIndex;
  // words are split like identifiers, and kept whole too
  auto has = [](const std::vector<std::string>& list, const char* word) { return std::find(list.begin(), list.end(), word) != list.end(); };
  auto split = ProjectIndex::words("playerX kernel_loop WSYNC HMOVE2");
  CHECK(has(split, "playerx") && has(split, "player") && has(split, "kernel_loop") && has(split, "kernel") && has(split, "loop") && has(split, "wsync") && has(split, "hmove"));

  fs::path root = fs::temp_directory_path() / "vcside_index_test";
  fs::remove_all(root);
  fs::create_directories(root / "src");
  fs::create_directories(root / "build");
  auto write = [](const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; };
  std::string joystick = "ReadJoystick\n  lda SWCHA\n  and #$80\n  bne NotRight\n  inc playerX\nNotRight\n  lda SWCHA\n  and #$40\n  bne NotLeft\n  dec playerX\nNotLeft\n  rts\n";
  std::string sound = "PlayTone\n  lda #4\n  sta AUDC0\n  lda #31\n  sta AUDF0\n  lda #8\n  sta AUDV0\n  rts\nStopTone\n  lda #0\n  sta AUDV0\n  rts\n";
  std::string kernel = "DrawKernel\n  sta WSYNC\n  lda #$0E\n  sta COLUBK\n  ldx #192\nKernelLoop\n  sta WSYNC\n  dex\n  bne KernelLoop\n  rts\n";
  write(root / "src" / "input.asm", joystick);
  write(root / "src" / "audio.asm", sound);
  write(root / "kernel.asm", kernel);
  write(root / ".env", "SECRET=1\n");
  write(root / "build" / "main.lst", "ReadJoystick junk\n");
  write(root / "game.bin", std::string("\0\1ReadJoystick", 13));
  ProjectIndex index;
  CHECK(index.update(root.string()) == 3 && index.files() == 3 && index.chunks() >= 3);
  CHECK(index.update(root.string()) == 0);  // nothing changed: nothing read
  auto hits = index.search("how does the joystick move the player left and right", 3);
  CHECK(!hits.empty() && hits[0].file == "src/input.asm" && hits[0].firstLine == 1 && hits[0].text.find("SWCHA") != std::string::npos);
  hits = index.search("where is the tone played, AUDV0 volume", 3);
  CHECK(!hits.empty() && hits[0].file == "src/audio.asm");
  hits = index.search("KernelLoop", 3);
  CHECK(!hits.empty() && hits[0].file == "kernel.asm");
  hits = index.search("what is the weather like in Madrid", 3);  // nothing in the project: no noise
  CHECK(hits.empty());
  CHECK(index.search("SWCHA", 3, "src/input.asm").empty());     // the attached file is left out
  for (auto& hit : index.search("SWCHA playerX AUDV0 WSYNC", 8)) CHECK(hit.file != ".env" && hit.file.find("build") != 0 && hit.file != "game.bin");
  std::string map = index.map("joystick", 400);
  CHECK(map.find("src/input.asm: ReadJoystick NotRight NotLeft") != std::string::npos && map.find("kernel.asm") != std::string::npos);
  CHECK(map.find("src/input.asm") < map.find("kernel.asm"));  // what the question mentions comes first
  // a changed file is read again and a deleted one forgotten
  write(root / "src" / "input.asm", joystick + "FireButton\n  lda INPT4\n  bmi NoFire\nNoFire\n  rts\n");
  fs::last_write_time(root / "src" / "input.asm", fs::file_time_type::clock::now() + std::chrono::seconds(5));
  CHECK(index.update(root.string()) == 1);
  hits = index.search("fire button INPT4", 3);
  CHECK(!hits.empty() && hits[0].file == "src/input.asm");
  fs::remove(root / "src" / "audio.asm");
  CHECK(index.update(root.string()) == 0 && index.files() == 2 && index.search("AUDV0 tone", 3).empty());
  // a file that is not UTF-8 is indexed without trouble
  write(root / "latin1.asm", "Caf\xE9\n  lda #1\n  rts\n");
  CHECK(index.update(root.string()) == 1);
  for (auto& hit : index.search("caf lda", 3)) CHECK(vcs::assistant::sanitizeUtf8(hit.text) == hit.text);
  fs::remove_all(root);
}

static void testEmbeddings() {
  namespace fs = std::filesystem;
  using namespace vcs::assistant;
  // vectors
  auto unit = normalised({3, 4});
  CHECK(std::abs(unit[0] - 0.6f) < 1e-6 && std::abs(unit[1] - 0.8f) < 1e-6 && std::abs(dotProduct(unit, unit) - 1.0f) < 1e-6);
  CHECK(normalised({0, 0}) == std::vector<float>({0, 0}) && dotProduct({1, 2}, {1}) == 0);
  CHECK(embeddingKey("m", "text").size() == 64 && embeddingKey("m", "text") != embeddingKey("n", "text") && embeddingKey("m", "text") == embeddingKey("m", "text"));

  // the request: the body, and the answer read back in order and normalised
  {
    std::string body = R"({"data":[{"index":1,"embedding":[0,2]},{"index":0,"embedding":[3,4]}]})";
    MockServer server("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    std::vector<std::vector<float>> vectors;
    std::string error;
    std::atomic<bool> never{false};
    bool ok = requestEmbeddings(server.url(), {"first", "second \xC3"}, &vectors, &error, never, 5);
    CHECK(ok && vectors.size() == 2 && std::abs(vectors[0][0] - 0.6f) < 1e-6 && std::abs(vectors[1][1] - 1.0f) < 1e-6);
    server.thread.join();
    server.thread = std::thread([] {});
    auto sent = nlohmann::json::parse(server.request.substr(server.request.find("\r\n\r\n") + 4));
    CHECK(sent["input"].size() == 2 && sent["input"][0] == "first" && server.request.find("POST /v1/embeddings") == 0);
  }
  {
    MockServer server("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"data\":[]}");
    std::vector<std::vector<float>> vectors;
    std::string error;
    std::atomic<bool> never{false};
    CHECK(!requestEmbeddings(server.url(), {"a"}, &vectors, &error, never, 5) && !error.empty());  // wrong number of vectors
  }

  // the cache: written, read back, damaged files ignored
  fs::path root = fs::temp_directory_path() / "vcside_cache_test";
  fs::remove_all(root);
  std::string path = (root / "index" / "m.vec").string();
  EmbeddingCache cache;
  cache.put(embeddingKey("m", "a"), {0.5f, -0.5f});
  cache.put(embeddingKey("m", "b"), {1.0f, 0.0f});
  CHECK(cache.save(path));
  EmbeddingCache loaded;
  CHECK(loaded.load(path) && loaded.size() == 2 && loaded.find(embeddingKey("m", "a")) && (*loaded.find(embeddingKey("m", "a")))[1] == -0.5f && !loaded.find("nothing"));
  loaded.keepOnly({{embeddingKey("m", "b"), true}});
  CHECK(loaded.size() == 1 && !loaded.find(embeddingKey("m", "a")));
  { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << "VCSV1" << "garbage"; }
  EmbeddingCache damaged;
  CHECK(!damaged.load(path) && damaged.size() == 0);
  fs::remove_all(root);

  // meaning in the index, with vectors made by hand: topics are directions, words need not match
  fs::path project = fs::temp_directory_path() / "vcside_meaning_test";
  fs::remove_all(project);
  fs::create_directories(project);
  auto write = [](const fs::path& file, const std::string& text) { std::ofstream(file, std::ios::binary) << text; };
  write(project / "pad.asm", "ReadPad\n  lda SWCHA\n  and #$F0\n  cmp #$E0\n  beq GoUp\n  rts\n  ; reads the controller port and decides the direction to move\n");
  write(project / "noise.asm", "MakeBeep\n  lda #4\n  sta AUDC0\n  lda #12\n  sta AUDF0\n  lda #8\n  sta AUDV0\n  rts\n  ; writes the sound registers to play a short tone\n");
  write(project / "tiny.txt", "x\n");
  ProjectIndex index;
  index.useEmbeddings("toy");
  index.update(project.string());
  CHECK(index.chunks() >= 3 && index.embedded() == 0);
  auto missing = index.missing();
  CHECK(missing.size() == index.chunks());
  for (auto& item : missing) {
    std::vector<float> vector = item.second.find("SWCHA") != std::string::npos ? std::vector<float>{1, 0, 0} : item.second.find("AUDC0") != std::string::npos ? std::vector<float>{0, 1, 0} : std::vector<float>{0, 0, 1};
    index.setVector(item.first, vector);
  }
  CHECK(index.embedded() == index.chunks() && index.missing().empty());
  std::vector<float> askInput = {0.9f, 0.1f, 0}, askSound = {0.1f, 0.95f, 0}, askNothing = {0, 0, 1};
  askInput = normalised(askInput), askSound = normalised(askSound);
  // no word in common with the code ("gamepad" is nowhere): keywords find nothing, meaning does
  CHECK(index.search("how does the gamepad steer the character", 3).empty());
  auto byMeaning = index.search("how does the gamepad steer the character", 3, "", &askInput, 0.5);
  CHECK(!byMeaning.empty() && byMeaning[0].file == "pad.asm");
  byMeaning = index.search("which routine makes noise", 3, "", &askSound, 0.5);
  CHECK(!byMeaning.empty() && byMeaning[0].file == "noise.asm");
  CHECK(index.search("something unrelated", 3, "", &askNothing, 0.5).empty());        // the one-line file is never offered
  CHECK(index.search("how does the gamepad steer", 3, "pad.asm", &askInput, 0.5).empty());  // the attached file is left out
  // both agree: the chunk named by the question comes first, and a threshold that nothing reaches leaves keywords alone
  auto both = index.search("MakeBeep tone", 3, "", &askSound, 0.5);
  CHECK(!both.empty() && both[0].file == "noise.asm");
  CHECK(index.search("MakeBeep", 3, "", &askInput, 0.999).size() == 1);
  // a change of model forgets the vectors; changed text needs new ones
  index.useEmbeddings("other");
  CHECK(index.embedded() == 0 && index.missing().size() == index.chunks());
  fs::remove_all(project);
}

static void testContextChips() {
  namespace fs = std::filesystem;
  using namespace vcs::assistant;
  auto labels = [](const ContextSet& set) { std::string text; for (auto& chip : set.chips("errors")) text += chip.label + "|"; return text; };
  ContextSet set;
  ContextSources sources;
  sources.fileRelative = "main.asm";
  set.sync(sources);
  CHECK(labels(set) == "main.asm|");
  // a selection stands for the file and names its lines
  sources.hasSelection = true, sources.selectionFirst = 12, sources.selectionLast = 20;
  set.sync(sources);
  CHECK(labels(set) == "main.asm:12-20|");
  sources.selectionLast = 12;
  set.sync(sources);
  CHECK(labels(set) == "main.asm:12|");
  // errors come with their own chip
  sources.hasErrors = true, sources.errorsKey = "e1";
  set.sync(sources);
  CHECK(labels(set) == "main.asm:12|errors|");
  // removing one keeps it removed while its source stays the same...
  set.remove(1);
  set.sync(sources);
  CHECK(labels(set) == "main.asm:12|");
  // ...and brings it back when the errors change
  sources.errorsKey = "e2";
  set.sync(sources);
  CHECK(labels(set) == "main.asm:12|errors|");
  // removing the selection does not fall back to the file; another selection or file brings it back
  set.remove(0);
  set.sync(sources);
  CHECK(labels(set) == "errors|");
  sources.selectionFirst = 3, sources.selectionLast = 4;
  set.sync(sources);
  CHECK(labels(set) == "main.asm:3-4|errors|");
  sources.hasSelection = false;
  set.sync(sources);
  CHECK(labels(set) == "main.asm|errors|");
  set.remove(0);
  sources.fileRelative = "other.asm";
  set.sync(sources);
  CHECK(labels(set) == "other.asm|errors|");
  // added files are listed once, after the automatic ones, and can be removed
  set.addFiles({"src/kernel.asm", "constants.h", "src/kernel.asm"});
  CHECK(labels(set) == "other.asm|errors|src/kernel.asm|constants.h|" && set.addedCount() == 2);
  set.remove(2);
  CHECK(labels(set) == "other.asm|errors|constants.h|");
  sources.fileRelative = "";
  sources.hasSelection = false, sources.hasErrors = false;
  set.sync(sources);
  CHECK(labels(set) == "constants.h|");  // nothing open: only what was added

  // the project's files for @
  fs::path root = fs::temp_directory_path() / "vcside_context_test";
  fs::remove_all(root);
  fs::create_directories(root / "src" / "deep" / "er");
  fs::create_directories(root / "build");
  fs::create_directories(root / ".git");
  auto write = [](const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; };
  write(root / "main.asm", "  nop\n");
  write(root / "constants.h", "A = 1\n");
  write(root / "src" / "kernel.asm", "Kernel\n  rts\n");
  write(root / "src" / "keys.asm", "Keys\n  rts\n");
  write(root / "src" / "deep" / "er" / "kernel_old.asm", "Old\n  rts\n");
  write(root / ".env", "SECRET=1\n");
  write(root / "game.bin", "xx");
  write(root / "build" / "out.lst", "x\n");
  write(root / ".git" / "config", "x\n");
  std::string dir = root.string();
  auto all = listProjectEntries(dir, "", 50);
  CHECK(std::find(all.begin(), all.end(), "src/") != all.end() && std::find(all.begin(), all.end(), "main.asm") != all.end());
  for (auto& entry : all) CHECK(entry != ".env" && entry != "game.bin" && entry.rfind("build", 0) != 0 && entry.rfind(".git", 0) != 0);
  auto kernel = listProjectEntries(dir, "kern", 5);
  CHECK(kernel.size() == 2 && kernel[0] == "src/kernel.asm" && kernel[1] == "src/deep/er/kernel_old.asm");  // the name starts with it; shallow first
  auto folder = listProjectEntries(dir, "src", 5);
  CHECK(!folder.empty() && folder[0] == "src/");
  CHECK(listProjectEntries(dir, "mnasm", 5).size() == 1);  // its letters in order: main.asm
  CHECK(listProjectEntries(dir, "zzzz", 5).empty() && listProjectEntries(dir, "env", 5).empty());
  CHECK(listProjectEntries(dir, "", 3).size() == 3);
  size_t total = 0;
  auto inSrc = filesInFolder(dir, "src", 2, &total);
  CHECK(total == 3 && inSrc.size() == 2 && inSrc[0] == "src/deep/er/kernel_old.asm");
  CHECK(filesInFolder(dir, "../elsewhere", 5, &total).empty() && total == 0);

  // what is sent: the chips' text within a budget
  ContextSet chosen;
  ContextSources open;
  open.fileRelative = "main.asm", open.hasErrors = true, open.errorsKey = "k";
  chosen.sync(open);
  chosen.addFiles({"src/kernel.asm", ".env", "missing.asm"});
  ContextText editor{"  nop\n  nop\n", "", "main.asm:3: error: x\n"};
  auto gathered = gatherContext(chosen.chips("Build errors"), editor, dir, 10000);
  CHECK(gathered.fileName == "main.asm" && gathered.fileText == editor.fileText && gathered.errors == editor.errors);
  CHECK(gathered.extraFiles.size() == 1 && gathered.extraFiles[0].first == "src/kernel.asm" && gathered.extraFiles[0].second == "Kernel\n  rts\n");
  CHECK(gathered.notes.size() == 2);  // .env (secrets) and the missing file said why they are not there
  CHECK(gathered.summary == "main.asm, Build errors, src/kernel.asm");
  auto tight = gatherContext(chosen.chips("Build errors"), ContextText{std::string(500, 'x'), "", "err\n"}, dir, 100);
  CHECK(tight.fileText.size() == 100 && tight.errors.empty() && tight.extraFiles.empty() && tight.notes.size() >= 2);  // the file used the budget
  open.hasSelection = true, open.selectionFirst = 1, open.selectionLast = 2;
  chosen.sync(open);
  auto selected = gatherContext(chosen.chips("Build errors"), ContextText{"whole file", "  nop\n  nop\n", ""}, dir, 10000);
  CHECK(selected.selectionLabel == "main.asm:1-2" && selected.selection == "  nop\n  nop\n" && selected.fileText.empty());
  // a long file is cut in whole lines around the cursor, not at its start; the note says which lines of how many
  std::string longFile;
  for (int line = 1; line <= 100; line++) longFile += "line " + std::to_string(100 + line) + "\n";  // 9 characters each
  ContextSet single;
  ContextSources only;
  only.fileRelative = "main.asm";
  single.sync(only);
  auto around = gatherContext(single.chips("e"), ContextText{longFile, "", "", 60}, dir, 90);  // room for 10 lines
  CHECK(around.fileText.size() <= 90 && around.fileText.find("line 160\n") != std::string::npos && around.fileText.find("line 101") == std::string::npos);
  CHECK(around.notes.size() == 1 && around.notes[0].label == "main.asm" && around.notes[0].totalLines == 100 && around.notes[0].firstLine > 1 && around.notes[0].lastLine < 100 &&
        around.notes[0].firstLine <= 60 && around.notes[0].lastLine >= 60 && around.notes[0].reason.empty());
  auto atStart = gatherContext(single.chips("e"), ContextText{longFile, "", "", 0}, dir, 90);
  CHECK(atStart.notes.size() == 1 && atStart.notes[0].firstLine == 1 && atStart.fileText.rfind("line 101\n", 0) == 0);
  auto whole = gatherContext(single.chips("e"), ContextText{longFile, "", "", 60}, dir, 5000);
  CHECK(whole.notes.empty() && whole.fileText == longFile);
  fs::remove_all(root);
}

static void testEditBlocks() {
  namespace fs = std::filesystem;
  using namespace vcs::assistant;
  EditBlock block;
  std::string problem;
  // an edit block names its file and holds sections
  CHECK(parseEditBlock("edit src/main.asm", "<<<<<<< SEARCH\n  lda #1\n=======\n  lda #2\n>>>>>>> REPLACE", &block, &problem) == EditParse::Edit);
  CHECK(block.path == "src/main.asm" && block.sections.size() == 1 && block.sections[0].search == "  lda #1" && block.sections[0].replace == "  lda #2");
  CHECK(parseEditBlock("EDIT  a b.asm ", "<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE\nsome text\n<<<<<<< SEARCH\nz\nw\n=======\n>>>>>>> REPLACE\n", &block, &problem) == EditParse::Edit);
  CHECK(block.path == "a b.asm" && block.sections.size() == 2 && block.sections[1].search == "z\nw" && block.sections[1].replace.empty());
  CHECK(parseEditBlock("asm src/x.asm", "<<<<< SEARCH\r\nx\r\n=====\r\ny\r\n>>>>> REPLACE\r\n", &block, &problem) == EditParse::Edit && block.path == "src/x.asm" && block.sections[0].search == "x");
  // an example is not an edit; an edit that is cut or has no file is broken, and says why
  CHECK(parseEditBlock("asm", "  lda #1\n  sta COLUBK\n", &block, &problem) == EditParse::NotAnEdit);
  CHECK(parseEditBlock("", "plain text", &block, &problem) == EditParse::NotAnEdit);
  CHECK(parseEditBlock("edit", "<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE", &block, &problem) == EditParse::Broken && problem.find("file") != std::string::npos);
  CHECK(parseEditBlock("edit a.asm", "<<<<<<< SEARCH\nx\n=======\ny\n", &block, &problem) == EditParse::Broken && problem.find("cut off") != std::string::npos);
  CHECK(parseEditBlock("edit a.asm", "<<<<<<< SEARCH\nx\n>>>>>>> REPLACE", &block, &problem) == EditParse::Broken);
  CHECK(parseEditBlock("edit a.asm", "just code", &block, &problem) == EditParse::Broken);
  // the arguments for edit_file
  parseEditBlock("edit a.asm", "<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE", &block, &problem);
  CHECK(editArguments(block)["edits"][0]["replace"] == "y" && editArguments(block)["path"] == "a.asm");
  parseEditBlock("edit new.asm", "<<<<<<< SEARCH\n=======\n  nop\n>>>>>>> REPLACE", &block, &problem);
  CHECK(editArguments(block)["content"] == "  nop\n" && !editArguments(block).contains("edits"));

  // finding the place in the real file
  fs::path root = fs::temp_directory_path() / "vcside_edit_block_test";
  fs::remove_all(root);
  fs::create_directories(root);
  auto write = [](const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; };
  write(root / "main.asm", "Start\n\tlda #1\n\tsta COLUBK\n\trts\nLoop\n\tlda #2\n\tsta COLUBK\n\trts\n");
  ToolEnv env;
  env.projectDir = root.string();
  auto locate = [&](const std::string& info, const std::string& body) {
    EditBlock found;
    std::string why;
    if (parseEditBlock(info, body, &found, &why) != EditParse::Edit) return ToolResult();
    return runTool(env, {"edit_file", editArguments(found)});
  };
  // exactly as in the file (the tab is a tab): one place, and which lines
  auto exact = locate("edit main.asm", "<<<<<<< SEARCH\nLoop\n\tlda #2\n=======\nLoop\n\tlda #3\n>>>>>>> REPLACE");
  CHECK(exact.proposed && exact.edit.after.find("lda #3") != std::string::npos && exact.edit.firstLine == 6 && exact.edit.lastLine == 6);
  // the model wrote spaces for the tab: still one place, found by the lines
  auto loose = locate("edit main.asm", "<<<<<<< SEARCH\nLoop\n    lda #2\n    sta COLUBK\n=======\nLoop\n    lda #9\n    sta COLUBK\n>>>>>>> REPLACE");
  CHECK(loose.proposed && loose.edit.after.find("lda #9") != std::string::npos && loose.edit.after.find("lda #2") == std::string::npos && loose.edit.firstLine == 6);
  CHECK(loose.edit.after.find("lda #1") != std::string::npos);  // the rest is untouched
  // more than one place: not applied, and it says where
  auto twice = locate("edit main.asm", "<<<<<<< SEARCH\n    sta COLUBK\n    rts\n=======\n    rts\n>>>>>>> REPLACE");
  CHECK(!twice.proposed && twice.error && twice.text.find("more than once") != std::string::npos && twice.text.find("3") != std::string::npos && twice.text.find("7") != std::string::npos);
  // not there: not applied
  auto missing = locate("edit main.asm", "<<<<<<< SEARCH\n  lda #77\n=======\n  lda #78\n>>>>>>> REPLACE");
  CHECK(!missing.proposed && missing.error && missing.text.find("not found") != std::string::npos);
  // a file that does not exist needs a new file, and one outside the project is refused
  CHECK(!locate("edit nothing.asm", "<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE").proposed);
  CHECK(locate("edit fresh.asm", "<<<<<<< SEARCH\n=======\n  nop\n>>>>>>> REPLACE").edit.created);
  CHECK(!locate("edit ../outside.asm", "<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE").proposed);
  CHECK(!locate("edit .env", "<<<<<<< SEARCH\n=======\nTOKEN=1\n>>>>>>> REPLACE").proposed);
  // the tiers and the hints of a search that is not there
  write(root / "typo.asm", "Hello\n  lda #\xE2\x80\x9C" "1\xE2\x80\x9D ; \xE2\x80\x93 note\n  rts\n");
  auto typography = locate("edit typo.asm", "<<<<<<< SEARCH\n  lda #\"1\" ; - note\n=======\n  lda #2\n>>>>>>> REPLACE");
  CHECK(typography.proposed && typography.edit.after.find("lda #2") != std::string::npos && typography.edit.after.find("Hello") == 0);  // curly quotes and a long dash match plain ones
  write(root / "crlf.asm", "One\r\n\tlda #1\r\n\trts\r\n");
  auto windows = locate("edit crlf.asm", "<<<<<<< SEARCH\n  lda #1\n=======\n  lda #1\n  nop\n>>>>>>> REPLACE");
  CHECK(windows.proposed && windows.edit.after == "One\r\n  lda #1\r\n  nop\r\n\trts\r\n");  // the lines are the file's, the new ones have its line ends
  auto near = locate("edit main.asm", "<<<<<<< SEARCH\nLoop\n\tlda #9\n\tsta COLUBK\n=======\nx\n>>>>>>> REPLACE");
  CHECK(near.error && near.text.find("closest lines in the file are 5-8") != std::string::npos && near.text.find("6: \tlda #2") != std::string::npos);  // two of its three lines vote for line 5
  write(root / "done.asm", "Start\n  lda #7\n  rts\n");
  auto already = locate("edit done.asm", "<<<<<<< SEARCH\n  lda #1\n=======\n  lda #7\n>>>>>>> REPLACE");
  CHECK(already.error && already.text.find("already in the file (line 2)") != std::string::npos);
  // line numbers copied from read_file into the search and the new text are taken off
  {
    write(root / "numbered.asm", "\t\t\t; Read from joystick port\n\t\t\tlda #0\n\t\t\tsta SWACNT\n");
    auto numbered = locate("edit numbered.asm", "<<<<<<< SEARCH\n898: \t\t\t; Read from joystick port\n899: \t\t\tlda #0\n=======\n898: \t\t\t; Read from joystick port\n899: \t\t\tlda #0\n900: \t\t\tlda INPT4\n>>>>>>> REPLACE");
    CHECK(numbered.proposed && numbered.edit.after == "\t\t\t; Read from joystick port\n\t\t\tlda #0\n\t\t\tlda INPT4\n\t\t\tsta SWACNT\n");
    fs::remove(root / "numbered.asm");
  }
  // the model names lines but leaves some out: the change is made on the real lines, and the lines it left out stay
  {
    write(root / "rebase.asm", "\t\t\tjsr SFX_TRIGGER\t\t\t\n.AddMovement\n\t\t\t; Check right\n\t\t\tlda PLAYER_DIRECTION\n\t\t\tcmp #RIGHT\n\t\t\tbne .RightDone\n\t\t\t; Moving to the right\n");
    auto skipped = locate("edit rebase.asm", "<<<<<<< SEARCH\n\t\t\tjsr SFX_TRIGGER\n\t\t\tlda PLAYER_DIRECTION\n\t\t\tcmp #RIGHT\n\t\t\tbne .RightDone\n=======\n\t\t\tjsr SFX_TRIGGER\n\t\t\tlda INPT4\n\t\t\tbpl .Fire\n.Fire\n\t\t\tlda PLAYER_DIRECTION\n\t\t\tcmp #RIGHT\n\t\t\tbne .RightDone\n>>>>>>> REPLACE");
    CHECK(skipped.proposed && skipped.edit.after == "\t\t\tjsr SFX_TRIGGER\t\t\t\n.AddMovement\n\t\t\t; Check right\n\t\t\tlda INPT4\n\t\t\tbpl .Fire\n.Fire\n\t\t\tlda PLAYER_DIRECTION\n\t\t\tcmp #RIGHT\n\t\t\tbne .RightDone\n\t\t\t; Moving to the right\n");
    CHECK(!locate("edit rebase.asm", "<<<<<<< SEARCH\n\t\t\tjsr SFX_TRIGGER\n\t\t\tlda NOTHING_LIKE_IT\n=======\nx\n>>>>>>> REPLACE").proposed);  // a named line that is not there: no guess
    write(root / "twice.asm", "a\nx\nb\nc\na\ny\nb\n");
    CHECK(!locate("edit twice.asm", "<<<<<<< SEARCH\na\nb\n=======\na\nnew\nb\n>>>>>>> REPLACE").proposed);  // two places that fit: no guess either
    fs::remove(root / "rebase.asm"), fs::remove(root / "twice.asm");
  }
  // a path a little off ("src/main.asm" for the "main.asm" it was shown) finds the only file with that name
  fs::create_directories(root / "docs");
  write(root / "docs" / "notes.txt", "notes\n");
  auto off = locate("edit src/main.asm", "<<<<<<< SEARCH\nLoop\n\tlda #2\n=======\nLoop\n\tlda #5\n>>>>>>> REPLACE");
  CHECK(off.proposed && off.edit.relative == "main.asm" && off.edit.after.find("lda #5") != std::string::npos);
  write(root / "docs" / "main.asm", "other\n");  // two files with the name: not guessed
  CHECK(!locate("edit src/main.asm", "<<<<<<< SEARCH\nLoop\n=======\nx\n>>>>>>> REPLACE").proposed);
  CHECK(locate("edit docs/notes.txt", "<<<<<<< SEARCH\nnotes\n=======\nmore\n>>>>>>> REPLACE").proposed);
  fs::remove(root / "docs" / "main.asm");
  // a file that does not exist, but only one file holds the text to change: that one
  write(root / "only.asm", "UniqueMarker\n  lda INPT4\n");
  auto byText = locate("edit src/ghost.asm", "<<<<<<< SEARCH\nUniqueMarker\n=======\nUniqueMarker\n  nop\n>>>>>>> REPLACE");
  CHECK(byText.proposed && byText.edit.relative == "only.asm");
  CHECK(!locate("edit src/ghost.asm", "<<<<<<< SEARCH\nNowhereToBeFound\n=======\nx\n>>>>>>> REPLACE").proposed);
  fs::remove(root / "only.asm");
  // nothing was written by any of them
  std::ifstream after(root / "main.asm");
  std::string disk((std::istreambuf_iterator<char>(after)), {});
  CHECK(disk.find("lda #3") == std::string::npos && !fs::exists(root / "fresh.asm"));
  fs::remove_all(root);
}

static void testUnknownNames(const KnowledgeBase& kb) {
  using namespace vcs::assistant;
  auto inProject = [](const std::string& word) { return word == "jumpheight" || word == "player_y"; };
  auto names = [&](const std::string& answer) { return unknownNames(answer, kb, inProject); };
  auto has = [](const std::vector<std::string>& list, const char* name) { return std::find(list.begin(), list.end(), name) != list.end(); };
  // real registers, instructions, project names and what the answer defines are fine; a made-up register is not
  auto invented = names("Read it with:\n```asm\n  lda PADDLE\n  and #1\n  sta JUMP_COUNTER\n  lda INPT4\n  bmi NoJump\n  lda JUMPHEIGHT\n```\n");
  CHECK(has(invented, "PADDLE") && has(invented, "JUMP_COUNTER") && !has(invented, "INPT4") && !has(invented, "JUMPHEIGHT") && !has(invented, "NOJUMP") && invented.size() == 2);
  // defined by the answer itself
  auto defined = names("```asm\nJUMP_COUNTER = $80\nFLAGS EQU $81\nSPEED SET 3\nHEIGHT DS 1\nStart\n  lda #0\n  sta JUMP_COUNTER\n  sta FLAGS\n  lda SPEED\n  lda HEIGHT\n```\n");
  CHECK(defined.empty());
  // only code counts: shouting in the prose is not a register; inline code is code
  CHECK(names("This is WRONG and the TIA hates it, use RAM. SWCHA and RESP0 are real.").empty());
  auto inline_ = names("Read `PADDLE` and `INPT4` and the mnemonic `LDA`, then `lda #$FF` and `%10101010`.");
  CHECK(inline_.size() == 1 && inline_[0] == "PADDLE");
  // lower case and short names are not judged; each unknown name is listed once, in order
  CHECK(names("```\n  lda paddle\n  lda XY\n```").empty());
  auto twice = names("```\n  lda FOO\n  sta BAR\n  lda FOO\n```");
  CHECK(twice.size() == 2 && twice[0] == "FOO" && twice[1] == "BAR");
  CHECK(unknownNames("```\nA1B C2D E3F G4H I5J K6L M7N\n```", kb, inProject, 3).size() == 3);
  // a word of batari Basic is real there and invented in assembly
  CHECK(unknownNames("`PADDLE`", kb, inProject, 6, false).size() == 1 && unknownNames("`PADDLE`", kb, inProject, 6, true).empty());
  // directives, formats and TV standards are known
  CHECK(names("```\n  processor 6502\n  SEG CODE\n  ORG $F000\n```\nUse F8 or 4K, NTSC or PAL").size() == 1);  // CODE is not a word of the knowledge base
  CHECK(names("An unfinished block ```asm\n  lda PADDLE").size() == 1);
  // the markers of an edit block are not names, but a made-up name inside one is
  auto inEdit = names("```edit main.asm\n<<<<<<< SEARCH\n  lda #0\n=======\n  lda #PLAYER_JUMP\n>>>>>>> REPLACE\n```");
  CHECK(inEdit.size() == 1 && inEdit[0] == "PLAYER_JUMP");
}

static void testFirstPass() {
  using namespace vcs::assistant;
  // the reply, read
  auto reply = parseFirstPass("{\"language\": \"es\", \"title\": \"Salto con el botón\", \"english\": \"Implement the jump action when the button is pressed\"}");
  CHECK(reply.ok && reply.language == "es" && reply.title == "Salto con el botón" && reply.english.find("jump action") != std::string::npos);
  auto fenced = parseFirstPass("<think>hmm</think>\n```json\n{\"language\":\"EN\",\"title\":\"\\\"Read the fire button.\\\"\",\"english\":\"x\"}\n```");
  CHECK(fenced.ok && fenced.language == "en" && fenced.title == "Read the fire button");
  // a reply for the English text alone has no title; one that was asked for it needs the text
  CHECK(parseFirstPass("{\"language\":\"es\",\"english\":\"Read the button\"}", false, true).ok && !parseFirstPass("{\"language\":\"es\",\"title\":\"T\"}", true, true).ok);
  CHECK(languageName("es") == "Spanish" && languageName("xx") == "xx" && translationPrompt("Spanish", "English").find("[[1]]") != std::string::npos);
  CHECK(!parseFirstPass("Sure! Here is a title: Jump").ok && !parseFirstPass("{\"language\":\"es\"}").ok && !parseFirstPass("{broken").ok && !parseFirstPass("").ok);
  // titles: one line, no quotes or final period, whole characters, a limit
  CHECK(cleanTitle("  \"Leer\n el   mando.\"  ") == "Leer el mando");
  CHECK(cleanTitle("# Title: ") == "Title");
  std::string accents = cleanTitle(std::string(40, 'a') + "ñandú ñandú ñandú");
  CHECK(accents.size() <= 44 && sanitizeUtf8(accents) == accents && accents.size() > 40 && accents.substr(accents.size() - 3) == "...");
  // the request: asked of a server, answered in either dialect
  {
    std::string body = R"({"choices":[{"message":{"content":"{\"language\":\"es\",\"title\":\"Leer el mando\",\"english\":\"Read the joystick\"}"}}]})";
    MockServer server("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    Provider provider;
    provider.baseUrl = server.url(), provider.model = "m";
    FirstPassRequest request;
    request.start(provider, "¿cómo leo el mando?", true, true);
    FirstPass answer;
    for (int i = 0; i < 300 && !request.poll(&answer); i++) usleep(10000);
    CHECK(answer.ok && answer.title == "Leer el mando" && answer.english == "Read the joystick");
    request.cancel();
    server.thread.join();
    server.thread = std::thread([] {});
    auto sent = nlohmann::json::parse(server.request.substr(server.request.find("\r\n\r\n") + 4));
    CHECK(sent["stream"] == false && sent["messages"].size() == 2 && sent["messages"][1]["content"] == "¿cómo leo el mando?" && !sent.contains("stream_options") && sent["messages"][0]["content"].get<std::string>().find("english") != std::string::npos);
  }
  {
    Provider provider;
    provider.baseUrl = "http://127.0.0.1:1/v1";  // nothing listens there
    FirstPassRequest request;
    request.start(provider, "x", true, false);
    FirstPass answer;
    for (int i = 0; i < 300 && !request.poll(&answer); i++) usleep(10000);
    CHECK(!answer.ok && !answer.error.empty());
  }
}

static void testMasking(const KnowledgeBase& kb) {
  using namespace vcs::assistant;
  auto isMnemonic = [&](const std::string& word) { return kb.instruction(word) != nullptr; };
  auto mask = [&](const std::string& text) { return maskCode(text, isMnemonic); };
  // inline code, fenced code and names are taken out, and put back as they were
  auto question = mask("Implementa el salto: mira `lda INPT4` y SWCHA en src/main.asm; $F000 y #$80 también.\n```asm\n  lda INPT4\n  bmi Skip\n```\n¿Qué hago con playerX?");
  CHECK(question.text.find("`") == std::string::npos && question.text.find("INPT4") == std::string::npos && question.text.find("SWCHA") == std::string::npos && question.text.find("main.asm") == std::string::npos);
  CHECK(question.text.find("Implementa el salto: mira [[1]] y [[") == 0 && question.spans[0] == "`lda INPT4`" && question.spans.size() >= 7);  // the code first, then the names
  CHECK(question.text.find("¿Qué hago con") != std::string::npos && question.hasProse());
  bool ok = false;
  CHECK(unmaskCode(question.text, question.spans, &ok) == "Implementa el salto: mira `lda INPT4` y SWCHA en src/main.asm; $F000 y #$80 también.\n```asm\n  lda INPT4\n  bmi Skip\n```\n¿Qué hago con playerX?" && ok);
  // the translation keeps the markers in other places: still fine
  CHECK(unmaskCode("Check [[2]] and [[1]].", {"AUDV0", "`sta`"}, &ok) == "Check `sta` and AUDV0." && ok);
  // lines of assembly pasted without a fence are code too
  auto pasted = mask("Esto no funciona:\n  lda INPT4\n  bmi Skip\n  jsr Jump\nSkip\n  rts\nPor qué?");
  CHECK(pasted.spans.size() == 1 && pasted.spans[0].find("jsr Jump") != std::string::npos && pasted.text.find("Esto no funciona:\n[[1]]\nPor") == 0);
  // prose is left alone: ordinary words, one capital, numbers, a sentence that mentions a mnemonic in words
  auto plain = mask("El salto dura 12 frames. Quiero que Mario suba rápido y que ande a la derecha.");
  CHECK(plain.spans.empty() && plain.text == "El salto dura 12 frames. Quiero que Mario suba rápido y que ande a la derecha.");
  CHECK(mask("Usa and, or y load en español").spans.empty());
  // a lost, doubled or invented marker is refused and the text comes back as it was
  CHECK(unmaskCode("Falta [[1]]", {"A1", "B2"}, &ok) == "Falta [[1]]" && !ok);
  CHECK(unmaskCode("[[1]] y [[1]]", {"A1"}, &ok) == "[[1]] y [[1]]" && !ok);
  CHECK(unmaskCode("[[1]] y [[7]]", {"A1"}, &ok) == "[[1]] y [[7]]" && !ok);
  CHECK(unmaskCode("[[1] roto", {"A1"}, &ok) == "[[1] roto" && !ok);
  CHECK(unmaskCode("nada que devolver", {}, &ok) == "nada que devolver" && ok);
  // text that already has [[ is not masked (its markers could be confused with ours)
  CHECK(mask("array[[0]] en C").spans.empty());
  // only code: no prose to translate
  CHECK(!mask("`lda #1`").hasProse() && !mask("```\n  lda #1\n  sta COLUBK\n```").hasProse() && mask("Hola, ¿qué tal?").hasProse());
  // is it English already?
  CHECK(looksEnglish("How do I read the fire button?") && looksEnglish("What does WSYNC do") && !looksEnglish("¿Cómo leo el botón?") && !looksEnglish("como leo el boton de disparo") &&
        !looksEnglish("Implementa el salto") && !looksEnglish("Wie lese ich den Knopf") && !looksEnglish("123"));
}

static void testKnownProviders() {
  using namespace vcs::assistant;
  // the shipped list: only https services and real protocols; Gemini through its OpenAI-compatible address
  auto known = loadKnownProviders(std::string(VCSIDE_SOURCE_DATA) + "/assistant/providers.json");
  CHECK(known.size() >= 4);
  const KnownProvider* gemini = nullptr;
  for (auto& item : known) {
    CHECK(item.baseUrl.rfind("https://", 0) == 0 && (item.dialect == "openai" || item.dialect == "anthropic") && !item.name.empty());
    if (item.id == "gemini") gemini = &item;
  }
  CHECK(gemini && gemini->baseUrl == "https://generativelanguage.googleapis.com/v1beta/openai" && gemini->dialect == "openai" && gemini->keyPage.rfind("https://", 0) == 0);
  Provider made = providerFrom(*gemini);
  CHECK(made.preset == "gemini" && made.name == "Google Gemini" && !made.isLocal() && made.model.empty());
  CHECK(Settings::fromJson(([&] { Settings s; s.providers.push_back(made); return s.toJson(); })()).providers.back().preset == "gemini");
  // "Don't ask again" is kept per server; a new one asks
  CHECK(!made.trusted);
  made.trusted = true;
  CHECK(Settings::fromJson(([&] { Settings s; s.providers.push_back(made); return s.toJson(); })()).providers.back().trusted);
  CHECK(!made.agentAnyway && !Settings::fromJson(([&] { Settings s; s.providers.push_back(made); return s.toJson(); })()).providers.back().agentAnyway);
  made.agentAnyway = true;
  CHECK(Settings::fromJson(([&] { Settings s; s.providers.push_back(made); return s.toJson(); })()).providers.back().agentAnyway);
  // a list with a plain-http service or an invented protocol is not offered
  namespace fs = std::filesystem;
  fs::path file = fs::temp_directory_path() / "vcside_providers_test.json";
  { std::ofstream(file) << R"({"providers":[{"id":"a","name":"A","dialect":"openai","base_url":"http://insecure.example/v1"},{"id":"b","name":"B","dialect":"soap","base_url":"https://b.example/v1"},{"id":"c","name":"C","dialect":"openai","base_url":"https://c.example/v1","key_page":"http://c.example"}]})"; }
  auto filtered = loadKnownProviders(file.string());
  CHECK(filtered.size() == 1 && filtered[0].id == "c" && filtered[0].keyPage.empty());
  fs::remove(file);

  // the names a service lists: prefix off, only for chatting, each once, sorted
  Provider google;
  google.baseUrl = "https://generativelanguage.googleapis.com/v1beta/openai";
  auto names = normaliseModelIds({"models/gemini-2.5-pro", "models/gemini-2.5-flash", "models/text-embedding-004", "models/imagen-3.0", "models/gemini-2.5-flash", "models/aqa", "models/veo-2.0"}, google);
  CHECK(names == std::vector<std::string>({"gemini-2.5-flash", "gemini-2.5-pro"}));
  Provider openai;
  openai.baseUrl = "https://api.openai.com/v1";
  auto openaiNames = normaliseModelIds({"gpt-4o", "whisper-1", "text-embedding-3-small", "dall-e-3", "tts-1", "o3-mini", "omni-moderation-latest", "models/not-stripped"}, openai);
  CHECK(openaiNames == std::vector<std::string>({"gpt-4o", "models/not-stripped", "o3-mini"}));
  CHECK(describeModelListError("HTTP 403: API key not valid").find("refused the key") != std::string::npos && describeModelListError("HTTP 404").find("type the name") != std::string::npos &&
        describeModelListError("connection refused") == "connection refused");

  // asking a service: the key goes as a Bearer header, the answer is cleaned, nothing is asked without a key
  setenv("VCSIDE_TEST_KEY", "sk-test-abcdef", 1);
  {
    std::string body = R"({"data":[{"id":"models/gemini-2.5-flash"},{"id":"models/text-embedding-004"},{"id":"models/gemini-2.5-pro"}]})";
    MockServer server("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    Provider remote;
    remote.name = "Mock Gemini";
    remote.baseUrl = server.url();
    remote.keyEnv = "VCSIDE_TEST_KEY";
    ModelLists lists;
    CHECK(!lists.needsKey(remote) && lists.of(remote).ids.empty() && !lists.of(remote).fetched);
    lists.request(remote);
    CHECK(lists.of(remote).fetching);
    for (int i = 0; i < 300 && lists.of(remote).fetching; i++) usleep(10000), lists.poll();
    CHECK(lists.of(remote).fetched && lists.of(remote).error.empty() && lists.of(remote).ids.size() == 2);  // a plain http address is not Google's: the prefix stays
    server.thread.join();
    server.thread = std::thread([] {});
    CHECK(server.request.find("GET /v1/models") == 0 && server.request.find("Authorization: Bearer sk-test-abcdef") != std::string::npos);
    lists.request(remote);  // the same address and key: not asked again
    CHECK(!lists.of(remote).fetching);
  }
  {
    Provider remote;
    remote.name = "Keyless";
    remote.baseUrl = "https://api.example.invalid/v1";
    ModelLists lists;
    CHECK(lists.needsKey(remote));
    lists.request(remote);
    CHECK(!lists.of(remote).fetching && !lists.of(remote).fetched);  // nothing is asked without a key
  }
  {
    MockServer server("HTTP/1.1 403 Forbidden\r\nContent-Length: 49\r\n\r\n{\"error\":{\"message\":\"API key not valid. Please pass\"}}");
    Provider remote;
    remote.name = "Refused";
    remote.baseUrl = server.url();
    remote.keyEnv = "VCSIDE_TEST_KEY";
    ModelLists lists;
    lists.request(remote);
    for (int i = 0; i < 300 && lists.of(remote).fetching; i++) usleep(10000), lists.poll();
    CHECK(lists.of(remote).fetched && lists.of(remote).ids.empty() && lists.of(remote).error.find("refused the key") != std::string::npos);
  }
  {  // Google's own shape: {"models": [{"name": "models/..."}]}, and something that is not a list at all
    std::string body = R"({"models":[{"name":"models/gemini-2.5-flash"},{"name":"models/text-embedding-004"}]})";
    MockServer server("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    Provider remote;
    remote.baseUrl = server.url();
    std::string error;
    auto ids = listModels(remote, &error);
    CHECK(ids.size() == 2 && ids[0] == "models/gemini-2.5-flash" && error.empty());
  }
  {
    std::string body = "<html>not a list</html>";
    MockServer server("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    Provider remote;
    remote.baseUrl = server.url();
    std::string error;
    CHECK(listModels(remote, &error).empty() && error.find("not a list") != std::string::npos);
  }
  unsetenv("VCSIDE_TEST_KEY");
}

static void testAssistant(const KnowledgeBase& kb) {
  using namespace vcs::assistant;
  // SSE, fed one byte at a time, with CRLF, comments and a data line split over two
  {
    std::string stream = ": ping\r\ndata: {\"a\":1}\r\n\r\ndata: one\ndata: two\n\nevent: x\ndata: [DONE]\n\n";
    SseParser parser;
    std::vector<std::string> got;
    for (char c : stream)
      for (auto& event : parser.feed(&c, 1)) got.push_back(event);
    CHECK(got.size() == 3 && got[0] == "{\"a\":1}" && got[1] == "one\ntwo" && got[2] == "[DONE]");
    SseParser whole;
    CHECK(whole.feed(stream.data(), stream.size()) == got);
  }
  // chunked transfer, cut at every byte
  {
    std::string body = chunk("hello ") + chunk("world") + "0\r\n\r\n";
    ChunkedDecoder decoder;
    std::string out;
    for (char c : body) decoder.feed(&c, 1, out);
    CHECK(out == "hello world" && decoder.done() && !decoder.failed());
    ChunkedDecoder bad;
    std::string ignored;
    bad.feed("zz\r\n", 4, ignored);
    CHECK(bad.failed());
  }
  // URLs
  {
    Url url = parseUrl("http://127.0.0.1:11434/v1");
    CHECK(url.valid && url.host == "127.0.0.1" && url.port == "11434" && url.path == "/v1");
    CHECK(parseUrl("http://[::1]:8080/x").host == "::1");
    CHECK(parseUrl("http://localhost").port == "80");
    CHECK(!parseUrl("http://user:pass@host/").valid);  // no credentials in URLs
    CHECK(!parseUrl("ftp://host/").valid && !parseUrl("localhost:80").valid);
    CHECK(isLoopbackHost("127.0.0.1") && isLoopbackHost("localhost") && isLoopbackHost("::1") && !isLoopbackHost("example.com") && !isLoopbackHost("127.evil.com") && !isLoopbackHost("127.0.0"));
    Provider provider;
    provider.baseUrl = "http://localhost:1234/v1";
    CHECK(provider.isLocal());
    provider.baseUrl = "http://192.168.1.5:1234/v1";
    CHECK(!provider.isLocal());
    CHECK(joinUrl("http://h/v1/", "/chat/completions") == "http://h/v1/chat/completions");
  }
  // the OpenAI dialect
  {
    Provider provider;
    provider.model = "m";
    auto body = nlohmann::json::parse(openAiRequestBody(provider, {{"system", "s"}, {"user", "u"}}));
    CHECK(body["stream"] == true && body["model"] == "m" && body["messages"].size() == 2 && body["messages"][1]["content"] == "u");
    std::vector<Event> events;
    parseOpenAiStreamData("{\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}", events);
    parseOpenAiStreamData("{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}", events);
    parseOpenAiStreamData("{\"choices\":[],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3}}", events);
    parseOpenAiStreamData("{\"error\":{\"message\":\"boom\"}}", events);
    parseOpenAiStreamData("not json", events);
    parseOpenAiStreamData("[DONE]", events);
    CHECK(events.size() == 4 && events[0].kind == Event::Text && events[0].text == "Hi" && events[1].kind == Event::Usage && events[1].promptTokens == 7 &&
          events[2].kind == Event::Error && events[2].text == "boom" && events[3].kind == Event::Done);
  }
  // prompt: attachments, trimming, code blocks
  {
    Attachments attachments;
    attachments.fileName = "main.asm", attachments.fileText = "LDA #0";
    attachments.errors = "main.asm:3: error";
    std::string turn = userTurn("why?", attachments);
    CHECK(turn.find("main.asm (data)") != std::string::npos && turn.find("Build errors (data)") != std::string::npos && turn.substr(turn.size() - 4) == "why?");
    attachments.projectNotes = "The kernel does sta HMOVE on every line.";
    std::string noted = userTurn("why?", attachments);
    CHECK(noted.find("VCSIDE.md") != std::string::npos && noted.find("sta HMOVE on every line") != std::string::npos && noted.find("sta HMOVE") < noted.find("main.asm (data)") && turn.find("VCSIDE.md") == std::string::npos);
    std::vector<ChatMessage> history;
    for (int i = 0; i < 20; i++) history.push_back({i % 2 ? "assistant" : "user", std::string(2000, 'x'), {}});
    int dropped = 0;
    auto messages = buildMessages("sys", history, "now", 3000, 500, &dropped);
    CHECK(messages.front().role == "system" && messages.back().content == "now" && dropped > 10 && messages[1].role == "user");
    auto blocks = splitBlocks("Look:\n```asm\nLDA #1\n```\nand\n```\nSTA X\n```\n");
    CHECK(blocks.size() == 4 && !blocks[0].code && blocks[1].code && blocks[1].language == "asm" && blocks[1].text == "LDA #1" && blocks[3].text == "STA X");
    auto open = splitBlocks("a\n```asm\nLDA");  // still streaming
    CHECK(open.size() == 2 && open[1].code && open[1].text == "LDA");
  }
  // grounding: the right entries from a question, code and errors; no noise from plain English
  {
    GroundingContext context;
    context.format = "F8";
    auto ids = [&](const std::string& question, const std::string& code, GroundingContext usedContext) {
      std::vector<std::string> found;
      for (auto& entry : groundText(kb, question, code, usedContext)) found.push_back(entry.id);
      return found;
    };
    auto contains = [](const std::vector<std::string>& list, const std::string& id) { return std::find(list.begin(), list.end(), id) != list.end(); };
    auto found = ids("How does RESP0 work after WSYNC?", "", context);
    CHECK(contains(found, "RESP0") && contains(found, "WSYNC"));
    found = ids("what does the and operation do, and how do I set a bit or clear it?", "", context);
    CHECK(found.empty());  // plain English is not a mnemonic
    found = ids("How many cycles?", "  lda $80\n  sta wsync\n", context);
    CHECK(contains(found, "WSYNC") && contains(found, "LDA"));
    found = ids("Explain LDA", "", context);
    CHECK(found.size() == 1 && found[0] == "LDA");
    found = ids("How do the banks work on this cartridge, F8 or F6?", "", context);
    CHECK(contains(found, "F8"));
    found = ids("my 4K video is slow", "", context);
    CHECK(found.empty());
    auto entries = groundText(kb, "RESP0 RESP1 RESM0 RESM1 RESBL WSYNC", "", context, 3);
    CHECK(entries.size() >= 2 && entries.size() <= 3);  // capped by count and by characters
    // the how-to entries: complete, sourced, and found by what people ask (in English and in Spanish)
    {
      CHECK(kb.techniques().size() >= 10);
      for (auto& technique : kb.techniques()) {
        CHECK(!technique.id.empty() && !technique.title.empty() && !technique.summary.empty() && technique.steps.size() >= 3);
        CHECK(!technique.sources.empty() && !technique.keywords.empty());
        for (auto& source : technique.sources) CHECK(kb.source(source) != nullptr);
        for (auto& name : technique.related) CHECK(kb.reg(name) != nullptr || kb.format(name) != nullptr || kb.instruction(name) != nullptr);  // every register or instruction it names exists
      }
      auto ids = [&](const std::string& question) {
        std::set<std::string> found;
        for (auto& entry : groundText(kb, question, "", context)) found.insert(entry.id);
        return found;
      };
      CHECK(ids("How do I position a sprite horizontally?").count("position-object-x"));
      CHECK(ids("Para posicionar un sprite en un valor específico de X en la pantalla").count("position-object-x"));
      CHECK(ids("¿Cómo hago que la consola reproduzca un tono?").count("make-sound"));
      CHECK(ids("My game is bigger than 4 KB, what can I do?").count("add-banks"));
      CHECK(ids("Why does my picture roll?").count("fix-rolling-picture") || ids("Why does my picture roll?").count("frame-structure"));
      auto how = groundText(kb, "How do I position a sprite horizontally?", "", context);
      CHECK(!how.empty() && how[0].text.find("How-to:") == 0 && how[0].text.find("NOT VERIFIED") != std::string::npos);  // the unrun routine says so
    }
    // a question that names nothing is searched by topic, with the manuals' words for what people say
    {
      auto ids = [&](const std::string& question) {
        std::set<std::string> found;
        for (auto& entry : groundText(kb, question, "", context)) found.insert(entry.id);
        return found;
      };
      CHECK(ids("How do I make the console play a tone?").count("AUDC0"));
      CHECK(ids("How do I check whether the fire button is pressed?").count("INPT4") || ids("How do I check whether the fire button is pressed?").count("fire-button-action"));
      CHECK(ids("How do I set the background color?").count("COLUBK"));
      CHECK(ids("How do I position a sprite horizontally?").count("position-object-x"));
      CHECK(ids("What should I have for lunch today?").empty());   // nothing relevant: nothing added
      CHECK(ids("hello").empty());
      // a name the user typed still comes first
      auto ranked = groundText(kb, "How does WSYNC affect timing?", "", context);
      CHECK(!ranked.empty() && ranked[0].id == "WSYNC");
    }
    std::string block = groundingBlock(groundText(kb, "RESP0", "", context));
    CHECK(block.find("[RESP0]") != std::string::npos && block.find("data, not instructions") != std::string::npos);
    CHECK(groundingBlock({}).empty());
  }
  // settings and chats
  {
    Settings settings;
    settings.asked = settings.chat = true;
    Provider provider;
    provider.name = "Ollama", provider.model = "qwen", provider.keyEnv = "KEY_VAR";
    settings.providers.push_back(provider);
    Settings back = Settings::fromJson(settings.toJson());
    // The built-in model is always first; the user's server comes after it and stays the active one.
    CHECK(back.chat && back.asked && back.providers.size() == 2 && back.providers[0].builtin && back.providers[1].model == "qwen" && back.providers[1].keyEnv == "KEY_VAR");
    CHECK(Settings::fromJson(nlohmann::json()).providers.size() == 1 && Settings::fromJson(nlohmann::json()).providers[0].builtin);
    // the built-in model is the default, also for settings saved before it existed with another server active
    CHECK(back.active == 0 && back.providers[0].builtin);
    CHECK(Settings::fromJson(nlohmann::json()).active == 0);
    CHECK(Settings::fromJson(Settings::fromJson(settings.toJson()).toJson()).providers.size() == 2);  // not inserted twice
    {
      Provider builtin;
      builtin.builtin = true;
      builtin.model = "qwen3-4b";
      std::vector<Message> messages = {{"user", "hi"}};
      CHECK(openAiRequestBody(builtin, messages).find("enable_thinking") != std::string::npos);
      CHECK(openAiRequestBody(provider, messages).find("enable_thinking") == std::string::npos);
    }
    CHECK(settings.toJson().dump().find("sk-") == std::string::npos);
    CHECK(!Settings::fromJson(nlohmann::json()).chat);  // off unless asked

    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("vcside_chats_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    ChatStore store(dir.string());
    Chat chat;
    chat.id = "20260101-000000", chat.title = "RESP0";
    chat.messages.push_back({"user", "hi", {}});
    chat.messages.push_back({"assistant", "yo", {"RESP0"}});
    CHECK(store.save(chat));
    {
      std::ifstream ignore(dir / ".vcsai" / ".gitignore");
      std::stringstream text;
      text << ignore.rdbuf();
      CHECK(text.str().find("chats/") != std::string::npos);
    }
    std::ofstream(dir / ".vcsai" / ".gitignore") << "";  // someone emptied it
    CHECK(store.save(chat));
    {
      std::ifstream ignore(dir / ".vcsai" / ".gitignore");
      std::stringstream text;
      text << ignore.rdbuf();
      CHECK(text.str().find("chats/") != std::string::npos);
    }
    Chat loaded;
    CHECK(store.load(chat.id, false, &loaded) && loaded.messages.size() == 2 && loaded.messages[1].reference[0] == "RESP0");
    CHECK(store.list().size() == 1 && !store.list()[0].archived);
    CHECK(store.archive(chat.id) && store.list()[0].archived && store.load(chat.id, true, &loaded));
    CHECK(store.save(chat) && store.list().size() == 1);  // saving an archived chat keeps it archived
    Chat bad;
    bad.id = "../escape";
    CHECK(!store.save(bad));
    CHECK(store.remove(chat.id, true) && store.list().empty());
    fs::remove_all(dir);
  }
#ifndef _WIN32
  // a whole answer from a mock server: chunked SSE cut in many pieces
  {
    std::string sse = "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\ndata: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n" +
                      std::string("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":2}}\n\ndata: [DONE]\n\n");
    std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n" + chunk(sse.substr(0, 40)) + chunk(sse.substr(40)) + "0\r\n\r\n";
    MockServer server(reply, 9);
    Provider provider;
    provider.baseUrl = server.url();
    provider.model = "test";
    ChatRequest request;
    request.start(provider, {{"system", "s"}, {"user", "hello"}});
    std::string text;
    int promptTokens = 0;
    bool done = false;
    for (int i = 0; i < 300 && !done; i++) {
      for (auto& event : request.poll()) {
        if (event.kind == Event::Text) text += event.text;
        if (event.kind == Event::Usage) promptTokens = event.promptTokens;
        if (event.kind == Event::Done) done = true;
        CHECK(event.kind != Event::Error);
      }
      usleep(10000);
    }
    CHECK(done && text == "Hello" && promptTokens == 5);
    CHECK(server.request.find("POST /v1/chat/completions HTTP/1.1") == 0 && server.request.find("\"hello\"") != std::string::npos);
    CHECK(server.request.find("Authorization") == std::string::npos);  // no key configured: none sent
  }
  // an error answer, and a model list
  {
    MockServer server("HTTP/1.1 404 Not Found\r\nContent-Length: 40\r\n\r\n{\"error\":{\"message\":\"model not found\"}}   ");
    Provider provider;
    provider.baseUrl = server.url();
    ChatRequest request;
    request.start(provider, {{"user", "x"}});
    std::string error;
    for (int i = 0; i < 300 && error.empty(); i++) {
      for (auto& event : request.poll())
        if (event.kind == Event::Error) error = event.text;
      usleep(10000);
    }
    CHECK(error.find("404") != std::string::npos && error.find("model not found") != std::string::npos);
  }
  {
    std::string body = "{\"data\":[{\"id\":\"a\"},{\"id\":\"b\"}]}";
    MockServer server("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    Provider provider;
    provider.baseUrl = server.url();
    std::string error;
    auto ids = listModels(provider, &error);
    CHECK(ids.size() == 2 && ids[1] == "b" && error.empty());
  }
  {
    Provider provider;
    provider.baseUrl = "http://127.0.0.1:1/v1";  // nothing listens there
    std::string error;
    CHECK(listModels(provider, &error).empty() && !error.empty());
    Provider remote;
    remote.baseUrl = "https://example.com/v1";
    ChatRequest request;
    request.start(remote, {{"user", "x"}});
    std::string message;
    for (int i = 0; i < 100 && message.empty(); i++) {
      for (auto& event : request.poll())
        if (event.kind == Event::Error) message = event.text;
      usleep(10000);
    }
    CHECK(message.find("https is not available") != std::string::npos);
  }
#endif
}

// The documentation cards as Markdown, with their See also.
static void testDocCards(const KnowledgeBase& kb) {
  const Register* swcha = kb.reg("SWCHA");
  CHECK(swcha && !swcha->fields.empty() && swcha->fields.size() == 8);
  std::string card = swcha ? registerMarkdown(*swcha) : "";
  CHECK(card.find("| Bits |") != std::string::npos && card.find("| `7` | P0 right |") != std::string::npos && card.find("```asm\n") != std::string::npos);
  bool seeFire = false, seeRecipe = false;
  if (swcha)
    for (auto& link : registerSeeAlso(kb, *swcha)) seeFire = seeFire || link.topic == "R:INPT4", seeRecipe = seeRecipe || link.topic == "K:read-controls";
  CHECK(seeFire && seeRecipe);
  const Technique* fire = nullptr;
  for (auto& technique : kb.techniques())
    if (technique.id == "fire-button-action") fire = &technique;
  CHECK(fire && techniqueMarkdown(*fire).find("1. ") != std::string::npos && !techniqueSeeAlso(kb, *fire).empty());
  for (auto& reg : kb.registers()) CHECK(registerMarkdown(reg).find("|||") == std::string::npos);
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
  testDocCards(kb);
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
  testPlusCartServer();
  testMusic();
  testSong();
  testMusicExport();
  testMusicSongs();
  testSampleExport();
  testDpcExport();
  testCdfjExport();
  testTiaTrackerImport();
  testBasic();
  testBasicSyntax(kb);
  testFormatOrder(kb);
  testGfx();
  testFolding();
  testSourceLinks();
  testThemes(data);
  testHeader(kb, data);
  testAssistant(kb);
  testBundledModel();
  testMarkdown();
  testAgentTools(kb);
  testRemote();
  testChatTabs();
  testInvalidUtf8();
  testFileOperations();
  testProjectIndex();
  testEmbeddings();
  testContextChips();
  testEditBlocks();
  testUnknownNames(kb);
  testFirstPass();
  testLengthStops();
  testNativeToolCalls();
  testBusyServiceMessages();
  testMasking(kb);
  testKnownProviders();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
