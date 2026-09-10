/*
 * win32_main.c - Entry point of the RVVM Windows host skeleton.
 *
 * Usage:
 *   rvvm_winhost.exe <guest-elf> [guest args...]
 */

#include <stdio.h>
#include "win32_cmdpost_bridge.h"
#include "utils.h"

int main(int argc, char** argv)
{
    int rc;

    /* Verbose RVVM logging (syscall trace); toggle via env RVVM_VERBOSE=1 */
    rvvm_set_loglevel(getenv("RVVM_VERBOSE") ? LOG_INFO : LOG_WARN);

    if (argc < 2) {
        fprintf(stderr, "Usage: rvvm_winhost.exe <guest-elf> [guest args...]\n");
        return 2;
    }

    if (!win32_host_init("RVVM WinHost", 640, 480)) {
        fprintf(stderr, "Failed to initialize the Win32 host\n");
        return 1;
    }

    if (!win32_host_start_guest(argc - 1, &argv[1])) {
        fprintf(stderr, "Failed to launch the guest\n");
        win32_host_shutdown();
        return 1;
    }

    rc = win32_host_message_loop();
    win32_host_shutdown();
    return rc;
}
