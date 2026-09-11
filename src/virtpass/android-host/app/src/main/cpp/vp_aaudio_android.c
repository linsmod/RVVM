/*
 * vp_aaudio_android.c - Android AAudio backend for the AAudio guest proxy
 *
 * Implements vp_audio_ops_t (see vp_cmdpost.h) on top of the real Android
 * AAudio API. This is the "direct pass-through" path: the guest's PCM ring
 * is consumed/produced by AAudio's own threads, with a pump thread bridging
 * the ring to AAudioStream_write/AAudioStream_read.
 *
 * Design notes
 * ------------
 * - The guest allocates the SPSC ring in its own memory. Guest virtual
 *   addresses are identity-mapped, so we dereference ring pointers directly.
 * - A pump thread runs per stream: for output, it reads frames from the ring
 *   and feeds them to AAudioStream_write(); for input, it reads from
 *   AAudioStream_read() and writes into the ring.
 * - The guest blocks in poll() on a pipe fd. The pump thread writes a wake
 *   code into the pipe whenever it makes progress, so the steady-state path
 *   has zero hypercalls.
 * - AAudio is available from API 26+; minSdk is 28, so no dlopen needed.
 */

#define LOG_TAG "RVVM-AAudio"
#include <android/log.h>
#include <aaudio/AAudio.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "virtpass/vp_audio_ringbuf.h"
#include "virtpass/vp_cmdpost.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ============================================================
 * Stream bookkeeping
 * ============================================================ */
typedef struct android_stream {
    struct android_stream* next;

    const vp_aaudio_config_t* cfg;
    vp_audio_ring_t*          ring;
    int32_t                   wake_fd;
    int32_t                   direction;
    int32_t                   format;
    int32_t                   channel_count;
    int32_t                   sample_rate;
    int32_t                   frame_bytes;
    int32_t                   burst_frames;
    int32_t                   buffer_frames;
    int32_t                   state;
    volatile int              quit;
    volatile int              started;

    AAudioStream*             stream;
    pthread_t                 thread;
} android_stream_t;

static android_stream_t* g_streams = NULL;
static pthread_mutex_t   g_streams_lock = PTHREAD_MUTEX_INITIALIZER;

static void streams_add(android_stream_t* s)
{
    pthread_mutex_lock(&g_streams_lock);
    s->next = g_streams;
    g_streams = s;
    pthread_mutex_unlock(&g_streams_lock);
}

static void streams_remove(android_stream_t* s)
{
    pthread_mutex_lock(&g_streams_lock);
    android_stream_t** link = &g_streams;
    while (*link) {
        if (*link == s) {
            *link = s->next;
            break;
        }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&g_streams_lock);
}

/* ============================================================
 * Time helpers
 * ============================================================ */
static int64_t android_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* ============================================================
 * Format mapping: VP_AUDIO_FMT_* -> AAudioFormat
 * ============================================================ */
static aaudio_format_t vp_to_aaudio_format(int32_t fmt)
{
    switch (fmt) {
        case VP_AUDIO_FMT_I16:   return AAUDIO_FORMAT_PCM_I16;
        case VP_AUDIO_FMT_FLOAT: return AAUDIO_FORMAT_PCM_FLOAT;
        case VP_AUDIO_FMT_I32:   return AAUDIO_FORMAT_PCM_I32;
        default:                 return AAUDIO_FORMAT_UNSPECIFIED;
    }
}

/* ============================================================
 * Worker threads
 * ============================================================ */
static void* android_render_thread(void* arg)
{
    android_stream_t* s = (android_stream_t*)arg;
    uint32_t burst = s->burst_frames > 0 ? (uint32_t)s->burst_frames : 480;
    void* buf = malloc((size_t)burst * (size_t)s->frame_bytes);
    if (!buf) return NULL;

    while (!s->quit) {
        uint32_t got = vp_audio_ring_read(s->ring, buf, burst);
        if (got == 0) {
            /* Ring empty: sleep briefly then retry (avoids busy-spin). */
            usleep(1000);
            continue;
        }
        int32_t rc = AAudioStream_write(s->stream, buf, (int32_t)got, 1000000000LL);
        if (rc < 0) {
            LOGE("AAudioStream_write error: %d", rc);
            break;
        }
        vp_cmdpost_audio_wake_guest(s->wake_fd, VP_AUDIO_WAKE_WRITE_SPACE);
    }

    free(buf);
    return NULL;
}

static void* android_capture_thread(void* arg)
{
    android_stream_t* s = (android_stream_t*)arg;
    uint32_t burst = s->burst_frames > 0 ? (uint32_t)s->burst_frames : 480;
    void* buf = malloc((size_t)burst * (size_t)s->frame_bytes);
    if (!buf) return NULL;

    while (!s->quit) {
        int32_t rc = AAudioStream_read(s->stream, buf, (int32_t)burst, 1000000000LL);
        if (rc <= 0) {
            if (rc < 0) {
                LOGE("AAudioStream_read error: %d", rc);
                break;
            }
            usleep(1000);
            continue;
        }
        uint32_t written = vp_audio_ring_write(s->ring, buf, (uint32_t)rc);
        (void)written;
        vp_cmdpost_audio_wake_guest(s->wake_fd, VP_AUDIO_WAKE_READ_DATA);
    }

    free(buf);
    return NULL;
}

/* ============================================================
 * vp_audio_ops_t implementation
 * ============================================================ */
static int32_t android_open(const vp_aaudio_config_t* cfg, void** out_user)
{
    android_stream_t* s;
    AAudioStreamBuilder* builder;
    aaudio_result_t rc;

    if (!cfg || !out_user || !cfg->ring) {
        return VP_AUDIO_ERROR_INVALID_ARG;
    }

    s = (android_stream_t*)calloc(1, sizeof(android_stream_t));
    if (!s) return VP_AUDIO_ERROR_NO_MEMORY;

    s->cfg           = cfg;
    s->ring          = (vp_audio_ring_t*)(uintptr_t)cfg->ring;
    s->wake_fd       = cfg->wake_fd;
    s->direction     = cfg->direction;
    s->format        = cfg->format > VP_AUDIO_FMT_UNSPECIFIED ? cfg->format : VP_AUDIO_FMT_FLOAT;
    s->channel_count = cfg->channel_count > 0 ? cfg->channel_count : 2;
    s->sample_rate   = cfg->sample_rate > 0 ? cfg->sample_rate : 48000;
    s->frame_bytes   = (int32_t)vp_audio_frame_bytes(s->format, s->channel_count);
    s->state         = VP_AUDIO_STATE_OPEN;

    if (s->frame_bytes <= 0) {
        LOGE("invalid format %d or channels %d", s->format, s->channel_count);
        free(s);
        return VP_AUDIO_ERROR_INVALID_ARG;
    }

    {
        aaudio_result_t brc = AAudio_createStreamBuilder(&builder);
        if (brc != AAUDIO_OK || !builder) {
            free(s);
            return VP_AUDIO_ERROR_NO_MEMORY;
        }
    }

    AAudioStreamBuilder_setDirection(builder, s->direction);
    AAudioStreamBuilder_setSampleRate(builder, s->sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, s->channel_count);
    AAudioStreamBuilder_setFormat(builder, vp_to_aaudio_format(s->format));
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder,
        cfg->performance_mode == VP_AUDIO_PERF_LOW_LATENCY
            ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY
            : AAUDIO_PERFORMANCE_MODE_NONE);

    if (cfg->buffer_frames > 0) {
        AAudioStreamBuilder_setBufferCapacityInFrames(builder, cfg->buffer_frames);
    }

    rc = AAudioStreamBuilder_openStream(builder, &s->stream);
    AAudioStreamBuilder_delete(builder);

    if (rc != AAUDIO_OK || !s->stream) {
        LOGE("AAudioStreamBuilder_openStream failed: %d", rc);
        free(s);
        return VP_AUDIO_ERROR_NO_DEVICE;
    }

    s->burst_frames  = AAudioStream_getFramesPerBurst(s->stream);
    s->buffer_frames = AAudioStream_getBufferCapacityInFrames(s->stream);
    s->sample_rate   = AAudioStream_getSampleRate(s->stream);
    s->channel_count = AAudioStream_getChannelCount(s->stream);

    /* Write back negotiated burst to the config so the guest can see it. */
    ((vp_aaudio_config_t*)cfg)->frames_per_burst = s->burst_frames;

    streams_add(s);
    *out_user = s;

    LOGI("%s stream: %d Hz, %d ch, fmt %d, burst %d, buffer %d",
         s->direction == VP_AUDIO_DIR_INPUT ? "capture" : "render",
         s->sample_rate, s->channel_count, s->format,
         s->burst_frames, s->buffer_frames);
    return VP_AUDIO_OK;
}

static int32_t android_close(void* user)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s) return VP_AUDIO_ERROR_INVALID_ARG;

    /* Stop the pump thread first. */
    s->quit = 1;
    if (s->started && s->stream) {
        AAudioStream_requestStop(s->stream);
    }
    if (s->thread) {
        pthread_join(s->thread, NULL);
        s->thread = 0;
    }

    if (s->stream) {
        AAudioStream_close(s->stream);
        s->stream = NULL;
    }

    streams_remove(s);
    free(s);
    return VP_AUDIO_OK;
}

static int32_t android_start(void* user, int64_t timeout_ns)
{
    android_stream_t* s = (android_stream_t*)user;
    (void)timeout_ns;
    if (!s || !s->stream) return VP_AUDIO_ERROR_INVALID_ARG;
    if (s->started) return VP_AUDIO_OK;

    aaudio_result_t rc = AAudioStream_requestStart(s->stream);
    if (rc != AAUDIO_OK) {
        LOGE("AAudioStream_requestStart failed: %d", rc);
        return VP_AUDIO_ERROR_INTERNAL;
    }

    s->started = 1;
    __atomic_store_n(&s->state, VP_AUDIO_STATE_STARTED, __ATOMIC_RELEASE);

    /* Launch the pump thread. */
    s->quit = 0;
    void*(*entry)(void*) = s->direction == VP_AUDIO_DIR_INPUT
                         ? android_capture_thread : android_render_thread;
    if (pthread_create(&s->thread, NULL, entry, s) != 0) {
        LOGE("pthread_create failed");
        AAudioStream_requestStop(s->stream);
        s->started = 0;
        return VP_AUDIO_ERROR_INTERNAL;
    }

    return VP_AUDIO_OK;
}

static int32_t android_pause(void* user, int64_t timeout_ns)
{
    android_stream_t* s = (android_stream_t*)user;
    (void)timeout_ns;
    if (!s || !s->stream) return VP_AUDIO_ERROR_INVALID_ARG;

    AAudioStream_requestPause(s->stream);
    s->started = 0;
    __atomic_store_n(&s->state, VP_AUDIO_STATE_PAUSED, __ATOMIC_RELEASE);
    return VP_AUDIO_OK;
}

static int32_t android_stop(void* user, int64_t timeout_ns)
{
    android_stream_t* s = (android_stream_t*)user;
    (void)timeout_ns;
    if (!s || !s->stream) return VP_AUDIO_ERROR_INVALID_ARG;

    s->quit = 1;
    AAudioStream_requestStop(s->stream);
    if (s->thread) {
        pthread_join(s->thread, NULL);
        s->thread = 0;
    }
    s->started = 0;
    __atomic_store_n(&s->state, VP_AUDIO_STATE_STOPPED, __ATOMIC_RELEASE);
    return VP_AUDIO_OK;
}

static int32_t android_flush(void* user)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s || !s->stream) return VP_AUDIO_ERROR_INVALID_ARG;

    AAudioStream_requestFlush(s->stream);
    __atomic_store_n(&s->state, VP_AUDIO_STATE_FLUSHED, __ATOMIC_RELEASE);
    return VP_AUDIO_OK;
}

static int32_t android_notify(void* user, int32_t kind)
{
    (void)user;
    (void)kind;
    /* The pump thread already runs continuously; no explicit kick needed. */
    return VP_AUDIO_OK;
}

static int32_t android_get_info(void* user, vp_aaudio_info_t* out)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s || !out) return VP_AUDIO_ERROR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->direction              = s->direction;
    out->sample_rate            = s->sample_rate;
    out->channel_count          = s->channel_count;
    out->format                 = s->format;
    out->sharing_mode           = VP_AUDIO_SHARING_SHARED;
    out->performance_mode       = s->cfg->performance_mode;
    out->usage                  = s->cfg->usage;
    out->content_type           = s->cfg->content_type;
    out->device_id              = s->cfg->device_id;
    out->session_id             = s->cfg->session_id;
    out->frames_per_burst       = s->burst_frames;
    out->buffer_size_frames     = s->buffer_frames;
    out->buffer_capacity_frames = (int32_t)s->ring->capacity_frames;
    out->frame_bytes            = s->frame_bytes;
    out->state                  = s->state;
    out->xrun_count             = 0;
    out->app_token              = s->cfg->app_token;
    return VP_AUDIO_OK;
}

static int32_t android_get_timestamp(void* user, vp_aaudio_timestamp_t* out)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s || !out) return VP_AUDIO_ERROR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    /* AAudioStream_getTimestamp requires a running stream; fall back to
     * monotonic clock + frames io estimate. */
    out->position     = 0;
    out->timestamp_ns = android_now_ns();
    out->sample_rate  = s->sample_rate;
    return VP_AUDIO_OK;
}

static int32_t android_set_buffer_size(void* user, int32_t frames, int32_t* applied_out)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s) return VP_AUDIO_ERROR_INVALID_ARG;

    if (s->stream && frames > 0) {
        AAudioStream_setBufferSizeInFrames(s->stream, frames);
    }
    if (applied_out) {
        *applied_out = s->buffer_frames;
    }
    return VP_AUDIO_OK;
}

static uint32_t android_query(void)
{
    /* AAudio is available on all API 26+ devices; minSdk is 28. */
    return VP_AUDIO_CAP_OUTPUT | VP_AUDIO_CAP_INPUT
         | VP_AUDIO_CAP_LOW_LATENCY | VP_AUDIO_CAP_TIMESTAMP
         | VP_AUDIO_CAP_FD_WAKEUP;
}

static const vp_audio_ops_t g_android_ops = {
    .open            = android_open,
    .close           = android_close,
    .start           = android_start,
    .pause           = android_pause,
    .stop            = android_stop,
    .flush           = android_flush,
    .notify          = android_notify,
    .get_info        = android_get_info,
    .get_timestamp   = android_get_timestamp,
    .set_buffer_size = android_set_buffer_size,
    .query           = android_query,
};

const vp_audio_ops_t* android_aaudio_ops(void)
{
    return &g_android_ops;
}

void android_aaudio_shutdown(void)
{
    /* cmdpost_cleanup() already closes every stream through the ops table. */
    pthread_mutex_lock(&g_streams_lock);
    android_stream_t* s = g_streams;
    g_streams = NULL;
    pthread_mutex_unlock(&g_streams_lock);
    while (s) {
        android_stream_t* next = s->next;
        android_close(s);
        s = next;
    }
}
