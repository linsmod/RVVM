/*
 * test_render.c - Test software rendering via ANativeWindow
 *
 * This program tests the ANativeWindow_lock/unlockAndPost API
 * by rendering a gradient pattern to the screen.
 *
 * Build: riscv64-unknown-elf-gcc -static -O2 -o test_render.exe test_render.c -L. -landroid_stubs
 * Run:   ./test_render.exe
 */

#include "virtpass/vp_android.h"

/* Simple delay function */
static void delay(int count)
{
    for (int i = 0; i < count; i++) {
        __asm__ __volatile__("nop");
    }
}

/* Render: black background + one red square bouncing off the edges.
 * Nothing else - this is the canonical "is the whole pipeline correct"
 * test: any tearing/striding bug shows as a smeared or doubled square. */
static void render_bounce(ANativeWindow_Buffer* buf, int frame)
{
    if (!buf || !buf->bits) return;

    uint32_t* pixels = (uint32_t*)buf->bits;
    int width = buf->width;
    int height = buf->height;
    int stride = buf->stride;

    /* Clear to black */
    for (int y = 0; y < height; y++) {
        uint32_t* row = pixels + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            row[x] = 0xFF000000u;
        }
    }

    /* Bouncing square: constant velocity, reflects at the borders */
    int rect_w = width / 6;
    int rect_h = rect_w;
    int speed = 3;

    int px = frame * speed;
    int py = frame * (speed + 1);
    int range_x = (width - rect_w) * 2;
    int range_y = (height - rect_h) * 2;
    int rect_x = px % range_x;
    int rect_y = py % range_y;
    if (rect_x > (width - rect_w)) rect_x = range_x - rect_x;
    if (rect_y > (height - rect_h)) rect_y = range_y - rect_y;

    for (int y = rect_y; y < rect_y + rect_h; y++) {
        uint32_t* row = pixels + (size_t)y * stride;
        for (int x = rect_x; x < rect_x + rect_w; x++) {
            row[x] = 0xFF0000FFu; /* opaque red in RGBA_8888 */
        }
    }
}

int main(void)
{
    /* Get window dimensions */
    int width = ANativeWindow_getWidth(NULL);
    int height = ANativeWindow_getHeight(NULL);

    if (width <= 0) width = 720;
    if (height <= 0) height = 1280;

    /* Set buffer format */
    ANativeWindow_setBuffersGeometry(NULL, width, height, WINDOW_FORMAT_RGBA_8888);

    /* Render loop - run forever (app-like behavior) */
    for (int frame = 0; ; frame++) {
        ANativeWindow_Buffer buffer;
        ARect dirty;

        /* Lock the window */
        if (ANativeWindow_lock(NULL, &buffer, &dirty) == 0) {
            render_bounce(&buffer, frame);
            /* Unlock and post */
            ANativeWindow_unlockAndPost(NULL);
        }

        /* Small delay */
        delay(10000);
    }

    return 0;
}
