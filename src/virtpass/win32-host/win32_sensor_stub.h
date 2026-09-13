/*
 * win32_sensor_stub.h - virtual sensors for the Win32 virtpass host.
 *
 * Win32 has no sensor stack, so this exposes a few synthetic sensors through
 * the same vp_sensor_ops_t contract the Android host implements with real
 * hardware. Events are produced by win32_sensor_stub_tick(), which the host
 * calls from its WM_TIMER (UI thread), i.e. on a different thread than the
 * guest vCPU - exactly the decoupling the subsystem's staging FIFO exists for.
 */

#ifndef WIN32_SENSOR_STUB_H
#define WIN32_SENSOR_STUB_H

#include "virtpass/vp_cmdpost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ops table. Never NULL: query() reports the capabilities the stub can
 * actually honour. */
const vp_sensor_ops_t* win32_sensor_stub_ops(void);

/* Advance the virtual sensors by one tick, emitting a sample for every enabled
 * sensor whose requested period has elapsed. */
void win32_sensor_stub_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* WIN32_SENSOR_STUB_H */
