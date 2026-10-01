/*
 * stubs.c: features of the Unix port that have no counterpart on RISC OS (yet).
 * Each of these is a deliberate "not supported" answer, not an omission.
 */

#include "putty.h"
#include "network.h"
#include "ssh.h"

/*
 * Connection sharing between PuTTY instances needs Unix-domain sockets (or
 * Windows named pipes).  Say there is nothing to share.
 */
int platform_ssh_share(const char *name, Conf *conf,
                       Plug *downplug, Plug *upplug, Socket **sock,
                       char **logtext, char **ds_err, char **us_err,
                       bool can_upstream, bool can_downstream)
{
    return SHARE_NONE;
}

void platform_ssh_share_cleanup(const char *name)
{
}

const bool share_can_be_downstream = false;
const bool share_can_be_upstream = false;

/*
 * X11 forwarding: there is no X server to forward to.
 */
SockAddr *platform_get_x11_unix_address(const char *path, int displaynum)
{
    return sk_nonamelookup("");        /* never usable: no Unix-domain sockets */
}

char *platform_get_x_display(void)
{
    return NULL;
}

/*
 * Local proxy commands ("run a program and talk to its stdin/stdout") need
 * pipes and subprocesses.
 */
Socket *platform_new_connection(SockAddr *addr, const char *hostname,
                                int port, bool privport,
                                bool oobinline, bool nodelay, bool keepalive,
                                Plug *plug, Conf *conf, Interactor *itr)
{
    if (conf_get_int(conf, CONF_proxy_type) == PROXY_CMD) {
        sk_addr_free(addr);
        return new_error_socket_fmt(
            plug, "Local proxy commands are not supported on RISC OS");
    }
    return NULL;                       /* an ordinary network connection */
}

/*
 * The "Pageant" agent has no RISC OS implementation, so there is nothing
 * to ask for keys; stubs/no-agent.c supplies the agent_* entry points.
 */

/*
 * X11 forwarding hooks used by x11disp.c: with no X server there is no
 * authority to look up and no Unix-domain socket to prefer.
 */
const bool platform_uses_x11_unix_by_default = false;

void platform_get_x11_auth(struct X11Display *display, Conf *conf)
{
}

/*
 * The "pre-connect command" feature runs a local program first.  We cannot, so
 * answer as a program that failed to start would: an error socket, and a
 * waiter that reports exit status 127 as soon as anyone asks to be told.
 * (PuTTY logs the status and goes on to connect, as it does for any exit.)
 */
struct SubprocessWaiter {
    SubprocessWaiterCallback cb;
    void *cbctx;
};

static void subproc_waiter_fire(void *vw)
{
    SubprocessWaiter *w = (SubprocessWaiter *)vw;
    if (w->cb)
        w->cb(w->cbctx, EXITTYPE_NORMAL, 127);
}

Socket *platform_start_subprocess(
    const char *cmd, Plug *plug, const char *pfx, SubprocessWaiter **waiter)
{
    if (waiter) {
        *waiter = snew(SubprocessWaiter);
        (*waiter)->cb = NULL;
        (*waiter)->cbctx = NULL;
    }
    return new_error_socket_fmt(
        plug, "Running local commands is not supported on RISC OS");
}

void subproc_waiter_set_callback(
    SubprocessWaiter *waiter, SubprocessWaiterCallback cb, void *cbctx)
{
    waiter->cb = cb;
    waiter->cbctx = cbctx;
    queue_toplevel_callback(subproc_waiter_fire, waiter);
}

void subproc_waiter_free(SubprocessWaiter *waiter)
{
    delete_callbacks_for_context(waiter);
    sfree(waiter);
}
