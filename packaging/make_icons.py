#!/usr/bin/env python3
"""Draws the app icon and writes it in every format the platforms want.

    python3 packaging/make_icons.py        (needs Pillow: pip install pillow)

Outputs, all committed so a build never needs Python:
    packaging/icon.png            1024 px preview
    packaging/macos/AppIcon.icns  the .app bundle's icon
    packaging/windows/app.ico     the .exe's icon (linked in through app.rc)

The icon is the title-bar mark, set the way the UI sets anything active: a
solid ink tile with the wireframe globe knocked out in paper white, and the
small filled square that marks the focused pane. Every size is drawn on its
own rather than scaled down from the largest, so the small ones keep strokes
of at least a pixel and drop the latitudes that would blur into a grey disc.
"""

import io
import math
import os
import struct

from PIL import Image, ImageDraw

INK = (0x0E, 0x0E, 0x0D, 255)
PAPER = (0xFC, 0xFC, 0xFA, 255)
EDGE = (0x3A, 0x3A, 0x37, 255)
SUPERSAMPLE = 8

HERE = os.path.dirname(os.path.abspath(__file__))


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
    # A faint edge, so the tile still reads against a dark Dock or menu.
    edge = max(1, round(max(size / 512, 1) * SUPERSAMPLE))
    d.rounded_rectangle([margin, margin, s - margin - 1, s - margin - 1], radius, fill=INK,
                        outline=EDGE, width=edge)

    # Strokes are specified in output pixels, so they stay crisp and never
    # fall below one pixel at 16 and 32.
    w = round(max(size * 0.015, 1.3) * SUPERSAMPLE)

    cx = cy = s / 2
    r = tile * 0.30
    detailed = size >= 48

    # Outline.
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=PAPER, width=w)

    # Equator and latitudes, each ending inside the outline's stroke so the
    # square line ends never poke out past the circle.
    def chord(dy):
        inner = r - w * 0.5
        half = math.sqrt(max(inner * inner - dy * dy, 0.0))
        d.line([cx - half, cy + dy, cx + half, cy + dy], fill=PAPER, width=w)

    chord(0)
    if detailed:
        chord(-r * 0.43)
        chord(r * 0.43)

    # One meridian.
    mr = r * 0.45
    d.ellipse([cx - mr, cy - r, cx + mr, cy + r], outline=PAPER, width=w)

    # The focus mark, in the tile's top-left corner like "■ Ø2 EDITOR".
    if size >= 32:
        sq = tile * 0.062
        inset = margin + tile * 0.13
        d.rectangle([inset, inset, inset + sq, inset + sq], fill=PAPER)

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
