/*
 * {{NAME}} - the Atari 7800's screen: MARIA draws the same room as the TIA does on a 2600.
 * CC0: use it as you like.
 *
 * How a frame is made:
 *   - The console's RAM holds a display list list (DLL) with one zone per line of the room and
 *     a display list (DL_LINE) that every one of those zones shares: three objects in 160B
 *     (2 pixels a byte, 12 colours), 32 + 32 + 16 bytes = 160 pixels, whose graphics are at
 *     GFX, in the cartridge's space.
 *   - The ARM serves those 80 bytes on each line as MARIA reads them (vcsInjectDmaData, one
 *     call per object: between two objects MARIA reads the next header from RAM). Lines with
 *     the same picture share a buffer: the walls, the floor and each row of the hero.
 *   - The 6502 waits in a loop in RAM (LOOP). In each VBLANK it reads 19 bytes the ARM serves
 *     at REGS (the background, the 12 palette colours and the TIA's six sound registers) and
 *     stores them, then reads the joystick, the console buttons and the two fire buttons, which
 *     the ARM sees on the bus (vcsSnoopRead), in the same order.
 *   - The ARM runs the game in the VBLANK, then serves the next frame's lines: each call waits
 *     for MARIA's read, so the ARM and MARIA meet again on every line and in every VBLANK.
 *
 * Addresses: the Chameleon Cart only sees A0-A12, so nothing the 6502 or MARIA reads may look
 * like (modulo $2000) an address the ARM waits for. The lists and the loop are at $2300-$26FF
 * (never $1800-$1FFF: with A12 set the cartridge takes it for its own; never $2280: it looks
 * like SWCHA); the served bytes are at $5000 and $5100, with A12 set, because the cartridge lets
 * go of the bus when A12 goes low after a run.
 *
 * Checked in Test7800. On a real Chameleon Cart it needs a firmware with the 7800 mode, and
 * whether the ARM keeps up with MARIA on every line is only known on the cartridge.
 */
#include "vcsLib.h"
#include "game.h"
#include "screens.h"

/* MARIA and the 7800's inputs */
#define BACKGRND  0x20
#define MSTAT     0x28
#define DPPH      0x2C
#define DPPL      0x30
#define OFFSET    0x38
#define CTRL      0x3C
#define CTRL_OFF  0x68           /* DMA off, black border */
#define CTRL_ON   0x48           /* DMA on, black border, 160A/160B */
#define INPT0     0x08
#define INPT1     0x09
#define SWBCNT    0x0283

/* The console's RAM */
#define DLL       0x2300         /* up to 200 entries of 3 bytes: $2300-$2557 */
#define DL_EMPTY  0x2600         /* a list with no objects: the lines above and below the room */
#define DL_LINE   0x2602         /* the room's lines: three objects */
#define LOOP      0x2620         /* the 6502's loop */
/* Served by the ARM */
#define GFX       0x5000         /* a line: 80 bytes of 160B */
#define REGS      0x5100         /* 19 bytes for the loop */
#define CART(a)   ((uint16_t)((a) & 0x1FFF))  /* what the cartridge sees of an address */

#define LINE_BYTES 80
#define COLOURS    12            /* 160B: palettes 0-3, colours 1-3 */
#define N_REGS     19

/* Where the loop stores each served byte: BACKGRND, P0C1-P3C3, then the TIA's sound. */
static const uint8_t reg_address[N_REGS] = {
	BACKGRND, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x29, 0x2A, 0x2B, 0x2D, 0x2E, 0x2F,
	AUDC0, AUDF0, AUDV0, AUDC1, AUDF1, AUDV1};

static uint8_t colours[COLOURS];             /* palette colour of each slot (0-11) */
static int n_colours;
static uint8_t slot_floor, slot_wall;
static uint8_t regs[N_REGS];
static uint8_t band_line[LINE_BYTES], plain_line[LINE_BYTES], hero_lines[HERO_MAX_ROWS][LINE_BYTES];
static int top_lines, bottom_lines;          /* lines with DMA above and below the room */
static uint8_t swcha = 0xFF, swchb = 0xFF, inpt4 = 0x80, inpt0, inpt1;

/* lda #v / sta address, from the cartridge: back to $1000 every so often, so that the 6502
 * never fetches from $1800 on, where its RAM starts. */
static unsigned pokes;
static void poke(uint16_t address, uint8_t value)
{
	vcsWrite6(address, value);
	if (++pokes % 32 == 0) vcsJmp3();
}

/* DLL entries for a number of lines, in zones of up to height lines, all showing one list. */
static uint16_t zones(uint16_t at, int lines, int height, uint16_t list)
{
	while (lines > 0) {
		int h = lines < height ? lines : height;
		poke(at++, (uint8_t)(h - 1));        /* no DLI, no holey DMA, h lines */
		poke(at++, (uint8_t)(list >> 8));
		poke(at++, (uint8_t)list);
		lines -= h;
	}
	return at;
}

/* The slot (0-11) that shows a colour; colours not in game_colours show as the floor. */
static uint8_t slot_of(uint8_t colour)
{
	for (int i = 0; i < n_colours; i++)
		if (colours[i] == colour) return (uint8_t)i;
	return slot_floor;
}

/* 160 pixels (a slot each) into 80 bytes of 160B: a byte is two pixels, the left one's colour
 * (1-3) in bits 7-6 and its palette (0-3) in bits 3-2, the right one's in bits 5-4 and 1-0.
 * Slot s is colour s % 3 + 1 of palette s / 3 (the M0+ has no divide: a table). */
static const uint8_t slot_left[COLOURS] = {0x40, 0x80, 0xC0, 0x44, 0x84, 0xC4, 0x48, 0x88, 0xC8, 0x4C, 0x8C, 0xCC};
static void pack(const uint8_t *pixels, uint8_t *bytes)
{
	for (int i = 0; i < LINE_BYTES; i++)
		bytes[i] = (uint8_t)(slot_left[pixels[2 * i]] | slot_left[pixels[2 * i + 1]] >> 2);
}

static void room_pixels(uint8_t *pixels, int band)
{
	for (int x = 0; x < ROOM_WIDTH; x++)
		pixels[x] = band || x < WALL_PIXELS || x >= ROOM_WIDTH - WALL_PIXELS ? slot_wall : slot_floor;
}

/* The hero's rows in this frame's position, frame and direction. */
static void prepare_hero(void)
{
	uint8_t pixels[ROOM_WIDTH];
	for (int row = 0; row < game.hero_rows; row++) {
		room_pixels(pixels, 0);
		uint8_t bits = game.hero_gfx[row], slot = slot_of(game.hero_colour[row]);
		for (int i = 0; i < HERO_WIDTH; i++)
			if (bits & (game.facing_left ? 1 << i : 0x80 >> i)) pixels[game.hero_x + i] = slot;
		pack(pixels, hero_lines[row]);
	}
	for (int i = 0; i < 6; i++) regs[13 + i] = game.audio[i];
}

/* The frame's 192 lines, then the loop's 19 bytes and its reads of the controls. */
static RAM_FUNC void serve_frame(void)
{
	int hero_top = game.hero_y, hero_end = hero_top + game.hero_rows * LINES_PER_ROW;
	for (int y = 0; y < ROOM_LINES; y++) {
		const uint8_t *line = y < WALL_LINES || y >= ROOM_LINES - WALL_LINES ? band_line
		                      : y >= hero_top && y < hero_end ? hero_lines[(y - hero_top) / LINES_PER_ROW]
		                                                      : plain_line;
		vcsInjectDmaData(CART(GFX), 32, line);
		vcsInjectDmaData(CART(GFX + 32), 32, line + 32);
		vcsInjectDmaData(CART(GFX + 64), 16, line + 64);
	}
	for (int i = 0; i < N_REGS; i++) vcsInjectDmaData(CART(REGS + i), 1, &regs[i]);
	swcha = vcsSnoopRead(SWCHA);
	swchb = vcsSnoopRead(SWCHB);
	inpt4 = vcsSnoopRead(INPT4);
	inpt0 = vcsSnoopRead(INPT0);
	inpt1 = vcsSnoopRead(INPT1);
}

/* The 6502's loop: wait for the VBLANK, store the served bytes, read the controls. */
static void write_loop(void)
{
	uint16_t at = LOOP;
	static const uint8_t wait[] = {
		0x24, MSTAT, 0x30, 0xFC,              /* LOOP: bit MSTAT / bmi LOOP: until the lines start */
		0x24, MSTAT, 0x10, 0xFC};             /*       bit MSTAT / bpl *-2: until the VBLANK starts */
	for (unsigned i = 0; i < sizeof wait; i++) poke(at++, wait[i]);
	for (int i = 0; i < N_REGS; i++) {         /* lda REGS+i / sta register */
		poke(at++, 0xAD), poke(at++, (uint8_t)(REGS + i)), poke(at++, (uint8_t)((REGS + i) >> 8));
		poke(at++, 0x85), poke(at++, reg_address[i]);
	}
	static const uint8_t controls[] = {
		0xAD, SWCHA & 0xFF, SWCHA >> 8,       /* lda SWCHA */
		0xAD, SWCHB & 0xFF, SWCHB >> 8,       /* lda SWCHB */
		0xA5, INPT4, 0xA5, INPT0, 0xA5, INPT1, /* lda INPT4 / lda INPT0 / lda INPT1 */
		0x4C, LOOP & 0xFF, LOOP >> 8};        /* jmp LOOP */
	for (unsigned i = 0; i < sizeof controls; i++) poke(at++, controls[i]);
}

int maria_screen_run(uint32_t *args)
{
	int pal = args[MP_SYSTEM_TYPE] == ST_PAL_7800;
	/* MARIA reads lists on 243 lines (NTSC) or 293 (PAL) in Test7800; the room is centred
	 * where a TV shows it, and the DLL goes 8 lines further, so MARIA never reads past it. */
	top_lines = pal ? 51 : 25;
	bottom_lines = (pal ? 293 : 243) + 8 - top_lines - ROOM_LINES;

	/* The colours: one slot per colour the game shows; slot 0 is the floor. */
	n_colours = game_colours(colours, COLOURS);
	slot_floor = slot_of(FLOOR_COLOUR);
	slot_wall = slot_of(WALL_COLOUR);
	regs[0] = 0x00;                            /* BACKGRND: black around the room */
	for (int i = 0; i < COLOURS; i++) regs[1 + i] = i < n_colours ? colours[i] : 0;
	uint8_t pixels[ROOM_WIDTH];
	room_pixels(pixels, 1);
	pack(pixels, band_line);
	room_pixels(pixels, 0);
	pack(pixels, plain_line);

	/* The 6502 runs what the ARM gives it, as on a 2600: DMA off, then the lists, the loop and
	 * MARIA's registers. */
	vcsJmp3();
	vcsWrite5(CTRL, CTRL_OFF);
	vcsWrite5(OFFSET, 0);
	vcsWrite6(SWCHB, 0x00);                    /* two-button joysticks: SWCHB bit 2 an output at 0 */
	vcsWrite6(SWBCNT, 0x04);
	vcsJmp3();
	uint16_t at = zones(DLL, top_lines, 16, DL_EMPTY);
	at = zones(at, ROOM_LINES, 1, DL_LINE);
	zones(at, bottom_lines, 16, DL_EMPTY);
	static const uint8_t lists[] = {
		0x00, 0x00,                                                  /* DL_EMPTY: the end */
		/* DL_LINE: three 5-byte headers (address low, $C0 = write mode 160B, address high,
		 * palette 0-3 and width, x), then the end */
		(uint8_t)GFX, 0xC0, GFX >> 8, 0x00, 0,                       /* 32 bytes at x = 0 */
		(uint8_t)(GFX + 32), 0xC0, (GFX + 32) >> 8, 0x00, 64,        /* 32 bytes at x = 64 */
		(uint8_t)(GFX + 64), 0xC0, (GFX + 64) >> 8, -16 & 0x1F, 128, /* 16 bytes at x = 128 */
		0x00, 0x00};
	for (unsigned i = 0; i < sizeof lists; i++) poke((uint16_t)(DL_EMPTY + i), lists[i]);
	write_loop();
	vcsJmp3();
	for (int i = 0; i < 1 + COLOURS; i++) vcsWrite5(reg_address[i], regs[i]);
	vcsWrite5(DPPH, DLL >> 8);
	vcsWrite5(DPPL, DLL & 0xFF);
	/* DMA on at the start of a VBLANK (MSTAT bit 7 goes to 1): MARIA takes the DLL at its end. */
	while (vcsRead4(MSTAT) & 0x80) vcsJmp3();
	while (!(vcsRead4(MSTAT) & 0x80)) vcsJmp3();
	vcsWrite5(CTRL, CTRL_ON);
	vcsJmpToRam3(LOOP);

	/* From here the 6502 never comes back to the cartridge. In Test7800 this first snoop only
	 * ends the byte streaming of the calls above; on the cartridge it sees the loop read MSTAT. */
	vcsSnoopRead(MSTAT);
	prepare_hero();
	for (;;) {
		serve_frame();
		/* VBLANK: the game */
		GameInput input = {0};
		input.right = !(swcha & 0x80);
		input.left = !(swcha & 0x40);
		input.down = !(swcha & 0x20);
		input.up = !(swcha & 0x10);
		/* the 2600 joystick's button (INPT4, 0 = pressed) or either of the 7800 joystick's
		 * (INPT0 right, INPT1 left: 1 = pressed) */
		input.fire = !(inpt4 & 0x80) || (inpt0 & 0x80) || (inpt1 & 0x80);
		input.pause = !(swchb & 0x08);
		game_frame(&input);
		prepare_hero();
	}
}
