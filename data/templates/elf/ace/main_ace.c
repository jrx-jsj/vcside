/*
 * {{NAME}} - the ACE file's main (make ace): takes the 6507 over and calls the game's
 * elf_main as an ELF loader would, started from the cartridge's menu.
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * ACE gives the program no TV system: ACE_SYSTEM (the Makefile's, from the project's TV
 * standard: 0 NTSC, 1 PAL, 2 PAL60) is passed as the ELF loaders pass theirs.
 */
#include "ace.h"

#ifndef ACE_SYSTEM
#define ACE_SYSTEM 0
#endif

int elf_main(uint32_t* args);

int main(void)
{
    uint32_t args[3];
    args[0] = ACE_SYSTEM;                    /* MP_SYSTEM_TYPE */
    args[1] = ACE_ARGUMENTS->clock_hz;       /* MP_CLOCK_HZ */
    args[2] = 1;                             /* MP_FEATURE_FLAGS: FF_MULTI_CART, RESET may leave */
    ace_start();
    elf_main(args);
    /* The game has left the 6507 running nops; from there to the reset vector, and the
     * start code hands the bus back to the firmware's menu. */
    ace_leave();
    return 1;
}
