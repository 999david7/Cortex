#!/usr/bin/env python3
"""Draws the app icon and writes it in every format the platforms want.

    python3 packaging/make_icons.py        (needs Pillow: pip install pillow)

Outputs, all committed so a build never needs Python:
    packaging/icon.png            1024 px preview
    packaging/macos/AppIcon.icns  the .app bundle's icon
    packaging/windows/app.ico     the .exe's icon (linked in through app.rc)

The icon is the title-bar globe printed as a halftone on paper: a grid of
square dots, larger and darker where the sphere turns away from an ochre sun,
with the globe's wireframe left as a trace of lighter dots. Every size is
drawn on its own rather than scaled down from the largest: the grid gets
coarser as the icon shrinks so each dot stays a pixel or more, the wireframe
goes once there are too few cells to draw it, and at 32 px and below the
globe is solid.
"""

import io
import math
import os
import struct

from PIL import Image, ImageDraw

PAPER = (0xFC, 0xFC, 0xFA, 255)
HAIRLINE = (0xE4, 0xE4, 0xE0, 255)  # the UI's hairline rule
OCHRE = (0xD4, 0x8A, 0x14, 255)     # the UI's ochre, brightened to carry at icon size
DOT_LIGHT = (0x9A, 0x9A, 0x96)      # the faintest halftone square
DOT_DARK = (0x1C, 0x1C, 0x1A)       # the densest one
SUPERSAMPLE = 8

HERE = os.path.dirname(os.path.abspath(__file__))


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def draw_icon(size, mac_grid):
    """One icon of size x size pixels.

    mac_grid: lay the tile out on Apple's icon grid (an 824/1024 rounded
    square with transparent margin, so it sits right next to other Dock
    icons). Otherwise the tile fills the canvas, as Windows expects.
    """
    s = size * SUPERSAMPLE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    if mac_grid:
        margin = s * 100 / 1024
        radius = s * 185 / 1024
    else:
        margin = 0
        radius = s * 0.18
    tile = s - 2 * margin
    u = tile / 824  # one unit of the 1024-px design, whatever the size

    # The paper, framed by the hairline every pane in the UI carries.
    edge = max(round(max(4 * u, SUPERSAMPLE)), 1)
    d.rounded_rectangle([margin, margin, s - margin - 1, s - margin - 1], radius, fill=PAPER,
                        outline=HAIRLINE, width=edge)

    # The globe and the sun that lights it, placed on the 1024 design grid.
    cx, cy, r = margin + 405 * u, margin + 455 * u, 245 * u
    sun_x, sun_y, sun_r = margin + 640 * u, margin + 175 * u, 42 * u

    if size <= 32:
        # Halftone cannot survive at this size: a solid globe and the sun.
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=lerp(DOT_LIGHT, DOT_DARK, 0.8) + (255,))
        d.ellipse([sun_x - sun_r * 1.4, sun_y - sun_r * 1.4, sun_x + sun_r * 1.4,
                   sun_y + sun_r * 1.4], fill=OCHRE)
        return img.resize((size, size), Image.LANCZOS)

    # The globe is a halftone: a grid of squares whose size and tone follow
    # how much light each point of the sphere gets from the sun. Fewer cells
    # at small sizes, so every square stays at least a pixel or two.
    # An odd count puts a row and a column through the centre, so the
    # equator and the meridian each land on exactly one line of cells.
    cells = max(7, min(23, round(size / 11)) | 1)
    pitch = 2 * r / cells
    half = cells // 2
    wireframe = cells >= 15

    lx, ly, lz = sun_x - cx, sun_y - cy, r * 0.6
    ln = math.sqrt(lx * lx + ly * ly + lz * lz)
    lx, ly, lz = lx / ln, ly / ln, lz / ln

    # The title-bar globe's lines, snapped to the grid: the equator, two
    # latitudes and one meridian, each a single row or column of cells.
    lat = round(0.43 * half)

    def on_wire(i, j):
        if j == 0 or abs(j) == lat:
            return True
        ny = j * pitch / r
        return abs(i) == round(0.45 * math.sqrt(max(1 - ny * ny, 0.0)) * r / pitch)

    for j in range(-half - 1, half + 2):
        for i in range(-half - 1, half + 2):
            px, py = cx + i * pitch, cy + j * pitch
            nx, ny = (px - cx) / r, (py - cy) / r
            dist = math.sqrt(nx * nx + ny * ny)
            if dist > 1.0:
                continue
            nz = math.sqrt(1 - dist * dist)
            lit = max(0.0, nx * lx + ny * ly + nz * lz)
            v = 0.42 + 0.58 * (1 - lit)  # darker away from the sun
            if dist > 0.86:
                v *= 1 - (dist - 0.86) / 0.14 * 0.55  # the rim thins out, as in print
            if wireframe and on_wire(i, j):
                v = min(v, 0.22)
            side = pitch * (0.20 + 0.36 * v)
            color = lerp(DOT_LIGHT, DOT_DARK, v) + (255,)
            d.rectangle([px - side / 2, py - side / 2, px + side / 2, py + side / 2], fill=color)

    # The shadow it casts: one row of the smallest, faintest squares.
    shadow_y = cy + r + pitch * 1.1
    for i in range(-half, half + 1):
        px = cx + i * pitch
        t = abs(px - cx) / (r * 0.85)
        if t > 1:
            continue
        side = pitch * (0.10 + 0.10 * (1 - t))
        color = lerp((0xC8, 0xC8, 0xC4), DOT_LIGHT, 1 - t) + (255,)
        d.rectangle([px - side / 2, shadow_y - side / 2, px + side / 2, shadow_y + side / 2],
                    fill=color)

    d.ellipse([sun_x - sun_r, sun_y - sun_r, sun_x + sun_r, sun_y + sun_r], fill=OCHRE)
    return img.resize((size, size), Image.LANCZOS)


def png_bytes(img):
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_icns(path):
    # (type, pixel size). PNG payloads are valid for every one of these on
    # macOS 10.7 and later, which is what iconutil itself produces.
    entries = [
        (b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128),
        (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024),
        (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512),
    ]
    cache = {}
    chunks = []
    for kind, px in entries:
        if px not in cache:
            cache[px] = png_bytes(draw_icon(px, mac_grid=True))
        data = cache[px]
        chunks.append(kind + struct.pack(">I", len(data) + 8) + data)
    body = b"".join(chunks)
    with open(path, "wb") as f:
        f.write(b"icns" + struct.pack(">I", len(body) + 8) + body)


def write_ico(path):
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [png_bytes(draw_icon(px, mac_grid=False)) for px in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    directory = b""
    for px, data in zip(sizes, images):
        dim = 0 if px >= 256 else px  # 0 means 256 in an ICO directory
        directory += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(header + directory + b"".join(images))


def main():
    os.makedirs(os.path.join(HERE, "macos"), exist_ok=True)
    os.makedirs(os.path.join(HERE, "windows"), exist_ok=True)
    draw_icon(1024, mac_grid=True).save(os.path.join(HERE, "icon.png"), optimize=True)
    write_icns(os.path.join(HERE, "macos", "AppIcon.icns"))
    write_ico(os.path.join(HERE, "windows", "app.ico"))
    print("wrote packaging/icon.png, packaging/macos/AppIcon.icns, packaging/windows/app.ico")


if __name__ == "__main__":
    main()
