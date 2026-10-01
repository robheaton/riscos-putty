#!/usr/bin/env python3
"""Convert a RISC OS sprite file (as written by *ScreenSave) to a PNG.

    sprite2png.py shot,ff9 shot.png [x0 y0 x1 y1]     # optional crop, top-left origin

Handles 32 bpp (the emulator's normal desktop), 16 bpp and 8 bpp (grey/palette
read from the sprite's own palette when present, else the standard 256 colours).
Only the first sprite in the file is read.
"""
import struct
import sys

from PIL import Image


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def load(path):
    data = open(path, "rb").read()
    nsprites, first, free = struct.unpack_from("<III", data, 0)
    off = first - 4                      # file offsets are 4 less than area offsets
    nxt = u32(data, off)
    name = data[off + 4:off + 16].split(b"\0")[0].decode("latin-1")
    wwords = u32(data, off + 16) + 1
    height = u32(data, off + 20) + 1
    first_bit = u32(data, off + 24)
    last_bit = u32(data, off + 28)
    image = off + u32(data, off + 32)
    mode = u32(data, off + 40)
    if mode & 1:                          # new-format mode word
        sprtype = (mode >> 27) & 31
        bpp = {1: 1, 2: 2, 3: 4, 4: 8, 5: 16, 6: 32, 7: 64}.get(sprtype)
        if sprtype == 5:
            bpp = 16
    else:                                 # old numbered mode: 8bpp is the only one we need
        bpp = 8
    if bpp is None:
        raise SystemExit("unsupported sprite type in mode word %#x" % mode)
    width = (wwords * 32 - first_bit - (31 - last_bit)) // bpp
    return data, image, width, height, wwords, bpp, name


def convert(path):
    data, image, width, height, wwords, bpp, name = load(path)
    rowbytes = wwords * 4
    img = Image.new("RGB", (width, height))
    px = img.load()
    for y in range(height):
        row = image + y * rowbytes
        if bpp == 32:
            raw = data[row:row + width * 4]
            for x in range(width):
                r, g, b = raw[4 * x], raw[4 * x + 1], raw[4 * x + 2]
                px[x, y] = (r, g, b)
        elif bpp == 16:
            for x in range(width):
                v = struct.unpack_from("<H", data, row + 2 * x)[0]
                r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
                px[x, y] = (r * 255 // 31, g * 255 // 31, b * 255 // 31)
        elif bpp == 8:
            for x in range(width):
                v = data[row + x]
                px[x, y] = (v, v, v)
    return img


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    im = convert(sys.argv[1])
    if len(sys.argv) >= 7:
        x0, y0, x1, y1 = map(int, sys.argv[3:7])
        im = im.crop((x0, y0, x1, y1))
    im.save(sys.argv[2])
    print("wrote %s (%dx%d)" % (sys.argv[2], im.width, im.height))
