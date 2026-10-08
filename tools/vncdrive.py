#!/usr/bin/env python3
"""Drive RPCEmu's VNC server (screen, keyboard, mouse) from a script.

The VNC control password is read from the environment (RPCEMU_VNC_PASSWORD) by the
MCP bridge module this builds on; it is never stored here.  Run it with the bridge's
interpreter so the DES library is available:

    RPCEMU_VNC_PASSWORD=... ~/.rpcemu-mcp/bin/python tools/vncdrive.py 'shot a.png' 'click 300 200'
    RPCEMU_VNC_PASSWORD=... ~/.rpcemu-mcp/bin/python tools/vncdrive.py < script.txt

Commands (one per argument, or one per line on stdin; '#' starts a comment):
    shot FILE                  save the screen as a PNG
    move X Y                   move the pointer (pixel position; see VNC_EIG0 for a mode with 1 OS unit to a pixel)
    down BTN / up BTN          hold / release a button   (select, adjust, menu)
    click X Y [BTN [COUNT]]    press and release (COUNT 2 = double click)
    drag X1 Y1 X2 Y2 [BTN]     press at 1, move in steps, release at 2
    wheel up|down [N]          mouse wheel notches at the pointer
    key NAME                   tap a key (Return, Escape, Tab, BackSpace, Delete, Left, Right,
                               Up, Down, Home, End, PageUp, PageDown, Insert, F1..F12, or a
                               character, or 0xKEYSYM)
    keydown NAME / keyup NAME  hold / release a key (Shift, Ctrl, Alt too)
    type TEXT                  type characters (UK layout: Shift is held for capitals and for
                               the characters that are on the shifted half of a key)
    wait SECONDS
The buttons are RISC OS's: in RPCEmu the middle VNC button delivers Adjust and the right
one delivers Menu, which is the reverse of what the bridge's own tool names say.
"""
import os
import struct
import sys
import time

sys.path.insert(0, "/usr/share/rpcemu/mcp")
import rpcemu_mcp as m  # noqa: E402  (reads the password from the environment)

BTN = {"select": 1, "adjust": 2, "menu": 4}
# Keys and buttons are held and spaced for a time the emulated machine can keep up with; when the host is busy
# (a big compile) it needs longer: VNC_SLOW=3 triples every such time.
SLOW = float(os.environ.get("VNC_SLOW", "1"))
# RPCEmu turns the pointer position it is sent into RISC OS coordinates as if the screen mode had 2 OS units to a
# pixel (eigen factors 1,1), whatever the mode is.  In a mode with 1 OS unit to a pixel (a "180 dpi" mode: EX0 EY0) the
# desktop therefore sees the pointer at twice the distance from the bottom left corner that the picture shows, and the
# top half of the screen cannot be reached.  VNC_EIG0=1 makes every position given to move, click and drag a pixel
# position in such a mode, and sends the VNC position that puts the desktop's pointer there (x/2, and y measured from the
# bottom likewise halved).
KEYS = {
    "return": 0xFF0D, "escape": 0xFF1B, "tab": 0xFF09, "backspace": 0xFF08, "delete": 0xFFFF,
    "left": 0xFF51, "up": 0xFF52, "right": 0xFF53, "down": 0xFF54, "home": 0xFF50, "end": 0xFF57,
    "pageup": 0xFF55, "pagedown": 0xFF56, "insert": 0xFF63, "space": 0x20,
    "shift": 0xFFE1, "ctrl": 0xFFE3, "alt": 0xFFE9,
}
KEYS.update({"f%d" % i: 0xFFBD + i for i in range(1, 13)})


# What a UK keyboard types with Shift held: the unshifted key it is on.
SHIFTED = {
    ":": ";", "$": "4", '"': "2", "@": "'", "_": "-", "+": "=", "*": "8", "(": "9", ")": "0",
    "?": "/", "<": ",", ">": ".", "{": "[", "}": "]", "|": "\\", "!": "1", "%": "5", "^": "6",
    "&": "7", "~": "#",
}


def keysym(name):
    n = name.lower()
    if n in KEYS:
        return KEYS[n]
    if n.startswith("0x"):
        return int(n, 16)
    if len(name) == 1:
        return ord(name)
    raise SystemExit("unknown key %r" % name)


EIG0 = os.environ.get("VNC_EIG0", "") not in ("", "0")


class Drive:
    def __init__(self):
        self.s, self.w, self.h = m._vnc_connect()
        self.x = self.y = self.mask = 0

    def ptr(self, x=None, y=None, mask=None):
        self.x = self.x if x is None else x
        self.y = self.y if y is None else y
        self.mask = self.mask if mask is None else mask
        px, py = self.x, self.y
        if EIG0:
            px, py = px // 2, self.h - (self.h - py) // 2
        m._vnc_pointer(self.s, px, py, self.mask)

    def key(self, sym, down):
        self.s.sendall(struct.pack(">BBHI", 4, 1 if down else 0, 0, sym))

    def run(self, line):
        a = line.split()
        if not a or a[0].startswith("#"):
            return
        c, args = a[0], a[1:]
        if c == "shot":
            open(args[0], "wb").write(m._vnc_screenshot_png())
        elif c == "move":
            self.ptr(int(args[0]), int(args[1])); time.sleep(0.05)
        elif c == "down":
            self.ptr(mask=self.mask | BTN[args[0]]); time.sleep(0.08)
        elif c == "up":
            self.ptr(mask=self.mask & ~BTN[args[0]]); time.sleep(0.05)
        elif c == "click":
            x, y = int(args[0]), int(args[1])
            b = BTN[args[2]] if len(args) > 2 else 1
            n = int(args[3]) if len(args) > 3 else 1
            self.ptr(x, y, 0); time.sleep(0.05)
            for i in range(n):
                if i:
                    time.sleep(0.12)
                self.ptr(mask=b); time.sleep(0.08)
                self.ptr(mask=0)
            time.sleep(0.05)
        elif c == "drag":
            x1, y1, x2, y2 = map(int, args[:4])
            b = BTN[args[4]] if len(args) > 4 else 1
            self.ptr(x1, y1, 0); time.sleep(0.05)
            self.ptr(mask=b); time.sleep(0.10)
            for i in range(1, 9):
                self.ptr(x1 + (x2 - x1) * i // 8, y1 + (y2 - y1) * i // 8); time.sleep(0.04)
            time.sleep(0.10)
            self.ptr(mask=0); time.sleep(0.05)
        elif c == "wheel":
            bit = 8 if args[0] == "up" else 16
            for _ in range(int(args[1]) if len(args) > 1 else 1):
                self.ptr(mask=self.mask | bit); time.sleep(0.06)
                self.ptr(mask=self.mask & ~bit); time.sleep(0.08)
        elif c == "key":
            sym = keysym(args[0])
            self.key(sym, True); time.sleep(0.06 * SLOW); self.key(sym, False); time.sleep(0.09 * SLOW)
        elif c == "keydown":
            self.key(keysym(args[0]), True); time.sleep(0.07 * SLOW)
        elif c == "keyup":
            self.key(keysym(args[0]), False); time.sleep(0.07 * SLOW)
        elif c == "type":
            for ch in line.split(None, 1)[1]:
                shift = ch.isupper() or ch in SHIFTED
                sym = keysym(SHIFTED.get(ch, ch.lower() if ch.isupper() else ch))
                if shift:
                    self.key(0xFFE1, True); time.sleep(0.08 * SLOW)
                self.key(sym, True); time.sleep(0.05 * SLOW); self.key(sym, False); time.sleep(0.08 * SLOW)
                if shift:
                    self.key(0xFFE1, False); time.sleep(0.08 * SLOW)
        elif c == "wait":
            time.sleep(float(args[0]))
        else:
            raise SystemExit("unknown command %r" % c)


def main(argv):
    d = Drive()
    lines = argv if argv else sys.stdin.read().splitlines()
    for line in lines:
        d.run(line)


if __name__ == "__main__":
    main(sys.argv[1:])
