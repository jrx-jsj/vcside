/*
 * {{NAME}} - VCSIDE The Game, made with VCSIDE (ELF cartridge, C on the cartridge's ARM).
 *
 * A room with walls where the hero walks and shoots at two enemies: hitting one scores a
 * point, touching one costs a life, and after the last life the title screen comes back.
 * The title screen has a menu: PLAY, SYSTEM (NTSC, PAL60, PAL), HMOVE (NORMAL, SHORT) and,
 * when the cartridge was started from a menu, EXIT.
 *
 *   game.c    the rules (movement, walls, enemies, shot, score, lives)
 *   menu.c    the title screen's menu
 *   screen.c  what the kernels below draw, worked out in the overblank
 *   main.c    elf_main, the kernels and the sound: everything that talks to the 6507
 *
 * Graphics: hero.vcsgfx (player 0, idle and walk), enemy.vcsgfx (player 1), room.vcsgfx
 * (the room's playfield) and title.vcsgfx (the title's cartridge); saving one writes its
 * .gfx.h. Music: theme.vcsmusic (saving it writes theme.music.h), with the effects shot,
 * hit and lose. It is played as samples: the ARM mixes the song's voices into one level per
 * scanline, written to AUDV0 (with AUDC0 = 0) on every line of the kernel and by the
 * overblank program on its own lines; "hit" plays on the TIA's channel 1 (music_tia).
 *
 * The ARM feeds the 6507 every instruction of the visible lines through vcsLib; during the
 * overblank the 6507 runs VSYNC and the blank lines by itself and the ARM runs the game.
 *
 * Build: F7 (runs make). Run: F5. This template is CC0: use it as you like.
 */
#include "vcsLib.h"
#include "game.h"
#include "menu.h"
#include "screen.h"
#include "theme.music.h"

#define DEFAULT_SYSTEM {{MENU_SYSTEM}}   /* the project's TV standard, when the cartridge says nothing */

static int lines = 199;          /* visible lines: 262 - 63 of overblank (NTSC, PAL60), 312 - 63 (PAL) */
static int short_hmove;          /* the late HMOVE moves one pixel less (HMOVE: SHORT) */
static uint8_t swcha = 0xFF, swchb = 0xFF;  /* joysticks and console switches, read after the kernel
                                            (0xFF: nothing pressed until the first frame reads them) */
static uint8_t inpt4 = 0x80;     /* fire button 0 (bit 7: 0 pressed; released until read) */
static const uint8_t *sample;    /* the next line's sample, in music_level */

static Menu menu;
static Game game;
static TitleScreen title;
static GameScreen screen;

/* ---- The 6507 --------------------------------------------------------------------------- */

/* Starts the 6507: back to $1000, TIA and RIOT RAM cleared (AUDC0 = 0 too: AUDV0 is then
 * a plain level), then the overblank program. */
static RAM_FUNC void start_6507(void)
{
    vcsJmp3();
    vcsLda2(0);
    for (int i = 0; i < 256; i++)
        vcsSta3((uint8_t)i);
    vcsCopyOverblankToRiotRam();
    /* The overblank program writes a sample per line to $2D, which does nothing: its
     * "sta $2d" (at $86-$87) becomes "sta AUDV0". Only if it is that program: another
     * cartridge's may differ, and changing another byte would break it. */
    if (vcsRead4(0x86) == 0x85 && vcsRead4(0x87) == 0x2D) vcsWrite5(0x87, AUDV0);
    vcsStartOverblank();          /* nothing long before this: the 6507 would run out of code */
}

/* Back to the menu: the 6507 runs nops from $1000, as the menu left it. */
static RAM_FUNC void leave(void)
{
    vcsEndOverblank();
    vcsWrite5(VBLANK, 2);
    vcsWrite5(AUDV0, 0);
    vcsWrite5(AUDV1, 0);
    vcsJmp3();
    vcsNop2n(1024);
}

/* ---- Lines every kernel uses -----------------------------------------------------------
 * Between two vcsLib calls the 6507 is already waiting for its next instruction, so these
 * only copy values worked out before; the cycle counts in the comments are where each
 * write ends, from the line's start. Every line writes its sample to AUDV0. */

/* A line with nothing new on it. */
static RAM_K void blank_line(void)
{
    vcsSta3(WSYNC);
    vcsWrite5(AUDV0, *sample++);
    vcsJmp3();                    /* once a line: the 6507's PC must stay in its 4 KB */
}

/* Where an object goes: RESxx after "nops" nops puts a player at 6 x nops - 39 (the write
 * ends at cycle 8 + 2 x nops, and x = 3 x cycle - 63); a missile lands a pixel to the left
 * of a player, so missiles are placed at x + 1. Before cycle 24 it would still be in the
 * horizontal blank; after cycle 73 (x 152) the next WSYNC would fall on the next line. The
 * early HMOVE (at the start of a line) takes it back to x: HMxx > 0 moves it left. Worked
 * out in the overblank: between two vcsLib calls there is no time for a division. */
typedef struct {
    uint16_t nops;
    uint8_t fine;               /* HMxx */
} Place;

static Place places[4];          /* the objects' places at the top of the frame */

static void place(Place *object, int x)
{
    int nops = (x + 39 + 5) / 6;  /* the first place at x or to its right */
    if (nops < 8) nops = 8;
    object->nops = (uint16_t)nops;
    object->fine = (uint8_t)((6 * nops - 39 - x) << 4);
}

static RAM_K void position_line(uint8_t reset, const Place *object)
{
    vcsSta3(WSYNC);
    vcsWrite5(AUDV0, *sample++);
    vcsNop2n(object->nops);
    vcsSta3(reset);
}

/* The overblank program's 62 samples, as 31 bytes at $E0-$FE, on the first lines after it
 * (eight a line: AUDV0 and eight writes take 45 cycles). */
static RAM_K void overblank_samples(int line)
{
    for (int i = 8 * line; i < 8 * line + 8 && i < 31; i++)
        vcsWrite5((uint8_t)(0xE0 + i), music_nibbles[i]);
}

/* The last line: the beam off, the TIA's channel 1 for the "hit" effect, and the inputs. */
static RAM_K void last_line(void)
{
    vcsSta3(WSYNC);
    vcsWrite5(AUDV0, *sample++);
    vcsWrite5(VBLANK, 2);
    vcsWrite5(AUDC1, music_tia[0]);
    vcsWrite5(AUDF1, music_tia[1]);
    vcsWrite5(AUDV1, music_tia[2]);
    swcha = vcsRead4(SWCHA);
    swchb = vcsRead4(SWCHB);
    inpt4 = vcsRead4(INPT4);
    vcsStartOverblank();
}

/* ---- The title screen ------------------------------------------------------------------ */

#define TITLE_P0_X       56       /* 48 pixels of text: players 0 and 1 at 56 and 64, three */
#define TITLE_P1_X       64       /* copies 16 pixels apart each (NUSIZ = 3) */
#define CART_LEFT_SIDE   36       /* the label's sides: missiles 0 and 1, 4 pixels wide */
#define CART_RIGHT_SIDE  116

/* A line of 48 pixels: with VDELP0 and VDELP1 on, each player shows what its register had
 * when the other one was last written. The first three columns go early; the last three
 * just as the beam reaches them (cycles 45, 48, 51 and 54), from A, X and Y. The 48-pixel
 * technique of Activision's six-digit scores. */
static RAM_K void line48(const uint8_t *columns)
{
    vcsSta3(WSYNC);
    vcsWrite5(AUDV0, *sample++);  /* 5 */
    vcsWrite5(GRP0, columns[0]);  /* 10 */
    vcsWrite5(GRP1, columns[1]);  /* 15 */
    vcsWrite5(GRP0, columns[2]);  /* 20 */
    vcsLda2(columns[3]);          /* 22 */
    vcsLdx2(columns[4]);          /* 24 */
    vcsLdy2(columns[5]);          /* 26 */
    vcsJmp3();                    /* 29 */
    vcsNop2n(5);                  /* 39 */
    vcsJmp3();                    /* 42 */
    vcsSta3(GRP1);                /* 45: column 4, column 3 shows */
    vcsStx3(GRP0);                /* 48: column 5, column 4 shows */
    vcsSty3(GRP1);                /* 51: column 6, column 5 shows */
    vcsSta3(GRP0);                /* 54: column 6 shows */
    vcsJmp3();
}

static RAM_K void no_players(void)
{
    vcsSta3(WSYNC);
    vcsWrite5(AUDV0, *sample++);
    vcsWrite5(GRP0, 0);
    vcsWrite5(GRP1, 0);
    vcsWrite5(GRP0, 0);           /* the delayed GRP0 too */
    vcsJmp3();
}

/* A row of the cartridge's line: an asymmetric playfield, the right half's registers
 * written again as the beam passes the middle (PF0 from cycle 28 to 49, PF1 from 39 to 54,
 * PF2 from 50 to 65). */
static RAM_K void cart_line(int row, int sides)
{
    vcsSta3(WSYNC);
    vcsWrite5(COLUPF, title.cart_colour[row]);  /* 5 */
    vcsWrite5(PF0, cart_pf0[row]);              /* 10 */
    vcsWrite5(PF1, cart_pf1[row]);              /* 15 */
    vcsWrite5(PF2, cart_pf2[row]);              /* 20 */
    vcsWrite5(AUDV0, *sample++);                /* 25 */
    vcsWrite5(PF0, cart_pf0r[row]);             /* 30 */
    vcsNop2n(3);                                /* 36 */
    vcsWrite5(PF1, cart_pf1r[row]);             /* 41 */
    vcsNop2n(2);                                /* 45 */
    vcsWrite5(PF2, cart_pf2r[row]);             /* 50 */
    if (sides) {                                /* the label's sides from the next line on */
        vcsWrite5(ENAM0, 2);                    /* 55 */
        vcsWrite5(ENAM1, 2);                    /* 60 */
    }
    vcsJmp3();
}

/* The title screen: 10 lines to set it up (the objects' places), VCSIDE's cartridge (90),
 * THE GAME (14) and the menu (10 lines a text), centred on PAL's 50 more lines. */
static RAM_FUNC void title_kernel(void)
{
    int line = 0, top = (lines - 199) / 2;
    sample = music_level;
    vcsEndOverblank();
    vcsSta3(WSYNC);
    vcsWrite5(AUDV0, *sample++);
    vcsWrite5(NUSIZ0, 0x20);      /* the missiles 4 pixels wide, one copy */
    vcsWrite5(NUSIZ1, 0x20);
    vcsWrite5(VDELP0, 1);
    vcsWrite5(VDELP1, 1);
    vcsWrite5(REFP0, 0);
    vcsWrite5(CTRLPF, CART_CTRLPF);
    vcsWrite5(COLUP0, title.sides_colour);
    vcsWrite5(COLUP1, title.sides_colour);
    vcsWrite5(HMP0, places[0].fine);
    vcsWrite5(HMP1, places[1].fine);
    vcsWrite5(HMM0, places[2].fine);
    vcsJmp3();
    vcsWrite5(HMM1, places[3].fine);
    position_line(RESP0, &places[0]);
    position_line(RESP1, &places[1]);
    position_line(RESM0, &places[2]);
    position_line(RESM1, &places[3]);
    vcsSta3(WSYNC);
    vcsSta3(HMOVE);
    vcsWrite5(AUDV0, *sample++);
    vcsJmp3();
    line = 6;
    for (int i = 0; i < 4; i++, line++) {
        vcsSta3(WSYNC);
        vcsWrite5(AUDV0, *sample++);
        overblank_samples(i);
        if (i == 0) vcsWrite5(VBLANK, 0);
        vcsJmp3();
    }
    for (; line < 10 + top; line++) blank_line();
    for (int row = 0; row < CART_ROWS; row++)
        for (int i = 0; i < CART_LINES; i++, line++) cart_line(row, row == 0 && i == CART_LINES - 1);
    vcsSta3(WSYNC);
    vcsWrite5(AUDV0, *sample++);
    vcsWrite5(ENAM0, 0);
    vcsWrite5(ENAM1, 0);
    vcsWrite5(PF0, 0);
    vcsWrite5(PF1, 0);
    vcsWrite5(PF2, 0);
    vcsWrite5(COLUP0, title.title_colour);
    vcsWrite5(COLUP1, title.title_colour);
    vcsWrite5(NUSIZ0, 0x03);      /* the players for the texts: three close copies each */
    vcsWrite5(NUSIZ1, 0x03);
    vcsJmp3();
    line++;
    for (int i = 1; i < 8; i++, line++) blank_line();
    for (int i = 0; i < TITLE_LINES; i++, line++) line48(screen_the_game[i / 2]);
    no_players();
    line++;
    for (int i = 1; i < 10; i++, line++) blank_line();
    for (int text = 0; text < title.texts; text++) {
        vcsSta3(WSYNC);
        vcsWrite5(AUDV0, *sample++);
        vcsWrite5(COLUP0, title.text_colour[text]);
        vcsWrite5(COLUP1, title.text_colour[text]);
        vcsJmp3();
        line++;
        for (int i = 0; i < TEXT_LINES; i++, line++) line48(title.text[text][i]);
        no_players();
        line++;
        blank_line();
        line++;
    }
    for (; line < lines - 1; line++) blank_line();
    last_line();
}

/* ---- The game -------------------------------------------------------------------------- */

/* Player 1 moves to the lower enemy between two pairs of the room. The first line of the
 * band pair sets HMP1; its second line clears GRP1 and hits RESP1 at the right cycle; the
 * next pair's first line ends with an HMOVE at cycle 73, which leaves no black comb on the
 * left (the HMOVE at the start of a line does). That late HMOVE moves an object 8 pixels
 * further left than HMxx says, or 7 on the consoles with the short HMOVE (2600s made in
 * China from 1989 on, the late Jr. and every PAL 7800: ZackAttack's measurements and
 * Eckhard Stolberg's note, AtariAge 2018), which the menu's HMOVE: SHORT makes up for.
 * The other objects get HMxx = "still" for it. */
static int band_cycle;            /* where RESP1's write ends */
static uint8_t band_hmp1, still;

/* Worked out in the overblank: RESP1 can end at cycle 25 at the earliest (after the line's
 * four writes and a wait) and puts player 1 at 3 x cycle - 63, 0 to 14 pixels to the right
 * of x; the late HMOVE takes it back. */
static void band_timing(int x)
{
    int cycle = (x + 63 + 2) / 3;
    if (cycle < 25) cycle = 25;
    int left = 3 * cycle - 63 - x;            /* 0-14 pixels to the left */
    band_cycle = cycle;
    band_hmp1 = (uint8_t)(((left - 8 + short_hmove) & 0x0F) << 4);
}

/* A wait of cycles (2 or more) with nops and a jmp. */
static RAM_K void wait_cycles(int cycles)
{
    if (cycles & 1) {
        vcsJmp3();
        cycles -= 3;
    }
    if (cycles) vcsNop2n((uint16_t)(cycles / 2));
}

/* The game: the objects' places and the overblank's samples (9 lines), the score (12),
 * the room (176), centred on PAL's 50 more lines. */
static RAM_FUNC void game_kernel(void)
{
    int line, top = (lines - 199) / 2;
    sample = music_level;
    vcsEndOverblank();
    vcsSta3(WSYNC);
    vcsWrite5(AUDV0, *sample++);
    vcsWrite5(NUSIZ0, 0x20);      /* missile 0 four pixels wide, one copy of each player */
    vcsWrite5(NUSIZ1, 0);
    vcsWrite5(VDELP0, 0);         /* (the title's text uses them) */
    vcsWrite5(VDELP1, 0);
    vcsWrite5(REFP0, screen.refp0);
    vcsWrite5(HMP0, places[0].fine);
    vcsWrite5(HMP1, places[1].fine);
    vcsWrite5(HMM0, places[2].fine);
    vcsJmp3();
    position_line(RESP0, &places[0]);
    position_line(RESP1, &places[1]);
    position_line(RESM0, &places[2]);
    vcsSta3(WSYNC);
    vcsSta3(HMOVE);
    vcsWrite5(AUDV0, *sample++);
    vcsWrite5(CTRLPF, 0x02);      /* score mode: the left half in COLUP0, the right in COLUP1 */
    vcsWrite5(COLUP0, screen.score_colour);
    vcsWrite5(COLUP1, screen.lives_colour);
    vcsWrite5(COLUPF, screen.wall_colour);
    vcsJmp3();
    for (int i = 0; i < 4; i++) {
        vcsSta3(WSYNC);
        vcsWrite5(AUDV0, *sample++);
        overblank_samples(i);
        if (i == 0) {             /* for the late HMOVE: nothing moves but player 1 */
            vcsWrite5(HMP0, still);
            vcsWrite5(HMM0, still);
            vcsWrite5(VBLANK, 0);
        }
        vcsJmp3();
    }
    line = 9;
    for (; line < 9 + top; line++) blank_line();
    /* the score, two lines a row: PF1 on the left, and on the right (cycles 39 to 54) */
    blank_line();
    line++;
    for (int row = 0; row < SCORE_ROWS; row++)
        for (int i = 0; i < 2; i++, line++) {
            vcsSta3(WSYNC);
            vcsWrite5(AUDV0, *sample++);              /* 5 */
            vcsWrite5(PF1, screen.score_pf1[row]);    /* 10 */
            vcsJmp3();                                /* 13 */
            vcsNop2n(12);                             /* 37 */
            vcsWrite5(PF1, screen.lives_pf1[row]);    /* 42 */
            vcsJmp3();
        }
    vcsSta3(WSYNC);
    vcsWrite5(AUDV0, *sample++);
    vcsWrite5(PF1, 0);
    vcsWrite5(CTRLPF, ROOM_CTRLPF);
    vcsJmp3();
    line++;
    /* the room, a line pair at a time */
    int band = screen.band_pair, late = band >= 0 ? band + 1 : -1;
    for (int pair = 0; pair < ROOM_PAIRS; pair++, line += 2) {
        int row = 2 * pair / ROOM_LINES;
        vcsSta3(WSYNC);
        vcsWrite5(PF0, room_pf0[row]);                /* 5 */
        vcsWrite5(PF1, room_pf1[row]);                /* 10 */
        vcsWrite5(PF2, room_pf2[row]);                /* 15 */
        vcsWrite5(ENAM0, screen.enam0[pair]);         /* 20 */
        vcsWrite5(AUDV0, *sample++);                  /* 25 */
        if (pair == band) vcsWrite5(HMP1, band_hmp1); /* 30 */
        if (pair == late) {
            wait_cycles(70 - 25);
            vcsSta3(HMOVE);                           /* 73: the late HMOVE */
        } else {
            vcsJmp3();
        }
        vcsSta3(WSYNC);
        if (pair == band) {                           /* player 1 to the lower enemy */
            vcsWrite5(GRP1, 0);                       /* 5 */
            vcsWrite5(GRP0, screen.grp0[pair]);       /* 10 */
            vcsWrite5(COLUP0, screen.colup0[pair]);   /* 15 */
            vcsWrite5(AUDV0, *sample++);              /* 20 */
            wait_cycles(band_cycle - 3 - 20);
            vcsSta3(RESP1);                           /* band_cycle */
            if (band_cycle <= 70) vcsJmp3();          /* (later, the next WSYNC would miss the line) */
        } else {
            vcsWrite5(GRP0, screen.grp0[pair]);       /* 5 */
            vcsWrite5(COLUP0, screen.colup0[pair]);   /* 10 */
            vcsWrite5(GRP1, screen.grp1[pair]);       /* 15 */
            vcsWrite5(COLUP1, screen.colup1[pair]);   /* 20 */
            vcsWrite5(AUDV0, *sample++);              /* 25 */
            vcsJmp3();
        }
    }
    vcsSta3(WSYNC);
    vcsWrite5(PF0, 0);
    vcsWrite5(PF1, 0);
    vcsWrite5(PF2, 0);
    vcsWrite5(GRP0, 0);
    vcsWrite5(GRP1, 0);
    vcsWrite5(ENAM0, 0);
    vcsWrite5(AUDV0, *sample++);
    vcsJmp3();
    line++;
    for (; line < lines - 1; line++) blank_line();
    last_line();
}

/* ---- elf_main -------------------------------------------------------------------------- */

int elf_main(uint32_t *args)
{
    /* What the cartridge says (each loader fills these in its own way):
     * - args[MP_SYSTEM_TYPE]: 0 NTSC, 1 PAL, 2 PAL60 (3 and 4: a 7800's NTSC and PAL), from
     *   Stella's Display.Format, the PlusCart's TV menu option and Gopher2600's TV; measured
     *   at power on by the Chameleon Cart, which never says PAL60. 0 may also be a cartridge
     *   that does not fill it in, so only PAL and PAL60 are taken from it; NTSC comes from
     *   the project's TV standard. The menu's SYSTEM row starts there and can change it.
     * - args[MP_CLOCK_HZ]: the ARM's clock in Hz in Stella, Gopher2600 and the PlusCart, but
     *   timer ticks on the Chameleon Cart: not a way to tell the system, nor to time
     *   anything. The frame is timed by the 6507's lines; this game does not use it.
     * - args[MP_FEATURE_FLAGS], read with masks (the Chameleon passes 2 or 6):
     *   FF_MULTI_CART: started from a menu (PlusCart, UnoCart), so EXIT can go back to it
     *   by returning from elf_main (Stella stops with an error if it returns; the Chameleon
     *   loops). FF_CHAMELEON_CART: elf_main is called during an overblank. FF_REDUCED_HMOVE:
     *   the Chameleon found the short late HMOVE: the menu's HMOVE row starts at SHORT. */
    unsigned system_type = args[MP_SYSTEM_TYPE];
    uint32_t flags = args[MP_FEATURE_FLAGS];
    int system = system_type == ST_PAL_2600 || system_type == ST_PAL_7800 ? SYSTEM_PAL
               : system_type == ST_PAL60_2600 ? SYSTEM_PAL60 : DEFAULT_SYSTEM;
    int multicart = (flags & FF_MULTI_CART) != 0;
    if (flags & FF_CHAMELEON_CART) vcsEndOverblank();
    menu_init(&menu, system, 1, (flags & FF_REDUCED_HMOVE) != 0, multicart);
    music_init();
    start_6507();
    int playing = 0;
    uint8_t last_inpt4 = 0x80, last_swchb = 0xFF;
    for (;;) {
        /* Overblank: the game logic. The button counts when it goes down. */
        int fire = !(inpt4 & 0x80) && (last_inpt4 & 0x80);
        int reset = !(swchb & 0x01) && (last_swchb & 0x01);
        last_inpt4 = inpt4;
        last_swchb = swchb;
        if (!playing) {
            int chosen = menu_frame(&menu, swcha, fire);
            if (chosen == MENU_PLAY) {
                game_start(&game);
                playing = 1;
            } else if (chosen == MENU_EXIT) {
                leave();
                return 0;
            }
        } else {
            int events = game_frame(&game, swcha, fire);
            if (events & GAME_SHOT) music_effect(MUSIC_EFFECT_SHOT);
            if (events & GAME_HIT) music_effect(MUSIC_EFFECT_HIT);
            if (events & GAME_LOSE) music_effect(MUSIC_EFFECT_LOSE);
            if ((events & GAME_OVER) || reset) {     /* back to the title, on PLAY */
                playing = 0;
                menu.row = 0;
            }
        }
        short_hmove = menu.short_hmove;
        lines = menu.system == SYSTEM_PAL ? 312 - 63 : 262 - 63;
        if (playing) {
            screen_game(&screen, &game, menu.system);
            place(&places[0], screen.hero_x);
            place(&places[1], screen.enemy_x);
            place(&places[2], screen.shot_x + 1);
            still = (uint8_t)(0x80 + (short_hmove << 4));
            if (screen.band_pair >= 0) band_timing(screen.band_x);
        } else {
            screen_title(&title, &menu);
            place(&places[0], TITLE_P0_X);
            place(&places[1], TITLE_P1_X);
            place(&places[2], CART_LEFT_SIDE + 1);
            place(&places[3], CART_RIGHT_SIDE + 1);
        }
        /* the frame's music: samples for the kernel's lines and for the overblank's */
        music_frame(lines + MUSIC_OVERBLANK_LINES);
        if (playing) game_kernel();
        else title_kernel();
    }
}
