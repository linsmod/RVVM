/*
 * vp_sensor.c - host-side sensor subsystem.
 *
 * See vp_sensor.h for the state/threading model. This file is the only place
 * that knows about queues, the staging FIFO and the Looper wake edge; the
 * platform backends behind vp_sensor_ops_t only produce raw events.
 *
 * Wake edge rule (the reason the host never blocks on the guest's pipe):
 *   - a wake byte is written on the queue's empty -> non-empty edge only;
 *   - every guest consultation of a queue (QUEUE_READ / QUEUE_HAS) clears the
 *     armed flag, and the guest stub drains the pipe before consulting, so at
 *     most one byte is ever outstanding and write() cannot fill the pipe.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>

#include "utils.h"
#include "rvvm_types.h"
#include "util/locking.h"
#include "core/rvvm_user.h"

#include "vp_sensor.h"

#if defined(ANDROID)
#include <android/log.h>
#define SENSLOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "RVVM-SENSOR", fmt, ##__VA_ARGS__)
#else
#define SENSLOG(fmt, ...) printf("[sensor] " fmt "\n", ##__VA_ARGS__)
#endif

/* ============================================================
 * State
 * ============================================================ */

typedef struct {
    bool              used;
    int32_t           wake_fd;                            /* guest write end, -1 if none */
    bool              enabled[VP_SENSOR_MAX_HANDLES];
    int32_t           rate_us[VP_SENSOR_MAX_HANDLES];     /* 0 = backend default */
    int32_t           batch_us[VP_SENSOR_MAX_HANDLES];    /* 0 = no batching */
    bool              wake_armed;                         /* one wake byte outstanding */
    uint32_t          head;                               /* next write index */
    uint32_t          tail;                               /* oldest event index */
    uint32_t          count;
    uint64_t          dropped;                            /* overflow accounting */
    vp_sensor_event_t fifo[VP_SENSOR_FIFO_MAX_EVENTS];
} vp_sensor_queue_t;

static vp_sensor_queue_t      g_queues[VP_SENSOR_MAX_QUEUES];
static vp_sensor_info_t       g_desc[VP_SENSOR_MAX_HANDLES];
static int32_t                g_desc_count = 0;
static uint32_t               g_caps = 0;
static bool                   g_ready = false;
static const vp_sensor_ops_t* g_ops = NULL;

/* Zero-initialized static storage is a valid quiescent rvvm_lock_t. */
static rvvm_lock_t            g_lock;

/* Aggregate per-handle state last pushed to the backend, so a queue change
 * only produces a platform call when the aggregate actually changes. Only the
 * guest thread (and teardown) touches these. */
static bool                   g_backend_on[VP_SENSOR_MAX_HANDLES];
static int32_t                g_backend_rate[VP_SENSOR_MAX_HANDLES];

static void sensor_lock(void)   { rvvm_lock_slow(&g_lock); }
static void sensor_unlock(void) { rvvm_unlock(&g_lock); }

static vp_sensor_queue_t* sensor_queue(int64_t id)
{
    if (id < 0 || id >= VP_SENSOR_MAX_QUEUES || !g_queues[id].used) {
        return NULL;
    }
    return &g_queues[id];
}

static vp_sensor_info_t* sensor_desc(int32_t handle)
{
    if (handle < 0 || handle >= g_desc_count) {
        return NULL;
    }
    return &g_desc[handle];
}

/* ============================================================
 * Queue FIFO and wake fd
 * ============================================================ */

/* Cap the queue at VP_SENSOR_FIFO_MAX_EVENTS by dropping the oldest event,
 * which is what an Android HAL batch FIFO does when the app falls behind. */
static void sensor_fifo_push(vp_sensor_queue_t* q, const vp_sensor_event_t* ev)
{
    if (q->count == VP_SENSOR_FIFO_MAX_EVENTS) {
        q->tail = (q->tail + 1) % VP_SENSOR_FIFO_MAX_EVENTS;
        q->count--;
        q->dropped++;
    }
    q->fifo[q->head] = *ev;
    q->head = (q->head + 1) % VP_SENSOR_FIFO_MAX_EVENTS;
    q->count++;
}

/* Arm the guest's Looper fd on the empty -> non-empty edge. A full pipe means
 * one byte is already pending, which already wakes poll(), so treat that as
 * armed rather than blocking the sensor thread. */
static void sensor_wake(vp_sensor_queue_t* q)
{
    if (q->wake_armed || q->wake_fd < 0) {
        return;
    }
    q->wake_armed = true;

    uint8_t byte = 1;
    if (write(q->wake_fd, &byte, 1) != 1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        SENSLOG("queue wake fd %d write failed: %s (dropping it)", q->wake_fd, strerror(errno));
        q->wake_fd = -1;
    }
}

/* ============================================================
 * Producer entry point
 * ============================================================ */

void vp_sensor_ingest(int32_t handle, const vp_sensor_event_t* ev)
{
    if (!ev) {
        return;
    }

    sensor_lock();
    const vp_sensor_info_t* desc = g_ops ? sensor_desc(handle) : NULL;
    if (desc) {
        /* The wire identity is ours, not the backend's: fill it from the same
         * descriptor the guest enumerated, so ev.sensor/ev.type can never
         * disagree with ASensor_getHandle()/ASensor_getType(). */
        vp_sensor_event_t wire = *ev;
        wire.version = VP_SENSOR_EVENT_VERSION;
        wire.sensor = desc->handle;
        wire.type = desc->type;
        wire.flags = desc->wake_up ? VP_SENSOR_FLAG_WAKE_UP : 0;

        for (int32_t i = 0; i < VP_SENSOR_MAX_QUEUES; i++) {
            vp_sensor_queue_t* q = &g_queues[i];
            if (q->used && q->enabled[handle]) {
                sensor_fifo_push(q, &wire);
                sensor_wake(q);
            }
        }
    }
    sensor_unlock();
}

/* ============================================================
 * Backend aggregation
 * ============================================================ */

/* Push the aggregate state of one handle to the backend: the platform source
 * runs while at least one queue has the sensor enabled, at the fastest rate
 * anybody asked for. This is what keeps a backend free of queue knowledge. */
static void sensor_sync_handle(int32_t handle)
{
    bool any = false;
    int32_t period = 0;
    int32_t batch = 0;
    const vp_sensor_ops_t* ops = NULL;

    if (handle < 0 || handle >= VP_SENSOR_MAX_HANDLES) {
        return;
    }

    sensor_lock();
    for (int32_t i = 0; i < VP_SENSOR_MAX_QUEUES; i++) {
        const vp_sensor_queue_t* q = &g_queues[i];
        if (!q->used || !q->enabled[handle]) {
            continue;
        }
        any = true;
        int32_t r = q->rate_us[handle];
        if (r > 0 && (period == 0 || r < period)) {
            period = r;
        }
        int32_t b = q->batch_us[handle];
        if (b > 0 && (batch == 0 || b < batch)) {
            batch = b;
        }
    }
    ops = g_ops;
    sensor_unlock();

    if (!ops) {
        return;
    }
    if (period > 0 && period != g_backend_rate[handle] && ops->set_rate) {
        if (ops->set_rate(handle, period, batch) == VP_SENSOR_OK) {
            g_backend_rate[handle] = period;
        }
    }
    if (any != g_backend_on[handle] && ops->set_enabled) {
        if (ops->set_enabled(handle, any) == VP_SENSOR_OK) {
            g_backend_on[handle] = any;
        } else {
            SENSLOG("backend refused %s for handle %d", any ? "enable" : "disable", handle);
        }
    }
}

/* ============================================================
 * Manager init
 * ============================================================ */

static int64_t sensor_manager_init(void)
{
    int64_t rc;

    sensor_lock();
    if (!g_ready) {
        g_ready = true;
        g_caps = 0;

        if (g_ops) {
            if (g_ops->query) {
                g_caps = g_ops->query();
            }
            if ((g_caps & VP_SENSOR_CAP_LIST) && g_ops->enumerate) {
                int32_t n = g_ops->enumerate(g_desc, VP_SENSOR_MAX_HANDLES);
                g_desc_count = (n > 0 && n <= VP_SENSOR_MAX_HANDLES) ? n : 0;
            }
            for (int32_t i = 0; i < g_desc_count; i++) {
                /* The handle IS the descriptor index: a backend cannot hand out
                 * a handle the guest could not look up in the list it got. */
                g_desc[i].handle = i;
                /* Advertise only what the staging FIFO can actually hold. */
                if (g_desc[i].fifo_max_events <= 0 ||
                    g_desc[i].fifo_max_events > VP_SENSOR_FIFO_MAX_EVENTS) {
                    g_desc[i].fifo_max_events = VP_SENSOR_FIFO_MAX_EVENTS;
                }
            }
            if (g_desc_count == 0) {
                g_caps = 0;
            } else {
                bool wake_up = false;
                for (int32_t i = 0; i < g_desc_count; i++) {
                    if (g_desc[i].wake_up) {
                        wake_up = true;
                        break;
                    }
                }
                if (wake_up) {
                    g_caps |= VP_SENSOR_CAP_WAKEUP;
                } else {
                    g_caps &= ~VP_SENSOR_CAP_WAKEUP;
                }
            }
        }
    }
    rc = (int64_t)g_caps;
    sensor_unlock();
    return rc;
}

/* ============================================================
 * Sub-command dispatch
 * ============================================================ */

static int64_t sensor_queue_create(int64_t wake_fd)
{
    int64_t rc = VP_SENSOR_ERROR_UNSUPPORTED;

    sensor_lock();
    if (g_ops) {
        rc = VP_SENSOR_ERROR_NO_MEMORY;
        for (int32_t i = 0; i < VP_SENSOR_MAX_QUEUES; i++) {
            if (g_queues[i].used) {
                continue;
            }
            memset(&g_queues[i], 0, sizeof(g_queues[i]));
            g_queues[i].used = true;
            if (wake_fd >= 0 && (g_caps & VP_SENSOR_CAP_FD_WAKEUP)) {
                g_queues[i].wake_fd = (int32_t)wake_fd;
            } else {
                g_queues[i].wake_fd = -1;
            }
            rc = i;
            break;
        }
    }
    sensor_unlock();
    return rc;
}

static int64_t sensor_queue_destroy(int64_t id)
{
    int64_t rc = VP_SENSOR_ERROR_INVALID_ARG;

    sensor_lock();
    vp_sensor_queue_t* q = sensor_queue(id);
    if (q) {
        memset(q, 0, sizeof(*q));
        rc = VP_SENSOR_OK;
    }
    sensor_unlock();

    if (rc == VP_SENSOR_OK) {
        /* A handle can lose its last subscriber here. */
        for (int32_t h = 0; h < g_desc_count; h++) {
            sensor_sync_handle(h);
        }
    }
    return rc;
}

static int64_t sensor_queue_enable(int64_t id, int64_t handle, bool enable)
{
    int64_t rc = VP_SENSOR_ERROR_INVALID_ARG;

    sensor_lock();
    vp_sensor_queue_t* q = sensor_queue(id);
    if (q && sensor_desc((int32_t)handle)) {
        q->enabled[handle] = enable;
        rc = VP_SENSOR_OK;
    }
    sensor_unlock();

    if (rc == VP_SENSOR_OK) {
        sensor_sync_handle((int32_t)handle);
    }
    return rc;
}

static int64_t sensor_queue_set_rate(int64_t id, int64_t handle,
                                    int64_t period_us, int64_t batch_us)
{
    int64_t rc = VP_SENSOR_ERROR_INVALID_ARG;

    sensor_lock();
    vp_sensor_queue_t* q = sensor_queue(id);
    if (q && sensor_desc((int32_t)handle)) {
        /* 0 keeps the NDK meaning of "use the backend default". */
        q->rate_us[handle] = (int32_t)(period_us > 0 ? period_us : 0);
        q->batch_us[handle] = (int32_t)(batch_us > 0 ? batch_us : 0);
        rc = VP_SENSOR_OK;
    }
    sensor_unlock();

    if (rc == VP_SENSOR_OK) {
        sensor_sync_handle((int32_t)handle);
    }
    return rc;
}

static int64_t sensor_queue_read(int64_t id, int64_t guest_events, int64_t want)
{
    vp_sensor_event_t* dst = guest_events ? rvvm_user_guest_ptr((uint64_t)guest_events) : NULL;
    int64_t rc = 0;

    if (!dst || want <= 0) {
        return VP_SENSOR_ERROR_INVALID_ARG;
    }

    sensor_lock();
    vp_sensor_queue_t* q = sensor_queue(id);
    if (q) {
        int32_t n = 0;
        while (n < (int32_t)want && q->count > 0) {
            dst[n++] = q->fifo[q->tail];
            q->tail = (q->tail + 1) % VP_SENSOR_FIFO_MAX_EVENTS;
            q->count--;
        }
        /* The guest has consulted the queue, and its stub drained the wake
         * pipe before this call: the next event may arm it again. */
        q->wake_armed = false;
        rc = n;
    }
    sensor_unlock();
    return rc;
}

int64_t vp_sensor_dispatch(int64_t sub, int64_t a1, int64_t a2, int64_t a3, int64_t a4)
{
    switch (sub) {
        case VP_SENSOR_MANAGER_INIT:
            return sensor_manager_init();

        case VP_SENSOR_LIST: {
            vp_sensor_info_t* dst = a1 ? rvvm_user_guest_ptr((uint64_t)a1) : NULL;
            int32_t max = (int32_t)a2;
            int64_t rc = 0;

            if (!dst || max <= 0) {
                return 0;
            }
            sensor_lock();
            int32_t n = max < g_desc_count ? max : g_desc_count;
            if (n > 0) {
                memcpy(dst, g_desc, sizeof(g_desc[0]) * (size_t)n);
            }
            rc = n;
            sensor_unlock();
            return rc;
        }

        case VP_SENSOR_DEFAULT: {
            int32_t type = (int32_t)a1;
            int64_t rc = -1;

            sensor_lock();
            for (int32_t i = 0; i < g_desc_count; i++) {
                if (g_desc[i].type == type) {
                    rc = g_desc[i].handle;
                    break;
                }
            }
            sensor_unlock();
            return rc;
        }

        case VP_SENSOR_QUEUE_CREATE:
            return sensor_queue_create(a1);

        case VP_SENSOR_QUEUE_DESTROY:
            return sensor_queue_destroy(a1);

        case VP_SENSOR_QUEUE_ENABLE:
            return sensor_queue_enable(a1, a2, true);

        case VP_SENSOR_QUEUE_DISABLE:
            return sensor_queue_enable(a1, a2, false);

        case VP_SENSOR_QUEUE_SET_RATE:
            return sensor_queue_set_rate(a1, a2, a3, a4);

        case VP_SENSOR_QUEUE_HAS: {
            int64_t rc = VP_SENSOR_ERROR_INVALID_ARG;

            sensor_lock();
            vp_sensor_queue_t* q = sensor_queue(a1);
            if (q) {
                q->wake_armed = false;
                rc = q->count > 0 ? 1 : 0;
            }
            sensor_unlock();
            return rc;
        }

        case VP_SENSOR_QUEUE_READ:
            return sensor_queue_read(a1, a2, a3);

        default:
            return VP_SENSOR_ERROR_INVALID_ARG;
    }
}

/* ============================================================
 * Backend registration / teardown
 * ============================================================ */

void vp_sensor_set_ops(const vp_sensor_ops_t* ops)
{
    sensor_lock();
    g_ops = ops;
    sensor_unlock();
}

void vp_sensor_reset(void)
{
    /* Nothing the guest handed us survives a teardown: drop the queues first
     * so an in-flight ingest from the platform thread becomes a no-op. */
    sensor_lock();
    memset(g_queues, 0, sizeof(g_queues));
    sensor_unlock();

    /* The platform source is owned by the backend but armed by us: stop every
     * sensor we turned on before dropping the table. */
    if (g_ops && g_ops->set_enabled) {
        for (int32_t h = 0; h < VP_SENSOR_MAX_HANDLES; h++) {
            if (g_backend_on[h]) {
                g_ops->set_enabled(h, false);
            }
        }
    }
    memset(g_backend_on, 0, sizeof(g_backend_on));
    memset(g_backend_rate, 0, sizeof(g_backend_rate));

    sensor_lock();
    g_desc_count = 0;
    g_caps = 0;
    g_ready = false;
    g_ops = NULL;
    sensor_unlock();
}
