#!/usr/bin/env python3
"""Exercise PuTTY's SSH algorithms on the RISC OS build (plink in RPCEmu) against Uppity.

PuTTY's code is compiled unchanged for 32-bit ARM with an old GCC, so what needs
checking is that every cipher, MAC, key exchange, host key type, user key type and the
compression give the same answers there as everywhere else.  Uppity (PuTTY's own test
server, built for the host) can be told to offer just one algorithm of each kind, so
each case is a plink run that can only succeed if that one algorithm worked.

    tools/cryptomatrix.py setup [--fresh]   make keys, start the servers, write the case list
                                     (leave it running; Ctrl-C stops the servers)
    (then in RPCEmu, for each case:  Obey HostFS::HostFS.$.PuttyDev.mxc.c00, c01, ...)
    tools/cryptomatrix.py check      read the outputs the run left behind and report

The servers listen on ports 3000 and up of this machine's loopback, which the guest
sees as 10.10.10.2.  Like tools/testserver.sh this is for testing only.
"""
import hashlib
import os
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "build", "host")
WORK = os.path.join(ROOT, "build", "test", "matrix")
HOSTFS = os.environ.get("RPCEMU_HOSTFS", os.path.expanduser("~/RPCEmu/machines/RISC OS 5/hostfs"))
DEST = os.path.join(HOSTFS, "PuttyDev")
GUEST = "HostFS::HostFS.$.PuttyDev"
GUEST_IP = "10.10.10.2"
FIRST_PORT = 3000
BULK_LINES = 200000                 # "seq 1 N" is about 1.3 MB
BULK_IN_LINES = 40000               # the file sent to the server, about 1.2 MB
BULK_IN = "".join("line %d of the bulk input\n" % n for n in range(1, BULK_IN_LINES + 1)).encode()
BULK_IN_MD5 = hashlib.md5(BULK_IN).hexdigest()

# name -> puttygen arguments.  Used as host keys and as user keys.
KEYS = {
    "ed25519": ["-t", "ed25519"],
    "ed448":   ["-t", "ed448"],
    "p256":    ["-t", "ecdsa", "-b", "256"],
    "p384":    ["-t", "ecdsa", "-b", "384"],
    "p521":    ["-t", "ecdsa", "-b", "521"],
    "rsa2048": ["-t", "rsa", "-b", "2048"],
    "dsa1024": ["-t", "dsa", "-b", "1024"],
    "rsa1kex": ["-t", "rsa1", "-b", "2048"],     # SSH-1 format: the key for rsa2048-sha256 key exchange
}

CIPHERS = ["aes256-ctr", "aes192-ctr", "aes128-ctr", "aes256-cbc", "aes192-cbc", "aes128-cbc",
           "aes256-gcm@openssh.com", "aes128-gcm@openssh.com", "chacha20-poly1305@openssh.com",
           "3des-cbc"]
MACS = ["hmac-sha2-256", "hmac-sha2-512", "hmac-sha1", "hmac-sha1-96", "hmac-md5",
        "hmac-sha2-256-etm@openssh.com", "hmac-sha2-512-etm@openssh.com", "hmac-sha1-etm@openssh.com"]
KEXES = ["curve25519-sha256", "curve448-sha512", "ecdh-sha2-nistp256", "ecdh-sha2-nistp384",
         "ecdh-sha2-nistp521", "diffie-hellman-group-exchange-sha256", "diffie-hellman-group-exchange-sha1",
         "diffie-hellman-group14-sha256", "diffie-hellman-group14-sha1", "diffie-hellman-group15-sha512",
         "diffie-hellman-group16-sha512", "diffie-hellman-group17-sha512", "diffie-hellman-group18-sha512",
         "sntrup761x25519-sha512", "mlkem768x25519-sha256", "mlkem768nistp256-sha256",
         "mlkem1024nistp384-sha384", "rsa2048-sha256"]
HOSTKEYS = [("ssh-ed25519", "ed25519"), ("ssh-ed448", "ed448"), ("ecdsa-sha2-nistp256", "p256"),
            ("ecdsa-sha2-nistp384", "p384"), ("ecdsa-sha2-nistp521", "p521"),
            ("rsa-sha2-256", "rsa2048"), ("rsa-sha2-512", "rsa2048"), ("ssh-rsa", "rsa2048")]
USERKEYS = ["ed25519", "ed448", "p256", "p384", "p521", "rsa2048", "dsa1024"]


def run(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kw).stdout


def make_keys():
    os.makedirs(WORK, exist_ok=True)
    fps = {}
    for name, gen in KEYS.items():
        ppk = os.path.join(WORK, name + ".ppk")
        pub = os.path.join(WORK, name + ".pub")
        if not os.path.exists(ppk):
            run(os.path.join(HOST, "puttygen"), *gen, "-o", ppk, "--new-passphrase", "/dev/null")
        if name == "rsa1kex":
            continue
        if not os.path.exists(pub):
            run(os.path.join(HOST, "puttygen"), "-O", "public-openssh", ppk, "-o", pub)
        fps[name] = run(os.path.join(HOST, "puttygen"), "-l", ppk).split()[2]
    return fps


def cases():
    """(name, server options, plink options, user key or None) for each case."""
    out = []
    base_c = ["--kexinit-kex", "curve25519-sha256"]

    def add(name, server, plink=(), user="ed25519", hostkey="ed25519", remote=None, verify=None, stdin=None):
        out.append(dict(name=name, server=list(server), plink=list(plink), user=user, hostkey=hostkey,
                        remote=remote, verify=verify, stdin=stdin))

    for c in CIPHERS:
        add("cipher-" + c.split("@")[0], ["--kexinit-cscipher", c, "--kexinit-sccipher", c])
    for m in MACS:
        add("mac-" + m.split("@")[0],
            ["--kexinit-cscipher", "aes256-ctr", "--kexinit-sccipher", "aes256-ctr",
             "--kexinit-csmac", m, "--kexinit-scmac", m])
    for k in KEXES:
        extra = []
        hk = "ed25519"
        if k == "rsa2048-sha256":
            extra = ["--rsakexkey", os.path.join(WORK, "rsa1kex.ppk")]
        add("kex-" + k, ["--kexinit-kex", k] + extra)
    for alg, key in HOSTKEYS:
        add("hostkey-" + alg, ["--kexinit-hostkey", alg], hostkey=key)
    for key in USERKEYS:
        add("userkey-" + key, [], user=key)
    add("compress-zlib", ["--kexinit-cscomp", "zlib", "--kexinit-sccomp", "zlib"], plink=["-C"])
    add("compress-zlib-openssh", ["--kexinit-cscomp", "zlib@openssh.com", "--kexinit-sccomp", "zlib@openssh.com"],
        plink=["-C"])
    add("password", ["--deny-auth", "kbdint"], user=None)

    # Bulk data, both ways, with and without rekeying and compression: the output must be
    # exactly what the host computes (an md5 of what was sent or received).
    down = "seq 1 %d" % BULK_LINES
    down_md5 = hashlib.md5(("".join("%d\n" % n for n in range(1, BULK_LINES + 1))).encode()).hexdigest()

    def same_as_down(text):
        return hashlib.md5(text.encode(errors="replace")).hexdigest() == down_md5

    def md5_of_input(text):
        return BULK_IN_MD5 in text

    add("bulk-down", [], remote=down, verify=same_as_down)
    add("bulk-down-rekey", [], plink=["-load", "mxrekey"], remote=down, verify=same_as_down)
    add("bulk-down-zlib", ["--kexinit-cscomp", "zlib", "--kexinit-sccomp", "zlib"], plink=["-C"], remote=down,
        verify=same_as_down)
    add("bulk-down-chacha-rekey", ["--kexinit-cscipher", "chacha20-poly1305@openssh.com",
                                    "--kexinit-sccipher", "chacha20-poly1305@openssh.com"],
        plink=["-load", "mxrekey"], remote=down, verify=same_as_down)
    add("bulk-up", [], remote="md5sum", verify=md5_of_input, stdin="mxbulkin")
    add("bulk-up-rekey", [], plink=["-load", "mxrekey"], remote="md5sum", verify=md5_of_input, stdin="mxbulkin")
    add("bulk-up-aesgcm-rekey", ["--kexinit-cscipher", "aes256-gcm@openssh.com",
                                  "--kexinit-sccipher", "aes256-gcm@openssh.com"],
        plink=["-load", "mxrekey"], remote="md5sum", verify=md5_of_input, stdin="mxbulkin")
    return out


def start_servers(fps, cs):
    args = [os.path.join(HOST, "uppity")]
    for i, c in enumerate(cs):
        if i:
            args.append("--and")
        args += ["--listen", str(FIRST_PORT + i), "--hostkey", os.path.join(WORK, c["hostkey"] + ".ppk")]
        for u in USERKEYS:
            args += ["--userkey", os.path.join(WORK, u + ".pub")]
        args += ["--sessiondir", os.path.join(WORK, "home")] + c["server"]
    os.makedirs(os.path.join(WORK, "home"), exist_ok=True)
    env = {"HOME": os.path.join(WORK, "home"), "PATH": "/usr/bin:/bin", "SHELL": "/bin/sh", "PS1": "$ "}
    log = open(os.path.join(WORK, "uppity.log"), "w")
    p = subprocess.Popen(args, env=env, stdout=log, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    if p.poll() is not None:
        sys.exit("uppity did not start; see %s" % os.path.join(WORK, "uppity.log"))
    return p


def write_list(fps, cs):
    os.makedirs(os.path.join(DEST, "mx"), exist_ok=True)
    if "--fresh" in sys.argv:                           # forget the results of earlier runs
        for f in os.listdir(os.path.join(DEST, "mx")):
            os.remove(os.path.join(DEST, "mx", f))
    for u in USERKEYS:
        dst = os.path.join(DEST, "mxkey_" + u)
        with open(os.path.join(WORK, u + ".ppk"), "rb") as s, open(dst, "wb") as d:
            d.write(s.read())
        os.chmod(dst, 0o600)
    # One Obey file per case: a plink that fails would stop an Obey file that held several,
    # and a program cannot run another from inside itself, so the cases are run one by one
    # (Obey HostFS::HostFS.$.PuttyDev.mxc.cNN) from outside.
    os.makedirs(os.path.join(DEST, "mxc"), exist_ok=True)
    for f in os.listdir(os.path.join(DEST, "mxc")):
        os.remove(os.path.join(DEST, "mxc", f))
    with open(os.path.join(DEST, "mxbulkin"), "wb") as f:
        f.write(BULK_IN)
    sessions = os.path.join(HOSTFS, "!Boot", "Choices", "PuTTY", "Sessions")
    os.makedirs(sessions, exist_ok=True)
    with open(os.path.join(sessions, "mxrekey"), "w") as f:         # rekey every 100 KB
        f.write("RekeyBytes=100K\n")
    lines = ["# generated by tools/cryptomatrix.py"]
    for i, c in enumerate(cs):
        cmd = [GUEST + ".plink", "-batch", "-ssh", "-P", str(FIRST_PORT + i), "-l", "tester",
               "-hostkey", fps[c["hostkey"]]]
        if c["user"]:
            cmd += ["-i", GUEST + ".mxkey_" + c["user"]]
        else:
            cmd += ["-pw", "weasel"]
        cmd += c["plink"] + [GUEST_IP] + (c["remote"] or "echo OK-" + c["name"]).split()
        line = "Run " + " ".join(cmd)
        if c["stdin"]:                      # the C library's "< file"; the Wimp's "{ < file }" is not seen by plink
            line += " < %s.%s" % (GUEST, c["stdin"])
        line += " { > %s.mx.o%02d }" % (GUEST, i)
        lines.append(line)
        with open(os.path.join(DEST, "mxc", "c%02d,feb" % i), "w") as f:
            f.write(line + "\n")
    with open(os.path.join(DEST, "matrix"), "w") as f:
        f.write("\n".join(lines) + "\n")


def check(cs):
    bad = 0
    for i, c in enumerate(cs):
        found = [f for f in os.listdir(os.path.join(DEST, "mx")) if f.split(",")[0] == "o%02d" % i]
        path = os.path.join(DEST, "mx", found[0]) if found else None
        # the guest ends lines with LF CR; read bytes so that Python does not make that two lines
        text = open(path, "rb").read().replace(b"\r", b"").decode(errors="replace") if path else "(no output)"
        ok = c["verify"](text) if c["verify"] else ("OK-" + c["name"]) in text
        if not ok:
            bad += 1
        print("%-4s %-46s %s" % ("ok" if ok else "FAIL", c["name"],
                                 "" if ok else " | ".join(l.strip() for l in text.strip().splitlines()[-3:])[:200]))
    print("%d cases, %d failed" % (len(cs), bad))
    return bad


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("setup", "check"):
        sys.exit(__doc__)
    fps = make_keys()
    cs = cases()
    if sys.argv[1] == "check":
        sys.exit(1 if check(cs) else 0)
    write_list(fps, cs)
    p = start_servers(fps, cs)
    print("%d servers on ports %d-%d; case list written to %s" % (len(cs), FIRST_PORT, FIRST_PORT + len(cs) - 1,
                                                                   os.path.join(DEST, "matrix")))
    print("now, in RPCEmu, for each case N:  Obey %s.mxc.cNN   (then: tools/cryptomatrix.py check)" % GUEST)
    try:
        p.wait()
    except KeyboardInterrupt:
        p.send_signal(signal.SIGTERM)


main()
