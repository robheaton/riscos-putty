#!/usr/bin/env python3
"""Check the RISC OS puttygen (running in RPCEmu) against the host's puttygen and OpenSSH.

PuTTY's key code is compiled unchanged for 32-bit ARM with an old GCC, so what needs
checking is that it gives the same answers there as everywhere else: that the keys it
makes are good keys, that it reads every key format the host tools write, that what it
writes the host tools can read back, and that the RISC OS parts (file names, file types,
the files that hold passphrases, redirected input) behave.

    tools/keytest.py setup     make the input keys and write the scripts for the guest
                               (tools/keytest.py scripts rewrites only the scripts)
    (then in RPCEmu:  Obey HostFS::HostFS.$.PuttyDev.kg.run1, run2, run3  - run1 takes a while)
    tools/keytest.py check     compare what the guest left behind with the host's answers

Everything lives in PuttyDev/kg of the emulator's HostFS (the guest sees HostFS::HostFS.$.
PuttyDev.kg): in/ holds the inputs, out/ the results, log the output of every command.
Passphrases are made up for the test.  Names contain no dots because HostFS turns a dot
into a directory separator.
"""
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOSTBIN = os.path.join(ROOT, "build", "host")
PUTTYGEN = os.path.join(HOSTBIN, "puttygen")
HOSTFS = os.environ.get("RPCEMU_HOSTFS", os.path.expanduser("~/RPCEmu/machines/RISC OS 5/hostfs"))
KG = os.path.join(HOSTFS, "PuttyDev", "kg")
IN = os.path.join(KG, "in")
OUT = os.path.join(KG, "out")
REF = os.path.join(ROOT, "build", "test", "kgref")      # host-only: public halves and the like
GUEST = "HostFS::HostFS.$.PuttyDev.kg"

PW1 = "alpha passphrase one"
PW2 = "beta passphrase two"

# Keys to generate on the guest: name -> puttygen arguments.
GENS = {
    "ed25519": ["-t", "ed25519"],
    "ed448": ["-t", "ed448"],
    "p256": ["-t", "ecdsa", "-b", "256"],
    "p384": ["-t", "ecdsa", "-b", "384"],
    "p521": ["-t", "ecdsa", "-b", "521"],
    "rsa1024": ["-t", "rsa", "-b", "1024"],
    "rsa2048": ["-t", "rsa", "-b", "2048"],
    "rsa1024proven": ["-t", "rsa", "-b", "1024", "--primes", "proven"],
    "rsa1024strong": ["-t", "rsa", "-b", "1024", "--strong-rsa"],
    "dsa1024": ["-t", "dsa", "-b", "1024"],
    "rsa1v1": ["-t", "rsa1", "-b", "1024"],
}
# Types whose private key OpenSSH (10.x) can read, to confirm a generated key with software
# that shares no code with PuTTY.  (No DSA: it has been removed.  No SSH-1 either.)
OPENSSH_OK = {"ed25519", "p256", "p384", "p521", "rsa1024", "rsa2048", "rsa1024proven", "rsa1024strong"}


def host(*args, check=True, input=None):
    r = subprocess.run([PUTTYGEN, *args], capture_output=True, text=True, input=input)
    if check and r.returncode:
        raise RuntimeError("host puttygen %s: %s" % (" ".join(args), r.stderr.strip()))
    return r


def sshkeygen(*args):
    subprocess.run(["ssh-keygen", "-q", *args], check=True, capture_output=True)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def pwfile(name):
    return {"pw1": PW1, "pw2": PW2, "empty": ""}[name]


# ---------------------------------------------------------------------------------------
# The input keys.  name -> (how to make it, passphrase file name or None)
# ---------------------------------------------------------------------------------------
def make_inputs():
    for d in (IN, REF):
        os.makedirs(d, exist_ok=True)
    for name in ("pw1", "pw2", "empty"):
        write(os.path.join(IN, name), pwfile(name) + "\n")
    write(os.path.join(IN, "rand512"), os.urandom(512))
    write(os.path.join(IN, "rand16"), os.urandom(16))
    write(os.path.join(IN, "eof"), b"")
    # stdin for "puttygen KEY -P": the old passphrase, then the new one twice
    write(os.path.join(IN, "stdin3"), "%s\n%s\n%s\n" % (PW1, PW2, PW2))

    priv = {}              # name -> [passphrase file name or None, kind of key]

    def ossh(name, args, kind, pw=None):
        tmp = os.path.join(REF, "tmp_" + name)
        for p in (tmp, tmp + ".pub"):
            if os.path.exists(p):
                os.remove(p)
        sshkeygen(*args, "-N", pwfile(pw) if pw else "", "-C", "OpenSSH key " + name, "-f", tmp)
        shutil.copy(tmp, os.path.join(IN, name))
        os.remove(tmp)
        os.remove(tmp + ".pub")
        priv[name] = [pw, kind]

    ossh("osshEd", ["-t", "ed25519"], "ed25519")
    ossh("osshEdPw", ["-t", "ed25519"], "ed25519", "pw1")
    ossh("osshRsaPem", ["-t", "rsa", "-b", "2048", "-m", "PEM"], "rsa")
    ossh("osshRsaPemPw", ["-t", "rsa", "-b", "2048", "-m", "PEM"], "rsa", "pw1")
    ossh("osshRsaNew", ["-t", "rsa", "-b", "3072"], "rsa")
    ossh("osshP256", ["-t", "ecdsa", "-b", "256"], "ecdsa")
    ossh("osshP384PemPw", ["-t", "ecdsa", "-b", "384", "-m", "PEM"], "ecdsa", "pw1")
    ossh("osshP521", ["-t", "ecdsa", "-b", "521"], "ecdsa")

    def hostkey(name, gen, kind, pw=None, extra=()):
        out = os.path.join(IN, name)
        if os.path.exists(out):
            os.remove(out)
        host(*gen, "-C", "PuTTY key " + name, "-o", out,
             "--new-passphrase", os.path.join(IN, pw or "empty"), *extra)
        priv[name] = [pw, kind]

    hostkey("hostPpk3Ed", ["-t", "ed25519"], "ed25519")
    hostkey("hostPpk3Ed448", ["-t", "ed448"], "ed448")
    hostkey("hostPpk3RsaPw", ["-t", "rsa", "-b", "2048"], "rsa", "pw1")
    hostkey("hostPpk2Rsa", ["-t", "rsa", "-b", "1536"], "rsa", None, ["--ppk-param", "version=2"])
    hostkey("hostPpk2RsaPw", ["-t", "rsa", "-b", "1024"], "rsa", "pw1", ["--ppk-param", "version=2"])
    hostkey("hostPpk3Dsa", ["-t", "dsa", "-b", "1024"], "dsa")
    hostkey("hostPpk3P384Pw", ["-t", "ecdsa", "-b", "384"], "ecdsa", "pw1")
    hostkey("hostRsa1", ["-t", "rsa1", "-b", "1024"], "rsa1")
    hostkey("hostRsa1Pw", ["-t", "rsa1", "-b", "1024"], "rsa1", "pw1")

    # keys in the other foreign formats, written by the host's puttygen (ssh.com's format
    # holds only RSA and DSA keys)
    def export(name, src, fmt, pw=None):
        out = os.path.join(IN, name)
        if os.path.exists(out):
            os.remove(out)
        host(os.path.join(IN, src), "--old-passphrase", os.path.join(IN, priv[src][0] or "empty"),
             "-O", fmt, "-o", out, "--new-passphrase", os.path.join(IN, pw or "empty"))
        priv[name] = [pw, priv[src][1]]

    export("hostSshcomRsa", "hostPpk2Rsa", "private-sshcom")
    export("hostSshcomDsaPw", "hostPpk3Dsa", "private-sshcom", "pw1")
    export("hostOsshDsa", "hostPpk3Dsa", "private-openssh")
    export("hostOsshDsaPw", "hostPpk3Dsa", "private-openssh", "pw1")
    export("hostOsshNewEd448", "hostPpk3Ed448", "private-openssh-new")

    # public keys on their own
    pubs = {}
    src = os.path.join(IN, "hostPpk3Ed")
    for name, kind in (("pubRfc", "public"), ("pubOssh", "public-openssh")):
        out = os.path.join(IN, name)
        host(src, "-O", kind, "-o", out)
        pubs[name] = kind
    out = os.path.join(IN, "pubRsa1")
    host(os.path.join(IN, "hostRsa1"), "-O", "public-openssh", "-o", out)
    pubs["pubRsa1"] = "public-openssh"
    return priv, pubs


# ---------------------------------------------------------------------------------------
# The guest scripts
# ---------------------------------------------------------------------------------------
class Script:
    def __init__(self, name):
        self.name = name
        self.lines = [
            "| generated by tools/keytest.py - do not edit",
            "Set Sys$RCLimit 255",
            "Dir %s" % GUEST,
            "Set Run$Path HostFS::HostFS.$.PuttyDev.,<Run$Path>",
        ]
        self.names = []

    def run(self, case, args, stdin=True):
        """One puttygen command; its output and return code go to the log.  Unless it has
        input of its own, it reads standard input from an empty file, so that a passphrase
        prompt nobody expected fails (and is logged) instead of waiting for a key press."""
        if stdin and "<" not in args:
            args += " < in.eof"
        self.names.append(case)
        log = "{ >> %s.log }" % GUEST          # not "log": some cases change the current directory
        self.lines += [
            "Echo === %s %s" % (case, log),
            "Echo <Sys$Time> %s" % log,
            "puttygen %s %s" % (args, log),
            "Echo rc=<Sys$ReturnCode> %s" % log,
            "Echo <Sys$Time> %s" % log,
        ]

    def raw(self, line):
        self.lines.append(line)

    def save(self):
        self.lines.append("Echo script %s finished { >> %s.log }" % (self.name, GUEST))
        write(os.path.join(KG, self.name + ",feb"), "\n".join(self.lines) + "\n")


def build_scripts(priv, pubs):
    s1, s2, s3 = Script("run1"), Script("run2"), Script("run3")

    # --- run1: generate keys, with and without a passphrase ---------------------------
    for name, gen in GENS.items():
        a = " ".join(gen)
        s1.run("gen-%s-n" % name, "%s -o out.g_%s_n --new-passphrase in.empty" % (a, name))
        s1.run("gen-%s-p" % name, "%s -o out.g_%s_p --new-passphrase in.pw1" % (a, name))
        for v in ("n", "p"):
            pw = "" if v == "n" else "--old-passphrase in.pw1"
            s1.run("gen-%s-%s-fp" % (name, v), "out.g_%s_%s -O fingerprint %s -o out.g_%s_%s_fp" % (name, v, pw, name, v))
            s1.run("gen-%s-%s-txt" % (name, v), "out.g_%s_%s -O text %s -o out.g_%s_%s_txt" % (name, v, pw, name, v))

    # --- run2: read every kind of key, write every kind of key -------------------------
    for name, (pw, kind) in priv.items():
        old = "--old-passphrase in.%s" % pw if pw else ""
        s2.run("in-%s-fp" % name, "in.%s %s -O fingerprint -o out.%s_fp" % (name, old, name))
        s2.run("in-%s-txt" % name, "in.%s %s -O text -o out.%s_txt" % (name, old, name))
        # (a key already in PuTTY's own format needs -P, or puttygen says it would do nothing)
        same = "-P" if name.startswith(("hostPpk", "hostRsa1")) else ""
        s2.run("in-%s-ppk" % name, "in.%s %s %s -o out.%s_ppk --new-passphrase in.pw2" % (name, old, same, name))
        if kind == "rsa1":
            s2.run("in-%s-pub" % name, "in.%s %s -O public-openssh -o out.%s_pub" % (name, old, name))
            continue
        s2.run("in-%s-pub" % name, "out.%s_ppk --old-passphrase in.pw2 -O public-openssh -o out.%s_pub" % (name, name))
        s2.run("in-%s-pubrfc" % name, "out.%s_ppk --old-passphrase in.pw2 -O public -o out.%s_pubrfc" % (name, name))
        s2.run("in-%s-osshold" % name, "out.%s_ppk --old-passphrase in.pw2 -O private-openssh -o out.%s_osshold --new-passphrase in.pw1" % (name, name))
        s2.run("in-%s-osshnew" % name, "out.%s_ppk --old-passphrase in.pw2 -O private-openssh-new -o out.%s_osshnew --new-passphrase in.empty" % (name, name))
        if kind in ("rsa", "dsa"):
            s2.run("in-%s-sshcom" % name, "out.%s_ppk --old-passphrase in.pw2 -O private-sshcom -o out.%s_sshcom --new-passphrase in.pw1" % (name, name))
    for name, kind in pubs.items():
        s2.run("pub-%s-fp" % name, "in.%s -O fingerprint -o out.%s_fp" % (name, name))
        s2.run("pub-%s-ossh" % name, "in.%s -O public-openssh -o out.%s_pub" % (name, name))
        s2.run("pub-%s-rfc" % name, "in.%s -O public -o out.%s_pubrfc" % (name, name))
        s2.run("pub-%s-txt" % name, "in.%s -O text -o out.%s_txt" % (name, name))

    # --- run3: editing keys in place, PPK parameters, stdin, errors ----------------------
    for n, src in (("ip1", "ed25519_p"), ("ip2", "ed25519_n"), ("ip3", "p256_p"), ("ip5", "rsa1024_n"),
                   ("ip6", "ed25519_p"), ("ip7", "rsa1v1_p"), ("ip8", "dsa1024_n")):
        s3.raw("X Delete out.%s" % n)
        s3.raw("Copy out.g_%s out.%s ~C~V" % (src, n))
    s3.run("op-change-passphrase", "out.ip1 -P --old-passphrase in.pw1 --new-passphrase in.pw2")
    s3.run("op-change-comment", "out.ip2 -C \"a new comment, with spaces\"")
    s3.run("op-reencrypt", "out.ip3 --reencrypt --old-passphrase in.pw1")
    s3.run("op-ppk-v2", "out.g_rsa1024_p --old-passphrase in.pw1 -P -o out.ip4v2 --new-passphrase in.pw2 --ppk-param version=2")
    s3.run("op-ppk-argon2i", "out.ip5 -P --new-passphrase in.pw1 --ppk-param kdf=argon2i,memory=1024,passes=4")
    s3.run("op-ppk-argon2d", "out.g_p256_n -P -o out.ip9 --new-passphrase in.pw1 --ppk-param kdf=argon2d,memory=2048,parallelism=2,time=50")
    s3.run("op-stdin", "out.ip6 -P < in.stdin3")
    s3.run("op-ssh1-passphrase", "out.ip7 -P --old-passphrase in.pw1 --new-passphrase in.pw2")
    s3.run("op-ssh1-comment", "out.ip7 -C ssh1comment --old-passphrase in.pw2 --new-passphrase in.pw2")
    s3.run("op-dsa-comment-to-openssh", "out.ip8 -C dsacomment -O private-openssh -o out.ip8_ossh --new-passphrase in.empty")
    s3.run("op-dump-stdin-key", "- -O fingerprint < out.g_ed25519_n")
    s3.run("op-short-l", "out.g_ed25519_n -l -o out.sf_fp")
    s3.run("op-short-L", "out.g_ed25519_n -L -o out.sf_ossh")
    s3.run("op-short-p", "out.g_ed25519_n -p -o out.sf_rfc")
    s3.run("op-fp-md5", "out.g_rsa1024_n -l -E md5 -o out.fp_md5")
    s3.run("op-quiet", "-q -t rsa -b 768 -o out.q1 --new-passphrase in.empty", stdin=False)
    s3.run("op-progress", "-t rsa -b 768 -o out.q2 --new-passphrase in.empty", stdin=False)
    s3.raw("X Delete out.ip10")
    s3.raw("Copy out.g_ed25519_n out.ip10 ~C~V")
    s3.run("op-inplace-at-sign", "@.out.ip10 -C \"via at sign\"")
    s3.raw("X Delete out.ip11")
    s3.raw("Copy out.g_ed25519_n out.ip11 ~C~V")
    s3.raw("Dir out")
    s3.run("op-inplace-bare-name", "ip11 -C \"bare name\" < %s.in.eof" % GUEST)
    s3.raw("Dir %s" % GUEST)
    s3.run("op-random-device", "-t ed25519 -o out.rd_ok --new-passphrase in.empty --random-device in.rand512")
    s3.run("err-random-device-short", "-t ed25519 -o out.rd_short --new-passphrase in.empty --random-device in.rand16")
    s3.run("err-random-device-missing", "-t ed25519 -o out.rd_missing --new-passphrase in.empty --random-device in.nosuchfile")
    s3.run("err-wrong-passphrase", "out.g_ed25519_p --old-passphrase in.pw2 -O text")
    s3.run("err-no-such-file", "out.nosuchfile -l")
    s3.run("err-bad-type", "-t foo -o out.x")
    s3.run("err-bad-ecdsa-bits", "-t ecdsa -b 300 -o out.x --new-passphrase in.empty")
    s3.run("err-short-rsa", "-t rsa -b 100 -o out.x --new-passphrase in.empty")
    s3.run("err-no-action", "out.g_ed25519_n")
    s3.run("err-load-and-generate", "out.g_ed25519_n -t rsa")
    s3.run("err-not-a-key", "in.pw1 -l")
    s3.run("err-bad-ppk-param", "out.g_ed25519_n -P --new-passphrase in.pw1 --ppk-param nonsense=1 -o out.x")
    s3.run("err-no-output-file", "-t ed25519 --new-passphrase in.empty")
    s3.run("help", "--help")
    s3.run("version", "--version")
    s3.run("pgpfp", "--pgpfp")
    for s in (s1, s2, s3):
        s.save()


# ---------------------------------------------------------------------------------------
# Checking
# ---------------------------------------------------------------------------------------
results = []


def report(name, ok, why=""):
    results.append((name, ok, why))
    print("  %-4s %s%s" % ("ok" if ok else "FAIL", name, "" if ok else ": " + why))


def readout(name):
    """A result file the guest wrote, whatever filetype suffix HostFS gave it; None if missing."""
    base = os.path.join(OUT, name)
    for cand in (base, base + ",fff", base + ",ffd"):
        if os.path.exists(cand):
            with open(cand, "rb") as f:
                return f.read().decode("latin-1")
    return None


def outpath(name):
    base = os.path.join(OUT, name)
    for cand in (base, base + ",fff", base + ",ffd"):
        if os.path.exists(cand):
            return cand
    return None


def parse_log():
    """case -> (output text, rc, start time, end time)"""
    cases = {}
    path = os.path.join(KG, "log")
    if not os.path.exists(path):
        path = path + ",fff"
    if not os.path.exists(path):
        return cases, False
    text = open(path, "rb").read().decode("latin-1").replace("\r", "")
    finished = 0
    for chunk in re.split(r"^=== ", text, flags=re.M)[1:]:
        lines = chunk.split("\n")
        name = lines[0].strip()
        body = lines[1:]
        times = [l for l in body if re.fullmatch(r"\d\d:\d\d:\d\d", l.strip())]
        rcs = re.findall(r"rc=(\d+) *\n", chunk)
        out = "\n".join(l for l in body if l not in times and not l.startswith("script "))
        out = re.sub(r"rc=\d+ *$", "", out.rstrip()).rstrip()
        cases[name] = (out, int(rcs[-1]) if rcs else None,
                       times[0].strip() if times else None, times[-1].strip() if times else None)
        finished += len([l for l in body if l.startswith("script ")])
    return cases, True


def secs(a, b):
    def tosec(t):
        h, m, s = map(int, t.split(":"))
        return h * 3600 + m * 60 + s
    d = tosec(b) - tosec(a)
    return d + 86400 if d < 0 else d


def pg_text(path, pw=None):
    args = [path, "-O", "text"]
    if pw:
        args += ["--old-passphrase", os.path.join(IN, pw)]
    return host(*args).stdout


def pg_fp(path, pw=None):
    args = [path, "-O", "fingerprint"]
    if pw:
        args += ["--old-passphrase", os.path.join(IN, pw)]
    return host(*args).stdout.strip()


def ssh_pub(path, pw=None):
    """The public key (type and blob) OpenSSH derives from a private key file, or None."""
    work = os.path.join(REF, "tmp_check")
    shutil.copy(path, work)
    os.chmod(work, 0o600)
    r = subprocess.run(["ssh-keygen", "-y", "-P", pwfile(pw) if pw else "", "-f", work],
                       capture_output=True, text=True)
    os.remove(work)
    return " ".join(r.stdout.split()[:2]) if r.returncode == 0 else None


def key_pub(path, pw=None):
    args = [path, "-O", "public-openssh"]
    if pw:
        args += ["--old-passphrase", os.path.join(IN, pw)]
    return " ".join(host(*args).stdout.split()[:2])


def check_generated(cases):
    print("generated keys")
    for name in GENS:
        for v, pw in (("n", None), ("p", "pw1")):
            key = "g_%s_%s" % (name, v)
            tag = "gen %s %s" % (name, "with passphrase" if pw else "no passphrase")
            path = outpath(key)
            if not path:
                report(tag, False, "the guest made no key file")
                continue
            try:
                head = open(path, "rb").read(80).decode("latin-1")
                if name == "rsa1v1":
                    ok_head = head.startswith("SSH PRIVATE KEY FILE FORMAT 1.1")
                else:
                    ok_head = head.startswith("PuTTY-User-Key-File-3: ")
                enc = ("Encryption: aes256-cbc" in open(path, "rb").read(200).decode("latin-1")) if name != "rsa1v1" else True
                if not ok_head:
                    report(tag, False, "unexpected file header %r" % head[:40])
                    continue
                if name != "rsa1v1" and enc != bool(pw):
                    report(tag, False, "encryption header does not match the passphrase")
                    continue
                gfp, gtxt = readout(key + "_fp"), readout(key + "_txt")
                if gfp is None or gtxt is None:
                    report(tag, False, "the guest could not read back its own key")
                    continue
                if gfp.strip() != pg_fp(path, pw):
                    report(tag, False, "fingerprint differs: guest %r host %r" % (gfp.strip(), pg_fp(path, pw)))
                    continue
                if gtxt != pg_text(path, pw):
                    report(tag, False, "component dump differs")
                    continue
                if name in OPENSSH_OK:
                    tmp = os.path.join(REF, "tmp_export")
                    host(path, "-O", "private-openssh", "-o", tmp, "--new-passphrase", os.path.join(IN, "empty"),
                         *(["--old-passphrase", os.path.join(IN, pw)] if pw else []))
                    a, b = ssh_pub(tmp), key_pub(path, pw)
                    os.remove(tmp)
                    if a != b:
                        report(tag, False, "OpenSSH derives a different public key: %r vs %r" % (a, b))
                        continue
                report(tag, True)
            except Exception as e:                  # a host tool failing to read a guest key is the finding
                report(tag, False, str(e))
        log = cases.get("gen-%s-n" % name)
        if log and log[2] and log[3]:
            print("       (%s took %d s to generate and save without a passphrase)" % (name, secs(log[2], log[3])))
    # two keys of one type must differ
    for name in GENS:
        a, b = outpath("g_%s_n" % name), outpath("g_%s_p" % name)
        if a and b and name != "rsa1v1":
            report("gen %s: two keys differ" % name, pg_fp(a) != pg_fp(b, "pw1"), "same fingerprint twice")


def check_inputs(priv, pubs):
    print("reading and converting keys")
    for name, (pw, kind) in priv.items():
        src = os.path.join(IN, name)
        tag = "in %s" % name
        try:
            ref_fp, ref_txt = pg_fp(src, pw), pg_text(src, pw)
        except RuntimeError as e:
            report(tag, False, "the host cannot read its own input: %s" % e)
            continue
        problems = []
        fp, txt = readout(name + "_fp"), readout(name + "_txt")
        if fp is None or fp.strip() != ref_fp:
            problems.append("fingerprint (guest %r, host %r)" % (fp and fp.strip(), ref_fp))
        if txt != ref_txt:
            problems.append("component dump")
        ppk = outpath(name + "_ppk")
        if not ppk:
            problems.append("no converted key")
        else:
            try:
                if pg_text(ppk, "pw2") != ref_txt:
                    problems.append("converted key differs")
                if host(ppk, "-O", "text", "--old-passphrase", os.path.join(IN, "pw1"), check=False).returncode == 0:
                    problems.append("converted key opens with the wrong passphrase")
            except RuntimeError as e:
                problems.append("host cannot read the converted key: %s" % e)
        if kind != "rsa1":
            pub = readout(name + "_pub")
            try:
                if pub is None or pub.strip() != host(src, "-O", "public-openssh",
                       *(["--old-passphrase", os.path.join(IN, pw)] if pw else [])).stdout.strip():
                    problems.append("public key (OpenSSH form)")
                rfc = readout(name + "_pubrfc")
                want = host(src, "-O", "public",
                            *(["--old-passphrase", os.path.join(IN, pw)] if pw else [])).stdout
                if rfc is None or rfc.replace("\r", "") != want:
                    problems.append("public key (RFC 4716 form)")
                exports = [("old", "_osshold", "pw1"), ("new", "_osshnew", None)]
                if kind in ("rsa", "dsa"):
                    exports.append(("sshcom", "_sshcom", "pw1"))
                for fmt, suffix, kpw in exports:
                    p = outpath(name + suffix)
                    if not p:
                        problems.append("no %s export" % fmt)
                        continue
                    if pg_text(p, kpw) != ref_txt:
                        problems.append("%s export reads back differently" % fmt)
                    if fmt != "sshcom" and kind in ("rsa", "ecdsa", "ed25519"):   # OpenSSH cannot read the others
                        a = ssh_pub(p, kpw)
                        if a != key_pub(src, pw):
                            problems.append("OpenSSH disagrees about the %s export (%r)" % (fmt, a))
            except RuntimeError as e:
                problems.append(str(e))
        else:
            pub = readout(name + "_pub")
            want = host(src, "-O", "public-openssh",
                        *(["--old-passphrase", os.path.join(IN, pw)] if pw else [])).stdout
            if pub is None or pub.strip() != want.strip():
                problems.append("SSH-1 public key")
        report(tag, not problems, "; ".join(problems))
    for name, kind in pubs.items():
        src = os.path.join(IN, name)
        tag = "in %s" % name
        problems = []
        fp = readout(name + "_fp")
        if fp is None or fp.strip() != pg_fp(src):
            problems.append("fingerprint")
        txt = readout(name + "_txt")
        if txt != pg_text(src):
            problems.append("component dump")
        if not name == "pubRsa1":
            pub, rfc = readout(name + "_pub"), readout(name + "_pubrfc")
            if pub is None or pub.strip() != host(src, "-O", "public-openssh").stdout.strip():
                problems.append("OpenSSH form")
            if rfc is None or rfc.replace("\r", "") != host(src, "-O", "public").stdout:
                problems.append("RFC 4716 form")
        report(tag, not problems, "; ".join(problems))


def check_ops(cases):
    print("editing keys, parameters, redirected input, errors")

    def rc_is(case, want):
        got = cases.get(case)
        if got is None:
            report(case, False, "the guest did not run it")
            return None
        if (got[1] == 0) != (want == 0):
            report(case, False, "return code %r, wanted %s; output: %s" % (got[1], "0" if want == 0 else "nonzero", got[0].strip()[:200]))
            return None
        return got

    def says(case, want, text):
        got = rc_is(case, want)
        if got is None:
            return
        report(case, text in got[0], "output lacks %r: %s" % (text, got[0].strip()[:200]))

    def leftovers(case):
        stray = [f for f in os.listdir(OUT) if f.startswith("PgTmp")]
        report(case + ": no scratch file left", not stray, "left behind: %s" % stray)

    # in place: the file keeps its name, the new passphrase works, the old one does not
    if rc_is("op-change-passphrase", 0):
        p = outpath("ip1")
        before = pg_fp(outpath("g_ed25519_p"), "pw1")
        ok = p and pg_fp(p, "pw2") == before
        report("op-change-passphrase: opens with the new passphrase, same key", ok, "no")
        report("op-change-passphrase: the old passphrase no longer works",
               host(p, "-O", "text", "--old-passphrase", os.path.join(IN, "pw1"), check=False).returncode != 0, "still opens")
        leftovers("op-change-passphrase")
    if rc_is("op-change-comment", 0):
        p = outpath("ip2")
        txt = open(p, "rb").read().decode("latin-1")
        report("op-change-comment: new comment in the file", "Comment: a new comment, with spaces" in txt, txt[:120])
        report("op-change-comment: same key", pg_fp(p) == pg_fp(outpath("g_ed25519_n")), "fingerprint changed")
        leftovers("op-change-comment")
    if rc_is("op-reencrypt", 0):
        p, orig = outpath("ip3"), outpath("g_p256_p")
        a, b = open(p, "rb").read(), open(orig, "rb").read()
        report("op-reencrypt: still opens with the same passphrase, same key",
               pg_fp(p, "pw1") == pg_fp(orig, "pw1"), "no")
        report("op-reencrypt: the file was rewritten (fresh salt)", a != b, "identical bytes")
        leftovers("op-reencrypt")
    if rc_is("op-ppk-v2", 0):
        p = outpath("ip4v2")
        txt = open(p, "rb").read().decode("latin-1")
        report("op-ppk-v2: format 2 file", txt.startswith("PuTTY-User-Key-File-2: "), txt[:40])
        report("op-ppk-v2: same key", pg_fp(p, "pw2") == pg_fp(outpath("g_rsa1024_p"), "pw1"), "fingerprint changed")
    if rc_is("op-ppk-argon2i", 0):
        txt = open(outpath("ip5"), "rb").read().decode("latin-1")
        report("op-ppk-argon2i: parameters in the file",
               all(s in txt for s in ("Key-Derivation: Argon2i", "Argon2-Memory: 1024", "Argon2-Passes: 4")), txt[:300])
        report("op-ppk-argon2i: opens", pg_fp(outpath("ip5"), "pw1") == pg_fp(outpath("g_rsa1024_n")), "no")
    if rc_is("op-ppk-argon2d", 0):
        txt = open(outpath("ip9"), "rb").read().decode("latin-1")
        report("op-ppk-argon2d: parameters in the file",
               all(s in txt for s in ("Key-Derivation: Argon2d", "Argon2-Memory: 2048", "Argon2-Parallelism: 2")), txt[:300])
        report("op-ppk-argon2d: opens", pg_fp(outpath("ip9"), "pw1") == pg_fp(outpath("g_p256_n")), "no")
    if rc_is("op-stdin", 0):
        p = outpath("ip6")
        report("op-stdin: passphrases read from redirected input", pg_fp(p, "pw2") == pg_fp(outpath("g_ed25519_p"), "pw1"), "no")
    if rc_is("op-ssh1-passphrase", 0):
        p = outpath("ip7")
        report("op-ssh1-passphrase: SSH-1 key opens with the new passphrase",
               host(p, "-O", "fingerprint", "--old-passphrase", os.path.join(IN, "pw2"), check=False).returncode == 0, "no")
    if rc_is("op-ssh1-comment", 0):
        txt = host(outpath("ip7"), "-O", "public-openssh").stdout
        report("op-ssh1-comment: comment changed", txt.strip().endswith("ssh1comment"), txt.strip()[-40:])
    if rc_is("op-dsa-comment-to-openssh", 0):
        p = outpath("ip8_ossh")
        report("op-dsa-comment-to-openssh: DSA exported to OpenSSH reads back as the same key",
               p and pg_fp(p) == pg_fp(outpath("g_dsa1024_n")), "no")
    got = rc_is("op-dump-stdin-key", 0)
    if got:
        report("op-dump-stdin-key: key read from standard input",
               got[0].strip().endswith(pg_fp(outpath("g_ed25519_n")).split()[-1]), got[0].strip()[:100])
    for case, flag, out in (("op-short-l", "-l", "sf_fp"), ("op-short-L", "-L", "sf_ossh"), ("op-short-p", "-p", "sf_rfc")):
        if rc_is(case, 0):
            got = (readout(out) or "").replace("\r", "")
            report("%s: same as the host's" % case,
                   got == host(outpath("g_ed25519_n"), flag).stdout, repr(got[:80]))
    if rc_is("op-fp-md5", 0):
        got = (readout("fp_md5") or "").strip()
        report("op-fp-md5: MD5 fingerprint as the host's",
               got == host(outpath("g_rsa1024_n"), "-l", "-E", "md5").stdout.strip(), repr(got))
    got = rc_is("op-quiet", 0)
    if got:
        report("op-quiet: no progress output", "+" not in got[0], repr(got[0][:60]))
        report("op-quiet: the key loads", bool(outpath("q1")) and bool(pg_fp(outpath("q1"))), "no")
    got = rc_is("op-progress", 0)
    if got:
        report("op-progress: a progress bar without -q", "+" in got[0], repr(got[0][:60]))
    for case, out, comment in (("op-inplace-at-sign", "ip10", "via at sign"), ("op-inplace-bare-name", "ip11", "bare name")):
        if rc_is(case, 0):
            txt = open(outpath(out), "rb").read().decode("latin-1")
            report("%s: comment changed in the right file" % case, ("Comment: " + comment) in txt, txt[:100])
            leftovers(case)
    if rc_is("op-random-device", 0):
        report("op-random-device: key made from a supplied file loads", bool(outpath("rd_ok")) and bool(pg_fp(outpath("rd_ok"))), "no")
    says("err-random-device-short", 1, "too short")
    says("err-random-device-missing", 1, "in.nosuchfile")
    says("err-wrong-passphrase", 1, "wrong passphrase")
    says("err-no-such-file", 1, "unable to load file")
    says("err-bad-type", 1, "unknown key type")
    says("err-bad-ecdsa-bits", 1, "invalid bits for ECDSA")
    says("err-short-rsa", 1, "shorter than 256 bits")
    says("err-no-action", 1, "no useful action")
    says("err-load-and-generate", 1, "cannot both load and generate")
    says("err-not-a-key", 1, "unable to load file")
    says("err-bad-ppk-param", 1, "unrecognised PPK parameter")
    says("err-no-output-file", 1, "need to specify an output file")
    says("help", 0, "key generator and converter")
    says("version", 0, "Release 0.85")
    says("pgpfp", 0, "PuTTY Master Key")
    for stray in (outpath("x"), outpath("rd_short"), outpath("rd_missing")):
        if stray:
            report("failed command left no output file %s" % os.path.basename(stray), False, "exists")


def check():
    priv, pubs = load_manifest()
    cases, have_log = parse_log()
    if not have_log:
        sys.exit("no log: run the guest scripts first (see the top of this file)")
    check_generated(cases)
    check_inputs(priv, pubs)
    check_ops(cases)
    bad = [r for r in results if not r[1]]
    print("\n%d checks, %d failed" % (len(results), len(bad)))
    sys.exit(1 if bad else 0)


def load_manifest():
    """The input names (and which need a passphrase) that setup made."""
    with open(os.path.join(REF, "manifest.json")) as f:
        m = json.load(f)
    return m["priv"], m["pubs"]


def setup():
    if not os.path.exists(PUTTYGEN):
        sys.exit("build the host tools first (see tools/testserver.sh)")
    shutil.rmtree(OUT, ignore_errors=True)
    for f in ("log", "log,fff"):
        p = os.path.join(KG, f)
        if os.path.exists(p):
            os.remove(p)
    os.makedirs(OUT)
    write(os.path.join(KG, "log"), "")          # the scripts append to it, and ">>" needs the file to exist
    priv, pubs = make_inputs()
    with open(os.path.join(REF, "manifest.json"), "w") as f:
        json.dump({"priv": priv, "pubs": pubs}, f)
    build_scripts(priv, pubs)
    print("inputs in %s, scripts run1 run2 run3 written" % KG)
    print("in RPCEmu:  Obey %s.run1   (then run2, run3);  then  tools/keytest.py check" % GUEST)


def scripts():
    """Write the guest scripts again (after editing them) without making new keys."""
    priv, pubs = load_manifest()
    build_scripts(priv, pubs)
    print("scripts rewritten; the inputs and results are as they were")


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("setup", "check", "scripts"):
        sys.exit(__doc__)
    {"setup": setup, "check": check, "scripts": scripts}[sys.argv[1]]()
