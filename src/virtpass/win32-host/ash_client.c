/*
ash_client.c - the ash core/client split (WSL-style)

rvvm_ash is the bash.exe of this project, so it has to answer the two halves a
WSL user takes for granted:

    rvvm_ash --serve     the *core*: one run (one rvvm_user machine, one rootfs),
                         kept up while clients come and go. The guest program it
                         boots is the session server (vpsessiond), so the core
                         itself only relays what each session's pty produces.
    rvvm_ash             a *client*: connect to the core, ask for a session, and
                         be the terminal for it. If no core is running, start
                         one detached first (the "wsl" autostart), then connect.

A client is stateless on purpose: it owns no machine and no guest filesystem, it
just puts the local console in raw mode, sends the window size as a control
frame, and copies bytes both ways. That is what keeps "one --serve = one run"
the only run boundary, and a client a session within it.

The wire protocol is the session server's, not a new one:

    ESC ] 999 ; R<rows>;<cols> BEL     the client's terminal size (SIGWINCH)
    ESC ] 999 ; C<command>    BEL      run one command instead of a shell
    anything else                      bytes for the session's pty
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win/win_socket.h"
#include "win32_cmdpost_bridge.h"
#include "ash_core.h" /* the --serve exit codes this file returns */
#include "utils.h" /* rvvm_set_loglevel: RVVM_VERBOSE in the core */
#include "core/rvvm_user.h" /* rvvm_user_set_mode_store: persisted guest modes */
#include "virtpass/vp_rootfs.h" /* VP_GUEST_SESSIOND: the core's guest path */

#define ASH_PORT_DEFAULT 7900

#define ASH_FRAME_HEAD   "\x1b]999;"
#define ASH_FRAME_END    0x07

/* AF_UNIX / AF_INET / SOCK_STREAM, in the Linux numbering win_socket.c
 * translates from. */
#define ASH_AF_UNIX      1
#define ASH_AF_INET      2
#define ASH_SOCK_STREAM  1
#define ASH_SUN_PATH_MAX 108

/* Defined with the client below; the core's control verbs use them too. */
static int  ash_connect(int port);
static void ash_send_frame(int fd, const char* body);

/* ------------------------------------------------------------------ */
/* The core: --serve                                                   */
/* ------------------------------------------------------------------ */

/* The guest program a core boots: the host-owned system program /sbin/idle,
 * installed from the bundle's system layer. idle holds the machine open and
 * spawns a session per client on a pty the *host* hands it (see vp_core.h) -
 * the host owns the terminal, which is what lets a `vp` client attach over the
 * same adb-shaped byte pipe the Android host serves. A core program is not
 * selectable - it is part of the system, not an app - though `init=` can still
 * boot the legacy guest-side model (see ash_serve). */
static const char* ash_default_core_shell(void)
{
    return VP_GUEST_IDLE;
}

/* The full path of this executable. CreateProcess wants the program itself
 * (not the directory - handing it a directory is ERROR_ACCESS_DENIED). */
static bool ash_exe_path(char* out, size_t size)
{
    char module[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, module, sizeof(module));

    if (!len || len >= sizeof(module) || strlen(module) + 1 > size) {
        return false;
    }
    strcpy(out, module);
    return true;
}

/* ...and the directory it lives in, which is the working directory a core must
 * be started from (its guest image paths are resolved relative to it). */
static bool ash_exe_directory(char* out, size_t size)
{
    char module[MAX_PATH];

    if (!ash_exe_path(module, sizeof(module))) {
        return false;
    }
    for (DWORD i = (DWORD)strlen(module); i > 0; --i) {
        if (module[i - 1] == '\\' || module[i - 1] == '/') {
            module[i - 1] = 0;
            break;
        }
    }
    if (strlen(module) + 1 > size) {
        return false;
    }
    strcpy(out, module);
    return true;
}

/* ------------------------------------------------------------------ */
/* Core registry (discovery + the one-core-per-port lock)              */
/*                                                                    */
/* A core writes <exe>\runtime\cores\<port>.core with its pid and the  */
/* program it runs, and removes it on the way out. `--list` reads the  */
/* directory, `--serve` refuses a port another live core already owns. */
/* A crashed core leaves a stale file; liveness of the pid is what      */
/* tells the two apart, and a stale one is simply reclaimed.            */
/* ------------------------------------------------------------------ */

static bool ash_runtime_cores_dir(char* out, size_t size)
{
    char exe_dir[MAX_PATH];
    char runtime[MAX_PATH];

    if (!ash_exe_directory(exe_dir, sizeof(exe_dir))) {
        return false;
    }
    snprintf(runtime, sizeof(runtime), "%s\\runtime", exe_dir);
    CreateDirectoryA(runtime, NULL);        /* already there after a run */
    snprintf(out, size, "%s\\runtime\\cores", exe_dir);
    CreateDirectoryA(out, NULL);
    return true;
}

static bool ash_core_file(int port, char* out, size_t size)
{
    char dir[MAX_PATH];

    if (!ash_runtime_cores_dir(dir, sizeof(dir))) {
        return false;
    }
    snprintf(out, size, "%s\\%d.core", dir, port);
    return true;
}

/* The host directory the guest's /run/vpsessiond is mounted from (see
 * win32_cmdpost_bridge.c). The session endpoints live here, one per port, and the
 * client reaches them as plain host paths - beside runtime\cores (the core
 * registry) and runtime\rootfs (the rootfs), because all three are the release
 * tree's own runtime state rather than anything the guest owns. */
static bool ash_runtime_run_dir(char* out, size_t size)
{
    char exe_dir[MAX_PATH];
    char runtime[MAX_PATH];

    if (!ash_exe_directory(exe_dir, sizeof(exe_dir))) {
        return false;
    }
    snprintf(runtime, sizeof(runtime), "%s\\runtime", exe_dir);
    CreateDirectoryA(runtime, NULL);        /* already there after a run */
    snprintf(out, size, "%s\\runtime\\run", exe_dir);
    CreateDirectoryA(out, NULL);
    return true;
}

/* The AF_UNIX endpoint a core publishes, as a host path. It is a filesystem
 * object and not a TCP port, and it comes from a guest path -
 * /run/vpsessiond/<port>.sock, see vpsessiond.c - that the host mounts a directory
 * of its own at. So the endpoint does not move when the guest's / is backed by
 * something else, which under a prefix it used to: the path was derived from
 * <exe>\runtime\rootfs, and a run whose root is memory has no such tree. */
static bool ash_session_sock_path(int port, char* out, size_t size)
{
    char dir[MAX_PATH];

    if (!ash_runtime_run_dir(dir, sizeof(dir))) {
        return false;
    }
    if (snprintf(out, size, "%s\\%d.sock", dir, port) >= (int)size) {
        return false;
    }
    return true;
}

/* One "key=value" out of a core's registry file (defined below, where the file is
 * read). Declared here because resolving an endpoint asks the registry first. */
static bool ash_core_field(int port, const char* key, char* out, size_t out_size);

/* Discovery broker (stub).
 *
 * The client should not have to know the release layout: the eventual shape is
 * WSL's - a well-known local pipe (\\.\pipe\rvvm-ash-broker) that a machine-wide
 * service answers with the endpoint of the active distro ("list", or
 * "resolve <id>" -> the socket path). Until that service exists, this resolves
 * the one layout that does: the registry file the core writes, which names its own
 * endpoint. Keeping it a single call here means the broker lands as one function,
 * not as a change at every connect site.
 *
 * The registry is asked first because it is the core's own answer to "where am I";
 * the derived path is only what a core of this version writes, and a registry
 * without the line is one an older core left behind. */
static bool ash_broker_resolve(int port, char* out, size_t size)
{
    if (ash_core_field(port, "endpoint", out, size) && out[0]) {
        return true;
    }
    return ash_session_sock_path(port, out, size);
}

/* Print the endpoint, so a test harness does not have to re-derive the layout
 * (the same single source of truth the broker will serve). */
int ash_sock_path(int port)
{
    char path[MAX_PATH];

    if (!ash_broker_resolve(port, path, sizeof(path))) {
        fprintf(stderr, "ash: cannot locate the run directory\n");
        return 1;
    }
    printf("%s\n", path);
    return 0;
}

static bool ash_pid_alive(DWORD pid)
{
    HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
    bool   alive;

    if (!h) {
        return false;
    }
    alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

/* One "key=value" out of a core's registry file. False when the file is gone or
 * carries no such key, which for the pid is the answer every reader of it
 * already had. */
static bool ash_core_field(int port, const char* key, char* out, size_t out_size)
{
    char  path[MAX_PATH];
    char  line[256];
    char  want[64];
    FILE* f;
    size_t klen;

    if (out_size) {
        out[0] = '\0';
    }
    if (!ash_core_file(port, path, sizeof(path))) {
        return false;
    }
    f = fopen(path, "r");
    if (!f) {
        return false;
    }
    klen = strlen(key);
    snprintf(want, sizeof(want), "%s=", key);
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, want, klen) && line[klen] == '=') {
            char* v = line + klen + 1;
            char* e = strpbrk(v, "\r\n");
            if (e) {
                *e = '\0';
            }
            snprintf(out, out_size, "%s", v);
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

/* The pid a core's registry file names, or 0 when there is none. */
static DWORD ash_core_pid(int port)
{
    char buf[64];

    if (!ash_core_field(port, "pid", buf, sizeof(buf))) {
        return 0;
    }
    return (DWORD)strtoul(buf, NULL, 10);
}

static void ash_core_register(int port, const char* shell)
{
    char  path[MAX_PATH];
    char  endpoint[MAX_PATH];
    FILE* f;

    if (!ash_core_file(port, path, sizeof(path))) {
        return;
    }
    /* The endpoint is written down beside the pid rather than left for a reader to
     * derive: a client that wants to connect should have to know which port a core
     * owns, not where that core decided to listen. It is the same file that says
     * whether the core is alive, so there is one place to ask both. The idle core
     * listens on the loopback console port; only the legacy guest-side model binds
     * an AF_UNIX socket. */
    if (!strcmp(shell, VP_GUEST_IDLE)) {
        snprintf(endpoint, sizeof(endpoint), "tcp:127.0.0.1:%d", port);
    } else if (!ash_session_sock_path(port, endpoint, sizeof(endpoint))) {
        endpoint[0] = '\0';
    }
    f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "port=%d\npid=%lu\nshell=%s\nstarted=%llu\nendpoint=%s\n",
            port, (unsigned long)GetCurrentProcessId(), shell,
            (unsigned long long)GetTickCount64(), endpoint);
    fclose(f);
}

static void ash_core_unregister(int port)
{
    char path[MAX_PATH];

    if (ash_core_file(port, path, sizeof(path))) {
        DeleteFileA(path);
    }
}

/* Is a live core already on this port? A stale registration is reclaimed.
 *
 * Deliberately the pid and not the endpoint: this decides whether a *second*
 * core may start, and a wedged core - one whose guest died before it bound -
 * still holds both this port and the tree's rootfs lock. Refusing to start
 * beside it is the whole point; asking it whether it is well would wave the
 * next core straight into the same lock. `ash --list` is where the endpoint
 * gets asked, because reporting is a different question from gating. */
static bool ash_core_up(int port)
{
    DWORD pid = ash_core_pid(port);

    if (!pid) {
        return false;
    }
    if (ash_pid_alive(pid)) {
        return true;
    }
    ash_core_unregister(port);
    return false;
}

/* Does the console TCP port answer right now? This is the endpoint the idle core
 * this host now runs publishes: vp_console.c listens on it for `vp`. */
static bool ash_tcp_serving(int port)
{
    uint8_t sa[16];
    int     fd;
    bool    ok;

    win_socket_init();
    fd = win_socket_create(ASH_AF_INET, ASH_SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    /* The guest ABI's sockaddr_in - family, big-endian port, 127.0.0.1 - hand
     * packed for the same reason ash_sockaddr_unix is: this file carries no
     * socket headers, and win_socket_connect() translates from exactly this. */
    memset(sa, 0, sizeof(sa));
    sa[0] = ASH_AF_INET;
    sa[1] = 0;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)(port & 0xff);
    sa[4] = 127;
    sa[5] = 0;
    sa[6] = 0;
    sa[7] = 1;
    ok = win_socket_connect(fd, sa, sizeof(sa)) == 0;
    win_socket_close(fd);
    return ok;
}

/* Does a core answer right now? The socket is the contract: a core whose guest
 * trapped before it bound has a live pid, a registration, and nothing listening
 * - so this is the only question that separates a core a client can use from one
 * it cannot. The idle core answers on the console TCP port; the legacy
 * guest-side model (init=/sbin/vpsessiond) answers on its AF_UNIX endpoint, so
 * that is tried second. */
static bool ash_core_serving(int port)
{
    int fd;

    if (ash_tcp_serving(port)) {
        return true;
    }
    fd = ash_connect(port);
    if (fd < 0) {
        return false;
    }
    win_socket_close(fd);
    return true;
}

/* How long a registration may go without answering before it is called wedged.
 *
 * The registration is written *after* the guest is started and before it binds
 * (see ash_serve), so a core that is merely still coming up looks exactly like
 * one that will never come up. Reporting the second as the first sends the
 * reader to kill a healthy core; reporting the first as the second sends them
 * away from a dead one. Ten seconds is long past the gap on a warm cache and
 * short enough that a wedged core is named while it is still being looked at. */
#define ASH_STARTING_GRACE_MS 10000

static bool ash_core_starting(int port)
{
    char                buf[64];
    unsigned long long  started;

    if (!ash_core_field(port, "started", buf, sizeof(buf))) {
        return false;
    }
    started = strtoull(buf, NULL, 10);
    /* GetTickCount64 wraps every ~49.7 days and the subtraction is unsigned, so
     * it comes out right across the wrap - which a plain "<" would not. */
    return (unsigned long long)GetTickCount64() - started < ASH_STARTING_GRACE_MS;
}

/* ------------------------------------------------------------------ */
/* The rootfs lock                                                     */
/*                                                                    */
/* `runtime/rootfs` is the persisted writable layer, and its state is  */
/* not safe for two machines at once - two cores on different ports    */
/* would still share it. One lock file beside the tree (with the pid)  */
/* serializes them, the same way the port registry does for a port.    */
/* ------------------------------------------------------------------ */

static bool ash_lock_path(char* out, size_t size)
{
    char exe_dir[MAX_PATH];
    char runtime[MAX_PATH];

    if (!ash_exe_directory(exe_dir, sizeof(exe_dir))) {
        return false;
    }
    snprintf(runtime, sizeof(runtime), "%s\\runtime", exe_dir);
    CreateDirectoryA(runtime, NULL);
    snprintf(out, size, "%s\\ash-core.lock", runtime);
    return true;
}

/* Where a run keeps the permission bits the host filesystem cannot hold:
 * beside the persisted rootfs, so the two live and are cleared together. */
static bool ash_mode_store_path(char* out, size_t size)
{
    char exe_dir[MAX_PATH];
    char runtime[MAX_PATH];

    if (!ash_exe_directory(exe_dir, sizeof(exe_dir))) {
        return false;
    }
    snprintf(runtime, sizeof(runtime), "%s\\runtime", exe_dir);
    CreateDirectoryA(runtime, NULL);
    snprintf(out, size, "%s\\rootfs.modes", runtime);
    return true;
}

static DWORD ash_lock_pid(const char* path)
{
    FILE* f = fopen(path, "r");
    char  line[128];
    DWORD pid = 0;

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "pid=%lu", &pid) == 1) {
            break;
        }
    }
    fclose(f);
    return pid;
}

/* Take the rootfs lock for this process: false when another live core holds
 * it. A stale lock (a core that died) is reclaimed. */
static bool ash_rootfs_lock(char* out, size_t size)
{
    char path[MAX_PATH];

    if (!ash_lock_path(path, sizeof(path))) {
        return true;   /* cannot locate it: do not block a run over it */
    }
    if (strlen(path) + 1 > size) {
        return true;
    }
    strcpy(out, path);

    DWORD holder = ash_lock_pid(path);
    if (holder && holder != GetCurrentProcessId() && ash_pid_alive(holder)) {
        return false;
    }
    FILE* f = fopen(path, "w");
    if (!f) {
        return true;
    }
    fprintf(f, "pid=%lu\n", (unsigned long)GetCurrentProcessId());
    fclose(f);
    return true;
}

static void ash_rootfs_unlock(const char* path)
{
    char mine[MAX_PATH];

    if (ash_lock_path(mine, sizeof(mine)) && strcmp(mine, path) == 0 &&
        ash_lock_pid(mine) == GetCurrentProcessId()) {
        DeleteFileA(mine);
    }
}

/* ------------------------------------------------------------------ */
/* Being stopped from outside                                          */
/*                                                                     */
/* Windows hands a console process these when something outside stops */
/* it: the console going away - the window being closed, which is    */
/* what the task manager's "End task" does to a console program, plus */
/* logoff and shutdown - and Ctrl+C / Ctrl+Break, the same request   */
/* made at the keyboard.                                              */
/*                                                                     */
/* Left unhandled, the process is torn down where it stands and the  */
/* caller sees whatever code the killer chose:                       */
/* STATUS_CONTROL_C_EXIT, 0xC000013A. That is an NTSTATUS where a    */
/* caller expects an exit code, it is not in the set ash_core.h      */
/* declares closed, and it reads as neither "stopped cleanly" nor a  */
/* number anyone can act on. So every catchable stop is answered     */
/* with ASH_EXIT_TERMINATED, which says the one thing that is true   */
/* of all of them: the run was cut short, and the guest never        */
/* decided anything.                                                  */
/*                                                                     */
/* A console handler runs on a thread of its own and on a deadline,  */
/* so it does only what is safe there: the two registrations this   */
/* core owns are file deletes, each guarded by the pid already       */
/* recorded in it (a lock a later run has since reclaimed is left    */
/* alone, as on the orderly path), and then it leaves at once.       */
/* Anything heavier - asking the guest to shut down - would be a     */
/* wait on threads that are not going to answer, and the deadline    */
/* would run out while it waited.                                    */
/*                                                                     */
/* A hard TerminateProcess (Stop-Process -Force, taskkill /F) is    */
/* not a control event and cannot be caught by anyone, so that one   */
/* still exits with the killer's code.                               */
/* ------------------------------------------------------------------ */

/* The port this process registered a core under, or -1 when it is not a core. */
static int ash_serve_port = -1;

static BOOL WINAPI ash_console_ctrl(DWORD type)
{
    char    line[128];
    HANDLE  err = GetStdHandle(STD_ERROR_HANDLE);
    int     n;

    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    /* The console going away: the window being closed, which is what the task
     * manager's "End task" does to a console program, plus logoff and shutdown. */
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        break;
    default:
        return FALSE;
    }

    if (ash_serve_port >= 0 && ash_core_pid(ash_serve_port) == GetCurrentProcessId()) {
        char core[MAX_PATH];
        if (ash_core_file(ash_serve_port, core, sizeof(core))) {
            DeleteFileA(core);
        }
    }
    {
        char lock[MAX_PATH];
        if (ash_lock_path(lock, sizeof(lock)) && ash_lock_pid(lock) == GetCurrentProcessId()) {
            DeleteFileA(lock);
        }
    }

    /* WriteFile, not fprintf: stdio can be held by another thread at this
     * point, and a console handler that blocks is one Windows simply kills -
     * which would put us back to reporting the killer's code. */
    n = snprintf(line, sizeof(line),
                 "ash: terminated from outside (console control event %lu), "
                 "leaving with ASH_EXIT_TERMINATED\n", (unsigned long)type);
    if (n > 0 && err != NULL && err != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(err, line, (DWORD)n, &written, NULL);
    }
    ExitProcess(ASH_EXIT_TERMINATED);
    return TRUE;
}

void ash_install_termination_handler(void)
{
    static bool installed = false;

    if (!installed) {
        installed = true;
        SetConsoleCtrlHandler(ash_console_ctrl, TRUE);
    }
}

int ash_serve(int port, int idle_s, const char* dlog, const char* cmdline)
{
    const char* shell = ash_default_core_shell();
    const char* init_arg;
    char        port_buf[16];
    char        idle_buf[16];
    char*       guest[5];
    int         rc;

    /* One core per port: the lock is the registry, and it fails before a second
     * machine is built only to lose the port race inside the guest. The code says
     * which refusal this was, because the two want different remedies - another
     * port, or another core gone. */
    if (ash_core_up(port)) {
        fprintf(stderr, "ash --serve: a core is already up on port %d\n", port);
        return ASH_EXIT_PORT_BUSY;
    }

    char lock[MAX_PATH];
    if (!ash_rootfs_lock(lock, sizeof(lock))) {
        fprintf(stderr, "ash --serve: this release's rootfs is in use by another core\n");
        return ASH_EXIT_ROOTFS_BUSY;
    }

    /* The shell is a guest path now (sbin/vpsessiond), resolved inside the run's
     * rootfs - so the core no longer has to be standing in the release directory
     * for the image to be found: the bundle is located by the executable's own
     * directory, never the cwd. */

    /* `init=` names the core program, and is applied here rather than deeper in
     * the bridge because this is where the program's own arguments are decided:
     * the host bridge is handed argv and has no business choosing what argv[0]
     * is. What comes with the default is the session server's own signature,
     * which is why init= overrides the program and not the way it is called - a
     * `rvvm_ash --serve --cmdline "init=/data/app/test_std/bin/test_std"`
     * therefore gets that program's usage and exits, which is what booting it
     * directly would do.
     *
     * Used verbatim as a guest path, like the default, and resolved inside the
     * run rather than here: the host does not know this run's rootfs (root= may
     * have just changed it) and a check against the wrong tree would refuse a
     * program that is there. An init= naming nothing is loud rather than
     * silent - the ELF load fails, the guest leaves, and the line below says the
     * core stopped with its status. */
    init_arg = cmdline ? rvvm_cmdline_get(cmdline, "init") : NULL;
    if (init_arg && init_arg[0]) {
        shell = init_arg;
    }

    snprintf(port_buf, sizeof(port_buf), "%d", port);
    snprintf(idle_buf, sizeof(idle_buf), "%d", idle_s > 0 ? idle_s : 0);

    /* Two core models, told apart by the program init= chose. The default is
     * idle, whose terminal the host drives (vp_core.c): the host appends
     * `--control <path>` itself, so there is nothing to pass on this command
     * line. Anything else (init=/sbin/vpsessiond) is the legacy guest-side
     * model, where the program takes the port and the idle timeout itself. */
    bool vp_core = !strcmp(shell, VP_GUEST_IDLE);
    int  guest_argc = 1;
    guest[0] = (char*)shell;
    guest[1] = NULL;
    if (!vp_core) {
        /* argv[3]: the legacy daemon's log, as a guest path, only when one was
         * asked for. The count follows, because the guest's argv is what
         * carries it - a fixed count would drop it on the floor. */
        guest_argc = 3;
        guest[1] = port_buf;
        guest[2] = idle_buf;
        if (dlog && *dlog) {
            guest[3] = (char*)dlog;
            guest_argc = 4;
        }
        guest[4] = NULL;
    }
    if (vp_core && idle_s > 0) {
        fprintf(stderr, "ash --serve: --idle is not honoured by the idle core; "
                        "it belongs to the legacy model (init=/sbin/vpsessiond)\n");
    }

    /* Same switch as rvvm_winhost (win32_main.c): RVVM_VERBOSE=1 lifts the log
     * to LOG_INFO so the per-syscall lines (sys_openat etc.) reach stderr.
     *
     * Applied here rather than in ash_main.c, and immediately after this line,
     * because this line is a default that would otherwise overwrite it: a
     * --cmdline asking for loglevel=debug on a run with RVVM_VERBOSE unset has
     * to end up loud, and the ordering that gets that right is the whole reason
     * the two are adjacent. ash_main.c parses --cmdline and passes it down; it
     * does not decide the level. */
    rvvm_set_loglevel(getenv("RVVM_VERBOSE") ? LOG_INFO : LOG_WARN);
    if (cmdline) {
        rvvm_apply_cmdline_logging(cmdline);
    }

    /* Before the guest starts, because the machine does not exist until it does.
     * root= is applied inside win32_host_start_guest(), after the bundle mount,
     * so that a command line overrides the image rather than the other way
     * round. */
    win32_host_set_cmdline(cmdline);

    /* The guest's permission bits outlive this process: the host filesystem
     * has nowhere to put them, so they go to a file beside the rootfs. */
    {
        char modes[MAX_PATH];
        if (ash_mode_store_path(modes, sizeof(modes))) {
            rvvm_user_set_mode_store(modes);
        }
    }

    if (vp_core) {
        /* The core's port is the console's, and the control terminal has to
         * exist before the guest starts - idle opens it as its first act (see
         * win32_host_start_guest). */
        win32_host_arm_core();
        win32_host_set_console_port(port);
    }

    if (!win32_host_init_console(0, 0, 0)) {
        fprintf(stderr, "ash --serve: could not initialize the host\n");
        ash_rootfs_unlock(lock);
        return ASH_EXIT_HOST_INIT;
    }
    /* A core is a daemon: it must not put the terminal it was started from into
     * raw mode or eat its input (the sessions are on the socket, not here). */
    win32_host_no_stdin();
    if (!win32_host_start_guest(guest_argc, guest)) {
        fprintf(stderr, "ash --serve: could not start %s\n", shell);
        win32_host_shutdown();
        ash_rootfs_unlock(lock);
        return ASH_EXIT_NO_GUEST;
    }

    ash_core_register(port, shell);
    /* From here the run owns a port, so a control event has a registration to
     * drop (see ash_console_ctrl). Cleared again on the way out, below. */
    ash_serve_port = port;
    fprintf(stderr, "ash: core up (shell %s, port %d%s%s) - connect with `ash`\n",
            shell, port, idle_s > 0 ? ", idle " : "", idle_s > 0 ? idle_buf : "");
    rc = win32_host_wait_guest();
    /* The guest's status is recorded, not returned: the exit code is the core's
     * own vocabulary (see ash_core.h), and a code that can mean either thing is
     * a code nobody can branch on. The line goes to the core's stderr beside the
     * log that explains the run, so nothing is lost by not returning it. */
    fprintf(stderr, "ash: core stopped (the guest program left with %d)\n", rc);
    ash_core_unregister(port);
    ash_serve_port = -1;
    ash_rootfs_unlock(lock);
    win32_host_shutdown();
    return rc == 0 ? ASH_EXIT_OK : ASH_EXIT_GUEST_NONZERO;
}

/* `ash --list`: the cores this release directory has registrations for. */
int ash_list(void)
{
    char             dir[MAX_PATH];
    char             pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              found = 0;

    if (!ash_runtime_cores_dir(dir, sizeof(dir))) {
        fprintf(stderr, "ash: cannot locate the runtime directory\n");
        return ASH_EXIT_USAGE;
    }
    snprintf(pattern, sizeof(pattern), "%s\\*.core", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        printf("no cores running\n");
        return 0;
    }
    do {
        int   port = atoi(fd.cFileName);   /* "<port>.core" */
        DWORD pid  = ash_core_pid(port);
        char  shell[128] = "";

        if (!pid) {
            continue;
        }
        ash_core_field(port, "shell", shell, sizeof(shell));
        /* Four answers, and the middle two are the ones this used to collapse.
         * A registration is a claim, not a proof: the pid can be alive with a
         * guest that never bound (it trapped), and a core that is merely still
         * starting up looks the same for a moment. Calling the first "up" sent
         * a client at a socket nobody was listening on, and calling the second
         * "dead" would have had an agent killing healthy cores. */
        if (!ash_pid_alive(pid)) {
            printf("stale  port=%-5d (reclaimed)\n", port);
            ash_core_unregister(port);
        } else if (ash_core_serving(port)) {
            printf("up     port=%-5d pid=%-6lu shell=%s\n", port, (unsigned long)pid, shell);
        } else if (ash_core_starting(port)) {
            printf("start  port=%-5d pid=%-6lu shell=%s (endpoint not up yet)\n",
                   port, (unsigned long)pid, shell);
        } else {
            /* Not reclaimed, and that is the point: the process is still there
             * and still holds this tree's rootfs lock, so dropping its
             * registration would hide the one thing the reader has to act on. */
            printf("wedged port=%-5d pid=%-6lu shell=%s (registered, endpoint dead; "
                   "still holds the rootfs - end pid %lu)\n",
                   port, (unsigned long)pid, shell, (unsigned long)pid);
        }
        found++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (!found) {
        printf("no cores running\n");
    }
    return 0;
}

/* `ash --shutdown`: ask the core on @port to stop. The request is the session
 * protocol's `Q` frame, so no pid or signal is needed. */
int ash_shutdown(int port)
{
    int   fd = ash_connect(port);
    char  buf[256];

    if (fd < 0) {
        DWORD pid = ash_core_pid(port);

        if (pid && ash_pid_alive(pid)) {
            /* It is there and cannot be asked. The endpoint lives in the guest,
             * so a guest that died leaves a core that answers nothing, still
             * holds this tree's rootfs, and refuses the next --serve with
             * "another core holds this release rootfs" - a lock nobody can open
             * because the only key is the thing that is broken. "no core on port
             * N" points at a core that does not exist; name the one that does. */
            fprintf(stderr, "ash: the core on port %d is registered (pid %lu) but its endpoint "
                            "does not answer - it is wedged and still holds this tree's rootfs; "
                            "end pid %lu to release it\n",
                    port, (unsigned long)pid, (unsigned long)pid);
        } else {
            fprintf(stderr, "ash: no core on port %d\n", port);
        }
        return 1;
    }
    ash_send_frame(fd, "Q");
    /* Drain until the core closes the socket (it is on its way out). */
    for (int i = 0; i < 200; i++) {
        int r = win_socket_wait(fd, WIN_POLLIN, 50);
        if (r < 0) {
            break;
        }
        if (r > 0 && win_socket_read(fd, buf, sizeof(buf)) <= 0) {
            break;
        }
    }
    win_socket_close(fd);
    fprintf(stderr, "ash: asked the core on %d to stop\n", port);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The client                                                          */
/* ------------------------------------------------------------------ */

/* sockaddr_un, laid out as win_socket.c (and WinSock) expect: a 2-byte family
 * then the pathname. Kept as bytes so this file never has to include a socket
 * header next to <windows.h>. Returns the address length, or -1 when the path
 * does not fit the ABI struct. */
static int ash_sockaddr_unix(void* out, const char* path)
{
    uint8_t* p = (uint8_t*)out;
    size_t   n = strlen(path);

    if (n + 1 > ASH_SUN_PATH_MAX) {
        return -1;
    }
    memset(p, 0, 2 + ASH_SUN_PATH_MAX);
    p[0] = ASH_AF_UNIX;
    memcpy(p + 2, path, n + 1);
    return 2 + ASH_SUN_PATH_MAX;
}

static int ash_connect(int port)
{
    char    path[MAX_PATH];
    uint8_t sa[2 + ASH_SUN_PATH_MAX];
    int     fd;
    int     len;

    if (!ash_broker_resolve(port, path, sizeof(path))) {
        return -1;
    }
    len = ash_sockaddr_unix(sa, path);
    if (len < 0) {
        return -1;
    }
    win_socket_init();
    fd = win_socket_create(ASH_AF_UNIX, ASH_SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (win_socket_connect(fd, sa, len) < 0) {
        win_socket_close(fd);
        return -1;
    }
    return fd;
}

/* Start a core detached so it outlives this client and the next one finds it
 * already up. Some environments refuse DETACHED_PROCESS (it can return
 * ERROR_ACCESS_DENIED for a console app), so the flags are tried in turn:
 * detached first, then a hidden console, then plain. */
static bool ash_spawn_core(const char* exe_path, const char* exe_dir, int port)
{
    static const DWORD flags[] = {
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        CREATE_NO_WINDOW,
        0,
    };

    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        char                cmd[2 * MAX_PATH];
        STARTUPINFOA        si;
        PROCESS_INFORMATION pi;

        snprintf(cmd, sizeof(cmd), "\"%s\" --serve --port %d", exe_path, port);
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        if (CreateProcessA(exe_path, cmd, NULL, NULL, FALSE, flags[i], NULL, exe_dir,
                           &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
    }
    return false;
}

/* Console mode handling, mirroring the bridge's: raw input so the guest's own
 * line discipline owns ^C/^Z (and a shell gets its SIGINT), VT output so the
 * session's escapes render, and the old modes put back on every way out. */
static DWORD g_in_mode  = 0;
static DWORD g_out_mode = 0;
static bool  g_mode_saved = false;

static HANDLE g_hin  = INVALID_HANDLE_VALUE;
static HANDLE g_hout = INVALID_HANDLE_VALUE;

static void ash_console_enter_raw(void)
{
    DWORD mode;

    g_hin  = GetStdHandle(STD_INPUT_HANDLE);
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);

    if (g_hin && g_hin != INVALID_HANDLE_VALUE && GetConsoleMode(g_hin, &mode)) {
        g_in_mode = mode;
        g_mode_saved = true;
        SetConsoleMode(g_hin, (mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                               ENABLE_PROCESSED_INPUT))
                              | ENABLE_VIRTUAL_TERMINAL_INPUT);
    }
    if (g_hout && g_hout != INVALID_HANDLE_VALUE && GetConsoleMode(g_hout, &mode)) {
        g_out_mode = mode;
        SetConsoleMode(g_hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                      ENABLE_PROCESSED_OUTPUT);
    }
}

static void ash_console_restore(void)
{
    if (!g_mode_saved) {
        return;
    }
    g_mode_saved = false;
    if (g_hin && g_hin != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_hin, g_in_mode);
    }
    if (g_hout && g_hout != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_hout, g_out_mode);
    }
}

static bool ash_console_size(int* rows, int* cols)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    DWORD  mode = 0;
    HANDLE h = g_hout;

    if (!h || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) {
        h = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }
    }
    if (!GetConsoleScreenBufferInfo(h, &info)) {
        return false;
    }
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    *cols = info.srWindow.Right - info.srWindow.Left + 1;
    return *rows > 0 && *cols > 0;
}

static void ash_send_frame(int fd, const char* body)
{
    char frame[1024];
    int  n = snprintf(frame, sizeof(frame), ASH_FRAME_HEAD "%s%c", body, ASH_FRAME_END);
    if (n > 0) {
        win_socket_write(fd, frame, (size_t)n);
    }
}

/* The window size, sent once at connect and again whenever it changes - the
 * server turns the change into a SIGWINCH for the session's foreground group. */
static int g_last_rows = 0;
static int g_last_cols = 0;

static void ash_send_size(int fd, bool force)
{
    int rows = 0, cols = 0;
    char body[32];

    if (!ash_console_size(&rows, &cols)) {
        if (!force) {
            return;
        }
        rows = 24;
        cols = 80;
    }
    if (!force && rows == g_last_rows && cols == g_last_cols) {
        return;
    }
    g_last_rows = rows;
    g_last_cols = cols;
    snprintf(body, sizeof(body), "R%u;%u", (unsigned)rows, (unsigned)cols);
    ash_send_frame(fd, body);
}

static DWORD WINAPI ash_stdin_thread(LPVOID param)
{
    int    fd = (int)(intptr_t)param;
    char   buf[512];
    DWORD  got = 0;

    for (;;) {
        if (!ReadFile(g_hin, buf, sizeof(buf), &got, NULL) || got == 0) {
            /* The local input ended. A closed socket would tell the server the
             * client left, which would hang the session up before it ran
             * anything; a terminal says "input ended" with Ctrl-D, so that is
             * what the session's line discipline gets - the same thing the
             * direct host's stdin pump does. The session ends when its shell
             * does (an `exit`, or the shell acting on this EOF at a prompt). */
            win_socket_write(fd, "\x04", 1);
            return 0;
        }
        if (win_socket_write(fd, buf, (size_t)got) <= 0) {
            return 0;
        }
    }
}

int ash_client(int port, const char* one_cmd, bool autostart)
{
    int   fd;
    char  buf[4096];

    fd = ash_connect(port);
    if (fd < 0 && autostart) {
        /* WSL behaviour: a client starts the core if none is running. Detached,
         * so it outlives this client and the next one finds it already up. */
        char exe_path[MAX_PATH];
        char exe_dir[MAX_PATH];
        bool started = false;

        if (ash_exe_path(exe_path, sizeof(exe_path)) &&
            ash_exe_directory(exe_dir, sizeof(exe_dir))) {
            started = ash_spawn_core(exe_path, exe_dir, port);
        }
        if (!started) {
            fprintf(stderr, "ash: no core on port %d and could not start one (err %lu)\n",
                    port, (unsigned long)GetLastError());
            return 1;
        }
        for (int i = 0; i < 100 && fd < 0; i++) {   /* up to ~5 s */
            Sleep(50);
            fd = ash_connect(port);
        }
        if (fd < 0) {
            fprintf(stderr, "ash: started a core but it never listened on %d\n", port);
            return 1;
        }
    } else if (fd < 0) {
        fprintf(stderr, "ash: cannot reach a core on port %d (start `ash --serve`)\n", port);
        return 1;
    }

    ash_console_enter_raw();
    ash_send_size(fd, true);
    if (one_cmd && *one_cmd) {
        char body[1024];
        snprintf(body, sizeof(body), "C%s", one_cmd);
        ash_send_frame(fd, body);
    } else {
        /* Only an interactive session forwards stdin: a one-shot command must
         * not half-close the session the moment a piped stdin reports EOF. */
        CreateThread(NULL, 0, ash_stdin_thread, (LPVOID)(intptr_t)fd, 0, NULL);
    }

    for (;;) {
        int r = win_socket_wait(fd, WIN_POLLIN, 250);
        if (r < 0) {
            break;
        }
        if (r == 0) {
            ash_send_size(fd, false);
            continue;
        }
        long n = win_socket_read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;   /* the session ended: the core closed our socket */
        }
        {
            DWORD written = 0;
            WriteFile(g_hout, buf, (DWORD)n, &written, NULL);
        }
    }

    ash_console_restore();
    win_socket_close(fd);
    return 0;
}