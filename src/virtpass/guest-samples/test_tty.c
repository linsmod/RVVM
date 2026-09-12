/*
 * test_tty.c - Smoke test for the WinHost virtual TTY renderer
 *
 * Pure-stdio RISC-V guest program exercising the rvvm_user libvterm path
 * end to end: ONLCR line handling, SGR colors (ANSI/256/RGB), bold/reverse,
 * Unicode (CJK, fullwidth, combining marks, emoji) and in-place '\r'
 * updates. Each section prints with a small delay so the paint-time
 * throttled renderer is visibly exercised step by step; the final PASS
 * line also verifies the frozen last screen after guest exit.
 *
 * Expected result in the WinHost window (24x80 overlay):
 *   - colored text (8/16/256/truecolor), bold and reverse video
 *   - CJK/fullwidth glyphs aligned inside the box border
 *   - a progress bar updating in place, ending at 100%
 *   - "=== PASS ===" staying on screen after the guest exits
 *
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 * Run:   WinHost -> Run -> test_tty
 */

#include <stdio.h>
#include <string.h>

/* Tiny busy-wait so each section stays visible despite paint throttling */
static void delay(int count)
{
    for (int i = 0; i < count; i++) {
        __asm__ __volatile__("nop");
    }
}

static void section(const char* title)
{
    printf("\n--- %s ---\n", title);
    fflush(stdout);
    delay(2000000);
}

/* 8 standard ANSI colors + bright variants, foreground and background */
static void test_ansi16(void)
{
    static const char* names[] = {
        "blk", "red", "grn", "yel", "blu", "mag", "cyn", "wht",
    };
    for (int fg = 30; fg <= 37; fg++) {
        printf("\033[%dm%s \033[0m", fg, names[fg - 30]);
    }
    printf("\n");
    for (int fg = 90; fg <= 97; fg++) {
        printf("\033[%dm%s \033[0m", fg, names[fg - 90]);
    }
    printf("\n");
    for (int bg = 40; bg <= 47; bg++) {
        printf("\033[%dm %s \033[0m", bg, names[bg - 40]);
    }
    printf("\n");
}

/* 256-color ramps: 16..231 are a 6x6x6 cube, 232..255 a grayscale ramp */
static void test_256(void)
{
    printf("256-color cube:\n");
    for (int g = 0; g < 6; g++) {
        for (int r = 0; r < 6; r++) {
            for (int b = 0; b < 6; b++) {
                printf("\033[48;5;%dm ", 16 + 36 * r + 6 * g + b);
            }
            printf("\033[0m");
        }
        printf("\n");
    }
    printf("Grayscale:");
    for (int i = 232; i < 256; i++) {
        printf("\033[48;5;%dm ", i);
    }
    printf("\033[0m\n");
}

/* 24-bit truecolor gradient, foreground on one line, background on the next */
static void test_truecolor(void)
{
    for (int i = 0; i < 16; i++) {
        printf("\033[38;2;%d;0;%dm#", i * 16, 255 - i * 16);
    }
    printf("\033[0m\n");
    for (int i = 0; i < 16; i++) {
        printf("\033[48;2;0;%d;%dm ", i * 16, i * 8);
    }
    printf("\033[0m\n");
}

/* Bold, reverse and combinations */
static void test_attrs(void)
{
    printf("\033[1mBold\033[0m normal \033[1;31mbold-red\033[0m ");
    printf("\033[7mreverse\033[0m \033[1;7;32mbold-rev-green\033[0m\n");
}

/* Unicode: box drawing alignment, CJK, fullwidth, combining, emoji.
 * The box border proves the grid is not drifting on double-width glyphs. */
static void test_unicode(void)
{
    printf("\u250c\u2500\u2500\u2500\u2500\u2510  box drawing\n");
    printf("\u2502 \u4e2d\u6587CJK \u306b\u307b\u3093\u3054 \ud55c\uad6d\uc5b4 \u2502\n");
    printf("\u2502 \uff26\uff55\uff4c\uff4c\uff57\uff49\uff44\uff54\uff48 \u2502\n");
    printf("\u2514\u2500\u2500\u2500\u2500\u2518\n");
    printf("combining: e\u0301 a\u0308 o\u030a  |  emoji: \U0001F680 \u2713 \u00d7\n");
}

/* In-place '\r' update - the whole point of a parsed-screen renderer over a
 * dumb byte pipe. Each step flushes explicitly: line-buffered stdio only
 * flushes on '\n', and the progress line has none until it is done. */
static void test_carriage_return(void)
{
    for (int pct = 0; pct <= 100; pct += 10) {
        printf("\r[");
        for (int i = 0; i < 20; i++) {
            printf(i < pct / 5 ? "#" : "-");
        }
        printf("] %3d%%", pct);
        fflush(stdout);
        delay(1000000);
    }
    printf("  \033[32mdone\033[0m\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== TTY Renderer Test ===\n");
    printf("grid 24x80 | ONLCR | SGR | Unicode\n");

    section("ANSI 16 color");
    test_ansi16();

    section("256 color");
    test_256();

    section("truecolor");
    test_truecolor();

    section("attrs: bold / reverse");
    test_attrs();

    section("unicode");
    test_unicode();

    section("carriage return (in-place update)");
    test_carriage_return();

    printf("\n=== PASS ===\n");
    fflush(stdout);
    return 0;
}
