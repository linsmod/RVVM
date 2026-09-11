/*
 * win32_aaudio_wasapi.h - WASAPI backend for the AAudio proxy.
 *
 * Mirrors the role of the Android side's real-AAudio glue: it turns the
 * platform-neutral vp_audio_ops_t contract into device I/O. Register it once
 * with cmdpost_set_audio_callbacks(win32_aaudio_ops()) and drop it with
 * win32_aaudio_shutdown() during host teardown.
 */

#ifndef WIN32_AAUDIO_WASAPI_H
#define WIN32_AAUDIO_WASAPI_H

#include "virtpass/vp_cmdpost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ops table. Always non-NULL: when no audio device exists, query() returns
 * 0 and every AAudio call on the guest fails with AAUDIO_ERROR_UNAVAILABLE. */
const vp_audio_ops_t* win32_aaudio_ops(void);

/* Stops every backend thread and releases COM objects. Safe to call twice. */
void win32_aaudio_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* WIN32_AAUDIO_WASAPI_H */
