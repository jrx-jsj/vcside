/*
 * {{NAME}} - the Atari 7800's side of VCSIDE The Game: the same game (game.c, menu.c) drawn by
 * MARIA, with wood on the walls, tiled floors, new sprites and a new cartridge on the title
 * screen (art7800.c), and the same sampled music. CC0: use it as you like.
 *
 * How a frame is made:
 *   - The 7800's RAM holds a display list list (DLL) with zones of 16 lines in the picture,
 *     every one showing the same display list (DL_LINE): three objects in 160B (2 pixels a
 *     byte, 12 colours and black), 32 + 32 + 16 bytes = 160 pixels, read from GFX, in the
 *     cartridge's space; line k of a zone of h lines from GFX + (h - 1 - k) * 256 (MARIA's
 *     OFFSET counts down). Tall zones leave the 6502 time on each line: MARIA reads the DLL
 *     once a zone, not once a line. The ARM draws each line in a buffer just before MARIA
 *     reads it, and serves it as MARIA reads it (vcsInjectDmaData, one call per object: MARIA
 *     reads the next header from RAM between two objects).
 *   - The 6502 runs a loop in RAM (code[]): on each line of the display it reads two bytes
 *     the ARM serves after MARIA's reads, the line's sample (to AUDV0, with AUDC0 = 0: the
 *     2600's sampled music) and one it keeps for the VBLANK, whose lines play those. On the
 *     VBLANK's first line it reads the joystick, the console buttons and the three fire
 *     buttons, which the ARM sees on the bus (vcsSnoopRead), and 16 served bytes: the 12
 *     palette colours, the background and the TIA's channel 1 (the "hit" effect).
 *   - The ARM runs the game in the VBLANK.
 * The two samples a line, the loop that counts the display's lines, the zones of 16 lines and
 * waiting for MARIA's first read of the DLL come from La Abadía del Crimen 2600's 7800 screen
 * (screen7800.c), as does the way lines are counted at power on.
 *
 * Addresses: the Chameleon Cart only sees A0-A12, so nothing the 6502 or MARIA reads may look
 * like (modulo $2000) an address the ARM waits for. The lists and the loop are at $2100-$26FF
 * (never $1800-$1FFF: with A12 set the cartridge takes it for its own; never $2280: it looks
 * like SWCHA); the served bytes are at $5000-$5F4F (the lines), $5F80 and $5FF0, with A12 set,
 * because the cartridge lets go of the bus when A12 goes low after a run.
 *
 * Checked in Test7800, which runs the ARM at 70 MHz: the ARM has the rest of a line, after the
 * 6502 reads its samples, to draw the next one (draw_line), so lines are only word copies and
 * the sprites' pixels. On a real Chameleon Cart that time is to be measured.
 */
#include "vcsLib.h"
#include "game.h"
#include "menu.h"
#include "screen.h"
#include "art7800.h"
#include "screens.h"

/* MARIA and the 7800's inputs */
#define BACKGRND  0x20
#define M_WSYNC   0x24
#define MSTAT     0x28
#define DPPH      0x2C
#define DPPL      0x30
#define OFFSET    0x38
#define CTRL      0x3C
#define CTRL_OFF  0x7F           /* DMA off */
#define CTRL_ON   0x48           /* DMA on, black border, 160A/160B */
#define INPT0     0x08
#define INPT1     0x09
#define SWBCNT    0x0283

/* The 7800's RAM */
#define BUF       0x2100         /* the samples for the VBLANK (a page) */
#define DLL       0x2300         /* up to 320 lines of zones: $2300-$24FF */
#define DL_EMPTY  0x2600         /* a list with no objects: the lines above and below the picture */
#define DL_LINE   0x2602         /* the picture's lines: three objects */
#define CODE      0x2620         /* the 6502's loop */
#define PASSES    0x80           /* (zero page) the loop's passes of 256 lines still to go */
/* Served by the ARM */
#define GFX       0x5000         /* a line: 80 bytes of 160B, in one of 16 pages */
#define REGS      0x5F80         /* 16 bytes for the VBLANK */
#define ZONE      16             /* the picture's zones: 16 lines */
#define SMP       0x5FF0         /* the line's two samples */
#define CART(a)   ((uint16_t)((a) & 0x1FFF))  /* what the cartridge sees of an address */

#define LINE_BYTES 80
#define N_REGS     16
#define PICTURE    188           /* the picture's lines: the 2600's from its score to the room's end */
#define SCORE_TOP  0             /* the score: 5 rows of two lines */
#define ROOM_TOP   11            /* the room: ROOM_ROWS x ROOM_LINES lines */
#define CART_X     40            /* the title screen: the cartridge, */
#define THE_GAME_Y 98            /* THE GAME (7 rows of two lines) */
#define MENU_Y     122           /* and the menu's texts, one every 10 lines */
#define TEXT_X     56            /* 48 pixels of text, as the 2600's players draw them */
_Static_assert(CART_X % 8 == 0 && TEXT_X % 8 == 0, "the cartridge and the texts start on a word (8 pixels)");

static Menu menu;
static Game game;
static TitleScreen title;
static GameScreen scores;        /* the 2600's tables: only the score's and lives' digits */
static int playing, pal_colours;
static int n_dma, n_vbl, top;    /* lines with DMA and of the VBLANK, the picture's first line */
static uint8_t regs[N_REGS];
static uint8_t swcha = 0xFF, swchb = 0xFF, inpt4 = 0x80, inpt0, inpt1;

/* ---- 160B ----------------------------------------------------------------------------------
 * A byte is two pixels: the left one's colour (1-3) in bits 7-6 and its palette (0-3) in bits
 * 3-2, the right one's in bits 5-4 and 1-0; colour 0 is the background (black). A slot is
 * 0 (black) or 1-12: colour (slot - 1) % 3 + 1 of palette (slot - 1) / 3. */
#define CLEAR 0xFF               /* transparent, in the pictures */
static const uint8_t left_code[ART_COLOURS + 1] = {
	0x00, 0x40, 0x80, 0xC0, 0x44, 0x84, 0xC4, 0x48, 0x88, 0xC8, 0x4C, 0x8C, 0xCC};

static uint8_t pair_byte(uint8_t left, uint8_t right)
{
	return (uint8_t)(left_code[left == CLEAR ? 0 : left] | left_code[right == CLEAR ? 0 : right] >> 2);
}

static RAM_K void put(uint8_t *line, int x, uint8_t slot)
{
	if (x < 0 || x >= 160 || slot == CLEAR) return;
	uint8_t *byte = &line[x >> 1];
	if (x & 1) *byte = (uint8_t)((*byte & 0xCC) | left_code[slot] >> 2);
	else *byte = (uint8_t)((*byte & 0x33) | left_code[slot]);
}

/* A picture's letter: its slot (1-12 from letters), 0 for 'k', CLEAR for anything else. */
static uint8_t slot_of_letter(const char *letters, char letter)
{
	if (letter == 'k') return 0;
	for (int i = 0; i < ART_COLOURS; i++)
		if (letters[i] == letter) return (uint8_t)(i + 1);
	return CLEAR;
}

static uint8_t slot_of_colour(const uint8_t *colours, uint8_t colour)
{
	for (int i = 0; i < ART_COLOURS; i++)
		if (colours[i] == colour) return (uint8_t)(i + 1);
	return ART_COLOURS - 1;      /* (a colour the palette lacks) */
}

/* A PAL 7800's colour for an NTSC one: the same luminance, the hue one step on (the greys
 * stay), as La Abadía del Crimen 2600 measured on Test7800's palettes. */
static uint8_t colour(uint8_t ntsc)
{
	return pal_colours && (ntsc & 0xF0) ? (uint8_t)(ntsc + 0x10) : ntsc;
}

/* ---- The pictures, in 160B or as slots ----------------------------------------------------
 * Lines are built a 32-bit word (8 pixels) at a time where they can be: the ARM has only the
 * rest of a line to draw the next one. */

#define LINE_WORDS (LINE_BYTES / 4)
static uint32_t wood[ART_WOOD_LINES][4];           /* 32 pixels, repeated along a wall */
static uint32_t floor_tiles[ART_FLOOR_LINES][4];   /* 32 pixels, repeated along the floor */
static uint32_t wall_mask[ROOM_ROWS][LINE_WORDS];  /* each row's walls: $FF bytes where they are */
static uint8_t hero[HERO_FRAMES][ART_HERO_LINES][ART_SPRITE_WIDTH];
static uint8_t enemy[ENEMY_FRAMES][ART_ENEMY_LINES][ART_SPRITE_WIDTH];
static uint32_t cart[ART_CART_LINES][ART_CART_WIDTH / 8];
static uint8_t slot_white, slot_yellow, slot_enemy, slot_other_enemy;
static uint8_t slot_title, slot_shadow;

/* Rows of letters in 160B, each line starting shift bytes (two pixels each) along, as it repeats. */
static void pack_rows(const char *const *rows, int lines, int width, const char *letters, int shift, uint8_t *out)
{
	int bytes = width / 2;
	for (int y = 0; y < lines; y++)
		for (int b = 0; b < bytes; b++) {
			const char *row = rows[y];
			int x = 2 * ((b + shift) % bytes);
			uint8_t left = row ? slot_of_letter(letters, row[x]) : CLEAR;
			uint8_t right = row ? slot_of_letter(letters, row[x + 1]) : CLEAR;
			*out++ = pair_byte(left, right);
		}
}

static void sprite_slots(const char *const *rows, int lines, uint8_t (*out)[ART_SPRITE_WIDTH])
{
	for (int y = 0; y < lines; y++)
		for (int x = 0; x < ART_SPRITE_WIDTH; x++)
			out[y][x] = rows[y] ? slot_of_letter(art_game_letters, rows[y][x]) : CLEAR;
}

/* Is the room's playfield on at column (0-39) and row? As game.c reads it. */
static int room_wall(int column, int row)
{
	if (column >= 20) column = (ROOM_CTRLPF & 1) ? 39 - column : column - 20;
	if (column < 4) return (room_pf0[row] >> (4 + column)) & 1;
	if (column < 12) return (room_pf1[row] >> (11 - column)) & 1;
	return (room_pf2[row] >> (column - 12)) & 1;
}

static void prepare_pictures(void)
{
	pack_rows(art_wood, ART_WOOD_LINES, ART_WOOD_WIDTH, art_game_letters, 0, (uint8_t *)wood);
	pack_rows(art_floor, ART_FLOOR_LINES, ART_FLOOR_WIDTH, art_game_letters, 2, (uint8_t *)floor_tiles);
	pack_rows(art_cart, ART_CART_LINES, ART_CART_WIDTH, art_title_letters, 0, (uint8_t *)cart);
	for (int row = 0; row < ROOM_ROWS; row++) {
		uint8_t *mask = (uint8_t *)wall_mask[row];
		for (int column = 0; column < 40; column++)
			mask[2 * column] = mask[2 * column + 1] = room_wall(column, row) ? 0xFF : 0x00;
	}
	for (int frame = 0; frame < HERO_FRAMES; frame++) sprite_slots(art_hero[frame], ART_HERO_LINES, hero[frame]);
	for (int frame = 0; frame < ENEMY_FRAMES; frame++) sprite_slots(art_enemy[frame], ART_ENEMY_LINES, enemy[frame]);
	slot_white = slot_of_letter(art_game_letters, 'W');
	slot_yellow = slot_of_letter(art_game_letters, 'Y');
	slot_enemy = slot_of_letter(art_game_letters, 'E');
	slot_other_enemy = slot_of_letter(art_game_letters, 'B');
	slot_shadow = slot_of_letter(art_title_letters, '1');
}

/* ---- What a frame shows, worked out in the VBLANK ----------------------------------------- */

typedef struct {
	int x, top, lines;           /* in the room's lines */
	const uint8_t (*rows)[ART_SPRITE_WIDTH];
	int mirror;
	uint8_t from, to;            /* a slot drawn as another (the second enemy's colour) */
} Sprite;

static Sprite sprites[1 + ENEMIES];
static int n_sprites, shot_x, shot_top;            /* shot_top < 0: no shot */
static vcsgfx_player hero_animation;

/* The frame an animation shows after a number of video frames (it loops), as screen.c. */
static int animation_frame(const vcsgfx_animation *animation, unsigned frames)
{
	unsigned length = 0;
	for (int i = 0; i < animation->count; i++) length += animation->steps[i].ticks;
	if (!length) return 0;
	frames %= length;
	for (int i = 0; i < animation->count; i++) {
		if (frames < animation->steps[i].ticks) return animation->steps[i].frame;
		frames -= animation->steps[i].ticks;
	}
	return 0;
}

static void prepare_score(void);

/* The game's sprites, where game.c's boxes are: a sprite at pair y on the room's lines
 * 2y + 1 on, the shot on its pair's two lines. */
static void prepare_game(void)
{
	vcsgfx_play(&hero_animation, game.walking ? &hero_walk : &hero_idle);
	int hero_frame = vcsgfx_animate(&hero_animation), enemy_frame = animation_frame(&enemy_crawl, game.frame);
	n_sprites = 0;
	for (int i = 0; i < ENEMIES; i++) {
		const Enemy *e = &game.enemy[i];
		if (!e->alive) continue;
		sprites[n_sprites++] = (Sprite){e->x, 2 * e->y + 1, ART_ENEMY_LINES, enemy[enemy_frame], 0,
		                                slot_enemy, i ? slot_other_enemy : slot_enemy};
	}
	sprites[n_sprites++] = (Sprite){game.hero_x, 2 * game.hero_y + 1, ART_HERO_LINES, hero[hero_frame],
	                                game.facing_left, 0, 0};
	shot_top = game.shot_on ? 2 * game.shot_y : -1;
	shot_x = game.shot_x;
	prepare_score();
	regs[0] = 0x00;
	for (int i = 0; i < ART_COLOURS; i++) regs[1 + i] = colour(art_game_colours[i]);
}

/* ---- Texts and digits, drawn in 160B when they change ------------------------------------ */

#define TEXT_WORDS 7             /* 56 pixels from TEXT_X: a text (48) and its shadow (a pixel on) */
static uint32_t the_game_lines[TITLE_LINES + 1][TEXT_WORDS];
static uint32_t text_lines[MENU_TEXTS][TEXT_LINES + 1][TEXT_WORDS];
static uint8_t shown_text[MENU_TEXTS][TEXT_LINES][6], shown_slot[MENU_TEXTS], the_game_slot;
static uint32_t score_lines[SCORE_ROWS][LINE_WORDS];

static int bit48(const uint8_t *bits, int x)
{
	return bits && x >= 0 && x < 48 && (bits[x >> 3] & (0x80 >> (x & 7)));
}

/* A line of a 48-pixel text (six bytes, bit 7 first; 0: none) with the shadow of the line
 * above's a pixel down and right. */
static void text_line(uint32_t *out, const uint8_t *bits, const uint8_t *above, uint8_t slot)
{
	uint8_t *bytes = (uint8_t *)out;
	for (int b = 0; b < 4 * TEXT_WORDS; b++) {
		uint8_t pair[2];
		for (int k = 0; k < 2; k++) {
			int x = 2 * b + k;
			pair[k] = bit48(bits, x) ? slot : bit48(above, x - 1) ? slot_shadow : 0;
		}
		bytes[b] = pair_byte(pair[0], pair[1]);
	}
}

static void text_lines_of(uint32_t (*out)[TEXT_WORDS], const uint8_t (*rows)[6], int rows_count, int scale, uint8_t slot)
{
	int lines = rows_count * scale;
	for (int t = 0; t <= lines; t++)
		text_line(out[t], t < lines ? rows[t / scale] : 0, t > 0 ? rows[(t - 1) / scale] : 0, slot);
}

static int same_bytes(const uint8_t *a, const uint8_t *b, int n)
{
	while (n--) if (*a++ != *b++) return 0;
	return 1;
}

/* The title screen: the menu's texts from screen.c (their colours as NTSC values); only the
 * ones that changed are drawn again. */
static void prepare_title(void)
{
	Menu ntsc = menu;
	ntsc.system = SYSTEM_NTSC;
	screen_title(&title, &ntsc);                   /* the colours, as NTSC values */
	uint8_t text_colour[MENU_TEXTS];
	for (int i = 0; i < title.texts; i++) text_colour[i] = title.text_colour[i];
	slot_title = slot_of_colour(art_title_colours, title.title_colour);
	screen_title(&title, &menu);                   /* the texts: SYSTEM says NTSC or PAL */
	if (slot_title != the_game_slot) {
		the_game_slot = slot_title;
		text_lines_of(the_game_lines, screen_the_game, TITLE_LINES / 2, 2, slot_title);
	}
	for (int i = 0; i < title.texts; i++) {
		uint8_t slot = slot_of_colour(art_title_colours, text_colour[i]);
		if (slot == shown_slot[i] && same_bytes(&title.text[i][0][0], &shown_text[i][0][0], sizeof shown_text[i])) continue;
		shown_slot[i] = slot;
		for (int r = 0; r < TEXT_LINES; r++)
			for (int b = 0; b < 6; b++) shown_text[i][r][b] = title.text[i][r][b];
		text_lines_of(text_lines[i], (const uint8_t (*)[6])title.text[i], TEXT_LINES, 1, slot);
	}
	regs[0] = 0x00;
	for (int i = 0; i < ART_COLOURS; i++) regs[1 + i] = colour(art_title_colours[i]);
}

/* The score's rows: PF1's bits, 4 pixels each, as the 2600 shows them in score mode (the
 * score from x = 16, the lives from x = 96). */
static void prepare_score(void)
{
	screen_game(&scores, &game, SYSTEM_NTSC);
	for (int row = 0; row < SCORE_ROWS; row++) {
		uint8_t *bytes = (uint8_t *)score_lines[row];
		for (int i = 0; i < LINE_BYTES; i++) bytes[i] = 0;
		for (int bit = 0; bit < 8; bit++) {
			uint8_t white = pair_byte(slot_white, slot_white), yellow = pair_byte(slot_yellow, slot_yellow);
			if (scores.score_pf1[row] & (0x80 >> bit)) bytes[8 + 2 * bit] = bytes[9 + 2 * bit] = white;
			if (scores.lives_pf1[row] & (0x80 >> bit)) bytes[48 + 2 * bit] = bytes[49 + 2 * bit] = yellow;
		}
	}
}

/* ---- Drawing a line ------------------------------------------------------------------------
 * Called for each line of the picture just before MARIA reads it, in the rest of the line
 * before: only word copies and the sprites' pixels, from what the VBLANK worked out. */

static uint32_t line[LINE_WORDS];

static RAM_K void draw_title_line(int y)
{
	for (int i = 0; i < LINE_WORDS; i++) line[i] = 0;
	if (y < ART_CART_LINES) {
		for (int i = 0; i < ART_CART_WIDTH / 8; i++) line[CART_X / 8 + i] = cart[y][i];
		return;
	}
	const uint32_t *text = 0;
	int t = y - THE_GAME_Y;
	if (t >= 0 && t <= TITLE_LINES) text = the_game_lines[t];
	t = y - MENU_Y;
	if (t >= 0 && t < 10 * title.texts) {
		int row = t % 10;
		if (row <= TEXT_LINES) text = text_lines[t / 10][row];
	}
	if (text) for (int i = 0; i < TEXT_WORDS; i++) line[TEXT_X / 8 + i] = text[i];
}

/* The room's line: wood where the playfield is (each row's planks further along), tiles
 * elsewhere, a black line under a wall as its shadow; then the sprites and the shot. */
static RAM_K void draw_room_line(int y)
{
	int row = y >> 3, in_row = y & 7;
	const uint32_t *wood_line = wood[in_row], *floor_line = floor_tiles[(y + 24) & 31], *mask = wall_mask[row];
	for (int i = 0; i < LINE_WORDS; i++)
		line[i] = (wood_line[(i + row) & 3] & mask[i]) | (floor_line[i & 3] & ~mask[i]);
	if (in_row == 0 && row) {
		const uint32_t *above = wall_mask[row - 1];
		for (int i = 0; i < LINE_WORDS; i++) line[i] &= ~(above[i] & ~mask[i]);
	}
	uint8_t *bytes = (uint8_t *)line;
	for (int i = 0; i < n_sprites; i++) {
		const Sprite *s = &sprites[i];
		int r = y - s->top;
		if (r < 0 || r >= s->lines) continue;
		const uint8_t *pixels = s->rows[r];
		for (int x = 0; x < ART_SPRITE_WIDTH; x++) {
			uint8_t slot = pixels[s->mirror ? ART_SPRITE_WIDTH - 1 - x : x];
			if (slot == s->from && s->from) slot = s->to;
			put(bytes, s->x + x, slot);
		}
	}
	if (shot_top >= 0 && (y == shot_top || y == shot_top + 1))
		for (int x = 0; x < SHOT_WIDTH; x++) put(bytes, shot_x + x, slot_yellow);
}

static RAM_K void draw_line(int y)
{
	if (!playing) draw_title_line(y);
	else if (y >= ROOM_TOP) draw_room_line(y - ROOM_TOP);
	else if (y < SCORE_TOP + 2 * SCORE_ROWS) {
		const uint32_t *score = score_lines[(y - SCORE_TOP) >> 1];
		for (int i = 0; i < LINE_WORDS; i++) line[i] = score[i];
	} else for (int i = 0; i < LINE_WORDS; i++) line[i] = 0;
}

/* ---- Sound: the frame's samples -----------------------------------------------------------
 * music_frame gives a 2600 frame's samples: music_level for its first lines - 63, and
 * music_nibbles, two a byte, for the 63 of its overblank (the first two as their mean). Here
 * they are one list, a sample per line of the 7800's frame. */
#define SMP_MAX 320
static uint8_t smp[SMP_MAX], vbl[64];

static void prepare_samples(void)
{
	int lines = n_dma + n_vbl > 312 ? 312 : n_dma + n_vbl, kernel = lines - 63, n = 0;
	music_frame(lines);
	for (int i = 0; i < kernel; i++) smp[n++] = music_level[i];
	for (int i = 0; i < 31; i++) {
		smp[n++] = music_nibbles[i] & 0x0F;
		smp[n++] = music_nibbles[i] >> 4;
	}
	while (n < SMP_MAX) smp[n] = smp[n - 1], n++;
	/* The loop plays n_dma + n_vbl - 2 samples in the frame's lines: the last line of the
	 * display and the first of the VBLANK last two lines each, and get the mean of two. */
	smp[n_dma - 1] = (uint8_t)((smp[n_dma - 1] + smp[n_dma] + 1) >> 1);
	const uint8_t *v = smp + n_dma + 1;
	vbl[0] = (uint8_t)((v[0] + v[1] + 1) >> 1);
	for (int k = 1; k < n_vbl - 2 && k < 64; k++) vbl[k] = v[k + 1];
	regs[13] = music_tia[0];
	regs[14] = music_tia[1];
	regs[15] = music_tia[2];
}

/* ---- The frame on the bus -------------------------------------------------------------- */

/* From MARIA's first read of the DLL (as La Abadía del Crimen 2600 does, which keeps the ARM
 * in step with the display every frame), line by line. The 6502 reads its first samples on
 * the line MARIA reads the DLL, the one before the display's; on each line MARIA first reads
 * its objects (picture line y on the display's line top + y), then the 6502 reads two samples;
 * then the ARM draws the next line, while the 6502 runs the rest of this one. */
static RAM_FUNC void serve_frame(void)
{
	draw_line(0);
	vcsSnoopRead(CART(DLL));                      /* MARIA's first read of the DLL (vcsSnoopRead waits
	                                              * for it, as vcsWaitForAddress, which the Chameleon
	                                              * template's firmware lacks) */
	for (int j = 0; j < n_dma; j++) {
		int y = j - 1 - top;                      /* the picture's line MARIA reads on this one */
		if (y >= 0 && y < PICTURE) {
			const uint8_t *bytes = (const uint8_t *)line;
			int zone_top = y & -ZONE, h = PICTURE - zone_top < ZONE ? PICTURE - zone_top : ZONE;
			uint16_t a = (uint16_t)(GFX + (h - 1 - (y - zone_top)) * 256);
			vcsInjectDmaData(CART(a), 32, bytes);
			vcsInjectDmaData(CART(a + 32), 32, bytes + 32);
			vcsInjectDmaData(CART(a + 64), 16, bytes + 64);
		}
		int k = j & 255;
		uint8_t second = j + 256 >= n_dma && k < n_vbl - 2 && k < 64 ? vbl[k] : 0;
		vcsInjectDmaData(CART(SMP), 1, &smp[j]);
		vcsInjectDmaData(CART(SMP + 1), 1, &second);
		if (y >= 0 && y + 1 < PICTURE) draw_line(y + 1);
	}
	/* The VBLANK's first line: what the loop reads, in its order. */
	swcha = vcsSnoopRead(SWCHA);
	swchb = vcsSnoopRead(SWCHB);
	inpt4 = vcsSnoopRead(INPT4);
	inpt0 = vcsSnoopRead(INPT0);
	inpt1 = vcsSnoopRead(INPT1);
	for (int i = 0; i < N_REGS; i++) vcsInjectDmaData(CART(REGS + i), 1, &regs[i]);
}

/* ---- Starting the 7800 ---------------------------------------------------------------------
 * The 6502 runs what the ARM gives it, as on a 2600, until vcsJmpToRam3. */

static unsigned pokes;
static void poke(uint16_t address, uint8_t value)
{
	vcsWrite6(address, value);
	if (++pokes % 32 == 0) vcsJmp3();        /* the 6502 never fetches from $1800 on, its RAM */
}

static void wait_vblank(void)
{
	while (vcsRead4(MSTAT) & 0x80) vcsJmp3();
	while (!(vcsRead4(MSTAT) & 0x80)) vcsJmp3();
}

/* The lines of the VBLANK and of the display, read as the loop reads them: MSTAT just after
 * WSYNC. 243 + 20 (NTSC) and 293 + 20 (PAL) in Test7800. */
static void count_lines(void)
{
	wait_vblank();
	n_vbl = 1;
	for (;;) {
		vcsJmp3();
		vcsWrite5(M_WSYNC, 0);
		if (!(vcsRead4(MSTAT) & 0x80)) break;
		n_vbl++;
	}
	n_dma = 1;
	for (;;) {
		vcsJmp3();
		vcsWrite5(M_WSYNC, 0);
		if (vcsRead4(MSTAT) & 0x80) break;
		n_dma++;
	}
}

/* DLL entries for a number of lines, in zones of up to height lines, all showing one list. */
static uint16_t zones(uint16_t at, int lines, int height, uint16_t list)
{
	while (lines > 0) {
		int h = lines < height ? lines : height;
		poke(at++, (uint8_t)(h - 1));         /* no DLI, no holey DMA, h lines */
		poke(at++, (uint8_t)(list >> 8));
		poke(at++, (uint8_t)list);
		lines -= h;
	}
	return at;
}

/* The 6502's loop, at CODE. */
#define LO(a) ((a) & 0xFF)
#define HI(a) ((a) >> 8)
#define V1    (CODE + 2)
#define N_LO  (CODE + 20)                      /* operands of ldx #lo and lda #hi: n_dma */
#define N_HI  (CODE + 22)
#define REG(i, r) 0xAD, LO(REGS + (i)), HI(REGS + (i)), 0x85, (r)
static const uint8_t code[] = {
	/* CODE: entry, in the VBLANK */
	0xA0, 0x00,                                 /* ldy #0 */
	/* V1: the VBLANK, a line each time */
	0x85, M_WSYNC,                              /* sta WSYNC */
	0x24, MSTAT, 0x10, 0x09,                    /* bit MSTAT / bpl DISP: the display starts */
	0xB9, LO(BUF), HI(BUF),                     /* lda BUF,y */
	0x85, AUDV0,                                /* sta AUDV0 */
	0xC8,                                       /* iny */
	0x4C, LO(V1), HI(V1),                       /* jmp V1 */
	/* DISP: n_dma lines, counted (the ARM serves exactly n_dma pairs of samples). X counts
	 * them (0 = 256), PASSES the further passes of 256 (PAL: 293 = 37 + 256). */
	0xA0, 0x00,                                 /* ldy #0 */
	0xA2, 0x00,                                 /* ldx #lo */
	0xA9, 0x00, 0x85, PASSES,                   /* lda #hi / sta PASSES */
	/* D: a line of the display (MARIA's DMA stops the 6502 from cycle 10 on) */
	0xEA, 0xEA, 0xEA,                           /* nop x 3: the reads after MARIA's */
	0xAD, LO(SMP), HI(SMP), 0x85, AUDV0,        /* lda SMP / sta AUDV0 (served) */
	0xAD, LO(SMP + 1), HI(SMP + 1),             /* lda SMP+1 (served) */
	0x99, LO(BUF), HI(BUF),                     /* sta BUF,y */
	0xC8, 0xCA,                                 /* iny / dex */
	0x85, M_WSYNC,                              /* sta WSYNC */
	0xD0, 0xEC,                                 /* bne D */
	0xC6, PASSES, 0x10, 0xE8,                   /* dec PASSES / bpl D */
	/* the VBLANK's first line (it lasts two) */
	0xA0, 0x00,                                 /* ldy #0 */
	0xAD, LO(BUF), HI(BUF), 0x85, AUDV0,        /* lda BUF / sta AUDV0 */
	0xAD, LO(SWCHA), HI(SWCHA),                 /* lda SWCHA (snooped) */
	0xAD, LO(SWCHB), HI(SWCHB),                 /* lda SWCHB */
	0xA5, INPT4, 0xA5, INPT0, 0xA5, INPT1,      /* lda INPT4, INPT0, INPT1 */
	REG(0, BACKGRND),                           /* lda REGS+i / sta register: */
	REG(1, 0x21), REG(2, 0x22), REG(3, 0x23),   /* P0C1-P3C3 */
	REG(4, 0x25), REG(5, 0x26), REG(6, 0x27),
	REG(7, 0x29), REG(8, 0x2A), REG(9, 0x2B),
	REG(10, 0x2D), REG(11, 0x2E), REG(12, 0x2F),
	REG(13, AUDC1), REG(14, AUDF1), REG(15, AUDV1),  /* the TIA's channel 1 */
	0xC8,                                       /* iny */
	0x4C, LO(V1), HI(V1),                       /* jmp V1 */
};
_Static_assert(REGS + N_REGS <= SMP && (REGS & 0xFF) >= LINE_BYTES, "the served bytes overlap");
_Static_assert(CODE + sizeof code <= 0x2700, "the 6502's loop goes past the RAM it has");

int maria_game_run(uint32_t *args)
{
	(void)args;
	prepare_pictures();
	vcsJmp3();
	vcsWrite5(CTRL, CTRL_OFF);                  /* DMA off while the lists are written */
	vcsWrite5(OFFSET, 0);
	vcsWrite5(AUDC0, 0);                        /* AUDV0 as a DAC */
	vcsWrite5(AUDV0, 0);
	vcsWrite5(AUDC1, 0);
	vcsWrite5(AUDV1, 0);
	poke(SWBCNT, 0x04);                         /* two-button joysticks: SWCHB bit 2 an output at 0 */
	poke(SWCHB, 0x00);
	count_lines();
	int pal = n_vbl + n_dma > 287;
	pal_colours = pal;
	top = (n_dma - PICTURE) / 2;

	/* The lists: empty zones above and below the picture (8 lines further than MARIA reads,
	 * so it never reads past them), zones of 16 lines in it. */
	uint16_t at = zones(DLL, top, 16, DL_EMPTY);
	at = zones(at, PICTURE, ZONE, DL_LINE);
	zones(at, n_dma + 8 - top - PICTURE, 16, DL_EMPTY);
	static const uint8_t lists[] = {
		0x00, 0x00,                                                  /* DL_EMPTY: the end */
		/* DL_LINE: three 5-byte headers (address low, $C0 = write mode 160B, address high,
		 * palette 0-3 and width, x), then the end */
		(uint8_t)GFX, 0xC0, GFX >> 8, 0x00, 0,                       /* 32 bytes at x = 0 */
		(uint8_t)(GFX + 32), 0xC0, (GFX + 32) >> 8, 0x00, 64,        /* 32 bytes at x = 64 */
		(uint8_t)(GFX + 64), 0xC0, (GFX + 64) >> 8, -16 & 0x1F, 128, /* 16 bytes at x = 128 */
		0x00, 0x00};
	for (unsigned i = 0; i < sizeof lists; i++) poke((uint16_t)(DL_EMPTY + i), lists[i]);
	for (unsigned i = 0; i < sizeof code; i++) poke((uint16_t)(CODE + i), code[i]);
	poke(N_LO, (uint8_t)n_dma);                 /* 243 = 243 + 0, 293 = 37 + 256 */
	poke(N_HI, (uint8_t)((n_dma - 1) >> 8));

	/* The game, as on the 2600: the menu has PLAY and SYSTEM (here the colours: NTSC or PAL;
	 * the lines are the console's), no HMOVE and no EXIT. */
	menu_init(&menu, pal ? SYSTEM_PAL : SYSTEM_NTSC, 0, 0, 0);
	music_init();
	prepare_title();
	prepare_samples();
	vcsWrite5(BACKGRND, 0);
	vcsWrite5(DPPH, DLL >> 8);
	vcsWrite5(DPPL, DLL & 0xFF);
	/* DMA on at the start of a VBLANK: MARIA takes the DLL at its end. */
	wait_vblank();
	vcsWrite5(CTRL, CTRL_ON);
	vcsJmpToRam3(CODE);
	/* From here the 6502 never comes back to the cartridge. In Test7800 this first snoop only
	 * ends the byte streaming of the calls above; on the cartridge it sees the loop read MSTAT. */
	vcsSnoopRead(MSTAT);

	uint8_t last_fire = 0, last_swchb = 0xFF;
	for (;;) {
		serve_frame();
		/* VBLANK: the game. The button (the 2600 joystick's, INPT4, or either of the 7800's,
		 * INPT0 and INPT1: 1 pressed) counts when it goes down; RESET goes back to the title. */
		uint8_t fire_now = !(inpt4 & 0x80) || (inpt0 & 0x80) || (inpt1 & 0x80);
		int fire = fire_now && !last_fire;
		int reset = !(swchb & 0x01) && (last_swchb & 0x01);
		last_fire = fire_now;
		last_swchb = swchb;
		if (!playing) {
			int was = menu.system;
			int chosen = menu_frame(&menu, swcha, fire);
			if (menu.system == SYSTEM_PAL60) menu.system = was == SYSTEM_NTSC ? SYSTEM_PAL : SYSTEM_NTSC;  /* a 7800 has no PAL60 */
			pal_colours = menu.system == SYSTEM_PAL;
			if (chosen == MENU_PLAY) {
				game_start(&game);
				playing = 1;
			}
		} else {
			int events = game_frame(&game, swcha, fire);
			game_sound_events(events);
			if ((events & GAME_OVER) || reset) {   /* back to the title, on PLAY */
				playing = 0;
				menu.row = 0;
			}
		}
		if (playing) prepare_game();
		else prepare_title();
		prepare_samples();
	}
}
