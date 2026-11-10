/* SPDX-License-Identifier: CC0-1.0 (VCSIDE ACE proof of concept) */
/* Minimal ACE program: serves a 4K 6507 ROM from the ARM, reading the bus through the
 * register addresses the firmware leaves at 0x20000000. The firmware may put the file anywhere
 * (ACE-UF00), so data is reached as an offset from __ace_start plus the file's real address,
 * word 0 of that block. On the UnoCart the firmware's data pointers already point at the
 * high byte of its GPIOE, so the same code runs on both carts. */
#include <stdint.h>

extern const uint8_t vcs_rom[4096];
extern const uint8_t __ace_start[];

enum { ARG_FILE = 0, ARG_ADDR_IDR = 7, ARG_DATA_ODR = 9, ARG_DATA_MODER = 10 };

__attribute__((noreturn)) void ace_entry(void) {
  const uint32_t* arguments = (const uint32_t*)0x20000000;
  const uint8_t* rom = (const uint8_t*)(arguments[ARG_FILE] + (uint32_t)(vcs_rom - __ace_start));
  volatile uint16_t* addressBus = (volatile uint16_t*)arguments[ARG_ADDR_IDR];
  volatile uint8_t* dataOut = (volatile uint8_t*)arguments[ARG_DATA_ODR];
  volatile uint16_t* dataMode = (volatile uint16_t*)arguments[ARG_DATA_MODER];
  for (;;) {
    uint16_t address = *addressBus;
    if (address & 0x1000) {
      *dataOut = rom[address & 0x0FFF];
      *dataMode = 0x5555;  /* data pins as outputs */
    } else {
      *dataMode = 0;       /* TIA or RIOT: let go of the bus */
    }
  }
}
