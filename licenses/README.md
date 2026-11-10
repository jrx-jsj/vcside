# Licences

## VCSIDE

| Part | Licence | Text |
|---|---|---|
| Code (`src/`, `tests/`, `tools/`, `data/i18n/` catalogues) | GPL-3.0-or-later | [`LICENSE`](../LICENSE) |
| Icon (`assets/icon/`, `data/vcside.png`) | GPL-3.0-or-later | [`LICENSE`](../LICENSE) |
| Knowledge base (`data/kb/`) | CC BY-SA 4.0 | `CC-BY-SA-4.0.txt` |
| Project templates (`data/templates/`) | CC0 1.0 (public domain) | `CC0-1.0.txt` |
| Themes (`data/themes/`) | CC0 1.0 (public domain), except the palette of the two Solarized themes | `CC0-1.0.txt`; Solarized palette: MIT, `solarized.txt` |

Games made with VCSIDE do not inherit any of these licences.

Texts downloaded from the official sources (gnu.org, creativecommons.org) on 2026-11-08; the GPL-3 text matches Gopher2600's `COPYING` byte for byte.

## Third parties

VCSIDE links against these libraries. Their licences ship with the executable: CMake copies them into `data/licenses` (on macOS, inside `VCSIDE.app/Contents/Resources/data/licenses`).

| Library | Version | Licence | File |
|---|---|---|---|
| [SDL 3](https://libsdl.org) | 3.2+ | zlib | `sdl3.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.9b (docking) | MIT | `imgui.txt` |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | `nlohmann-json.txt` |
| [llama.cpp and ggml](https://github.com/ggml-org/llama.cpp) (the language-model helper, built from source with the IDE and run as a separate program) | tag set in `cmake/llm_helper.cmake` | MIT | `llama.cpp-MIT.txt` |
| [stb_image](https://github.com/nothings/stb) | 2.30 (commit `4e9b368`) | MIT or public domain (your choice) | `stb.txt` |

The language models themselves are **not** included. The assistant offers to download them (with your confirmation) from their publishers: the catalogue in `data/assistant/models.json` lists only models under Apache-2.0 for now (Qwen3, Alibaba Cloud), and shows each model's licence before the download.

External tools (DASM, Stella, Gopher2600, batari Basic, wasmtime, Arm GNU toolchain, GNU Make) are **not included or redistributed**: VCSIDE uses the ones already installed and links to their official pages. Each keeps its own licence (DASM, Stella and Gopher2600 are GPL).

stb_image is only downloaded and used when SDL 3 is older than 3.4 (3.4 already ships `SDL_LoadPNG`): it is used to read the window icon.

The icon is an original VCSIDE drawing: a stylised console with six switches and a cartridge, with no logos or trademarks. The "VCS" lettering uses a block typeface made for the icon.

The Solarized Light and Solarized Dark themes take the 16 palette values from [Solarized](https://ethanschoonover.com/solarized/) by Ethan Schoonover (MIT, `solarized.txt`); how they are distributed among VCSIDE's colours is our own.

The typefaces are the system ones (Menlo, SF, Segoe UI, Consolas, DejaVu): they are loaded at run time and are not redistributed.

DASM's `vcs.h` and `macro.h` are not included (the former expressly asks not to be redistributed); the template ships `2600.h`, written for VCSIDE with the register names from Atari's *Stella Programmer's Guide* (1979), which are the same ones used by programs written with `vcs.h`.

The sources of the knowledge base are in [`docs/sources.md`](../docs/sources.md).
