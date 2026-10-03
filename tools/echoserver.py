#!/usr/bin/env python3
"""A tiny line-echo server for testing PuTTY's Telnet, Rlogin and Raw connections.

    tools/echoserver.py --telnet 2323     (negotiates ECHO and SGA, strips option bytes)
    tools/echoserver.py --rlogin 2325     (reads the Rlogin start-up, says what it saw, then echoes)
    tools/echoserver.py 2324              (raw: bytes in, bytes out)

From inside RPCEmu the host is 10.10.10.2.  Each typed character is echoed, and a
completed line is answered with  got: <line>.  Not for anything but testing.
"""
import socket
import sys
import threading

IAC, DONT, DO, WONT, WILL = 255, 254, 253, 252, 251
ECHO, SGA = 1, 3


def serve(conn, telnet, rlogin=False):
    pending = b""
    if telnet:
        conn.sendall(bytes([IAC, WILL, ECHO, IAC, WILL, SGA]))
    if rlogin:
        # the client sends  \0 client-user \0 server-user \0 terminal/speed \0  and waits for a \0
        buf = b""
        while buf.count(b"\0") < 4:
            data = conn.recv(4096)
            if not data:
                conn.close()
                return
            buf += data
        fields = buf.split(b"\0", 4)
        pending = fields[4]                # the client does not wait for our \0: input may already be here
        conn.sendall(b"\0")
        conn.sendall(b"rlogin ok: client=%s server=%s terminal=%s\r\n> " % (fields[1], fields[2], fields[3]))
    else:
        conn.sendall(b"echo server ready (%s)\r\n> " % (b"telnet" if telnet else b"raw"))
    line = bytearray()
    state = 0
    try:
        while True:
            if pending:
                data, pending = pending, b""
            else:
                data = conn.recv(4096)
            if not data:
                break
            if rlogin:
                print("rlogin client sent %r" % data, flush=True)
            for b in data:
                if telnet:
                    if state == 0 and b == IAC:
                        state = 1
                        continue
                    if state == 1:
                        state = 2 if b in (DO, DONT, WILL, WONT) else 0
                        continue
                    if state == 2:
                        state = 0
                        continue
                if b in (13, 10):
                    if b == 13 or not line:
                        conn.sendall(b"\r\ngot: " + bytes(line) + b"\r\n> ")
                        line.clear()
                    continue
                if b in (8, 127):
                    if line:
                        line.pop()
                        conn.sendall(b"\b \b")
                    continue
                line.append(b)
                conn.sendall(bytes([b]))
    except OSError:
        pass
    finally:
        conn.close()


def main():
    args = sys.argv[1:]
    telnet = "--telnet" in args
    rlogin = "--rlogin" in args
    args = [a for a in args if a not in ("--telnet", "--rlogin")]
    port = int(args[0]) if args else 2323
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(5)
    print("listening on port %d (%s)" % (port, "telnet" if telnet else "rlogin" if rlogin else "raw"))
    while True:
        c, _ = s.accept()
        threading.Thread(target=serve, args=(c, telnet, rlogin), daemon=True).start()


main()
