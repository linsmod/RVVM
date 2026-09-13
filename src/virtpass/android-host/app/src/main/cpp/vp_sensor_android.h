/*
 * vp_sensor_android.h - Android backend for the Virtpass sensor subsystem.
 *
 * Mirrors the role vp_aaudio_android.c plays for audio: it turns the
 * platform-neutral vp_sensor_ops_t contract into real device I/O, using the
 * NDK's ASensorManager / ASensorEventQueue directly (the host IS Android).
 */

#ifndef VP_SENSOR_ANDROID_H
#define VP_SENSOR_ANDROID_H

#include "virtpass/vp_cmdpost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the sensor manager and start the platform sensor thread. Idempotent,
 * so the re-registration pass before every guest run can call it freely. */
void android_sensor_start(void);

/* The ops table. Always non-NULL: on a device without sensors query() still
 * reports the list capability and enumerate() returns 0 sensors. */
const vp_sensor_ops_t* android_sensor_ops(void);

/* Stop the platform sensor thread and destroy its event queue. Safe twice. */
void android_sensor_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* VP_SENSOR_ANDROID_H */
