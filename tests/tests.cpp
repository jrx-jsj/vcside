// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// Tests without GUI: knowledge base, cycle counter, output parsers.
#include <cstdio>
#include <string>

#include "builder_parse.h"
#include "cycles.h"
#include "kb.h"

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

int main() {
  KnowledgeBase kb;
  std::string err;
  kb.load(VCSIDE_SOURCE_DATA "/kb", &err);
  if (!err.empty()) std::fprintf(stderr, "knowledge base: %s", err.c_str());
  CHECK(err.empty());
  testKb(kb);
  testCycles(kb);
  testParsers();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
