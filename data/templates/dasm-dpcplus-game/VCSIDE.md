# {{NAME}}: notes for the assistant

VCSIDE The Game on a 32K DPC+ cartridge: a title screen with a menu (PLAY, TV: NTSC / PAL60 / PAL), then a room with a hero, two enemies, a shot, score and lives. The music is played by the cartridge's ARM as three sampled voices.

## Kernel rules
- Every line copies the voices' sum to the TIA: `SOUND` (`lda AMPLITUDE`) or, with fast fetch on, `FAST_SOUND` (`lda #<AMPLITUDE`), then `sta AUDV0`. Logic longer than a line has `SOUND` in between.
- The kernels (`TitleKernel`, `GameKernel`) run with fast fetch on: `lda #n` with n < $28 reads DPC+ register n. Constants there go through `ldx #`/`ldy #`. `BeamOn` turns it on; the kernels turn it off on return.
- The TV system is chosen at run time (`System`: 0 NTSC, 1 PAL60, 2 PAL): 262 or 312 lines, `VblankTime`/`OverscanTime`, polled with `bit TIMINT`. Measured: change them only by measuring again.
- `DrawMode` (Mode when the frame began) says which kernel draws: logic that changes `Mode` takes effect on the next frame.
- `GameKernel`: two lines per pass: the room (fractional fetchers 4-6, a row every 4 passes), then the hero (`DF0DATAW`, windowed), its colours (`DF1DATA`), the enemy (`DF2DATAW`) and the shot as the ball (`DF3DATAW`). `PointGame` points them: pointer = buffer - passes before the object's top row.
- `TitleKernel`: the cartridge (8 fractional fetchers, `CartTablesLo`), THE GAME (48 pixels, fetchers 0-5, writes at cycles 44-53), the menu's texts (`PointText`, `DrawText`). Count cycles before changing them.
- `HMOVE` once a frame, in the vertical blank, after `PosObject`.

## Where things live
- Bank 5: the game. Bank 4: the music player and the song, called through `PlayerInit`, `PlayerFrame` and `PlayerEffect` (trampolines at `TRAMPOLINES`, the same address in both banks). Banks 0-3: empty.
- The display data: the waveforms, the playfields (`CopyTables` writes them at power on, top row first), the hero's and the enemy's frames (`Animate`, `GameLogic`, through `DF7WRITE`), `TheGame0D-5D`.
- Joystick and button: `StickPresses`, `ButtonPress` (menu), `MoveHero`, `Shoot`. Hits and walls: `Collisions` (overscan). Effects: `PlayerEffect` with `MUSIC_EFFECT_SHOT`, `_HIT` (TIA channel 1) or `_LOSE`.

## Folders
- `src/`: programs; `resources/gfx/`, `resources/music/`: `.vcsgfx`, `.vcsmusic`; `generated/`: what VCSIDE writes from them; `build/`: the build's. Set in the `.vcsproj` (`*_dir`).

## Build
- F7 assembles `src/main.asm` with DASM (`-f3`) into `build/{{NAME}}.bin`, 32768 bytes. F5 runs it.

## Do not touch
- `DPCplus.arm`: DPC+'s ARM driver, copied from batari Basic.
- `generated/gfx/*.gfx.asm` and `generated/music/music.asm`: written by VCSIDE when the `.vcsgfx` and `.vcsmusic` files are saved. Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `MusicPlay` with `MUSIC_SONG_<NAME>` in A starts another song, `MusicStop` stops it.
- `2600.h` and the register addresses at the top of `main.asm`.
