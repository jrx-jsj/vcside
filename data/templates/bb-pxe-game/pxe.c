/* C for the PXE kernel, made with VCSIDE (CC0: use and change it freely).
 *
 * VCSIDE builds main.bas with batari Basic, then compiles this file and links it into
 * the ELF batari Basic wrote (a relocatable ELF that runs on the cartridge's ARM, in
 * Stella and in Gopher2600). The link sends the kernel's call to vcsEndOverblank, once
 * per frame, to __wrap_vcsEndOverblank below, which calls the real one first.
 *
 * Here it plays the music: music.h, written by VCSIDE when theme.vcsmusic is saved,
 * is a player in C. The game asks for music through one batari Basic variable,
 * musicCommand (z): 0 nothing, 1 + an effect (MUSIC_EFFECT_x + 1), 128 + a song
 * (MUSIC_SONG_x + 128), 255 stop; this reads it and sets it back to 0.
 *
 * The 6502 program runs on the kernel's emulated CPU: cpu.read and cpu.write reach
 * its RAM and its TIA registers, which the kernel writes to the TIA every frame.
 */
#include <stdint.h>
#include "music.h"

/* the kernel's emulated 6502 (C++ names: CPU::read, CPU::write, the cpu object) */
extern char cpu[];
extern uint8_t _ZN3CPU4readEt(void *self, uint16_t address);
extern void _ZN3CPU5writeEth(void *self, uint16_t address, uint8_t value);
#define CPU_READ(address) _ZN3CPU4readEt(cpu, (address))
#define CPU_WRITE(address, value) _ZN3CPU5writeEth(cpu, (address), (uint8_t)(value))

#define MUSIC_COMMAND 0xB7  /* z, in batari Basic's RAM with the PXE kernel */
#define AUDC0 0x15
#define AUDF0 0x17
#define AUDV0 0x19

extern void __real_vcsEndOverblank(void);

void __wrap_vcsEndOverblank(void)
{
    static uint8_t started;
    __real_vcsEndOverblank();
    if (!started) {
        started = 1;
        music_init();
        music_stop();                       /* silent until the game asks */
    }
    uint8_t command = CPU_READ(MUSIC_COMMAND);
    if (command) {
        CPU_WRITE(MUSIC_COMMAND, 0);
        if (command == 255) music_stop();
        else if (command & 0x80) music_play(command & 0x7F);
        else if (command <= MUSIC_EFFECTS) music_effect(command - 1);
    }
    music_frame();
    for (int channel = 0; channel < MUSIC_CHANNELS && channel < 2; channel++) {
        CPU_WRITE(AUDC0 + channel, music_audio[channel][0]);
        CPU_WRITE(AUDF0 + channel, music_audio[channel][1]);
        CPU_WRITE(AUDV0 + channel, music_audio[channel][2]);
    }
}
