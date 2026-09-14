/*
 * vp_sensor.h - host-side sensor subsystem.
 *
 * State model
 * -----------
 * Two levels, because a sensor is a device and a queue is a subscriber:
 *
 *   device (process-wide, one)
 *     The backend table registered with vp_sensor_set_ops(), the aggregate
 *     state pushed to it (a sensor runs while *any* guest queue wants it, at
 *     the fastest rate anybody asked for), and the list of live instances.
 *     vp_sensor_ingest() belongs here: one platform source, one event, fanned
 *     out to every subscriber.
 *
 *   instance (one per guest)
 *     A vp_sensor_t holds the descriptor table it enumerated (its own copy of
 *     a device list), its event queues - each one the guest's ASensorEventQueue
 *     handle - and the bounded FIFO that decouples the platform sensor thread
 *     (producer) from the guest vCPU thread (consumer), plus the optional wake
 *     fd the guest's Looper polls. Nothing of this is visible over the ABI:
 *     vp_sensor_abi.h only defines the copy-out calls.
 *
 *     Queue handles are indices into the *instance's* table, so one guest
 *     cannot name (let alone read or destroy) another one's queue, and a
 *     teardown only clears the queues that belong to the guest it is ending.
 *
 * Threading
 * ---------
 * vp_sensor_ingest() is called from the platform sensor thread; every other
 * entry point runs on the guest thread. Each instance has one lock covering its
 * descriptor table and all of its queues; the device has one covering the
 * backend table and the instance list.
 *
 * Lock order is device -> instance, never the reverse. Ingest and the aggregate
 * recomputation hold the device lock while taking each instance's lock in turn;
 * no guest-thread path holds an instance lock while reaching for the device
 * lock. Neither lock is ever held across a call into the backend.
 *
 * Backend contract
 * ----------------
 * A backend implements vp_sensor_ops_t and calls vp_sensor_ingest() with the
 * payload and the timestamp only: the subsystem fills in version, sensor, type
 * and flags from the instance's descriptor table, so the wire fields can never
 * disagree with what the guest was told during enumeration.
 */

#ifndef VP_SENSOR_H
#define VP_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

#include "virtpass/vp_sensor_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vp_sensor_ops {
    /* Describe the device's sensors into out[0..max); returns the count
     * written. Called once per instance, from VP_SENSOR_MANAGER_INIT, so it
     * must not call back into this subsystem. */
    int32_t (*enumerate)(vp_sensor_info_t* out, int32_t max);

    /* Start/stop the platform source for one sensor. The subsystem reference
     * counts across queues *and instances*, so the backend sees at most one
     * transition per actual aggregate change. enable is false for handles it
     * never enabled. */
    int32_t (*set_enabled)(int32_t handle, bool enable);

    /* Best-effort sampling period in microseconds; max_batch_us is the largest
     * batching latency the guest is willing to accept (0 = none). */
    int32_t (*set_rate)(int32_t handle, int32_t period_us, int32_t max_batch_us);

    /* VP_SENSOR_CAP_* bitmask. 0 means this host has no sensor backend, and
     * every sensor call then degrades to an empty list. */
    uint32_t (*query)(void);
} vp_sensor_ops_t;

/* ---- Device level ---- */

/* Register the host backend. Pass NULL to detach (used at host teardown).
 * Process-wide: the backend is the device, shared by every instance. */
void vp_sensor_set_ops(const vp_sensor_ops_t* ops);

/* Producer entry point, callable from any thread and safe after teardown
 * (events are then dropped). `handle` is a device descriptor index; only the
 * timestamp and the union payload of *ev are read. The event is fanned out to
 * every instance that has queues with that handle enabled. */
void vp_sensor_ingest(int32_t handle, const vp_sensor_event_t* ev);

/* ---- Instance level ---- */

typedef struct vp_sensor vp_sensor_t;

/* Create/destroy one guest's subscriber state and add/remove it from the
 * device's fan-out list. vp_sensor_create() returns NULL when the instance
 * table is full or out of memory: callers treat that as "this guest has no
 * sensors", which is the same thing a host with no backend sees.
 * vp_sensor_destroy() runs vp_sensor_reset() first, so the device's aggregate
 * is recomputed without this instance before it goes away. */
vp_sensor_t* vp_sensor_create(void);
void vp_sensor_destroy(vp_sensor_t* self);

/* SYS_ANDROID_SENSOR_* sub-command handler, called from cmdpost_dispatch().
 * `sub` is the a0 value, a1..a4 the remaining syscall arguments; guest
 * addresses are translated here with rvvm_user_guest_ptr(). NULL is accepted
 * and answers like an instance with no device behind it. */
int64_t vp_sensor_dispatch(vp_sensor_t* self, int64_t sub, int64_t a1, int64_t a2, int64_t a3, int64_t a4);

/* Forget this guest's queues, stop the sensors it was keeping alive and drop
 * its descriptor table. Called at the end of a guest run (cmdpost_end_run())
 * and when the instance is destroyed. The backend registration is *not*
 * touched: that is the device's, and the aggregate recomputation turns a
 * sensor off when this was its last subscriber. */
void vp_sensor_reset(vp_sensor_t* self);

#ifdef __cplusplus
}
#endif

#endif /* VP_SENSOR_H */
