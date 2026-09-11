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

/* ============================================================
 * Hypercall plumbing
 * ============================================================ */
static inline long virtpass_syscall(long nr, long a0, long a1, long a2,
                                    long a3, long a4, long a5)
{
    register long t0 __asm__("a7") = nr;
    register long t1 __asm__("a0") = a0;
    register long t2 __asm__("a1") = a1;
    register long t3 __asm__("a2") = a2;
    register long t4 __asm__("a3") = a3;
    register long t5 __asm__("a4") = a4;
    register long t6 __asm__("a5") = a5;

    __asm__ __volatile__(
        "ecall"
        : "+r"(t1)
        : "r"(t2), "r"(t3), "r"(t4), "r"(t5), "r"(t6), "r"(t0)
        : "memory"
    );
    return t1;
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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * AAUDIO_NS_PER_SEC + (int64_t)ts.tv_nsec;
}

static void aaudio_set_state(AAudioStream* stream, int32_t state)
{
    pthread_mutex_lock(&stream->lock);
    if (stream->state != state) {
        stream->state = state;
        pthread_cond_broadcast(&stream->cond);
    }
    pthread_mutex_unlock(&stream->lock);
}

static aaudio_result_t aaudio_refresh_info(AAudioStream* stream);

/* ============================================================
 * Data-callback worker
 * ============================================================ */
static void* aaudio_callback_worker(void* arg)
{
    AAudioStream* stream = (AAudioStream*)arg;

    for (;;) {
        pthread_mutex_lock(&stream->lock);
        while (!stream->callback_quit && stream->state != AAUDIO_STREAM_STATE_STARTED) {
            pthread_cond_wait(&stream->cond, &stream->lock);
        }
        bool quit = stream->callback_quit;
        pthread_mutex_unlock(&stream->lock);
        if (quit) {
            break;
        }

        int32_t frames = stream->frames_per_data_callback > 0
                       ? stream->frames_per_data_callback
                       : stream->frames_per_burst;
        if (frames <= 0) {
            frames = 1024;
        }
        if (frames > stream->buffer_capacity_frames) {
            frames = stream->buffer_capacity_frames;
        }

        if (stream->direction == AAUDIO_DIRECTION_OUTPUT) {
            stream->data_callback(stream, stream->data_user, stream->callback_buf, frames);
            int64_t rc = AAUDIO_CALL3(SYS_ANDROID_AAUDIO_WRITE, stream->handle,
                                      (intptr_t)stream->callback_buf, frames);
            if (rc > 0) {
                stream->frames_written += rc;
            }
        } else {
            memset(stream->callback_buf, 0, (size_t)frames * stream->frame_bytes);
            int64_t rc = AAUDIO_CALL3(SYS_ANDROID_AAUDIO_READ, stream->handle,
                                      (intptr_t)stream->callback_buf, frames);
            int32_t got = rc > 0 ? (int32_t)rc : 0;
            if (got > 0) {
                stream->frames_read += got;
            }
            stream->data_callback(stream, stream->data_user, stream->callback_buf, got);
        }
    }

    return NULL;
}

static void aaudio_callback_start(AAudioStream* stream)
{
    if (!stream->data_callback) {
        return;
    }
    if (stream->callback_started) {
        return;
    }
    stream->callback_quit = false;
    if (pthread_create(&stream->callback_thread, NULL, aaudio_callback_worker, stream) == 0) {
        stream->callback_started = true;
    }
}

static void aaudio_callback_stop(AAudioStream* stream)
{
    if (!stream->callback_started) {
        return;
    }
    pthread_mutex_lock(&stream->lock);
    stream->callback_quit = true;
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);

    pthread_join(stream->callback_thread, NULL);
    stream->callback_started = false;
}

/* ============================================================
 * StreamBuilder
 * ============================================================ */
AAudioStreamBuilder* AAudio_createStreamBuilder(void)
{
    AAudioStreamBuilder* builder = (AAudioStreamBuilder*)calloc(1, sizeof(AAudioStreamBuilder));
    if (!builder) {
        return NULL;
    }
    builder->direction              = AAUDIO_DIRECTION_OUTPUT;
    builder->format                 = AAUDIO_FORMAT_UNSPECIFIED;
    builder->sample_rate            = AAUDIO_UNSPECIFIED;
    builder->channel_count          = AAUDIO_UNSPECIFIED;
    builder->samples_per_frame      = AAUDIO_UNSPECIFIED;
    builder->sharing_mode           = AAUDIO_SHARING_MODE_SHARED;
    builder->performance_mode       = AAUDIO_PERFORMANCE_MODE_NONE;
    builder->usage                  = AAUDIO_UNSPECIFIED;
    builder->content_type           = AAUDIO_UNSPECIFIED;
    builder->input_preset           = AAUDIO_UNSPECIFIED;
    builder->session_id             = AAUDIO_SESSION_ID_NONE;
    builder->buffer_capacity_frames = AAUDIO_UNSPECIFIED;
    builder->device_id              = AAUDIO_DEVICE_ID_UNSPECIFIED;
    return builder;
}

void AAudioStreamBuilder_delete(AAudioStreamBuilder* builder)
{
    free(builder);
}

void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder* builder, int32_t deviceId)
{
    if (builder) builder->device_id = deviceId;
}

void AAudioStreamBuilder_setPackageName(AAudioStreamBuilder* builder, const char* packageName)
{
    (void)builder;
    (void)packageName;
}

void AAudioStreamBuilder_setAttributionTag(AAudioStreamBuilder* builder, const char* attributionTag)
{
    (void)builder;
    (void)attributionTag;
}

void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* builder, int32_t sampleRate)
{
    if (builder) builder->sample_rate = sampleRate;
}

void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* builder, int32_t channelCount)
{
    if (builder) builder->channel_count = channelCount;
}

void AAudioStreamBuilder_setSamplesPerFrame(AAudioStreamBuilder* builder, int32_t samplesPerFrame)
{
    if (builder) builder->samples_per_frame = samplesPerFrame;
}

void AAudioStreamBuilder_setFormat(AAudioStreamBuilder* builder, aaudio_format_t format)
{
    if (builder) builder->format = format;
}

void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder* builder, aaudio_sharing_mode_t sharingMode)
{
    if (builder) builder->sharing_mode = sharingMode;
}

void AAudioStreamBuilder_setDirection(AAudioStreamBuilder* builder, aaudio_direction_t direction)
{
    if (builder) builder->direction = direction;
}

void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder* builder, int32_t numFrames)
{
    if (builder) builder->buffer_capacity_frames = numFrames;
}

void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* builder, aaudio_performance_mode_t mode)
{
    if (builder) builder->performance_mode = mode;
}

void AAudioStreamBuilder_setUsage(AAudioStreamBuilder* builder, aaudio_usage_t usage)
{
    if (builder) builder->usage = usage;
}

void AAudioStreamBuilder_setContentType(AAudioStreamBuilder* builder, aaudio_content_type_t contentType)
{
    if (builder) builder->content_type = contentType;
}

void AAudioStreamBuilder_setInputPreset(AAudioStreamBuilder* builder, aaudio_input_preset_t inputPreset)
{
    if (builder) builder->input_preset = inputPreset;
}

void AAudioStreamBuilder_setSessionId(AAudioStreamBuilder* builder, aaudio_session_id_t sessionId)
{
    if (builder) builder->session_id = sessionId;
}

void AAudioStreamBuilder_setChannelMask(AAudioStreamBuilder* builder, aaudio_channel_mask_t channelMask)
{
    (void)builder;
    (void)channelMask;
}

void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder* builder, int32_t numFrames)
{
    if (builder) builder->frames_per_data_callback = numFrames;
}

void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* builder,
                                         AAudioStream_dataCallback callback,
                                         void* userData)
{
    if (builder) {
        builder->data_callback = callback;
        builder->data_user = userData;
    }
}

void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* builder,
                                          AAudioStream_errorCallback callback,
                                          void* userData)
{
    if (builder) {
        builder->error_callback = callback;
        builder->error_user = userData;
    }
}

/* ============================================================
 * Re-read negotiated geometry from the host.
 * ============================================================ */
static aaudio_result_t aaudio_refresh_info(AAudioStream* stream)
{
    vp_aaudio_info_t info;
    memset(&info, 0, sizeof(info));
    int64_t rc = AAUDIO_CALL2(SYS_ANDROID_AAUDIO_INFO, stream->handle, (intptr_t)&info);
    if (rc != VP_AUDIO_OK) {
        return (aaudio_result_t)rc;
    }

    stream->sample_rate      = info.sample_rate;
    stream->channel_count    = info.channel_count;
    stream->format           = info.format;
    stream->sharing_mode     = info.sharing_mode;
    stream->performance_mode = info.performance_mode;
    stream->usage            = info.usage;
    stream->content_type     = info.content_type;
    stream->device_id        = info.device_id;
    stream->session_id       = info.session_id;
    stream->frames_per_burst = info.frames_per_burst;
    stream->buffer_size_frames = info.buffer_size_frames;
    stream->buffer_capacity_frames = info.buffer_capacity_frames;
    stream->xrun_count       = info.xrun_count;
    if (info.frame_bytes > 0) {
        stream->frame_bytes = info.frame_bytes;
    }
    if (info.state != stream->state) {
        aaudio_set_state(stream, info.state);
    }
    return AAUDIO_OK;
}

AAudioStream* AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder,
                                             aaudio_result_t* pError)
{
    if (!builder) {
        if (pError) *pError = AAUDIO_ERROR_NULL;
        return NULL;
    }

    if ((int64_t)AAUDIO_CALL0(SYS_ANDROID_AAUDIO_QUERY) == 0) {
        if (pError) *pError = AAUDIO_ERROR_UNAVAILABLE;
        return NULL;
    }

    int32_t format  = builder->format > AAUDIO_FORMAT_UNSPECIFIED
                    ? builder->format : AAUDIO_FORMAT_PCM_FLOAT;
    int32_t channels = builder->channel_count > 0
                     ? builder->channel_count : AAUDIO_DEFAULT_CHANNELS;
    int32_t rate    = builder->sample_rate > 0
                    ? builder->sample_rate : AAUDIO_DEFAULT_SAMPLE_RATE;
    if (channels > AAUDIO_MAX_CHANNELS) {
        channels = AAUDIO_MAX_CHANNELS;
    }

    uint32_t frame_bytes = vp_audio_frame_bytes(format, channels);
    if (frame_bytes == 0) {
        if (pError) *pError = AAUDIO_ERROR_INVALID_FORMAT;
        return NULL;
    }

    int32_t capacity = builder->buffer_capacity_frames > 0
                     ? builder->buffer_capacity_frames : AAUDIO_DEFAULT_BUFFER_FRAMES;
    if (capacity < AAUDIO_MIN_BUFFER_FRAMES) {
        capacity = AAUDIO_MIN_BUFFER_FRAMES;
    }

    AAudioStream* stream = (AAudioStream*)calloc(1, sizeof(AAudioStream));
    if (!stream) {
        if (pError) *pError = AAUDIO_ERROR_NO_MEMORY;
        return NULL;
    }
    stream->handle = -1;
    stream->state = AAUDIO_STREAM_STATE_UNINITIALIZED;
    stream->direction = builder->direction;
    stream->format = format;
    stream->channel_count = channels;
    stream->sample_rate = rate;
    stream->frame_bytes = (int32_t)frame_bytes;
    stream->buffer_capacity_frames = capacity;
    stream->sharing_mode = builder->sharing_mode;
    stream->performance_mode = builder->performance_mode;
    stream->usage = builder->usage > 0 ? builder->usage : AAUDIO_USAGE_MEDIA;
    stream->content_type = builder->content_type > 0 ? builder->content_type : AAUDIO_CONTENT_TYPE_MUSIC;
    stream->input_preset = builder->input_preset > 0 ? builder->input_preset : AAUDIO_INPUT_PRESET_GENERIC;
    stream->device_id = builder->device_id;
    stream->session_id = builder->session_id;
    stream->data_callback = builder->data_callback;
    stream->data_user = builder->data_user;
    stream->error_callback = builder->error_callback;
    stream->error_user = builder->error_user;
    stream->frames_per_data_callback = builder->frames_per_data_callback;

    pthread_mutex_init(&stream->lock, NULL);
    pthread_cond_init(&stream->cond, NULL);

    /* Allocate callback staging buffer (used in callback mode). */
    stream->callback_buf = (uint8_t*)calloc((size_t)capacity, frame_bytes);
    if (!stream->callback_buf) {
        goto fail;
    }

    /* Build the config for the host. */
    memset(&stream->cfg, 0, sizeof(stream->cfg));
    stream->cfg.direction        = stream->direction;
    stream->cfg.sharing_mode     = stream->sharing_mode;
    stream->cfg.performance_mode = stream->performance_mode;
    stream->cfg.format           = stream->format;
    stream->cfg.sample_rate      = stream->sample_rate;
    stream->cfg.channel_count    = stream->channel_count;
    stream->cfg.usage            = stream->usage;
    stream->cfg.content_type     = stream->content_type;
    stream->cfg.input_preset     = stream->input_preset;
    stream->cfg.device_id        = stream->device_id;
    stream->cfg.session_id       = stream->session_id;
    stream->cfg.buffer_frames    = capacity;
    stream->cfg.callback_flags   = stream->data_callback ? 1 : 0;
    stream->cfg.timeout_ns       = 0;
    stream->cfg.app_token        = (int64_t)(intptr_t)stream;

    int64_t handle = AAUDIO_CALL1(SYS_ANDROID_AAUDIO_OPEN, (intptr_t)&stream->cfg);
    if (handle < 0) {
        if (pError) *pError = (aaudio_result_t)handle;
        goto fail;
    }
    stream->handle = handle;

    aaudio_refresh_info(stream);
    if (stream->state == AAUDIO_STREAM_STATE_UNINITIALIZED) {
        aaudio_set_state(stream, AAUDIO_STREAM_STATE_OPEN);
    }

    if (pError) *pError = AAUDIO_OK;
    return stream;

fail:
    pthread_cond_destroy(&stream->cond);
    pthread_mutex_destroy(&stream->lock);
    free(stream->callback_buf);
    free(stream);
    if (pError && *pError == AAUDIO_OK) {
        *pError = AAUDIO_ERROR_NO_MEMORY;
    }
    return NULL;
}

/* ============================================================
 * Stream lifecycle
 * ============================================================ */
static aaudio_result_t aaudio_request_state(AAudioStream* stream, int64_t cmd,
                                            int32_t transient, int32_t settled)
{
    if (!stream || stream->handle < 0) {
        return AAUDIO_ERROR_INVALID_HANDLE;
    }
    aaudio_set_state(stream, transient);

    int64_t rc = AAUDIO_CALL2(cmd, stream->handle, (int64_t)AAUDIO_NS_PER_SEC);
    if (rc != VP_AUDIO_OK) {
        aaudio_refresh_info(stream);
        if (stream->state == transient) {
            aaudio_set_state(stream, AAUDIO_STREAM_STATE_UNKNOWN);
        }
        return (aaudio_result_t)rc;
    }

    aaudio_set_state(stream, settled);
    aaudio_refresh_info(stream);
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestStart(AAudioStream* stream)
{
    aaudio_result_t rc = aaudio_request_state(
        stream, SYS_ANDROID_AAUDIO_START,
        AAUDIO_STREAM_STATE_STARTING, AAUDIO_STREAM_STATE_STARTED);
    if (rc == AAUDIO_OK) {
        aaudio_callback_start(stream);
    }
    return rc;
}

aaudio_result_t AAudioStream_requestPause(AAudioStream* stream)
{
    return aaudio_request_state(stream, SYS_ANDROID_AAUDIO_PAUSE,
                                AAUDIO_STREAM_STATE_PAUSING, AAUDIO_STREAM_STATE_PAUSED);
}

aaudio_result_t AAudioStream_requestStop(AAudioStream* stream)
{
    aaudio_callback_stop(stream);
    return aaudio_request_state(stream, SYS_ANDROID_AAUDIO_STOP,
                                AAUDIO_STREAM_STATE_STOPPING, AAUDIO_STREAM_STATE_STOPPED);
}

aaudio_result_t AAudioStream_requestFlush(AAudioStream* stream)
{
    if (!stream || stream->handle < 0) {
        return AAUDIO_ERROR_INVALID_HANDLE;
    }
    aaudio_set_state(stream, AAUDIO_STREAM_STATE_FLUSHING);

    int64_t rc = AAUDIO_CALL1(SYS_ANDROID_AAUDIO_FLUSH, stream->handle);
    if (rc != VP_AUDIO_OK) {
        aaudio_refresh_info(stream);
        return (aaudio_result_t)rc;
    }
    aaudio_set_state(stream, AAUDIO_STREAM_STATE_FLUSHED);
    aaudio_refresh_info(stream);
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_close(AAudioStream* stream)
{
    if (!stream) {
        return AAUDIO_ERROR_NULL;
    }

    stream->closed = true;
    aaudio_callback_stop(stream);

    aaudio_result_t rc = AAUDIO_OK;
    if (stream->handle >= 0) {
        int64_t host_rc = AAUDIO_CALL1(SYS_ANDROID_AAUDIO_CLOSE, stream->handle);
        if (host_rc != VP_AUDIO_OK) {
            rc = (aaudio_result_t)host_rc;
        }
        stream->handle = -1;
    }

    pthread_cond_destroy(&stream->cond);
    pthread_mutex_destroy(&stream->lock);
    free(stream->callback_buf);
    free(stream);

    return rc;
}

aaudio_stream_state_t AAudioStream_getState(AAudioStream* stream)
{
    if (!stream) {
        return AAUDIO_STREAM_STATE_UNINITIALIZED;
    }
    pthread_mutex_lock(&stream->lock);
    int32_t state = stream->state;
    pthread_mutex_unlock(&stream->lock);
    return state;
}

aaudio_result_t AAudioStream_waitForStateChange(AAudioStream* stream,
                                                aaudio_stream_state_t inputState,
                                                aaudio_stream_state_t* nextState,
                                                int64_t timeoutNanoseconds)
{
    if (!stream) {
        return AAUDIO_ERROR_NULL;
    }

    int64_t deadline = timeoutNanoseconds < 0 ? -1 : aaudio_now_ns() + timeoutNanoseconds;

    pthread_mutex_lock(&stream->lock);
    while (stream->state == inputState) {
        if (deadline < 0) {
            pthread_cond_wait(&stream->cond, &stream->lock);
            continue;
        }
        int64_t left = deadline - aaudio_now_ns();
        if (left <= 0) {
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(left / AAUDIO_NS_PER_SEC);
        ts.tv_nsec += (long)(left % AAUDIO_NS_PER_SEC);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&stream->cond, &stream->lock, &ts);
    }
    int32_t state = stream->state;
    pthread_mutex_unlock(&stream->lock);

    if (nextState) {
        *nextState = state;
    }
    return state == inputState ? AAUDIO_ERROR_TIMEOUT : AAUDIO_OK;
}

/* ============================================================
 * Blocking I/O — hypercall passthrough
 * ============================================================ */
aaudio_result_t AAudioStream_write(AAudioStream* stream, const void* buffer,
                                   int32_t numFrames, int64_t timeoutNanoseconds)
{
    (void)timeoutNanoseconds;
    if (!stream || stream->handle < 0 || stream->closed) {
        return AAUDIO_ERROR_INVALID_HANDLE;
    }
    if (!buffer || numFrames <= 0) {
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }
    if (stream->direction != AAUDIO_DIRECTION_OUTPUT) {
        return AAUDIO_ERROR_INVALID_STATE;
    }

    int64_t rc = AAUDIO_CALL3(SYS_ANDROID_AAUDIO_WRITE, stream->handle,
                              (intptr_t)buffer, numFrames);
    if (rc < 0) {
        return (aaudio_result_t)rc;
    }
    stream->frames_written += rc;
    return (aaudio_result_t)rc;
}

aaudio_result_t AAudioStream_read(AAudioStream* stream, void* buffer,
                                  int32_t numFrames, int64_t timeoutNanoseconds)
{
    (void)timeoutNanoseconds;
    if (!stream || stream->handle < 0 || stream->closed) {
        return AAUDIO_ERROR_INVALID_HANDLE;
    }
    if (!buffer || numFrames <= 0) {
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }
    if (stream->direction != AAUDIO_DIRECTION_INPUT) {
        return AAUDIO_ERROR_INVALID_STATE;
    }

    int64_t rc = AAUDIO_CALL3(SYS_ANDROID_AAUDIO_READ, stream->handle,
                              (intptr_t)buffer, numFrames);
    if (rc < 0) {
        return (aaudio_result_t)rc;
    }
    stream->frames_read += rc;
    return (aaudio_result_t)rc;
}

/* ============================================================
 * Introspection
 * ============================================================ */
int64_t AAudioStream_getFramesWritten(AAudioStream* stream)
{
    return stream ? stream->frames_written : 0;
}

int64_t AAudioStream_getFramesRead(AAudioStream* stream)
{
    return stream ? stream->frames_read : 0;
}

int32_t AAudioStream_getSampleRate(AAudioStream* stream)
{
    return stream ? stream->sample_rate : AAUDIO_UNSPECIFIED;
}

int32_t AAudioStream_getChannelCount(AAudioStream* stream)
{
    return stream ? stream->channel_count : AAUDIO_UNSPECIFIED;
}

aaudio_format_t AAudioStream_getFormat(AAudioStream* stream)
{
    return stream ? stream->format : AAUDIO_FORMAT_INVALID;
}

aaudio_sharing_mode_t AAudioStream_getSharingMode(AAudioStream* stream)
{
    return stream ? stream->sharing_mode : AAUDIO_SHARING_MODE_SHARED;
}

aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream* stream)
{
    return stream ? stream->performance_mode : AAUDIO_PERFORMANCE_MODE_NONE;
}

aaudio_direction_t AAudioStream_getDirection(AAudioStream* stream)
{
    return stream ? stream->direction : AAUDIO_DIRECTION_OUTPUT;
}

aaudio_usage_t AAudioStream_getUsage(AAudioStream* stream)
{
    return stream ? stream->usage : AAUDIO_USAGE_MEDIA;
}

aaudio_content_type_t AAudioStream_getContentType(AAudioStream* stream)
{
    return stream ? stream->content_type : AAUDIO_CONTENT_TYPE_MUSIC;
}

aaudio_input_preset_t AAudioStream_getInputPreset(AAudioStream* stream)
{
    return stream ? stream->input_preset : AAUDIO_INPUT_PRESET_GENERIC;
}

int32_t AAudioStream_getDeviceId(AAudioStream* stream)
{
    return stream ? stream->device_id : AAUDIO_DEVICE_ID_UNSPECIFIED;
}

aaudio_session_id_t AAudioStream_getSessionId(AAudioStream* stream)
{
    return stream ? stream->session_id : AAUDIO_SESSION_ID_NONE;
}

int32_t AAudioStream_getFramesPerBurst(AAudioStream* stream)
{
    return stream ? stream->frames_per_burst : 0;
}

int32_t AAudioStream_getBufferSizeInFrames(AAudioStream* stream)
{
    return stream ? stream->buffer_size_frames : 0;
}

int32_t AAudioStream_setBufferSizeInFrames(AAudioStream* stream, int32_t numFrames)
{
    if (!stream || stream->handle < 0) {
        return AAUDIO_ERROR_INVALID_HANDLE;
    }
    int64_t rc = AAUDIO_CALL2(SYS_ANDROID_AAUDIO_BUFSZ, stream->handle, numFrames);
    if (rc < 0) {
        return (int32_t)rc;
    }
    stream->buffer_size_frames = (int32_t)rc;
    return (int32_t)rc;
}

int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream* stream)
{
    return stream ? stream->buffer_capacity_frames : 0;
}

int32_t AAudioStream_getXRunCount(AAudioStream* stream)
{
    return stream ? stream->xrun_count : 0;
}

int32_t AAudioStream_getFramesPerDataCallback(AAudioStream* stream)
{
    return stream ? stream->frames_per_data_callback : AAUDIO_UNSPECIFIED;
}

aaudio_result_t AAudioStream_getTimestamp(AAudioStream* stream, clockid_t clockid,
                                          int64_t* framePosition,
                                          int64_t* timeNanoseconds)
{
    (void)clockid;
    if (!stream || stream->handle < 0) {
        return AAUDIO_ERROR_INVALID_HANDLE;
    }
    if (!framePosition || !timeNanoseconds) {
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }

    vp_aaudio_timestamp_t ts;
    memset(&ts, 0, sizeof(ts));
    int64_t rc = AAUDIO_CALL2(SYS_ANDROID_AAUDIO_TS, stream->handle, (intptr_t)&ts);
    if (rc != VP_AUDIO_OK) {
        return (aaudio_result_t)rc;
    }

    *framePosition   = ts.position;
    *timeNanoseconds = ts.timestamp_ns;
    return AAUDIO_OK;
}

/* ============================================================
 * Diagnostics
 * ============================================================ */
const char* AAudio_convertResultToText(aaudio_result_t returnCode)
{
    switch (returnCode) {
        case AAUDIO_OK:                     return "AAUDIO_OK";
        case AAUDIO_ERROR_DISCONNECTED:     return "AAUDIO_ERROR_DISCONNECTED";
        case AAUDIO_ERROR_ILLEGAL_ARGUMENT: return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
        case AAUDIO_ERROR_INTERNAL:         return "AAUDIO_ERROR_INTERNAL";
        case AAUDIO_ERROR_INVALID_STATE:    return "AAUDIO_ERROR_INVALID_STATE";
        case AAUDIO_ERROR_INVALID_HANDLE:   return "AAUDIO_ERROR_INVALID_HANDLE";
        case AAUDIO_ERROR_UNIMPLEMENTED:    return "AAUDIO_ERROR_UNIMPLEMENTED";
        case AAUDIO_ERROR_UNAVAILABLE:      return "AAUDIO_ERROR_UNAVAILABLE";
        case AAUDIO_ERROR_NO_FREE_HANDLES:  return "AAUDIO_ERROR_NO_FREE_HANDLES";
        case AAUDIO_ERROR_NO_MEMORY:        return "AAUDIO_ERROR_NO_MEMORY";
        case AAUDIO_ERROR_NULL:             return "AAUDIO_ERROR_NULL";
        case AAUDIO_ERROR_TIMEOUT:          return "AAUDIO_ERROR_TIMEOUT";
        case AAUDIO_ERROR_WOULD_BLOCK:      return "AAUDIO_ERROR_WOULD_BLOCK";
        case AAUDIO_ERROR_INVALID_FORMAT:   return "AAUDIO_ERROR_INVALID_FORMAT";
        case AAUDIO_ERROR_OUT_OF_RANGE:     return "AAUDIO_ERROR_OUT_OF_RANGE";
        case AAUDIO_ERROR_NO_SERVICE:       return "AAUDIO_ERROR_NO_SERVICE";
        case AAUDIO_ERROR_INVALID_RATE:     return "AAUDIO_ERROR_INVALID_RATE";
        default:                            return "AAUDIO_ERROR_UNKNOWN";
    }
}

const char* AAudio_convertStreamStateToText(aaudio_stream_state_t state)
{
    switch (state) {
        case AAUDIO_STREAM_STATE_UNINITIALIZED: return "UNINITIALIZED";
        case AAUDIO_STREAM_STATE_UNKNOWN:       return "UNKNOWN";
        case AAUDIO_STREAM_STATE_OPENING:       return "OPENING";
        case AAUDIO_STREAM_STATE_OPEN:          return "OPEN";
        case AAUDIO_STREAM_STATE_STARTING:      return "STARTING";
        case AAUDIO_STREAM_STATE_STARTED:       return "STARTED";
        case AAUDIO_STREAM_STATE_PAUSING:       return "PAUSING";
        case AAUDIO_STREAM_STATE_PAUSED:        return "PAUSED";
        case AAUDIO_STREAM_STATE_FLUSHING:      return "FLUSHING";
        case AAUDIO_STREAM_STATE_FLUSHED:       return "FLUSHED";
        case AAUDIO_STREAM_STATE_STOPPING:      return "STOPPING";
        case AAUDIO_STREAM_STATE_STOPPED:       return "STOPPED";
        case AAUDIO_STREAM_STATE_CLOSING:       return "CLOSING";
        case AAUDIO_STREAM_STATE_CLOSED:        return "CLOSED";
        case AAUDIO_STREAM_STATE_DISCONNECTED:  return "DISCONNECTED";
        default:                                return "UNKNOWN";
    }
}
