/*
 * win32_main.c - Entry point of the RVVM Windows host skeleton.
 *
 * Usage:
 *   rvvm_winhost.exe [--display WxH@PPI] <guest-elf> [guest args...]
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

int main(int argc, char** argv)
{
    int rc, i;
    int win_w = WIN_DEF_W, win_h = WIN_DEF_H;
    int virt_w = VIRT_DEF_W, virt_h = VIRT_DEF_H, virt_ppi = VIRT_DEF_PPI;

    /* Verbose RVVM logging (syscall trace); toggle via env RVVM_VERBOSE=1 */
    rvvm_set_loglevel(getenv("RVVM_VERBOSE") ? LOG_INFO : LOG_WARN);

    /* Environment provides the layer-2 window and layer-1 panel defaults. */
    win_w    = parse_positive(getenv("RVVM_WIN_W"), win_w);
    win_h    = parse_positive(getenv("RVVM_WIN_H"), win_h);
    virt_w   = parse_positive(getenv("RVVM_VIRT_W"), virt_w);
    virt_h   = parse_positive(getenv("RVVM_VIRT_H"), virt_h);
    virt_ppi = parse_positive(getenv("RVVM_VIRT_PPI"), virt_ppi);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            parse_display_spec(argv[++i], &virt_w, &virt_h, &virt_ppi);
            continue;
        }
        if (strncmp(argv[i], "--display=", 10) == 0) {
            parse_display_spec(argv[i] + 10, &virt_w, &virt_h, &virt_ppi);
            continue;
        }
        break; /* first non-option argument: the guest ELF */
    }

    if (i >= argc) {
        fprintf(stderr,
                "Usage: rvvm_winhost.exe [--display WxH@PPI] <guest-elf> [guest args...]\n"
                "  window: %dx%d px; virtual display: %dx%d px @ %d ppi\n",
                WIN_DEF_W, WIN_DEF_H, VIRT_DEF_W, VIRT_DEF_H, VIRT_DEF_PPI);
        return 2;
    }

    if (!win32_host_init("RVVM WinHost", win_w, win_h, virt_w, virt_h, virt_ppi)) {
        fprintf(stderr, "Failed to initialize the Win32 host\n");
        return 1;
    }

    if (!win32_host_start_guest(argc - i, &argv[i])) {
        fprintf(stderr, "Failed to launch the guest\n");
        win32_host_shutdown();
        return 1;
    }

    rc = win32_host_message_loop();
    win32_host_shutdown();
    return rc;
}
