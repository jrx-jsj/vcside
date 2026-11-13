# {{NAME}}: notes for the assistant

A 32K DPC+ program from VCSIDE's DPC+ music example: a square moved with the joystick and a song played by the cartridge's ARM as three sampled voices.

## Kernel rules
- Every line, kernel and blank lines alike, copies the voices' sum to the TIA: `SOUND` (`lda AMPLITUDE` / `sta AUDV0`, `AUDC0` = 0). Code that takes longer than a line must do it in between, or the sound stops.
- Only the last bank is used (`BANK5`, where DPC+ starts). The first 128 bytes of every bank are the cartridge's registers: code starts after them.
- `HMOVE` once a frame, in the vertical blank, after `PosObject`. The blank lines are timed by `TIM64T`/`INTIM`.

## Where things live
- Joystick: `Joystick` (`SWCHA`). Fire button: `Sounds` (`INPT4`).
- Music: `MusicInit`, `MusicFrame` once a frame (sets the voices' waveforms and notes), `MusicEffect` with `MUSIC_EFFECT_FIRE` (takes the third voice) or `MUSIC_EFFECT_BUMP` (TIA channel 1).
- `music.asm` is included three times (`MUSIC_PART` 1, 2, 3): player and song, waveforms in the display data, frequencies.

## Build
- F7 assembles `main.asm` with DASM (`-f3`) into `build/{{NAME}}.bin`, 32768 bytes. F5 runs it.

## Do not touch
- `DPCplus.arm`: DPC+'s ARM driver, copied from batari Basic.
- `music.asm`: written by VCSIDE when `theme.vcsmusic` is saved. Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `MusicPlay` with `MUSIC_SONG_<NAME>` in A starts another song, `MusicStop` stops it.
- `2600.h` and the register addresses at the top of `main.asm`.
