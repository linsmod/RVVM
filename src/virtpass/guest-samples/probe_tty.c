/*
 * probe_tty.c - print what /dev/tty actually resolves to, in a guest
 *
 * A diagnostic, not a test with a verdict: it has no pass/fail, because the
 * question it answers is "which device is /dev/tty right now", and the answer
 * is a number that only means something next to what the core thinks it is.
 *
 * It exists because two guest observations disagreed and neither of them said
 * which was wrong. `stat /dev/tty` reported rdev major 136 - the pty major -
 * while `tty` reported "not a tty" and the run root had issued no TIOCSCTTY
 * at all. Those cannot both describe the same descriptor, so the disagreement
 * is in the resolution path and neither reading locates it.
 *
 * So this walks the resolution by hand, printing every step:
 *
 *   1. stat("/dev/tty")            - what the kernel hands back
 *   2. isatty() on it              - whether the core considers it a terminal
 *   3. tcgetattr() on it           - and whether termios succeeds
 *   4. the same on the bare console (/dev/console), as the reference the
 *      first reading is supposed to match when nothing has been claimed
 *   5. an ioctl this core may intercept for a tty (TIOCGWINSZ), to see
 *      which handler answers
 *   6. /proc/self/stat's tty_nr, which the core fills from the *process*
 *      rather than from a descriptor - if this disagrees with (1) then the
 *      two read different sources and the fault is in one of them
 *
 * Then the same probe with a pty claimed (posix_openpt + grantpt + unlockpt +
 * setsid + TIOCSCTTY, reporting what TIOCSCTTY returned), which is what a
 * session server does and what this whole arrangement depends on.
 *
 * Usage: probe_tty
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static void probe(const char* label, int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) {
        printf("  %-22s fstat failed: %s\n", label, strerror(errno));
        return;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    int winsz = ioctl(fd, TIOCGWINSZ, &ws);
    struct termios t;
    int attrs = ioctl(fd, TCGETS, &t);

    printf("  %-22s rdev=%llu (major %llu minor %llu) isatty=%d winsz=%d%s%lux%u tcgets=%d\n",
           label,
           (unsigned long long)st.st_rdev,
           (unsigned long long)(st.st_rdev >> 8),
           (unsigned long long)(st.st_rdev & 0xFF),
           isatty(fd), winsz,
           winsz == 0 ? " " : "",
           (unsigned long)ws.ws_row, (unsigned long)ws.ws_col,
           attrs);
    if (attrs < 0) {
        printf("  %-22s   tcgets errno=%d (%s)\n", "", errno, strerror(errno));
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== /dev/tty resolution probe ===\n");

    /* Phase 1: nothing claimed. This is the state the job-control test calls
     * "with nothing claimed, /dev/tty is the run's console". */
    printf("\n[1] nothing claimed:\n");
    int tty = open("/dev/tty", O_RDWR);
    printf("  open(\"/dev/tty\") = %d (%s)\n", tty, tty < 0 ? strerror(errno) : "ok");
    if (tty >= 0) {
        probe("/dev/tty", tty);
        close(tty);
    }
    int con = open("/dev/console", O_RDWR);
    printf("  open(\"/dev/console\") = %d (%s)\n", con, con < 0 ? strerror(errno) : "ok");
    if (con >= 0) {
        probe("/dev/console", con);
        close(con);
    }

    /* Phase 2: the run's own fd 0, for a third reading of the same question. */
    printf("\n[2] the run's fd 0:\n");
    probe("fd 0", 0);
    {
        FILE* f = fopen("/proc/self/stat", "r");
        if (f) {
            char buf[512];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            fclose(f);
            /* tty_nr is the 7th field, and it is what shells read to decide
             * whether they have a terminal at all. */
            char* p = strrchr(buf, ')');
            if (p) {
                int tty_nr = -999;
                if (sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %d", &tty_nr) == 1) {
                    printf("  /proc/self/stat tty_nr = %d (0 = none)\n", tty_nr);
                } else {
                    printf("  /proc/self/stat: could not read tty_nr\n");
                }
            }
        }
    }

    /* Phase 3: what a session does. The order is the whole point, and it is
     * the order vpsessiond uses: setsid() *first*, which drops whatever
     * terminal was inherited, and only then the claim.
     *
     * Probing the inherited state first is what makes this worth printing: a
     * process run as a session's command is not in the neutral state its
     * parent's /dev/tty would suggest, because fork() hands the child the
     * parent's controlling terminal. "Nothing claimed" and "something was
     * inherited" read the same through /dev/tty and are not the same state. */
    printf("\n[3] inherited state (before any claim of our own):\n");
    {
        int t0 = open("/dev/tty", O_RDWR);
        if (t0 >= 0) {
            probe("/dev/tty inherited", t0);
            close(t0);
        }
    }

    printf("\n[4] after setsid() + TIOCSCTTY (what a session server does):\n");
    pid_t sid = getsid(0);
    pid_t pid = getpid();
    printf("  getpid=%d getsid=%d (session leader: %s)\n",
           (int)pid, (int)sid, pid == sid ? "yes" : "NO");

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        printf("  posix_openpt failed: %s\n", strerror(errno));
        printf("=== probe done ===\n");
        return 0;
    }
    if (grantpt(master) || unlockpt(master)) {
        printf("  grantpt/unlockpt failed: %s\n", strerror(errno));
        close(master);
        printf("=== probe done ===\n");
        return 0;
    }
    const char* name = ptsname(master);
    printf("  ptsname = %s\n", name ? name : "(null)");
    int slave = name ? open(name, O_RDWR) : -1;
    if (slave < 0) {
        printf("  open(slave) failed: %s\n", strerror(errno));
        close(master);
        printf("=== probe done ===\n");
        return 0;
    }

    /* setsid() refuses when the caller already leads a group, which is the
     * probe a shell uses to learn it is already a session leader. Not fatal
     * here: a process that already leads its own session may still claim a
     * terminal, and that is the case a run root is in. */
    errno = 0;
    pid_t newsid = setsid();
    printf("  setsid() = %d%s%s\n", (int)newsid,
           newsid < 0 ? " errno=" : "", newsid < 0 ? strerror(errno) : "");

    /* TIOCSCTTY's own return value is printed because "it claimed but /dev/tty
     * did not move" and "the claim was refused" are different faults with the
     * same symptom from the outside, and only the errno tells them apart. */
    errno = 0;
    int rc = ioctl(slave, TIOCSCTTY, 0);
    printf("  ioctl(slave, TIOCSCTTY, 0) = %d%s%s\n", rc,
           rc < 0 ? " errno=" : "", rc < 0 ? strerror(errno) : "");

    int tty2 = open("/dev/tty", O_RDWR);
    printf("  open(\"/dev/tty\") after the claim = %d\n", tty2);
    if (tty2 >= 0) {
        probe("/dev/tty (claimed)", tty2);
        close(tty2);
    }
    probe("the pty slave itself", slave);

    printf("\n=== probe done ===\n");
    return 0;
}
