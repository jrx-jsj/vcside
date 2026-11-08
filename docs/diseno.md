# VCSIDE — diseño

IDE minimalista y nativo para hacer juegos de Atari 2600 (más adelante, 7800 con el Chameleon Cart). No deriva de VS Code ni de ningún editor web: un solo ejecutable que funciona igual en **macOS, Windows y Linux**.

## Decisiones

| Tema | Decisión | Motivo |
|---|---|---|
| Lenguaje | C++17 | Compila con clang, gcc y MSVC; mismo código en los tres sistemas. |
| Interfaz | Dear ImGui (rama *docking*) + SDL3 + OpenGL 3.2 | Ligero, sin dependencias del sistema, paneles acoplables, fácil de dibujar editores gráficos (sprites, playfield). |
| Procesos | `SDL_Process` de SDL3 | Lanzar ensambladores y emuladores y leer su salida sin código distinto por sistema. |
| Editor de texto | ImGuiColorTextEdit (adaptado) | Resaltado propio para DASM, batariBasic y C; hooks para autocompletado y selección de bloques. |
| Menús | Descritos como datos (`src/menu.*`): ImGui los dibuja en Windows y Linux; en macOS van a la barra de menús del sistema (`src/platform/menu_macos.mm`) | En macOS todas las aplicaciones usan la barra del sistema. Los atajos los sigue atendiendo la aplicación (el menú solo los muestra), así hay un único sitio para cada atajo; los del menú de la aplicación y de Ventana (Cmd+Q, Cmd+H, Cmd+M…) son los del sistema. |
| Datos | JSON (nlohmann/json) | Proyectos (`*.vcsproj`), ajustes y base de conocimiento legibles y versionables. |
| Construcción | CMake ≥ 3.24 + FetchContent | SDL3 del sistema si está; si no, se descarga. ImGui, el editor y JSON siempre se descargan con versión fija. |
| Idioma | Inglés como idioma fuente; catálogos `data/i18n/<código>.json` con el texto inglés como clave | Añadir un idioma es añadir un archivo; los catálogos pueden ser parciales (lo que falta sale en inglés); `F()` con `{0}`, `{1}`… deja reordenar las frases; `tools/i18n_check.py` lista lo que falta. La base de conocimiento se escribe en inglés (más adelante, capas `data/kb/<código>/` con el mismo respaldo). |
| Opcodes ilegales | Separados en los datos (`illegal`, `stable`) y en la interfaz (colores, grupos, avisos) | Cada proyecto elige: permitir, solo estables, avisar o prohibir. |
| Fuentes y licencias | Todo dato de la base de conocimiento cita su fuente (`docs/fuentes.md`); nada se copia literalmente | Las dependencias (zlib/MIT) llevan su licencia junto al ejecutable; las herramientas GPL no se redistribuyen. |

## Módulos

```
src/
  main.cpp          ventana SDL3 + bucle ImGui
  app.*             estado global, menús, paneles acoplados
  project.*         workspace y proyecto (.vcsproj): archivos, toolchain, formato, TV
  toolchain.*       detección de herramientas (PATH, rutas típicas por sistema, ajustes del usuario)
  builder.*         ejecuta la compilación y analiza errores (archivo:línea → saltar al editor)
  emulator.*        lanza Stella / Gopher2600 con la ROM, el formato y la TV del proyecto
  kb.*              base de conocimiento: opcodes, registros, formatos, normas de TV
  cycles.*          contador de ciclos de una selección de ensamblador
  editor/*          pestañas de código, resaltado, autocompletado, ayuda flotante
  tools/*           editores de sprites, playfield y sonido (integrados o externos)
data/kb/            6502.json, tia.json, riot.json, formats.json, tv.json
```

## Windows y Linux

- **Compilación:** en Docker (`tools/docker/`), desde cualquier sistema. Linux: Debian trixie con su SDL3. Windows: MinGW-w64 desde Debian, enlazado estático con SDL3 3.4.18 compilado aparte; el `.exe` no necesita DLL.
- **Pruebas:** las de `vcside_tests` pasan en los tres. La de Linux se ve con Xvfb y Mesa (`xvfb-run`); la de Windows, en Wine 6 sobre macOS, que solo da OpenGL 3.2 core o 2.1 (de ahí los intentos 3.0 → 3.2 core → 2.1).
- **`process.h`:** el nombre choca con la cabecera de Windows que incluyen los hilos de MinGW; por eso es `child_process.h`.
- **Procesos en Windows:** SDL entrecomilla cada argumento al llamar a `CreateProcess`, y `cmd /c` no entiende eso. Para `cmd /c ...` (las órdenes de compilación propias y el `2600bas.bat` de batari Basic) VCSIDE escribe la línea completa, `cmd.exe /c ""C:\a b\x.bat" "main.bas""`, con `SDL_PROP_PROCESS_CREATE_CMDLINE_STRING` (SDL 3.4).
- **Sin probar en un Windows de verdad** ni con un emulador allí: solo en Wine.

## Herramientas

Cada herramienta se busca en este orden: ruta fijada en ajustes → `PATH` → rutas típicas del sistema (p. ej. `/Applications/Stella.app`, `C:\Program Files\Stella`, `/usr/games`). Si no está, el IDE ofrece la página oficial de descarga y la carpeta donde dejarla (`~/.vcside/tools/<herramienta>`); más adelante, descarga directa de las versiones publicadas con su suma de comprobación.

| Herramienta | Uso | Notas |
|---|---|---|
| DASM | ensamblador 6502 | `dasm main.asm -f3 -omain.bin -lmain.lst -smain.sym`; errores `file (line): error:` |
| batari Basic 1.9 | BASIC → DASM | versión oficial en WebAssembly: `2600basic.sh/.bat` y wasmtime; trae su propio DASM |
| ELF-VCS (arm-none-eabi-gcc + vcsLib) | C para cartuchos ELF | plantilla con el Makefile de las reglas ELF del Chameleon/PlusCart |
| Tiny-ELF PXE Basic | BASIC para ELF | — |
| cc65/ca65 | alternativa a DASM | opcional |
| Stella | ejecutar y depurar | `stella -format <tipo> -tv.* ... rom.bin`; símbolos `.sym` y listado `.lst` junto a la ROM |
| Gopher2600 | ejecutar y depurar | binario `gopher2600`; ELF con el *nightly* 07d8de5 o posterior |

## Base de conocimiento

- **6502**: los 151 opcodes oficiales y los ilegales estables, con modos, bytes, ciclos, +1 al cruzar página / saltar, banderas y descripción.
- **TIA y RIOT**: cada registro con dirección, lectura/escritura, bits, uso, temporización (p. ej. `RESP0` en el ciclo de color) y problemas conocidos (HMOVE en ciclo 73, `RESBL` en HBLANK, colisiones, diferencias entre TIA).
- **Formatos**: 2K, 4K, F8, F6, F4, FE, E0, E7, 3F, 3E, FA, DPC, DPC+, CDFJ/CDFJ+, ELF; con *hotspots*, RAM extra, detección y el nombre que esperan Stella y Gopher2600.
- **TV**: NTSC, PAL, PAL60, SECAM: líneas, ritmo de cuadro, paletas y consejos.

El autocompletado, la ayuda flotante y el panel de documentación salen de estos JSON.

## Contador de ciclos

Al seleccionar un bloque de ensamblador se suman los ciclos mínimos y máximos (cruce de página, ramas tomadas), los bytes y la posición en la línea (76 ciclos por línea, 228 relojes de color). Se muestra en la barra de estado y como columna junto al código.

## Emuladores y depuración

Comprobado con Stella 7.0 y Gopher2600 v0.56.1, en su código fuente y probándolos.

- **Símbolos:** los dos cargan solos el `.sym` que está junto a la ROM con su mismo nombre. Stella, además, el `.lst`.
- **Puntos de ruptura:** se ponen en el margen o con F9 y se guardan en `<proyecto>.vcsproj.user`.
  - Con F6, cada uno se lleva a la primera instrucción de su línea o de las siguientes.
  - La dirección y el banco salen del listado de DASM: el listado da la dirección de `ORG`, así que se sigue `RORG`/`REND` en cada segmento. Las líneas de una macro se asignan a la línea que la llama.
- **Gopher2600:** `DEBUG -script <archivo>` ejecuta el guion al arrancar el depurador, con líneas `BREAK PC $F100 & BANK 1`. No guarda nada en su configuración.
- **Stella:**
  - **Guarda todo:** las opciones de la línea de órdenes se guardan en su configuración al salir (`Settings::load`/`save`); solo se libran las locales (`-basedir`, `-break`, `-debug`, `-holdreset`, `-holdselect`) y las propiedades de la ROM (`-bs`, `-format`, `-tv`, `-ld`, `-lc`…).
  - **Copia propia:** por eso VCSIDE lo arranca con una copia de la configuración del usuario, renovada en cada arranque, en `<config de VCSIDE>/emulators/`. En las versiones Unix y Windows con `-basedir`; en Stella.app, que no hace caso de `-basedir` (está quitado con `#if 0`), con `HOME` apuntando a la copia, porque busca `~/Library/Application Support/Stella` a través de `HOME`.
  - **Puntos de ruptura:** el `<rom>.script` que ejecuta al abrir el depurador se busca en su carpeta de usuario (`userdir`, `Debugger::autoExec`), no junto a la ROM como dice el manual. Con la copia propia se le pasa `-userdir build/` y VCSIDE escribe ahí `build/<rom>.script` (`break $F100 1`, todos los bancos).
  - **Capturas:** `-snapsavedir build/` salvo que el proyecto diga otra cosa.
- **Opciones de arranque:** `data/kb/emulators.json` (CC BY-SA, como el resto de la base) describe cada opción: grupo, tipo, valores, argumento de Stella (`dev: true` si necesita `-dev.settings 1`, que VCSIDE añade solo), opción o clave de preferencias de Gopher2600, y traducciones. El proyecto guarda solo las que se cambian (`"emulator_options"` y `"emulator_args"` en el `.vcsproj`); el resto queda como lo tenga el emulador.
- **Gopher2600 y preferencias:** sonido, comprobación de VSYNC, estado al azar, rarezas del TIA… solo se leen del archivo `preferences` (las preferencias por línea de órdenes son solo para la emulación de comparación). Si se cambia alguna, se arranca en modo portátil: carpeta de trabajo con `portable.txt` y `Gopher2600_UserData/` con una copia de los archivos del usuario y esas claves cambiadas.
- **Normas de TV:** NTSC, PAL, PAL60 y SECAM en los dos; NTSC50 y SECAM60 solo en Stella; PAL-M solo en Gopher2600. Si el emulador no tiene la elegida, no se pasa y lo elige él (con un aviso).
- **Ir a dirección** (Ctrl+Shift+G): de una dirección (o etiqueta) y, si se quiere, un banco, a su línea. Tiene en cuenta los espejos de la 6507 ($F000 = $1000).
- **Pendiente:** recibir del emulador la dirección donde se ha parado (no tienen un canal para avisar a otra aplicación).
- **batari Basic:** el listado es del `.asm` generado; cada línea se lleva a la del `.bas` por la última marca `;;line N;;` que la precede (lo que va antes de la primera y después del programa no es de ninguna línea).
- **ELF:** los puntos de ruptura no se pasan (ni Stella ni Gopher2600 pueden parar en una línea de C); `-bs ELF` y `-mapping ELF`.

## Proyectos ELF

- **Plantilla** (`data/templates/elf`, CC0): `Makefile`, `include/vcs.h`, `src/main.c`, `src/trampolines.S` y `chameleon/support.c`, escritos para VCSIDE siguiendo las reglas ELF de los juegos de esta carpeta (`reglas-elf.md`): Cortex-M0, `-fdata-sections` sin `-ffunction-sections`, enlace parcial con libgcc, `strip --strip-unneeded`, divisiones de libgcc renombradas, llamadas largas al cargador, trampolines de `memcpy`/`memset` y `RAM_FUNC` para el kernel. El ejemplo es un arcoíris con una barra que se mueve con el joystick; vuelve al menú con RESET en un multicart.
- **Comprobaciones tras enlazar:** cada símbolo sin definir que no sea del cargador sale como `src/<archivo>.c: error: undefined symbol '...'` (con `-r`, el enlazador no lo da por error, y Stella lo enlazaría a 0); y ninguna llamada corta al cargador.
- **Chameleon Cart:** `make chameleon CHAMELEON_FW=<carpeta chameleon_cart de su plantilla>`. La plantilla de Zack no se incluye porque no tiene licencia.
- **Errores:** además de `archivo:línea:`, los mensajes sin línea (las comprobaciones, `arm-none-eabi-gcc: error:`), los de ld (`main.c:(.text+0x1c): undefined reference`, con la función; región desbordada) y los de make que no repiten otro error. Los archivos que ld da sin carpeta se buscan en el proyecto.
- **Espacio:** VCSIDE lee el ELF directamente. De la ROM (reubicable): código, `.RamFunc`, constantes, variables con valor y a cero. Si hay `build/<rom>-chameleon.elf` más nuevo que la ROM, sus cabeceras de programa dan la flash (lo guardado desde `0x0800_0000`) y la RAM (lo que vive en `0x2000_0000`) exactas; si no, se estima con lo que ocupa el firmware de la plantilla del 2025-10-23, medido: unos 10 100 bytes de flash y 6 000 de RAM (la estimación de la Abadía queda a 41 bytes de la real). Límites del STM32G070RB: 128 KB y 36 KB.

## Proyectos batari Basic

- **Compilador:** batari Basic 1.9 en WebAssembly (GPL-2), que se ejecuta con wasmtime. Se busca en *Ajustes → Herramientas* o en `<config de VCSIDE>/tools/bb/2600basic.sh`. El guion se ejecuta en la carpeta del `.bas` (wasmtime solo le deja ver `.`), con la variable `bB` apuntando a su carpeta y la de wasmtime delante del `PATH`, porque una aplicación abierta desde el Finder no tiene el `PATH` de la terminal.
- **Éxito:** el guion acaba siempre con 0, aunque falle. Antes de compilar se borran las salidas anteriores; la compilación es buena si no hay errores y aparece `main.bas.bin`.
- **Salidas:** junto al `.bas` quedan `main.bas.bin`, `.asm`, `.list.txt` y `.symbol.txt`. VCSIDE copia la ROM a la salida del proyecto y el listado y los símbolos a `<rom>.lst` y `.sym`, los nombres que buscan Stella y VCSIDE.
- **Errores:** los suyos (`line 4: Error: ...`) ya traen la línea del `.bas`; los de DASM en `main.bas.asm (L)` se llevan a ella con las marcas `;;line N;;`. Un salto a una etiqueta que no existe (`Unknown Mnemonic 'jmp .x'`) se explica como tal.
- **Espacio:** de su salida, `N bytes of ROM space left [in bank K]`, con bancos de 4 KB (numerados desde 0, como en los emuladores). La RAM no se mide: sus variables son fijas.
- **Formato:** `AUTO`, porque el tamaño y el kernel los fija el programa (`set romsize`, `set kernel`).
- **Editor:** `data/kb/batari-basic.json` (CC BY-SA) con las palabras del compilador (nombres sacados de `keywords.c` y `statements.c`) y resúmenes propios en inglés y español. Las palabras con `N` y un `range` se expanden (`joyNup` → `joy0up` ... `joy3up`). Las etiquetas son lo que empieza en la columna 0.
- **Plantilla** (`data/templates/bb/main.bas`, CC0): un cuadrado que se mueve con el joystick dentro de un playfield con paredes.

## Editor de gráficos

Un archivo `.vcsgfx` (JSON, con las filas escritas como `X` y `.` para que se lea y se compare como un dibujo) guarda sprites y playfields. Se abre en una pestaña del editor (*Archivo → Gráficos nuevos...*).

- **Sprites como capas de objetos.** Un sprite se compone con los cinco objetos móviles del TIA (P0, P1, M0, M1, BL), cada uno en una capa con su posición en relojes de color y filas, su ancho (NUSIZ en jugadores; 1, 2, 4 u 8 en misiles y bola), su reflejo y sus colores por fila y fotograma. Así un personaje multicolor puede ser, por ejemplo, P0 + P1 superpuestos y un misil como espada.
- **Fases de parpadeo.** Cada capa tiene una fase (1 a 4): las de una fase se dibujan en los mismos fotogramas. Un objeto puede repetirse en fases distintas, nunca dos veces en la misma. La vista previa alterna las fases a 60 Hz o las muestra a la vez.
- **Lo que hace el hardware.** El lienzo y la vista previa dibujan como el TIA: prioridad P0/M0 sobre P1/M1 sobre bola y playfield, y cada misil con el color que su jugador pone en esa línea. Las proporciones aproximan las de una tele (un reloj de color es más ancho que alto una línea).
- **Playfields por filas.** 40 columnas; cada fila tiene su modo (espejo o repetido, el bit 0 de CTRLPF), sus colores de playfield y fondo y, si se dibuja en su mitad derecha, su propia mitad derecha (asimétrica). Con eso se describen cambios a mitad de pantalla (de modo, de color, de dibujo) y a mitad de línea.
- **Perfiles de kernel.** Cada `.vcsgfx` sabe qué lo va a dibujar: un kernel propio en ensamblador (todo el TIA) o uno de batari Basic (estándar, multisprite, DPC+ o PXE). Por defecto es el del proyecto: en ensamblador, el propio; en batari Basic, el que pone el programa principal (`set kernel`), con sus `set kernel_options` y `const pfres`, que se vuelven a leer cuando cambia el archivo. El archivo puede fijar otro (campo `kernel`). El editor se ajusta a lo que el kernel muestra:

  | | Propio | Estándar | Multisprite | DPC+ | PXE |
  |---|---|---|---|---|---|
  | Jugadores | P0, P1 | P0, P1 | P0-P5 | P0-P9 | P0-P16 |
  | Misiles y bola | con forma | bloques; `player1colors`/`playercolors` quitan M1 (y M0), `no_blank_lines`/`readpaddle` quitan M0 | misiles de una línea | bloques | bloques |
  | Color por fila | todos | P1 con `player1colors`, P0 con `playercolors` | no (un `COLUPn` por jugador) | jugadores | jugadores |
  | Orden de las filas | de arriba abajo | de abajo arriba | de abajo arriba | de arriba abajo | de arriba abajo |
  | Playfield | 40 columnas, modo por fila | 32 columnas desde la 5, asimétrico; 11 filas o `pfres` | 16 columnas desde la 5, reflejado | 32 columnas | 32 columnas |
  | Color por fila del playfield | playfield y fondo | con `pfcolors` (al fondo con `background`) | no | `pfcolors:` y `bkcolors:` | `pfcolors:` y `bkcolors:` |

  Los jugadores que el kernel no tiene no se ofrecen (con el motivo al pasar el ratón), los colores por fila desaparecen donde el kernel da uno solo, las columnas que no muestra salen rayadas y no se pueden pintar, y en multisprite cada fila se dibuja reflejada. El lienzo y la vista previa enseñan lo que mostrará el kernel. Las exportaciones se comprobaron compilándolas con batari Basic 1.9 en los cuatro kernels y viéndolas en Stella.
- **Avisos según el kernel.** Avisan de un objeto usado dos veces en una fase o que el kernel no dibuja, del parpadeo y su frecuencia, de los jugadores extra que comparten líneas (el kernel los alterna), del color compartido de misiles y bola, de las escrituras que necesitará cada línea en un kernel propio, de las filas asimétricas y su margen de tiempo, y de lo que un kernel de batari Basic no muestra (columnas, forma de misiles y bola, colores por fila sin la opción que los activa, número de filas).
- **Salida.** En proyectos DASM, al guardar se escribe `<nombre>.gfx.asm` con tablas etiquetadas (`<sprite>_<objeto>[_<capa>]_Frame<n>`, `_Colour<n>`, posiciones y tamaños como constantes; playfields en tablas por registro, con PF0R/PF1R/PF2R para la mitad derecha y tablas de CTRLPF y colores si cambian). Las filas van de abajo arriba, como las leen la mayoría de kernels. *Copiar como...* da el mismo texto o los bloques de batari Basic del kernel (`player0:`…`player16:`, `playerNcolor:`, `playfield:` de 32 o 16 columnas, `pfcolors:`, `bkcolors:`), con las filas en el orden que lee, los colores fijos como asignaciones (`COLUP0`, `_COLUP1`, `COLUP2`…) que van antes de cada `drawscreen`, y en DPC+ y PXE los `DFnFRACINC` que dan a las filas su altura. En un proyecto de ensamblador se copia para el kernel estándar con las opciones que los gráficos necesitan.
- **Archivos de salida.** Las tablas van a un solo archivo (por defecto), a uno por sprite o playfield (`<nombre>.<elemento>.gfx.asm`) o a uno por grupo (`<nombre>.<grupo>.gfx.asm`; cada elemento tiene un campo `group`, y los que no lo tienen se quedan en `<nombre>.gfx.asm`). Se elige en la barra del editor (campo `split` del `.vcsgfx`: `each` o `group`). Al guardar se borran los archivos que se generaron antes desde ese `.vcsgfx` y ya no tocan, reconocidos por su primera línea; los demás no se tocan. En C, lo mismo con `.gfx.h`, cada uno con su guarda.
- **Animaciones.** Un sprite tiene animaciones con nombre: pasos que muestran cada uno un fotograma durante unos fotogramas de vídeo, en bucle o parándose en el último. Un mismo fotograma puede salir en varios pasos (andar: 0-1-2-1) y con duraciones distintas. Al insertar, borrar o mover fotogramas, los pasos los siguen. La vista previa reproduce la animación elegida. En ensamblador salen tablas de punteros a los fotogramas de cada capa (`_FramePtrLo/Hi`, `_ColourPtrLo/Hi`) y, por animación, `<sprite>_<animación>_FRAME` y `_TICKS` (fotograma y duración de cada paso) con `_STEPS` y `_LOOP`: el programa lleva un contador y un índice de paso y lee de ahí el fotograma. En batari Basic sale una subrutina (`gosub <sprite>_<animación>` una vez por fotograma, `_reset` para empezar de nuevo) con las duraciones en una tabla `data`, un `on ... goto` al fotograma del paso y los bloques de cada fotograma; usa dos variables que se eligen en la animación. También se puede tener al día en el programa con `rem vcsgfx <archivo> <sprite> animation <nombre>`. Se comprobó compilándola con batari Basic 1.9 en los kernels estándar y DPC+ y viendo en Stella que cambia de fotograma a su ritmo.
- **Ejemplo en ensamblador** (*Proyecto nuevo → Empezar con → Ejemplo: un sprite animado*, `data/templates/dasm-animation`, CC0): un personaje dibujado en `hero.vcsgfx` con dos animaciones, `idle` (parpadea) y `walk`. Al crear el proyecto se escribe `hero.gfx.asm` como lo escribe el editor al guardar. El programa lo dibuja con un kernel de dos líneas por fila que lee el fotograma por puntero (`hero_P0_FramePtrLo/Hi`, `_ColourPtrLo/Hi`) y, en el borrado vertical, avanza la animación con sus tablas `_FRAME`/`_TICKS`/`_STEPS`/`_LOOP`. El joystick lo mueve y lo refleja. Se ensambla en NTSC y PAL (prueba automática) y se comprobó en Stella el parpadeo y el paso al andar.
- **Salida en C (proyectos ELF).** En un proyecto ELF el kernel también es propio, pero en C: al guardar se escribe `<nombre>.gfx.h` junto al `.vcsgfx`, con guarda de inclusión. Cada capa da `static const uint8_t <sprite>_<capa>_gfx[<SPRITE>_FRAMES][alto]` (con el dibujo y el color de la fila en un comentario) y `_colour`, con las filas de arriba abajo, que es como las recorre un bucle de líneas; posición, alto, fase, NUSIZ y REFP van en `#define`. Cada animación es un array de pasos `{fotograma, duración}` y un `vcsgfx_animation` con su número de pasos y si vuelve a empezar. Los tipos y dos funciones `static inline`, `vcsgfx_play` (cambia de animación si no es la que suena) y `vcsgfx_animate` (una vez por fotograma de vídeo, devuelve el fotograma que toca), van una sola vez aunque se incluyan varios `.gfx.h`. Los playfields dan arrays por registro (`_pf0`…, `_pf0r`… si hay filas asimétricas, `_ctrlpf`, `_colupf`, `_colubk` si cambian). *Copiar como → Arrays de C* da lo mismo de un elemento. Una prueba compila la cabecera de varios gráficos incluida dos veces con `-Wall -Wextra -Werror`.
- **Ejemplo en C** (*Proyecto nuevo → C para ELF → Empezar con → Ejemplo: un sprite animado*, `data/templates/elf-animation`, CC0): la plantilla ELF con otro `main.c` y el mismo `hero.vcsgfx` en `src/`; al crear el proyecto se escribe `src/hero.gfx.h`. El kernel coloca al jugador 0 con RESP0 tras 10 + x/6 nops (queda 1-6 píxeles a la derecha; antes de 10 caería en el borrado horizontal) y HMOVE lo devuelve a su sitio; después escribe GRP0 y COLUP0 en cada línea. En el overblank el joystick lo mueve en las cuatro direcciones y elige `walk` o `idle`. Compila con `make` sin avisos (prueba automática si está arm-none-eabi-gcc) y se comprobó en Stella la animación y la posición exacta de 0 a 159.
- **Gráficos al día en un programa de batari Basic.** batari Basic solo incluye ensamblador, no código suyo, así que los bloques tienen que estar en el `.bas`. Las líneas entre `rem vcsgfx <archivo>.vcsgfx <nombre> [frame N | animation NOMBRE]` y `rem vcsgfx end` son de VCSIDE: cada vez que se guarda el `.vcsgfx`, se reescriben en los `.bas` del proyecto con la exportación para su kernel (un fotograma, si se indica, para poder poner cada uno en su rama de la animación). Si el programa está abierto se cambia en el editor (se puede deshacer) y se guarda, salvo que tuviera cambios propios; si no, en el disco. *Copiar como → batari Basic, siempre al día* da las zonas listas para pegar, una por fotograma. Una zona con un nombre que no existe, un fotograma de más o sin `end` se queda como estaba y se avisa en la salida.
- **Intercambio.** *Importar* lee del portapapeles esas mismas tablas, las de PlayerPal (`.byte #%...;$xx`, con el color de la fila en el comentario) y los bloques de batari Basic (en el orden de filas del kernel; si el texto es un programa con `set kernel`, el suyo), y los playfields del *Tiny VCS Playfield Editor* de masswerk.at: su cabecera `; mode: symmetric|asymmetric repeat|mirrored line-height N` da el modo de las filas y su alto, y vale tanto una línea `.byte` por fila (en la primera columna, con tres o seis valores) como una tabla por registro, que en modo asimétrico alterna la mitad izquierda y la derecha de cada fila, así que los gráficos pueden ir y venir entre VCSIDE y esas herramientas. Hay enlaces a PlayerPal y a Atari Background Builder para quien prefiera dibujar allí.
- **Colores.** Las paletas NTSC y PAL se calculan con la disposición de tonos y luminancias de cada norma (son aproximadas; el emulador muestra las exactas). SECAM usa sus ocho colores fijos por luminancia.

## Plegado de código

`src/editor/folding.cpp` saca las regiones del texto, sin compilar nada: en DASM, `MAC`/`ENDM`, `IF`/`IFCONST`/`IFNCONST` hasta `ENDIF`/`EIF`, `REPEAT`/`REPEND`, cada `SEG` hasta el siguiente, cada `SUBROUTINE` y cada etiqueta global en la columna 0 hasta la siguiente (o hasta el próximo `SEG`), y las rachas de dos o más comentarios. En batari Basic, `function`, `data`, `asm` y los bloques como `player0:` o `playfield:` hasta `end`, los bloques `rem vcsgfx`, las etiquetas y las rachas de `rem`. En C, las llaves que ocupan varias líneas (una llave sola en su línea se une a la de arriba), `#if`/`#endif` y los comentarios. Las secciones no se quedan con las líneas en blanco ni con los comentarios que las cierran, que suelen presentar lo siguiente. Si dos regiones empiezan en la misma línea, se queda la mayor.

El editor muestra filas en vez de líneas: una región plegada deja su primera línea con una marca que dice cuántas oculta. Los pliegues siguen al texto como los puntos de ruptura, y se abren si el cursor entra (una búsqueda, *Ir a línea*, un error). Las flechas arriba y abajo se saltan lo plegado.

## Árbol del proyecto

El panel *Proyecto* dibuja el árbol fila a fila: flecha, icono y nombre, con guías verticales por nivel y la fila entera resaltada al pasar el ratón o si es el archivo que se edita. Las carpetas van primero; `build` y lo que contiene salen atenuados. Los iconos son formas vectoriales propias (`src/file_icons.cpp`) que toman el color del tema: una hoja con letra para el código (A ensamblador, C, H cabeceras e includes, B batari Basic, L listados y símbolos), un sprite para `.vcsgfx`, un cartucho para las ROM (`.bin`, `.a26`, `.elf`), un joystick para el `.vcsproj`, un cuadro para las imágenes y una hoja con renglones para el resto.

## Temas

Todos los colores de VCSIDE tienen un papel con nombre, agrupados: `ui.*` (interfaz), `editor.*` (editor de código), `syntax.*` (colores de la sintaxis), `messages.*` (errores, avisos, notas), `space.*` (informe de espacio), `graphics.*` (editor de gráficos) y `files.*` (árbol del proyecto: filas, guías y un color por tipo de archivo). La lista está en `src/theme.h`; el código nunca usa un color fijo, salvo los de la propia 2600 (la paleta NTSC/PAL/SECAM del editor de gráficos). A partir de los papeles `ui.*` se calculan todos los colores de ImGui, y un tema puede corregir cualquiera de ellos por su nombre en `imgui`.

Un tema es un archivo `.vcstheme` (JSON):

- `base`: el tema del que toma lo que no dice. Un tema basado en otro solo guarda sus diferencias y sigue los cambios de la base.
- `palette`: colores con nombre. Un papel puede ser `#RRGGBB`, `#RRGGBBAA`, `$nombre` (de la paleta), `@papel` (otro papel) y cualquiera de ellos con `/AA` para fijar el alfa (`$blue/40`). Las referencias se resuelven después de juntar las bases, así que cambiar una entrada de la paleta o `ui.accent` cambia todo lo que se refiere a ellos.
- `colours`: los papeles. `indent`: un color por nivel de sangría en el editor (lista vacía, sin sombreado), con la línea `editor.indentGuide` donde empieza cada nivel. `imgui`: colores de ImGui sueltos.

Vienen cuatro (`data/themes`): Oscuro (los colores de siempre), Claro, Solarized Light y Solarized Dark. Los dos Solarized usan la paleta publicada de Solarized con nombres por función (`bg`, `bgHi`, `fg`, `fgEm`, `dim`); el oscuro es el claro con esas cinco entradas cambiadas, como hace la propia paleta. Un problema (un papel sin color, una referencia que no existe, un bucle de referencias o de bases) se ve en magenta y se lista en el editor de temas.

*Ver → Tema* cambia de tema. *Editar temas...* abre una ventana con los temas (incluidos y tuyos), la paleta, los papeles por grupo con buscador, los niveles de sangría y los problemas. Los cambios se ven al momento y se guardan solos en la carpeta de temas del usuario (`<carpeta de ajustes>/themes`). Los temas incluidos no se cambian: «Nuevo, basado en este» crea uno vacío que lo sigue y «Duplicar», una copia. Para compartir: exportar a un archivo o copiar como texto, e importar un archivo o pegar un tema. Lo exportado lleva dentro las bases que no son de VCSIDE, así que solo necesita un tema incluido. Al importar nunca se pisa un tema: si el nombre existe, se le da otro.

## Avisos de espacio

Tras cada compilación con DASM, el IDE mide cuánto espacio queda (`src/space.*`). Los datos de cada formato (tamaños de ROM válidos, tamaño de banco, direcciones reservadas, RAM extra y sus ventanas) están en el objeto `space` de `formats.json`.

- **Cómo se mide:**
  - **ROM:** una segunda pasada de DASM con `-f2` escribe cada bloque de bytes emitidos con su origen y longitud, así que se sabe exactamente qué está ocupado. El relleno de `ALIGN` y `ds` cuenta como ocupado. El listado no sirve para esto: `incbin` no da tamaño y las macros arrastran direcciones viejas.
  - **RAM:** las líneas sin inicializar (`U`) del listado, con el tamaño de cada `ds` leído de su línea de código y evaluado con los símbolos. Las variables declaradas con `=` no se cuentan; si no hay ningún `SEG.U`, la RAM sale como «sin medir».
- **ROM:**
  - lo ocupado y lo libre por banco;
  - error si la imagen es mayor que la que admite el formato, aviso si es de otro tamaño;
  - aviso si código o datos caen en un *hotspot* o en los puertos de la RAM del cartucho. No avisa del relleno `ds`/`align`, que es la forma normal de saltarse la ventana del Superchip.
- **RAM de la 2600:** lo que ocupan las variables y lo que queda por encima para la pila. Aviso si queda menos que la reserva del proyecto y error si una variable pasa de `$FF`.
- **RAM del cartucho:**
  - Superchip (128 bytes), FA (256) y CV (1 KB): las declaraciones a través de la ventana de lectura o de escritura se cuentan juntas;
  - E7 sale como «sin medir», porque su RAM se direcciona por bancos.
- **Umbrales:** aviso al llegar a `space_warn_percent` (90 % por defecto) y error al desbordarse. La reserva de pila es `stack_reserve` (8 bytes por defecto). Los dos se configuran en el `.vcsproj`.
- **Dónde se ve:** el panel *Espacio* (una barra por banco y por RAM), el resumen en la barra de estado y los avisos en *Problemas*, con su línea cuando se puede.
- **Pendiente:**
  - formatos sin datos de espacio (3E, DPC, DPC+, CDFJ…), que se miden como bancos de 4 KB;
  - avisar de las lecturas del puerto de escritura del Superchip.

## Ideas pendientes

- **Cancelar una compilación en marcha** (un botón y una orden de menú), y reintentar si el guardado coincide con ella.
- **Enviar la ROM al PlusCart** desde el IDE. Hay que ver con los autores del PlusCart qué vía ofrece (red o USB) y cómo se autoriza.
- **Imagen para el Chameleon Cart**: generar el archivo que se graba en el cartucho a partir de la ROM, eligiendo el firmware que se usa.
- **Buscar referencias** de un símbolo (todos los sitios donde se usa) y **ver su definición en una ventana flotante** sin salir del archivo.
- **Esquema del archivo**: la lista de etiquetas, subrutinas, macros y constantes, para saltar a cualquiera.

## Fases

1. Esqueleto: ventana, workspace, editor, compilar con DASM, ejecutar en Stella. Base de conocimiento 6502/TIA/RIOT, contador de ciclos.
2. Autocompletado, ayuda flotante, panel de documentación; editor completo (buscar/reemplazar, ir a definición…); avisos de espacio de ROM y RAM; batariBasic; Gopher2600.
3. ELF-VCS y Tiny-ELF; plantillas de proyecto por formato.
4. Editores de sprites, playfield y sonido (y exportar/importar de los editores de la comunidad: PlayerPal, Atari Background Builder, TIATracker…).
5. Asistente de descarga de herramientas; empaquetado para los tres sistemas.
6. 7800 con el Chameleon Cart.
