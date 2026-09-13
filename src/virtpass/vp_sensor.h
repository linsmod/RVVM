/*
 * vp_sensor.h - host-side sensor subsystem.
 *
 * State model
 * -----------
 * The host owns one descriptor table (the device's sensors, pulled from the
 * backend once per manager init) and a small table of event queues. A queue is
 * what the guest's ASensorEventQueue handle refers to. It holds the bounded
 * FIFO that decouples the platform sensor thread (producer) from the guest
 * vCPU thread (consumer), plus the optional wake fd that the guest's Looper
 * polls. Nothing of this is visible over the ABI: vp_sensor_abi.h only defines
 * the copy-out calls.
 *
 * Threading
 * ---------
 * vp_sensor_ingest() is called from the platform sensor thread; every other
 * entry point runs on the guest thread. One lock covers the descriptor table
 * and all queues, and it is never held across a call into the backend.
 *
 * Backend contract
 * ----------------
 * A backend implements vp_sensor_ops_t and calls vp_sensor_ingest() with the
 * payload and the timestamp only: the subsystem fills in version, sensor, type
 * and flags from the descriptor table, so the wire fields can never disagree
 * with what the guest was told during enumeration.
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
     * written. Called once, from VP_SENSOR_MANAGER_INIT, so it must not call
     * back into this subsystem. */
    int32_t (*enumerate)(vp_sensor_info_t* out, int32_t max);

    /* Start/stop the platform source for one sensor. The subsystem reference
     * counts across queues, so the backend sees at most one transition per
     * actual aggregate change. enable is false for handles it never enabled. */
    int32_t (*set_enabled)(int32_t handle, bool enable);

    /* Best-effort sampling period in microseconds; max_batch_us is the largest
     * batching latency the guest is willing to accept (0 = none). */
    int32_t (*set_rate)(int32_t handle, int32_t period_us, int32_t max_batch_us);

    /* VP_SENSOR_CAP_* bitmask. 0 means this host has no sensor backend, and
     * every sensor call then degrades to an empty list. */
    uint32_t (*query)(void);
} vp_sensor_ops_t;

/* Register the host backend. Pass NULL to detach (used on teardown). */
void vp_sensor_set_ops(const vp_sensor_ops_t* ops);

/* Producer entry point, callable from any thread and safe after teardown
 * (events are then dropped). `handle` is the descriptor index; only the
 * timestamp and the union payload of *ev are read. */
void vp_sensor_ingest(int32_t handle, const vp_sensor_event_t* ev);

/* SYS_ANDROID_SENSOR_* sub-command handler, called from cmdpost_dispatch().
 * `sub` is the a0 value, a1..a4 the remaining syscall arguments; guest
 * addresses are translated here with rvvm_user_guest_ptr(). */
int64_t vp_sensor_dispatch(int64_t sub, int64_t a1, int64_t a2, int64_t a3, int64_t a4);

/* Forget every queue, stop the sensors we enabled and detach the backend
 * (called by cmdpost_cleanup()). */
void vp_sensor_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* VP_SENSOR_H */
