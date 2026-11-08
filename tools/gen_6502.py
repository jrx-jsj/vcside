#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 VCSIDE contributors
"""Generates data/kb/6502.json: every 6502 opcode with mode, bytes, cycles and docs.

Cycle counts: "cycles" is the base count; "page" = +1 when an indexed access
crosses a page; branches take +1 when taken and +1 more when the target is on
another page. Illegal mnemonics use DASM's names where DASM has one.

Verification (see docs/fuentes.md): all 256 opcodes were cross-checked for
size and cycles against masswerk.at, "No More Secrets" (groepaz) and the
opcode table shipped with Gopher2600; the DASM names were checked by
assembling each opcode with /opt/homebrew/bin/dasm 2.20.14.1.
All descriptions are written in our own words.
"""
import json, os, sys

MODES = {
    "imp":  {"bytes": 1, "syntax": "",        "name": "Implied"},
    "acc":  {"bytes": 1, "syntax": "",        "name": "Accumulator (DASM takes no operand: write ASL, not ASL A)"},
    "imm":  {"bytes": 2, "syntax": "#$nn",    "name": "Immediate"},
    "zp":   {"bytes": 2, "syntax": "$nn",     "name": "Zero page"},
    "zpx":  {"bytes": 2, "syntax": "$nn,X",   "name": "Zero page,X"},
    "zpy":  {"bytes": 2, "syntax": "$nn,Y",   "name": "Zero page,Y"},
    "abs":  {"bytes": 3, "syntax": "$nnnn",   "name": "Absolute"},
    "absx": {"bytes": 3, "syntax": "$nnnn,X", "name": "Absolute,X"},
    "absy": {"bytes": 3, "syntax": "$nnnn,Y", "name": "Absolute,Y"},
    "ind":  {"bytes": 3, "syntax": "($nnnn)", "name": "Indirect"},
    "indx": {"bytes": 2, "syntax": "($nn,X)", "name": "(Indirect,X)"},
    "indy": {"bytes": 2, "syntax": "($nn),Y", "name": "(Indirect),Y"},
    "rel":  {"bytes": 2, "syntax": "label",   "name": "Relative"},
}

# Read-type group: imm zp zpx abs absx absy indx indy (with page penalty on absx/absy/indy).
def alu(ops):
    modes = ["imm", "zp", "zpx", "abs", "absx", "absy", "indx", "indy"]
    cyc = [2, 3, 4, 4, 4, 4, 6, 5]
    page = [0, 0, 0, 0, 1, 1, 0, 1]
    return [(o, m, c, p) for o, m, c, p in zip(ops, modes, cyc, page) if o is not None]

# Read-modify-write: acc zp zpx abs absx
def rmw(ops):
    modes = ["acc", "zp", "zpx", "abs", "absx"]
    cyc = [2, 5, 6, 6, 7]
    return [(o, m, c, 0) for o, m, c in zip(ops, modes, cyc) if o is not None]

# Illegal RMW+ALU combos: zp zpx abs absx absy indx indy
def rmw_alu(ops):
    modes = ["zp", "zpx", "abs", "absx", "absy", "indx", "indy"]
    cyc = [5, 6, 6, 7, 7, 8, 8]
    return [(o, m, c, 0) for o, m, c in zip(ops, modes, cyc)]

def imp(op, c=2):
    return [(op, "imp", c, 0)]

OFFICIAL = {
    "ADC": alu([0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x61, 0x71]),
    "AND": alu([0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x21, 0x31]),
    "ASL": rmw([0x0A, 0x06, 0x16, 0x0E, 0x1E]),
    "BCC": [(0x90, "rel", 2, 0)], "BCS": [(0xB0, "rel", 2, 0)],
    "BEQ": [(0xF0, "rel", 2, 0)], "BMI": [(0x30, "rel", 2, 0)],
    "BNE": [(0xD0, "rel", 2, 0)], "BPL": [(0x10, "rel", 2, 0)],
    "BVC": [(0x50, "rel", 2, 0)], "BVS": [(0x70, "rel", 2, 0)],
    "BIT": [(0x24, "zp", 3, 0), (0x2C, "abs", 4, 0)],
    "BRK": imp(0x00, 7),  # 1 opcode byte; the pushed return address skips one padding byte
    "CLC": imp(0x18), "CLD": imp(0xD8), "CLI": imp(0x58), "CLV": imp(0xB8),
    "CMP": alu([0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xC1, 0xD1]),
    "CPX": [(0xE0, "imm", 2, 0), (0xE4, "zp", 3, 0), (0xEC, "abs", 4, 0)],
    "CPY": [(0xC0, "imm", 2, 0), (0xC4, "zp", 3, 0), (0xCC, "abs", 4, 0)],
    "DEC": rmw([None, 0xC6, 0xD6, 0xCE, 0xDE]),
    "DEX": imp(0xCA), "DEY": imp(0x88),
    "EOR": alu([0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x41, 0x51]),
    "INC": rmw([None, 0xE6, 0xF6, 0xEE, 0xFE]),
    "INX": imp(0xE8), "INY": imp(0xC8),
    "JMP": [(0x4C, "abs", 3, 0), (0x6C, "ind", 5, 0)],
    "JSR": [(0x20, "abs", 6, 0)],
    "LDA": alu([0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xA1, 0xB1]),
    "LDX": [(0xA2, "imm", 2, 0), (0xA6, "zp", 3, 0), (0xB6, "zpy", 4, 0), (0xAE, "abs", 4, 0), (0xBE, "absy", 4, 1)],
    "LDY": [(0xA0, "imm", 2, 0), (0xA4, "zp", 3, 0), (0xB4, "zpx", 4, 0), (0xAC, "abs", 4, 0), (0xBC, "absx", 4, 1)],
    "LSR": rmw([0x4A, 0x46, 0x56, 0x4E, 0x5E]),
    "NOP": imp(0xEA),
    "ORA": alu([0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x01, 0x11]),
    "PHA": imp(0x48, 3), "PHP": imp(0x08, 3), "PLA": imp(0x68, 4), "PLP": imp(0x28, 4),
    "ROL": rmw([0x2A, 0x26, 0x36, 0x2E, 0x3E]),
    "ROR": rmw([0x6A, 0x66, 0x76, 0x6E, 0x7E]),
    "RTI": imp(0x40, 6), "RTS": imp(0x60, 6),
    "SBC": alu([0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xE1, 0xF1]),
    "SEC": imp(0x38), "SED": imp(0xF8), "SEI": imp(0x78),
    "STA": [(0x85, "zp", 3, 0), (0x95, "zpx", 4, 0), (0x8D, "abs", 4, 0), (0x9D, "absx", 5, 0),
            (0x99, "absy", 5, 0), (0x81, "indx", 6, 0), (0x91, "indy", 6, 0)],
    "STX": [(0x86, "zp", 3, 0), (0x96, "zpy", 4, 0), (0x8E, "abs", 4, 0)],
    "STY": [(0x84, "zp", 3, 0), (0x94, "zpx", 4, 0), (0x8C, "abs", 4, 0)],
    "TAX": imp(0xAA), "TAY": imp(0xA8), "TSX": imp(0xBA),
    "TXA": imp(0x8A), "TXS": imp(0x9A), "TYA": imp(0x98),
}

# (opcodes, stable)
ILLEGAL = {
    "LAX": ([(0xA7, "zp", 3, 0), (0xB7, "zpy", 4, 0), (0xAF, "abs", 4, 0), (0xBF, "absy", 4, 1),
             (0xA3, "indx", 6, 0), (0xB3, "indy", 5, 1)], True),
    "SAX": ([(0x87, "zp", 3, 0), (0x97, "zpy", 4, 0), (0x8F, "abs", 4, 0), (0x83, "indx", 6, 0)], True),
    "DCP": (rmw_alu([0xC7, 0xD7, 0xCF, 0xDF, 0xDB, 0xC3, 0xD3]), True),
    "ISB": (rmw_alu([0xE7, 0xF7, 0xEF, 0xFF, 0xFB, 0xE3, 0xF3]), True),
    "SLO": (rmw_alu([0x07, 0x17, 0x0F, 0x1F, 0x1B, 0x03, 0x13]), True),
    "RLA": (rmw_alu([0x27, 0x37, 0x2F, 0x3F, 0x3B, 0x23, 0x33]), True),
    "SRE": (rmw_alu([0x47, 0x57, 0x4F, 0x5F, 0x5B, 0x43, 0x53]), True),
    "RRA": (rmw_alu([0x67, 0x77, 0x6F, 0x7F, 0x7B, 0x63, 0x73]), True),
    "ANC": ([(0x0B, "imm", 2, 0), (0x2B, "imm", 2, 0)], True),
    "ASR": ([(0x4B, "imm", 2, 0)], True),
    "ARR": ([(0x6B, "imm", 2, 0)], True),
    "SBX": ([(0xCB, "imm", 2, 0)], True),
    "USBC": ([(0xEB, "imm", 2, 0)], True),
    "NOP*": ([(o, "imp", 2, 0) for o in (0x1A, 0x3A, 0x5A, 0x7A, 0xDA, 0xFA)] +
             [(o, "imm", 2, 0) for o in (0x80, 0x82, 0x89, 0xC2, 0xE2)] +
             [(o, "zp", 3, 0) for o in (0x04, 0x44, 0x64)] +
             [(o, "zpx", 4, 0) for o in (0x14, 0x34, 0x54, 0x74, 0xD4, 0xF4)] +
             [(0x0C, "abs", 4, 0)] +
             [(o, "absx", 4, 1) for o in (0x1C, 0x3C, 0x5C, 0x7C, 0xDC, 0xFC)], True),
    "LAS": ([(0xBB, "absy", 4, 1)], True),
    "ANE": ([(0x8B, "imm", 2, 0)], False),
    "LXA": ([(0xAB, "imm", 2, 0)], False),
    "SHA": ([(0x9F, "absy", 5, 0), (0x93, "indy", 6, 0)], False),
    "SHS": ([(0x9B, "absy", 5, 0)], False),
    "SHX": ([(0x9E, "absy", 5, 0)], False),
    "SHY": ([(0x9C, "absx", 5, 0)], False),
    "JAM": ([(o, "imp", 0, 0) for o in (0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xB2, 0xD2, 0xF2)], False),
}

# Shared source list (also used, with the same ids, by the other data/kb files).
SOURCES = [
    {"id": "mcs6500-manual", "title": "MOS MCS6500 Microcomputer Family Hardware/Programming Manual (datasheet set)", "author": "MOS Technology", "url": "http://archive.6502.org/datasheets/mos_6500_mpu_nov_1985.pdf", "used_for": "Official instruction set, flags, BRK/JMP behaviour"},
    {"id": "masswerk", "title": "6502 Instruction Set", "author": "Norbert Landsteiner (masswerk.at)", "url": "https://www.masswerk.at/6502/6502_instruction_set.html", "used_for": "Opcode sizes and cycles, page-crossing rules, illegal opcode cycle counts"},
    {"id": "nms", "title": "No More Secrets - NMOS 6510 Unintended Opcodes v0.91", "author": "groepaz / solution", "url": "http://csdb.dk/getinternalfile.php/153783/NoMoreSecrets-NMOS6510UnintendedOpcodes-20162412.pdf", "used_for": "Illegal opcode semantics, sizes, cycles, stable/unstable classification"},
    {"id": "clark-decimal", "title": "Decimal Mode (6502.org tutorial)", "author": "Bruce Clark", "url": "http://www.6502.org/tutorials/decimal_mode.html", "used_for": "Decimal-mode flag behaviour on the NMOS 6502 (cited, not re-opened in this pass)"},
    {"id": "gopher-src", "title": "Gopher2600 source (commit 07d8de5): hardware/cpu/instructions/definitions.json", "author": "JetSetIlly", "url": "https://github.com/JetSetIlly/Gopher2600", "used_for": "Second opinion on size, cycles and page sensitivity of all 256 opcodes (GPL-3, consulted only)"},
    {"id": "dasm", "title": "DASM 2.20.14.1 (local assembler)", "author": "DASM team", "url": "https://github.com/dasm-assembler/dasm", "used_for": "Which mnemonics and operand forms assemble to which opcode bytes (every opcode assembled)"},
]


DOCS = {
    "ADC": ("Add with carry", "NVZC", "A = A + M + C. In decimal mode (SED) the addition is BCD; on the NMOS 6502 the N, V and Z flags are not reliable after a decimal ADC (Bruce Clark's decimal-mode tutorial describes the exact behaviour)."),
    "AND": ("Logical AND", "NZ", "A = A & M."),
    "ASL": ("Arithmetic shift left", "NZC", "Shifts left one bit; bit 7 goes to C, 0 enters bit 0."),
    "BCC": ("Branch if carry clear", "", "Branches if C = 0. 2 cycles, +1 if taken, +1 more if the target is on another page than the next instruction."),
    "BCS": ("Branch if carry set", "", "Branches if C = 1. 2 cycles, +1 if taken, +1 more if the target is on another page."),
    "BEQ": ("Branch if equal (Z set)", "", "Branches if Z = 1. 2 cycles, +1 if taken, +1 more if the target is on another page."),
    "BMI": ("Branch if minus (N set)", "", "Branches if N = 1. 2 cycles, +1 if taken, +1 more if the target is on another page."),
    "BNE": ("Branch if not equal (Z clear)", "", "Branches if Z = 0. 2 cycles, +1 if taken, +1 more if the target is on another page."),
    "BPL": ("Branch if plus (N clear)", "", "Branches if N = 0. 2 cycles, +1 if taken, +1 more if the target is on another page."),
    "BVC": ("Branch if overflow clear", "", "Branches if V = 0. 2 cycles, +1 if taken, +1 more if the target is on another page."),
    "BVS": ("Branch if overflow set", "", "Branches if V = 1. 2 cycles, +1 if taken, +1 more if the target is on another page."),
    "BIT": ("Bit test", "NVZ", "Z = (A & M) == 0; N and V take bits 7 and 6 of M. Common on the 2600 to test collision and input bits (D7/D6) without touching A. BIT abs is also used as a 'skip next 2 bytes' trick."),
    "BRK": ("Force interrupt", "B I", "One opcode byte, but the return address pushed is PC+2, so the byte after BRK is skipped (DASM emits only $00). Pushes P with B set, sets I and jumps through the vector at $FFFE (seen at $1FFE on the 6507, which has 13 address lines). The 6507 has no IRQ or NMI pin, so BRK is the only way to reach that vector."),
    "CLC": ("Clear carry", "C", "C = 0."),
    "CLD": ("Clear decimal mode", "D", "D = 0. Power-on state of D is undefined: clear it at start-up."),
    "CLI": ("Clear interrupt disable", "I", "I = 0. Irrelevant on the 6507 (no IRQ pin)."),
    "CLV": ("Clear overflow", "V", "V = 0."),
    "CMP": ("Compare accumulator", "NZC", "Computes A - M: C = A >= M, Z = A == M, N = bit 7 of the result."),
    "CPX": ("Compare X", "NZC", "Computes X - M and sets flags like CMP."),
    "CPY": ("Compare Y", "NZC", "Computes Y - M and sets flags like CMP."),
    "DEC": ("Decrement memory", "NZ", "M = M - 1."),
    "DEX": ("Decrement X", "NZ", "X = X - 1."),
    "DEY": ("Decrement Y", "NZ", "Y = Y - 1."),
    "EOR": ("Exclusive OR", "NZ", "A = A ^ M."),
    "INC": ("Increment memory", "NZ", "M = M + 1."),
    "INX": ("Increment X", "NZ", "X = X + 1."),
    "INY": ("Increment Y", "NZ", "Y = Y + 1."),
    "JMP": ("Jump", "", "PC = address. Indirect JMP bug: JMP ($xxFF) reads the high byte from $xx00, not from the next page."),
    "JSR": ("Jump to subroutine", "", "Pushes the address of its own last byte (PC+2 of the opcode) and jumps; RTS adds 1 when it returns. With only 128 bytes of RAM shared with variables, every nested JSR costs 2 bytes of stack."),
    "LDA": ("Load accumulator", "NZ", "A = M."),
    "LDX": ("Load X", "NZ", "X = M."),
    "LDY": ("Load Y", "NZ", "Y = M."),
    "LSR": ("Logical shift right", "NZC", "Shifts right one bit; bit 0 goes to C, 0 enters bit 7."),
    "NOP": ("No operation", "", "Does nothing for 2 cycles."),
    "ORA": ("Logical OR", "NZ", "A = A | M."),
    "PHA": ("Push accumulator", "", "Pushes A."),
    "PHP": ("Push processor status", "", "Pushes P with B and bit 5 set. The stack lives in page 1, which on the 2600 mirrors the TIA/RAM area, so if S points at a TIA write register the push writes P into it; because bit 1 of P is Z, one 3-cycle PHP can switch ENAM0, ENAM1 or ENABL (all use bit 1)."),
    "PLA": ("Pull accumulator", "NZ", "Pulls A."),
    "PLP": ("Pull processor status", "all", "Pulls P."),
    "ROL": ("Rotate left", "NZC", "Rotates left through carry."),
    "ROR": ("Rotate right", "NZC", "Rotates right through carry."),
    "RTI": ("Return from interrupt", "all", "Pulls P and PC."),
    "RTS": ("Return from subroutine", "", "Pulls PC and adds 1. Pushing an address-1 and doing RTS is a common jump-table trick."),
    "SBC": ("Subtract with carry", "NVZC", "A = A - M - (1 - C). Set C (SEC) before a single-byte subtraction."),
    "SEC": ("Set carry", "C", "C = 1."),
    "SED": ("Set decimal mode", "D", "D = 1: ADC/SBC work in BCD. Handy for scores."),
    "SEI": ("Set interrupt disable", "I", "I = 1. Customary at reset even though the 6507 has no IRQ pin."),
    "STA": ("Store accumulator", "", "M = A. STA abs,X / abs,Y and (zp),Y always take the extra cycle."),
    "STX": ("Store X", "", "M = X."),
    "STY": ("Store Y", "", "M = Y."),
    "TAX": ("Transfer A to X", "NZ", "X = A."),
    "TAY": ("Transfer A to Y", "NZ", "Y = A."),
    "TSX": ("Transfer S to X", "NZ", "X = S."),
    "TXA": ("Transfer X to A", "NZ", "A = X."),
    "TXS": ("Transfer X to S", "", "S = X. Used to aim the stack at a TIA register before a PHP/PHA that writes it (see PHP)."),
    "TYA": ("Transfer Y to A", "NZ", "A = Y."),
    # Illegal
    "LAX": ("Load A and X (illegal)", "NZ", "A = X = M. Stable except the immediate form (LXA)."),
    "SAX": ("Store A AND X (illegal)", "", "M = A & X; flags untouched."),
    "DCP": ("Decrement and compare (illegal)", "NZC", "M = M - 1, then compares A with the result as CMP does (C = A >= M, Z = A == M). Saves a separate DEC and CMP; groepaz's document shows loop-counter uses. Documented as DCM/DCP."),
    "ISB": ("Increment and subtract (illegal)", "NVZC", "M = M + 1, then SBC M. Also known as ISC or INS; DASM spells it ISB."),
    "SLO": ("Shift left and OR (illegal)", "NZC", "M = M << 1, then A = A | M."),
    "RLA": ("Rotate left and AND (illegal)", "NZC", "M = ROL M, then A = A & M."),
    "SRE": ("Shift right and EOR (illegal)", "NZC", "M = M >> 1, then A = A ^ M."),
    "RRA": ("Rotate right and add (illegal)", "NVZC", "M = ROR M, then A = A + M + C."),
    "ANC": ("AND, carry = bit 7 (illegal)", "NZC", "A = A & #imm; C is set to bit 7 of the result (the N flag). Both $0B and $2B do this."),
    "ASR": ("AND then LSR (illegal)", "NZC", "A = (A & #imm) >> 1. Also known as ALR (groepaz's name); DASM spells it ASR."),
    "ARR": ("AND then ROR (illegal)", "NVZC", "A = ROR(A & #imm), but C and V are derived from bits 6 and 5 of the result instead of the normal ROR rule (see groepaz's document); decimal mode adds further quirks."),
    "SBX": ("X = (A AND X) - imm (illegal)", "NZC", "X = (A & X) - #imm; the subtraction ignores the incoming carry, and C, Z, N come out like CMP. Also known as AXS or SAX in older lists; DASM spells it SBX."),
    "USBC": ("SBC immediate (illegal duplicate)", "NVZC", "Opcode $EB behaves like SBC #imm. DASM has no USBC mnemonic and always assembles SBC #imm as $E9, so $EB needs .byte."),
    "NOP*": ("Illegal NOPs", "", "Do nothing but read their operand. NOP zp (3 cycles) and NOP abs (4) are handy for exact delays and 1-byte/2-byte padding; because the operand is really read, NOP abs on a hotspot address can bankswitch. DASM only assembles the forms with an operand (NOP zp, NOP zp,X, NOP abs, NOP abs,X, NOP #imm); the 1-byte variants ($1A, $3A, $5A, $7A, $DA, $FA) and $44/$64/$54/$74/$D4/$F4/$82/$89/$C2/$E2/$3C etc. must be emitted with .byte. One source claims $82, $C2 and $E2 may halt the CPU on some chips, so avoid them."),
    "LAS": ("A = X = S = M AND S (illegal)", "NZ", "A, X and S all receive M & S. Classed as stable by groepaz and masswerk. Also called LAR. Rarely useful."),
    "ANE": ("A = (A | magic) & X & imm (highly unstable)", "NZ", "Result depends on a chip- and temperature-dependent constant: do not use. Also called XAA."),
    "LXA": ("LAX immediate (highly unstable)", "NZ", "Opcode $AB. A = X = (A | magic) & imm; the constant depends on chip and temperature, so avoid it. Also called LAX #imm, ATX or OAL."),
    "SHA": ("Store A & X & (H+1) (unstable)", "", "H is the high byte of the target address. Unreliable when indexing crosses a page. Also called AHX or AXA. Avoid."),
    "SHS": ("S = A & X; store S & (H+1) (unstable)", "", "Also called TAS. Unreliable when indexing crosses a page. Avoid."),
    "SHX": ("Store X & (H+1) (unstable)", "", "Also called SXA or XAS. Unreliable when indexing crosses a page. Avoid."),
    "SHY": ("Store Y & (H+1) (unstable)", "", "Also called SYA or SAY. Unreliable when indexing crosses a page. Avoid."),
    "JAM": ("Halt the CPU", "", "Locks up the processor until reset; 12 opcodes ($02 $12 $22 $32 $42 $52 $62 $72 $92 $B2 $D2 $F2). Also called KIL or HLT. Deterministic, but marked unstable here so the 'stable only' policy rejects it. DASM has no mnemonic: use .byte. The cycle count is 0 because it never finishes."),
}

# Opcodes that DASM 2.20.14.1 cannot emit from a mnemonic (checked by assembling every
# opcode): JAM and USBC have no mnemonic and the other-than-canonical NOPs only
# exist as NOP with an operand. Use .byte for these.
DASM_BYTE_ONLY = {0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xB2, 0xD2, 0xF2,
                  0x1A, 0x3A, 0x5A, 0x7A, 0xDA, 0xFA, 0x34, 0x44, 0x54, 0x64, 0x74, 0xD4, 0xF4,
                  0x3C, 0x5C, 0x7C, 0xDC, 0xFC, 0x82, 0x89, 0xC2, 0xE2, 0x2B, 0xEB}
# Spelling used by DASM when it differs from the common name.
DASM_NAME = {"ASR": "ASR", "SBX": "SBX", "ISB": "ISB", "SHS": "SHS", "SHA": "SHA",
             "SHX": "SHX", "SHY": "SHY", "LXA": "LXA", "ANE": "ANE", "LAS": "LAS",
             "ANC": "ANC", "DCP": "DCP", "SLO": "SLO", "RLA": "RLA", "SRE": "SRE",
             "RRA": "RRA", "LAX": "LAX", "SAX": "SAX", "NOP": "NOP", "USBC": None, "JAM": None}
# Other names found in the literature (groepaz, masswerk).
ALIASES = {"ASR": ["ALR"], "SBX": ["AXS", "SAX"], "ISB": ["ISC", "INS"], "SHS": ["TAS", "XAS"],
           "SHA": ["AHX", "AXA"], "SHX": ["SXA", "XAS", "A11"], "SHY": ["SYA", "SAY", "A11"],
           "LXA": ["LAX #imm", "ATX", "OAL"], "ANE": ["XAA"], "LAS": ["LAR"], "JAM": ["KIL", "HLT"],
           "SLO": ["ASO"], "SRE": ["LSE"], "SAX": ["AAX", "AXS"], "DCP": ["DCM"],
           "USBC": ["SBC #imm ($EB)"], "NOP": ["DOP", "SKB", "TOP", "SKW"]}
ILLEGAL_SRC = ["nms", "masswerk", "gopher-src", "dasm"]
OFFICIAL_SRC = ["mcs6500-manual", "masswerk", "nms", "gopher-src", "dasm", "clark-decimal"]


def main(out):
    opcodes = []
    for table, illegal in ((OFFICIAL, False), ({k: v[0] for k, v in ILLEGAL.items()}, True)):
        for mn, ops in table.items():
            for op, mode, cyc, page in ops:
                stable = True if not illegal else ILLEGAL[mn][1]
                entry = {
                    "op": f"${op:02X}", "mnemonic": mn.rstrip("*"), "mode": mode,
                    "bytes": MODES[mode]["bytes"],
                    "cycles": cyc, "page_penalty": bool(page), "branch": mode == "rel",
                    "illegal": illegal, "stable": stable,
                    # False = DASM cannot assemble this opcode from a mnemonic: emit it with .byte
                    "dasm": op not in DASM_BYTE_ONLY,
                }
                if mn == "JAM":
                    entry["hangs"] = True
                opcodes.append(entry)
    opcodes.sort(key=lambda o: int(o["op"][1:], 16))
    ops_seen = [o["op"] for o in opcodes]
    assert len(ops_seen) == len(set(ops_seen)) == 256, len(set(ops_seen))
    instructions = {}
    for mn, (summary, flags, desc) in DOCS.items():
        name = mn.rstrip("*")
        if name in instructions:
            continue
        ill = mn in ILLEGAL
        e = {"summary": summary, "flags": flags, "description": desc, "illegal": ill,
             "verified": True, "sources": ILLEGAL_SRC if ill else OFFICIAL_SRC}
        if name in ALIASES:
            e["aliases"] = ALIASES[name]
        if ill:
            e["dasm_name"] = DASM_NAME.get(name)
        instructions[name] = e
    data = {
        "cpu": "6502/6507",
        "sources": SOURCES,
        "instruction_sources": OFFICIAL_SRC + [x for x in ILLEGAL_SRC if x not in OFFICIAL_SRC],
        "notes": [
            "The 6507 in the 2600 is a 6502 with 13 address lines (8 KB space, cartridge at $1000-$1FFF) and no IRQ/NMI pins.",
            "CPU clock is the colour clock / 3: 76 CPU cycles per scanline (228 colour clocks).",
            "Indexed reads (abs,X / abs,Y / (zp),Y) take +1 cycle when the effective address crosses a page; stores and read-modify-write instructions already include that cycle in their fixed count.",
            "Branches: 2 cycles not taken, 3 taken, 4 taken to another page.",
            "Zero-page indexed addressing wraps inside page 0 ($FF,X with X=2 reads $01).",
            "Size and cycles of all 256 opcodes agree between masswerk.at, groepaz's 'No More Secrets' and the opcode table in Gopher2600; the 'dasm' flag records whether DASM 2.20.14.1 can emit the opcode from a mnemonic (false = use .byte).",
            "Unstable illegal opcodes (groepaz): SHA, SHX, SHY, SHS/TAS (unreliable address-high-byte group) and ANE, LXA (magic constant). JAM is deterministic but marked unstable here because it freezes the CPU.",
        ],
        "modes": MODES, "instructions": instructions, "opcodes": opcodes,
    }
    with open(out, "w") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"{out}: {len(opcodes)} opcodes, {len(instructions)} instructions")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "data", "kb", "6502.json"))
