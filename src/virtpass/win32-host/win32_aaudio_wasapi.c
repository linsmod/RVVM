/*
 * win32_aaudio_wasapi.c - Windows WASAPI backend for the AAudio guest proxy
 *
 * Implements vp_audio_ops_t (see vp_cmdpost.h) on top of Windows WASAPI.
 * Thin passthrough: each WRITE/READ hypercall does a memcpy between guest
 * memory and a local buffer, then calls the real WASAPI render/capture.
 * No pump threads, no shared ring buffer.
 */

#include "rvvm.h"
#include "riscv.h"
#include "rvvm_types.h"
#include "virtpass/vp_audio_ringbuf.h"
#include "virtpass/vp_cmdpost.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

/* COM vtable pointers resolved once at load time. */
static struct {
    HMODULE ole32;
    HMODULE avrt;
    HRESULT (WINAPI *CoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    HRESULT (WINAPI *CoInitializeEx)(LPVOID, DWORD);
    HANDLE  (WINAPI *AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
    BOOL    (WINAPI *AvRevertMmThreadCharacteristics)(HANDLE);
} g_com;

static void com_load(void)
{
    if (g_com.ole32) return;
    g_com.ole32 = LoadLibraryW(L"ole32.dll");
    if (g_com.ole32) {
        g_com.CoCreateInstance = (void*)GetProcAddress(g_com.ole32, "CoCreateInstance");
        g_com.CoInitializeEx  = (void*)GetProcAddress(g_com.ole32, "CoInitializeEx");
    }
    g_com.avrt = LoadLibraryW(L"avrt.dll");
    if (g_com.avrt) {
        g_com.AvSetMmThreadCharacteristicsW = (void*)GetProcAddress(g_com.avrt, "AvSetMmThreadCharacteristicsW");
        g_com.AvRevertMmThreadCharacteristics = (void*)GetProcAddress(g_com.avrt, "AvRevertMmThreadCharacteristics");
    }
}

static void com_unload(void)
{
    if (g_com.avrt)  { FreeLibrary(g_com.avrt);  g_com.avrt = NULL; }
    if (g_com.ole32) { FreeLibrary(g_com.ole32); g_com.ole32 = NULL; }
}

/* ============================================================
 * Logging
 * ============================================================ */
#define wasapi_log(...) fprintf(stderr, "[WASAPI] " __VA_ARGS__)

/* ============================================================
 * COM vtable wrappers (avoids #including x64 headers)
 * ============================================================ */
typedef struct IMMDeviceEnumeratorVtbl {
    HRESULT (WINAPI *QueryInterface)(void*,REFIID,void**);
    ULONG   (WINAPI *AddRef)(void*);
    ULONG   (WINAPI *Release)(void*);
    HRESULT (WINAPI *EnumAudioEndpoints)(void*,EDataFlow,DWORD,void**);
    HRESULT (WINAPI *GetDefaultAudioEndpoint)(void*,EDataFlow,ERole,void**);
} IMMDeviceEnumeratorVtbl;

typedef struct IMMDeviceVtbl {
    HRESULT (WINAPI *QueryInterface)(void*,REFIID,void**);
    ULONG   (WINAPI *AddRef)(void*);
    ULONG   (WINAPI *Release)(void*);
    HRESULT (WINAPI *Activate)(void*,REFIID,DWORD,PROPVARIANT,void**);
    HRESULT (WINAPI *OpenPropertyStore)(void*,DWORD,void**);
    HRESULT (WINAPI *GetId)(void*,LPWSTR*);
    HRESULT (WINAPI *GetState)(void*,DWORD*);
} IMMDeviceVtbl;

typedef struct IAudioClientVtbl {
    HRESULT (WINAPI *QueryInterface)(void*,REFIID,void**);
    ULONG   (WINAPI *AddRef)(void*);
    ULONG   (WINAPI *Release)(void*);
    HRESULT (WINAPI *Initialize)(void*,AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX*,void*);
    HRESULT (WINAPI *GetBufferFormat)(void*,WAVEFORMATEX**);
    HRESULT (WINAPI *GetMixFormat)(void*,WAVEFORMATEX**);
    HRESULT (WINAPI *GetDevicePeriod)(void*,REFERENCE_TIME*,REFERENCE_TIME*);
    HRESULT (WINAPI *Start)(void*);
    HRESULT (WINAPI *Stop)(void*);
    HRESULT (WINAPI *Reset)(void*);
    HRESULT (WINAPI *SetEventHandle)(void*,HANDLE);
    HRESULT (WINAPI *GetService)(void*,REFIID,void**);
} IAudioClientVtbl;

typedef struct IAudioRenderClientVtbl {
    HRESULT (WINAPI *QueryInterface)(void*,REFIID,void**);
    ULONG   (WINAPI *AddRef)(void*);
    ULONG   (WINAPI *Release)(void*);
    HRESULT (WINAPI *GetBuffer)(void*,UINT32,BYTE**);
    HRESULT (WINAPI *ReleaseBuffer)(void*,UINT32,DWORD);
    HRESULT (WINAPI *GetPadding)(void*,UINT32*);
    HRESULT (WINAPI *GetCurrentPadding)(void*,UINT32*);
    HRESULT (WINAPI *IsFormatSupported)(void*,AUDCLNT_SHAREMODE,WAVEFORMATEX*,WAVEFORMATEX**);
    HRESULT (WINAPI *GetMixFormat)(void*,WAVEFORMATEX**);
    HRESULT (WINAPI *GetDevicePeriod)(void*,REFERENCE_TIME*,REFERENCE_TIME*);
    HRESULT (WINAPI *Start)(void*);
    HRESULT (WINAPI *Stop)(void*);
    HRESULT (WINAPI *Reset)(void*);
} IAudioRenderClientVtbl;

typedef struct IAudioCaptureClientVtbl {
    HRESULT (WINAPI *QueryInterface)(void*,REFIID,void**);
    ULONG   (WINAPI *AddRef)(void*);
    ULONG   (WINAPI *Release)(void*);
    HRESULT (WINAPI *GetBuffer)(void*,BYTE**,UINT32*,DWORD*,UINT64*,UINT64*);
    HRESULT (WINAPI *ReleaseBuffer)(void*,UINT32);
    HRESULT (WINAPI *GetNextPacketSize)(void*,UINT32*);
} IAudioCaptureClientVtbl;

typedef struct IAudioClockVtbl {
    HRESULT (WINAPI *QueryInterface)(void*,REFIID,void**);
    ULONG   (WINAPI *AddRef)(void*);
    ULONG   (WINAPI *Release)(void*);
    HRESULT (WINAPI *GetFrequency)(void*,UINT64*);
    HRESULT (WINAPI *GetPosition)(void*,UINT64*,UINT64*);
    HRESULT (WINAPI *GetCharacteristics)(void*,DWORD*);
} IAudioClockVtbl;

#define IMMDeviceEnumerator_QueryInterface(e,i,p)      ((IMMDeviceEnumeratorVtbl*)(e))->QueryInterface(e,i,p)
#define IMMDeviceEnumerator_AddRef(e)                  ((IMMDeviceEnumeratorVtbl*)(e))->AddRef(e)
#define IMMDeviceEnumerator_Release(e)                 ((IMMDeviceEnumeratorVtbl*)(e))->Release(e)
#define IMMDeviceEnumerator_GetDefaultAudioEndpoint(e,f,r,p) ((IMMDeviceEnumeratorVtbl*)(e))->GetDefaultAudioEndpoint(e,f,r,p)

#define IMMDevice_QueryInterface(e,i,p)  ((IMMDeviceVtbl*)(e))->QueryInterface(e,i,p)
#define IMMDevice_AddRef(e)              ((IMMDeviceVtbl*)(e))->AddRef(e)
#define IMMDevice_Release(e)             ((IMMDeviceVtbl*)(e))->Release(e)
#define IMMDevice_Activate(e,i,d,v,p)    ((IMMDeviceVtbl*)(e))->Activate(e,i,d,v,p)

#define IAudioClient_QueryInterface(c,i,p)  ((IAudioClientVtbl*)(c))->QueryInterface(c,i,p)
#define IAudioClient_AddRef(c)              ((IAudioClientVtbl*)(c))->AddRef(c)
#define IAudioClient_Release(c)             ((IAudioClientVtbl*)(c))->Release(c)
#define IAudioClient_Initialize(c,m,f,d,b,f2,p) ((IAudioClientVtbl*)(c))->Initialize(c,m,f,d,b,f2,p)
#define IAudioClient_Start(c)               ((IAudioClientVtbl*)(c))->Start(c)
#define IAudioClient_Stop(c)                ((IAudioClientVtbl*)(c))->Stop(c)
#define IAudioClient_Reset(c)               ((IAudioClientVtbl*)(c))->Reset(c)
#define IAudioClient_SetEventHandle(c,h)    ((IAudioClientVtbl*)(c))->SetEventHandle(c,h)
#define IAudioClient_GetService(c,i,p)      ((IAudioClientVtbl*)(c))->GetService(c,i,p)
#define IAudioClient_GetBufferSize(c,p)     ((IAudioClientVtbl*)(c))->GetBufferSize(c,p)

#define IAudioRenderClient_Release(r)         ((IAudioRenderClientVtbl*)(r))->Release(r)
#define IAudioRenderClient_GetBuffer(r,n,b)   ((IAudioRenderClientVtbl*)(r))->GetBuffer(r,n,b)
#define IAudioRenderClient_ReleaseBuffer(r,n,f) ((IAudioRenderClientVtbl*)(r))->ReleaseBuffer(r,n,f)
#define IAudioRenderClient_GetCurrentPadding(r,p) ((IAudioRenderClientVtbl*)(r))->GetCurrentPadding(r,p)

#define IAudioCaptureClient_Release(c)              ((IAudioCaptureClientVtbl*)(c))->Release(c)
#define IAudioCaptureClient_GetBuffer(c,b,n,f,t,t2) ((IAudioCaptureClientVtbl*)(c))->GetBuffer(c,b,n,f,t,t2)
#define IAudioCaptureClient_ReleaseBuffer(c,n)      ((IAudioCaptureClientVtbl*)(c))->ReleaseBuffer(c,n)
#define IAudioCaptureClient_GetNextPacketSize(c,p)  ((IAudioCaptureClientVtbl*)(c))->GetNextPacketSize(c,p)

#define IAudioClock_Release(cl)          ((IAudioClockVtbl*)(cl))->Release(cl)
#define IAudioClock_GetFrequency(cl,f)   ((IAudioClockVtbl*)(cl))->GetFrequency(cl,f)
#define IAudioClock_GetPosition(cl,p,q)  ((IAudioClockVtbl*)(cl))->GetPosition(cl,p,q)

/* Well-known GUIDs */
static const GUID IID_CLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID IID_IMMDeviceEnumeratorL = {
    0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID IID_IAudioClientL = {
    0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID IID_IAudioRenderClientL = {
    0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID IID_IAudioCaptureClientL = {
    0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const GUID IID_IAudioClockL = {
    0xCD63314F, 0x3FBA, 0x4A1B, { 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7 } };
static const GUID SUBTYPE_PCM_L = {
    0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID SUBTYPE_IEEE_FLOAT_L = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

/* ============================================================
 * Stream bookkeeping
 * ============================================================ */
typedef struct wasapi_stream {
    struct wasapi_stream* next;

    int32_t                   direction;
    int32_t                   format;
    int32_t                   channel_count;
    int32_t                   sample_rate;
    int32_t                   frame_bytes;
    int32_t                   burst_frames;
    int32_t                   buffer_frames;
    LONG                      state;
    LONG                      xruns;

    IAudioClient*             client;
    IAudioRenderClient*       render;
    IAudioCaptureClient*      capture;
    IAudioClock*              clock;
    HANDLE                    audio_event;
    CRITICAL_SECTION          lock;
} wasapi_stream_t;

static wasapi_stream_t* g_streams = NULL;
static CRITICAL_SECTION g_streams_lock;
static LONG g_streams_lock_ready = 0;

static void streams_lock_init(void)
{
    if (InterlockedCompareExchange(&g_streams_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_streams_lock);
    }
}

static void streams_add(wasapi_stream_t* s)
{
    EnterCriticalSection(&g_streams_lock);
    s->next = g_streams;
    g_streams = s;
    LeaveCriticalSection(&g_streams_lock);
}

static void streams_remove(wasapi_stream_t* s)
{
    EnterCriticalSection(&g_streams_lock);
    wasapi_stream_t** link = &g_streams;
    while (*link) {
        if (*link == s) {
            *link = s->next;
            break;
        }
        link = &(*link)->next;
    }
    LeaveCriticalSection(&g_streams_lock);
}

/* ============================================================
 * Time helpers
 * ============================================================ */
static int64_t wasapi_now_ns(void)
{
    return (int64_t)GetTickCount64() * 1000000LL;
}

/* ============================================================
 * Format plumbing
 * ============================================================ */
static int wasapi_make_format(int32_t format, int32_t channels, int32_t rate,
                              WAVEFORMATEXTENSIBLE* wf)
{
    int bits;
    const GUID* sub;

    switch (format) {
        case VP_AUDIO_FMT_I16:   bits = 16; sub = &SUBTYPE_PCM_L;         break;
        case VP_AUDIO_FMT_I32:   bits = 32; sub = &SUBTYPE_PCM_L;         break;
        case VP_AUDIO_FMT_FLOAT: bits = 32; sub = &SUBTYPE_IEEE_FLOAT_L;  break;
        default:
            return -1;
    }

    memset(wf, 0, sizeof(*wf));
    wf->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wf->Format.nChannels       = (WORD)channels;
    wf->Format.nSamplesPerSec  = (DWORD)rate;
    wf->Format.wBitsPerSample  = (WORD)bits;
    wf->Format.nBlockAlign     = (WORD)(channels * bits / 8);
    wf->Format.nAvgBytesPerSec = wf->Format.nSamplesPerSec * wf->Format.nBlockAlign;
    wf->Format.cbSize          = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    wf->Samples.wValidBitsPerSample = (WORD)bits;
    wf->dwChannelMask = channels == 1 ? 0x4 : (channels == 2 ? 0x3 : 0);
    wf->SubFormat = *sub;
    return 0;
}

/* ============================================================
 * Endpoint setup
 * ============================================================ */
static int wasapi_open_client(wasapi_stream_t* stream, IMMDevice** out_device,
                              IAudioClient** out_client)
{
    IMMDeviceEnumerator* enumerator = NULL;
    IMMDevice* device = NULL;
    IAudioClient* client = NULL;
    HRESULT hr;

    hr = g_com.CoCreateInstance(&IID_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumeratorL, (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        wasapi_log("CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lX", (unsigned long)hr);
        return -1;
    }

    EDataFlow flow = stream->direction == VP_AUDIO_DIR_INPUT ? eCapture : eRender;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, flow, eConsole, &device);
    IMMDeviceEnumerator_Release(enumerator);
    if (FAILED(hr) || !device) {
        wasapi_log("GetDefaultAudioEndpoint failed: 0x%08lX", (unsigned long)hr);
        return -1;
    }

    hr = IMMDevice_Activate(device, &IID_IAudioClientL, CLSCTX_ALL, NULL, (void**)&client);
    if (FAILED(hr) || !client) {
        wasapi_log("IAudioClient activation failed: 0x%08lX", (unsigned long)hr);
        IMMDevice_Release(device);
        return -1;
    }

    *out_device = device;
    *out_client = client;
    return 0;
}

static HRESULT wasapi_initialize(IAudioClient* client, const WAVEFORMATEX* format)
{
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    static const REFERENCE_TIME durations[] = { 0, 200000, 1000000 };
    HRESULT hr = E_FAIL;
    for (size_t i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, flags,
                                     durations[i], 0, format, NULL);
        if (SUCCEEDED(hr)) {
            return hr;
        }
    }
    return hr;
}

/* ============================================================
 * vp_audio_ops_t implementation
 * ============================================================ */
static int32_t wasapi_open(const vp_aaudio_config_t* cfg, void** out_user)
{
    wasapi_stream_t* s;
    WAVEFORMATEXTENSIBLE wf;
    IMMDevice* device = NULL;
    HRESULT hr;

    if (!cfg || !out_user) {
        return VP_AUDIO_ERROR_INVALID_ARG;
    }

    com_load();
    if (!g_com.ole32 || !g_com.CoCreateInstance) {
        return VP_AUDIO_ERROR_UNSUPPORTED;
    }

    int32_t channels = cfg->channel_count > 0 ? cfg->channel_count : 2;
    int32_t rate     = cfg->sample_rate > 0 ? cfg->sample_rate : 48000;
    int32_t format   = cfg->format > VP_AUDIO_FMT_UNSPECIFIED ? cfg->format : VP_AUDIO_FMT_FLOAT;

    if (channels < 1 || channels > 8) return VP_AUDIO_ERROR_INVALID_ARG;
    if (wasapi_make_format(format, channels, rate, &wf) != 0) return VP_AUDIO_ERROR_UNSUPPORTED;

    s = (wasapi_stream_t*)calloc(1, sizeof(wasapi_stream_t));
    if (!s) return VP_AUDIO_ERROR_NO_MEMORY;
    InitializeCriticalSection(&s->lock);

    s->direction     = cfg->direction;
    s->format        = format;
    s->channel_count = channels;
    s->sample_rate   = rate;
    s->frame_bytes   = (int32_t)vp_audio_frame_bytes(format, channels);
    s->state         = VP_AUDIO_STATE_OPEN;

    g_com.CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (wasapi_open_client(s, &device, &s->client) != 0) {
        DeleteCriticalSection(&s->lock);
        free(s);
        return VP_AUDIO_ERROR_NO_DEVICE;
    }

    hr = wasapi_initialize(s->client, &wf.Format);
    if (FAILED(hr)) {
        wasapi_log("IAudioClient::Initialize failed: 0x%08lX (fmt=%d ch=%d rate=%d)",
                   (unsigned long)hr, (int)format, (int)channels, (int)rate);
        IAudioClient_Release(s->client);
        IMMDevice_Release(device);
        DeleteCriticalSection(&s->lock);
        free(s);
        return VP_AUDIO_ERROR_UNSUPPORTED;
    }

    UINT32 buffer_frames = 0;
    IAudioClient_GetBufferSize(s->client, &buffer_frames);
    s->buffer_frames = (int32_t)buffer_frames;
    s->burst_frames  = (int32_t)buffer_frames;

    s->audio_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s->audio_event) goto fail_client;
    if (FAILED(IAudioClient_SetEventHandle(s->client, s->audio_event))) {
        wasapi_log("SetEventHandle failed");
        goto fail_client;
    }

    if (s->direction == VP_AUDIO_DIR_INPUT) {
        hr = IAudioClient_GetService(s->client, &IID_IAudioCaptureClientL, (void**)&s->capture);
    } else {
        hr = IAudioClient_GetService(s->client, &IID_IAudioRenderClientL, (void**)&s->render);
    }
    if (FAILED(hr) || (!s->render && !s->capture)) {
        wasapi_log("GetService failed: 0x%08lX", (unsigned long)hr);
        goto fail_client;
    }

    IAudioClient_GetService(s->client, &IID_IAudioClockL, (void**)&s->clock);
    if (s->clock) {
        UINT64 freq = 0;
        IAudioClock_GetFrequency(s->clock, &freq);
    }

    IMMDevice_Release(device);
    streams_add(s);
    *out_user = s;

    wasapi_log("%s stream: %d Hz, %d ch, fmt %d, buffer %d, burst %d",
               s->direction == VP_AUDIO_DIR_INPUT ? "capture" : "render",
               (int)rate, (int)channels, (int)format,
               (int)s->buffer_frames, (int)s->burst_frames);
    return VP_AUDIO_OK;

fail_client:
    if (s->audio_event) { CloseHandle(s->audio_event); s->audio_event = NULL; }
    if (s->clock)   { IAudioClock_Release(s->clock);         s->clock = NULL; }
    if (s->render)  { IAudioRenderClient_Release(s->render); s->render = NULL; }
    if (s->capture) { IAudioCaptureClient_Release(s->capture); s->capture = NULL; }
    if (s->client)  { IAudioClient_Release(s->client);       s->client = NULL; }
    IMMDevice_Release(device);
    DeleteCriticalSection(&s->lock);
    free(s);
    return VP_AUDIO_ERROR_NO_DEVICE;
}

static int32_t wasapi_close(void* user)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    if (!s) return VP_AUDIO_ERROR_INVALID_ARG;

    if (s->client) {
        IAudioClient_Stop(s->client);
    }
    if (s->clock)   { IAudioClock_Release(s->clock);           s->clock = NULL; }
    if (s->render)  { IAudioRenderClient_Release(s->render);   s->render = NULL; }
    if (s->capture) { IAudioCaptureClient_Release(s->capture); s->capture = NULL; }
    if (s->client)  { IAudioClient_Release(s->client);         s->client = NULL; }
    if (s->audio_event) { CloseHandle(s->audio_event); s->audio_event = NULL; }

    streams_remove(s);
    DeleteCriticalSection(&s->lock);
    free(s);
    return VP_AUDIO_OK;
}

static int32_t wasapi_start(void* user, int64_t timeout_ns)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    (void)timeout_ns;
    if (!s || !s->client) return VP_AUDIO_ERROR_INVALID_ARG;

    if (FAILED(IAudioClient_Start(s->client))) {
        return VP_AUDIO_ERROR_INTERNAL;
    }
    InterlockedExchange(&s->state, VP_AUDIO_STATE_STARTED);
    return VP_AUDIO_OK;
}

static int32_t wasapi_pause(void* user, int64_t timeout_ns)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    (void)timeout_ns;
    if (!s || !s->client) return VP_AUDIO_ERROR_INVALID_ARG;

    IAudioClient_Stop(s->client);
    InterlockedExchange(&s->state, VP_AUDIO_STATE_PAUSED);
    return VP_AUDIO_OK;
}

static int32_t wasapi_stop(void* user, int64_t timeout_ns)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    (void)timeout_ns;
    if (!s || !s->client) return VP_AUDIO_ERROR_INVALID_ARG;

    IAudioClient_Stop(s->client);
    IAudioClient_Reset(s->client);
    InterlockedExchange(&s->state, VP_AUDIO_STATE_STOPPED);
    return VP_AUDIO_OK;
}

static int32_t wasapi_flush(void* user)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    if (!s || !s->client) return VP_AUDIO_ERROR_INVALID_ARG;

    IAudioClient_Reset(s->client);
    InterlockedExchange(&s->state, VP_AUDIO_STATE_FLUSHED);
    return VP_AUDIO_OK;
}

/* ============================================================
 * Data-path passthrough: memcpy between guest and real WASAPI
 * ============================================================ */
static int32_t wasapi_write(void* user, const void* buf, int32_t frames, int32_t frame_bytes)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    if (!s || !s->render) return VP_AUDIO_ERROR_INVALID_ARG;

    /* Wait for engine period so the buffer is available. */
    WaitForSingleObject(s->audio_event, 100);

    UINT32 padding = 0;
    if (FAILED(IAudioClient_GetCurrentPadding(s->client, &padding))) {
        return VP_AUDIO_ERROR_INTERNAL;
    }
    int32_t available = s->buffer_frames - (int32_t)padding;
    if (available <= 0) {
        return 0;  /* buffer full */
    }
    if (frames > available) {
        frames = available;
    }

    BYTE* dst = NULL;
    if (FAILED(IAudioRenderClient_GetBuffer(s->render, (UINT32)frames, &dst)) || !dst) {
        return VP_AUDIO_ERROR_INTERNAL;
    }

    size_t bytes = (size_t)frames * (size_t)frame_bytes;
    memcpy(dst, buf, bytes);

    IAudioRenderClient_ReleaseBuffer(s->render, (UINT32)frames, 0);
    return frames;
}

static int32_t wasapi_read(void* user, void* buf, int32_t frames, int32_t frame_bytes)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    if (!s || !s->capture) return VP_AUDIO_ERROR_INVALID_ARG;

    /* Drain any available capture data. */
    int32_t total_read = 0;
    BYTE* dst = (BYTE*)buf;

    while (total_read < frames) {
        UINT32 packet = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(s->capture, &packet)) || packet == 0) {
            break;
        }

        BYTE* src = NULL;
        UINT32 avail = 0;
        DWORD flags = 0;
        if (FAILED(IAudioCaptureClient_GetBuffer(s->capture, &src, &avail, &flags, NULL, NULL))) {
            break;
        }

        int32_t to_copy = frames - total_read;
        if ((int32_t)avail < to_copy) {
            to_copy = (int32_t)avail;
        }

        size_t bytes = (size_t)to_copy * (size_t)frame_bytes;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            memset(dst, 0, bytes);
        } else {
            memcpy(dst, src, bytes);
        }

        IAudioCaptureClient_ReleaseBuffer(s->capture, avail);
        dst += bytes;
        total_read += to_copy;
    }

    return total_read;
}

/* ============================================================
 * Introspection
 * ============================================================ */
static int32_t wasapi_get_info(void* user, vp_aaudio_info_t* out)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    if (!s || !out) return VP_AUDIO_ERROR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->direction              = s->direction;
    out->sample_rate            = s->sample_rate;
    out->channel_count          = s->channel_count;
    out->format                 = s->format;
    out->sharing_mode           = VP_AUDIO_SHARING_SHARED;
    out->frames_per_burst       = s->burst_frames;
    out->buffer_size_frames     = s->buffer_frames;
    out->buffer_capacity_frames = s->buffer_frames;
    out->frame_bytes            = s->frame_bytes;
    out->state                  = s->state;
    out->xrun_count            = s->xruns;
    return VP_AUDIO_OK;
}

static int32_t wasapi_get_timestamp(void* user, vp_aaudio_timestamp_t* out)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    if (!s || !out) return VP_AUDIO_ERROR_INVALID_ARG;

    int64_t position = 0;
    if (s->clock) {
        UINT64 pos = 0;
        UINT64 qpc = 0;
        if (SUCCEEDED(IAudioClock_GetPosition(s->clock, &pos, &qpc))) {
            position = (int64_t)pos;
        }
    }

    memset(out, 0, sizeof(*out));
    out->position     = position;
    out->timestamp_ns = wasapi_now_ns();
    out->sample_rate  = s->sample_rate;
    return VP_AUDIO_OK;
}

static int32_t wasapi_set_buffer_size(void* user, int32_t frames, int32_t* applied_out)
{
    wasapi_stream_t* s = (wasapi_stream_t*)user;
    if (!s) return VP_AUDIO_ERROR_INVALID_ARG;

    /* Shared mode: the engine owns the period, report it back. */
    if (applied_out) {
        *applied_out = s->buffer_frames;
    }
    return VP_AUDIO_OK;
}

static LONG g_device_probe = -1;

static int wasapi_probe_device(int32_t direction)
{
    if (!g_com.ole32 || !g_com.CoCreateInstance) return 0;
    if (g_device_probe >= 0) return g_device_probe;

    int found = 0;
    g_com.CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = NULL;
    HRESULT hr = g_com.CoCreateInstance(&IID_CLSID_MMDeviceEnumerator, NULL,
                                        CLSCTX_ALL, &IID_IMMDeviceEnumeratorL,
                                        (void**)&enumerator);
    if (SUCCEEDED(hr) && enumerator) {
        IMMDevice* device = NULL;
        EDataFlow flow = direction == VP_AUDIO_DIR_INPUT ? eCapture : eRender;
        if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, flow,
                                                                 eConsole, &device)) && device) {
            found = 1;
            IMMDevice_Release(device);
        }
        IMMDeviceEnumerator_Release(enumerator);
    }

    g_device_probe = found;
    return found;
}

static uint32_t wasapi_query(void)
{
    com_load();
    if (!g_com.ole32 || !g_com.CoCreateInstance) {
        return 0;
    }
    uint32_t caps = VP_AUDIO_CAP_OUTPUT;
    if (wasapi_probe_device(VP_AUDIO_DIR_INPUT)) {
        caps |= VP_AUDIO_CAP_INPUT;
    }
    caps |= VP_AUDIO_CAP_TIMESTAMP;
    return caps;
}

static const vp_audio_ops_t g_wasapi_ops = {
    .open            = wasapi_open,
    .close           = wasapi_close,
    .start           = wasapi_start,
    .pause           = wasapi_pause,
    .stop            = wasapi_stop,
    .flush           = wasapi_flush,
    .write           = wasapi_write,
    .read            = wasapi_read,
    .get_info        = wasapi_get_info,
    .get_timestamp   = wasapi_get_timestamp,
    .set_buffer_size = wasapi_set_buffer_size,
    .query           = wasapi_query,
};

const vp_audio_ops_t* win32_aaudio_ops(void)
{
    streams_lock_init();
    return &g_wasapi_ops;
}

void win32_aaudio_shutdown(void)
{
    if (g_streams_lock_ready) {
        EnterCriticalSection(&g_streams_lock);
        wasapi_stream_t* s = g_streams;
        g_streams = NULL;
        LeaveCriticalSection(&g_streams_lock);
        while (s) {
            wasapi_stream_t* next = s->next;
            wasapi_close(s);
            s = next;
        }
    }
    com_unload();
}
