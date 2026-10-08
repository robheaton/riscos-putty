# PuTTY for RISC OS

A native RISC OS port of the PuTTY SSH/Telnet client (upstream 0.85): a terminal
window that runs in the desktop, with a Connect box and saved sessions, a file transfer
window, a key agent (PuTTY's Pageant, inside the program), a key generator (`!PuTTYgen`), and the
command-line tools plink, puttygen, psftp and pscp.

**Status (2026-10-02): usable.** PuTTY's own code (SSH-2 with the hybrid
NTRU Prime / Curve25519 key exchange, ed25519 and the other host and user key types,
AES, ChaCha20, the terminal emulator) is compiled unchanged; this project adds the
RISC OS front end and platform layer. The terminal has been run on a real Pi (the Info box
was the first thing to change); everything else has been tested in RPCEmu against PuTTY's
test server and a second SSH implementation, with vim, htop and whiptail, and by the
scripts in `tools/` (see Testing). The command-line tools have not yet been run on a
real machine.

## Using it

Run `!PuTTY` (it puts an icon on the icon bar and opens the Connect box). Type the
host name, check the port and connection type, give a login name (or leave it empty
to be asked), and click Open. A private key (.ppk) can be dragged onto the Connect box;
otherwise PuTTY asks for a password in the terminal window. Save, Load and Delete keep
named sessions; Sessions... lists them. Settings... opens PuTTY's full configuration dialog
(the tree of Session, Terminal, Window, Connection and SSH panels: fonts, colours, keys,
proxies, tunnels and the rest); its Open button starts the session.

Starting `!PuTTY` again while it is running does not make a second copy: the one running opens the Connect box (or, if
the command line names a host or a saved session, starts that session) and the new one goes away, so there is one icon
and one key agent.

Menu on the icon bar icon gives Info (the standard "About this program" box), Help, New session...,
File transfer..., Sessions (the saved sessions: choose one to connect at once), Keys... (the key agent), Key generator... and Quit.

In the terminal window: select with the mouse (drag, double-click, triple-click;
Adjust extends); the selection goes straight to the RISC OS clipboard. Shift-Insert
pastes, Ctrl-Insert copies (Ctrl-Shift-C/V too, if Window > Selection says so), and a text file
dropped on the window is pasted. The mouse wheel, the scroll bar and Shift-Page Up/Down scroll back
(Ctrl-Page Up/Down a line). The left Alt key is Meta: Alt with any key sends Escape then what the key sends
(Alt-F, Alt-Backspace, Alt-Left...); the right one still composes characters. Menu opens PuTTY's window
menu: Copy, Paste, Special command, Event log (what PuTTY is doing, and why a
connection failed), Change settings (the configuration dialog for the running
session), Restart session, Reset terminal, Clear scrollback, Duplicate session,
New session. `putty -ssh -P 22 -l user -i key.ppk host` and `putty -load
session` work from the command line.

Settings and host keys live in the PuTTY directory of `<Choices$Write>`.

Random numbers: good session keys need the **CryptRandom** module. It is normally already in your `!System`
(`*Info System:Modules.CryptRand` will tell you) and PuTTY loads it itself; if it is missing, install the
CryptRandom package with !PackMan (System section; or see
<http://www.chiark.greenend.org.uk/~theom/riscos/crypto/>). Without it PuTTY warns, once a run, that its random
numbers come only from timing jitter.

**File transfer.** The Connect box's File transfer button (or File transfer... on the icon bar menu) connects to
the same kind of address, but opens a window like a Filer directory display of the other computer: name, size, date
and permissions, with the sprite for each file's type. Double-click a directory to go into it (the first row is the
parent). Drag a file (or several selected ones) to a Filer window, or onto an application's icon or window, and
it is copied there; double-click a file for a save box; Menu on the window has Copy to... (several files or whole
directories), Copy to here..., New directory, Rename, Delete, Select all, Parent, Home, Go to, Refresh, Hide dot files
and Sort by (name, size or date).
Drag files from a Filer window onto the window to copy them up (onto a directory's row to put them in it). A
progress box with a Cancel button shows each copy. Names and filetypes are converted exactly as psftp and pscp do.

**Key agent.** Keys... on the icon bar menu lists the private keys PuTTY is holding for logging in (PuTTY's
Pageant, which here lives inside the program: it serves PuTTY's own connections, sessions and the file transfer
window, not the command-line tools). Add a `.ppk` file with Add key... or by dropping it on the window or the icon bar
icon; a key with a passphrase asks for it once. SSH logins then use the keys without asking for the passphrase, and a
session with agent forwarding ticked lets the other computer ask the agent to sign for it (it never sees the keys).
Start-up list... edits a text file of key files to add whenever PuTTY starts. Keys are forgotten when PuTTY quits.

**Key generator.** `!PuTTYgen` (next to `!PuTTY`; PuTTY's Key generator... starts it) makes RSA, DSA, ECDSA and
EdDSA keys with a progress bar and a Cancel button, loads private keys (PuTTY `.ppk`, OpenSSH, ssh.com: drop the file
on its window or icon), changes a key's comment or passphrase, saves the private key (`.ppk`), the public key, or the
private key converted for OpenSSH or ssh.com, and copies the public key to the clipboard ready to paste into
`authorized_keys`. Files are saved by dragging the file icon from the save box to a directory display, or by typing a
pathname. It needs the CryptRandom module, or else asks you to move the mouse over its window until it has enough
randomness. It is a separate program because upstream deliberately keeps key generation out of anything that makes
SSH connections.

**puttygen** is in `!PuTTY.Tools` (on the Run path once the Filer has seen !PuTTY): it makes keys and converts
them to and from OpenSSH and ssh.com formats, so keys made elsewhere can be used with PuTTY, which reads only `.ppk`.
`puttygen -t ed25519 -o MyKey`, `puttygen id_rsa -o MyKey`, `puttygen MyKey -O public-openssh`; see `!Help` for the
rest. It will not make a key without the CryptRandom module (or `--random-device FILE`).

**psftp** and **pscp** (also in `!PuTTY.Tools`) copy files over SFTP or SCP. Local names are RISC OS ones; the
names on the far side are converted as files cross (`notes/txt` <-> `notes.txt`, a space is a hard space, characters
RISC OS cannot have become `_`, and `name,xxx` carries a filetype). A fetched file is typed from that suffix, else
from its extension, else Text or Data by its content; dates are kept. See `!Help`.

## Building

Needs GCCSDK (`arm-unknown-riscos-gcc` 4.7), Perl, CMake and Python 3 (CMake and Perl
only to generate a couple of source files). Same toolchain settings as `riscos-vncclient`:
`-mlibscl -march=armv4`, no runtime modules, runs in RPCEmu and on a Pi.

    make core      # compile the portable PuTTY core (244 files) into build/ro/libputty.a
    make plink     # build/ro/plink,ff8: the console tool (bring-up and testing)
    make puttygen  # build/ro/puttygen,ff8: the key generator and converter
    make psftp pscp # build/ro/psftp,ff8 and pscp,ff8: the file transfer tools
    make putty     # build/ro/putty,ff8: the Wimp application
    make app       # build/app/!PuTTY: the application directory
    make dist      # build/PuTTY-<version>.zip, with RISC OS filetypes
    make tests     # the diagnostics in tests/ro, for running in the emulator
    make V=1 ...   # show the full compiler commands

PuTTY is compiled straight from the unpacked upstream tree (`upstream/putty-0.85`,
untar `upstream/putty-0.85.tar.gz` there; `upstream/verify.sh` checks the tarball's
checksum and PGP signature). The source lists in the Makefile are transcribed from
upstream's `CMakeLists.txt` files. `src/ro/cmake.h` replaces the generated config
header. The port's version is in `src/ro/roversion.h`.

## Testing

The scripts in `tools/` drive the emulator: `cryptomatrix.py` (every cipher, MAC, key exchange
and key type through plink), `keytest.py` (puttygen against the host's puttygen and OpenSSH),
`sftptest.py` (psftp and pscp against Uppity) and `kbtest.py` (the keyboard prompts: passphrases,
host key questions, the psftp command line). The prompts are fed from a file named by the
system variable `PuTTY$KeyFile` (nothing can be typed at the headless emulator, and a program
cannot fill its keyboard buffer there); `PuTTY$Timeout` makes a prompt nobody answers end.

Upstream ships a test SSH server, Uppity. Build the native tools once, then:

    cmake -S upstream/putty-0.85 -B build/host -DPUTTY_GTK_VERSION=0
    make -C build/host
    tools/testserver.sh      # starts Uppity on 127.0.0.1:2222 with a test key
    tools/deploy.sh          # copies plink, putty, !PuTTY and the test key into the emulator's HostFS

Inside RPCEmu the host is 10.10.10.2; `testserver.sh` prints the host key fingerprint
and an example command. Uppity also accepts the password `weasel` and
keyboard-interactive answers `stoat` and `weasel`. `tools/vncdrive.py` drives the
emulator's screen, mouse and keyboard over VNC for GUI tests (the VNC password is
read from `RPCEMU_VNC_PASSWORD`), and `tools/sprite2png.py` turns a `*ScreenSave`
into a PNG. For the console tool, set the system variable `PuTTY$Timeout` (seconds)
so a stuck test gives up, and `PuTTY$Trace` (a filename) to log socket events. `PuTTYsave$Trace` (a filename)
logs what the receiver of a dragged file answers (`ro_trace` in `util.c` is the helper for such hooks), and
`PuTTY$ForceMouseEntropy` makes the key generator ask for mouse movements even when CryptRandom is there.
`PuTTY$AllowSeveral` (any value) lets a second copy of the program start instead of handing over to the first, for
tests that want separate tasks. `PuTTYkey$Trace` (a filename) logs every key event the terminal window receives, with the modifiers, and
`tests/ro/keyscan.c` (`make tests`) lists the keys the keyboard says are held, which is how the INKEY numbers in
`rowindow.c` were found. To check what the keys send, run `sh -c 'stty raw -echo; exec cat > keys.bin'` in a session
and press them with `vncdrive.py` (hold each one for a few tenths of a second: `keydown`, `wait`, `keyup`).
`VNC_SLOW=3` in the environment of `vncdrive.py` triples its key and button timings, for a busy host.
The windows are the same size in OS units in every screen mode; to check a 180 dpi style one (one OS unit to the
pixel) in the emulator, `WimpMode X1600 Y1200 C16M EX0 EY0`, start PuTTY, and run `vncdrive.py` with `VNC_EIG0=1`
(RPCEmu delivers the pointer position as if the mode had two OS units to the pixel, so without it clicks go astray; see
docs/DESIGN.md). `tests/ro/modeinfo.c` and `tests/ro/ptrinfo.c` print the mode and where the system thinks the pointer is.

## Layout

- `upstream/` - pristine PuTTY 0.85 tarball, signature, checksums, public keys, `verify.sh`.
- `src/ro/` - the RISC OS code. The platform layer: `net.c` (sockets over the Internet
  module), `storage.c` (sessions, host keys, seed under `Choices:`), `noise.c` (entropy),
  `unicode.c`, `util.c`, `stubs.c`, `compat.c`, `platform.h`, `cmake.h`, and the console
  tool `console.c`/`plink.c`. The Wimp front end: `roapp.c` (task, poll loop, icon bar),
  `rowindow.c` (terminal window: cell grid, painting, keys, mouse), `roseat.c` (how a
  session talks to its user), `roclip.c` (clipboard), `rocontext.c` (window menu),
  `roconnect.c` (Connect box), `rofont.c`, `rodlg.c`/`rodialog.c`/`romenu.c`/`rowimp.c`
  (dialogue, menu and Wimp helpers), `rofiles.c`/`roftp.c`/`rofile.c` (the file transfer window, its
  SFTP connection, and the local file layer shared with psftp and pscp), `rosave.c` (the save box and the
  drag-to-save protocol), `roagent.c` (the key agent: glue to upstream's `pageant.c`, and its window),
  `rogen.c`/`rokeygen.c` (the key generator) and `roinfo.c` (the Info box).
- `app/!PuTTY/`, `app/!PuTTYgen/` - the application directories' static files (`!PuTTYgen` is built from
  `rogen.c` and `rokeygen.c`; its sprite is drawn by `tools/mksprites.py`).
- `patches/` - the few lines changed in upstream files (`cmdgen.patch`, `psftp.patch`, `pscp.patch`), applied at build time.
- `tests/ro/` - diagnostics run in the emulator: the Resolver SWIs (`dnstest`), the Font Manager, the HAL
  counter, half-closed connections on the loopback (`halfclose`), the CryptRandom module (`crtest`), file types
  (`ftype`) and what the C library says after a failed file call (`errtest`).
- `tools/echoserver.py`, `tools/conchserver.py`, `tools/testproxy.py` - test servers: a Telnet/Raw echo
  server, a second SSH implementation (Twisted Conch, with port forwarding), and an HTTP/SOCKS proxy.
- `tools/cryptomatrix.py` - runs every cipher, MAC, key exchange, key type and compression mode
  through plink in RPCEmu against Uppity (see docs/DESIGN.md).
- `tools/sftptest.py` - runs psftp and pscp in RPCEmu against Uppity (SFTP and SCP, both directions, names, types, dates).
- `tools/termkeys.py` - holds each special key in a terminal window in RPCEmu and checks the bytes sent
  (51 keys: cursor, editing and function keys, Shift/Ctrl/Alt combinations, Ctrl-Shift-C/V).
- `tools/keytest.py` - runs puttygen in RPCEmu against the host's puttygen and OpenSSH: keys generated,
  every key format read and written, editing, errors (see docs/DESIGN.md).
- `tools/` - test server, deploy, sprite and zip tools.
- `docs/DESIGN.md` - decisions and findings.

## Licence

The same as PuTTY's: the MIT licence. See `LICENSE`, which carries the copyright
line for the new code and, in full, PuTTY's own licence text.
