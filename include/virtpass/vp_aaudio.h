/*
 * vp_aaudio.h - AAudio API surface for the VirtPass guest
 *
 * Mirrors the parts of Android's <aaudio/AAudio.h> that the proxy implements.
 * Only the ABI matters: every enum-like parameter/return travels as a 32-bit
 * integer, so a program compiled against the real NDK header (Oboe, SDL3, ...)
 * can link straight against these stubs.
 *
 * Implementation: src/virtpass/vp_aaudio_stub.c
 * Transport:      include/virtpass/vp_audio_ringbuf.h
 *
 * If a real <aaudio/AAudio.h> has already been included, we stay out of the
 * way and let the guest use the NDK declarations instead.
 */

#ifndef VP_AAUDIO_H
#define VP_AAUDIO_H

#if defined(ANDROID_AAUDIO_H) || defined(AAUDIO_H_)
/* A real NDK AAudio header is in scope; nothing for us to declare. */
#else

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "virtpass/vp_audio_ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Opaque handles
 * ============================================================ */
typedef struct AAudioStreamBuilder AAudioStreamBuilder;
typedef struct AAudioStream AAudioStream;

typedef int32_t aaudio_result_t;

/* ============================================================
 * Enumerations (values match the NDK ABI)
 * ============================================================ */
typedef int32_t aaudio_direction_t;
#define AAUDIO_UNSPECIFIED              0
#define AAUDIO_DIRECTION_OUTPUT         0
#define AAUDIO_DIRECTION_INPUT          1

typedef int32_t aaudio_sharing_mode_t;
#define AAUDIO_SHARING_MODE_EXCLUSIVE   1
#define AAUDIO_SHARING_MODE_SHARED      2

typedef int32_t aaudio_performance_mode_t;
#define AAUDIO_PERFORMANCE_MODE_NONE        10
#define AAUDIO_PERFORMANCE_MODE_POWER_SAVING 11
#define AAUDIO_PERFORMANCE_MODE_LOW_LATENCY 12

typedef int32_t aaudio_format_t;
#define AAUDIO_FORMAT_INVALID           (-1)
#define AAUDIO_FORMAT_UNSPECIFIED       0
#define AAUDIO_FORMAT_PCM_I16           1
#define AAUDIO_FORMAT_PCM_FLOAT         2
#define AAUDIO_FORMAT_PCM_I24_PACKED    3
#define AAUDIO_FORMAT_PCM_I32           4

typedef int32_t aaudio_usage_t;
#define AAUDIO_USAGE_MEDIA                        1
#define AAUDIO_USAGE_VOICE_COMMUNICATION          2
#define AAUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING 3
#define AAUDIO_USAGE_ALARM                        4
#define AAUDIO_USAGE_NOTIFICATION                 5
#define AAUDIO_USAGE_NOTIFICATION_RINGTONE        6
#define AAUDIO_USAGE_NOTIFICATION_EVENT           10
#define AAUDIO_USAGE_ASSISTANCE_ACCESSIBILITY     11
#define AAUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE 12
#define AAUDIO_USAGE_ASSISTANCE_SONIFICATION      13
#define AAUDIO_USAGE_GAME                         14

typedef int32_t aaudio_content_type_t;
#define AAUDIO_CONTENT_TYPE_SPEECH      1
#define AAUDIO_CONTENT_TYPE_MUSIC       2
#define AAUDIO_CONTENT_TYPE_MOVIE       3
#define AAUDIO_CONTENT_TYPE_SONIFICATION 4

typedef int32_t aaudio_input_preset_t;
#define AAUDIO_INPUT_PRESET_GENERIC             1
#define AAUDIO_INPUT_PRESET_CAMCORDER           5
#define AAUDIO_INPUT_PRESET_VOICE_RECOGNITION   6
#define AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION 7
#define AAUDIO_INPUT_PRESET_UNPROCESSED         9
#define AAUDIO_INPUT_PRESET_VOICE_PERFORMANCE   10

typedef int32_t aaudio_session_id_t;
#define AAUDIO_SESSION_ID_NONE          (-1)
#define AAUDIO_SESSION_ID_ALLOCATE      0

typedef int32_t aaudio_channel_mask_t;
#define AAUDIO_CHANNEL_MONO             (1u << 0)
#define AAUDIO_CHANNEL_STEREO           (1u << 1)

typedef int32_t aaudio_stream_state_t;
#define AAUDIO_STREAM_STATE_UNINITIALIZED 0
#define AAUDIO_STREAM_STATE_UNKNOWN       1
#define AAUDIO_STREAM_STATE_OPENING       2
#define AAUDIO_STREAM_STATE_OPEN          3
#define AAUDIO_STREAM_STATE_STARTING      4
#define AAUDIO_STREAM_STATE_STARTED       5
#define AAUDIO_STREAM_STATE_PAUSING       6
#define AAUDIO_STREAM_STATE_PAUSED        7
#define AAUDIO_STREAM_STATE_FLUSHING      8
#define AAUDIO_STREAM_STATE_FLUSHED       9
#define AAUDIO_STREAM_STATE_STOPPING      10
#define AAUDIO_STREAM_STATE_STOPPED       11
#define AAUDIO_STREAM_STATE_CLOSING       12
#define AAUDIO_STREAM_STATE_CLOSED        13
#define AAUDIO_STREAM_STATE_DISCONNECTED  14

typedef int32_t aaudio_data_callback_result_t;
#define AAUDIO_CALLBACK_RESULT_CONTINUE 0
#define AAUDIO_CALLBACK_RESULT_STOP     1

/* ============================================================
 * Result codes (values match the NDK ABI)
 * ============================================================ */
#define AAUDIO_OK                       0
#define AAUDIO_ERROR_BASE               (-900)
#define AAUDIO_ERROR_DISCONNECTED       (AAUDIO_ERROR_BASE + 1)
#define AAUDIO_ERROR_ILLEGAL_ARGUMENT   (AAUDIO_ERROR_BASE + 2)
#define AAUDIO_ERROR_INTERNAL           (AAUDIO_ERROR_BASE + 4)
#define AAUDIO_ERROR_INVALID_STATE      (AAUDIO_ERROR_BASE + 5)
#define AAUDIO_ERROR_INVALID_HANDLE     (AAUDIO_ERROR_BASE + 8)
#define AAUDIO_ERROR_UNIMPLEMENTED      (AAUDIO_ERROR_BASE + 10)
#define AAUDIO_ERROR_UNAVAILABLE        (AAUDIO_ERROR_BASE + 11)
#define AAUDIO_ERROR_NO_FREE_HANDLES    (AAUDIO_ERROR_BASE + 12)
#define AAUDIO_ERROR_NO_MEMORY          (AAUDIO_ERROR_BASE + 13)
#define AAUDIO_ERROR_NULL               (AAUDIO_ERROR_BASE + 14)
#define AAUDIO_ERROR_TIMEOUT            (AAUDIO_ERROR_BASE + 15)
#define AAUDIO_ERROR_WOULD_BLOCK        (AAUDIO_ERROR_BASE + 16)
#define AAUDIO_ERROR_INVALID_FORMAT     (AAUDIO_ERROR_BASE + 17)
#define AAUDIO_ERROR_OUT_OF_RANGE       (AAUDIO_ERROR_BASE + 18)
#define AAUDIO_ERROR_NO_SERVICE         (AAUDIO_ERROR_BASE + 19)
#define AAUDIO_ERROR_INVALID_RATE       (AAUDIO_ERROR_BASE + 20)

#define AAUDIO_DEVICE_ID_UNSPECIFIED    0
#define AAUDIO_TIME_UNSPECIFIED         INT64_MIN

/* ============================================================
 * Callbacks
 * ============================================================ */
/* Called by the stream's own worker thread to ask for / hand over PCM.
 * `audioData` points at `numFrames` frames of the stream's format. */
typedef void (*AAudioStream_dataCallback)(AAudioStream* stream,
                                          void* userData,
                                          void* audioData,
                                          int32_t numFrames);

typedef void (*AAudioStream_errorCallback)(AAudioStream* stream,
                                           void* userData,
                                           aaudio_result_t error);

/* ============================================================
 * StreamBuilder
 * ============================================================ */
AAudioStreamBuilder* AAudio_createStreamBuilder(void);
void AAudioStreamBuilder_delete(AAudioStreamBuilder* builder);

void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder* builder, int32_t deviceId);
void AAudioStreamBuilder_setPackageName(AAudioStreamBuilder* builder, const char* packageName);
void AAudioStreamBuilder_setAttributionTag(AAudioStreamBuilder* builder, const char* attributionTag);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* builder, int32_t sampleRate);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* builder, int32_t channelCount);
void AAudioStreamBuilder_setSamplesPerFrame(AAudioStreamBuilder* builder, int32_t samplesPerFrame);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder* builder, aaudio_format_t format);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder* builder, aaudio_sharing_mode_t sharingMode);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder* builder, aaudio_direction_t direction);
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder* builder, int32_t numFrames);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* builder, aaudio_performance_mode_t mode);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder* builder, aaudio_usage_t usage);
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder* builder, aaudio_content_type_t contentType);
void AAudioStreamBuilder_setInputPreset(AAudioStreamBuilder* builder, aaudio_input_preset_t inputPreset);
void AAudioStreamBuilder_setSessionId(AAudioStreamBuilder* builder, aaudio_session_id_t sessionId);
void AAudioStreamBuilder_setChannelMask(AAudioStreamBuilder* builder, aaudio_channel_mask_t channelMask);
void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder* builder, int32_t numFrames);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* builder,
                                         AAudioStream_dataCallback callback,
                                         void* userData);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* builder,
                                          AAudioStream_errorCallback callback,
                                          void* userData);

/* Opens the stream and drives it up to the builder's requested state. */
AAudioStream* AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder,
                                             aaudio_result_t* pError);

/* ============================================================
 * Stream lifecycle
 * ============================================================ */
aaudio_result_t AAudioStream_close(AAudioStream* stream);
aaudio_result_t AAudioStream_requestStart(AAudioStream* stream);
aaudio_result_t AAudioStream_requestPause(AAudioStream* stream);
aaudio_result_t AAudioStream_requestFlush(AAudioStream* stream);
aaudio_result_t AAudioStream_requestStop(AAudioStream* stream);
aaudio_stream_state_t AAudioStream_getState(AAudioStream* stream);
aaudio_result_t AAudioStream_waitForStateChange(AAudioStream* stream,
                                                aaudio_stream_state_t inputState,
                                                aaudio_stream_state_t* nextState,
                                                int64_t timeoutNanoseconds);

/* ============================================================
 * Blocking I/O
 * ============================================================ */
aaudio_result_t AAudioStream_read(AAudioStream* stream, void* buffer,
                                  int32_t numFrames, int64_t timeoutNanoseconds);
aaudio_result_t AAudioStream_write(AAudioStream* stream, const void* buffer,
                                   int32_t numFrames, int64_t timeoutNanoseconds);

/* ============================================================
 * Introspection
 * ============================================================ */
int64_t AAudioStream_getFramesWritten(AAudioStream* stream);
int64_t AAudioStream_getFramesRead(AAudioStream* stream);
int32_t AAudioStream_getSampleRate(AAudioStream* stream);
int32_t AAudioStream_getChannelCount(AAudioStream* stream);
aaudio_format_t AAudioStream_getFormat(AAudioStream* stream);
aaudio_sharing_mode_t AAudioStream_getSharingMode(AAudioStream* stream);
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream* stream);
aaudio_direction_t AAudioStream_getDirection(AAudioStream* stream);
aaudio_usage_t AAudioStream_getUsage(AAudioStream* stream);
aaudio_content_type_t AAudioStream_getContentType(AAudioStream* stream);
aaudio_input_preset_t AAudioStream_getInputPreset(AAudioStream* stream);
int32_t AAudioStream_getDeviceId(AAudioStream* stream);
aaudio_session_id_t AAudioStream_getSessionId(AAudioStream* stream);
int32_t AAudioStream_getFramesPerBurst(AAudioStream* stream);
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream* stream);
int32_t AAudioStream_setBufferSizeInFrames(AAudioStream* stream, int32_t numFrames);
int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream* stream);
int32_t AAudioStream_getXRunCount(AAudioStream* stream);
int32_t AAudioStream_getFramesPerDataCallback(AAudioStream* stream);
aaudio_result_t AAudioStream_getTimestamp(AAudioStream* stream, clockid_t clockid,
                                          int64_t* framePosition,
                                          int64_t* timeNanoseconds);

/* ============================================================
 * Diagnostics
 * ============================================================ */
const char* AAudio_convertResultToText(aaudio_result_t returnCode);
const char* AAudio_convertStreamStateToText(aaudio_stream_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* !ANDROID_AAUDIO_H */

#endif /* VP_AAUDIO_H */
