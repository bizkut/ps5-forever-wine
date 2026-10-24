/*
 * Title-only PS5 direct-backed Wine view lifecycle
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

#ifdef __PROSPERO__

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "unix_private.h"

#define DIRECT_UNIT_SIZE PS5_KERNEL_DIRECT_ALIGNMENT
#define DIRECT_RW (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)
#define DIRECT_METADATA_SIZE ((size_t)0x200000)
#define DIRECT_TEST_BASE ((void *)(uintptr_t)UINT64_C(0x1000000000))
#define DIRECT_LARGE_BASE ((void *)(uintptr_t)UINT64_C(0x1400000000))
#define DIRECT_TEST_SIZE ((size_t)1 << 30)
#define DIRECT_LARGE_SIZE ((size_t)4 << 30)
#define DIRECT_LARGE_CHUNK ((size_t)64 << 20)
#define MIB_ARGS(bytes) ((bytes) >> 20), (((bytes) & 0xfffff) * 10000 >> 20)

/* One metadata allocation per original reservation, shared by its split views.
 * Physical offsets and three bitmaps (owned, backed, pending commit) cost 8 3/8
 * bytes per 64 KiB. A commit never allocates metadata. Unexpected kernel
 * placements remain owned separately until their unmap succeeds.
 *
 * Like virtual.c's view blocks, metadata blocks are retained and reused, not
 * returned to mmap on every view deletion. The initial block is allocated
 * before the native test takes its pool baseline. All access, including the
 * free list and reference count, is serialized by virtual_mutex. */
struct ps5_direct_view
{
    struct ps5_direct_view *next;
    size_t capacity;
    void *base;
    size_t units;
    unsigned int refs;
    size_t failed_unit;
    void *unexpected_mapping;
    void *unexpected_reservation;
    int64_t physical[];
};

struct ps5_direct_pools
{
    size_t direct;
    size_t flexible;
    int valid;
};

static int direct_memory_enabled;
static int direct_memory_initialized;
static int direct_test_enabled;
static int direct_chunked_reserve;
static int64_t direct_memory_size;
static char *metadata_next, *metadata_end;
static struct ps5_direct_view *free_metadata;

/* Allocation-stage census for CPU profiling. virtual_mutex serializes every
 * writer; disabled titles pay no clock reads or diagnostic native calls. */
static struct
{
    uint64_t since, views, requested_units, metadata_capacity, metadata_ns;
    uint64_t backing_ns, units, allocate_ns, map_ns, zero_ns, hide_ns, largest_units, hide_calls;
} direct_profile;

static void direct_profile_report( uint64_t now )
{
    char line[512];

    if (!direct_profile.since) direct_profile.since = now;
    if (now - direct_profile.since < UINT64_C(10000000000)) return;
    snprintf( line, sizeof(line),
              "title direct allocation window_ms=%llu views=%llu requested_kib=%llu "
              "metadata_capacity_kib=%llu metadata_ms=%llu backing_ms=%llu units=%llu "
              "allocate_ms=%llu map_ms=%llu zero_ms=%llu hide_ms=%llu largest_kib=%llu hide_calls=%llu\n",
              (unsigned long long)((now - direct_profile.since) / 1000000),
              (unsigned long long)direct_profile.views,
              (unsigned long long)(direct_profile.requested_units * DIRECT_UNIT_SIZE / 1024),
              (unsigned long long)(direct_profile.metadata_capacity * DIRECT_UNIT_SIZE / 1024),
              (unsigned long long)(direct_profile.metadata_ns / 1000000),
              (unsigned long long)(direct_profile.backing_ns / 1000000),
              (unsigned long long)direct_profile.units,
              (unsigned long long)(direct_profile.allocate_ns / 1000000),
              (unsigned long long)(direct_profile.map_ns / 1000000),
              (unsigned long long)(direct_profile.zero_ns / 1000000),
              (unsigned long long)(direct_profile.hide_ns / 1000000),
              (unsigned long long)(direct_profile.largest_units * DIRECT_UNIT_SIZE / 1024),
              (unsigned long long)direct_profile.hide_calls );
    sceKernelDebugOutText( 0, line );
    memset( &direct_profile, 0, sizeof(direct_profile) );
    direct_profile.since = now;
}

/* stderr is a pipe in the title; its undrained tail is lost at process exit. */
static void __attribute__((format(printf, 1, 2))) direct_log( const char *format, ... )
{
    char line[1024];
    va_list args;

    va_start( args, format );
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );
    sceKernelDebugOutText( 0, line );
}

static int32_t direct_error( const char *stage, const char *operation, int32_t result )
{
    direct_log( "wine: ps5-direct failure stage=%s operation=%s result=0x%08x\n",
                stage, operation, (unsigned int)result );
    return result;
}

static int32_t direct_wrong_address( const char *stage, const char *operation,
                                     void *expected, void *actual )
{
    direct_log( "wine: ps5-direct failure stage=%s operation=%s result=0x00000000 "
                "exact=0 expected=%p actual=%p\n", stage, operation, expected, actual );
    return -1;
}

/* WINE_PS5_DIRECT_FAIL_FAST_TEST_BEGIN */
void ps5_direct_fail_fast(void)
{
    /* SIGKILL applies to the native process, even when sent to this thread.
     * It cannot enter Wine/loader exit callbacks or wait for children/menu. */
    sceKernelDebugOutText( 0, "wine: fatal direct memory state; SIGKILL current native process\n" );
    pthread_kill( pthread_self(), SIGKILL );
    __builtin_trap(); /* Never select orderly shutdown if signal dispatch returns. */
}
/* WINE_PS5_DIRECT_FAIL_FAST_TEST_END */

static BYTE *direct_bitmap( const struct ps5_direct_view *view, unsigned int bitmap )
{
    return (BYTE *)(view->physical + view->capacity) + bitmap * (view->capacity / 8);
}

static BOOL direct_bit( const struct ps5_direct_view *view, unsigned int bitmap, size_t unit )
{
    return !!(direct_bitmap( view, bitmap )[unit / 8] & (1 << (unit % 8)));
}

static void direct_set_bit( struct ps5_direct_view *view, unsigned int bitmap, size_t unit, BOOL set )
{
    BYTE *byte = direct_bitmap( view, bitmap ) + unit / 8;

    if (set) *byte |= 1 << (unit % 8);
    else *byte &= ~(1 << (unit % 8));
}

static size_t direct_unit_index( const struct ps5_direct_view *view, const void *addr )
{
    size_t unit = ((const char *)addr - (const char *)view->base) / DIRECT_UNIT_SIZE;

    assert( addr >= view->base && unit < view->units );
    return unit;
}

static BOOL direct_metadata_block( size_t size )
{
    void *ptr;

    size = (max( size, DIRECT_METADATA_SIZE ) + PS5_KERNEL_PAGE_SIZE - 1) &
           ~(PS5_KERNEL_PAGE_SIZE - 1);
    if ((ptr = anon_mmap_alloc( size, PROT_READ | PROT_WRITE )) == MAP_FAILED)
    {
        direct_log( "wine: ps5-direct failure stage=metadata operation=mmap errno=%d size=%zu\n",
                    errno, size );
        return FALSE;
    }
    metadata_next = ptr;
    metadata_end = metadata_next + size;
    return TRUE;
}

BOOL ps5_direct_enabled(void)
{
    return direct_memory_enabled;
}

/* Keep the startup counters at the native-call boundary: one direct-view
 * operation can reserve/map/protect/unmap many individual backing units. */
static int title_startup_unix_prot( int prot )
{
    return ((prot & PS5_KERNEL_PROT_CPU_READ) ? PROT_READ : 0) |
           ((prot & PS5_KERNEL_PROT_CPU_WRITE) ? PROT_WRITE : 0) |
           ((prot & PS5_KERNEL_PROT_CPU_EXEC) ? PROT_EXEC : 0);
}

static int32_t title_startup_reserve( void **addr, size_t size, int flags, size_t alignment )
{
    uint64_t start = ps5_title_profile_now_ns();
    int32_t ret = sceKernelReserveVirtualRange( addr, size, flags, alignment );
    uint64_t elapsed = ps5_title_profile_now_ns() - start;

    ps5_title_startup_host_call( PS5_TITLE_HOST_RESERVE, *addr, size, 0, !ret, elapsed );
    return ret;
}

static int32_t title_startup_direct_map( void **addr, size_t size, int prot, int flags,
                                       int64_t offset, size_t alignment )
{
    uint64_t start = ps5_title_profile_now_ns();
    int32_t ret = sceKernelMapDirectMemory( addr, size, prot, flags, offset, alignment );
    uint64_t elapsed = ps5_title_profile_now_ns() - start;

    ps5_title_startup_host_call( PS5_TITLE_HOST_MAP, *addr, size, title_startup_unix_prot( prot ),
                                !ret, elapsed );
    return ret;
}

static int32_t title_startup_direct_protect( void *addr, size_t size, int prot )
{
    uint64_t start = ps5_title_profile_now_ns();
    int32_t ret = sceKernelMprotect( addr, size, prot );
    uint64_t elapsed = ps5_title_profile_now_ns() - start;

    ps5_title_startup_host_call( PS5_TITLE_HOST_PROTECT, addr, size, title_startup_unix_prot( prot ),
                                !ret, elapsed );
    return ret;
}

static int32_t title_startup_direct_unmap( void *addr, size_t size )
{
    uint64_t start = ps5_title_profile_now_ns();
    int32_t ret = sceKernelMunmap( addr, size );
    uint64_t elapsed = ps5_title_profile_now_ns() - start;

    ps5_title_startup_host_call( PS5_TITLE_HOST_UNMAP, addr, size, 0, !ret, elapsed );
    return ret;
}

#define sceKernelReserveVirtualRange(a,s,f,l) \
    (__atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED ) ? \
     title_startup_reserve(a,s,f,l) : sceKernelReserveVirtualRange(a,s,f,l))
#define sceKernelMapDirectMemory(a,s,p,f,o,l) \
    (__atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED ) ? \
     title_startup_direct_map(a,s,p,f,o,l) : sceKernelMapDirectMemory(a,s,p,f,o,l))
#define sceKernelMprotect(a,s,p) (__atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED ) ? \
                                title_startup_direct_protect(a,s,p) : sceKernelMprotect(a,s,p))
#define sceKernelMunmap(a,s) (__atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED ) ? \
                             title_startup_direct_unmap(a,s) : sceKernelMunmap(a,s))

/* WINE_PS5_CHUNKED_RESERVE_TEST_BEGIN */
/* Never use MAP_FIXED here: a kernel-only mapping can occupy a Wine-free
 * candidate. Hints plus exact-placement checks preserve it. Reservations are
 * owned by the existing per-64-KiB view bitmap, independently of their native
 * chunk boundaries, so split/release/decommit keep the same ownership model. */
static int direct_reserve_chunks( void *base, size_t size )
{
    const size_t chunk_size = (size_t)1 << 30;
    size_t offset = 0, count;
    void *placed;
    int32_t result;

    while (offset < size)
    {
        count = min( size - offset, chunk_size );
        placed = (char *)base + offset;
        result = sceKernelReserveVirtualRange( &placed, count, 0, DIRECT_UNIT_SIZE );
        if (result) break;
        if (placed != (char *)base + offset)
        {
            if (sceKernelMunmap( placed, count )) ps5_direct_fail_fast();
            break;
        }
        offset += count;
    }
    if (offset == size)
    {
        direct_log( "wine: ps5-direct chunk-reserve base=%p size=%zu chunk=%zu result=0\n",
                    base, size, chunk_size );
        return 0;
    }
    /* Even ENOMEM may be placement-dependent. Let virtual.c's bounded,
     * alignment/limit-aware free-area search try its next candidate. */
    direct_log( "wine: ps5-direct chunk-reserve base=%p size=%zu owned=%zu "
                "result=0x%08x actual=%p rollback=1\n", base, size, offset,
                (unsigned int)result, placed );
    while (offset)
    {
        count = min( offset, chunk_size );
        offset -= count;
        if (sceKernelMunmap( (char *)base + offset, count )) ps5_direct_fail_fast();
    }
    return EEXIST;
}

void *ps5_direct_reserve( void *base, size_t size )
{
    void *placed = base;
    int32_t result;

    assert( direct_memory_enabled && !((uintptr_t)base % DIRECT_UNIT_SIZE) &&
            !(size % DIRECT_UNIT_SIZE) );
    result = sceKernelReserveVirtualRange( &placed, size, 0, DIRECT_UNIT_SIZE );
    if (result)
    {
        if (direct_chunked_reserve && result == (int32_t)0x8002000c && size > ((size_t)4 << 30))
        {
            if (!(errno = direct_reserve_chunks( base, size ))) return base;
            return MAP_FAILED;
        }
        direct_error( "view-reserve", "reserve", result );
        errno = result == (int32_t)0x8002000c ? ENOMEM : EEXIST;
        return MAP_FAILED;
    }
    if (placed == base) return base;
    /* The hint was occupied and the kernel placed the range elsewhere. This is
     * the EEXIST miss anon_mmap_tryfixed() reports silently on the ordinary
     * path; try_map_free_area() probes the next candidate. Not a failure. */
    if ((result = sceKernelMunmap( placed, size )))
    {
        direct_error( "view-reserve", "unmap-unexpected-reservation", result );
        if (direct_chunked_reserve) ps5_direct_fail_fast();
    }
    errno = EEXIST;
    return MAP_FAILED;
}
/* WINE_PS5_CHUNKED_RESERVE_TEST_END */

struct ps5_direct_view *ps5_direct_create_view( void *base, size_t size )
{
    struct ps5_direct_view *view, **prev;
    size_t units = size / DIRECT_UNIT_SIZE, capacity = (units + 63) & ~(size_t)63;
    size_t bytes = sizeof(*view) + capacity * sizeof(view->physical[0]) + 3 * capacity / 8;
    uint64_t profile_started = title_profile_is_enabled() ? ps5_title_profile_now_ns() : 0;

    assert( direct_memory_enabled && !(size % DIRECT_UNIT_SIZE) );
    for (prev = &free_metadata; (view = *prev); prev = &view->next)
        if (view->capacity >= capacity)
        {
            *prev = view->next;
            capacity = view->capacity;
            break;
        }
    if (!view)
    {
        if ((!metadata_next || (size_t)(metadata_end - metadata_next) < bytes) &&
            !direct_metadata_block( bytes )) return NULL;
        view = (struct ps5_direct_view *)metadata_next;
        metadata_next += bytes;
    }
    memset( view, 0, sizeof(*view) );
    view->capacity = capacity;
    view->base = base;
    view->units = units;
    view->refs = 1;
    view->failed_unit = units;
    memset( view->physical, 0xff, capacity * sizeof(view->physical[0]) );
    memset( direct_bitmap( view, 0 ), 0xff, capacity / 8 );
    memset( direct_bitmap( view, 1 ), 0, 2 * capacity / 8 );
    if (profile_started)
    {
        direct_profile.views++;
        direct_profile.requested_units += units;
        direct_profile.metadata_capacity += capacity;
        if (units > direct_profile.largest_units) direct_profile.largest_units = units;
        direct_profile.metadata_ns += ps5_title_profile_now_ns() - profile_started;
    }
    return view;
}

void ps5_direct_retain_view( struct ps5_direct_view *view )
{
    view->refs++;
}

void ps5_direct_free_view( struct ps5_direct_view *view )
{
    size_t i;

    if (--view->refs) return;
    for (i = 0; i < view->units; i++)
        assert( !direct_bit( view, 0, i ) && !direct_bit( view, 1, i ) && view->physical[i] < 0 );
    assert( !view->unexpected_mapping && !view->unexpected_reservation );
    view->next = free_metadata;
    free_metadata = view;
}

BOOL ps5_direct_is_backed( const struct ps5_direct_view *view, const void *addr )
{
    return direct_bit( view, 1, direct_unit_index( view, addr ) );
}

BOOL ps5_direct_drop_unit( struct ps5_direct_view *view, void *addr, BOOL reserve )
{
    size_t unit = direct_unit_index( view, addr );
    void *placed;
    int32_t result;
    BOOL ret = TRUE;

    assert( !((uintptr_t)addr % DIRECT_UNIT_SIZE) );
    if (view->failed_unit != unit && (view->unexpected_mapping || view->unexpected_reservation))
        if (!ps5_direct_drop_unit( view, (char *)view->base + view->failed_unit * DIRECT_UNIT_SIZE, TRUE ))
            return FALSE;  /* never overwrite an exceptional placement's ownership */
    if (reserve && direct_bit( view, 0, unit ) && !direct_bit( view, 1, unit ) &&
        !direct_bit( view, 2, unit ) && view->physical[unit] < 0 && view->failed_unit != unit)
        return TRUE;
    if (view->failed_unit == unit)
    {
        if (view->unexpected_mapping)
        {
            result = sceKernelMunmap( view->unexpected_mapping, DIRECT_UNIT_SIZE );
            if (result)
            {
                direct_error( "view-drop", "unmap-unexpected-mapping", result );
                return FALSE;
            }
            view->unexpected_mapping = NULL;
        }
        if (view->unexpected_reservation)
        {
            result = sceKernelMunmap( view->unexpected_reservation, DIRECT_UNIT_SIZE );
            if (result)
            {
                direct_error( "view-drop", "unmap-unexpected-reservation", result );
                return FALSE;
            }
            view->unexpected_reservation = NULL;
        }
        view->failed_unit = view->units;
    }
    if (direct_bit( view, 0, unit ))
    {
        result = sceKernelMunmap( addr, DIRECT_UNIT_SIZE );
        if (result)
        {
            direct_error( "view-drop", "unmap-unit", result );
            return FALSE;  /* physical memory must remain under a live mapping */
        }
        direct_set_bit( view, 0, unit, FALSE );
        direct_set_bit( view, 1, unit, FALSE );
    }
    if (view->physical[unit] >= 0)
    {
        result = sceKernelReleaseDirectMemory( view->physical[unit], DIRECT_UNIT_SIZE );
        if (result)
        {
            direct_error( "view-drop", "release-physical", result );
            ret = FALSE;  /* keep the offset for another cleanup attempt */
        }
        else view->physical[unit] = -1;
    }
    if (reserve)
    {
        placed = addr;
        result = sceKernelReserveVirtualRange( &placed, DIRECT_UNIT_SIZE, PS5_KERNEL_MAP_FIXED,
                                               DIRECT_UNIT_SIZE );
        if (result)
        {
            direct_error( "view-drop", "restore-reservation", result );
            ret = FALSE;
        }
        else if (placed != addr)
        {
            direct_wrong_address( "view-drop", "restore-reservation", addr, placed );
            view->failed_unit = unit;
            view->unexpected_reservation = placed;
            ret = FALSE;
        }
        else direct_set_bit( view, 0, unit, TRUE );
    }
    if (ret) direct_set_bit( view, 2, unit, FALSE );
    return ret;
}

BOOL ps5_direct_is_whole_backed( const struct ps5_direct_view *view, void *base, size_t size )
{
    size_t i;

    if (view->refs != 1 || view->base != base || view->units * DIRECT_UNIT_SIZE != size ||
        view->failed_unit != view->units || view->unexpected_mapping || view->unexpected_reservation)
        return FALSE;
    for (i = 0; i < view->units; i++)
        if (!direct_bit( view, 0, i ) || !direct_bit( view, 1, i ) ||
            direct_bit( view, 2, i ) || view->physical[i] < 0) return FALSE;
    return TRUE;
}

/* Detached cached views have no RB-tree neighbours. Release only their own
 * retained units; a failed unmap/release remains represented for retry. */
BOOL ps5_direct_release_whole( struct ps5_direct_view *view )
{
    size_t i;
    BOOL ret = TRUE;

    assert( view->refs == 1 );
    for (i = 0; i < view->units; i++)
        if (!ps5_direct_drop_unit( view, (char *)view->base + i * DIRECT_UNIT_SIZE, FALSE ))
            ret = FALSE;
    return ret;
}

BOOL ps5_direct_cache_pressure(void)
{
    int64_t physical = 0;
    size_t largest = 0;

    /* Conservative under fragmentation or a failed query: never retain a
     * cache when even a 256-MiB direct allocation cannot be satisfied. */
    return sceKernelAvailableDirectMemorySize( 0, direct_memory_size, DIRECT_UNIT_SIZE,
                                               &physical, &largest ) || largest < ((size_t)256 << 20);
}

/* Captured from the actual loaded image before entering Windows code.
 * Unknown images, Battle.net and CEF retain the ordinary allocation path. */
BOOL ps5_title_agent_process;

/* WINE_PS5_DIRECT_TUNE_TEST_BEGIN */
static uint64_t direct_commit_tune_ns;
static int direct_commit_batch = 1;
static int direct_cache = 1;

/* Same exact-line, once-per-second Unix-side tune convention as file.c. */
static void title_direct_tune_poll(void)
{
    uint64_t now;
    const char *root;
    char path[PATH_MAX], text[1024], *line, *next;
    ssize_t length;
    int fd, path_len;

    if (!__wine_ps5_is_title() || !(root = ps5_title_drive( 'c' ))) return;
    now = ps5_title_profile_now_ns();
    if (direct_commit_tune_ns && now - direct_commit_tune_ns < UINT64_C(1000000000)) return;
    direct_commit_tune_ns = now;
    if ((path_len = snprintf( path, sizeof(path), "%s/wineps5-tune.txt", root )) < 0 ||
        path_len >= (int)sizeof(path) || (fd = open( path, O_RDONLY )) == -1) return;
    length = read( fd, text, sizeof(text) - 1 );
    close( fd );
    if (length <= 0) return;
    text[length] = 0;
    for (line = text; *line; line = next)
    {
        next = strchr( line, '\n' );
        if (next) *next++ = 0;
        else next = line + strlen( line );
        if (*line && line[strlen( line ) - 1] == '\r') line[strlen( line ) - 1] = 0;
        if (!strcmp( line, "direct_commit_batch=off" )) direct_commit_batch = 0;
        else if (!strcmp( line, "direct_commit_batch=on" )) direct_commit_batch = 1;
        else if (!strcmp( line, "direct_cache=off" )) direct_cache = 0;
        else if (!strcmp( line, "direct_cache=on" )) direct_cache = 1;
    }
}

static int title_direct_commit_batch_enabled(void)
{
    title_direct_tune_poll();
    return __wine_ps5_is_title() && direct_commit_batch;
}

BOOL ps5_direct_cache_enabled(void)
{
    if (!__atomic_load_n( &ps5_title_agent_process, __ATOMIC_ACQUIRE )) return FALSE;
    title_direct_tune_poll();
    return __wine_ps5_is_title() && direct_cache;
}
/* WINE_PS5_DIRECT_TUNE_TEST_END */

BOOL ps5_direct_finish_commit( struct ps5_direct_view *view, void *base, size_t size, BOOL rollback )
{
    size_t first = direct_unit_index( view, base );
    size_t last = direct_unit_index( view, (char *)base + size - 1 ), i;
    BOOL ret = TRUE;

    for (i = first; i <= last; i++)
    {
        if (!direct_bit( view, 2, i )) continue;
        if (rollback)
        {
            if (!ps5_direct_drop_unit( view, (char *)view->base + i * DIRECT_UNIT_SIZE, TRUE ))
                ret = FALSE;
        }
        else direct_set_bit( view, 2, i, FALSE );
    }
    return ret;
}

BOOL ps5_direct_ensure_backed( struct ps5_direct_view *view, void *base, size_t size )
{
    size_t first = direct_unit_index( view, base );
    size_t last = direct_unit_index( view, (char *)base + size - 1 ), i;
    void *addr, *placed;
    int32_t result;
    int64_t physical;
    uint64_t profile_started = title_profile_is_enabled() ? ps5_title_profile_now_ns() : 0;
    uint64_t stage_started = 0;
    char *hide_start = NULL;
    size_t hide_size = 0;
    BOOL batch = title_direct_commit_batch_enabled();

    /* Retry ownership-preserving cleanup from a previous failed rollback
     * before starting a new transaction on those units. */
    if (!ps5_direct_finish_commit( view, base, size, TRUE )) return FALSE;
    for (i = first; i <= last; i++)
    {
        if (direct_bit( view, 1, i )) continue;
        addr = (char *)view->base + i * DIRECT_UNIT_SIZE;
        if ((!direct_bit( view, 0, i ) || view->physical[i] >= 0) &&
            !ps5_direct_drop_unit( view, addr, TRUE )) goto failed;
        if (profile_started) stage_started = ps5_title_profile_now_ns();
        result = sceKernelAllocateDirectMemory( 0, direct_memory_size, DIRECT_UNIT_SIZE, DIRECT_UNIT_SIZE,
                                                PS5_KERNEL_DIRECT_TYPE_CPU, &physical );
        if (result && virtual_title_direct_cache_trim())
            result = sceKernelAllocateDirectMemory( 0, direct_memory_size, DIRECT_UNIT_SIZE, DIRECT_UNIT_SIZE,
                                                    PS5_KERNEL_DIRECT_TYPE_CPU, &physical );
        if (profile_started)
        {
            direct_profile.allocate_ns += ps5_title_profile_now_ns() - stage_started;
            stage_started = ps5_title_profile_now_ns();
        }
        if (result)
        {
            direct_error( "view-commit", "allocate", result );
            goto failed;
        }
        view->physical[i] = physical;
        direct_set_bit( view, 2, i, TRUE );
        placed = addr;
        result = sceKernelMapDirectMemory( &placed, DIRECT_UNIT_SIZE, DIRECT_RW, PS5_KERNEL_MAP_FIXED,
                                           view->physical[i], PS5_KERNEL_PAGE_SIZE );
        if (profile_started) direct_profile.map_ns += ps5_title_profile_now_ns() - stage_started;
        if (result)
        {
            direct_error( "view-commit", "map", result );
            goto failed;
        }
        if (placed != addr)
        {
            direct_wrong_address( "view-commit", "map", addr, placed );
            view->failed_unit = i;
            view->unexpected_mapping = placed;
            goto failed;
        }
        direct_set_bit( view, 1, i, TRUE );
        /* Recycled physical memory is dirty. Map RW, zero, then hide every
         * page; virtual.c applies the prospective 4 KiB vprot host-page union.
         * In particular, execute is added only by mprotect, never at map time. */
        if (profile_started) stage_started = ps5_title_profile_now_ns();
        memset( addr, 0, DIRECT_UNIT_SIZE );
        if (profile_started)
        {
            direct_profile.zero_ns += ps5_title_profile_now_ns() - stage_started;
            stage_started = ps5_title_profile_now_ns();
        }
        if (!hide_start) hide_start = addr;
        hide_size += DIRECT_UNIT_SIZE;
        /* Only newly backed, NX units belong to this transaction. Never
         * reclose an existing committed neighbour, even in a mixed range. */
        if (!batch || i == last || direct_bit( view, 1, i + 1 ))
        {
            if ((result = sceKernelMprotect( hide_start, hide_size, 0 )))
            {
                direct_error( "view-commit", "protect-uncommitted", result );
                goto failed;
            }
            if (profile_started)
            {
                direct_profile.hide_ns += ps5_title_profile_now_ns() - stage_started;
                direct_profile.hide_calls++;
            }
            hide_start = NULL;
            hide_size = 0;
        }
        if (profile_started) direct_profile.units++;
    }
    if (profile_started)
    {
        uint64_t now = ps5_title_profile_now_ns();
        direct_profile.backing_ns += now - profile_started;
        direct_profile_report( now );
    }
    return TRUE;

failed:
    /* A later allocation/map failure can leave a batched run RW. Close it
     * before rollback: a failed unmap deliberately preserves its ownership. */
    if (hide_start && hide_size && (result = sceKernelMprotect( hide_start, hide_size, 0 )))
        direct_error( "view-commit", "close-unhidden-rollback", result );
    if (!ps5_direct_finish_commit( view, base, size, TRUE ))
    {
        for (i = first; i <= last; i++)
        {
            if (!direct_bit( view, 2, i ) || !direct_bit( view, 1, i )) continue;
            addr = (char *)view->base + i * DIRECT_UNIT_SIZE;
            if ((result = sceKernelMprotect( addr, DIRECT_UNIT_SIZE, 0 )))
            {
                ps5_direct_fail_fast(); /* No callback waits while virtual_mutex is held. */
            }
        }
    }
    return FALSE;
}

int ps5_direct_protect( struct ps5_direct_view *view, void *base, size_t size, int prot )
{
    char *addr = base, *end = addr + size;
    size_t unit, count;
    int32_t result;
    int protection = 0;

    if (prot & PROT_READ) protection |= PS5_KERNEL_PROT_CPU_READ;
    if (prot & PROT_WRITE) protection |= PS5_KERNEL_PROT_CPU_WRITE;
    if (prot & PROT_EXEC) protection |= PS5_KERNEL_PROT_CPU_EXEC;
    while (addr < end)
    {
        unit = direct_unit_index( view, addr );
        count = min( end - addr, DIRECT_UNIT_SIZE - ((uintptr_t)addr % DIRECT_UNIT_SIZE) );
        if (direct_bit( view, 1, unit ))
        {
            /* Coalesce contiguous backed units with the same requested protection. */
            while (addr + count < end && !((uintptr_t)(addr + count) % DIRECT_UNIT_SIZE) &&
                   direct_bit( view, 1, direct_unit_index( view, addr + count ) ))
                count += min( end - addr - count, DIRECT_UNIT_SIZE );
            result = sceKernelMprotect( addr, count, protection );
            if (result)
            {
                direct_error( "view-protect", "mprotect", result );
                errno = EACCES;
                return -1;
            }
        }
        else if (protection)
        {
            direct_error( "view-protect", "unbacked-unit", -1 );
            errno = ENOMEM;
            return -1;
        }
        addr += count;
    }
    return 0;
}

static int direct_read_pools( struct ps5_direct_pools *pools, const char *stage )
{
    ULONGLONG total, available;
    int64_t physical = 0;
    size_t largest = 0;
    int32_t direct_result, flexible_result;

    direct_result = sceKernelAvailableDirectMemorySize( 0, direct_memory_size, DIRECT_UNIT_SIZE,
                                                       &physical, &largest );
    flexible_result = sceKernelAvailableFlexibleMemorySize( &pools->flexible );
    if (direct_result) direct_error( stage, "direct-available", direct_result );
    if (flexible_result) direct_error( stage, "flexible-available", flexible_result );
    pools->valid = !direct_result && !flexible_result;
    if (pools->valid)
    {
        /* The largest hole is not the pool total when the title has fragmented
         * direct memory. Reuse Wine's complete free-range summation. */
        ps5_get_memory_info( &total, &available );
        pools->direct = available >= pools->flexible ? available - pools->flexible : 0;
    }
    direct_log( "wine: ps5-direct pools stage=%s valid=%d direct_free_bytes=%zu "
                "direct_largest_bytes=%zu flexible_bytes=%zu direct_mib=%zu.%04zu flexible_mib=%zu.%04zu\n",
                stage, pools->valid, pools->direct, largest, pools->flexible,
                MIB_ARGS(pools->direct), MIB_ARGS(pools->flexible) );
    return pools->valid;
}

static int direct_expect_pools( const struct ps5_direct_pools *pools, size_t direct, size_t flexible,
                                const char *stage )
{
    if (!pools->valid) return 0;
    if (pools->direct == direct && pools->flexible == flexible) return 1;
    direct_log( "wine: ps5-direct failure stage=%s operation=accounting "
                "direct_expected_bytes=%zu direct_actual_bytes=%zu "
                "flexible_expected_bytes=%zu flexible_actual_bytes=%zu\n",
                stage, direct, pools->direct, flexible, pools->flexible );
    return 0;
}

static int direct_test_status( NTSTATUS status, const char *stage, const char *operation )
{
    if (!status) return 1;
    direct_error( stage, operation, status );
    return 0;
}

static int direct_check_zero( const void *base, size_t size, const char *stage )
{
    const volatile BYTE *bytes = base;
    size_t i;

    for (i = 0; i < size; i++)
        if (bytes[i])
        {
            direct_log( "wine: ps5-direct failure stage=%s operation=zero offset=%zu value=%#x\n",
                        stage, i, bytes[i] );
            return 0;
        }
    return 1;
}

static int direct_check_pattern( const void *base, size_t size, const char *stage,
                                 BYTE first, BYTE last )
{
    const volatile BYTE *bytes = base;
    unsigned int actual_first = bytes[0], actual_last = bytes[size - 1];

    if (actual_first == first && actual_last == last) return 1;
    direct_log( "wine: ps5-direct failure stage=%s operation=contents "
                "first=%#x last=%#x expected_first=%#x expected_last=%#x\n",
                stage, actual_first, actual_last, first, last );
    return 0;
}

static int direct_test_query( const void *addr, DWORD state, const char *stage )
{
    MEMORY_BASIC_INFORMATION info;
    NTSTATUS status;

    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation,
                                   &info, sizeof(info), NULL );
    if (!direct_test_status( status, stage, "query" )) return 0;
    if (info.State == state && info.Type == MEM_PRIVATE) return 1;
    direct_log( "wine: ps5-direct failure stage=%s operation=query-state "
                "address=%p state=%#lx expected=%#lx type=%#lx\n",
                stage, addr, (unsigned long)info.State, (unsigned long)state, (unsigned long)info.Type );
    return 0;
}

static int direct_test_release( void **base, const char *stage )
{
    SIZE_T size = 0;

    if (!*base) return 1;
    if (!direct_test_status( NtFreeVirtualMemory( NtCurrentProcess(), base, &size, MEM_RELEASE ),
                            stage, "release" )) return 0;
    *base = NULL;
    return 1;
}

static int direct_self_test(void)
{
    static const size_t offsets[] = { 0, 0x1d000, 16 << 20 };
    static const size_t sizes[] = { 0x1000, 0x5000, 1 << 20 };
    const size_t committed_bytes = 19 * DIRECT_UNIT_SIZE, decommitted_bytes = 17 * DIRECT_UNIT_SIZE;
    struct ps5_direct_pools before = {0}, pools = {0}, during = {0}, large_before = {0}, large_after = {0};
    void *small = NULL, *large = NULL, *addr;
    SIZE_T size, offset, i, page;
    ULONG old;
    NTSTATUS status;
    int reserve = 0, commit = 0, query = 0, protection = 0, zero_recommit = 0, partial_zero = 0;
    int large_commit = 0, accounting = 1, release = 1, passed = 0, works;

    if (direct_memory_size <= 0)
    {
        direct_error( "views-before", "direct-size", -1 );
        goto done;
    }
    /* Prime persistent vprot tables and the reusable per-view metadata before
     * measuring physical backing. No committed pages or raw kernel test path. */
    small = DIRECT_TEST_BASE;
    size = DIRECT_TEST_SIZE;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &small, 0, &size, MEM_RESERVE, PAGE_READWRITE );
    if (!direct_test_status( status, "views-warmup", "reserve-small" ))
    {
        small = NULL;
        goto done;
    }
    large = DIRECT_LARGE_BASE;
    size = DIRECT_LARGE_SIZE;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &large, 0, &size, MEM_RESERVE, PAGE_READWRITE );
    if (!direct_test_status( status, "views-warmup", "reserve-large" ))
    {
        large = NULL;
        goto done;
    }
    if (!direct_test_release( &large, "views-warmup" ) ||
        !direct_test_release( &small, "views-warmup" )) goto done;
    if (!direct_read_pools( &before, "views-before" )) goto done;
    if (before.direct < DIRECT_LARGE_SIZE + committed_bytes)
    {
        direct_log( "wine: ps5-direct failure stage=views-before operation=direct-capacity bytes=%zu\n",
                    before.direct );
        goto done;
    }

    small = DIRECT_TEST_BASE;
    size = DIRECT_TEST_SIZE;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &small, 0, &size, MEM_RESERVE, PAGE_READWRITE );
    if (!direct_test_status( status, "views-reserve", "reserve" ))
    {
        small = NULL;
        goto done;
    }
    direct_read_pools( &pools, "views-after-reserve" );
    accounting &= direct_expect_pools( &pools, before.direct, before.flexible, "views-after-reserve" );
    reserve = direct_test_query( small, MEM_RESERVE, "views-reserve" );

    for (i = 0; i < ARRAY_SIZE(offsets); i++)
    {
        addr = (char *)small + offsets[i];
        size = sizes[i];
        status = NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT, PAGE_READWRITE );
        if (!direct_test_status( status, "views-commit", "commit" )) goto done;
        if (!direct_check_zero( addr, size, "views-fresh-commit" )) goto done;
        ((volatile BYTE *)addr)[0] = 0x5a + i;
        ((volatile BYTE *)addr)[size - 1] = 0xa5 + i;
    }
    commit = 1;
    query = 1;
    for (i = 0; i < ARRAY_SIZE(offsets); i++)
    {
        query &= direct_test_query( (char *)small + offsets[i], MEM_COMMIT, "views-query-commit" );
        query &= direct_test_query( (char *)small + offsets[i] + sizes[i] - 1,
                                    MEM_COMMIT, "views-query-commit-end" );
    }
    query &= direct_test_query( (char *)small + (8 << 20), MEM_RESERVE, "views-query-gap" );
    direct_read_pools( &during, "views-after-commit" );
    accounting &= direct_expect_pools( &during, before.direct - committed_bytes,
                                       before.flexible, "views-after-commit" );

    addr = (char *)small + offsets[2];
    size = sizes[2];
    if (!direct_test_status( NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, PAGE_READONLY, &old ),
                            "views-protect-r", "protect" ) ||
        !direct_check_pattern( addr, size, "views-protect-r", 0x5c, 0xa7 )) goto done;
    if (!direct_test_status( NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, PAGE_READWRITE, &old ),
                            "views-protect-rw", "protect" ) ||
        !direct_check_pattern( addr, size, "views-protect-rw", 0x5c, 0xa7 )) goto done;
    addr = small;
    size = sizes[0];
    if (!direct_test_status( NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, PAGE_EXECUTE_READ, &old ),
                            "views-protect-rx", "protect" )) goto done;
    protection = 1;
    direct_read_pools( &pools, "views-after-protect" );
    accounting &= direct_expect_pools( &pools, before.direct - committed_bytes,
                                       before.flexible, "views-after-protect" );

    addr = (char *)small + offsets[1];
    size = sizes[1];
    if (!direct_test_status( NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_DECOMMIT ),
                            "views-decommit", "decommit" )) goto done;
    direct_read_pools( &pools, "views-after-decommit" );
    accounting &= direct_expect_pools( &pools, before.direct - decommitted_bytes,
                                       before.flexible, "views-after-decommit" );
    query &= direct_test_query( addr, MEM_RESERVE, "views-query-decommit" );
    if (!direct_test_status( NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT,
                                                     PAGE_READWRITE ), "views-recommit", "commit" )) goto done;
    zero_recommit = direct_check_zero( addr, size, "views-zero-recommit" );
    direct_read_pools( &pools, "views-after-recommit" );
    accounting &= direct_expect_pools( &pools, before.direct - committed_bytes,
                                       before.flexible, "views-after-recommit" );

    /* Also exercise a 4 KiB decommit with committed neighbours in the same
     * 16 KiB host page and the same physical unit. Its backing must stay. */
    addr = (char *)small + offsets[2] + 0x5000;
    size = 0x1000;
    memset( addr, 0xcc, size );
    if (!direct_test_status( NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_DECOMMIT ),
                            "views-partial-decommit", "decommit" ) ||
        !direct_test_status( NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT,
                                                     PAGE_READWRITE ), "views-partial-recommit", "commit" ))
        goto done;
    partial_zero = direct_check_zero( addr, size, "views-partial-zero" ) &&
                   direct_check_pattern( (char *)small + offsets[2], sizes[2],
                                          "views-partial-neighbours", 0x5c, 0xa7 );
    direct_read_pools( &pools, "views-after-partial-recommit" );
    accounting &= direct_expect_pools( &pools, before.direct - committed_bytes,
                                       before.flexible, "views-after-partial-recommit" );

    large = DIRECT_LARGE_BASE;
    size = DIRECT_LARGE_SIZE;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &large, 0, &size, MEM_RESERVE, PAGE_READWRITE );
    if (!direct_test_status( status, "views-large-reserve", "reserve" ))
    {
        large = NULL;
        goto done;
    }
    direct_read_pools( &large_before, "views-large-before" );
    accounting &= direct_expect_pools( &large_before, before.direct - committed_bytes,
                                       before.flexible, "views-large-before" );
    for (offset = 0; offset < DIRECT_LARGE_SIZE; offset += DIRECT_LARGE_CHUNK)
    {
        addr = (char *)large + offset;
        size = DIRECT_LARGE_CHUNK;
        if (!direct_test_status( NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT,
                                                         PAGE_READWRITE ), "views-large-commit", "commit" ))
            goto done;
        for (page = 0; page < size; page += PS5_KERNEL_PAGE_SIZE)
            ((volatile BYTE *)addr)[page] = (BYTE)(offset / DIRECT_LARGE_CHUNK + 1);
        ((volatile BYTE *)addr)[size - 1] = 0x3c;
    }
    large_commit = 1;
    direct_read_pools( &large_after, "views-large-after-commit" );
    accounting &= direct_expect_pools( &large_after, large_before.direct - DIRECT_LARGE_SIZE,
                                       large_before.flexible, "views-large-after-commit" );
    if (!direct_test_release( &large, "views-large-release" )) goto done;
    direct_read_pools( &pools, "views-large-after-release" );
    accounting &= direct_expect_pools( &pools, large_before.direct, large_before.flexible,
                                       "views-large-after-release" );
    passed = reserve && commit && query && protection && zero_recommit && partial_zero && large_commit;

done:
    release &= direct_test_release( &large, "views-cleanup-large" );
    release &= direct_test_release( &small, "views-release" );
    direct_read_pools( &pools, "views-after-release" );
    accounting &= before.valid && direct_expect_pools( &pools, before.direct, before.flexible,
                                                      "views-after-release" );
    works = passed && release && accounting;
    direct_log( "wine: ps5-direct views works=%d reserve=%d commit=%d query=%d rw_r_rw_rx=%d "
                "zero_recommit=%d partial_zero=%d large_4g=%d release=%d accounting=%d "
                "units=19 direct_before_bytes=%zu direct_during_bytes=%zu direct_after_bytes=%zu "
                "flexible_before_bytes=%zu flexible_during_bytes=%zu flexible_after_bytes=%zu\n",
                works, reserve, commit, query, protection, zero_recommit, partial_zero, large_commit,
                release, accounting, before.direct, during.direct, pools.direct,
                before.flexible, during.flexible, pools.flexible );
    return works;
}

void ps5_direct_test(void)
{
    if (!direct_test_enabled) return;
    direct_test_enabled = 0;
    if (!direct_self_test()) exit(1);
}

void ps5_direct_init(void)
{
    const char *memory_mode = getenv( "WINEPS5_DIRECT_MEMORY" );
    const char *test_mode = getenv( "WINEPS5_DIRECT_TEST" );
    const char *chunk_mode = getenv( "WINEPS5_CHUNKED_RESERVE" );
    int enabled = memory_mode && !strcmp( memory_mode, "1" );
    int test = test_mode && !strcmp( test_mode, "1" );
    int error = 0;

    /* These title-only markers must never reach a child or the wineserver. */
    if (unsetenv( "WINEPS5_DIRECT_MEMORY" )) error = errno;
    if (unsetenv( "WINEPS5_DIRECT_TEST" ) && !error) error = errno;
    if (error)
    {
        direct_log( "wine: ps5-direct failure stage=init operation=consume-markers errno=%d\n", error );
        exit(1);
    }
    if (direct_memory_initialized) return;
    direct_memory_initialized = 1;
    direct_memory_enabled = enabled;
    direct_test_enabled = enabled && test;
    direct_chunked_reserve = enabled && chunk_mode && !strcmp( chunk_mode, "1" );
    if (enabled)
    {
        direct_memory_size = sceKernelGetDirectMemorySize();
        direct_log( "wine: ps5-direct enabled size_bytes=%lld\n", (long long)direct_memory_size );
        direct_metadata_block( DIRECT_METADATA_SIZE );
    }
}

#endif /* __PROSPERO__ */
