/*
 * vp_client.c - a client for the Android host's guest console, spelled like adb
 *
 * The Win32 host has `rvvm_ash` and the Android host had nothing: the only way
 * to exercise a guest on a phone was to start it, photograph it and synthesise
 * taps. The Android host's console is a byte pipe (see vp_console.c), so what
 * is missing here is a program that speaks it - and there is no reason for that
 * program to invent a shape.
 *
 * So this is adb, with the shell pointed at the guest instead of at Android:
 *
 *     adb devices            ->  vp devices
 *     adb shell              ->  vp shell
 *     adb exec-out CMD       ->  vp exec-out CMD
 *     adb forward L R        ->  vp forward L R
 *     adb -s SERIAL ...      ->  vp -s SERIAL ...
 *
 * Same verbs, same global options, same meanings. Someone who has used adb can
 * read the usage and be right about it, and the difference is one noun - "the
 * device" is a phone, "the target" is a guest.
 *
 * The one thing this does *not* add is a state of its own. `vp devices` is
 * adb's `adb devices`, and `vp shell` starts the host on demand the way
 * `adb shell` starts a service - so a phone nobody has typed at yet is a
 * `device`, not something this invents a word for. `devices -l` carries the
 * extra fact (`host:up` / `host:asleep`) where it informs without overruling
 * adb. An earlier version reported `offline` here, which is adb's word for a
 * broken transport, and it made working phones look dead.
 *
 * Deliberately *not* implemented: install, push, root, reverse, start-server.
 * Everything on the device side stays adb's job, and adb stays the only thing
 * that talks to the USB device. A second claimant on that bus is a fight
 * neither side wins.
 *
 * adb is invoked as a subprocess, never spoken to directly: the daemon already
 * owns the transport, and going around it would mean duplicating the protocol
 * and racing adb for the interface.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Plain WinSock, not the shim. win_socket.c gives the *emulator*
 * Linux-numbered sockets, because a guest's syscalls arrive untranslated; vp is
 * an ordinary Windows program with no guest behind it and no reason to be
 * numbered in anything.
 */

#define VP_VERSION      "0.1"
#define VP_DEFAULT_PORT 7979
#define VP_DEFAULT_GUEST "test_busybox"

/* The core's run root, by guest path. A system program (the bundle's system layer
 * lays it out at /sbin/), so it has no manifest and no app id - which is why
 * boot_core() names it by path instead of looking an app up by name. */
#define VP_CORE_ROOT "/sbin/idle"

/* AF_INET / SOCK_STREAM, WinSock's own numbering. */
#define VP_AF_INET      AF_INET
#define VP_SOCK_STREAM  SOCK_STREAM

/* The packet ids, as adb's ShellProtocol has them. Spelled out here rather than
 * included: vp_console.h is in the android-host tree, and this program must
 * build without the device side. The two are checked against each other by
 * tools/android_console.ps1 -SelfTest, which encodes the same numbers. */
enum {
    VP_ID_STDIN      = 0,
    VP_ID_STDOUT     = 1,
    VP_ID_STDERR     = 2,
    VP_ID_EXIT       = 3,
    VP_ID_CLOSESTDIN  = 4,
    VP_ID_WINDOWSIZE  = 5,
    VP_ID_READY       = 6,
};

#define VP_HDR_BYTES 5

static const char* g_serial = NULL;  /* -s, NULL = the only device attached */
static int         g_port   = VP_DEFAULT_PORT;
static int         g_long   = 0;     /* -l on devices */

/* ------------------------------------------------------------------ */
/* running adb                                                          */
/* ------------------------------------------------------------------ */

/* Run adb with @argv, optionally capturing its stdout. Returns the exit code,
 * or -1 when adb could not be started at all. */
static int run_adb(const char* serial, char* const argv[], char* out, size_t outsz)
{
    char   line[2048];
    size_t at = 0;
    line[0] = '\0';
    /* The program goes first and is not a caller's to supply: every argument
     * list in this file is adb's arguments, and forgetting the program in one
     * of them is a FILE_NOT_FOUND that names no file. */
    int n0 = snprintf(line, sizeof(line), "\"adb\"");
    if (n0 <= 0 || (size_t)n0 >= sizeof(line)) {
        return -1;
    }
    at = (size_t)n0;
    /* -s is a *global* flag: it has to sit between the program name and the
     * verb. `adb forward -s SERIAL l r` is rejected by forward's own argument
     * count, which is how one misplaced flag turns into "forward takes two
     * arguments" and points at the wrong thing entirely. Putting it here means
     * no call site has to remember. */
    if (serial) {
        int ns = snprintf(line + at, sizeof(line) - at, " \"-s\" \"%s\"", serial);
        if (ns < 0 || (size_t)ns >= sizeof(line) - at) {
            return -1;
        }
        at += (size_t)ns;
    }
    for (int i = 0; argv[i]; i++) {
        /* Every argument quoted, including the first: an unquoted leading
         * token here is how `devices` silently becomes `devices" "-l"` and adb
         * answers nothing. */
        int n = snprintf(line + at, sizeof(line) - at, " \"%s\"", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(line) - at) {
            return -1;
        }
        at += (size_t)n;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE  rd = NULL, wr = NULL;
    if (out && outsz) {
        out[0] = '\0';
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            return -1;
        }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    si.hStdOutput = out ? wr : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = out ? wr : GetStdHandle(STD_ERROR_HANDLE);
    /* NUL, not our own stdin. `adb shell CMD` forwards its local stdin to the
     * command it runs, so handing it the driver's stdin lets adb drink the
     * whole script before the guest ever exists - and what is left for the
     * console is an empty pipe, which reads as a client that typed nothing.
     * adb has no business reading this process's input in the first place. */
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, NULL);
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul
                                                 : GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags    = STARTF_USESTDHANDLES;

    /* CREATE_NO_WINDOW: vp is a console program and adb must not flash a
     * window for every probe - `devices` runs one per attached device. */
    /* NULL for the application name, so the module comes from the first token
     * of the command line and is looked up through PATH with the .exe rule
     * applied. Passing "adb" as the application name instead does not search
     * PATH at all, and fails with a -1 that says nothing about why. */
    BOOL ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi);
    if (!ok) {
        /* A bare -1 says nothing: "adb is not installed" and "the handles you
         * passed are unusable" are the same number. */
        static DWORD last;
        last = GetLastError();
        fprintf(stderr, "vp: cannot run adb (%lu): %s\n", last, line);
    }
    if (out && wr) {
        CloseHandle(wr);
    }
    if (!ok) {
        if (rd) {
            CloseHandle(rd);
        }
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }

    if (out && rd) {
        char    buf[4096];
        DWORD   got = 0;
        size_t  used = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got) {
            size_t room = outsz - 1 - used;
            size_t take = got < room ? got : room;
            memcpy(out + used, buf, take);
            used += take;
            out[used] = '\0';
        }
        CloseHandle(rd);
    }
    return (int)code;
}

/* ------------------------------------------------------------------ */
/* devices                                                              */
/* ------------------------------------------------------------------ */

#define VP_MAX_DEVICES 16

typedef struct {
    char serial[64];
    char state[32];
    char detail[192];
} vp_device;

static int g_device_count;
static vp_device g_devices[VP_MAX_DEVICES];

/* WSAStartup once. */
static void wsa_start(void)
{
    static bool done;
    if (done) {
        return;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    done = true;
}

/* Readable within @timeout_ms? >0 yes, 0 timed out, -1 the socket is gone. */
static int sock_wait(int fd, int timeout_ms)
{
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int n = select(fd + 1, &r, NULL, NULL, &tv);
    if (n == SOCKET_ERROR) {
        return -1;
    }
    return n;
}

/* `adb devices -l`:
 *
 *     List of devices attached
 *     7e3fb930  device product:OnePlus8T_CH model:KB2000 device:KB2000
 *
 * The first line is a header and is skipped; everything after it that has a
 * second whitespace-separated field is a device. */
static void devices_load(void)
{
    char out[16384];
    char* argv[] = { "devices", "-l", NULL };
    g_device_count = 0;
    int rc = run_adb(NULL, argv, out, sizeof(out));
    if (rc != 0 || !out[0]) {
        /* adb's own words, because "no devices" and "adb is not on PATH" and
         * "the daemon is not running" are three different problems and
         * vp: no devices does not distinguish any of them. */
        fprintf(stderr, "vp: `adb devices` did not answer (exit %d)%s%s\n",
                rc, out[0] ? ": " : "", out);
        return;
    }
    char* line = out;
    bool first = true;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (first) {
            first = false;   /* "List of devices attached" */
        } else {
            char  state[32];
            char  serial[64];
            int   ns = 0, ss = 0;
            if (sscanf(line, "%63s %31s%n", serial, state, &ss) == 2 && ss > 0 && ns >= 0) {
                if (g_device_count < VP_MAX_DEVICES) {
                    vp_device* d = &g_devices[g_device_count++];
                    snprintf(d->serial, sizeof(d->serial), "%s", serial);
                    snprintf(d->state, sizeof(d->state), "%s", state);
                    snprintf(d->detail, sizeof(d->detail), "%s", line + ss);
                    while (*d->detail == ' ') {
                        memmove(d->detail, d->detail + 1, strlen(d->detail));
                    }
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

/* Whether this device's RVVM host is listening: forward the port and try to
 * hold a connection open. adb forward accepts on the host before it reaches
 * for the device, so a connect that succeeds proves nothing - the probe waits
 * for a byte that will not come, which a live console never sends and a dead
 * relay answers with end-of-stream at once. */
static bool host_listening(const char* serial)
{
    /* tcp: on both ends. A bare number is not a socket specification and adb
     * says so only after it has already agreed there were two arguments. */
    char  spec[16];
    snprintf(spec, sizeof(spec), "tcp:%d", g_port);
    char* args[4];
    int n = 0;
    args[n++] = (char*)"forward";
    args[n++] = spec;
    args[n++] = spec;
    args[n]   = NULL;
    /* Captured, not inherited: adb forward echoes the port it bound, and
     * `vp devices` printing 7979 above its own list is noise the caller cannot
     * tell from a device serial. Kept for the failure path, where adb's own
     * reason is the only thing worth printing. */
    char reply[512];
    if (run_adb(serial, args, reply, sizeof(reply)) != 0) {
        fprintf(stderr, "vp: adb forward %s failed%s%s\n", spec,
                reply[0] ? ": " : "", reply);
        return false;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = VP_AF_INET;
    sa.sin_port        = htons((uint16_t)g_port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");

    wsa_start();
    /* Liveness is "connected and the connection is not already over". A relay
     * that found no listener on the device says so by ending the stream at
     * once; a console with nothing to say yet says nothing. Retried, because
     * the device may be an app that is starting up right now, and bounded so a
     * device that will never have a host still answers. */
    bool alive = false;
    for (int attempt = 0; attempt < 8 && !alive; attempt++) {
        int fd = socket(VP_AF_INET, VP_SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
            int ready = sock_wait(fd, 300);
            if (ready == 0) {
                alive = true;
            } else if (ready > 0) {
                char peek;
                alive = recv(fd, &peek, 1, MSG_PEEK) != 0;
            }
        }
        closesocket(fd);
        if (!alive) {
            Sleep(500);
        }
    }
    return alive;
}

static vp_device* pick_device(void)
{
    if (!g_device_count) {
        fprintf(stderr, "vp: no devices attached (adb devices lists none)\n");
        return NULL;
    }
    if (g_serial) {
        for (int i = 0; i < g_device_count; i++) {
            if (!strcmp(g_devices[i].serial, g_serial)) {
                return &g_devices[i];
            }
        }
        fprintf(stderr, "vp: device '%s' not connected\n", g_serial);
        return NULL;
    }
    if (g_device_count > 1) {
        fprintf(stderr, "vp: more than one device attached; name one with -s\n");
        for (int i = 0; i < g_device_count; i++) {
            fprintf(stderr, "     %s\t%s\n", g_devices[i].serial, g_devices[i].state);
        }
        return NULL;
    }
    return &g_devices[0];
}

static int cmd_devices(void)
{
    devices_load();
    printf("List of devices attached\n");
    for (int i = 0; i < g_device_count; i++) {
        vp_device* d = &g_devices[i];
        /* adb's state, unchanged. Whether this phone's host is listening is not
         * a device state and must not be dressed up as one: `vp shell` starts
         * the host on demand, the way `adb shell` starts a service, so "not
         * listening yet" is the normal state of a phone nobody has typed at.
         * Reporting it as adb's `offline` - which means the transport is broken -
         * made a perfectly good phone look dead, and sent the reader off to
         * install an APK that was already fine. The fact is still one `-l`
         * away, as `host:` on the detail line, where it can inform without
         * overruling adb. */
        const char* state = d->state;
        if (g_long) {
            char host[32];
            if (strcmp(d->state, "device")) {
                snprintf(host, sizeof(host), "host:unknown");
            } else {
                snprintf(host, sizeof(host), "host:%s",
                         host_listening(d->serial) ? "up" : "asleep");
            }
            printf("%s\t%s %s %s\n", d->serial, state, d->detail, host);
        } else {
            printf("%s\t%s\n", d->serial, state);
        }
    }
    printf("\n");
    return 0;
}

static int cmd_get_state(void)
{
    devices_load();
    vp_device* d = pick_device();
    if (!d) {
        return 1;
    }
    /* adb's state, unchanged - the same reasoning as cmd_devices: a phone with
     * no host up yet is a phone `vp shell` will start one on, not a broken
     * transport. */
    printf("%s\n", d->state);
    return 0;
}

static int cmd_get_serialno(void)
{
    devices_load();
    vp_device* d = pick_device();
    if (!d) {
        return 1;
    }
    printf("%s\n", d->serial);
    return 0;
}

static int cmd_wait_for_device(void)
{
    for (;;) {
        devices_load();
        for (int i = 0; i < g_device_count; i++) {
            if (g_serial && strcmp(g_devices[i].serial, g_serial)) {
                continue;
            }
            /* adb's condition: the transport is up. Not "a host is listening" -
             * that is what the next command starts, so waiting for it here
             * would be waiting for the caller's own next step. */
            if (!strcmp(g_devices[i].state, "device")) {
                return 0;
            }
        }
        Sleep(500);
    }
}

static int cmd_forward(int argc, char** argv)
{
    devices_load();
    if (!pick_device()) {
        return 1;
    }
    int  extra = 0;
    bool remove = false;
    if (argc && !strcmp(argv[0], "--remove")) {
        remove = true;
        argv++;
        argc--;
    }
    if (argc && !strcmp(argv[0], "--list")) {
        char* q[] = { (char*)"forward", (char*)"--list", NULL };
        return run_adb(g_serial, q, NULL, 0) == 0 ? 0 : 1;
    }
    if (argc < 1 || argc > 2) {
        fprintf(stderr,
                "usage: vp [-s SERIAL] forward LOCAL [REMOTE]\n"
                "       vp [-s SERIAL] forward --list\n"
                "       vp [-s SERIAL] forward --remove LOCAL\n"
                "  LOCAL and REMOTE are tcp:PORT; a bare number is read as that.\n");
        return 2;
    }
    /* Bare numbers become tcp:N, and a missing REMOTE becomes LOCAL. adb wants
     * both ends spelled out and rejects one; anything adb accepts still means
     * the same thing here. */
    char  local[32], remote[32];
    const char* lp = argv[0];
    const char* rp = argc > 1 ? argv[1] : argv[0];
    if (strspn(lp, "0123456789") == strlen(lp) && *lp) {
        snprintf(local, sizeof(local), "tcp:%s", lp);
    } else {
        snprintf(local, sizeof(local), "%s", lp);
    }
    if (strspn(rp, "0123456789") == strlen(rp) && *rp) {
        snprintf(remote, sizeof(remote), "tcp:%s", rp);
    } else {
        snprintf(remote, sizeof(remote), "%s", rp);
    }
    (void)extra;
    char* args[5];
    int n = 0;
    args[n++] = (char*)"forward";
    if (remove) {
        args[n++] = (char*)"--remove";
    }
    args[n++] = local;
    if (!remove) {
        args[n++] = remote;
    }
    args[n] = NULL;
    return run_adb(g_serial, args, NULL, 0) == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* the console                                                          */
/* ------------------------------------------------------------------ */

static int console_fd = -1;

/* How long to keep knocking before saying nobody is home. The app the caller
 * just asked for has to cold-start - process, JNI, bundle unpack - before it
 * binds, and on a slow phone with a cold page cache that is seconds, not
 * milliseconds. Named rather than written inline because the failure message
 * quotes the budget back, and a number typed twice is a number that drifts. */
#define VP_CONNECT_TRIES   30
#define VP_CONNECT_WAIT_MS 300

static bool console_connect(void)
{
    char  spec[16];
    snprintf(spec, sizeof(spec), "tcp:%d", g_port);
    char* args[4];
    int n = 0;
    args[n++] = (char*)"forward";
    args[n++] = spec;
    args[n++] = spec;
    args[n] = NULL;
    char reply[512];
    if (run_adb(g_serial, args, reply, sizeof(reply)) != 0) {
        fprintf(stderr, "vp: adb forward %s failed%s%s\n", spec,
                reply[0] ? ": " : "", reply);
        return false;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = VP_AF_INET;
    sa.sin_port        = htons((uint16_t)g_port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");

    wsa_start();

    /* Connected, and nothing beyond that is required. A live console may well
     * have output waiting the instant a client attaches - the guest's prompt
     * was written before anyone connected - so data on connect is the normal
     * case, not a protocol error. What marks a dead end is the stream being
     * already over: `adb forward` accepts on the host before it reaches for the
     * device, and when the device has no listener the relay connects and then
     * ends immediately. Peeked at rather than read, so that byte still reaches
     * the caller.
     *
     * Retried, because the app the caller just started needs a moment to bind,
     * and bounded so a device that will never have a host still answers. */
    for (int attempt = 1; attempt <= VP_CONNECT_TRIES; attempt++) {
        int fd = socket(VP_AF_INET, VP_SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
            int ready = sock_wait(fd, VP_CONNECT_WAIT_MS);
            if (ready == 0) {
                console_fd = fd;
                return true;
            }
            char peek;
            int  got = recv(fd, &peek, 1, MSG_PEEK);
            if (got != 0) {
                console_fd = fd;
                return true;
            }
        }
        closesocket(fd);
        Sleep(VP_CONNECT_WAIT_MS);
    }
    /* Say what to do, because the fact alone is a dead end. "Nothing is
     * listening" names a symptom; every way of getting here has an action, and
     * the two common ones - no app installed, or an app too old to have a
     * console at all - look identical from the socket. The app is checked
     * rather than guessed at, and its absence is the answer, because that is
     * the case where no amount of waiting helps. */
    char* pm[5] = { (char*)"shell", (char*)"pm", (char*)"list",
                    (char*)"packages", (char*)"com.rvvm.android" };
    char listed[1024];
    if (run_adb(g_serial, pm, listed, sizeof(listed)) == 0 &&
        !strstr(listed, "com.rvvm.android")) {
        fprintf(stderr,
                "vp: %s has no com.rvvm.android - the host app is not installed.\n"
                "    Install it (adb -s %s install app-debug.apk), or point -P at\n"
                "    a port it is already listening on.\n",
                g_serial ? g_serial : "the device", g_serial ? g_serial : "SERIAL");
    } else {
        fprintf(stderr,
                "vp: no console on %s after %d seconds. The app is installed but\n"
                "    nothing bound port %d - usually an APK older than the console\n"
                "    feature. Reinstall it, or check logcat:\n"
                "      adb -s %s logcat -s RVVM-JNI RVVM-RvvmHost\n",
                g_serial ? g_serial : "the device",
                (VP_CONNECT_TRIES * VP_CONNECT_WAIT_MS) / 1000, g_port,
                g_serial ? g_serial : "SERIAL");
    }
    return false;
}

static bool send_packet(int id, const void* data, size_t len)
{
    uint8_t hdr[VP_HDR_BYTES];
    hdr[0] = (uint8_t)id;
    hdr[1] = (uint8_t)(len & 0xff);
    hdr[2] = (uint8_t)((len >> 8) & 0xff);
    hdr[3] = (uint8_t)((len >> 16) & 0xff);
    hdr[4] = (uint8_t)((len >> 24) & 0xff);
    if (send(console_fd, (const char*)hdr, (int)sizeof(hdr), 0) < 0) {
        return false;
    }
    return !len || send(console_fd, (const char*)data, (int)len, 0) >= 0;
}

/* One packet. Returns the id, or -1 at end of stream, or -2 on a short read. */
static int recv_packet(char** out, size_t* outlen);

/* Wait for the console to say it is ready, and only then let the caller type.
 *
 * This is the whole handshake, and it is a gate rather than a greeting: the
 * client types nothing until this arrives, so the driver's script cannot be
 * queued in front of a prompt that has not been printed yet. That was the
 * ordering bug - the shell asks where the cursor is, goes to read the answer,
 * and finds the script instead, and eats it as a malformed report.
 *
 * The console sends READY just behind the guest's first output, not first on
 * the wire, so anything before it is the guest already talking: handed to the
 * caller's output side rather than discarded, or `exec-out` would lose the
 * first line of every command. */
static bool await_ready(void)
{
    for (;;) {
        int r = sock_wait(console_fd, 15000);
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            fprintf(stderr, "vp: the console never said it was ready\n");
            return false;
        }
        char*  body = NULL;
        size_t blen = 0;
        int    id   = recv_packet(&body, &blen);
        if (id == VP_ID_READY) {
            free(body);
            return true;
        }
        if (id < 0) {
            free(body);
            fprintf(stderr, "vp: the connection closed before the console was ready\n");
            return false;
        }
        if (id == VP_ID_STDOUT || id == VP_ID_STDERR) {
            fwrite(body ? body : "", 1, blen, stdout);
            fflush(stdout);
        }
        free(body);
    }
}

static int recv_packet(char** out, size_t* outlen)
{
    uint8_t hdr[VP_HDR_BYTES];
    size_t  got = 0;
    while (got < sizeof(hdr)) {
        long n = recv(console_fd, (char*)hdr + got, (int)(sizeof(hdr) - got), 0);
        if (n <= 0) {
            return (n == 0) ? -1 : -2;
        }
        got += (size_t)n;
    }
    uint32_t len = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) |
                   ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
    char*  body = NULL;
    if (len) {
        /* A guest write is bounded by what the program wrote in one call, so
         * this is not an arbitrary buffer. */
        body = (char*)malloc(len + 1);
        if (!body) {
            return -2;
        }
        size_t have = 0;
        while (have < len) {
            long n = recv(console_fd, body + have, (int)(len - have), 0);
            if (n <= 0) {
                free(body);
                return -1;
            }
            have += (size_t)n;
        }
        body[len] = '\0';
    }
    *out    = body;
    *outlen = len;
    return hdr[0];
}

static void send_winsize(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return;
    }
    /* The visible window, not the whole buffer: a terminal's size is what the
     * user can see, and the guest's TIOCGWINSZ is meant to match that. */
    int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    int cols = info.srWindow.Right - info.srWindow.Left + 1;
    char buf[32];
    int  n = snprintf(buf, sizeof(buf), "%d:%d", rows, cols);
    send_packet(VP_ID_WINDOWSIZE, buf, (size_t)n);
}

/* One intent, and the order inside it is the whole point. The guest name and
 * its argv ride along with `console=true`, and the host holds them until a
 * client is actually on the socket - see SimpleLauncherActivity.onConsoleClient.
 *
 * The earlier arrangement was two `am start`s: console host, connect, then
 * guest. That put a race in the client, because "the listener is bound" is not
 * "a client is here" and the only way to tell them apart from outside was to
 * wait and hope. Handing the run to the app in the same intent that asks for
 * the console moves the decision to the one place that knows the answer. */
/* Boot the core and connect to it, rather than running a one-shot guest.
 *
 * The two are different machines, and which one you want is the whole question
 * `vp shell` cannot answer on its own:
 *
 *   - a one-shot guest is a *command*. `am start` names an app, a run boots it,
 *     it answers and the machine is torn down by on_guest_exit. Two such
 *     commands are two machines, which is why they can never see each other.
 *   - a core is a *machine that outlives its clients*. Its run root is /sbin/idle
 *     - a program that holds the machine open and forks a shell per client - and
 *     every client gets a pty out of that one machine's pool, so `ps` in one
 *     session names every other session's processes.
 *
 * So the flag is not a mode of the same thing, it is a different thing, and the
 * app name is not used: idle is a system program with no manifest, named here by
 * guest path. */
static bool boot_core(const char* serial, const char* cmd)
{
    char* gs[12];
    int   gn = 0;
    gs[gn++] = (char*)"shell";
    gs[gn++] = (char*)"am";
    gs[gn++] = (char*)"start";
    gs[gn++] = (char*)"-n";
    gs[gn++] = (char*)"com.rvvm.android/.SimpleLauncherActivity";
    gs[gn++] = (char*)"--es";
    gs[gn++] = (char*)"guest_path";
    gs[gn++] = (char*)VP_CORE_ROOT;
    gs[gn++] = (char*)"--ez";
    gs[gn++] = (char*)"core";
    gs[gn++] = (char*)"true";
    gs[gn++] = (char*)"--ez";
    gs[gn++] = (char*)"console";
    gs[gn++] = (char*)"true";
    gs[gn] = NULL;
    char told[512];
    if (run_adb(serial, gs, told, sizeof(told)) != 0) {
        fprintf(stderr, "vp: could not start the core%s%s\n",
                told[0] ? ": " : "", told);
        return false;
    }
    /* The core is up by the time this returns if one was already running, and
     * otherwise the connect below is what starts it: the run is held until a
     * client is on the socket, which is the point of holding it. So there is no
     * wait here - connecting *is* the start. */
    if (!console_connect() || !await_ready()) {
        return false;
    }
    /* Typed after READY, never before. The session's shell issues a cursor
     * query as soon as it has a terminal, and anything sent ahead of its prompt
     * lands in the same read as the answer to that query - where a shell reads it
     * as a malformed report and drops it. Awaity_ready() is the gate that makes
     * this safe, and it is the reason a scripted command works on a core at all. */
    if (cmd && *cmd) {
        char line[2048];
        int  n = snprintf(line, sizeof(line), "%s\n", cmd);
        if (n > 0 && (size_t)n < sizeof(line)) {
            send_packet(VP_ID_STDIN, line, (size_t)n);
        }
    }
    return true;
}

static bool boot_guest(const char* serial, const char* app, const char* cmd)
{
    /* argv travels base64'd, NUL-joined: `--esa` is a multi-value option and
     * multi-value options are where shells disagree (a ColorOS build takes one
     * value and reads a leading dash as the next option). */
    char  blob[4096];
    blob[0] = '\0';
    if (cmd && *cmd) {
        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        /* -c, the command, NUL - which is what the Activity splits on */
        char   joined[2048];
        size_t jn = 0;
        jn += (size_t)snprintf(joined + jn, sizeof(joined) - jn, "-c");
        joined[jn++] = '\0';
        size_t clen = strlen(cmd);
        if (jn + clen >= sizeof(joined)) {
            clen = sizeof(joined) - jn - 1;
        }
        memcpy(joined + jn, cmd, clen);
        jn += clen;
        joined[jn++] = '\0';

        size_t nb = 0;
        for (size_t i = 0; i < jn; i += 3) {
            uint32_t v = (uint32_t)(uint8_t)joined[i] << 16;
            if (i + 1 < jn) v |= (uint32_t)(uint8_t)joined[i + 1] << 8;
            if (i + 2 < jn) v |= (uint32_t)(uint8_t)joined[i + 2];
            blob[nb++] = b64[(v >> 18) & 0x3f];
            blob[nb++] = b64[(v >> 12) & 0x3f];
            blob[nb++] = (i + 1 < jn) ? b64[(v >> 6) & 0x3f] : '=';
            blob[nb++] = (i + 2 < jn) ? b64[v & 0x3f] : '=';
        }
        blob[nb] = '\0';
    }

    char* gs[16];
    int   gn = 0;
    gs[gn++] = (char*)"shell";
    gs[gn++] = (char*)"am";
    gs[gn++] = (char*)"start";
    gs[gn++] = (char*)"-n";
    gs[gn++] = (char*)"com.rvvm.android/.SimpleLauncherActivity";
    gs[gn++] = (char*)"--es";
    gs[gn++] = (char*)"guest_app_name";
    gs[gn++] = (char*)(app ? app : VP_DEFAULT_GUEST);
    gs[gn++] = (char*)"--ez";
    gs[gn++] = (char*)"console";
    gs[gn++] = (char*)"true";
    if (blob[0]) {
        gs[gn++] = (char*)"--es";
        gs[gn++] = (char*)"argv_b64";
        gs[gn++] = (char*)blob;
    }
    gs[gn] = NULL;
    /* `am start` narrates the Intent it delivered. That is not the guest's
     * output and must not land in the middle of it - `vp exec-out` stdout is
     * the command's stdout, nothing else. It is kept for the failure path,
     * where "Warning: Activity not started" is the whole diagnosis. */
    char told[512];
    if (run_adb(serial, gs, told, sizeof(told)) != 0) {
        fprintf(stderr, "vp: could not start the guest%s%s\n",
                told[0] ? ": " : "", told);
        return false;
    }
    return console_connect() && await_ready();
}

/* ------------------------------------------------------------------ */
/* shell                                                                */
/* ------------------------------------------------------------------ */

/* Keys the console reports as a virtual key with no ASCII character, spelled
 * out here because in raw mode there is no line editor to turn them into
 * something. These are the sequences a terminal would have sent, which is the
 * whole point: the guest's shell expects the bytes a local terminal produces,
 * not Windows key codes. */
static const char* vt_for_vkey(WORD vk)
{
    switch (vk) {
    case VK_UP:     return "\x1b[A";
    case VK_DOWN:   return "\x1b[B";
    case VK_RIGHT:  return "\x1b[C";
    case VK_LEFT:   return "\x1b[D";
    case VK_HOME:   return "\x1b[H";
    case VK_END:    return "\x1b[F";
    case VK_INSERT: return "\x1b[2~";
    case VK_DELETE: return "\x1b[3~";
    case VK_PRIOR:  return "\x1b[5~";
    case VK_NEXT:   return "\x1b[6~";
    default:        return NULL;
    }
}

/* Reads the caller's console and forwards it, blocking.
 *
 * ReadConsoleInput rather than ReadFile because it is the only read that also
 * reports the window changing size - which is how the guest's TIOCGWINSZ
 * follows the terminal, with no polling thread to maintain it. A keypress the
 * console has no ASCII character for (the arrows, Home, End, ...) is spelled
 * out by vt_for_vkey, in the bytes a terminal would have produced, because the
 * guest's line editor expects those and not Windows key codes.
 *
 * Only reached when stdin really is a terminal; cmd_shell turns a piped stdin
 * away before getting here, because a script and the guest's own cursor query
 * cannot share one read. */
static DWORD WINAPI stdin_thread(LPVOID arg)
{
    HANDLE in = (HANDLE)arg;
    INPUT_RECORD rec[128];
    for (;;) {
        DWORD got = 0;
        if (!ReadConsoleInput(in, rec, sizeof(rec) / sizeof(rec[0]), &got) || !got) {
            return 0;
        }
        for (DWORD i = 0; i < got; i++) {
            if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                send_winsize();
            } else if (rec[i].EventType == KEY_EVENT &&
                       rec[i].Event.KeyEvent.bKeyDown) {
                char c = rec[i].Event.KeyEvent.uChar.AsciiChar;
                if (c) {
                    if (!send_packet(VP_ID_STDIN, &c, 1)) {
                        return 0;
                    }
                } else {
                    const char* seq = vt_for_vkey(rec[i].Event.KeyEvent.wVirtualKeyCode);
                    if (seq && !send_packet(VP_ID_STDIN, seq, strlen(seq))) {
                        return 0;
                    }
                }
            }
        }
    }
}

static int cmd_shell(int argc, char** argv)
{
    devices_load();
    vp_device* d = pick_device();
    if (!d) {
        return 1;
    }
    /* adb's `shell [CMD...]` is the command, and nothing else - there is no
     * slot in it for a guest name. Guessing which token is the app is how
     * `vp shell "echo hi"` ends up launching a guest called "echo hi" that
     * never exits, so the app comes from a flag and every remaining token is
     * part of the command. */
    const char* app = NULL;
    bool core = false;
    while (argc > 0 && argv[0][0] == '-') {
        if (!strcmp(argv[0], "--app") && argc > 1) {
            app = argv[1];
            argv += 2;
            argc -= 2;
        } else if (!strncmp(argv[0], "--app=", 6)) {
            app = argv[0] + 6;
            argv++;
            argc--;
        } else if (!strcmp(argv[0], "--core")) {
            /* One machine, one session per client. Anything typed here is run by
             * a shell of this session's own, and `ps` names the other sessions'
             * processes - which a one-shot guest cannot do, because each is its
             * own machine with its own /proc. */
            core = true;
            argv++;
            argc--;
        } else {
            fprintf(stderr, "vp shell: unknown option %s\n", argv[0]);
            return 2;
        }
    }
    if (core && app) {
        fprintf(stderr,
                "vp shell: --core and --app are different things.\n"
                "    --core is one machine with a session each; its shell is\n"
                "    the host's, not an app of yours.\n");
        return 2;
    }
    char cmd[2048];
    cmd[0] = '\0';
    for (int i = 0; i < argc; i++) {
        size_t at = strlen(cmd);
        snprintf(cmd + at, sizeof(cmd) - at, "%s%s", at ? " " : "", argv[i]);
    }
    /* A core is attached to, not booted into: the run is held until a client is
     * on the socket, so connecting is what starts it, and there is nothing to
     * run *at* - whatever tokens followed are the command to type, not an argv
     * for a guest. */
    bool ok = core ? boot_core(d->serial, cmd[0] ? cmd : NULL)
                   : boot_guest(d->serial, app, cmd[0] ? cmd : NULL);
    if (!ok) {
        return 1;
    }

    /* Raw on this side, or the local line editor eats every keystroke before it
     * can be forwarded - which is the same reason adb puts a terminal into raw
     * mode for an interactive shell. Whether stdin is a terminal is also what
     * decides whether this is an interactive session at all - a pipe is turned
     * away below, before any mode is touched. */
    HANDLE in  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  old_in = 0, old_out = 0;
    bool   raw = GetConsoleMode(in, &old_in) && GetConsoleMode(out, &old_out);
    if (raw) {
        SetConsoleMode(in, (old_in & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                       ENABLE_PROCESSED_INPUT)) | ENABLE_EXTENDED_FLAGS);
        SetConsoleMode(out, old_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    send_winsize();

    /* One thread per direction, each in a blocking read, and no poll loop.
     * The socket side is the caller's; the keyboard side is detached when the
     * guest exits rather than cancelled, because a thread parked in
     * ReadConsoleInput cannot be interrupted and a console input record is a
     * rude way to unblock it. The process is about to exit anyway. */
    HANDLE kt = NULL;
    if (!raw) {
        /* Not a terminal, so nobody is typing: this is a driver feeding bytes,
         * and `shell` is the wrong verb for it. The console is a live pipe
         * carrying one stream in both directions at once, and a guest shell
         * spends its first read waiting for the answer to its own cursor
         * query - a script's first line lands in the same read and is eaten as
         * a malformed reply. That is not a transport bug and no amount of
         * ordering on this side fixes it; the script and the query want
         * different things at the same instant. A human at a terminal does not
         * hit it, because nobody types before the prompt.
         *
         * So: say so, and point at the verb that is built for it. */
        fprintf(stderr,
                "vp shell: stdin is not a terminal, so there is nobody typing.\n"
                "         For a script use `vp exec-out CMD`, which runs the\n"
                "         command and hands back its output and its exit code.\n");
        if (console_fd >= 0) {
            closesocket(console_fd);
            console_fd = -1;
        }
        return 2;
    }
    /* A person at a terminal. The keyboard is read on its own thread in a
     * blocking ReadConsoleInput - one thread per direction, no poll loop - and
     * detached rather than cancelled when the guest exits, because a thread
     * parked in ReadConsoleInput cannot be interrupted. The process is about to
     * exit anyway. */
    kt = CreateThread(NULL, 0, stdin_thread, in, 0, NULL);

    char* body = NULL;
    size_t blen = 0;
    int    exit_code = 0;

    for (;;) {
        int id = recv_packet(&body, &blen);
        if (id < 0) {
            break;
        }
        if (id == VP_ID_STDOUT || id == VP_ID_STDERR) {
            /* Both to the terminal: a terminal shows both, and splitting them
             * here would mean deciding which one a person wants to see.
             * `exec-out` is the one that keeps them apart. */
            DWORD put = 0;
            WriteFile(out, body ? body : "", (DWORD)blen, &put, NULL);
        } else if (id == VP_ID_EXIT) {
            exit_code = body ? atoi(body) : 0;
            /* Drain what the guest wrote before it exited. Its thread handed
             * every byte to the writer before reaching the exit callback, so
             * the packets already queued here are the tail of its output and
             * dropping them would truncate a transcript that is complete. */
            while (sock_wait(console_fd, 0) > 0) {
                int more = recv_packet(&body, &blen);
                if (more < 0) {
                    break;
                }
                if (more == VP_ID_STDOUT || more == VP_ID_STDERR) {
                    DWORD put = 0;
                    WriteFile(out, body ? body : "", (DWORD)blen, &put, NULL);
                }
                free(body);
                body = NULL;
            }
            break;
        }
        free(body);
        body = NULL;
    }
    free(body);

    /* No CLOSESTDIN here on purpose. Whether the guest's stdin is over is not
     * this program's decision - the driver may have closed its own stdin, or
     * may have typed nothing at all, and either way the console already knows
     * when the connection is gone. It is the console that owns the guest, so
     * it is the console that ends the guest's input, and it ends it on every
     * path (clean exit, a client killed mid-session, a client dropped for
     * falling behind) rather than only the one this code could reach. */

    if (kt) {
        CloseHandle(kt);
    }
    if (raw) {
        SetConsoleMode(in, old_in);
        SetConsoleMode(out, old_out);
    }
    closesocket(console_fd);
    console_fd = -1;
    return exit_code;
}

static int cmd_exec_out(int argc, char** argv)
{
    bool core = false;
    /* `--core` first, so it can be spelled before the command. It is a flag of
     * this verb and not a global, exactly like `devices -l`. */
    while (argc > 0 && argv[0][0] == '-' && argv[0][1]) {
        if (!strcmp(argv[0], "--core")) {
            core = true;
            argv++;
            argc--;
        } else {
            break;
        }
    }
    if (argc < 1) {
        fprintf(stderr, core ? "usage: vp exec-out --core COMMAND\n"
                             : "usage: vp exec-out COMMAND [APP]\n");
        return 1;
    }
    devices_load();
    vp_device* d = pick_device();
    if (!d) {
        return 1;
    }
    /* With a core the command is run by that session's own shell, so the app
     * slot does not apply: there is no guest to name, and the point is that the
     * session is one of several in the same machine. */
    const char* app = (!core && argc > 1) ? argv[1] : NULL;
    bool ok = core ? boot_core(d->serial, argv[0]) : boot_guest(d->serial, app, argv[0]);
    if (!ok) {
        return 1;
    }
    if (core) {
        /* A core's shell is interactive and does not exit with the command, so
         * there is no exit packet coming and no exit code to report. Typed into
         * a session like any other input, and the transcript ends when the
         * connection does. */
        char* body = NULL;
        size_t blen = 0;
        for (;;) {
            int id = recv_packet(&body, &blen);
            if (id < 0) {
                break;
            }
            if (id == VP_ID_STDOUT || id == VP_ID_STDERR) {
                fwrite(body ? body : "", 1, blen, stdout);
                fflush(stdout);
            }
            free(body);
            body = NULL;
        }
        free(body);
        closesocket(console_fd);
        console_fd = -1;
        return 0;
    }
    char* body = NULL;
    size_t blen = 0;
    int    exit_code = 0;
    for (;;) {
        int id = recv_packet(&body, &blen);
        if (id < 0) {
            break;
        }
        if (id == VP_ID_STDOUT || id == VP_ID_STDERR) {
            /* Raw to our own stdout, which is what exec-out means: the
             * command's output and nothing else, so it can be redirected. */
            fwrite(body ? body : "", 1, blen, stdout);
            fflush(stdout);
        } else if (id == VP_ID_EXIT) {
            exit_code = body ? atoi(body) : 0;
            while (sock_wait(console_fd, 0) > 0) {
                int more = recv_packet(&body, &blen);
                if (more < 0) {
                    break;
                }
                if (more == VP_ID_STDOUT || more == VP_ID_STDERR) {
                    fwrite(body ? body : "", 1, blen, stdout);
                }
            }
            break;
        }
        free(body);
        body = NULL;
    }
    free(body);
    closesocket(console_fd);
    console_fd = -1;
    return exit_code;
}

/* ------------------------------------------------------------------ */

static void usage(FILE* f)
{
    fprintf(f,
        "vp - a client for the Android host's guest console, shaped like adb\n"
        "\n"
        "usage: vp [-s SERIAL] COMMAND [args]\n"
        "\n"
        "  devices [-l]        attached devices, adb's states verbatim; -l adds\n"
        "                      host:up / host:asleep (whether a console is up -\n"
        "                      `shell` starts one, so it is not a device state)\n"
        "  get-state           adb's state for the selected device\n"
        "  get-serialno        the serial of the selected device\n"
        "  wait-for-device     block until one is attached\n"
        "  shell [CMD...]     an interactive shell on the guest; needs a real\n"
        "                      terminal, since a piped stdin is a script and\n"
        "                      belongs in exec-out. --app APP picks the guest\n"
        "  exec-out CMD [APP] run CMD in the guest, raw output, its exit code\n"
        "                      (APP defaults to %s)\n"
        "  forward L R         adb forward, for tools that speak the raw pipe\n"
        "  version\n"
        "\n"
        "The global options are adb's: -s SERIAL picks the device, -P PORT\n"
        "picks the console port (%d by default).\n"
        "\n"
        "The guest's console is a byte pipe framed as adb frames a shell, so\n"
        "`adb forward` plus a client that speaks that framing also works.\n",
        VP_DEFAULT_GUEST, VP_DEFAULT_PORT);
}

int main(int argc, char** argv)
{
    /* stderr is unbuffered, whatever it is pointed at. A client that is killed
     * - which is what a timeout does to one - otherwise takes its own last
     * words with it, fully buffered in a pipe that never gets flushed, and the
     * reason it was killed is exactly what got lost. */
    setvbuf(stderr, NULL, _IONBF, 0);

    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            g_serial = argv[++i];
        } else if (!strcmp(argv[i], "-P") && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            break;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return 1;
    }
    const char* cmd = argv[i++];
    int rest_argc = argc - i;
    char** rest_argv = argv + i;

    if (!strcmp(cmd, "devices")) {
        /* -l belongs to the verb, not the globals, so it is taken here rather
         * than in the option loop above: adb spells it `adb devices -l`, and the
         * global loop would never see it. It used to be read from a flag that
         * nothing ever set, so `devices -l` quietly printed the short form. */
        for (int k = 0; k < rest_argc; k++) {
            if (!strcmp(rest_argv[k], "-l")) {
                g_long = 1;
            }
        }
        return cmd_devices();
    }
    if (!strcmp(cmd, "get-state"))      return cmd_get_state();
    if (!strcmp(cmd, "get-serialno"))   return cmd_get_serialno();
    if (!strcmp(cmd, "wait-for-device")) return cmd_wait_for_device();
    if (!strcmp(cmd, "shell"))          return cmd_shell(rest_argc, rest_argv);
    if (!strcmp(cmd, "exec-out"))       return cmd_exec_out(rest_argc, rest_argv);
    if (!strcmp(cmd, "forward"))        return cmd_forward(rest_argc, rest_argv);
    if (!strcmp(cmd, "version")) {
        printf("vp version %s", VP_VERSION);
        printf("\n");
        return 0;
    }
    if (!strcmp(cmd, "help")) {
        usage(stdout);
        return 0;
    }

    /* The verbs adb has and this does not, named so the miss is a message
     * rather than a shrug - and so nobody reaches for `vp install` expecting
     * it to mean something. */
    static const char* not_ours[] = {
        "install", "uninstall", "push", "pull", "root", "unroot", "reverse",
        "start-server", "kill-server", "logcat", "bugreport", "tcpip", "usb",
        "wait-for-recovery", "reboot", "shell-settings", "pairing", NULL,
    };
    for (int k = 0; not_ours[k]; k++) {
        if (!strcmp(cmd, not_ours[k])) {
            fprintf(stderr,
                "vp: '%s' is adb's, not vp's.\n"
                "    Everything on the device side stays adb's job - run\n"
                "    `adb %s`. vp is the guest's console.\n", cmd, cmd);
            return 1;
        }
    }

    fprintf(stderr, "vp: unknown command '%s' (try `vp help`)\n", cmd);
    return 1;
}
