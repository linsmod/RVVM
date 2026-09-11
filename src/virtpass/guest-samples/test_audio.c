/*
 * test_audio.c - Smoke test for the AAudio guest proxy
 *
 * RISC-V guest program that exercises the AAudio stub API:
 *   1. Query host capabilities (SYS_ANDROID_AAUDIO_QUERY)
 *   2. Build and open an output stream
 *   3. Write a 440 Hz sine tone for ~1 second via blocking write
 *   4. Close the stream
 *
 * Build (riscv64-linux-musl via zig):
 *   zig cc -target riscv64-linux-musl -O2 -I../../include -static \
 *       -L$(ANDROID_GUEST_DIR) -landroid_stubs -o test_audio test_audio.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "virtpass/vp_aaudio.h"

#define SAMPLE_RATE   48000
#define CHANNEL_COUNT 2
#define DURATION_SEC  1
#define FREQUENCY_HZ  440.0
#define AMPLITUDE     0.3f

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("=== AAudio Guest Proxy Smoke Test ===\n");
    printf("Guest: RISC-V 64-bit\n");

    /* Step 0: query capabilities */
    printf("\n[1] Querying AAudio capabilities...\n");
    /* The guest stub translates this to SYS_ANDROID_AAUDIO_QUERY ecall. */
    AAudioStreamBuilder* builder = AAudio_createStreamBuilder();
    if (!builder) {
        printf("FAIL: AAudio_createStreamBuilder returned NULL\n");
        return 1;
    }
    printf("    Builder created OK\n");

    /* Step 1: configure the builder */
    printf("\n[2] Configuring stream builder...\n");
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setChannelCount(builder, CHANNEL_COUNT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    printf("    direction=output rate=%d ch=%d fmt=FLOAT\n", SAMPLE_RATE, CHANNEL_COUNT);

    /* Step 2: open the stream */
    printf("\n[3] Opening stream...\n");
    aaudio_result_t result;
    AAudioStream* stream = AAudioStreamBuilder_openStream(builder, &result);
    AAudioStreamBuilder_delete(builder);
    builder = NULL;

    if (!stream || result != AAUDIO_OK) {
        printf("FAIL: openStream returned result=%d (%s)\n",
               result, AAudio_convertResultToText(result));
        return 1;
    }
    printf("    Stream opened: rate=%d ch=%d burst=%d buffer=%d\n",
           AAudioStream_getSampleRate(stream),
           AAudioStream_getChannelCount(stream),
           AAudioStream_getFramesPerBurst(stream),
           AAudioStream_getBufferCapacityInFrames(stream));

    /* Step 3: generate and write a 440 Hz sine tone */
    printf("\n[4] Writing %d second(s) of 440 Hz tone...\n", DURATION_SEC);
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    if (burst <= 0) burst = 960;
    float* buf = (float*)malloc((size_t)burst * (size_t)CHANNEL_COUNT * sizeof(float));
    if (!buf) {
        printf("FAIL: malloc\n");
        AAudioStream_close(stream);
        return 1;
    }

    result = AAudioStream_requestStart(stream);
    if (result != AAUDIO_OK) {
        printf("FAIL: requestStart returned %d\n", result);
        free(buf);
        AAudioStream_close(stream);
        return 1;
    }

    int total_frames = SAMPLE_RATE * DURATION_SEC;
    int written = 0;
    double phase = 0.0;
    double phase_inc = 2.0 * 3.14159265358979323846 * FREQUENCY_HZ / (double)SAMPLE_RATE;

    while (written < total_frames) {
        int n = (total_frames - written < burst) ? (total_frames - written) : burst;
        for (int i = 0; i < n; i++) {
            float sample = (float)(AMPLITUDE * sin(phase));
            phase += phase_inc;
            if (phase >= 2.0 * 3.14159265358979323846) {
                phase -= 2.0 * 3.14159265358979323846;
            }
            for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
                buf[i * CHANNEL_COUNT + ch] = sample;
            }
        }
        aaudio_result_t wr = AAudioStream_write(stream, buf, n, 2000000000LL);
        if (wr < 0) {
            printf("FAIL: AAudioStream_write returned %d\n", wr);
            break;
        }
        written += (int)wr;
        printf("    wrote %d / %d frames\r", written, total_frames);
    }
    printf("\n    Done: %d frames written\n", written);

    /* Step 4: stop and clean up */
    printf("\n[5] Stopping and closing stream...\n");
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    free(buf);

    printf("\n=== PASS ===\n");
    return 0;
}
