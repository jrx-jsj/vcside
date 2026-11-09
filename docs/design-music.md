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
- **Output on save:**
  - **DASM:** `<name>.music.asm` with the data tables and a player written for VCSIDE (CC0), called once per frame.
  - **batari Basic:** `data` tables and a player as an include for the standard kernel. In DPC+, its music data.
  - **C (ELF):** `<name>.music.h` with the song and the synthesis player, ready to call once per scanline from the kernel and once per frame.
  - Each file states its size in ROM and RAM, so the space report can show it.
- **Import:**
  - TIATracker `.ttt`: its instruments, percussion and patterns.
  - MIDI files (later): notes per channel, then tuned.
- **Example project:** a short song in the DASM and C templates, as with the animated sprite.

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
| Finer pitch by switching AUDF values (fractional bits) | Aleksi Eeben, Tiatonic Tuning in Star Bars (2023); Natt (AYCE), TIunA (2024) |
| Exact pitch by software dividers | utz, TIAtune (2017), with Thomas Jentzsch (2022) |
| 3 voices mixed by the cartridge into one channel | David Crane, DPC in Pitfall II (Activision, 1984) |
| Waveforms in the cartridge's music; ADSR by shaping them | Darrell Spice Jr. and Fred Quimby, DPC+ (Stay Frosty 2); reused in CDF and CDFJ |
| 4-bit samples by writing AUDV with AUDC = 0 | Still to be found |
| More than 4 bits by sampling the volume between audio clocks | Discussed in AtariAge's "8-bit digital audio from 2600" thread; authors still to be credited |
| Direct digital synthesis (phase accumulator) | A general technique with no single author |

## Phases for 0.2.0

1. Tuning maths and TIA sound synthesis, with tests. Each credit is checked against a primary source and recorded in `docs/sources.md`. The cents tables are computed and checked against the formula and against Stella recordings.
2. The editor: TIA instruments, two channels, patterns, listening, tuning views and automatic tuning.
3. DASM and batari Basic export, and the DASM example. Checked in Stella and Gopher2600.
4. Samples for ELF: waveforms, voices, mixing, the C player and the C example. Checked in Stella; on the PlusCart and the Chameleon if possible.
5. TIATracker import; DPC+ and CDFJ export.
6. Finer pitch: switching AUDF values within a frame (TIA) or per scanline (ELF).

## Decisions (2026-11-09)

1. **Both views:** a tracker grid to write in, and a piano roll of the same pattern that shows each note's tuning.
2. **Only VCSIDE's own players** (CC0), so games carry no licence notice. TIATracker songs are imported, not exported.
3. **All of it in 0.2.0:** TIA with tuning, samples for ELF, DPC+ and CDFJ export, TIATracker import and finer pitch by switching AUDF values.
