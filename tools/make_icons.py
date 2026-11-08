#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 VCSIDE contributors
"""Builds the VCSIDE icons from the SVG sources in assets/icon/.

  vcside.svg      main drawing (1024x1024), used from 48 px up
  vcside-16.svg   pixel version for 16 px
  vcside-32.svg   pixel version for 32 px

The "VCS" lettering on the cartridge is a <g id="label-text"> group with a
data-label="x y width height" attribute (the label, in 1024 units). For every
size it is redrawn with blocks snapped to that size's pixel grid, so the
letters always have hard edges, never antialiasing.

Output in assets/icon/: vcside-<n>.png (16..1024; 256 and 512 are the Linux
icons), vcside.ico (Windows), vcside.icns (macOS, via iconutil), plus
data/vcside.png (256 px), which the program loads as its window icon.
Needs rsvg-convert; iconutil only on macOS (without it the .icns is left as is).
Python standard library only.

    python3 tools/make_icons.py
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON = os.path.join(ROOT, "assets", "icon")
SIZES = [16, 32, 48, 64, 128, 256, 512, 1024]
ICO_SIZES = [16, 32, 48, 64, 128, 256]

# 5x7 block font, drawn for VCSIDE.
FONT = {
    "V": ["X...X", "X...X", "X...X", "X...X", ".X.X.", ".X.X.", "..X.."],
    "C": [".XXX.", "X...X", "X....", "X....", "X....", "X...X", ".XXX."],
    "S": [".XXXX", "X....", "X....", ".XXX.", "....X", "....X", "XXXX."],
}
TEXT = "VCS"
ASPECT = 1.25  # pixels a little wider than tall, like the 2600 playfield


def text_blocks(label, size):
    """Lettering blocks, in 1024 units, aligned to the pixel grid of `size` px."""
    lx, ly, lw, lh = label
    u = 1024 / size                      # one output pixel, in SVG units
    cols = len(TEXT) * 6 - 1
    avail_w = lw / u - 2 * max(1, round(lw / u * 0.06))
    avail_h = lh / u * 0.62
    ph = max(1, int(avail_h // 7))
    while True:
        pw = max(1, round(ph * ASPECT))
        if (cols * pw <= avail_w and 7 * ph <= avail_h) or ph == 1:
            break
        ph -= 1
    pw = max(1, round(ph * ASPECT))
    if cols * pw > avail_w:
        pw = ph = 1
    tw, th = cols * pw, 7 * ph
    ox = round((lx / u) + (lw / u - tw) / 2)
    oy = round((ly / u) + (lh / u - th) / 2)
    rects = []
    for i, ch in enumerate(TEXT):
        for r, row in enumerate(FONT[ch]):
            c = 0
            while c < 5:                 # merge runs of blocks in each row
                if row[c] != "X":
                    c += 1
                    continue
                e = c
                while e < 5 and row[e] == "X":
                    e += 1
                x = (ox + (i * 6 + c) * pw) * u
                y = (oy + r * ph) * u
                rects.append(f'<rect x="{x:g}" y="{y:g}" width="{(e - c) * pw * u:g}" height="{ph * u:g}"/>')
                c = e
    return "".join(rects)


LABEL_RE = re.compile(r'(<g id="label-text"[^>]*data-label="([^"]+)"[^>]*>)(.*?)(</g>)', re.S)


def svg_for_size(svg, size):
    m = LABEL_RE.search(svg)
    if not m:
        return svg
    label = [float(v) for v in m.group(2).split()]
    return svg[:m.start(3)] + text_blocks(label, size) + svg[m.end(3):]


def render(svg_text, size, out):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(svg_text)
        tmp = f.name
    try:
        subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), tmp, "-o", out], check=True)
    finally:
        os.unlink(tmp)


def write_ico(pngs, out):
    """ICO with embedded PNGs (supported since Windows Vista)."""
    data = [open(p, "rb").read() for _, p in pngs]
    head = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    entries = b""
    for (size, _), d in zip(pngs, data):
        b = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(d), offset)
        offset += len(d)
    with open(out, "wb") as f:
        f.write(head + entries + b"".join(data))


def main():
    if not shutil.which("rsvg-convert"):
        sys.exit("rsvg-convert (librsvg) is missing.")
    main_svg = open(os.path.join(ICON, "vcside.svg"), encoding="utf-8").read()
    small = {n: open(os.path.join(ICON, f"vcside-{n}.svg"), encoding="utf-8").read() for n in (16, 32)}

    # The main SVG keeps the lettering snapped to 1024.
    fixed = svg_for_size(main_svg, 1024)
    if fixed != main_svg:
        open(os.path.join(ICON, "vcside.svg"), "w", encoding="utf-8").write(fixed)

    pngs = {}
    for n in SIZES:
        src = small[n] if n in small else main_svg
        out = os.path.join(ICON, f"vcside-{n}.png")
        render(svg_for_size(src, n), n, out)
        pngs[n] = out
        print(out)

    window_icon = os.path.join(ROOT, "data", "vcside.png")
    shutil.copy(pngs[256], window_icon)
    print(window_icon)

    write_ico([(n, pngs[n]) for n in ICO_SIZES], os.path.join(ICON, "vcside.ico"))
    print(os.path.join(ICON, "vcside.ico"))

    if shutil.which("iconutil"):
        with tempfile.TemporaryDirectory() as d:
            iconset = os.path.join(d, "vcside.iconset")
            os.mkdir(iconset)
            for n in (16, 32, 128, 256, 512):
                shutil.copy(pngs[n], os.path.join(iconset, f"icon_{n}x{n}.png"))
                shutil.copy(pngs[n * 2], os.path.join(iconset, f"icon_{n}x{n}@2x.png"))
            subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(ICON, "vcside.icns")], check=True)
        print(os.path.join(ICON, "vcside.icns"))
    else:
        print("No iconutil (macOS only): vcside.icns left unchanged.")


if __name__ == "__main__":
    main()
