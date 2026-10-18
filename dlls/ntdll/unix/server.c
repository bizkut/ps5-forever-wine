/*
 * Wine server communication
 *
 * Copyright (C) 1998 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_LWP_H
#include <lwp.h>
#endif
#ifdef HAVE_PTHREAD_NP_H
# include <pthread_np.h>
#endif
#ifdef HAVE_PWD_H
# include <pwd.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_PRCTL_H
# include <sys/prctl.h>
#endif
#include <sys/stat.h>
#ifdef HAVE_SYS_SYSCALL_H
# include <sys/syscall.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_THR_H
#include <sys/thr.h>
#endif
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <spawn.h>
#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR 0x0100
#endif
#endif
#ifdef __PROSPERO__
# include <dlfcn.h>
# include <ps5platform/klog.h>
# include "wine/ps5_title.h"
extern int ps5_openat( int, const char *, int, ... );
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winioctl.h"
#include "wine/server.h"
#include "wine/debug.h"
#include "unix_private.h"
#include "ddk/wdm.h"

#include "fsync.h"

WINE_DEFAULT_DEBUG_CHANNEL(server);

/* The PS5's file systems report owners and modes that say nothing about the
 * payload's user, and the console has one user: the multi-user checks below
 * are left out there. */
#ifdef __PROSPERO__
#define OWNED_BY_ME(st) 1
#define PRIVATE_MODE(st) 1
#else
#define OWNED_BY_ME(st) ((st).st_uid == getuid())
#define PRIVATE_MODE(st) (!((st).st_mode & 077))
#endif
WINE_DECLARE_DEBUG_CHANNEL(syscall);
WINE_DECLARE_DEBUG_CHANNEL(client);
WINE_DECLARE_DEBUG_CHANNEL(ftrace);

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

#define SOCKETNAME "socket"        /* name of the socket file */
#define LOCKNAME   "lock"          /* name of the lock file */

static const char *server_dir;

unsigned int supported_machines_count = 0;
USHORT supported_machines[8] = { 0 };
USHORT native_machine = 0;
BOOL process_exiting = FALSE;

timeout_t server_start_time = 0;  /* time of server startup */

sigset_t server_block_set;  /* signals to block during server calls */
static int fd_socket = -1;  /* socket to exchange file descriptors with the server */
static int initial_cwd = -1;
static pid_t server_pid;
pthread_mutex_t fd_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef __GNUC__
static void fatal_error( const char *err, ... ) __attribute__((noreturn, format(printf,1,2)));
#else
static void fatal_error( const char *err, ... );
#endif


#ifdef __PROSPERO__
static struct wine_ps5_title_runtime title_runtime;
static void (*ps5_server_stop)(void);
#ifndef WINE_PS5_DIRECT_REQUESTS
#define WINE_PS5_DIRECT_REQUESTS 1
#endif
static int (*ps5_server_call_direct)( unsigned int, client_ptr_t, struct __server_request_info * );
static void (*ps5_server_dump_threads)( unsigned int, unsigned int );
static void (*ps5_set_hang_diagnostic)( void (*)( unsigned int, unsigned int ) );
static void (*ps5_set_thread_profile)( void (*)(void) );
static int (*ps5_profile_cpu_time)( uint64_t * );
static int (*ps5_profile_clock_id)( pthread_t, int * );
static int (*ps5_server_mapping_name)( unsigned int, client_ptr_t, obj_handle_t,
                                      char *, unsigned int, unsigned long long * );
static int (*ps5_server_wake_info)( unsigned int, client_ptr_t, unsigned int,
                                   unsigned long long *, unsigned int * );
static uint64_t title_profile_clock_lifetime;
static unsigned int title_profile_clock_logs;
int title_profile_enabled;
static pthread_key_t title_thread_key;
static BOOL title_thread_key_valid;
static struct wine_ps5_title_threads title_threads = WINE_PS5_TITLE_THREADS_INIT;
static struct wine_ps5_title_callbacks title_callbacks = WINE_PS5_TITLE_CALLBACKS_INIT;
static struct wine_ps5_title_join title_initial_join = WINE_PS5_TITLE_JOIN_INIT;
static struct wine_ps5_title_lifetimes title_lifetimes = WINE_PS5_TITLE_LIFETIMES_INIT;
static struct wine_ps5_title_startup title_startup = WINE_PS5_TITLE_STARTUP_INIT;
sigset_t ps5_title_block_set;
static char *title_c_drive, *title_w_drive;

struct ntdll_thread_data *ps5_title_profile_thread_data(void)
{
    const struct wine_ps5_title_thread *thread;

    if (!title_profile_is_enabled() || !title_thread_key_valid || !title_runtime.get_specific) return NULL;
    if (!(thread = title_runtime.get_specific( title_thread_key )) || !thread->teb) return NULL;
    return (void *)&((TEB *)thread->teb)->GdiTebBatch;
}

uint64_t ps5_title_profile_now_ns(void)
{
    struct timespec now;
    uint64_t ns = 0;
    int saved_errno = errno;

    if (!clock_gettime( CLOCK_MONOTONIC, &now ))
        ns = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    errno = saved_errno;
    return ns;
}

/* Completed calls, not samples. Slots survive thread exit so short-lived
 * startup workers are included. Only the reporting thread drains counters. */
#define TITLE_WAIT_THREADS 1024
#define TITLE_WAIT_BINS 7
#define TITLE_WAKE_BINS 32
struct title_wait_stat
{
    uint64_t count, ns, bins[TITLE_WAIT_BINS];
    uint64_t requested, overshoot, overshoot_max, timed;
};
struct title_wake_stat
{
    uint64_t count, us, max, bins[TITLE_WAKE_BINS];
};
struct title_wait_thread
{
    unsigned int tid;
    struct title_wait_stat kinds[PS5_TITLE_WAIT_KINDS];
    struct title_wake_stat wake;
    uint64_t immediate, blocked;
};
struct title_wait_slot
{
    unsigned int tid, active, writers[2], pending;
    struct title_wait_thread bank[2];
};
static struct title_wait_slot title_wait_threads[TITLE_WAIT_THREADS];
static unsigned int title_wait_count, title_wait_main_tid, title_wait_overflow;
static uint64_t title_wait_report_ns;
static unsigned int title_wake_metadata_lost;
static void title_main_capture( struct ps5_title_wait_scope *scope );
static void title_main_wait_record( struct ps5_title_wait_scope *scope, uint64_t ns );
static void title_main_wait_take( unsigned int bank );
static void title_main_wait_emit( unsigned long long ms );

BOOL ps5_title_mapping_name( HANDLE handle, char *name, unsigned int size, uint64_t *id )
{
    struct ntdll_thread_data *data = ps5_title_profile_thread_data();
    unsigned long long hash;
    sigset_t previous;
    int ret, saved_errno = errno;

    if (!data || !ps5_server_mapping_name) return FALSE;
    pthread_sigmask( SIG_BLOCK, &server_block_set, &previous );
    ret = ps5_server_mapping_name( data->title_thread.id, wine_server_client_ptr( data->title_thread.teb ),
                                   wine_server_obj_handle( handle ), name, size, &hash );
    pthread_sigmask( SIG_SETMASK, &previous, NULL );
    if (ret) *id = hash;
    errno = saved_errno;
    return ret;
}

static struct title_wait_slot *title_wait_get_thread( struct ntdll_thread_data *data )
{
    unsigned int slot;

    if (!data->wait_stats)
    {
        slot = __atomic_fetch_add( &title_wait_count, 1, __ATOMIC_RELAXED );
        if (slot >= TITLE_WAIT_THREADS)
        {
            __atomic_add_fetch( &title_wait_overflow, 1, __ATOMIC_RELAXED );
            data->wait_stats = (void *)~(ULONG_PTR)0;
        }
        else
        {
            data->wait_stats = &title_wait_threads[slot];
            __atomic_store_n( &data->wait_stats->tid, data->title_thread.id, __ATOMIC_RELEASE );
        }
    }
    return data->wait_stats == (void *)~(ULONG_PTR)0 ? NULL : data->wait_stats;
}

/* Pin a bank before updating it. A window flip cannot split a completed
 * record across count/total/histogram, even if a signal nests another wait.
 * The reporter never waits for a preempted writer. */
static struct title_wait_thread *title_wait_acquire( struct title_wait_slot *slot )
{
    unsigned int bank;

    for (;;)
    {
        bank = __atomic_load_n( &slot->active, __ATOMIC_SEQ_CST );
        __atomic_add_fetch( &slot->writers[bank], 1, __ATOMIC_SEQ_CST );
        if (bank == __atomic_load_n( &slot->active, __ATOMIC_SEQ_CST )) return &slot->bank[bank];
        __atomic_sub_fetch( &slot->writers[bank], 1, __ATOMIC_SEQ_CST );
    }
}

static void title_wait_release( struct title_wait_slot *slot, struct title_wait_thread *bank )
{
    __atomic_sub_fetch( &slot->writers[bank - slot->bank], 1, __ATOMIC_SEQ_CST );
}

static void title_wait_max( uint64_t *value, uint64_t sample )
{
    uint64_t previous = __atomic_load_n( value, __ATOMIC_RELAXED );
    while (sample > previous &&
           !__atomic_compare_exchange_n( value, &previous, sample, FALSE,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED )) {}
}

static void title_wait_record( struct ntdll_thread_data *data, enum ps5_title_wait_kind kind,
                               uint64_t ns, uint64_t requested )
{
    static const uint64_t limits[] = { 50000, 200000, 1000000, 5000000, 20000000, 100000000 };
    struct title_wait_slot *slot = title_wait_get_thread( data );
    struct title_wait_thread *thread;
    struct title_wait_stat *stat;
    unsigned int bin;

    if (!slot) return;
    thread = title_wait_acquire( slot );
    stat = &thread->kinds[kind];
    for (bin = 0; bin < ARRAY_SIZE(limits) && ns >= limits[bin]; bin++) {}
    __atomic_add_fetch( &stat->count, 1, __ATOMIC_RELAXED );
    __atomic_add_fetch( &stat->ns, ns, __ATOMIC_RELAXED );
    __atomic_add_fetch( &stat->bins[bin], 1, __ATOMIC_RELAXED );
    if (kind == PS5_TITLE_WAIT_DELAY && requested != ~(uint64_t)0)
    {
        uint64_t overshoot = ns > requested ? ns - requested : 0;
        __atomic_add_fetch( &stat->timed, 1, __ATOMIC_RELAXED );
        __atomic_add_fetch( &stat->requested, requested, __ATOMIC_RELAXED );
        __atomic_add_fetch( &stat->overshoot, overshoot, __ATOMIC_RELAXED );
        title_wait_max( &stat->overshoot_max, overshoot );
    }
    title_wait_release( slot, thread );
}

void ps5_title_wait_begin( struct ps5_title_wait_scope *scope, enum ps5_title_wait_kind kind,
                           const LARGE_INTEGER *timeout )
{
    scope->started = 0;
    scope->data = ps5_title_profile_thread_data();
    if (!scope->data) return;
    scope->previous = scope->data->wait_scope;
    scope->kind = kind;
    scope->requested = ~(uint64_t)0;
    scope->cookie = NULL;
    scope->handles = scope->waker = scope->depth = 0;
    scope->blocked = FALSE;
    if (kind == PS5_TITLE_WAIT_DELAY && scope->data->title_thread.id == title_wait_main_tid)
        title_main_capture( scope );
    if (kind == PS5_TITLE_WAIT_DELAY && timeout && timeout->QuadPart != TIMEOUT_INFINITE)
    {
        LONGLONG ticks = timeout->QuadPart;
        if (ticks > 0)
        {
            LARGE_INTEGER now;
            NtQuerySystemTime( &now );
            ticks = ticks > now.QuadPart ? ticks - now.QuadPart : 0;
        }
        else ticks = -ticks;
        scope->requested = (uint64_t)ticks > ~(uint64_t)0 / 100 ?
                           ~(uint64_t)0 : (uint64_t)ticks * 100;
    }
    scope->started = ps5_title_profile_now_ns();
    scope->data->wait_scope = scope;
}

void ps5_title_wait_end( struct ps5_title_wait_scope *scope, BOOL record )
{
    uint64_t ended;

    if (!scope->data || scope->data->wait_scope != scope) return; /* Recovered fault abandoned the scope. */
    ended = ps5_title_profile_now_ns();
    scope->data->wait_scope = scope->previous;
    /* Outer API scopes own nested select/transport waits; don't count twice. */
    if (record && !scope->previous && scope->started && ended >= scope->started)
        title_wait_record( scope->data, scope->kind, ended - scope->started, scope->requested );
    if (record && scope->started && ended >= scope->started &&
        scope->data->title_thread.id == title_wait_main_tid)
        title_main_wait_record( scope, ended - scope->started );
}

static void title_wait_wake( unsigned int token, const void *cookie )
{
    struct ntdll_thread_data *data;
    struct title_wait_slot *slot;
    struct title_wait_thread *thread;
    uint64_t now, us;
    unsigned long long sent;
    unsigned int bin = 0, waker;

    if (!token || !(data = ps5_title_profile_thread_data()) ||
        !(slot = title_wait_get_thread( data ))) return;
    now = ps5_title_profile_now_ns();
    if (!now || !ps5_server_wake_info( token, wine_server_client_ptr( cookie ), data->title_thread.id,
                                      &sent, &waker ) || now / 1000 < sent)
    {
        __atomic_add_fetch( &title_wake_metadata_lost, 1, __ATOMIC_RELAXED );
        return;
    }
    if (data->wait_scope && data->wait_scope->cookie == cookie) data->wait_scope->waker = waker;
    us = now / 1000 - sent;
    while (bin < TITLE_WAKE_BINS - 1 && us > ((uint64_t)1 << bin)) bin++;
    thread = title_wait_acquire( slot );
    __atomic_add_fetch( &thread->wake.count, 1, __ATOMIC_RELAXED );
    __atomic_add_fetch( &thread->wake.us, us, __ATOMIC_RELAXED );
    __atomic_add_fetch( &thread->wake.bins[bin], 1, __ATOMIC_RELAXED );
    title_wait_max( &thread->wake.max, us );
    title_wait_release( slot, thread );
}

static BOOL title_wait_take( struct title_wait_slot *slot, struct title_wait_thread *result )
{
    unsigned int bank;

    if (!slot->pending)
        slot->pending = __atomic_fetch_xor( &slot->active, 1, __ATOMIC_SEQ_CST ) + 1;
    bank = slot->pending - 1;
    if (__atomic_load_n( &slot->writers[bank], __ATOMIC_SEQ_CST )) return FALSE;
    if (slot->tid == title_wait_main_tid) title_main_wait_take( bank );
    *result = slot->bank[bank];
    memset( &slot->bank[bank], 0, sizeof(slot->bank[bank]) );
    result->tid = slot->tid;
    slot->pending = 0;
    return TRUE;
}

static uint64_t title_wake_percentile( const struct title_wake_stat *stat, unsigned int pct )
{
    uint64_t count = 0, target = (stat->count * pct + 99) / 100;
    unsigned int i;

    if (!target) return 0;
    for (i = 0; i < TITLE_WAKE_BINS; i++)
    {
        count += stat->bins[i];
        if (count >= target) return min( (uint64_t)1 << i, stat->max );
    }
    return stat->max;
}

static void title_wait_emit_wake( const struct title_wake_stat *stat, const char *tid,
                                  unsigned long long ms )
{
    char line[256];

    snprintf( line, sizeof(line), "title wake latency window_ms=%llu tid=%s count=%llu "
              "mean_us=%llu p50_us=%llu p90_us=%llu p99_us=%llu max_us=%llu\n",
              ms, tid, (unsigned long long)stat->count,
              (unsigned long long)(stat->count ? stat->us / stat->count : 0),
              (unsigned long long)title_wake_percentile( stat, 50 ),
              (unsigned long long)title_wake_percentile( stat, 90 ),
              (unsigned long long)title_wake_percentile( stat, 99 ),
              (unsigned long long)stat->max );
    sceKernelDebugOutText( 0, line );
}

static void title_wait_emit_thread( const struct title_wait_thread *thread, unsigned long long ms )
{
    static const char * const names[] =
    {
        "select-infinite", "select-timed", "delay", "alert-wait",
        "io-read", "io-write", "ioctl-complete", "unix-block"
    };
    char line[640];
    unsigned int i, j;
    size_t pos;

    for (i = 0; i < PS5_TITLE_WAIT_KINDS; i++)
    {
        const struct title_wait_stat *stat = &thread->kinds[i];
        if (!stat->count && thread->tid != title_wait_main_tid) continue;
        pos = snprintf( line, sizeof(line),
                        "title waits window_ms=%llu tid=%04x kind=%s count=%llu total_ms=%llu.%03llu h=",
                        ms, thread->tid, names[i], (unsigned long long)stat->count,
                        (unsigned long long)(stat->ns / 1000000),
                        (unsigned long long)(stat->ns / 1000 % 1000) );
        for (j = 0; j < TITLE_WAIT_BINS; j++)
            pos += snprintf( line + pos, sizeof(line) - pos, "%s%llu", j ? "/" : "",
                             (unsigned long long)stat->bins[j] );
        if (i == PS5_TITLE_WAIT_DELAY)
        {
            uint64_t req = stat->timed ? stat->requested / stat->timed / 1000 : 0;
            uint64_t over = stat->timed ? stat->overshoot / stat->timed / 1000 : 0;
            uint64_t max = stat->overshoot_max / 1000;
            pos += snprintf( line + pos, sizeof(line) - pos,
                             " req_ms_mean=%llu.%03llu overshoot_ms_mean=%llu.%03llu "
                             "overshoot_ms_max=%llu.%03llu timed_count=%llu",
                             (unsigned long long)(req / 1000), (unsigned long long)(req % 1000),
                             (unsigned long long)(over / 1000), (unsigned long long)(over % 1000),
                             (unsigned long long)(max / 1000), (unsigned long long)(max % 1000),
                             (unsigned long long)stat->timed );
        }
        snprintf( line + pos, sizeof(line) - pos, "\n" );
        sceKernelDebugOutText( 0, line );
    }
    if (thread->tid == title_wait_main_tid)
    {
        snprintf( line, sizeof(line), "title selects window_ms=%llu tid=%04x immediate=%llu blocked=%llu\n",
                  ms, thread->tid, (unsigned long long)thread->immediate, (unsigned long long)thread->blocked );
        sceKernelDebugOutText( 0, line );
    }
}

static void title_wait_emit_window(void)
{
    struct title_wait_thread top[3] = {{0}}, main = {0}, thread;
    struct title_wake_stat all = {0};
    uint64_t totals[3] = {0}, total, now = ps5_title_profile_now_ns();
    unsigned long long ms = now >= title_wait_report_ns ? (now - title_wait_report_ns) / 1000000 : 0;
    unsigned int count = min( __atomic_load_n( &title_wait_count, __ATOMIC_RELAXED ), TITLE_WAIT_THREADS );
    unsigned int i, j, k, overflow, deferred = 0;
    char line[128];

    title_wait_report_ns = now;
    main.tid = title_wait_main_tid;
    for (i = 0; i < count; i++)
    {
        struct title_wait_slot *src = &title_wait_threads[i];
        if (!__atomic_load_n( &src->tid, __ATOMIC_ACQUIRE )) continue;
        if (!title_wait_take( src, &thread ))
        {
            deferred++;
            continue;
        }
        all.count += thread.wake.count;
        all.us += thread.wake.us;
        all.max = max( all.max, thread.wake.max );
        for (j = 0; j < TITLE_WAKE_BINS; j++) all.bins[j] += thread.wake.bins[j];
        if (thread.tid == title_wait_main_tid) main = thread;
        else
        {
            for (total = 0, j = 0; j < PS5_TITLE_WAIT_KINDS; j++) total += thread.kinds[j].ns;
            for (j = 0; j < ARRAY_SIZE(top) && total <= totals[j]; j++) {}
            if (j == ARRAY_SIZE(top)) continue;
            for (k = ARRAY_SIZE(top) - 1; k > j; k--)
            {
                top[k] = top[k - 1];
                totals[k] = totals[k - 1];
            }
            top[j] = thread;
            totals[j] = total;
        }
    }
    if (deferred)
    {
        snprintf( line, sizeof(line), "title waits window_ms=%llu deferred_threads=%u\n", ms, deferred );
        sceKernelDebugOutText( 0, line );
    }
    title_wait_emit_thread( &main, ms );
    for (i = 0; i < ARRAY_SIZE(top); i++)
        if (top[i].tid) title_wait_emit_thread( &top[i], ms );
    title_wait_emit_wake( &main.wake, "main", ms );
    title_wait_emit_wake( &all, "all", ms );
    title_main_wait_emit( ms );
    virtual_title_maps_report( now );
    if ((overflow = __atomic_exchange_n( &title_wake_metadata_lost, 0, __ATOMIC_RELAXED )))
    {
        snprintf( line, sizeof(line), "title wake metadata window_ms=%llu unavailable=%u\n", ms, overflow );
        sceKernelDebugOutText( 0, line );
    }
    if ((overflow = __atomic_exchange_n( &title_wait_overflow, 0, __ATOMIC_RELAXED )))
    {
        snprintf( line, sizeof(line), "title waits window_ms=%llu overflow_threads=%u capacity=%u\n",
                  ms, overflow, TITLE_WAIT_THREADS );
        sceKernelDebugOutText( 0, line );
    }
}

/* QueryPerformanceCounter nanoseconds: the clock d3d12core reads, so it can
 * add a wait still in progress at its frame boundary. */
static uint64_t title_hitch_now_ns(void)
{
    LARGE_INTEGER now;
    int saved_errno = errno;

    NtQueryPerformanceCounter( &now, NULL );
    errno = saved_errno;
    return (uint64_t)now.QuadPart * 100;
}

/* RDTSCP's TSC_AUX, which FreeBSD-derived kernels program with the CPU number
 * when they use it for getcpu. Reported as-is: the spread across threads in
 * the d3d12core summary shows whether this kernel programs it. */
static uint32_t title_hitch_core(void)
{
    static int rdtscp = -1;
    unsigned int eax, ebx, ecx, edx, aux;

    if (rdtscp < 0)
    {
        __asm__ volatile( "cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000), "c"(0) );
        if (eax >= 0x80000001)
            __asm__ volatile( "cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001), "c"(0) );
        else
            edx = 0;
        rdtscp = (edx >> 27) & 1;
    }
    if (!rdtscp) return WINE_PS5_HITCH_NO_CORE;
    __asm__ volatile( "rdtscp" : "=a"(eax), "=d"(edx), "=c"(aux) );
    return aux;
}

/* Owner CPU clock (the profiler's CLOCK_THREAD_CPUTIME_ID provider). */
static int title_hitch_cpu( uint64_t *cpu )
{
    int saved_errno = errno, valid = ps5_profile_cpu_time && !ps5_profile_cpu_time( cpu );

    errno = saved_errno;
    return valid;
}

/* 20-Hz profiler tick, in the sampled thread's SIGURG handler. */
void ps5_title_hitch_profile_tick( struct ntdll_thread_data *data, BOOL cpu_valid, uint64_t cpu )
{
    if (!cpu_valid || !data->hitch.armed) return;
    wine_ps5_hitch_cpu_sample( &data->hitch, title_hitch_now_ns(), cpu, title_hitch_core() );
}

static struct wine_ps5_hitch_counters *title_hitch_main;

__attribute__((visibility("default"))) BOOL ps5_title_profile_set_blocking( BOOL blocking )
{
    const struct wine_ps5_title_thread *thread;
    struct ntdll_thread_data *data;
    BOOL previous;
    TEB *teb;

    if (!title_runtime.exit || !title_thread_key_valid || !title_runtime.get_specific) return FALSE;
    /* Only registered title threads have a usable TEB. Server calls during
     * startup run earlier, when NtCurrentTeb() is not a TEB at all (runs
     * 144/145 crashed writing at 0x57e through a TEB value of 2). */
    if (!(thread = title_runtime.get_specific( title_thread_key )) || !(teb = (TEB *)thread->teb))
        return FALSE;
    data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    /* A thread can enter a long wait before profiling starts, and nested
     * suspend/Unix calls must restore rather than clear an outer wait. */
    previous = __atomic_exchange_n( &data->profile_waiting, !!blocking, __ATOMIC_ACQ_REL );
    if (title_profile_is_enabled() && !previous != !blocking && !data->wait_scope)
    {
        uint64_t now = ps5_title_profile_now_ns();
        if (blocking) data->wait_block_started = now;
        else if (data->wait_block_started)
        {
            if (now >= data->wait_block_started)
                title_wait_record( data, PS5_TITLE_WAIT_UNIX, now - data->wait_block_started, 0 );
            data->wait_block_started = 0;
        }
    }
    /* Per-hitch attribution: only threads d3d12core armed pay for clock reads. */
    if (data->hitch.armed && !previous != !blocking && title_profile_is_enabled())
    {
        uint64_t cpu = 0;
        int cpu_valid = title_hitch_cpu( &cpu );

        wine_ps5_hitch_blocking( &data->hitch, previous, !!blocking, title_hitch_now_ns(),
                                 cpu_valid, cpu, title_hitch_core() );
    }
    return previous;
}
static unsigned int title_hang_epoch;

static void unregister_title_thread( void *entry )
{
    struct ntdll_thread_data *data = (struct ntdll_thread_data *)
        ((char *)entry - offsetof( struct ntdll_thread_data, title_thread ));
    sigset_t previous;

    /* TEBs are recycled: let hitch readers see that this owner is gone. */
    __atomic_store_n( &data->hitch.armed, 0, __ATOMIC_RELAXED );
    __atomic_store_n( &data->hitch.tid, 0, __ATOMIC_RELEASE );

    pthread_sigmask( SIG_BLOCK, &ps5_title_block_set, &previous );
    wine_ps5_title_unregister_thread( &title_threads, entry );
    pthread_sigmask( SIG_SETMASK, &previous, NULL );
}

void ps5_title_unregister_thread(void)
{
    if (!title_runtime.exit) return;
    unregister_title_thread( &ntdll_get_thread_data()->title_thread );
    title_runtime.set_specific( title_thread_key, NULL );
}

void ps5_title_request_exit( int status )
{
    sigset_t previous;
    ps5_title_stop_coalesced_writes();

    /* SIGQUIT must not abandon an active callback count before its return
     * into ntdll. After closure, late exits never enter loader text again. */
    pthread_sigmask( SIG_BLOCK, &ps5_title_block_set, &previous );
    if (wine_ps5_title_enter_callback( &title_callbacks ))
    {
        title_runtime.exit( status );
        wine_ps5_title_leave_callback( &title_callbacks );
    }
    pthread_sigmask( SIG_SETMASK, &previous, NULL );
    pthread_exit( UIntToPtr(status) );
}

/* Outside title mode the lifetime is 0 and allocation always succeeds.
 * Fails only once the title's tickets are exhausted. */
BOOL ps5_title_allocate_lifetime( uint64_t *lifetime )
{
    sigset_t previous;

    *lifetime = 0;
    if (!title_runtime.exit) return TRUE;
    pthread_sigmask( SIG_BLOCK, &ps5_title_block_set, &previous );
    *lifetime = wine_ps5_title_new_lifetime( &title_lifetimes );
    pthread_sigmask( SIG_SETMASK, &previous, NULL );
    return *lifetime != 0;
}

/* The initial client runs on a thread the title created. libkernel sets up
 * its pthread TLS lazily and identifies the caller by return PC, which must
 * be a native FSELF; ntdll is not one (2026-10-05 SIGSEGV at NULL+0x70 in
 * pthread_setspecific from virtual_alloc_first_teb). Use the title's
 * callbacks for ntdll's TEB key, as the private server does for all keys. */
int ps5_tls_key_create( pthread_key_t *key )
{
    return title_runtime.exit ? title_runtime.create_key( key, NULL ) : pthread_key_create( key, NULL );
}

void *ps5_tls_get( pthread_key_t key )
{
    return title_runtime.exit ? title_runtime.get_specific( key ) : pthread_getspecific( key );
}

int ps5_tls_set( pthread_key_t key, const void *value )
{
    return title_runtime.exit ? title_runtime.set_specific( key, value ) : pthread_setspecific( key, value );
}

int ps5_title_join_thread( struct ntdll_thread_data *data )
{
    sigset_t previous;
    int ret;

    if (!title_runtime.exit) return pthread_join( data->pthread_id, NULL );
    pthread_sigmask( SIG_BLOCK, &ps5_title_block_set, &previous );
    ret = wine_ps5_title_join_client( &data->title_thread, data->pthread_id, &title_initial_join,
                                     title_runtime.join_thread, pthread_join );
    pthread_sigmask( SIG_SETMASK, &previous, NULL );
    return ret;
}

/* Called once by the loader's startup-thread supervisor, never by a Wine
 * client or the private server which might have requested the shutdown. */
__attribute__((visibility("default"))) void __wine_ps5_shutdown_title_runtime(void)
{
    if (ps5_set_hang_diagnostic) ps5_set_hang_diagnostic( NULL );
    __atomic_store_n( &title_profile_enabled, 0, __ATOMIC_RELEASE );
    if (ps5_set_thread_profile) ps5_set_thread_profile( NULL );
    wine_ps5_title_close_callbacks( &title_callbacks );
    wine_ps5_title_cancel_startup( &title_startup );
    if (ps5_server_stop && title_runtime.server_socket < 0) ps5_server_stop();
    /* Lifetime tickets never repeat, even if the initial Windows ID, TEB,
     * and native pthread handle have all been recycled by a later worker. */
    wine_ps5_title_signal_lifetime( &title_threads, title_initial_join.lifetime, SIGQUIT );
    wine_ps5_title_join_once( &title_initial_join, title_runtime.join_thread );
    ps5_title_flush_all_coalesced_writes();
    sceKernelDebugOutText( 0, "wine: title clients quiesced; loader callbacks closed; initial thread joined\n" );
}

/* Set only by the title-mode loader, before __wine_main. No environment
 * variable or PE export can enable this callback in a payload client. */
__attribute__((visibility("default"))) void __wine_ps5_set_title_runtime( const struct wine_ps5_title_runtime *runtime )
{
    if (!runtime || runtime->version != WINE_PS5_TITLE_RUNTIME_VERSION || runtime->stdout_fd < 0)
        fatal_error( "invalid native title runtime ABI or stdout descriptor\n" );
    title_runtime = *runtime;
    title_initial_join.thread = pthread_self();
    wine_ps5_title_critical_signals( &ps5_title_block_set );
    if (!ps5_title_allocate_lifetime( &title_initial_join.lifetime )) ps5_title_request_exit( 1 );
    if (title_runtime.create_key( &title_thread_key, unregister_title_thread ))
        ps5_title_request_exit( 1 );
    title_thread_key_valid = TRUE;
}

__attribute__((visibility("default"))) int __wine_ps5_is_title(void)
{
    return !!title_runtime.exit;
}

const struct wine_ps5_title_runtime *ps5_title_get_runtime(void)
{
    return &title_runtime;
}

unsigned int ps5_title_current_thread_id(void)
{
    struct wine_ps5_title_thread *thread;

    if (!title_thread_key_valid || !title_runtime.get_specific) return 0;
    thread = title_runtime.get_specific( title_thread_key );
    return thread ? thread->id : 0;
}

__attribute__((visibility("default"))) int __wine_ps5_get_stdout_fd(void)
{
    return wine_ps5_title_stdout_fd( title_runtime.exit ? &title_runtime : NULL );
}

__attribute__((visibility("default"))) int __wine_ps5_desktop_available(void)
{
    return !!(title_runtime.exit && title_runtime.desktop_connect);
}

__attribute__((visibility("default"))) int __wine_ps5_desktop_connect(void)
{
    if (title_runtime.exit && title_runtime.desktop_connect) return title_runtime.desktop_connect();
    errno = ENOSYS;
    return -1;
}

__attribute__((visibility("default")))
int __wine_ps5_desktop_surface( const char *name, uint64_t size, int op )
{
    if (title_runtime.exit && title_runtime.desktop_surface)
        return title_runtime.desktop_surface( name, size, op );
    errno = ENOSYS;
    return -1;
}

__attribute__((visibility("default"))) void *__wine_ps5_get_vulkan_proc( const char *name )
{
    return title_runtime.vulkan_proc ? title_runtime.vulkan_proc( name ) : NULL;
}

__attribute__((visibility("default")))
int __wine_ps5_get_hdr_luminance( struct wine_ps5_hdr_luminance *luminance )
{
    if (title_runtime.exit && title_runtime.get_hdr_luminance)
        return title_runtime.get_hdr_luminance( luminance );
    errno = ENOSYS;
    return -1;
}

__attribute__((visibility("default"))) int __wine_ps5_audio_available(void)
{
    return !!(title_runtime.exit && title_runtime.audio_open &&
              title_runtime.audio_output && title_runtime.audio_close);
}

__attribute__((visibility("default")))
int __wine_ps5_audio_open( unsigned int rate, unsigned int grain )
{
    if (title_runtime.exit && title_runtime.audio_open)
        return title_runtime.audio_open( rate, grain );
    errno = ENOSYS;
    return -1;
}

__attribute__((visibility("default"))) int __wine_ps5_audio_output( const int16_t *frames )
{
    if (title_runtime.exit && title_runtime.audio_output)
    {
        BOOL previous_blocking = ps5_title_profile_set_blocking( TRUE );
        int ret = title_runtime.audio_output( frames );

        ps5_title_profile_set_blocking( previous_blocking );
        return ret;
    }
    errno = ENOSYS;
    return -1;
}

__attribute__((visibility("default"))) void __wine_ps5_audio_close(void)
{
    if (title_runtime.exit && title_runtime.audio_close) title_runtime.audio_close();
}

/* Unix graphics modules share ntdll's trusted native TLS context. Preserve
 * the key destructor; payload clients still use the original pthread API. */
__attribute__((visibility("default"))) int __wine_ps5_key_create( pthread_key_t *key, void (*destructor)( void * ))
{
    return title_runtime.exit ? title_runtime.create_key( key, destructor ) : pthread_key_create( key, destructor );
}

__attribute__((visibility("default"))) int __wine_ps5_key_delete( pthread_key_t key )
{
    return title_runtime.exit ? title_runtime.delete_key( key ) : pthread_key_delete( key );
}

__attribute__((visibility("default"))) void *__wine_ps5_get_specific( pthread_key_t key )
{
    return ps5_tls_get( key );
}

__attribute__((visibility("default"))) int __wine_ps5_set_specific( pthread_key_t key, const void *value )
{
    return ps5_tls_set( key, value );
}

const char *ps5_title_drive( char drive )
{
    if (!ps5_direct_enabled()) return NULL;
    switch (drive)
    {
    case 'c': return title_c_drive;
    case 'w': return title_w_drive;
    /* Payload Z: is the synthetic /data + /mnt storage view. An unelevated
     * title sees neither target, so do not expose a misleading sandbox Z:. */
    default: return NULL;
    }
}
#endif
/* atomically exchange a 64-bit value */
static inline LONG64 interlocked_xchg64( LONG64 *dest, LONG64 val )
{
#ifdef _WIN64
    return (LONG64)InterlockedExchangePointer( (void **)dest, (void *)val );
#else
    LONG64 tmp = *dest;
    while (InterlockedCompareExchange64( dest, val, tmp ) != tmp) tmp = *dest;
    return tmp;
#endif
}

#ifdef __GNUC__
static void fatal_perror( const char *err, ... ) __attribute__((noreturn, format(printf,1,2)));
static void server_connect_error( const char *serverdir ) __attribute__((noreturn));
#endif

/* die on a fatal error; use only during initialization */
static void fatal_error( const char *err, ... )
{
    va_list args;

    va_start( args, err );
#ifdef __PROSPERO__
    if (title_runtime.exit)
    {
        char line[1024];
        vsnprintf( line, sizeof(line), err, args );
        va_end( args );
        sceKernelDebugOutText( 0, "wine: " );
        sceKernelDebugOutText( 0, line );
        ps5_title_request_exit( 1 );
    }
#endif
    fprintf( stderr, "wine: " );
    vfprintf( stderr, err, args );
    va_end( args );
    exit(1);
}

/* die on a fatal error; use only during initialization */
static void fatal_perror( const char *err, ... )
{
    va_list args;

    va_start( args, err );
#ifdef __PROSPERO__
    if (title_runtime.exit)
    {
        char line[1024];
        int error = errno;
        vsnprintf( line, sizeof(line), err, args );
        va_end( args );
        sceKernelDebugOutText( 0, "wine: " );
        sceKernelDebugOutText( 0, line );
        sceKernelDebugOutText( 0, strerror( error ));
        ps5_title_request_exit( 1 );
    }
#endif
    fprintf( stderr, "wine: " );
    vfprintf( stderr, err, args );
    perror( " " );
    va_end( args );
    exit(1);
}

/***********************************************************************
 *           server_protocol_error
 */
static DECLSPEC_NORETURN void server_protocol_error( const char *err, ... )
{
    va_list args;

    va_start( args, err );
#ifdef __PROSPERO__
    if (title_runtime.exit)
    {
        char line[1024];
        int used = snprintf( line, sizeof(line), "wine client error:%x: ", (int)GetCurrentThreadId() );
        va_list copy;

        va_copy( copy, args );
        vsnprintf( line + used, sizeof(line) - used, err, copy );
        va_end( copy );
        sceKernelDebugOutText( 0, line );
    }
#endif
    fprintf( stderr, "wine client error:%x: ", GetCurrentThreadId() );
    vfprintf( stderr, err, args );
    va_end( args );
    abort_thread(1);
}


/***********************************************************************
 *           server_protocol_perror
 */
static DECLSPEC_NORETURN void server_protocol_perror( const char *err )
{
#ifdef __PROSPERO__
    if (title_runtime.exit)
    {
        char line[256];
        int error = errno;

        snprintf( line, sizeof(line), "wine client error:%x: %s: %s\n", (int)GetCurrentThreadId(),
                  err, strerror( error ));
        sceKernelDebugOutText( 0, line );
        errno = error;
    }
#endif
    fprintf( stderr, "wine client error:%x: ", GetCurrentThreadId() );
    perror( err );
    abort_thread(1);
}


/***********************************************************************
 *           send_request
 *
 * Send a request to the server.
 */
static unsigned int send_request( const struct __server_request_info *req )
{
    int request_fd = ntdll_get_thread_data()->request_fd;

    if (!req->u.req.request_header.request_size)
    {
        data_size_t to_write = sizeof(req->u.req);
        const char *write_ptr = (const char *)&req->u.req;

        for (;;)
        {
            BOOL previous_blocking = ps5_title_profile_set_blocking( TRUE );
            ssize_t ret = write( request_fd, write_ptr, to_write );

            ps5_title_profile_set_blocking( previous_blocking );
            if (ret == to_write) return STATUS_SUCCESS;
            if (ret < 0) break;
            to_write -= ret;
            write_ptr += ret;
        }
    }
    else
    {
        data_size_t to_write = sizeof(req->u.req) + req->u.req.request_header.request_size;
        struct iovec vec[__SERVER_MAX_DATA+1];
        unsigned int i, j;

        vec[0].iov_base = (void *)&req->u.req;
        vec[0].iov_len = sizeof(req->u.req);
        for (i = 0; i < req->data_count; i++)
        {
            vec[i+1].iov_base = (void *)req->data[i].ptr;
            vec[i+1].iov_len = req->data[i].size;
        }

        for (;;)
        {
            BOOL previous_blocking = ps5_title_profile_set_blocking( TRUE );
            ssize_t ret = writev( request_fd, vec, i + 1 );

            ps5_title_profile_set_blocking( previous_blocking );
            if (ret == to_write) return STATUS_SUCCESS;
            if (ret < 0) break;
            to_write -= ret;
            for (j = 0; j < i + 1; j++)
            {
                if (ret >= vec[j].iov_len)
                {
                    ret -= vec[j].iov_len;
                    vec[j].iov_len = 0;
                }
                else
                {
                    vec[j].iov_base = (char *)vec[j].iov_base + ret;
                    vec[j].iov_len -= ret;
                    break;
                }
            }
        }
    }

    if (errno == EPIPE) abort_thread(0);
    if (errno == EFAULT) return STATUS_ACCESS_VIOLATION;
    server_protocol_perror( "write" );
}


/***********************************************************************
 *           read_reply_data
 *
 * Read data from the reply buffer; helper for wait_reply.
 */
static void read_reply_data( void *buffer, size_t size )
{
    int ret;

    for (;;)
    {
        BOOL previous_blocking = ps5_title_profile_set_blocking( TRUE );

        ret = read( ntdll_get_thread_data()->reply_fd, buffer, size );
        ps5_title_profile_set_blocking( previous_blocking );
        if (ret > 0)
        {
            if (!(size -= ret)) return;
            buffer = (char *)buffer + ret;
            continue;
        }
        if (!ret) break;
        if (errno == EINTR) continue;
        if (errno == EPIPE) break;
        server_protocol_perror("read");
    }
    /* the server closed the connection; time to die... */
    abort_thread(0);
}


/***********************************************************************
 *           wait_reply
 *
 * Wait for a reply from the server.
 */
static inline unsigned int wait_reply( struct __server_request_info *req )
{
    read_reply_data( &req->u.reply, sizeof(req->u.reply) );
    if (req->u.reply.reply_header.reply_size)
        read_reply_data( req->reply_data, req->u.reply.reply_header.reply_size );
    return req->u.reply.reply_header.error;
}


/***********************************************************************
 *           server_call_unlocked
 */
unsigned int server_call_unlocked( void *req_ptr )
{
    struct __server_request_info * const req = req_ptr;
    unsigned int ret;
#ifdef __PROSPERO__
    struct ntdll_thread_data *profile_data = ntdll_get_thread_data();
    const char *previous_profile_request = NULL;
    unsigned int previous_profile_request_id = 0;
    BOOL profile_active = title_profile_is_enabled();


    if (profile_active)
    {
        previous_profile_request = profile_data->profile_request;
        previous_profile_request_id = profile_data->profile_request_id;
        __atomic_store_n( &profile_data->profile_request_id,
                          req->u.req.request_header.req, __ATOMIC_RELAXED );
        __atomic_store_n( &profile_data->profile_request, req->name, __ATOMIC_RELEASE );
    }
    if (WINE_PS5_DIRECT_REQUESTS && __wine_ps5_is_title() && ps5_server_call_direct)
    {
        void *teb = NtCurrentTeb64();
        if (!teb) teb = NtCurrentTeb();
        switch (ps5_server_call_direct( GetCurrentThreadId(), wine_server_client_ptr( teb ), req ))
        {
        case 0:
            ret = req->u.reply.reply_header.error;
            goto done;
        case 2:
            abort_thread( 0 );
        }
    }
#endif

    FTRACE_BLOCK_START("req %s", req->name)
    TRACE_(client)("%s start\n", req->name); \
    if (!(ret = send_request( req )))
        ret = wait_reply( req );
    TRACE_(client)("%s end\n", req->name);
    FTRACE_BLOCK_END()
#ifdef __PROSPERO__
done:
    if (profile_active)
    {
        __atomic_store_n( &profile_data->profile_request_id,
                          previous_profile_request_id, __ATOMIC_RELAXED );
        __atomic_store_n( &profile_data->profile_request,
                          previous_profile_request, __ATOMIC_RELEASE );
    }
#endif
    return ret;
}


/***********************************************************************
 *           wine_server_call
 *
 * Perform a server call.
 */
unsigned int CDECL wine_server_call( void *req_ptr )
{
    sigset_t old_set;
    unsigned int ret;

    pthread_sigmask( SIG_BLOCK, &server_block_set, &old_set );
    ret = server_call_unlocked( req_ptr );
    pthread_sigmask( SIG_SETMASK, &old_set, NULL );
    return ret;
}


/***********************************************************************
 *           unixcall_wine_server_call
 *
 * Perform a server call.
 */
NTSTATUS unixcall_wine_server_call( void *args )
{
    return wine_server_call( args );
}


/***********************************************************************
 *           server_enter_uninterrupted_section
 */
void server_enter_uninterrupted_section( pthread_mutex_t *mutex, sigset_t *sigset )
{
    pthread_sigmask( SIG_BLOCK, &server_block_set, sigset );
#ifdef __PROSPERO__
    if (WINE_PS5_DIRECT_REQUESTS && __wine_ps5_is_title() && ps5_server_call_direct)
    {
        unsigned int i;
        for (i = 0; i < 200; i++)
        {
            if (!pthread_mutex_trylock( mutex )) return;
#if defined(__i386__) || defined(__x86_64__)
            __asm__ __volatile__( "pause" );
#endif
        }
    }
#endif
    {
        BOOL previous_blocking = ps5_title_profile_set_blocking( TRUE );

        mutex_lock( mutex );
        ps5_title_profile_set_blocking( previous_blocking );
    }
}


/***********************************************************************
 *           server_leave_uninterrupted_section
 */
void server_leave_uninterrupted_section( pthread_mutex_t *mutex, sigset_t *sigset )
{
    mutex_unlock( mutex );
    pthread_sigmask( SIG_SETMASK, sigset, NULL );
}


/***********************************************************************
 *              wait_select_reply
 *
 * Wait for a reply on the waiting pipe of the current thread.
 */
static int wait_select_reply( void *cookie )
{
    int signaled;
    struct wake_up_reply reply;
    for (;;)
    {
        int ret;
        BOOL previous_blocking = ps5_title_profile_set_blocking( TRUE );

        ret = read( ntdll_get_thread_data()->wait_fd[0], &reply, sizeof(reply) );
#ifdef __PROSPERO__
        if (title_profile_is_enabled() && ret == sizeof(reply) &&
            wine_server_get_ptr(reply.cookie) == cookie)
            title_wait_wake( reply.__pad, cookie );
#endif
        ps5_title_profile_set_blocking( previous_blocking );
        if (ret == sizeof(reply))
        {
            if (!reply.cookie) abort_thread( reply.signaled );  /* thread got killed */
            if (wine_server_get_ptr(reply.cookie) == cookie)
            {
                return reply.signaled;
            }
            /* we stole another reply, wait for the real one */
            signaled = wait_select_reply( cookie );
            /* and now put the wrong one back in the pipe */
            for (;;)
            {
                BOOL previous_blocking = ps5_title_profile_set_blocking( TRUE );

                ret = write( ntdll_get_thread_data()->wait_fd[1], &reply, sizeof(reply) );
                ps5_title_profile_set_blocking( previous_blocking );
                if (ret == sizeof(reply)) break;
                if (ret >= 0) server_protocol_error( "partial wakeup write %d\n", ret );
                if (errno == EINTR) continue;
                server_protocol_perror("wakeup write");
            }
            return signaled;
        }
        if (ret >= 0) server_protocol_error( "partial wakeup read %d\n", ret );
        if (errno == EINTR) continue;
        server_protocol_perror("wakeup read");
    }
}


/***********************************************************************
 *              invoke_user_apc
 */
static NTSTATUS invoke_user_apc( CONTEXT *context, const struct user_apc *apc, NTSTATUS status )
{
    return call_user_apc_dispatcher( context, apc->flags, apc->args[0], apc->args[1], apc->args[2],
                                     wine_server_get_ptr( apc->func ), status );
}


/***********************************************************************
 *              invoke_system_apc
 */
static void invoke_system_apc( const union apc_call *call, union apc_result *result, BOOL self )
{
    SIZE_T size, bits;
    void *addr;

    memset( result, 0, sizeof(*result) );

    switch (call->type)
    {
    case APC_NONE:
        break;
    case APC_ASYNC_IO:
    {
        struct async_fileio *user = wine_server_get_ptr( call->async_io.user );
        ULONG_PTR info = call->async_io.result;
        unsigned int status;

        result->type = call->type;
        status = call->async_io.status;
        if (user->callback( user, &info, &status ))
        {
            result->async_io.status = status;
            result->async_io.total = info;
            /* the server will pass us NULL if a call failed synchronously */
            set_async_iosb( call->async_io.sb, result->async_io.status, info );
        }
        else result->async_io.status = STATUS_PENDING; /* restart it */
        break;
    }
    case APC_VIRTUAL_ALLOC:
        result->type = call->type;
        addr = wine_server_get_ptr( call->virtual_alloc.addr );
        size = call->virtual_alloc.size;
        bits = call->virtual_alloc.zero_bits;
        if ((ULONG_PTR)addr == call->virtual_alloc.addr && size == call->virtual_alloc.size &&
            bits == call->virtual_alloc.zero_bits)
        {
            result->virtual_alloc.status = NtAllocateVirtualMemory( NtCurrentProcess(), &addr, bits, &size,
                                                                    call->virtual_alloc.op_type,
                                                                    call->virtual_alloc.prot );
            result->virtual_alloc.addr = wine_server_client_ptr( addr );
            result->virtual_alloc.size = size;
        }
        else result->virtual_alloc.status = STATUS_WORKING_SET_LIMIT_RANGE;
        break;
    case APC_VIRTUAL_ALLOC_EX:
    {
        MEM_ADDRESS_REQUIREMENTS r;
        MEM_EXTENDED_PARAMETER ext[2];
        ULONG count = 0;

        result->type = call->type;
        addr = wine_server_get_ptr( call->virtual_alloc_ex.addr );
        size = call->virtual_alloc_ex.size;
        if ((ULONG_PTR)addr != call->virtual_alloc_ex.addr || size != call->virtual_alloc_ex.size)
        {
            result->virtual_alloc_ex.status = STATUS_WORKING_SET_LIMIT_RANGE;
            break;
        }
        if (call->virtual_alloc_ex.limit_low || call->virtual_alloc_ex.limit_high || call->virtual_alloc_ex.align)
        {
            SYSTEM_BASIC_INFORMATION sbi;
            SIZE_T limit_low, limit_high, align;

            virtual_get_system_info( &sbi, is_wow64() );
            limit_low = call->virtual_alloc_ex.limit_low;
            limit_high = min( (ULONG_PTR)sbi.HighestUserAddress, call->virtual_alloc_ex.limit_high );
            align = call->virtual_alloc_ex.align;
            if (limit_low != call->virtual_alloc_ex.limit_low || align != call->virtual_alloc_ex.align)
            {
                result->virtual_alloc_ex.status = STATUS_WORKING_SET_LIMIT_RANGE;
                break;
            }
            r.LowestStartingAddress = (void *)limit_low;
            r.HighestEndingAddress = (void *)limit_high;
            r.Alignment = align;
            ext[count].Type = MemExtendedParameterAddressRequirements;
            ext[count].Pointer = &r;
            count++;
        }
        if (call->virtual_alloc_ex.attributes)
        {
            ext[count].Type = MemExtendedParameterAttributeFlags;
            ext[count].ULong64 = call->virtual_alloc_ex.attributes;
            count++;
        }
        result->virtual_alloc_ex.status = NtAllocateVirtualMemoryEx( NtCurrentProcess(), &addr, &size,
                                                                     call->virtual_alloc_ex.op_type,
                                                                     call->virtual_alloc_ex.prot,
                                                                     ext, count );
        result->virtual_alloc_ex.addr = wine_server_client_ptr( addr );
        result->virtual_alloc_ex.size = size;
        break;
    }
    case APC_VIRTUAL_FREE:
        result->type = call->type;
        addr = wine_server_get_ptr( call->virtual_free.addr );
        size = call->virtual_free.size;
        if ((ULONG_PTR)addr == call->virtual_free.addr && size == call->virtual_free.size)
        {
            result->virtual_free.status = NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size,
                                                               call->virtual_free.op_type );
            result->virtual_free.addr = wine_server_client_ptr( addr );
            result->virtual_free.size = size;
        }
        else result->virtual_free.status = STATUS_INVALID_PARAMETER;
        break;
    case APC_VIRTUAL_QUERY:
    {
        MEMORY_BASIC_INFORMATION info;
        result->type = call->type;
        addr = wine_server_get_ptr( call->virtual_query.addr );
        if ((ULONG_PTR)addr == call->virtual_query.addr)
            result->virtual_query.status = NtQueryVirtualMemory( NtCurrentProcess(),
                                                                 addr, MemoryBasicInformation, &info,
                                                                 sizeof(info), NULL );
        else
            result->virtual_query.status = STATUS_WORKING_SET_LIMIT_RANGE;

        if (result->virtual_query.status == STATUS_SUCCESS)
        {
            result->virtual_query.base       = wine_server_client_ptr( info.BaseAddress );
            result->virtual_query.alloc_base = wine_server_client_ptr( info.AllocationBase );
            result->virtual_query.size       = info.RegionSize;
            result->virtual_query.prot       = info.Protect;
            result->virtual_query.alloc_prot = info.AllocationProtect;
            result->virtual_query.state      = info.State >> 12;
            result->virtual_query.alloc_type = info.Type >> 16;
        }
        break;
    }
    case APC_VIRTUAL_PROTECT:
        result->type = call->type;
        addr = wine_server_get_ptr( call->virtual_protect.addr );
        size = call->virtual_protect.size;
        if ((ULONG_PTR)addr == call->virtual_protect.addr && size == call->virtual_protect.size)
        {
            ULONG prot;
            result->virtual_protect.status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size,
                                                                     call->virtual_protect.prot, &prot );
            result->virtual_protect.addr = wine_server_client_ptr( addr );
            result->virtual_protect.size = size;
            result->virtual_protect.prot = prot;
        }
        else result->virtual_protect.status = STATUS_INVALID_PARAMETER;
        break;
    case APC_VIRTUAL_FLUSH:
        result->type = call->type;
        addr = wine_server_get_ptr( call->virtual_flush.addr );
        size = call->virtual_flush.size;
        if ((ULONG_PTR)addr == call->virtual_flush.addr && size == call->virtual_flush.size)
        {
            result->virtual_flush.status = NtFlushVirtualMemory( NtCurrentProcess(),
                                                                 (const void **)&addr, &size, 0 );
            result->virtual_flush.addr = wine_server_client_ptr( addr );
            result->virtual_flush.size = size;
        }
        else result->virtual_flush.status = STATUS_INVALID_PARAMETER;
        break;
    case APC_VIRTUAL_LOCK:
        result->type = call->type;
        addr = wine_server_get_ptr( call->virtual_lock.addr );
        size = call->virtual_lock.size;
        if ((ULONG_PTR)addr == call->virtual_lock.addr && size == call->virtual_lock.size)
        {
            result->virtual_lock.status = NtLockVirtualMemory( NtCurrentProcess(), &addr, &size, 0 );
            result->virtual_lock.addr = wine_server_client_ptr( addr );
            result->virtual_lock.size = size;
        }
        else result->virtual_lock.status = STATUS_INVALID_PARAMETER;
        break;
    case APC_VIRTUAL_UNLOCK:
        result->type = call->type;
        addr = wine_server_get_ptr( call->virtual_unlock.addr );
        size = call->virtual_unlock.size;
        if ((ULONG_PTR)addr == call->virtual_unlock.addr && size == call->virtual_unlock.size)
        {
            result->virtual_unlock.status = NtUnlockVirtualMemory( NtCurrentProcess(), &addr, &size, 0 );
            result->virtual_unlock.addr = wine_server_client_ptr( addr );
            result->virtual_unlock.size = size;
        }
        else result->virtual_unlock.status = STATUS_INVALID_PARAMETER;
        break;
    case APC_MAP_VIEW:
        result->type = call->type;
        addr = wine_server_get_ptr( call->map_view.addr );
        size = call->map_view.size;
        bits = call->map_view.zero_bits;
        if ((ULONG_PTR)addr == call->map_view.addr && size == call->map_view.size &&
            bits == call->map_view.zero_bits)
        {
            LARGE_INTEGER offset;
            offset.QuadPart = call->map_view.offset;
            result->map_view.status = NtMapViewOfSection( wine_server_ptr_handle(call->map_view.handle),
                                                          NtCurrentProcess(),
                                                          &addr, bits, 0, &offset, &size, 0,
                                                          call->map_view.alloc_type, call->map_view.prot );
            result->map_view.addr = wine_server_client_ptr( addr );
            result->map_view.size = size;
        }
        else result->map_view.status = STATUS_INVALID_PARAMETER;
        if (!self) NtClose( wine_server_ptr_handle(call->map_view.handle) );
        break;
    case APC_MAP_VIEW_EX:
    {
        MEM_ADDRESS_REQUIREMENTS addr_req;
        MEM_EXTENDED_PARAMETER ext[2];
        ULONG count = 0;
        LARGE_INTEGER offset;
        ULONG_PTR limit_low, limit_high;

        result->type = call->type;
        addr = wine_server_get_ptr( call->map_view_ex.addr );
        size = call->map_view_ex.size;
        offset.QuadPart = call->map_view_ex.offset;
        limit_low = call->map_view_ex.limit_low;
        if ((ULONG_PTR)addr != call->map_view_ex.addr || size != call->map_view_ex.size ||
            limit_low != call->map_view_ex.limit_low)
        {
            result->map_view_ex.status = STATUS_WORKING_SET_LIMIT_RANGE;
            break;
        }
        if (call->map_view_ex.limit_low || call->map_view_ex.limit_high)
        {
            SYSTEM_BASIC_INFORMATION sbi;

            virtual_get_system_info( &sbi, is_wow64() );
            limit_high = min( (ULONG_PTR)sbi.HighestUserAddress, call->map_view_ex.limit_high );
            addr_req.LowestStartingAddress = (void *)limit_low;
            addr_req.HighestEndingAddress = (void *)limit_high;
            addr_req.Alignment = 0;
            ext[count].Type = MemExtendedParameterAddressRequirements;
            ext[count].Pointer = &addr_req;
            count++;
        }
        if (call->map_view_ex.machine)
        {
            ext[count].Type = MemExtendedParameterImageMachine;
            ext[count].ULong = call->map_view_ex.machine;
            count++;
        }
        result->map_view_ex.status = NtMapViewOfSectionEx( wine_server_ptr_handle(call->map_view_ex.handle),
                                                           NtCurrentProcess(), &addr, &offset, &size,
                                                           call->map_view_ex.alloc_type,
                                                           call->map_view_ex.prot, ext, count );
        result->map_view_ex.addr = wine_server_client_ptr( addr );
        result->map_view_ex.size = size;
        if (!self) NtClose( wine_server_ptr_handle(call->map_view_ex.handle) );
        break;
    }
    case APC_UNMAP_VIEW:
        result->type = call->type;
        addr = wine_server_get_ptr( call->unmap_view.addr );
        if ((ULONG_PTR)addr == call->unmap_view.addr)
            result->unmap_view.status = NtUnmapViewOfSectionEx( NtCurrentProcess(), addr, call->unmap_view.flags );
        else
            result->unmap_view.status = STATUS_INVALID_PARAMETER;
        break;
    case APC_CREATE_THREAD:
    {
        ULONG_PTR buffer[offsetof( PS_ATTRIBUTE_LIST, Attributes[2] ) / sizeof(ULONG_PTR)];
        PS_ATTRIBUTE_LIST *attr = (PS_ATTRIBUTE_LIST *)buffer;
        CLIENT_ID id;
        HANDLE handle;
        TEB *teb;
        ULONG_PTR zero_bits = call->create_thread.zero_bits;
        SIZE_T reserve = call->create_thread.reserve;
        SIZE_T commit = call->create_thread.commit;
        void *func = wine_server_get_ptr( call->create_thread.func );
        void *arg  = wine_server_get_ptr( call->create_thread.arg );

        result->type = call->type;
        if (reserve == call->create_thread.reserve && commit == call->create_thread.commit &&
            (ULONG_PTR)func == call->create_thread.func && (ULONG_PTR)arg == call->create_thread.arg)
        {
            /* FIXME: hack for debugging 32-bit process without a 64-bit ntdll */
            if (is_old_wow64() && func == (void *)0x7ffe1000) func = pDbgUiRemoteBreakin;
            attr->TotalLength = sizeof(buffer);
            attr->Attributes[0].Attribute    = PS_ATTRIBUTE_CLIENT_ID;
            attr->Attributes[0].Size         = sizeof(id);
            attr->Attributes[0].ValuePtr     = &id;
            attr->Attributes[0].ReturnLength = NULL;
            attr->Attributes[1].Attribute    = PS_ATTRIBUTE_TEB_ADDRESS;
            attr->Attributes[1].Size         = sizeof(teb);
            attr->Attributes[1].ValuePtr     = &teb;
            attr->Attributes[1].ReturnLength = NULL;
            result->create_thread.status = NtCreateThreadEx( &handle, THREAD_ALL_ACCESS, NULL,
                                                             NtCurrentProcess(), func, arg,
                                                             call->create_thread.flags, zero_bits,
                                                             commit, reserve, attr );
            result->create_thread.handle = wine_server_obj_handle( handle );
            result->create_thread.pid = HandleToULong(id.UniqueProcess);
            result->create_thread.tid = HandleToULong(id.UniqueThread);
            result->create_thread.teb = wine_server_client_ptr( teb );
        }
        else result->create_thread.status = STATUS_INVALID_PARAMETER;
        break;
    }
    case APC_DUP_HANDLE:
    {
        HANDLE dst_handle = NULL;

        result->type = call->type;

        result->dup_handle.status = NtDuplicateObject( NtCurrentProcess(),
                                                       wine_server_ptr_handle(call->dup_handle.src_handle),
                                                       wine_server_ptr_handle(call->dup_handle.dst_process),
                                                       &dst_handle, call->dup_handle.access,
                                                       call->dup_handle.attributes, call->dup_handle.options );
        result->dup_handle.handle = wine_server_obj_handle( dst_handle );
        if (!self) NtClose( wine_server_ptr_handle(call->dup_handle.dst_process) );
        break;
    }
    default:
        server_protocol_error( "get_apc_request: bad type %d\n", call->type );
        break;
    }
}


/***********************************************************************
 *              server_select
 */
unsigned int server_select( const union select_op *select_op, data_size_t size, UINT flags,
                            timeout_t abs_timeout, struct context_data *context, struct user_apc *user_apc )
{
    unsigned int ret;
    int cookie;
    obj_handle_t apc_handle = 0;
    BOOL suspend_context = !!context;
    union apc_result result;
    sigset_t old_set;
    int signaled;
    data_size_t reply_size;
    struct
    {
        union apc_call call;
        struct context_data context[2];
    } reply_data;
#ifdef __PROSPERO__
    struct ps5_title_wait_scope wait_scope;

    wait_scope.data = NULL;
    if (title_profile_is_enabled())
    {
        ps5_title_wait_begin( &wait_scope, abs_timeout == TIMEOUT_INFINITE ?
                             PS5_TITLE_WAIT_SELECT : PS5_TITLE_WAIT_SELECT_TIMED, NULL );
        if (wait_scope.data)
        {
            wait_scope.cookie = &cookie;
            if (size && select_op->op == SELECT_SIGNAL_AND_WAIT) wait_scope.handles = 1;
            else if (size >= offsetof( union select_op, wait.handles ))
                wait_scope.handles = (size - offsetof( union select_op, wait.handles )) / sizeof(obj_handle_t);
        }
    }
#endif

    memset( &result, 0, sizeof(result) );

    do
    {
        pthread_sigmask( SIG_BLOCK, &server_block_set, &old_set );
        for (;;)
        {
            SERVER_START_REQ( select )
            {
                req->flags    = flags;
                req->cookie   = wine_server_client_ptr( &cookie );
                req->prev_apc = apc_handle;
                req->timeout  = abs_timeout;
                req->size     = size;
                wine_server_add_data( req, &result, sizeof(result) );
                wine_server_add_data( req, select_op, size );
                if (suspend_context)
                {
                    data_size_t ctx_size = (context[1].machine ? 2 : 1) * sizeof(*context);
                    wine_server_add_data( req, context, ctx_size );
                    suspend_context = FALSE; /* server owns the context now */
                }
                wine_server_set_reply( req, &reply_data,
                                       context ? sizeof(reply_data) : sizeof(reply_data.call) );
                ret = server_call_unlocked( req );
                signaled    = reply->signaled;
                apc_handle  = reply->apc_handle;
                reply_size  = wine_server_reply_size( reply );
            }
            SERVER_END_REQ;
#ifdef __PROSPERO__
            if (wait_scope.data)
            {
                struct title_wait_slot *slot = title_wait_get_thread( wait_scope.data );
                if (slot)
                {
                    struct title_wait_thread *thread = title_wait_acquire( slot );
                    __atomic_add_fetch( signaled ? &thread->immediate : &thread->blocked,
                                        1, __ATOMIC_RELAXED );
                    title_wait_release( slot, thread );
                }
            }
#endif

            if (ret != STATUS_KERNEL_APC) break;
            invoke_system_apc( &reply_data.call, &result, FALSE );

            /* don't signal multiple times */
            if (size >= sizeof(select_op->signal_and_wait) && select_op->op == SELECT_SIGNAL_AND_WAIT)
                size = offsetof( union select_op, signal_and_wait.signal );
        }
        pthread_sigmask( SIG_SETMASK, &old_set, NULL );
        if (signaled) break;
#ifdef __PROSPERO__
        if (wait_scope.data)
        {
            wait_scope.blocked = TRUE;
            wait_scope.waker = 0;
        }
#endif

        FTRACE_BLOCK_START("select_reply")
        ret = wait_select_reply( &cookie );
        FTRACE_BLOCK_END()
    }
    while (ret == STATUS_USER_APC || ret == STATUS_KERNEL_APC);

    if (ret == STATUS_USER_APC) *user_apc = reply_data.call.user;
    if (reply_size > sizeof(reply_data.call))
    {
        memcpy( context, reply_data.context, reply_size - sizeof(reply_data.call) );
        context[0].flags &= ~SERVER_CTX_EXEC_SPACE;
        context[1].flags &= ~SERVER_CTX_EXEC_SPACE;
    }
#ifdef __PROSPERO__
    if (wait_scope.data) ps5_title_wait_end( &wait_scope, !!size );
#endif
    return ret;
}


/***********************************************************************
 *              server_wait
 */
unsigned int server_wait( const union select_op *select_op, data_size_t size, UINT flags,
                          const LARGE_INTEGER *timeout )
{
    timeout_t abs_timeout = timeout ? timeout->QuadPart : TIMEOUT_INFINITE;
    unsigned int ret;
    struct user_apc apc;

#ifdef __PROSPERO__
    if (__wine_ps5_is_title()) virtual_restore_title_mutex_depth( 0, "server_wait" );
#endif

    if (abs_timeout < 0)
    {
        LARGE_INTEGER now;

        NtQueryPerformanceCounter( &now, NULL );
        abs_timeout -= now.QuadPart;
    }

    ret = server_select( select_op, size, flags, abs_timeout, NULL, &apc );
    if (ret == STATUS_USER_APC) return invoke_user_apc( NULL, &apc, ret );

    /* A test on Windows 2000 shows that Windows always yields during
       a wait, but a wait that is hit by an event gets a priority
       boost as well.  This seems to model that behavior the closest.  */
    if (ret == STATUS_TIMEOUT) NtYieldExecution();
    return ret;
}


/* helper function to perform a server-side wait on an internal handle without
 * using the fast synchronization path */
unsigned int server_wait_for_object( HANDLE handle, BOOL alertable, const LARGE_INTEGER *timeout )
{
    union select_op select_op;
    UINT flags = SELECT_INTERRUPTIBLE;

    if (alertable) flags |= SELECT_ALERTABLE;

    select_op.wait.op = SELECT_WAIT;
    select_op.wait.handles[0] = wine_server_obj_handle( handle );
    return server_wait( &select_op, offsetof( union select_op, wait.handles[1] ), flags, timeout );
}


/***********************************************************************
 *              NtContinue  (NTDLL.@)
 */
NTSTATUS WINAPI NtContinue( CONTEXT *context, BOOLEAN alertable )
{
    return NtContinueEx( context, ULongToPtr(alertable) );
}


/***********************************************************************
 *              NtContinueEx  (NTDLL.@)
 */
NTSTATUS WINAPI NtContinueEx( CONTEXT *context, KCONTINUE_ARGUMENT *args )
{
    struct user_apc apc;
    NTSTATUS status;
    BOOL alertable;

    if ((UINT_PTR)args > 0xff)
        alertable = args->ContinueFlags & KCONTINUE_FLAG_TEST_ALERT;
    else
        alertable = !!args;

    if (alertable)
    {
        status = server_select( NULL, 0, SELECT_INTERRUPTIBLE | SELECT_ALERTABLE, 0, NULL, &apc );
        if (status == STATUS_USER_APC) return invoke_user_apc( context, &apc, status );
    }
    return signal_set_full_context( context );
}


/***********************************************************************
 *              NtTestAlert  (NTDLL.@)
 */
NTSTATUS WINAPI NtTestAlert(void)
{
    struct user_apc apc;
    NTSTATUS status;

    status = server_select( NULL, 0, SELECT_INTERRUPTIBLE | SELECT_ALERTABLE, 0, NULL, &apc );
    if (status == STATUS_USER_APC) invoke_user_apc( NULL, &apc, STATUS_SUCCESS );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           server_queue_process_apc
 */
unsigned int server_queue_process_apc( HANDLE process, const union apc_call *call, union apc_result *result )
{
    for (;;)
    {
        unsigned int ret;
        HANDLE handle = 0;
        BOOL self = FALSE;

        SERVER_START_REQ( queue_apc )
        {
            req->handle = wine_server_obj_handle( process );
            wine_server_add_data( req, call, sizeof(*call) );
            if (!(ret = wine_server_call( req )))
            {
                handle = wine_server_ptr_handle( reply->handle );
                self = reply->self;
            }
        }
        SERVER_END_REQ;
        if (ret != STATUS_SUCCESS) return ret;

        if (self)
        {
            invoke_system_apc( call, result, TRUE );
        }
        else
        {
            sigset_t sigset;

            NtWaitForSingleObject( handle, FALSE, NULL );

            server_enter_uninterrupted_section( &fd_cache_mutex, &sigset );

            /* remove the handle from the cache, get_apc_result will close it for us */
            close_inproc_sync( handle );

            SERVER_START_REQ( get_apc_result )
            {
                req->handle = wine_server_obj_handle( handle );
                if (!(ret = server_call_unlocked( req ))) *result = reply->result;
            }
            SERVER_END_REQ;

            server_leave_uninterrupted_section( &fd_cache_mutex, &sigset );

            if (!ret && result->type == APC_NONE) continue;  /* APC didn't run, try again */
        }
        return ret;
    }
}


/***********************************************************************
 *           wine_server_send_fd
 *
 * Send a file descriptor to the server.
 */
void CDECL wine_server_send_fd( int fd )
{
    struct send_fd data;
    struct msghdr msghdr;
    struct iovec vec;
    char cmsg_buffer[256];
    struct cmsghdr *cmsg;
    int ret;

    msghdr.msg_name    = NULL;
    msghdr.msg_namelen = 0;
    msghdr.msg_iov     = &vec;
    msghdr.msg_iovlen  = 1;
    msghdr.msg_control = cmsg_buffer;
    msghdr.msg_controllen = sizeof(cmsg_buffer);
    msghdr.msg_flags   = 0;

    vec.iov_base = (void *)&data;
    vec.iov_len  = sizeof(data);

    data.tid = GetCurrentThreadId();
    data.fd  = fd;

    cmsg = CMSG_FIRSTHDR( &msghdr );
    cmsg->cmsg_len   = CMSG_LEN( sizeof(fd) );
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    *(int *)CMSG_DATA(cmsg) = fd;
    msghdr.msg_controllen = cmsg->cmsg_len;

    for (;;)
    {
        if ((ret = sendmsg( fd_socket, &msghdr, 0 )) == sizeof(data)) return;
        if (ret >= 0) server_protocol_error( "partial write %d\n", ret );
        if (errno == EINTR) continue;
        if (errno == EPIPE) abort_thread(0);
        server_protocol_perror( "sendmsg" );
    }
}


/***********************************************************************
 *           receive_fd
 *
 * Receive a file descriptor passed from the server.
 */
int wine_server_receive_fd( obj_handle_t *handle )
{
    struct iovec vec;
    struct msghdr msghdr;
    char cmsg_buffer[256];
    int ret, fd = -1;

    msghdr.msg_name    = NULL;
    msghdr.msg_namelen = 0;
    msghdr.msg_iov     = &vec;
    msghdr.msg_iovlen  = 1;
    msghdr.msg_control = cmsg_buffer;
    msghdr.msg_controllen = sizeof(cmsg_buffer);
    msghdr.msg_flags   = 0;

    vec.iov_base = (void *)handle;
    vec.iov_len  = sizeof(*handle);

    for (;;)
    {
        if ((ret = recvmsg( fd_socket, &msghdr, MSG_CMSG_CLOEXEC )) > 0)
        {
            struct cmsghdr *cmsg;
            for (cmsg = CMSG_FIRSTHDR( &msghdr ); cmsg; cmsg = CMSG_NXTHDR( &msghdr, cmsg ))
            {
                if (cmsg->cmsg_level != SOL_SOCKET) continue;
                if (cmsg->cmsg_type == SCM_RIGHTS) fd = *(int *)CMSG_DATA(cmsg);
#ifdef SCM_CREDENTIALS
                else if (cmsg->cmsg_type == SCM_CREDENTIALS)
                {
                    struct ucred *ucred = (struct ucred *)CMSG_DATA(cmsg);
                    server_pid = ucred->pid;
                }
#endif
            }
            if (fd != -1) fcntl( fd, F_SETFD, FD_CLOEXEC ); /* in case MSG_CMSG_CLOEXEC is not supported */
            return fd;
        }
        if (!ret) break;
        if (errno == EINTR) continue;
        if (errno == EPIPE) break;
        server_protocol_perror("recvmsg");
    }
    /* the server closed the connection; time to die... */
    abort_thread(0);
}


/***********************************************************************/
/* fd cache support */

union fd_cache_entry
{
    LONG64 data;
    struct
    {
        int fd;
        enum server_fd_type type : 5;
        unsigned int        access : 3;
        unsigned int        options : 24;
    } s;
};

C_ASSERT( sizeof(union fd_cache_entry) == sizeof(LONG64) );

#define FD_CACHE_BLOCK_SIZE  (65536 / sizeof(union fd_cache_entry))
#define FD_CACHE_ENTRIES     128

static union fd_cache_entry *fd_cache[FD_CACHE_ENTRIES];
static union fd_cache_entry fd_cache_initial_block[FD_CACHE_BLOCK_SIZE];

static inline unsigned int handle_to_index( HANDLE handle, unsigned int *entry )
{
    unsigned int idx = (wine_server_obj_handle(handle) >> 2) - 1;
    *entry = idx / FD_CACHE_BLOCK_SIZE;
    return idx % FD_CACHE_BLOCK_SIZE;
}


/***********************************************************************
 *           add_fd_to_cache
 *
 * Caller must hold fd_cache_mutex.
 */
static BOOL add_fd_to_cache( HANDLE handle, int fd, enum server_fd_type type,
                            unsigned int access, unsigned int options )
{
    unsigned int entry, idx = handle_to_index( handle, &entry );
    union fd_cache_entry cache;

    if (entry >= FD_CACHE_ENTRIES)
    {
        FIXME( "too many allocated handles, not caching %p\n", handle );
        return FALSE;
    }

    if (!fd_cache[entry])  /* do we need to allocate a new block of entries? */
    {
        if (!entry) fd_cache[0] = fd_cache_initial_block;
        else
        {
            void *ptr = anon_mmap_alloc( FD_CACHE_BLOCK_SIZE * sizeof(union fd_cache_entry),
                                         PROT_READ | PROT_WRITE );
            if (ptr == MAP_FAILED) return FALSE;
            fd_cache[entry] = ptr;
        }
    }

    /* store fd+1 so that 0 can be used as the unset value */
    cache.s.fd = fd + 1;
    cache.s.type = type;
    cache.s.access = access;
    cache.s.options = options;
    cache.data = interlocked_xchg64( &fd_cache[entry][idx].data, cache.data );
    assert( !cache.s.fd );
    return TRUE;
}


/***********************************************************************
 *           get_cached_fd
 */
static inline NTSTATUS get_cached_fd( HANDLE handle, int *fd, enum server_fd_type *type,
                                      unsigned int *access, unsigned int *options )
{
    unsigned int entry, idx = handle_to_index( handle, &entry );
    union fd_cache_entry cache;

    if (entry >= FD_CACHE_ENTRIES || !fd_cache[entry]) return STATUS_INVALID_HANDLE;

    cache.data = InterlockedCompareExchange64( &fd_cache[entry][idx].data, 0, 0 );
    if (!cache.data) return STATUS_INVALID_HANDLE;

    /* if fd type is invalid, fd stores an error value */
    if (cache.s.type == FD_TYPE_INVALID) return cache.s.fd - 1;

    *fd = cache.s.fd - 1;
    if (type) *type = cache.s.type;
    if (access) *access = cache.s.access;
    if (options) *options = cache.s.options;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           remove_fd_from_cache
 */
static int remove_fd_from_cache( HANDLE handle )
{
    unsigned int entry, idx = handle_to_index( handle, &entry );
    int fd = -1;
#ifdef __PROSPERO__
    ps5_title_read_forget_handle( handle ); /* Also covers DUPLICATE_CLOSE_SOURCE. */
#endif

    if (entry < FD_CACHE_ENTRIES && fd_cache[entry])
    {
        union fd_cache_entry cache;
        cache.data = interlocked_xchg64( &fd_cache[entry][idx].data, 0 );
        if (cache.s.type != FD_TYPE_INVALID) fd = cache.s.fd - 1;
    }

    return fd;
}


/***********************************************************************
 *           server_get_unix_fd
 *
 * The returned unix_fd should be closed iff needs_close is non-zero.
 */
int server_get_unix_fd( HANDLE handle, unsigned int wanted_access, int *unix_fd,
                        int *needs_close, enum server_fd_type *type, unsigned int *options )
{
    sigset_t sigset;
    obj_handle_t fd_handle;
    int ret, fd = -1;
    unsigned int access = 0;

    *unix_fd = -1;
    *needs_close = 0;
    wanted_access &= FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA;

    ret = get_cached_fd( handle, &fd, type, &access, options );
    if (ret != STATUS_INVALID_HANDLE) goto done;

    server_enter_uninterrupted_section( &fd_cache_mutex, &sigset );
    ret = get_cached_fd( handle, &fd, type, &access, options );
    if (ret == STATUS_INVALID_HANDLE)
    {
        SERVER_START_REQ( get_handle_fd )
        {
            req->handle = wine_server_obj_handle( handle );
            if (!(ret = wine_server_call( req )))
            {
                if (type) *type = reply->type;
                if (options) *options = reply->options;
                access = reply->access;
                if ((fd = wine_server_receive_fd( &fd_handle )) != -1)
                {
                    assert( wine_server_ptr_handle(fd_handle) == handle );
                    *needs_close = (!reply->cacheable ||
                                    !add_fd_to_cache( handle, fd, reply->type,
                                                      reply->access, reply->options ));
                }
                else ret = STATUS_TOO_MANY_OPENED_FILES;
            }
            else if (reply->cacheable)
            {
                add_fd_to_cache( handle, ret, FD_TYPE_INVALID, 0, 0 );
            }
        }
        SERVER_END_REQ;
    }
    server_leave_uninterrupted_section( &fd_cache_mutex, &sigset );

done:
    if (!ret && ((access & wanted_access) != wanted_access))
    {
        ret = STATUS_ACCESS_DENIED;
        if (*needs_close) close( fd );
    }
    if (!ret) *unix_fd = fd;
    return ret;
}


/***********************************************************************
 *           wine_server_fd_to_handle
 */
NTSTATUS CDECL wine_server_fd_to_handle( int fd, unsigned int access, unsigned int attributes, HANDLE *handle )
{
    unsigned int ret;

    *handle = 0;
    wine_server_send_fd( fd );

    SERVER_START_REQ( alloc_file_handle )
    {
        req->access     = access;
        req->attributes = attributes;
        req->fd         = fd;
        if (!(ret = wine_server_call( req ))) *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/***********************************************************************
 *           unixcall_wine_server_fd_to_handle
 */
NTSTATUS unixcall_wine_server_fd_to_handle( void *args )
{
    struct wine_server_fd_to_handle_params *params = args;

    return wine_server_fd_to_handle( params->fd, params->access, params->attributes, params->handle );
}


/***********************************************************************
 *           wine_server_handle_to_fd
 *
 * Retrieve the file descriptor corresponding to a file handle.
 */
NTSTATUS CDECL wine_server_handle_to_fd( HANDLE handle, unsigned int access, int *unix_fd,
                                         unsigned int *options )
{
    int needs_close;
    NTSTATUS ret = server_get_unix_fd( handle, access, unix_fd, &needs_close, NULL, options );

    if (!ret && !needs_close)
    {
        if ((*unix_fd = dup(*unix_fd)) == -1) ret = STATUS_TOO_MANY_OPENED_FILES;
    }
    return ret;
}


/***********************************************************************
 *           unixcall_wine_server_handle_to_fd
 */
NTSTATUS unixcall_wine_server_handle_to_fd( void *args )
{
    struct wine_server_handle_to_fd_params *params = args;

    return wine_server_handle_to_fd( params->handle, params->access, params->unix_fd, params->options );
}


/***********************************************************************
 *           server_pipe
 *
 * Create a pipe for communicating with the server.
 */
int server_pipe( int fd[2] )
{
    int ret;
#ifdef __PROSPERO__
    /* Unix socket pairs, not pipes: every thread has three of these (requests,
     * replies, wakeups), and pipe buffers come from a small kernel pool
     * (kern.ipc.maxpipekva) that payloads cannot read and that Battle.net ran
     * out: wineserver's reply writes failed with ENOMEM and threads crashed
     * (2026-10-04). Socket buffers come from elsewhere, and sockets read,
     * write and poll as these pipes are used. */
    if (!(ret = socketpair( AF_UNIX, SOCK_STREAM, 0, fd )))
#else
#ifdef HAVE_PIPE2
    static BOOL have_pipe2 = TRUE;

    if (have_pipe2)
    {
        if (!(ret = pipe2( fd, O_CLOEXEC ))) return ret;
        if (errno == ENOSYS || errno == EINVAL) have_pipe2 = FALSE;  /* don't try again */
    }
#endif
    if (!(ret = pipe( fd )))
#endif
    {
        fcntl( fd[0], F_SETFD, FD_CLOEXEC );
        fcntl( fd[1], F_SETFD, FD_CLOEXEC );
    }
    return ret;
}


/***********************************************************************
 *           init_server_dir
 */
static const char *init_server_dir( dev_t dev, ino_t ino )
{
    char *dir = NULL;

#ifdef __ANDROID__  /* there's no /tmp dir on Android */
    asprintf( &dir, "%s/.wineserver/server-%llx-%llx", config_dir, (unsigned long long)dev, (unsigned long long)ino );
#elif defined(__PROSPERO__)  /* the PS5 has no /tmp, and binds AF_UNIX sockets only in /system_tmp */
    asprintf( &dir, "/system_tmp/.wine-%u/server-%llx-%llx", getuid(), (unsigned long long)dev, (unsigned long long)ino );
#else
    asprintf( &dir, "/tmp/.wine-%u/server-%llx-%llx", getuid(), (unsigned long long)dev, (unsigned long long)ino );
#endif
    return dir;
}


#ifdef __PROSPERO__
/* Make link point at target, replacing a link that points elsewhere. */
static void set_dosdevice_link( const char *link, const char *target )
{
    char buf[PATH_MAX];
    ssize_t len = readlink( link, buf, sizeof(buf) - 1 );

    if (len >= 0)
    {
        buf[len] = 0;
        if (!strcmp( buf, target )) return;
        unlink( link );
    }
    if (symlink( target, link ) == -1 && errno != EEXIST) fatal_perror( "cannot create %s", link );
}

/***********************************************************************
 *           init_dosdevices_dir
 *
 * /data has no symbolic links and /system_tmp does (ps5/probes/symlink), so
 * on the PS5 the drive letters live there beside the server directory, keyed
 * by the prefix's st (dev and ino). The console empties /system_tmp on
 * restart, so every process makes them when missing: the first through
 * setup_config_dir, the ones it starts in server_init_process.
 *
 * z: shows the console's storage and nothing else: a folder beside the drive
 * letters holding two links, data -> /data and mnt -> /mnt (the internal
 * drive and external drives), so a program can install anywhere the person
 * chooses but sees none of the system's files. w: is the folder that holds
 * the prefix (PS5_WINE_ROOT for the prefix PS5_WINE_ROOT/prefix, inside the
 * launcher title's folder), where installers are dropped and Wine starts.
 */
static void init_dosdevices_dir( const struct stat *st )
{
    char *base, *dir, *view, *link, *target, *root, *slash;
    static const char *const storage[] = { "/data", "/mnt" };
    unsigned int i;

    if (asprintf( &base, "/system_tmp/.wine-%u", getuid() ) == -1 ||
        asprintf( &dir, "%s/dosdevices-%llx-%llx", base,
                  (unsigned long long)st->st_dev, (unsigned long long)st->st_ino ) == -1 ||
        asprintf( &view, "%s/storage-%llx-%llx", base,
                  (unsigned long long)st->st_dev, (unsigned long long)st->st_ino ) == -1 ||
        asprintf( &link, "%s/c:", dir ) == -1 ||
        asprintf( &target, "%s/drive_c", config_dir ) == -1 ||
        !(root = strdup( config_dir )))
        fatal_error( "out of memory\n" );
    if ((slash = strrchr( root, '/' )) && slash != root) *slash = 0;
    else strcpy( root, "/" );
    mkdir( base, 0700 );
    if (mkdir( dir, 0777 ) == -1 && errno != EEXIST) fatal_perror( "cannot create %s", dir );
    if (mkdir( view, 0777 ) == -1 && errno != EEXIST) fatal_perror( "cannot create %s", view );
    for (i = 0; i < ARRAY_SIZE(storage); i++)
    {
        char *entry;
        if (asprintf( &entry, "%s%s", view, storage[i] ) == -1) fatal_error( "out of memory\n" );
        set_dosdevice_link( entry, storage[i] );
        free( entry );
    }
    set_dosdevice_link( link, target );
    link[strlen( link ) - 2] = 'z';
    set_dosdevice_link( link, view );
    link[strlen( link ) - 2] = 'w';
    set_dosdevice_link( link, root );
    dosdevices_dir = dir;
    setenv( "WINEDOSDEVICES", dir, 1 );  /* for mountmgr */
    free( base );
    free( view );
    free( link );
    free( target );
    free( root );
}
#endif

#ifdef __PROSPERO__
static int stat_dir( const char *path, struct stat *st )
{
    int fd, ret, err;

    if ((fd = open( path, O_RDONLY | O_DIRECTORY )) == -1)
    {
        fprintf( stderr, "wine: open directory %s failed errno=%d\n", path, errno );
        return -1;
    }
    ret = fstat( fd, st );
    err = errno;
    if (ret == -1) fprintf( stderr, "wine: fstat directory %s failed errno=%d\n", path, err );
    close( fd );
    errno = err;
    return ret;
}
#endif



#ifdef __PROSPERO__
static int setup_title_prefix(void)
{
    char path[PATH_MAX], timestamp[64], *slash;
    struct stat st;
    FILE *file;
    static const char *const registry_files[] = { "system.reg", "user.reg", "userdef.reg" };
    unsigned int i;
#ifdef WINE_PS5_TITLE_ROOT
    char *timestamp_end;
#endif

    if (asprintf( &title_c_drive, "%s/drive_c", config_dir ) == -1 ||
        !(title_w_drive = strdup( config_dir )))
        fatal_error( "out of memory\n" );
    slash = strrchr( title_w_drive, '/' );
    if (!slash || slash == title_w_drive) fatal_error( "title prefix must have a parent directory\n" );
    *slash = 0;
#ifdef WINE_PS5_TITLE_ROOT
    /* The B1 build reuses an installed prefix but owns a separate runtime. */
    if (asprintf( (char **)&server_dir, "%s/server-title", WINE_PS5_TITLE_ROOT ) == -1)
#else
    if (asprintf( (char **)&server_dir, "%s/server-title", title_w_drive ) == -1)
#endif
        fatal_error( "out of memory\n" );
    if (mkdir( server_dir, 0700 ) == -1 && errno != EEXIST)
        fatal_perror( "cannot create private title server directory %s", server_dir );
    snprintf( path, sizeof(path), "%s/.update-timestamp", config_dir );
    if (!(file = fopen( path, "r" )))
        fatal_error( "title prefix needs updating by payload Wine: missing %s\n", path );
    if (!fgets( timestamp, sizeof(timestamp), file )) timestamp[0] = 0;
    fclose( file );
    if (build_dir) snprintf( path, sizeof(path), "%s/loader/wine.inf", build_dir );
    else snprintf( path, sizeof(path), "%s/wine.inf", data_dir );
    if (stat( path, &st ) == -1)
        fatal_perror( "cannot read title prefix version %s", path );
#ifndef WINE_PS5_TITLE_ROOT
    if (strncmp( timestamp, "disable", 7 ) && strtoul( timestamp, NULL, 10 ) != st.st_mtime)
        fatal_error( "title prefix needs updating by payload Wine: wine.inf timestamp changed\n" );
#else
    /* B1 deliberately does not update the existing application prefix. Its
     * initialized registries are read by the non-saving in-process server;
     * a newer title-only PE runtime does not imply a payload wineboot. */
    if (strncmp( timestamp, "disable", 7 ))
    {
        /* wineboot writes "%u\r\n"; accept the number plus line ending only. */
        if (!strtoul( timestamp, &timestamp_end, 10 )) timestamp_end = timestamp;
        while (*timestamp_end == '\r' || *timestamp_end == '\n') timestamp_end++;
        if (timestamp_end == timestamp || *timestamp_end)
            fatal_error( "title prefix has an invalid update timestamp\n" );
    }
#endif
    for (i = 0; i < ARRAY_SIZE(registry_files); i++)
    {
        snprintf( path, sizeof(path), "%s/%s", config_dir, registry_files[i] );
        if (!(file = fopen( path, "r" )))
            fatal_error( "title prefix needs initialization by payload Wine: missing %s\n", path );
        if (!fgets( timestamp, sizeof(timestamp), file )) timestamp[0] = 0;
        fclose( file );
        if (strncmp( timestamp, "WINE REGISTRY Version 2", 23 ))
            fatal_error( "title prefix needs initialization by payload Wine: invalid %s\n", path );
    }
    snprintf( path, sizeof(path), "wine: title prefix=%s server=%s C:=%s W:=%s; no dosdevice symlinks\n",
              config_dir, server_dir, title_c_drive, title_w_drive );
    sceKernelDebugOutText( 0, path );
    return ps5_openat( AT_FDCWD, title_w_drive, O_RDONLY | O_DIRECTORY | O_CLOEXEC );
}

static void register_title_thread( void *teb, BOOL initial )
{
    struct ntdll_thread_data *data = ntdll_get_thread_data();
    struct wine_ps5_title_thread *entry = &data->title_thread;
    struct wine_ps5_suspend_diagnostic *diagnostic = &data->suspend_diagnostic;
    sigset_t previous;
    int ret;

    data->profile_request = NULL;
    data->profile_request_id = 0;
    data->profile_waiting = 0;
    data->wait_stats = NULL;
    data->map_sample = NULL;
    data->wait_scope = NULL;
    data->wait_block_started = 0;
    if (initial) title_wait_main_tid = GetCurrentThreadId();
    memset( diagnostic, 0, sizeof(*diagnostic) );
    memset( &data->hitch, 0, sizeof(data->hitch) );
    /* The process initial thread runs the game's entry point; every block
     * points d3d12core at it as the hitch "main" thread. */
    if (initial) title_hitch_main = &data->hitch;
    data->hitch.main_block = (uintptr_t)title_hitch_main;
    data->hitch.tid = GetCurrentThreadId();
    data->hitch.magic = WINE_PS5_HITCH_MAGIC;
    __atomic_store_n( &diagnostic->magic, WINE_PS5_SUSPEND_DIAGNOSTIC_MAGIC, __ATOMIC_RELEASE );
    pthread_sigmask( SIG_BLOCK, &ps5_title_block_set, &previous );
    ret = wine_ps5_title_register_current_thread( &title_threads, entry, GetCurrentThreadId(), teb,
                                                 title_thread_key, title_runtime.set_specific );
    pthread_sigmask( SIG_SETMASK, &previous, NULL );
    if (ret) fatal_error( "cannot register title thread: %s\n", strerror( ret ));
}

static int signal_title_thread( unsigned int id, void *teb, int sig )
{
    return wine_ps5_title_signal_thread( &title_threads, id, teb, sig );
}

enum title_profile_skip_reason
{
    TITLE_PROFILE_SKIP_NONE,
    TITLE_PROFILE_SKIP_REGISTRY_LOCK
};

#define TITLE_EVENT_SAMPLE_EVERY 128
#define TITLE_EVENT_CALLERS 256
#define TITLE_EVENT_CHAIN 3

struct title_event_caller
{
    ULONG_PTR key;  /* 0 = empty, 1 = being published; immutable once published */
    uint64_t count;
    unsigned int depth;
    struct ps5_title_stack_address frames[TITLE_EVENT_CHAIN];
};

static struct title_event_caller title_event_callers[TITLE_EVENT_CALLERS];
static uint64_t title_event_total, title_event_samples, title_event_unresolved, title_event_overflow;
static uint64_t title_event_report_ns;

#ifdef __x86_64__
/* A diagnostic cannot call the PE fast unwinder while inside a syscall: that
 * path may allocate TLS, poll the tune file or wait for the loader lock, all
 * through another syscall using this same frame. Instead interpret only the
 * bounded, ordinary x64 image unwind records here. Never search stack words
 * for plausible PCs: unsupported records end the chain, not invent callers. */
static const void *title_event_image_data( const struct ps5_title_stack_address *image,
                                          ULONG_PTR rva, size_t size )
{
    if (image->size > ~(ULONG_PTR)0 - image->base ||
        rva > image->size || size > image->size - rva) return NULL;
    return (const void *)(image->base + rva);
}

static BOOL title_event_stack_word( ULONG_PTR address, ULONG_PTR low, ULONG_PTR high,
                                    ULONG64 *value )
{
    if ((address & 7) || address < low || address >= high || high - address < 8) return FALSE;
    *value = *(const ULONG64 *)address;
    return TRUE;
}

/* Called only inside the Unix fault guard, with no Wine lock held. */
static BOOL title_event_unwind_image( const struct ps5_title_stack_address *image,
                                      ULONG64 regs[16], ULONG_PTR *pc, ULONG_PTR low,
                                      ULONG_PTR high )
{
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_DATA_DIRECTORY *dir;
    const RUNTIME_FUNCTION *table, *entry = NULL;
    RUNTIME_FUNCTION function;
    unsigned int left, right, mid, chain, i;
    ULONG_PTR rva = *pc - image->base, frame;
    ULONG64 next;

    if (!(dos = title_event_image_data( image, 0, sizeof(*dos) )) ||
        dos->e_magic != IMAGE_DOS_SIGNATURE || (LONG)dos->e_lfanew < 0 ||
        !(nt = title_event_image_data( image, dos->e_lfanew, sizeof(*nt) )) ||
        nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
        return FALSE;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir->Size % sizeof(*table) ||
        !(table = title_event_image_data( image, dir->VirtualAddress, dir->Size ))) return FALSE;
    left = 0;
    right = dir->Size / sizeof(*table);
    while (left < right)
    {
        mid = left + (right - left) / 2;
        if (rva < table[mid].BeginAddress) right = mid;
        else if (rva >= table[mid].EndAddress) left = mid + 1;
        else
        {
            entry = &table[mid];
            break;
        }
    }
    if (entry) function = *entry;
    frame = regs[4];
    for (chain = 0; entry; chain++)
    {
        const BYTE *info;
        const USHORT *codes;
        unsigned int version, count, flags, reg;

        if (chain == 8 || (function.UnwindData & 1) ||
            !(info = title_event_image_data( image, function.UnwindData, 4 ))) return FALSE;
        version = info[0] & 7;
        flags = info[0] >> 3;
        count = info[2];
        reg = info[3] & 15;
        /* Calls do not normally return into a prolog. Do not partially
         * interpret one, or an unrecognized version/machine frame. */
        if ((version != 1 && version != 2) ||
            (rva >= function.BeginAddress && rva - function.BeginAddress < info[1]) ||
            !(codes = title_event_image_data( image, (ULONG_PTR)function.UnwindData + 4, count * 2 )))
            return FALSE;
        if (reg) frame = regs[reg] - (info[3] >> 4) * 16;
        if (frame < low || frame >= high) return FALSE;
        for (i = 0; i < count;)
        {
            unsigned int op = (codes[i] >> 8) & 15, operand = codes[i] >> 12;
            unsigned int slots = 1;
            ULONG_PTR offset = 0;

            if (op == 1) slots = operand ? 3 : 2;  /* ALLOC_LARGE */
            else if (op == 4 || op == 8 || op == 6) slots = 2;
            else if (op == 5 || op == 9) slots = 3;
            if (slots > count - i) return FALSE;
            if (slots >= 2) offset = codes[i + 1];
            if (slots == 3) offset |= (ULONG_PTR)codes[i + 2] << 16;
            switch (op)
            {
            case 0: /* PUSH_NONVOL */
                if (operand == 4 ||
                    !title_event_stack_word( regs[4], low, high, &regs[operand] )) return FALSE;
                regs[4] += 8;
                break;
            case 1: /* ALLOC_LARGE */
                if (operand > 1) return FALSE;
                regs[4] += operand ? offset : offset * 8;
                break;
            case 2: /* ALLOC_SMALL */
                regs[4] += (operand + 1) * 8;
                break;
            case 3: /* SET_FPREG */
                if (!reg || operand) return FALSE;
                regs[4] = frame;
                break;
            case 4: /* SAVE_NONVOL */
            case 5: /* SAVE_NONVOL_FAR */
                if (op == 4) offset *= 8;
                if (operand == 4 || offset > high - frame ||
                    !title_event_stack_word( frame + offset, low, high, &regs[operand] )) return FALSE;
                break;
            case 6: /* EPILOG (v2 descriptor, not an operation) */
                if (version != 2) return FALSE;
                break;
            case 8: /* SAVE_XMM128 */
            case 9: /* SAVE_XMM128_FAR */
                break;  /* XMM registers cannot affect the caller's PC or stack. */
            default:
                return FALSE;
            }
            if (regs[4] < low || regs[4] >= high) return FALSE;
            i += slots;
        }
        if (!(flags & UNW_FLAG_CHAININFO)) break;
        entry = title_event_image_data( image,
                                       (ULONG_PTR)function.UnwindData + 4 + ((count + 1) & ~1) * 2,
                                       sizeof(function) );
        if (!entry) return FALSE;
        function = *entry;
    }
    if (!title_event_stack_word( regs[4], low, high, &next )) return FALSE;
    regs[4] += 8;
    *pc = next;
    return TRUE;
}

static BOOL title_event_unwind_frame( const struct ps5_title_stack_address *image,
                                      ULONG64 regs[16], ULONG_PTR *pc, ULONG_PTR high )
{
    ULONG_PTR low = regs[4];
    BYTE code[4];
    BOOL volatile ret = FALSE;

    /* A plain RET has a fully restored stack: [rsp] is its return address.
     * In particular NtCreateEvent's syscall frame points at its C3 stub,
     * not at the caller. Reject other epilogs rather than applying body
     * unwind codes to a partly restored stack. */
    if (ntdll_get_thread_data()->jmp_buf ||
        !virtual_read_title_code( *pc, code, sizeof(code) )) return FALSE;
    if ((code[0] >= 0x58 && code[0] <= 0x5f) || code[0] == 0xc2 ||
        code[0] == 0xe9 || code[0] == 0xeb || code[0] == 0xff || code[0] == 0xf3 ||
        ((code[0] & 0xf0) == 0x40 &&
         ((code[1] >= 0x58 && code[1] <= 0x5f) || code[1] == 0x81 ||
          code[1] == 0x83 || code[1] == 0x8d || code[1] == 0xff)))
        return FALSE;
    __TRY
    {
        if (code[0] == 0xc3)
        {
            ULONG64 next;
            if ((ret = title_event_stack_word( regs[4], low, high, &next )))
            {
                regs[4] += 8;
                *pc = next;
            }
        }
        else ret = title_event_unwind_image( image, regs, pc, low, high );
    }
    __EXCEPT
    {
        ret = FALSE;
    }
    __ENDTRY
    return ret && regs[4] > low && regs[4] <= high;
}

static unsigned int title_event_capture( struct ps5_title_stack_address frames[TITLE_EVENT_CHAIN] )
{
    CONTEXT context;
    ULONG64 regs[16];
    ULONG_PTR pc, high = (ULONG_PTR)NtCurrentTeb()->Tib.StackBase;
    unsigned int walked, depth = 0;
    C_ASSERT( offsetof(CONTEXT, R15) - offsetof(CONTEXT, Rax) == 15 * sizeof(ULONG64) );

    if (!get_syscall_frame()) return 0;
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (NtGetContextThread( GetCurrentThread(), &context )) return 0;
    memcpy( regs, &context.Rax, sizeof(regs) );
    pc = context.Rip;
    for (walked = 0; pc && walked < 16 && depth < TITLE_EVENT_CHAIN; walked++)
    {
        struct ps5_title_stack_address address = {0};

        if (regs[4] < (ULONG_PTR)NtCurrentTeb()->Tib.StackLimit || regs[4] >= high) break;
        address.pc = pc;
        virtual_resolve_title_pe_modules( &address, 1 );
        if (!address.base) break;  /* No guessing across JIT/unmapped modules. */
        if (!wine_ps5_hang_name_equal( address.module, "ntdll.dll" ) &&
            !wine_ps5_hang_name_equal( address.module, "kernelbase.dll" ) &&
            !wine_ps5_hang_name_equal( address.module, "kernel32.dll" ))
            frames[depth++] = address;
        if (depth == TITLE_EVENT_CHAIN || !title_event_unwind_frame( &address, regs, &pc, high )) break;
    }
    return depth;
}
#endif

#define TITLE_MAIN_CALLERS 128
#define TITLE_MAIN_CAPTURE_LIMIT 4096
struct title_main_caller
{
    ULONG_PTR key;
    uint64_t count, ns, requested, timed;
    unsigned int depth, handles;
    struct ps5_title_stack_address frames[TITLE_EVENT_CHAIN];
};
struct title_main_waits
{
    struct title_main_caller callers[2][TITLE_MAIN_CALLERS];
    struct { unsigned int tid; uint64_t count, ns; } wakers[TITLE_MAIN_CALLERS];
    unsigned int overflow;
};
static struct title_main_waits title_main_banks[2], title_main_report;
static unsigned int title_main_captures, title_main_capped;
static BOOL title_main_report_ready;

static void title_main_capture( struct ps5_title_wait_scope *scope )
{
    int saved_errno = errno;

    scope->depth = 0;
    if (__atomic_fetch_add( &title_main_captures, 1, __ATOMIC_RELAXED ) >= TITLE_MAIN_CAPTURE_LIMIT)
        __atomic_add_fetch( &title_main_capped, 1, __ATOMIC_RELAXED );
#ifdef __x86_64__
    else scope->depth = title_event_capture( scope->callers );
#endif
    errno = saved_errno;
}

static void title_main_wait_record( struct ps5_title_wait_scope *scope, uint64_t ns )
{
    struct title_wait_slot *slot;
    struct title_wait_thread *pin;
    struct title_main_waits *bank;
    ULONG_PTR key, expected;
    unsigned int kind = scope->kind != PS5_TITLE_WAIT_DELAY, i, index;

    if (kind)
    {
        if ((scope->kind != PS5_TITLE_WAIT_SELECT && scope->kind != PS5_TITLE_WAIT_SELECT_TIMED) ||
            !scope->blocked || ns < 10000000) return;
        title_main_capture( scope ); /* still in server_select's original syscall frame */
    }
    if (!(slot = title_wait_get_thread( scope->data ))) return;
    pin = title_wait_acquire( slot );
    bank = &title_main_banks[pin - slot->bank];
    if (kind && scope->waker)
    {
        for (i = 0; i < TITLE_MAIN_CALLERS; i++)
        {
            unsigned int tid = __atomic_load_n( &bank->wakers[i].tid, __ATOMIC_ACQUIRE );
            if (!tid && __atomic_compare_exchange_n( &bank->wakers[i].tid, &tid, scope->waker,
                                                     FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
                tid = scope->waker;
            if (tid != scope->waker) continue;
            __atomic_add_fetch( &bank->wakers[i].count, 1, __ATOMIC_RELAXED );
            __atomic_add_fetch( &bank->wakers[i].ns, ns, __ATOMIC_RELAXED );
            break;
        }
        if (i == TITLE_MAIN_CALLERS) __atomic_add_fetch( &bank->overflow, 1, __ATOMIC_RELAXED );
    }
    key = scope->depth ? scope->callers[0].pc : 2; /* retain unresolved/capped durations */
    index = wine_ps5_profile_hash( key, 0 ) & (TITLE_MAIN_CALLERS - 1);
    for (i = 0; i < TITLE_MAIN_CALLERS; i++)
    {
        struct title_main_caller *entry = &bank->callers[kind][(index + i) & (TITLE_MAIN_CALLERS - 1)];
        expected = __atomic_load_n( &entry->key, __ATOMIC_ACQUIRE );
        if (!expected && __atomic_compare_exchange_n( &entry->key, &expected, 1, FALSE,
                                                      __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE ))
        {
            entry->depth = scope->depth;
            memcpy( entry->frames, scope->callers, scope->depth * sizeof(*scope->callers) );
            __atomic_store_n( &entry->key, key, __ATOMIC_RELEASE );
            expected = key;
        }
        if (expected == 1) break;
        if (expected != key) continue;
        __atomic_add_fetch( &entry->count, 1, __ATOMIC_RELAXED );
        __atomic_add_fetch( &entry->ns, ns, __ATOMIC_RELAXED );
        __atomic_store_n( &entry->handles, scope->handles, __ATOMIC_RELAXED );
        if (!kind && scope->requested != ~(uint64_t)0)
        {
            __atomic_add_fetch( &entry->requested, scope->requested, __ATOMIC_RELAXED );
            __atomic_add_fetch( &entry->timed, 1, __ATOMIC_RELAXED );
        }
        break;
    }
    if (i == TITLE_MAIN_CALLERS || expected == 1)
        __atomic_add_fetch( &bank->overflow, 1, __ATOMIC_RELAXED );
    title_wait_release( slot, pin );
}

static void title_main_wait_take( unsigned int bank )
{
    title_main_report = title_main_banks[bank];
    memset( &title_main_banks[bank], 0, sizeof(title_main_banks[bank]) );
    title_main_report_ready = TRUE;
}

static void title_main_wait_emit( unsigned long long ms )
{
    unsigned int kind, rank, i, best, capped;
    char line[768], via[256], extra[96];

    __atomic_exchange_n( &title_main_captures, 0, __ATOMIC_RELAXED );
    capped = __atomic_exchange_n( &title_main_capped, 0, __ATOMIC_RELAXED );
    if (capped || (title_main_report_ready && title_main_report.overflow))
    {
        snprintf( line, sizeof(line), "title main waits window_ms=%llu capped=%u overflow=%u capture_limit=%u\n",
                  ms, capped, title_main_report_ready ? title_main_report.overflow : 0, TITLE_MAIN_CAPTURE_LIMIT );
        sceKernelDebugOutText( 0, line );
    }
    if (!title_main_report_ready) return;
    title_main_report_ready = FALSE;
    for (kind = 0; kind < 2; kind++)
        for (rank = 1; rank <= 5; rank++)
        {
            struct title_main_caller *entry;
            size_t pos = 0;
            best = TITLE_MAIN_CALLERS;
            for (i = 0; i < TITLE_MAIN_CALLERS; i++)
                if (title_main_report.callers[kind][i].count &&
                    (best == TITLE_MAIN_CALLERS || title_main_report.callers[kind][i].ns >
                     title_main_report.callers[kind][best].ns)) best = i;
            if (best == TITLE_MAIN_CALLERS) break;
            entry = &title_main_report.callers[kind][best];
            if (!entry->depth) strcpy( via, "unavailable" );
            else for (i = 0; i < entry->depth; i++)
                pos += snprintf( via + pos, sizeof(via) - pos, "%s%s+0x%lx", i ? ">" : "",
                                 entry->frames[i].module, entry->frames[i].pc - entry->frames[i].base );
            if (kind) snprintf( extra, sizeof(extra), "handles=%u", entry->handles );
            else
            {
                uint64_t req = entry->timed ? entry->requested / entry->timed / 1000 : 0;
                snprintf( extra, sizeof(extra), "req_ms_mean=%llu.%03llu",
                          (unsigned long long)(req / 1000), (unsigned long long)(req % 1000) );
            }
            snprintf( line, sizeof(line), "title main waits window_ms=%llu kind=%s rank=%u count=%llu "
                      "total_ms=%llu.%03llu %s where=%s+0x%lx via=%s\n",
                      ms, kind ? "select" : "delay", rank, (unsigned long long)entry->count,
                      (unsigned long long)(entry->ns / 1000000), (unsigned long long)(entry->ns / 1000 % 1000),
                      extra, entry->depth ? entry->frames[0].module : "unresolved",
                      entry->depth ? entry->frames[0].pc - entry->frames[0].base : 0, via );
            sceKernelDebugOutText( 0, line );
            entry->count = 0;
        }
    for (rank = 1; rank <= 3; rank++)
    {
        best = TITLE_MAIN_CALLERS;
        for (i = 0; i < TITLE_MAIN_CALLERS; i++)
            if (title_main_report.wakers[i].count && (best == TITLE_MAIN_CALLERS ||
                title_main_report.wakers[i].ns > title_main_report.wakers[best].ns)) best = i;
        if (best == TITLE_MAIN_CALLERS) break;
        snprintf( line, sizeof(line), "title main wakers window_ms=%llu rank=%u tid=%04x count=%llu "
                  "total_ms=%llu.%03llu\n", ms, rank, title_main_report.wakers[best].tid,
                  (unsigned long long)title_main_report.wakers[best].count,
                  (unsigned long long)(title_main_report.wakers[best].ns / 1000000),
                  (unsigned long long)(title_main_report.wakers[best].ns / 1000 % 1000) );
        sceKernelDebugOutText( 0, line );
        title_main_report.wakers[best].count = 0;
    }
}

void ps5_title_profile_event(void)
{
    struct ps5_title_stack_address frames[TITLE_EVENT_CHAIN];
    ULONG_PTR key, expected;
    uint64_t total = __atomic_fetch_add( &title_event_total, 1, __ATOMIC_RELAXED );
    unsigned int depth = 0, i, index;
    int saved_errno;

    if (total % TITLE_EVENT_SAMPLE_EVERY) return;
    saved_errno = errno;
    __atomic_add_fetch( &title_event_samples, 1, __ATOMIC_RELAXED );
#ifdef __x86_64__
    depth = title_event_capture( frames );
#endif
    errno = saved_errno;
    if (!depth)
    {
        __atomic_add_fetch( &title_event_unresolved, 1, __ATOMIC_RELAXED );
        return;
    }
    key = frames[0].pc;
    index = wine_ps5_profile_hash( key, 0 ) & (TITLE_EVENT_CALLERS - 1);
    for (i = 0; i < TITLE_EVENT_CALLERS; i++)
    {
        struct title_event_caller *entry = &title_event_callers[(index + i) & (TITLE_EVENT_CALLERS - 1)];

        expected = __atomic_load_n( &entry->key, __ATOMIC_ACQUIRE );
        if (!expected && __atomic_compare_exchange_n( &entry->key, &expected, 1, FALSE,
                                                      __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE ))
        {
            entry->depth = depth;
            memcpy( entry->frames, frames, depth * sizeof(*frames) );
            __atomic_store_n( &entry->key, key, __ATOMIC_RELEASE );
            expected = key;
        }
        /* Do not publish a duplicate site while another thread fills it. */
        if (expected == 1) break;
        if (expected != key || entry->frames[0].base != frames[0].base ||
            strcmp( entry->frames[0].module, frames[0].module )) continue;
        __atomic_add_fetch( &entry->count, 1, __ATOMIC_RELAXED );
        return;
    }
    __atomic_add_fetch( &title_event_overflow, 1, __ATOMIC_RELAXED );
}

static void title_event_emit_window(void)
{
    uint64_t counts[TITLE_EVENT_CALLERS], now = ps5_title_profile_now_ns();
    uint64_t total = __atomic_exchange_n( &title_event_total, 0, __ATOMIC_RELAXED );
    uint64_t samples = __atomic_exchange_n( &title_event_samples, 0, __ATOMIC_RELAXED );
    uint64_t unresolved = __atomic_exchange_n( &title_event_unresolved, 0, __ATOMIC_RELAXED );
    uint64_t overflow = __atomic_exchange_n( &title_event_overflow, 0, __ATOMIC_RELAXED );
    unsigned long long elapsed = now >= title_event_report_ns ? (now - title_event_report_ns) / 1000000 : 0;
    unsigned int i, rank;
    char line[512], via[224];

    title_event_report_ns = now;
    snprintf( line, sizeof(line), "title event callers window_ms=%llu total=%llu samples=%llu "
              "sample_every=%u unresolved=%llu overflow=%llu method=x64_image_unwind\n",
              elapsed, (unsigned long long)total, (unsigned long long)samples, TITLE_EVENT_SAMPLE_EVERY,
              (unsigned long long)unresolved, (unsigned long long)overflow );
    sceKernelDebugOutText( 0, line );
    for (i = 0; i < TITLE_EVENT_CALLERS; i++)
    {
        ULONG_PTR key = __atomic_load_n( &title_event_callers[i].key, __ATOMIC_ACQUIRE );
        counts[i] = key > 1 ? __atomic_exchange_n( &title_event_callers[i].count, 0, __ATOMIC_RELAXED ) : 0;
    }
    for (rank = 1; rank <= 8; rank++)
    {
        struct title_event_caller *entry;
        unsigned int best = TITLE_EVENT_CALLERS;
        size_t pos = 0;

        for (i = 0; i < TITLE_EVENT_CALLERS; i++)
            if (counts[i] && (best == TITLE_EVENT_CALLERS || counts[i] > counts[best])) best = i;
        if (best == TITLE_EVENT_CALLERS) break;
        entry = &title_event_callers[best];
        for (i = 0; i < entry->depth; i++)
            pos += snprintf( via + pos, sizeof(via) - pos, "%s%s+0x%lx", i ? ">" : "",
                             entry->frames[i].module, entry->frames[i].pc - entry->frames[i].base );
        snprintf( line, sizeof(line), "title event callers window_ms=%llu total=%llu rank=%u count=%llu "
                  "where=%s+0x%lx via=%s sample_every=%u\n", elapsed, (unsigned long long)total,
                  rank, (unsigned long long)counts[best], entry->frames[0].module,
                  entry->frames[0].pc - entry->frames[0].base, via, TITLE_EVENT_SAMPLE_EVERY );
        sceKernelDebugOutText( 0, line );
        counts[best] = 0;
    }
}

struct title_profile_bucket
{
    ULONG_PTR where;
    ULONG_PTR module_base;
    unsigned int count;
    unsigned int tid;
    unsigned short kind;
    char module[40];
};

struct title_profile_thread
{
    unsigned int tid;
    unsigned int total;
    unsigned int user;
    unsigned int unix_count;
    unsigned int wait;
    uint64_t cpu_first, cpu_last, wall_first, wall_last;
    uint64_t lifetime;
    BOOL cpu_valid;
};

C_ASSERT( !(WINE_PS5_PROFILE_BUCKETS & (WINE_PS5_PROFILE_BUCKETS - 1)) );
C_ASSERT( !(WINE_PS5_HANG_THREAD_MAX & (WINE_PS5_HANG_THREAD_MAX - 1)) );

static struct title_profile_bucket title_profile_buckets[WINE_PS5_PROFILE_BUCKETS];
static struct title_profile_thread title_profile_threads[WINE_PS5_HANG_THREAD_MAX];
static struct
{
    unsigned int tid;
    struct wine_ps5_alert_stats stats;
    struct wine_ps5_read_stats reads;
} title_profile_alerts[WINE_PS5_HANG_THREAD_MAX];
static unsigned int title_profile_alert_count;
static unsigned int title_profile_generation, title_profile_last_generation;
static unsigned int title_profile_ticks, title_profile_samples, title_profile_thread_count;
static unsigned int title_profile_overflow, title_profile_main_tid;
static enum title_profile_skip_reason title_profile_skip;

static void title_profile_reset_window(void)
{
    memset( title_profile_buckets, 0, sizeof(title_profile_buckets) );
    memset( title_profile_threads, 0, sizeof(title_profile_threads) );
    title_profile_alert_count = 0;
    title_profile_last_generation = 0;
    title_profile_samples = title_profile_thread_count = title_profile_overflow = 0;
    title_profile_main_tid = 0;
    title_profile_skip = TITLE_PROFILE_SKIP_NONE;
}

static struct title_profile_bucket *title_profile_get_bucket( unsigned int kind,
                                                              unsigned int tid,
                                                              ULONG_PTR where )
{
    unsigned int index = wine_ps5_profile_hash( ((uint64_t)kind << 32) | tid, where ) &
                         (WINE_PS5_PROFILE_BUCKETS - 1);
    unsigned int i;

    for (i = 0; i < WINE_PS5_PROFILE_BUCKETS; i++)
    {
        struct title_profile_bucket *bucket =
            &title_profile_buckets[(index + i) & (WINE_PS5_PROFILE_BUCKETS - 1)];

        if (!bucket->count)
        {
            bucket->kind = kind;
            bucket->tid = tid;
            bucket->where = where;
            return bucket;
        }
        if (bucket->kind == kind && bucket->tid == tid && bucket->where == where)
            return bucket;
    }
    return NULL;
}

static struct title_profile_thread *title_profile_get_thread( unsigned int tid )
{
    unsigned int index = wine_ps5_profile_hash( tid, 0 ) &
                         (WINE_PS5_HANG_THREAD_MAX - 1);
    unsigned int i;

    for (i = 0; i < WINE_PS5_HANG_THREAD_MAX; i++)
    {
        struct title_profile_thread *thread =
            &title_profile_threads[(index + i) & (WINE_PS5_HANG_THREAD_MAX - 1)];

        if (!thread->total)
        {
            thread->tid = tid;
            title_profile_thread_count++;
            return thread;
        }
        if (thread->tid == tid) return thread;
    }
    return NULL;
}

BOOL ps5_title_profile_read_cpu( uint64_t *cpu, uint64_t *wall )
{
    struct timespec wall_time;

    if (!ps5_profile_cpu_time || ps5_profile_cpu_time( cpu ) ||
        clock_gettime( CLOCK_MONOTONIC, &wall_time ))
        return FALSE;
    *wall = (uint64_t)wall_time.tv_sec * 1000000000 + wall_time.tv_nsec;
    return TRUE;
}

static void title_profile_update_cpu( struct title_profile_thread *thread,
                                      const struct wine_ps5_title_thread *entry,
                                      const struct wine_ps5_suspend_diagnostic *diagnostic )
{
    BOOL valid = diagnostic->profile_cpu_valid;
    uint64_t cpu = diagnostic->profile_cpu_ns, wall = diagnostic->profile_wall_ns;

    if (ps5_profile_clock_id && title_profile_clock_logs < 2 &&
        entry->lifetime != title_profile_clock_lifetime)
    {
        int clock = 0, error = ps5_profile_clock_id( entry->thread, &clock );
        char line[160];

        snprintf( line, sizeof(line),
                  "title profile clock tid=%04x pthread=%p clock_id=%d error=%d source=current_thread\n",
                  entry->id, (void *)entry->thread, clock, error );
        sceKernelDebugOutText( 0, line );
        title_profile_clock_lifetime = entry->lifetime;
        title_profile_clock_logs++;
    }

    if (!thread->total)
    {
        thread->lifetime = entry->lifetime;
        thread->cpu_valid = valid;
        thread->cpu_first = thread->cpu_last = cpu;
        thread->wall_first = thread->wall_last = wall;
    }
    else if (thread->lifetime != entry->lifetime)
        thread->cpu_valid = FALSE; /* Never attach a reused ID's CPU clock to its predecessor. */
    else if (thread->cpu_valid)
    {
        thread->cpu_valid = valid && cpu >= thread->cpu_last && wall > thread->wall_last;
        thread->cpu_last = cpu;
        thread->wall_last = wall;
    }
}

static void title_profile_collect( const struct wine_ps5_title_thread *entry )
{
    struct wine_ps5_suspend_diagnostic *diagnostic =
        wine_ps5_suspend_diagnostic_from_teb64( (void *)entry->teb );
    struct title_profile_bucket *bucket;
    struct title_profile_thread *thread;
    unsigned int kind, bucket_tid;
    ULONG_PTR where, module_base;

    if (!title_profile_last_generation ||
        __atomic_load_n( &diagnostic->profile_completed, __ATOMIC_ACQUIRE ) !=
        title_profile_last_generation)
        return;
    kind = __atomic_load_n( &diagnostic->profile_kind, __ATOMIC_RELAXED );
    where = __atomic_load_n( &diagnostic->profile_where, __ATOMIC_RELAXED );
    module_base = __atomic_load_n( &diagnostic->profile_module_base, __ATOMIC_RELAXED );
    if (kind < WINE_PS5_PROFILE_USER || kind > WINE_PS5_PROFILE_WAIT) return;

    bucket_tid = kind == WINE_PS5_PROFILE_WAIT ? 0 : entry->id;
    if ((bucket = title_profile_get_bucket( kind, bucket_tid, where )))
    {
        if ((kind == WINE_PS5_PROFILE_USER && module_base &&
             (!bucket->count || !bucket->module_base)) ||
            ((kind == WINE_PS5_PROFILE_UNIX_REQUEST || kind == WINE_PS5_PROFILE_UNIX_CALL) &&
             !bucket->count))
        {
            bucket->module_base = module_base;
            memcpy( bucket->module, diagnostic->profile_module, sizeof(bucket->module) );
        }
        bucket->count++;
    }
    else
        title_profile_overflow++;

    if (!(thread = title_profile_get_thread( entry->id )))
    {
        title_profile_overflow++;
        return;
    }
    if (!title_profile_main_tid || entry->id < title_profile_main_tid)
        title_profile_main_tid = entry->id;
    title_profile_update_cpu( thread, entry, diagnostic );
    thread->total++;
    switch (kind)
    {
    case WINE_PS5_PROFILE_USER:
        thread->user++;
        break;
    case WINE_PS5_PROFILE_WAIT:
        thread->wait++;
        break;
    default:
        thread->unix_count++;
        break;
    }
    title_profile_samples++;
}

static BOOL title_profile_sample_threads( BOOL send_next )
{
    struct wine_ps5_title_thread *entry;
    unsigned int generation, count = 0;

    if (pthread_mutex_trylock( &title_threads.mutex )) return FALSE;
    for (entry = title_threads.head;
         entry && count < WINE_PS5_HANG_THREAD_MAX; entry = entry->next, count++)
    {
        title_profile_collect( entry );
        if (!title_profile_ticks || !send_next)
        {
            struct ntdll_thread_data *data = (void *)&((TEB *)entry->teb)->GdiTebBatch;
            struct wine_ps5_alert_stats stats = wine_ps5_alert_take_stats( &data->alert_stats );
            struct wine_ps5_read_stats reads = wine_ps5_read_take_stats( &data->read_stats );

            /* Discard pre-window counts, including any skipped profile window. */
            if (!title_profile_ticks) continue;
            title_profile_alerts[title_profile_alert_count].tid = entry->id;
            title_profile_alerts[title_profile_alert_count].stats = stats;
            title_profile_alerts[title_profile_alert_count++].reads = reads;
        }
    }
    if (entry) title_profile_overflow++;

    if (send_next)
    {
        generation = ++title_profile_generation;
        if (!generation) generation = ++title_profile_generation;
        count = 0;
        for (entry = title_threads.head;
             entry && count < WINE_PS5_HANG_THREAD_MAX; entry = entry->next, count++)
        {
            struct wine_ps5_suspend_diagnostic *diagnostic =
                wine_ps5_suspend_diagnostic_from_teb64( (void *)entry->teb );
            if (!title_profile_main_tid || entry->id < title_profile_main_tid)
                title_profile_main_tid = entry->id;

            __atomic_store_n( &diagnostic->profile_pending, generation, __ATOMIC_RELEASE );
            pthread_kill( entry->thread, SIGURG );
        }
        title_profile_last_generation = generation;
    }
    pthread_mutex_unlock( &title_threads.mutex );
    return TRUE;
}

static void title_profile_format_where( unsigned int kind, ULONG_PTR where,
                                        ULONG_PTR module_base, const char *module,
                                        char *buffer, size_t size )
{
    switch (kind)
    {
    case WINE_PS5_PROFILE_USER:
        if (module_base && module && module[0])
            snprintf( buffer, size, "%s+0x%lx", module, where - module_base );
        else
            snprintf( buffer, size, "unresolved+0x%lx", where );
        break;
    case WINE_PS5_PROFILE_UNIX_REQUEST:
        snprintf( buffer, size, "request=%s", module && module[0] ? module : "unknown" );
        break;
    case WINE_PS5_PROFILE_UNIX_CALL:
        snprintf( buffer, size, "unixcall=%s", module && module[0] ? module : "unknown" );
        break;
    case WINE_PS5_PROFILE_UNIX_SYSCALL:
        snprintf( buffer, size, "syscall=0x%lx", where );
        break;
    default:
        snprintf( buffer, size, "blocking" );
        break;
    }
}

static const char *title_profile_kind_name( unsigned int kind )
{
    return kind == WINE_PS5_PROFILE_USER ? "user" :
           kind == WINE_PS5_PROFILE_WAIT ? "wait" : "unix";
}

static void title_profile_emit_modules(void)
{
    uint64_t modules[5] = {0}, non_wait = 0;
    unsigned int pct[5], i;
    char line[256];

    for (i = 0; i < ARRAY_SIZE(title_profile_threads); i++)
        non_wait += title_profile_threads[i].user + title_profile_threads[i].unix_count;
    for (i = 0; i < ARRAY_SIZE(title_profile_buckets); i++)
    {
        struct title_profile_bucket *bucket = &title_profile_buckets[i];
        unsigned int index;

        if (!bucket->count || bucket->kind != WINE_PS5_PROFILE_USER) continue;
        if (wine_ps5_hang_name_equal( bucket->module, "WowB.exe" )) index = 0;
        else if (wine_ps5_hang_name_equal( bucket->module, "d3d12core.dll" )) index = 1;
        else if (wine_ps5_hang_name_equal( bucket->module, "ntdll.dll" )) index = 2;
        else if (wine_ps5_hang_name_equal( bucket->module, "win32u.dll" )) index = 3;
        else index = 4;
        modules[index] += bucket->count;
    }
    for (i = 0; i < ARRAY_SIZE(pct); i++)
        pct[i] = wine_ps5_profile_permille( modules[i], non_wait );
    snprintf( line, sizeof(line),
              "title profile modules WowB.exe=%u.%u d3d12core.dll=%u.%u "
              "ntdll.dll=%u.%u win32u.dll=%u.%u other=%u.%u\n",
              pct[0] / 10, pct[0] % 10, pct[1] / 10, pct[1] % 10,
              pct[2] / 10, pct[2] % 10, pct[3] / 10, pct[3] % 10,
              pct[4] / 10, pct[4] % 10 );
    sceKernelDebugOutText( 0, line );
}

static void title_profile_emit_thread( const struct title_profile_thread *thread )
{
    struct title_profile_bucket *user_top[3] = {0}, *unix_top = NULL;
    unsigned int busy, user, unix_pct, wait, i, j;
    char line[768], where[96], cpu_pct[16];
    size_t pos;

    for (i = 0; i < ARRAY_SIZE(title_profile_buckets); i++)
    {
        struct title_profile_bucket *bucket = &title_profile_buckets[i];

        if (!bucket->count || bucket->tid != thread->tid) continue;
        if (bucket->kind == WINE_PS5_PROFILE_USER)
        {
            for (j = 0; j < ARRAY_SIZE(user_top) && user_top[j] &&
                 user_top[j]->count >= bucket->count; j++) {}
            if (j == ARRAY_SIZE(user_top)) continue;
            memmove( user_top + j + 1, user_top + j,
                     (ARRAY_SIZE(user_top) - j - 1) * sizeof(*user_top) );
            user_top[j] = bucket;
        }
        else if ((bucket->kind == WINE_PS5_PROFILE_UNIX_SYSCALL ||
                  bucket->kind == WINE_PS5_PROFILE_UNIX_REQUEST ||
                  bucket->kind == WINE_PS5_PROFILE_UNIX_CALL) &&
                 (!unix_top || bucket->count > unix_top->count))
            unix_top = bucket;
    }

    busy = wine_ps5_profile_permille( thread->user + thread->unix_count, thread->total );
    user = wine_ps5_profile_permille( thread->user, thread->total );
    unix_pct = wine_ps5_profile_permille( thread->unix_count, thread->total );
    wait = wine_ps5_profile_permille( thread->wait, thread->total );
    if (thread->cpu_valid && thread->wall_last > thread->wall_first)
    {
        unsigned int cpu = wine_ps5_profile_cpu_permille(
            thread->cpu_first, thread->cpu_last, thread->wall_first, thread->wall_last );

        snprintf( cpu_pct, sizeof(cpu_pct), "%u.%u", cpu / 10, cpu % 10 );
    }
    else
        strcpy( cpu_pct, "unavailable" );
    pos = snprintf( line, sizeof(line),
                    "title profile thread tid=%04x busy_pct=%u.%u user_pct=%u.%u "
                    "unix_pct=%u.%u wait_pct=%u.%u cpu_pct=%s top=",
                    thread->tid, busy / 10, busy % 10, user / 10, user % 10,
                    unix_pct / 10, unix_pct % 10, wait / 10, wait % 10, cpu_pct );
    if (!user_top[0])
        pos += snprintf( line + pos, sizeof(line) - pos, "none" );
    else for (i = 0; i < ARRAY_SIZE(user_top) && user_top[i]; i++)
    {
        unsigned int pct = wine_ps5_profile_permille( user_top[i]->count, thread->total );

        title_profile_format_where( user_top[i]->kind, user_top[i]->where,
                                    user_top[i]->module_base, user_top[i]->module,
                                    where, sizeof(where) );
        pos += snprintf( line + pos, sizeof(line) - pos, "%s%s(%u.%u)",
                         i ? " " : "", where, pct / 10, pct % 10 );
    }
    if (unix_top)
    {
        unsigned int pct = wine_ps5_profile_permille( unix_top->count, thread->total );

        title_profile_format_where( unix_top->kind, unix_top->where,
                                    unix_top->module_base, unix_top->module,
                                    where, sizeof(where) );
        pos += snprintf( line + pos, sizeof(line) - pos, " unix_top=%s(%u.%u)",
                         where, pct / 10, pct % 10 );
    }
    else
        pos += snprintf( line + pos, sizeof(line) - pos, " unix_top=none" );
    snprintf( line + min( pos, sizeof(line) - 2 ), sizeof(line) - min( pos, sizeof(line) - 2 ),
              "\n" );
    sceKernelDebugOutText( 0, line );
}

static void title_profile_emit_main_unix(void)
{
    struct title_profile_bucket *top[6] = {0};
    unsigned int i, j, samples = 0;
    char line[768], where[96];
    size_t pos;

    for (i = 0; i < ARRAY_SIZE(title_profile_threads); i++)
        if (title_profile_threads[i].tid == title_wait_main_tid)
            samples = title_profile_threads[i].total;
    for (i = 0; i < ARRAY_SIZE(title_profile_buckets); i++)
    {
        struct title_profile_bucket *bucket = &title_profile_buckets[i];
        if (!bucket->count || bucket->tid != title_wait_main_tid ||
            (bucket->kind != WINE_PS5_PROFILE_UNIX_SYSCALL &&
             bucket->kind != WINE_PS5_PROFILE_UNIX_REQUEST &&
             bucket->kind != WINE_PS5_PROFILE_UNIX_CALL)) continue;
        for (j = 0; j < ARRAY_SIZE(top) && top[j] && top[j]->count >= bucket->count; j++) {}
        if (j == ARRAY_SIZE(top)) continue;
        memmove( top + j + 1, top + j, (ARRAY_SIZE(top) - j - 1) * sizeof(*top) );
        top[j] = bucket;
    }
    pos = snprintf( line, sizeof(line), "title profile main unix window_s=10 top=" );
    if (!top[0]) pos += snprintf( line + pos, sizeof(line) - pos, "none" );
    for (i = 0; i < ARRAY_SIZE(top) && top[i]; i++)
    {
        unsigned int pct = wine_ps5_profile_permille( top[i]->count, samples );
        title_profile_format_where( top[i]->kind, top[i]->where, top[i]->module_base,
                                    top[i]->module, where, sizeof(where) );
        pos += snprintf( line + pos, sizeof(line) - pos, "%s%s(%u.%u)",
                         i ? " " : "", where, pct / 10, pct % 10 );
    }
    snprintf( line + pos, sizeof(line) - pos, "\n" );
    sceKernelDebugOutText( 0, line );
}

static void title_profile_histogram( char *buffer, size_t size, const uint64_t *bins )
{
    snprintf( buffer, size, "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
              (unsigned long long)bins[0], (unsigned long long)bins[1],
              (unsigned long long)bins[2], (unsigned long long)bins[3],
              (unsigned long long)bins[4], (unsigned long long)bins[5],
              (unsigned long long)bins[6], (unsigned long long)bins[7] );
}

static void title_profile_emit_alert( unsigned int index )
{
    const struct wine_ps5_alert_stats *stats = &title_profile_alerts[index].stats;
    char line[640], gap[192];

    title_profile_histogram( gap, sizeof(gap), stats->gap );
    snprintf( line, sizeof(line),
              "wine: title alert window tid=%04x alerts=%llu wakes=%llu "
              "sleepers_inferred=%llu wake_errors=%llu mean_ns=%llu max_ns=%llu "
              "gap_us=%s gap_unavailable=%llu window_s=10\n",
              title_profile_alerts[index].tid, (unsigned long long)stats->alerts,
              (unsigned long long)stats->wakes, (unsigned long long)stats->sleepers,
              (unsigned long long)stats->wake_errors,
              (unsigned long long)(stats->alerts ? stats->total_ns / stats->alerts : 0),
              (unsigned long long)stats->max_ns, gap, (unsigned long long)stats->gap_unavailable );
    sceKernelDebugOutText( 0, line );
}

static void title_profile_emit_alerts(void)
{
    unsigned int top[WINE_PS5_PROFILE_TOP_THREADS], count = 0, i, j;
    BOOL main_printed = FALSE;

    for (i = 0; i < title_profile_alert_count; i++)
    {
        if (!title_profile_alerts[i].stats.alerts) continue;
        for (j = 0; j < count && title_profile_alerts[top[j]].stats.alerts >=
             title_profile_alerts[i].stats.alerts; j++) {}
        if (j >= ARRAY_SIZE(top)) continue;
        if (count < ARRAY_SIZE(top)) count++;
        memmove( top + j + 1, top + j, (count - j - 1) * sizeof(*top) );
        top[j] = i;
    }
    for (i = 0; i < count; i++)
    {
        if (title_profile_alerts[top[i]].tid == title_profile_main_tid) main_printed = TRUE;
        title_profile_emit_alert( top[i] );
    }
    if (!main_printed)
        for (i = 0; i < title_profile_alert_count; i++)
            if (title_profile_alerts[i].tid == title_profile_main_tid)
            {
                title_profile_emit_alert( i );
                break;
            }
}

static void title_profile_emit_read( unsigned int index )
{
    const struct wine_ps5_read_stats *stats = &title_profile_alerts[index].reads;
    uint64_t native = stats->pread_ns + stats->read_ns;
    unsigned int native_pct = wine_ps5_profile_permille( native, stats->total_ns );
    char line[1024];

    snprintf( line, sizeof(line),
              "wine: title read window tid=%04x calls=%llu bytes=%llu total_ns=%llu "
              "mean_ns=%llu max_ns=%llu pread_ns=%llu read_ns=%llu lookup_ns=%llu "
              "overhead_ns=%llu native_pct=%u.%u native_calls=%llu sizes=%llu/%llu/%llu/%llu "
              "sync=%llu pending=%llu failed=%llu backend_unknown=%llu "
              "classes_casc_other_unknown=%llu/%llu/%llu window_s=10\n",
              title_profile_alerts[index].tid, (unsigned long long)stats->calls,
              (unsigned long long)stats->bytes, (unsigned long long)stats->total_ns,
              (unsigned long long)(stats->calls ? stats->total_ns / stats->calls : 0),
              (unsigned long long)stats->max_ns, (unsigned long long)stats->pread_ns,
              (unsigned long long)stats->read_ns, (unsigned long long)stats->lookup_ns,
              (unsigned long long)(stats->total_ns > native ? stats->total_ns - native : 0),
              native_pct / 10, native_pct % 10, (unsigned long long)stats->native_calls,
              (unsigned long long)stats->sizes[0], (unsigned long long)stats->sizes[1],
              (unsigned long long)stats->sizes[2], (unsigned long long)stats->sizes[3],
              (unsigned long long)stats->sync_calls, (unsigned long long)stats->pending_calls,
              (unsigned long long)stats->failed_calls, (unsigned long long)stats->unknown_backend,
              (unsigned long long)stats->classes[WINE_PS5_READ_CASC],
              (unsigned long long)stats->classes[WINE_PS5_READ_OTHER],
              (unsigned long long)stats->classes[WINE_PS5_READ_UNKNOWN] );
    sceKernelDebugOutText( 0, line );
}

static void title_profile_emit_wake( unsigned int index )
{
    const struct wine_ps5_alert_stats *stats = &title_profile_alerts[index].stats;
    char line[320], wake[192];

    title_profile_histogram( wake, sizeof(wake), stats->wake );
    snprintf( line, sizeof(line), "wine: title wake window tid=%04x wake_us=%s "
              "wake_unavailable=%llu window_s=10\n",
              title_profile_alerts[index].tid, wake, (unsigned long long)stats->wake_unavailable );
    sceKernelDebugOutText( 0, line );
}

static void title_profile_emit_read_and_wake_windows(void)
{
    unsigned int top[6], count, i, j, kind;
    uint64_t totals[WINE_PS5_HANG_THREAD_MAX];
    BOOL main_printed;

    for (kind = 0; kind < 2; kind++)
    {
        count = 0;
        main_printed = FALSE;
        for (i = 0; i < title_profile_alert_count; i++)
        {
            if (!kind) totals[i] = title_profile_alerts[i].reads.calls;
            else
            {
                totals[i] = title_profile_alerts[i].stats.wake_unavailable;
                for (j = 0; j < WINE_PS5_LATENCY_BUCKETS; j++)
                    totals[i] += title_profile_alerts[i].stats.wake[j];
            }
            if (!totals[i]) continue;
            for (j = 0; j < count && totals[top[j]] >= totals[i]; j++) {}
            if (j >= ARRAY_SIZE(top)) continue;
            if (count < ARRAY_SIZE(top)) count++;
            memmove( top + j + 1, top + j, (count - j - 1) * sizeof(*top) );
            top[j] = i;
        }
        for (i = 0; i < count; i++)
        {
            if (title_profile_alerts[top[i]].tid == title_profile_main_tid) main_printed = TRUE;
            if (!kind) title_profile_emit_read( top[i] );
            else title_profile_emit_wake( top[i] );
        }
        if (!main_printed)
            for (i = 0; i < title_profile_alert_count; i++)
                if (title_profile_alerts[i].tid == title_profile_main_tid)
                {
                    if (!kind) title_profile_emit_read( i );
                    else title_profile_emit_wake( i );
                    break;
                }
    }
}

static void title_profile_emit_window(void)
{
    struct title_profile_bucket *top[WINE_PS5_PROFILE_TOP_BUCKETS] = {0};
    struct title_profile_thread *threads[WINE_PS5_PROFILE_TOP_THREADS] = {0};
    unsigned int i, j, tops = 0, thread_tops = 0, cpu_total = 0, cpu_threads = 0;
    char where[96], thread_name[16], line[320];

    for (i = 0; i < ARRAY_SIZE(title_profile_threads); i++)
    {
        const struct title_profile_thread *thread = &title_profile_threads[i];

        if (!thread->total || !thread->cpu_valid || thread->wall_last <= thread->wall_first) continue;
        cpu_total += wine_ps5_profile_cpu_permille(
            thread->cpu_first, thread->cpu_last, thread->wall_first, thread->wall_last );
        cpu_threads++;
    }
    snprintf( line, sizeof(line),
              "title profile window_s=10 samples=%u threads=%u cpu_total_pct=%u.%u cpu_threads=%u\n",
              title_profile_samples, title_profile_thread_count,
              cpu_total / 10, cpu_total % 10, cpu_threads );
    sceKernelDebugOutText( 0, line );
    if (title_profile_overflow)
    {
        snprintf( line, sizeof(line), "title profile overflow=%u\n", title_profile_overflow );
        sceKernelDebugOutText( 0, line );
    }
    title_profile_emit_modules();

    for (i = 0; i < ARRAY_SIZE(title_profile_buckets); i++)
    {
        struct title_profile_bucket *bucket = &title_profile_buckets[i];

        if (!bucket->count) continue;
        for (j = 0; j < tops && top[j]->count >= bucket->count; j++) {}
        if (j >= ARRAY_SIZE(top)) continue;
        if (tops < ARRAY_SIZE(top)) tops++;
        memmove( top + j + 1, top + j, (tops - j - 1) * sizeof(*top) );
        top[j] = bucket;
    }
    for (i = 0; i < tops; i++)
    {
        unsigned int pct = wine_ps5_profile_permille( top[i]->count, title_profile_samples );

        title_profile_format_where( top[i]->kind, top[i]->where,
                                    top[i]->module_base, top[i]->module,
                                    where, sizeof(where) );
        if (top[i]->tid) snprintf( thread_name, sizeof(thread_name), "%04x", top[i]->tid );
        else strcpy( thread_name, "all" );
        snprintf( line, sizeof(line),
                  "title profile top rank=%u pct=%u.%u kind=%s thread=%s where=%s\n",
                  i + 1, pct / 10, pct % 10, title_profile_kind_name( top[i]->kind ),
                  thread_name, where );
        sceKernelDebugOutText( 0, line );
    }

    for (i = 0; i < ARRAY_SIZE(title_profile_threads); i++)
    {
        struct title_profile_thread *thread = &title_profile_threads[i];
        unsigned int busy;

        if (!thread->total) continue;
        busy = thread->user + thread->unix_count;
        for (j = 0; j < thread_tops &&
             threads[j]->user + threads[j]->unix_count >= busy; j++) {}
        if (j >= ARRAY_SIZE(threads)) continue;
        if (thread_tops < ARRAY_SIZE(threads)) thread_tops++;
        memmove( threads + j + 1, threads + j,
                 (thread_tops - j - 1) * sizeof(*threads) );
        threads[j] = thread;
    }
    {
        BOOL main_printed = FALSE;

        for (i = 0; i < thread_tops; i++)
        {
            if (threads[i]->tid == title_profile_main_tid) main_printed = TRUE;
            title_profile_emit_thread( threads[i] );
        }
        if (!main_printed && title_profile_main_tid)
            for (i = 0; i < ARRAY_SIZE(title_profile_threads); i++)
                if (title_profile_threads[i].total &&
                    title_profile_threads[i].tid == title_profile_main_tid)
                {
                    title_profile_emit_thread( &title_profile_threads[i] );
                    break;
                }
    }
    title_profile_emit_alerts();
    title_profile_emit_read_and_wake_windows();
    title_profile_emit_main_unix();
}

static void title_profile_emit_skip(void)
{
    const char *reason = "registry_lock";
    char line[128];

    snprintf( line, sizeof(line), "title profile skipped window_s=10 reason=%s\n", reason );
    sceKernelDebugOutText( 0, line );
}

static void ps5_sample_title_profile(void)
{
    BOOL final_tick;

    if (!title_profile_is_enabled()) return;
    if (!title_profile_ticks)
    {
        title_profile_reset_window();
        /* This is only a fallback for the signal handler's PE-list lookup.
         * A busy virtual_mutex (or no images yet) must not discard startup. */
        virtual_prepare_hang_module_snapshot( NULL, NULL );
    }

    final_tick = title_profile_ticks + 1 >= WINE_PS5_PROFILE_WINDOW_TICKS;
    if (!title_profile_skip && !title_profile_sample_threads( !final_tick ))
        title_profile_skip = TITLE_PROFILE_SKIP_REGISTRY_LOCK;

    if (++title_profile_ticks < WINE_PS5_PROFILE_WINDOW_TICKS) return;
    if (title_profile_skip) title_profile_emit_skip();
    else
    {
        unsigned int i;

        /* Images can arrive after the first tick. Retry at report time, but
         * keep unresolved PCs and all CPU/request samples if still unavailable. */
        virtual_prepare_hang_module_snapshot( NULL, NULL );
        for (i = 0; i < ARRAY_SIZE(title_profile_buckets); i++)
        {
            struct title_profile_bucket *bucket = &title_profile_buckets[i];

            if (!bucket->count || bucket->kind != WINE_PS5_PROFILE_USER ||
                bucket->module_base) continue;
            /* The reporter need not be a Wine thread, so only use the native
             * snapshot here, not the PE-list helper's TEB fault guard. */
            if (!virtual_find_hang_module( bucket->where, &bucket->module_base, bucket->module,
                                           sizeof(bucket->module) ))
            {
                bucket->module_base = 0;
                bucket->module[0] = 0;
            }
        }
        title_profile_emit_window();
    }
    title_event_emit_window();
    title_wait_emit_window();
    title_profile_ticks = 0;
    title_profile_last_generation = 0;
}

static void ps5_dump_title_hang( unsigned int snapshot, unsigned int total )
{
    struct wine_ps5_title_thread *thread;
    unsigned int virtual_mutex_owner = 0;
    ULONG_PTR virtual_mutex_site = 0;
    BOOL primary = wine_ps5_hang_sample_is_primary( snapshot );
    unsigned int modules = primary
        ? virtual_prepare_hang_module_snapshot( &virtual_mutex_owner, &virtual_mutex_site ) : 0;
    unsigned int epoch, threads = 0, signaled = 0, truncated;
    char line[256];

    if (snapshot == 1)
    {
        epoch = __atomic_add_fetch( &title_hang_epoch, 1, __ATOMIC_RELAXED );
        if (!epoch) epoch = __atomic_add_fetch( &title_hang_epoch, 1, __ATOMIC_RELAXED );
    }
    else
        epoch = __atomic_load_n( &title_hang_epoch, __ATOMIC_RELAXED );

    pthread_mutex_lock( &title_threads.mutex );
    for (thread = title_threads.head;
         thread && threads < WINE_PS5_HANG_THREAD_MAX; thread = thread->next, threads++)
    {
        struct wine_ps5_suspend_diagnostic *diagnostic =
            wine_ps5_suspend_diagnostic_from_teb64( (void *)thread->teb );

        __atomic_store_n( &diagnostic->hang_epoch, epoch, __ATOMIC_RELEASE );
        if (snapshot == 1) __atomic_store_n( &diagnostic->hang_valid, 0, __ATOMIC_RELEASE );
        __atomic_store_n( &diagnostic->hang_snapshot, snapshot, __ATOMIC_RELEASE );
        __atomic_store_n( &diagnostic->hang_pending, 1, __ATOMIC_RELEASE );
        if (!pthread_kill( thread->thread, SIGURG )) signaled++;
    }
    truncated = !!thread;
    pthread_mutex_unlock( &title_threads.mutex );
    snprintf( line, sizeof(line),
              "title hang diagnostic=threads event=%u sample=%u/%u phase=%s "
              "threads=%u signaled=%u truncated=%u modules=%u "
              "virtual_mutex_owner=%04x virtual_mutex_site=%016lx\n",
              epoch, snapshot, total, primary ? "full" : "registers",
              threads, signaled, truncated, modules, virtual_mutex_owner, virtual_mutex_site );
    sceKernelDebugOutText( 0, line );
    if (primary && ps5_server_dump_threads) ps5_server_dump_threads( snapshot, total );
}

/* Native child control agents use the same lifetime/TEB-checked local route
 * as the host's in-process server, never a Unix PID signal. */
__attribute__((visibility("default")))
int __wine_ps5_signal_thread( unsigned int thread_id, void *teb, int sig )
{
    return signal_title_thread( thread_id, teb, sig );
}

static void publish_title_startup_channel( enum wine_ps5_title_startup_channel channel, int fd )
{
    sigset_t previous;
    int ret;

    if (!title_runtime.exit ||
        ntdll_get_thread_data()->title_thread.lifetime != title_initial_join.lifetime) return;
    pthread_sigmask( SIG_BLOCK, &ps5_title_block_set, &previous );
    ret = wine_ps5_title_publish_startup_channel( &title_startup, channel, fd );
    pthread_sigmask( SIG_SETMASK, &previous, NULL );
    if (ret) fatal_perror( "cannot publish title startup channel" );
}

static int connect_title_server(void)
{
    char path[PATH_MAX], nls[PATH_MAX];
    struct wine_ps5_title_runtime server_runtime = title_runtime;
    void *module;
    int pair[2];
    int (*start)( int, const char *, const char *, const char *,
                  SIZE_T (*)( const void *, void *, SIZE_T ),
                  NTSTATUS (*)( void *, const void *, SIZE_T ), int (*)( unsigned int, void *, int ),
                  const struct wine_ps5_title_runtime * );

    if (build_dir)
    {
        snprintf( path, sizeof(path), "%s/server/wineserver.so", build_dir );
        snprintf( nls, sizeof(nls), "%s/nls", build_dir );
    }
    else
    {
#ifdef WINE_PS5_TITLE_ROOT
        snprintf( path, sizeof(path), "%s/bin/wineserver.so", WINE_PS5_TITLE_ROOT );
#else
        snprintf( path, sizeof(path), "%s/bin/wineserver.so", title_w_drive );
#endif
        snprintf( nls, sizeof(nls), "%s/nls", data_dir );
    }
    if (!(module = dlopen( path, RTLD_NOW | RTLD_LOCAL )) ||
        !(start = dlsym( module, "wineserver_start_inprocess" )) ||
        !(ps5_server_stop = dlsym( module, "wineserver_stop_inprocess" )) ||
        !(ps5_server_dump_threads = dlsym( module, "wineserver_dump_title_threads" )))
        fatal_error( "cannot load private title wineserver %s: %s\n", path, dlerror() );
    if (WINE_PS5_DIRECT_REQUESTS &&
        !(getenv( "WINEPS5_DIRECT_REQUESTS" ) && !strcmp( getenv( "WINEPS5_DIRECT_REQUESTS" ), "0" )))
        ps5_server_call_direct = dlsym( module, "wineserver_call_direct" );
    TRACE( "private wineserver_start_inprocess=%p\n", start );
    if (server_pipe( pair ) == -1) fatal_perror( "title server socketpair" );
    publish_title_startup_channel( WINE_PS5_TITLE_STARTUP_MESSAGE, pair[0] );
    server_runtime.exit = ps5_title_request_exit;  /* never call loader text without its gate */
    if (start( pair[1], config_dir, server_dir, nls, virtual_uninterrupted_read_memory,
               virtual_uninterrupted_write_memory, signal_title_thread, &server_runtime ) == -1)
        fatal_perror( "cannot start private title wineserver" );
    if (title_runtime.vulkan_proc &&
        (ps5_set_hang_diagnostic =
         (void (*)( void (*)( unsigned int, unsigned int ) ))
         title_runtime.vulkan_proc( "wineSetHangDiagnostic" )))
        ps5_set_hang_diagnostic( ps5_dump_title_hang );
    if (title_runtime.vulkan_proc && getenv( "WINEPS5_PROFILE" ) &&
        !strcmp( getenv( "WINEPS5_PROFILE" ), "1" ) &&
        (ps5_set_thread_profile =
         (void (*)( void (*)(void) ))title_runtime.vulkan_proc( "wineSetThreadProfile" )))
    {
        if (!(ps5_server_mapping_name = dlsym( module, "wineserver_title_mapping_name" )) ||
            !(ps5_server_wake_info = dlsym( module, "wineserver_title_wake_info" )))
            fatal_error( "private title wineserver lacks profile metadata callbacks\n" );
        ps5_profile_cpu_time =
            (int (*)( uint64_t * ))title_runtime.vulkan_proc( "wineGetCurrentThreadCpuTime" );
        ps5_profile_clock_id =
            (int (*)( pthread_t, int * ))title_runtime.vulkan_proc( "wineGetThreadCpuClockId" );
        title_event_report_ns = ps5_title_profile_now_ns();
        title_wait_report_ns = title_event_report_ns;
        __atomic_store_n( &title_profile_enabled, 1, __ATOMIC_RELEASE );
        ps5_set_thread_profile( ps5_sample_title_profile );
    }
    return pair[0];  /* both endpoints belong to this process; don't close pair[1] */
}
#endif

/***********************************************************************
 *           setup_config_dir
 *
 * Setup the wine configuration dir.
 */
static int setup_config_dir(void)
{
    char *p;
    struct stat st;
    int fd_cwd;

#ifdef __PROSPERO__
    if (ps5_direct_enabled())
    {
        if ((fd_cwd = setup_title_prefix()) == -1) fatal_perror( "cannot open title working directory" );
        return fd_cwd;
    }
#endif
    fd_cwd = open( ".", O_RDONLY );

    if (chdir( config_dir ) == -1)
    {
        if (errno != ENOENT) fatal_perror( "cannot use directory %s", config_dir );
        if ((p = strrchr( config_dir, '/' )) && p != config_dir)
        {
            while (p > config_dir + 1 && p[-1] == '/') p--;
            *p = 0;
            if (!stat( config_dir, &st ) && !OWNED_BY_ME(st))
                fatal_error( "'%s' is not owned by you, refusing to create a configuration directory there\n",
                             config_dir );
            *p = '/';
        }
        mkdir( config_dir, 0777 );
        if (chdir( config_dir ) == -1) fatal_perror( "chdir to %s", config_dir );
        MESSAGE( "wine: created the configuration directory '%s'\n", config_dir );
    }

#ifdef __PROSPERO__
    if (stat_dir( config_dir, &st ) == -1) fatal_perror( "stat %s", config_dir );
#else
    if (stat( ".", &st ) == -1) fatal_perror( "stat %s", config_dir );
#endif
    if (!OWNED_BY_ME(st)) fatal_error( "'%s' is not owned by you\n", config_dir );

    server_dir = init_server_dir( st.st_dev, st.st_ino );

#ifdef __PROSPERO__
    mkdir( "drive_c", 0777 );
    init_dosdevices_dir( &st );
    if (fd_cwd == -1)
    {
        char *c_drive;
        if (asprintf( &c_drive, "%s/c:", dosdevices_dir ) != -1)
        {
            fd_cwd = open( c_drive, O_RDONLY );
            free( c_drive );
        }
    }
#else
    if (!mkdir( "dosdevices", 0777 ))
    {
        mkdir( "drive_c", 0777 );
        symlink( "../drive_c", "dosdevices/c:" );
        symlink( "/", "dosdevices/z:" );
    }
    else if (errno != EEXIST) fatal_perror( "cannot create %s/dosdevices", config_dir );

    if (fd_cwd == -1) fd_cwd = open( "dosdevices/c:", O_RDONLY );
#endif
    fcntl( fd_cwd, F_SETFD, FD_CLOEXEC );
    return fd_cwd;
}


/***********************************************************************
 *           server_connect_error
 *
 * Try to display a meaningful explanation of why we couldn't connect
 * to the server.
 */
static void server_connect_error( const char *serverdir )
{
    int fd;
    struct flock fl;

    if ((fd = open( LOCKNAME, O_WRONLY )) == -1)
        fatal_error( "for some mysterious reason, the wine server never started.\n" );

    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 1;
    if (fcntl( fd, F_GETLK, &fl ) != -1)
    {
        if (fl.l_type == F_WRLCK)  /* the file is locked */
            fatal_error( "a wine server seems to be running, but I cannot connect to it.\n"
                         "   You probably need to kill that process (it might be pid %d).\n",
                         (int)fl.l_pid );
        fatal_error( "for some mysterious reason, the wine server failed to run.\n" );
    }
    fatal_error( "the file system of '%s' doesn't support locks,\n"
          "   and there is a 'socket' file in that directory that prevents wine from starting.\n"
          "   You should make sure no wine server is running, remove that file and try again.\n",
                 serverdir );
}


/***********************************************************************
 *           server_connect
 *
 * Attempt to connect to an existing server socket.
 */
static int server_connect(void)
{
    struct sockaddr_un addr;
    struct stat st;
    int s, slen, retry;

    initial_cwd = setup_config_dir();

#ifdef __PROSPERO__
    if (ps5_direct_enabled())
    {
        if (title_runtime.server_socket >= 0)
        {
            /* Trusted bootstrap descriptor; no private server or pathname
             * reconnect. Cancellation must also wake this initial channel. */
            publish_title_startup_channel( WINE_PS5_TITLE_STARTUP_MESSAGE, title_runtime.server_socket );
            return title_runtime.server_socket;
        }
        return connect_title_server();
    }
#endif

    /* chdir to the server directory */
    if (chdir( server_dir ) == -1)
    {
        if (errno != ENOENT) fatal_perror( "chdir to %s", server_dir );
        start_server( TRACE_ON(server) );
#ifdef __PROSPERO__
        /* the server starts in another process, about a second away */
        for (retry = 0; retry < 100 && chdir( server_dir ) == -1; retry++) usleep( 100000 );
#endif
        if (chdir( server_dir ) == -1) fatal_perror( "chdir to %s", server_dir );
    }

    /* make sure we are at the right place */
#ifdef __PROSPERO__
    if (stat_dir( server_dir, &st ) == -1) fatal_perror( "stat %s", server_dir );
#else
    if (stat( ".", &st ) == -1) fatal_perror( "stat %s", server_dir );
#endif
    if (!OWNED_BY_ME(st)) fatal_error( "'%s' is not owned by you\n", server_dir );
    if (!PRIVATE_MODE(st)) fatal_error( "'%s' must not be accessible by other users\n", server_dir );

    for (retry = 0; retry < 6; retry++)
    {
        /* if not the first try, wait a bit to leave the previous server time to exit */
        if (retry)
        {
            usleep( 100000 * retry * retry );
            start_server( TRACE_ON(server) );
            if (lstat( SOCKETNAME, &st ) == -1) continue;  /* still no socket, wait a bit more */
        }
        else if (lstat( SOCKETNAME, &st ) == -1) /* check for an already existing socket */
        {
            if (errno != ENOENT) fatal_perror( "lstat %s/%s", server_dir, SOCKETNAME );
            start_server( TRACE_ON(server) );
            if (lstat( SOCKETNAME, &st ) == -1) continue;  /* still no socket, wait a bit more */
        }

        /* make sure the socket is sane (ISFIFO needed for Solaris) */
        if (!S_ISSOCK(st.st_mode) && !S_ISFIFO(st.st_mode))
            fatal_error( "'%s/%s' is not a socket\n", server_dir, SOCKETNAME );
        if (!OWNED_BY_ME(st))
            fatal_error( "'%s/%s' is not owned by you\n", server_dir, SOCKETNAME );

        /* try to connect to it */
        addr.sun_family = AF_UNIX;
        strcpy( addr.sun_path, SOCKETNAME );
        slen = sizeof(addr) - sizeof(addr.sun_path) + strlen(addr.sun_path) + 1;
#ifdef HAVE_STRUCT_SOCKADDR_UN_SUN_LEN
        addr.sun_len = slen;
#endif
        if ((s = socket( AF_UNIX, SOCK_STREAM, 0 )) == -1) fatal_perror( "socket" );
#ifdef SO_PASSCRED
        else
        {
            int enable = 1;
            setsockopt( s, SOL_SOCKET, SO_PASSCRED, &enable, sizeof(enable) );
        }
#endif
        if (connect( s, (struct sockaddr *)&addr, slen ) != -1)
        {
            fchdir( initial_cwd );  /* switch back to the starting directory */
            fcntl( s, F_SETFD, FD_CLOEXEC );
            return s;
        }
        close( s );
    }
    server_connect_error( server_dir );
}


#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <servers/bootstrap.h>

/* send our task port to the server */
static void send_server_task_port(void)
{
    mach_port_t bootstrap_port, wineserver_port;
    kern_return_t kret;

    struct {
        mach_msg_header_t           header;
        mach_msg_body_t             body;
        mach_msg_port_descriptor_t  task_port;
    } msg;

    if (task_get_bootstrap_port(mach_task_self(), &bootstrap_port) != KERN_SUCCESS) return;

    if (!server_dir)
    {
        struct stat st;
        stat( config_dir, &st );
        server_dir = init_server_dir( st.st_dev, st.st_ino );
    }
    kret = bootstrap_look_up(bootstrap_port, server_dir, &wineserver_port);
    if (kret != KERN_SUCCESS)
        fatal_error( "cannot find the server port: 0x%08x\n", kret );

    mach_port_deallocate(mach_task_self(), bootstrap_port);

    msg.header.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    msg.header.msgh_size        = sizeof(msg);
    msg.header.msgh_remote_port = wineserver_port;
    msg.header.msgh_local_port  = MACH_PORT_NULL;

    msg.body.msgh_descriptor_count  = 1;
    msg.task_port.name              = mach_task_self();
    msg.task_port.disposition       = MACH_MSG_TYPE_COPY_SEND;
    msg.task_port.type              = MACH_MSG_PORT_DESCRIPTOR;

    kret = mach_msg_send(&msg.header);
    if (kret != KERN_SUCCESS)
        server_protocol_error( "mach_msg_send failed: 0x%08x\n", kret );

    mach_port_deallocate(mach_task_self(), wineserver_port);
}
#endif  /* __APPLE__ */


/***********************************************************************
 *           get_unix_tid
 *
 * Retrieve the Unix tid to use on the server side for the current thread.
 */
static int get_unix_tid(void)
{
    int ret = -1;
#ifdef HAVE_PTHREAD_GETTHREADID_NP
    ret = pthread_getthreadid_np();
#elif defined(linux)
    ret = syscall( __NR_gettid );
#elif defined(__sun)
    ret = pthread_self();
#elif defined(__APPLE__)
    ret = mach_thread_self();
    mach_port_deallocate(mach_task_self(), ret);
#elif defined(__NetBSD__)
    ret = _lwp_self();
#elif defined(__FreeBSD__)
    long lwpid;
    thr_self( &lwpid );
    ret = lwpid;
#elif defined(__DragonFly__)
    ret = lwp_gettid();
#endif
    return ret;
}


/***********************************************************************
 *           init_thread_pipe
 *
 * Create the server->client communication pipe.
 */
static int init_thread_pipe(void)
{
    int reply_pipe[2];
    stack_t ss;

    ss.ss_sp    = get_signal_stack();
    ss.ss_size  = signal_stack_size;
    ss.ss_flags = 0;
    sigaltstack( &ss, NULL );

    if (server_pipe( reply_pipe ) == -1) server_protocol_perror( "pipe" );
    if (server_pipe( ntdll_get_thread_data()->wait_fd ) == -1) server_protocol_perror( "pipe" );
#ifdef __PROSPERO__
    publish_title_startup_channel( WINE_PS5_TITLE_STARTUP_REPLY_READ, reply_pipe[0] );
    publish_title_startup_channel( WINE_PS5_TITLE_STARTUP_REPLY_WRITE, reply_pipe[1] );
    publish_title_startup_channel( WINE_PS5_TITLE_STARTUP_WAIT_READ, ntdll_get_thread_data()->wait_fd[0] );
    publish_title_startup_channel( WINE_PS5_TITLE_STARTUP_WAIT_WRITE, ntdll_get_thread_data()->wait_fd[1] );
#endif
    wine_server_send_fd( reply_pipe[1] );
    wine_server_send_fd( ntdll_get_thread_data()->wait_fd[1] );
    ntdll_get_thread_data()->reply_fd = reply_pipe[0];
    return reply_pipe[1];
}


/***********************************************************************
 *           process_exit_wrapper
 *
 * Close server socket and exit process normally.
 */
void process_exit_wrapper( int status )
{
#ifdef __PROSPERO__
    if (title_runtime.exit) ps5_title_request_exit( status );
#endif
    close( fd_socket );
    exit( status );
}


/***********************************************************************
 *           server_init_process
 *
 * Start the server and create the initial socket pair.
 */
size_t server_init_process(void)
{
    struct cpu_topology_override *cpu_override;
    const char *arch = getenv( "WINEARCH" );
    const char *env_socket = getenv( "WINESERVERSOCKET" );
    struct ntdll_thread_data *data = ntdll_get_thread_data();
    obj_handle_t version;
    unsigned int i;
    int ret, reply_pipe;
    struct sigaction sig_act;
    size_t info_size;
    DWORD pid, tid;

    server_pid = -1;
#ifdef __PROSPERO__
    if (ps5_direct_enabled())
    {
        /* Inherited payload sockets are untrusted in title boot.txt. */
        env_socket = NULL;
        unsetenv( "WINESERVERSOCKET" );
        data->pthread_id = pthread_self();
        data->title_thread.lifetime = title_initial_join.lifetime;
    }
#endif
    if (env_socket)
    {
        fd_socket = atoi( env_socket );
        if (fcntl( fd_socket, F_SETFD, FD_CLOEXEC ) == -1)
            fatal_perror( "Bad server socket %d", fd_socket );
        unsetenv( "WINESERVERSOCKET" );
#ifdef __PROSPERO__
        {
            struct stat st;
            if (stat( config_dir, &st ) == -1) fatal_perror( "stat %s", config_dir );
            init_dosdevices_dir( &st );
        }
#endif
    }
    else
    {
        const char *arch = getenv( "WINEARCH" );

        if (is_win64 && arch && !strcmp( arch, "win32" ))
            fatal_error( "WINEARCH is set to 'win32' but this is not supported in wow64 mode.\n" );
        if (arch && strcmp( arch, "win32" ) && strcmp( arch, "win64" ) && strcmp( arch, "wow64" ))
            fatal_error( "WINEARCH set to invalid value '%s', it must be win32, win64, or wow64.\n", arch );

        fd_socket = server_connect();
    }

    /* setup the signal mask */
    sigemptyset( &server_block_set );
    sigaddset( &server_block_set, SIGALRM );
    sigaddset( &server_block_set, SIGIO );
    sigaddset( &server_block_set, SIGINT );
    sigaddset( &server_block_set, SIGHUP );
#ifdef __PROSPERO__
    sigaddset( &server_block_set, SIGQUIT );
#endif
    sigaddset( &server_block_set, SIGUSR1 );
    sigaddset( &server_block_set, SIGUSR2 );
    sigaddset( &server_block_set, SIGCHLD );
    pthread_sigmask( SIG_BLOCK, &server_block_set, NULL );

    /* receive the first thread request fd on the main socket */
    data->request_fd = wine_server_receive_fd( &version );
#ifdef __PROSPERO__
    publish_title_startup_channel( WINE_PS5_TITLE_STARTUP_REQUEST, data->request_fd );
#endif

#ifdef SO_PASSCRED
    /* now that we hopefully received the server_pid, disable SO_PASSCRED */
    {
        int enable = 0;
        setsockopt( fd_socket, SOL_SOCKET, SO_PASSCRED, &enable, sizeof(enable) );
    }
#endif

    if (version != SERVER_PROTOCOL_VERSION)
        server_protocol_error( "version mismatch %d/%d.\n"
                               "Your %s binary was not upgraded correctly,\n"
                               "or you have an older one somewhere in your PATH.\n"
                               "Or maybe the wrong wineserver is still running?\n",
                               version, SERVER_PROTOCOL_VERSION,
                               (version > SERVER_PROTOCOL_VERSION) ? "wine" : "wineserver" );
#if defined(__linux__) && defined(HAVE_PRCTL)
    /* work around Ubuntu's ptrace breakage */
    if (server_pid != -1) prctl( 0x59616d61 /* PR_SET_PTRACER */, server_pid );
#endif

    /* ignore SIGPIPE so that we get an EPIPE error instead  */
    sig_act.sa_handler = SIG_IGN;
    sig_act.sa_flags   = 0;
    sigemptyset( &sig_act.sa_mask );
    sigaction( SIGPIPE, &sig_act, NULL );

    reply_pipe = init_thread_pipe();

    fill_cpu_override();
    cpu_override = get_cpu_topology_override();

    SERVER_START_REQ( init_first_thread )
    {
        if (cpu_override)
            wine_server_add_data( req, cpu_override, sizeof(*cpu_override) );
        req->unix_pid    = getpid();
        req->unix_tid    = get_unix_tid();
        req->reply_fd    = reply_pipe;
        req->wait_fd     = data->wait_fd[1];
        req->debug_level = (TRACE_ON(server) != 0);
        wine_server_set_reply( req, supported_machines, sizeof(supported_machines) );
        if (!(ret = wine_server_call( req )))
        {
            obj_handle_t handle;
            pid               = reply->pid;
            tid               = reply->tid;
            peb->SessionId    = reply->session_id;
            info_size         = reply->info_size;
            server_start_time = reply->server_start;
            supported_machines_count = wine_server_reply_size( reply ) / sizeof(*supported_machines);
            if (reply->inproc_device == FSYNC_USED_BY_SERVER)
            {
                inproc_device_fd = FSYNC_USED_BY_SERVER;
                fsync_init( pid );
            }
            else if (reply->inproc_device)
            {
                inproc_device_fd = wine_server_receive_fd( &handle );
                assert( handle == reply->inproc_device );
            }
        }
    }
    SERVER_END_REQ;
    close( reply_pipe );

    if (ret) server_protocol_error( "init_first_thread failed with status %x\n", ret );

    if (!supported_machines_count)
        fatal_error( "'%s' is a 64-bit installation, it cannot be used with a 32-bit wineserver.\n",
                     config_dir );

    native_machine = supported_machines[0];
    if (is_machine_64bit( native_machine ))
    {
        if (arch && !strcmp( arch, "win32" ))
            fatal_error( "WINEARCH set to win32 but '%s' is a 64-bit installation.\n", config_dir );
#ifndef _WIN64
        NtCurrentTeb()->GdiBatchCount = PtrToUlong( (char *)NtCurrentTeb() - teb_offset );
        NtCurrentTeb()->WowTebOffset  = -teb_offset;
        wow_peb = (PEB64 *)((char *)peb - page_size);
#endif
    }
    else
    {
        if (is_win64)
            fatal_error( "'%s' is a 32-bit installation, it cannot support 64-bit applications.\n", config_dir );
        if (arch && (!strcmp( arch, "win64" ) || !strcmp( arch, "wow64" )))
            fatal_error( "WINEARCH set to %s but '%s' is a 32-bit installation.\n", arch, config_dir );
    }

    set_thread_id( NtCurrentTeb(), pid, tid );

    for (i = 0; i < supported_machines_count; i++)
        if (supported_machines[i] == current_machine) return info_size;

    fatal_error( "wineserver doesn't support the %04x architecture\n", current_machine );
}



/***********************************************************************
 *           server_init_process_done
 */
void server_init_process_done(void)
{
    void *teb;
    unsigned int status;
    int suspend;
    FILE_FS_DEVICE_INFORMATION info;
    struct ntdll_thread_data *thread_data = ntdll_get_thread_data();

    if (!get_device_info( initial_cwd, &info ) && (info.Characteristics & FILE_REMOVABLE_MEDIA))
        chdir( "/" );
    close( initial_cwd );

#ifdef __APPLE__
    send_server_task_port();
#endif

    /* Install signal handlers; this cannot be done earlier, since we cannot
     * send exceptions to the debugger before the create process event that
     * is sent by init_process_done */
    signal_init_process();
    thread_data->syscall_table = KeServiceDescriptorTable;
    thread_data->syscall_trace = TRACE_ON(syscall);

    /* always send the native TEB */
    if (!(teb = NtCurrentTeb64())) teb = NtCurrentTeb();
#ifdef __PROSPERO__
    if (title_runtime.exit)
    {
        sigset_t previous;
        int ret;
        register_title_thread( teb, TRUE );
        pthread_sigmask( SIG_BLOCK, &ps5_title_block_set, &previous );
        ret = wine_ps5_title_complete_startup( &title_startup );
        pthread_sigmask( SIG_SETMASK, &previous, NULL );
        if (ret) fatal_error( "title startup was cancelled\n" );
    }
#endif

    /* Signal the parent process to continue */
    SERVER_START_REQ( init_process_done )
    {
        req->teb = wine_server_client_ptr( teb );
        req->peb = NtCurrentTeb64() ? NtCurrentTeb64()->Peb : wine_server_client_ptr( peb );
        status = wine_server_call( req );
        suspend = reply->suspend;
    }
    SERVER_END_REQ;

    assert( !status );
    if (peb->BeingDebugged)
    {
        peb->ProcessParameters->Flags &= ~PROCESS_PARAMS_IMAGE_KEY_MISSING;
        if (wow_peb)
        {
#ifdef _WIN64
            RTL_USER_PROCESS_PARAMETERS32 *wow64_params = ULongToPtr(wow_peb->ProcessParameters);
#else
            RTL_USER_PROCESS_PARAMETERS64 *wow64_params = ULongToPtr(wow_peb->ProcessParameters);
#endif
            wow64_params->Flags &= ~PROCESS_PARAMS_IMAGE_KEY_MISSING;
        }
    }
    signal_start_thread( main_image_info.TransferAddress, peb, suspend, NtCurrentTeb() );
}


/***********************************************************************
 *           server_init_thread
 *
 * Send an init thread request.
 */
void server_init_thread( void *entry_point, BOOL *suspend )
{
    void *teb;
    int reply_pipe = init_thread_pipe();

    /* always send the native TEB */
    if (!(teb = NtCurrentTeb64())) teb = NtCurrentTeb();
#ifdef __PROSPERO__
    if (title_runtime.exit) register_title_thread( teb, FALSE );
#endif

    SERVER_START_REQ( init_thread )
    {
        req->unix_tid  = get_unix_tid();
        req->teb       = wine_server_client_ptr( teb );
        req->entry     = wine_server_client_ptr( entry_point );
        req->reply_fd  = reply_pipe;
        req->wait_fd   = ntdll_get_thread_data()->wait_fd[1];
        wine_server_call( req );
        *suspend = reply->suspend;
    }
    SERVER_END_REQ;
    close( reply_pipe );
}

NTSTATUS WINAPI NtAllocateReserveObject( HANDLE *handle, const OBJECT_ATTRIBUTES *attr,
                                         MEMORY_RESERVE_OBJECT_TYPE type )
{
    struct object_attributes *objattr;
    unsigned int ret;
    data_size_t len;

    TRACE("(%p, %p, %d)\n", handle, attr, type);

    *handle = 0;
    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( allocate_reserve_object )
    {
        req->type = type;
        wine_server_add_data( req, objattr, len );
        if (!(ret = wine_server_call( req )))
            *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    free( objattr );
    return ret;
}


/******************************************************************************
 *           NtDuplicateObject
 */
NTSTATUS WINAPI NtDuplicateObject( HANDLE source_process, HANDLE source, HANDLE dest_process, HANDLE *dest,
                                   ACCESS_MASK access, ULONG attributes, ULONG options )
{
    sigset_t sigset;
    unsigned int ret;
    int fd = -1;

    if (dest) *dest = 0;
#ifdef __PROSPERO__
    if (source_process == NtCurrentProcess())
        ret = ps5_title_flush_coalesced_write_reason(
            source, WINE_PS5_TITLE_BUFFER_BARRIER_DUPLICATE );
    else
        ret = ps5_title_flush_all_coalesced_writes_reason(
            WINE_PS5_TITLE_BUFFER_BARRIER_DUPLICATE );
    if (ret) return ret;
#endif

    if ((options & DUPLICATE_CLOSE_SOURCE) && source_process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.dup_handle.type        = APC_DUP_HANDLE;
        call.dup_handle.src_handle  = wine_server_obj_handle( source );
        call.dup_handle.dst_process = wine_server_obj_handle( dest_process );
        call.dup_handle.access      = access;
        call.dup_handle.attributes  = attributes;
        call.dup_handle.options     = options;
        ret = server_queue_process_apc( source_process, &call, &result );
        if (ret != STATUS_SUCCESS) return ret;

        if (!result.dup_handle.status)
            *dest = wine_server_ptr_handle( result.dup_handle.handle );
        return result.dup_handle.status;
    }

    /* hold fd_cache_mutex to prevent the fd from being added again between the
     * call to remove_fd_from_cache and close_handle */
    server_enter_uninterrupted_section( &fd_cache_mutex, &sigset );

    /* always remove the cached fd; if the server request fails we'll just
     * retrieve it again */
    if (options & DUPLICATE_CLOSE_SOURCE)
    {
        fd = remove_fd_from_cache( source );
        close_inproc_sync( source );
    }

    SERVER_START_REQ( dup_handle )
    {
        req->src_process = wine_server_obj_handle( source_process );
        req->src_handle  = wine_server_obj_handle( source );
        req->dst_process = wine_server_obj_handle( dest_process );
        req->access      = access;
        req->attributes  = attributes;
        req->options     = options;
        if (!(ret = wine_server_call( req )))
        {
            if (dest) *dest = wine_server_ptr_handle( reply->handle );
        }
    }
    SERVER_END_REQ;
#ifdef __PROSPERO__
    if (!ret && source_process == NtCurrentProcess() && (options & DUPLICATE_CLOSE_SOURCE))
        ps5_title_close_coalesced_write( source );
#endif

    server_leave_uninterrupted_section( &fd_cache_mutex, &sigset );

    if (fd != -1) close( fd );
    return ret;
}


/**************************************************************************
 *           NtCompareObjects   (NTDLL.@)
 */
NTSTATUS WINAPI NtCompareObjects( HANDLE first, HANDLE second )
{
    unsigned int status;

    SERVER_START_REQ( compare_objects )
    {
        req->first = wine_server_obj_handle( first );
        req->second = wine_server_obj_handle( second );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;

    return status;
}


/**************************************************************************
 *           NtCompareTokens   (NTDLL.@)
 */
NTSTATUS WINAPI NtCompareTokens( HANDLE first, HANDLE second, BOOLEAN *equal )
{
    FIXME( "%p,%p,%p: stub\n", first, second, equal );
    return STATUS_NOT_IMPLEMENTED;
}


/**************************************************************************
 *           NtClose
 */
NTSTATUS WINAPI NtClose( HANDLE handle )
{
    sigset_t sigset;
    HANDLE port;
    unsigned int ret;
    int fd;
#ifdef __PROSPERO__
    NTSTATUS coalesce_status;
#endif

    if (HandleToLong( handle ) >= ~5 && HandleToLong( handle ) <= ~0)
        return STATUS_SUCCESS;
#ifdef __PROSPERO__
    coalesce_status = ps5_title_close_coalesced_write( handle );
#endif

    /* hold fd_cache_mutex to prevent the fd from being added again between the
     * call to remove_fd_from_cache and close_handle */
    server_enter_uninterrupted_section( &fd_cache_mutex, &sigset );

    /* always remove the cached fd; if the server request fails we'll just
     * retrieve it again */
    fd = remove_fd_from_cache( handle );
    close_inproc_sync( handle );

    SERVER_START_REQ( close_handle )
    {
        req->handle = wine_server_obj_handle( handle );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;

    server_leave_uninterrupted_section( &fd_cache_mutex, &sigset );

    if (fd != -1) close( fd );
#ifdef __PROSPERO__
    if (!ret && coalesce_status) ret = coalesce_status;
#endif

    if (ret != STATUS_INVALID_HANDLE || !handle) return ret;
    if (!peb->BeingDebugged) return ret;
    if (!NtQueryInformationProcess( NtCurrentProcess(), ProcessDebugPort, &port, sizeof(port), NULL) && port)
    {
        NtCurrentTeb()->ExceptionCode = ret;
        call_raise_user_exception_dispatcher();
    }
    return ret;
}

#ifdef _WIN64

struct __server_request_info32
{
    union
    {
        union generic_request req;
        union generic_reply   reply;
    } u;
    unsigned int            data_count;
    ULONG                   reply_data;
    struct { ULONG ptr; data_size_t size; } data[__SERVER_MAX_DATA];
};

/**********************************************************************
 *		wow64_wine_server_call
 */
NTSTATUS wow64_wine_server_call( void *args )
{
    struct __server_request_info32 *req32 = args;
    unsigned int i;
    NTSTATUS status;
    struct __server_request_info req;

    req.u.req = req32->u.req;
    req.data_count = req32->data_count;
    for (i = 0; i < req.data_count; i++)
    {
        req.data[i].ptr = ULongToPtr( req32->data[i].ptr );
        req.data[i].size = req32->data[i].size;
    }
    req.reply_data = ULongToPtr( req32->reply_data );
    status = wine_server_call( &req );
    req32->u.reply = req.u.reply;
    return status;
}

/***********************************************************************
 *		wow64_wine_server_fd_to_handle
 */
NTSTATUS wow64_wine_server_fd_to_handle( void *args )
{
    struct
    {
        int          fd;
        unsigned int access;
        unsigned int attributes;
        ULONG        handle;
    } const *params32 = args;

    ULONG *handle32 = ULongToPtr( params32->handle );
    HANDLE handle;
    NTSTATUS ret;

    ret = wine_server_fd_to_handle( params32->fd, params32->access, params32->attributes, &handle );
    *handle32 = HandleToULong( handle );
    return ret;
}

/**********************************************************************
 *           wow64_wine_server_handle_to_fd
 */
NTSTATUS wow64_wine_server_handle_to_fd( void *args )
{
    struct
    {
        ULONG        handle;
        unsigned int access;
        ULONG        unix_fd;
        ULONG        options;
    } const *params32 = args;

    return wine_server_handle_to_fd( ULongToHandle( params32->handle ), params32->access,
                                     ULongToPtr( params32->unix_fd ), ULongToPtr( params32->options ));
}

#endif /* _WIN64 */
