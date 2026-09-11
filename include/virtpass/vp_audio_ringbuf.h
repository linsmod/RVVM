/*
 * vp_audio_ringbuf.h - AAudio transport ABI (wire protocol only)
 *
 * Guest (riscv64) and host (arm64/x86_64) share this header: it defines the
 * command numbers, result codes, and wire structures for the AAudio proxy.
 *
 * Data path: hypercall passthrough.
 *   guest AAudioStream_write(buf, n)
 *     -> ecall(WRITE, handle, guest_buf_ptr, n)
 *     -> host memcpy from guest buffer -> real AAudioStream_write
 *
 * No shared ring buffer. Each data transfer is one hypercall + one memcpy.
 */

#ifndef VP_AUDIO_RINGBUF_H
#define VP_AUDIO_RINGBUF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Sub-commands for SYS_ANDROID_CALL (0x10022), passed in a0.
 * ============================================================ */
#ifndef VP_AUDIO_SYS_BASE
#define VP_AUDIO_SYS_BASE 0x10000
#endif

#define SYS_ANDROID_AAUDIO_OPEN   (VP_AUDIO_SYS_BASE + 40)
#define SYS_ANDROID_AAUDIO_CLOSE  (VP_AUDIO_SYS_BASE + 41)
#define SYS_ANDROID_AAUDIO_START  (VP_AUDIO_SYS_BASE + 42)
#define SYS_ANDROID_AAUDIO_PAUSE  (VP_AUDIO_SYS_BASE + 43)
#define SYS_ANDROID_AAUDIO_STOP   (VP_AUDIO_SYS_BASE + 44)
#define SYS_ANDROID_AAUDIO_FLUSH  (VP_AUDIO_SYS_BASE + 45)
#define SYS_ANDROID_AAUDIO_WRITE  (VP_AUDIO_SYS_BASE + 46)
#define SYS_ANDROID_AAUDIO_READ   (VP_AUDIO_SYS_BASE + 47)
#define SYS_ANDROID_AAUDIO_INFO   (VP_AUDIO_SYS_BASE + 48)
#define SYS_ANDROID_AAUDIO_TS     (VP_AUDIO_SYS_BASE + 49)
#define SYS_ANDROID_AAUDIO_BUFSZ  (VP_AUDIO_SYS_BASE + 50)
#define SYS_ANDROID_AAUDIO_QUERY  (VP_AUDIO_SYS_BASE + 51)

/* ============================================================
 * Result codes (host -> guest, plain int32_t)
 * ============================================================ */
#define VP_AUDIO_OK                    0
#define VP_AUDIO_ERROR_UNEXPECTED     (-1)
#define VP_AUDIO_ERROR_INVALID_ARG    (-2)
#define VP_AUDIO_ERROR_UNSUPPORTED    (-3)
#define VP_AUDIO_ERROR_NO_MEMORY      (-4)
#define VP_AUDIO_ERROR_CLOSED         (-5)
#define VP_AUDIO_ERROR_DISCONNECTED   (-6)
#define VP_AUDIO_ERROR_TIMEOUT        (-7)
#define VP_AUDIO_ERROR_NO_DEVICE      (-8)
#define VP_AUDIO_ERROR_WOULD_BLOCK    (-9)
#define VP_AUDIO_ERROR_INVALID_STATE  (-10)
#define VP_AUDIO_ERROR_INTERNAL       (-11)

/* ============================================================
 * Enumerations (values mirror <aaudio/AAudio.h>)
 * ============================================================ */
#define VP_AUDIO_DIR_OUTPUT   0
#define VP_AUDIO_DIR_INPUT    1

#define VP_AUDIO_SHARING_EXCLUSIVE 1
#define VP_AUDIO_SHARING_SHARED    2

#define VP_AUDIO_PERF_NONE        10
#define VP_AUDIO_PERF_POWER_SAVING 11
#define VP_AUDIO_PERF_LOW_LATENCY 12

#define VP_AUDIO_FMT_INVALID     (-1)
#define VP_AUDIO_FMT_UNSPECIFIED 0
#define VP_AUDIO_FMT_I16         1
#define VP_AUDIO_FMT_FLOAT       2
#define VP_AUDIO_FMT_I24         3
#define VP_AUDIO_FMT_I32         4

#define VP_AUDIO_USAGE_MEDIA                   1
#define VP_AUDIO_USAGE_VOICE_COMMUNICATION     2
#define VP_AUDIO_USAGE_ALARM                   4
#define VP_AUDIO_USAGE_NOTIFICATION            5
#define VP_AUDIO_USAGE_GAME                    14

#define VP_AUDIO_CONTENT_SPEECH      1
#define VP_AUDIO_CONTENT_MUSIC       2
#define VP_AUDIO_CONTENT_MOVIE       3
#define VP_AUDIO_CONTENT_SONIFICATION 4

#define VP_AUDIO_STATE_UNINITIALIZED 0
#define VP_AUDIO_STATE_UNKNOWN       1
#define VP_AUDIO_STATE_OPENING       2
#define VP_AUDIO_STATE_OPEN          3
#define VP_AUDIO_STATE_STARTING      4
#define VP_AUDIO_STATE_STARTED       5
#define VP_AUDIO_STATE_PAUSING       6
#define VP_AUDIO_STATE_PAUSED        7
#define VP_AUDIO_STATE_FLUSHING      8
#define VP_AUDIO_STATE_FLUSHED       9
#define VP_AUDIO_STATE_STOPPING      10
#define VP_AUDIO_STATE_STOPPED       11
#define VP_AUDIO_STATE_CLOSING       12
#define VP_AUDIO_STATE_CLOSED        13
#define VP_AUDIO_STATE_DISCONNECTED  14

/* Capability bits returned by SYS_ANDROID_AAUDIO_QUERY */
#define VP_AUDIO_CAP_OUTPUT      (1u << 0)
#define VP_AUDIO_CAP_INPUT       (1u << 1)
#define VP_AUDIO_CAP_LOW_LATENCY (1u << 2)
#define VP_AUDIO_CAP_EXCLUSIVE   (1u << 3)
#define VP_AUDIO_CAP_TIMESTAMP   (1u << 4)
#define VP_AUDIO_CAP_CALLBACK    (1u << 5)

static inline uint32_t vp_audio_bytes_per_sample(int32_t format)
{
    switch (format) {
        case VP_AUDIO_FMT_I16:   return 2;
        case VP_AUDIO_FMT_I24:   return 3;
        case VP_AUDIO_FMT_FLOAT:
        case VP_AUDIO_FMT_I32:   return 4;
        default:                 return 0;
    }
}

static inline uint32_t vp_audio_frame_bytes(int32_t format, int32_t channels)
{
    if (format < 0 || channels <= 0) {
        return 0;
    }
    return vp_audio_bytes_per_sample(format) * (uint32_t)channels;
}

/* ============================================================
 * Wire structures passed by address through SYS_ANDROID_CALL
 * ============================================================ */

/* a1 -> vp_aaudio_config_t* : AAudioStreamBuilder_openStream() request */
typedef struct vp_aaudio_config {
    int32_t  direction;          /* VP_AUDIO_DIR_*                          */
    int32_t  sharing_mode;       /* VP_AUDIO_SHARING_*                      */
    int32_t  performance_mode;   /* VP_AUDIO_PERF_*                         */
    int32_t  format;             /* VP_AUDIO_FMT_*                          */
    int32_t  sample_rate;        /* Hz, 0 = host default                    */
    int32_t  channel_count;      /* 1..8, 0 = host default                  */
    int32_t  usage;              /* VP_AUDIO_USAGE_*                        */
    int32_t  content_type;       /* VP_AUDIO_CONTENT_*                      */
    int32_t  input_preset;       /* aaudio_input_preset_t                   */
    int32_t  device_id;          /* 0 = unspecified                         */
    int32_t  session_id;         /* -1 = none                               */
    int32_t  buffer_frames;      /* requested buffer capacity, in frames    */
    int32_t  frames_per_burst;   /* host answer: its preferred burst        */
    int32_t  preferred_state;    /* state the host should reach after open  */
    int32_t  callback_flags;     /* 1 = guest wants a data callback         */
    int32_t  timeout_ns;         /* host-side operation timeout             */
    int64_t  app_token;          /* opaque guest stream id, echoed back     */
    int32_t  reserved[8];
} vp_aaudio_config_t;

/* a2 -> vp_aaudio_info_t* : negotiated geometry / current state */
typedef struct vp_aaudio_info {
    int32_t  direction;
    int32_t  sample_rate;        /* effective                            */
    int32_t  channel_count;      /* effective                            */
    int32_t  format;             /* effective                            */
    int32_t  sharing_mode;       /* effective                            */
    int32_t  performance_mode;   /* effective                            */
    int32_t  usage;
    int32_t  content_type;
    int32_t  device_id;
    int32_t  session_id;
    int32_t  frames_per_burst;   /* backend burst size, in frames        */
    int32_t  buffer_size_frames; /* buffer the backend keeps queued      */
    int32_t  buffer_capacity_frames; /* buffer capacity, in frames       */
    int32_t  frame_bytes;        /* channels * bytes per sample          */
    int32_t  state;              /* VP_AUDIO_STATE_*                     */
    int32_t  xrun_count;         /* underruns + overruns observed        */
    int64_t  app_token;
    int32_t  reserved[8];
} vp_aaudio_info_t;

/* a2 -> vp_aaudio_timestamp_t* : AAudioStream_getTimestamp() result */
typedef struct vp_aaudio_timestamp {
    int64_t  position;       /* frames presented/captured (monotonic)     */
    int64_t  timestamp_ns;   /* CLOCK_MONOTONIC time of that position     */
    int32_t  sample_rate;
    int32_t  reserved[5];
} vp_aaudio_timestamp_t;

#ifdef __cplusplus
}
#endif

#endif /* VP_AUDIO_RINGBUF_H */
