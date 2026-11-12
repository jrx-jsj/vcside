/*
 * {{NAME}} - the ACE bus library (see include/ace.h).
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * The 6507 fetches its instructions one byte per cycle from increasing addresses, so the
 * library only has to know the next address and wait for it: when the 6507 puts that
 * address on the bus, the ARM drives the byte; when it puts a TIA, RIOT or RAM address,
 * the ARM lets go of the data bus. A repeated read of the next address (the dummy read of
 * a NOP) gets the same byte, which is what the 6507 then fetches.
 */
#include "ace.h"

static volatile uint16_t* address_in;
static volatile uint8_t* data_in;
static volatile uint8_t* data_out;
static volatile uint16_t* data_mode;
static uint16_t next_fetch;           /* the cartridge address the 6507 fetches next */

#define CARTRIDGE      0x1000         /* A12: the cartridge's addresses */
#define START          0x1000         /* where the 6507's code starts, after each JMP */
#define JUMP_BACK_FROM 0x1F00         /* past this, JMP $1000 before the next instruction */
#define READ_PORT      0x1FF0         /* ace_read stores A here, and the ARM reads it */

/* The address bus once it has settled: the same value read twice in a row. */
static inline uint16_t stable_address(void)
{
	uint16_t address = *address_in & 0x1FFF, again;
	while ((again = *address_in & 0x1FFF) != address)
		address = again;
	return address;
}

static inline void drive(uint8_t value)
{
	*data_out = value;
	*data_mode = 0x5555;
}

/* Waits for the 6507 to put `target` on the bus, letting go of the data bus meanwhile
 * whenever it reads or writes outside the cartridge. */
static void wait_for(uint16_t target)
{
	for (;;) {
		uint16_t address = stable_address();
		if (address == target) return;
		if (!(address & CARTRIDGE)) *data_mode = 0;
	}
}

static void feed(uint8_t value)
{
	wait_for(next_fetch);
	drive(value);
	next_fetch++;
}

/* Room for an instruction of `bytes` bytes before the end of the 4 KB window. */
static void room(int bytes)
{
	if (next_fetch + bytes > JUMP_BACK_FROM) ace_jmp();
}

/* The 6507 may be anywhere when the program starts (start.S has already asked the firmware
 * to reboot the console into this cartridge). Every cartridge read gets $24: "BIT $24"
 * takes three cycles, opcode, operand and a read of the TIA, so once the 6507 runs them a
 * cartridge read that follows a non-cartridge one is an opcode fetch, and there it gets
 * JMP $1000. The reset vector, if the 6507 reads it, also leads to $1000. There is no
 * clock line: a change of address is how a new cycle is seen. */
void ace_start(void)
{
	const AceArguments* arguments = ACE_ARGUMENTS;
	address_in = arguments->address_in;
	data_in = arguments->data_in;
	data_out = arguments->data_out;
	data_mode = arguments->data_mode;
	__asm volatile("cpsid i");    /* no interrupts from here on: they would miss bus cycles */
	uint16_t previous = START;
	for (;;) {
		uint16_t address = stable_address();
		if (address == previous) continue;
		bool after_other_address = !(previous & CARTRIDGE);
		previous = address;
		if (!(address & CARTRIDGE)) {
			*data_mode = 0;
		} else if (address == 0x1FFC) {
			drive(START & 0xFF);
		} else if (address == 0x1FFD) {
			drive(START >> 8);
			break;
		} else if (after_other_address && address < READ_PORT) {
			drive(0x4C);              /* an opcode fetch: JMP $1000 */
			next_fetch = (uint16_t)(address + 1);
			feed(START & 0xFF);
			feed(START >> 8);
			break;
		} else {
			drive(0x24);
		}
	}
	next_fetch = START;
}

void ace_jmp(void)
{
	feed(0x4C);
	feed(START & 0xFF);
	feed(START >> 8);
	next_fetch = START;
}

void ace_lda(uint8_t value)
{
	room(2);
	feed(0xA9);
	feed(value);
}

void ace_sta(uint8_t address)
{
	room(2);
	feed(0x85);
	feed(address);
}

void ace_write(uint8_t address, uint8_t value)
{
	room(4);
	feed(0xA9);
	feed(value);
	feed(0x85);
	feed(address);
}

void ace_wsync(void)
{
	ace_sta(WSYNC);
}

void ace_nops(int count)
{
	while (count-- > 0) {
		room(1);
		feed(0xEA);
	}
}

void ace_code(const uint8_t* bytes, int count)
{
	room(count);
	while (count-- > 0) feed(*bytes++);
}

uint8_t ace_read(uint16_t address)
{
	room(6);
	if (address < 0x100) {
		feed(0xA5);                   /* LDA zero page */
		feed((uint8_t)address);
	} else {
		feed(0xAD);                   /* LDA absolute */
		feed((uint8_t)address);
		feed((uint8_t)(address >> 8));
	}
	feed(0x8D);                       /* STA $1FF0 */
	feed(READ_PORT & 0xFF);
	feed(READ_PORT >> 8);
	/* The write cycle: the 6507 drives the bus. The last sample may be taken after the
	 * address has changed, so the one before it is the value. */
	wait_for(READ_PORT);
	*data_mode = 0;
	uint8_t value = *data_in, previous = value;
	while ((*address_in & 0x1FFF) == READ_PORT) {
		previous = value;
		value = *data_in;
	}
	return previous;
}

/* The overblank program, at $80 of the 6507's RAM. Line 0 turns VSYNC on and plays the
 * low nibble of $E0; then 31 pairs of lines play the low and the high nibble of $E0-$FE,
 * and the first line of the second pair (line 3) turns VSYNC off. 63 WSYNCs in all. */
static const uint8_t overblank_program[] = {
	0xA0, 0x00,             /* $80        ldy #0          */
	0xA9, 0x02,             /* $82        lda #2          */
	0x85, 0x02,             /* $84        sta WSYNC       line 0 */
	0x85, 0x00,             /* $86        sta VSYNC       */
	0xA5, 0xE0,             /* $88        lda $E0         */
	0x85, 0x19,             /* $8A        sta AUDV0       */
	0xA2, 0x1F,             /* $8C        ldx #31         */
	0x85, 0x02,             /* $8E pair:  sta WSYNC       line 2y + 1 */
	0xE0, 0x1E,             /* $90        cpx #30         */
	0xD0, 0x04,             /* $92        bne sample      */
	0xA9, 0x00,             /* $94        lda #0          */
	0x85, 0x00,             /* $96        sta VSYNC       line 3: off */
	0xB9, 0xE0, 0x00,       /* $98 sample: lda $00E0,y    */
	0x85, 0x19,             /* $9B        sta AUDV0       low nibble */
	0x4A, 0x4A, 0x4A, 0x4A, /* $9D        lsr (4 times)   */
	0xC8,                   /* $A1        iny             */
	0x85, 0x02,             /* $A2        sta WSYNC       line 2y + 2 */
	0x85, 0x19,             /* $A4        sta AUDV0       high nibble */
	0xCA,                   /* $A6        dex             */
	0xD0, 0xE5,             /* $A7        bne pair        */
	0x4C, 0x00, 0x10,       /* $A9        jmp $1000       back to the ARM */
};

void ace_leave(void)
{
	room(3);
	feed(0x6C);                       /* JMP ($FFFC): the next two reads are the vector, */
	feed(0xFC);                       /* which the firmware's menu answers once main has */
	feed(0xFF);                       /* returned non-zero */
}

void ace_overblank_load(void)
{
	for (unsigned i = 0; i < sizeof overblank_program; i++)
		ace_write((uint8_t)(0x80 + i), overblank_program[i]);
}

void ace_overblank_start(void)
{
	room(3);
	feed(0x4C);                       /* JMP $0080 */
	feed(0x80);
	feed(0x00);
	next_fetch = START;               /* where the program comes back */
}

void ace_overblank_end(void)
{
	wait_for(START);
}
