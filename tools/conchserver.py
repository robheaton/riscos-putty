#!/usr/bin/env python3
"""A second, independent SSH server (Twisted Conch) for testing the RISC OS PuTTY against.

Uppity is built from PuTTY's own code, so it can only show that the port agrees with
PuTTY.  This server is somebody else's implementation of SSH (and, unlike Uppity, it does
port forwarding), so it shows that the port can talk to other software, and exercises
local (-L) and remote (-R) forwarding.

    tools/conchserver.py [PORT]      default 3200; user "tester", password "weasel"

It listens on the loopback only (the guest sees it as 10.10.10.2), runs commands with a
clean environment in a scratch directory, and is for testing only.  Needs python3-twisted
with its conch and cryptography dependencies.
"""
import os
import struct
import sys

from twisted.conch import avatar
from twisted.conch.ssh import factory, forwarding, keys, session, userauth, connection, transport
from twisted.cred import checkers, portal
from twisted.internet import reactor
from twisted.python import components, log

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(os.path.dirname(HERE), "build", "test", "conch")


class TestAvatar(avatar.ConchUser):
    def __init__(self, username):
        avatar.ConchUser.__init__(self)
        self.username = username
        self.channelLookup[b"session"] = session.SSHSession
        self.channelLookup[b"direct-tcpip"] = forwarding.openConnectForwardingClient
        self.listeners = {}

    # remote forwarding: listen where the client asks, and open a channel back for each connection
    def global_tcpip_forward(self, data):
        host, port = forwarding.unpackGlobal_tcpip_forward(data)
        factory = forwarding.SSHListenForwardingFactory(
            self.conn, (host, port), forwarding.SSHListenServerForwardingChannel)
        listener = reactor.listenTCP(port, factory, interface="127.0.0.1")
        self.listeners[(host, port)] = listener
        log.msg("remote forward: listening on %s:%d" % (host, listener.getHost().port))
        if port == 0:
            return 1, struct.pack(">L", listener.getHost().port)
        return 1

    def global_cancel_tcpip_forward(self, data):
        host, port = forwarding.unpackGlobal_tcpip_forward(data)
        listener = self.listeners.pop((host, port), None)
        if listener:
            listener.stopListening()
            return 1
        return 0

    def logout(self):
        for listener in self.listeners.values():
            listener.stopListening()


class TestSession:
    def __init__(self, avatar):
        self.avatar = avatar

    def _spawn(self, proto, argv):
        os.makedirs(WORK, exist_ok=True)
        env = {"PATH": "/usr/bin:/bin", "HOME": WORK, "SHELL": "/bin/sh", "PS1": "$ "}
        reactor.spawnProcess(proto, "/bin/sh", argv, env=env, path=WORK)

    def execCommand(self, proto, cmd):
        self._spawn(proto, ["sh", "-c", cmd.decode()])

    def openShell(self, proto):
        self._spawn(proto, ["sh"])

    def getPty(self, term, windowSize, modes):
        pass

    def windowChanged(self, windowSize):
        pass

    def eofReceived(self):
        pass

    def closed(self):
        pass


components.registerAdapter(TestSession, TestAvatar, session.ISession)


class Realm:
    def requestAvatar(self, avatarId, mind, *interfaces):
        user = TestAvatar(avatarId.decode() if isinstance(avatarId, bytes) else avatarId)
        return interfaces[0], user, user.logout


def host_key():
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, "hostkey")
    if not os.path.exists(path):
        from cryptography.hazmat.primitives.asymmetric import ed25519
        from cryptography.hazmat.primitives import serialization
        k = ed25519.Ed25519PrivateKey.generate()
        data = k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.OpenSSH,
                               serialization.NoEncryption())
        with open(path, "wb") as f:
            f.write(data)
        os.chmod(path, 0o600)
    private = keys.Key.fromFile(path)
    return private, private.public()


class Factory(factory.SSHFactory):
    services = {b"ssh-userauth": userauth.SSHUserAuthServer, b"ssh-connection": connection.SSHConnection}


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 3200
    log.startLogging(sys.stdout)
    priv, pub = host_key()
    f = Factory()
    f.privateKeys = {pub.sshType(): priv}
    f.publicKeys = {pub.sshType(): pub}
    p = portal.Portal(Realm())
    p.registerChecker(checkers.InMemoryUsernamePasswordDatabaseDontUse(tester=b"weasel"))
    f.portal = p
    reactor.listenTCP(port, f, interface="127.0.0.1")
    import base64
    import hashlib
    print("fingerprint: SHA256:" + base64.b64encode(hashlib.sha256(pub.blob()).digest()).decode().rstrip("="),
          flush=True)
    reactor.run()


main()
