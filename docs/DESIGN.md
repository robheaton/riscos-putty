# Design notes and findings

## Approach

PuTTY is compiled unmodified from the upstream tarball. Everything RISC OS-specific
lives in `src/ro/`, written against the same interfaces upstream's `unix/` directory
implements (Socket/Plug, storage.h, the noise functions, `platform.h`). Where an
upstream Unix file is not Unix-specific it is used as it stands (`unix/uxsel.c`,
`unix/utils/cmdline_arg.c`, `fontspec.c`, `pgp_fingerprints.c`).

The C library is the Shared C Library as wrapped by GCCSDK's `-mlibscl`. It lacks
`stat`, `opendir`, `getaddrinfo`, `poll` and the wide-character functions, so
`storage.c` uses `OS_File`/`OS_GBPB` directly and `compat.c` supplies `wcslen`,
`mbrtowc`, `wcrtomb` (Latin-1, which is what DEFAULT_CODEPAGE means here).

## Networking (`net.c`)

Follows upstream's `unix/network.c` (its IPv4-only path). Sockets are Internet module
handles driven by `_swix`; readiness comes from `Socket_Select` over the table that
`uxsel.c` maintains. `ro_net_poll(timeout_cs)` is the single entry point the event
loops call.

Name lookup uses the Resolver's "multitasking" `Resolver_GetHost` (0x46001), so a slow or dead name
server never freezes the desktop: `sk_namelookup()` returns at once with an address that is still
"resolving", `ro_net_poll()` asks again each time round the loop, and a socket waiting for the address
connects when it arrives. The calling convention (found by experiment, `tests/ro/dnstest.c`): the first
call with a name returns R0 = 36 (EINPROGRESS) and starts the query; later calls with the same name return
R0 = 0 and a hostent in R1, or R0 = -1 (not found) or -2 (the name server did not answer). **On RISC OS 5.30
the pointer to the name must be in R0** (R1 is ignored there); with R0 = 0 every name is "not found", and any
other value that is not a pointer gives "Bad parameters to Resolver SWI". Other documentation (OSLib,
RISC OS 6) says R1. So `resolver_lookup()` passes the name in both, falls back to R0 = 0 with the name in R1,
and finally to the blocking `Resolver_GetHostByName` (which freezes the desktop for as long as the
resolver's own settings allow, Inet$ResolverDelay x Inet$ResolverRetries). Both lookup SWIs know nothing of
numeric addresses (`GetHostByName` says "not found" for 10.10.10.2), which `sk_namelookup()` parses itself.
In RPCEmu a name server that does not answer makes the Resolver give up after a second or two and answer
127.0.0.1, so a mistyped name can end as "Connection refused" rather than "not found".

## Entropy (`noise.c`) - security-relevant

Upstream reads `/dev/urandom`. RISC OS has no equivalent, and the 5.31 BCM2835 kernel sources contain no
entropy API. The usual answer is the **CryptRandom** module (Theo Markettos; 0.13 of 2012 is ARMv7 and RISC OS 5
compatible; it is what UnixLib's `/dev/random` uses): it gathers noise from mouse movements, key presses and
the HAL timer, mixes it with a seed file kept in `Choices:Crypto`, and says in its own documentation that it is
much less secure than a true random source but unpredictable and different on every start.

**Where it is.** The standard RISC OS 5 `!Boot` already carries it as `!Boot.Resources.!System.310.Modules.CryptRand`
(the emulator's, from 2016, is "0.13 (01 Jun 2012)"), but nothing loads it at boot: programs are expected to
`RMEnsure` it. If it is not on a machine, !PackMan has it (package "CryptRandom", System section, 9,866 bytes,
https://www.riscos.info/packages/arm/System/CryptRandom_0.13-1.zip; that package's module is "0.13 (16 Jul
2012)", file `System/310/Modules/CryptRand`, MIT-style licence); author's page
http://www.chiark.greenend.org.uk/~theom/riscos/crypto/ . It must be 0.09 or later: before that
`CryptRandom_Block` had a hole (a request bigger than the pool came back partly unfilled).

**What `noise.c` does.** `probe()` runs `RMEnsure CryptRandom 0.09 Error ...` (through `OS_CLI`, so success and
failure are unambiguous), and if that fails tries `RMEnsure CryptRandom 0.09 RMLoad System:Modules.CryptRand`
(UnixLib does the same) and checks again; then `CryptRandom_Stir` (no registers) must work. It then reads two
16-byte blocks with `CryptRandom_Block` (R0 -> buffer, R1 = length) and refuses a module whose blocks are all
zero, all ones or equal, treating it as absent. In use it asks for 64 bytes at start-up and 16 on every noise
timer tick. **Whether or not the module is there** we also mix in our own timing jitter (HAL counter, about a tenth
of a second at start-up), the time of every key and network event to the same accuracy, system state, and PuTTY's
own seed file. Without the module that is WEAK (perhaps a hundred bits, and an attacker who knew roughly when a
session started could search for them), so `ro_entropy_is_weak()` says so and both front ends warn (a box, once a
run, in the GUI; always in the event log; set `PuTTY$NoEntropyWarning` to silence the box). **Decision (2026-10-03): this is the design.** Strong randomness comes from CryptRandom, which is
in every standard RISC OS 5 `!System`; our own jitter, the key and network event times and PuTTY's seed
file are always mixed in as well; without the module the tools say so, and a new private key is never made
from the weaker sources alone (puttygen stops; the Wimp key generator makes the user supply mouse
movement). The Raspberry Pi's hardware generator is deliberately not used: its registers sit at an
address that differs between models (and RISC OS versions), reaching them needs a physical-memory
mapping from a program that cannot check what machine it is on, a wrong guess can hang the computer, and
nothing in this port can test it. If RISC OS ever gains an entropy SWI, `ro_cryptrandom_bytes()` is the
one place to add it.

**Tested** (`tests/ro/crtest.c`, RISC OS 5.30 in RPCEmu, both the stock module and the PackMan one): the SWI
numbers are Byte 0x51980, Stir 0x51981, AddNoise 0x51982, Block 0x51983, Word 0x51984 (from the module's own SWI
table, as in UnixLib's `swis.h`); `Block` fills 1 to 20000 bytes completely with nothing written past the end, and
8192 bytes look random (32692 of 65536 bits set, chi-square 262 over the byte values, no repeated blocks); `Byte`
is fine; **`Word` is broken in 0.13: every word is one random byte repeated four times** (100 of 100), so noise.c
must not use it (build 0.01 did, and would have had a quarter of the entropy it thought). With the module absent,
loaded, or installed but not loaded (auto-loaded), plink and the GUI behave as described.

## puttygen (`cmdgen.c`)

Upstream's console key generator and converter, built as `build/ro/puttygen,ff8` (`make puttygen`) and shipped as
`!PuTTY.Tools.puttygen`, which `!Boot` and `!Run` put on the `Run$Path`. It links `cmdgen.c` with the `keygen/`
library (added to the core's source list: the prime and key generation for RSA, DSA, ECDSA and EdDSA), our
`console.c` (prompts), `keygen-noise.c`, `noise.c`, `util.c`, `storage.c` and upstream's `stubs/no-timing.c`; it
needs no sockets or timers, so `net.c` stays out. (`libputty.a` now holds two `dsa.o` and two `rsa.o`, from
`crypto/` and `keygen/`, as it already held two `telnet.o`; the linker picks members by symbol, so that is fine.)

**One upstream file is patched**, the first: `patches/cmdgen.patch`, applied by the Makefile to a copy in
`build/gen`, so `upstream/` stays pristine. It changes (1) the scratch file for an in-place edit, which upstream
names `<key>.tmp` - on RISC OS a dot is a directory separator, so that names a file inside the key - to
`ro_temp_sibling()`: the canonical pathname (`OS_FSControl 37`, so `@.key` or a path variable ends up in the
right directory) with a leaf `PgTmpNNN` in the same directory (the rename that follows cannot cross
filing systems); (2) the `--random-device` help text; (3) one `#include`. `move()` already copes with RISC OS's
`rename`, which will not replace an existing file (it removes the target and renames again).

**Entropy policy.** A new key is the one thing that must not rest on weak randomness (it lives for years and
nothing would warn the user), so `get_random_data` (`keygen-noise.c`, replacing the Unix `/dev/urandom` reader)
takes its bytes only from the CryptRandom module (`ro_cryptrandom_bytes`, which shares `noise.c`'s probe and
auto-load) or from a file named with `--random-device`; with neither it explains how to install the module and
stops. Our timing jitter is still mixed into the PRNG (`noise_get_heavy`) as everywhere, but it is not enough to
be the only source of a private key. Converting or re-encrypting a key needs randomness only for salts, IVs
and padding, none of them secret, so that works without the module. Not done: gathering entropy from the mouse
and keyboard, as Windows PuTTYgen does; that belongs in a Wimp front end.

**File types.** The C library makes a file opened `"w"` Text (&FFF) and one opened `"wb"` Data (&FFD)
(`tests/ro/ftype.c`), and the key code uses `"wb"`. `f_open` now types every file it creates or empties Text,
straight after `fopen`: setting a type on an open file is allowed and survives the writes and the close. A file
appended to keeps its type. Key files, public keys, fingerprints and logs are therefore Text.

**Error messages.** After a failed file call the C library leaves `errno` at -1 (or at 0 after `rename` and
`remove`) and `strerror` says "Unknown Error"; the real message ("File 'x' not found", "'x' is a directory")
is the operating system's last error (`tests/ro/errtest.c`). `platform.h` redirects `strerror` and `perror` to
`ro_strerror` and `ro_perror` (`util.c`), which use it. This fixed the messages in `storage.c` and `sshpubk.c`
as well, and meant rebuilding the whole core.

**Memory.** Saving a key with a passphrase hashes it with Argon2 using 8 MB by default; `riscos_run`'s command
line has that, and a task window with a small slot may not (the failure is a clean "FATAL ERROR: Out of
memory", rc 1; `--ppk-param memory=1024` is the cheaper setting). Key generation takes 0-1 s for ed25519, ed448
and the ECDSA curves, 1-10 s for RSA-1024 and DSA-1024, and 44-138 s for RSA-2048 on RPCEmu (the time is
random: it is a prime search).

**Verified** (`tools/keytest.py`; 104 checks, all passing, RPCEmu against the host's puttygen and OpenSSH 10.2):
11 key types generated on the guest, each with and without a passphrase (ed25519, ed448, ECDSA 256/384/521,
RSA 1024/2048 and with `--primes proven` and `--strong-rsa`, DSA 1024, SSH-1 RSA), every one loading on the host
with the same fingerprint and component dump, and, where OpenSSH can read the type, OpenSSH deriving the same
public key from the exported private key; 22 private keys and 3 public-key files in every format the host tools write (OpenSSH PEM and
new format, with and without passphrases, PuTTY v2 and v3 with each Argon2 flavour, ssh.com, SSH-1, public
keys in both forms), each read by the guest and compared (fingerprint, `--dump`, converted to PPK and back
out as OpenSSH old and new format and ssh.com); in-place `-P`, `-C`, `--reencrypt`, with `@.` and bare names, no
scratch file left; `--ppk-param` (version 2, argon2i with memory and passes, argon2d with parallelism and time);
passphrases from files and from redirected input; the short forms `-l -L -p`, `-E md5`, `-q` and the progress
bar; and the error paths with their messages. With the module absent: generation refuses and says why,
conversion and `--random-device` work, and an installed-but-not-loaded module is loaded. Not yet checked:
a task window's memory, a real Pi (the keyboard prompts are covered by kbtest.py, see below).

## psftp and pscp (`sftp.c`)

Upstream's `psftp.c` and `pscp.c`, built from copies patched at build time (`patches/psftp.patch`, `patches/pscp.patch`:
six call sites, below), with `src/ro/sftp.c` as the platform layer in place of `unix/sftp.c`. The core lists gained
`ssh/sftp`, `ssh/sftpcommon` and `psftpcommon`; `psftp.c` and `pscp.c` define `share_can_be_*` themselves, so those two
constants moved out of `stubs.c` into `plink.c` and `roapp.c`.

**Two kinds of name.** A *local path* is a native RISC OS pathname as the user typed it (a path dragged to a task window
works). A *leaf* is one name as the remote Unix system has it. PuTTY's generic code only ever joins and splits paths with
`/`, so the platform layer holds the convention: names from `read_filename` and from the server are leaves, and
`dir_file_cat` turns a leaf into a local path. The patches cover the places where the generic code crosses between the
two itself: the default local name for `get` and the default remote name for `put` (`psftp.c`), the name sent by
`source`/`rsource` and the join in `rsource`, the default target when none is given, and the progress line (`pscp.c`).
Conversion: `.` and `/` swap, characters RISC OS names cannot hold (`: $ & @ ^ % \ * # " |`, control characters) become
`_`, and a space becomes the hard space (&A0), because **a space ends a name in a RISC OS call** (`OS_File 5` on
"space name/txt" looked at "space"; HostFS lists the name with a space and stores U+00A0 on the host). Spaces in
user-typed local names are converted the same way as they come in.

**File types.** A leaf ending `,xxx` (three hex digits) is a file type, as with NFS, Samba and the FTP clients. Downloads:
the suffix if there is one; else the extension (a small built-in table: `MimeMap_Translate` answered "No MIME mapping
found" to everything in the emulator, and its reason codes are poorly documented); else Text or Data from the first
512 bytes (a NUL or more than 2% control characters means Data). The type is set when the file is closed, together with the
date, because FileCore stamps a file when it is closed. Uploads: `read_filename` and the default remote name add `,xxx`
unless the type is Text or Data (`PuTTY$SftpNoTypes` leaves it off). Reading a name that ends `,xxx` falls back to the
name without it when no such file exists (`resolve_read`). Dates: the five-byte centisecond stamp in load/exec
(`OS_File 5`) both ways; permissions are not sent (`perms` is -1).

**pscp: local or host:path?** pscp decides by the colon (`colon()`, patched with `ro_looks_local`). A prefix before the
first colon that is `FileSystem::` is local; one with a `.` or `@` is a host; otherwise `OS_FSControl 37` on `prefix:` decides:
it succeeds for a path variable (`Choices:`), reports "Filing system X: must be given a filename" for a filing system that
exists, and "not present" or "not recognised" for anything else. That relies on the English error text. `user@host:` always works.

**Directories and wildcards** use `OS_GBPB 10` (load, exec, length, attributes, object type, name; the buffer must be
word aligned). Wildcards are RISC OS ones (`*` and `#`) in the last part of the path only. **The loop and the prompt:**
`ssh_sftp_loop_iteration` is plink's wait (`ro_net_poll`, timers, callbacks) and returns -1 when no sockets are left and no
lookup is pending; at the prompt the keyboard is polled with `OS_Byte 129` every 5 cs, with echo, Backspace, Ctrl-U,
Ctrl-D and Up/Down through the last 32 commands; with redirected input lines are read with `fgetc`. `PuTTY$Timeout` gives
up after N seconds, for tests.

**The keyboard prompts** (`tools/kbtest.py`, 26 checks pass): typing at the headless emulator needs its VNC
password, and nothing else gets a key to a program there: `OS_Byte 138` reports success but neither INKEY nor
`OS_ReadC` ever sees the character (and a program that waits for it hangs the harness for half an hour), and
`*Exec` does not reach them either. So the tools have a test hook: with `PuTTY$KeyFile` set, `ro_test_key()`
(util.c) supplies the keystrokes from that file, to the prompt reader in console.c and to psftp's. It exercises
all of our logic - the line editor (Backspace, Ctrl-U, Ctrl-D, Up/Down history), passphrase and password
prompts, keyboard-interactive with an echoing and a silent prompt, and every host key question ("not cached":
y stores it, n connects once, Return abandons; "does not match": Return cancels, y updates the cache) - but not
the two OS calls underneath, which are as plink has always used them. With `PuTTY$Timeout` set a prompt that
gets no key within that many seconds counts as Escape (INKEY(20) polling instead of `OS_ReadC`). Note that an
abort at a password prompt is a clean exit in PuTTY (`ssh_user_close`), so the return code is 0.

**Verified** (`tools/sftptest.py`, 43 checks pass, RPCEmu against Uppity; Uppity serves SFTP and SCP out of the host's
real file system): psftp `get`, `put`, `-r`, `mget`, `mput`, `reget`, `reput` (restarting a part-done transfer), `mkdir`, `mv`, `ls`, `lcd`, `lpwd`, `-b` batch files, errors; 14
file names/types including a 3 MB and a 2 MB file; pscp both ways with SFTP and with `-scp`, `-r`, `-p` (dates kept both
ways), wildcards, `-ls`, errors; and psftp commands read from redirected input. Not checked: a real Pi, transfers over a
slow link.

## Console tool (`plink.c`)

Based on `unix/plink.c`. With a keyboard stdin the keys are polled with `OS_Byte 129` (INKEY). Redirect
stdin with the C library's `< file` (then it is read with `fread` and EOF is sent when the file ends), **not**
the Wimp's `{ < file }`: that redirects only `OS_ReadC`, plink does not use it, and the command waits for ever
for keys that never come (kill the far end to get out). Output may be redirected either way (`> file` or
`{ > file }`, which then holds the Wimp's LF CR line ends). Remote output is always stripped of control
characters when stdout is a terminal, because VDU codes can change the screen mode.
`PuTTY$Timeout` makes the program give up after N seconds, for tests.

## Verified so far (RPCEmu, RISC OS 5.30, StrongARM)

Against Uppity: SSH-2 with sntrup761x25519-sha512, ssh-ed25519 host key (checked with
`-hostkey`), AES-256-CTR, HMAC-SHA-256, public-key login from a `.ppk`, exec channel,
exit status. Unknown host key refused in batch mode; connection refused reported;
name lookup through the system resolver.

Also against Uppity: password login, keyboard-interactive login (echoed and silent prompts, a
zero-prompt step), host key prompts and the host key cache.

**The algorithm matrix** (`tools/cryptomatrix.py`, run 2026-10-01): PuTTY's code is compiled unchanged for 32-bit
ARM with an old GCC, so every algorithm was checked against Uppity (which can be told to offer just one of each
kind): ciphers aes256/192/128-ctr, aes256/192/128-cbc, aes256-gcm, aes128-gcm, chacha20-poly1305, 3des-cbc; MACs
hmac-sha2-256, hmac-sha2-512, hmac-sha1, hmac-sha1-96, hmac-md5 and the -etm variants of sha2-256, sha2-512 and
sha1; key exchanges curve25519, curve448, ecdh-nistp256/384/521, DH group exchange (sha1, sha256), DH groups
14 (sha1 and sha256), 15, 16, 17 and 18, sntrup761x25519, mlkem768x25519, mlkem768nistp256, mlkem1024nistp384,
and RSA key exchange (rsa2048-sha256); host keys ed25519, ed448, ECDSA nistp256/384/521, RSA (rsa-sha2-256,
rsa-sha2-512, ssh-rsa); user keys ed25519, ed448, ECDSA nistp256/384/521, RSA-2048, DSA-1024; zlib and
zlib@openssh.com compression; password login. **All passed.** DH groups 17 and 18 (6144 and 8192 bit) take
minutes in the emulator (a 600 s allowance covers them) and are only used if a server offers nothing newer. Bulk
data: 1.3 MB downloaded (`seq 1 200000`) and 1.1 MB uploaded (`md5sum` of a redirected file) arrive bit for bit,
with rekeying (a 100 KB limit), with zlib, with chacha20-poly1305 and with aes256-gcm. (With a 20 KB limit a
590 KB download rekeys only once or twice rather than thirty times, on the ARM build and on a host-built plink
alike, so that cadence is upstream's behaviour, not a porting problem.)

**A second implementation.** Uppity is built from PuTTY's own code, so it only shows that the port agrees with
PuTTY. `tools/conchserver.py` is a small server on Twisted Conch (somebody else's SSH, with port forwarding):
the RISC OS plink connected to it with curve25519-sha256, ssh-ed25519, aes256-ctr, hmac-sha2-256 and a
password, and ran a command.

**Port forwarding** (against Conch): remote forwarding (`-R 4000:10.10.10.2:2324`; a client on the host
connected to the server's port and talked to an echo server through the RISC OS PuTTY) and local forwarding
(`-L 8889:127.0.0.1:2324`; the GUI task listens on a RISC OS port, and a Raw session in another window of the
same application typed through the tunnel and got the echo back; this also exercises `sk_newlistener`,
accepting, frozen sockets and half-close). Note that the destination of `-L` is looked up by the *server*, so
use 127.0.0.1 there, not the guest's name for the host.

**Proxies** (`tools/testproxy.py`): HTTP CONNECT, SOCKS4, SOCKS4a and SOCKS5, with addresses and with names the
proxy resolves (it saw the names, so PuTTY's "proxy does the DNS" setting works).

**Telnet and Raw** against a small echo server (`tools/echoserver.py`): typing, echo and Return work in the
Wimp terminal (Return and Backspace are sent as PuTTY's "dedicated" keys, which matters for local line editing
and Telnet's New Line; keyboard-interactive login needed the same). A server that goes away is reported
("Remote side unexpectedly closed network connection") and the window is marked inactive. The name lookup is
non-blocking (see Networking).

**Rlogin** (`tools/echoserver.py --rlogin`, plink in RPCEmu, 2026-10-02): the start-up is sent as the protocol wants
(`\0`, local user, server user `tester` from `-l`, `xterm/38400`, `\0`), the server's `\0` is swallowed, and typed
data goes to the server and its echo comes back. The client does not wait for the server's acknowledgement before
sending input, so a server must keep what arrives with the start-up (the test server dropped it at first, which looked
like plink not sending). Rlogin has no half-close, so plink with its input from a file does not stop when the file ends:
it waits for the server to close, as upstream does. (The host's own plink cannot be used for comparison: Rlogin needs a
privileged source port.)

Not yet tested: SSH-1 (no SSH-1 server to test against), X11 forwarding (not applicable), a real Pi. (Agent forwarding is tested: see the key agent, below.)

A **paste** of 2000 lines (10 KB, taken with Copy all from the scrollback and pasted back through `tee`) arrives
complete and in order.

**Testing on RISC OS, and its traps.** A command-line program started by the emulator's host-command channel is
the foreground: it does not multitask, so a Wimp task such as the GUI gets no time until it exits (a test where
plink talks to the GUI's tunnel listener only "worked" after plink ended, which looked like lost data). A blocking
`Socket_Recv` hangs the whole machine; test programs use `FIONBIO` sockets and bounded delays (`tests/ro/halfclose.c`).
Plink's stdin must be redirected with the C library's `< file`; the Wimp's `{ < file }` is not seen. `PuTTY$Trace`
(a file name) logs socket events with centisecond stamps.

## The Wimp application

Files: `roapp.c` (the Wimp task, poll loop and icon bar), `rowindow.c` (terminal window: cell grid,
painting, keys, mouse, Event Log), `roseat.c` (Seat and LogPolicy, starting and restarting the
connection), `roclip.c` (the global clipboard), `rocontext.c` (the window menu), `roconnect.c` (the
Connect box), `roconfig.c`/`rocfgwin.c` (the configuration dialogue), `rofont.c` (fonts),
`rodlg.c`/`rodialog.c` (dialogues and message boxes), `romenu.c`, `rowimp.[ch]`, `rolocal.c` (the local terminal) and
`be_putty.c` (the program's list of backends). `rowimp`, `rodlg`, `romenu` and the clipboard protocol are adapted from riscos-vncclient
(same author).

The icon bar menu follows the usual RISC OS pattern: Info (first), Help..., the application's entries (New
session..., Local terminal, File transfer..., Sessions >, Keys..., Key generator...), Quit. **Info** opens the standard "About this program" window. It is not a
message box: it is a window made by `rodlg.c` in its `DLG_MENU` style (a title bar and nothing else) and used
directly as the submenu of the Info item (`menu_set_window`: the item's submenu pointer is the window handle,
which the Wimp opens beside the item and closes with the menu). The layout was measured from the ROM's !Edit
and !Paint (Toolbox ProgInfo) in the emulator: a 320 pixel wide window, labels 76 wide and right-aligned, sunken
read-only fields (`DI_DISPLAY`, validation `R2`) 26 high and 30 apart; the title is "About this program" and the
rows are Name, Purpose, Author, Port, Licence and Version ("0.04 (01-Oct-26), PuTTY 0.85", the date from
`RO_PORT_DATE` in `roversion.h`). The terminal's window menu does not repeat Info (the Style Guide keeps it on the icon bar menu). **Sessions**
lists the saved sessions each time the menu is opened (so one saved a moment ago is there) and starts the
chosen one at once, or opens the Connect box with it loaded if it has no host name. **Help...** runs
`Filer_Run <PuTTY$Dir>.!Help` (with the real path, so the editor's title shows it). A window's size in rows and
columns is kept in its `Conf` as it is dragged, so Change settings shows it, Duplicate session copies it, and
saving the session from the Session panel keeps it.

The poll loop (`roapp.c`): one Wimp task runs every session. Each trip round the loop gives the Wimp a
turn (`Wimp_PollIdle`, waking every two centiseconds while a session exists so incoming data is
noticed promptly), runs what PuTTY has pending (sockets through `ro_net_poll`, timers, top-level
callbacks) and lets each window paint what changed. Menus and their selections: whoever opens a
menu registers a handler with `roapp_menu_opened()`, because Menu_Selection events do not say
which menu they belong to.

The terminal window:
- PuTTY's Terminal draws through `TermWin`. We store what it draws in a grid of cells and paint from
  the grid (in the Wimp redraw loop, and with `Wimp_UpdateWindow` for changed cells), so a redraw is
  always right whatever PuTTY thinks is already on screen.
- The window's work area is as tall as the scrollback, but rows are always painted from the top of the
  visible area; every change of scroll offset is followed by a full redraw because the Wimp copies pixels
  when it changes.
- **The scroll bar's thumb is honest** (0.07). The thumb is the window's height as a fraction of the
  work area's, so a screen-high work area made it look too small while the scrollback was short - but the
  Wimp will not let a window be dragged bigger than its work area, and sends no request when the drag is
  clamped, so the work area must be screen high BEFORE a resize starts. So `roterm_idle()` (every turn of the
  main loop) makes it screen high only while the pointer is at the window's size box or toggle size icon
  (with margin) or a button is held after that (the drag), and otherwise it is exactly the lines there are
  (`extent_h`, `wide_extent`). Programmatic resizes (Change settings, an Open_Window_Request that changes
  the size) widen it first. Where the zones are matters: with a vertical scroll bar and no horizontal one
  the size box is in the scroll bar column at the bottom, level with the last rows of the work area, not
  below it. Checked in RPCEmu: full-track thumb at rest, shrinks when the pointer reaches the size box,
  a drag past the old size works, the toggle icon maximises and restores.
- **Window position memory** (0.07): when a window is closed (or the app quits) its top left is kept in
  `<Choices$Write>.PuTTY.WindowPos` under `host:port` (64 entries, most recent last: `ro_winpos_put`), and
  the next window for that host opens there, clamped to the screen and stepped aside (+28 px, -24 px) if
  another of the task's windows is already there (a duplicate). Without an entry it cascades as before.
- The palette starts all black: the Terminal reports only entries that differ from black.
- Box drawing and block elements (U+2500-U+259F) are drawn with rectangles because the fonts have none
  of them; they join up from cell to cell. Everything else is painted with the Font Manager
  (`\EUTF8` encoding, so characters the font lacks are simply missing).
- Keys use PuTTY's `format_arrow_key`/`format_function_key`/... helpers. Modifiers are read with
  `OS_Byte 129` and INKEY numbers (the numbers key events use do not work). The INKEY number of a key is
  one more than the scan number `OS_Byte 121` finds for it (Page Up is scan 63, INKEY(-64)); `tests/ro/keyscan.c`
  prints both for the keys held, and the table in `rowindow.c` once had three of the scan numbers in it.
  Some keys arrive as the code of another: Page Up/Down as Shift-Up/Shift-Down (and, with Shift held, which
  cancels that, as plain Up/Down), Home as Ctrl-^ (0x1E, so Ctrl-6 looks like Home). They are told apart by asking
  whether the real key is down; if the key was let go before we looked, a code with the Shift bit and nothing else
  is taken for the page key, and Ctrl-^ for Ctrl-^. Shift-Page scrolls the terminal by a page and Ctrl-Page by a
  line, as PuTTY does; Ctrl-Space is NUL; Ctrl-Shift-C/V copy and paste when "Window > Selection" asks.
  With Alt (Meta) held the Wimp is told of no key but Tab and Space - not the letters, the cursor, editing or
  function keys, Backspace, Return or Escape - so `poll_meta` scans the keyboard (every 2 cs) for them and sends
  Escape and the key's usual sequence when one goes down (each press once: there is no auto-repeat).
- The mouse uses button type 6 (click/drag); double and triple clicks are timed by us; a held button
  is tracked by polling the pointer each trip round the loop. Select is PuTTY's left button, Adjust
  its right (extend); Menu opens the window menu.
- Clipboard: RISC OS has one global clipboard, so, as in PuTTY for Windows, a mouse selection is copied
  to it at once; Shift-Insert pastes and Ctrl-Insert copies. The protocol (ClaimEntity, DataRequest,
  DataSave, DataSaveAck, DataLoad, RAMFetch) is carried out by `roclip.c`; a text file dropped on a
  terminal is pasted.
- The Event Log window is another terminal with no connection: events are written to it as text
  (control characters stripped, since some come from the server), which gives scrolling, selection and
  copying for free.
- Questions (host key, weak crypto, log file exists, close this session) are non-modal boxes answered
  through PuTTY's callbacks. Closing a window or quitting with a live connection asks first.
- A session that ends keeps its window ("(inactive)" in the title) unless Close on exit says otherwise;
  Restart session on the menu reconnects.

The Connect dialogue (`roconnect.c`) edits a working `Conf`: host, port, protocol, login name and key file
come from the fields, everything else from the defaults or the saved session that was loaded. A key
file can be dropped on the dialogue. Sessions are PuTTY's own key=value files under
`<Choices$Write>.PuTTY.Sessions`, named with every awkward character escaped as =XX.

The configuration dialogue (`roconfig.c`, `rocfgwin.c`, internals in `roconfig_i.h`) is PuTTY's own
`config.c` driven through its `dlg_*` interface. config.c describes the box as a tree of controls
(`struct controlbox`); `roconfig.c` keeps each control's value in a `Widget` (an edit box's text is the
indirected buffer of its icon, so the Wimp edits it in place), lays the controls out once in pixels with
the same column flow as the GTK front end (`ctrl_columns` percentages, column start and span), and
implements `dlg_*` on the model. `rocfgwin.c` makes the Wimp icons of the panel on show (and of the
tree and the Open/Cancel row) and deletes them when another panel is chosen; list boxes are rows of
icons with scroll arrows, draglists add Up and Down buttons, drop-downs and combos open a pop-up menu,
the colour chooser (three RGB fields and a swatch of the colour they make) and font chooser (name, size, a
menu of installed fonts) are small `rodlg` dialogues, and a file dropped on a file field fills it in.
The swatch (`DI_SWATCH`, 0.07) is the one thing in `rodlg` that is not just icons: a dialogue with one is
not auto-redrawn, so it gets Redraw_Window_Request, lets the Wimp draw its icons, and paints the colours
over them (`dlg_event_redraw`). The swatch icon must NOT be filled - the Wimp painted a filled icon's
background over ours and the well stayed white. The Wimp says nothing as digits are typed in a writable
field, so while the chooser is open the main loop keeps getting null events (`roconfig_idle_wanted`) and
looks at the fields each time.
The Colours panel of the main dialogue has a swatch too (0.08): `cfgwin_create` no longer makes the window
auto-redraw, `roconfig_event_redraw` lets the Wimp draw the icons and paints the selected colour in a sunken
well (a decor icon) beside the Modify button, found by the labels PuTTY gives its controls (Red, Green, Blue,
Modify) in the panel "Window/Colours"; `cfgwin_update_widget` repaints it when PuTTY changes the numbers. The category tree is an accordion: only
the branch leading to the panel on show is open, so it never outgrows the window. One code path serves
three uses: the full dialogue from the Connect box (Open starts the session), and Change settings on a
running session (`apply_settings` in `rowindow.c`, after unix/window.c: log, line discipline, terminal,
back end, clipboards, font, character set, window size, scroll bar; a change of "Display scrollbar" makes
a new Wimp window because the bar is a creation flag).
Controls for settings the port does not act on are left out rather than shown doing nothing
(`control_is_inert` in `roconfig.c`: "Hide mouse pointer when typing", "Separate window and icon titles" and "Gap
between text and window edge" - the Wimp has one title and its own window border); a box that is left with
nothing in it goes too. The settings are still stored in saved sessions, so a session saved by another PuTTY
keeps them.

Measured in RPCEmu (RISC OS 5.30): the screen is 1600x1200 with one pixel = 2 OS units; Corpus.Medium
is fixed pitch with an advance of 7200 millipoints at 12 pt (18 OS units; 400 millipoints per OS
unit). A screenshot without VNC: `*ScreenSave file`, then `tools/sprite2png.py`.

**Screen modes: design pixels.** The dialogues (`rodlg`), the configuration window (`rocfgwin.c`) and the file
transfer window (`rofiles.c`) are laid out in pixels, as they come in a 90 dpi mode, and `UI_PX` (rowimp.h, 2) says what
each is worth in OS units *in every mode*. Until 0.09 a layout was scaled by the mode's eigen factors instead, which is
right in a 90 dpi mode (eigen factors 1,1: a pixel is two OS units) and wrong in a 180 dpi one (0,0: one OS unit to the
pixel): an OS unit is 1/180 inch in every mode and the desktop font is a fixed number of OS units tall (12 point is 30
of them), so the windows came out half the size while their text stayed the same, and the labels and buttons were
squashed into each other (found by a user on a high density display; the Connect box showed "name:" and "ection:" with
the first letters cut off). Now a window is the same physical size in any mode, as the Filer's are. What really is a
real pixel still uses the mode's own factors (`xeig`, `yeig`): the terminal's cells and box drawing in `rowindow.c`
(the font is scaled by the Font Manager, the grid is snapped to pixels), the one-pixel-wide lines of the
progress bar and colour swatch and their inset inside the bevel (the Wimp draws bevels a pixel thick).
`tests/ro/modeinfo.c` prints the mode and how wide the Wimp makes a few strings, which is the same in any mode.

A design that is a fixed number of OS units can be bigger than a screen of few OS units (a 180 dpi mode of a 1080p
display is 1920x1080 OS units; a 90 dpi 800x600 is 1600x1200). The configuration dialogue is 1440x1160, the biggest
window, so `cfgwin_create` checks: if it does not fit in the screen's height under a 42 unit title bar (or its width
plus a scroll bar), the visible part is made as big as the screen allows and the window gets scroll bars. They need no
code of ours: the window has no "scroll request" flag, so the Wimp scrolls it itself and sends the new position as
an ordinary open request (`roconfig_event_open` passes it on). Anything that works out a screen position from a work
area one must include the scroll offsets (`open_popup` does). Other windows are 1320x816 (PuTTYgen) or smaller.

**Changing the mode while the program runs (0.11).** The windows keep their sizes in OS units (the Wimp reopens them
where they were, in the same OS units, so after a switch from 90 to 180 dpi a window sits twice as far from the screen
edges in pixels and is twice as big), but a terminal's font is a Font Manager handle, and the Font Manager works a
font's size in pixels out from the mode when the font is *found* and does not redo it when the mode changes. A terminal
window open across a switch from 90 to 180 dpi drew its letters half the size they should be inside cells that were
still the right size (the text spread out like "e c h o"), and across a switch the other way twice the size, on top of
each other: what a user saw as "all the text is too big and squashed". `Message_ModeChange` now calls
`roterm_mode_changed` (`rowindow.c`), which loses each window's font and finds it again (in that order: found first,
`Font_FindFont` would return the same handle and the same stale size), then resizes the window if the cell size came
out different and redraws. Only the terminal uses fonts; the dialogues' text is the Wimp's own, which it redoes itself.
It was found by switching with `WimpMode` while a terminal was open (the earlier tests all opened their windows in the
mode they ran in) and checked with screen dumps in both directions.

**Testing in a 1 OS unit per pixel mode.** `WimpMode X1600 Y1200 C16M EX0 EY0` makes one (C32K is "unsuitable for
displaying the desktop"). RPCEmu's mouse hack turns the VNC pointer position into RISC OS coordinates as if the mode
had two OS units to the pixel whatever the mode is, so there the desktop sees the pointer at twice the distance
from the bottom left that the picture shows: the pointer cannot get into the top half of the screen, and a click
lands somewhere else - the first attempts to click Settings... did nothing, and the pointer drawn was not where
`OS_Mouse` (tests/ro/ptrinfo.c) said it was (x 820 y 844 for a click at 410,778). `VNC_EIG0=1` in the environment
of `tools/vncdrive.py` sends the position that makes the desktop's pointer land where the picture is (x/2, and y
measured up from the bottom halved), so scripts can use the pixel positions of the screenshot. After `WimpMode` the
windows of programs already running keep their sizes in OS units, so for the size of a window made in the new mode,
restart the program (or the emulator: the mode is lost then, set it again before starting PuTTY).

**The RISC OS Style Guide's dialogue conventions (0.11, after issue #1 on GitHub).** Measured from the Style Guide's
own templates (the window layouts in the Style Guide application's Templates file, read with a small parser): a group
box is a border `R4` (etched; `R2` is the sunken look of a field) with its title in a separate filled icon straddling the
top edge; an action button is `R5,3` and the default one `R6,3`; the action buttons sit at the bottom right with Cancel on
the left and the default action on its right, about 20 OS units apart and flush with the right edge of the boxes above;
and applying settings is "Set", not "Apply". The configuration dialogue takes its buttons from upstream's `config.c`,
which lays them out the Windows way (Open and Cancel a fifth of the way in from each side) and calls the mid-session
button "Apply", so `place_footer_buttons` and `new_widget` (`roconfig.c`) put them right and rename it. The window menu
of a terminal no longer repeats Info (it stays on the icon bar menu).

## The local terminal (`rolocal.c`)

A session type for programs that need an xterm and the RISC OS command line: a PuTTY terminal window whose "connection" is
a task window of the TaskWindow module. It is a `Backend` like Telnet or Raw (`local_backend`, id "local", protocol
`PROT_LOCAL` = 7, which PuTTY does not use), so Duplicate session, Restart session, Change settings, saved sessions, logging
and the Sessions menu work for it without change. Upstream's `be_list.c` has no way to add a backend, so the program's list
is `src/ro/be_putty.c`, which is that file for the PuTTY program (ssh, telnet, rlogin, supdup, raw, sshconn) plus this one.
`conf_launchable` is overridden in `rolocal.c` (a local session has no host name, and upstream's says such a session cannot
be started); the linker takes ours first and leaves the library's for the other programs. `putty -local` works because
`cmdline.c` matches `-<id>` for every backend; the "host" argument is taken as the command.

**The protocol** (PRM volume 3, "TaskWindow"; DeskLib's `TaskWindow` library implements the parent side, and
`tests/ro/twprobe.c` is a script-driven parent that logs every message, which is how the details below were found rather than
guessed). PuTTY starts `TaskWindow ["command" -quit] -ctrl -display -task &<our handle> -txt &<id> -name PuTTY -wimpslot nK`
with `Wimp_StartTask`, which returns the task's handle. With no command the task shows the supervisor prompt, the single
character `*` (it comes as an Output message at once: `-display`), and runs until told to quit; with a command it runs it and,
with `-quit`, finishes (without `-quit` a prompt follows the command). The messages: Input (&808C0, parent to task: word 5 is
the number of **bytes**, the bytes follow from word 6, at most 232 a message; DeskLib's `SendInput` puts 24 more than that in
the count of a short message, which is not what the module wants), Output (&808C1, task to parent, the same layout, and the
parent must acknowledge each by sending it back as event 19 with `your_ref` set to its `my_ref`, or the task waits), Ego
(&808C2: the `-txt` number; the sender is the task), Morio (&808C3: the task has finished, however it ended: `ShellCLI_TaskQuit`
typed at the prompt, `-quit`, or being killed) and Morite (&808C4: parent to task, kill). All three the task sends must be in
the list given to `Wimp_Initialise` (the Wimp only delivers the messages named there). What a task writes comes through as
written (with `-ctrl`), VDU codes and all. The Escape character in an Input message is the Escape key: the module raises an
escape condition, which interrupts a running program and **throws away what was still waiting to be read** (a test that sent
a program's name and the keys for it in one message lost the name); a program that has turned the terminal raw (UnixLib's
termios, `tests/ro/xtest.c`) does not get an escape condition, but reads ESC as an ordinary byte, so cursor key sequences and
everything else arrive exactly as PuTTY's terminal sent them. 13 messages of input sent back to back (3000 bytes,
`tests/ro/inecho.c`) arrived without loss; 15 KB of output in 232 byte messages came through in order.

**Input flow control.** The module's input buffer is small: 3000 bytes in 13 back-to-back Input messages arrived short by a
few bytes, 6000 by about 90, and 500 typed at a program that was not yet reading lost the end. So `rolocal.c` queues what is
typed. A few bytes with nothing in front of them go as Input messages; anything more goes by the Wimp data transfer protocol,
which the module answers (`twprobe`'s `ds`): Message_DataSave, its Message_RAMFetch for a 1040 byte buffer, Wimp_TransferBlock,
Message_RAMTransmit. It takes ONE block per DataSave and does not ask again after a full one, but it does not answer the next
DataSave until the program has used the block up: that is the flow control. The two routes do not keep their order (Input
messages sent while a block was waiting overtook it), so after every block another DataSave goes out at once, empty-handed if
need be; its answer says the block is used, and until then typing is queued for it. A module that never answers the first
DataSave gets paced Input messages instead (`PuTTY$LocalNoRAM` forces that, for tests). A lone Escape jumps the queue and
empties it. `tests/ro/inecho.c` (with a number of seconds it sleeps before reading) and `tools/localtest.py` check it: 500 to
8000 bytes, to a reader that sleeps and to one that does not, all arrive exactly.

**Screen codes.** The prompt, BASIC and plain RISC OS programs write VDU codes: Delete is 127 (a line editor echoes it for
Backspace; an xterm ignores it, so a deleted character stayed on screen), CLS is 12, `COLOUR n` is 17,n, `TAB(x,y)` is
31,x,y, MODE is 22,n. `filter_output` turns the useful ones into ANSI (127 into BS SP BS, 12 and 22 into clear and home, 30 home,
11 up, 17 into the colour change, 20 into the default colours, 31 into cursor addressing; BBC colours 0-7 are ANSI's in the same
order) and drops the others with their parameter bytes (printer, graphics, palette, windows); parameters are consumed by count,
across Output messages. Escape sequences, Tab, Return, Backspace and the bell pass through, and so do 14 and 15 (shift out and
in, which a vt100 terminal uses for line drawing; RISC OS only means paged mode by them). Tab (9) is not "cursor right" and FF
(12) is not a line feed, for the same reason: the Unix programs this is for use them as ANSI does.

**The environment.** UnixLib builds a program's environment, at its start, from the system variables called `UnixEnv$NAME` and
those with no `$` in the name, and its `TIOCGWINSZ` in a task window answers from `LINES` and `COLUMNS` (outside a task it
reads the VDU variables, which would give the size of the desktop's text window: nothing like the PuTTY window). So the
backend sets `UnixEnv$TERM` (the session's terminal type), `UnixEnv$COLUMNS` and `UnixEnv$LINES` when the first local terminal
starts (from the Conf's window size) and on every resize, remembers what they were, and puts them back when the last one
goes. A program already running never hears of a resize (there is no SIGWINCH to send); one started afterwards does. (An
early note here said `Unix$TERM`; that is not a name UnixLib looks at.)

**Settings and details.** The command to run is the Conf's `CONF_remote_cmd` (the SSH panel's "Remote command", unused by a
session of this kind), with a Connection > Local panel (`add_local_panel`, `roconfig.c`). It is `STR_AMBI`: `conf_get_str` on it is
an assertion failure, which here is a modal error box that stops the whole desktop (see below). The task window's memory is
4096K, or `PuTTY$LocalSlot`, and never more than the free memory leaves (`Wimp_SlotSize` with -1, -1 reads it). A new local
terminal from the icon bar or the command line takes Latin-1 for the received text unless the Default Settings say otherwise
(RISC OS programs write the RISC OS alphabet). Special command has Interrupt (an Escape character) and Quit task window
(Morite). The exit code is always 0 (the module does not say how the task ended). The terminal being behind (`seat_output`
returning more than 4096 bytes) holds back the acknowledgement of an Output message until `unthrottle` says it has caught up.
For tests: `PuTTY$LocalType` is typed into a new local terminal as it starts (or the contents of the file named after an `@`;
`\r \n \e \t \xHH \\`, and `\p` waits 1.5 seconds), and `PuTTYlocal$Trace` names a file that gets a line for the start, the
memory figures and every message from the module. `tests/ro/xtest.c` and `inecho.c` need UnixLib, so they are built `-static` and
turned into AIF images with `elf2aif` (a UnixLib program linked the default way is a dynamically linked ELF that RISC OS
cannot run; it died with "undefined instruction" at the first word of its data).

**Quitting.** Closing a local terminal window (after "Close session?") and quitting PuTTY both end the task window: the
backend sends Morite from `local_free`, and `cleanup_exit` calls `rolocal_shutdown` for the ones still open (and puts the
`UnixEnv$` variables back). Checked by starting two local terminals, listing the tasks (`tests/ro/tasks.c`: the program
shows as "PuTTY", each task window as "PuTTY local", 4 MB each), sending PuTTY Message_Quit with `wimprun`'s `quit`
command (its task handle is in `PuTTYlocal$Trace` as `-task`) and listing again: all three tasks were gone and
`Show UnixEnv$*` printed nothing.

**Tests.** `tools/localtest.py` (setup, run `wimprun` on the script it writes, check) is the regression test: five cases
(prompt, BASIC, xtest, inecho, a command) that type into a local terminal by themselves and compare PuTTY's own session log
with what a terminal ought to have been sent. An Obey file cannot drive it (`WimpTask` lines in one wait until it ends, so
every session sees the last value of `PuTTY$LocalType`), hence `tests/ro/wimprun.c`, a desktop task with waits.
`--settings`, `--settings-mid` and `--panel Path` on the command line open the Settings dialogue (as for a new session or
as Change settings, on a given panel) without a mouse, so that a screen dump can show it.

Not done: the cursor keys at the `*` prompt (it wants RISC OS key codes, a program wants escape sequences, and there is no way to
tell which is reading), per-window `TERM`/size (the variables are global), the exit status, and answering
`Message_TaskWindowNewTask` so that other programs' task windows could open here.

## Things that went wrong, and why

- **The scroll bar arrows did nothing in the terminal and file windows (0.01 to 0.09, found while testing 180 dpi modes).**
  A `Scroll_Request` (the Wimp's event 10, for a window made with `WF_SCROLL_REQUEST`) is the Open_Window_Request's eight
  words - window, visible area, scroll x and y, the window to go behind - followed by two words: the **x direction at
  +32 (word 8) and the y direction at +36 (word 9)**: -2 page, -1 step, +1 step, +2 page, with up and right positive.
  We read +40, the word after, which is whatever the last event happened to leave in the (reused) event block; the code
  comment said the block had a flags word before the directions, which is the layout of `Wimp_GetWindowState`'s block,
  not of this event. Nothing caught it because the mouse wheel does not come this way - this Wimp turns a wheel notch
  into an ordinary Open_Window_Request with a new scroll offset, which the terminal already handled (two lines to a
  notch) - and the scroll bar's thumb, which also arrives as an Open_Window_Request. The window's arrows and the clicks
  in the track (a page) were the only users and each moved the view by nothing at all. Found by tracing the block
  (`ro_trace`) after the new horizontal bar of the file window did not respond.

- **Squashed windows in a 180 dpi mode (0.09, fixed in 0.10).** The dialogues, the configuration window and the file
  window multiplied their pixel layouts by the mode's eigen factors, which was right in the 90 dpi mode they were drawn
  in and wrong in one with a pixel per OS unit, where the desktop font is as big as ever and the windows were half
  size. The fix is a constant, `UI_PX` (see "Screen modes: design pixels"). It went unnoticed because every test ran
  in one mode: the emulator's default 1600x1200 at 2 OS units to the pixel.

- **The terminal's text in the wrong size after a screen mode change (0.01 to 0.10, found from a user's report).** See
  "Changing the mode while the program runs".
- **The local terminal "hung" the desktop at its first start (0.11 development).** `conf_get_str(conf, CONF_remote_cmd)`:
  `remote_cmd` is `STR_AMBI`, so the call asserts, `modalfatalbox` shows the Wimp's error box, and the headless emulator
  has no one to click it - as the same mistake with `CONF_username` did before. It looked like a deadlock: the host command
  channel timed out, the emulator used little CPU, and the registers (`riscos_debug_registers`) showed the desktop's error box
  loop (`OS_Byte 129` calls and screen coordinates of a box 740 units wide in R6 to R9). What found it: `ro_trace` lines
  along the start-up path (the last line written was the one before the failing call), then a breakpoint at the function from
  the link map (`-Wl,-Map`; a static function is not in the map, but its offset in the object file added to the object's
  `.text` address is) and breakpoints after each call inside it, which showed that the CPU never came back from
  `conf_get_str`. Reading `conf.h` for the key's type took a minute.
- **A second command line for a running copy was misread (0.09, 0.10; found while testing the local terminal).** Starting
  `!PuTTY` again with a host name hands the command line to the copy that is running (see "One copy at a time"), which
  parsed it with the same `cmdline.c` it had already used once - and that file keeps state in static variables for the life
  of the process: the flag that says a host name has been seen (so the second command line's host name was taken for a
  *port*, nothing was started and the usage box came up), the flag for a loaded session, and the arguments saved for later
  (which point into the argument list that was freed after the first line, and are never run if the line was refused).
  `cmdline_reset()` (`patches/cmdline.patch`, compiled from a patched copy in `build/gen` and linked ahead of the library's
  `cmdline.o`, which the linker then leaves out; plink and the others keep the library's) clears them, and
  `start_from_command_line` calls it first. The `-pw` password is deliberately not cleared: it belongs to a session that
  may still be logging in. It also exposed that **the `-pw` password was offered to every session the process started
  afterwards**, including ones made from the Connect box for other hosts (`cmdline_password` is a static, and
  `cmdline_get_passwd_input` hands it to any session whose state has not tried it yet): now only a session started from the
  command line (`RoTerm.cmdline_pw`, set from `roterm_cmdline_session` around its `roterm_new`) asks for it; the rest prompt.
- The first GUI run "hung": the app had crashed in `Wimp_CreateIcon` (a validation pointer of -1 on an
  iconbar text icon makes the Wimp fault), and the Wimp's error box waits for a click that a headless
  emulator never gets. Describe then Postmortem in that box gives a C backtrace with function names.
- `CONF_username` is a string of unknown encoding: read it with `conf_get_str_ambi`, not `conf_get_str`.
- Changing `platform.h` rebuilds the whole core.
- `Wimp_DeleteIcon` takes a pointer to a block `{window, icon}`, not two registers. With the registers the
  call fails ("Bad pointer passed to Wimp in R1"), nothing is ever deleted, and the icons of every panel
  that has been shown pile up on top of each other. Even when it works it does not repaint what the icon
  covered: follow it with `Wimp_ForceRedraw`.
- The Wimp edits a writable icon itself and does not send Key_Pressed for the characters typed into it,
  so a dialogue cannot learn about edits as they happen. The configuration dialogue hands every text field
  to its PuTTY handler (`sync_fields`) before it acts on a click, a change of panel, Tab or Return; without
  that, what was typed never reached the settings.
- Tracing a Wimp problem: append to a file on HostFS from the code (`fopen("HostFS::HostFS.$.x", "a")`),
  log SWI results, and read the file from the host. That found both of the above in minutes.

- The guest's command channel (`riscos_run`) is one line at a time: a multi-line command wedges it. A test
  that can reach a passphrase prompt waits for a key press for ever (it cost half an hour once): run such
  commands with standard input redirected from an empty file, so that the prompt fails and is logged.
- `{ >> file }` appends only to a file that exists; `Copy` does not overwrite an existing file (`X Delete`
  first); RISC OS file names are not case sensitive (`sf_l` and `sf_L` were one file); after `Dir x` a
  relative name in a redirection no longer means what it did.

## The key generator (`!PuTTYgen`)

A separate Wimp task (`rogen.c` is its `main`, `rokeygen.c` the window, `Makefile` target `pgen`; the application
directory `app/!PuTTYgen`, whose icon is a golden key drawn by `tools/mksprites.py key`). It has to be separate:
upstream defines the symbol `deliberate_symbol_clash` in both `keygen/mpunsafe.c` and `ssh/kex2-client.c`, so that the
non-constant-time arithmetic of key generation can never be linked into a program that makes SSH connections - and the
terminal application is one. (The same libputty.a serves both; the link is what differs.) PuTTY's icon bar menu has
Key generator..., which `Filer_Run`s `<PuTTYgen$Dir>` (set by !Boot once the Filer has seen it) or else
`<PuTTY$Dir>.^.!PuTTYgen`. PuTTYgen is single-instance: a second copy broadcasts `MSG_GEN_PING` (0x4B7C3), waits a
third of a second for `MSG_GEN_PONG`, and exits; the first brings its window to the front. (Null events must be unmasked
during that wait: with them masked, `Wimp_PollIdle` never returns and the second copy hung at start-up.)

The window follows windows/puttygen.c: key type radios (RSA, DSA, ECDSA, EdDSA) and one size field (bits for RSA and
DSA; 256, 384 or 521 for ECDSA; 255 or 448 for EdDSA - the same numbers `puttygen -b` takes), Generate / Cancel with a
progress bar, then the key's public key (OpenSSH one-line form, as long as the field allows; Copy public key puts all
of it on the clipboard), fingerprint (SHA256, or MD5 from the window menu), editable comment, passphrase twice, and
Load / Copy public key / Save public key / Save private key. The window menu adds Export (OpenSSH key, OpenSSH new
format, ssh.com key, RFC 4716 public key) and the fingerprint choice. Short keys (under 2048 bits) are confirmed,
under 256 refused; an unsaved key is confirmed before it is replaced, before the window is closed, and before the
program quits (also on the desktop's PreQuit); a private key is only saved when the two passphrases match and a missing
passphrase has been confirmed. Loading uses `key_type`, `ppk_encrypted_f`/`import_encrypted`, then `ppk_load_f` or
`import_ssh2`, with a passphrase box that repeats on a wrong passphrase; a key in a foreign format is imported with
the notice Windows PuTTYgen gives. SSH-1 keys are left to the puttygen command.

Randomness. With the CryptRandom module (see Entropy) the generator is seeded as the puttygen command does: 32 bytes
from `ro_cryptrandom_bytes` into `random_reseed` (the PRNG's reseed mixes in its previous key, it does not replace it),
and the global PRNG is held with `random_ref` for as long as the window is open. Without the module the person has to
supply it, as in PuTTYgen on every other system: `collect_mouse` samples the pointer while it is over the window and
the time (`OS_Mouse`, once per centisecond at most, 2 bits counted per sample), until `min(bits, random_seed_bits())`
bits are in, then `random_reseed`s with them. Set the system variable `PuTTY$ForceMouseEntropy` to use that path on a
machine that has the module (for testing). The jitter-only PRNG is never used alone for a private key.

Making a key is one long function call that cannot return early, so the progress callbacks (`ProgressReceiver`, a port
of the Windows one: phases, expected-attempt maths) call `roapp_yield()`, which gives the rest of the desktop a turn,
and Cancel (or Quit, or the close icon) sets a flag the callback turns into a `longjmp` out of the generator - the
half-made key (a few kilobytes) is lost, which is the price of not forking a process. Timings in RPCEmu: Ed25519 under
a second, DSA-1024 about 10 s, RSA-2048 1-2 minutes, RSA-3072 ~100 s.

## Long operations and the poll loop (`roapp_yield`, `roapp_wait`)

`roapp.c`'s `poll_once(mode)` is the main loop's body (run PuTTY's sockets, timers and callbacks, give the Wimp a turn,
dispatch, then each window's idle job) and is also what a long operation calls to keep the desktop alive:
`roapp_yield()` (every ~2 cs at most) for work that is computing, `roapp_wait()` for a wait on the network (the Wimp
may sleep up to two centiseconds, none if the network had something to say). Rules learnt the hard way:
- **The Wimp returns one event per poll, and redraw requests come ahead of mouse clicks.** A progress bar that asks
  for a redraw before every yield therefore starves the mouse: a single poll per yield never reached the click on
  Cancel. So a yield polls repeatedly until the Wimp reports a null event (at most 16 times).
- Each poll uses its own event block (an outer `dispatch` may still be using its block when the nested poll runs).
- Only one long operation runs at a time: `roapp_op_begin()` says no if one is running (the SFTP client keeps its table
  of outstanding requests in a global, so there can only be one connection in use, and key generation shares the rule).
- A yield is not re-entered, so a handler that starts something long inside another long operation just runs it
  without yielding; nothing in the program does that.
- The main loop must keep waking while there is something to watch that the Wimp does not report (a pointer to sample,
  a network connection that may drop): each such window has an `..._idle_wanted()` and `..._idle()` hook. The file
  window's first version slept until the next click after the server went away, because `rofiles_live()` turned false
  once the connection died and so stopped asking for null events before the loss had been reported.

## Saving by dragging (`rosave.c`)

The Wimp's data transfer protocol, from our side as the sender: on the end of a drag (`Wimp_DragBox` type 5, or
`DragASprite_Start` when there is a sprite to carry) we send `Message_DataSave` (recorded, to the window and icon under
the pointer) with the leaf name, estimated size and filetype; the receiver replies `Message_DataSaveAck` with a full
pathname; we write the file there, and if the ack's size word is -1 - a scrap file, which is what an application asks
for - send `Message_DataLoad`; if it is anything else (the Filer's directory displays answer that way) the file is
simply in its place and nothing more is sent. Checked in RPCEmu with a Filer directory display and with !Edit's icon
(which then opens the file). A message nobody answers comes back as event 19 and the drag is simply over. Several
files can be dragged at once: from the done callback of one, `rosave_drop_another` sends the next DataSave to the same
place. The "save as" box built on this (icon, name, Cancel, Save) opens at the pointer, saves to a typed whole
pathname if there is one (a bare leaf is refused with the usual advice; an existing file is confirmed), and is used for
the key files and for a double-clicked remote file. A real destination named in a DataSaveAck that already holds a file
is confirmed too ("The file ... already exists": Overwrite or Cancel, `ask_overwrite`; the question is asynchronous, so
`xf.asking` keeps another drag out meanwhile); a scrap file (size word -1) is never asked about. The Filer itself does
not ask. `DI_FILE` in `rodlg` is the draggable icon (an indirected sprite
icon of button type "click/drag"); a drag arrives at the dialogue's callback as `BTN_DRAG_SELECT`.

## The file transfer window (`rofiles.c`, `roftp.c`, `rofile.c`)

`roftp.c` is an SFTP connection: it makes a Conf for SFTP in the way psftp's `psftp_connect` does (SSH-2 only, no
forwarding, subsystem `sftp` with the `sftp-server` fallback), runs a Backend with a Seat of its own whose prompts and
host-key questions are the same boxes the terminal uses (`roseat.c`'s `roseat_confirm_host_key` /
`roseat_confirm_yesno`, and a new `ro_prompt` in `rodialog.c` for passwords and keyboard-interactive questions), and
implements the three hooks upstream's `ssh/sftp.c` wants (`sftp_senddata`, `sftp_recvdata`, `sftp_sendbuffer`) so that
`sftp_recvdata` waits with `roapp_wait()`. On that it offers synchronous operations - list a directory, stat, realpath,
mkdir, rename, delete, and download/upload with a progress callback that can cancel - the last two being psftp's
`sftp_get_file` / `sftp_put_file` loops (`xfer_download_*`, `xfer_upload_*`), with the local files of `rofile.c` (the file
layer split out of `sftp.c`, so psftp, pscp and the window share one set of name and filetype rules: `.`<->`/`, hard
spaces, `,xxx` suffixes, a filetype from the extension table or by sniffing, dates). A cancelled or failed download
removes the part-copied file; a cancel leaves the connection in step (a following download works).

`rofiles.c` is the window. It is a plain Wimp window whose rows are painted in the redraw loop with `Wimp_PlotIcon`
(sprites `directory`/`file_xxx` and the small variants if they exist, text columns for name, size, date,
permissions). **`Wimp_PlotIcon` takes coordinates relative to the work area's origin, not the screen's** - the first
version added the window's screen position and drew everything off to one side. The window's button type is 10, so
a click arrives as `BTN_CLICK_SELECT`/`ADJUST`/`MENU` (256/512/1024) at the press, a double click as the plain
`BTN_SELECT`..., and a drag as `BTN_DRAG_SELECT`. Select-click selects one row, Adjust toggles a row; a Select-click on a
selected row of a multi-selection waits (to see whether a drag follows - the Filer's behaviour) and collapses the
selection if the button is released without one (`rofiles_idle` looks at the pointer). A file dragged out goes through
`rosave_drag` (all the selected files, one after another); files dropped in arrive as `Message_DataLoad`, are
acknowledged, and queued as uploads to the directory shown or the directory row they were dropped on (a directory is
copied recursively); the queue is drained when no other operation is running. Everything runs inside the event that
asked for it with the desktop kept alive by the wait, so the window can be closed mid-operation (the operation is told
to stop, and the window goes when it has). Quitting with the connection open asks first, as for a session.
A connection that drops while the window is idle is noticed by `rofiles_idle` (title "disconnected" and a notice).

The window keeps the whole listing (`fw.all`, which owns the names) and a view of it (`fw.ent`, shallow copies of the entries
shown, rebuilt by `rebuild_view`) so that hiding dot files and sorting - by name, by size (biggest first) or by date (newest
first), directories always first - need no new request to the server. Changing either clears the selection and scrolls to
the top. The two choices are statics (`pref_hide_dots`, `pref_sort`), so they last until the program quits and apply to the
next window.

The copying that does not go through a save box asks before it replaces a file (`ask_replace`, `confirm_local`): the
files of "Copy to...", of a directory tree downloaded, and everything uploaded (`ft_stat` finds the remote file). The
box says what is there and what would replace it (size and date of each) and offers Replace, Replace all (no more
questions for that lot of copying: `ow.all`, reset when an operation begins and at the start of a queue of dropped
files), Skip and Cancel; the default and Escape are Cancel. Unlike PuTTY's own questions it is synchronous: it is put
up with `ro_msgbox` and waited for with `roapp_wait()` inside the operation, which keeps every other operation out
(`op_begin`) and the desktop going; closing the window meanwhile drops the box (`ro_msgbox_drop_owner`) and cancels.
Directories that exist are merged into without asking.

The Connect box serves both uses: its File transfer button (or the icon bar's File transfer... item) opens a window
for the same host, port, login name and key; it asks for the password or host-key confirmation in boxes as a terminal
would. Only one window is open at a time.

Checked in RPCEmu against Uppity (its SFTP server serves the host's real file system): listing and navigation
(parent, double-click, Go to, Home, Refresh, a symbolic link shown as the directory it points to), downloads by drag to
a Filer window (text and binary, a 3 MB file byte-identical, `,fff` and `,aff` suffixes becoming types, dates kept),
by double-click and a typed pathname, onto !Edit's icon, of several selected files at once and of whole directories
(Copy to...), uploads by drag (a text file, a Data file, a `,aff` file, onto a directory row, byte-identical),
New directory, Rename, Delete with its confirmation, Cancel of a 95 MB download (no partial file left, the connection
still good), closing the window during a transfer and during authentication, keyboard-interactive login through
prompt boxes, a refused connection, the server killed while idle, Quit with a live connection, and the console tools
re-run afterwards (`tools/sftptest.py`: 43 checks, none failed).

## One copy at a time (`another_copy_running` in `roapp.c`)

A second `!PuTTY` would be a second Wimp task with its own icon, its own windows and (now) its own key agent, which is
not what anyone double-clicking the application twice wants. So at start-up, straight after `Wimp_Initialise` and before
anything else is made, a copy broadcasts `MSG_PUTTY_PING` (0x4B7C1, a private user message; the numbers and the magic word
were already in `rowimp.h`) carrying its command line - the arguments after the program name as NUL-ended strings - and waits
a third of a second, with null events allowed (they are what ends the wait), for `MSG_PUTTY_PONG`. A running copy answers
at once and does what the command line says (`other_copy_started`: `start_from_command_line` on the received arguments, or
the Connect box if there were none), so the new session or the box appears in the copy that is already there, and the copy
that asked closes its task and exits. Without an answer it carries on as the first copy. A command line too long for one
message (about 220 bytes) is not sent and that copy runs on its own. A broadcast that nobody answers is never bounced back
by the Wimp, so "nobody is there" can only be learned by waiting, which costs a third of a second at every cold start. A
copy that is busy (a key being decrypted keeps the desktop frozen for ten seconds) will not answer in time, and a second
copy then starts anyway. `PuTTY$AllowSeveral` turns the check off for tests. !PuTTYgen does the same with its own pair of
messages.

## The key agent (`roagent.c`)

Upstream's `pageant.c` is the agent proper (the key store, the SSH agent protocol, the client-side helpers
`pageant_add_keyfile`, `pageant_enum_keys`, `pageant_delete_key`); the Unix and Windows ports wrap it in a socket or a
window message. Here it runs inside the Wimp program, so PuTTY's own connections talk to it directly:
`roagent.c` defines the four entry points the SSH code expects of a platform's agent client - `agent_exists`
(true when it holds keys, so an empty agent does not clutter the log with "Pageant has no keys"), `agent_query`,
`agent_cancel_query`, and `agent_connect` (an error socket, so agent forwarding falls back to upstream's message-at-a-time
`agentf.c`). `pageant_handle_msg` answers through `got_response` some time later, from a toplevel callback, so a query
is asynchronous: `agent_query` returns a pending query and a callback of ours (queued from `got_response`, never run
inside it, because pageant's own clean-up after the response still touches the client) calls the SSH code back.
`ask_passphrase` is refused: keys are held decrypted, so no request waits for the user.

`pageant.c` is compiled as it stands from the upstream tree, with `-Drandom_read=pageant_random_read_unused` (it defines a
`random_read` that is fatal, for the stand-alone Pageant, which clashes with `sshrand.c`'s real one), and `aqsync.c` joins the
core. `pageant_init()` makes the agent "local", and the client helpers then act directly on its key store, in the same
call (that is how the Keys window lists, adds and removes keys). `keylist_update()` is the hook pageant calls when the key
list changes; ours refreshes the window.

The window is a `rodlg` dialogue: six rows of tick boxes (a row's label is the key type and size, then the comment) that
are hidden (`dlg_set_hidden`, which sets the Wimp's "deleted" icon flag - the Wimp neither draws nor notices such an icon)
when there is no key for them, paging buttons for more than six, the fingerprint of the key last ticked in a field, and
Add key..., Remove, Remove all, Start-up list... and Close. Adding is `pageant_add_keyfile(fn, NULL, ...)`; it says
NEED_PP for an encrypted key, then a passphrase box (`ro_prompt`, secret) and a second call with the passphrase; a wrong
one comes back as NEED_PP again and the box is shown again with "That passphrase was wrong". Passphrases that worked are
kept by pageant for the next key of the same go, and forgotten when it ends (`pageant_forget_passphrases`). Decrypting a
`.ppk` version 3 key with Argon2 takes a while with the desktop frozen (about ten seconds in RPCEmu, with 34 passes of
8 MB) and needs the memory, which `!Run`'s WimpSlot allows. A key file dropped on the window, or on the icon bar icon when
it is a private key, is added (`roagent_message`).

The start-up list is `<Choices$Write>.PuTTY.AgentKeys`, a text file with one pathname to a line (`#` starts a comment),
read when the program starts: after the Connect box has opened if no session was asked for on the command line (so a
passphrase box is above it, not under it), and before the session if one was. The keys are added one at a time - each add
ends with a queued callback that starts the next - so a passphrase box never competes with another.

Checked in RPCEmu against Uppity: add by typed path, by drop on the window, an encrypted key with a wrong passphrase and
then the right one, the list and the fingerprint (equal to `puttygen -l`), login with no key file configured
("Authenticating with public key ... from agent"), agent forwarding (Uppity's agent socket: `ssh-add -l` on the server
lists the key, and `ssh-keygen -Y sign` through it makes a signature the host verifies), and the start-up list (a key
needing a passphrase, a key already loaded, a missing file).

## Not done yet

Tests on a real Pi, and a scrolling category tree (the accordion is enough for PuTTY 0.85's panels). The file transfer
window has one connection at a time and no resume of an interrupted copy (psftp's `reget`/`reput` do); symbolic links
to files are copied as the files they point to; permissions are shown but not edited. The key generator has no SSH-1
keys (the puttygen command does) and no certificate handling.
