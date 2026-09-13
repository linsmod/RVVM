/*
 * GENERATED FILE - produced by tools/gen_stub_notimpl.py - DO NOT EDIT BY HAND.
 *
 * "Not implemented" build variant of src/virtpass/vp_aaudio_stub.c.
 * Regenerate from the repository root:
 *     python tools/gen_stub_notimpl.py src/virtpass/vp_aaudio_stub.c --outdir src/virtpass/vp-sdk
 *
 * Every API function below keeps its original signature and reports itself on
 * stderr instead of issuing a hypercall to the host. Link a guest against this
 * file instead of the real stub to see which host APIs it actually asks for.
 */

/*
 * vp_aaudio_stub.c - Guest-side AAudio proxy for statically-linked riscv64 ELFs
 *
 * Implements the API declared in virtpass/vp_aaudio.h on top of the shared
 * wire protocol in virtpass/vp_audio_ringbuf.h. Every control operation and
 * every data transfer is one hypercall; no shared ring buffer.
 *
 * How a stream works:
 *
 *   AAudioStreamBuilder_openStream()
 *       - build config, SYS_ANDROID_AAUDIO_OPEN hypercall
 *       - SYS_ANDROID_AAUDIO_INFO  (learn the effective geometry)
 *
 *   AAudioStream_write()   ->  SYS_ANDROID_AAUDIO_WRITE hypercall
 *   AAudioStream_read()    ->  SYS_ANDROID_AAUDIO_READ  hypercall
 *
 *   AAudioStream_close()   ->  SYS_ANDROID_AAUDIO_CLOSE hypercall
 *
 * Data-callback streams run a guest thread that calls the app callback,
 * fills a staging buffer, and pushes it through a WRITE hypercall.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "virtpass/vp_android.h"
#include "virtpass/vp_audio_ringbuf.h"
#include <stdio.h>       /* generator: stubs report via stderr */
#include <stddef.h>      /* generator: NULL / size_t */

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

/* ============================================================
 * Generated fallback reporter
 *
 * One line per distinct API is printed the first time it is reached: a guest
 * render loop calls these functions thousands of times per second, so
 * reporting every call would drown the host console in identical lines. The
 * dedup table compares the __func__ literals (stable per function); the race
 * between guest threads at worst repeats a line.
 * ============================================================ */
static void vp_stub_not_implemented(const char* api)
{
    static const char* reported[512];
    static unsigned     count = 0;
    unsigned            i;

    for (i = 0; i < count; i++) {
        if (reported[i] == api) {
            return;
        }
    }
    if (count < sizeof(reported) / sizeof(reported[0])) {
        reported[count++] = api;
    }
    fprintf(stderr, "[virtpass] not implemented: %s()\n", api);
}


/* ============================================================
 * Hypercall plumbing
 * ============================================================ */
static inline long virtpass_syscall(long nr, long a0, long a1, long a2,
                                    long a3, long a4, long a5)
{
    vp_stub_not_implemented(__func__);
    (void)nr;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return 0;
}

#define AAUDIO_CALL0(cmd) \
    ((int64_t)virtpass_syscall((long)(SYS_ANDROID_CALL), (long)(int64_t)(cmd), 0, 0, 0, 0, 0))

#define AAUDIO_CALL1(cmd, x) \
    ((int64_t)virtpass_syscall((long)(SYS_ANDROID_CALL), (long)(int64_t)(cmd), \
                               (long)(int64_t)(x), 0, 0, 0, 0))

#define AAUDIO_CALL2(cmd, x, y) \
    ((int64_t)virtpass_syscall((long)(SYS_ANDROID_CALL), (long)(int64_t)(cmd), \
                               (long)(int64_t)(x), (long)(int64_t)(y), 0, 0, 0))

#define AAUDIO_CALL3(cmd, x, y, z) \
    ((int64_t)virtpass_syscall((long)(SYS_ANDROID_CALL), (long)(int64_t)(cmd), \
                               (long)(int64_t)(x), (long)(int64_t)(y), \
                               (long)(int64_t)(z), 0, 0))

/* ============================================================
 * Defaults / limits
 * ============================================================ */
#define AAUDIO_DEFAULT_SAMPLE_RATE   48000
#define AAUDIO_DEFAULT_CHANNELS      2
#define AAUDIO_DEFAULT_FORMAT        AAUDIO_FORMAT_PCM_FLOAT
#define AAUDIO_DEFAULT_BUFFER_FRAMES 4096
#define AAUDIO_MIN_BUFFER_FRAMES     256
#define AAUDIO_MAX_CHANNELS          8
#define AAUDIO_NS_PER_SEC            1000000000LL
#define AAUDIO_NS_PER_MS             1000000LL

/* ============================================================
 * Objects
 * ============================================================ */
struct AAudioStreamBuilder {
    int32_t device_id;
    int32_t sample_rate;
    int32_t channel_count;
    int32_t samples_per_frame;
    int32_t format;
    int32_t sharing_mode;
    int32_t direction;
    int32_t buffer_capacity_frames;
    int32_t performance_mode;
    int32_t usage;
    int32_t content_type;
    int32_t input_preset;
    int32_t session_id;
    int32_t channel_mask;
    int32_t frames_per_data_callback;
    AAudioStream_dataCallback data_callback;
    void* data_user;
    AAudioStream_errorCallback error_callback;
    void* error_user;
};

struct AAudioStream {
    int64_t  handle;              /* transport slot; negative once detached   */

    pthread_mutex_t lock;
    pthread_cond_t  cond;         /* start/stop transitions + waitForState    */

    int32_t  state;
    int32_t  direction;
    int32_t  format;
    int32_t  channel_count;
    int32_t  sample_rate;
    int32_t  frame_bytes;
    int32_t  frames_per_burst;
    int32_t  buffer_capacity_frames;
    int32_t  buffer_size_frames;
    int32_t  sharing_mode;
    int32_t  performance_mode;
    int32_t  usage;
    int32_t  content_type;
    int32_t  input_preset;
    int32_t  device_id;
    int32_t  session_id;
    int32_t  xrun_count;
    int64_t  frames_written;
    int64_t  frames_read;
    bool     source_lost;
    bool     closed;

    vp_aaudio_config_t cfg;

    /* Data callback mode */
    AAudioStream_dataCallback  data_callback;
    void*                      data_user;
    AAudioStream_errorCallback error_callback;
    void*                      error_user;
    int32_t                    frames_per_data_callback;
    bool                       callback_running;
    bool                       callback_quit;
    bool                       callback_started;
    pthread_t                  callback_thread;
    uint8_t*                   callback_buf;
};

/* ============================================================
 * Small helpers
 * ============================================================ */
static int64_t aaudio_now_ns(void)
{
    vp_stub_not_implemented(__func__);
    return 0;
}

static void aaudio_set_state(AAudioStream* stream, int32_t state)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    (void)state;
}

static aaudio_result_t aaudio_refresh_info(AAudioStream* stream);

/* ============================================================
 * Data-callback worker
 * ============================================================ */
static void* aaudio_callback_worker(void* arg)
{
    vp_stub_not_implemented(__func__);
    (void)arg;
    return NULL;
}

static void aaudio_callback_start(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
}

static void aaudio_callback_stop(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
}

/* ============================================================
 * StreamBuilder
 * ============================================================ */
AAudioStreamBuilder* AAudio_createStreamBuilder(void)
{
    vp_stub_not_implemented(__func__);
    return NULL;
}

void AAudioStreamBuilder_delete(AAudioStreamBuilder* builder)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
}

void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder* builder, int32_t deviceId)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)deviceId;
}

void AAudioStreamBuilder_setPackageName(AAudioStreamBuilder* builder, const char* packageName)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)packageName;
}

void AAudioStreamBuilder_setAttributionTag(AAudioStreamBuilder* builder, const char* attributionTag)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)attributionTag;
}

void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* builder, int32_t sampleRate)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)sampleRate;
}

void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* builder, int32_t channelCount)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)channelCount;
}

void AAudioStreamBuilder_setSamplesPerFrame(AAudioStreamBuilder* builder, int32_t samplesPerFrame)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)samplesPerFrame;
}

void AAudioStreamBuilder_setFormat(AAudioStreamBuilder* builder, aaudio_format_t format)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)format;
}

void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder* builder, aaudio_sharing_mode_t sharingMode)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)sharingMode;
}

void AAudioStreamBuilder_setDirection(AAudioStreamBuilder* builder, aaudio_direction_t direction)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)direction;
}

void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder* builder, int32_t numFrames)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)numFrames;
}

void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* builder, aaudio_performance_mode_t mode)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)mode;
}

void AAudioStreamBuilder_setUsage(AAudioStreamBuilder* builder, aaudio_usage_t usage)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)usage;
}

void AAudioStreamBuilder_setContentType(AAudioStreamBuilder* builder, aaudio_content_type_t contentType)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)contentType;
}

void AAudioStreamBuilder_setInputPreset(AAudioStreamBuilder* builder, aaudio_input_preset_t inputPreset)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)inputPreset;
}

void AAudioStreamBuilder_setSessionId(AAudioStreamBuilder* builder, aaudio_session_id_t sessionId)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)sessionId;
}

void AAudioStreamBuilder_setChannelMask(AAudioStreamBuilder* builder, aaudio_channel_mask_t channelMask)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)channelMask;
}

void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder* builder, int32_t numFrames)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)numFrames;
}

void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* builder,
                                         AAudioStream_dataCallback callback,
                                         void* userData)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)callback;
    (void)userData;
}

void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* builder,
                                          AAudioStream_errorCallback callback,
                                          void* userData)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)callback;
    (void)userData;
}

/* ============================================================
 * Re-read negotiated geometry from the host.
 * ============================================================ */
static aaudio_result_t aaudio_refresh_info(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

AAudioStream* AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder,
                                             aaudio_result_t* pError)
{
    vp_stub_not_implemented(__func__);
    (void)builder;
    (void)pError;
    return NULL;
}

/* ============================================================
 * Stream lifecycle
 * ============================================================ */
static aaudio_result_t aaudio_request_state(AAudioStream* stream, int64_t cmd,
                                            int32_t transient, int32_t settled)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    (void)cmd;
    (void)transient;
    (void)settled;
    return 0;
}

aaudio_result_t AAudioStream_requestStart(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_result_t AAudioStream_requestPause(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_result_t AAudioStream_requestStop(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_result_t AAudioStream_requestFlush(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_result_t AAudioStream_close(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_stream_state_t AAudioStream_getState(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_result_t AAudioStream_waitForStateChange(AAudioStream* stream,
                                                aaudio_stream_state_t inputState,
                                                aaudio_stream_state_t* nextState,
                                                int64_t timeoutNanoseconds)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    (void)inputState;
    (void)nextState;
    (void)timeoutNanoseconds;
    return 0;
}

/* ============================================================
 * Blocking I/O — hypercall passthrough
 * ============================================================ */
aaudio_result_t AAudioStream_write(AAudioStream* stream, const void* buffer,
                                   int32_t numFrames, int64_t timeoutNanoseconds)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    (void)buffer;
    (void)numFrames;
    (void)timeoutNanoseconds;
    return 0;
}

aaudio_result_t AAudioStream_read(AAudioStream* stream, void* buffer,
                                  int32_t numFrames, int64_t timeoutNanoseconds)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    (void)buffer;
    (void)numFrames;
    (void)timeoutNanoseconds;
    return 0;
}

/* ============================================================
 * Introspection
 * ============================================================ */
int64_t AAudioStream_getFramesWritten(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

int64_t AAudioStream_getFramesRead(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

int32_t AAudioStream_getSampleRate(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

int32_t AAudioStream_getChannelCount(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_format_t AAudioStream_getFormat(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_sharing_mode_t AAudioStream_getSharingMode(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_direction_t AAudioStream_getDirection(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_usage_t AAudioStream_getUsage(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_content_type_t AAudioStream_getContentType(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_input_preset_t AAudioStream_getInputPreset(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

int32_t AAudioStream_getDeviceId(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_session_id_t AAudioStream_getSessionId(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

int32_t AAudioStream_getFramesPerBurst(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

int32_t AAudioStream_getBufferSizeInFrames(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

int32_t AAudioStream_setBufferSizeInFrames(AAudioStream* stream, int32_t numFrames)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    (void)numFrames;
    return 0;
}

int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

int32_t AAudioStream_getXRunCount(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

int32_t AAudioStream_getFramesPerDataCallback(AAudioStream* stream)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    return 0;
}

aaudio_result_t AAudioStream_getTimestamp(AAudioStream* stream, clockid_t clockid,
                                          int64_t* framePosition,
                                          int64_t* timeNanoseconds)
{
    vp_stub_not_implemented(__func__);
    (void)stream;
    (void)clockid;
    (void)framePosition;
    (void)timeNanoseconds;
    return 0;
}

/* ============================================================
 * Diagnostics
 * ============================================================ */
const char* AAudio_convertResultToText(aaudio_result_t returnCode)
{
    vp_stub_not_implemented(__func__);
    (void)returnCode;
    return NULL;
}

const char* AAudio_convertStreamStateToText(aaudio_stream_state_t state)
{
    vp_stub_not_implemented(__func__);
    (void)state;
    return NULL;
}
