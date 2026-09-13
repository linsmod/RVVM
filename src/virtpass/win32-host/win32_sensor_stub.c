/*
 * win32_sensor_stub.c - virtual sensors for the Win32 virtpass host.
 *
 * Three synthetic sensors (accelerometer, gyroscope, light) driven by the
 * host's WM_TIMER. The device facts are declared here, the payloads are
 * generated here, and everything else - wire identity, per-queue fan-out, the
 * Looper wake edge - belongs to vp_sensor.c.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <string.h>

#include "win32_sensor_stub.h"
#include "virtpass/vp_android.h"  /* ASENSOR_TYPE_*, WINDOW_FORMAT_* constants */

typedef struct {
    int32_t     type;
    const char* name;
    const char* string_type;
    float       resolution;
    int32_t     min_delay_us;
} win32_sensor_def_t;

/* Handle == index in this table; the subsystem re-stamps the handle anyway, so
 * the order here is what the guest will enumerate. */
static const win32_sensor_def_t g_sensor_defs[] = {
    { ASENSOR_TYPE_ACCELEROMETER, "RVVM Virtual Accelerometer",
      "android.sensor.accelerometer", 0.01f, 10000 },
    { ASENSOR_TYPE_GYROSCOPE, "RVVM Virtual Gyroscope",
      "android.sensor.gyroscope", 0.001f, 10000 },
    { ASENSOR_TYPE_LIGHT, "RVVM Virtual Light",
      "android.sensor.light", 1.0f, 0 },
};

#define WIN32_SENSOR_COUNT (int32_t)(sizeof(g_sensor_defs) / sizeof(g_sensor_defs[0]))

/* Until the guest calls setEventRate(), samples come at 10 Hz - the cadence the
 * old fixed timer used. */
#define WIN32_SENSOR_DEFAULT_PERIOD_US 100000
#define WIN32_SENSOR_MIN_PERIOD_MS     1

static bool     g_enabled[WIN32_SENSOR_COUNT];
static int32_t  g_period_us[WIN32_SENSOR_COUNT];
static uint64_t g_due_ms[WIN32_SENSOR_COUNT];

static void win32_sensor_emit(int32_t index)
{
    vp_sensor_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.timestamp = (int64_t)GetTickCount64() * 1000000LL;   /* ms -> ns */

    switch (g_sensor_defs[index].type) {
    case ASENSOR_TYPE_ACCELEROMETER:
        /* Resting device: gravity along +Z. */
        ev.acceleration.x = 0.0f;
        ev.acceleration.y = 0.0f;
        ev.acceleration.z = 9.81f;
        break;
    case ASENSOR_TYPE_GYROSCOPE:
        ev.vector.x = 0.0f;
        ev.vector.y = 0.0f;
        ev.vector.z = 0.0f;
        break;
    case ASENSOR_TYPE_LIGHT:
        ev.light = 0.0f;
        break;
    default:
        break;
    }

    /* version/sensor/type/flags are the subsystem's to fill. */
    vp_sensor_ingest(index, &ev);
}

void win32_sensor_stub_tick(void)
{
    uint64_t now = GetTickCount64();

    for (int32_t i = 0; i < WIN32_SENSOR_COUNT; i++) {
        if (!g_enabled[i] || now < g_due_ms[i]) {
            continue;
        }
        int32_t period_us = g_period_us[i] > 0 ? g_period_us[i]
                                              : WIN32_SENSOR_DEFAULT_PERIOD_US;
        int32_t period_ms = period_us / 1000;
        if (period_ms < WIN32_SENSOR_MIN_PERIOD_MS) {
            period_ms = WIN32_SENSOR_MIN_PERIOD_MS;
        }
        g_due_ms[i] = now + (uint64_t)period_ms;
        win32_sensor_emit(i);
    }
}

/* ============================================================
 * vp_sensor_ops_t
 * ============================================================ */

static int32_t win32_sensor_enumerate(vp_sensor_info_t* out, int32_t max)
{
    int32_t count = 0;

    if (!out || max <= 0) {
        return 0;
    }
    for (int32_t i = 0; i < WIN32_SENSOR_COUNT && count < max; i++) {
        const win32_sensor_def_t* def = &g_sensor_defs[i];
        vp_sensor_info_t* info = &out[count];

        memset(info, 0, sizeof(*info));
        info->handle = count;
        info->type = def->type;
        info->reporting_mode = AREPORTING_MODE_CONTINUOUS;
        info->min_delay_us = def->min_delay_us;
        info->wake_up = 0;
        info->highest_direct_rate_level = ASENSOR_DIRECT_RATE_STOP;
        info->resolution = def->resolution;
        /* fifo_max_events stays 0: the subsystem fills in what its staging FIFO
         * can actually hold, so the two can never disagree. */
        strncpy(info->string_type, def->string_type, sizeof(info->string_type) - 1);
        strncpy(info->name, def->name, sizeof(info->name) - 1);
        strncpy(info->vendor, "RVVM", sizeof(info->vendor) - 1);
        count++;
    }
    return count;
}

static int32_t win32_sensor_set_enabled(int32_t handle, bool enable)
{
    if (handle < 0 || handle >= WIN32_SENSOR_COUNT) {
        return VP_SENSOR_ERROR_INVALID_ARG;
    }
    g_enabled[handle] = enable;
    /* Emit the first sample on the next tick instead of a period later. */
    if (enable) {
        g_due_ms[handle] = GetTickCount64();
    }
    return VP_SENSOR_OK;
}

static int32_t win32_sensor_set_rate(int32_t handle, int32_t period_us, int32_t max_batch_us)
{
    (void)max_batch_us;  /* the stub delivers every sample it produces */

    if (handle < 0 || handle >= WIN32_SENSOR_COUNT) {
        return VP_SENSOR_ERROR_INVALID_ARG;
    }
    g_period_us[handle] = period_us;
    return VP_SENSOR_OK;
}

static uint32_t win32_sensor_query(void)
{
    return VP_SENSOR_CAP_LIST | VP_SENSOR_CAP_RATE | VP_SENSOR_CAP_FD_WAKEUP;
}

static const vp_sensor_ops_t g_win32_sensor_ops = {
    .enumerate = win32_sensor_enumerate,
    .set_enabled = win32_sensor_set_enabled,
    .set_rate = win32_sensor_set_rate,
    .query = win32_sensor_query,
};

const vp_sensor_ops_t* win32_sensor_stub_ops(void)
{
    return &g_win32_sensor_ops;
}
