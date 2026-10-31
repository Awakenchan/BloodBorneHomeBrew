# SPDX-License-Identifier: GPL-3.0-or-later
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SIZE = 512
HEIGHT = 14
COLUMNS = list(range(0x20, 0x7f)) + list(range(0xa0, 0x100)) + [0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x25a0, 0x25b6, 0x25c0]


def morton(x, y):
    out = 0
    for bit in range(3):
        out |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
    return out


def block(pixels, x, y):
    bits = 0
    for row in range(4):
        for column in range(4):
            if pixels[x + column, y + row] < 128:
                bits |= 3 << (2 * (row * 4 + column))
    return struct.pack("<HHI", 0xffff, 0xffff, bits)


def main():
    font_path, out = Path(sys.argv[1]), Path(sys.argv[2])
    font = ImageFont.truetype(str(font_path), HEIGHT)
    notdef = bytes(font.getmask("\uffff"))
    atlas = Image.new("L", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(atlas)
    x = y = 1
    ascent = font.getmetrics()[0]
    glyphs = []
    for code in sorted(set(COLUMNS)):
        text = chr(code)
        if code != 0x20 and bytes(font.getmask(text)) == notdef:
            continue
        advance = max(1, round(font.getlength(text)))
        box = font.getbbox(text)
        left = min(0, box[0])
        width = max(advance, box[2]) - left
        cell = HEIGHT + 5
        if x + width + 2 > SIZE:
            x, y = 1, y + cell + 1
        if y + cell > SIZE:
            raise SystemExit("the atlas is full")
        draw.text((x - left, y), text, fill=255, font=font)
        glyphs.append((code, x, y, x + width, y + cell, left, width, advance))
        x += width + 2
    if ascent <= 0:
        raise SystemExit("no ascent")
    header = 0x20
    table = header
    regions = table + 24 * len(glyphs)
    total = regions + 8 * len(glyphs)
    ccm = bytearray(total)
    struct.pack_into("<IIhhhhIIII", ccm, 0, 0x20000, total, HEIGHT, SIZE, SIZE, 1, len(glyphs), regions, table, 0)
    for n, (code, x0, y0, x1, y1, left, width, advance) in enumerate(glyphs):
        at = regions + n * 8
        struct.pack_into("<hhhh", ccm, at, x0, y0, x1, y1)
        struct.pack_into("<iIHhhhII", ccm, table + n * 24, code, at, 0, left, width, advance, 0, 0)
    pixels = atlas.load()
    blocks = SIZE // 4
    tiles = (blocks + 7) // 8
    data = bytearray(blocks * blocks * 8)
    for by in range(blocks):
        for bx in range(blocks):
            index = ((by // 8) * tiles + bx // 8) * 64 + morton(bx % 8, by % 8)
            data[index * 8:index * 8 + 8] = block(pixels, bx * 4, by * 4)
    name = b"DbgFont14h\0"
    start = 0x10 + 36
    offset = start + len(name)
    offset += -offset % 16
    tpf = bytearray(offset + len(data))
    tpf[0:4] = b"TPF\0"
    struct.pack_into("<III", tpf, 4, len(data), 1, 0x10304)
    struct.pack_into("<IIIHHIIIII", tpf, 0x10, offset, len(data), 0x10000, SIZE, SIZE, 1, 0xd, start, 0, 71)
    tpf[start:start + len(name)] = name
    tpf[offset:] = data
    out.mkdir(parents=True, exist_ok=True)
    (out / "DbgFont14h.ccm").write_bytes(ccm)
    (out / "DbgFont14h.tpf").write_bytes(tpf)
    atlas.save(out.parent / "atlas-check.png") if len(sys.argv) > 3 else None
    print(f"{len(glyphs)} glyphs, ccm {len(ccm)} bytes, tpf {len(tpf)} bytes")


main()
