# Project templates licence

The files in this folder are copied into every new VCSIDE project. They are dedicated to the public domain under **CC0 1.0 Universal** (full text: [`licenses/CC0-1.0.txt`](../../licenses/CC0-1.0.txt)). You may use them in any game, free or commercial, without crediting VCSIDE. A ROM made with VCSIDE does not inherit any VCSIDE licence. The one exception is a TinyELF Basic project's ROM: it is TinyELF Basic itself (GPL-3.0, [`../tinyelf`](../tinyelf/README.md)) with the program in it, so passing it on follows TinyELF Basic's licence; the program's text stays the user's.

SPDX-License-Identifier: CC0-1.0

`shared/` holds files that several templates take (music, graphics), and each template's `shared.json` says which ones go into the project; both are under the same licence. A created project gets plain copies of them.
