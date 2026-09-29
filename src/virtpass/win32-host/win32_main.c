/*
 * win32_main.c - Entry point of the RVVM Windows host skeleton.
 *
 * Usage:
 *   rvvm_winhost.exe [options] --launcher
 *   rvvm_winhost.exe [options] --app <id> [args...]
 *   rvvm_winhost.exe --help     (options + environment, exits)
 *
 * A guest is only ever run as an app *package*: --app names one from the
 * bundle (apps.tar.gz -> apps/<id>.vapp) and its entry point and arguments come
 * from the package's own manifest. There is no way to hand the host a loose ELF
 * path - the bundle and its packages are the only launch surface, which is what
 * keeps a run from being pointed at an arbitrary program.
 *
 * --launcher shows an Android-style picker: a dropdown listing the bundle's
 * apps plus Run / Stop / Exit.
 *
 * This is a console application: its stdout carries the guest's console output
 * and its stdin is pumped into the guest's console, so a run can be scripted
 * (`printf 'ls /\nexit\n' | rvvm_winhost.exe --app test_cli`) while the
 * window keyboard remains the interactive path.
 *
 * Two independent display layers:
 *   - Layer 2 (OS window): a fixed 1024x768 viewport by default. It is a pure
 *     presentation surface; the virtual panel is scaled uniformly to fit
 *     (contain, never cropped) and centred, with the leftover area filled
 *     black. Override with RVVM_WIN_W / RVVM_WIN_H.
 *   - Layer 1 (virtual display): the panel the guest renders into. It is a
 *     fixed 640x480 panel by default, independent of the window size.
 *     --display configures it: WxH in pixels and PPI as physical
 *     pixels-per-inch. Any part may be omitted, e.g. "1024x768", "@240" or
 *     "1024x768@240". The same values may be supplied through the RVVM_VIRT_W
 *     / RVVM_VIRT_H / RVVM_VIRT_PPI environment variables; --display wins over
 *     the environment.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h> /* MAX_PATH for the launcher assets dir */
#include "win32_cmdpost_bridge.h"
#include "virtpass/vp_rootfs.h" /* VP_GUEST_APP_DIR, the app guest path */
#include "utils.h"

/* Layer-2 OS window default (the presentation viewport). */
#define WIN_DEF_W    1024
#define WIN_DEF_H    768

/* Layer-1 virtual display default geometry. Independent of the window size:
 * the guest renders into this 640x480 panel and the viewport scales it. */
#define VIRT_DEF_W   640
#define VIRT_DEF_H   480

/* Layer-1 virtual display default density (mdpi). */
#define VIRT_DEF_PPI 160

static int parse_positive(const char* s, int fallback)
{
    char* end = NULL;
    long v;

    if (!s || !*s) return fallback;
    v = strtol(s, &end, 10);
    if (end == s || v <= 0 || v > 65536) return fallback;
    return (int)v;
}

/* Accepts "WxH", "@PPI" or "WxH@PPI"; unspecified parts stay untouched. */
static void parse_display_spec(const char* spec, int* w, int* h, int* ppi)
{
    char buf[64];
    const char* at = strchr(spec, '@');
    size_t n = at ? (size_t)(at - spec) : strlen(spec);

    if (at)
        *ppi = parse_positive(at + 1, *ppi);

    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, spec, n);
    buf[n] = '\0';

    {
        char* x = strchr(buf, 'x');
        if (!x) x = strchr(buf, 'X');
        if (x) {
            *x = '\0';
            *w = parse_positive(buf, *w);
            *h = parse_positive(x + 1, *h);
        }
    }
}

/* Usage text, also shown by --help / -h. */
static void print_help(const char* prog)
{
    printf(
        "RVVM WinHost - RISC-V Linux userland emulator with a Win32 surface\n"
        "\n"
        "Usage:\n"
        "  %s [options] --launcher\n"
        "  %s [options] --app <id> [args...]\n"
        "\n"
        "Options:\n"
        "  --display SPEC   Layer-1 virtual panel geometry: \"WxH\", \"@PPI\" or\n"
        "                   \"WxH@PPI\" (default 640x480@160; env RVVM_VIRT_W/H/PPI)\n"
        "  --app ID         Boot an app package from the bundle (apps.tar.gz ->\n"
        "                   apps/<id>.vapp); its entry and args come from the\n"
        "                   package, not the command line - the only way to run\n"
        "  --assets DIR     Tree the guest's AAssetManager_* reads (env RVVM_ASSETS)\n"
        "  --help, -h       Show this help and exit\n"
        "\n"
        "Environment:\n"
        "  RVVM_WIN_W, RVVM_WIN_H       Layer-2 OS window size (default 1024x768)\n"
        "  RVVM_VIRT_W, RVVM_VIRT_H     Layer-1 virtual panel size (default 640x480)\n"
        "  RVVM_VIRT_PPI                Virtual panel density, PPI (default 160)\n"
        "  RVVM_ASSETS                  Asset tree for AAssetManager_*\n"
        "  RVVM_VERBOSE=1               Verbose logging (syscall trace) on stderr\n"
        "  RVVM_TRACE                   Trace categories (e.g. job, all)\n"
        "  RVVM_LOG_FILE=<path>         Append every host log line to a file\n"
        "  RVVM_LOG_RING_DUMP=<path>    Write the log ring (last 128 KiB) at exit\n"
        "  RVVM_GL_BACKEND              angle (default) | swiftshader | off\n"
        "  RVVM_GL_DLL_DIR              Directory holding libEGL.dll/libGLESv2.dll\n"
        "                               (or <sdk>\\emulator\\lib64\\gles_<name>)\n"
        "  RVVM_GL_TRACE=1              Trace every GL/EGL call on stderr\n"
        "\n"
        "Console: the guest's output goes to this process's stdout and this\n"
        "process's stdin is fed to the guest's console, so a run can be scripted:\n"
        "  printf 'ls /\\nexit\\n' | %s --app test_cli\n"
        "\n"
        "Host diagnostics go to stderr, never to stdout - which is the guest's\n"
        "transcript, and a driver reading it expects to find the guest's own\n"
        "output there and nothing else.\n"
        "\n"
        "Exit status: the guest's exit code (1 on a host error); in launcher mode\n"
        "the window stays open across guests.\n",
        prog, prog, prog);
}

int main(int argc, char** argv)
{
    int rc, i;
    int win_w = WIN_DEF_W, win_h = WIN_DEF_H;
    int virt_w = VIRT_DEF_W, virt_h = VIRT_DEF_H, virt_ppi = VIRT_DEF_PPI;
    char assets_dir[MAX_PATH];
    const char* assets_env = getenv("RVVM_ASSETS");
    const char* app_id = NULL;
    bool        launcher = false;

    /* Verbose RVVM logging (syscall trace); toggle via env RVVM_VERBOSE=1 */
    rvvm_set_loglevel(getenv("RVVM_VERBOSE") ? LOG_INFO : LOG_WARN);

    /* Environment provides the layer-2 window and layer-1 panel defaults. */
    win_w    = parse_positive(getenv("RVVM_WIN_W"), win_w);
    win_h    = parse_positive(getenv("RVVM_WIN_H"), win_h);
    virt_w   = parse_positive(getenv("RVVM_VIRT_W"), virt_w);
    virt_h   = parse_positive(getenv("RVVM_VIRT_H"), virt_h);
    virt_ppi = parse_positive(getenv("RVVM_VIRT_PPI"), virt_ppi);

    /* The asset tree AAssetManager_* reads (--assets / RVVM_ASSETS). It has
     * nothing to do with launching: a guest is only ever an app package. */
    snprintf(assets_dir, sizeof(assets_dir), "%s",
             (assets_env && *assets_env) ? assets_env : "");

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            parse_display_spec(argv[++i], &virt_w, &virt_h, &virt_ppi);
            continue;
        }
        if (strncmp(argv[i], "--display=", 10) == 0) {
            parse_display_spec(argv[i] + 10, &virt_w, &virt_h, &virt_ppi);
            continue;
        }
        if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            snprintf(assets_dir, sizeof(assets_dir), "%s", argv[++i]);
            continue;
        }
        if (strncmp(argv[i], "--assets=", 9) == 0) {
            snprintf(assets_dir, sizeof(assets_dir), "%s", argv[i] + 9);
            continue;
        }
        if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
            app_id = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--app=", 6) == 0) {
            app_id = argv[i] + 6;
            continue;
        }
        if (strcmp(argv[i], "--launcher") == 0) {
            launcher = true;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
        break; /* first non-option: the app's extra arguments (only with --app) */
    }

    /* A guest only ever comes from a package. A bare path is refused, so a run
     * cannot be pointed at an arbitrary program. */
    if (!app_id && i < argc) {
        fprintf(stderr, "winhost: unexpected argument '%s' (a guest is named with --app)\n",
                argv[i]);
        fprintf(stderr, "run '%s --help' for usage\n", argv[0]);
        return 2;
    }
    if (launcher && app_id) {
        fprintf(stderr, "winhost: --launcher and --app are exclusive\n");
        return 2;
    }

    if (!win32_host_init("RVVM WinHost", win_w, win_h, virt_w, virt_h, virt_ppi, !app_id)) {
        fprintf(stderr, "Failed to initialize the Win32 host\n");
        return 1;
    }

    /* The asset tree the guest's AAssetManager_* reads. Set before either launch
     * path, so both see the same tree. */
    win32_host_set_assets_dir(assets_dir);

    if (app_id) {
        /* The controlled launch: the id names a package, and everything about
         * the run - entry point, default args - comes from its manifest. Extra
         * command-line arguments are appended after the package's own. */
        vp_app_t app;
        char     entry[VP_APP_ID_MAX + VP_APP_ENTRY_MAX + 16];
        char*    guest[8];
        int      n = 0;

        if (!win32_host_app_manifest(app_id, &app)) {
            fprintf(stderr, "winhost: no app package '%s' in the bundle\n", app_id);
            win32_host_shutdown();
            return 1;
        }
        if (snprintf(entry, sizeof(entry), "%s/%s/%s", VP_GUEST_APP_DIR, app.id, app.entry) >=
            (int)sizeof(entry)) {
            fprintf(stderr, "winhost: app '%s' has an unusable entry\n", app_id);
            win32_host_shutdown();
            return 1;
        }
        guest[n++] = entry;
        if (app.args[0]) {
            guest[n++] = app.args;
        }
        for (; i < argc && n < (int)(sizeof(guest) / sizeof(guest[0])) - 1; i++) {
            guest[n++] = argv[i];
        }
        if (!win32_host_start_guest(n, guest)) {
            fprintf(stderr, "Failed to launch the app '%s'\n", app_id);
            win32_host_shutdown();
            return 1;
        }
    } else {
        /* No app: the Android-style picker. It manages launches (Run/Stop) and
         * keeps the window open across them. */
        win32_host_set_launcher(assets_dir);
    }

    rc = win32_host_message_loop();
    win32_host_shutdown();
    return rc;
}
