/*
 * test_sensor_guest.c - sensor proxy conformance test.
 *
 * This is deliberately written as a plain NDK program: enumerate the sensors,
 * post the queue's Looper, enable one at a chosen rate and read events in the
 * callback. It is the regression gate for the sensor path - it fails loudly on
 * the mistakes the old stubs could hide:
 *
 *   - ASensorEventQueue_getEvents() must only report events it actually wrote
 *     into the caller's array (the old stub returned a queue length and left
 *     the array untouched);
 *   - ev.sensor must equal ASensor_getHandle() and ev.type ASensor_getType()
 *     (they used to be the same value, and one host never filled ev.sensor);
 *   - the sensor facts printed below must come from the host, not from a
 *     table compiled into the guest stub.
 *
 * Build: bundled into the APK assets / the win32 host assets by the Makefile
 *   (see project.mk: android_guest_samples).
 * Run:   android: am start -n com.rvvm.android/.MainActivity --es guest test_sensor_guest
 *        win32:   rvvm_winhost.exe --assets <dir>  (pick it in the launcher)
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "virtpass/vp_android.h"

/* How many accelerometer samples to collect, and how long to wait for them
 * before declaring the source dead. A host with no sensor backend is a valid
 * configuration, so the test then reports SKIP instead of failing. */
#define SENSOR_TEST_WANT        10
#define SENSOR_TEST_IDLE_ROUNDS 10     /* x 500 ms poll timeout */
#define SENSOR_TEST_RATE_US     20000  /* 50 Hz */

static ASensorEventQueue* g_queue = NULL;

typedef struct {
    int32_t want_count;
    int32_t expect_handle;
    int32_t got;
    int64_t last_timestamp;
    bool    timestamps_monotonic;
    bool    identity_ok;
    bool    done;
} sensor_test_state_t;

static int on_sensor_events(int fd, int events, void* data)
{
    (void)fd;
    (void)events;
    sensor_test_state_t* state = data;
    ASensorEvent batch[8];

    for (;;) {
        ssize_t count = ASensorEventQueue_getEvents(g_queue, batch, 8);
        if (count <= 0) {
            break;
        }
        for (ssize_t i = 0; i < count; i++) {
            const ASensorEvent* ev = &batch[i];

            if (state->got > 0 && ev->timestamp < state->last_timestamp) {
                state->timestamps_monotonic = false;
            }
            state->last_timestamp = ev->timestamp;

            /* The event must identify the sensor the guest enabled, using the
             * guest-visible handle - never a platform handle. */
            if (ev->type != ASENSOR_TYPE_ACCELEROMETER ||
                ev->sensor != state->expect_handle) {
                state->identity_ok = false;
            }
            if (state->got == 0) {
                printf("  first event: handle=%d type=%d ts=%lld data=(%.3f, %.3f, %.3f)\n",
                       ev->sensor, ev->type, (long long)ev->timestamp,
                       ev->data[0], ev->data[1], ev->data[2]);
            }
            state->got++;
        }
        if (state->got >= state->want_count) {
            break;
        }
    }

    if (state->got >= state->want_count) {
        state->done = true;
        return 0;   /* NDK contract: returning 0 unregisters the fd */
    }
    return 1;
}

static bool print_sensor_facts(const ASensor* sensor)
{
    const char* name = ASensor_getName(sensor);

    printf("  name=%s\n", name && name[0] ? name : "(unnamed)");
    printf("  vendor=%s\n", ASensor_getVendor(sensor));
    printf("  stringType=%s\n", ASensor_getStringType(sensor));
    printf("  handle=%d type=%d reportingMode=%d wakeUp=%d\n",
           ASensor_getHandle(sensor), ASensor_getType(sensor),
           ASensor_getReportingMode(sensor),
           ASensor_isWakeUpSensor(sensor) ? 1 : 0);
    printf("  resolution=%.4f\n", ASensor_getResolution(sensor));
    printf("  minDelay=%dus fifo=(%d/%d) directRate=%d\n",
           ASensor_getMinDelay(sensor),
           ASensor_getFifoMaxEventCount(sensor),
           ASensor_getFifoReservedEventCount(sensor),
           ASensor_getHighestDirectReportRateLevel(sensor));

    return ASensor_getHandle(sensor) >= 0 && name != NULL;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("=== Virtpass sensor conformance test ===\n");

    ASensorManager* manager = ASensorManager_getInstance();
    if (!manager) {
        printf("FAIL: ASensorManager_getInstance() returned NULL\n");
        return 1;
    }

    ASensorList list = NULL;
    int sensor_count = ASensorManager_getSensorList(manager, &list);
    printf("sensor count: %d\n", sensor_count);
    for (int i = 0; i < sensor_count; i++) {
        printf("sensor %d:\n", i);
        if (!print_sensor_facts(list[i])) {
            printf("FAIL: ASensor facts incomplete\n");
            return 1;
        }
    }

    ASensor const* accel = ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_ACCELEROMETER);
    if (!accel) {
        printf("SKIP: no accelerometer on this host (an empty sensor list is valid)\n");
        return 0;
    }

    int accel_handle = ASensor_getHandle(accel);
    printf("using accelerometer: handle=%d\n", accel_handle);

    /* Direct Channel is not part of the Virtpass subset: the guest must be
     * told so, not handed a channel that never produces events. */
    if (ASensor_isDirectChannelTypeSupported(accel, 0) ||
        ASensor_getHighestDirectReportRateLevel(accel) != ASENSOR_DIRECT_RATE_STOP) {
        printf("FAIL: Direct Channel reported as available\n");
        return 1;
    }

    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    sensor_test_state_t state;
    memset(&state, 0, sizeof(state));
    state.want_count = SENSOR_TEST_WANT;
    state.expect_handle = accel_handle;
    state.identity_ok = true;
    state.timestamps_monotonic = true;

    /* The callback form is what the NDK documents for a Looper-driven queue. */
    ASensorEventQueue* queue = ASensorManager_createEventQueue(
            manager, looper, ALOOPER_POLL_CALLBACK, on_sensor_events, &state);
    if (!queue) {
        printf("FAIL: ASensorManager_createEventQueue() returned NULL\n");
        return 1;
    }
    g_queue = queue;

    if (ASensorEventQueue_setEventRate(queue, accel, SENSOR_TEST_RATE_US) != 0) {
        printf("FAIL: setEventRate() failed\n");
        ASensorManager_destroyEventQueue(manager, queue);
        return 1;
    }
    if (ASensorEventQueue_enableSensor(queue, accel) != 0) {
        printf("FAIL: enableSensor() failed\n");
        ASensorManager_destroyEventQueue(manager, queue);
        return 1;
    }

    /* Drain the Looper until the samples arrive or the source looks dead. */
    int idle_rounds = 0;
    while (!state.done && idle_rounds < SENSOR_TEST_IDLE_ROUNDS) {
        int result = ALooper_pollAll(500, NULL, NULL, NULL);
        if (result == ALOOPER_POLL_TIMEOUT) {
            idle_rounds++;
        }
    }

    printf("events received: %d (wanted %d)\n", state.got, SENSOR_TEST_WANT);

    /* Once the callback drained the queue there must be nothing pending. */
    int pending = ASensorEventQueue_hasEvents(queue);
    printf("hasEvents after drain: %d\n", pending);

    int disable_rc = ASensorEventQueue_disableSensor(queue, accel);
    ASensorManager_destroyEventQueue(manager, queue);
    g_queue = NULL;

    if (state.got < SENSOR_TEST_WANT) {
        printf("FAIL: only %d of %d events arrived\n", state.got, SENSOR_TEST_WANT);
        return 1;
    }
    if (!state.identity_ok) {
        printf("FAIL: an event did not carry (handle=%d, type=%d)\n",
               accel_handle, ASENSOR_TYPE_ACCELEROMETER);
        return 1;
    }
    if (!state.timestamps_monotonic) {
        printf("FAIL: event timestamps are not monotonic\n");
        return 1;
    }
    if (pending != 0) {
        printf("FAIL: queue still reports pending events after a full drain\n");
        return 1;
    }
    if (disable_rc != 0) {
        printf("FAIL: disableSensor() failed\n");
        return 1;
    }

    printf("PASS: %d accelerometer events, handle/type/timestamps consistent\n", state.got);
    return 0;
}
