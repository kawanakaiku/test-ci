/* トポロジーの解決に失敗したとき、メディアセッションとメディアエンジンが何を知らせるかを記録する。
 * Windows (GitHub の windows-11-arm) と Wine で同じ exe を流して比べる。
 *
 * 自作のソース (偽の subtype 'WTST' の映像、または無圧縮の RGB32 の映像、偽の音声) を使うので、
 * 端末や Windows に入っている復号の部品に左右されない。
 *
 * 使い方: mftopoerr.exe [場合...]   場合を省くと全部
 *   session-*: セッション (sample grabber へ)。engine-*: エンジン (frame server)。一覧は main の cases
 */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfmediaengine.h>
#include <dxgiformat.h>
#include <stdio.h>
#include <string.h>

DEFINE_GUID(MFVideoFormat_WTST, MAKEFOURCC('W','T','S','T'), 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(MFAudioFormat_WTSA, MAKEFOURCC('W','T','S','A'), 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

static DWORD start_tick;
static IMFTopology *session_topology;
#define LOG(...) do { printf("%6lu ", GetTickCount() - start_tick); printf(__VA_ARGS__); fflush(stdout); } while (0)

static const char *session_event_name(MediaEventType t)
{
    switch (t)
    {
        case MEError: return "MEError";
        case MESessionTopologySet: return "MESessionTopologySet";
        case MESessionTopologiesCleared: return "MESessionTopologiesCleared";
        case MESessionStarted: return "MESessionStarted";
        case MESessionPaused: return "MESessionPaused";
        case MESessionStopped: return "MESessionStopped";
        case MESessionClosed: return "MESessionClosed";
        case MESessionEnded: return "MESessionEnded";
        case MESessionRateChanged: return "MESessionRateChanged";
        case MESessionCapabilitiesChanged: return "MESessionCapabilitiesChanged";
        case MESessionTopologyStatus: return "MESessionTopologyStatus";
        case MESessionNotifyPresentationTime: return "MESessionNotifyPresentationTime";
        case MENewPresentation: return "MENewPresentation";
        case MEEndOfPresentation: return "MEEndOfPresentation";
        case MESessionStreamSinkFormatChanged: return "MESessionStreamSinkFormatChanged";
        case MENonFatalError: return "MENonFatalError";
        default: return "?";
    }
}

static const char *engine_event_name(DWORD e)
{
    static const char *names[] = {"0", "LOADSTART", "PROGRESS", "SUSPEND", "ABORT", "ERROR", "EMPTIED", "STALLED", "PLAY",
        "PAUSE", "LOADEDMETADATA", "LOADEDDATA", "WAITING", "PLAYING", "CANPLAY", "CANPLAYTHROUGH", "SEEKING", "SEEKED",
        "TIMEUPDATE", "ENDED", "RATECHANGE", "DURATIONCHANGE", "VOLUMECHANGE"};
    static const char *ext[] = {"FORMATCHANGE", "PURGEQUEUEDEVENTS", "TIMELINE_MARKER", "BALANCECHANGE", "DOWNLOADCOMPLETE",
        "BUFFERINGSTARTED", "BUFFERINGENDED", "FRAMESTEPCOMPLETED", "NOTIFYSTABLESTATE", "FIRSTFRAMEREADY", "TRACKSCHANGE",
        "OPMINFO", "RESOURCELOST", "DELAYLOADEVENT_CHANGED", "STREAMRENDERINGERROR", "SUPPORTEDRATES_CHANGED", "AUDIOENDPOINTCHANGE"};
    if (e < ARRAYSIZE(names)) return names[e];
    if (e >= 1000 && e - 1000 < ARRAYSIZE(ext)) return ext[e - 1000];
    return "?";
}

/* ---- 自作のソース ---- */

enum stream_kind { KIND_FAKE_VIDEO, KIND_RGB32_VIDEO, KIND_FAKE_AUDIO, KIND_PCM_AUDIO, KIND_RGB32_ERR };

/* KIND_RGB32_ERR: この回数目の RequestSample で流れに MEError (E_FAIL) を積む */
#define ERROR_AT 3

struct test_source;

struct test_stream
{
    IMFMediaStream IMFMediaStream_iface;
    LONG refcount;
    struct test_source *source;
    IMFStreamDescriptor *sd;
    IMFMediaEventQueue *queue;
    enum stream_kind kind;
    BOOL active, started_once, ended;
    int sent;
};

struct test_source
{
    IMFMediaSource IMFMediaSource_iface;
    LONG refcount;
    IMFMediaEventQueue *queue;
    IMFPresentationDescriptor *pd;
    struct test_stream *streams[2];
    unsigned int count;
    CRITICAL_SECTION cs;
};

static struct test_stream *impl_from_IMFMediaStream(IMFMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct test_stream, IMFMediaStream_iface);
}

static struct test_source *impl_from_IMFMediaSource(IMFMediaSource *iface)
{
    return CONTAINING_RECORD(iface, struct test_source, IMFMediaSource_iface);
}

static HRESULT WINAPI stream_QueryInterface(IMFMediaStream *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFMediaStream) || IsEqualIID(riid, &IID_IMFMediaEventGenerator) || IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFMediaStream_AddRef(iface);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI stream_AddRef(IMFMediaStream *iface)
{
    return InterlockedIncrement(&impl_from_IMFMediaStream(iface)->refcount);
}

static ULONG WINAPI stream_Release(IMFMediaStream *iface)
{
    struct test_stream *stream = impl_from_IMFMediaStream(iface);
    ULONG refcount = InterlockedDecrement(&stream->refcount);
    if (!refcount)
    {
        IMFMediaEventQueue_Shutdown(stream->queue);
        IMFMediaEventQueue_Release(stream->queue);
        IMFStreamDescriptor_Release(stream->sd);
        free(stream);
    }
    return refcount;
}

static HRESULT WINAPI stream_GetEvent(IMFMediaStream *iface, DWORD flags, IMFMediaEvent **event)
{
    return IMFMediaEventQueue_GetEvent(impl_from_IMFMediaStream(iface)->queue, flags, event);
}

static HRESULT WINAPI stream_BeginGetEvent(IMFMediaStream *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    return IMFMediaEventQueue_BeginGetEvent(impl_from_IMFMediaStream(iface)->queue, callback, state);
}

static HRESULT WINAPI stream_EndGetEvent(IMFMediaStream *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    return IMFMediaEventQueue_EndGetEvent(impl_from_IMFMediaStream(iface)->queue, result, event);
}

static HRESULT WINAPI stream_QueueEvent(IMFMediaStream *iface, MediaEventType type, REFGUID ext, HRESULT hr, const PROPVARIANT *value)
{
    return IMFMediaEventQueue_QueueEventParamVar(impl_from_IMFMediaStream(iface)->queue, type, ext, hr, value);
}

static HRESULT WINAPI stream_GetMediaSource(IMFMediaStream *iface, IMFMediaSource **source)
{
    struct test_stream *stream = impl_from_IMFMediaStream(iface);
    *source = &stream->source->IMFMediaSource_iface;
    IMFMediaSource_AddRef(*source);
    return S_OK;
}

static HRESULT WINAPI stream_GetStreamDescriptor(IMFMediaStream *iface, IMFStreamDescriptor **sd)
{
    struct test_stream *stream = impl_from_IMFMediaStream(iface);
    *sd = stream->sd;
    IMFStreamDescriptor_AddRef(*sd);
    return S_OK;
}

#define FRAMES 90
#define FRAME_DURATION 333333

static HRESULT WINAPI stream_RequestSample(IMFMediaStream *iface, IUnknown *token)
{
    struct test_stream *stream = impl_from_IMFMediaStream(iface);
    struct test_source *source = stream->source;
    IMFMediaBuffer *buffer;
    IMFSample *sample;
    DWORD size = (stream->kind == KIND_FAKE_AUDIO || stream->kind == KIND_PCM_AUDIO) ? 4800 * 4 : 64 * 64 * 4, i;
    BYTE *data;
    HRESULT hr;

    EnterCriticalSection(&source->cs);
    if (stream->sent == 0)
        LOG("  source: 流れ %u の最初の RequestSample\n", stream->kind);
    if (stream->kind == KIND_RGB32_ERR && stream->sent == ERROR_AT)
    {
        LOG("  source: 流れに MEError (E_FAIL) を積む\n");
        stream->sent++;
        IMFMediaEventQueue_QueueEventParamVar(stream->queue, MEError, &GUID_NULL, E_FAIL, NULL);
        LeaveCriticalSection(&source->cs);
        return S_OK;
    }
    if (stream->sent >= FRAMES)
    {
        BOOL all = TRUE;
        if (!stream->ended)
        {
            stream->ended = TRUE;
            IMFMediaEventQueue_QueueEventParamVar(stream->queue, MEEndOfStream, &GUID_NULL, S_OK, NULL);
            for (i = 0; i < source->count; ++i)
                if (source->streams[i]->active && !source->streams[i]->ended) all = FALSE;
            if (all)
                IMFMediaEventQueue_QueueEventParamVar(source->queue, MEEndOfPresentation, &GUID_NULL, S_OK, NULL);
        }
        LeaveCriticalSection(&source->cs);
        return S_OK;
    }
    hr = MFCreateMemoryBuffer(size, &buffer);
    if (SUCCEEDED(hr))
    {
        IMFMediaBuffer_Lock(buffer, &data, NULL, NULL);
        for (i = 0; i < size; ++i)
            data[i] = (BYTE)(i + stream->sent * 7);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_SetCurrentLength(buffer, size);
        hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr))
        {
            IMFSample_AddBuffer(sample, buffer);
            if (stream->kind == KIND_FAKE_AUDIO || stream->kind == KIND_PCM_AUDIO)
            {
                IMFSample_SetSampleTime(sample, (LONGLONG)stream->sent * 1000000);
                IMFSample_SetSampleDuration(sample, 1000000);
            }
            else
            {
                IMFSample_SetSampleTime(sample, (LONGLONG)stream->sent * FRAME_DURATION);
                IMFSample_SetSampleDuration(sample, FRAME_DURATION);
            }
            if (token)
                IMFSample_SetUnknown(sample, &MFSampleExtension_Token, token);
            stream->sent++;
            hr = IMFMediaEventQueue_QueueEventParamUnk(stream->queue, MEMediaSample, &GUID_NULL, S_OK, (IUnknown *)sample);
            IMFSample_Release(sample);
        }
        IMFMediaBuffer_Release(buffer);
    }
    LeaveCriticalSection(&source->cs);
    return hr;
}

static const IMFMediaStreamVtbl stream_vtbl =
{
    stream_QueryInterface, stream_AddRef, stream_Release,
    stream_GetEvent, stream_BeginGetEvent, stream_EndGetEvent, stream_QueueEvent,
    stream_GetMediaSource, stream_GetStreamDescriptor, stream_RequestSample,
};

static HRESULT WINAPI source_QueryInterface(IMFMediaSource *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFMediaSource) || IsEqualIID(riid, &IID_IMFMediaEventGenerator) || IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFMediaSource_AddRef(iface);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI source_AddRef(IMFMediaSource *iface)
{
    return InterlockedIncrement(&impl_from_IMFMediaSource(iface)->refcount);
}

static ULONG WINAPI source_Release(IMFMediaSource *iface)
{
    struct test_source *source = impl_from_IMFMediaSource(iface);
    ULONG refcount = InterlockedDecrement(&source->refcount);
    unsigned int i;
    if (!refcount)
    {
        IMFMediaEventQueue_Shutdown(source->queue);
        IMFMediaEventQueue_Release(source->queue);
        IMFPresentationDescriptor_Release(source->pd);
        for (i = 0; i < source->count; ++i)
            IMFMediaStream_Release(&source->streams[i]->IMFMediaStream_iface);
        DeleteCriticalSection(&source->cs);
        free(source);
    }
    return refcount;
}

static HRESULT WINAPI source_GetEvent(IMFMediaSource *iface, DWORD flags, IMFMediaEvent **event)
{
    return IMFMediaEventQueue_GetEvent(impl_from_IMFMediaSource(iface)->queue, flags, event);
}

static HRESULT WINAPI source_BeginGetEvent(IMFMediaSource *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    return IMFMediaEventQueue_BeginGetEvent(impl_from_IMFMediaSource(iface)->queue, callback, state);
}

static HRESULT WINAPI source_EndGetEvent(IMFMediaSource *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    return IMFMediaEventQueue_EndGetEvent(impl_from_IMFMediaSource(iface)->queue, result, event);
}

static HRESULT WINAPI source_QueueEvent(IMFMediaSource *iface, MediaEventType type, REFGUID ext, HRESULT hr, const PROPVARIANT *value)
{
    return IMFMediaEventQueue_QueueEventParamVar(impl_from_IMFMediaSource(iface)->queue, type, ext, hr, value);
}

static HRESULT WINAPI source_GetCharacteristics(IMFMediaSource *iface, DWORD *flags)
{
    *flags = MFMEDIASOURCE_CAN_PAUSE | MFMEDIASOURCE_CAN_SEEK;
    return S_OK;
}

static HRESULT WINAPI source_CreatePresentationDescriptor(IMFMediaSource *iface, IMFPresentationDescriptor **pd)
{
    LOG("  source: CreatePresentationDescriptor\n");
    return IMFPresentationDescriptor_Clone(impl_from_IMFMediaSource(iface)->pd, pd);
}

static HRESULT WINAPI source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *pd, const GUID *time_format, const PROPVARIANT *position)
{
    struct test_source *source = impl_from_IMFMediaSource(iface);
    PROPVARIANT pos;
    unsigned int i;
    DWORD count;

    LOG("  source: Start (位置の型 %u)\n", position ? position->vt : 0);
    EnterCriticalSection(&source->cs);
    IMFPresentationDescriptor_GetStreamDescriptorCount(pd, &count);
    for (i = 0; i < source->count; ++i)
        source->streams[i]->active = FALSE;
    for (i = 0; i < count; ++i)
    {
        IMFStreamDescriptor *sd;
        BOOL selected;
        DWORD id;
        if (FAILED(IMFPresentationDescriptor_GetStreamDescriptorByIndex(pd, i, &selected, &sd)))
            continue;
        IMFStreamDescriptor_GetStreamIdentifier(sd, &id);
        IMFStreamDescriptor_Release(sd);
        if (!selected || id >= source->count)
            continue;
        LOG("  source: 流れ %lu を選んでいる\n", id);
        source->streams[id]->active = TRUE;
    }
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = position && position->vt == VT_I8 ? position->hVal.QuadPart : 0;
    for (i = 0; i < source->count; ++i)
    {
        struct test_stream *stream = source->streams[i];
        if (!stream->active)
            continue;
        IMFMediaEventQueue_QueueEventParamUnk(source->queue, stream->started_once ? MEUpdatedStream : MENewStream,
                &GUID_NULL, S_OK, (IUnknown *)&stream->IMFMediaStream_iface);
        stream->started_once = TRUE;
        IMFMediaEventQueue_QueueEventParamVar(stream->queue, MEStreamStarted, &GUID_NULL, S_OK, &pos);
    }
    IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourceStarted, &GUID_NULL, S_OK, &pos);
    LeaveCriticalSection(&source->cs);
    return S_OK;
}

static HRESULT WINAPI source_Stop(IMFMediaSource *iface)
{
    struct test_source *source = impl_from_IMFMediaSource(iface);
    unsigned int i;
    LOG("  source: Stop\n");
    EnterCriticalSection(&source->cs);
    for (i = 0; i < source->count; ++i)
        if (source->streams[i]->active)
            IMFMediaEventQueue_QueueEventParamVar(source->streams[i]->queue, MEStreamStopped, &GUID_NULL, S_OK, NULL);
    IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourceStopped, &GUID_NULL, S_OK, NULL);
    LeaveCriticalSection(&source->cs);
    return S_OK;
}

static HRESULT WINAPI source_Pause(IMFMediaSource *iface)
{
    struct test_source *source = impl_from_IMFMediaSource(iface);
    unsigned int i;
    LOG("  source: Pause\n");
    EnterCriticalSection(&source->cs);
    for (i = 0; i < source->count; ++i)
        if (source->streams[i]->active)
            IMFMediaEventQueue_QueueEventParamVar(source->streams[i]->queue, MEStreamPaused, &GUID_NULL, S_OK, NULL);
    IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourcePaused, &GUID_NULL, S_OK, NULL);
    LeaveCriticalSection(&source->cs);
    return S_OK;
}

static HRESULT WINAPI source_Shutdown(IMFMediaSource *iface)
{
    struct test_source *source = impl_from_IMFMediaSource(iface);
    unsigned int i;
    LOG("  source: Shutdown\n");
    IMFMediaEventQueue_Shutdown(source->queue);
    for (i = 0; i < source->count; ++i)
        IMFMediaEventQueue_Shutdown(source->streams[i]->queue);
    return S_OK;
}

static const IMFMediaSourceVtbl source_vtbl =
{
    source_QueryInterface, source_AddRef, source_Release,
    source_GetEvent, source_BeginGetEvent, source_EndGetEvent, source_QueueEvent,
    source_GetCharacteristics, source_CreatePresentationDescriptor, source_Start, source_Stop, source_Pause, source_Shutdown,
};

static IMFMediaType *create_type(enum stream_kind kind)
{
    IMFMediaType *type;
    MFCreateMediaType(&type);
    if (kind == KIND_FAKE_AUDIO || kind == KIND_PCM_AUDIO)
    {
        IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, kind == KIND_FAKE_AUDIO ? &MFAudioFormat_WTSA : &MFAudioFormat_PCM);
        IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, 2);
        IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        if (kind == KIND_PCM_AUDIO)
        {
            IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
            IMFMediaType_SetUINT32(type, &MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
        }
    }
    else
    {
        IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, kind == KIND_FAKE_VIDEO ? &MFVideoFormat_WTST : &MFVideoFormat_RGB32);
        IMFMediaType_SetUINT64(type, &MF_MT_FRAME_SIZE, ((UINT64)64 << 32) | 64);
        IMFMediaType_SetUINT64(type, &MF_MT_FRAME_RATE, ((UINT64)30 << 32) | 1);
        IMFMediaType_SetUINT64(type, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
        if (kind == KIND_RGB32_VIDEO || kind == KIND_RGB32_ERR)
        {
            IMFMediaType_SetUINT32(type, &MF_MT_DEFAULT_STRIDE, 64 * 4);
            IMFMediaType_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            IMFMediaType_SetUINT32(type, &MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
            IMFMediaType_SetUINT32(type, &MF_MT_FIXED_SIZE_SAMPLES, 1);
            IMFMediaType_SetUINT32(type, &MF_MT_SAMPLE_SIZE, 64 * 64 * 4);
        }
    }
    return type;
}

static struct test_source *create_source(const enum stream_kind *kinds, unsigned int count)
{
    IMFStreamDescriptor *sds[2];
    struct test_source *source = calloc(1, sizeof(*source));
    unsigned int i;

    source->IMFMediaSource_iface.lpVtbl = &source_vtbl;
    source->refcount = 1;
    InitializeCriticalSection(&source->cs);
    MFCreateEventQueue(&source->queue);
    for (i = 0; i < count; ++i)
    {
        struct test_stream *stream = calloc(1, sizeof(*stream));
        IMFMediaType *type = create_type(kinds[i]);
        IMFMediaTypeHandler *handler;

        stream->IMFMediaStream_iface.lpVtbl = &stream_vtbl;
        stream->refcount = 1;
        stream->source = source;
        stream->kind = kinds[i];
        MFCreateEventQueue(&stream->queue);
        MFCreateStreamDescriptor(i, 1, &type, &stream->sd);
        IMFStreamDescriptor_GetMediaTypeHandler(stream->sd, &handler);
        IMFMediaTypeHandler_SetCurrentMediaType(handler, type);
        IMFMediaTypeHandler_Release(handler);
        IMFMediaType_Release(type);
        source->streams[i] = stream;
        sds[i] = stream->sd;
    }
    source->count = count;
    MFCreatePresentationDescriptor(count, sds, &source->pd);
    for (i = 0; i < count; ++i)
        IMFPresentationDescriptor_SelectStream(source->pd, i);
    IMFPresentationDescriptor_SetUINT64(source->pd, &MF_PD_DURATION, (UINT64)FRAMES * FRAME_DURATION);
    return source;
}

/* ---- セッション ---- */

struct grabber
{
    IMFSampleGrabberSinkCallback iface;
    LONG refcount, samples;
};

static HRESULT WINAPI grabber_QueryInterface(IMFSampleGrabberSinkCallback *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFSampleGrabberSinkCallback) || IsEqualIID(riid, &IID_IMFClockStateSink) || IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFSampleGrabberSinkCallback_AddRef(iface);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI grabber_AddRef(IMFSampleGrabberSinkCallback *iface) { return InterlockedIncrement(&((struct grabber *)iface)->refcount); }
static ULONG WINAPI grabber_Release(IMFSampleGrabberSinkCallback *iface) { return InterlockedDecrement(&((struct grabber *)iface)->refcount); }
static HRESULT WINAPI grabber_OnClockStart(IMFSampleGrabberSinkCallback *iface, MFTIME t, LONGLONG o) { return S_OK; }
static HRESULT WINAPI grabber_OnClockStop(IMFSampleGrabberSinkCallback *iface, MFTIME t) { return S_OK; }
static HRESULT WINAPI grabber_OnClockPause(IMFSampleGrabberSinkCallback *iface, MFTIME t) { return S_OK; }
static HRESULT WINAPI grabber_OnClockRestart(IMFSampleGrabberSinkCallback *iface, MFTIME t) { return S_OK; }
static HRESULT WINAPI grabber_OnClockSetRate(IMFSampleGrabberSinkCallback *iface, MFTIME t, float r) { return S_OK; }
static HRESULT WINAPI grabber_OnSetPresentationClock(IMFSampleGrabberSinkCallback *iface, IMFPresentationClock *c) { return S_OK; }
static HRESULT WINAPI grabber_OnProcessSample(IMFSampleGrabberSinkCallback *iface, REFGUID major, DWORD flags,
        LONGLONG time, LONGLONG duration, const BYTE *buffer, DWORD size)
{
    InterlockedIncrement(&((struct grabber *)iface)->samples);
    return S_OK;
}
static HRESULT WINAPI grabber_OnShutdown(IMFSampleGrabberSinkCallback *iface) { return S_OK; }

static const IMFSampleGrabberSinkCallbackVtbl grabber_vtbl =
{
    grabber_QueryInterface, grabber_AddRef, grabber_Release,
    grabber_OnClockStart, grabber_OnClockStop, grabber_OnClockPause, grabber_OnClockRestart, grabber_OnClockSetRate,
    grabber_OnSetPresentationClock, grabber_OnProcessSample, grabber_OnShutdown,
};

static void print_session_event(IMFMediaEvent *event)
{
    MediaEventType type = 0;
    HRESULT status = S_OK;
    PROPVARIANT value;
    UINT32 topo_status = 0;

    PropVariantInit(&value);
    IMFMediaEvent_GetType(event, &type);
    IMFMediaEvent_GetStatus(event, &status);
    IMFMediaEvent_GetValue(event, &value);
    if (type == MESessionTopologyStatus)
        IMFMediaEvent_GetUINT32(event, &MF_EVENT_TOPOLOGY_STATUS, &topo_status);
    LOG("session event %lu %s status %#lx value vt %u%s", type, session_event_name(type), status, value.vt,
            value.vt == VT_UNKNOWN && !value.punkVal ? " (NULL)" : "");
    if (type == MESessionTopologyStatus)
        printf(" topo_status %u", topo_status);
    if (type == MESessionTopologySet && value.vt == VT_UNKNOWN && value.punkVal)
    {
        IMFTopology *t = NULL;
        WORD nodes = 0;
        IUnknown_QueryInterface(value.punkVal, &IID_IMFTopology, (void **)&t);
        if (t) { IMFTopology_GetNodeCount(t, &nodes); IMFTopology_Release(t); }
        printf(" %s nodes %u", t == session_topology ? "(渡したトポロジー)" : "(別のトポロジー)", nodes);
    }
    printf("\n");
    fflush(stdout);
    PropVariantClear(&value);
}

static void pump_session(IMFMediaSession *session, DWORD ms, MediaEventType until)
{
    DWORD end = GetTickCount() + ms;
    while ((LONG)(end - GetTickCount()) > 0)
    {
        IMFMediaEvent *event;
        MediaEventType type = 0;
        HRESULT hr = IMFMediaSession_GetEvent(session, MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE)
        {
            Sleep(10);
            continue;
        }
        if (FAILED(hr))
        {
            LOG("GetEvent hr %#lx\n", hr);
            return;
        }
        print_session_event(event);
        IMFMediaEvent_GetType(event, &type);
        IMFMediaEvent_Release(event);
        if (until && type == until)
            return;
    }
}



static void run_session(const char *name, const enum stream_kind *kinds, unsigned int count)
{
    struct grabber grabbers[2] = {{{&grabber_vtbl}, 1, 0}, {{&grabber_vtbl}, 1, 0}};
    struct test_source *source = create_source(kinds, count);
    IMFTopologyNode *src_nodes[2], *sink_nodes[2];
    IMFActivate *activates[2];
    IMFPresentationDescriptor *pd;
    IMFMediaSession *session;
    IMFTopology *topology;
    PROPVARIANT start;
    unsigned int i;
    HRESULT hr;

    LOG("== %s\n", name);
    hr = MFCreateMediaSession(NULL, &session);
    LOG("MFCreateMediaSession hr %#lx\n", hr);
    if (FAILED(hr)) return;
    MFCreateTopology(&topology);
    session_topology = topology;
    IMFMediaSource_CreatePresentationDescriptor(&source->IMFMediaSource_iface, &pd);
    for (i = 0; i < count; ++i)
    {
        IMFStreamDescriptor *sd;
        IMFMediaType *type;
        BOOL selected, audio = kinds[i] == KIND_FAKE_AUDIO || kinds[i] == KIND_PCM_AUDIO;

        IMFPresentationDescriptor_GetStreamDescriptorByIndex(pd, i, &selected, &sd);
        MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &src_nodes[i]);
        IMFTopologyNode_SetUnknown(src_nodes[i], &MF_TOPONODE_SOURCE, (IUnknown *)&source->IMFMediaSource_iface);
        IMFTopologyNode_SetUnknown(src_nodes[i], &MF_TOPONODE_PRESENTATION_DESCRIPTOR, (IUnknown *)pd);
        IMFTopologyNode_SetUnknown(src_nodes[i], &MF_TOPONODE_STREAM_DESCRIPTOR, (IUnknown *)sd);
        IMFStreamDescriptor_Release(sd);
        IMFTopology_AddNode(topology, src_nodes[i]);
        MFCreateMediaType(&type);
        IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, audio ? &MFMediaType_Audio : &MFMediaType_Video);
        IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, audio ? &MFAudioFormat_PCM : &MFVideoFormat_RGB32);
        hr = MFCreateSampleGrabberSinkActivate(type, &grabbers[i].iface, &activates[i]);
        IMFMediaType_Release(type);
        LOG("MFCreateSampleGrabberSinkActivate %u hr %#lx\n", i, hr);
        MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &sink_nodes[i]);
        IMFTopologyNode_SetObject(sink_nodes[i], (IUnknown *)activates[i]);
        IMFTopology_AddNode(topology, sink_nodes[i]);
        IMFTopologyNode_ConnectOutput(src_nodes[i], 0, sink_nodes[i], 0);
    }

    hr = IMFMediaSession_SetTopology(session, 0, topology);
    LOG("SetTopology hr %#lx\n", hr);
    pump_session(session, 2000, 0);
    PropVariantInit(&start);
    hr = IMFMediaSession_Start(session, &GUID_NULL, &start);
    LOG("Start hr %#lx\n", hr);
    pump_session(session, 2500, 0);
    for (i = 0; i < count; ++i)
        LOG("grabber %u の受けたサンプル %ld\n", i, grabbers[i].samples);
    hr = IMFMediaSession_Close(session);
    LOG("Close hr %#lx\n", hr);
    pump_session(session, 2000, MESessionClosed);
    IMFMediaSession_Shutdown(session);
    IMFMediaSource_Shutdown(&source->IMFMediaSource_iface);
    IMFMediaSession_Release(session);
    for (i = 0; i < count; ++i)
    {
        IMFTopologyNode_Release(src_nodes[i]);
        IMFTopologyNode_Release(sink_nodes[i]);
        IMFActivate_ShutdownObject(activates[i]);
        IMFActivate_Release(activates[i]);
    }
    session_topology = NULL;
    IMFTopology_Release(topology);
    IMFPresentationDescriptor_Release(pd);
    IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

/* ---- エンジン ---- */

struct notify
{
    IMFMediaEngineNotify iface;
    LONG refcount;
    IMFMediaEngine *engine;
    HANDLE error_event;
};

static HRESULT WINAPI notify_QueryInterface(IMFMediaEngineNotify *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFMediaEngineNotify) || IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFMediaEngineNotify_AddRef(iface);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI notify_AddRef(IMFMediaEngineNotify *iface) { return InterlockedIncrement(&((struct notify *)iface)->refcount); }
static ULONG WINAPI notify_Release(IMFMediaEngineNotify *iface) { return InterlockedDecrement(&((struct notify *)iface)->refcount); }

static HRESULT WINAPI notify_EventNotify(IMFMediaEngineNotify *iface, DWORD event, DWORD_PTR param1, DWORD param2)
{
    struct notify *notify = (struct notify *)iface;
    LOG("engine event %lu %s param1 %#Ix param2 %#lx\n", event, engine_event_name(event), param1, param2);
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR && notify->engine)
    {
        IMFMediaError *error = NULL;
        HRESULT hr = IMFMediaEngine_GetError(notify->engine, &error);
        LOG("  GetError hr %#lx error %p code %u extended %#lx network %u ready %u\n", hr, error,
                error ? IMFMediaError_GetErrorCode(error) : 0, error ? IMFMediaError_GetExtendedErrorCode(error) : 0,
                IMFMediaEngine_GetNetworkState(notify->engine), IMFMediaEngine_GetReadyState(notify->engine));
        if (error) IMFMediaError_Release(error);
        SetEvent(notify->error_event);
    }
    return S_OK;
}

static const IMFMediaEngineNotifyVtbl notify_vtbl = {notify_QueryInterface, notify_AddRef, notify_Release, notify_EventNotify};

struct extension
{
    IMFMediaEngineExtension iface;
    LONG refcount;
    const enum stream_kind *kinds;
    unsigned int count;
};

static HRESULT WINAPI extension_QueryInterface(IMFMediaEngineExtension *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFMediaEngineExtension) || IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFMediaEngineExtension_AddRef(iface);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI extension_AddRef(IMFMediaEngineExtension *iface) { return InterlockedIncrement(&((struct extension *)iface)->refcount); }
static ULONG WINAPI extension_Release(IMFMediaEngineExtension *iface) { return InterlockedDecrement(&((struct extension *)iface)->refcount); }
static HRESULT WINAPI extension_CanPlayType(IMFMediaEngineExtension *iface, BOOL audio_only, BSTR mime, MF_MEDIA_ENGINE_CANPLAY *answer)
{
    *answer = MF_MEDIA_ENGINE_CANPLAY_PROBABLY;
    return S_OK;
}
static HRESULT WINAPI extension_BeginCreateObject(IMFMediaEngineExtension *iface, BSTR url, IMFByteStream *bytestream,
        MF_OBJECT_TYPE type, IUnknown **cancel_cookie, IMFAsyncCallback *callback, IUnknown *state)
{
    struct extension *extension = (struct extension *)iface;
    struct test_source *source;
    IMFAsyncResult *result;
    HRESULT hr;

    LOG("  extension: BeginCreateObject type %u url %ls\n", type, url ? url : L"(null)");
    if (cancel_cookie) *cancel_cookie = NULL;
    if (type != MF_OBJECT_MEDIASOURCE)
        return MF_E_UNEXPECTED;
    source = create_source(extension->kinds, extension->count);
    hr = MFCreateAsyncResult((IUnknown *)&source->IMFMediaSource_iface, callback, state, &result);
    IMFMediaSource_Release(&source->IMFMediaSource_iface);
    if (FAILED(hr)) return hr;
    hr = MFInvokeCallback(result);
    IMFAsyncResult_Release(result);
    return hr;
}
static HRESULT WINAPI extension_CancelObjectCreation(IMFMediaEngineExtension *iface, IUnknown *cookie) { return S_OK; }
static HRESULT WINAPI extension_EndCreateObject(IMFMediaEngineExtension *iface, IMFAsyncResult *result, IUnknown **object)
{
    HRESULT hr = IMFAsyncResult_GetObject(result, object);
    LOG("  extension: EndCreateObject hr %#lx\n", hr);
    return hr;
}

static const IMFMediaEngineExtensionVtbl extension_vtbl =
{
    extension_QueryInterface, extension_AddRef, extension_Release,
    extension_CanPlayType, extension_BeginCreateObject, extension_CancelObjectCreation, extension_EndCreateObject,
};

static void print_engine_state(IMFMediaEngine *engine, const char *label)
{
    IMFMediaError *error = NULL;
    IMFMediaEngine_GetError(engine, &error);
    LOG("%s: ready %u network %u time %.3f error %u extended %#lx ended %d paused %d\n", label,
            IMFMediaEngine_GetReadyState(engine), IMFMediaEngine_GetNetworkState(engine), IMFMediaEngine_GetCurrentTime(engine),
            error ? IMFMediaError_GetErrorCode(error) : 0, error ? IMFMediaError_GetExtendedErrorCode(error) : 0,
            IMFMediaEngine_IsEnded(engine), IMFMediaEngine_IsPaused(engine));
    if (error) IMFMediaError_Release(error);
}

static void run_engine(const char *name, const enum stream_kind *kinds, unsigned int count)
{
    struct notify notify = {{&notify_vtbl}, 1, NULL, CreateEventW(NULL, TRUE, FALSE, NULL)};
    struct extension extension = {{&extension_vtbl}, 1, kinds, count};
    IMFMediaEngineClassFactory *factory;
    IMFMediaEngine *engine;
    IMFAttributes *attributes;
    BSTR url;
    HRESULT hr;

    LOG("== %s\n", name);
    hr = CoCreateInstance(&CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IMFMediaEngineClassFactory, (void **)&factory);
    LOG("factory hr %#lx\n", hr);
    if (FAILED(hr)) return;
    MFCreateAttributes(&attributes, 4);
    IMFAttributes_SetUnknown(attributes, &MF_MEDIA_ENGINE_CALLBACK, (IUnknown *)&notify.iface);
    IMFAttributes_SetUnknown(attributes, &MF_MEDIA_ENGINE_EXTENSION, (IUnknown *)&extension.iface);
    IMFAttributes_SetUINT32(attributes, &MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    hr = IMFMediaEngineClassFactory_CreateInstance(factory, 0, attributes, &engine);
    LOG("CreateInstance hr %#lx\n", hr);
    IMFAttributes_Release(attributes);
    IMFMediaEngineClassFactory_Release(factory);
    if (FAILED(hr)) return;
    notify.engine = engine;
    url = SysAllocString(L"mftopoerr://test");
    hr = IMFMediaEngine_SetSource(engine, url);
    LOG("SetSource hr %#lx\n", hr);
    SysFreeString(url);
    WaitForSingleObject(notify.error_event, 3000);
    Sleep(300);
    print_engine_state(engine, "読み込みの後");
    hr = IMFMediaEngine_Play(engine);
    LOG("Play hr %#lx\n", hr);
    {
        DWORD end = GetTickCount() + 2500;
        LONGLONG pts;
        int frames = 0;
        while ((LONG)(end - GetTickCount()) > 0)
        {
            if (IMFMediaEngine_OnVideoStreamTick(engine, &pts) == S_OK) frames++;
            Sleep(16);
        }
        LOG("OnVideoStreamTick の新しいコマ %d\n", frames);
    }
    print_engine_state(engine, "Play の後");
    hr = IMFMediaEngine_Shutdown(engine);
    LOG("Shutdown hr %#lx\n", hr);
    notify.engine = NULL;
    IMFMediaEngine_Release(engine);
    CloseHandle(notify.error_event);
}

int main(int argc, char **argv)
{
    static const enum stream_kind fake[] = {KIND_FAKE_VIDEO}, ok[] = {KIND_RGB32_VIDEO}, mixed[] = {KIND_FAKE_AUDIO, KIND_RGB32_VIDEO},
            afake[] = {KIND_FAKE_AUDIO}, pcm[] = {KIND_PCM_AUDIO, KIND_RGB32_VIDEO}, vfake_pcm[] = {KIND_PCM_AUDIO, KIND_FAKE_VIDEO},
            err[] = {KIND_RGB32_ERR}, two_video[] = {KIND_FAKE_VIDEO, KIND_RGB32_VIDEO};
    static const struct { const char *name; BOOL engine; const enum stream_kind *kinds; unsigned int count; } cases[] =
    {
        {"session-fake", FALSE, fake, 1},
        {"session-ok", FALSE, ok, 1},
        {"session-two", FALSE, two_video, 2},
        {"session-err", FALSE, err, 1},
        {"session-pcm", FALSE, pcm, 2},
        {"session-mixed", FALSE, mixed, 2},
        {"engine-fake", TRUE, fake, 1},
        {"engine-mixed", TRUE, mixed, 2},
        {"engine-afake", TRUE, afake, 1},
        {"engine-pcm", TRUE, pcm, 2},
        {"engine-vfake-pcm", TRUE, vfake_pcm, 2},
        {"engine-err", TRUE, err, 1},
        {"engine-ok", TRUE, ok, 1},
    };
    unsigned int i;
    int j;

    start_tick = GetTickCount();
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    for (i = 0; i < ARRAYSIZE(cases); ++i)
    {
        BOOL want = argc < 2;
        for (j = 1; j < argc; ++j)
            if (!strcmp(argv[j], cases[i].name)) want = TRUE;
        if (!want) continue;
        if (cases[i].engine) run_engine(cases[i].name, cases[i].kinds, cases[i].count);
        else run_session(cases[i].name, cases[i].kinds, cases[i].count);
    }
    MFShutdown();
    CoUninitialize();
    LOG("done\n");
    return 0;
}
