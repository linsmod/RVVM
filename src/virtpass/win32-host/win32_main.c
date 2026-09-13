/*
 * win32_main.c - Entry point of the RVVM Windows host skeleton.
 *
 * Usage:
 *   rvvm_winhost.exe [--display WxH@PPI] [--assets DIR] <guest-elf> [guest args...]
 *   rvvm_winhost.exe            (no guest: Android-style launcher picker)
 *   rvvm_winhost.exe --help     (options + environment, exits)
 *
 * With no guest given, an Android-style launcher picker is shown: a dropdown
 * listing the guest programs in the assets directory plus Run / Stop / Exit.
 * --assets points at that directory (default: src\virtpass\android-host\app\
 * src\main\assets, overridable via the RVVM_ASSETS environment variable).
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
        "  %s [options] <guest-elf> [guest args...]\n"
        "  %s                (no guest: show the Android-style launcher picker)\n"
        "  %s <assets-dir>   (a directory is taken as the picker's guest folder)\n"
        "\n"
        "Options:\n"
        "  --display SPEC   Layer-1 virtual panel geometry: \"WxH\", \"@PPI\" or\n"
        "                   \"WxH@PPI\" (default 640x480@160; env RVVM_VIRT_W/H/PPI)\n"
        "  --assets DIR     Directory the launcher picker lists guests from\n"
        "                   (default src\\virtpass\\android-host\\app\\src\\main\\assets;\n"
        "                   env RVVM_ASSETS)\n"
        "  --help, -h       Show this help and exit\n"
        "\n"
        "Environment:\n"
        "  RVVM_WIN_W, RVVM_WIN_H       Layer-2 OS window size (default 1024x768)\n"
        "  RVVM_VIRT_W, RVVM_VIRT_H     Layer-1 virtual panel size (default 640x480)\n"
        "  RVVM_VIRT_PPI                Virtual panel density, PPI (default 160)\n"
        "  RVVM_ASSETS                  Launcher guest directory\n"
        "  RVVM_USER_PREFIX             Directory the guest's absolute paths resolve\n"
        "                               against (default: host paths pass through\n"
        "                               unchanged)\n"
        "  RVVM_VERBOSE=1               Verbose logging (syscall trace)\n"
        "  RVVM_GL_BACKEND              angle (default) | swiftshader | off\n"
        "  RVVM_GL_DLL_DIR              Directory holding libEGL.dll/libGLESv2.dll\n"
        "                               (or <sdk>\\emulator\\lib64\\gles_<name>)\n"
        "  RVVM_GL_TRACE=1              Trace every GL/EGL call on stderr\n"
        "\n"
        "Exit status: the guest's exit code in direct mode (1 on a host error);\n"
        "in launcher mode the window stays open across guests.\n",
        prog, prog, prog);
}

int main(int argc, char** argv)
{
    int rc, i;
    int win_w = WIN_DEF_W, win_h = WIN_DEF_H;
    int virt_w = VIRT_DEF_W, virt_h = VIRT_DEF_H, virt_ppi = VIRT_DEF_PPI;
    char assets_dir[MAX_PATH];
    const char* assets_env = getenv("RVVM_ASSETS");

    /* Verbose RVVM logging (syscall trace); toggle via env RVVM_VERBOSE=1 */
    rvvm_set_loglevel(getenv("RVVM_VERBOSE") ? LOG_INFO : LOG_WARN);

    /* Environment provides the layer-2 window and layer-1 panel defaults. */
    win_w    = parse_positive(getenv("RVVM_WIN_W"), win_w);
    win_h    = parse_positive(getenv("RVVM_WIN_H"), win_h);
    virt_w   = parse_positive(getenv("RVVM_VIRT_W"), virt_w);
    virt_h   = parse_positive(getenv("RVVM_VIRT_H"), virt_h);
    virt_ppi = parse_positive(getenv("RVVM_VIRT_PPI"), virt_ppi);

    /* Default assets dir for the launcher picker (relative to CWD, which is
     * the repo root when launched from the build scripts). */
    if (assets_env && *assets_env) {
        snprintf(assets_dir, sizeof(assets_dir), "%s", assets_env);
    } else {
        snprintf(assets_dir, sizeof(assets_dir),
                 "src\\virtpass\\android-host\\app\\src\\main\\assets");
    }

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
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            /* Intercepted before the "first non-option" break below, which
             * would otherwise treat "--help" as the guest ELF path. */
            print_help(argv[0]);
            return 0;
        }
        break; /* first non-option argument: the guest ELF */
    }

    /* A directory is not a guest ELF. Handing over the assets folder is an easy
     * mistake to make (it is exactly what the Android host gets passed): rvopen()
     * cannot open a directory, so the guest fails to load, exits with -1 at once
     * and drags the window down with it. Treat it as the picker's guest folder
     * instead of killing the session. */
    if (i < argc) {
        DWORD attr = GetFileAttributesA(argv[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            snprintf(assets_dir, sizeof(assets_dir), "%s", argv[i]);
            printf("'%s' is a directory, not a guest ELF - showing the launcher picker\n",
                   argv[i]);
            i = argc; /* no guest: launcher mode, see below */
        }
    }

    if (!win32_host_init("RVVM WinHost", win_w, win_h, virt_w, virt_h, virt_ppi,
                         i >= argc)) {
        fprintf(stderr, "Failed to initialize the Win32 host\n");
        return 1;
    }

    if (i >= argc) {
        /* No guest given: show the Android-style picker instead of running a
         * specific guest. The launcher manages guest launches (Run/Stop) and
         * keeps the window open across them. */
        win32_host_set_launcher(assets_dir);
    } else if (!win32_host_start_guest(argc - i, &argv[i])) {
        fprintf(stderr, "Failed to launch the guest\n");
        win32_host_shutdown();
        return 1;
    }

    rc = win32_host_message_loop();
    win32_host_shutdown();
    return rc;
}
