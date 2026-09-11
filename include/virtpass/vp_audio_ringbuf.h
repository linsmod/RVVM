/*
 * vp_audio_ringbuf.h - Shared AAudio transport ABI + lock-free PCM ring buffer
 *
 * Guest (riscv64) and host (arm64/x86_64) share this header: it defines the
 * wire structures that travel through SYS_ANDROID_CALL for the AAudio proxy.
 *
 * Data path (no copies in the steady state):
 *
 *   guest                                    host
 *   -----                                    ----
 *   AAudioStream_write() --+                 +--> WASAPI render / AAudio write
 *                          |                 |
 *                     [PCM ring]  <-------->  [PCM ring]
 *                          |                 |
 *   AAudioStream_read() <--+                 +<-- WASAPI capture / AAudio read
 *
 * - The ring lives in guest memory. Guest virtual addresses are identity
 *   mapped, so the host dereferences the pointers the guest hands over.
 * - The ring is strictly single-producer/single-consumer: exactly one side
 *   owns `head`, the other owns `tail`. Both are free-running frame counters,
 *   which makes full/empty detection trivial and wraparound free.
 * - The guest owns a pipe; the host writes a wake code into it whenever the
 *   ring has room for (or data available to) the guest. The guest blocks in
 *   read()/poll() instead of spinning inside a hypercall.
 */

#ifndef VP_AUDIO_RINGBUF_H
#define VP_AUDIO_RINGBUF_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol revision exchanged at AAUDIO_QUERY time. Bump on layout changes. */
#define VP_AUDIO_PROTOCOL_VERSION 1

/* ============================================================
 * Sub-commands for SYS_ANDROID_CALL (0x10022), passed in a0.
 * Reserved in the 40..49 window, clear of the sensor/window/game/
 * choreographer groups which stop at 28.
 *
 *   OPEN    a1 = vp_aaudio_config_t*  ->  stream handle (>= 0) | error
 *   CLOSE   a1 = handle
 *   START   a1 = handle, a2 = timeout_ns
 *   PAUSE   a1 = handle, a2 = timeout_ns
 *   STOP    a1 = handle, a2 = timeout_ns
 *   FLUSH   a1 = handle
 *   NOTIFY  a1 = handle, a2 = VP_AUDIO_NOTIFY_*
 *   INFO    a1 = handle, a2 = vp_aaudio_info_t*
 *   TS      a1 = handle, a2 = vp_aaudio_timestamp_t*
 *   BUFSZ   a1 = handle, a2 = requested frames  ->  applied frames | error
 *   QUERY   ->  VP_AUDIO_CAP_* bitmask
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
#define SYS_ANDROID_AAUDIO_NOTIFY (VP_AUDIO_SYS_BASE + 46)
#define SYS_ANDROID_AAUDIO_INFO   (VP_AUDIO_SYS_BASE + 47)
#define SYS_ANDROID_AAUDIO_TS     (VP_AUDIO_SYS_BASE + 48)
#define SYS_ANDROID_AAUDIO_BUFSZ  (VP_AUDIO_SYS_BASE + 49)
#define SYS_ANDROID_AAUDIO_QUERY  (VP_AUDIO_SYS_BASE + 50)

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
 * Enumerations carried as int32_t (values mirror <aaudio/AAudio.h>)
 * so neither side has to share the NDK header.
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
#define VP_AUDIO_CAP_FD_WAKEUP   (1u << 6)
#define VP_AUDIO_CAP_SHARED_ALIAS_PLACEHOLDER (1u << 7)

/* host -> guest wake payload written into the guest's pipe */
#define VP_AUDIO_WAKE_WRITE_SPACE  1  /* output: room appeared in the ring    */
#define VP_AUDIO_WAKE_READ_DATA    2  /* input:  frames landed in the ring    */
#define VP_AUDIO_WAKE_STATE_CHANGED 3 /* stream state changed, re-read info   */
#define VP_AUDIO_WAKE_SOURCE_LOST  4  /* backend gone, fail outstanding calls */

/* guest -> host notification kind (SYS_ANDROID_AAUDIO_NOTIFY a2) */
#define VP_AUDIO_NOTIFY_WROTE  1  /* guest produced frames (output path) */
#define VP_AUDIO_NOTIFY_READ   2  /* guest consumed frames (input path)  */
#define VP_AUDIO_NOTIFY_STATE  3  /* guest wants the host to re-apply    */

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
 * SPSC PCM ring buffer
 *
 * `data` points at the PCM storage (channels * bytes-per-sample per frame).
 * The storage must hold capacity_frames frames. Cursor arithmetic uses
 * unsigned wraparound; `count`/`space` stay correct as long as the producer
 * never runs more than 2^32 frames ahead of the consumer.
 * ============================================================ */
typedef struct {
    atomic_uint_fast32_t head;   /* producer cursor, in frames (monotonic) */
    atomic_uint_fast32_t tail;   /* consumer cursor, in frames (monotonic) */
    uint32_t capacity_frames;    /* frames the storage can hold            */
    uint32_t frame_bytes;        /* channels * bytes per sample            */
    uint64_t data;               /* guest address of the PCM storage       */
    uint32_t reserved[4];        /* keep the two cursors in separate lines */
} vp_audio_ring_t;

static inline void vp_audio_ring_init(vp_audio_ring_t* ring, void* data,
                                      uint32_t capacity_frames, uint32_t frame_bytes)
{
    atomic_store_explicit(&ring->head, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->tail, 0, memory_order_relaxed);
    ring->capacity_frames = capacity_frames;
    ring->frame_bytes = frame_bytes;
    ring->data = (uint64_t)(uintptr_t)data;
    for (uint32_t i = 0; i < 4; i++) {
        ring->reserved[i] = 0;
    }
}

/* Frames the consumer may read right now. */
static inline uint32_t vp_audio_ring_count(const vp_audio_ring_t* ring)
{
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    return (uint32_t)(head - tail);
}

/* Frames the producer may write right now. */
static inline uint32_t vp_audio_ring_space(const vp_audio_ring_t* ring)
{
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return ring->capacity_frames - (uint32_t)(head - tail);
}

/*
 * Zero-copy producer view: hands back up to two contiguous spans that
 * together hold `frames` writable frames. Call vp_audio_ring_commit_write()
 * once the spans are filled.
 */
static inline uint32_t vp_audio_ring_peek_write(vp_audio_ring_t* ring,
                                                void** span0, uint32_t* frames0,
                                                void** span1, uint32_t* frames1,
                                                uint32_t frames)
{
    uint32_t space = vp_audio_ring_space(ring);
    if (frames > space) {
        frames = space;
    }
    uint8_t* base = (uint8_t*)(uintptr_t)ring->data;
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t index = head % ring->capacity_frames;
    uint32_t first = ring->capacity_frames - index;
    if (first > frames) {
        first = frames;
    }
    *span0 = base + (size_t)index * ring->frame_bytes;
    *frames0 = first;
    if (frames > first) {
        *span1 = base;
        *frames1 = frames - first;
    } else {
        *span1 = NULL;
        *frames1 = 0;
    }
    return frames;
}

static inline void vp_audio_ring_commit_write(vp_audio_ring_t* ring, uint32_t frames)
{
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    atomic_store_explicit(&ring->head, head + frames, memory_order_release);
}

/*
 * Zero-copy consumer view: mirrors peek_write for the reading side.
 */
static inline uint32_t vp_audio_ring_peek_read(vp_audio_ring_t* ring,
                                               const void** span0, uint32_t* frames0,
                                               const void** span1, uint32_t* frames1,
                                               uint32_t frames)
{
    uint32_t count = vp_audio_ring_count(ring);
    if (frames > count) {
        frames = count;
    }
    const uint8_t* base = (const uint8_t*)(uintptr_t)ring->data;
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    uint32_t index = tail % ring->capacity_frames;
    uint32_t first = ring->capacity_frames - index;
    if (first > frames) {
        first = frames;
    }
    *span0 = base + (size_t)index * ring->frame_bytes;
    *frames0 = first;
    if (frames > first) {
        *span1 = base;
        *frames1 = frames - first;
    } else {
        *span1 = NULL;
        *frames1 = 0;
    }
    return frames;
}

static inline void vp_audio_ring_commit_read(vp_audio_ring_t* ring, uint32_t frames)
{
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    atomic_store_explicit(&ring->tail, tail + frames, memory_order_release);
}

/* Blocking write: returns the frames actually copied (0 only on error). */
static inline uint32_t vp_audio_ring_write(vp_audio_ring_t* ring,
                                           const void* src, uint32_t frames)
{
    const uint8_t* in = (const uint8_t*)src;
    uint32_t written = 0;
    while (written < frames) {
        void* s0;
        void* s1;
        uint32_t n0;
        uint32_t n1;
        uint32_t got = vp_audio_ring_peek_write(ring, &s0, &n0, &s1, &n1, frames - written);
        if (got == 0) {
            break;
        }
        if (n0) {
            memcpy(s0, in + (size_t)written * ring->frame_bytes,
                   (size_t)n0 * ring->frame_bytes);
        }
        if (n1) {
            memcpy(s1, in + (size_t)(written + n0) * ring->frame_bytes,
                   (size_t)n1 * ring->frame_bytes);
        }
        vp_audio_ring_commit_write(ring, got);
        written += got;
    }
    return written;
}

/* Blocking read: returns the frames actually copied (0 only on error). */
static inline uint32_t vp_audio_ring_read(vp_audio_ring_t* ring,
                                          void* dst, uint32_t frames)
{
    uint8_t* out = (uint8_t*)dst;
    uint32_t read_frames = 0;
    while (read_frames < frames) {
        const void* s0;
        const void* s1;
        uint32_t n0;
        uint32_t n1;
        uint32_t got = vp_audio_ring_peek_read(ring, &s0, &n0, &s1, &n1, frames - read_frames);
        if (got == 0) {
            break;
        }
        if (n0) {
            memcpy(out + (size_t)read_frames * ring->frame_bytes,
                   s0, (size_t)n0 * ring->frame_bytes);
        }
        if (n1) {
            memcpy(out + (size_t)(read_frames + n0) * ring->frame_bytes,
                   s1, (size_t)n1 * ring->frame_bytes);
        }
        vp_audio_ring_commit_read(ring, got);
        read_frames += got;
    }
    return read_frames;
}

/* Fill up to `frames` with silence (used on underrun instead of stalling). */
static inline uint32_t vp_audio_ring_fill_silence(vp_audio_ring_t* ring, uint32_t frames)
{
    uint32_t filled = 0;
    while (filled < frames) {
        void* s0;
        void* s1;
        uint32_t n0;
        uint32_t n1;
        uint32_t got = vp_audio_ring_peek_write(ring, &s0, &n0, &s1, &n1, frames - filled);
        if (got == 0) {
            break;
        }
        if (n0) {
            memset(s0, 0, (size_t)n0 * ring->frame_bytes);
        }
        if (n1) {
            memset(s1, 0, (size_t)n1 * ring->frame_bytes);
        }
        vp_audio_ring_commit_write(ring, got);
        filled += got;
    }
    return filled;
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
    int32_t  buffer_frames;      /* requested ring capacity, in frames      */
    int32_t  frames_per_burst;   /* host answer: its preferred burst        */
    int32_t  preferred_state;    /* state the host should reach after open  */
    int32_t  callback_flags;     /* 1 = guest wants a data callback         */
    int32_t  timeout_ns;         /* host-side operation timeout             */
    uint64_t ring;               /* guest address of vp_audio_ring_t        */
    int32_t  wake_fd;            /* guest pipe write end (host -> guest)    */
    int32_t  state_fd;           /* guest pipe write end for state changes  */
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
    int32_t  buffer_capacity_frames; /* ring capacity, in frames        */
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
