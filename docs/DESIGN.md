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

Not yet tested: SSH-1 (no SSH-1 server to test against), X11 and agent forwarding (not applicable), a real Pi.

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
`rodlg.c`/`rodialog.c` (dialogues and message boxes), `romenu.c`, `rowimp.[ch]`. `rowimp`, `rodlg`, `romenu` and the clipboard protocol are adapted from riscos-vncclient
(same author).

The icon bar menu follows the usual RISC OS pattern: Info (first), Help..., the application's entries (New
session..., Sessions >), Quit. **Info** opens the standard "About this program" window. It is not a
message box: it is a window made by `rodlg.c` in its `DLG_MENU` style (a title bar and nothing else) and used
directly as the submenu of the Info item (`menu_set_window`: the item's submenu pointer is the window handle,
which the Wimp opens beside the item and closes with the menu). The layout was measured from the ROM's !Edit
and !Paint (Toolbox ProgInfo) in the emulator: a 320 pixel wide window, labels 76 wide and right-aligned, sunken
read-only fields (`DI_DISPLAY`, validation `R2`) 26 high and 30 apart; the title is "About this program" and the
rows are Name, Purpose, Author, Port, Licence and Version ("0.04 (01-Oct-26), PuTTY 0.85", the date from
`RO_PORT_DATE` in `roversion.h`). The terminal's window menu has the same Info item at its foot. **Sessions**
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
  `OS_Byte 129` and INKEY numbers (the numbers key events use do not work). Page Up/Down arrive as
  Shift-Up/Shift-Down codes and are told apart by testing whether the arrow key is physically down.
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

Measured in RPCEmu (RISC OS 5.30): the screen is 1600x1200 with one pixel = 2 OS units; Corpus.Medium
is fixed pitch with an advance of 7200 millipoints at 12 pt (18 OS units; 400 millipoints per OS
unit). A screenshot without VNC: `*ScreenSave file`, then `tools/sprite2png.py`.

## Things that went wrong, and why

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
the key files and for a double-clicked remote file. `DI_FILE` in `rodlg` is the draggable icon (an indirected sprite
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

## Not done yet

Tests on a real Pi, and a scrolling category tree (the accordion is enough for PuTTY 0.85's panels). The file transfer
window has one connection at a time and no resume of an interrupted copy (psftp's `reget`/`reput` do); symbolic links
to files are copied as the files they point to; permissions are shown but not edited. The key generator has no SSH-1
keys (the puttygen command does) and no certificate handling.
