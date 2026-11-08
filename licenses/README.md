# Licencias

## VCSIDE

| Parte | Licencia | Texto |
|---|---|---|
| Código (`src/`, `tests/`, `tools/`, catálogos `data/i18n/`) | GPL-3.0-or-later | [`LICENSE`](../LICENSE) |
| Base de conocimiento (`data/kb/`) | CC BY-SA 4.0 | `CC-BY-SA-4.0.txt` |
| Plantillas de proyecto (`data/templates/`) | CC0 1.0 (dominio público) | `CC0-1.0.txt` |

Los juegos hechos con VCSIDE no heredan ninguna de estas licencias.

Textos descargados de las fuentes oficiales (gnu.org, creativecommons.org) el 2026-11-08; el de la GPL-3 coincide byte a byte con el `COPYING` de Gopher2600.

## Terceros

VCSIDE se enlaza con estas bibliotecas. Sus licencias acompañan al ejecutable: CMake las copia en `data/licenses` (en macOS, dentro de `VCSIDE.app/Contents/Resources/data/licenses`).

| Biblioteca | Versión | Licencia | Archivo |
|---|---|---|---|
| [SDL 3](https://libsdl.org) | 3.2+ | zlib | `sdl3.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.9b (docking) | MIT | `imgui.txt` |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | `nlohmann-json.txt` |

Herramientas externas (DASM, Stella, Gopher2600, batari Basic, Arm GNU toolchain, cc65) **no se incluyen ni se redistribuyen**: VCSIDE usa las que ya estén instaladas y enlaza a sus páginas oficiales. Cada una mantiene su licencia (DASM, Stella y Gopher2600 son GPL).

Las tipografías son las del sistema (Menlo, SF, Segoe UI, Consolas, DejaVu): se cargan en tiempo de ejecución y no se redistribuyen.

`vcs.h` y `macro.h` de DASM no se incluyen (el primero pide expresamente no redistribuirlo); la plantilla trae `hardware.h`, escrito para VCSIDE.

Las fuentes de la base de conocimiento están en [`docs/fuentes.md`](../docs/fuentes.md).
