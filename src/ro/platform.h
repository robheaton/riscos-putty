/*
 * platform.h: RISC OS-specific inter-module stuff.
 *
 * Modelled on upstream's unix/platform.h, with the GTK, X11, pty and signal
 * parts removed.
 */

#ifndef PUTTY_RISCOS_PLATFORM_H
#define PUTTY_RISCOS_PLATFORM_H

#include <stdio.h>
#include <stdint.h>
#include <strings.h>                   /* strcasecmp, strncasecmp */
#include <sys/types.h>
#include "charset.h"

#define BUILDINFO_PLATFORM "RISC OS"

struct Filename {
    char *path;                        /* a native RISC OS pathname */
};
FILE *f_open(const struct Filename *, char const *, bool);

struct FontSpec {
    char *name;    /* may be "" to indicate no selected font at all */
};
struct FontSpec *fontspec_new(const char *name);

#define BROKEN_PIPE_ERROR_CODE EPIPE   /* used in ssh/sharing.c */

/*
 * The Wimp sends a single click and then a double click when a button is
 * pressed for the second time, as X does, so the front end sends MA_CLICK
 * _and_ MA_2CLK / MA_3CLK.
 */
#define MULTICLICK_ONLY_EVENT 0

/* There is no context help to hook up. */
typedef void *HelpCtx;
#define NULL_HELPCTX ((HelpCtx)NULL)
#define HELPCTX(x) NULL

/* Selection data placed on the clipboard must not be NUL-terminated. */
#define SELECTION_NUL_TERMINATED 0

/* RISC OS text uses a bare LF as its line terminator. */
#define SEL_NL { 10 }

/* Wraparound millisecond timer, from OS_ReadMonotonicTime. */
unsigned long getticks(void);
#define GETTICKCOUNT getticks
#define TICKSPERSEC    1000
#define CURSORBLINK     450

#define WCHAR wchar_t
#define BYTE unsigned char

/*
 * RISC OS has exactly one clipboard: the global one.  As in PuTTY for Windows,
 * selecting text with the mouse copies it there (so it can be pasted into any
 * other application), and Shift-Insert pastes it.  The Wimp has no middle button,
 * so a mouse paste is a menu item; the "implicit" mouse paste is the last text
 * selected in the terminal.
 */
#define PLATFORM_CLIPBOARDS(X)                            \
    X(CLIP_CLIPBOARD, "RISC OS clipboard")                \
    X(CLIP_CUSTOM_1, "<custom#1>")                        \
    X(CLIP_CUSTOM_2, "<custom#2>")                        \
    X(CLIP_CUSTOM_3, "<custom#3>")                        \
    /* end of list */

#define MOUSE_SELECT_CLIPBOARD CLIP_NULL
#define MOUSE_PASTE_CLIPBOARD CLIP_LOCAL
#define CLIPNAME_IMPLICIT "Last selected text"
#define CLIPNAME_EXPLICIT "System clipboard"
#define CLIPNAME_EXPLICIT_OBJECT "system clipboard"
#define CLIPUI_DEFAULT_AUTOCOPY true
#define CLIPUI_DEFAULT_MOUSE CLIPUI_IMPLICIT
#define CLIPUI_DEFAULT_INS CLIPUI_EXPLICIT
#define MENU_CLIPBOARD CLIP_CLIPBOARD
#define COPYALL_CLIPBOARDS CLIP_CLIPBOARD

/*
 * DEFAULT_CODEPAGE is a special value that means "the system's own
 * character set", which on RISC OS is the Latin-1 alphabet.
 */
#define DEFAULT_CODEPAGE 0xFFFF
#define CP_UTF8 CS_UTF8
#define CP_437 CS_CP437                /* used for test suites */
#define CP_ISO8859_1 CS_ISO8859_1      /* used for test suites */

#define strnicmp strncasecmp
#define stricmp strcasecmp

/*
 * Exports from unicode.c.
 */
bool init_ucs(struct unicode_data *ucsdata, char *line_codepage,
              bool utf8_override, int font_charset, int vtmode);

/*
 * The interface used by uxsel.c (upstream's file-descriptor interchange,
 * used unchanged: our "file descriptors" are Internet module socket handles).
 */
typedef struct uxsel_id uxsel_id;
void uxsel_init(void);
typedef void (*uxsel_callback_fn)(int fd, int event);
void uxsel_set(int fd, int rwx, uxsel_callback_fn callback);
void uxsel_del(int fd);
enum { SELECT_R = 1, SELECT_W = 2, SELECT_X = 4 };
void select_result(int fd, int event);
int first_fd(int *state, int *rwx);
int next_fd(int *state, int *rwx);
/* Expected to be provided _to_ uxsel.c by the event loop. */
uxsel_id *uxsel_input_add(int fd, int rwx);  /* returns an id */
void uxsel_input_remove(uxsel_id *id);

/* Spare functions exported directly from network.c. */
void *sk_getxdmdata(Socket *sock, int *lenp);
int sk_net_get_fd(Socket *sock);
void plug_closing_errno(Plug *plug, int error);

SeatPromptResult make_spr_sw_abort_errno(const char *prefix, int errno_value);

/* Unix-style extras in cmdline_arg.c */
CmdlineArgList *cmdline_arg_list_from_argv(int argc, char **argv);

/* utils */
char *make_dir_path(const char *path, mode_t mode);

/* console.c: call at startup (does nothing here; kept for plink's sake) */
void stderr_tty_init(void);

#endif /* PUTTY_RISCOS_PLATFORM_H */
