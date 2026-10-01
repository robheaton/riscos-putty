/*
 * cmake.h - hand-written configuration for the RISC OS build.
 *
 * Upstream generates this from cmake/cmake.h.in; we have no cmake step, so the
 * answers are fixed here.  (Every HAVE_xxx flag that is 0 leaves out a feature
 * RISC OS cannot provide: IPv6, GSSAPI, hardware crypto, POSIX extras.)
 */
#define NO_IPV6
#define NO_GSSAPI

#define HAVE_COUNTOF 0
#define HAVE_NO_STDINT_H 0
#define HAVE_STRTOUMAX 1

/* None of the Windows/Unix-only facilities exist here. */
#define HAVE_WINRESRC_H 0
#define HAVE_WINRES_H 0
#define HAVE_WIN_H 0
#define HAVE_AFUNIX_H 0
#define HAVE_GCP_RESULTSW 0
#define HAVE_ADDDLLDIRECTORY 0
#define HAVE_GETNAMEDPIPECLIENTPROCESSID 0
#define HAVE_SETDEFAULTDLLDIRECTORIES 0
#define HAVE_DWMAPI_H 0
#define HAVE_ASM_HWCAP_H 0
#define HAVE_SYS_AUXV_H 0
#define HAVE_SYS_SYSCTL_H 0
#define HAVE_SYS_TYPES_H 1
#define HAVE_GLOB_H 0
#define HAVE_UTMP_H 0
#define HAVE_FUTIMES 0
#define HAVE_GETADDRINFO 0
#define HAVE_POSIX_OPENPT 0
#define HAVE_PTSNAME 0
#define HAVE_SETRESUID 0
#define HAVE_SETRESGID 0
#define HAVE_STRSIGNAL 0
#define HAVE_UPDWTMPX 0
#define HAVE_FSTATAT 0
#define HAVE_DIRFD 0
#define HAVE_SETPWENT 0
#define HAVE_ENDPWENT 0
#define HAVE_GETAUXVAL 0
#define HAVE_ELF_AUX_INFO 0
#define HAVE_SYSCTLBYNAME 0
#define HAVE_CLOCK_MONOTONIC 0
#define HAVE_CLOCK_GETTIME 0
#define HAVE_SO_PEERCRED 0
#define HAVE_NULLARY_SETPGRP 0
#define HAVE_BINARY_SETPGRP 0
#define HAVE_PANGO_FONT_FAMILY_IS_MONOSPACE 0
#define HAVE_PANGO_FONT_MAP_LIST_FAMILIES 0
#define HAVE_G_APPLICATION_DEFAULT_FLAGS 0

/* No hardware-accelerated crypto: ARMv4 build, software implementations only. */
#define HAVE_AES_NI 0
#define HAVE_SHA_NI 0
#define HAVE_SHAINTRIN_H 0
#define HAVE_CLMUL 0
#define HAVE_NEON_CRYPTO 0
#define HAVE_NEON_PMULL 0
#define HAVE_NEON_VADDQ_P128 0
#define HAVE_NEON_SHA512 0
#define HAVE_NEON_SHA512_INTRINSICS 0
#define USE_ARM64_NEON_H 0
#define HAVE_ARM_DIT 0
