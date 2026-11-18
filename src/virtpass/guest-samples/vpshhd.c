/*

 * vpshhd.c - the guest's SSH entry point: a listener that hands each TCP
 * connection to tinysshnoneauthd.
 *
 * tinysshd is an inetd-style server: it speaks the SSH transport on its own
 * stdin/stdout and expects somebody else to have accepted the connection. That
 * is the whole reason this file exists. It is not a protocol implementation and
 * it must never grow into one.
 *
 *   accept() -> fork() -> dup2(socket, 0), dup2(socket, 1) -> exec tinysshnoneauthd
 *
 * Why 127.0.0.1:7900 and not a UNIX socket: the guest's socket calls are
 * forwarded to the host one for one (src/core/rvvm_user.c, sys_bind/sys_listen/
 * sys_accept), so binding here binds on the host - the port is real, and the
 * ssh the user already has connects to it with nothing in between. -nonet
 * removes a virtual NIC, not this path: it is syscall forwarding, not a network.
 *
 * Three things are deliberate:
 *
 *   - The name it execs is `tinysshnoneauthd`, not `tinysshd`. tinysshd chooses
 *     its mode from basename(argv[0]) (tinysshd.c:34), so the mode is the argv[0]
 *     this file passes - not the file's name on disk. That is why the bundle
 *     carries one ELF and no links: none-auth is what makes
 *     `ssh -p 7900 127.0.0.1 ls` work with no user name and no key, the wsl
 *     behaviour.
 *
 *   - stderr goes to /dev/null. tinysshd logs to fd 2, and fd 1 is the SSH
 *     transport here: a log line written to the socket would be parsed as key
 *     exchange and break the session in a way that looks like a crypto bug.
 *
 *   - The host key is generated on first boot, not shipped in the bundle.
 *     tinysshd-makekey is a guest program (it cannot run on Windows), and a key
 *     baked into an image would be the same key everywhere. It lands in
 *     /etc/tinyssh/sshkeydir, on the writable layer, so it lives as long as the
 *     instance does.
 *
 * Usage: vpshhd [port]          (default 7900)
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define VP_SSH_PORT   7900
#define VP_SSH_ADDR   "127.0.0.1"
#define VP_SSH_KEYDIR "/etc/tinyssh/sshkeydir"
#define VP_SSH_KEY    VP_SSH_KEYDIR "/ed25519.pk"
#define VP_TINYSSHD   "/sbin/tinysshd"
#define VP_RUN_DIR    "/run/vpsessiond"
#define VP_AUTH_PUB   VP_RUN_DIR "/auth.pub"
#define VP_AUTH_KEYS  "/root/.ssh/authorized_keys"

/* Children are short-lived by construction (one SSH connection each), so the
 * only thing the listener has to do about them is not leave zombies behind. */
static void vp_reap(int sig)
{
    (void) sig;
    for (;;) {
        int   status = 0;
        pid_t pid    = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            return;
        }
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "vpshhd: session %ld killed by signal %d\n", (long) pid,
                    WTERMSIG(status));
        } else {
            fprintf(stderr, "vpshhd: session %ld exited %d\n", (long) pid,
                    WEXITSTATUS(status));
        }
    }
}

/* Auth mode: when the host drops <session mount>/auth.pub into the run
 * directory, sessions require the matching private key instead of admitting
 * everyone (tinysshd's mode is argv[0]-spelled; see vp_serve). The public key
 * is planted for the root account here, inside the guest, so the ownership
 * and permission bits tinysshd's checks demand are the real ones - a file
 * materialized on a Windows host cannot promise those. No auth.pub: dev
 * behaviour, none-auth, unchanged. Returns 1 when auth mode is on. */
static int vp_plant_authorized_keys(void)
{
    FILE *in = fopen(VP_AUTH_PUB, "r");
    FILE *out;
    char  line[4096];

    if (!in) {
        return 0;
    }
    fclose(in);
    mkdir("/root", 0755);
    mkdir("/root/.ssh", 0700);
    out = fopen(VP_AUTH_KEYS, "w");
    if (!out) {
        fprintf(stderr, "vpshhd: cannot write %s: %s\n", VP_AUTH_KEYS,
                strerror(errno));
        return 0;
    }
    in = fopen(VP_AUTH_PUB, "r");
    while (in && fgets(line, sizeof line, in)) {
        fputs(line, out);
    }
    if (in) {
        fclose(in);
    }
    fclose(out);
    chmod(VP_AUTH_KEYS, 0600);
    chmod("/root/.ssh", 0700);
    fprintf(stderr, "vpshhd: auth mode (public key from %s)\n", VP_AUTH_PUB);
    return 1;
}

/* Returns 0 when a host key is present, -1 when it could not be made. */
static int vp_ensure_hostkey(void)
{
    pid_t pid;
    int   status = 0;

    if (access(VP_SSH_KEY, F_OK) == 0) {
        return 0;
    }

    /* tinysshd-makekey creates the keydir itself and fails if it already
     * exists, so only the parent is made here. An attempt that died before
     * writing the key leaves the empty directory behind - remove it, or every
     * boot after the first failure would fail the same way. */
    mkdir("/etc/tinyssh", 0755);
    (void) rmdir(VP_SSH_KEYDIR);

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execl(VP_TINYSSHD, "tinysshd-makekey", VP_SSH_KEYDIR, (char *) 0);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return access(VP_SSH_KEY, F_OK) == 0 ? 0 : -1;
}

static int vp_listen(int port)
{
    struct sockaddr_in addr;
    int                fd  = -1;
    int                opt = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    /* A core that is restarted while a session's socket is in TIME_WAIT must be
     * able to take the port again immediately. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short) port);
    addr.sin_addr.s_addr = inet_addr(VP_SSH_ADDR);

    if (bind(fd, (struct sockaddr *) &addr, sizeof addr) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* One connection, one process. Returns only in the child, which becomes
 * tinysshnoneauthd; the parent closes its copy first. */
static void vp_serve(int listener, int conn)
{
    pid_t pid = fork();
    int   nul;
    int   vp_auth_mode = vp_plant_authorized_keys();

    if (pid < 0) {
        close(conn);
        return;
    }
    if (pid != 0) {
        /* The session owns the connection now; this closes the parent's
         * descriptor, not the child's. */
        close(conn);
        return;
    }

    close(listener);

    /* The session is its own process group with no controlling terminal;
     * tinysshd makes the pty when the client asks for one (pty-req). */
    setsid();

    if (dup2(conn, 0) < 0 || dup2(conn, 1) < 0) {
        _exit(127);
    }
    nul = open(VP_RUN_DIR "/tinysshd.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (nul >= 0) {
        dup2(nul, 2);
        if (nul > 2) {
            close(nul);
        }
    }
    if (conn > 2) {
        close(conn);
    }

    execl(VP_TINYSSHD, vp_auth_mode ? "tinysshd" : "tinysshnoneauthd", "-v",
          VP_SSH_KEYDIR, (char *) 0);
    _exit(127);
}

int main(int argc, char **argv)
{
    int port = VP_SSH_PORT;
    int listener;

    if (argc > 1 && argv[1] && argv[1][0]) {
        int v = atoi(argv[1]);
        if (v > 0 && v < 65536) {
            port = v;
        }
    }

    signal(SIGCHLD, vp_reap);
    signal(SIGPIPE, SIG_IGN);

    /* Diagnostics go to the host-visible mount: the guest's console is not
     * always reachable from whoever started the core, and "the port never
     * opened" with no reason is the worst failure to chase. */
    {
        int lg = open("/run/vpsessiond/vpshhd.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lg >= 0) {
            dup2(lg, 2);
            if (lg > 2) {
                close(lg);
            }
        }
    }

    if (vp_ensure_hostkey() != 0) {
        fprintf(stderr, "vpshhd: cannot create host key in %s\n", VP_SSH_KEYDIR);
        return 1;
    }

    listener = vp_listen(port);
    if (listener < 0) {
        fprintf(stderr, "vpshhd: cannot listen on %s:%d: %s\n", VP_SSH_ADDR, port,
                strerror(errno));
        return 1;
    }

    fprintf(stderr, "vpshhd: listening on %s:%d\n", VP_SSH_ADDR, port);

    for (;;) {
        int conn = accept(listener, 0, 0);
        if (conn < 0) {
            /* The host's accept() can come back with EAGAIN when the forwarded
             * descriptor is non-blocking; that is "nothing yet", not "stop
             * listening". */
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            fprintf(stderr, "vpshhd: accept: %s\n", strerror(errno));
            break;
        }
        vp_serve(listener, conn);
    }

    close(listener);
    return 0;
}
