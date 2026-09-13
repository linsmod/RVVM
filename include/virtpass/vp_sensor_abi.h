/*
 * vp_sensor_abi.h - sensor transport ABI (wire protocol only)
 *
 * Guest (riscv64) and host (arm64 / x86_64 / win32) share this header: it
 * defines the sensor sub-commands, capability bits, result codes and the two
 * wire structures exchanged through SYS_ANDROID_CALL.
 *
 * There is deliberately no ring buffer and no shared memory here. The NDK's
 * only sensor data API is
 *
 *     ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* queue,
 *                                         ASensorEvent* events, size_t count);
 *
 * i.e. the caller supplies its own array and the host copies events into it
 * (see <android/sensor.h>). Virtpass mirrors that contract verbatim, so any
 * buffering the guest application wants stays the guest's own business. The
 * host side keeps a bounded staging FIFO internally (vp_sensor.c) purely to
 * decouple its sensor thread from the guest vCPU thread; that FIFO is an
 * implementation detail and is NOT visible over this ABI.
 *
 * The NDK Direct Channel mechanism (AHardwareBuffer / shared memory) is a
 * separate opt-in API and is intentionally absent: no Virtpass host implements
 * it, so nothing here pretends it exists.
 */

#ifndef VP_SENSOR_ABI_H
#define VP_SENSOR_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Sub-commands for SYS_ANDROID_CALL, passed in a0.
 *
 * Sensors own a contiguous window of the private proxy range (the audio block
 * starts at BASE + 40), so the numbers stay greppable in a trace and the
 * retired BASE + 1..4 block can never be reused by accident.
 * ============================================================ */
#define VP_SENSOR_SYS_BASE          0x10000

#define VP_SENSOR_MANAGER_INIT      (VP_SENSOR_SYS_BASE + 60)
#define VP_SENSOR_LIST              (VP_SENSOR_SYS_BASE + 61)
#define VP_SENSOR_DEFAULT           (VP_SENSOR_SYS_BASE + 62)
#define VP_SENSOR_QUEUE_CREATE      (VP_SENSOR_SYS_BASE + 63)
#define VP_SENSOR_QUEUE_DESTROY     (VP_SENSOR_SYS_BASE + 64)
#define VP_SENSOR_QUEUE_ENABLE      (VP_SENSOR_SYS_BASE + 65)
#define VP_SENSOR_QUEUE_DISABLE     (VP_SENSOR_SYS_BASE + 66)
#define VP_SENSOR_QUEUE_SET_RATE    (VP_SENSOR_SYS_BASE + 67)
#define VP_SENSOR_QUEUE_HAS         (VP_SENSOR_SYS_BASE + 68)
#define VP_SENSOR_QUEUE_READ        (VP_SENSOR_SYS_BASE + 69)

/* ============================================================
 * Result codes (host -> guest, plain int32_t)
 * ============================================================ */
#define VP_SENSOR_OK                 0
#define VP_SENSOR_ERROR_INVALID_ARG  (-1)
#define VP_SENSOR_ERROR_UNSUPPORTED  (-2)
#define VP_SENSOR_ERROR_NO_MEMORY    (-3)

/* ============================================================
 * Capability bits returned by VP_SENSOR_MANAGER_INIT.
 *
 * 0 means "this host has no sensor backend at all": the guest then sees an
 * empty sensor list, exactly like a device without the hardware, instead of a
 * fabricated stub sensor it could never read from.
 * ============================================================ */
#define VP_SENSOR_CAP_LIST      (1u << 0)  /* sensors can be enumerated */
#define VP_SENSOR_CAP_RATE      (1u << 1)  /* setEventRate is honoured     */
#define VP_SENSOR_CAP_WAKEUP    (1u << 2)  /* a wake-up sensor is present  */
#define VP_SENSOR_CAP_FD_WAKEUP (1u << 3)  /* host can write the queue wake fd */

/* ============================================================
 * Limits. Every host must fit these; they are part of the ABI so a guest can
 * size its own bookkeeping without asking.
 * ============================================================ */
#define VP_SENSOR_MAX_HANDLES      16
#define VP_SENSOR_MAX_QUEUES       4
#define VP_SENSOR_FIFO_MAX_EVENTS  64  /* staging FIFO depth per queue */
#define VP_SENSOR_NAME_MAX         64
#define VP_SENSOR_VENDOR_MAX       64
#define VP_SENSOR_STRING_TYPE_MAX  64

/* Reported in vp_sensor_event_t.version. */
#define VP_SENSOR_EVENT_VERSION    1

/* ============================================================
 * vp_sensor_event_t - the single event definition, shared by both sides.
 *
 * The layout is byte-identical to the NDK's ASensorEvent on every 64-bit
 * target (104 bytes, 8-byte aligned; vp_android.h makes ASensorEvent an alias
 * of this type), so a guest can pass its own ASensorEvent array straight
 * through and the host can memcpy a platform event into it. The named union
 * members are the subset of the NDK's that Virtpass sensors report.
 * ============================================================ */
typedef struct vp_sensor_event {
    int32_t version;      /* VP_SENSOR_EVENT_VERSION                        */
    int32_t sensor;       /* sensor handle (== ASensor_getHandle()), NDK semantics */
    int32_t type;         /* ASENSOR_TYPE_*                                 */
    int32_t reserved0;
    int64_t timestamp;    /* nanoseconds; the host's monotonic clock domain  */
    union {
        float data[16];
        struct { float x; float y; float z; float pad[13]; } vector;
        struct { float x; float y; float z; float pad[13]; } acceleration;
        struct { float x; float y; float z; float pad[13]; } magnetic;
        struct { float azimuth; float pitch; float roll; float pad[13]; } orientation;
        float light;
        float pressure;
        float temperature;
        float distance;
        float relative_humidity;
    };
    uint32_t flags;       /* VP_SENSOR_FLAG_*                                */
    int32_t reserved1[3];
} vp_sensor_event_t;

/* Set by the host on events that came from a wake-up sensor. */
#define VP_SENSOR_FLAG_WAKE_UP 0x1u

/* The guest memcpy's platform events into this type and passes its own array
 * to the host, so the layout is a hard ABI gate on both sides. */
_Static_assert(sizeof(vp_sensor_event_t) == 104, "vp_sensor_event_t ABI size");
_Static_assert(offsetof(vp_sensor_event_t, timestamp) == 16, "vp_sensor_event_t ABI offset");
_Static_assert(offsetof(vp_sensor_event_t, flags) == 88, "vp_sensor_event_t ABI offset");

/* ============================================================
 * vp_sensor_info_t - one entry of the sensor list.
 *
 * The fields are exactly the facts the NDK's ASensor accessors expose, no
 * more: the guest stub caches these once through VP_SENSOR_LIST and then
 * answers ASensor_getXxx() locally, exactly like the NDK accessors read their
 * ASensor object. Adding a field here that no NDK accessor can expose would
 * only create data the guest has no way to ask for.
 * ============================================================ */
typedef struct vp_sensor_info {
    int32_t handle;                 /* dense index; also ev.sensor          */
    int32_t type;                   /* ASENSOR_TYPE_*                       */
    int32_t reporting_mode;         /* AREPORTING_MODE_*                    */
    int32_t min_delay_us;           /* ASensor_getMinDelay()                */
    int32_t fifo_max_events;        /* <= VP_SENSOR_FIFO_MAX_EVENTS         */
    int32_t fifo_reserved_events;
    int32_t wake_up;                /* ASensor_isWakeUpSensor()             */
    int32_t highest_direct_rate_level;  /* always ASENSOR_DIRECT_RATE_STOP  */
    float   resolution;             /* ASensor_getResolution()              */
    char    string_type[VP_SENSOR_STRING_TYPE_MAX];
    char    name[VP_SENSOR_NAME_MAX];
    char    vendor[VP_SENSOR_VENDOR_MAX];
} vp_sensor_info_t;

#ifdef __cplusplus
}
#endif

#endif /* VP_SENSOR_ABI_H */
