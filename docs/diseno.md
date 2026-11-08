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

## Herramientas

Cada herramienta se busca en este orden: ruta fijada en ajustes → `PATH` → rutas típicas del sistema (p. ej. `/Applications/Stella.app`, `C:\Program Files\Stella`, `/usr/games`). Si no está, el IDE ofrece la página oficial de descarga y la carpeta donde dejarla (`~/.vcside/tools/<herramienta>`); más adelante, descarga directa de las versiones publicadas con su suma de comprobación.

| Herramienta | Uso | Notas |
|---|---|---|
| DASM | ensamblador 6502 | `dasm main.asm -f3 -omain.bin -lmain.lst -smain.sym`; errores `file (line): error:` |
| batariBasic | BASIC → DASM | trae su propio DASM y `2600basic.sh/.bat` |
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
- **Pendiente:** recibir del emulador la dirección donde se ha parado (no tienen un canal para avisar a otra aplicación); batari Basic.
- **ELF:** los puntos de ruptura no se pasan (ni Stella ni Gopher2600 pueden parar en una línea de C); `-bs ELF` y `-mapping ELF`.

## Proyectos ELF

- **Plantilla** (`data/templates/elf`, CC0): `Makefile`, `include/vcs.h`, `src/main.c`, `src/trampolines.S` y `chameleon/support.c`, escritos para VCSIDE siguiendo las reglas ELF de los juegos de esta carpeta (`reglas-elf.md`): Cortex-M0, `-fdata-sections` sin `-ffunction-sections`, enlace parcial con libgcc, `strip --strip-unneeded`, divisiones de libgcc renombradas, llamadas largas al cargador, trampolines de `memcpy`/`memset` y `RAM_FUNC` para el kernel. El ejemplo es un arcoíris con una barra que se mueve con el joystick; vuelve al menú con RESET en un multicart.
- **Comprobaciones tras enlazar:** cada símbolo sin definir que no sea del cargador sale como `src/<archivo>.c: error: undefined symbol '...'` (con `-r`, el enlazador no lo da por error, y Stella lo enlazaría a 0); y ninguna llamada corta al cargador.
- **Chameleon Cart:** `make chameleon CHAMELEON_FW=<carpeta chameleon_cart de su plantilla>`. La plantilla de Zack no se incluye porque no tiene licencia.
- **Errores:** además de `archivo:línea:`, los mensajes sin línea (las comprobaciones, `arm-none-eabi-gcc: error:`), los de ld (`main.c:(.text+0x1c): undefined reference`, con la función; región desbordada) y los de make que no repiten otro error. Los archivos que ld da sin carpeta se buscan en el proyecto.
- **Espacio:** VCSIDE lee el ELF directamente. De la ROM (reubicable): código, `.RamFunc`, constantes, variables con valor y a cero. Si hay `build/<rom>-chameleon.elf` más nuevo que la ROM, sus cabeceras de programa dan la flash (lo guardado desde `0x0800_0000`) y la RAM (lo que vive en `0x2000_0000`) exactas; si no, se estima con lo que ocupa el firmware de la plantilla del 2025-10-23, medido: unos 10 100 bytes de flash y 6 000 de RAM (la estimación de la Abadía queda a 41 bytes de la real). Límites del STM32G070RB: 128 KB y 36 KB.

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
  - batari Basic;
  - avisar de las lecturas del puerto de escritura del Superchip.

## Fases

1. Esqueleto: ventana, workspace, editor, compilar con DASM, ejecutar en Stella. Base de conocimiento 6502/TIA/RIOT, contador de ciclos.
2. Autocompletado, ayuda flotante, panel de documentación; editor completo (buscar/reemplazar, ir a definición…); avisos de espacio de ROM y RAM; batariBasic; Gopher2600.
3. ELF-VCS y Tiny-ELF; plantillas de proyecto por formato.
4. Editores de sprites, playfield y sonido (y exportar/importar de los editores de la comunidad: PlayerPal, Atari Background Builder, TIATracker…).
5. Asistente de descarga de herramientas; empaquetado para los tres sistemas.
6. 7800 con el Chameleon Cart.
