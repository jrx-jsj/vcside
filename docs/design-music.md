# VCSIDE — music editor (0.2.0)

This is a proposal, not a decision record yet. It covers what already exists, what the 2600 can do, and what VCSIDE would add. The questions at the end still need an answer before work starts.

## What already exists

| Tool | What it is | Licence | Use for VCSIDE |
|---|---|---|---|
| TIATracker (Andre "Kylearan" Wichmann, 2016-2017) | A tracker for TIA music, written in Qt, with its own player routine. It exports to DASM, K65 and CSV. Its songs are `.ttt` files (JSON). | Program: GPL-2.0-or-later. Player (`player/`): Apache-2.0. | The format is open and readable, so VCSIDE can **import `.ttt`**. Its player could be offered as an export target, with its Apache notice. VCSIDE does not reuse any of its code or interface. |
| Furnace (tildearrow) | A multi-system tracker with a TIA mode. It also exports to TIunA, a software pitch driver. | GPL-2.0-or-later | Out of scope: its `.fur` files are a compressed binary format covering dozens of chips. Anyone who prefers Furnace can keep using it, and include its export as an ordinary source file. |
| TIunA (AYCE, in "Twin", 2024) | A driver that switches between two AUDF values at a fast rate to get pitches in between them. | MIT | Shows the idea of finer pitch on plain TIA by switching. VCSIDE would write its own version. |
| TIA Music Explorer (pizzatari) | A web table of TIA pitches against piano keys, with A4 and transposition settings. | None stated | Reference only. |
| Paul Slocum's music kit and tuning charts | Hand-picked "in tune" sets of AUDF values for some distortions. | None stated | Reference only: the tables are computed here from the formula. |
| DPC, DPC+, CDF, CDFJ music drivers | The cartridge mixes 3 voices into one TIA channel. DPC (Pitfall II) plays square waves. DPC+ and CDFJ play 32-sample waveforms. The 6507 copies the amplitude to AUDV0 on every scanline. | Varies | VCSIDE would export data for them, not the drivers themselves. |

**Recommendation:** build VCSIDE's own editor, as with the graphics editor, and **import TIATracker files**. What no existing tool offers, and what makes the editor worth having inside an IDE:
- tuning shown and solved for the whole song;
- samples for ELF and other ARM cartridges;
- export straight into DASM, batari Basic and C projects, kept up to date on save as `.vcsgfx` is.

## The TIA's pitches

The TIA has two audio clocks per scanline: 31,399.5 Hz on NTSC (3,579,545 / 114) and 31,113.1 Hz on PAL (3,546,894 / 114). A tone's frequency is:

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
| 15 | 5-bit poly ÷6 | 186 | 5 – 169 Hz |
| 2, 3 | 465-step poly (rumble) | 465 | low noise |
| 8 | 9-bit poly | 511 | white noise |
| 0, 11 | constant: AUDV becomes the output level | — | used for samples |

The dividers are integers, so only some AUDF values land near equal-tempered notes. For example, with a ÷6 square on NTSC, only 5 of the 32 values are within 10 cents of an A440 note. PAL is a little worse.

### How the editor shows it

- **Absolute.** Each note shows the closest TIA pitch for every distortion, and how many cents it is off A440. The piano keyboard and the note grid colour each key and note by how far off it is: in tune, slightly off, clearly off, out of range.
- **Relative.** What the ear notices in a song is mostly the intervals between its notes, not the distance to A440. The editor measures every note against the song's own reference. It shows the error of each note and the spread: worst note, and the mean of the errors.
- **Automatic tuning.** "Tune the song" tries every transposition (0–11 semitones) and every reference from −50 to +50 cents. For each note it may use any of the distortions the instrument allows. It keeps the choice with the smallest worst-note error, or the smallest mean error (the user picks which). In a C major melody over two octaves with only ÷6 and square:

  | Reference | Worst note | Mean |
  |---|---|---|
  | A440 | 46 cents | 14 |
  | +14 cents | 32 cents | 23 |

  Adding the pulse and buzzy distortions brings the worst note down to 27 cents. Neither criterion is right for every song, so both are offered.
- **NTSC and PAL** have different clocks. The song is tuned for the project's standard. With "Multiple (in the code)", two AUDF tables are written, and the editor shows the errors of both.
- **Finer pitch** (later, optional): switching between two AUDF values in the same frame, as TIunA does, or as the cartridge's ARM can do on every scanline. It costs CPU time, so it is an instrument setting, not a default.

## Samples: ELF and the ARM cartridges

With AUDC = 0 the TIA outputs AUDV as a level, so a program that writes AUDV0 on every scanline plays a 4-bit sample. NTSC gives 15,700 samples per second, PAL 15,600. On an ELF cartridge the ARM can compute that sample itself, which means **real synthesis**:
- **Voices:** several voices, each with a 32-bit phase accumulator, so pitch is exact. Out-of-tune notes become a TIA-only problem.
- **Waveforms:**
  - square with adjustable duty;
  - triangle;
  - sawtooth;
  - sine;
  - noise, from an LFSR with a choice of lengths, including the TIA's own 4-, 5- and 9-bit sequences;
  - user-drawn waveforms of 32 samples.
- **Shaping:** volume envelopes (attack, decay, sustain, release) and pitch effects (vibrato, slide, arpeggio).
- **Mixing:** the voices are added, scaled and limited to 16 levels, with dithering as an option. Two channels (AUDV0 + AUDV1) give more levels. The TIA mixes the channels non-linearly, and that is taken into account. Players that keep one channel free for sound effects use only one.
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
- **Composition:** tracker grid or piano roll (open question 1). Either way:
  - two channels on the TIA, or N voices with samples;
  - patterns reused in an order list;
  - speed in video frames per row, with swing (alternating speeds);
  - a piano keyboard coloured by tuning;
  - input from the computer keyboard.
- **Listening:**
  - VCSIDE synthesises the sound itself, from the published behaviour of the TIA's audio counters (written for VCSIDE, not copied from an emulator), through SDL3's audio.
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

## Phases for 0.2.0

1. Tuning maths and TIA sound synthesis, with tests. The cents tables are computed and checked against the formula and against Stella recordings.
2. The editor: TIA instruments, two channels, patterns, listening, tuning views and automatic tuning.
3. DASM and batari Basic export, and the DASM example. Checked in Stella and Gopher2600.
4. Samples for ELF: waveforms, voices, mixing, the C player and the C example. Checked in Stella; on the PlusCart and the Chameleon if possible.
5. TIATracker import; DPC+ and CDFJ export.

## Open questions

1. **Tracker grid, piano roll, or both?** A grid is closer to how 2600 music is usually written and to the data the player reads. A piano roll makes tuning easier to see.
2. **Players:** write VCSIDE's own (CC0, no conditions for games), or also offer TIATracker's (Apache-2.0, needs its notice in the game)?
3. **Scope of 0.2.0:**
   - Is TIA plus ELF samples enough, with DPC+, CDFJ and the TIATracker import left for 0.3.0?
   - Is finer pitch (switching AUDF values) in or out?
