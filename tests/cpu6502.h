// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// A 6502 for the tests: the documented instructions, binary arithmetic only (the code
// it runs never sets decimal mode), 64 KB of plain memory. It runs a routine and returns
// when it does, so a test can check what the routine left in memory.
#pragma once
#include <cstdint>
#include <vector>

struct Cpu6502 {
  uint8_t memory[65536] = {};
  uint8_t a = 0, x = 0, y = 0, stack = 0xfd;
  bool carry = false, zero = false, overflow = false, negative = false, decimal = false;
  uint16_t pc = 0;
  long instructions = 0;  // run so far
  bool failed = false;

  uint8_t read(uint16_t address) { return memory[address]; }
  void write(uint16_t address, uint8_t value) { memory[address] = value; }
  uint16_t word(uint16_t address) { return (uint16_t)(read(address) | read((uint16_t)(address + 1)) << 8); }
  uint16_t zeroPageWord(uint8_t address) { return (uint16_t)(read(address) | read((uint8_t)(address + 1)) << 8); }
  void push(uint8_t value) { write((uint16_t)(0x100 + stack--), value); }
  uint8_t pull() { return read((uint16_t)(0x100 + ++stack)); }
  void flags(uint8_t value) { zero = value == 0, negative = (value & 0x80) != 0; }
  uint8_t status() const { return (uint8_t)(negative << 7 | overflow << 6 | 0x30 | decimal << 3 | zero << 1 | carry); }
  void setStatus(uint8_t value) {
    negative = value & 0x80, overflow = value & 0x40, decimal = value & 8, zero = value & 2, carry = value & 1;
  }

  // Calls the routine at address as a JSR would and runs until its RTS.
  bool call(uint16_t address, long limit = 200000) {
    const uint16_t back = 0xfff0;
    push((uint8_t)((back - 1) >> 8)), push((uint8_t)(back - 1));
    pc = address;
    for (long steps = 0; steps < limit; steps++) {
      if (pc == back) return !failed;
      step();
      if (failed) return false;
    }
    return false;
  }

  void step() {
    uint8_t opcode = read(pc++);
    instructions++;
    auto immediate = [&] { return pc++; };
    auto zeroPage = [&] { return (uint16_t)read(pc++); };
    auto zeroPageX = [&] { return (uint16_t)(uint8_t)(read(pc++) + x); };
    auto zeroPageY = [&] { return (uint16_t)(uint8_t)(read(pc++) + y); };
    auto absolute = [&] { uint16_t address = word(pc); pc += 2; return address; };
    auto absoluteX = [&] { uint16_t address = (uint16_t)(word(pc) + x); pc += 2; return address; };
    auto absoluteY = [&] { uint16_t address = (uint16_t)(word(pc) + y); pc += 2; return address; };
    auto indirectX = [&] { return zeroPageWord((uint8_t)(read(pc++) + x)); };
    auto indirectY = [&] { return (uint16_t)(zeroPageWord(read(pc++)) + y); };
    auto branch = [&](bool taken) {
      int8_t offset = (int8_t)read(pc++);
      if (taken) pc = (uint16_t)(pc + offset);
    };
    auto add = [&](uint8_t value) {
      if (decimal) failed = true;
      unsigned sum = a + value + carry;
      overflow = (~(a ^ value) & (a ^ sum) & 0x80) != 0;
      carry = sum > 0xff;
      a = (uint8_t)sum;
      flags(a);
    };
    auto compare = [&](uint8_t reg, uint8_t value) {
      carry = reg >= value;
      flags((uint8_t)(reg - value));
    };
    auto shiftLeft = [&](uint8_t value) { carry = value & 0x80; value <<= 1; flags(value); return value; };
    auto shiftRight = [&](uint8_t value) { carry = value & 1; value >>= 1; flags(value); return value; };
    auto rotateLeft = [&](uint8_t value) { bool in = carry; carry = value & 0x80; value = (uint8_t)(value << 1 | in); flags(value); return value; };
    auto rotateRight = [&](uint8_t value) { bool in = carry; carry = value & 1; value = (uint8_t)(value >> 1 | in << 7); flags(value); return value; };
    auto modify = [&](uint16_t address, auto operation) { write(address, operation(read(address))); };
    auto bitTest = [&](uint8_t value) { zero = (a & value) == 0, negative = value & 0x80, overflow = value & 0x40; };
    switch (opcode) {
      // loads and stores
      case 0xa9: a = read(immediate()), flags(a); break;
      case 0xa5: a = read(zeroPage()), flags(a); break;
      case 0xb5: a = read(zeroPageX()), flags(a); break;
      case 0xad: a = read(absolute()), flags(a); break;
      case 0xbd: a = read(absoluteX()), flags(a); break;
      case 0xb9: a = read(absoluteY()), flags(a); break;
      case 0xa1: a = read(indirectX()), flags(a); break;
      case 0xb1: a = read(indirectY()), flags(a); break;
      case 0xa2: x = read(immediate()), flags(x); break;
      case 0xa6: x = read(zeroPage()), flags(x); break;
      case 0xb6: x = read(zeroPageY()), flags(x); break;
      case 0xae: x = read(absolute()), flags(x); break;
      case 0xbe: x = read(absoluteY()), flags(x); break;
      case 0xa0: y = read(immediate()), flags(y); break;
      case 0xa4: y = read(zeroPage()), flags(y); break;
      case 0xb4: y = read(zeroPageX()), flags(y); break;
      case 0xac: y = read(absolute()), flags(y); break;
      case 0xbc: y = read(absoluteX()), flags(y); break;
      case 0x85: write(zeroPage(), a); break;
      case 0x95: write(zeroPageX(), a); break;
      case 0x8d: write(absolute(), a); break;
      case 0x9d: write(absoluteX(), a); break;
      case 0x99: write(absoluteY(), a); break;
      case 0x81: write(indirectX(), a); break;
      case 0x91: write(indirectY(), a); break;
      case 0x86: write(zeroPage(), x); break;
      case 0x96: write(zeroPageY(), x); break;
      case 0x8e: write(absolute(), x); break;
      case 0x84: write(zeroPage(), y); break;
      case 0x94: write(zeroPageX(), y); break;
      case 0x8c: write(absolute(), y); break;
      // transfers and the stack
      case 0xaa: x = a, flags(x); break;
      case 0xa8: y = a, flags(y); break;
      case 0x8a: a = x, flags(a); break;
      case 0x98: a = y, flags(a); break;
      case 0xba: x = stack, flags(x); break;
      case 0x9a: stack = x; break;
      case 0x48: push(a); break;
      case 0x68: a = pull(), flags(a); break;
      case 0x08: push(status()); break;
      case 0x28: setStatus(pull()); break;
      // arithmetic and logic
      case 0x69: add(read(immediate())); break;
      case 0x65: add(read(zeroPage())); break;
      case 0x75: add(read(zeroPageX())); break;
      case 0x6d: add(read(absolute())); break;
      case 0x7d: add(read(absoluteX())); break;
      case 0x79: add(read(absoluteY())); break;
      case 0x61: add(read(indirectX())); break;
      case 0x71: add(read(indirectY())); break;
      case 0xe9: add((uint8_t)~read(immediate())); break;
      case 0xe5: add((uint8_t)~read(zeroPage())); break;
      case 0xf5: add((uint8_t)~read(zeroPageX())); break;
      case 0xed: add((uint8_t)~read(absolute())); break;
      case 0xfd: add((uint8_t)~read(absoluteX())); break;
      case 0xf9: add((uint8_t)~read(absoluteY())); break;
      case 0xe1: add((uint8_t)~read(indirectX())); break;
      case 0xf1: add((uint8_t)~read(indirectY())); break;
      case 0x29: a &= read(immediate()), flags(a); break;
      case 0x25: a &= read(zeroPage()), flags(a); break;
      case 0x35: a &= read(zeroPageX()), flags(a); break;
      case 0x2d: a &= read(absolute()), flags(a); break;
      case 0x3d: a &= read(absoluteX()), flags(a); break;
      case 0x39: a &= read(absoluteY()), flags(a); break;
      case 0x21: a &= read(indirectX()), flags(a); break;
      case 0x31: a &= read(indirectY()), flags(a); break;
      case 0x09: a |= read(immediate()), flags(a); break;
      case 0x05: a |= read(zeroPage()), flags(a); break;
      case 0x15: a |= read(zeroPageX()), flags(a); break;
      case 0x0d: a |= read(absolute()), flags(a); break;
      case 0x1d: a |= read(absoluteX()), flags(a); break;
      case 0x19: a |= read(absoluteY()), flags(a); break;
      case 0x01: a |= read(indirectX()), flags(a); break;
      case 0x11: a |= read(indirectY()), flags(a); break;
      case 0x49: a ^= read(immediate()), flags(a); break;
      case 0x45: a ^= read(zeroPage()), flags(a); break;
      case 0x55: a ^= read(zeroPageX()), flags(a); break;
      case 0x4d: a ^= read(absolute()), flags(a); break;
      case 0x5d: a ^= read(absoluteX()), flags(a); break;
      case 0x59: a ^= read(absoluteY()), flags(a); break;
      case 0x41: a ^= read(indirectX()), flags(a); break;
      case 0x51: a ^= read(indirectY()), flags(a); break;
      case 0xc9: compare(a, read(immediate())); break;
      case 0xc5: compare(a, read(zeroPage())); break;
      case 0xd5: compare(a, read(zeroPageX())); break;
      case 0xcd: compare(a, read(absolute())); break;
      case 0xdd: compare(a, read(absoluteX())); break;
      case 0xd9: compare(a, read(absoluteY())); break;
      case 0xc1: compare(a, read(indirectX())); break;
      case 0xd1: compare(a, read(indirectY())); break;
      case 0xe0: compare(x, read(immediate())); break;
      case 0xe4: compare(x, read(zeroPage())); break;
      case 0xec: compare(x, read(absolute())); break;
      case 0xc0: compare(y, read(immediate())); break;
      case 0xc4: compare(y, read(zeroPage())); break;
      case 0xcc: compare(y, read(absolute())); break;
      case 0x24: bitTest(read(zeroPage())); break;
      case 0x2c: bitTest(read(absolute())); break;
      // increments, decrements and shifts
      case 0xe6: modify(zeroPage(), [&](uint8_t value) { value++; flags(value); return value; }); break;
      case 0xf6: modify(zeroPageX(), [&](uint8_t value) { value++; flags(value); return value; }); break;
      case 0xee: modify(absolute(), [&](uint8_t value) { value++; flags(value); return value; }); break;
      case 0xfe: modify(absoluteX(), [&](uint8_t value) { value++; flags(value); return value; }); break;
      case 0xc6: modify(zeroPage(), [&](uint8_t value) { value--; flags(value); return value; }); break;
      case 0xd6: modify(zeroPageX(), [&](uint8_t value) { value--; flags(value); return value; }); break;
      case 0xce: modify(absolute(), [&](uint8_t value) { value--; flags(value); return value; }); break;
      case 0xde: modify(absoluteX(), [&](uint8_t value) { value--; flags(value); return value; }); break;
      case 0xe8: x++, flags(x); break;
      case 0xc8: y++, flags(y); break;
      case 0xca: x--, flags(x); break;
      case 0x88: y--, flags(y); break;
      case 0x0a: a = shiftLeft(a); break;
      case 0x06: modify(zeroPage(), shiftLeft); break;
      case 0x16: modify(zeroPageX(), shiftLeft); break;
      case 0x0e: modify(absolute(), shiftLeft); break;
      case 0x1e: modify(absoluteX(), shiftLeft); break;
      case 0x4a: a = shiftRight(a); break;
      case 0x46: modify(zeroPage(), shiftRight); break;
      case 0x56: modify(zeroPageX(), shiftRight); break;
      case 0x4e: modify(absolute(), shiftRight); break;
      case 0x5e: modify(absoluteX(), shiftRight); break;
      case 0x2a: a = rotateLeft(a); break;
      case 0x26: modify(zeroPage(), rotateLeft); break;
      case 0x36: modify(zeroPageX(), rotateLeft); break;
      case 0x2e: modify(absolute(), rotateLeft); break;
      case 0x3e: modify(absoluteX(), rotateLeft); break;
      case 0x6a: a = rotateRight(a); break;
      case 0x66: modify(zeroPage(), rotateRight); break;
      case 0x76: modify(zeroPageX(), rotateRight); break;
      case 0x6e: modify(absolute(), rotateRight); break;
      case 0x7e: modify(absoluteX(), rotateRight); break;
      // jumps and branches
      case 0x4c: pc = absolute(); break;
      case 0x6c: { uint16_t pointer = absolute(); pc = (uint16_t)(read(pointer) | read((uint16_t)((pointer & 0xff00) | ((pointer + 1) & 0xff))) << 8); break; }
      case 0x20: { uint16_t target = absolute(); uint16_t back = (uint16_t)(pc - 1); push((uint8_t)(back >> 8)), push((uint8_t)back); pc = target; break; }
      case 0x60: { uint8_t low = pull(); pc = (uint16_t)((low | pull() << 8) + 1); break; }
      case 0x40: { setStatus(pull()); uint8_t low = pull(); pc = (uint16_t)(low | pull() << 8); break; }
      case 0x10: branch(!negative); break;
      case 0x30: branch(negative); break;
      case 0x50: branch(!overflow); break;
      case 0x70: branch(overflow); break;
      case 0x90: branch(!carry); break;
      case 0xb0: branch(carry); break;
      case 0xd0: branch(!zero); break;
      case 0xf0: branch(zero); break;
      // flags
      case 0x18: carry = false; break;
      case 0x38: carry = true; break;
      case 0x58: break;  // CLI
      case 0x78: break;  // SEI
      case 0xb8: overflow = false; break;
      case 0xd8: decimal = false; break;
      case 0xf8: decimal = true; break;
      case 0xea: break;
      default: failed = true; break;  // BRK or an undocumented opcode: not in the code it runs
    }
  }
};
