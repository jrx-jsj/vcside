#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Juan Sánchez Jurado
"""Builds the VCSIDE icons from the SVG sources in assets/icon/.

  vcside.svg      main drawing (1024x1024), used from 48 px up
  vcside-16.svg   pixel version for 16 px
  vcside-32.svg   pixel version for 32 px

The "VCS" lettering on the cartridge is a <g id="label-text"> group with a
data-label="x y width height" attribute (the label, in 1024 units). For every
size it is redrawn with blocks snapped to that size's pixel grid, so the
letters always have hard edges, never antialiasing.

The grip bands under the label are a <g id="grip"> group with
data-grip="x y width step height rows". From GRIP_MIN px up they spell "IDE"
as gaps cut out of the bands (with a solid band above, below and at each
side); smaller sizes get five plain bands. Also snapped to the pixel grid.

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

# 3/4x5 font for the word cut out of the grip bands.
GRIP_FONT = {
    "I": ["XXX", ".X.", ".X.", ".X.", "XXX"],
    "D": ["XXX.", "X..X", "X..X", "X..X", "XXX."],
    "E": ["XXXX", "X...", "XXX.", "X...", "XXXX"],
}
GRIP_WORD = "IDE"
GRIP_MIN = 128
ASPECT = 1.25  # pixels a little wider than tall, like the 2600 playfield


def text_blocks(label, size):
    """Lettering blocks, in 1024 units, aligned to the pixel grid of `size` px."""
    label_x, label_y, label_width, label_height = label
    unit = 1024 / size                   # one output pixel, in SVG units
    columns = len(TEXT) * 6 - 1
    room_width = label_width / unit - 2 * max(1, round(label_width / unit * 0.06))
    room_height = label_height / unit * 0.62
    block_height = max(1, int(room_height // 7))
    while True:
        block_width = max(1, round(block_height * ASPECT))
        if (columns * block_width <= room_width and 7 * block_height <= room_height) or block_height == 1:
            break
        block_height -= 1
    block_width = max(1, round(block_height * ASPECT))
    if columns * block_width > room_width:
        block_width = block_height = 1
    text_width, text_height = columns * block_width, 7 * block_height
    origin_x = round((label_x / unit) + (label_width / unit - text_width) / 2)
    origin_y = round((label_y / unit) + (label_height / unit - text_height) / 2)
    rects = []
    for letter_index, letter in enumerate(TEXT):
        for row_index, row in enumerate(FONT[letter]):
            column = 0
            while column < 5:            # merge runs of blocks in each row
                if row[column] != "X":
                    column += 1
                    continue
                run_end = column
                while run_end < 5 and row[run_end] == "X":
                    run_end += 1
                x = (origin_x + (letter_index * 6 + column) * block_width) * unit
                y = (origin_y + row_index * block_height) * unit
                rects.append(f'<rect x="{x:g}" y="{y:g}" width="{(run_end - column) * block_width * unit:g}" height="{block_height * unit:g}"/>')
                column = run_end
    return "".join(rects)


def grip_blocks(grip, size):
    """Grip bands, in 1024 units, aligned to the pixel grid of `size` px."""
    grip_x, grip_y, grip_width, step, band_height, rows = grip
    rows = int(rows)
    unit = 1024 / size
    rects = []
    if size < GRIP_MIN:
        # Five plain bands, inset from the IDE block.
        left, right = round((grip_x + 40) / unit), round((grip_x + grip_width - 40) / unit)
        for band in range(5):
            top = round((grip_y + 20 + band * 48) / unit)
            rects.append((left, top, right - left, max(1, round((grip_y + 40 + band * 48) / unit) - top)))
    else:
        left, right = round(grip_x / unit), round((grip_x + grip_width) / unit)
        columns = sum(len(GRIP_FONT[letter][0]) for letter in GRIP_WORD) + len(GRIP_WORD) - 1
        column_width = max(1, int((grip_width / unit) / (columns + 2)))
        text_left = left + ((right - left) - columns * column_width) // 2
        for band in range(rows):
            top = round((grip_y + band * step) / unit)
            height = max(1, round((grip_y + band * step + band_height) / unit) - top)
            font_row = band - (rows - 5) // 2  # letters centred vertically
            gaps = []
            column = 0
            for letter in GRIP_WORD:
                if 0 <= font_row < 5:
                    gaps += [column + offset for offset, cell in enumerate(GRIP_FONT[letter][font_row]) if cell == "X"]
                column += len(GRIP_FONT[letter][0]) + 1
            drawn_to = left
            for gap in gaps:
                gap_x = text_left + gap * column_width
                if gap_x > drawn_to:
                    rects.append((drawn_to, top, gap_x - drawn_to, height))
                drawn_to = max(drawn_to, gap_x + column_width)
            if right > drawn_to:
                rects.append((drawn_to, top, right - drawn_to, height))
    return "".join(f'<rect x="{x * unit:g}" y="{y * unit:g}" width="{width * unit:g}" height="{height * unit:g}"/>'
                   for x, y, width, height in rects)


GRIP_RE = re.compile(r'(<g id="grip"[^>]*data-grip="([^"]+)"[^>]*>)(.*?)(</g>)', re.S)
LABEL_RE = re.compile(r'(<g id="label-text"[^>]*data-label="([^"]+)"[^>]*>)(.*?)(</g>)', re.S)


def svg_for_size(svg, size):
    match = GRIP_RE.search(svg)
    if match:
        grip = [float(value) for value in match.group(2).split()]
        svg = svg[:match.start(3)] + grip_blocks(grip, size) + svg[match.end(3):]
    match = LABEL_RE.search(svg)
    if match:
        label = [float(value) for value in match.group(2).split()]
        svg = svg[:match.start(3)] + text_blocks(label, size) + svg[match.end(3):]
    return svg


def render(svg_text, size, out):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as svg_file:
        svg_file.write(svg_text)
        svg_path = svg_file.name
    try:
        subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), svg_path, "-o", out], check=True)
    finally:
        os.unlink(svg_path)


def write_ico(pngs, out):
    """ICO with embedded PNGs (supported since Windows Vista)."""
    images = [open(path, "rb").read() for _, path in pngs]
    head = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    entries = b""
    for (size, _), image in zip(pngs, images):
        side = 0 if size >= 256 else size  # 0 means 256
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    with open(out, "wb") as ico:
        ico.write(head + entries + b"".join(images))


def main():
    if not shutil.which("rsvg-convert"):
        sys.exit("rsvg-convert (librsvg) is missing.")
    main_svg = open(os.path.join(ICON, "vcside.svg"), encoding="utf-8").read()
    small = {size: open(os.path.join(ICON, f"vcside-{size}.svg"), encoding="utf-8").read() for size in (16, 32)}

    # The main SVG keeps the lettering snapped to 1024.
    fixed = svg_for_size(main_svg, 1024)
    if fixed != main_svg:
        open(os.path.join(ICON, "vcside.svg"), "w", encoding="utf-8").write(fixed)

    pngs = {}
    for size in SIZES:
        source = small[size] if size in small else main_svg
        out = os.path.join(ICON, f"vcside-{size}.png")
        render(svg_for_size(source, size), size, out)
        pngs[size] = out
        print(out)

    window_icon = os.path.join(ROOT, "data", "vcside.png")
    shutil.copy(pngs[256], window_icon)
    print(window_icon)

    write_ico([(size, pngs[size]) for size in ICO_SIZES], os.path.join(ICON, "vcside.ico"))
    print(os.path.join(ICON, "vcside.ico"))

    if shutil.which("iconutil"):
        with tempfile.TemporaryDirectory() as folder:
            iconset = os.path.join(folder, "vcside.iconset")
            os.mkdir(iconset)
            for size in (16, 32, 128, 256, 512):
                shutil.copy(pngs[size], os.path.join(iconset, f"icon_{size}x{size}.png"))
                shutil.copy(pngs[size * 2], os.path.join(iconset, f"icon_{size}x{size}@2x.png"))
            subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(ICON, "vcside.icns")], check=True)
        print(os.path.join(ICON, "vcside.icns"))
    else:
        print("No iconutil (macOS only): vcside.icns left unchanged.")


if __name__ == "__main__":
    main()
