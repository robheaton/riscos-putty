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

Name lookup uses the blocking `Resolver_GetHostByName`, as UnixLib does. The
"multitasking" `Resolver_GetHost` (0x46001) was tried and is not understood: with R0 = 0
it answers R0 = -1, R1 = 0 on every call and never completes; any other R0 gives "Bad
parameters to Resolver SWI", and R0 = -1 faults. OSLib's header mentions an
`Event_Internet` sub-event 0x102 (DNS resolve complete), which may be how completion is
meant to be signalled. The asynchronous state machine in `net.c` (an address that is
"resolving", sockets that wait for it) is kept so that a working non-blocking lookup
can be dropped into `resolve_step()`.

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

Based on `unix/plink.c`. The keyboard is polled with `OS_Byte 129` (INKEY), so a
redirected stdin is not seen as EOF; raw/Telnet-style sessions therefore never finish on
their own (they need both ends to send EOF). Remote output is always stripped of control
characters when stdout is a terminal, because VDU codes can change the screen mode.
`PuTTY$Timeout` makes the program give up after N seconds, for tests.

## Verified so far (RPCEmu, RISC OS 5.30, StrongARM)

Against Uppity: SSH-2 with sntrup761x25519-sha512, ssh-ed25519 host key (checked with
`-hostkey`), AES-256-CTR, HMAC-SHA-256, public-key login from a `.ppk`, exec channel,
exit status. Unknown host key refused in batch mode; connection refused reported;
name lookup through the system resolver.

Not yet tested: password / keyboard-interactive login, RSA/ECDSA/DSA keys, ChaCha20,
AES-GCM, compression, port forwarding, large transfers, Telnet/Rlogin.
