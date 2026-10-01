#!/usr/bin/env python3
"""A tiny line-echo server for testing PuTTY's Telnet and Raw connections.

    tools/echoserver.py --telnet 2323     (negotiates ECHO and SGA, strips option bytes)
    tools/echoserver.py 2324              (raw: bytes in, bytes out)

From inside RPCEmu the host is 10.10.10.2.  Each typed character is echoed, and a
completed line is answered with  got: <line>.  Not for anything but testing.
"""
import socket
import sys
import threading

IAC, DONT, DO, WONT, WILL = 255, 254, 253, 252, 251
ECHO, SGA = 1, 3


def serve(conn, telnet):
    if telnet:
        conn.sendall(bytes([IAC, WILL, ECHO, IAC, WILL, SGA]))
    conn.sendall(b"echo server ready (%s)\r\n> " % (b"telnet" if telnet else b"raw"))
    line = bytearray()
    state = 0
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
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
    args = [a for a in args if a != "--telnet"]
    port = int(args[0]) if args else 2323
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(5)
    print("listening on port %d (%s)" % (port, "telnet" if telnet else "raw"))
    while True:
        c, _ = s.accept()
        threading.Thread(target=serve, args=(c, telnet), daemon=True).start()


main()
