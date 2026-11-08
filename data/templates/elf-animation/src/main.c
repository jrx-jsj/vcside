/*
 * {{NAME}} - made with VCSIDE (ELF cartridge, C on the cartridge's ARM).
 *
 * An animated sprite that walks with the joystick. Its frames, colours and animations are
 * drawn in hero.vcsgfx (open it in VCSIDE): every save writes hero.gfx.h, which this file
 * includes. The "idle" animation blinks now and then; "walk" plays while it moves.
 *
 * The ARM feeds the 6507 every instruction of the visible lines; during the overblank the
 * 6507 runs VSYNC, VBLANK and overscan by itself and the ARM runs the game logic.
 *
 * Build: F7 (runs make). Run: F5. This template is CC0: use it as you like.
 */
#include "vcs.h"
#include "hero.gfx.h"

#define LINES_PER_ROW 2          /* each row of the sprite is drawn on two lines */

static int lines = 199;          /* visible lines: 262 - 63 of overblank (NTSC, PAL60) */
static int multicart;            /* started from a menu (PlusCart, UnoCart): can go back */
static uint8_t swcha, swchb;     /* joysticks and console switches, read after the kernel */
static int hero_x = 76, hero_y = 90, facing_left;
static int hero_frame;           /* the frame the kernel draws */
static vcsgfx_player hero;       /* the animation that is playing and where it is */

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

/* Puts player 0 at x (0-159): RESP0 on the right cycle of a line, then HMOVE for the
 * last pixels. RESP0 after n nops puts it at 6n - 54 (from 10 nops on: earlier, it would
 * still be in the horizontal blank), so it goes 1-6 pixels to the right of x and HMOVE
 * takes it back. Two lines. */
static RAM_K void position_p0(int x)
{
    vcsSta3(WSYNC);
    vcsNop2n((uint16_t)(10 + x / 6));
    vcsSta3(RESP0);
    vcsWrite5(HMP0, (uint8_t)((6 - x % 6) << 4));  /* positive: to the left */
    vcsSta3(WSYNC);
    vcsSta3(HMOVE);
}

/* The visible lines. Between two vcsLib calls the 6507 is already waiting for the next
 * instruction, so work is only done right after a WSYNC (or in the overblank). */
static RAM_FUNC void kernel(void)
{
    const uint8_t *gfx = hero_P0_gfx[hero_frame], *colour = hero_P0_colour[hero_frame];
    vcsEndOverblank();
    vcsWrite5(NUSIZ0, HERO_P0_NUSIZ);
    vcsWrite5(REFP0, facing_left ? 0x08 : 0x00);
    position_p0(hero_x);
    vcsJmp3();
    for (int i = 2; i < lines - 1; i++) {
        vcsSta3(WSYNC);
        if (i == 2) vcsWrite5(VBLANK, 0);
        unsigned row = (unsigned)(i - hero_y) / LINES_PER_ROW;
        if (i >= hero_y && row < HERO_P0_HEIGHT) {
            vcsWrite5(GRP0, gfx[row]);
            vcsWrite5(COLUP0, colour[row]);
        } else {
            vcsWrite5(GRP0, 0);
        }
        vcsJmp3();                /* once a line: the 6507's PC must stay in its 4 KB */
    }
    vcsSta3(WSYNC);
    vcsWrite5(VBLANK, 2);
    vcsWrite5(GRP0, 0);
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
        int moving = 0;
        if (!(swcha & 0x80) && hero_x < 160 - 8) hero_x++, facing_left = 0, moving = 1;   /* right */
        if (!(swcha & 0x40) && hero_x > 0) hero_x--, facing_left = 1, moving = 1;         /* left */
        if (!(swcha & 0x10) && hero_y > 2) hero_y--, moving = 1;                          /* up */
        if (!(swcha & 0x20) && hero_y < lines - 1 - HERO_P0_HEIGHT * LINES_PER_ROW) hero_y++, moving = 1;
        vcsgfx_play(&hero, moving ? &hero_walk : &hero_idle);
        hero_frame = vcsgfx_animate(&hero);
        if (multicart && !(swchb & 0x01)) {               /* RESET: back to the menu */
            leave();
            return 0;
        }
        kernel();
    }
}
