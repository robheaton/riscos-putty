# riscos-putty

A native RISC OS port of the PuTTY SSH/Telnet client (upstream 0.85).

**Status (2026-10-01): the PuTTY core runs on RISC OS.** A console `plink` builds
and works: in RPCEmu it connects to an SSH server, performs the hybrid NTRU
Prime / Curve25519 key exchange, checks the host key, logs in with an ed25519
key and runs a remote command. The Wimp terminal window - the real goal - is not
started yet.

## Goal

First milestone: a Wimp terminal window plus a simple Connect dialog (host, port,
protocol, username, saved sessions, password and key login). PuTTY's full tabbed
configuration dialog follows. The console `plink` is a bring-up step.

## Building

Needs GCCSDK (`arm-unknown-riscos-gcc` 4.7), Perl and CMake (the last two only to
generate a couple of source files). Same toolchain settings as `riscos-vncclient`:
`-mlibscl -march=armv4`, no runtime modules, runs in RPCEmu and on a Pi.

    make core      # compile the portable PuTTY core (244 files) into build/ro/libputty.a
    make plink     # link build/ro/plink,ff8
    make V=1 ...   # show the full compiler commands

PuTTY is compiled straight from the unpacked upstream tree (`upstream/putty-0.85`,
untar `upstream/putty-0.85.tar.gz` there; `upstream/verify.sh` checks the tarball's
checksum and PGP signature). The source lists in the Makefile are transcribed from
upstream's `CMakeLists.txt` files. `src/ro/cmake.h` replaces the generated config
header.

## Testing

Upstream ships a test SSH server, Uppity. Build the native tools once, then:

    cmake -S upstream/putty-0.85 -B build/host -DPUTTY_GTK_VERSION=0
    make -C build/host
    tools/testserver.sh      # starts Uppity on 127.0.0.1:2222 with a test key
    tools/deploy.sh          # copies plink and the test key into the emulator's HostFS

Inside RPCEmu the host is 10.10.10.2; the script prints the exact `plink` command
to run. Set the system variable `PuTTY$Timeout` (seconds) so a stuck test gives up
instead of wedging the emulator, and `PuTTY$Trace` (a filename) to log socket events.

## Layout

- `upstream/` - pristine PuTTY 0.85 tarball, signature, checksums, public keys, `verify.sh`.
- `src/ro/` - the RISC OS platform layer:
  `net.c` (sockets over the Internet module), `storage.c` (sessions, host keys, seed under `Choices:`),
  `noise.c` (entropy), `console.c`, `plink.c`, `unicode.c`, `util.c`, `stubs.c`, `compat.c`, `platform.h`, `cmake.h`.
- `tests/ro/rtest.c` - diagnostic that shows what the Resolver SWIs return.
- `tools/` - `testserver.sh`, `deploy.sh`.
- `docs/DESIGN.md` - decisions and findings.

## Next steps

1. Known-answer crypto tests on ARM compared with the host build.
2. The Wimp terminal window (`TermWin` and `Seat` vtables, modelled on `unix/window.c`) and Connect dialog.
3. Then the generic configuration dialog, and psftp/pscp/puttygen.

## Licence

The same as PuTTY's: the MIT licence. See `LICENSE`, which carries the copyright
line for the new code and, in full, PuTTY's own licence text.
