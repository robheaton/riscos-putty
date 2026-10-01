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

## Entropy (`noise.c`) - OPEN, security-relevant

Upstream reads `/dev/urandom`. RISC OS has no equivalent. `noise.c` uses the
CryptRandom module's `CryptRandom_Word` if it is loaded; otherwise it gathers timing
jitter and system state and mixes in the seed file, and `plink` warns that the result is
weak. The SWI is called through `CryptRandom_Word` only because it takes no pointer;
`CryptRandom_Block`/`_Byte` must not be used until their register interface is
confirmed. The RISC OS 5.31 BCM2835 kernel sources contain no entropy API. Options:
require CryptRand, read the Pi's hardware RNG registers, or ask the user to supply a
seed file made on a machine with a good source (PuTTY mixes `RandomSeed` in).

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

Not yet tested: SSH-1, Rlogin, X11 and agent forwarding (not applicable), SFTP/SCP (not built), a real Pi.

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
- The window's extent is at least as tall as the scrollback (and at least screen sized: the Wimp
  will not let a window be dragged bigger than its work area, and sends no request when the drag is
  clamped), but rows are always painted from the top of the visible area; every change of scroll
  offset is followed by a full redraw because the Wimp copies pixels when it changes. Side effect:
  while the scrollback is shorter than the screen the scroll bar thumb looks too small.
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
the colour chooser (three RGB fields) and font chooser (name, size, a menu of installed fonts) are small
`rodlg` dialogues, and a file dropped on a file field fills it in. The category tree is an accordion: only
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

## Not done yet

Window position and size memory, psftp/pscp/puttygen, a settled entropy source,
tests on a real Pi, a colour swatch in the colour chooser, a scrolling category tree (the accordion is enough
for PuTTY 0.85's panels).
