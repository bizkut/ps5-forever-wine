/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine audio driver for a PS5 title, Unix side (wineps5audio.so).
 *
 * The stream and mmdevapi interface follow Wine's OSS driver
 * (dlls/wineoss.drv/oss.c, Copyright 2011 Andrew Eikum for CodeWeavers,
 * 2022 Huw Davies).  The single bounded mixer thread, console-clocked grains,
 * and PCM conversion design are adapted from prospero-win
 * wine/wineps5/unix.c and src/pw_audio_mix.c (LGPL-2.1-or-later).  The native
 * port shape follows WoWPS5 v0.1.1 runtime/wine-ps5/wineps5_audio.c
 * (LGPL-2.1-or-later); console calls stay behind Wine's trusted title ABI.
 *
 * One render endpoint mixes every started stream into 256-frame, 48-kHz,
 * signed-16 stereo grains.  There is no capture endpoint.  A game parent is
 * paced by sceAudioOutOutput; a title child without the parent-only callback
 * exposes the same endpoint but discards and clock-paces grains silently.
 */
#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "audioclient.h"
#include "mmdeviceapi.h"
#include "mmddk.h"

#include "wine/debug.h"
#include "wine/ps5_title.h"
#include "wine/unixlib.h"

#include "mixer.h"
#include "unixlib.h"

struct ps5_stream
{
    struct ps5_stream *next;
    WAVEFORMATEX *fmt;
    AUDCLNT_SHAREMODE share;
    UINT flags;
    HANDLE event;
    BOOL playing, timer_quit;

    UINT64 written_frames, last_pos_frames;
    UINT32 period_frames, bufsize_frames, tmp_buffer_frames;
    UINT32 period_output_frames, since_event;
    REFERENCE_TIME period;

    BYTE *local_buffer, *tmp_buffer;
    INT32 getbuf_last;
    struct ps5_audio_mix_stream mix;
};

static const GUID subtype_pcm =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID subtype_float =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const PROPERTYKEY physical_speakers_key =
    {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 3};
static const REFERENCE_TIME default_period = 100000;
static const REFERENCE_TIME minimum_period = 50000;
static const char device_name[] = "ps5audio";
static const WCHAR endpoint_name[] = {'P','S','5',' ','T','i','t','l','e',' ','A','u','d','i','o',0};

static ULONG_PTR zero_bits;
static pthread_mutex_t ps5_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ps5_started = PTHREAD_COND_INITIALIZER;
static sigset_t ps5_signals;
static struct ps5_stream *ps5_streams;
static struct ps5_stream *ps5_mixer_owner;
static BOOL ps5_shutdown;
/* -1: not attempted; 0: clocked silent sink; 1: real parent port. */
static int ps5_port = -1;

static BOOL profile_blocking( BOOL blocking )
{
#ifdef __PROSPERO__
    return ps5_title_profile_set_blocking( blocking );
#else
    (void)blocking;
    return FALSE;
#endif
}

static void wait_for_mixer(void)
{
    BOOL previous_blocking = profile_blocking( TRUE );

    pthread_cond_wait( &ps5_started, &ps5_lock );
    profile_blocking( previous_blocking );
}

static void ps5_enter( sigset_t *saved )
{
    BOOL previous_blocking;

    pthread_sigmask( SIG_BLOCK, &ps5_signals, saved );
    previous_blocking = profile_blocking( TRUE );
    pthread_mutex_lock( &ps5_lock );
    profile_blocking( previous_blocking );
}

static void ps5_leave( const sigset_t *saved )
{
    pthread_mutex_unlock( &ps5_lock );
    pthread_sigmask( SIG_SETMASK, saved, NULL );
}

static void init_signals(void)
{
    sigemptyset( &ps5_signals );
    sigaddset( &ps5_signals, SIGALRM );
    sigaddset( &ps5_signals, SIGIO );
    sigaddset( &ps5_signals, SIGINT );
    sigaddset( &ps5_signals, SIGHUP );
    sigaddset( &ps5_signals, SIGQUIT );
    sigaddset( &ps5_signals, SIGUSR1 );
    sigaddset( &ps5_signals, SIGUSR2 );
    sigaddset( &ps5_signals, SIGCHLD );
}

static BOOL title_process(void)
{
#ifdef __PROSPERO__
    return __wine_ps5_is_title();
#else
    return FALSE;
#endif
}

static BOOL title_audio_marker(void)
{
    const char *value = getenv( "WINEPS5_TITLE_AUDIO" );
    return value && value[0] == '1' && !value[1];
}

static BOOL port_available(void)
{
    BOOL callbacks = FALSE, first = FALSE, marker = title_audio_marker(), title = title_process();
    int open_result = -1, port;
    sigset_t saved;

    if (!title && !marker)
    {
        MESSAGE( "wine: title audio init step=availability title=0 marker=0 callbacks=0 sink=unavailable\n" );
        return FALSE;
    }
    ps5_enter( &saved );
    if (ps5_port < 0)
    {
        first = TRUE;
#ifdef __PROSPERO__
        callbacks = __wine_ps5_audio_available();
        if (callbacks)
        {
            open_result = __wine_ps5_audio_open( PS5_AUDIO_RATE, PS5_AUDIO_GRAIN );
            ps5_port = open_result >= 0;
        }
        else
#endif
            ps5_port = 0;
    }
    port = ps5_port;
    ps5_leave( &saved );
    if (first)
        MESSAGE( "wine: title audio init step=availability title=%u marker=%u callbacks=%u sink=%s open_rc=%d\n",
                 title, marker, callbacks, port > 0 ? "audioout" : "silent", open_result );
    return TRUE;
}

static int output_grain( const int16_t *grain )
{
#ifdef __PROSPERO__
    if (ps5_port > 0) return __wine_ps5_audio_output( grain ) < 0 ? -1 : 0;
#endif
    return 1;
}

static NTSTATUS ps5_not_implemented( void *args )
{
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_process_attach( void *args )
{
    init_signals();
    MESSAGE( "wine: title audio init step=driver_process_attach status=00000000\n" );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_process_detach( void *args )
{
    int close_port;
    sigset_t saved;

    ps5_enter( &saved );
    ps5_shutdown = TRUE;
    pthread_cond_broadcast( &ps5_started );
    while (ps5_mixer_owner) wait_for_mixer();
    close_port = ps5_port > 0;
    ps5_leave( &saved );

#ifdef __PROSPERO__
    if (close_port) __wine_ps5_audio_close();
#endif
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_main_loop( void *args )
{
    struct main_loop_params *params = args;

    NtSetEvent( params->event, NULL );
    return STATUS_SUCCESS;
}

static int muldiv( int a, int b, int c )
{
    LONGLONG result;

    if (!c) return -1;
    if (c < 0)
    {
        a = -a;
        c = -c;
    }
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        result = ((LONGLONG)a * b + c / 2) / c;
    else
        result = ((LONGLONG)a * b - c / 2) / c;
    if (result > 2147483647 || result < -2147483647) return -1;
    return result;
}

static NTSTATUS leave_result( const sigset_t *saved, HRESULT *result, HRESULT value )
{
    *result = value;
    ps5_leave( saved );
    return STATUS_SUCCESS;
}

static struct ps5_stream *handle_get_stream( stream_handle handle )
{
    return (struct ps5_stream *)(UINT_PTR)handle;
}

static BOOL is_device( const char *device, EDataFlow flow )
{
    return flow == eRender && device && !strcmp( device, device_name );
}

static NTSTATUS ps5_test_connect( void *args )
{
    struct test_connect_params *params = args;

    params->priority = port_available() ? Priority_Preferred : Priority_Unavailable;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_endpoint_ids( void *args )
{
    struct get_endpoint_ids_params *params = args;
    unsigned int count = params->flow == eRender && port_available() ? 1 : 0;
    unsigned int offset, needed;

    offset = needed = count * sizeof(*params->endpoints);
    if (count) needed += sizeof(endpoint_name) + ((sizeof(device_name) + 1) & ~1);
    params->num = count;
    params->default_idx = 0;
    if (needed > params->size)
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
        return STATUS_SUCCESS;
    }
    if (count)
    {
        params->endpoints[0].name = offset;
        memcpy( (char *)params->endpoints + offset, endpoint_name, sizeof(endpoint_name) );
        offset += sizeof(endpoint_name);
        params->endpoints[0].device = offset;
        memcpy( (char *)params->endpoints + offset, device_name, sizeof(device_name) );
    }
    params->result = S_OK;
    return STATUS_SUCCESS;
}


static HRESULT mixer_format( const WAVEFORMATEX *fmt, struct ps5_audio_format *format )
{
    const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
    BOOL is_float;

    format->channel_mask = 0;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) is_float = FALSE;
    else if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) is_float = TRUE;
    else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        if (IsEqualGUID( &ext->SubFormat, &subtype_pcm )) is_float = FALSE;
        else if (IsEqualGUID( &ext->SubFormat, &subtype_float )) is_float = TRUE;
        else return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (ext->Samples.wValidBitsPerSample > fmt->wBitsPerSample) return E_INVALIDARG;
        format->channel_mask = ext->dwChannelMask;
    }
    else return AUDCLNT_E_UNSUPPORTED_FORMAT;

    format->rate = fmt->nSamplesPerSec;
    format->channels = fmt->nChannels;
    format->bits = fmt->wBitsPerSample;
    format->block_align = fmt->nBlockAlign;
    format->encoding = is_float ? PS5_AUDIO_FLOAT : PS5_AUDIO_PCM;
    if (ps5_audio_mix_format_check( format ) != PS5_AUDIO_MIX_OK)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    return S_OK;
}

static WAVEFORMATEXTENSIBLE *clone_format( const WAVEFORMATEX *fmt )
{
    WAVEFORMATEXTENSIBLE *copy;
    size_t size = fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                  ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);

    if (!(copy = malloc( size ))) return NULL;
    memcpy( copy, fmt, size );
    copy->Format.cbSize = size - sizeof(WAVEFORMATEX);
    return copy;
}

static void free_stream( struct ps5_stream *stream )
{
    SIZE_T size = 0;

    if (stream->local_buffer)
        NtFreeVirtualMemory( NtCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE );
    size = 0;
    if (stream->tmp_buffer)
        NtFreeVirtualMemory( NtCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE );
    free( stream->fmt );
    free( stream );
}

static NTSTATUS ps5_create_stream( void *args )
{
    struct create_stream_params *params = args;
    WAVEFORMATEXTENSIBLE *format_copy;
    struct ps5_audio_format format;
    struct ps5_stream *stream;
    int period_frames, period_output_frames, buffer_frames;
    sigset_t saved;
    SIZE_T size;

    if (!is_device( params->device, params->flow ))
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if (FAILED(params->result = mixer_format( params->fmt, &format ))) return STATUS_SUCCESS;
    if (!(stream = calloc( 1, sizeof(*stream) )))
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    if (!(format_copy = clone_format( params->fmt )))
    {
        params->result = E_OUTOFMEMORY;
        goto failed;
    }
    stream->fmt = &format_copy->Format;
    period_frames = muldiv( params->fmt->nSamplesPerSec, params->period, 10000000 );
    period_output_frames = muldiv( PS5_AUDIO_RATE, params->period, 10000000 );
    if (period_frames <= 0 || period_output_frames <= 0)
    {
        params->result = E_INVALIDARG;
        goto failed;
    }
    buffer_frames = muldiv( params->duration, params->fmt->nSamplesPerSec, 10000000 );
    if (buffer_frames <= 0)
    {
        params->result = AUDCLNT_E_BUFFER_SIZE_ERROR;
        goto failed;
    }
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        buffer_frames -= buffer_frames % period_frames;
    if (buffer_frames <= 0)
    {
        params->result = AUDCLNT_E_BUFFER_SIZE_ERROR;
        goto failed;
    }
    stream->period = params->period;
    stream->period_frames = period_frames;
    stream->period_output_frames = period_output_frames;
    stream->bufsize_frames = buffer_frames;
    size = (SIZE_T)stream->bufsize_frames * params->fmt->nBlockAlign;
    if (NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&stream->local_buffer,
                                 zero_bits, &size, MEM_COMMIT, PAGE_READWRITE ))
    {
        params->result = E_OUTOFMEMORY;
        goto failed;
    }
    if (ps5_audio_mix_stream_init( &stream->mix, &format, stream->local_buffer,
                                   stream->bufsize_frames ) != PS5_AUDIO_MIX_OK)
    {
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
        goto failed;
    }
    stream->share = params->share;
    stream->flags = params->flags;

    ps5_enter( &saved );
    stream->next = ps5_streams;
    ps5_streams = stream;
    ps5_leave( &saved );
    *params->channel_count = params->fmt->nChannels;
    *params->stream = (stream_handle)(UINT_PTR)stream;
    params->result = S_OK;
    return STATUS_SUCCESS;

failed:
    free_stream( stream );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_release_stream( void *args )
{
    struct release_stream_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream ), **link;
    sigset_t saved;

    /* A timer thread can be the global mixer or be waiting to take over.
     * Stop and join it before removing the stream the mixer may still read. */
    ps5_enter( &saved );
    stream->timer_quit = TRUE;
    pthread_cond_broadcast( &ps5_started );
    ps5_leave( &saved );
    if (params->timer_thread)
    {
        NtWaitForSingleObject( params->timer_thread, FALSE, NULL );
        NtClose( params->timer_thread );
    }

    ps5_enter( &saved );
    for (link = &ps5_streams; *link; link = &(*link)->next)
    {
        if (*link != stream) continue;
        *link = stream->next;
        break;
    }
    ps5_leave( &saved );
    free_stream( stream );
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static UINT64 monotonic_ns(void)
{
    struct timespec now;

    clock_gettime( CLOCK_MONOTONIC, &now );
    return (UINT64)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void sleep_ns( UINT64 ns )
{
    struct timespec delay;
    BOOL previous_blocking;

    if (ns > 100000000) ns = 100000000;
    delay.tv_sec = 0;
    delay.tv_nsec = ns;
    previous_blocking = profile_blocking( TRUE );
    nanosleep( &delay, NULL );
    profile_blocking( previous_blocking );
}

static void mixer_loop( struct ps5_stream *owner )
{
    static float accumulator[2 * PS5_AUDIO_GRAIN];
    static int16_t grain[2 * PS5_AUDIO_GRAIN];
    HANDLE events[32];
    struct ps5_stream *stream;
    UINT64 epoch = 0, frames = 0, grains = 0, underruns = 0, failures = 0, next_report = 0;
    unsigned int event_count, playing, i;
    UINT64 now, due;
    sigset_t saved;
    int status;

    ps5_enter( &saved );
    while (!ps5_shutdown && !owner->timer_quit)
    {
        playing = 0;
        for (stream = ps5_streams; stream; stream = stream->next) playing += stream->playing;
        if (!playing)
        {
            wait_for_mixer();
            epoch = 0;
            continue;
        }

        event_count = 0;
        memset( accumulator, 0, sizeof(accumulator) );
        for (stream = ps5_streams; stream; stream = stream->next)
        {
            uint32_t mixed;

            if (!stream->playing) continue;
            mixed = ps5_audio_mix_add( &stream->mix, accumulator, PS5_AUDIO_GRAIN );
            underruns += mixed != PS5_AUDIO_GRAIN;
            stream->since_event += PS5_AUDIO_GRAIN;
            if (stream->since_event < stream->period_output_frames) continue;
            stream->since_event %= stream->period_output_frames;
            if (stream->event && event_count < ARRAY_SIZE(events)) events[event_count++] = stream->event;
        }
        ps5_leave( &saved );

        for (i = 0; i < event_count; i++) NtSetEvent( events[i], NULL );
        ps5_audio_mix_store( accumulator, grain, PS5_AUDIO_GRAIN );
        now = monotonic_ns();
        if (!epoch)
        {
            epoch = now;
            frames = 0;
        }
        status = output_grain( grain );
        frames += PS5_AUDIO_GRAIN;
        grains++;
        if (status < 0) failures++;

        now = monotonic_ns();
        due = epoch + frames * 1000000000 / PS5_AUDIO_RATE;
        if (status && due > now) sleep_ns( due - now );
        else if (!status && due > now + 60000000) sleep_ns( due - now - 40000000 );
        else if (now > due + 250000000)
        {
            epoch = now;
            frames = 0;
        }
        if (ps5_port > 0)
        {
            if (!next_report) next_report = now + (UINT64)10 * 1000000000;
            else if (now >= next_report)
            {
                MESSAGE( "wine: title audio summary grains_output=%llu underruns=%llu failures=%llu\n",
                         (unsigned long long)grains, (unsigned long long)underruns,
                         (unsigned long long)failures );
                next_report = now + (UINT64)10 * 1000000000;
            }
        }
        ps5_enter( &saved );
    }
    if (ps5_mixer_owner == owner) ps5_mixer_owner = NULL;
    pthread_cond_broadcast( &ps5_started );
    ps5_leave( &saved );
}

static NTSTATUS ps5_start( void *args )
{
    struct start_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        return leave_result( &saved, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_SET );
    if (stream->playing)
        return leave_result( &saved, &params->result, AUDCLNT_E_NOT_STOPPED );
    stream->playing = TRUE;
    stream->since_event = 0;
    pthread_cond_broadcast( &ps5_started );
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_stop( void *args )
{
    struct stop_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    if (!stream->playing) return leave_result( &saved, &params->result, S_FALSE );
    stream->playing = FALSE;
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_reset( void *args )
{
    struct reset_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    if (stream->playing)
        return leave_result( &saved, &params->result, AUDCLNT_E_NOT_STOPPED );
    if (stream->getbuf_last)
        return leave_result( &saved, &params->result, AUDCLNT_E_BUFFER_OPERATION_PENDING );
    stream->written_frames = 0;
    stream->last_pos_frames = 0;
    stream->since_event = 0;
    ps5_audio_mix_stream_reset( &stream->mix );
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_timer_loop( void *args )
{
    struct timer_loop_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    /* mmdevapi supplies one timer thread per stream. Exactly one is elected
     * to mix all streams; the others sleep and can take over when its stream
     * is released. No waiting thread allocates or touches a grain. */
    ps5_enter( &saved );
    while (!ps5_shutdown && !stream->timer_quit && ps5_mixer_owner)
        wait_for_mixer();
    if (ps5_shutdown || stream->timer_quit)
    {
        ps5_leave( &saved );
        return STATUS_SUCCESS;
    }
    ps5_mixer_owner = stream;
    ps5_leave( &saved );
    mixer_loop( stream );
    return STATUS_SUCCESS;
}

static void silence_buffer( struct ps5_stream *stream, BYTE *buffer, UINT32 frames )
{
    int value = stream->mix.format.encoding == PS5_AUDIO_PCM && stream->mix.format.bits == 8 ? 128 : 0;

    memset( buffer, value, (SIZE_T)frames * stream->fmt->nBlockAlign );
}

static NTSTATUS ps5_get_render_buffer( void *args )
{
    struct get_render_buffer_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    UINT32 write_position, frames = params->frames;
    sigset_t saved;
    SIZE_T size;

    ps5_enter( &saved );
    if (stream->getbuf_last)
        return leave_result( &saved, &params->result, AUDCLNT_E_OUT_OF_ORDER );
    if (!frames) return leave_result( &saved, &params->result, S_OK );
    if (stream->mix.held + frames > stream->bufsize_frames)
        return leave_result( &saved, &params->result, AUDCLNT_E_BUFFER_TOO_LARGE );

    write_position = (stream->mix.offset + stream->mix.held) % stream->bufsize_frames;
    if (write_position + frames > stream->bufsize_frames)
    {
        if (stream->tmp_buffer_frames < frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory( NtCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE );
                stream->tmp_buffer = NULL;
            }
            size = (SIZE_T)frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&stream->tmp_buffer,
                                         zero_bits, &size, MEM_COMMIT, PAGE_READWRITE ))
            {
                stream->tmp_buffer_frames = 0;
                return leave_result( &saved, &params->result, E_OUTOFMEMORY );
            }
            stream->tmp_buffer_frames = frames;
        }
        *params->data = stream->tmp_buffer;
        stream->getbuf_last = -(INT32)frames;
    }
    else
    {
        *params->data = stream->local_buffer + (SIZE_T)write_position * stream->fmt->nBlockAlign;
        stream->getbuf_last = frames;
    }
    silence_buffer( stream, *params->data, frames );
    return leave_result( &saved, &params->result, S_OK );
}

static void wrap_buffer( struct ps5_stream *stream, const BYTE *buffer, UINT32 written_frames )
{
    UINT32 write_frames = (stream->mix.offset + stream->mix.held) % stream->bufsize_frames;
    UINT32 chunk_frames = stream->bufsize_frames - write_frames;
    SIZE_T chunk_bytes = (SIZE_T)chunk_frames * stream->fmt->nBlockAlign;
    SIZE_T written_bytes = (SIZE_T)written_frames * stream->fmt->nBlockAlign;

    if (written_bytes <= chunk_bytes)
        memcpy( stream->local_buffer + (SIZE_T)write_frames * stream->fmt->nBlockAlign,
                buffer, written_bytes );
    else
    {
        memcpy( stream->local_buffer + (SIZE_T)write_frames * stream->fmt->nBlockAlign,
                buffer, chunk_bytes );
        memcpy( stream->local_buffer, buffer + chunk_bytes, written_bytes - chunk_bytes );
    }
}

static NTSTATUS ps5_release_render_buffer( void *args )
{
    struct release_render_buffer_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    UINT32 written = params->written_frames;
    BYTE *buffer;
    sigset_t saved;

    ps5_enter( &saved );
    if (!written)
    {
        stream->getbuf_last = 0;
        return leave_result( &saved, &params->result, S_OK );
    }
    if (!stream->getbuf_last)
        return leave_result( &saved, &params->result, AUDCLNT_E_OUT_OF_ORDER );
    if (written > (stream->getbuf_last > 0 ? stream->getbuf_last : -stream->getbuf_last))
        return leave_result( &saved, &params->result, AUDCLNT_E_INVALID_SIZE );
    if (stream->getbuf_last > 0)
        buffer = stream->local_buffer + (SIZE_T)stream->fmt->nBlockAlign *
                 ((stream->mix.offset + stream->mix.held) % stream->bufsize_frames);
    else
        buffer = stream->tmp_buffer;
    if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT) silence_buffer( stream, buffer, written );
    if (stream->getbuf_last < 0) wrap_buffer( stream, buffer, written );
    stream->mix.held += written;
    stream->written_frames += written;
    stream->getbuf_last = 0;
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_capture_buffer( void *args )
{
    struct get_capture_buffer_params *params = args;

    params->result = AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_release_capture_buffer( void *args )
{
    struct release_capture_buffer_params *params = args;

    params->result = AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_is_format_supported( void *args )
{
    struct is_format_supported_params *params = args;
    struct ps5_audio_format format;

    if (!is_device( params->device, params->flow ))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else
        params->result = mixer_format( params->fmt_in, &format );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_mix_format( void *args )
{
    struct get_mix_format_params *params = args;
    WAVEFORMATEXTENSIBLE *fmt = params->fmt;

    if (!is_device( params->device, params->flow ))
    {
        params->result = params->flow == eRender ? AUDCLNT_E_DEVICE_INVALIDATED : E_UNEXPECTED;
        return STATUS_SUCCESS;
    }
    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.nChannels = 2;
    fmt->Format.nSamplesPerSec = PS5_AUDIO_RATE;
    fmt->Format.wBitsPerSample = 32;
    fmt->Format.nBlockAlign = 8;
    fmt->Format.nAvgBytesPerSec = PS5_AUDIO_RATE * 8;
    fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt->Samples.wValidBitsPerSample = 32;
    fmt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    fmt->SubFormat = subtype_float;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_device_period( void *args )
{
    struct get_device_period_params *params = args;

    if (params->def_period) *params->def_period = default_period;
    if (params->min_period) *params->min_period = minimum_period;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_buffer_size( void *args )
{
    struct get_buffer_size_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    *params->frames = stream->bufsize_frames;
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_latency( void *args )
{
    struct get_latency_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    *params->latency = stream->period +
        (REFERENCE_TIME)PS5_AUDIO_GRAIN * 10000000 / PS5_AUDIO_RATE;
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_current_padding( void *args )
{
    struct get_current_padding_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    *params->padding = stream->mix.held;
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_next_packet_size( void *args )
{
    struct get_next_packet_size_params *params = args;

    *params->frames = 0;
    params->result = AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_frequency( void *args )
{
    struct get_frequency_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    *params->freq = stream->fmt->nSamplesPerSec;
    if (stream->share == AUDCLNT_SHAREMODE_SHARED) *params->freq *= stream->fmt->nBlockAlign;
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_position( void *args )
{
    struct get_position_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    UINT64 *position = params->pos;
    sigset_t saved;

    if (params->device)
    {
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }
    ps5_enter( &saved );
    *position = stream->written_frames - stream->mix.held;
    if (*position < stream->last_pos_frames) *position = stream->last_pos_frames;
    stream->last_pos_frames = *position;
    if (stream->share == AUDCLNT_SHAREMODE_SHARED) *position *= stream->fmt->nBlockAlign;
    if (params->qpctime)
    {
        LARGE_INTEGER stamp, frequency;
        NtQueryPerformanceCounter( &stamp, &frequency );
        *params->qpctime = stamp.QuadPart * (INT64)10000000 / frequency.QuadPart;
    }
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_set_volumes( void *args )
{
    struct set_volumes_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    float gain[PS5_AUDIO_MAX_CHANNELS];
    unsigned int channel;
    sigset_t saved;

    ps5_enter( &saved );
    for (channel = 0; channel < stream->mix.format.channels; channel++)
        gain[channel] = params->master_volume * params->volumes[channel] * params->session_volumes[channel];
    ps5_audio_mix_set_gain( &stream->mix, gain, stream->mix.format.channels );
    ps5_leave( &saved );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_set_event_handle( void *args )
{
    struct set_event_handle_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        return leave_result( &saved, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED );
    if (stream->event)
        return leave_result( &saved, &params->result, HRESULT_FROM_WIN32(ERROR_INVALID_NAME) );
    stream->event = params->event;
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_set_sample_rate( void *args )
{
    struct set_sample_rate_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    if (!(stream->flags & AUDCLNT_STREAMFLAGS_RATEADJUST))
        return leave_result( &saved, &params->result, AUDCLNT_E_UNSUPPORTED_FORMAT );
    if (!(params->rate >= PS5_AUDIO_MIN_RATE && params->rate <= PS5_AUDIO_MAX_RATE))
        return leave_result( &saved, &params->result, E_INVALIDARG );
    ps5_audio_mix_set_rate( &stream->mix, params->rate );
    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_is_started( void *args )
{
    struct is_started_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );
    return leave_result( &saved, &params->result, stream->playing ? S_OK : S_FALSE );
}

static NTSTATUS ps5_get_prop_value( void *args )
{
    struct get_prop_value_params *params = args;

    if (is_device( params->device, params->flow ) &&
        IsEqualPropertyKey( *params->prop, physical_speakers_key ))
    {
        params->value->vt = VT_UI4;
        params->value->ulVal = KSAUDIO_SPEAKER_STEREO;
        params->result = S_OK;
    }
    else params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

static UINT no_device( UINT message, UINT count_message )
{
    switch (message)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        return MMSYSERR_NOERROR;
    default:
        return message == count_message ? 0 : MMSYSERR_BADDEVICEID;
    }
}

static NTSTATUS ps5_midi_out_message( void *args )
{
    struct midi_out_message_params *params = args;

    params->notify->send_notify = FALSE;
    *params->err = no_device( params->msg, MODM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_midi_in_message( void *args )
{
    struct midi_in_message_params *params = args;

    params->notify->send_notify = FALSE;
    *params->err = no_device( params->msg, MIDM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_midi_notify_wait( void *args )
{
    struct midi_notify_wait_params *params = args;

    *params->quit = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_aux_message( void *args )
{
    struct aux_message_params *params = args;

    *params->err = no_device( params->msg, AUXDM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    ps5_process_attach,
    ps5_process_detach,
    ps5_main_loop,
    ps5_get_endpoint_ids,
    ps5_create_stream,
    ps5_release_stream,
    ps5_start,
    ps5_stop,
    ps5_reset,
    ps5_timer_loop,
    ps5_get_render_buffer,
    ps5_release_render_buffer,
    ps5_get_capture_buffer,
    ps5_release_capture_buffer,
    ps5_is_format_supported,
    ps5_not_implemented,
    ps5_get_mix_format,
    ps5_get_device_period,
    ps5_get_buffer_size,
    ps5_get_latency,
    ps5_get_current_padding,
    ps5_get_next_packet_size,
    ps5_get_frequency,
    ps5_get_position,
    ps5_set_volumes,
    ps5_set_event_handle,
    ps5_set_sample_rate,
    ps5_test_connect,
    ps5_is_started,
    ps5_get_prop_value,
    ps5_not_implemented,
    ps5_not_implemented,
    ps5_not_implemented,
    ps5_midi_out_message,
    ps5_midi_in_message,
    ps5_midi_notify_wait,
    ps5_aux_message,
};

C_ASSERT(ARRAY_SIZE(__wine_unix_call_funcs) == funcs_count);

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS ps5_wow64_process_attach( void *args )
{
    SYSTEM_BASIC_INFORMATION info;

    ps5_process_attach( args );
    NtQuerySystemInformation( SystemEmulationBasicInformation, &info, sizeof(info), NULL );
    zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_main_loop( void *args )
{
    struct { PTR32 event; } *params32 = args;
    struct main_loop_params params = { ULongToHandle(params32->event) };

    return ps5_main_loop( &params );
}

static NTSTATUS ps5_wow64_test_connect( void *args )
{
    struct { PTR32 name; enum driver_priority priority; } *params32 = args;
    struct test_connect_params params = { ULongToPtr(params32->name), Priority_Neutral };

    ps5_test_connect( &params );
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_endpoint_ids( void *args )
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        params32->flow, ULongToPtr(params32->endpoints), params32->size,
        params32->result, params32->num, params32->default_idx
    };

    ps5_get_endpoint_ids( &params );
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_create_stream( void *args )
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        DWORD flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        ULongToPtr(params32->name), ULongToPtr(params32->device), params32->flow,
        params32->share, params32->flags, params32->duration, params32->period,
        ULongToPtr(params32->fmt), params32->result,
        ULongToPtr(params32->channel_count), ULongToPtr(params32->stream)
    };

    ps5_create_stream( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_release_stream( void *args )
{
    struct { stream_handle stream; PTR32 timer_thread; HRESULT result; } *params32 = args;
    struct release_stream_params params =
        { params32->stream, ULongToHandle(params32->timer_thread), params32->result };

    ps5_release_stream( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_render_buffer( void *args )
{
    struct { stream_handle stream; UINT32 frames; HRESULT result; PTR32 data; } *params32 = args;
    BYTE *data;
    struct get_render_buffer_params params =
        { params32->stream, params32->frames, params32->result, &data };

    ps5_get_render_buffer( &params );
    params32->result = params.result;
    if (SUCCEEDED(params.result)) *(PTR32 *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_capture_buffer( void *args )
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data, frames, flags, devpos, qpcpos;
    } *params32 = args;

    params32->result = AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_is_format_supported( void *args )
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
        { ULongToPtr(params32->device), params32->flow, params32->share,
          ULongToPtr(params32->fmt_in), params32->result };

    ps5_is_format_supported( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_mix_format( void *args )
{
    struct { PTR32 device; EDataFlow flow; PTR32 fmt; HRESULT result; } *params32 = args;
    struct get_mix_format_params params =
        { ULongToPtr(params32->device), params32->flow, ULongToPtr(params32->fmt), params32->result };

    ps5_get_mix_format( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_device_period( void *args )
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period, min_period;
    } *params32 = args;
    struct get_device_period_params params =
        { ULongToPtr(params32->device), params32->flow, params32->result,
          ULongToPtr(params32->def_period), ULongToPtr(params32->min_period) };

    ps5_get_device_period( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_buffer_size( void *args )
{
    struct { stream_handle stream; HRESULT result; PTR32 frames; } *params32 = args;
    struct get_buffer_size_params params =
        { params32->stream, params32->result, ULongToPtr(params32->frames) };

    ps5_get_buffer_size( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_latency( void *args )
{
    struct { stream_handle stream; HRESULT result; PTR32 latency; } *params32 = args;
    struct get_latency_params params =
        { params32->stream, params32->result, ULongToPtr(params32->latency) };

    ps5_get_latency( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_current_padding( void *args )
{
    struct { stream_handle stream; HRESULT result; PTR32 padding; } *params32 = args;
    struct get_current_padding_params params =
        { params32->stream, params32->result, ULongToPtr(params32->padding) };

    ps5_get_current_padding( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_next_packet_size( void *args )
{
    struct { stream_handle stream; HRESULT result; PTR32 frames; } *params32 = args;
    struct get_next_packet_size_params params =
        { params32->stream, params32->result, ULongToPtr(params32->frames) };

    ps5_get_next_packet_size( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_frequency( void *args )
{
    struct { stream_handle stream; HRESULT result; PTR32 frequency; } *params32 = args;
    struct get_frequency_params params =
        { params32->stream, params32->result, ULongToPtr(params32->frequency) };

    ps5_get_frequency( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_position( void *args )
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 position, qpctime;
    } *params32 = args;
    struct get_position_params params =
        { params32->stream, params32->device, params32->result,
          ULongToPtr(params32->position), ULongToPtr(params32->qpctime) };

    ps5_get_position( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_set_volumes( void *args )
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes, session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
        { params32->stream, params32->master_volume,
          ULongToPtr(params32->volumes), ULongToPtr(params32->session_volumes) };

    return ps5_set_volumes( &params );
}

static NTSTATUS ps5_wow64_set_event_handle( void *args )
{
    struct { stream_handle stream; PTR32 event; HRESULT result; } *params32 = args;
    struct set_event_handle_params params =
        { params32->stream, ULongToHandle(params32->event), params32->result };

    ps5_set_event_handle( &params );
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_get_prop_value( void *args )
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid, prop;
        HRESULT result;
        PTR32 value, buffer, buffer_size;
    } *params32 = args;
    struct
    {
        WORD vt, pad1, pad2, pad3;
        union { ULONG ulVal; PTR32 ptr; ULARGE_INTEGER uhVal; };
    } *value32 = ULongToPtr(params32->value);
    PROPVARIANT value;
    struct get_prop_value_params params =
        { ULongToPtr(params32->device), params32->flow, ULongToPtr(params32->guid),
          ULongToPtr(params32->prop), params32->result, &value,
          ULongToPtr(params32->buffer), ULongToPtr(params32->buffer_size) };

    ps5_get_prop_value( &params );
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32->vt = value.vt;
        if (value.vt == VT_UI4) value32->ulVal = value.ulVal;
    }
    return STATUS_SUCCESS;
}

struct midi_message_params32
{
    UINT dev_id, msg, user, param_1, param_2;
    PTR32 err, notify;
};

static NTSTATUS ps5_wow64_midi_out_message( void *args )
{
    struct midi_message_params32 *params = args;

    *(BOOL *)ULongToPtr(params->notify) = FALSE;
    *(UINT *)ULongToPtr(params->err) = no_device( params->msg, MODM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_midi_in_message( void *args )
{
    struct midi_message_params32 *params = args;

    *(BOOL *)ULongToPtr(params->notify) = FALSE;
    *(UINT *)ULongToPtr(params->err) = no_device( params->msg, MIDM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_midi_notify_wait( void *args )
{
    struct { PTR32 quit, notify; } *params = args;

    *(BOOL *)ULongToPtr(params->quit) = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_wow64_aux_message( void *args )
{
    struct { UINT dev_id, msg, user, param_1, param_2; PTR32 err; } *params = args;

    *(UINT *)ULongToPtr(params->err) = no_device( params->msg, AUXDM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    ps5_wow64_process_attach,
    ps5_process_detach,
    ps5_wow64_main_loop,
    ps5_wow64_get_endpoint_ids,
    ps5_wow64_create_stream,
    ps5_wow64_release_stream,
    ps5_start,
    ps5_stop,
    ps5_reset,
    ps5_timer_loop,
    ps5_wow64_get_render_buffer,
    ps5_release_render_buffer,
    ps5_wow64_get_capture_buffer,
    ps5_release_capture_buffer,
    ps5_wow64_is_format_supported,
    ps5_not_implemented,
    ps5_wow64_get_mix_format,
    ps5_wow64_get_device_period,
    ps5_wow64_get_buffer_size,
    ps5_wow64_get_latency,
    ps5_wow64_get_current_padding,
    ps5_wow64_get_next_packet_size,
    ps5_wow64_get_frequency,
    ps5_wow64_get_position,
    ps5_wow64_set_volumes,
    ps5_wow64_set_event_handle,
    ps5_set_sample_rate,
    ps5_wow64_test_connect,
    ps5_is_started,
    ps5_wow64_get_prop_value,
    ps5_not_implemented,
    ps5_not_implemented,
    ps5_not_implemented,
    ps5_wow64_midi_out_message,
    ps5_wow64_midi_in_message,
    ps5_wow64_midi_notify_wait,
    ps5_wow64_aux_message,
};

C_ASSERT(ARRAY_SIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
