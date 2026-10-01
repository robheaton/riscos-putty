#!/usr/bin/env python3
"""A throw-away HTTP CONNECT / SOCKS4 / SOCKS5 proxy for testing PuTTY's proxy support.

    tools/testproxy.py            HTTP CONNECT on 3128, SOCKS5 on 1080, SOCKS4 on 1081

Run it on the host and point a session at 10.10.10.2 (how RPCEmu sees this machine's
loopback).  It prints each request, so you can see whether PuTTY sent a host name or an
address, and it connects the name "10.10.10.2" (and any name ending in .test) to 127.0.0.1.
No authentication; for testing only, listening on the loopback only.
"""
import socket
import struct
import sys
import threading


def log(*a):
    print(*a, flush=True)


def local(host):
    return "127.0.0.1" if host == "10.10.10.2" or host.endswith(".test") else host


def pipe(a, b):
    try:
        while True:
            data = a.recv(65536)
            if not data:
                break
            b.sendall(data)
    except OSError:
        pass
    finally:
        for s in (a, b):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def relay(client, host, port, kind):
    try:
        upstream = socket.create_connection((local(host), port), timeout=10)
    except OSError as e:
        log("%s: cannot connect to %s:%d: %s" % (kind, host, port, e))
        return None
    threading.Thread(target=pipe, args=(client, upstream), daemon=True).start()
    threading.Thread(target=pipe, args=(upstream, client), daemon=True).start()
    return upstream


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise OSError("closed")
        buf += chunk
    return buf


def http_connect(c):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = c.recv(4096)
        if not chunk:
            return
        data += chunk
    first = data.split(b"\r\n")[0].decode(errors="replace")
    log("HTTP  request: %r" % first)
    parts = first.split()
    if len(parts) < 2 or parts[0] != "CONNECT":
        c.sendall(b"HTTP/1.1 405 Method Not Allowed\r\n\r\n")
        return
    host, _, port = parts[1].rpartition(":")
    if relay(c, host, int(port), "HTTP"):
        c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
    else:
        c.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")


def socks5(c):
    ver, n = recv_exact(c, 2)
    methods = recv_exact(c, n)
    log("SOCKS5 greeting: methods %s" % list(methods))
    c.sendall(b"\x05\x00")
    ver, cmd, _, atyp = recv_exact(c, 4)
    if atyp == 1:
        host = socket.inet_ntoa(recv_exact(c, 4))
    elif atyp == 3:
        host = recv_exact(c, recv_exact(c, 1)[0]).decode()
    else:
        host = socket.inet_ntop(socket.AF_INET6, recv_exact(c, 16))
    port = struct.unpack(">H", recv_exact(c, 2))[0]
    log("SOCKS5 request: cmd %d, %s %s:%d" % (cmd, "name" if atyp == 3 else "address", host, port))
    ok = cmd == 1 and relay(c, host, port, "SOCKS5")
    c.sendall(b"\x05" + (b"\x00" if ok else b"\x05") + b"\x00\x01\x00\x00\x00\x00\x00\x00")


def socks4(c):
    ver, cmd, port = struct.unpack(">BBH", recv_exact(c, 4))
    addr = recv_exact(c, 4)
    user = b""
    while True:
        ch = c.recv(1)
        if ch in (b"", b"\0"):
            break
        user += ch
    if addr[:3] == b"\0\0\0" and addr[3] != 0:          # SOCKS4a: a name follows
        name = b""
        while True:
            ch = c.recv(1)
            if ch in (b"", b"\0"):
                break
            name += ch
        host = name.decode()
        log("SOCKS4a request: cmd %d, name %s:%d (user %r)" % (cmd, host, port, user))
    else:
        host = socket.inet_ntoa(addr)
        log("SOCKS4 request: cmd %d, address %s:%d (user %r)" % (cmd, host, port, user))
    ok = cmd == 1 and relay(c, host, port, "SOCKS4")
    c.sendall(b"\0" + (b"\x5a" if ok else b"\x5b") + b"\0\0\0\0\0\0")


def serve(port, handler):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(5)
    # the guest reaches this machine's loopback as 10.10.10.2, which RPCEmu maps to 127.0.0.1
    log("listening on 127.0.0.1:%d (%s)" % (port, handler.__name__))
    while True:
        c, _ = s.accept()

        def run(c=c):
            try:
                handler(c)
            except OSError as e:
                log("%s: %s" % (handler.__name__, e))

        threading.Thread(target=run, daemon=True).start()


def main():
    for port, handler in ((3128, http_connect), (1080, socks5), (1081, socks4)):
        threading.Thread(target=serve, args=(port, handler), daemon=True).start()
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        sys.exit(0)


main()
