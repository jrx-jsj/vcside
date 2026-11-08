/*
 * {{NAME}} - made with VCSIDE (ELF cartridge, C on the cartridge's ARM).
 *
 * A scrolling rainbow with a white bar you move with the joystick. The ARM feeds the
 * 6507 every instruction of the visible lines; during the overblank the 6507 runs VSYNC,
 * VBLANK and overscan by itself and the ARM is free for the game logic.
 *
 * Build: F7 (runs make). Run: F5. This template is CC0: use it as you like.
 */
#include "vcs.h"

static int lines = 199;          /* visible lines: 262 - 63 of overblank (NTSC, PAL60) */
static int multicart;            /* started from a menu (PlusCart, UnoCart): can go back */
static uint8_t swcha, swchb;     /* joysticks and console switches, read after the kernel */
static int bar = 90, frame;

/* Starts the 6507: back to $1000, TIA and RIOT RAM cleared, then the overblank program. */
static RAM_FUNC void start_6507(void)
{
    vcsJmp3();
    vcsLda2(0);
    for (int i = 0; i < 256; i++)
        vcsSta3((uint8_t)i);
    vcsCopyOverblankToRiotRam();
    vcsStartOverblank();          /* nothing long before this: the 6507 would run out of code */
}

/* The visible lines. Between two vcsLib calls the 6507 is already waiting for the next
 * instruction, so work is only done right after a WSYNC (or in the overblank). */
static RAM_FUNC void kernel(void)
{
    vcsEndOverblank();
    for (int i = 0; i < lines - 1; i++) {
        vcsSta3(WSYNC);
        if (i == 0) vcsWrite5(VBLANK, 0);
        uint8_t colour = (i >= bar && i < bar + 8) ? 0x0E : (uint8_t)((((i >> 2) + frame) & 0x0F) << 4 | 0x06);
        vcsWrite5(COLUBK, colour);
        vcsJmp3();                /* once a line: the 6507's PC must stay in its 4 KB */
    }
    vcsSta3(WSYNC);
    vcsWrite5(VBLANK, 2);
    vcsWrite5(COLUBK, 0);
    swcha = vcsRead4(SWCHA);
    swchb = vcsRead4(SWCHB);
    vcsStartOverblank();
}

/* Back to the menu: the 6507 runs nops from $1000, as the menu left it. */
static RAM_FUNC void leave(void)
{
    vcsEndOverblank();
    vcsWrite5(VBLANK, 2);
    vcsJmp3();
    vcsNop2n(1024);
}

int elf_main(uint32_t *args)
{
    unsigned system = args[MP_SYSTEM_TYPE];
    if (system == ST_PAL_2600 || system == ST_PAL_7800) lines = 312 - 63;
    multicart = (args[MP_FEATURE_FLAGS] & FF_MULTI_CART) != 0;
    /* The Chameleon Cart calls elf_main with the 6507 already in an overblank. */
    if (args[MP_FEATURE_FLAGS] & FF_CHAMELEON_CART) vcsEndOverblank();
    start_6507();
    for (;;) {
        /* Overblank: the game logic goes here. */
        if (!(swcha & 0x10) && bar > 0) bar--;            /* joystick 0 up */
        if (!(swcha & 0x20) && bar < lines - 9) bar++;    /* joystick 0 down */
        frame++;
        if (multicart && !(swchb & 0x01)) {               /* RESET: back to the menu */
            leave();
            return 0;
        }
        kernel();
    }
}
