# VCSIDE — music editor (0.2.0)

It covers what already exists, what the 2600 can do, and what VCSIDE adds. The decisions taken are at the end. Every technique the editor uses is credited to whoever devised or spread it, here and in the editor's own help: see [Credits](#credits).

## What already exists

| Tool | What it is | Licence | Use for VCSIDE |
|---|---|---|---|
| TIATracker (Andre "Kylearan" Wichmann, 2016-2017) | A tracker for TIA music, written in Qt, with its own player routine. It exports to DASM, K65 and CSV. Its songs are `.ttt` files (JSON). | Program: GPL-2.0-or-later. Player (`player/`): Apache-2.0. | The format is open and readable, so VCSIDE can **import `.ttt`**. VCSIDE does not reuse any of its code or interface. |
| Furnace (tildearrow) | A multi-system tracker with a TIA mode. It also exports to TIunA, a software pitch driver. | GPL-2.0-or-later | Out of scope: its `.fur` files are a compressed binary format covering dozens of chips. Anyone who prefers Furnace can keep using it, and include its export as an ordinary source file. |
| Star Bars (Aleksi Eeben, 2023) | A sequencer whose "Tiatonic Tuning" adds fractional bits to each frequency, written as 8-bit patterns that switch the AUDF value every 8 or 16 scanlines: 8 times the pitch resolution. The editor runs on a Commodore 64. | None stated | The earliest switching driver found. VCSIDE writes its own version. |
| TIunA (Natt, AYCE, in "Twin", 2024) | A driver that switches between two AUDF values at a fast rate to get pitches in between them. Furnace exports to it. | MIT | The same idea, in a driver Furnace exports to. |
| TIAtune (utz, 2017; updated with Thomas Jentzsch, 2022) | A driver that ignores the TIA's dividers and generates the waves in software with 16-bit dividers: pitch within 0.15 %, at the cost of all the CPU time. | MIT | The proof that exact pitch is possible on a plain cartridge. Not a VCSIDE target, because it leaves no time for a game. |
| TIA Music Explorer (pizzatari, 2025) | A web table of TIA pitches against piano keys, with A4 and transposition settings. | None stated | Reference only. |
| Paul Slocum's *Atari 2600 Music and Sound Programming Guide* (2003), music kit and sequencer kit | Three non-tempered setups: sets of AUDF values chosen to be in tune with each other, some of them shifted from A440 (the drum setup is about 50 cents up). | None stated | The idea of tuning against the song rather than A440. The tables are computed here from the formula. |
| Eckhard Stolberg's frequency and waveform guide | The tuning of every note the TIA can play, for every distortion. | None stated | Reference to check the computed tables against. |
| Ron Fries' TIASOUND (1996) | The first TIA sound emulation: the distortions' bit patterns and the frequency formula. Most emulators started from it. | LGPL-2 | The source of the formula and the periods in the table below. VCSIDE's synthesis is its own code. |
| DPC, DPC+, CDF, CDFJ music drivers | The cartridge mixes 3 voices into one TIA channel. DPC (David Crane, Pitfall II, Activision 1984) plays square waves. DPC+ (Darrell Spice Jr. and Fred Quimby; first heard in Stay Frosty 2) plays 32-sample waveforms. CDF and CDFJ reuse its music routines. The 6507 copies the amplitude to AUDV0 on every scanline. | Varies | VCSIDE exports data for them, not the drivers themselves. |

**Recommendation:** build VCSIDE's own editor, as with the graphics editor, and **import TIATracker files**. What no existing tool offers, and what makes the editor worth having inside an IDE:
- tuning shown and solved for the whole song;
- samples for ELF and other ARM cartridges;
- export straight into DASM, batari Basic and C projects, kept up to date on save as `.vcsgfx` is.

## The TIA's pitches

The TIA (designed at Atari by Jay Miner and Joe Decuir) has two audio clocks per scanline: 31,399.5 Hz on NTSC (3,579,545 / 114) and 31,113.1 Hz on PAL (3,546,894 / 114). A tone's frequency is, as Ron Fries' TIASOUND documents it:

```
frequency = audio clock / ((AUDF + 1) × period of the distortion)
```

| AUDC | Sound | Period (audio clocks) | Range with AUDF 31..0, NTSC |
|---|---|---|---|
| 4, 5 | square | 2 | 491 – 15,700 Hz |
| 12, 13 | square, ÷6 | 6 | 164 – 5,233 Hz |
| 1 | 4-bit poly (buzzy) | 15 | 65 – 2,093 Hz |
| 6, 10 | pulse, ÷31 | 31 | 32 – 1,013 Hz |
| 7, 9 | 5-bit poly | 31 | 32 – 1,013 Hz |
| 14 | ÷93 | 93 | 11 – 338 Hz |
| 15 | 5-bit poly ÷6 | 93 | 11 – 338 Hz |
| 2, 3 | 465-step poly (rumble) | 465 | low noise |
| 8 | 9-bit poly | 511 | white noise |
| 0, 11 | constant: AUDV becomes the output level | — | used for samples |

TIASOUND gives AUDC 15 a period of 186, but at the counter level (Chris Brenner's model, which the emulators follow) it repeats every 93 clocks, like AUDC 14 with a different waveform; VCSIDE uses 93. The dividers are integers, so only some AUDF values land near equal-tempered notes. For example, with a ÷6 square on NTSC, only 5 of the 32 values are within 10 cents of an A440 note. PAL is a little worse.

### How the editor shows it

- **Absolute.** Each note shows the closest TIA pitch for every distortion, and how many cents it is off A440. The piano keyboard and the note grid colour each key and note by how far off it is: in tune, slightly off, clearly off, out of range.
- **Relative.** Paul Slocum's setups already tuned against the song rather than A440. What the ear notices in a song is mostly the intervals between its notes, not the distance to A440. The editor measures every note against the song's own reference. It shows the error of each note and the spread: worst note, and the mean of the errors.
- **Automatic tuning.** "Tune the song" tries every transposition (TIA Music Explorer offers the same two settings to explore by hand) (0–11 semitones) and every reference from −50 to +50 cents. For each note it may use any of the distortions the instrument allows. It keeps the choice with the smallest worst-note error, or the smallest mean error (the user picks which). In a C major melody over two octaves with only ÷6 and square:

  | Reference | Worst note | Mean |
  |---|---|---|
  | A440 | 46 cents | 14 |
  | +14 cents | 32 cents | 23 |

  Adding the pulse and buzzy distortions brings the worst note down to 27 cents. Neither criterion is right for every song, so both are offered.
- **NTSC and PAL** have different clocks. The song is tuned for the project's standard. With "Multiple (in the code)", two AUDF tables are written, and the editor shows the errors of both.
- **Finer pitch** (optional per instrument): switching between two AUDF values in the same frame, as Aleksi Eeben's Tiatonic Tuning (Star Bars) and Natt's TIunA do, or as the cartridge's ARM can do on every scanline. It costs CPU time, so it is an instrument setting, not a default.

## Samples: ELF and the ARM cartridges

With AUDC = 0 the TIA outputs AUDV as a level, so a program that writes AUDV0 on every scanline plays a 4-bit sample. This has been used since the 1980s for digitised speech; the earliest named author has not been found yet, and the editor will credit them once known. NTSC gives 15,700 samples per second, PAL 15,600. On an ELF cartridge the ARM can compute that sample itself, which means **real synthesis**:
- **Voices:** several voices, each with a 32-bit phase accumulator (direct digital synthesis, the usual technique, with no single author), so pitch is exact. Out-of-tune notes become a TIA-only problem.
- **Waveforms:**
  - square with adjustable duty;
  - triangle;
  - sawtooth;
  - sine;
  - noise, from an LFSR with a choice of lengths, including the TIA's own 4-, 5- and 9-bit sequences;
  - user-drawn waveforms of 32 samples.
- **Shaping:** volume envelopes (attack, decay, sustain, release) and pitch effects (vibrato, slide, arpeggio). The Stay Frosty 2 driver (Darrell Spice Jr.) already shaped its waveforms on the fly to get ADSR envelopes.
- **Mixing:** the voices are added, scaled and limited to 16 levels, with dithering as an option. Two channels (AUDV0 + AUDV1) give more levels. The TIA mixes the channels non-linearly: out = 2 × sum / (30 + sum), for a sum of levels from 0 to 30 (Chris Brenner, simplified by Thomas Jentzsch). Players that keep one channel free for sound effects use only one.
- **Cost:** the ARM code is a generated C player for the project, with its CPU time per scanline estimated, so it fits next to the kernel. On the Chameleon Cart (Cortex-M0+) that matters more than on the PlusCart.

**DPC+ and CDFJ** (batari Basic DPC+ and assembly projects) have 3 hardware voices with 32-sample waveforms and 32-bit frequencies. The same song, limited to 3 voices and to waveforms of that size, can be exported for them. **DPC** (Pitfall II) has 3 square voices.

## The editor

- **File:** `.vcsmusic`, JSON, readable and diffable. It holds:
  - the instruments;
  - the song's patterns and order;
  - the tuning chosen;
  - the target: TIA, samples (ELF), DPC+ or CDFJ.

  It opens in a tab, as `.vcsgfx` does. *File → New music...*.
- **Instruments:**
  - **TIA:** an AUDC (or a list of allowed ones, for the tuning) and envelopes per video frame for volume, AUDC and pitch offset.
    The instrument panel names the register each part writes: *Volume (AUDV)* and *Release (AUDV)*; a melodic instrument's *Sounds it may use (AUDC)* and *Pitch (AUDF)*; a drum's *Waveform (AUDC)* and *Pitch (AUDF)*, frame by frame whatever the note. A sample voice has no register of its own (the ARM mixes the voices into AUDV0), so its titles name none.
  - **Samples:** a waveform, an envelope and effects.
- **Composition:** a tracker grid to write in and a piano roll of the same pattern. Both have:
  - two channels on the TIA, or N voices with samples;
  - patterns reused in an order list;
  - speed in video frames per row, with swing (alternating speeds, as in TIATracker);
  - a piano keyboard coloured by tuning;
  - input from the computer keyboard.
- **Listening:**
  - VCSIDE synthesises the sound itself (`src/music/tia_audio.cpp`), counter by counter as Chris Brenner described the TIA, mixing the two channels as he measured them, through SDL3's audio. The code is VCSIDE's; a test checks it gives exactly what Gopher2600 gives for every AUDC.
  - The sample target goes through the same path as the console: 4-bit levels at the scanline rate.
  - Playback follows the cursor, and one channel can be heard alone.
  - Export to WAV.
- **Sound effects:** sounds the game plays when something happens (a jump, a shot): an instrument and a note on a channel, with a name. While one sounds, the song's notes on that channel wait.
  - **TIA songs:** an effect takes one of the two channels, usually channel 1, and the song's notes there wait until it ends.
  - **Songs of samples (ELF, DPC+, CDFJ):** the voices all go to AUDV0, so the TIA's channel 1 is free. An effect marked *TIA channel 1* plays there as a TIA instrument (drum or melodic, noise included) and the song keeps all its voices. It is exported as a table, one AUDC1/AUDF1/AUDV1 triplet per frame, made by the editor's own player, so it sounds the same; its volume envelope must end on 0 (at most 254 frames). The C player leaves the frame's values in `music_tia[3]` for the game to write once per frame; the DPC+ 6502 player writes them itself in `MusicFrame` (3 more bytes of zero page).
- **Output on save** (*Export...* chooses where; the song remembers it). The export is one file per folder, with every song of the folder exported the same way and one player for all of them: a song added to a game's folder is in the game at once (see *Several songs* below).
  - **DASM:** `music.asm` with VCSIDE's player (CC0) and the songs' tables, in one file. The program defines `MUSIC_RAM` (19 bytes of zero page) before including it.
  - **batari Basic:** the same file, with `MUSIC_RAM` set to a variable (`h` uses h to z), put in with `inline` and called from `asm` blocks.
  - **C (ELF):** `music.h` with the songs and the synthesis player, ready to call once per scanline from the kernel and once per frame.
  - **DPC+ (DASM):** `music.asm`, included three times with `MUSIC_PART` 1 (the 6502 player and the song, in a bank), 2 (the waveforms, in the display data) and 3 (the frequency table).
  - **CDFJ (C):** `music.h` for the cartridge's ARM code: the C player without the mixing, calling two hooks the game defines.
  - Each file states its size in ROM and RAM, so the space report can show it.
- **Import:**
  - TIATracker `.ttt`: its instruments, percussion and patterns.
  - MIDI files (later): notes per channel, then tuned.
- **Example projects:** the DASM example and the batari Basic square example play a short song, with a sound effect on the fire button and another when the hero (or the square) reaches an edge or a wall; VCSIDE The Game (DASM and batari Basic) plays it with three effects: shot, hit and lose. The C (ELF) template and its example play a 3-voice song as samples, with the same two effects: "fire" on a voice and "bump" on the TIA's channel 1. The DASM example "music on a DPC+ cartridge" plays that song on DPC+'s three voices, with the same effects ("bump" on the TIA's channel 1); each scanline's luminance is the amplitude written to AUDV0. The example "C and music on a CDFJ cartridge" does the same on CDFJ, with the game logic and the player in C on the cartridge's ARM.

## Credits

Each technique, with whoever devised or spread it. The editor's help shows the same credits next to each feature.

| Technique | Credit |
|---|---|
| The TIA's sound: dividers, distortions, polynomial counters | Jay Miner and Joe Decuir, Atari (1977) |
| Frequency formula and bit patterns of the distortions | Ron Fries, TIASOUND (1996) |
| The TIA's audio counter by counter, and how its two channels mix | Chris Brenner, "TIA Sounding Off in the Digital Domain" (AtariAge); the mixing formula in Thomas Jentzsch's simplified form. Implemented in Stella, 6502.ts and Gopher2600 |
| Tables of every TIA note and its tuning | Eckhard Stolberg; Paul Slocum (2003) |
| Tuning against the song, not A440 (non-tempered setups) | Paul Slocum, *Atari 2600 Music and Sound Programming Guide* (2003) |
| Exploring A4 and transposition against TIA pitches | pizzatari, TIA Music Explorer |
| Instruments with per-frame envelopes, swing, a tracker for the TIA | Andre "Kylearan" Wichmann, TIATracker (2016) |
| The tracker itself: patterns, an order list and an effect column per cell | Karsten Obarski, Ultimate Soundtracker (Amiga, 1987) |
| Finer pitch by switching AUDF values (fractional bits) | Aleksi Eeben, Tiatonic Tuning in Star Bars (2023); Natt (AYCE), TIunA (2024) |
| Exact pitch by software dividers | utz, TIAtune (2017), with Thomas Jentzsch (2022) |
| 3 voices mixed by the cartridge into one channel | David Crane, DPC in Pitfall II (Activision, 1984) |
| Waveforms in the cartridge's music; ADSR by shaping them | Darrell Spice Jr. and Fred Quimby, DPC+ (Stay Frosty 2); reused in CDF and CDFJ |
| 4-bit samples by writing AUDV with AUDC = 0 | Still to be found |
| More than 4 bits by sampling the volume between audio clocks | Discussed in AtariAge's "8-bit digital audio from 2600" thread; authors still to be credited |
| Direct digital synthesis (phase accumulator) | A general technique with no single author |
| Samples on the overblank's lines of an ELF game | The ELF loader's own overblank program (United Carts of Atari firmware, Stella, Gopher2600) plays a nibble per line from RIOT RAM to $2D; VCSIDE's templates change that write to AUDV0. Who designed the program's sample slot is still to be found |
| Sound effects that take a music channel while they sound | Common in 2600 games; who did it first is still to be found |
| Sound effects on the TIA's free channel while the cartridge plays the music on the other | Done in DPC+ and CDFJ games; who did it first is still to be found |

## Phases for 0.2.0

1. Tuning maths and TIA sound synthesis, with tests. Each credit is checked against a primary source and recorded in `docs/sources.md`. The cents tables are computed and checked against the formula and against Stella recordings.
2. The editor: TIA instruments, two channels, patterns, listening, tuning views and automatic tuning. **Done**: `src/music/song.*` (the `.vcsmusic` file), `player.*` (frame by frame, the rules the exported players will follow), `music_audio.*` (SDL3 output) and `music_editor.*`.
   The player's rules, which the exported players must keep:
   - A row lasts `speed` frames (`swing` on odd rows if set).
   - A note starts its instrument's envelopes at frame 0, with volume envelope 0 and release 0.
   - While held, the volume repeats from `loop` to its end, or stays on its last value; an envelope that ends on 0 with no loop ends the note.
   - A note off (`===`) plays the release once and silences the channel.
   - The effect column, applied after the cell's note and before its note off: `Vn` switches to volume envelope n (on a cell with no note, the held note goes on at its frame with envelope n); `Rn` sets the release a later note off uses. A variant the instrument does not have is its first one.
   - Melodic pitch, frame by frame (`Song::pitchSequence`): a note tuned by hand (by the note heard, after the transposition) plays its own AUDC and AUDF list; otherwise the note gets the closest AUDC and AUDF, and the pitch envelope moves it, in cents (the AUDF of the same AUDC closest to the note plus those cents) or in AUDF steps (added to AUDF, clamped to 0-31). The last value is held.
   - A note no allowed AUDC reaches is silent.
   - At the song's end every voice is released, and the song loops to `loop_to` or stops.

   Exporting will turn every combination a song uses (instrument, note tuned by hand, variant) into tables of its own, so the players on the console only read tables frame by frame: the extra cost is ROM, not cycles.

   Added after the first version (2026-11-10): the *Registers* view (what is written to AUDC, AUDF and AUDV on every frame, the values that change and why), the pitch envelope in cents resolved note by note, notes tuned by hand per instrument, and volume and release variants picked by the effect column. `.vcsmusic` is now version 2; version 1 files are read with their pitch in AUDF steps.
3. DASM and batari Basic export, and the DASM example. Checked in Stella and Gopher2600. **Done** (2026-11-10): `src/music/export.*`, sound effects, the *Export...* window and the examples.
   - **The player** is VCSIDE's own, written for this and dedicated to the public domain (CC0), so a game carries no notice. `MusicInit` starts the song, `MusicFrame` plays a frame, `MusicEffect` plays the effect whose number is in A (`MUSIC_EFFECT_<NAME>` constants). It is about 400 bytes; the example's song adds 326 bytes of tables.
   - **RAM:** 19 bytes of zero page from `MUSIC_RAM`. The song: order entry, row, frames left, pointer to the next row. Per channel: the volume (or release) table and its index, the pitch table and its index (bit 7: an effect has the channel), and a state byte (bit 7 sounding, bit 6 released, bits 0-5 the release).
   - **Tables:** every combination the song uses (instrument, note, tuning by hand, variant) is a *sound*: a volume envelope, a pitch table of AUDC and AUDF pairs frame by frame (at most 120), and the releases it may use. Identical tables are shared. Rows store, per channel, an event code: nothing, note off, volume envelope, release, note off with release, silence, or a sound to start. At most 250 sounds, 64 releases, 256 envelopes and 254 order entries; more is reported as a problem.
   - **Exactly what the editor plays:** the editor's player follows the same rules (below), and the tests run each export on a 6502 emulator against `Player` write by write: 60 random songs, 1500 frames each, effects included. A cell that would play differently depending on what came before (a note with no instrument after different instruments) is reported, so the editor and the console never disagree silently.
   - **Rules added for the console:** a cell's instrument becomes the channel's for later notes; the volume index goes on frame by frame and the `V` effect switches envelope keeping it; the release is chosen at the note off; a voice that stops writes AUDC, AUDF and AUDV = 0; the pitch stays on its last step.
   - **The TIA track (2026-11-13):** a song of samples can have one more column, *TIA channel 1*, played on the TIA's channel 1, which the voices leave free. Its instruments play as TIA instruments (AUDC, AUDF and AUDV once per frame, melodic ones tuned by the song's transposition and A4, drums with their lists), with the same rules as a channel of a TIA song. It is the voice the sound effects on the TIA's channel 1 use: an effect there takes it as an effect takes a voice (the track's events are skipped, and its note is gone) until the effect ends, and the next note gets it back. Its cells are `cells[kTiaTrack]` (column 4, whatever the voices), in the file a pattern's `"tia"` list and `"tia_track": true`. The C player (ELF, ACE and CDFJ) has `MUSIC_TIA_TRACK`: a voice of its own with the same envelopes and releases, its sounds in `music_tia_sound_*` and its AUDC and AUDF pairs in `music_tia_pitch_data`, read after the voices' events in each row; `music_frame` leaves its AUDC1, AUDF1 and AUDV1 in `music_tia` when no effect sounds there (so the game's code does not change). `music_tia` is now always defined. DPC+ does not play it yet (reported, and left out). Tests: random songs with a TIA track and effects there, the C player against `Player` line by line and on channel 1 (17 songs, 800 frames each).
   - **Several songs (2026-11-13):** the folder's `.vcsmusic` files exported the same way go into one `music.asm` (or `music.h`) with one player, sorted by file name and numbered from 0: `MUSIC_SONG_<FILE NAME>`. `MusicInit` (power on) clears the player's RAM and plays song 0; `MusicPlay` with a song in A (`music_play(song)` in C) starts it from its first entry, silencing the voices of the last song but not a sound effect that plays; `MusicStop` (`music_stop()`) lets the notes go as at a song's end. The order lists go one after another, each song closed by an entry of 0 rows whose low byte is the entry it loops to (its own index: it stops); the player needs no more RAM, and each song costs 3 bytes (5 with pattern speeds) beyond its own entries, plus a byte in `MusicSongStart`. The tables (sounds, envelopes, releases, pitches) are shared, so what two songs both play is stored once. Songs with different speeds turn on the per-pattern speeds. Effects of every song go together; one with the same name and the same sound in two songs is kept once, another with the same name is reported. Songs of samples go together only with the same voices and loudness. The player grew by about 40 bytes; in batari Basic's VCSIDE The Game, 4K with the title, 20 bytes are left. A new song made next to an exported one takes its export, sound and voices, so it joins the game; one that stops exporting leaves the others in the file. Tests: three random songs exported together, each played with `MusicPlay` against `Player` for that song alone, effects included, and started over another one as it plays (42 of each, write by write), `MusicStop` ending in silence; and the second of two songs of samples in the C player, line by line.
   - **Sound effects:** an effect starts its sound on its channel and locks it: the song's events there are skipped, and the song's end does not release it. The lock ends when the effect's voice stops (an envelope that ends on 0, or its release).
   - **Cost:** at most 190 instructions a frame in the tests, about 800 cycles. In the DASM example the vertical blank keeps more than 1700 cycles free (INTIM, measured in Stella); in the batari Basic square example about 1900 cycles of the overscan are left. Both keep their line count (Stella and Gopher2600).
4. Samples for ELF: waveforms, voices, mixing, the C player and the C example. **Done** (2026-11-10), checked in Stella and Gopher2600; not yet on a PlusCart or a Chameleon Cart.
   - **The song:** *Sound* chooses *TIA* or *Samples (ELF)*, 1 to 4 voices and the loudness (`gain`, in sixteenths: 16 means every voice at full volume together reaches AUDV 15; more is louder and clips). An instrument has a waveform (square with a duty of 1 to 31 of 32, triangle, sawtooth, sine, noise from a 4-, 5-, 9- or 15-bit register, or 32 levels drawn by hand), the same volume envelopes and releases, and a pitch envelope in cents. Pitch is exact, so there is no tuning, only the transposition and A4.
   - **The mix, line by line (`SampleMixer`):** each voice adds a 32-bit phase step per scanline (`Song::phaseSteps`: the note's frequency over half the audio clock, which is the line rate); the top 5 bits pick one of the wave's 32 levels. Noise moves its Galois register each time those 5 bits change, so the note sets how bright it is. The voices' levels times their volumes are added, scaled by `gain` and the voice count into quarters of an AUDV level, and a 64-entry table turns that into the AUDV whose output (through the TIA's non-linear mix) is closest. A new note restarts its voice's phase.
   - **The C player** (`<name>.music.h`, CC0, the same rules as the 6502 player): `music_init()`, `music_effect(n)` and `music_frame(lines)`, which plays the frame's row and fills `music_level[]` (one AUDV0 value per line of the kernel) and `music_nibbles[31]` (the 63 lines of the overblank, two per byte). The tests compile it with the system's C compiler (`-Wall -Wextra`, warnings fail) and compare it line by line with the editor's mixer: 24 random songs of 400 frames, at 262 and 312 lines, effects included.
   - **The ELF templates:** `start_6507` changes the overblank program's `sta $2d` (at $86-$87) into `sta AUDV0`, only if those bytes are there; AUDC0 is already 0. The kernel writes `music_level[line]` to AUDV0 right after each WSYNC and spreads the 31 bytes of `music_nibbles` over its first lines ($E0-$FE); the overblank then plays them. In Stella every one of the 262 lines gets exactly one write (the overblank's at colour clock 9); the hero example's first line, which positions the sprite, gets its sample at the end of the line before. The tuning is the project's TV standard at export; a game that switches to PAL at run time keeps the NTSC pitches.
5. TIATracker import; DPC+ and CDFJ export (done, 2026-11-10).
   - **TIATracker import (done, 2026-11-10):** *File → Import TIATracker song...*, or opening a `.ttt`, writes a `.vcsmusic` next to it (`src/music/import_tiatracker.*`). The two channels' sequences are laid side by side and cut into VCSIDE patterns wherever either starts one; when both loop, the song is as long as their loops take to meet again (at most 8192 rows). Each AUDF a melodic instrument plays becomes the closest note, tuned by hand to that AUDC and AUDF list (with the envelope's AUDF offsets), so it sounds the same; percussion becomes drums. Patterns keep their speeds: patterns now have an optional speed and swing of their own, in the editor, the `.vcsmusic` file and the three players (`MUSIC_PATTERN_SPEEDS`, with a speed and a swing byte per order entry). Kept approximately, with a warning: slides (a new note), overlay percussion (a plain one), pitch bends in a release (the release keeps the note's pitch), and pitch bends in a held loop (written out for 120 frames). The tests play a song of their own through a small model of TIATracker's playback and through VCSIDE's player after the import, frame by frame; with `VCSIDE_TTT_DIR` they do the same for a folder of songs (TIATracker's 50 examples: 30 identical, 1 off only in a release bend, 19 not compared because they use slides or overlay percussion; 49 export, the other has more than 254 order entries).
   - **DPC+ export (done, 2026-11-10):** for songs played as samples with up to 3 voices. DPC+ adds its 3 voices' samples with no volume of its own, and AUDV0 takes 4 bits, so each voice's volume becomes one of 15 / voices levels (5 with 3 voices; scaled by the song's gain) and every waveform is stored once per level, 32 bytes each, in the display data; a voice changes volume by pointing `WAVEFORMx` at another copy. Frequencies go to the frequency table (2^32 x Hz / 20000; entry 0 is silence) and `NOTEx` picks one. The player is the 6502 one with `MUSIC_CHANNELS` 3 and `MUSIC_DPC`: its pitch pairs become (the waveform's first slot, the frequency's index), and its RAM grows to 5 + 7 per voice (26 bytes). Limits, reported: 255 frequencies (batari Basic's DPC+ keeps only 128 when it has ARM code of its own), 128 waveform slots. Noise repeats every 32 steps of its register, and notes do not restart the waveform's phase. Tests: random songs assembled and run on the 6502 emulator, every voice's waveform and frequency compared with the editor's player frame by frame; and a test ROM (batari Basic's CC0 DPC+ driver, the export, a kernel writing AMPLITUDE to AUDV0) played in Stella: an A4 measures 439.99 Hz.
   - **CDFJ export (done, 2026-11-10):** the C player with `MUSIC_CDFJ` 1, for the ARM code of a CDFJ game: it mixes nothing and calls `MUSIC_CDFJ_WAVE(voice, samples)` when a voice's 32 samples change (its waveform at its level, as in DPC+) and `MUSIC_CDFJ_NOTE(voice, frequency, restart)` every frame a voice sounds (the frequency for `setNote`, and whether a note started, for `resetWave`). The game copies the samples to the voice's wave in display RAM. Tested on the computer with hooks that write down what they are told, against the editor's player, and in Stella with the CDFJ example (a CDFJ driver found in a public repository, used only for the test): AUDV0 takes values 0-15 and the same song as the DPC+ example. Each sample is scaled with a multiplication (x 547 >> 14 is / 30 for these values): the ARM7 has no divide instruction, and libgcc's division for it is ARM code, which Stella's ARM emulator does not run. Not tried on a Harmony or Melody cartridge.
6. Finer pitch: switching AUDF values within a frame (TIA) or per scanline (ELF).

## Decisions (2026-11-09)

1. **Both views:** a tracker grid to write in, and a piano roll of the same pattern that shows each note's tuning.
2. **Only VCSIDE's own players** (CC0), so games carry no licence notice. TIATracker songs are imported, not exported.
3. **All of it in 0.2.0:** TIA with tuning, samples for ELF, DPC+ and CDFJ export, TIATracker import and finer pitch by switching AUDF values.
