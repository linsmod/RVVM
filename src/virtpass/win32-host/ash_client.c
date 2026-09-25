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

#define ASH_PORT_DEFAULT 7900

#define ASH_FRAME_HEAD   "\x1b]999;"
#define ASH_FRAME_END    0x07

/* AF_INET / SOCK_STREAM, in the Linux numbering win_socket.c translates from. */
#define ASH_AF_INET      2
#define ASH_SOCK_STREAM  1

/* ------------------------------------------------------------------ */
/* The core: --serve                                                   */
/* ------------------------------------------------------------------ */

/* The guest program a core boots. It has to be reachable the way any guest
 * image is: a path `map_abs_path()` leaves alone, i.e. relative to the host's
 * working directory (an absolute path is read as a guest path and prefixed).
 * So the default names the dev build's copy relative to the release directory,
 * and --serve chdir()s there before launching. A bundle build overrides it with
 * RVVM_ASH_SHELL (e.g. an app entry). */
static const char* ash_default_core_shell(void)
{
    const char* env = getenv("RVVM_ASH_SHELL");
    if (env && *env) {
        return env;
    }
    return "guest-assets\\vpsessiond.exe";
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

int ash_serve(int port)
{
    const char* env_shell = getenv("RVVM_ASH_SHELL");
    const char* shell     = ash_default_core_shell();
    char        port_buf[16];
    char*       guest[3];
    int         rc;

    /* The default shell is a relative guest path, so the host has to be standing
     * in the release directory when the image is opened. An explicit
     * RVVM_ASH_SHELL is the caller's path to place, so it is not second-guessed. */
    if (!env_shell || !*env_shell) {
        char exe_dir[MAX_PATH];
        if (ash_exe_directory(exe_dir, sizeof(exe_dir))) {
            SetCurrentDirectoryA(exe_dir);
        }
    }

    snprintf(port_buf, sizeof(port_buf), "%d", port);
    guest[0] = (char*)shell;
    guest[1] = port_buf;
    guest[2] = NULL;

    if (!win32_host_init_console(0, 0, 0)) {
        fprintf(stderr, "ash --serve: could not initialize the host\n");
        return 1;
    }
    /* A core is a daemon: it must not put the terminal it was started from into
     * raw mode or eat its input (the sessions are on the socket, not here). */
    win32_host_no_stdin();
    if (!win32_host_start_guest(2, guest)) {
        fprintf(stderr, "ash --serve: could not start %s\n", shell);
        win32_host_shutdown();
        return 1;
    }

    fprintf(stderr, "ash: core up (shell %s, port %d) - connect with `ash`\n", shell, port);
    rc = win32_host_wait_guest();
    win32_host_shutdown();
    return rc;
}

/* ------------------------------------------------------------------ */
/* The client                                                          */
/* ------------------------------------------------------------------ */

/* 16-byte AF_INET 127.0.0.1:port, laid out as win_socket.c (and WinSock) expect:
 * family, port in network order, address in network order. Kept as bytes so this
 * file never has to include a socket header next to <windows.h>. */
static void ash_sockaddr_loopback(void* out, int port)
{
    uint8_t* p = (uint8_t*)out;
    memset(p, 0, 16);
    p[0] = ASH_AF_INET;
    p[2] = (uint8_t)((port >> 8) & 0xFF);
    p[3] = (uint8_t)(port & 0xFF);
    p[4] = 127;
    p[7] = 1;
}

static int ash_connect(int port)
{
    int fd;
    uint8_t sa[16];

    win_socket_init();
    fd = win_socket_create(ASH_AF_INET, ASH_SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    ash_sockaddr_loopback(sa, port);
    if (win_socket_connect(fd, sa, (int)sizeof(sa)) < 0) {
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
            fprintf(stderr, "ash: no core on 127.0.0.1:%d and could not start one (err %lu)\n",
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
        fprintf(stderr, "ash: cannot reach a core on 127.0.0.1:%d (start `ash --serve`)\n", port);
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