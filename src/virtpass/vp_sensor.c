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
 *
 * The split between the device and an instance is what makes a second guest
 * possible: descriptors, queues and FIFOs live per instance (one guest cannot
 * name another's queue, and ending a run clears only its own), while the
 * backend, the aggregate it is driven with and the fan-out stay process-wide -
 * there is one accelerometer, and it serves every subscriber.
 */

#include <stdio.h>
#include <stdlib.h>  /* calloc/free: one instance per guest, made on demand */
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

/* How many guests can have sensors at once in one process. Each host runs one
 * guest at a time today; this is the ceiling for the day one wants more, and
 * vp_sensor_create() fails (a guest without sensors) beyond it rather than
 * silently dropping a guest's events. */
#define VP_SENSOR_MAX_INSTANCES 8

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

/* One guest's subscriber state. Zero-initialized storage is a valid starting
 * point (an empty queue table, an empty descriptor table, a quiescent lock). */
struct vp_sensor {
    vp_sensor_queue_t queues[VP_SENSOR_MAX_QUEUES];
    vp_sensor_info_t  desc[VP_SENSOR_MAX_HANDLES];
    int32_t           desc_count;
    uint32_t          caps;
    bool              ready;

    /* Guards this instance's descriptor table and queues. */
    rvvm_lock_t       lock;
};

/* ---- Device level ----
 *
 * Shared by every instance: the backend, the aggregate last pushed to it, and
 * the list of instances to fan events out to. Zero-initialized static storage
 * is a valid quiescent rvvm_lock_t. */
static const vp_sensor_ops_t* g_ops = NULL;
static bool                   g_backend_on[VP_SENSOR_MAX_HANDLES];
static int32_t                g_backend_rate[VP_SENSOR_MAX_HANDLES];
static rvvm_lock_t            g_dev_lock;
static vp_sensor_t*           g_instances[VP_SENSOR_MAX_INSTANCES];

static void sensor_lock(vp_sensor_t* self)   { rvvm_lock_slow(&self->lock); }
static void sensor_unlock(vp_sensor_t* self) { rvvm_unlock(&self->lock); }

/* The registered backend, read under the device lock. The pointer stays valid
 * until the host detaches it, and a caller must not hold this lock while using
 * it (the backend can be slow, and must never be called under a lock). */
static const vp_sensor_ops_t* sensor_ops(void)
{
    const vp_sensor_ops_t* ops;

    rvvm_lock_slow(&g_dev_lock);
    ops = g_ops;
    rvvm_unlock(&g_dev_lock);
    return ops;
}

static vp_sensor_queue_t* sensor_queue(vp_sensor_t* self, int64_t id)
{
    if (id < 0 || id >= VP_SENSOR_MAX_QUEUES || !self->queues[id].used) {
        return NULL;
    }
    return &self->queues[id];
}

static vp_sensor_info_t* sensor_desc(vp_sensor_t* self, int32_t handle)
{
    if (handle < 0 || handle >= self->desc_count) {
        return NULL;
    }
    return &self->desc[handle];
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
 * Producer entry point (device level)
 * ============================================================ */

void vp_sensor_ingest(int32_t handle, const vp_sensor_event_t* ev)
{
    if (!ev) {
        return;
    }

    if (handle < 0 || handle >= VP_SENSOR_MAX_HANDLES) {
        return;
    }

    /* One device event, every subscriber. Each instance gets its own copy of
     * the wire identity, taken from *its* descriptor table, so the fields can
     * never disagree with the list that instance was handed. */
    rvvm_lock_slow(&g_dev_lock);
    for (int32_t i = 0; i < VP_SENSOR_MAX_INSTANCES; i++) {
        vp_sensor_t* self = g_instances[i];
        if (!self) {
            continue;
        }

        sensor_lock(self);
        const vp_sensor_info_t* desc = sensor_desc(self, handle);
        if (desc) {
            /* The wire identity is ours, not the backend's: fill it from the
             * same descriptor the guest enumerated, so ev.sensor/ev.type can
             * never disagree with ASensor_getHandle()/ASensor_getType(). */
            vp_sensor_event_t wire = *ev;
            wire.version = VP_SENSOR_EVENT_VERSION;
            wire.sensor = desc->handle;
            wire.type = desc->type;
            wire.flags = desc->wake_up ? VP_SENSOR_FLAG_WAKE_UP : 0;

            for (int32_t q = 0; q < VP_SENSOR_MAX_QUEUES; q++) {
                vp_sensor_queue_t* qu = &self->queues[q];
                if (qu->used && qu->enabled[handle]) {
                    sensor_fifo_push(qu, &wire);
                    sensor_wake(qu);
                }
            }
        }
        sensor_unlock(self);
    }
    rvvm_unlock(&g_dev_lock);
}

/* ============================================================
 * Backend aggregation (device level)
 * ============================================================ */

/* Push the aggregate state of one handle to the backend: the platform source
 * runs while at least one queue *of any instance* has the sensor enabled, at
 * the fastest rate anybody asked for. This is what keeps a backend free of
 * queue - and guest - knowledge. */
static void sensor_sync_handle(int32_t handle)
{
    bool any = false;
    int32_t period = 0;
    int32_t batch = 0;
    const vp_sensor_ops_t* ops;

    if (handle < 0 || handle >= VP_SENSOR_MAX_HANDLES) {
        return;
    }

    rvvm_lock_slow(&g_dev_lock);
    for (int32_t i = 0; i < VP_SENSOR_MAX_INSTANCES; i++) {
        vp_sensor_t* self = g_instances[i];
        if (!self) {
            continue;
        }

        sensor_lock(self);
        for (int32_t q = 0; q < VP_SENSOR_MAX_QUEUES; q++) {
            const vp_sensor_queue_t* qu = &self->queues[q];
            if (!qu->used || !qu->enabled[handle]) {
                continue;
            }
            any = true;
            int32_t r = qu->rate_us[handle];
            if (r > 0 && (period == 0 || r < period)) {
                period = r;
            }
            int32_t b = qu->batch_us[handle];
            if (b > 0 && (batch == 0 || b < batch)) {
                batch = b;
            }
        }
        sensor_unlock(self);
    }
    ops = g_ops;
    rvvm_unlock(&g_dev_lock);

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
 * Instance lifetime
 * ============================================================ */

vp_sensor_t* vp_sensor_create(void)
{
    vp_sensor_t* self = calloc(1, sizeof(*self));
    bool added = false;

    if (!self) {
        return NULL;
    }
    /* rvvm_lock_t is a single flag whose zero is the quiescent state, so the
     * calloc'd lock is already valid. */

    rvvm_lock_slow(&g_dev_lock);
    for (int32_t i = 0; i < VP_SENSOR_MAX_INSTANCES; i++) {
        if (!g_instances[i]) {
            g_instances[i] = self;
            added = true;
            break;
        }
    }
    rvvm_unlock(&g_dev_lock);

    if (!added) {
        /* Out of instance slots: refuse rather than hand back one that would
         * never receive an event (its queues would look alive to the guest). */
        SENSLOG("no instance slot left (max %d)", VP_SENSOR_MAX_INSTANCES);
        free(self);
        return NULL;
    }
    return self;
}

void vp_sensor_destroy(vp_sensor_t* self)
{
    if (!self) {
        return;
    }

    /* Off the device's aggregate first, so the sensors this guest was keeping
     * alive are switched off before its queues disappear. */
    vp_sensor_reset(self);

    rvvm_lock_slow(&g_dev_lock);
    for (int32_t i = 0; i < VP_SENSOR_MAX_INSTANCES; i++) {
        if (g_instances[i] == self) {
            g_instances[i] = NULL;
        }
    }
    rvvm_unlock(&g_dev_lock);

    free(self);
}

/* ============================================================
 * Manager init
 * ============================================================ */

static int64_t sensor_manager_init(vp_sensor_t* self)
{
    int64_t rc;
    const vp_sensor_ops_t* ops = sensor_ops();

    /* The instance lock is held across the backend calls below: enumerate()
     * and query() are documented as "must not call back into this subsystem",
     * so there is no path back through the device lock from here. */
    sensor_lock(self);
    if (!self->ready) {
        self->ready = true;
        self->caps = 0;

        if (ops) {
            if (ops->query) {
                self->caps = ops->query();
            }
            if ((self->caps & VP_SENSOR_CAP_LIST) && ops->enumerate) {
                int32_t n = ops->enumerate(self->desc, VP_SENSOR_MAX_HANDLES);
                self->desc_count = (n > 0 && n <= VP_SENSOR_MAX_HANDLES) ? n : 0;
            }
            for (int32_t i = 0; i < self->desc_count; i++) {
                /* The handle IS the descriptor index: a backend cannot hand out
                 * a handle the guest could not look up in the list it got. */
                self->desc[i].handle = i;
                /* Advertise only what the staging FIFO can actually hold. */
                if (self->desc[i].fifo_max_events <= 0 ||
                    self->desc[i].fifo_max_events > VP_SENSOR_FIFO_MAX_EVENTS) {
                    self->desc[i].fifo_max_events = VP_SENSOR_FIFO_MAX_EVENTS;
                }
            }
            if (self->desc_count == 0) {
                self->caps = 0;
            } else {
                bool wake_up = false;
                for (int32_t i = 0; i < self->desc_count; i++) {
                    if (self->desc[i].wake_up) {
                        wake_up = true;
                        break;
                    }
                }
                if (wake_up) {
                    self->caps |= VP_SENSOR_CAP_WAKEUP;
                } else {
                    self->caps &= ~VP_SENSOR_CAP_WAKEUP;
                }
            }
        }
    }
    rc = (int64_t)self->caps;
    sensor_unlock(self);
    return rc;
}

/* ============================================================
 * Sub-command dispatch
 * ============================================================ */

static int64_t sensor_queue_create(vp_sensor_t* self, int64_t wake_fd)
{
    int64_t rc = VP_SENSOR_ERROR_UNSUPPORTED;

    sensor_lock(self);
    if (sensor_ops()) {
        rc = VP_SENSOR_ERROR_NO_MEMORY;
        for (int32_t i = 0; i < VP_SENSOR_MAX_QUEUES; i++) {
            if (self->queues[i].used) {
                continue;
            }
            memset(&self->queues[i], 0, sizeof(self->queues[i]));
            self->queues[i].used = true;
            if (wake_fd >= 0 && (self->caps & VP_SENSOR_CAP_FD_WAKEUP)) {
                self->queues[i].wake_fd = (int32_t)wake_fd;
            } else {
                self->queues[i].wake_fd = -1;
            }
            rc = i;
            break;
        }
    }
    sensor_unlock(self);
    return rc;
}

static int64_t sensor_queue_destroy(vp_sensor_t* self, int64_t id)
{
    int64_t rc = VP_SENSOR_ERROR_INVALID_ARG;

    sensor_lock(self);
    vp_sensor_queue_t* q = sensor_queue(self, id);
    if (q) {
        memset(q, 0, sizeof(*q));
        rc = VP_SENSOR_OK;
    }
    sensor_unlock(self);

    if (rc == VP_SENSOR_OK) {
        /* A handle can lose its last subscriber here. */
        for (int32_t h = 0; h < self->desc_count; h++) {
            sensor_sync_handle(h);
        }
    }
    return rc;
}

static int64_t sensor_queue_enable(vp_sensor_t* self, int64_t id, int64_t handle, bool enable)
{
    int64_t rc = VP_SENSOR_ERROR_INVALID_ARG;

    sensor_lock(self);
    vp_sensor_queue_t* q = sensor_queue(self, id);
    if (q && sensor_desc(self, (int32_t)handle)) {
        q->enabled[handle] = enable;
        rc = VP_SENSOR_OK;
    }
    sensor_unlock(self);

    if (rc == VP_SENSOR_OK) {
        sensor_sync_handle((int32_t)handle);
    }
    return rc;
}

static int64_t sensor_queue_set_rate(vp_sensor_t* self, int64_t id, int64_t handle,
                                    int64_t period_us, int64_t batch_us)
{
    int64_t rc = VP_SENSOR_ERROR_INVALID_ARG;

    sensor_lock(self);
    vp_sensor_queue_t* q = sensor_queue(self, id);
    if (q && sensor_desc(self, (int32_t)handle)) {
        /* 0 keeps the NDK meaning of "use the backend default". */
        q->rate_us[handle] = (int32_t)(period_us > 0 ? period_us : 0);
        q->batch_us[handle] = (int32_t)(batch_us > 0 ? batch_us : 0);
        rc = VP_SENSOR_OK;
    }
    sensor_unlock(self);

    if (rc == VP_SENSOR_OK) {
        sensor_sync_handle((int32_t)handle);
    }
    return rc;
}

static int64_t sensor_queue_read(vp_sensor_t* self, int64_t id, int64_t guest_events, int64_t want)
{
    vp_sensor_event_t* dst = guest_events ? rvvm_user_guest_ptr((uint64_t)guest_events) : NULL;
    int64_t rc = 0;

    if (!dst || want <= 0) {
        return VP_SENSOR_ERROR_INVALID_ARG;
    }

    sensor_lock(self);
    vp_sensor_queue_t* q = sensor_queue(self, id);
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
    sensor_unlock(self);
    return rc;
}

int64_t vp_sensor_dispatch(vp_sensor_t* self, int64_t sub, int64_t a1, int64_t a2, int64_t a3, int64_t a4)
{
    if (!self) {
        /* No instance (out of slots, or a host that never created one): answer
         * like a device with no sensors, which is what the guest stub expects
         * from a host with no backend. */
        return VP_SENSOR_ERROR_INVALID_ARG;
    }

    switch (sub) {
        case VP_SENSOR_MANAGER_INIT:
            return sensor_manager_init(self);

        case VP_SENSOR_LIST: {
            vp_sensor_info_t* dst = a1 ? rvvm_user_guest_ptr((uint64_t)a1) : NULL;
            int32_t max = (int32_t)a2;
            int64_t rc = 0;

            if (!dst || max <= 0) {
                return 0;
            }
            sensor_lock(self);
            int32_t n = max < self->desc_count ? max : self->desc_count;
            if (n > 0) {
                memcpy(dst, self->desc, sizeof(self->desc[0]) * (size_t)n);
            }
            rc = n;
            sensor_unlock(self);
            return rc;
        }

        case VP_SENSOR_DEFAULT: {
            int32_t type = (int32_t)a1;
            int64_t rc = -1;

            sensor_lock(self);
            for (int32_t i = 0; i < self->desc_count; i++) {
                if (self->desc[i].type == type) {
                    rc = self->desc[i].handle;
                    break;
                }
            }
            sensor_unlock(self);
            return rc;
        }

        case VP_SENSOR_QUEUE_CREATE:
            return sensor_queue_create(self, a1);

        case VP_SENSOR_QUEUE_DESTROY:
            return sensor_queue_destroy(self, a1);

        case VP_SENSOR_QUEUE_ENABLE:
            return sensor_queue_enable(self, a1, a2, true);

        case VP_SENSOR_QUEUE_DISABLE:
            return sensor_queue_enable(self, a1, a2, false);

        case VP_SENSOR_QUEUE_SET_RATE:
            return sensor_queue_set_rate(self, a1, a2, a3, a4);

        case VP_SENSOR_QUEUE_HAS: {
            int64_t rc = VP_SENSOR_ERROR_INVALID_ARG;

            sensor_lock(self);
            vp_sensor_queue_t* q = sensor_queue(self, a1);
            if (q) {
                q->wake_armed = false;
                rc = q->count > 0 ? 1 : 0;
            }
            sensor_unlock(self);
            return rc;
        }

        case VP_SENSOR_QUEUE_READ:
            return sensor_queue_read(self, a1, a2, a3);

        default:
            return VP_SENSOR_ERROR_INVALID_ARG;
    }
}

/* ============================================================
 * Backend registration / instance reset
 * ============================================================ */

void vp_sensor_set_ops(const vp_sensor_ops_t* ops)
{
    rvvm_lock_slow(&g_dev_lock);
    g_ops = ops;
    rvvm_unlock(&g_dev_lock);
}

void vp_sensor_reset(vp_sensor_t* self)
{
    if (!self) {
        return;
    }

    /* Nothing the guest handed us survives: drop the queues first so an
     * in-flight ingest from the platform thread becomes a no-op. Only this
     * instance's queues - another guest's are its own. */
    sensor_lock(self);
    memset(self->queues, 0, sizeof(self->queues));
    sensor_unlock(self);

    /* The platform source is owned by the backend but armed by us: recompute
     * the aggregate for every handle, which turns one off when this guest was
     * its last subscriber. The backend registration itself stays: it belongs to
     * the device, not to the guest that is ending. */
    for (int32_t h = 0; h < VP_SENSOR_MAX_HANDLES; h++) {
        sensor_sync_handle(h);
    }

    sensor_lock(self);
    self->desc_count = 0;
    self->caps = 0;
    self->ready = false;
    sensor_unlock(self);
}
