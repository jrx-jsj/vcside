# Fuentes de la base de conocimiento

La base de conocimiento de VCSIDE (`data/kb/*.json`) se revisó el 2026-11-08. Antes, `tia.json`, `riot.json`, `formats.json` y `tv.json` se habían escrito de memoria y sin fuentes; `6502.json` se genera con `tools/gen_6502.py`. Todas las descripciones están redactadas con palabras propias: de las fuentes solo se han tomado hechos (direcciones, máscaras de bits, tamaños, nombres, recuentos de ciclos). No hay texto copiado.

Cada fichero JSON lleva un array `sources` de nivel superior (`id`, `title`, `author`, `url`, `used_for`). Cada registro, formato y estándar de TV lleva `sources` (lista de ids) y `verified` (`false` = contiene alguna afirmación que no se pudo confirmar; esas afirmaciones empiezan por «Unverified» en `issues`). En `6502.json` cada instrucción lleva `sources` y `verified`, y hay un `instruction_sources` de nivel superior.

## Fuentes y licencias

| id | Fuente | URL | Licencia / estado |
|---|---|---|---|
| `stella-guide` | *Stella Programmer's Guide*, Steve Wright, 12/03/1979 (texto reconstruido por Charles Sinnett, 1993) | https://atarihq.com/danb/files/stella.pdf | Documento de 1979 distribuido libremente por la comunidad. Los derechos de autor (Atari) no están aclarados: solo se han tomado hechos. |
| `towers-tia` | *Atari 2600 TIA Hardware Notes* v1.0, Andrew Towers (2003) | https://www.retroisle.com/atari/vcs/Technical/Hardware/TIAnotes.php | Distribuido libremente; no hay licencia explícita en las páginas consultadas. Solo se han tomado hechos. Se leyó mediante un extracto (la página se corta a media frase). |
| `mos6532` | Hoja de datos *MOS 6532 RIOT* | https://git.applefritter.com/6502/EightBit/raw/branch/master/M6532/documentation/mos_6532_riot.pdf | Hoja de datos de MOS/Commodore, ampliamente archivada; sin licencia explícita (estado dudoso, solo hechos). |
| `mcs6500-manual` | *MCS6500 Microcomputer Family* (conjunto de hojas de datos de la familia 6500) | http://archive.6502.org/datasheets/mos_6500_mpu_nov_1985.pdf | Descargado (6 MB) pero no leído página a página en esta pasada; las afirmaciones sobre instrucciones oficiales se contrastaron con masswerk, Gopher2600 y DASM. Estado de derechos dudoso, solo hechos. |
| `masswerk` | *6502 Instruction Set*, masswerk.at | https://www.masswerk.at/6502/6502_instruction_set.html | Sin aviso de licencia visible en la parte leída (las últimas 39 000 caracteres de la página no se leyeron). Solo hechos (tamaños, ciclos, qué opcodes son inestables). |
| `nms` | *No More Secrets – NMOS 6510 Unintended Opcodes* v0.91 (2016), groepaz / solution | http://csdb.dk/getinternalfile.php/153783/NoMoreSecrets-NMOS6510UnintendedOpcodes-20162412.pdf | Documento publicado en CSDb; sin licencia explícita (estado dudoso, solo hechos). La v0.96 (2021) no se pudo descargar (HTTP 403). |
| `clark-decimal` | *Decimal Mode*, Bruce Clark | http://www.6502.org/tutorials/decimal_mode.html | Citado de conocimiento propio, no reabierto en esta pasada. Documento de 6502.org; licencia no comprobada. |
| `horton` | *Cart Information* (tamaños y bankswitching) v6.00, Kevin Horton, 1997 | https://atariage.com/2600/programming/bankswitch_sizes.txt | Texto de la comunidad sin licencia explícita; solo hechos. Tiene dos erratas (E7: selector de bloque RAM en `$1FF8-$1FFB`; E0: tercer rango `$1FF0-$1FF8`). |
| `stella-src` | Código fuente de Stella (`Bankswitch.hxx`, `Cart*.hxx/cxx`, `CartDetector.cxx`, `M6532.cxx`, `TIA.cxx`) | https://github.com/stella-emu/stella | GPL-2, solo consultado; se citan comportamientos, no se copia texto ni código. |
| `gopher-src` | Código fuente de Gopher2600, copia local en commit `07d8de5` (2026-11-05) | https://github.com/JetSetIlly/Gopher2600 | GPL-3, solo consultado; ídem. |
| `gopher-bin` | Pruebas de comportamiento con el binario local de Gopher2600 | (mismo repositorio) | Mediciones propias con ROMs de prueba ensambladas con DASM. |
| `stella-7.0-help`, `stella-7.0-manual` | Stella 7.0: `stella -help`, manual (`docs/index.html`) y código (`Settings.cxx`, `main.cxx`, `OSystem*.cxx`, `Props.cxx`) | https://stella-emu.github.io/docs/index.html | GPL-2, solo consultado: nombres y valores de las opciones de arranque (`emulators.json`), qué opciones se guardan, carpeta base de cada sistema. Las etiquetas y descripciones son propias. |
| `gopher2600-0.56.1` | Gopher2600 v0.56.1: `RUN/DEBUG -help`, ventana de preferencias y paquete `resources` (modo portátil) | https://github.com/JetSetIlly/Gopher2600/tree/v0.56.1 | GPL-3, solo consultado: opciones, claves de preferencias y su significado. |
| `elf-template` | Plantilla ELF de VCSIDE (`data/templates/elf`): nombres y argumentos de vcsLib, `elf_main(args)` y sus constantes, comportamiento del cargador | Código de Stella (`CartELF.cxx`, `VcsLib.cxx`) y Gopher2600, y pruebas propias en los juegos ELF de esta carpeta | Interfaces (nombres de funciones y valores), no texto: `vcs.h` y el resto se escribieron de cero y son CC0. La plantilla del Chameleon Cart (ZackAttack) no tiene licencia y no se incluye: el Makefile la toma de una carpeta que indica el usuario. |
| `bb-1.9-src` | batari Basic 1.9: código del compilador (`keywords.c`, `statements.c`, `2600bas.c`), guiones `2600basic.sh/.bat` y compilaciones de prueba | https://github.com/batari-Basic/batari-Basic | GPL-2, solo consultado y usado como herramienta: nombres de las palabras clave, formato de los mensajes y de las salidas. Los resúmenes de `batari-basic.json` son propios. El ensamblador que genera y sus *includes* son CC0 según su licencia; los juegos son de sus autores. |
| `dasm` | DASM 2.20.14.1 (`/opt/homebrew/bin/dasm`) | https://github.com/dasm-assembler/dasm | GPL-2+, solo usado como herramienta: se ensambló cada opcode. |
| `stella-4a50` | *4A50 bankswitching*, John Payson, alojado por el proyecto Stella | https://stella-emu.github.io/4A50.html | Solo enlazado; la página no se abrió. |
| `cdf-authors` | Cabeceras de documentación de Stella sobre DPC+, CDF/CDFJ/CDFJ+ y BUS (autores: Darrell Spice Jr, Chris Walton, Fred Quimby, John Champeau y otros) | https://github.com/stella-emu/stella/tree/master/src/emucore | GPL-2 (comentarios del código de Stella). Las páginas de los propios autores en AtariAge no estaban disponibles; se usó el resumen de Stella. |

## Qué se tomó de cada fuente

- **Stella Programmer's Guide**: mapa de registros de escritura y lectura de la TIA, significado de los bits, codificación de HMxx, tabla de AUDC (los 16 valores), bits de VBLANK, prioridad y modo SCORE, puertos de entrada INPT0-5, direcciones y bits de SWCHA/SWCHB (D7 = dificultad P1, D6 = dificultad P0), direcciones de temporizador (`$294-$297`), 262/312 líneas, 76 ciclos por línea, la regla de 24 ciclos tras HMOVE, la tabla SECAM de 8 colores.
- **Towers**: retardo de 5 relojes de color de los jugadores tras RESPx (4 en misiles/bola), posiciones de las copias de NUSIZ (16, 32 y 64 píxeles), HMOVE extiende el HBLANK 8 relojes, HMOVE tardío en el ciclo 74 (movimiento solo a la izquierda), RESBL redispara la bola.
- **Hoja de datos del 6532**: A0 = polaridad y A1 = habilitación de IRQ en el control de borde PA7 (el fichero previo lo tenía al revés), A3 habilita la IRQ del temporizador, el temporizador expira N·D + 1 ciclos después, bandera de temporizador borrada al leer o escribir el temporizador, bandera PA7 borrada al leer el registro de banderas.
- **masswerk, NMS y definiciones de Gopher2600**: bytes, ciclos y penalización de página de los 256 opcodes (coinciden las tres), clasificación estable/inestable. El único cambio: LAS es estable (solo SHA, SHX, SHY, SHS/TAS, ANE y LXA son inestables).
- **DASM**: se ensambló cada opcode para comprobar el nombre y los bytes. 36 opcodes no se pueden generar con mnemónico (marcados con `"dasm": false`; hay que usar `.byte`): los 12 JAM, el USBC `$EB`, `$2B` (ANC), los NOP de 1 byte y varios NOP con operando. DASM no acepta `ASL A` con operando (hay que escribir `ASL`); el nombre DASM para `$4B` es `ASR`, no `ALR`.
- **Stella (código)**: nombres de tipo de cartucho de `-bs`, tamaños admitidos, hotspots de FA, FA2, UA, 0840, SB, X07, EF, DF, BF, E0, E7, 3E, 3E+, CDF, DPC+, BUS, detección por cadenas y por tamaño, detección de Superchip, lectura del puerto de escritura de SC = escritura no deseada, decodificación del RIOT (`M6532.cxx`).
- **Gopher2600 (código)**: ids de `-mapping` aceptados (`cartridge.go`: no existen F0, 0840, X07, 4A50 ni BUS), huella de ELF/CDF/DPC+/3E+/AR/SC, tabla de hotspots de CDF y E7, nombres vcsLib del cargador ELF, recuentos de líneas por especificación de TV.
- **Pruebas con el binario de Gopher2600** (programas de prueba propios): posición de RESP0 = 3·N − 68 + 5 (N = ciclo de escritura; N = 23 da 6), RESM0/RESBL = 3·N − 68 + 4, en HBLANK sin HMOVE el jugador queda en 3 y misil/bola en 2; WSYNC escrito en los ciclos 72, 74 o 75 reanuda en el ciclo 0 de la línea siguiente, y escrito en el ciclo 0 espera una línea entera.

## Errores principales corregidos

1. **WSYNC en el ciclo 75**: la nota decía que la espera era «muy corta»; en realidad la espera termina igual al inicio de la línea siguiente (la parada es de menos de un ciclo) y solo una escritura en el ciclo 0 espera una línea completa.
2. **Misiles y NUSIZ**: el fichero decía que los misiles *no* se replican con las copias del jugador; la guía dice lo contrario (comparten los mismos decodificadores).
3. **RIOT, control de borde PA7**: A0 es la polaridad y A1 la habilitación (estaba al revés). El temporizador expira N·D + 1 ciclos después (datos de la hoja de datos), y el flag de temporizador solo lo borra leer INTIM o escribir un temporizador.
4. **PHP en la pila**: «escribe varios registros TIA en 3 ciclos» era falso; escribe uno, y el valor es P (el bit 1 es Z, de ahí el truco de ENAM0/ENAM1/ENABL).
5. **LAS** estaba clasificado como inestable; es estable. **BRK** pasa a 1 byte (DASM emite solo `$00`). **JAM** sigue marcado como inestable por seguridad (cuelga la CPU), con `hangs: true`.
6. **Formatos**: 3E+ admite hasta 64 KB de ROM y 32 KB de RAM (no 512 KB); E7 tiene 2 KB de RAM (no 1 KB) y el selector del bloque de 256 bytes es `$1FE8-$1FEB`; la detección de Superchip compara los primeros 128 bytes con los siguientes 128 bytes de cada banco (no «todos iguales»); F0/MB, 0840, X07, 4A50 y BUS **no** existen en Gopher2600; el id de Gopher para F8SC es `F8SC` (no `F8S`); CDFJ y CDFJ+ son el mismo tipo `CDF` en Stella; FA2 admite 24-32 KB; añadidas las entradas BF y CV.
7. **Entradas de INPT/CX**: eliminadas las afirmaciones sin fuente («380 scanlines», 7800, «los objetos fuera de pantalla no colisionan»).

## Cambios en números

- `6502.json`: 256 opcodes revisados contra 3 fuentes; 77 instrucciones con `sources`/`verified`; 1 byte-size corregido (BRK), 1 clasificación corregida (LAS); 36 opcodes marcados `dasm: false`; ~25 descripciones reescritas.
- `tia.json`: 59 registros con `sources`/`verified`; notas reescritas (16); unos 40 registros con descripción o `issues` corregidos; 22 con `verified: false`.
- `riot.json`: 12 registros; notas reescritas; ~10 con textos corregidos; 1 con `verified: false` (`TIM64T`, valores idiomáticos).
- `formats.json`: 34 formatos (32 + BF + CV); casi todos con texto reescrito; 8 con `verified: false`; plantillas SC corregidas (los primeros 256 bytes de cada banco quedan reservados).
- `tv.json`: 4 estándares; notas reescritas; solo PAL con `verified: false` (hues grises 0, 1, 14, 15).

## Hechos que siguen sin verificar

- Posición de un objeto reiniciado durante HBLANK (la guía dice «borde izquierdo»; Gopher2600 usa 3 para jugadores y 2 para misiles/bola; no medido en hardware real) y su variación con HMOVE y la revisión de la TIA.
- Efecto exacto de HMOVE tardío en ciclos distintos del 74, y HMOVE a mitad de línea visible.
- RSYNC: retardo exacto y su efecto en los strobes RESxx.
- Cambios de NUSIZ a mitad de línea y retardos adicionales de los jugadores doble y cuádruple.
- Ventanas de reescritura de PF0/PF1/PF2 a mitad de línea (derivadas de las posiciones de píxel, no medidas).
- Retardo de carga de los pines INPT0-3 (barrido completo de una pala) y comportamiento de los bits no conducidos de los registros de lectura.
- AUDV: si el cambio se aplica al instante o en el siguiente reloj de audio (se usa para muestras PCM).
- Comportamiento de INTIM leído justo en el ciclo en que da la vuelta (0 → `$FF`), y valores de TIM64T «idiomáticos» (43, 35/36).
- Formatos dudosos: FA (el comentario de Stella dice que D0 debe ser 1; el código lo tiene comentado), FA2 (número de bancos por variante), BUS (siete bancos según el comentario, seis hotspots en el código), DF/DFSC (Gopher2600 no tiene id propio), CV (título/layout solo según Stella), 4A50 (direcciones exactas de hotspots), F8 (banco de arranque real).
- ELF: lo dicho sobre el firmware 2.3.32 del PlusCart, las versiones antiguas de Gopher2600 con código en RAM y los tamaños del Chameleon vienen de las notas del proyecto, no de una fuente pública.
- Colores PAL: hues 0, 1, 14 y 15 como grises (no comprobado en un documento primario).
- MCS6500 (hoja de datos de 6 MB) y la tutorial de modo decimal de Bruce Clark no se leyeron en esta pasada.
- La versión 0.96 de *No More Secrets* no se pudo descargar; se usó la 0.91.

## Fuentes con licencia dudosa

Todas las fuentes documentales (Stella Programmer's Guide, notas de Towers, hoja de datos del 6532, MCS6500, NMS, Horton, masswerk) carecen de licencia explícita en los ejemplares consultados; se han usado solo para extraer hechos y se han reescrito siempre con palabras propias. Stella (GPL-2), Gopher2600 (GPL-3) y DASM (GPL-2+) se han consultado o ejecutado sin copiar texto ni código.
