#!/usr/bin/env python3
"""Test the local terminal (src/ro/rolocal.c) in RPCEmu without typing at the emulator.

A local terminal is a PuTTY window talking to a RISC OS task window.  The program types into it by itself when
the system variable PuTTY$LocalType is set (text, or @file; \\r \\n \\e \\t \\xHH are escapes and \\p waits a second and
a half: the TaskWindow module takes an Escape character as the Escape key and throws away what is waiting to be read,
so a program's name and the keys it is to be given cannot be sent in one go), and PuTTY's own session log
(-sessionlog) records everything the terminal received, escape sequences and all, so a case is "type this, then
compare the log with what a terminal should have been sent".  The cases:

    prompt   the supervisor prompt: Delete echoed as BS SP BS (a deleted character must leave the screen), a command,
             the environment a Unix program is given (UnixEnv$TERM, COLUMNS, LINES), and the window ending when the
             task window does (ShellCLI_TaskQuit)
    basic    BBC BASIC's VDU codes turned into ANSI: CLS, COLOUR (text and background), TAB(x,y), VDU 31
    xtest    a UnixLib program (tests/ro/xtest.c): TERM, COLUMNS, LINES and TIOCGWINSZ as it sees them, its escape
             sequences arriving whole, and the bytes it reads for keys, ESC sequences and control characters included
    inecho   3000 bytes typed at a program in raw mode (tests/ro/inecho.c): none may be lost
    escape   cursor and function key sequences, Ctrl-[ and Alt-keys (all start with an Escape character) typed at a program
             that has signals on, as bash does: the module would take each for the Escape key and end the program
    command  putty -local "Echo ...": the command is the window and it ends with it

    tools/localtest.py setup     write the input files and the script (make tests && tools/deploy.sh first)
    (then in RPCEmu:  WimpTask Run HostFS::HostFS.$.PuttyDev.wimprun HostFS::HostFS.$.PuttyDev.lt.script
     which takes about 40 seconds.  wimprun, tests/ro/wimprun.c, is a desktop task that sets the variable for each case
     and starts PuTTY with waits in between; an Obey file cannot do it: the programs it starts with WimpTask begin only
     when it has finished, so they all find the variable as the last line left it)
    tools/localtest.py check     compare the logs with what should have happened
"""
import os
import random
import sys

HOSTFS = os.environ.get("RPCEMU_HOSTFS", os.path.expanduser("~/RPCEmu/machines/RISC OS 5/hostfs"))
LT = os.path.join(HOSTFS, "PuttyDev", "lt")
GUEST = "HostFS::HostFS.$.PuttyDev"

ESC = b"\x1b"


def big_input():
    random.seed(5)
    data = bytes(random.choice(b"abcdefghijklmnopqrstuvwxyz 0123456789.,;:") for _ in range(3000))
    return data


def inputs():
    big = big_input()
    return {
        "in1": b"Echo abcdef\\x7f\\x7f\\x7fXY\\r\\pShow UnixEnv$*\\r\\pShellCLI_TaskQuit\\r",
        "in2": b'BASIC\\r\\pCLS\\rCOLOUR 1:PRINT "red":COLOUR 130:PRINT "bg":COLOUR 128:COLOUR 7\\r'
               b'PRINT TAB(10,10)"at":VDU 31,2,3,66\\rQUIT\\r\\pShellCLI_TaskQuit\\r',
        "in3": b"%b.xtest\\r\\pabc\\x7f\\t\\e[A\\eOP\\x03\\x04q\\r\\pShellCLI_TaskQuit\\r" % GUEST.encode(),
        "in4": b"%b.inecho\\r\\p%bQ\\r\\pShellCLI_TaskQuit\\r" % (GUEST.encode(), big),
        "in5": b"",
        "in6": b"%b.xtest isig\\r\\pa\\e[Ab\\e[Bc\\e[Cd\\eOPe\\e\\ef\\x1bq\\r\\pShellCLI_TaskQuit\\r" % GUEST.encode(),
    }


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def setup():
    for name, data in inputs().items():
        write(os.path.join(LT, name), data)
    for n in range(1, 7):
        try:
            os.remove(os.path.join(LT, "log%d" % n))
        except FileNotFoundError:
            pass
    script = ["| the local terminal tests (tools/localtest.py): each case is a session with its own log",
              "set PuTTY$NoEntropyWarning 1"]
    for n in range(1, 5):
        script += ["set PuTTY$LocalType @%s.lt.in%d" % (GUEST, n),
                   "run Run %s.!PuTTY.!Run -local -sessionlog %s.lt.log%d -logoverwrite" % (GUEST, GUEST, n),
                   "wait 700"]
    script += ["set PuTTY$LocalType @%s.lt.in6" % GUEST,
               "run Run %s.!PuTTY.!Run -local -sessionlog %s.lt.log6 -logoverwrite" % (GUEST, GUEST),
               "wait 700",
               "unset PuTTY$LocalType",
               'run Run %s.!PuTTY.!Run -local -sessionlog %s.lt.log5 -logoverwrite "Echo from a command"'
               % (GUEST, GUEST),
               "wait 300"]
    write(os.path.join(LT, "script"), "\n".join(script) + "\n")
    print("written to", LT)
    print("now in RPCEmu:  WimpTask Run %s.wimprun %s.lt.script   (about 40 seconds), then tools/localtest.py check"
          % (GUEST, GUEST))


def log(n):
    try:
        with open(os.path.join(LT, "log%d" % n), "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def check():
    big_sum = sum(big_input())
    cases = [
        ("prompt", 1, [
            b"Echo abcdef\x08 \x08\x08 \x08\x08 \x08XY",
            b"abcXY",
            b"UnixEnv$COLUMNS : 80", b"UnixEnv$LINES : 24", b"UnixEnv$TERM : xterm",
            b"[The task window has finished]",
        ]),
        ("basic", 2, [
            ESC + b"[2J" + ESC + b"[H",                  # CLS
            ESC + b"[31m",                               # COLOUR 1
            ESC + b"[42m",                               # COLOUR 130: background 2, green
            ESC + b"[40m", ESC + b"[37m",                # COLOUR 128, COLOUR 7
            ESC + b"[11;11H" + b"at",                    # TAB(10,10)
            ESC + b"[4;3H" + b"B",                       # VDU 31,2,3,66
            b"[The task window has finished]",
        ]),
        ("xtest", 3, [
            b"TERM=xterm COLUMNS=80 LINES=24 winsize=80x24 isatty(0)=1 isatty(1)=1",
            ESC + b"[2J" + ESC + b"[H" + ESC + b"[1;31mred" + ESC + b"[0m " + ESC + b"[42mgreen bg" + ESC + b"[0m"
            + ESC + b"[3;5Hat 3,5" + ESC + b"[10;1Htab:\tx",
            ESC + b"(0lqqk" + ESC + b"(B",
            b"61 62 63 7f 09 1b 5b 41 1b 4f 50 03 04",
            b"[The task window has finished]",
        ]),
        ("inecho", 4, [
            b"inecho: ready",
            b"inecho: count=3000 sum=%d" % big_sum,
            b"[The task window has finished]",
        ]),
        ("escape", 6, [                                  # signals on, as bash has them: Escapes are data, no SIGINT
            b"61 1b 5b 41 62 1b 5b 42 63 1b 5b 43 64 1b 4f 50",
            b"65 1b 1b 66 1b",
            b"bye",
        ]),
        ("command", 5, [
            b"from a command",
            b"[The task window has finished]",
        ]),
    ]
    failed = 0
    for name, n, wants in cases:
        data = log(n)
        if data is None:
            print("FAIL %-8s no log (did the guest script run?)" % name)
            failed += 1
            continue
        missing = [w for w in wants if w not in data]
        if missing:
            failed += 1
            print("FAIL %-8s missing %s" % (name, ", ".join(repr(w) for w in missing)))
        else:
            print("ok   %-8s" % name)
    return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("setup", "check"):
        print(__doc__)
        sys.exit(2)
    if sys.argv[1] == "setup":
        setup()
    else:
        sys.exit(check())
