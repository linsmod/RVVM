/*
 * vp_aaudio_android.c - Android AAudio backend for the AAudio guest proxy
 *
 * Implements vp_audio_ops_t (see vp_cmdpost.h) on top of the real Android
 * AAudio API. This is a thin passthrough: each WRITE/READ hypercall does a
 * memcpy between guest memory and a local buffer, then calls the real
 * AAudioStream_write/read. No pump threads, no shared ring buffer.
 */

#define LOG_TAG "RVVM-AAudio"
#include <android/log.h>
#include <aaudio/AAudio.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    int32_t                   direction;
    int32_t                   format;
    int32_t                   channel_count;
    int32_t                   sample_rate;
    int32_t                   frame_bytes;
    int32_t                   burst_frames;
    int32_t                   buffer_frames;
    int32_t                   state;

    AAudioStream*             stream;
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
 * vp_audio_ops_t implementation
 * ============================================================ */
static int32_t android_open(const vp_aaudio_config_t* cfg, void** out_user)
{
    android_stream_t* s;
    AAudioStreamBuilder* builder;
    aaudio_result_t rc;

    if (!cfg || !out_user) {
        return VP_AUDIO_ERROR_INVALID_ARG;
    }

    s = (android_stream_t*)calloc(1, sizeof(android_stream_t));
    if (!s) return VP_AUDIO_ERROR_NO_MEMORY;

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

    aaudio_result_t rc = AAudioStream_requestStart(s->stream);
    if (rc != AAUDIO_OK) {
        LOGE("AAudioStream_requestStart failed: %d", rc);
        return VP_AUDIO_ERROR_INTERNAL;
    }

    s->state = VP_AUDIO_STATE_STARTED;
    return VP_AUDIO_OK;
}

static int32_t android_pause(void* user, int64_t timeout_ns)
{
    android_stream_t* s = (android_stream_t*)user;
    (void)timeout_ns;
    if (!s || !s->stream) return VP_AUDIO_ERROR_INVALID_ARG;

    AAudioStream_requestPause(s->stream);
    s->state = VP_AUDIO_STATE_PAUSED;
    return VP_AUDIO_OK;
}

static int32_t android_stop(void* user, int64_t timeout_ns)
{
    android_stream_t* s = (android_stream_t*)user;
    (void)timeout_ns;
    if (!s || !s->stream) return VP_AUDIO_ERROR_INVALID_ARG;

    AAudioStream_requestStop(s->stream);
    s->state = VP_AUDIO_STATE_STOPPED;
    return VP_AUDIO_OK;
}

static int32_t android_flush(void* user)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s || !s->stream) return VP_AUDIO_ERROR_INVALID_ARG;

    AAudioStream_requestFlush(s->stream);
    s->state = VP_AUDIO_STATE_FLUSHED;
    return VP_AUDIO_OK;
}

/* ============================================================
 * Data-path passthrough: memcpy between guest and real AAudio
 * ============================================================ */
static int32_t android_write(void* user, const void* buf, int32_t frames, int32_t frame_bytes)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s || !s->stream) return VP_AUDIO_ERROR_INVALID_ARG;

    /* Copy from guest memory (identity-mapped) into a local buffer, then
     * hand it to the real AAudio stream.  The local copy is necessary
     * because AAudio may hold onto the buffer after write returns (async
     * rendering), and the guest could reclaim it immediately. */
    size_t bytes = (size_t)frames * (size_t)frame_bytes;
    void* local = malloc(bytes);
    if (!local) return VP_AUDIO_ERROR_NO_MEMORY;

    memcpy(local, buf, bytes);
    int32_t rc = AAudioStream_write(s->stream, local, frames, 1000000000LL);
    free(local);

    if (rc < 0) {
        LOGE("AAudioStream_write error: %d", rc);
        return VP_AUDIO_ERROR_INTERNAL;
    }
    return rc;
}

static int32_t android_read(void* user, void* buf, int32_t frames, int32_t frame_bytes)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s || !s->stream) return VP_AUDIO_ERROR_INVALID_ARG;

    /* Read into a local buffer, then copy to guest memory. */
    size_t bytes = (size_t)frames * (size_t)frame_bytes;
    void* local = malloc(bytes);
    if (!local) return VP_AUDIO_ERROR_NO_MEMORY;

    int32_t rc = AAudioStream_read(s->stream, local, frames, 1000000000LL);
    if (rc > 0) {
        memcpy(buf, local, (size_t)rc * (size_t)frame_bytes);
    }
    free(local);

    if (rc < 0) {
        LOGE("AAudioStream_read error: %d", rc);
        return VP_AUDIO_ERROR_INTERNAL;
    }
    return rc;
}

/* ============================================================
 * Introspection
 * ============================================================ */
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
    out->frames_per_burst       = s->burst_frames;
    out->buffer_size_frames     = s->buffer_frames;
    out->buffer_capacity_frames = s->buffer_frames;
    out->frame_bytes            = s->frame_bytes;
    out->state                  = s->state;
    out->xrun_count             = s->stream ? AAudioStream_getXRunCount(s->stream) : 0;
    return VP_AUDIO_OK;
}

static int32_t android_get_timestamp(void* user, vp_aaudio_timestamp_t* out)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s || !out) return VP_AUDIO_ERROR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    if (s->stream) {
        int64_t position = 0;
        int64_t timestamp_ns = 0;
        aaudio_result_t rc = AAudioStream_getTimestamp(s->stream, CLOCK_MONOTONIC,
                                                       &position, &timestamp_ns);
        if (rc == AAUDIO_OK) {
            out->position     = position;
            out->timestamp_ns = timestamp_ns;
            out->sample_rate  = s->sample_rate;
            return VP_AUDIO_OK;
        }
    }
    /* Fallback: monotonic clock, position unknown. */
    out->timestamp_ns = android_now_ns();
    out->sample_rate  = s->sample_rate;
    return VP_AUDIO_OK;
}

static int32_t android_set_buffer_size(void* user, int32_t frames, int32_t* applied_out)
{
    android_stream_t* s = (android_stream_t*)user;
    if (!s) return VP_AUDIO_ERROR_INVALID_ARG;

    if (s->stream && frames > 0) {
        int32_t actual = AAudioStream_setBufferSizeInFrames(s->stream, frames);
        if (actual > 0) {
            s->buffer_frames = actual;
        }
    }
    if (applied_out) {
        *applied_out = s->buffer_frames;
    }
    return VP_AUDIO_OK;
}

static uint32_t android_query(void)
{
    return VP_AUDIO_CAP_OUTPUT | VP_AUDIO_CAP_INPUT
         | VP_AUDIO_CAP_LOW_LATENCY | VP_AUDIO_CAP_TIMESTAMP;
}

static const vp_audio_ops_t g_android_ops = {
    .open            = android_open,
    .close           = android_close,
    .start           = android_start,
    .pause           = android_pause,
    .stop            = android_stop,
    .flush           = android_flush,
    .write           = android_write,
    .read            = android_read,
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
