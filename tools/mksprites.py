#!/usr/bin/env python3
"""Generate the applications' sprite files (!Sprites) - no drawing program needed.

!PuTTY's icon is a small monitor showing a terminal prompt, !PuTTYgen's a golden key.
Each is drawn on a 34 x 34 pixel grid (68 x 68 OS units in a 90 dpi mode, the usual size
for an iconbar / Filer icon) plus a 17 x 17 one for the Filer's "small icons" view, in 32
bit colour with a 1 bit mask, which every RISC OS since 3.5 (with SpriteExtend) can plot
in any screen mode.  (The writer is the one from riscos-vncclient.)

    python3 tools/mksprites.py app/'!PuTTY'/'!Sprites,ff9'
    python3 tools/mksprites.py app/'!PuTTYgen'/'!Sprites,ff9' key
"""
import struct
import sys

BLACK = (0, 0, 0)
BODY = (0xDD, 0xDD, 0xDD)
LIGHT = (0xFF, 0xFF, 0xFF)
SHADE = (0x99, 0x99, 0x99)
DARK = (0x55, 0x55, 0x55)
SCREEN = (0x0A, 0x0A, 0x0A)
GLARE = (0x30, 0x30, 0x30)
GREEN = (0x40, 0xE0, 0x40)
AMBER = (0xF0, 0xB0, 0x30)
CLEAR = (0, 0, 0, 0)


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.p = [[CLEAR] * w for _ in range(h)]

    def px(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.p[y][x] = c if len(c) == 4 else (c[0], c[1], c[2], 255)

    def rect(self, x0, y0, x1, y1, c):
        """Fill x0 <= x < x1, y0 <= y < y1."""
        for y in range(y0, y1):
            for x in range(x0, x1):
                self.px(x, y, c)

    def frame(self, x0, y0, x1, y1, c):
        self.rect(x0, y0, x1, y0 + 1, c)
        self.rect(x0, y1 - 1, x1, y1, c)
        self.rect(x0, y0, x0 + 1, y1, c)
        self.rect(x1 - 1, y0, x1, y1, c)

    def bitmap(self, x, y, rows, colour):
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch == "X":
                    self.px(x + i, y + j, colour)


PROMPT = [
    "XX...",
    ".XX..",
    "..XX.",
    ".XX..",
    "XX...",
]

SMALL_PROMPT = [
    "X..",
    ".X.",
    "X..",
]


def large():
    c = Canvas(34, 34)
    # monitor body with rounded corners
    c.rect(1, 1, 33, 25, BLACK)
    for x, y in ((1, 1), (32, 1), (1, 24), (32, 24)):
        c.px(x, y, CLEAR)
    c.rect(2, 2, 32, 24, BODY)
    for x, y in ((2, 2), (31, 2), (2, 23), (31, 23)):
        c.px(x, y, BLACK)
    c.rect(3, 2, 31, 3, LIGHT)              # top highlight
    c.rect(2, 3, 3, 22, LIGHT)              # left highlight
    c.rect(3, 23, 31, 24, SHADE)            # bottom shadow
    c.rect(31, 3, 32, 23, SHADE)            # right shadow
    # the screen: a dark terminal with a prompt, a line of output and a cursor
    c.frame(4, 4, 30, 21, DARK)
    c.rect(5, 5, 29, 20, SCREEN)
    c.rect(5, 5, 29, 6, GLARE)
    c.bitmap(7, 8, PROMPT, GREEN)
    c.rect(14, 11, 20, 12, AMBER)           # something typed
    c.rect(14, 14, 25, 15, GREEN)           # output
    c.rect(14, 17, 18, 19, GREEN)           # block cursor
    # neck and base
    c.rect(14, 25, 20, 29, BLACK)
    c.rect(15, 25, 19, 29, SHADE)
    c.rect(9, 29, 25, 32, BLACK)
    c.rect(10, 30, 24, 31, BODY)
    for x, y in ((9, 31), (24, 31)):
        c.px(x, y, CLEAR)
    return c


def small():
    c = Canvas(17, 17)
    c.rect(0, 1, 17, 13, BLACK)
    for x, y in ((0, 1), (16, 1), (0, 12), (16, 12)):
        c.px(x, y, CLEAR)
    c.rect(1, 2, 16, 12, BODY)
    c.rect(2, 3, 15, 11, SCREEN)
    c.bitmap(3, 4, SMALL_PROMPT, GREEN)
    c.rect(7, 6, 12, 7, AMBER)
    c.rect(7, 8, 10, 9, GREEN)
    c.rect(6, 13, 11, 14, BLACK)
    c.rect(4, 14, 13, 16, BLACK)
    c.rect(5, 14, 12, 15, BODY)
    return c


GOLD = (0xF0, 0xC0, 0x30)
GOLD_LIGHT = (0xFF, 0xEC, 0x90)
GOLD_DARK = (0xB8, 0x86, 0x10)
KEY_EDGE = (0x40, 0x2C, 0x00)


def key(size):
    """A key lying diagonally, bow at the top left and teeth at the bottom right, drawn by
    working out for every pixel whether it is inside the key (so one description serves
    both sizes)."""
    k = size / 34.0
    c = Canvas(size, size)
    bow = (10.5 * k, 10.5 * k)
    inside = [[False] * size for _ in range(size)]
    root2 = 2 ** 0.5
    for y in range(size):
        for x in range(size):
            px, py = (x + 0.5) / k, (y + 0.5) / k
            bx, by = px - bow[0] / k, py - bow[1] / k
            r = (bx * bx + by * by) ** 0.5
            t = (bx + by) / root2                # along the shaft
            s = (by - bx) / root2                # across it (positive: towards the bottom left)
            in_bow = 3.4 < r <= 8.6
            in_shaft = 6.0 <= t <= 27.0 and abs(s) <= 2.0
            in_tooth = any(t0 <= t <= t0 + 2.6 and 0 <= s <= 6.2 for t0 in (19.0, 23.8))
            in_tip = 24.0 <= t <= 27.0 and abs(s) <= 2.0
            inside[y][x] = in_bow or in_shaft or in_tooth or in_tip
    for y in range(size):
        for x in range(size):
            if not inside[y][x]:
                continue
            edge = any(not (0 <= x + dx < size and 0 <= y + dy < size and inside[y + dy][x + dx])
                       for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            if edge:
                col = KEY_EDGE
            else:
                up = y > 0 and not inside[y - 1][x] or x > 0 and not inside[y][x - 1]
                lo = y + 1 < size and not inside[y + 1][x] or x + 1 < size and not inside[y][x + 1]
                col = GOLD_LIGHT if up else GOLD_DARK if lo else GOLD
            c.px(x, y, col)
    # the hole in the bow
    return c


def sprite(name, cv):
    """One sprite: 32bpp image + 1bpp mask, as a byte string."""
    w, h = cv.w, cv.h
    mask_words = (w + 31) // 32
    image_off = 44
    mask_off = image_off + w * 4 * h
    size = mask_off + mask_words * 4 * h
    mode = (6 << 27) | (90 << 14) | (90 << 1) | 1      # 32bpp, 90 x 90 dpi
    hdr = struct.pack("<I12sIIIIIII", size, name.encode().ljust(12, b"\0"),
                      w - 1, h - 1, 0, 31, image_off, mask_off, mode)
    body = bytearray()
    for row in cv.p:
        for (r, g, b, a) in row:
            body += struct.pack("<I", ((b << 16) | (g << 8) | r) if a else 0)
    for row in cv.p:
        words = [0] * mask_words
        for x, (_, _, _, a) in enumerate(row):
            if a:
                words[x // 32] |= 1 << (x % 32)
        body += struct.pack("<%dI" % mask_words, *words)
    return hdr + bytes(body)


def sprite_file(sprites):
    """A sprite file: count, first-sprite offset, free offset, then the sprites."""
    data = b"".join(sprites)
    return struct.pack("<III", len(sprites), 16, 16 + len(data)) + data


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "!Sprites,ff9"
    which = sys.argv[2] if len(sys.argv) > 2 else "putty"
    if which == "key":
        sprites = [sprite("!puttygen", key(34)), sprite("sm!puttygen", key(17))]
    else:
        sprites = [sprite("!putty", large()), sprite("sm!putty", small())]
    with open(out, "wb") as f:
        f.write(sprite_file(sprites))
    print("wrote", out)


if __name__ == "__main__":
    main()
