#!/usr/bin/env python3
"""Check what the terminal window sends for the special keys, in the emulator.

    RPCEMU_VNC_PASSWORD=... ~/.rpcemu-mcp/bin/python tools/termkeys.py [--session NAME] [--click X Y]

Needs the test server running (tools/testserver.sh) and a PuTTY session window open on it, with the
saved session this script makes (it writes `keytest` into the emulator's Choices, a copy of the test
server's session with Ctrl-Shift-C/V set to copy and paste):

    WimpTask Run HostFS::HostFS.$.PuttyDev.!PuTTY.!RunImage -load keytest

The window must be where a new one opens (click X Y, default 800 400, is a point inside it).  The script
starts `cat` on the server with the line discipline switched off, writing what arrives to a file in the
server's home directory (build/test/home/keys.bin, which the host can read), holds each key for a
moment through VNC with `tools/vncdrive.py`, follows it with a "/" and compares what was captured with
what PuTTY sends on Windows and Unix.  Keys are held, not tapped: Page Up/Down and Home arrive as the
codes of other keys and are told apart by asking the keyboard which key is down.

Exits 1 if any key sent something else.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vncdrive  # noqa: E402  (reads the VNC password from the environment)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOSTFS = os.environ.get("RPCEMU_HOSTFS", os.path.expanduser("~/RPCEmu/machines/RISC OS 5/hostfs"))
KEYS_BIN = os.path.join(ROOT, "build", "test", "home", "keys.bin")

ESC = b"\x1b"

# label, modifiers held, key, what PuTTY sends (a key may repeat while it is held: the bytes may come
# several times over; None means the clipboard's contents).
CASES = [
    ("Up", [], "Up", ESC + b"[A"),
    ("Down", [], "Down", ESC + b"[B"),
    ("Right", [], "Right", ESC + b"[C"),
    ("Left", [], "Left", ESC + b"[D"),
    ("Home", [], "Home", ESC + b"[1~"),
    ("End", [], "End", ESC + b"[4~"),
    ("Insert", [], "Insert", ESC + b"[2~"),
    ("Delete", [], "Delete", ESC + b"[3~"),
    ("PageUp", [], "PageUp", ESC + b"[5~"),
    ("PageDown", [], "PageDown", ESC + b"[6~"),
    ("F1", [], "F1", ESC + b"[11~"),
    ("F5", [], "F5", ESC + b"[15~"),
    ("F6", [], "F6", ESC + b"[17~"),
    ("F9", [], "F9", ESC + b"[20~"),
    ("F10", [], "F10", ESC + b"[21~"),
    ("F11", [], "F11", ESC + b"[23~"),
    ("F12", [], "F12", ESC + b"[24~"),
    ("Shift+F1", ["shift"], "F1", ESC + b"[23~"),
    ("Tab", [], "Tab", b"\t"),
    ("Shift+Tab", ["shift"], "Tab", ESC + b"[Z"),
    ("BackSpace", [], "BackSpace", b"\x7f"),
    ("Shift+BackSpace", ["shift"], "BackSpace", b"\x08"),
    ("Return", [], "Return", b"\r"),
    ("Escape", [], "Escape", ESC),
    ("Shift+PageUp", ["shift"], "PageUp", b""),         # scrolls the terminal instead
    ("Shift+PageDown", ["shift"], "PageDown", b""),
    ("Ctrl+PageUp", ["ctrl"], "PageUp", b""),           # ... by a line
    ("Ctrl+PageDown", ["ctrl"], "PageDown", b""),
    ("Shift+Up", ["shift"], "Up", ESC + b"[A"),
    ("Shift+Home", ["shift"], "Home", ESC + b"[1~"),
    ("Ctrl+Up", ["ctrl"], "Up", ESC + b"OA"),
    ("Ctrl+Down", ["ctrl"], "Down", ESC + b"OB"),
    ("Ctrl+Home", ["ctrl"], "Home", ESC + b"[1~"),       # not Ctrl-^, which has the same code
    ("Ctrl+a", ["ctrl"], "a", b"\x01"),
    ("Ctrl+Space", ["ctrl"], "space", b"\x00"),
    ("Ctrl+6", ["ctrl"], "6", b"\x1e"),                  # Ctrl-^
    ("Ctrl+Shift+C", ["ctrl", "shift"], "c", b""),       # copies (keytest session): not ^C
    ("Ctrl+Shift+V", ["ctrl", "shift"], "v", None),      # ... and pastes the clipboard: whatever it holds, not ^V
    ("Alt+x", ["alt"], "x", ESC + b"x"),
    ("Alt+Space", ["alt"], "space", ESC + b" "),
    ("Alt+Tab", ["alt"], "Tab", ESC + b"\t"),
    ("Alt+Up", ["alt"], "Up", ESC + ESC + b"[A"),
    ("Alt+Left", ["alt"], "Left", ESC + ESC + b"[D"),
    ("Alt+Home", ["alt"], "Home", ESC + ESC + b"[1~"),
    ("Alt+Delete", ["alt"], "Delete", ESC + ESC + b"[3~"),
    ("Alt+PageUp", ["alt"], "PageUp", ESC + ESC + b"[5~"),
    ("Alt+BackSpace", ["alt"], "BackSpace", ESC + b"\x7f"),
    ("Alt+Return", ["alt"], "Return", ESC + b"\r"),
    ("Alt+Escape", ["alt"], "Escape", ESC + ESC),
    ("Alt+F1", ["alt"], "F1", ESC + ESC + b"[11~"),
    ("Alt+F12", ["alt"], "F12", ESC + ESC + b"[24~"),
]


def make_session():
    """Write the saved session `keytest` from the test server's, if there is one to copy."""
    sdir = os.path.join(HOSTFS, "!Boot", "Choices", "PuTTY", "Sessions")
    src, dst = os.path.join(sdir, "testserver"), os.path.join(sdir, "keytest")
    lines = [l for l in open(src).read().splitlines() if not l.startswith(("CtrlShiftCV=", "CtrlShiftIns="))]
    lines += ["CtrlShiftCV=explicit", "CtrlShiftIns=explicit"]
    open(dst, "w").write("\n".join(lines) + "\n")
    print("wrote", dst)


def main(argv):
    x, y = 800, 400
    if "--click" in argv:
        i = argv.index("--click")
        x, y = int(argv[i + 1]), int(argv[i + 2])
    make_session()
    d = vncdrive.Drive()
    for line in ("click %d %d" % (x, y), "wait 1", "keydown ctrl", "wait 0.2", "key u", "wait 0.2", "keyup ctrl",
                 "wait 0.5", "type sh -c 'stty raw -echo; exec cat > keys.bin'", "key Return", "wait 3"):
        d.run(line)
    for label, mods, key, want in CASES:
        for m in mods:
            d.run("keydown " + m)
        time.sleep(0.15)
        d.run("keydown " + key)
        time.sleep(0.2)
        d.run("keyup " + key)
        time.sleep(0.1)
        for m in reversed(mods):
            d.run("keyup " + m)
        time.sleep(0.4)
        d.run("type /")
        time.sleep(0.3)
    time.sleep(2)
    got = open(KEYS_BIN, "rb").read().split(b"/")
    bad = 0
    for i, (label, mods, key, want) in enumerate(CASES):
        g = got[i] if i < len(got) else None
        if want is None:                    # the clipboard's contents: anything but the control character
            ok = g is not None and b"\x16" not in g
        else:
            ok = g is not None and (g == b"" if not want else
                                     len(g) >= len(want) and len(g) % len(want) == 0 and
                                     g == want * (len(g) // len(want)))
        print("%-4s %-16s sent %-24r wanted %r" % ("ok" if ok else "FAIL", label, g, want if want is not None else "the clipboard"))
        bad += not ok
    print("%d of %d keys right" % (len(CASES) - bad, len(CASES)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
