#!/usr/bin/env python3
"""Check the key agent in the emulator: add a key, log in through it, forward it, take it out again.

    RPCEMU_VNC_PASSWORD=... ~/.rpcemu-mcp/bin/python tools/agenttest.py [--encrypted]

Needs the test server (tools/testserver.sh), tools/deploy.sh done, and !PuTTY just started in RPCEmu
(WimpTask Run HostFS::HostFS.$.PuttyDev.!PuTTY.!RunImage) with no key in its agent and no start-up list: the
Connect box is open on a clean desktop.  It drives the screen over VNC (the positions are those of a 1600x1200
desktop) and checks what the server saw:

  - the key is added with Add key..., by its pathname (with --encrypted, the passphrase-protected copy of the
    test key, EncKey, whose passphrase is `agentpass`: make it with the host's puttygen, see docs/DESIGN.md);
  - a session with no key file logs in by the agent, and agent forwarding works: `ssh-add -l` on the server
    shows the key (its fingerprint must be puttygen's), and `ssh-keygen -Y sign` through the forwarded agent
    makes a signature that the host's ssh-keygen accepts;
  - after Remove all, a new session cannot use the agent.

The saved session `agentfwd` (the test server, no key file, agent forwarding on) is written for it.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vncdrive  # noqa: E402  (reads the VNC password from the environment)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOSTFS = os.environ.get("RPCEMU_HOSTFS", os.path.expanduser("~/RPCEmu/machines/RISC OS 5/hostfs"))
HOME = os.path.join(ROOT, "build", "test", "home")      # the test server's home directory
TEST = os.path.join(ROOT, "build", "test")
PUTTYGEN = os.path.join(ROOT, "build", "host", "puttygen")

bad = 0


def check(ok, what):
    global bad
    print("%-4s %s" % ("ok" if ok else "FAIL", what))
    bad += not ok


def make_session():
    sdir = os.path.join(HOSTFS, "!Boot", "Choices", "PuTTY", "Sessions")
    lines = []
    for l in open(os.path.join(sdir, "testserver")).read().splitlines():
        if l.startswith("PublicKeyFile="):
            l = "PublicKeyFile="
        elif l.startswith("AgentFwd="):
            l = "AgentFwd=1"
        lines.append(l)
    open(os.path.join(sdir, "agentfwd"), "w").write("\n".join(lines) + "\n")


def read(name):
    try:
        return open(os.path.join(HOME, name), "rb").read().decode("utf-8", "replace")
    except OSError:
        return ""


def main(argv):
    encrypted = "--encrypted" in argv
    d = vncdrive.Drive()
    run = d.run
    make_session()
    for f in ("agent.out", "sign.out", "m.txt", "m.txt.sig", "agent.done", "agent2.out", "agent2.done"):
        try:
            os.remove(os.path.join(HOME, f))
        except OSError:
            pass
    fp = subprocess.run([PUTTYGEN, "-l", os.path.join(TEST, "client.ppk")], capture_output=True,
                        text=True).stdout.split()[2]

    # the Connect box is open: close it, and open the Keys window from the icon bar menu
    for line in ("click 921 689", "wait 1", "click 1430 1167 menu", "wait 1.5", "click 1436 1097", "wait 2"):
        run(line)
    # Add key...: type the pathname
    key = "HostFS::HostFS.$.PuttyDev." + ("EncKey" if encrypted else "ClientKey")
    for line in ("click 588 688", "wait 2", "type " + key, "key Return", "wait 4"):
        run(line)
    if encrypted:
        for line in ("type agentpass", "key Return"):
            run(line)
        time.sleep(30)                          # Argon2 on the emulated machine
    run("wait 2")
    run("click 1018 688")                        # Close
    run("wait 1")

    # a session by the saved session `agentfwd`: no key file, agent forwarding on
    for line in ("click 1430 1167", "wait 2", "click 800 601", "type agentfwd", "click 688 636", "wait 1",
                 "click 1009 689", "wait 8", "click 900 600", "wait 0.5"):
        run(line)
    cmd = ("ssh-add -l > agent.out 2>&1; echo hello > m.txt; ssh-keygen -Y sign -n file -f "
           + os.path.join(TEST, "client.pub") + " m.txt > sign.out 2>&1; echo done > agent.done")
    run("type " + cmd)
    run("key Return")
    for _ in range(40):
        if os.path.exists(os.path.join(HOME, "agent.done")):
            break
        time.sleep(1)
    time.sleep(1)
    out = read("agent.out")
    check(fp in out, "ssh-add -l on the server lists the key through the forwarded agent (%s)" % out.strip()[:80])
    sig = os.path.join(HOME, "m.txt.sig")
    ok = os.path.exists(sig) and subprocess.run(
        ["ssh-keygen", "-Y", "check-novalidate", "-n", "file", "-f", os.path.join(TEST, "client.pub"), "-s", sig],
        stdin=open(os.path.join(HOME, "m.txt"), "rb"), capture_output=True).returncode == 0
    check(ok, "a signature made through the forwarded agent verifies on the host")

    # take the key out (tick it, Remove): a new session cannot use the agent, so it has to ask for a password
    for line in ("click 1430 1167 menu", "wait 1.5", "click 1436 1097", "wait 2", "click 547 425", "wait 1",
                 "click 688 688", "wait 2", "click 1018 688", "wait 1"):
        run(line)
    for line in ("click 1430 1167", "wait 2", "click 800 601", "type agentfwd", "click 688 636", "wait 1",
                 "click 1009 689", "wait 8", "click 900 600", "wait 0.5", "type stoat", "key Return", "wait 3",
                 "type weasel", "key Return", "wait 5"):
        run(line)
    run("type ssh-add -l > agent2.out 2>&1; echo done > agent2.done")
    run("key Return")
    for _ in range(30):
        if os.path.exists(os.path.join(HOME, "agent2.done")):
            break
        time.sleep(1)
    time.sleep(1)
    out = read("agent2.out")
    check(os.path.exists(os.path.join(HOME, "agent2.done")),
          "the second session logged in by keyboard-interactive (the test server's answers stoat and weasel)")
    check(fp not in out, "after Remove the agent has nothing to offer (%s)" % out.strip()[:80])
    print("%d problem%s" % (bad, "" if bad == 1 else "s"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
