/*
 * Win32 virtual memory functions
 *
 * Copyright 1997, 2002, 2020 Alexandre Julliard
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
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#ifdef HAVE_SYS_SYSINFO_H
# include <sys/sysinfo.h>
#endif
#ifdef HAVE_SYS_SYSCALL_H
# include <sys/syscall.h>
#endif
#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYS_QUEUE_H
# include <sys/queue.h>
#endif
#ifdef HAVE_SYS_USER_H
# include <sys/user.h>
#endif
#ifdef HAVE_LINK_H
# include <link.h>
#endif
#ifdef HAVE_SYS_LINK_H
# include <sys/link.h>
#endif
#ifdef HAVE_LIBPROCSTAT_H
# include <libprocstat.h>
#endif
#include <unistd.h>
#include <dlfcn.h>
#ifdef HAVE_VALGRIND_VALGRIND_H
# include <valgrind/valgrind.h>
#endif
#if defined(__APPLE__)
#define host_page_size mac_host_page_size
# include <mach/mach_init.h>
# include <mach/mach_vm.h>
# include <mach/task.h>
# include <mach/thread_state.h>
# include <mach/vm_map.h>
#undef host_page_size
#endif

#if defined(HAVE_LINUX_USERFAULTFD_H) && defined(HAVE_LINUX_FS_H)
# include <linux/userfaultfd.h>
# include <linux/fs.h>
#include "uffd_tmp_defs.h"

#if defined(UFFD_FEATURE_WP_ASYNC) && defined(PM_SCAN_WP_MATCHING)
#define USE_UFFD_WRITEWATCH
#endif
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/list.h"
#include "wine/rbtree.h"
#include "unix_private.h"
#include "wine/debug.h"
#include "wine/ps5_title.h"
#ifdef __PROSPERO__
#include "wine/ps5_title_private.h"
#include <ps5platform/klog.h>
#endif

WINE_DEFAULT_DEBUG_CHANNEL(virtual);
WINE_DECLARE_DEBUG_CHANNEL(module);
WINE_DECLARE_DEBUG_CHANNEL(virtual_ranges);
WINE_DECLARE_DEBUG_CHANNEL(virtstat);

/* Gdb integration, in loader/main.c */
static struct r_debug *wine_r_debug;

#if defined(HAVE_LINK_H) || defined(HAVE_SYS_LINK_H)

struct link_map_entry
{
    struct link_map map;
    const void *module;
};
static struct link_map link_map = {.l_name = (char *)""};

static void r_debug_set_state( int state )
{
    wine_r_debug->r_map = &link_map;
    wine_r_debug->r_state = state;
    ((void (*)(void))wine_r_debug->r_brk)();
}

static char *get_path_from_fd( int fd, int sz )
{
#ifdef linux
    char *ret = malloc( 32 + sz );

    if (ret) sprintf( ret, "/proc/self/fd/%u", fd );
    return ret;
#elif defined(F_GETPATH)
    char *ret = malloc( PATH_MAX + sz );

    if (!ret) return NULL;
    if (!fcntl( fd, F_GETPATH, ret )) return ret;
    free( ret );
    return NULL;
#else
    return NULL;
#endif
}

static char *r_debug_path_from_fd( int fd )
{
    char *real, *path;
    if (!(path = get_path_from_fd( fd, 0 ))) return NULL;
    if (!(real = realpath( path, NULL ))) return path;
    if (!strncmp( real, "/run/host", 9 )) memmove( real, real + 9, strlen( real ) - 8 );
    free( path );
    return real;
}

static void r_debug_add_module( void *module, int fd, INT_PTR offset )
{
    struct link_map *ptr = link_map.l_next, *next;
    struct link_map_entry *entry;

    if (!wine_r_debug) return;

    while (ptr)
    {
        entry = LIST_ENTRY(ptr, struct link_map_entry, map);
        if (entry->module == module) break;
        ptr = ptr->l_next;
    }

    r_debug_set_state( RT_ADD );

    /* l_addr is an address integer on Linux and a caddr_t on FreeBSD. */
    if (ptr) entry->map.l_addr = (__typeof__(entry->map.l_addr))offset;
    else if ((entry = calloc( 1, sizeof(*entry) )))
    {
        entry->module = module;
        entry->map.l_addr = (__typeof__(entry->map.l_addr))offset;
        entry->map.l_name = r_debug_path_from_fd( fd );

        entry->map.l_next = link_map.l_next;
        if ((next = entry->map.l_next)) next->l_prev = &entry->map;
        entry->map.l_prev = &link_map;
        link_map.l_next = &entry->map;
    }

    r_debug_set_state( RT_CONSISTENT );
}

static void r_debug_remove_module( void *module )
{
    struct link_map *ptr = link_map.l_next, *next;
    struct link_map_entry *entry;

    if (!wine_r_debug) return;

    while (ptr)
    {
        entry = LIST_ENTRY(ptr, struct link_map_entry, map);
        if (entry->module == module) break;
        ptr = ptr->l_next;
    }
    if (!ptr) return;

    r_debug_set_state( RT_DELETE );

    entry->map.l_prev->l_next = entry->map.l_next;
    if ((next = entry->map.l_next)) next->l_prev = entry->map.l_prev;

    r_debug_set_state( RT_CONSISTENT );

    free( entry->map.l_name );
    free( entry );
}

#else /* defined(HAVE_LINK_H) || defined(HAVE_SYS_LINK_H) */

#define RT_CONSISTENT 0
static void r_debug_set_state( int state ) {}
static void r_debug_add_module( void *module, int fd, INT_PTR offset ) {}
static void r_debug_remove_module( void *module ) {}

#endif /* defined(HAVE_LINK_H) || defined(HAVE_SYS_LINK_H) */

struct preload_info
{
    void  *addr;
    size_t size;
};

struct reserved_area
{
    struct list entry;
    void       *base;
    size_t      size;
};

static struct list reserved_areas = LIST_INIT(reserved_areas);

struct builtin_module
{
    struct list  entry;
    unsigned int refcount;
    void        *handle;
    void        *module;
    char        *unix_path;
    void        *unix_handle;
};

static struct list builtin_modules = LIST_INIT( builtin_modules );

struct file_view
{
    struct wine_rb_entry entry;  /* entry in global view tree */
    void         *base;          /* base address */
    size_t        size;          /* size in bytes */
    unsigned int  protect;       /* protection for all pages at allocation time and SEC_* flags */
#ifdef __PROSPERO__
    struct ps5_direct_view *direct; /* shared unit ownership for direct-backed split views */
    unsigned int map_profile;     /* fixed title mapping-profile slot + 1, zero if untracked */
    struct file_view *cache_next; /* detached, private direct-region cache linkage */
#endif
};

#ifdef __PROSPERO__
static BOOL title_direct_cache_put( struct file_view *view );
static BOOL title_direct_cache_evict_range( void *base, size_t size );
#endif

/* per-page protection flags */
#define VPROT_READ       0x01
#define VPROT_WRITE      0x02
#define VPROT_EXEC       0x04
#define VPROT_WRITECOPY  0x08
#define VPROT_GUARD      0x10
#define VPROT_COMMITTED  0x20
#define VPROT_WRITEWATCH 0x40
#define VPROT_COPIED     0x80
/* per-mapping protection flags */
#define VPROT_ARM64EC          0x0100  /* view may contain ARM64EC code */
#define VPROT_SYSTEM           0x0200  /* system view (underlying mmap not under our control) */
#define VPROT_PLACEHOLDER      0x0400
#define VPROT_FREE_PLACEHOLDER 0x0800
#define VPROT_NATIVE           0x1000
#ifdef __PROSPERO__
#define VPROT_PS5_DIRECT       0x2000  /* trusted-title direct-backed view */
#endif

/* Conversion from VPROT_* to Win32 flags */
static const BYTE VIRTUAL_Win32Flags[16] =
{
    PAGE_NOACCESS,              /* 0 */
    PAGE_READONLY,              /* READ */
    PAGE_READWRITE,             /* WRITE */
    PAGE_READWRITE,             /* READ | WRITE */
    PAGE_EXECUTE,               /* EXEC */
    PAGE_EXECUTE_READ,          /* READ | EXEC */
    PAGE_EXECUTE_READWRITE,     /* WRITE | EXEC */
    PAGE_EXECUTE_READWRITE,     /* READ | WRITE | EXEC */
    PAGE_WRITECOPY,             /* WRITECOPY */
    PAGE_WRITECOPY,             /* READ | WRITECOPY */
    PAGE_WRITECOPY,             /* WRITE | WRITECOPY */
    PAGE_WRITECOPY,             /* READ | WRITE | WRITECOPY */
    PAGE_EXECUTE_WRITECOPY,     /* EXEC | WRITECOPY */
    PAGE_EXECUTE_WRITECOPY,     /* READ | EXEC | WRITECOPY */
    PAGE_EXECUTE_WRITECOPY,     /* WRITE | EXEC | WRITECOPY */
    PAGE_EXECUTE_WRITECOPY      /* READ | WRITE | EXEC | WRITECOPY */
};

static struct wine_rb_tree views_tree;
static pthread_mutex_t virtual_mutex;
#ifdef __PROSPERO__
#define PS5_HANG_MODULE_MAX 1024
struct ps5_hang_module
{
    ULONG_PTR base, end;
    char name[40];
};
static struct ps5_hang_module ps5_hang_modules[PS5_HANG_MODULE_MAX];
static unsigned int ps5_hang_module_count, ps5_hang_module_sequence;
#endif

#ifdef __PROSPERO__
/* The no-present watchdog cannot wait for the virtual-memory lock. Track its
 * Windows owner, recursive depth, and outermost acquisition return address.
 * All virtual.c lock entry points below are wrapped; locks other than
 * virtual_mutex keep their upstream behavior. */
static UINT64 ps5_virtual_mutex_state;
static ULONG_PTR ps5_virtual_mutex_site;
static unsigned int ps5_virtual_mutex_leak_logged;

static void ps5_virtual_mutex_acquired( pthread_mutex_t *mutex, ULONG_PTR site )
{
    UINT64 old, next;
    unsigned int owner, depth, tid;

    if (mutex != &virtual_mutex || process_exiting || !__wine_ps5_is_title()) return;
    if (!(tid = ps5_title_current_thread_id())) tid = ~0u;
    do
    {
        old = __atomic_load_n( &ps5_virtual_mutex_state, __ATOMIC_RELAXED );
        owner = old >> 32;
        depth = (unsigned int)old;
        assert( !owner || owner == tid );
        next = ((UINT64)tid << 32) | (owner == tid ? depth + 1 : 1);
    } while (!__atomic_compare_exchange_n( &ps5_virtual_mutex_state, &old, next, FALSE,
                                           __ATOMIC_RELEASE, __ATOMIC_RELAXED ));
    if (!owner) __atomic_store_n( &ps5_virtual_mutex_site, site, __ATOMIC_RELEASE );
}

static void ps5_virtual_mutex_releasing( pthread_mutex_t *mutex )
{
    UINT64 old, next;
    unsigned int depth;

    if (mutex != &virtual_mutex || process_exiting || !__wine_ps5_is_title()) return;
    do
    {
        old = __atomic_load_n( &ps5_virtual_mutex_state, __ATOMIC_RELAXED );
        depth = (unsigned int)old;
        if (!depth) return;
        next = depth == 1 ? 0 : old - 1;
    } while (!__atomic_compare_exchange_n( &ps5_virtual_mutex_state, &old, next, FALSE,
                                           __ATOMIC_RELEASE, __ATOMIC_RELAXED ));
}

static unsigned int ps5_virtual_mutex_owner( ULONG_PTR *site )
{
    unsigned int owner = __atomic_load_n( &ps5_virtual_mutex_state, __ATOMIC_ACQUIRE ) >> 32;

    if (site) *site = owner ? __atomic_load_n( &ps5_virtual_mutex_site, __ATOMIC_ACQUIRE ) : 0;
    return owner;
}

void virtual_check_title_mutex_leak( const char *where )
{
    UINT64 state = __atomic_load_n( &ps5_virtual_mutex_state, __ATOMIC_ACQUIRE );
    unsigned int owner = state >> 32, tid = ps5_title_current_thread_id();
    char line[192];

    if (!tid) tid = ~0u;
    if (!owner || owner != tid || !__wine_ps5_is_title() ||
        __atomic_exchange_n( &ps5_virtual_mutex_leak_logged, 1, __ATOMIC_RELAXED ))
        return;
    snprintf( line, sizeof(line),
              "wine: title virtual_mutex leak tid=%04x site=%016lx depth=%u where=%s\n",
              tid, __atomic_load_n( &ps5_virtual_mutex_site, __ATOMIC_ACQUIRE ),
              (unsigned int)state, where );
    sceKernelDebugOutText( 0, line );
}

unsigned int virtual_title_mutex_depth(void)
{
    UINT64 state = __atomic_load_n( &ps5_virtual_mutex_state, __ATOMIC_ACQUIRE );
    unsigned int owner = state >> 32, tid = ps5_title_current_thread_id();

    if (!tid) tid = ~0u;
    return __wine_ps5_is_title() && owner == tid ? (unsigned int)state : 0;
}

void virtual_restore_title_mutex_depth( unsigned int depth, const char *where )
{
    unsigned int current = virtual_title_mutex_depth();

    if (current <= depth) return;
    virtual_check_title_mutex_leak( where );
    while ((current = virtual_title_mutex_depth()) > depth)
    {
        ps5_virtual_mutex_releasing( &virtual_mutex );
        if (pthread_mutex_unlock( &virtual_mutex )) break;
    }
}

static void __attribute__((noinline))
ps5_virtual_enter_uninterrupted_section( pthread_mutex_t *mutex, sigset_t *sigset )
{
    server_enter_uninterrupted_section( mutex, sigset );
    ps5_virtual_mutex_acquired( mutex, (ULONG_PTR)__builtin_return_address(0) );
}

static void ps5_virtual_leave_uninterrupted_section( pthread_mutex_t *mutex, sigset_t *sigset )
{
    ps5_virtual_mutex_releasing( mutex );
    server_leave_uninterrupted_section( mutex, sigset );
}

static void __attribute__((noinline)) ps5_virtual_mutex_lock( pthread_mutex_t *mutex )
{
    mutex_lock( mutex );
    ps5_virtual_mutex_acquired( mutex, (ULONG_PTR)__builtin_return_address(0) );
}

static void ps5_virtual_mutex_unlock( pthread_mutex_t *mutex )
{
    ps5_virtual_mutex_releasing( mutex );
    mutex_unlock( mutex );
}

static int __attribute__((noinline)) ps5_virtual_mutex_trylock( pthread_mutex_t *mutex )
{
    int ret = pthread_mutex_trylock( mutex );

    if (!ret) ps5_virtual_mutex_acquired( mutex, (ULONG_PTR)__builtin_return_address(0) );
    return ret;
}

static int ps5_virtual_pthread_mutex_unlock( pthread_mutex_t *mutex )
{
    ps5_virtual_mutex_releasing( mutex );
    return pthread_mutex_unlock( mutex );
}

#define server_enter_uninterrupted_section ps5_virtual_enter_uninterrupted_section
#define server_leave_uninterrupted_section ps5_virtual_leave_uninterrupted_section
#define mutex_lock                         ps5_virtual_mutex_lock
#define mutex_unlock                       ps5_virtual_mutex_unlock
#define pthread_mutex_trylock              ps5_virtual_mutex_trylock
#define pthread_mutex_unlock               ps5_virtual_pthread_mutex_unlock
#endif

static const UINT page_shift = 12;
static const UINT_PTR page_mask = 0xfff;
static const UINT_PTR granularity_mask = 0xffff;

/* Hosts whose pages are larger than Windows' 4 KiB: 16 KiB on Apple's arm64
 * and on the PS5, where mprotect works on whole 16 KiB pages. */
#if defined(__aarch64__) || defined(__PROSPERO__)
#define HOST_PAGE_SIZE_VARIES
#endif

#ifdef HOST_PAGE_SIZE_VARIES
static UINT_PTR host_page_size;
static UINT_PTR host_page_mask;
#else
static const UINT_PTR host_page_size = 0x1000;
static const UINT_PTR host_page_mask = 0xfff;
#endif

/* Note: these are Windows limits, you cannot change them. */
#if defined(__i386__) || defined(__x86_64__)
static void *address_space_start = (void *)0x110000; /* keep DOS area clear */
#else
static void *address_space_start = (void *)0x10000;
#endif
#ifdef _WIN64
static void *address_space_limit = (void *)0x7fffffff0000;  /* top of the total available address space */
static void *user_space_limit    = (void *)0x7fffffff0000;  /* top of the user address space */
static void *working_set_limit   = (void *)0x7fffffff0000;  /* top of the current working set */
#else
static void *address_space_limit = (void *)0xc0000000;
static void *user_space_limit    = (void *)0x7fff0000;
static void *working_set_limit   = (void *)0x7fff0000;
#endif

static void *host_addr_space_limit;  /* top of the host virtual address space */
#ifdef __PROSPERO__
/* get_host_addr_space_limit() infers 1 TiB by doubling a successful hint,
 * but the console probe proves mappings only through the 512 GiB candidate
 * and refuses 1 TiB. Do not offer Wine the unmeasured upper half. */
#define PS5_HOST_ADDR_SPACE_LIMIT ((void *)(UINT_PTR)0x8000000000)
static BOOL ps5_title_radv_ranges;
static UINT_PTR ps5_title_reserve_low = ~(UINT_PTR)0, ps5_title_reserve_high;

static void ps5_title_note_reservation( void *base, size_t size )
{
    UINT_PTR start = (UINT_PTR)base;
    char line[256];

    if (!ps5_title_radv_ranges) return;
    assert( !wine_ps5_title_radv_overlap( start, size, NULL, NULL ));
    if (start >= ps5_title_reserve_low && start <= ps5_title_reserve_high) return;
    ps5_title_reserve_low = min( ps5_title_reserve_low, start );
    ps5_title_reserve_high = max( ps5_title_reserve_high, start );
    snprintf( line, sizeof(line), "wine: title reservation bases lowest=%#llx highest=%#llx forbidden_overlap=0\n",
              (unsigned long long)ps5_title_reserve_low, (unsigned long long)ps5_title_reserve_high );
    sceKernelDebugOutText( 0, line );
}
#endif


static struct file_view *arm64ec_view;
static const ptrdiff_t max_try_map_step = 0x40000000;
static BOOL increase_try_map_step = TRUE;

ULONG_PTR user_space_wow_limit = 0;
struct _KUSER_SHARED_DATA *user_shared_data = (void *)0x7ffe0000;

/* TEB allocation blocks */
static void *teb_block;
static void **next_free_teb;
static int teb_block_pos;
static struct list teb_list = LIST_INIT( teb_list );

#define ROUND_ADDR(addr,mask) ((void *)((UINT_PTR)(addr) & ~(UINT_PTR)(mask)))
#define ROUND_SIZE(addr,size,mask) (((SIZE_T)(size) + ((UINT_PTR)(addr) & (mask)) + (mask)) & ~(UINT_PTR)(mask))

#define VIRTUAL_DEBUG_DUMP_VIEW(view) do { if (TRACE_ON(virtual)) dump_view(view); } while (0)
#define VIRTUAL_DEBUG_DUMP_RANGES() do { if (TRACE_ON(virtual_ranges)) dump_free_ranges(); } while (0)

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#ifdef _WIN64  /* on 64-bit the page protection bytes use a 2-level table */
static const size_t pages_vprot_shift = 20;
static const size_t pages_vprot_mask = (1 << 20) - 1;
static size_t pages_vprot_size;
static BYTE **pages_vprot;
#else  /* on 32-bit we use a simple array with one byte per page */
static BYTE *pages_vprot;
#endif

static int use_kernel_writewatch;
#ifdef USE_UFFD_WRITEWATCH
static int uffd_fd, pagemap_fd;
#endif

static struct file_view *view_block_start, *view_block_end, *next_free_view;
#ifdef _WIN64
static const size_t view_block_size = 0x200000;
#else
static const size_t view_block_size = 0x100000;
#endif
static void *preload_reserve_start;
static void *preload_reserve_end;
static BOOL force_exec_prot;  /* whether to force PROT_EXEC on all PROT_READ mmaps */
static BOOL enable_write_exceptions;  /* raise exception on writes to executable memory */

struct range_entry
{
    void *base;
    void *end;
};

static struct range_entry *free_ranges;
static struct range_entry *free_ranges_end;


static inline BOOL is_beyond_limit( const void *addr, size_t size, const void *limit )
{
    return (addr >= limit || (const char *)addr + size > (const char *)limit);
}

static inline BOOL is_vprot_exec_write( BYTE vprot )
{
    return (vprot & VPROT_EXEC) && (vprot & (VPROT_WRITE | VPROT_WRITECOPY));
}

#ifdef __PROSPERO__
/* Instrument actual native calls, not vprot transactions (which may issue
 * several calls or update vprot before mprotect). The small shadow only proves
 * unchanged protection when one recent successful call covers the whole range.
 * Eviction/partial overlap is reported as unknown, never guessed from vprot. */
static struct
{
    uint64_t count[4], ns[4], failures, reserved_bytes, unchanged, unknown;
    struct
    {
        UINT_PTR base, end;
        int prot;
    } ranges[64];
    unsigned int next;
} title_host;
static pthread_mutex_t title_host_mutex = PTHREAD_MUTEX_INITIALIZER;

void ps5_title_startup_host_call( enum ps5_title_host_call op, void *addr, size_t size,
                                int prot, BOOL success, uint64_t elapsed )
{
    UINT_PTR mask = host_page_mask ? host_page_mask : 0x3fff; /* before virtual_init: PS5 16 KiB */
    UINT_PTR base = (UINT_PTR)addr & ~mask, end = ((UINT_PTR)addr + size + mask) & ~mask;
    BOOL known = FALSE, unchanged = FALSE;
    unsigned int i;
    int saved_errno = errno;

    pthread_mutex_lock( &title_host_mutex );
    title_host.count[op]++;
    title_host.ns[op] += elapsed;
    if (!success) title_host.failures++;
    else
    {
        if (op == PS5_TITLE_HOST_RESERVE) title_host.reserved_bytes += size;
        if (op == PS5_TITLE_HOST_PROTECT)
        {
            for (i = 0; i < ARRAY_SIZE(title_host.ranges); i++)
                if (title_host.ranges[i].end && base >= title_host.ranges[i].base &&
                    end <= title_host.ranges[i].end)
                {
                    known = TRUE;
                    unchanged = title_host.ranges[i].prot == prot;
                    break;
                }
            if (!known) title_host.unknown++;
            else if (unchanged) title_host.unchanged++;
        }
        if (!unchanged)
        {
            for (i = 0; i < ARRAY_SIZE(title_host.ranges); i++)
                if (base < title_host.ranges[i].end && end > title_host.ranges[i].base)
                    title_host.ranges[i].end = 0;
            if (size && end > base && (op == PS5_TITLE_HOST_MAP || op == PS5_TITLE_HOST_PROTECT))
            {
                i = title_host.next++ % ARRAY_SIZE(title_host.ranges);
                title_host.ranges[i].base = base;
                title_host.ranges[i].end = end;
                title_host.ranges[i].prot = prot;
            }
        }
    }
    pthread_mutex_unlock( &title_host_mutex );
    errno = saved_errno;
}

void virtual_title_startup_report(void)
{
    uint64_t count[4], us[4], failures, reserved_bytes, unchanged, unknown;
    char line[512];
    unsigned int i;

    pthread_mutex_lock( &title_host_mutex );
    for (i = 0; i < ARRAY_SIZE(count); i++)
    {
        count[i] = title_host.count[i];
        us[i] = title_host.ns[i] / 1000;
    }
    failures = title_host.failures;
    reserved_bytes = title_host.reserved_bytes;
    unchanged = title_host.unchanged;
    unknown = title_host.unknown;
    pthread_mutex_unlock( &title_host_mutex );
#define HOST_ARGS(i) (unsigned long long)count[i], (unsigned long long)(us[i] / 1000), \
                     (unsigned int)(us[i] % 1000)
    snprintf( line, sizeof(line),
              "wine: title startup pid=%u host_map=%llu/%llu.%03u host_protect=%llu/%llu.%03u "
              "host_unmap=%llu/%llu.%03u host_reserve=%llu/%llu.%03u reserved_bytes=%llu "
              "host_protect_unchanged=%llu host_protect_unknown=%llu host_failed=%llu\n",
              (unsigned int)getpid(), HOST_ARGS(PS5_TITLE_HOST_MAP), HOST_ARGS(PS5_TITLE_HOST_PROTECT),
              HOST_ARGS(PS5_TITLE_HOST_UNMAP), HOST_ARGS(PS5_TITLE_HOST_RESERVE),
              (unsigned long long)reserved_bytes, (unsigned long long)unchanged,
              (unsigned long long)unknown, (unsigned long long)failures );
#undef HOST_ARGS
    sceKernelDebugOutText( 0, line );
}

#define TITLE_MAP_FILES 512
struct title_map_counts
{
    uint64_t maps, bytes, ns, read_ns, prep_ns, unmaps, unmap_bytes, failed;
};
struct title_map_file
{
    uint64_t id;
    unsigned int kind;
    char name[96];
    struct title_map_counts counts;
};
static struct title_map_file title_map_files[TITLE_MAP_FILES], title_map_snapshot[TITLE_MAP_FILES];
static struct title_map_counts title_map_total;
static unsigned int title_map_count, title_map_overflow, title_map_name_missing;
static uint64_t title_map_since;

static struct ps5_title_map_sample *title_map_sample(void)
{
    struct ntdll_thread_data *data = ps5_title_profile_thread_data();
    return data && data->map_sample && data->map_sample->file ? data->map_sample : NULL;
}

static uint64_t title_map_prep_begin( struct ps5_title_map_sample *sample )
{
    return sample && !sample->prep_depth++ ? ps5_title_profile_now_ns() : 0;
}

static void title_map_prep_end( struct ps5_title_map_sample *sample, uint64_t started )
{
    uint64_t now;
    if (!sample) return;
    sample->prep_depth--;
    if (started && (now = ps5_title_profile_now_ns()) >= started) sample->prep_ns += now - started;
}

static ssize_t title_map_pread( int fd, void *buffer, size_t size, off_t offset )
{
    struct ps5_title_map_sample *sample = title_map_sample();
    uint64_t started = sample ? ps5_title_profile_now_ns() : 0, ended;
    ssize_t ret = pread( fd, buffer, size, offset );

    if (started && (ended = ps5_title_profile_now_ns()) >= started) sample->read_ns += ended - started;
    return ret;
}

static void title_map_zero( void *buffer, size_t size )
{
    struct ps5_title_map_sample *sample = title_map_sample();
    uint64_t started = title_map_prep_begin( sample );
    memset( buffer, 0, size );
    title_map_prep_end( sample, started );
}

/* Mapping/unmapping publication is already serialized by virtual_mutex. */
static void title_map_add( struct title_map_counts *counts, const struct ps5_title_map_sample *sample,
                           uint64_t ns, size_t bytes, BOOL success )
{
    counts->maps++;
    counts->bytes += bytes;
    counts->ns += ns;
    counts->read_ns += sample->read_ns;
    counts->prep_ns += sample->prep_ns;
    if (!success) counts->failed++;
}

static unsigned int title_map_file_index( const struct ps5_title_map_sample *sample )
{
    unsigned int i;
    for (i = 0; i < title_map_count; i++)
        if (title_map_files[i].id == sample->id && title_map_files[i].kind == sample->kind) break;
    if (i == TITLE_MAP_FILES)
    {
        return ~0u;
    }
    if (i == title_map_count)
    {
        title_map_count++;
        title_map_files[i].id = sample->id;
        title_map_files[i].kind = sample->kind;
        snprintf( title_map_files[i].name, sizeof(title_map_files[i].name), "%s", sample->name );
    }
    return i + 1;
}

static void title_map_bind( struct file_view *view )
{
    struct ps5_title_map_sample *sample = title_map_sample();
    if (sample) view->map_profile = title_map_file_index( sample );
}

static void title_map_record( const struct ps5_title_map_sample *sample, uint64_t started,
                              uint64_t ended, size_t bytes, BOOL success )
{
    unsigned int index = title_map_file_index( sample );
    uint64_t ns = started && ended >= started ? ended - started : 0;

    if (!title_map_since) title_map_since = started;
    title_map_add( &title_map_total, sample, ns, bytes, success );
    if (!sample->id) title_map_name_missing++;
    if (index > TITLE_MAP_FILES) title_map_overflow++;
    else title_map_add( &title_map_files[index - 1].counts, sample, ns, bytes, success );
}

static void title_map_removed( unsigned int index, size_t size )
{
    if (!index) return;
    title_map_total.unmaps++;
    title_map_total.unmap_bytes += size;
    if (index <= TITLE_MAP_FILES)
    {
        title_map_files[index - 1].counts.unmaps++;
        title_map_files[index - 1].counts.unmap_bytes += size;
    }
}

static void title_map_unmapped( struct file_view *view )
{
    title_map_removed( view->map_profile, view->size );
    view->map_profile = 0;
}

void virtual_title_maps_report( uint64_t now )
{
    static const char * const kinds[] = { "image", "data-readonly", "data-writecopy", "shared" };
    struct title_map_counts total;
    unsigned int count, i, rank, best, overflow, missing;
    unsigned long long ms;
    char line[640];

    if (pthread_mutex_trylock( &virtual_mutex ))
    {
        sceKernelDebugOutText( 0, "title maps deferred=virtual_mutex\n" );
        return;
    }
    ms = title_map_since && now >= title_map_since ? (now - title_map_since) / 1000000 : 0;
    count = title_map_count;
    memcpy( title_map_snapshot, title_map_files, count * sizeof(*title_map_snapshot) );
    for (i = 0; i < count; i++) memset( &title_map_files[i].counts, 0, sizeof(title_map_files[i].counts) );
    total = title_map_total;
    memset( &title_map_total, 0, sizeof(title_map_total) );
    overflow = title_map_overflow;
    missing = title_map_name_missing;
    title_map_overflow = title_map_name_missing = 0;
    title_map_since = now;
    pthread_mutex_unlock( &virtual_mutex );
#define MAP_MS(v) (unsigned long long)((v) / 1000000), (unsigned long long)((v) / 1000 % 1000)
#define MAP_MIB(v) (unsigned long long)((v) / 1048576), (unsigned long long)(((v) % 1048576) * 1000 / 1048576)
    snprintf( line, sizeof(line), "title maps window_ms=%llu total_maps=%llu total_mib=%llu.%03llu "
              "total_ms=%llu.%03llu read_ms=%llu.%03llu prep_ms=%llu.%03llu "
              "unmaps=%llu unmap_mib=%llu.%03llu failed=%llu overflow=%u name_missing=%u\n",
              ms, (unsigned long long)total.maps, MAP_MIB(total.bytes), MAP_MS(total.ns),
              MAP_MS(total.read_ns), MAP_MS(total.prep_ns), (unsigned long long)total.unmaps,
              MAP_MIB(total.unmap_bytes), (unsigned long long)total.failed, overflow, missing );
    sceKernelDebugOutText( 0, line );
    for (rank = 1; rank <= 10; rank++)
    {
        const struct title_map_file *file;
        const struct title_map_counts *c;
        best = count;
        for (i = 0; i < count; i++)
            if ((title_map_snapshot[i].counts.maps || title_map_snapshot[i].counts.unmaps) &&
                (best == count || title_map_snapshot[i].counts.bytes > title_map_snapshot[best].counts.bytes))
                best = i;
        if (best == count) break;
        file = &title_map_snapshot[best];
        c = &file->counts;
        snprintf( line, sizeof(line), "title maps window_ms=%llu rank=%u file=%s kind=%s maps=%llu "
                  "mib=%llu.%03llu ms=%llu.%03llu read_ms=%llu.%03llu prep_ms=%llu.%03llu "
                  "unmaps=%llu unmap_mib=%llu.%03llu failed=%llu id=%016llx\n",
                  ms, rank, file->name, kinds[file->kind], (unsigned long long)c->maps,
                  MAP_MIB(c->bytes), MAP_MS(c->ns), MAP_MS(c->read_ns), MAP_MS(c->prep_ns),
                  (unsigned long long)c->unmaps, MAP_MIB(c->unmap_bytes), (unsigned long long)c->failed,
                  (unsigned long long)file->id );
        sceKernelDebugOutText( 0, line );
        title_map_snapshot[best].counts.maps = title_map_snapshot[best].counts.unmaps = 0;
    }
#undef MAP_MIB
#undef MAP_MS
}

static void *title_startup_mmap( void *addr, size_t size, int prot, int flags, int fd, off_t offset )
{
    struct ps5_title_map_sample *sample = title_map_sample();
    BOOL startup = __atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED );
    uint64_t start = startup || (sample && !sample->prep_depth) ? ps5_title_profile_now_ns() : 0;
    void *ret = mmap( addr, size, prot, flags, fd, offset );
    uint64_t elapsed = start ? ps5_title_profile_now_ns() - start : 0;

    if (sample && !sample->prep_depth) sample->prep_ns += elapsed;
    if (startup) ps5_title_startup_host_call( PS5_TITLE_HOST_MAP, ret, size, prot, ret != MAP_FAILED, elapsed );
    return ret;
}

static int title_startup_mprotect( void *addr, size_t size, int prot )
{
    struct ps5_title_map_sample *sample = title_map_sample();
    BOOL startup = __atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED );
    uint64_t start = startup || (sample && !sample->prep_depth) ? ps5_title_profile_now_ns() : 0;
    int ret = mprotect( addr, size, prot );
    uint64_t elapsed = start ? ps5_title_profile_now_ns() - start : 0;

    if (sample && !sample->prep_depth) sample->prep_ns += elapsed;
    if (startup) ps5_title_startup_host_call( PS5_TITLE_HOST_PROTECT, addr, size, prot, !ret, elapsed );
    return ret;
}

static int title_startup_munmap( void *addr, size_t size )
{
    struct ps5_title_map_sample *sample = title_map_sample();
    BOOL startup = __atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED );
    uint64_t start = startup || (sample && !sample->prep_depth) ? ps5_title_profile_now_ns() : 0;
    int ret = munmap( addr, size );
    uint64_t elapsed = start ? ps5_title_profile_now_ns() - start : 0;

    if (sample && !sample->prep_depth) sample->prep_ns += elapsed;
    if (startup) ps5_title_startup_host_call( PS5_TITLE_HOST_UNMAP, addr, size, 0, !ret, elapsed );
    return ret;
}

/* A disabled profile costs just the branch, with no clocks or shadow work. */
#define mmap(a,s,p,f,d,o) ((__atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED ) || \
                          title_profile_is_enabled()) ? title_startup_mmap(a,s,p,f,d,o) : mmap(a,s,p,f,d,o))
#define mprotect(a,s,p) ((__atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED ) || \
                        title_profile_is_enabled()) ? title_startup_mprotect(a,s,p) : mprotect(a,s,p))
#define munmap(a,s) ((__atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED ) || \
                    title_profile_is_enabled()) ? title_startup_munmap(a,s) : munmap(a,s))
#define pread(d,b,s,o) (title_profile_is_enabled() ? title_map_pread(d,b,s,o) : pread(d,b,s,o))
#endif

/* mmap() anonymous memory at a fixed address */
void *anon_mmap_fixed( void *start, size_t size, int prot, int flags )
{
    assert( !((UINT_PTR)start & host_page_mask) );
    assert( !(size & host_page_mask) );

    return mmap( start, size, prot, MAP_PRIVATE | MAP_ANON | MAP_FIXED | flags, -1, 0 );
}

/* allocate anonymous mmap() memory at any address */
void *anon_mmap_alloc( size_t size, int prot )
{
    assert( !(size & host_page_mask) );

    return mmap( NULL, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0 );
}

#ifdef USE_UFFD_WRITEWATCH
static void kernel_writewatch_init(void)
{
    struct uffdio_api uffdio_api;

    uffd_fd = syscall( __NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY );
    if (uffd_fd == -1) return;

    uffdio_api.api = UFFD_API;
    uffdio_api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
    if (ioctl( uffd_fd, UFFDIO_API, &uffdio_api ) || uffdio_api.api != UFFD_API)
    {
        close( uffd_fd );
        return;
    }
    pagemap_fd = open( "/proc/self/pagemap", O_CLOEXEC | O_RDONLY );
    if (pagemap_fd == -1)
    {
        ERR( "Error opening /proc/self/pagemap.\n" );
        close( uffd_fd );
        return;
    }
    use_kernel_writewatch = 1;
    TRACE( "Using kernel write watches.\n" );
}

static void kernel_writewatch_reset( void *start, SIZE_T len )
{
    struct pm_scan_arg arg = { 0 };

    len = ROUND_SIZE( start, len, host_page_mask );
    start = (char *)ROUND_ADDR( start, host_page_mask );

    arg.size = sizeof(arg);
    arg.start = (UINT_PTR)start;
    arg.end = arg.start + len;
    arg.flags = PM_SCAN_WP_MATCHING;
    arg.category_mask = PAGE_IS_WRITTEN;
    arg.return_mask = PAGE_IS_WRITTEN;
    if (ioctl( pagemap_fd, PAGEMAP_SCAN, &arg ) < 0)
        ERR( "ioctl(PAGEMAP_SCAN) failed, err %s.\n", strerror(errno) );
}

static void kernel_writewatch_register_range( struct file_view *view, void *base, size_t size )
{
    struct uffdio_register uffdio_register;
    struct uffdio_writeprotect wp;

    if (!(view->protect & VPROT_WRITEWATCH) || !use_kernel_writewatch) return;

    size = ROUND_SIZE( base, size, host_page_mask );
    base = (char *)ROUND_ADDR( base, host_page_mask );

    /* Transparent huge pages will result in larger areas reported as dirty. */
    madvise( base, size, MADV_NOHUGEPAGE );

    uffdio_register.range.start = (UINT_PTR)base;
    uffdio_register.range.len = size;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_WP;
    if (ioctl( uffd_fd, UFFDIO_REGISTER, &uffdio_register ) == -1)
    {
        ERR( "ioctl( UFFDIO_REGISTER ) failed, %s.\n", strerror(errno) );
        return;
    }

    if (!(uffdio_register.ioctls & UFFDIO_WRITEPROTECT))
    {
        ERR( "uffdio_register.ioctls %s.\n", wine_dbgstr_longlong(uffdio_register.ioctls) );
        return;
    }
    wp.range.start = (UINT_PTR)base;
    wp.range.len = size;
    wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;

    if (ioctl( uffd_fd, UFFDIO_WRITEPROTECT, &wp ) == -1)
        ERR( "ioctl( UFFDIO_WRITEPROTECT ) failed, %s.\n", strerror(errno) );
}

static void kernel_get_write_watches( void *base, SIZE_T size, void **buffer, ULONG_PTR *count, BOOL reset )
{
    struct pm_scan_arg arg = { 0 };
    struct page_region rgns[256];
    SIZE_T buffer_len = *count;
    char *addr, *next_addr;
    int rgn_count, i;
    size_t end, granularity = host_page_size / page_size;

    assert( !(size & page_mask) );

    end = (size_t)((char *)base + size);
    size = ROUND_SIZE( base, size, host_page_mask );
    addr = (char *)ROUND_ADDR( base, host_page_mask );

    arg.size = sizeof(arg);
    arg.vec = (ULONG_PTR)rgns;
    arg.vec_len = ARRAY_SIZE(rgns);
    if (reset) arg.flags |= PM_SCAN_WP_MATCHING;
    arg.category_mask = PAGE_IS_WRITTEN;
    arg.return_mask = PAGE_IS_WRITTEN;

    *count = 0;
    while (1)
    {
        arg.start = (UINT_PTR)addr;
        arg.end = arg.start + size;
        arg.max_pages = (buffer_len + granularity - 1) / granularity;

        if ((rgn_count = ioctl( pagemap_fd, PAGEMAP_SCAN, &arg )) < 0)
        {
            ERR( "ioctl( PAGEMAP_SCAN ) failed, error %s.\n", strerror(errno) );
            return;
        }
        if (!rgn_count) break;

        assert( rgn_count <= ARRAY_SIZE(rgns) );
        for (i = 0; i < rgn_count; ++i)
        {
            size_t c_addr = max( rgns[i].start, (size_t)base );

            rgns[i].end = min( rgns[i].end, end );
            assert( rgns[i].categories == PAGE_IS_WRITTEN );
            while (buffer_len && c_addr < rgns[i].end)
            {
                buffer[(*count)++] = (void *)c_addr;
                --buffer_len;
                c_addr += page_size;
            }
            if (!buffer_len) break;
        }
        if (!buffer_len || rgn_count < arg.vec_len) break;
        next_addr = (char *)(ULONG_PTR)arg.walk_end;
        assert( size >= next_addr - addr );
        if (!(size -= next_addr - addr)) break;
        addr = next_addr;
    }
}
#else
static void kernel_writewatch_init(void)
{
}

static void kernel_writewatch_reset( void *start, SIZE_T len )
{
}

static void kernel_writewatch_register_range( struct file_view *view, void *base, size_t size )
{
}

static void kernel_get_write_watches( void *base, SIZE_T size, void **buffer, ULONG_PTR *count, BOOL reset )
{
    assert( 0 );
}
#endif

static void mmap_add_reserved_area( void *addr, SIZE_T size )
{
    struct reserved_area *area;
    struct list *ptr, *next;
    void *end, *area_end;

    assert( !((UINT_PTR)addr & host_page_mask) );
    assert( !(size & host_page_mask) );

    if (!((intptr_t)addr + size)) size--;  /* avoid wrap-around */
    end = (char *)addr + size;

    LIST_FOR_EACH( ptr, &reserved_areas )
    {
        area = LIST_ENTRY( ptr, struct reserved_area, entry );
        area_end = (char *)area->base + area->size;

        if (area->base > end) break;
        if (area_end < addr) continue;
        if (area->base > addr)
        {
            area->size += (char *)area->base - (char *)addr;
            area->base = addr;
        }
        if (area_end >= end) return;

        /* try to merge with the following ones */
        while ((next = list_next( &reserved_areas, ptr )))
        {
            struct reserved_area *area_next = LIST_ENTRY( next, struct reserved_area, entry );
            void *next_end = (char *)area_next->base + area_next->size;

            if (area_next->base > end) break;
            list_remove( next );
            free( area_next );
            if (next_end >= end)
            {
                end = next_end;
                break;
            }
        }
        area->size = (char *)end - (char *)area->base;
        return;
    }

    if ((area = malloc( sizeof(*area) )))
    {
        area->base = addr;
        area->size = size;
        list_add_before( ptr, &area->entry );
    }
}

static void mmap_remove_reserved_area( void *addr, SIZE_T size )
{
    struct reserved_area *area;
    struct list *ptr;

    assert( !((UINT_PTR)addr & host_page_mask) );
    assert( !(size & host_page_mask) );

    if (!((intptr_t)addr + size)) size--;  /* avoid wrap-around */

    ptr = list_head( &reserved_areas );
    /* find the first area covering address */
    while (ptr)
    {
        area = LIST_ENTRY( ptr, struct reserved_area, entry );
        if ((char *)area->base >= (char *)addr + size) break;  /* outside the range */
        if ((char *)area->base + area->size > (char *)addr)  /* overlaps range */
        {
            if (area->base >= addr)
            {
                if ((char *)area->base + area->size > (char *)addr + size)
                {
                    /* range overlaps beginning of area only -> shrink area */
                    area->size -= (char *)addr + size - (char *)area->base;
                    area->base = (char *)addr + size;
                    break;
                }
                else
                {
                    /* range contains the whole area -> remove area completely */
                    ptr = list_next( &reserved_areas, ptr );
                    list_remove( &area->entry );
                    free( area );
                    continue;
                }
            }
            else
            {
                if ((char *)area->base + area->size > (char *)addr + size)
                {
                    /* range is in the middle of area -> split area in two */
                    struct reserved_area *new_area = malloc( sizeof(*new_area) );
                    if (new_area)
                    {
                        new_area->base = (char *)addr + size;
                        new_area->size = (char *)area->base + area->size - (char *)new_area->base;
                        list_add_after( ptr, &new_area->entry );
                    }
                    area->size = (char *)addr - (char *)area->base;
                    break;
                }
                else
                {
                    /* range overlaps end of area only -> shrink area */
                    area->size = (char *)addr - (char *)area->base;
                }
            }
        }
        ptr = list_next( &reserved_areas, ptr );
    }
}

static int mmap_is_in_reserved_area( void *addr, SIZE_T size )
{
    struct reserved_area *area;

    LIST_FOR_EACH_ENTRY( area, &reserved_areas, struct reserved_area, entry )
    {
        if (area->base > addr) break;
        if ((char *)area->base + area->size <= (char *)addr) continue;
        /* area must contain block completely */
        if ((char *)area->base + area->size < (char *)addr + size) return -1;
        return 1;
    }
    return 0;
}

/* whether the block lies in a reserved area that holds a mapping of ours, so
 * that mapping over it with MAP_FIXED replaces nothing else */
static BOOL mmap_is_reserved_and_mapped( void *addr, SIZE_T size )
{
#ifdef __PROSPERO__
    return FALSE;  /* reserved areas are not mapped there, see ps5_reserve_free_ranges */
#else
    return mmap_is_in_reserved_area( addr, size ) == 1;
#endif
}


/***********************************************************************
 *           unmap_area_above_user_limit
 *
 * Unmap memory that's above the user space limit, by replacing it with an empty mapping,
 * and return the remaining size below the limit. virtual_mutex must be held by caller.
 */
static size_t unmap_area_above_user_limit( void *addr, size_t size )
{
    size_t ret = 0;

    if (addr < user_space_limit)
    {
        ret = (char *)user_space_limit - (char *)addr;
        if (ret >= size) return size;  /* nothing is above limit */
        size -= ret;
        addr = user_space_limit;
    }
#ifdef __PROSPERO__
    munmap( addr, size );  /* reserved areas are not mapped, see ps5_reserve_free_ranges */
#else
    anon_mmap_fixed( addr, size, PROT_NONE, MAP_NORESERVE );
#endif
    mmap_add_reserved_area( addr, size );
    return ret;
}


static void *anon_mmap_tryfixed( void *start, size_t size, int prot, int flags )
{
    void *ptr;

#ifdef MAP_FIXED_NOREPLACE
    ptr = mmap( start, size, prot, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON | flags, -1, 0 );
#elif defined(MAP_TRYFIXED)
    ptr = mmap( start, size, prot, MAP_TRYFIXED | MAP_PRIVATE | MAP_ANON | flags, -1, 0 );
#elif (defined(__FreeBSD__) || defined(__FreeBSD_kernel__)) && !defined(__PROSPERO__)
    /* Not on the PS5: its kernel ignores MAP_EXCL, so MAP_FIXED | MAP_EXCL
     * replaced the host title's image at 0x400000 (ps5/probes/excl), whose
     * exit handlers then faulted at exit and left the process stopped for
     * good. There, the address is only a hint, which the kernel honours
     * when the range is free (the fallback below). */
    ptr = mmap( start, size, prot, MAP_FIXED | MAP_EXCL | MAP_PRIVATE | MAP_ANON | flags, -1, 0 );
    if (ptr == MAP_FAILED && errno == EINVAL) errno = EEXIST;
#elif defined(__APPLE__)
    mach_vm_address_t result = (mach_vm_address_t)start;
    kern_return_t ret = mach_vm_map( mach_task_self(), &result, size, 0, VM_FLAGS_FIXED,
                                     MEMORY_OBJECT_NULL, 0, 0, prot, VM_PROT_ALL, VM_INHERIT_COPY );

    if (!ret)
    {
        if ((ptr = anon_mmap_fixed( start, size, prot, flags )) == MAP_FAILED)
            mach_vm_deallocate( mach_task_self(), result, size );
    }
    else
    {
        errno = (ret == KERN_NO_SPACE ? EEXIST : ENOMEM);
        ptr = MAP_FAILED;
    }
#else
    ptr = mmap( start, size, prot, MAP_PRIVATE | MAP_ANON | flags, -1, 0 );
#endif
    if (ptr != MAP_FAILED && ptr != start)
    {
        size = unmap_area_above_user_limit( ptr, size );
        if (size) munmap( ptr, size );
        ptr = MAP_FAILED;
        errno = EEXIST;
    }
    return ptr;
}

#ifdef __PROSPERO__
/***********************************************************************
 *           ps5_reserve_free_ranges
 *
 * On the PS5 every mapping, PROT_NONE included, takes one page of the
 * kernel's CPU page-table pool (8192 pages for the whole console) per 2 MiB
 * of address space for as long as it exists (ps5/probes/ptcost). The 2 GiB
 * Wine keeps reserved for a 32-bit process held some 800 pages, and a dozen
 * processes emptied the pool; the kernel then starts nothing until a
 * restart. So a reserved area is only recorded, never mapped: the free gaps
 * of the range, read from the process's own map. Views are then placed in
 * it with anon_mmap_tryfixed (map_fixed_area), which refuses an address the
 * system has taken since; released or freed parts are unmapped, not replaced.
 */
static void ps5_reserve_free_ranges( void *addr, void *end )
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, getpid() };
    char *start = addr, *limit = end ? end : (char *)host_addr_space_limit, *cur = start, *buf, *p;
    size_t size = 0;

    if (host_addr_space_limit && limit > (char *)host_addr_space_limit) limit = host_addr_space_limit;
    if (limit <= start || sysctl( mib, 4, NULL, &size, NULL, 0 )) return;
    size += size / 4;  /* room for mappings made meanwhile */
    if (!(buf = malloc( size ))) return;
    if (sysctl( mib, 4, buf, &size, NULL, 0 ))
    {
        free( buf );
        return;
    }
    for (p = buf; ; )
    {
        const struct kinfo_vmentry *entry = (const struct kinfo_vmentry *)p;
        int last = p >= buf + size || entry->kve_structsize <= 0;
        char *used_start = last ? limit : (char *)(UINT_PTR)entry->kve_start;
        char *used_end = last ? limit : (char *)(UINT_PTR)entry->kve_end;
        char *gap_start = (char *)(((UINT_PTR)max( cur, start ) + granularity_mask) & ~granularity_mask);
        char *gap_end = (char *)((UINT_PTR)min( used_start, limit ) & ~granularity_mask);

        if (gap_end > gap_start) mmap_add_reserved_area( gap_start, gap_end - gap_start );
        if (last || used_start >= limit) break;
        if (used_end > cur) cur = used_end;
        p += entry->kve_structsize;
    }
    free( buf );
}
#endif

static void reserve_area( void *addr, void *end )
{
#ifdef __PROSPERO__
    ps5_reserve_free_ranges( addr, end );
#elif defined(__APPLE__)

#ifdef __i386__
    static const mach_vm_address_t max_address = VM_MAX_ADDRESS;
#else
    static const mach_vm_address_t max_address = MACH_VM_MAX_ADDRESS;
#endif
    mach_vm_address_t address = (mach_vm_address_t)addr;
    mach_vm_address_t end_address = (mach_vm_address_t)end;

    if (!end_address || max_address < end_address)
        end_address = max_address;

    while (address < end_address)
    {
        mach_vm_address_t hole_address = address;
        kern_return_t ret;
        mach_vm_size_t size;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t dummy_object_name = MACH_PORT_NULL;

        /* find the mapped region at or above the current address. */
        ret = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                             (vm_region_info_t)&info, &count, &dummy_object_name);
        if (ret != KERN_SUCCESS)
        {
            address = max_address;
            size = 0;
        }

        if (end_address < address)
            address = end_address;
        if (hole_address < address)
        {
            /* found a hole, attempt to reserve it. */
            size_t hole_size = address - hole_address;
            mach_vm_address_t alloc_address = hole_address;

            ret = mach_vm_map( mach_task_self(), &alloc_address, hole_size, 0, VM_FLAGS_FIXED,
                               MEMORY_OBJECT_NULL, 0, 0, PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY );
            if (!ret) mmap_add_reserved_area( (void*)hole_address, hole_size );
            else if (ret == KERN_NO_SPACE)
            {
                /* something filled (part of) the hole before we could.
                   go back and look again. */
                address = hole_address;
                continue;
            }
        }
        address += size;
    }
#else
    size_t size = (char *)end - (char *)addr;

    if (!size) return;

    if (anon_mmap_tryfixed( addr, size, PROT_NONE, MAP_NORESERVE ) != MAP_FAILED)
    {
        mmap_add_reserved_area( addr, size );
        return;
    }
    size = (size / 2) & ~granularity_mask;
    if (size)
    {
        reserve_area( addr, (char *)addr + size );
        reserve_area( (char *)addr + size, end );
    }
#endif /* __APPLE__ */
}


static void mmap_init( const struct preload_info *preload_info )
{
#ifndef _WIN64
#ifndef __APPLE__
    char stack;
    char * const stack_ptr = &stack;
#endif
    char *user_space_limit = (char *)0x7ffe0000;
    int i;

    if (preload_info)
    {
        /* check for a reserved area starting at the user space limit */
        /* to avoid wasting time trying to allocate it again */
        for (i = 0; preload_info[i].size; i++)
        {
            if ((char *)preload_info[i].addr > user_space_limit) break;
            if ((char *)preload_info[i].addr + preload_info[i].size > user_space_limit)
            {
                user_space_limit = (char *)preload_info[i].addr + preload_info[i].size;
                break;
            }
        }
    }
    else reserve_area( (void *)0x00010000, (void *)0x40000000 );


#ifndef __APPLE__
    if (stack_ptr >= user_space_limit)
    {
        char *end = 0;
        char *base = stack_ptr - ((unsigned int)stack_ptr & granularity_mask) - (granularity_mask + 1);
        if (base > user_space_limit) reserve_area( user_space_limit, base );
        base = stack_ptr - ((unsigned int)stack_ptr & granularity_mask) + (granularity_mask + 1);
#if defined(linux) || defined(__FreeBSD__) || defined (__FreeBSD_kernel__) || defined(__DragonFly__)
        /* Heuristic: assume the stack is near the end of the address */
        /* space, this avoids a lot of futile allocation attempts */
        end = (char *)(((unsigned long)base + 0x0fffffff) & 0xf0000000);
#endif
        reserve_area( base, end );
    }
    else
#endif
        reserve_area( user_space_limit, 0 );

#else

    if (preload_info) return;
    /* if we don't have a preloader, try to reserve the space now */
    reserve_area( (void *)0x000000010000, (void *)0x000068000000 );
    reserve_area( (void *)0x00007f000000, (void *)0x00007fff0000 );
    reserve_area( (void *)0x7ffffe000000, (void *)0x7fffffff0000 );

#endif
}


/***********************************************************************
 *           get_wow_user_space_limit
 */
static ULONG_PTR get_wow_user_space_limit(void)
{
#ifdef _WIN64
    return user_space_wow_limit & ~granularity_mask;
#endif
    return (ULONG_PTR)user_space_limit;
}


/***********************************************************************
 *           add_builtin_module
 */
static void add_builtin_module( void *module, void *handle )
{
    struct builtin_module *builtin;

    if (!(builtin = malloc( sizeof(*builtin) ))) return;
    builtin->handle      = handle;
    builtin->module      = module;
    builtin->refcount    = 1;
    builtin->unix_path   = NULL;
    builtin->unix_handle = NULL;
    list_add_tail( &builtin_modules, &builtin->entry );
}


/***********************************************************************
 *           release_builtin_module
 */
static void release_builtin_module( void *module )
{
    struct builtin_module *builtin;

    LIST_FOR_EACH_ENTRY( builtin, &builtin_modules, struct builtin_module, entry )
    {
        if (builtin->module != module) continue;
        if (!--builtin->refcount)
        {
            list_remove( &builtin->entry );
            if (builtin->handle) dlclose( builtin->handle );
            if (builtin->unix_handle) dlclose( builtin->unix_handle );
            free( builtin->unix_path );
            free( builtin );
        }
        break;
    }
}


/***********************************************************************
 *           get_builtin_so_handle
 */
void *get_builtin_so_handle( void *module )
{
    sigset_t sigset;
    void *ret = NULL;
    struct builtin_module *builtin;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    LIST_FOR_EACH_ENTRY( builtin, &builtin_modules, struct builtin_module, entry )
    {
        if (builtin->module != module) continue;
        ret = builtin->handle;
        if (ret) builtin->refcount++;
        break;
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return ret;
}

static void load_steam_overlay(const char *unix_lib_path)
{
    const char *preload, *p;
    char path[PATH_MAX];
    unsigned int len;
    void *handle;

    if (!strstr(unix_lib_path, "winex11.so")) return;
    if (getenv("LD_PRELOAD") || !(preload = getenv("WINE_LD_PRELOAD"))) return;

    p = preload;
    while (*(preload = p))
    {
        if (!(p = strchr( preload, ':' ))) p = preload + strlen( preload );
        len = p - preload;
        if (*p) ++p;
        if (len + 1 > sizeof(path)) continue;
        memcpy( path, preload, len );
        path[len] = 0;
        if (!strstr( path, "gameoverlayrenderer.so" )) continue;
        handle = dlopen( path, RTLD_NOW | RTLD_GLOBAL );
        FIXME( "HACK: tried to load %s, handle %p.\n", debugstr_a(path), handle );
    }
}

/* The console's loader gives a library its own copy of every DT_NEEDED
 * library that is not already loaded RTLD_GLOBAL (ps5/probes/needed in the
 * Proton tree): wineps5.so, which needs win32u.so, registered its driver in a
 * private win32u, and the process's win32u kept loading a driver lazily (and
 * started explorer from inside explorer). Builtin Unix libraries are loaded
 * global there, as the loader loads ntdll.so. */
#ifdef __PROSPERO__
#define UNIXLIB_DLOPEN_FLAGS (RTLD_NOW | RTLD_GLOBAL)
#else
#define UNIXLIB_DLOPEN_FLAGS RTLD_NOW
#endif

/***********************************************************************
 *           get_builtin_unix_funcs
 */
static NTSTATUS get_builtin_unix_funcs( void *module, BOOL wow, const void **funcs )
{
    const char *ptr_name = wow ? "__wine_unix_call_wow64_funcs" : "__wine_unix_call_funcs";
    sigset_t sigset;
    NTSTATUS status = STATUS_DLL_NOT_FOUND;
    struct builtin_module *builtin;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    LIST_FOR_EACH_ENTRY( builtin, &builtin_modules, struct builtin_module, entry )
    {
        if (builtin->module != module) continue;
        if (builtin->unix_path && !builtin->unix_handle)
        {
            load_steam_overlay(builtin->unix_path);
            builtin->unix_handle = dlopen( builtin->unix_path, UNIXLIB_DLOPEN_FLAGS );
            if (!builtin->unix_handle)
                WARN_(module)( "failed to load %s: %s\n", debugstr_a(builtin->unix_path), dlerror() );
        }
        if (builtin->unix_handle)
        {
            *funcs = dlsym( builtin->unix_handle, ptr_name );
            status = *funcs ? STATUS_SUCCESS : STATUS_ENTRYPOINT_NOT_FOUND;
        }
        break;
    }
#ifdef __PROSPERO__
    if (ps5_direct_enabled() && status)
    {
        char line[2048];
        snprintf( line, sizeof(line), "wine: title Unix call init module=%p path=%s status=%08x\n",
                  module, builtin && builtin->unix_path ? builtin->unix_path : "(unregistered)", status );
        sceKernelDebugOutText( 0, line );
    }
#endif
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return status;
}


/***********************************************************************
 *           load_builtin_unixlib
 */
NTSTATUS load_builtin_unixlib( void *module, const char *name )
{
    sigset_t sigset;
    NTSTATUS status = STATUS_SUCCESS;
    struct builtin_module *builtin;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    LIST_FOR_EACH_ENTRY( builtin, &builtin_modules, struct builtin_module, entry )
    {
        if (builtin->module != module) continue;
        if (!builtin->unix_path) builtin->unix_path = strdup( name );
        else status = STATUS_IMAGE_ALREADY_LOADED;
        if (!builtin->unix_handle)
        {
            load_steam_overlay(builtin->unix_path);
            builtin->unix_handle = dlopen( builtin->unix_path, UNIXLIB_DLOPEN_FLAGS );
        }
        break;
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return status;
}


/***********************************************************************
 *           free_ranges_lower_bound
 *
 * Returns the first range whose end is not less than addr, or end if there's none.
 */
static struct range_entry *free_ranges_lower_bound( void *addr )
{
    struct range_entry *begin = free_ranges;
    struct range_entry *end = free_ranges_end;
    struct range_entry *mid;

    while (begin < end)
    {
        mid = begin + (end - begin) / 2;
        if (mid->end < addr)
            begin = mid + 1;
        else
            end = mid;
    }

    return begin;
}

static void dump_free_ranges(void)
{
    struct range_entry *r;
    for (r = free_ranges; r != free_ranges_end; ++r)
        TRACE_(virtual_ranges)("%p - %p.\n", r->base, r->end);
}

/***********************************************************************
 *           free_ranges_insert_view
 *
 * Updates the free_ranges after a new view has been created.
 */
static void free_ranges_insert_view( struct file_view *view )
{
    void *view_base = ROUND_ADDR( view->base, granularity_mask );
    void *view_end = ROUND_ADDR( (char *)view->base + view->size + granularity_mask, granularity_mask );
    struct range_entry *range = free_ranges_lower_bound( view_base );
    struct range_entry *next = range + 1;

    /* free_ranges initial value is such that the view is either inside range or before another one. */
    assert( range != free_ranges_end );
    assert( range->end > view_base || next != free_ranges_end );

    /* Free ranges addresses are aligned at granularity_mask while the views may be not. */

    if (range->base > view_base)
        view_base = range->base;
    if (range->end < view_end)
        view_end = range->end;
    if (range->end == view_base && next->base >= view_end)
        view_end = view_base;

    TRACE_(virtual_ranges)( "%p - %p, aligned %p - %p.\n",
                            view->base, (char *)view->base + view->size, view_base, view_end );

    if (view_end <= view_base)
    {
        VIRTUAL_DEBUG_DUMP_RANGES();
        return;
    }

    /* this should never happen */
    if (range->base > view_base || range->end < view_end)
        ERR( "range %p - %p is already partially mapped\n", view_base, view_end );
    assert( range->base <= view_base && range->end >= view_end );

    /* need to split the range in two */
    if (range->base < view_base && range->end > view_end)
    {
        memmove( next + 1, next, (free_ranges_end - next) * sizeof(struct range_entry) );
        free_ranges_end += 1;
        if ((char *)free_ranges_end - (char *)free_ranges > view_block_size)
            ERR( "Free range sequence is full, trouble ahead!\n" );
        assert( (char *)free_ranges_end - (char *)free_ranges <= view_block_size );

        next->base = view_end;
        next->end = range->end;
        range->end = view_base;
    }
    else
    {
        /* otherwise we just have to shrink it */
        if (range->base < view_base)
            range->end = view_base;
        else
            range->base = view_end;

        if (range->base < range->end)
        {
            VIRTUAL_DEBUG_DUMP_RANGES();
            return;
        }
        /* and possibly remove it if it's now empty */
        memmove( range, next, (free_ranges_end - next) * sizeof(struct range_entry) );
        free_ranges_end -= 1;
        assert( free_ranges_end - free_ranges > 0 );
    }
    VIRTUAL_DEBUG_DUMP_RANGES();
}

/***********************************************************************
 *           free_ranges_remove_view
 *
 * Updates the free_ranges after a view has been destroyed.
 */
static void free_ranges_remove_view( struct file_view *view )
{
    void *view_base = ROUND_ADDR( view->base, granularity_mask );
    void *view_end = ROUND_ADDR( (char *)view->base + view->size + granularity_mask, granularity_mask );
    struct range_entry *range = free_ranges_lower_bound( view_base );
    struct range_entry *next = range + 1;

    /* Free ranges addresses are aligned at granularity_mask while the views may be not. */
    struct file_view *prev_view = RB_ENTRY_VALUE( rb_prev( &view->entry ), struct file_view, entry );
    struct file_view *next_view = RB_ENTRY_VALUE( rb_next( &view->entry ), struct file_view, entry );
    void *prev_view_base = prev_view ? ROUND_ADDR( prev_view->base, granularity_mask ) : NULL;
    void *prev_view_end = prev_view ? ROUND_ADDR( (char *)prev_view->base + prev_view->size + granularity_mask, granularity_mask ) : NULL;
    void *next_view_base = next_view ? ROUND_ADDR( next_view->base, granularity_mask ) : NULL;
    void *next_view_end = next_view ? ROUND_ADDR( (char *)next_view->base + next_view->size + granularity_mask, granularity_mask ) : NULL;

    if (prev_view_end && prev_view_end > view_base && prev_view_base < view_end)
        view_base = prev_view_end;
    if (next_view_base && next_view_base < view_end && next_view_end > view_base)
        view_end = next_view_base;

    TRACE_(virtual_ranges)( "%p - %p, aligned %p - %p.\n",
                            view->base, (char *)view->base + view->size, view_base, view_end );

    if (view_end <= view_base)
    {
        VIRTUAL_DEBUG_DUMP_RANGES();
        return;
    }
    /* free_ranges initial value is such that the view is either inside range or before another one. */
    assert( range != free_ranges_end );
    assert( range->end > view_base || next != free_ranges_end );

    /* this should never happen, but we can safely ignore it */
    if (range->base <= view_base && range->end >= view_end)
    {
        WARN( "range %p - %p is already unmapped\n", view_base, view_end );
        return;
    }

    /* this should never happen */
    if (range->base < view_end && range->end > view_base)
        ERR( "range %p - %p is already partially unmapped\n", view_base, view_end );
    assert( range->end <= view_base || range->base >= view_end );

    /* merge with next if possible */
    if (range->end == view_base && next->base == view_end)
    {
        range->end = next->end;
        memmove( next, next + 1, (free_ranges_end - next - 1) * sizeof(struct range_entry) );
        free_ranges_end -= 1;
        assert( free_ranges_end - free_ranges > 0 );
    }
    /* or try growing the range */
    else if (range->end == view_base)
        range->end = view_end;
    else if (range->base == view_end)
        range->base = view_base;
    /* otherwise create a new one */
    else
    {
        memmove( range + 1, range, (free_ranges_end - range) * sizeof(struct range_entry) );
        free_ranges_end += 1;
        if ((char *)free_ranges_end - (char *)free_ranges > view_block_size)
            ERR( "Free range sequence is full, trouble ahead!\n" );
        assert( (char *)free_ranges_end - (char *)free_ranges <= view_block_size );

        range->base = view_base;
        range->end = view_end;
    }
    VIRTUAL_DEBUG_DUMP_RANGES();
}


static inline int is_view_valloc( const struct file_view *view )
{
    return !(view->protect & (SEC_FILE | SEC_RESERVE | SEC_COMMIT));
}

/***********************************************************************
 *           get_page_vprot
 *
 * Return the page protection byte.
 */
static BYTE get_page_vprot( const void *addr )
{
    size_t idx = (size_t)addr >> page_shift;

#ifdef _WIN64
    if ((idx >> pages_vprot_shift) >= pages_vprot_size) return 0;
    if (!pages_vprot[idx >> pages_vprot_shift]) return 0;
    return pages_vprot[idx >> pages_vprot_shift][idx & pages_vprot_mask];
#else
    return pages_vprot[idx];
#endif
}


/***********************************************************************
 *           get_host_page_vprot
 *
 * Return the union of the page protection bytes of all the pages making up the host page.
 */
static BYTE get_host_page_vprot( const void *addr )
{
    size_t i, idx = (size_t)ROUND_ADDR( addr, host_page_mask ) >> page_shift;
    const BYTE *vprot_ptr;
    BYTE vprot = 0;

#ifdef _WIN64
    if ((idx >> pages_vprot_shift) >= pages_vprot_size) return 0;
    if (!pages_vprot[idx >> pages_vprot_shift]) return 0;
    assert( host_page_mask >> page_shift <= pages_vprot_mask );
    vprot_ptr = pages_vprot[idx >> pages_vprot_shift] + (idx & pages_vprot_mask);
#else
    vprot_ptr = pages_vprot + idx;
#endif
    for (i = 0; i < host_page_size / page_size; i++) vprot |= vprot_ptr[i];
    return vprot;
}


/***********************************************************************
 *           get_vprot_range_size
 *
 * Return the size of the region with equal masked vprot byte.
 * Also return the protections for the first page.
 * The function assumes that base and size are page aligned,
 * base + size does not wrap around and the range is within view so
 * vprot bytes are allocated for the range. */
static SIZE_T get_vprot_range_size( char *base, SIZE_T size, BYTE mask, BYTE *vprot )
{
    static const UINT_PTR word_from_byte = (UINT_PTR)0x101010101010101;
    static const UINT_PTR index_align_mask = sizeof(UINT_PTR) - 1;
    SIZE_T curr_idx, start_idx, end_idx, aligned_start_idx;
    UINT_PTR vprot_word, mask_word;
    const BYTE *vprot_ptr;

    TRACE("base %p, size %p, mask %#x.\n", base, (void *)size, mask);

    curr_idx = start_idx = (size_t)base >> page_shift;
    end_idx = start_idx + (size >> page_shift);

    aligned_start_idx = ROUND_SIZE( 0, start_idx, index_align_mask );
    if (aligned_start_idx > end_idx) aligned_start_idx = end_idx;

#ifdef _WIN64
    vprot_ptr = pages_vprot[curr_idx >> pages_vprot_shift] + (curr_idx & pages_vprot_mask);
#else
    vprot_ptr = pages_vprot + curr_idx;
#endif
    *vprot = *vprot_ptr;

    /* Page count page table is at least the multiples of sizeof(UINT_PTR)
     * so we don't have to worry about crossing the boundary on unaligned idx values. */

    for (; curr_idx < aligned_start_idx; ++curr_idx, ++vprot_ptr)
        if ((*vprot ^ *vprot_ptr) & mask) return (curr_idx - start_idx) << page_shift;

    vprot_word = word_from_byte * *vprot;
    mask_word = word_from_byte * mask;
    for (; curr_idx < end_idx; curr_idx += sizeof(UINT_PTR), vprot_ptr += sizeof(UINT_PTR))
    {
#ifdef _WIN64
        if (!(curr_idx & pages_vprot_mask)) vprot_ptr = pages_vprot[curr_idx >> pages_vprot_shift];
#endif
        if ((vprot_word ^ *(UINT_PTR *)vprot_ptr) & mask_word)
        {
            for (; curr_idx < end_idx; ++curr_idx, ++vprot_ptr)
                if ((*vprot ^ *vprot_ptr) & mask) break;
            return (curr_idx - start_idx) << page_shift;
        }
    }
    return size;
}

/***********************************************************************
 *           set_page_vprot
 *
 * Set a range of page protection bytes.
 */
static void set_page_vprot( const void *addr, size_t size, BYTE vprot )
{
    size_t idx = (size_t)addr >> page_shift;
    size_t end = ((size_t)addr + size + page_mask) >> page_shift;

#ifdef _WIN64
    while (idx >> pages_vprot_shift != end >> pages_vprot_shift)
    {
        size_t dir_size = pages_vprot_mask + 1 - (idx & pages_vprot_mask);
        memset( pages_vprot[idx >> pages_vprot_shift] + (idx & pages_vprot_mask), vprot, dir_size );
        idx += dir_size;
    }
    memset( pages_vprot[idx >> pages_vprot_shift] + (idx & pages_vprot_mask), vprot, end - idx );
#else
    memset( pages_vprot + idx, vprot, end - idx );
#endif
}


/***********************************************************************
 *           set_page_vprot_bits
 *
 * Set or clear bits in a range of page protection bytes.
 */
static void set_page_vprot_bits( const void *addr, size_t size, BYTE set, BYTE clear )
{
    size_t idx = (size_t)addr >> page_shift;
    size_t end = ((size_t)addr + size + page_mask) >> page_shift;

#ifdef _WIN64
    for ( ; idx < end; idx++)
    {
        BYTE *ptr = pages_vprot[idx >> pages_vprot_shift] + (idx & pages_vprot_mask);
        *ptr = (*ptr & ~clear) | set;
    }
#else
    for ( ; idx < end; idx++) pages_vprot[idx] = (pages_vprot[idx] & ~clear) | set;
#endif
}


/***********************************************************************
 *           set_page_vprot_exec_write_protect
 *
 * Write protect pages that are executable.
 */
static BOOL set_page_vprot_exec_write_protect( const void *addr, size_t size )
{
    BOOL ret = FALSE;
#ifdef _WIN64 /* only supported on 64-bit so assume 2-level table */
    size_t idx = (size_t)addr >> page_shift;
    size_t end = ((size_t)addr + size + page_mask) >> page_shift;

    for ( ; idx < end; idx++)
    {
        BYTE *ptr = pages_vprot[idx >> pages_vprot_shift] + (idx & pages_vprot_mask);
        if (!is_vprot_exec_write( *ptr )) continue;
        *ptr |= VPROT_WRITEWATCH;
        ret = TRUE;
    }
#endif
    return ret;
}


/***********************************************************************
 *           alloc_pages_vprot
 *
 * Allocate the page protection bytes for a given range.
 */
static BOOL alloc_pages_vprot( const void *addr, size_t size )
{
#ifdef _WIN64
    size_t idx = (size_t)addr >> page_shift;
    size_t end = ((size_t)addr + size + page_mask) >> page_shift;
    size_t i;
    void *ptr;

    assert( end <= pages_vprot_size << pages_vprot_shift );
    for (i = idx >> pages_vprot_shift; i < (end + pages_vprot_mask) >> pages_vprot_shift; i++)
    {
        if (pages_vprot[i]) continue;
        if ((ptr = anon_mmap_alloc( pages_vprot_mask + 1, PROT_READ | PROT_WRITE )) == MAP_FAILED)
        {
            ERR( "anon mmap error %s for vprot table, size %08lx\n", strerror(errno), pages_vprot_mask + 1 );
            return FALSE;
        }
        pages_vprot[i] = ptr;
    }
#endif
    return TRUE;
}


static inline UINT64 maskbits( size_t idx )
{
    return ~(UINT64)0 << (idx & 63);
}

/***********************************************************************
 *           set_arm64ec_range
 */
static void set_arm64ec_range( const void *addr, size_t size )
{
    UINT64 *map = arm64ec_view->base;
    size_t idx = (size_t)addr >> page_shift;
    size_t end = ((size_t)addr + size + page_mask) >> page_shift;
    size_t pos = idx / 64;
    size_t end_pos = end / 64;

    if (end_pos > pos)
    {
        map[pos++] |= maskbits( idx );
        while (pos < end_pos) map[pos++] = ~(UINT64)0;
        if (end & 63) map[pos] |= ~maskbits( end );
    }
    else map[pos] |= maskbits( idx ) & ~maskbits( end );
}


/***********************************************************************
 *           clear_arm64ec_range
 */
static void clear_arm64ec_range( const void *addr, size_t size )
{
    UINT64 *map = arm64ec_view->base;
    size_t idx = (size_t)addr >> page_shift;
    size_t end = ((size_t)addr + size + page_mask) >> page_shift;
    size_t pos = idx / 64;
    size_t end_pos = end / 64;

    if (end_pos > pos)
    {
        map[pos++] &= ~maskbits( idx );
        while (pos < end_pos) map[pos++] = 0;
        if (end & 63) map[pos] &= maskbits( end );
    }
    else map[pos] &= ~maskbits( idx ) | maskbits( end );
}


/***********************************************************************
 *           compare_view
 *
 * View comparison function used for the rb tree.
 */
static int compare_view( const void *addr, const struct wine_rb_entry *entry )
{
    struct file_view *view = WINE_RB_ENTRY_VALUE( entry, struct file_view, entry );

    if (addr < view->base) return -1;
    if (addr > view->base) return 1;
    return 0;
}


/***********************************************************************
 *           get_prot_str
 */
static const char *get_prot_str( BYTE prot )
{
    static char buffer[6];
    buffer[0] = (prot & VPROT_COMMITTED) ? 'c' : '-';
    buffer[1] = (prot & VPROT_GUARD) ? 'g' : ((prot & VPROT_WRITEWATCH) ? 'H' : '-');
    buffer[2] = (prot & VPROT_READ) ? 'r' : '-';
    buffer[3] = (prot & VPROT_WRITECOPY) ? (prot & VPROT_COPIED ? 'w' : 'W')
        : ((prot & VPROT_WRITE) ? 'w' : '-');
    buffer[4] = (prot & VPROT_EXEC) ? 'x' : '-';
    buffer[5] = 0;
    return buffer;
}


/***********************************************************************
 *           get_unix_prot
 *
 * Convert page protections to protection for mmap/mprotect.
 */
static int get_unix_prot( BYTE vprot )
{
    int prot = 0;
    if ((vprot & VPROT_COMMITTED) && !(vprot & VPROT_GUARD))
    {
        if (vprot & VPROT_READ) prot |= PROT_READ;
        if (vprot & VPROT_WRITE) prot |= PROT_WRITE | PROT_READ;
        if (vprot & VPROT_WRITECOPY) prot |= PROT_WRITE | PROT_READ;
        if (vprot & VPROT_EXEC) prot |= PROT_EXEC | PROT_READ;
        if (vprot & VPROT_WRITEWATCH) prot &= ~PROT_WRITE;
    }
    if (!prot) prot = PROT_NONE;
    return prot;
}


/***********************************************************************
 *           dump_view
 */
static void dump_view( struct file_view *view )
{
    UINT i, count;
    char *addr = view->base;
    BYTE prot = get_page_vprot( addr );

    TRACE( "View: %p - %p %s", addr, addr + view->size - 1, get_prot_str(view->protect) );
    if (view->protect & VPROT_NATIVE)
        TRACE(" (native)\n");
    else if (view->protect & VPROT_SYSTEM)
        TRACE( " (builtin image)\n" );
    else if (view->protect & VPROT_FREE_PLACEHOLDER)
        TRACE( " (placeholder)\n" );
    else if (view->protect & SEC_IMAGE)
        TRACE( " (image)\n" );
    else if (view->protect & SEC_FILE)
        TRACE( " (file)\n" );
    else if (view->protect & (SEC_RESERVE | SEC_COMMIT))
        TRACE( " (anonymous)\n" );
    else
        TRACE( " (valloc)\n");

    for (count = i = 1; i < view->size >> page_shift; i++, count++)
    {
        BYTE next = get_page_vprot( addr + (count << page_shift) );
        if (next == prot) continue;
        TRACE( "      %p - %p %s\n",
                 addr, addr + (count << page_shift) - 1, get_prot_str(prot) );
        addr += (count << page_shift);
        prot = next;
        count = 0;
    }
    if (count)
        TRACE( "      %p - %p %s\n",
                 addr, addr + (count << page_shift) - 1, get_prot_str(prot) );
}


/***********************************************************************
 *           VIRTUAL_Dump
 */
#ifdef WINE_VM_DEBUG
static void VIRTUAL_Dump(void)
{
    sigset_t sigset;
    struct file_view *view;

    TRACE( "Dump of all virtual memory views:\n" );
    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    WINE_RB_FOR_EACH_ENTRY( view, &views_tree, struct file_view, entry )
    {
        dump_view( view );
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
}
#endif

#ifdef __PROSPERO__
static const char *ps5_hang_module_path( const void *module )
{
    struct builtin_module *builtin;
#if defined(HAVE_LINK_H) || defined(HAVE_SYS_LINK_H)
    struct link_map *map;
#endif

    LIST_FOR_EACH_ENTRY( builtin, &builtin_modules, struct builtin_module, entry )
        if (builtin->module == module && builtin->unix_path) return builtin->unix_path;
#if defined(HAVE_LINK_H) || defined(HAVE_SYS_LINK_H)
    for (map = link_map.l_next; map; map = map->l_next)
    {
        struct link_map_entry *entry = LIST_ENTRY( map, struct link_map_entry, map );
        if (entry->module == module && map->l_name) return map->l_name;
    }
#endif
    return "image";
}

static struct file_view *find_view( const void *addr, size_t size );
static BOOL is_host_page_closed( const char *page );

static BOOL ps5_title_safe_copy( void *dest, const void *src, size_t size )
{
    BOOL copied = TRUE;

    if (ntdll_get_thread_data()->jmp_buf) return FALSE;
    __TRY
    {
        memcpy( dest, src, size );
    }
    __EXCEPT
    {
        copied = FALSE;
    }
    __ENDTRY
    return copied;
}

/* Caller holds virtual_mutex. Check Wine's 4-KiB protection and the actual
 * 16-KiB host-page state without touching the application range. */
static BOOL ps5_title_range_readable_locked( const void *ptr, size_t size, BYTE required )
{
    struct file_view *view;
    ULONG_PTR page, end = (ULONG_PTR)ptr + size;
    BYTE mask = VPROT_COMMITTED | VPROT_READ | VPROT_GUARD | required;
    BYTE expected = VPROT_COMMITTED | VPROT_READ | required;

    if (!(view = find_view( ptr, size )) || (view->protect & VPROT_SYSTEM)) return FALSE;
    for (page = (ULONG_PTR)ptr & ~page_mask; page < end; page += page_size)
    {
        BYTE vprot = get_page_vprot( (void *)page );
        char *host = ROUND_ADDR( (void *)page, host_page_mask );

        if ((vprot & mask) != expected ||
            !(get_unix_prot( get_host_page_vprot( host )) & PROT_READ) ||
            is_host_page_closed( host ))
            return FALSE;
    }
    return TRUE;
}

/* The caller first bounds this range against the current TEB stack. Read only
 * complete pointer slots, then classify their values without dereferencing
 * candidate code addresses. */
unsigned int virtual_find_title_stack_addresses( const ULONG_PTR *stack, size_t slots,
                                                 struct ps5_title_stack_address *addresses,
                                                 unsigned int max_addresses )
{
    ULONG_PTR stack_copy[WINE_PS5_HANG_STACK_SLOTS];
    struct file_view *view;
    unsigned int count = 0;
    BOOL readable;
    size_t i, j;

    if (!__wine_ps5_is_title() || !slots || !max_addresses ||
        slots > ARRAY_SIZE(stack_copy) || slots > (size_t)-1 / sizeof(*stack))
        return 0;

    if (pthread_mutex_trylock( &virtual_mutex )) return 0;
    readable = ps5_title_range_readable_locked( stack, slots * sizeof(*stack), 0 );
    pthread_mutex_unlock( &virtual_mutex );
    /* Never put application memory behind virtual_mutex. A concurrent mapping
     * change can only make this diagnostic sample incomplete. */
    if (!readable || !ps5_title_safe_copy( stack_copy, stack, slots * sizeof(*stack) ))
        return 0;

    if (pthread_mutex_trylock( &virtual_mutex )) return 0;
    for (i = 0; i < slots && count < max_addresses; i++)
    {
        struct ps5_title_stack_address *address;
        BYTE vprot;
        ULONG_PTR pc = stack_copy[i];

        if (!(view = find_view( (void *)pc, 0 ))) continue;
        vprot = get_page_vprot( (void *)pc );
        if ((vprot & (VPROT_COMMITTED | VPROT_EXEC | VPROT_GUARD)) !=
            (VPROT_COMMITTED | VPROT_EXEC))
            continue;
        for (j = 0; j < count; j++) if (addresses[j].pc == pc) break;
        if (j != count) continue;

        address = &addresses[count++];
        address->pc = pc;
        address->base = address->size = 0;
        address->module[0] = 0;
    }
    pthread_mutex_unlock( &virtual_mutex );
    return count;
}

/* The loader lock cannot be acquired from a signal handler. Walk a bounded,
 * fault-guarded view of its PE list; a concurrent loader mutation can only
 * make this sample incomplete. */
void virtual_resolve_title_pe_modules( struct ps5_title_stack_address *addresses,
                                       unsigned int count )
{
    PEB_LDR_DATA *ldr;
    LIST_ENTRY *head, *entry, list;
    ULONG_PTR ldr_ptr;
    unsigned int visited, i;

    if (!count || !peb || !ps5_title_safe_copy( &ldr, &peb->LdrData, sizeof(ldr) ) || !ldr)
        return;
    ldr_ptr = (ULONG_PTR)ldr;
    if (ldr_ptr > ~(ULONG_PTR)0 - offsetof( PEB_LDR_DATA, InMemoryOrderModuleList )) return;
    head = (LIST_ENTRY *)(ldr_ptr + offsetof( PEB_LDR_DATA, InMemoryOrderModuleList ));
    if (!ps5_title_safe_copy( &list, head, sizeof(list) )) return;

    for (entry = list.Flink, visited = 0;
         entry && entry != head && visited < PS5_HANG_MODULE_MAX; visited++)
    {
        LDR_DATA_TABLE_ENTRY module;
        ULONG_PTR module_ptr;

        if ((ULONG_PTR)entry < offsetof( LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks )) break;
        module_ptr = (ULONG_PTR)entry - offsetof( LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        if (!ps5_title_safe_copy( &module, (void *)module_ptr, sizeof(module) )) break;
        entry = module.InMemoryOrderLinks.Flink;
        for (i = 0; i < count; i++)
        {
            ULONG_PTR base = (ULONG_PTR)module.DllBase;
            ULONG_PTR name_ptr;
            size_t length, j;
            char name[sizeof(addresses[i].module)];

            if (addresses[i].base ||
                !wine_ps5_hang_address_in_image( addresses[i].pc, base, module.SizeOfImage ))
                continue;
            length = module.BaseDllName.Length / sizeof(WCHAR);
            name_ptr = (ULONG_PTR)module.BaseDllName.Buffer;
            if (!name_ptr || !length) continue;
            if (length >= sizeof(name)) length = sizeof(name) - 1;
            if (name_ptr > ~(ULONG_PTR)0 - (length - 1) * sizeof(WCHAR)) continue;
            for (j = 0; j < length; j++)
            {
                WCHAR ch;

                if (!ps5_title_safe_copy( &ch, (void *)(name_ptr + j * sizeof(WCHAR)),
                                          sizeof(ch) ))
                    break;
                name[j] = ch >= 0x20 && ch < 0x7f ? ch : '?';
            }
            if (j != length) continue;
            name[length] = 0;
            addresses[i].base = base;
            addresses[i].size = module.SizeOfImage;
            memcpy( addresses[i].module, name, length + 1 );
        }
    }
}

/* Copy only a short instruction window whose complete Wine range is committed,
 * readable and executable. The host mapping must currently be readable too;
 * diagnostics never open a sealed page or change its protection. */
BOOL virtual_read_title_code( ULONG_PTR pc, BYTE *bytes, size_t size )
{
    BOOL readable;

    if (!__wine_ps5_is_title() || !bytes || !size || size > WINE_PS5_HANG_CODE_BEFORE_BYTES ||
        pc > ~(ULONG_PTR)0 - size || pthread_mutex_trylock( &virtual_mutex ))
        return FALSE;
    readable = ps5_title_range_readable_locked( (void *)pc, size, VPROT_EXEC );
    pthread_mutex_unlock( &virtual_mutex );
    /* Fault containment runs only after releasing virtual_mutex. */
    return readable && ps5_title_safe_copy( bytes, (void *)pc, size );
}

/* Validate the node and its +8 link while holding VM metadata, then copy the
 * single pointer only after dropping virtual_mutex into the signal fault guard. */
BOOL virtual_read_title_pointer_link( ULONG_PTR address, ULONG_PTR *next )
{
    BOOL readable;

    if (!__wine_ps5_is_title() || !address || !next ||
        address > ~(ULONG_PTR)0 - 2 * sizeof(ULONG_PTR) ||
        pthread_mutex_trylock( &virtual_mutex ))
        return FALSE;
    readable = ps5_title_range_readable_locked( (void *)address, 2 * sizeof(ULONG_PTR), 0 );
    pthread_mutex_unlock( &virtual_mutex );
    return readable && ps5_title_safe_copy( next, (void *)(address + sizeof(ULONG_PTR)),
                                            sizeof(*next) );
}

static DWORD get_win32_prot( BYTE vprot, unsigned int map_prot );

/* Inspect only the VM metadata for a bounded register set. No application byte
 * is read, and a contended virtual lock makes the peek unavailable. */
unsigned int virtual_query_title_readable_regions(
    const ULONG_PTR *addresses, struct ps5_title_memory_region *regions, unsigned int count )
{
    unsigned int i, valid = 0;

    if (!__wine_ps5_is_title() || !addresses || !regions ||
        !count || count > WINE_PS5_HANG_MEMORY_REGS ||
        pthread_mutex_trylock( &virtual_mutex ))
        return 0;
    memset( regions, 0, count * sizeof(*regions) );
    for (i = 0; i < count; i++)
    {
        struct file_view *view;
        BYTE vprot;

        if (!addresses[i] || !(view = find_view( (void *)addresses[i], 0 ))) continue;
        vprot = get_page_vprot( (void *)addresses[i] );
        if ((vprot & (VPROT_COMMITTED | VPROT_READ | VPROT_GUARD)) !=
            (VPROT_COMMITTED | VPROT_READ))
            continue;
        regions[i].base = (ULONG_PTR)view->base;
        regions[i].size = view->size;
        regions[i].protect = get_win32_prot( vprot, view->protect );
        valid |= 1u << i;
    }
    pthread_mutex_unlock( &virtual_mutex );
    return valid;
}

unsigned int virtual_prepare_hang_module_snapshot( unsigned int *mutex_owner,
                                                   ULONG_PTR *mutex_site )
{
    struct file_view *view;
    unsigned int count = 0, sequence;

    if (mutex_owner) *mutex_owner = 0;
    if (mutex_site) *mutex_site = 0;
    if (!__wine_ps5_is_title()) return 0;
    if (pthread_mutex_trylock( &virtual_mutex ))
    {
        unsigned int owner = ps5_virtual_mutex_owner( mutex_site );

        if (mutex_owner) *mutex_owner = owner;
        return 0;
    }
    sequence = __atomic_load_n( &ps5_hang_module_sequence, __ATOMIC_RELAXED );
    __atomic_store_n( &ps5_hang_module_sequence, sequence + 1, __ATOMIC_RELEASE );
    WINE_RB_FOR_EACH_ENTRY( view, &views_tree, struct file_view, entry )
    {
        struct ps5_hang_module *module;
        const char *path, *name, *slash;
        size_t i;

        if (!(view->protect & SEC_IMAGE) || count == ARRAY_SIZE(ps5_hang_modules)) continue;
        module = &ps5_hang_modules[count++];
        module->base = (ULONG_PTR)view->base;
        module->end = module->base + view->size;
        path = ps5_hang_module_path( view->base );
        name = path;
        if ((slash = strrchr( path, '/' ))) name = slash + 1;
        if ((slash = strrchr( name, '\\' ))) name = slash + 1;
        for (i = 0; i + 1 < sizeof(module->name) && name[i]; i++) module->name[i] = name[i];
        module->name[i] = 0;
    }
    __atomic_store_n( &ps5_hang_module_count, count, __ATOMIC_RELAXED );
    __atomic_store_n( &ps5_hang_module_sequence, sequence + 2, __ATOMIC_RELEASE );
    pthread_mutex_unlock( &virtual_mutex );
    return count;
}

BOOL virtual_find_hang_module( ULONG_PTR pc, ULONG_PTR *base, char *name, size_t size )
{
    unsigned int sequence, count, i;

    sequence = __atomic_load_n( &ps5_hang_module_sequence, __ATOMIC_ACQUIRE );
    if (sequence & 1) return FALSE;
    count = __atomic_load_n( &ps5_hang_module_count, __ATOMIC_RELAXED );
    for (i = 0; i < count; i++)
        if (wine_ps5_hang_address_in_image( pc, ps5_hang_modules[i].base,
                                            ps5_hang_modules[i].end -
                                            ps5_hang_modules[i].base ))
        {
            size_t j;

            if (base) *base = ps5_hang_modules[i].base;
            if (name && size)
            {
                for (j = 0; j + 1 < size && ps5_hang_modules[i].name[j]; j++)
                    name[j] = ps5_hang_modules[i].name[j];
                name[j] = 0;
            }
            return sequence == __atomic_load_n( &ps5_hang_module_sequence, __ATOMIC_ACQUIRE );
        }
    return FALSE;
}
#endif


static void dump_memory_statistics(void)
{
    struct file_view *view;
    SIZE_T anon_reserved = 0, anon_committed = 0, mapped = 0, mapped_committed = 0, marked_native = 0;
    SIZE_T size, c_size;
    char *base;
    BYTE vprot;

    if (!TRACE_ON(virtstat)) return;

    WINE_RB_FOR_EACH_ENTRY( view, &views_tree, struct file_view, entry )
    {
        if (view->protect & VPROT_NATIVE)
        {
            marked_native += view->size;
            continue;
        }
        base = view->base;
        c_size = 0;
        while (base != (char *)view->base + view->size)
        {
            size = get_vprot_range_size( base, (char *)view->base + view->size - base, VPROT_COMMITTED, &vprot );
            if (vprot & VPROT_COMMITTED) c_size += size;
            base += size;
        }
        if (is_view_valloc( view ))
        {
            anon_reserved += view->size;
            anon_committed += c_size;
        }
        else
        {
            mapped += view->size;
            mapped_committed += c_size;
        }
    }

    anon_reserved /= 1024 * 1024;
    anon_committed /= 1024 * 1024;
    mapped /= 1024 * 1024;
    mapped_committed /= 1024 * 1024;
    marked_native /= 1024 * 1024;
    TRACE_(virtstat)( "Total: res %lu, comm %lu; Anon: res %lu, comm %lu, marked Unix %lu.\n",
                      anon_reserved + mapped, anon_committed + mapped_committed, anon_reserved, anon_committed,
                      marked_native );
}

/***********************************************************************
 *           find_view
 *
 * Find the view containing a given address. virtual_mutex must be held by caller.
 *
 * PARAMS
 *      addr  [I] Address
 *
 * RETURNS
 *	View: Success
 *	NULL: Failure
 */
static struct file_view *find_view( const void *addr, size_t size )
{
    struct wine_rb_entry *ptr = views_tree.root;

    if ((const char *)addr + size < (const char *)addr) return NULL; /* overflow */

    while (ptr)
    {
        struct file_view *view = WINE_RB_ENTRY_VALUE( ptr, struct file_view, entry );

        if (view->base > addr) ptr = ptr->left;
        else if ((const char *)view->base + view->size <= (const char *)addr) ptr = ptr->right;
        else if ((const char *)view->base + view->size < (const char *)addr + size) break;  /* size too large */
        else return view;
    }
    return NULL;
}


/***********************************************************************
 *           is_write_watch_range
 */
static inline BOOL is_write_watch_range( const void *addr, size_t size )
{
    struct file_view *view = find_view( addr, size );
    return view && (view->protect & VPROT_WRITEWATCH);
}


/***********************************************************************
 *           find_view_range
 *
 * Find the first view overlapping at least part of the specified range.
 * virtual_mutex must be held by caller.
 */
static struct file_view *find_view_range( const void *addr, size_t size )
{
    struct wine_rb_entry *ptr = views_tree.root;

    while (ptr)
    {
        struct file_view *view = WINE_RB_ENTRY_VALUE( ptr, struct file_view, entry );

        if ((const char *)view->base >= (const char *)addr + size) ptr = ptr->left;
        else if ((const char *)view->base + view->size <= (const char *)addr) ptr = ptr->right;
        else return view;
    }
    return NULL;
}



struct alloc_area
{
    size_t size;
    ptrdiff_t step;
    int unix_prot;
    BOOL top_down;
    UINT_PTR align_mask;
    char *native_mapped;
    size_t native_mapped_size;
#ifdef __PROSPERO__
    BOOL direct;
#endif
};

static void *map_alloc_area( const struct alloc_area *area, void *base )
{
#ifdef __PROSPERO__
    void *ret;
    if (ps5_title_radv_ranges && wine_ps5_title_radv_overlap( (UINT_PTR)base, area->size, NULL, NULL ))
    {
        errno = EEXIST;
        return MAP_FAILED;
    }
    if (!title_direct_cache_evict_range( base, area->size ))
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    ret = area->direct ? ps5_direct_reserve( base, area->size ) :
                        anon_mmap_tryfixed( base, area->size, area->unix_prot, 0 );
    if (ret != MAP_FAILED) ps5_title_note_reservation( ret, area->size );
    return ret;
#else
    return anon_mmap_tryfixed( base, area->size, area->unix_prot, 0 );
#endif
}

/***********************************************************************
 *           try_map_free_area
 *
 * Try mmaping some expected free memory region, eventually stepping and
 * retrying inside it, and return where it actually succeeded, or NULL.
 */
static void* try_map_free_area( struct alloc_area *area, void *base, void *end, void *start )
{
    ptrdiff_t step = area->step;
    size_t abs_step = step > 0 ? step : -step;
    size_t size = area->size;
    int unix_prot = area->unix_prot;

    while (start && base <= start && (char*)start + size <= (char*)end)
    {
        if (map_alloc_area( area, start ) != MAP_FAILED) return start;
        TRACE( "Found free area is already mapped, start %p.\n", start );
        if (errno != EEXIST)
        {
            ERR( "mmap() error %s, range %p-%p, unix_prot %#x.\n",
                 strerror(errno), start, (char *)start + size, unix_prot );
            return NULL;
        }
        if (!area->native_mapped && step && abs_step < (granularity_mask + 1) * 2)
        {
            area->native_mapped = start;
            area->native_mapped_size = abs_step;
            area->native_mapped_size = min(area->native_mapped_size, (char *)end - (char *)start);
        }
        if ((step > 0 && (char *)end - (char *)start < step) ||
            (step < 0 && (char *)start - (char *)base < -step) ||
            step == 0)
            break;
        start = (char *)start + step;
        if (increase_try_map_step && llabs(step) < max_try_map_step)
            step *= 2;
    }

    return NULL;
}


/***********************************************************************
 *           remove_reserved_area
 *
 * Remove a reserved area from the list maintained by libwine.
 * virtual_mutex must be held by caller.
 */
static void remove_reserved_area( void *addr, size_t size )
{
    struct file_view *view;
    size_t view_size;

    TRACE( "removing %p-%p\n", addr, (char *)addr + size );
    mmap_remove_reserved_area( addr, size );
#ifdef __PROSPERO__
    return;  /* nothing was mapped for it, see ps5_reserve_free_ranges */
#endif

    /* unmap areas not covered by an existing view */
    WINE_RB_FOR_EACH_ENTRY( view, &views_tree, struct file_view, entry )
    {
        if ((char *)view->base >= (char *)addr + size) break;
        if ((char *)view->base + view->size <= (char *)addr) continue;
        if (view->base > addr) munmap( addr, (char *)view->base - (char *)addr );
        if ((char *)view->base + view->size > (char *)addr + size) return;
        view_size = ROUND_SIZE( view->base, view->size, host_page_mask );
        size = (char *)addr + size - ((char *)view->base + view_size);
        addr = (char *)view->base + view_size;
    }
    munmap( addr, size );
}


/***********************************************************************
 *           unmap_area
 *
 * Unmap an area, or simply replace it by an empty mapping if it is
 * in a reserved area. virtual_mutex must be held by caller.
 */
static void unmap_area( void *start, size_t size )
{
    struct reserved_area *area;
    void *end;

    assert( !((UINT_PTR)start & host_page_mask) );
    size = ROUND_SIZE( 0, size, host_page_mask );

    if (!(size = unmap_area_above_user_limit( start, size ))) return;

    end = (char *)start + size;

#ifdef __PROSPERO__
    /* reserved areas are not mapped there, see ps5_reserve_free_ranges */
    munmap( start, size );
    return;
#endif
    LIST_FOR_EACH_ENTRY( area, &reserved_areas, struct reserved_area, entry )
    {
        void *area_start = area->base;
        void *area_end = (char *)area_start + area->size;

        if (area_start >= end) break;
        if (area_end <= start) continue;
        if (area_start > start)
        {
            munmap( start, (char *)area_start - (char *)start );
            start = area_start;
        }
        if (area_end >= end)
        {
            anon_mmap_fixed( start, (char *)end - (char *)start, PROT_NONE, MAP_NORESERVE );
            return;
        }
        anon_mmap_fixed( start, (char *)area_end - (char *)start, PROT_NONE, MAP_NORESERVE );
        start = area_end;
    }
    munmap( start, (char *)end - (char *)start );
}


/***********************************************************************
 *           alloc_view
 *
 * Allocate a new view. virtual_mutex must be held by caller.
 */
static struct file_view *alloc_view(void)
{
    if (next_free_view)
    {
        struct file_view *ret = next_free_view;
        next_free_view = *(struct file_view **)ret;
        return ret;
    }
    if (view_block_start == view_block_end)
    {
        void *ptr = anon_mmap_alloc( view_block_size, PROT_READ | PROT_WRITE );
        if (ptr == MAP_FAILED) return NULL;
        view_block_start = ptr;
        view_block_end = view_block_start + view_block_size / sizeof(*view_block_start);
    }
    return view_block_start++;
}


/***********************************************************************
 *           free_view
 *
 * Free memory for view structure. virtual_mutex must be held by caller.
 */
static void free_view( struct file_view *view )
{
#ifdef __PROSPERO__
    if (view->protect & VPROT_PS5_DIRECT) ps5_direct_free_view( view->direct );
#endif
    *(struct file_view **)view = next_free_view;
    next_free_view = view;
}


/***********************************************************************
 *           unregister_view
 *
 * Remove view from the tree and update free ranges. virtual_mutex must be held by caller.
 */
static void unregister_view( struct file_view *view )
{
    free_ranges_remove_view( view );
    wine_rb_remove( &views_tree, &view->entry );
}

#ifdef __PROSPERO__
static NTSTATUS ps5_free_pages( struct file_view *view, char *base, size_t size );
#endif


/***********************************************************************
 *           delete_view
 *
 * Deletes a view. virtual_mutex must be held by caller.
 */
static NTSTATUS delete_view( struct file_view *view ) /* [in] View */
{
#ifdef __PROSPERO__
    if (title_direct_cache_put( view )) return STATUS_SUCCESS;
    if (view->protect & VPROT_PS5_DIRECT)
    {
        NTSTATUS status = ps5_free_pages( view, view->base, view->size );
        if (status) return status;
    }
    else
#endif
    if (!(view->protect & VPROT_SYSTEM)) unmap_area( view->base, view->size );
    set_page_vprot( view->base, view->size, 0 );
    if (view->protect & VPROT_ARM64EC) clear_arm64ec_range( view->base, view->size );
#ifdef __PROSPERO__
    if (title_profile_is_enabled()) title_map_unmapped( view );
#endif
    unregister_view( view );
    free_view( view );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           register_view
 *
 * Add view to the tree and update free ranges. virtual_mutex must be held by caller.
 */
static void register_view( struct file_view *view )
{
    wine_rb_put( &views_tree, view->base, &view->entry );
    free_ranges_insert_view( view );
}


/***********************************************************************
 *           create_view
 *
 * Create a view. virtual_mutex must be held by caller.
 */
static NTSTATUS create_view( struct file_view **view_ret, void *base, size_t size, unsigned int vprot )
{
    struct file_view *view;

    assert( !((UINT_PTR)base & host_page_mask) );
    assert( !(size & page_mask) );

    /* Check for overlapping views. This can happen if the previous view
     * was a system view that got unmapped behind our back. In that case
     * we recover by simply deleting it. */

    while ((view = find_view_range( base, size )))
    {
        TRACE( "overlapping view %p-%p for %p-%p\n",
               view->base, (char *)view->base + view->size, base, (char *)base + size );
        assert( view->protect & VPROT_SYSTEM );
        delete_view( view );
    }

    if (!alloc_pages_vprot( base, size )) return STATUS_NO_MEMORY;

    /* Create the view structure */

    if (!(view = alloc_view()))
    {
        FIXME( "out of memory for %p-%p\n", base, (char *)base + size );
        return STATUS_NO_MEMORY;
    }

    view->base    = base;
    view->size    = size;
    view->protect = vprot;
#ifdef __PROSPERO__
    view->direct = NULL;
    view->map_profile = 0;
    if (vprot & VPROT_PS5_DIRECT)
    {
        view->direct = ps5_direct_create_view( base, ROUND_SIZE( 0, size, granularity_mask ) );
        if (!view->direct)
        {
            view->protect &= ~VPROT_PS5_DIRECT;
            free_view( view );
            return STATUS_NO_MEMORY;
        }
        vprot &= ~VPROT_COMMITTED;  /* publish commit only after backing and protections succeed */
    }
#endif
    if (use_kernel_writewatch) vprot &= ~VPROT_WRITEWATCH;
    set_page_vprot( base, size, vprot );

    register_view( view );
    kernel_writewatch_register_range( view, view->base, view->size );

    *view_ret = view;
    return STATUS_SUCCESS;
}

#ifdef __PROSPERO__
/* WINE_PS5_VM_CPU_TEST_BEGIN */
enum title_vm_cpu_kind { TITLE_VM_ALLOCATE, TITLE_VM_FREE, TITLE_VM_CACHE_ZERO, TITLE_VM_CPU_KINDS };
struct title_vm_cpu_scope
{
    uint64_t cpu, wall;
    BOOL cpu_valid;
};
static struct
{
    uint64_t calls, cpu_calls, cpu_ns, wall_ns, bytes, failures;
} title_vm_cpu_stats[TITLE_VM_CPU_KINDS];
static unsigned int title_vm_cpu_pid;

static struct title_vm_cpu_scope title_vm_cpu_begin(void)
{
    struct title_vm_cpu_scope scope = {0};
    int saved_errno = errno;

    if (title_profile_is_enabled())
    {
        scope.cpu_valid = ps5_title_profile_read_cpu( &scope.cpu, &scope.wall );
        if (!scope.wall) scope.wall = ps5_title_profile_now_ns();
        if (!__atomic_load_n( &title_vm_cpu_pid, __ATOMIC_RELAXED ))
            __atomic_store_n( &title_vm_cpu_pid, GetCurrentProcessId(), __ATOMIC_RELAXED );
    }
    errno = saved_errno;
    return scope;
}

static void title_vm_cpu_end( enum title_vm_cpu_kind kind, struct title_vm_cpu_scope scope,
                              uint64_t bytes, NTSTATUS status )
{
    uint64_t cpu = 0, wall = 0;
    BOOL valid;
    int saved_errno = errno;

    if (!scope.wall) return;
    valid = ps5_title_profile_read_cpu( &cpu, &wall );
    if (!wall) wall = ps5_title_profile_now_ns();
    if (scope.cpu_valid && valid && cpu >= scope.cpu)
    {
        __atomic_add_fetch( &title_vm_cpu_stats[kind].cpu_ns, cpu - scope.cpu, __ATOMIC_RELAXED );
        __atomic_add_fetch( &title_vm_cpu_stats[kind].cpu_calls, 1, __ATOMIC_RELAXED );
    }
    if (wall >= scope.wall)
        __atomic_add_fetch( &title_vm_cpu_stats[kind].wall_ns, wall - scope.wall, __ATOMIC_RELAXED );
    __atomic_add_fetch( &title_vm_cpu_stats[kind].bytes, bytes, __ATOMIC_RELAXED );
    if (status) __atomic_add_fetch( &title_vm_cpu_stats[kind].failures, 1, __ATOMIC_RELAXED );
    __atomic_add_fetch( &title_vm_cpu_stats[kind].calls, 1, __ATOMIC_RELEASE );
    errno = saved_errno;
}

/* Completed-operation CPU counters, not sampled-PC attribution. Cache zero is
 * a subset of allocate, so its CPU must not be added to the allocate total. */
void virtual_title_vm_cpu_report( uint64_t now )
{
    static const char * const names[] = { "allocate", "free", "cache-zero" };
    unsigned int i;
    char line[384];

    for (i = 0; i < TITLE_VM_CPU_KINDS; i++)
    {
        uint64_t calls = __atomic_load_n( &title_vm_cpu_stats[i].calls, __ATOMIC_ACQUIRE );
        if (!calls) continue;
        snprintf( line, sizeof(line), "title vm cpu pid=%04x kind=%s cumulative=1 now_ns=%llu calls=%llu "
                  "cpu_calls=%llu cpu_ns=%llu wall_ns=%llu bytes=%llu failures=%llu\n",
                  __atomic_load_n( &title_vm_cpu_pid, __ATOMIC_RELAXED ), names[i],
                  (unsigned long long)now, (unsigned long long)calls,
                  (unsigned long long)__atomic_load_n( &title_vm_cpu_stats[i].cpu_calls, __ATOMIC_RELAXED ),
                  (unsigned long long)__atomic_load_n( &title_vm_cpu_stats[i].cpu_ns, __ATOMIC_RELAXED ),
                  (unsigned long long)__atomic_load_n( &title_vm_cpu_stats[i].wall_ns, __ATOMIC_RELAXED ),
                  (unsigned long long)__atomic_load_n( &title_vm_cpu_stats[i].bytes, __ATOMIC_RELAXED ),
                  (unsigned long long)__atomic_load_n( &title_vm_cpu_stats[i].failures, __ATOMIC_RELAXED ) );
        sceKernelDebugOutText( 0, line );
    }
}
/* WINE_PS5_VM_CPU_TEST_END */

/* WINE_PS5_DIRECT_CACHE_TEST_BEGIN */
/* Two nominal 32-MiB workspaces can each include one native alignment unit.
 * Retained native bytes, not requested bytes, are charged to this bound. */
#define TITLE_DIRECT_CACHE_LIMIT (((size_t)64 << 20) + 2 * (granularity_mask + 1))
#define TITLE_DIRECT_CACHE_REGION_MAX ((size_t)64 << 20)
#define TITLE_DIRECT_CACHE_REGION_MIN ((size_t)1 << 20)
#define TITLE_DIRECT_CACHE_ENTRIES 4
static struct file_view *title_direct_cache;
static size_t title_direct_cache_bytes;
static unsigned int title_direct_cache_count;
static uint64_t title_direct_cache_pressure_ns;
static BOOL title_direct_cache_under_pressure;
static struct
{
    uint64_t since, hits, puts, misses, evictions, failures, zero_ns, zero_bytes;
} title_direct_cache_stats;

static size_t title_direct_cache_size( const struct file_view *view )
{
    return ROUND_SIZE( 0, view->size, granularity_mask );
}

static void title_direct_cache_report(void)
{
    uint64_t now;
    char line[384];

    if (!title_profile_is_enabled()) return;
    now = ps5_title_profile_now_ns();
    if (!title_direct_cache_stats.since) title_direct_cache_stats.since = now;
    if (now - title_direct_cache_stats.since < UINT64_C(10000000000)) return;
    snprintf( line, sizeof(line), "title direct cache pid=%04x window_ms=%llu hits=%llu puts=%llu misses=%llu "
              "evictions=%llu failures=%llu retained_kib=%llu entries=%u zero_mib=%llu zero_ms=%llu\n",
              __atomic_load_n( &title_vm_cpu_pid, __ATOMIC_RELAXED ),
              (unsigned long long)((now - title_direct_cache_stats.since) / 1000000),
              (unsigned long long)title_direct_cache_stats.hits,
              (unsigned long long)title_direct_cache_stats.puts,
              (unsigned long long)title_direct_cache_stats.misses,
              (unsigned long long)title_direct_cache_stats.evictions,
              (unsigned long long)title_direct_cache_stats.failures,
              (unsigned long long)(title_direct_cache_bytes / 1024), title_direct_cache_count,
              (unsigned long long)(title_direct_cache_stats.zero_bytes >> 20),
              (unsigned long long)(title_direct_cache_stats.zero_ns / 1000000) );
    sceKernelDebugOutText( 0, line );
    memset( &title_direct_cache_stats, 0, sizeof(title_direct_cache_stats) );
    title_direct_cache_stats.since = now;
}

/* Cached views have already left views_tree. Never use ps5_free_pages here:
 * its split-view neighbour checks require a live RB-tree entry. */
static BOOL title_direct_cache_evict_range( void *base, size_t size )
{
    struct file_view **prev = &title_direct_cache, *view;
    UINT_PTR low = (UINT_PTR)base;
    BOOL ret = TRUE;

    while ((view = *prev))
    {
        size_t native_size = title_direct_cache_size( view );
        UINT_PTR cached = (UINT_PTR)view->base;

        if (base && (cached >= low + size || cached + native_size <= low))
        {
            prev = &view->cache_next;
            continue;
        }
        if (!ps5_direct_release_whole( view->direct ))
        {
            title_direct_cache_stats.failures++;
            ret = FALSE;
            prev = &view->cache_next;
            continue;
        }
        *prev = view->cache_next;
        title_direct_cache_bytes -= native_size;
        title_direct_cache_count--;
        title_direct_cache_stats.evictions++;
        free_view( view );
    }
    return ret;
}

/* A failed range protection may have changed a prefix. A detached free
 * address must be inaccessible before we return, even if both close and
 * ownership-preserving reclamation encounter failures. */
static void title_direct_cache_close_failed_take( struct file_view *view )
{
    size_t native_size = title_direct_cache_size( view );

    if (!ps5_direct_protect( view->direct, view->base, native_size, PROT_NONE )) return;
    if (title_direct_cache_evict_range( view->base, native_size )) return;
    /* A partial eviction may leave only a few mapped units. Protect those;
     * physical-release failures after successful unmaps are already safe. */
    if (!ps5_direct_protect( view->direct, view->base, native_size, PROT_NONE )) return;
    ps5_direct_fail_fast();
}

BOOL virtual_title_direct_cache_trim(void)
{
    BOOL retained = !!title_direct_cache;

    title_direct_cache_evict_range( NULL, 0 );
    return retained; /* A physical-allocation failure gets one retry after reclamation. */
}

static BOOL title_direct_cache_available(void)
{
    uint64_t now;

    if (!ps5_direct_cache_enabled())
    {
        title_direct_cache_evict_range( NULL, 0 );
        return FALSE;
    }
    now = ps5_title_profile_now_ns();
    if (!title_direct_cache_pressure_ns ||
        now - title_direct_cache_pressure_ns >= UINT64_C(1000000000))
    {
        title_direct_cache_under_pressure = ps5_direct_cache_pressure();
        title_direct_cache_pressure_ns = now;
    }
    if (title_direct_cache_under_pressure)
    {
        title_direct_cache_evict_range( NULL, 0 );
        return FALSE;
    }
    return TRUE;
}

static BOOL title_direct_cache_put( struct file_view *view )
{
    struct file_view *oldest;
    size_t native_size;
    BYTE vprot;

    if (!(view->protect & VPROT_PS5_DIRECT) || !title_direct_cache_available()) return FALSE;
    native_size = title_direct_cache_size( view );
    if (view->protect != (VPROT_PS5_DIRECT | VPROT_READ | VPROT_WRITE | VPROT_COMMITTED) ||
        force_exec_prot || native_size < TITLE_DIRECT_CACHE_REGION_MIN ||
        native_size > TITLE_DIRECT_CACHE_REGION_MAX ||
        get_vprot_range_size( view->base, view->size, 0xff, &vprot ) != view->size ||
        vprot != (VPROT_READ | VPROT_WRITE | VPROT_COMMITTED) ||
        !ps5_direct_is_whole_backed( view->direct, view->base, native_size ))
        return FALSE;

    while (title_direct_cache_bytes + native_size > TITLE_DIRECT_CACHE_LIMIT ||
           title_direct_cache_count >= TITLE_DIRECT_CACHE_ENTRIES)
    {
        for (oldest = title_direct_cache; oldest && oldest->cache_next; oldest = oldest->cache_next) {}
        if (!oldest || !title_direct_cache_evict_range( oldest->base, title_direct_cache_size( oldest ) ))
            return FALSE;
    }
    /* Freed addresses must fault, even though their backing stays owned.
     * Cache operations never add execute permission. */
    if (ps5_direct_protect( view->direct, view->base, native_size, PROT_NONE ))
    {
        title_direct_cache_stats.failures++;
        return FALSE;
    }
    set_page_vprot( view->base, view->size, 0 );
    if (title_profile_is_enabled()) title_map_unmapped( view );
    unregister_view( view );
    view->map_profile = 0;
    view->cache_next = title_direct_cache;
    title_direct_cache = view;
    title_direct_cache_bytes += native_size;
    title_direct_cache_count++;
    title_direct_cache_stats.puts++;
    title_direct_cache_report();
    return TRUE;
}

/* Limits are an exclusive native address range, after map_view's normal
 * Windows validation. A miss follows the unchanged allocation search. */
static NTSTATUS title_direct_cache_take( struct file_view **view_ret, void *base, size_t size,
                                        unsigned int vprot, UINT_PTR low, UINT_PTR high,
                                        UINT_PTR align_mask, BOOL top_down )
{
    struct file_view **prev, **chosen = NULL, *view;
    struct title_vm_cpu_scope zero_scope;
    size_t native_size, logical_host_size;
    uint64_t started;
    UINT_PTR addr;
    BOOL profile = title_profile_is_enabled();

    /* MEM_TOP_DOWN must consult the complete free-range search, not just the
     * highest retained entry. Its unchanged path evicts an overlap if needed. */
    if (!title_direct_cache_available() || force_exec_prot || top_down ||
        vprot != (VPROT_PS5_DIRECT | VPROT_READ | VPROT_WRITE | VPROT_COMMITTED))
        return STATUS_NOT_FOUND;
    for (prev = &title_direct_cache; (view = *prev); prev = &view->cache_next)
    {
        addr = (UINT_PTR)view->base;
        native_size = title_direct_cache_size( view );
        if (view->size != size || (base && view->base != base) || addr < low ||
            addr > high || native_size > high - addr || (addr & align_mask) ||
            !ps5_direct_is_whole_backed( view->direct, view->base, native_size ))
            continue;
        if (ps5_title_radv_ranges && wine_ps5_title_radv_overlap( addr, native_size, NULL, NULL ))
            continue;
        if (!chosen || addr < (UINT_PTR)(*chosen)->base)
            chosen = prev;
    }
    if (!chosen)
    {
        title_direct_cache_stats.misses++;
        title_direct_cache_report();
        return STATUS_NOT_FOUND;
    }
    view = *chosen;
    native_size = title_direct_cache_size( view );
    if (ps5_direct_protect( view->direct, view->base, native_size, PROT_READ | PROT_WRITE ))
    {
        title_direct_cache_close_failed_take( view );
        title_direct_cache_stats.failures++;
        return STATUS_ACCESS_DENIED;
    }
    started = profile ? ps5_title_profile_now_ns() : 0;
    if (profile) zero_scope = title_vm_cpu_begin();
    memset( view->base, 0, native_size ); /* Fresh private commits always read as zero. */
    if (profile)
    {
        title_vm_cpu_end( TITLE_VM_CACHE_ZERO, zero_scope, native_size, STATUS_SUCCESS );
        title_direct_cache_stats.zero_ns += ps5_title_profile_now_ns() - started;
    }
    logical_host_size = ROUND_SIZE( 0, size, host_page_mask );
    if (logical_host_size < native_size &&
        ps5_direct_protect( view->direct, (char *)view->base + logical_host_size,
                            native_size - logical_host_size, PROT_NONE ))
    {
        title_direct_cache_close_failed_take( view );
        title_direct_cache_stats.failures++;
        return STATUS_ACCESS_DENIED;
    }
    *chosen = view->cache_next;
    title_direct_cache_bytes -= native_size;
    title_direct_cache_count--;
    view->cache_next = NULL;
    view->protect = vprot;
    set_page_vprot( view->base, size, vprot );
    register_view( view );
    kernel_writewatch_register_range( view, view->base, size );
    title_direct_cache_stats.hits++;
    title_direct_cache_stats.zero_bytes += native_size;
    title_direct_cache_report();
    *view_ret = view;
    return STATUS_SUCCESS;
}
/* WINE_PS5_DIRECT_CACHE_TEST_END */
#endif



/***********************************************************************
 *           get_win32_prot
 *
 * Convert page protections to Win32 flags.
 */
static DWORD get_win32_prot( BYTE vprot, unsigned int map_prot )
{
    DWORD ret;

    if ((vprot & (VPROT_COPIED | VPROT_WRITECOPY)) == (VPROT_COPIED | VPROT_WRITECOPY))
        vprot = (vprot & ~VPROT_WRITECOPY) | VPROT_WRITE;
    ret = VIRTUAL_Win32Flags[vprot & 0x0f];
    if (vprot & VPROT_GUARD) ret |= PAGE_GUARD;
    if (map_prot & SEC_NOCACHE) ret |= PAGE_NOCACHE;
    return ret;
}


/***********************************************************************
 *           get_vprot_flags
 *
 * Build page protections from Win32 flags.
 */
static NTSTATUS get_vprot_flags( DWORD protect, unsigned int *vprot, BOOL image )
{
    switch(protect & 0xff)
    {
    case PAGE_READONLY:
        *vprot = VPROT_READ;
        break;
    case PAGE_READWRITE:
        if (image)
            *vprot = VPROT_READ | VPROT_WRITECOPY;
        else
            *vprot = VPROT_READ | VPROT_WRITE;
        break;
    case PAGE_WRITECOPY:
        *vprot = VPROT_READ | VPROT_WRITECOPY;
        break;
    case PAGE_EXECUTE:
        *vprot = VPROT_EXEC;
        break;
    case PAGE_EXECUTE_READ:
        *vprot = VPROT_EXEC | VPROT_READ;
        break;
    case PAGE_EXECUTE_READWRITE:
        if (image)
            *vprot = VPROT_EXEC | VPROT_READ | VPROT_WRITECOPY;
        else
            *vprot = VPROT_EXEC | VPROT_READ | VPROT_WRITE;
        break;
    case PAGE_EXECUTE_WRITECOPY:
        *vprot = VPROT_EXEC | VPROT_READ | VPROT_WRITECOPY;
        break;
    case PAGE_NOACCESS:
        *vprot = 0;
        break;
    default:
        return STATUS_INVALID_PAGE_PROTECTION;
    }
    if (protect & PAGE_GUARD) *vprot |= VPROT_GUARD;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           mprotect_exec
 *
 * Wrapper for mprotect, adds PROT_EXEC if forced by force_exec_prot
 */
static inline int mprotect_exec( void *base, size_t size, int unix_prot )
{
#ifdef __PROSPERO__
    struct file_view *view = ps5_direct_enabled() ? find_view_range( base, size ) : NULL;

    if (view && (view->protect & VPROT_PS5_DIRECT))
    {
        if (force_exec_prot && (unix_prot & PROT_READ) && !(unix_prot & PROT_EXEC))
        {
            if (!ps5_direct_protect( view->direct, base, size, unix_prot | PROT_EXEC )) return 0;
            if (!(unix_prot & PROT_WRITE)) return -1;
        }
        return ps5_direct_protect( view->direct, base, size, unix_prot );
    }
#endif
    if (force_exec_prot && (unix_prot & PROT_READ) && !(unix_prot & PROT_EXEC))
    {
        TRACE( "forcing exec permission on %p-%p\n", base, (char *)base + size - 1 );
        if (!mprotect( base, size, unix_prot | PROT_EXEC )) return 0;
        /* exec + write may legitimately fail, in that case fall back to write only */
        if (!(unix_prot & PROT_WRITE)) return -1;
    }

#ifdef __PROSPERO__
    /* A failed protection change fails VirtualProtect, which Chromium treats
     * as fatal (Battle.net's renderers stopped in PartitionAlloc's
     * permission toggles, 2026-10-04): say why, and what the kernel maps
     * there. A plain payload may make anonymous and shared memory, above
     * and below 4 GiB, executable (ps5/probes/wxflip); Wine's processes were
     * refused it for some ranges, read+execute included. */
    {
        int ret = mprotect( base, size, unix_prot ), error = errno, tries;
        /* the refusal could not be made in a test program (wxflip, m9rwx,
         * execcap, mapcount): retry it, briefly, and say whether that helped */
        for (tries = 1; ret && error == EPERM && tries <= 5; tries++)
        {
            usleep( 2000 * tries );
            if (!(ret = mprotect( base, size, unix_prot )))
                ERR( "mprotect %p-%p prot %#x refused, then allowed on retry %d\n", base, (char *)base + size,
                     unix_prot, tries );
            else error = errno;
        }
        if (ret)
        {
            int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, getpid() };
            size_t len = 0;
            char *buf;

            ERR( "mprotect %p-%p prot %#x failed: %s\n", base, (char *)base + size, unix_prot, strerror( error ) );
            if (!sysctl( mib, 4, NULL, &len, NULL, 0 ) && (buf = malloc( len += len / 4 )))
            {
                if (!sysctl( mib, 4, buf, &len, NULL, 0 ))
                {
                    for (char *p = buf; p < buf + len;)
                    {
                        const struct kinfo_vmentry *e = (const struct kinfo_vmentry *)p;
                        if (e->kve_structsize <= 0) break;
                        p += e->kve_structsize;
                        if (e->kve_end <= (UINT_PTR)base || e->kve_start >= (UINT_PTR)base + size) continue;
                        ERR( "  map %llx-%llx type %d prot %#x flags %#x ref %d shadow %d offset %#llx path %s\n",
                             (unsigned long long)e->kve_start, (unsigned long long)e->kve_end, e->kve_type,
                             e->kve_protection, e->kve_flags, e->kve_ref_count, e->kve_shadow_count,
                             (unsigned long long)e->kve_offset, e->kve_path );
                    }
                }
                free( buf );
            }
            errno = error;
        }
        return ret;
    }
#else
    return mprotect( base, size, unix_prot );
#endif
}

#ifdef __PROSPERO__
/* Adapted from WoWPS5's wine-ps5.patch: protected executables decrypt code on
 * first access to each Windows page. The host-page union must not let sealed
 * neighbours execute encrypted bytes. Keep image/executable-section host pages
 * closed until every committed page is accessible, unless one is writable.
 * virtual_handle_fault asks the loader to open the sealed neighbours in turn. */
static BOOL is_sealed_view( const struct file_view *view )
{
    return (view->protect & SEC_IMAGE) || (!is_view_valloc( view ) && (view->protect & VPROT_EXEC));
}

static BOOL is_host_page_closed( const char *page )
{
    struct file_view *view;
    BOOL closed = FALSE;
    size_t i;

    if (!__wine_ps5_is_title() || host_page_size == page_size) return FALSE;
    for (i = 0; i < host_page_size; i += page_size)
    {
        BYTE vprot = get_page_vprot( page + i );

        if (!(vprot & VPROT_COMMITTED)) continue;
        if (vprot & (VPROT_WRITE | VPROT_WRITECOPY)) return FALSE;
        if (!(vprot & (VPROT_READ | VPROT_EXEC | VPROT_GUARD))) closed = TRUE;
    }
    return closed && (view = find_view( page, 0 )) && is_sealed_view( view );
}

/* A handler may need an allowed page in the very host page it is decrypting.
 * Reclose these temporary openings on the next fault in a different host page. */
static char *opened_host_pages[16];
static unsigned int opened_host_pages_next;

static void close_opened_host_pages( const char *except )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(opened_host_pages); i++)
    {
        char *other = opened_host_pages[i];

        if (!other || other == except) continue;
        opened_host_pages[i] = NULL;
        if (is_host_page_closed( other )) mprotect_exec( other, host_page_size, PROT_NONE );
    }
}

BOOL virtual_is_sealed_page( const void *addr )
{
    struct file_view *view;
    BOOL sealed = FALSE;
    BYTE vprot;

    if (!__wine_ps5_is_title() || host_page_size == page_size) return FALSE;
    mutex_lock( &virtual_mutex );
    vprot = get_page_vprot( addr );
    if ((vprot & VPROT_COMMITTED) &&
        !(vprot & (VPROT_READ | VPROT_EXEC | VPROT_GUARD | VPROT_WRITE | VPROT_WRITECOPY)) &&
        (view = find_view( addr, 0 )))
        sealed = is_sealed_view( view );
    mutex_unlock( &virtual_mutex );
    return sealed;
}

/* From WoWPS5: Windows code inspects native dispatcher entry points, but the
 * title's native text is execute-only. Give it readable, register-neutral code
 * that does not begin with a jump. Each cached entry has its own direct-memory
 * host page, written once and then RX, so publishing a new entry never removes
 * execute permission from an entry another thread may already be using. */
void *virtual_readable_entry( void *func )
{
#ifdef __x86_64__
    static const BYTE code[] =
    {
        0x0f, 0x1f, 0x44, 0x00, 0x00,        /* nopl 0(%rax,%rax,1) */
        0x0f, 0x1f, 0x40, 0x00,              /* nopl 0(%rax) */
        0x66, 0x90,                          /* xchg %ax,%ax */
        0xff, 0x25, 0x00, 0x00, 0x00, 0x00,  /* jmp *0(%rip) */
    };
    struct readable_entry
    {
        struct readable_entry *next;
        void *func;
        BYTE code[32];
    };
    static struct readable_entry *entries;
    static pthread_mutex_t entry_mutex = PTHREAD_MUTEX_INITIALIZER;
    struct readable_entry *entry;
    void *base = NULL;
    SIZE_T size = max( page_size, host_page_size );
    ULONG old_protect;
    NTSTATUS status;
    sigset_t sigset;
    char line[128];

    if (!__wine_ps5_is_title()) return func;
    server_enter_uninterrupted_section( &entry_mutex, &sigset );
    for (entry = entries; entry; entry = entry->next)
        if (entry->func == func) break;
    if (!entry)
    {
        status = NtAllocateVirtualMemory( NtCurrentProcess(), &base, 0, &size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        if (status) goto failed;
        entry = base;
        entry->next = entries;
        entry->func = func;
        memcpy( entry->code, code, sizeof(code) );
        memcpy( entry->code + sizeof(code), &func, sizeof(func) );
        status = NtProtectVirtualMemory( NtCurrentProcess(), &base, &size, PAGE_EXECUTE_READ, &old_protect );
        if (status) goto failed;
        entries = entry;
        snprintf( line, sizeof(line), "wine: title readable entry %p -> %p\n", entry->code, func );
        sceKernelDebugOutText( 0, line );
    }
    server_leave_uninterrupted_section( &entry_mutex, &sigset );
    return entry->code;

failed:
    server_leave_uninterrupted_section( &entry_mutex, &sigset );
    ERR( "cannot create readable title entry for %p, status %08x\n", func, status );
    abort_process(1);
#else
    return func;
#endif
}
#endif

static int get_host_page_unix_prot( const char *addr, BYTE set, BYTE clear )
{
#ifdef __PROSPERO__
    if (!(set & (VPROT_WRITE | VPROT_WRITECOPY)) && is_host_page_closed( addr )) return PROT_NONE;
#endif
    return get_unix_prot( (get_host_page_vprot( addr ) & ~clear) | set );
}


/***********************************************************************
 *           mprotect_range
 *
 * Call mprotect on a page range, applying the protections from the per-page byte.
 */
#ifdef __PROSPERO__
static int title_mprotect_range( void *base, size_t size, BYTE set, BYTE clear )
#else
static int mprotect_range( void *base, size_t size, BYTE set, BYTE clear )
#endif
{
    size_t i, count;
    char *addr = ROUND_ADDR( base, host_page_mask );
    int prot, next;

    size = ROUND_SIZE( base, size, host_page_mask );

    prot = get_host_page_unix_prot( addr, set, clear );
    for (count = i = 1; i < size / host_page_size; i++, count++)
    {
        next = get_host_page_unix_prot( addr + count * host_page_size, set, clear );
        if (next == prot) continue;
        if (mprotect_exec( addr, count * host_page_size, prot )) return -1;
        addr += count * host_page_size;
        prot = next;
        count = 0;
    }
    return mprotect_exec( addr, count * host_page_size, prot );
}

#ifdef __PROSPERO__
static int mprotect_range( void *base, size_t size, BYTE set, BYTE clear )
{
    struct ps5_title_map_sample *sample;
    uint64_t started;
    int ret;

    if (!title_profile_is_enabled()) return title_mprotect_range( base, size, set, clear );
    sample = title_map_sample();
    started = title_map_prep_begin( sample );
    ret = title_mprotect_range( base, size, set, clear );
    title_map_prep_end( sample, started );
    return ret;
}
#endif

#ifdef __PROSPERO__
/* Apply a prospective vprot without changing pages_vprot until the whole
 * transaction succeeds. This is the direct-view commit/protection choke point,
 * shared by initial MEM_COMMIT, later commits and NtProtectVirtualMemory. */
static NTSTATUS title_ps5_set_vprot( struct file_view *view, void *base, size_t size, BYTE vprot )
{
    char *addr, *start = ROUND_ADDR( base, host_page_mask );
    char *end = start + ROUND_SIZE( base, size, host_page_mask );
    char *run = start, *page;
    int prot = -1, next;
    BYTE merged, old, prospective = vprot;
    BOOL seal = __wine_ps5_is_title() && host_page_size != page_size && is_sealed_view( view );
    BOOL closed;

    if (vprot & VPROT_COMMITTED)
        if (!ps5_direct_ensure_backed( view->direct, base, size )) return STATUS_NO_MEMORY;
    if (!(view->protect & VPROT_WRITEWATCH) || use_kernel_writewatch)
    {
        if (enable_write_exceptions && is_vprot_exec_write( prospective )) prospective |= VPROT_WRITEWATCH;
        else if (use_kernel_writewatch && (view->protect & VPROT_WRITEWATCH))
            prospective &= ~VPROT_WRITEWATCH;
    }
    for (addr = start; addr < end; addr += host_page_size)
    {
        merged = 0;
        closed = FALSE;
        for (page = addr; page < addr + host_page_size; page += page_size)
        {
            old = get_page_vprot( page );
            if (page >= (char *)base && page < (char *)base + size)
            {
                if (!use_kernel_writewatch && (view->protect & VPROT_WRITEWATCH))
                    old = (old & VPROT_WRITEWATCH) | (prospective & ~VPROT_WRITEWATCH);
                else old = prospective;
            }
            merged |= old;
            if ((old & VPROT_COMMITTED) && !(old & (VPROT_READ | VPROT_EXEC | VPROT_GUARD)))
                closed = TRUE;
        }
        next = seal && closed && !(merged & (VPROT_WRITE | VPROT_WRITECOPY)) ?
               PROT_NONE : get_unix_prot( merged );
        if (next == prot) continue;
        if (addr != run && mprotect_exec( run, addr - run, prot )) goto failed;
        run = addr;
        prot = next;
    }
    if (mprotect_exec( run, end - run, prot )) goto failed;
    if (!use_kernel_writewatch && (view->protect & VPROT_WRITEWATCH))
        set_page_vprot_bits( base, size, prospective & ~VPROT_WRITEWATCH,
                             ~prospective & ~VPROT_WRITEWATCH );
    else set_page_vprot( base, size, prospective );
    ps5_direct_finish_commit( view->direct, base, size, FALSE );
    return STATUS_SUCCESS;

failed:
    /* Old vprots still describe every old backed page. Undo any host-protection
     * changes before dropping only the units created by this transaction. */
    mprotect_range( base, size, 0, 0 );
    ps5_direct_finish_commit( view->direct, base, size, TRUE );
    return STATUS_NO_MEMORY;
}

static NTSTATUS ps5_set_vprot( struct file_view *view, void *base, size_t size, BYTE vprot )
{
    struct ps5_title_map_sample *sample;
    uint64_t started;
    NTSTATUS status;

    if (!title_profile_is_enabled()) return title_ps5_set_vprot( view, base, size, vprot );
    sample = title_map_sample();
    started = title_map_prep_begin( sample );
    status = title_ps5_set_vprot( view, base, size, vprot );
    title_map_prep_end( sample, started );
    return status;
}
#endif


/***********************************************************************
 *           set_vprot
 *
 * Change the protection of a range of pages.
 */
static BOOL set_vprot( struct file_view *view, void *base, size_t size, BYTE vprot )
{
#ifdef __PROSPERO__
    if (view->protect & VPROT_PS5_DIRECT) return !ps5_set_vprot( view, base, size, vprot );
#endif
    if (!use_kernel_writewatch && view->protect & VPROT_WRITEWATCH)
    {
        /* each page may need different protections depending on write watch flag */
        set_page_vprot_bits( base, size, vprot & ~VPROT_WRITEWATCH, ~vprot & ~VPROT_WRITEWATCH );
    }
    else
    {
        if (enable_write_exceptions && is_vprot_exec_write( vprot )) vprot |= VPROT_WRITEWATCH;
        else if (use_kernel_writewatch && view->protect & VPROT_WRITEWATCH) vprot &= ~VPROT_WRITEWATCH;
        set_page_vprot( base, size, vprot );
    }
    return !mprotect_range( base, size, 0, 0 );
}


/***********************************************************************
 *           set_protection
 *
 * Set page protections on a range of pages
 */
static NTSTATUS set_protection( struct file_view *view, void *base, SIZE_T size, ULONG protect )
{
    unsigned int vprot;
    NTSTATUS status;

    if ((status = get_vprot_flags( protect, &vprot, view->protect & SEC_IMAGE ))) return status;
    if (is_view_valloc( view ))
    {
        if (vprot & VPROT_WRITECOPY) return STATUS_INVALID_PAGE_PROTECTION;
    }
    else
    {
        BYTE access = vprot & (VPROT_READ | VPROT_WRITE | VPROT_EXEC);
        if ((view->protect & access) != access) return STATUS_INVALID_PAGE_PROTECTION;
    }

#ifdef __PROSPERO__
    if (view->protect & VPROT_PS5_DIRECT)
        return ps5_set_vprot( view, base, size, vprot | VPROT_COMMITTED );
#endif
    if (!set_vprot( view, base, size, vprot | VPROT_COMMITTED )) return STATUS_ACCESS_DENIED;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           commit_arm64ec_map
 *
 * Make sure that the pages corresponding to the address range of the view
 * are committed in the ARM64EC code map.
 */
static void commit_arm64ec_map( struct file_view *view )
{
    size_t start = ((size_t)view->base >> page_shift) / 8;
    size_t end = (((size_t)view->base + view->size) >> page_shift) / 8;
    size_t size = ROUND_SIZE( start, end + 1 - start, page_mask );
    void *base = ROUND_ADDR( (char *)arm64ec_view->base + start, page_mask );

    view->protect |= VPROT_ARM64EC;
    set_vprot( arm64ec_view, base, size, VPROT_READ | VPROT_WRITE | VPROT_COMMITTED );
}


/***********************************************************************
 *           update_write_watches
 */
static void update_write_watches( void *base, size_t size, size_t accessed_size )
{
    TRACE( "updating watch %p-%p-%p\n", base, (char *)base + accessed_size, (char *)base + size );
    /* clear write watch flag on accessed pages */
    set_page_vprot_bits( base, accessed_size, 0, VPROT_WRITEWATCH );
    /* restore page protections on the entire range */
    mprotect_range( base, size, 0, 0 );
}


/***********************************************************************
 *           reset_write_watches
 *
 * Reset write watches in a memory range.
 */
static void reset_write_watches( void *base, SIZE_T size )
{
    if (use_kernel_writewatch)
    {
        kernel_writewatch_reset( base, size );
        if (!enable_write_exceptions) return;
        if (!set_page_vprot_exec_write_protect( base, size )) return;
    }
    else set_page_vprot_bits( base, size, VPROT_WRITEWATCH, 0 );

    mprotect_range( base, size, 0, 0 );
}


/***********************************************************************
 *           unmap_extra_space
 *
 * Release the extra memory while keeping the range starting on the alignment boundary.
 */
static inline void *unmap_extra_space( void *ptr, size_t total_size, size_t wanted_size, size_t align_mask )
{
    if ((ULONG_PTR)ptr & align_mask)
    {
        size_t extra = align_mask + 1 - ((ULONG_PTR)ptr & align_mask);
        munmap( ptr, extra );
        ptr = (char *)ptr + extra;
        total_size -= extra;
    }
    if (total_size > wanted_size)
        munmap( (char *)ptr + wanted_size, total_size - wanted_size );
    return ptr;
}

static void *try_map_free_area_range( struct alloc_area *area, char *start, char *end )
{
    char *alloc_start;

    if (start >= end || (size_t)(end - start) < area->size) return NULL;

    if (area->top_down)
    {
        alloc_start = ROUND_ADDR( end - area->size, area->align_mask );
        return try_map_free_area( area, start, alloc_start + area->size, alloc_start );
    }

    alloc_start = ROUND_ADDR( start + area->align_mask, area->align_mask );
    return try_map_free_area( area, start, end, alloc_start );
}

static void *alloc_free_area_in_range( struct alloc_area *area, char *base, char *end )
{
    UINT_PTR align_mask = area->align_mask;
    char *intersect_start, *intersect_end, *result, *alloc_start;
    struct reserved_area *res_area;

    TRACE("range %p-%p.\n", base, end);

    if (base >= end)
        return NULL;
#ifdef __PROSPERO__
    if (ps5_title_radv_ranges)
    {
        uint64_t low, high;
        if (wine_ps5_title_radv_overlap( (UINT_PTR)base, end - base, &low, &high ))
        {
            char *before = min( end, (char *)(UINT_PTR)low );
            char *after = max( base, (char *)(UINT_PTR)high );
            if (area->top_down)
            {
                if ((result = alloc_free_area_in_range( area, after, end ))) return result;
                return alloc_free_area_in_range( area, base, before );
            }
            if ((result = alloc_free_area_in_range( area, base, before ))) return result;
            return alloc_free_area_in_range( area, after, end );
        }
    }
#endif

    if (area->top_down)
    {
        if ((ULONG_PTR)end < area->size) return NULL;
        alloc_start = ROUND_ADDR( end - area->size, align_mask );
        if (alloc_start >= end || alloc_start < base) return NULL;

        LIST_FOR_EACH_ENTRY_REV( res_area, &reserved_areas, struct reserved_area, entry )
        {
            char *res_start = res_area->base;
            char *res_end = res_start + res_area->size;

            if (res_start >= end) continue;
            if (res_end <= base) break;

            intersect_start = max( res_start, base );
            intersect_end = min( res_end, end );
            assert( intersect_start <= intersect_end );
            if ((result = try_map_free_area_range( area, intersect_end, end))) return result;

            if (intersect_end - intersect_start >= area->size)
            {
                alloc_start = ROUND_ADDR( intersect_end - area->size, align_mask );
                if (alloc_start >= intersect_start)
                {
#ifdef __PROSPERO__
                    /* not mapped there (ps5_reserve_free_ranges): take it only if still free */
                    if ((result = map_alloc_area( area, alloc_start )) != MAP_FAILED)
                        return result;
#else
                    if ((result = anon_mmap_fixed( alloc_start, area->size, area->unix_prot, 0 )) != alloc_start)
                        ERR("Could not map in reserved area, alloc_start %p, size %p.\n",
                                alloc_start, (void *)area->size);
                    return result;
#endif
                }
            }

            end = intersect_start;
            if (end - base < area->size) return NULL;
        }
        return try_map_free_area_range( area, base, end );
    }

    if (base + align_mask < base) return NULL;
    alloc_start = ROUND_ADDR( base + align_mask, align_mask );
    if (alloc_start >= end || end - alloc_start < area->size)
        return NULL;

    LIST_FOR_EACH_ENTRY( res_area, &reserved_areas, struct reserved_area, entry )
    {
        char *res_start = res_area->base;
        char *res_end = res_start + res_area->size;

        if (res_end <= base) continue;
        if (res_start >= end) break;

        intersect_start = max( res_start, base );
        intersect_end = min( res_end, end );
        assert( intersect_start <= intersect_end );
        if ((result = try_map_free_area_range( area, base, intersect_start ))) return result;

        if (intersect_end - intersect_start >= area->size)
        {
            alloc_start = ROUND_ADDR( intersect_start + align_mask, align_mask );
            if (alloc_start + area->size <= intersect_end)
            {
#ifdef __PROSPERO__
                /* not mapped there (ps5_reserve_free_ranges): take it only if still free */
                if ((result = map_alloc_area( area, alloc_start )) != MAP_FAILED)
                    return result;
#else
                if ((result = anon_mmap_fixed( alloc_start, area->size, area->unix_prot, 0 )) != alloc_start)
                    ERR("Could not map in reserved area, alloc_start %p, size %p.\n", alloc_start, (void *)area->size);
                return result;
#endif
            }
        }
        base = intersect_end;
        if (end - base < area->size) return NULL;
    }
    return try_map_free_area_range( area, base, end );
}

#ifdef __PROSPERO__
/* WINE_PS5_CHUNKED_RANGE_TEST_BEGIN */
static char *ps5_direct_reserve_floor( char *start, size_t size, BOOL direct )
{
    /* Native title mappings occupy the low Wine-free window and ignore
     * occupied hints. Console reserves established adjacent chunks at 64 GiB.
     * Only constrain unhinted large allocations: fixed-address requests must
     * retain their exact base, including free/re-reserve alignment trimming. */
    if (direct && ps5_direct_chunked_reserve_enabled( size ))
        return max( start, (char *)(UINT_PTR)0x1000000000 );
    return start;
}
/* WINE_PS5_CHUNKED_RANGE_TEST_END */
#endif

static void *alloc_free_area( char *limit_low, char *limit_high, size_t size, BOOL top_down,
                             int unix_prot, UINT_PTR align_mask, unsigned int vprot )
{
    struct range_entry *range, *ranges_start, *ranges_end;
    char *reserve_start, *reserve_end;
    struct alloc_area area;
    char *result = NULL;
    char *base, *end;
    int ranges_inc;
    UINT status;

    TRACE("limit %p-%p, size %p, top_down %#x.\n", limit_low, limit_high, (void *)size, top_down);

    if (top_down)
    {
        ranges_start = free_ranges_end - 1;
        ranges_end = free_ranges - 1;
        ranges_inc = -1;
    }
    else
    {
        ranges_start = free_ranges;
        ranges_end = free_ranges_end;
        ranges_inc = 1;
    }

    memset( &area, 0, sizeof(area) );
    area.step = top_down ? -(align_mask + 1) : (align_mask + 1);
    area.size = size;
    area.top_down = top_down;
    area.unix_prot = unix_prot;
    area.align_mask = align_mask;
#ifdef __PROSPERO__
    area.direct = !!(vprot & VPROT_PS5_DIRECT);
    limit_low = ps5_direct_reserve_floor( limit_low, size, area.direct );
#endif

    reserve_start = preload_reserve_start;
    reserve_end = preload_reserve_end;

    for (range = ranges_start; range != ranges_end; range += ranges_inc)
    {
        base = range->base;
        end = range->end;

        TRACE("range %p-%p.\n", base, end);

        if (base < limit_low) base = (void *)ROUND_SIZE( 0, limit_low, host_page_mask );
        if (end > limit_high) end = ROUND_ADDR( limit_high, host_page_mask );
        if (base > end || end - base < size) continue;

        if (reserve_end >= base)
        {
            if (reserve_end >= end)
            {
                if (reserve_start <= base)
                    continue;  /* no space in that area */

                if (reserve_start < end)
                    end = reserve_start;
            }
            else if (reserve_start <= base)
            {
                base = reserve_end;
            }
            else
            {
                /* range is split in two by the preloader reservation, try first part. */
                if ((result = alloc_free_area_in_range( &area, base, reserve_start )))
                    break;
                /* then fall through to try second part. */
                base = reserve_end;
            }
        }

        if ((result = alloc_free_area_in_range( &area, base, end )))
            break;
    }

    if (area.native_mapped)
    {
        char *native_mapped_start, *native_mapped_end;

        TRACE("Excluding %p - %p from free list.\n",
                area.native_mapped, (char *)area.native_mapped + area.native_mapped_size );

        native_mapped_start = ROUND_ADDR(area.native_mapped, granularity_mask);
        native_mapped_end = ROUND_ADDR(area.native_mapped + area.native_mapped_size + granularity_mask,
                granularity_mask);

        if (result >= native_mapped_end || result + size < native_mapped_start)
        /* In case of top down allocation try_map_free_area() result area can overlap the
         * area previously marked as native if the latter was unmapped behind our back. */
        {
            struct file_view *prev, *next;

            prev = find_view_range( native_mapped_start - 1, native_mapped_end - native_mapped_start + 2 );
            if (prev && (char *)prev->base >= native_mapped_end)
            {
                next = prev;
                prev = WINE_RB_ENTRY_VALUE( rb_prev( &next->entry ), struct file_view, entry );
            }
            else if (prev) next = WINE_RB_ENTRY_VALUE( rb_next( &prev->entry ), struct file_view, entry );
            else           next = NULL;

            if (prev && prev->protect & VPROT_NATIVE && (char *)prev->base + prev->size >= native_mapped_start)
            {
                assert( (char *)prev->base + prev->size == native_mapped_start );
                native_mapped_start = prev->base;
                delete_view( prev );
            }
            if (next && next->protect & VPROT_NATIVE && native_mapped_end >= (char *)next->base)
            {
                assert( native_mapped_end == (char *)next->base );
                native_mapped_end = (char *)next->base + next->size;
                delete_view( next );
            }
            if ((status = create_view( &next, native_mapped_start, native_mapped_end - native_mapped_start,
                    VPROT_SYSTEM | VPROT_NATIVE )))
                ERR("Could not create view for natively mapped area, status %#x.\n", status);
        }
    }
    return result;
}

/***********************************************************************
 *           map_fixed_area
 *
 * Map a memory area at a fixed address.
 * virtual_mutex must be held by caller.
 */
static NTSTATUS map_fixed_area( void *base, size_t size, int unix_prot, unsigned int vprot )
{
    struct reserved_area *area;
    NTSTATUS status;
    char *start = base, *end = (char *)base + ROUND_SIZE( 0, size, host_page_mask );

    if ((UINT_PTR)base & host_page_mask) return STATUS_CONFLICTING_ADDRESSES;
    if (find_view_range( base, size )) return STATUS_CONFLICTING_ADDRESSES;

#ifdef __PROSPERO__
    if (ps5_title_radv_ranges &&
        wine_ps5_title_radv_overlap( (UINT_PTR)start, end - start, NULL, NULL ))
        return STATUS_CONFLICTING_ADDRESSES;
    /* Reserved areas are not mapped there (ps5_reserve_free_ranges): map
     * only where the range is still free, never over what the system put
     * there since. */
    if (!title_direct_cache_evict_range( base, ROUND_SIZE( 0, size, granularity_mask ) ))
        return STATUS_NO_MEMORY;
    if (vprot & VPROT_PS5_DIRECT)
    {
        if ((UINT_PTR)base & granularity_mask) return STATUS_CONFLICTING_ADDRESSES;
        if (ps5_direct_reserve( base, ROUND_SIZE( 0, size, granularity_mask ) ) == MAP_FAILED) goto failed;
        ps5_title_note_reservation( base, ROUND_SIZE( 0, size, granularity_mask ) );
        return STATUS_SUCCESS;
    }
    if (anon_mmap_tryfixed( start, end - start, unix_prot, 0 ) == MAP_FAILED) goto failed;
    ps5_title_note_reservation( start, end - start );
    return STATUS_SUCCESS;
#endif
    LIST_FOR_EACH_ENTRY( area, &reserved_areas, struct reserved_area, entry )
    {
        char *area_start = area->base;
        char *area_end = area_start + area->size;

        if (area_start >= end) break;
        if (area_end <= start) continue;
        if (area_start > start)
        {
            if (anon_mmap_tryfixed( start, area_start - start, unix_prot, 0 ) == MAP_FAILED) goto failed;
            start = area_start;
        }
        if (area_end >= end)
        {
            if (anon_mmap_fixed( start, end - start, unix_prot, 0 ) == MAP_FAILED) goto failed;
            return STATUS_SUCCESS;
        }
        if (anon_mmap_fixed( start, area_end - start, unix_prot, 0 ) == MAP_FAILED) goto failed;
        start = area_end;
    }

    if (anon_mmap_tryfixed( start, end - start, unix_prot, 0 ) == MAP_FAILED) goto failed;
    return STATUS_SUCCESS;

failed:
    if (errno == ENOMEM)
    {
        ERR( "out of memory for %p-%p\n", base, (char *)base + size );
        status = STATUS_NO_MEMORY;
    }
    else if (errno == EEXIST) status = STATUS_CONFLICTING_ADDRESSES;
    else
    {
        ERR( "mmap error %s for %p-%p, unix_prot %#x\n",
             strerror(errno), base, (char *)base + size, unix_prot );
        status = STATUS_INVALID_PARAMETER;
    }
    unmap_area( base, start - (char *)base );
    return status;
}

static void clear_native_views(void)
{
    struct file_view *view, *next_view;

    WINE_RB_FOR_EACH_ENTRY_DESTRUCTOR( view, next_view, &views_tree, struct file_view, entry )
    {
        if (view->protect & VPROT_NATIVE)
            delete_view( view );
    }
}

static void fixup_effective_user_space_limit( const void **effective_user_space_limit )
{
#ifdef _WIN64
    static int cached = -1;

    if (cached == -1)
    {
        const char *e = getenv( "PROTON_LIMIT_ADDRESS_SPACE" );
        const char *sgi = getenv( "SteamGameId" );
        if (e) cached = e[0] != '0';
        else cached = sgi &&
            (
                !strcmp( sgi, "3092660" )
                || !strcmp( sgi, "3681810" )
            );
        if (cached) FIXME( "fixing up effective user space limit.\n" );
    }
    if (cached)
        *effective_user_space_limit = min( *effective_user_space_limit, (void *)0x700000000000 );
#endif
}

/***********************************************************************
 *           map_view
 *
 * Create a view and mmap the corresponding memory area.
 * virtual_mutex must be held by caller.
 */
#ifdef __PROSPERO__
static NTSTATUS title_map_view( struct file_view **view_ret, void *base, size_t size,
                                unsigned int alloc_type, unsigned int vprot,
                                ULONG_PTR limit_low, ULONG_PTR limit_high, size_t align_mask )
#else
static NTSTATUS map_view( struct file_view **view_ret, void *base, size_t size,
                          unsigned int alloc_type, unsigned int vprot,
                          ULONG_PTR limit_low, ULONG_PTR limit_high, size_t align_mask )
#endif
{
    int top_down = alloc_type & MEM_TOP_DOWN;
    void *ptr;
    int unix_prot = get_unix_prot( vprot );
    NTSTATUS status;
    size_t host_size = ROUND_SIZE( 0, size, host_page_mask );
    const void *effective_user_space_limit = !is_win64 && wine_allocs_2g_limit ?
        (void *)0x7fff0000 : min( user_space_limit, host_addr_space_limit);

    fixup_effective_user_space_limit( &effective_user_space_limit );

    if (!align_mask) align_mask = granularity_mask;
    assert( align_mask >= host_page_mask );
#ifdef __PROSPERO__
    if (vprot & VPROT_PS5_DIRECT)
    {
        host_size = ROUND_SIZE( 0, size, granularity_mask );
        align_mask = max( align_mask, granularity_mask );
    }
#endif

    if (alloc_type & MEM_REPLACE_PLACEHOLDER)
    {
        struct file_view *view;

        if (!(view = find_view( base, 0 ))) return STATUS_INVALID_PARAMETER;
        if (view->base != base || view->size != size) return STATUS_CONFLICTING_ADDRESSES;
        if (!(view->protect & VPROT_FREE_PLACEHOLDER)) return STATUS_INVALID_PARAMETER;

        TRACE( "found view %p, size %p, protect %#x.\n", view->base, (void *)view->size, view->protect );

        view->protect = vprot | VPROT_PLACEHOLDER;
        set_vprot( view, base, size, vprot );
        if (vprot & VPROT_WRITEWATCH)
        {
            kernel_writewatch_register_range( view, base, size );
            reset_write_watches( base, size );
        }
        *view_ret = view;
        return STATUS_SUCCESS;
    }

    if (limit_high && limit_low >= limit_high) return STATUS_INVALID_PARAMETER;

    if (use_kernel_writewatch && vprot & VPROT_WRITEWATCH)
        unix_prot = get_unix_prot( vprot & ~VPROT_WRITEWATCH );

    unix_prot &= ~PROT_EXEC;
#ifdef __PROSPERO__
    if (vprot & VPROT_PS5_DIRECT) unix_prot = PROT_NONE;
#endif

    if (base)
    {
        if (is_beyond_limit( base, size, address_space_limit )) return STATUS_WORKING_SET_LIMIT_RANGE;
        if (limit_low && base < (void *)limit_low) return STATUS_CONFLICTING_ADDRESSES;
        if (limit_high && is_beyond_limit( base, size, (void *)limit_high )) return STATUS_CONFLICTING_ADDRESSES;
        if (is_beyond_limit( base, size, host_addr_space_limit )) return STATUS_CONFLICTING_ADDRESSES;
#ifdef __PROSPERO__
        if ((vprot & VPROT_PS5_DIRECT) && is_beyond_limit( base, host_size, host_addr_space_limit ))
            return STATUS_CONFLICTING_ADDRESSES;
#endif
#ifdef __PROSPERO__
        status = title_direct_cache_take( view_ret, base, size, vprot, (UINT_PTR)base,
                                          (UINT_PTR)base + host_size, align_mask, top_down );
        if (status != STATUS_NOT_FOUND) return status;
#endif
        if ((status = map_fixed_area( base, size, unix_prot, vprot ))) return status;
        if (is_beyond_limit( base, size, working_set_limit )) working_set_limit = address_space_limit;
        ptr = base;
    }
    else
    {
        void *start = address_space_start;
        void *end = min( effective_user_space_limit, host_addr_space_limit );

        if (limit_low && (void *)limit_low > start) start = (void *)limit_low;
        if (limit_high && (void *)limit_high < end) end = (char *)limit_high + 1;
#ifdef __PROSPERO__
        status = title_direct_cache_take( view_ret, NULL, size, vprot, (UINT_PTR)start,
                                          (UINT_PTR)end, align_mask, top_down );
        if (status != STATUS_NOT_FOUND) return status;
#endif

        if (!(ptr = alloc_free_area( start, end, host_size, top_down, unix_prot, align_mask, vprot )))
        {
            WARN("Allocation failed, clearing native views.\n");

            clear_native_views();
            if (!is_win64) increase_try_map_step = FALSE;
#ifdef __PROSPERO__
            ptr = alloc_free_area( start, end, host_size, top_down, unix_prot, align_mask, vprot );
#else
            ptr = alloc_free_area( (void *)limit_low, (void *)limit_high, size, top_down, unix_prot, align_mask, vprot );
#endif
            if (!is_win64) increase_try_map_step = TRUE;
            if (!ptr) return STATUS_NO_MEMORY;
        }
    }

    status = create_view( view_ret, ptr, size, vprot );
    if (status != STATUS_SUCCESS) unmap_area( ptr, host_size );
#ifdef __PROSPERO__
    else if ((vprot & (VPROT_PS5_DIRECT | VPROT_COMMITTED)) == (VPROT_PS5_DIRECT | VPROT_COMMITTED))
    {
        status = ps5_set_vprot( *view_ret, ptr, size, vprot );
        if (status) delete_view( *view_ret );
    }
#endif
    return status;
}

#ifdef __PROSPERO__
static NTSTATUS map_view( struct file_view **view_ret, void *base, size_t size,
                          unsigned int alloc_type, unsigned int vprot,
                          ULONG_PTR limit_low, ULONG_PTR limit_high, size_t align_mask )
{
    struct ps5_title_map_sample *sample;
    uint64_t started;
    NTSTATUS status;

    if (!title_profile_is_enabled())
        return title_map_view( view_ret, base, size, alloc_type, vprot, limit_low, limit_high, align_mask );
    sample = title_map_sample();
    started = title_map_prep_begin( sample );
    status = title_map_view( view_ret, base, size, alloc_type, vprot, limit_low, limit_high, align_mask );
    title_map_prep_end( sample, started );
    return status;
}
#endif


#ifdef __PROSPERO__
/* The destination is already committed, zeroed and writable. Read straight
 * into it; an EOF before the promised span is a population failure, not a
 * successful mapping containing recycled physical memory. */
static NTSTATUS ps5_read_file_span( struct file_view *view, int fd, size_t start, size_t size, off_t offset )
{
    char *dest = (char *)view->base + start;
    ssize_t count;

    assert( start <= view->size && size <= view->size - start );
    while (size)
    {
        count = pread( fd, dest, min( size, SSIZE_MAX ), offset );
        if (count < 0)
        {
            if (errno == EINTR) continue;
            return errno_to_status( errno );
        }
        if (!count) return STATUS_END_OF_FILE;
        dest += count;
        offset += count;
        size -= count;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_map_private_file_view( struct file_view *view, int fd, off_t offset,
                                           size_t file_size, unsigned int vprot )
{
    NTSTATUS status;

    /* ensure_backed zeroes whole new 64 KiB units, including the unread file
     * tail and ownership slack. No file mmap may replace those owned units. */
    status = ps5_set_vprot( view, view->base, view->size, VPROT_COMMITTED | VPROT_READ | VPROT_WRITE );
    if (status) return status;
    status = ps5_read_file_span( view, fd, 0, min( view->size, file_size ), offset );
    if (status) return status;
    return ps5_set_vprot( view, view->base, view->size, vprot | VPROT_COMMITTED );
}
#endif

/***********************************************************************
 *           map_file_into_view
 *
 * Wrapper for mmap() to map a file into a view, falling back to read if mmap fails.
 * virtual_mutex must be held by caller.
 */
static NTSTATUS map_file_into_view( struct file_view *view, int fd, size_t start, size_t size,
                                    off_t offset, unsigned int vprot, BOOL removable )
{
    char *map_addr, *host_addr;
    size_t map_size, host_size;
    int prot = PROT_READ | PROT_WRITE;
    unsigned int flags = MAP_FIXED;

    assert( start < view->size );
    assert( start + size <= view->size );

    if (vprot & VPROT_WRITE) flags |= MAP_SHARED;
    else if (vprot & VPROT_WRITECOPY) flags |= MAP_PRIVATE;
    else
    {
        /* changes to the file are not guaranteed to be visible in read-only MAP_PRIVATE mappings,
         * but they are on Linux so we take advantage of it */
#ifdef __linux__
        flags |= MAP_PRIVATE;
#else
        flags |= MAP_SHARED;
        prot &= ~PROT_WRITE;
#endif
    }

    map_size = ROUND_SIZE( start, size, page_mask );
    map_addr = ROUND_ADDR( (char *)view->base + start, page_mask );
    host_addr = ROUND_ADDR( (char *)view->base + start, host_page_mask );
    /* last page doesn't need to be a full page */
    if (map_addr + map_size >= (char *)view->base + view->size) host_size = map_size;
    else host_size = ROUND_SIZE( 0, map_size, host_page_mask );

    /* only try mmap if media is not removable (or if we require write access),
       and if alignment is correct */
    if ((!removable || (flags & MAP_SHARED)) && host_addr == map_addr && host_size == map_size)
    {
        if (mmap( host_addr, host_size, prot, flags, fd, offset ) != MAP_FAILED)
            return STATUS_SUCCESS;

        switch (errno)
        {
        case EINVAL:  /* file offset is not page-aligned, fall back to read() */
            break;
        case ENOEXEC:
        case ENODEV:  /* filesystem doesn't support mmap(), fall back to read() */
            if (vprot & VPROT_WRITE)
            {
                ERR( "shared writable mmap not supported, broken filesystem?\n" );
                return STATUS_NOT_SUPPORTED;
            }
            break;
        case EACCES:
        case EPERM:  /* access error, fall back to read() */
            if (vprot & VPROT_WRITE) return STATUS_ACCESS_DENIED;
            break;
        default:
            ERR( "mmap error %s, range %p-%p, unix_prot %#x\n",
                 strerror(errno), map_addr, map_addr + map_size, prot );
            return STATUS_NO_MEMORY;
        }
    }

    if (vprot & VPROT_WRITE)
    {
        ERR( "unaligned shared mapping %p-%p not supported\n", map_addr, map_addr + map_size );
        return STATUS_INVALID_PARAMETER;
    }

    mprotect( map_addr, map_size, PROT_READ | PROT_WRITE );
    pread( fd, map_addr, size, offset );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           get_committed_size
 *
 * Get the size of the committed range with equal masked vprot bytes starting at base.
 * Also return the protections for the first page.
 */
static SIZE_T get_committed_size( struct file_view *view, void *base, size_t max_size, BYTE *vprot, BYTE vprot_mask )
{
    SIZE_T offset, size;

    base = ROUND_ADDR( base, page_mask );
    offset = (char *)base - (char *)view->base;

    if (view->protect & SEC_RESERVE)
    {
        size = 0;

        *vprot = get_page_vprot( base );

        SERVER_START_REQ( get_mapping_committed_range )
        {
            req->base   = wine_server_client_ptr( view->base );
            req->offset = offset;
            if (!wine_server_call( req ))
            {
                size = min( reply->size, max_size );
                if (reply->committed)
                {
                    *vprot |= VPROT_COMMITTED;
                    set_page_vprot_bits( base, size, VPROT_COMMITTED, 0 );
                }
            }
        }
        SERVER_END_REQ;

        if (!size || !(vprot_mask & ~VPROT_COMMITTED)) return size;
    }
    else size = min( view->size - offset, max_size );

    return get_vprot_range_size( base, size, vprot_mask, vprot );
}

#ifdef __PROSPERO__
static BOOL ps5_unit_has_committed_pages( char *unit, char *base, char *end )
{
    char *page;

    for (page = unit; page < unit + granularity_mask + 1; page += page_size)
        if ((page < base || page >= end) && (get_page_vprot( page ) & VPROT_COMMITTED)) return TRUE;
    return FALSE;
}

/* Split children may share a boundary unit, but never a unit with another
 * independent reservation. Keep its virtual reservation until the last child
 * owning any of its logical pages has gone, including an inaccessible tail. */
static BOOL ps5_unit_has_view( struct file_view *view, char *unit, char *base, char *end )
{
    char *unit_end = unit + granularity_mask + 1;
    char *view_end = (char *)view->base + view->size;
    struct file_view *other;

    if ((base > (char *)view->base && unit < base && unit_end > (char *)view->base) ||
        (end < view_end && unit_end > end && unit < view_end))
        return TRUE;
    other = WINE_RB_ENTRY_VALUE( rb_prev( &view->entry ), struct file_view, entry );
    if (other && (char *)other->base < unit_end && (char *)other->base + other->size > unit) return TRUE;
    other = WINE_RB_ENTRY_VALUE( rb_next( &view->entry ), struct file_view, entry );
    return other && (char *)other->base < unit_end && (char *)other->base + other->size > unit;
}

static NTSTATUS ps5_change_pages( struct file_view *view, char *base, size_t size, BOOL release )
{
    char *unit, *start, *end, *host_start;
    char *limit = base + size;
    size_t host_size;
    BOOL keep, ret;

    for (unit = ROUND_ADDR( base, granularity_mask ); unit < limit; unit += granularity_mask + 1)
    {
        start = max( unit, base );
        end = min( unit + granularity_mask + 1, limit );
        keep = !release || ps5_unit_has_view( view, unit, base, limit );
        if (!keep || !ps5_unit_has_committed_pages( unit, base, limit ))
        {
            ret = ps5_direct_drop_unit( view->direct, unit, keep );
            /* A physical-release or re-reserve failure may follow a successful
             * unmap. Do not leave Wine claiming those pages are committed. The
             * view and physical offset stay owned for a later cleanup attempt. */
            if (!ps5_direct_is_backed( view->direct, unit ))
                set_page_vprot_bits( start, end - start, 0, VPROT_COMMITTED );
            if (!ret) return STATUS_NO_MEMORY;
        }
        else if (ps5_direct_is_backed( view->direct, unit ))
        {
            /* A 4 KiB decommit must become zero even when committed neighbours
             * keep the unit alive. Temporarily open only the affected 16 KiB
             * host pages and never erase their neighbouring Windows pages. */
            host_start = ROUND_ADDR( start, host_page_mask );
            host_size = ROUND_SIZE( start, end - start, host_page_mask );
            if (ps5_direct_protect( view->direct, host_start, host_size, PROT_READ | PROT_WRITE ))
                return STATUS_ACCESS_DENIED;
            memset( start, 0, end - start );
        }
        if (release) set_page_vprot( start, end - start, 0 );
        else set_page_vprot_bits( start, end - start, 0, VPROT_COMMITTED );
        if (keep && mprotect_range( start, end - start, 0, 0 )) return STATUS_ACCESS_DENIED;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_free_pages( struct file_view *view, char *base, size_t size )
{
    return ps5_change_pages( view, base, size, TRUE );
}
#endif


/***********************************************************************
 *           decommit_pages
 *
 * Decommit some pages of a given view.
 * virtual_mutex must be held by caller.
 */
static NTSTATUS decommit_pages( struct file_view *view, char *base, size_t size )
{
    char *host_end, *host_start = (char *)ROUND_SIZE( 0, base, host_page_mask );
#ifdef __PROSPERO__
    if (view->protect & VPROT_PS5_DIRECT)
        return ps5_change_pages( view, base, size ? size : view->size, FALSE );
#endif

    if (!size)
    {
        size = view->size;
        host_end = host_start + view->size;
    }
    else host_end = ROUND_ADDR( base + size, host_page_mask );

    if (host_start < host_end) anon_mmap_fixed( host_start, host_end - host_start, PROT_NONE, 0 );
    set_page_vprot_bits( base, size, 0, VPROT_COMMITTED );
    if (host_start < host_end) kernel_writewatch_register_range( view, host_start, host_end - host_start );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           remove_pages_from_view
 *
 * Remove some pages of a given view.
 * virtual_mutex must be held by caller.
 */
static NTSTATUS remove_pages_from_view( struct file_view *view, char *base, size_t size )
{
    assert( size < view->size );

    if (view->base != base && base + size != (char *)view->base + view->size)
    {
        struct file_view *new_view = alloc_view();

        if (!new_view)
        {
            ERR( "out of memory for %p-%p\n", base, base + size );
            return STATUS_NO_MEMORY;
        }
#ifdef __PROSPERO__
        if (view->protect & VPROT_PS5_DIRECT)
        {
            NTSTATUS status = ps5_free_pages( view, base, size );
            if (status)
            {
                new_view->protect = 0;
                free_view( new_view );
                return status;
            }
        }
#endif
        new_view->base    = base + size;
        new_view->size    = (char *)view->base + view->size - (char *)new_view->base;
        new_view->protect = view->protect;
#ifdef __PROSPERO__
        new_view->direct = view->direct;
        new_view->map_profile = view->map_profile;
        if (view->protect & VPROT_PS5_DIRECT) ps5_direct_retain_view( view->direct );
#endif

        unregister_view( view );
        view->size = base - (char *)view->base;
        register_view( view );
        register_view( new_view );

        VIRTUAL_DEBUG_DUMP_VIEW( view );
        VIRTUAL_DEBUG_DUMP_VIEW( new_view );
    }
    else
    {
#ifdef __PROSPERO__
        if (view->protect & VPROT_PS5_DIRECT)
        {
            NTSTATUS status = ps5_free_pages( view, base, size );
            if (status) return status;
        }
#endif
        unregister_view( view );
        if (view->base == base)
        {
            view->base = base + size;
            view->size -= size;
        }
        else view->size = base - (char *)view->base;

        register_view( view );
        VIRTUAL_DEBUG_DUMP_VIEW( view );
    }
#ifdef __PROSPERO__
    if (title_profile_is_enabled()) title_map_removed( view->map_profile, size );
#endif
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           free_pages_preserve_placeholder
 *
 * Turn pages of a given view into a placeholder.
 * virtual_mutex must be held by caller.
 */
static NTSTATUS free_pages_preserve_placeholder( struct file_view *view, char *base, size_t size )
{
    NTSTATUS status;

    if (!size) return STATUS_INVALID_PARAMETER_3;
    if (!(view->protect & VPROT_PLACEHOLDER)) return STATUS_CONFLICTING_ADDRESSES;
    if (view->protect & VPROT_FREE_PLACEHOLDER && size == view->size) return STATUS_CONFLICTING_ADDRESSES;

    if (size < view->size)
    {
        if ((UINT_PTR)base & host_page_mask ||
            ((size & host_page_mask) && base + size != (char *)view->base + view->size))
        {
            ERR( "unaligned partial free %p-%p\n", base, base + size );
            return STATUS_CONFLICTING_ADDRESSES;
        }

        status = remove_pages_from_view( view, base, size );
        if (status) return status;

        status = create_view( &view, base, size, VPROT_PLACEHOLDER | VPROT_FREE_PLACEHOLDER );
        if (status) return status;
    }

    view->protect = VPROT_PLACEHOLDER | VPROT_FREE_PLACEHOLDER;
    set_page_vprot( view->base, view->size, 0 );
    anon_mmap_fixed( view->base, ROUND_SIZE( 0, view->size, host_page_mask ), PROT_NONE, 0 );
#ifdef __PROSPERO__
    if (title_profile_is_enabled()) title_map_unmapped( view );
#endif
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           free_pages
 *
 * Free some pages of a given view.
 * virtual_mutex must be held by caller.
 */
static NTSTATUS free_pages( struct file_view *view, char *base, size_t size )
{
    char *host_base = (char *)ROUND_SIZE( 0, base, host_page_mask );
    char *host_end = base + size;
    NTSTATUS status;

    if (size == view->size)
    {
        assert( base == view->base );
        return delete_view( view );
    }

    /* new view needs to start on page boundary */

    if (view->base == base)  /* shrink from the start */
    {
        if (size & host_page_mask)
        {
            ERR( "unaligned partial free %p-%p\n", base, base + size );
            return STATUS_CONFLICTING_ADDRESSES;
        }
    }
    else if (base + size < (char *)view->base + view->size)  /* create a hole */
    {
        if ((UINT_PTR)(base + size) & host_page_mask)
        {
            ERR( "unaligned partial free %p-%p\n", base, base + size );
            return STATUS_CONFLICTING_ADDRESSES;
        }
    }

    status = remove_pages_from_view( view, base, size );
    if (!status)
    {
        set_page_vprot( base, size, 0 );
        if (view->protect & VPROT_ARM64EC) clear_arm64ec_range( base, size );
#ifdef __PROSPERO__
        if (view->protect & VPROT_PS5_DIRECT) return STATUS_SUCCESS;
#endif
        if (host_base < host_end) unmap_area( host_base, host_end - host_base );
    }
    return status;
}


/***********************************************************************
 *           coalesce_placeholders
 *
 * Coalesce placeholder views.
 * virtual_mutex must be held by caller.
 */
static NTSTATUS coalesce_placeholders( struct file_view *view, char *base, size_t size )
{
    struct rb_entry *next;
    struct file_view *curr_view, *next_view;
    unsigned int i, view_count = 0;
    size_t views_size = 0;

    if (!size) return STATUS_INVALID_PARAMETER_3;
    if (base != view->base) return STATUS_CONFLICTING_ADDRESSES;

    curr_view = view;
    while (curr_view->protect & VPROT_FREE_PLACEHOLDER)
    {
        ++view_count;
        views_size += curr_view->size;
        if (views_size >= size) break;
        if (!(next = rb_next( &curr_view->entry ))) break;
        next_view = RB_ENTRY_VALUE( next, struct file_view, entry );
        if ((char *)curr_view->base + curr_view->size != next_view->base) break;
        curr_view = next_view;
    }

    if (view_count < 2 || size != views_size) return STATUS_CONFLICTING_ADDRESSES;

    for (i = 1; i < view_count; ++i)
    {
        curr_view = RB_ENTRY_VALUE( rb_next( &view->entry ), struct file_view, entry );
        unregister_view( curr_view );
        free_view( curr_view );
    }

    unregister_view( view );
    view->size = views_size;
    register_view( view );

    VIRTUAL_DEBUG_DUMP_VIEW( view );

    return STATUS_SUCCESS;
}


/***********************************************************************
 *           allocate_dos_memory
 *
 * Allocate the DOS memory range.
 */
static NTSTATUS allocate_dos_memory( struct file_view **view, unsigned int vprot )
{
    size_t size;
    void *addr = NULL;
    void * const low_64k = (void *)0x10000;
    const size_t dosmem_size = 0x110000;
    int unix_prot = get_unix_prot( vprot ) & ~PROT_EXEC;

    /* check for existing view */

    if (find_view_range( 0, dosmem_size )) return STATUS_CONFLICTING_ADDRESSES;

    /* check without the first 64K */

    if (!mmap_is_reserved_and_mapped( low_64k, dosmem_size - 0x10000 ))
    {
        addr = anon_mmap_tryfixed( low_64k, dosmem_size - 0x10000, unix_prot, 0 );
        if (addr == MAP_FAILED) return map_view( view, NULL, dosmem_size, 0, vprot, 0, 0, 0 );
    }

    /* now try to allocate the low 64K too */

    if (!mmap_is_reserved_and_mapped( NULL, 0x10000 ))
    {
        addr = anon_mmap_tryfixed( (void *)host_page_size, 0x10000 - host_page_size, unix_prot, 0 );
        if (addr != MAP_FAILED)
        {
            if (!anon_mmap_fixed( NULL, host_page_size, unix_prot, 0 ))
            {
                addr = NULL;
                TRACE( "successfully mapped low 64K range\n" );
            }
            else TRACE( "failed to map page 0\n" );
        }
        else
        {
            addr = low_64k;
            TRACE( "failed to map low 64K range\n" );
        }
    }

    /* now reserve the whole range */

    size = (char *)dosmem_size - (char *)addr;
    anon_mmap_fixed( addr, size, unix_prot, 0 );
    return create_view( view, addr, size, vprot );
}


/***********************************************************************
 *           map_pe_header
 *
 * Map the header of a PE file into memory.
 */
static NTSTATUS map_pe_header( void *ptr, size_t size, size_t map_size, int fd, BOOL *removable )
{
    if (!size) return STATUS_INVALID_IMAGE_FORMAT;

    map_size &= ~host_page_mask;

    if (!*removable && map_size)
    {
        if (mmap( ptr, map_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, fd, 0 ) != MAP_FAILED)
        {
            if (size > map_size) pread( fd, (char *)ptr + map_size, size - map_size, map_size );
            return STATUS_SUCCESS;
        }
        switch (errno)
        {
        case EPERM:
        case EACCES:
            WARN( "noexec file system, falling back to read\n" );
            break;
        case ENOEXEC:
        case ENODEV:
            WARN( "file system doesn't support mmap, falling back to read\n" );
            break;
        default:
            ERR( "mmap error %s, range %p-%p\n", strerror(errno), ptr, (char *)ptr + size );
            return STATUS_NO_MEMORY;
        }
        *removable = TRUE;
    }
    pread( fd, ptr, size, 0 );
    return STATUS_SUCCESS;  /* page protections will be updated later */
}

#ifdef _WIN64

/***********************************************************************
 *           get_host_addr_space_limit
 */
static void *get_host_addr_space_limit(void)
{
    unsigned int flags = MAP_PRIVATE | MAP_ANON;
    UINT_PTR addr = (UINT_PTR)1 << 63;

#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif

    while (addr >> 32)
    {
        void *ret = mmap( (void *)addr, host_page_size, PROT_NONE, flags, -1, 0 );
        if (ret != MAP_FAILED)
        {
            munmap( ret, host_page_size );
            if (ret >= (void *)addr) break;
        }
        else if (errno == EEXIST) break;
        addr >>= 1;
    }
    return (void *)((addr << 1) - (granularity_mask + 1));
}

#endif /* _WIN64 */

#ifdef __aarch64__

/***********************************************************************
 *           alloc_arm64ec_map
 */
static void alloc_arm64ec_map(void)
{
    unsigned int status;
    SIZE_T size = ((ULONG_PTR)address_space_limit + page_size) >> (page_shift + 3);  /* one bit per page */

    size = ROUND_SIZE( 0, size, host_page_mask );
    status = map_view( &arm64ec_view, NULL, size, MEM_TOP_DOWN, VPROT_READ | VPROT_COMMITTED, 0, 0, 0 );
    if (status)
    {
        ERR( "failed to allocate ARM64EC map: %08x\n", status );
        exit(1);
    }
    peb->EcCodeBitMap = arm64ec_view->base;
}


/***********************************************************************
 *           update_arm64ec_ranges
 */
static void update_arm64ec_ranges( struct file_view *view, IMAGE_NT_HEADERS *nt,
                                   const IMAGE_DATA_DIRECTORY *dir, UINT *entry_point )
{
    const IMAGE_ARM64EC_METADATA *metadata;
    const IMAGE_CHPE_RANGE_ENTRY *map;
    char *base = view->base;
    const IMAGE_LOAD_CONFIG_DIRECTORY *cfg = (void *)(base + dir->VirtualAddress);
    ULONG i, size = min( dir->Size, cfg->Size );

    if (size <= offsetof( IMAGE_LOAD_CONFIG_DIRECTORY, CHPEMetadataPointer )) return;
    if (!cfg->CHPEMetadataPointer) return;
    if (!arm64ec_view) alloc_arm64ec_map();
    commit_arm64ec_map( view );
    metadata = (void *)(base + (cfg->CHPEMetadataPointer - nt->OptionalHeader.ImageBase));
    *entry_point = redirect_arm64ec_rva( base, nt->OptionalHeader.AddressOfEntryPoint, metadata );
    if (!metadata->CodeMap) return;
    map = (void *)(base + metadata->CodeMap);

    for (i = 0; i < metadata->CodeMapCount; i++)
    {
        if ((map[i].StartOffset & 0x3) != 1 /* arm64ec */) continue;
        set_arm64ec_range( base + (map[i].StartOffset & ~3), map[i].Length );
    }
}


/***********************************************************************
 *           apply_arm64x_relocations
 */
static void apply_arm64x_relocations( char *base, const IMAGE_BASE_RELOCATION *reloc, size_t size )
{
    const IMAGE_BASE_RELOCATION *reloc_end = (const IMAGE_BASE_RELOCATION *)((const char *)reloc + size);

    while (reloc < reloc_end - 1 && reloc->SizeOfBlock)
    {
        const USHORT *rel = (const USHORT *)(reloc + 1);
        const USHORT *rel_end = (const USHORT *)reloc + reloc->SizeOfBlock / sizeof(USHORT);
        char *page = base + reloc->VirtualAddress;

        while (rel < rel_end && *rel)
        {
            USHORT offset = *rel & 0xfff;
            USHORT type = (*rel >> 12) & 3;
            USHORT arg = *rel >> 14;
            int val;
            rel++;
            switch (type)
            {
            case IMAGE_DVRT_ARM64X_FIXUP_TYPE_ZEROFILL:
                memset( page + offset, 0, 1 << arg );
                break;
            case IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE:
                memcpy( page + offset, rel, 1 << arg );
                rel += (1 << arg) / sizeof(USHORT);
                break;
            case IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA:
                val = (unsigned int)*rel++ * ((arg & 2) ? 8 : 4);
                if (arg & 1) val = -val;
                *(int *)(page + offset) += val;
                break;
            }
        }
        reloc = (const IMAGE_BASE_RELOCATION *)rel_end;
    }
}


/***********************************************************************
 *           update_arm64x_mapping
 */
static void update_arm64x_mapping( struct file_view *view, IMAGE_NT_HEADERS *nt,
                                   const IMAGE_DATA_DIRECTORY *dir, IMAGE_SECTION_HEADER *sections )
{
    const IMAGE_DYNAMIC_RELOCATION_TABLE *table;
    const char *ptr, *end;
    char *base = view->base;
    const IMAGE_LOAD_CONFIG_DIRECTORY *cfg = (void *)(base + dir->VirtualAddress);
    ULONG sec, offset, size = min( dir->Size, cfg->Size );

    if (size <= offsetof( IMAGE_LOAD_CONFIG_DIRECTORY, DynamicValueRelocTableSection )) return;
    offset = cfg->DynamicValueRelocTableOffset;
    sec = cfg->DynamicValueRelocTableSection;
    if (!sec || sec > nt->FileHeader.NumberOfSections) return;
    if (offset >= sections[sec - 1].Misc.VirtualSize) return;
    table = (const IMAGE_DYNAMIC_RELOCATION_TABLE *)(base + sections[sec - 1].VirtualAddress + offset);
    ptr = (const char *)(table + 1);
    end = ptr + table->Size;
    switch (table->Version)
    {
    case 1:
        while (ptr < end)
        {
            const IMAGE_DYNAMIC_RELOCATION64 *dyn = (const IMAGE_DYNAMIC_RELOCATION64 *)ptr;
            if (dyn->Symbol == IMAGE_DYNAMIC_RELOCATION_ARM64X)
            {
                apply_arm64x_relocations( base, (const IMAGE_BASE_RELOCATION *)(dyn + 1),
                                          dyn->BaseRelocSize );
                break;
            }
            ptr += sizeof(*dyn) + dyn->BaseRelocSize;
        }
        break;
    case 2:
        while (ptr < end)
        {
            const IMAGE_DYNAMIC_RELOCATION64_V2 *dyn = (const IMAGE_DYNAMIC_RELOCATION64_V2 *)ptr;
            if (dyn->Symbol == IMAGE_DYNAMIC_RELOCATION_ARM64X)
            {
                apply_arm64x_relocations( base, (const IMAGE_BASE_RELOCATION *)(dyn + 1),
                                          dyn->FixupInfoSize );
                break;
            }
            ptr += dyn->HeaderSize + dyn->FixupInfoSize;
        }
        break;
    default:
        FIXME( "unsupported version %u\n", table->Version );
        break;
    }
}

#endif  /* __aarch64__ */

/***********************************************************************
 *           get_data_dir
 */
static IMAGE_DATA_DIRECTORY *get_data_dir( IMAGE_NT_HEADERS *nt, SIZE_T total_size, ULONG dir )
{
    IMAGE_DATA_DIRECTORY *data;

    switch (nt->OptionalHeader.Magic)
    {
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
        if (dir >= ((IMAGE_NT_HEADERS64 *)nt)->OptionalHeader.NumberOfRvaAndSizes) return NULL;
        data = &((IMAGE_NT_HEADERS64 *)nt)->OptionalHeader.DataDirectory[dir];
        break;
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        if (dir >= ((IMAGE_NT_HEADERS32 *)nt)->OptionalHeader.NumberOfRvaAndSizes) return NULL;
        data = &((IMAGE_NT_HEADERS32 *)nt)->OptionalHeader.DataDirectory[dir];
        break;
    default:
        return NULL;
    }
    if (!data->Size) return NULL;
    if (!data->VirtualAddress) return NULL;
    if (data->VirtualAddress >= total_size) return NULL;
    if (data->Size > total_size - data->VirtualAddress) return NULL;
    return data;
}


/***********************************************************************
 *           process_relocation_block
 *
 * Reimplementation of LdrProcessRelocationBlock.
 */
static IMAGE_BASE_RELOCATION *process_relocation_block( char *page, IMAGE_BASE_RELOCATION *rel,
                                                        INT_PTR delta )
{
    USHORT *reloc = (USHORT *)(rel + 1);
    unsigned int count;

    for (count = (rel->SizeOfBlock - sizeof(*rel)) / sizeof(USHORT); count; count--, reloc++)
    {
        USHORT offset = *reloc & 0xfff;
        switch (*reloc >> 12)
        {
        case IMAGE_REL_BASED_ABSOLUTE:
            break;
        case IMAGE_REL_BASED_HIGH:
            *(short *)(page + offset) += HIWORD(delta);
            break;
        case IMAGE_REL_BASED_LOW:
            *(short *)(page + offset) += LOWORD(delta);
            break;
        case IMAGE_REL_BASED_HIGHLOW:
            *(int *)(page + offset) += delta;
            break;
        case IMAGE_REL_BASED_DIR64:
            *(INT64 *)(page + offset) += delta;
            break;
        case IMAGE_REL_BASED_THUMB_MOV32:
        {
            DWORD *inst = (DWORD *)(page + offset);
            WORD lo = ((inst[0] << 1) & 0x0800) + ((inst[0] << 12) & 0xf000) +
                      ((inst[0] >> 20) & 0x0700) + ((inst[0] >> 16) & 0x00ff);
            WORD hi = ((inst[1] << 1) & 0x0800) + ((inst[1] << 12) & 0xf000) +
                      ((inst[1] >> 20) & 0x0700) + ((inst[1] >> 16) & 0x00ff);
            DWORD imm = MAKELONG( lo, hi ) + delta;

            lo = LOWORD( imm );
            hi = HIWORD( imm );
            inst[0] = (inst[0] & 0x8f00fbf0) + ((lo >> 1) & 0x0400) + ((lo >> 12) & 0x000f) +
                                               ((lo << 20) & 0x70000000) + ((lo << 16) & 0xff0000);
            inst[1] = (inst[1] & 0x8f00fbf0) + ((hi >> 1) & 0x0400) + ((hi >> 12) & 0x000f) +
                                               ((hi << 20) & 0x70000000) + ((hi << 16) & 0xff0000);
            break;
        }
        default:
            FIXME( "Unknown/unsupported relocation %x\n", *reloc );
            return NULL;
        }
    }
    return (IMAGE_BASE_RELOCATION *)reloc;  /* return address of next block */
}


/***********************************************************************
 *           map_image_into_view
 *
 * Map an executable (PE format) image into an existing view.
 * virtual_mutex must be held by caller.
 */
static NTSTATUS map_image_into_view( struct file_view *view, const UNICODE_STRING *nt_name, int fd,
                                     struct pe_image_info *image_info, USHORT machine,
                                     int shared_fd, int layout_fd, BOOL removable )
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sections = NULL, *sec;
    IMAGE_DATA_DIRECTORY *imports, *dir;
    NTSTATUS status = STATUS_CONFLICTING_ADDRESSES;
    int i;
    off_t pos;
    struct stat st;
    char *header_end;
    char *ptr = view->base;
    SIZE_T header_size, header_map_size, total_size = view->size;
    SIZE_T align_mask = max( image_info->alignment - 1, page_mask );
    INT_PTR delta;
    ULONG_PTR image_base = image_info->base;
    BOOL laid_out = FALSE;
#ifdef __PROSPERO__
    SIZE_T populated_end = 0; /* high-water mark of file bytes read into freshly zeroed backing */
#endif

    TRACE_(module)( "mapping PE file %s at %p-%p\n", debugstr_us(nt_name), ptr, ptr + total_size );

    /* map the header */

    if (fstat( fd, &st ))
    {
#ifdef __PROSPERO__
        if (view->protect & VPROT_PS5_DIRECT) return errno_to_status( errno );
#endif
    }
    header_size = min( image_info->header_size, st.st_size );
    header_map_size = min( image_info->header_map_size, ROUND_SIZE( 0, st.st_size, host_page_mask ));
#ifdef __PROSPERO__
    if (view->protect & VPROT_PS5_DIRECT)
    {
        status = ps5_set_vprot( view, ptr, total_size, VPROT_COMMITTED | VPROT_READ | VPROT_WRITE );
        if (status) return status;
        if (header_size < sizeof(*dos) || header_size > total_size) return STATUS_INVALID_IMAGE_FORMAT;
        if ((status = ps5_read_file_span( view, fd, 0,
                                         image_info->image_flags & IMAGE_FLAGS_ImageMappedFlat
                                         ? min( total_size, st.st_size ) : header_size, 0 ))) return status;
        populated_end = header_size;
    }
    else
    /* The payload's image layout remains a shared copy-on-write file cache;
     * the private title server deliberately supplies no redundant layout. */
    if (layout_fd != -1 &&
        map_file_into_view( view, layout_fd, 0, total_size, 0, VPROT_COMMITTED | VPROT_READ | VPROT_WRITECOPY,
                            FALSE ) == STATUS_SUCCESS)
        laid_out = TRUE;
    else
#endif
    if ((status = map_pe_header( view->base, header_size, header_map_size, fd, &removable )))
        return status;

    status = STATUS_INVALID_IMAGE_FORMAT;  /* generic error */
    dos = (IMAGE_DOS_HEADER *)ptr;
#ifdef __PROSPERO__
    if ((view->protect & VPROT_PS5_DIRECT) &&
        (dos->e_lfanew < 0 || dos->e_lfanew > total_size ||
         sizeof(*nt) > total_size - dos->e_lfanew))
        return status;
#endif

    nt = (IMAGE_NT_HEADERS *)(ptr + dos->e_lfanew);
    header_end = ptr + ROUND_SIZE( 0, header_size, align_mask );
#ifdef __PROSPERO__
    if (!(view->protect & VPROT_PS5_DIRECT))
#endif
        {
#ifdef __PROSPERO__
            if (title_profile_is_enabled()) title_map_zero( ptr + header_size, header_end - (ptr + header_size) );
            else
#endif
                memset( ptr + header_size, 0, header_end - (ptr + header_size) );
        }
    if ((char *)(nt + 1) > header_end) return status;
    sec = IMAGE_FIRST_SECTION( nt );
    if ((char *)(sec + nt->FileHeader.NumberOfSections) > header_end) return status;
    if ((char *)(sec + nt->FileHeader.NumberOfSections) > ptr + image_info->header_map_size)
    {
        /* copy section data since it will get overwritten by a section mapping */
        if (!(sections = malloc( sizeof(*sections) * nt->FileHeader.NumberOfSections )))
            return STATUS_NO_MEMORY;
        memcpy( sections, sec, sizeof(*sections) * nt->FileHeader.NumberOfSections );
        sec = sections;
    }
    imports = get_data_dir( nt, total_size, IMAGE_DIRECTORY_ENTRY_IMPORT );

    /* check for non page-aligned binary */

    if (image_info->image_flags & IMAGE_FLAGS_ImageMappedFlat)
    {
        /* unaligned sections, this happens for native subsystem binaries */
        /* in that case Windows simply maps in the whole file */

        total_size = min( total_size, ROUND_SIZE( 0, st.st_size, page_mask ));
#ifdef __PROSPERO__
        if (!(view->protect & VPROT_PS5_DIRECT))
#endif
        if (map_file_into_view( view, fd, 0, total_size, 0, VPROT_COMMITTED | VPROT_READ | VPROT_WRITECOPY,
                                removable ) != STATUS_SUCCESS) goto done;

        /* check that all sections are loaded at the right offset */
        if (nt->OptionalHeader.FileAlignment != nt->OptionalHeader.SectionAlignment) goto done;
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            if (sec[i].VirtualAddress != sec[i].PointerToRawData)
                goto done;  /* Windows refuses to load in that case too */
        }

        /* set the image protections */
#ifdef __PROSPERO__
        if (view->protect & VPROT_PS5_DIRECT)
        {
            BYTE vprot = VPROT_COMMITTED | VPROT_READ | VPROT_WRITECOPY | VPROT_EXEC;
            if (enable_write_exceptions) vprot |= VPROT_WRITEWATCH;
            set_page_vprot( ptr, view->size, vprot );
            if (mprotect_range( ptr, view->size, 0, 0 )) { status = STATUS_ACCESS_DENIED; goto done; }
        }
        else
#endif
            set_vprot( view, ptr, total_size, VPROT_COMMITTED | VPROT_READ | VPROT_WRITECOPY | VPROT_EXEC );

        /* no relocations are performed on non page-aligned binaries */
        status = STATUS_SUCCESS;
        goto done;
    }


    /* map all the sections */

    for (i = pos = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        static const SIZE_T sector_align = 0x1ff;
        SIZE_T map_size, file_start, file_size, end;
        NTSTATUS read_status;

        if (!sec[i].Misc.VirtualSize)
            map_size = ROUND_SIZE( 0, sec[i].SizeOfRawData, align_mask );
        else
            map_size = ROUND_SIZE( 0, sec[i].Misc.VirtualSize, align_mask );

        /* file positions are rounded to sector boundaries regardless of OptionalHeader.FileAlignment */
        file_start = sec[i].PointerToRawData & ~sector_align;
        file_size = ROUND_SIZE( sec[i].PointerToRawData, sec[i].SizeOfRawData, sector_align );
        if (file_size > map_size) file_size = map_size;

        /* a few sanity checks */
        end = sec[i].VirtualAddress + ROUND_SIZE( sec[i].VirtualAddress, map_size, align_mask );
        if (sec[i].VirtualAddress > total_size || end > total_size || end < sec[i].VirtualAddress)
        {
            WARN_(module)( "%s section %.8s too large (%x+%lx/%lx)\n",
                           debugstr_us(nt_name), sec[i].Name, sec[i].VirtualAddress, map_size, total_size );
            goto done;
        }

        if ((sec[i].Characteristics & IMAGE_SCN_MEM_SHARED) &&
            (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE))
        {
            TRACE_(module)( "%s mapping shared section %.8s at %p off %x (%x) size %lx (%lx) flags %x\n",
                            debugstr_us(nt_name), sec[i].Name, ptr + sec[i].VirtualAddress,
                            sec[i].PointerToRawData, (int)pos, file_size, map_size,
                            sec[i].Characteristics );
            if (map_file_into_view( view, shared_fd, sec[i].VirtualAddress, map_size, pos,
                                    VPROT_COMMITTED | VPROT_READ | VPROT_WRITE, FALSE ) != STATUS_SUCCESS)
            {
                ERR_(module)( "Could not map %s shared section %.8s\n", debugstr_us(nt_name), sec[i].Name );
                goto done;
            }

            /* check if the import directory falls inside this section */
            if (imports && imports->VirtualAddress >= sec[i].VirtualAddress &&
                imports->VirtualAddress < sec[i].VirtualAddress + map_size)
            {
                UINT_PTR base = imports->VirtualAddress & ~host_page_mask;
                UINT_PTR end = base + ROUND_SIZE( imports->VirtualAddress, imports->Size, host_page_mask );
                if (end > sec[i].VirtualAddress + map_size) end = sec[i].VirtualAddress + map_size;
                if (end > base)
                    map_file_into_view( view, shared_fd, base, end - base,
                                        pos + (base - sec[i].VirtualAddress),
                                        VPROT_COMMITTED | VPROT_READ | VPROT_WRITECOPY, FALSE );
            }
            pos += map_size;
            continue;
        }

        TRACE_(module)( "mapping %s section %.8s at %p off %x size %x virt %x flags %x\n",
                        debugstr_us(nt_name), sec[i].Name, ptr + sec[i].VirtualAddress,
                        sec[i].PointerToRawData, sec[i].SizeOfRawData,
                        sec[i].Misc.VirtualSize, sec[i].Characteristics );

        if (!sec[i].PointerToRawData || !file_size) continue;

        /* Note: if the section is not aligned properly map_file_into_view will magically
         *       fall back to read(), so we don't need to check anything here.
         */
        end = file_start + file_size;
        if (!laid_out)
        {
            if (sec[i].PointerToRawData >= st.st_size ||
                end > ((st.st_size + sector_align) & ~sector_align) || end < file_start)
                goto truncated;
#ifdef __PROSPERO__
            if (view->protect & VPROT_PS5_DIRECT)
                read_status = ps5_read_file_span( view, fd, sec[i].VirtualAddress,
                                                  min( file_size, st.st_size - file_start ), file_start );
            else
#endif
                read_status = map_file_into_view( view, fd, sec[i].VirtualAddress, file_size, file_start,
                                                  VPROT_COMMITTED | VPROT_READ | VPROT_WRITECOPY, removable );
            if (read_status)
            {
truncated:
                ERR_(module)( "Could not map %s section %.8s, file probably truncated\n",
                              debugstr_us(nt_name), sec[i].Name );
                status = STATUS_INVALID_IMAGE_FORMAT;
                goto done;
            }
        }
#ifdef __PROSPERO__
        if (view->protect & VPROT_PS5_DIRECT)
            populated_end = max( populated_end, sec[i].VirtualAddress + file_size );
#endif

        end = min( ROUND_SIZE( 0, file_size, align_mask ), map_size );
#ifdef __PROSPERO__
        /* Materialize writable zero-filled image tails before applying the
         * final protections. Untouched tail pages can otherwise fault on
         * writes despite successful mprotect and writable Wine metadata
         * (Battle.net-Setup.exe); reapplying protection after the fault worked. */
        if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) end = map_size;
        /* Fresh direct backing is already zero. Clear only tails that may
         * overlap bytes populated by an earlier header/section, preserving
         * Wine's handling of overlapping sections without zeroing BSS twice. */
        if (view->protect & VPROT_PS5_DIRECT)
            end = min( end, max( file_size, populated_end > sec[i].VirtualAddress
                                ? populated_end - sec[i].VirtualAddress : 0 ) );
#endif
        if (end > file_size)
        {
            TRACE_(module)("clearing %p - %p\n",
                           ptr + sec[i].VirtualAddress + file_size,
                           ptr + sec[i].VirtualAddress + end );
#ifdef __PROSPERO__
            if (title_profile_is_enabled())
                title_map_zero( ptr + sec[i].VirtualAddress + file_size, end - file_size );
            else
#endif
            memset( ptr + sec[i].VirtualAddress + file_size, 0, end - file_size );
        }
    }

#ifdef __aarch64__
    if ((dir = get_data_dir( nt, total_size, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG )))
    {
        if (image_info->machine == IMAGE_FILE_MACHINE_ARM64 &&
            (machine == IMAGE_FILE_MACHINE_AMD64 ||
             (!machine && main_image_info.Machine == IMAGE_FILE_MACHINE_AMD64)))
        {
            update_arm64x_mapping( view, nt, dir, sec );
            /* reload changed machine from NT header */
            image_info->machine = nt->FileHeader.Machine;
        }
        if (image_info->machine == IMAGE_FILE_MACHINE_AMD64)
            update_arm64ec_ranges( view, nt, dir, &image_info->entry_point );
    }
#endif
    if (machine && machine != nt->FileHeader.Machine)
    {
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }

    /* relocate to dynamic base */

#ifdef __PROSPERO__
    /* a layout is relocated already, for the address its header names */
    if (laid_out)
        image_base = nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
                     ? ((IMAGE_NT_HEADERS64 *)nt)->OptionalHeader.ImageBase
                     : ((IMAGE_NT_HEADERS32 *)nt)->OptionalHeader.ImageBase;
#endif
    if (image_info->map_addr && (delta = image_info->map_addr - image_base))
    {
        TRACE_(module)( "relocating %s dynamic base %lx -> %lx mapped at %p\n", debugstr_us(nt_name),
                        (ULONG_PTR)image_info->base, (ULONG_PTR)image_info->map_addr, ptr );

        if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            ((IMAGE_NT_HEADERS64 *)nt)->OptionalHeader.ImageBase = image_info->map_addr;
        else
            ((IMAGE_NT_HEADERS32 *)nt)->OptionalHeader.ImageBase = image_info->map_addr;

        if ((dir = get_data_dir( nt, total_size, IMAGE_DIRECTORY_ENTRY_BASERELOC )))
        {
            IMAGE_BASE_RELOCATION *rel = (IMAGE_BASE_RELOCATION *)(ptr + dir->VirtualAddress);
            IMAGE_BASE_RELOCATION *end = (IMAGE_BASE_RELOCATION *)((char *)rel + dir->Size);

            while (rel && rel < end - 1 && rel->SizeOfBlock && rel->VirtualAddress < total_size)
                rel = process_relocation_block( ptr + rel->VirtualAddress, rel, delta );
        }
    }

    /* set the image protections */

#ifdef __PROSPERO__
    if (view->protect & VPROT_PS5_DIRECT)
    {
        /* This view is not yet registered with the server. Build all guest
         * protections first, including no-access holes, then apply their
         * 16 KiB host unions once while the PE metadata is still readable. */
        set_page_vprot( ptr, view->size, VPROT_COMMITTED );
        set_page_vprot( ptr, ROUND_SIZE( 0, header_size, align_mask ), VPROT_COMMITTED | VPROT_READ );
    }
    else
#endif
        set_vprot( view, ptr, ROUND_SIZE( 0, header_size, align_mask ), VPROT_COMMITTED | VPROT_READ );

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        SIZE_T size;
        BYTE vprot = VPROT_COMMITTED;

        if (sec[i].Misc.VirtualSize)
            size = ROUND_SIZE( sec[i].VirtualAddress, sec[i].Misc.VirtualSize, align_mask );
        else
            size = ROUND_SIZE( sec[i].VirtualAddress, sec[i].SizeOfRawData, align_mask );

        if (sec[i].Characteristics & IMAGE_SCN_MEM_READ)    vprot |= VPROT_READ;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)   vprot |= VPROT_WRITECOPY;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) vprot |= VPROT_EXEC;

#ifdef __PROSPERO__
        if (view->protect & VPROT_PS5_DIRECT)
        {
            if (enable_write_exceptions && is_vprot_exec_write( vprot )) vprot |= VPROT_WRITEWATCH;
            set_page_vprot( ptr + sec[i].VirtualAddress, size, vprot );
        }
        else
#endif
        if (!set_vprot( view, ptr + sec[i].VirtualAddress, size, vprot ) && (vprot & VPROT_EXEC))
            ERR( "failed to set %08x protection on %s section %.8s, noexec filesystem?\n",
                 sec[i].Characteristics, debugstr_us(nt_name), sec[i].Name );
    }
#ifdef __PROSPERO__
    if ((view->protect & VPROT_PS5_DIRECT) && mprotect_range( ptr, view->size, 0, 0 ))
    {
        status = STATUS_ACCESS_DENIED;
        goto done;
    }
#endif


#ifdef VALGRIND_LOAD_PDB_DEBUGINFO
    VALGRIND_LOAD_PDB_DEBUGINFO(fd, ptr, total_size, ptr - (char *)wine_server_get_ptr( image_info->base ));
#endif
    r_debug_add_module( ptr, fd, ptr - (char *)wine_server_get_ptr( image_info->base ) );
    status = STATUS_SUCCESS;

done:
    free( sections );
    return status;
}


/***********************************************************************
 *             get_mapping_info
 */
static unsigned int get_mapping_info( HANDLE handle, ACCESS_MASK access, unsigned int *sec_flags,
                                      mem_size_t *full_size, HANDLE *shared_file, HANDLE *layout_file,
                                      struct pe_image_info **info, UNICODE_STRING *nt_name,
                                      ANSI_STRING *exp_name, BOOL *shared_writable )
{
    struct pe_image_info *image_info;
    SIZE_T namelen, total, size = 1024;
    unsigned int status;

    for (;;)
    {
        if (!(image_info = malloc( size ))) return STATUS_NO_MEMORY;

        SERVER_START_REQ( get_mapping_info )
        {
            req->handle = wine_server_obj_handle( handle );
            req->access = access;
            wine_server_set_reply( req, image_info, size );
            status = wine_server_call( req );
            *sec_flags   = reply->flags;
            *full_size   = reply->size;
            if (shared_writable) *shared_writable = reply->shared_writable;
            namelen      = reply->name_len;
            total        = reply->total;
            *shared_file = wine_server_ptr_handle( reply->shared_file );
            *layout_file = wine_server_ptr_handle( reply->layout_file );
        }
        SERVER_END_REQ;
        if (!status && total <= size) break;
        free( image_info );
        if (status) return status;
        if (*shared_file) NtClose( *shared_file );
        if (*layout_file) NtClose( *layout_file );
        size = total;
    }

    if (total)
    {
        assert( total >= sizeof(*image_info) );
        total -= sizeof(*image_info);
        nt_name->Buffer = (WCHAR *)(image_info + 1);
        nt_name->Length = nt_name->MaximumLength = namelen;
        exp_name->Buffer = (char *)nt_name->Buffer + namelen;
        exp_name->Length = exp_name->MaximumLength = total - namelen;
        *info = image_info;
    }
    else free( image_info );

    return STATUS_SUCCESS;
}


/***********************************************************************
 *             map_image_view
 *
 * Map a view for a PE image at an appropriate address.
 */
static NTSTATUS map_image_view( struct file_view **view_ret, struct pe_image_info *image_info, SIZE_T size,
                                ULONG_PTR limit_low, ULONG_PTR limit_high, ULONG alloc_type, BOOL private_image )
{
    unsigned int vprot = SEC_IMAGE | SEC_FILE | VPROT_COMMITTED | VPROT_READ | VPROT_EXEC | VPROT_WRITECOPY;
    void *base;
    NTSTATUS status;
    ULONG_PTR start, end;
    BOOL top_down = (image_info->image_charact & IMAGE_FILE_DLL) &&
                    (image_info->image_flags & IMAGE_FLAGS_ImageDynamicallyRelocated);
#ifdef __PROSPERO__
    /* A PE with shared writable sections cannot be replaced by private
     * physical copies. Placeholder replacements retain their mmap lifecycle. */
    if (ps5_direct_enabled() && private_image && !(alloc_type & MEM_REPLACE_PLACEHOLDER))
        vprot = (vprot | VPROT_PS5_DIRECT) & ~VPROT_COMMITTED;
#endif


    limit_low = max( limit_low, (ULONG_PTR)address_space_start );  /* make sure the DOS area remains free */
    if (!limit_high) limit_high = (ULONG_PTR)user_space_limit;

    /* first try the specified base */

    if (image_info->map_addr)
    {
        base = wine_server_get_ptr( image_info->map_addr );
        if ((ULONG_PTR)base != image_info->map_addr) base = NULL;
    }
    else
    {
        base = wine_server_get_ptr( image_info->base );
        if ((ULONG_PTR)base != image_info->base) base = NULL;
    }
    if (base)
    {
        status = map_view( view_ret, base, size, alloc_type, vprot, limit_low, limit_high, 0 );
        if (!status) return status;
    }

    /* then some appropriate address range */

    if (image_info->base >= limit_4g)
    {
        start = max( limit_low, limit_4g );
        end = limit_high;
    }
    else
    {
        start = limit_low;
        end = min( limit_high, get_wow_user_space_limit() );
    }
    if (start < end && (start != limit_low || end != limit_high))
    {
        status = map_view( view_ret, NULL, size, top_down ? MEM_TOP_DOWN : 0, vprot, start, end, 0 );
        if (!status) return status;
    }

    /* then any suitable address */

    return map_view( view_ret, NULL, size, top_down ? MEM_TOP_DOWN : 0, vprot, limit_low, limit_high, 0 );
}


/***********************************************************************
 *             virtual_map_image
 *
 * Map a PE image section into memory.
 */
static NTSTATUS virtual_map_image( HANDLE mapping, void **addr_ptr, SIZE_T *size_ptr, HANDLE shared_file,
                                   HANDLE layout_file,
                                   ULONG_PTR limit_low, ULONG_PTR limit_high, ULONG alloc_type,
                                   USHORT machine, struct pe_image_info *image_info,
                                   UNICODE_STRING *nt_name, BOOL is_builtin, off_t offset)
{
    int unix_fd = -1, needs_close;
    int shared_fd = -1, shared_needs_close = 0;
    int layout_fd = -1, layout_needs_close = 0;
    SIZE_T size = image_info->map_size;
    struct file_view *view;
    unsigned int status;
    sigset_t sigset;
#ifdef __PROSPERO__
    void *debug_base;
#endif

    if (offset >= size)
        return STATUS_INVALID_PARAMETER;

    if ((status = server_get_unix_fd( mapping, 0, &unix_fd, &needs_close, NULL, NULL )))
        return status;

    if (shared_file && ((status = server_get_unix_fd( shared_file, FILE_READ_DATA|FILE_WRITE_DATA,
                                                      &shared_fd, &shared_needs_close, NULL, NULL ))))
    {
        if (needs_close) close( unix_fd );
        return status;
    }
    /* without its layout, the image is read from its file as elsewhere */
    if (layout_file && server_get_unix_fd( layout_file, FILE_READ_DATA, &layout_fd, &layout_needs_close, NULL, NULL ))
        layout_fd = -1;

    if (!image_info->map_addr &&
        (image_info->image_charact & IMAGE_FILE_DLL) &&
        (image_info->image_flags & IMAGE_FLAGS_ImageDynamicallyRelocated))
    {
        SERVER_START_REQ( get_image_map_address )
        {
            req->handle = wine_server_obj_handle( mapping );
            if (!wine_server_call( req )) image_info->map_addr = reply->addr;
        }
        SERVER_END_REQ;
    }

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );

    status = map_image_view( &view, image_info, size, limit_low, limit_high, alloc_type, !shared_file );
    if (status) goto done;
#ifdef __PROSPERO__
    debug_base = view->base;
#endif


    status = map_image_into_view( view, nt_name, unix_fd, image_info, machine, shared_fd, layout_fd, needs_close );
    if (status == STATUS_SUCCESS)
    {
        if (offset)
        {
#ifdef __PROSPERO__
            if (view->protect & VPROT_PS5_DIRECT)
            {
                if ((status = free_pages( view, view->base, offset ))) goto failed;
            }
            else
#endif
                free_pages( view, view->base, offset );
            size -= offset;
        }

        image_info->base = wine_server_client_ptr( view->base );
        SERVER_START_REQ( map_image_view )
        {
            req->mapping = wine_server_obj_handle( mapping );
            req->base    = image_info->base;
            req->size    = size;
            req->entry   = image_info->entry_point;
            req->machine = image_info->machine;
            req->offset  = offset;
            status = wine_server_call( req );
        }
        SERVER_END_REQ;
    }
    if (NT_SUCCESS(status))
    {
        if (is_builtin && !offset) add_builtin_module( view->base, NULL );
        *addr_ptr = view->base;
        *size_ptr = size;
#ifdef __PROSPERO__
        if (title_profile_is_enabled()) title_map_bind( view );
#endif
        VIRTUAL_DEBUG_DUMP_VIEW( view );
    }
    else
    {
failed:
#ifdef __PROSPERO__
        /* Population adds the debug entry before server registration. Remove
         * it on rollback too, using the original base if an image offset
         * already shortened the view. */
        if (view->protect & VPROT_PS5_DIRECT) r_debug_remove_module( debug_base );
#endif
        delete_view( view );
    }

done:
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    if (needs_close) close( unix_fd );
    if (shared_needs_close) close( shared_fd );
    if (layout_needs_close) close( layout_fd );
    return status;
}


/***********************************************************************
 *             virtual_map_section
 *
 * Map a file section into memory.
 */
#ifdef __PROSPERO__
static unsigned int title_map_section( HANDLE handle, PVOID *addr_ptr, ULONG_PTR limit_low,
                                       ULONG_PTR limit_high, SIZE_T commit_size,
                                       const LARGE_INTEGER *offset_ptr, SIZE_T *size_ptr,
                                       ULONG alloc_type, ULONG protect, USHORT machine )
#else
static unsigned int virtual_map_section( HANDLE handle, PVOID *addr_ptr, ULONG_PTR limit_low,
                                         ULONG_PTR limit_high, SIZE_T commit_size,
                                         const LARGE_INTEGER *offset_ptr, SIZE_T *size_ptr,
                                         ULONG alloc_type, ULONG protect, USHORT machine )
#endif
{
    unsigned int res;
    mem_size_t full_size;
    ACCESS_MASK access;
    SIZE_T size;
    struct pe_image_info *image_info = NULL;
    UNICODE_STRING nt_name;
    ANSI_STRING exp_name;
    void *base;
    int unix_handle = -1, needs_close;
    unsigned int vprot, sec_flags;
    struct file_view *view;
    HANDLE shared_file, layout_file;
    LARGE_INTEGER offset;
    sigset_t sigset;
    BOOL shared_writable;

    switch(protect)
    {
    case PAGE_NOACCESS:
    case PAGE_READONLY:
    case PAGE_WRITECOPY:
        access = SECTION_MAP_READ;
        break;
    case PAGE_READWRITE:
        access = SECTION_MAP_WRITE;
        break;
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_WRITECOPY:
        access = SECTION_MAP_READ | SECTION_MAP_EXECUTE;
        break;
    case PAGE_EXECUTE_READWRITE:
        access = SECTION_MAP_WRITE | SECTION_MAP_EXECUTE;
        break;
    default:
        return STATUS_INVALID_PAGE_PROTECTION;
    }

    res = get_mapping_info( handle, access, &sec_flags, &full_size, &shared_file, &layout_file,
                            &image_info, &nt_name, &exp_name, &shared_writable );
    if (res) return res;
#ifdef __PROSPERO__
    if (title_profile_is_enabled())
    {
        struct ps5_title_map_sample *sample = ntdll_get_thread_data()->map_sample;
        if (sample && (image_info || (sec_flags & SEC_FILE)))
        {
            sample->file = TRUE;
            if (image_info) sample->kind = 0;
            else if (protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_WRITECOPY) sample->kind = 2;
            else if (shared_writable || protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE)
                sample->kind = 3;
            else sample->kind = 1;
        }
    }
#endif

    offset.QuadPart = offset_ptr ? offset_ptr->QuadPart : 0;

    if (image_info)
    {
        SECTION_IMAGE_INFORMATION info;
        ULONG64 prev = 0;

        if (NtCurrentTeb64())
        {
            prev = NtCurrentTeb64()->Tib.ArbitraryUserPointer;
            NtCurrentTeb64()->Tib.ArbitraryUserPointer = PtrToUlong(NtCurrentTeb()->Tib.ArbitraryUserPointer);
        }
        /* check if we can replace that mapping with the builtin */
        res = load_builtin( image_info, &nt_name, &exp_name, machine, &info,
                            addr_ptr, size_ptr, limit_low, limit_high, offset.QuadPart );
        if (res == STATUS_IMAGE_ALREADY_LOADED)
            res = virtual_map_image( handle, addr_ptr, size_ptr, shared_file, layout_file, limit_low, limit_high,
                                     alloc_type, machine, image_info, &nt_name, FALSE, offset.QuadPart );
        if (shared_file) NtClose( shared_file );
        if (layout_file) NtClose( layout_file );
        free( image_info );
        if (NtCurrentTeb64()) NtCurrentTeb64()->Tib.ArbitraryUserPointer = prev;
        return res;
    }

    base = *addr_ptr;
    if (offset.QuadPart >= full_size) return STATUS_INVALID_PARAMETER;
    if (*size_ptr)
    {
        size = *size_ptr;
        if (size > full_size - offset.QuadPart) return STATUS_INVALID_VIEW_SIZE;
    }
    else
    {
        size = full_size - offset.QuadPart;
        if (size != full_size - offset.QuadPart)  /* truncated */
        {
            WARN( "Files larger than 4Gb (%s) not supported on this platform\n",
                  wine_dbgstr_longlong(full_size) );
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (!(size = ROUND_SIZE( 0, size, page_mask ))) return STATUS_INVALID_PARAMETER;  /* wrap-around */

    get_vprot_flags( protect, &vprot, FALSE );
    vprot |= sec_flags;
    if (!(sec_flags & SEC_RESERVE)) vprot |= VPROT_COMMITTED;
#ifdef __PROSPERO__
    /* A read-only view of a writable section must see its shared writers.
     * Only WRITECOPY views, or sections with no shared-write capability, may
     * be represented by an independent direct copy. */
    if (ps5_direct_enabled() && (sec_flags & SEC_FILE) && !(vprot & VPROT_WRITE) &&
        ((vprot & VPROT_WRITECOPY) || !shared_writable) && !(alloc_type & MEM_REPLACE_PLACEHOLDER))
        vprot |= VPROT_PS5_DIRECT;
#endif


    if ((res = server_get_unix_fd( handle, 0, &unix_handle, &needs_close, NULL, NULL ))) return res;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );

#ifdef __PROSPERO__
    res = map_view( &view, base, size, alloc_type,
                    vprot & VPROT_PS5_DIRECT ? vprot & ~VPROT_COMMITTED : vprot, limit_low, limit_high, 0 );
#else
    res = map_view( &view, base, size, alloc_type, vprot, limit_low, limit_high, 0 );
#endif
    if (res) goto done;

    TRACE( "handle=%p size=%lx offset=%s\n", handle, size, wine_dbgstr_longlong(offset.QuadPart) );
#ifdef __PROSPERO__
    if (view->protect & VPROT_PS5_DIRECT)
        res = ps5_map_private_file_view( view, unix_handle, offset.QuadPart,
                                         min( (mem_size_t)size, full_size - offset.QuadPart ), vprot );
    else
#endif
        res = map_file_into_view( view, unix_handle, 0, size, offset.QuadPart, vprot, needs_close );
    if (res == STATUS_SUCCESS)
    {
        /* file mappings must always be accessible */
#ifdef __PROSPERO__
        if (!(view->protect & VPROT_PS5_DIRECT))
#endif
            mprotect_range( view->base, view->size, VPROT_COMMITTED, 0 );

        SERVER_START_REQ( map_view )
        {
            req->mapping = wine_server_obj_handle( handle );
            req->access  = access;
            req->base    = wine_server_client_ptr( view->base );
            req->size    = size;
            req->start   = offset.QuadPart;
            res = wine_server_call( req );
        }
        SERVER_END_REQ;
    }
    else ERR( "mapping %p %lx %s failed\n", view->base, size, wine_dbgstr_longlong(offset.QuadPart) );

    if (NT_SUCCESS(res))
    {
        *addr_ptr = view->base;
        *size_ptr = size;
#ifdef __PROSPERO__
        if (title_profile_is_enabled()) title_map_bind( view );
#endif
        VIRTUAL_DEBUG_DUMP_VIEW( view );
    }
    else delete_view( view );

done:
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    if (needs_close) close( unix_handle );
    TRACE("status %#x.\n", res);
    return res;
}

#ifdef __PROSPERO__
static unsigned int virtual_map_section( HANDLE handle, PVOID *addr_ptr, ULONG_PTR limit_low,
                                         ULONG_PTR limit_high, SIZE_T commit_size,
                                         const LARGE_INTEGER *offset_ptr, SIZE_T *size_ptr,
                                         ULONG alloc_type, ULONG protect, USHORT machine )
{
    struct ntdll_thread_data *data = ps5_title_profile_thread_data();
    struct ps5_title_map_sample sample, *previous;
    struct file_view *view;
    uint64_t started, ended;
    unsigned int status;
    sigset_t sigset;

    if (!data)
        return title_map_section( handle, addr_ptr, limit_low, limit_high, commit_size,
                                  offset_ptr, size_ptr, alloc_type, protect, machine );
    memset( &sample, 0, sizeof(sample) );
    /* Resolve the existing server fd name without a variable-sized reply or
     * allocation. This diagnostic lookup is outside the measured interval. */
    if (!ps5_title_mapping_name( handle, sample.name, sizeof(sample.name), &sample.id ))
        strcpy( sample.name, "unavailable" );
    previous = data->map_sample;
    data->map_sample = &sample;
    started = ps5_title_profile_now_ns();
    status = title_map_section( handle, addr_ptr, limit_low, limit_high, commit_size,
                                offset_ptr, size_ptr, alloc_type, protect, machine );
    ended = ps5_title_profile_now_ns();
    if (sample.file)
    {
        server_enter_uninterrupted_section( &virtual_mutex, &sigset );
        /* Reused builtins may not traverse virtual_map_image on this call. */
        if (NT_SUCCESS(status) && (view = find_view( *addr_ptr, 0 )) && !view->map_profile)
            view->map_profile = title_map_file_index( &sample );
        title_map_record( &sample, started, ended, NT_SUCCESS(status) ? *size_ptr : 0, NT_SUCCESS(status) );
        server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    }
    if (data->map_sample == &sample) data->map_sample = previous;
    return status;
}
#endif


/* allocate some space for the virtual heap, if possible from a reserved area */
static void *alloc_virtual_heap( SIZE_T size )
{
    struct reserved_area *area;
    void *ret;

    size = ROUND_SIZE( 0, size, host_page_mask );

    LIST_FOR_EACH_ENTRY_REV( area, &reserved_areas, struct reserved_area, entry )
    {
        void *base = area->base;
        void *end = (char *)base + area->size;

        if (is_beyond_limit( base, area->size, address_space_limit ))
            address_space_limit = host_addr_space_limit = end;
        if (is_win64 && base < (void *)0x80000000) break;
        if (preload_reserve_end >= end)
        {
            if (preload_reserve_start <= base) continue;  /* no space in that area */
            if (preload_reserve_start < end) end = preload_reserve_start;
        }
        else if (preload_reserve_end > base)
        {
            if (preload_reserve_start <= base) base = preload_reserve_end;
            else if ((char *)end - (char *)preload_reserve_end >= size) base = preload_reserve_end;
            else end = preload_reserve_start;
        }
        if ((char *)end - (char *)base < size) continue;
#ifdef __PROSPERO__
        /* reserved areas are not mapped there, see ps5_reserve_free_ranges */
        ret = anon_mmap_tryfixed( (char *)end - size, size, PROT_READ | PROT_WRITE, 0 );
#else
        ret = anon_mmap_fixed( (char *)end - size, size, PROT_READ | PROT_WRITE, 0 );
#endif
        if (ret == MAP_FAILED) continue;
        mmap_remove_reserved_area( ret, size );
        return ret;
    }
    return anon_mmap_alloc( size, PROT_READ | PROT_WRITE );
}

/***********************************************************************
 *           virtual_init
 */
void virtual_init(void)
{
    const struct preload_info **preload_info = dlsym( RTLD_DEFAULT, "wine_main_preload_info" );
    struct r_debug **r_debug = dlsym( RTLD_DEFAULT, "wine_r_debug" );
    const char *preload;
    size_t size;
    int i;
    pthread_mutexattr_t attr;

#ifdef __PROSPERO__
    ps5_title_startup_step( "reservation_begin" );
    /* Consume title-only markers before Wine builds its child environment. */
    ps5_direct_init();
#endif

    if (r_debug && (wine_r_debug = *r_debug)) r_debug_set_state( RT_CONSISTENT );

    pthread_mutexattr_init( &attr );
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
    pthread_mutex_init( &virtual_mutex, &attr );
    pthread_mutexattr_destroy( &attr );

#ifdef HOST_PAGE_SIZE_VARIES
    host_page_size = sysconf( _SC_PAGESIZE );
    host_page_mask = host_page_size - 1;
    TRACE( "host page size: %uk\n", (UINT)host_page_size / 1024 );
#endif

#ifdef _WIN64
    host_addr_space_limit = get_host_addr_space_limit();
#ifdef __PROSPERO__
    if (host_addr_space_limit > PS5_HOST_ADDR_SPACE_LIMIT)
        host_addr_space_limit = PS5_HOST_ADDR_SPACE_LIMIT;
#endif
    TRACE( "host addr space limit: %p\n", host_addr_space_limit );
#else
    host_addr_space_limit = address_space_limit;
#endif

    kernel_writewatch_init();

    if (preload_info && *preload_info)
        for (i = 0; (*preload_info)[i].size; i++)
            mmap_add_reserved_area( (*preload_info)[i].addr, (*preload_info)[i].size );

    mmap_init( preload_info ? *preload_info : NULL );
#ifdef __PROSPERO__
    /* RADV's shader-pointer window (8..12 GiB) and device region
     * (256..512 GiB) belong to the native title, not Wine's free ranges.
     * Existing Unix modules in the window are already excluded by vmmap. */
    ps5_title_radv_ranges = !!__wine_ps5_get_vulkan_proc( "vkGetInstanceProcAddr" );
    if (ps5_title_radv_ranges)
    {
        mmap_remove_reserved_area( (void *)(UINT_PTR)0x200000000, (size_t)0x100000000 );
        mmap_remove_reserved_area( (void *)(UINT_PTR)0x4000000000, (size_t)0x4000000000 );
    }
#endif

    if ((preload = getenv("WINEPRELOADRESERVE")))
    {
        unsigned long start, end;
        if (sscanf( preload, "%lx-%lx", &start, &end ) == 2)
        {
            preload_reserve_start = ROUND_ADDR( start, host_page_mask );
            preload_reserve_end = (void *)ROUND_SIZE( 0, end, host_page_mask );
            /* some apps start inside the DOS area */
            if (preload_reserve_start)
                address_space_start = min( address_space_start, preload_reserve_start );
        }
        unsetenv( "WINEPRELOADRESERVE" );
    }

    /* try to find space in a reserved area for the views and pages protection table */
#ifdef _WIN64
    pages_vprot_size = ((size_t)host_addr_space_limit >> page_shift >> pages_vprot_shift) + 1;
    size = 2 * view_block_size + pages_vprot_size * sizeof(*pages_vprot);
#else
    size = 2 * view_block_size + (1U << (32 - page_shift));
#endif
    view_block_start = alloc_virtual_heap( size );
    assert( view_block_start != MAP_FAILED );
    view_block_end = view_block_start + view_block_size / sizeof(*view_block_start);
    free_ranges = (void *)((char *)view_block_start + view_block_size);
    pages_vprot = (void *)((char *)view_block_start + 2 * view_block_size);
    wine_rb_init( &views_tree, compare_view );

    free_ranges[0].base = (void *)0;
    free_ranges[0].end = (void *)~0;
    free_ranges_end = free_ranges + 1;

    /* make the DOS area accessible (except the low 64K) to hide bugs in broken apps like Excel 2003 */
    size = (char *)address_space_start - (char *)0x10000;
#ifdef __PROSPERO__
    /* not mapped there (ps5_reserve_free_ranges): take it only if still free */
    if (size && mmap_is_in_reserved_area( (void*)0x10000, size ) == 1)
        anon_mmap_tryfixed( (void *)0x10000, size, PROT_READ | PROT_WRITE, 0 );
#else
    if (size && mmap_is_in_reserved_area( (void*)0x10000, size ) == 1)
        anon_mmap_fixed( (void *)0x10000, size, PROT_READ | PROT_WRITE, 0 );
#endif
#ifdef __PROSPERO__
    ps5_direct_test();  /* views and vprot tables are ready; still before server connection */
    if (__atomic_load_n( &ps5_title_startup_active, __ATOMIC_RELAXED ))
    {
        struct reserved_area *area;
        uint64_t bytes = 0;
        char line[256];

        LIST_FOR_EACH_ENTRY( area, &reserved_areas, struct reserved_area, entry ) bytes += area->size;
        snprintf( line, sizeof(line),
                  "reservation_end host_map=%llu bytes_reserved=%llu reservation_kind=bookkeeping",
                  (unsigned long long)title_host.count[PS5_TITLE_HOST_MAP], (unsigned long long)bytes );
        ps5_title_startup_step( line );
    }
#endif
}


/***********************************************************************
 *           get_system_affinity_mask
 */
ULONG_PTR get_system_affinity_mask(void)
{
    ULONG num_cpus = peb->NumberOfProcessors;
    if (num_cpus >= sizeof(ULONG_PTR) * 8) return ~(ULONG_PTR)0;
    return ((ULONG_PTR)1 << num_cpus) - 1;
}

/***********************************************************************
 *           virtual_get_system_info
 */
void virtual_get_system_info( SYSTEM_BASIC_INFORMATION *info, BOOL wow64 )
{
#ifdef __PROSPERO__
    ULONGLONG total;

    ps5_get_memory_info( &total, NULL );
    info->MmHighestPhysicalPage = max( 1, total / page_size ) - 1;
#elif defined(HAVE_SYSINFO) \
    && defined(HAVE_STRUCT_SYSINFO_TOTALRAM) && defined(HAVE_STRUCT_SYSINFO_MEM_UNIT)
    struct sysinfo sinfo;

    if (!sysinfo(&sinfo))
    {
        ULONG64 total = (ULONG64)sinfo.totalram * sinfo.mem_unit;
        total -= min(total, ram_reporting_bias);
        info->MmHighestPhysicalPage = max(1, total / page_size);
    }
#elif defined(__APPLE__)
    /* sysconf(_SC_PHYS_PAGES) is buggy on macOS: in a 32-bit process, it
     * returns an error on Macs with >4GB of RAM.
     */
    INT64 memsize;
    size_t len = sizeof(memsize);

    if (!sysctlbyname( "hw.memsize", &memsize, &len, NULL, 0 ))
        info->MmHighestPhysicalPage = max(1, memsize / page_size);
#elif defined(_SC_PHYS_PAGES)
    LONG64 phys_pages = sysconf( _SC_PHYS_PAGES );

    info->MmHighestPhysicalPage = max(1, phys_pages);
#else
    info->MmHighestPhysicalPage = 0x7fffffff / page_size;
#endif

    info->unknown                 = 0;
    info->KeMaximumIncrement      = 0;  /* FIXME */
    info->PageSize                = page_size;
#ifdef __PROSPERO__
    info->MmLowestPhysicalPage    = 0;
    info->MmNumberOfPhysicalPages = total / page_size;
#else
    info->MmLowestPhysicalPage    = 1;
    info->MmNumberOfPhysicalPages = info->MmHighestPhysicalPage - info->MmLowestPhysicalPage;
#endif
    info->AllocationGranularity   = granularity_mask + 1;
    info->LowestUserAddress       = (void *)0x10000;
    info->ActiveProcessorsAffinityMask = get_system_affinity_mask();
    info->NumberOfProcessors      = peb->NumberOfProcessors;
    if (wow64) info->HighestUserAddress = (char *)get_wow_user_space_limit() - 1;
    else info->HighestUserAddress = (char *)user_space_limit - 1;
}


/***********************************************************************
 *           virtual_map_builtin_module
 */
NTSTATUS virtual_map_builtin_module( HANDLE mapping, void **module, SIZE_T *size,
                                     SECTION_IMAGE_INFORMATION *info, ULONG_PTR limit_low,
                                     ULONG_PTR limit_high, WORD machine, BOOL prefer_native, off_t offset )
{
    mem_size_t full_size;
    unsigned int sec_flags;
    HANDLE shared_file, layout_file;
    struct pe_image_info *image_info = NULL;
    NTSTATUS status;
    UNICODE_STRING nt_name;
    ANSI_STRING exp_name;

    if ((status = get_mapping_info( mapping, SECTION_MAP_READ, &sec_flags, &full_size, &shared_file, &layout_file,
                                    &image_info, &nt_name, &exp_name, NULL )))
        return status;

    if (!image_info) return STATUS_INVALID_PARAMETER;

    *module = NULL;
    *size = 0;

    if (!image_info->wine_builtin) /* ignore non-builtins */
    {
        if (!image_info->wine_fakedll)
            WARN_(module)( "%s found in WINEDLLPATH but not a builtin, ignoring\n", debugstr_us(&nt_name) );
        status = STATUS_DLL_NOT_FOUND;
    }
    else if (prefer_native && (image_info->dll_charact & IMAGE_DLLCHARACTERISTICS_PREFER_NATIVE))
    {
        TRACE_(module)( "%s has prefer-native flag, ignoring builtin\n", debugstr_us(&nt_name) );
        status = STATUS_IMAGE_ALREADY_LOADED;
    }
    else
    {
        status = virtual_map_image( mapping, module, size, shared_file, layout_file, limit_low, limit_high, 0,
                                    machine, image_info, &nt_name, TRUE, offset );
        virtual_fill_image_information( image_info, info );
    }

    if (shared_file) NtClose( shared_file );
    if (layout_file) NtClose( layout_file );
    free( image_info );
    return status;
}


/***********************************************************************
 *           virtual_map_module
 */
NTSTATUS virtual_map_module( HANDLE mapping, void **module, SIZE_T *size, SECTION_IMAGE_INFORMATION *info,
                             ULONG_PTR limit_low, ULONG_PTR limit_high, USHORT machine )
{
    unsigned int status;
    mem_size_t full_size;
    unsigned int sec_flags;
    HANDLE shared_file, layout_file;
    struct pe_image_info *image_info = NULL;
    UNICODE_STRING nt_name;
    ANSI_STRING exp_name;

    if ((status = get_mapping_info( mapping, SECTION_MAP_READ, &sec_flags, &full_size, &shared_file, &layout_file,
                                    &image_info, &nt_name, &exp_name, NULL )))
        return status;

    if (!image_info) return STATUS_INVALID_PARAMETER;

    *module = NULL;
    *size = 0;

    /* check if we can replace that mapping with the builtin */
    status = load_builtin( image_info, &nt_name, &exp_name, machine, info,
                           module, size, limit_low, limit_high, 0 );
    if (status == STATUS_IMAGE_ALREADY_LOADED)
    {
        status = virtual_map_image( mapping, module, size, shared_file, layout_file, limit_low, limit_high, 0,
                                    machine, image_info, &nt_name, FALSE, 0 );
        virtual_fill_image_information( image_info, info );
    }
    if (shared_file) NtClose( shared_file );
    if (layout_file) NtClose( layout_file );
    free( image_info );
    return status;
}


/***********************************************************************
 *           virtual_create_builtin_view
 */
NTSTATUS virtual_create_builtin_view( void *module, const UNICODE_STRING *nt_name,
                                      struct pe_image_info *info, void *so_handle )
{
    NTSTATUS status;
    sigset_t sigset;
    IMAGE_DOS_HEADER *dos = module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)dos + dos->e_lfanew);
    SIZE_T size = info->map_size;
    IMAGE_SECTION_HEADER *sec;
    struct file_view *view;
    void *base = wine_server_get_ptr( info->base );
    int i;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    status = create_view( &view, base, size, SEC_IMAGE | SEC_FILE | VPROT_SYSTEM |
                          VPROT_COMMITTED | VPROT_READ | VPROT_WRITECOPY | VPROT_EXEC );
    if (!status)
    {
        TRACE( "created %p-%p for %s\n", base, (char *)base + size, debugstr_us(nt_name) );

        /* The PE header is always read-only, no write, no execute. */
        set_page_vprot( base, page_size, VPROT_COMMITTED | VPROT_READ );

        sec = IMAGE_FIRST_SECTION( nt );
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            BYTE flags = VPROT_COMMITTED;

            if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) flags |= VPROT_EXEC;
            if (sec[i].Characteristics & IMAGE_SCN_MEM_READ) flags |= VPROT_READ;
            if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) flags |= VPROT_WRITE;
            set_page_vprot( (char *)base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, flags );
        }

        SERVER_START_REQ( map_builtin_view )
        {
            wine_server_add_data( req, info, sizeof(*info) );
            wine_server_add_data( req, nt_name->Buffer, nt_name->Length );
            status = wine_server_call( req );
        }
        SERVER_END_REQ;

        if (!status)
        {
            add_builtin_module( view->base, so_handle );
            VIRTUAL_DEBUG_DUMP_VIEW( view );
            if (is_beyond_limit( base, size, working_set_limit )) working_set_limit = address_space_limit;
        }
        else delete_view( view );
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );

    return status;
}


/***********************************************************************
 *           virtual_relocate_module
 */
NTSTATUS virtual_relocate_module( void *module )
{
    char *ptr = module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(ptr + ((IMAGE_DOS_HEADER *)module)->e_lfanew);
    IMAGE_DATA_DIRECTORY *relocs;
    IMAGE_BASE_RELOCATION *rel, *end;
    IMAGE_SECTION_HEADER *sec;
    ULONG total_size = ROUND_SIZE( 0, nt->OptionalHeader.SizeOfImage, page_mask );
    ULONG *protect_old, i;
    ULONG_PTR image_base;
    INT_PTR delta;

    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        image_base = ((const IMAGE_NT_HEADERS64 *)nt)->OptionalHeader.ImageBase;
    else
        image_base = ((const IMAGE_NT_HEADERS32 *)nt)->OptionalHeader.ImageBase;


    if (!(delta = (ULONG_PTR)module - image_base)) return STATUS_SUCCESS;

    if (nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)
    {
        ERR( "Need to relocate module from %p to %p, but relocation records are stripped\n",
             (void *)image_base, module );
        return STATUS_CONFLICTING_ADDRESSES;
    }

    TRACE( "%p -> %p\n", (void *)image_base, module );

    if (!(relocs = get_data_dir( nt, total_size, IMAGE_DIRECTORY_ENTRY_BASERELOC ))) return STATUS_SUCCESS;

    if (!(protect_old = malloc( nt->FileHeader.NumberOfSections * sizeof(*protect_old ))))
        return STATUS_NO_MEMORY;

    sec = IMAGE_FIRST_SECTION( nt );
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        void *addr = (char *)module + sec[i].VirtualAddress;
        SIZE_T size = sec[i].SizeOfRawData;
        NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, PAGE_READWRITE, &protect_old[i] );
    }


    rel = (IMAGE_BASE_RELOCATION *)((char *)module + relocs->VirtualAddress);
    end = (IMAGE_BASE_RELOCATION *)((char *)rel + relocs->Size);

    while (rel && rel < end - 1 && rel->SizeOfBlock && rel->VirtualAddress < total_size)
        rel = process_relocation_block( (char *)module + rel->VirtualAddress, rel, delta );

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        void *addr = (char *)module + sec[i].VirtualAddress;
        SIZE_T size = sec[i].SizeOfRawData;
        NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, protect_old[i], &protect_old[i] );
    }
    free( protect_old );
    return STATUS_SUCCESS;
}


/* set some initial values in a new TEB */
static TEB *init_teb( void *ptr, BOOL is_wow )
{
    struct ntdll_thread_data *thread_data;
    TEB *teb;
    TEB64 *teb64 = ptr;
    TEB32 *teb32 = (TEB32 *)((char *)ptr + teb_offset);

#ifdef _WIN64
    teb = (TEB *)teb64;
    teb32->Peb = PtrToUlong( (char *)peb + page_size );
    teb32->Tib.Self = PtrToUlong( teb32 );
    teb32->Tib.ExceptionList = ~0u;
    teb32->ActivationContextStackPointer = PtrToUlong( &teb32->ActivationContextStack );
    teb32->ActivationContextStack.FrameListCache.Flink =
        teb32->ActivationContextStack.FrameListCache.Blink =
            PtrToUlong( &teb32->ActivationContextStack.FrameListCache );
    teb32->StaticUnicodeString.Buffer = PtrToUlong( teb32->StaticUnicodeBuffer );
    teb32->StaticUnicodeString.MaximumLength = sizeof( teb32->StaticUnicodeBuffer );
    teb32->GdiBatchCount = PtrToUlong( teb64 );
    teb32->WowTebOffset  = -teb_offset;
    if (is_wow) teb64->WowTebOffset = teb_offset;
#else
    teb = (TEB *)teb32;
    teb32->Tib.ExceptionList = ~0u;
    teb64->Peb = PtrToUlong( (char *)peb - page_size );
    teb64->Tib.Self = PtrToUlong( teb64 );
    teb64->Tib.ExceptionList = PtrToUlong( teb32 );
    teb64->ActivationContextStackPointer = PtrToUlong( &teb64->ActivationContextStack );
    teb64->ActivationContextStack.FrameListCache.Flink =
        teb64->ActivationContextStack.FrameListCache.Blink =
            PtrToUlong( &teb64->ActivationContextStack.FrameListCache );
    teb64->StaticUnicodeString.Buffer = PtrToUlong( teb64->StaticUnicodeBuffer );
    teb64->StaticUnicodeString.MaximumLength = sizeof( teb64->StaticUnicodeBuffer );
    teb64->WowTebOffset = teb_offset;
    if (is_wow)
    {
        teb32->GdiBatchCount = PtrToUlong( teb64 );
        teb32->WowTebOffset  = -teb_offset;
    }
#endif
    teb->Peb = peb;
    teb->Tib.Self = &teb->Tib;
    teb->Tib.StackBase = (void *)~0ul;
    teb->ActivationContextStackPointer = &teb->ActivationContextStack;
    InitializeListHead( &teb->ActivationContextStack.FrameListCache );
    teb->StaticUnicodeString.Buffer = teb->StaticUnicodeBuffer;
    teb->StaticUnicodeString.MaximumLength = sizeof(teb->StaticUnicodeBuffer);
    thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    thread_data->request_fd = -1;
    thread_data->reply_fd   = -1;
    thread_data->wait_fd[0] = -1;
    thread_data->wait_fd[1] = -1;
    thread_data->alert_fd   = -1;
    list_add_head( &teb_list, &thread_data->entry );
    thread_data->fsync_apc_futex = NULL;
    return teb;
}


/***********************************************************************
 *           virtual_alloc_first_teb
 */
TEB *virtual_alloc_first_teb(void)
{
    void *ptr;
    TEB *teb;
    unsigned int status;
    SIZE_T data_size = page_size;
    SIZE_T block_size = signal_stack_mask + 1;
    SIZE_T total = 32 * block_size;
#ifdef __PROSPERO__
    struct file_view *view;
    sigset_t sigset;
#endif

    /* reserve space for shared user data */
#ifdef __PROSPERO__
    if (ps5_direct_enabled())
    {
        /* This bootstrap is replaced by a shared section, and shares its
         * 64 KiB unit with the separately mapped syscall dispatcher page.
         * Keep both on the existing mmap path, not an anonymous direct view. */
        server_enter_uninterrupted_section( &virtual_mutex, &sigset );
        status = map_view( &view, user_shared_data, data_size, 0, VPROT_READ | VPROT_COMMITTED,
                           0, 0, granularity_mask );
        server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    }
    else
#endif
    status = NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&user_shared_data, 0, &data_size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READONLY );
    if (status)
    {
        ERR( "wine: failed to map the shared user data: %08x\n", status );
        exit(1);
    }

    NtAllocateVirtualMemory( NtCurrentProcess(), &teb_block, is_win64 ? limit_2g - 1 : 0, &total,
                             MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE );
    teb_block_pos = 30;
    ptr = (char *)teb_block + 30 * block_size;
    data_size = 2 * block_size;
    NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&ptr, 0, &data_size, MEM_COMMIT, PAGE_READWRITE );
    peb = (PEB *)((char *)teb_block + 31 * block_size + (is_win64 ? 0 : page_size));
    teb = init_teb( ptr, FALSE );
#ifdef __PROSPERO__
    ps5_tls_key_create( &teb_key );
    ps5_tls_set( teb_key, teb );
#else
    pthread_key_create( &teb_key, NULL );
    pthread_setspecific( teb_key, teb );
#endif
    return teb;
}


/***********************************************************************
 *           virtual_alloc_teb
 */
NTSTATUS virtual_alloc_teb( TEB **ret_teb )
{
    sigset_t sigset;
    TEB *teb;
    void *ptr = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    SIZE_T block_size = signal_stack_mask + 1;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (next_free_teb)
    {
        ptr = next_free_teb;
        next_free_teb = *(void **)ptr;
        memset( ptr, 0, teb_size );
    }
    else
    {
        if (!teb_block_pos)
        {
            SIZE_T total = 32 * block_size;

            if ((status = NtAllocateVirtualMemory( NtCurrentProcess(), &ptr, user_space_wow_limit,
                                                   &total, MEM_RESERVE, PAGE_READWRITE )))
            {
                server_leave_uninterrupted_section( &virtual_mutex, &sigset );
                return status;
            }
            teb_block = ptr;
            teb_block_pos = 32;
        }
        ptr = ((char *)teb_block + --teb_block_pos * block_size);
        NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&ptr, 0, &block_size,
                                 MEM_COMMIT, PAGE_READWRITE );
    }
    *ret_teb = teb = init_teb( ptr, is_wow64() );

    if ((status = signal_alloc_thread( teb )))
    {
        *(void **)ptr = next_free_teb;
        next_free_teb = ptr;
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return status;
}


/***********************************************************************
 *           virtual_free_teb
 */
void virtual_free_teb( TEB *teb )
{
    struct ntdll_thread_data *thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    void *ptr;
    SIZE_T size;
    sigset_t sigset;
    WOW_TEB *wow_teb = get_wow_teb( teb );

    if (teb->DeallocationStack)
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &teb->DeallocationStack, &size, MEM_RELEASE );
    }
#ifdef __aarch64__
    if (teb->ChpeV2CpuAreaInfo)
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), (void **)&teb->ChpeV2CpuAreaInfo, &size, MEM_RELEASE );
    }
#endif
    if (thread_data->kernel_stack)
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &thread_data->kernel_stack, &size, MEM_RELEASE );
    }
    if (wow_teb && (ptr = ULongToPtr( wow_teb->DeallocationStack )))
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &ptr, &size, MEM_RELEASE );
    }

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    signal_free_thread( teb );
    list_remove( &thread_data->entry );
    ptr = teb;
    if (!is_win64) ptr = (char *)ptr - teb_offset;
    *(void **)ptr = next_free_teb;
    next_free_teb = ptr;
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
}


/* LDT support */

#if defined(__i386__) || defined(__x86_64__)

struct ldt_copy
{
    unsigned int    base[LDT_SIZE];
    struct ldt_bits bits[LDT_SIZE];
};
C_ASSERT( sizeof(struct ldt_copy) == 8 * LDT_SIZE );

static struct ldt_copy *ldt_copy;

UINT ldt_bitmap[LDT_SIZE / 32] = { ~0u };

/***********************************************************************
 *           ldt_update_entry
 */
WORD ldt_update_entry( WORD sel, LDT_ENTRY entry )
{
    unsigned int index = sel >> 3;

    if (!ldt_copy)
    {
        struct file_view *view;

        if (map_view( &view, NULL, sizeof(*ldt_copy), MEM_TOP_DOWN,
                      VPROT_COMMITTED | VPROT_READ | VPROT_WRITE,
                      is_win64 ? limit_2g : 0, limit_4g, 0 )) return 0;
        ldt_copy = view->base;
        if (is_win64) wow_peb->SpareUlongs[0] = PtrToUlong( ldt_copy );
        else peb->SpareUlongs[0] = PtrToUlong( ldt_copy );
    }

    ldt_set_entry( sel, entry );
    ldt_copy->base[index]             = ldt_get_base( entry );
    ldt_copy->bits[index].limit       = entry.LimitLow | (entry.HighWord.Bits.LimitHi << 16);
    ldt_copy->bits[index].type        = entry.HighWord.Bits.Type;
    ldt_copy->bits[index].granularity = entry.HighWord.Bits.Granularity;
    ldt_copy->bits[index].default_big = entry.HighWord.Bits.Default_Big;
    ldt_bitmap[index / 32] |= 1u << (index & 31);
    return sel;
}

/***********************************************************************
 *           ldt_get_entry
 */
NTSTATUS ldt_get_entry( WORD sel, CLIENT_ID client_id, LDT_ENTRY *entry )
{
    NTSTATUS status = STATUS_SUCCESS;
    unsigned int base = 0;
    struct ldt_bits bits = { 0 };
    unsigned int idx = sel >> 3;

    if (client_id.UniqueProcess == NtCurrentTeb()->ClientId.UniqueProcess)
    {
        if (ldt_copy)
        {
            base = ldt_copy->base[idx];
            bits = ldt_copy->bits[idx];
        }
    }
    else
    {
        HANDLE process;
        ULONG ptr = 0;
        PEB32 *peb32 = NULL;

        if ((status = NtOpenProcess( &process, PROCESS_ALL_ACCESS, NULL, &client_id ))) return status;

        if (!is_win64)
        {
            PROCESS_BASIC_INFORMATION pbi;

            NtQueryInformationProcess( process, ProcessBasicInformation, &pbi, sizeof(pbi), NULL );
            peb32 = (PEB32 *)pbi.PebBaseAddress;
        }
        else NtQueryInformationProcess( process, ProcessWow64Information, &peb32, sizeof(peb32), NULL );

        if (!NtReadVirtualMemory( process, &peb32->SpareUlongs[0], &ptr, sizeof(ptr), NULL ) && ptr)
        {
            struct ldt_copy *ldt = ULongToPtr( ptr );
            NtReadVirtualMemory( process, &ldt->base[idx], &base, sizeof(base), NULL );
            NtReadVirtualMemory( process, &ldt->bits[idx], &bits, sizeof(bits), NULL );
        }
        NtClose( process );
    }

    if (base || bits.limit || bits.type) *entry = ldt_make_entry( base, bits );
    else status = STATUS_UNSUCCESSFUL;

    return status;
}

/******************************************************************************
 *           NtSetLdtEntries   (NTDLL.@)
 *           ZwSetLdtEntries   (NTDLL.@)
 */
NTSTATUS WINAPI NtSetLdtEntries( ULONG sel1, LDT_ENTRY entry1, ULONG sel2, LDT_ENTRY entry2 )
{
    sigset_t sigset;

    if (is_win64 && !is_wow64()) return STATUS_NOT_IMPLEMENTED;
    if (sel1 >> 16 || sel2 >> 16) return STATUS_INVALID_LDT_DESCRIPTOR;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (sel1) ldt_update_entry( sel1, entry1 );
    if (sel2) ldt_update_entry( sel2, entry2 );
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return STATUS_SUCCESS;
}

#else /* defined(__i386__) || defined(__x86_64__) */

/******************************************************************************
 *           NtSetLdtEntries   (NTDLL.@)
 *           ZwSetLdtEntries   (NTDLL.@)
 */
NTSTATUS WINAPI NtSetLdtEntries( ULONG sel1, LDT_ENTRY entry1, ULONG sel2, LDT_ENTRY entry2 )
{
    return STATUS_NOT_IMPLEMENTED;
}

#endif /* defined(__i386__) || defined(__x86_64__) */


/***********************************************************************
 *           virtual_clear_tls_index
 */
NTSTATUS virtual_clear_tls_index( ULONG index )
{
    struct ntdll_thread_data *thread_data;
    sigset_t sigset;

    if (index < TLS_MINIMUM_AVAILABLE)
    {
        server_enter_uninterrupted_section( &virtual_mutex, &sigset );
        LIST_FOR_EACH_ENTRY( thread_data, &teb_list, struct ntdll_thread_data, entry )
        {
            TEB *teb = CONTAINING_RECORD( thread_data, TEB, GdiTebBatch );
#ifdef _WIN64
            WOW_TEB *wow_teb = get_wow_teb( teb );
            if (wow_teb) wow_teb->TlsSlots[index] = 0;
            else
#endif
            teb->TlsSlots[index] = 0;
        }
        server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    }
    else
    {
        index -= TLS_MINIMUM_AVAILABLE;
        if (index >= 8 * sizeof(peb->TlsExpansionBitmapBits)) return STATUS_INVALID_PARAMETER;

        server_enter_uninterrupted_section( &virtual_mutex, &sigset );
        LIST_FOR_EACH_ENTRY( thread_data, &teb_list, struct ntdll_thread_data, entry )
        {
            TEB *teb = CONTAINING_RECORD( thread_data, TEB, GdiTebBatch );
#ifdef _WIN64
            WOW_TEB *wow_teb = get_wow_teb( teb );
            if (wow_teb)
            {
                if (wow_teb->TlsExpansionSlots)
                    ((ULONG *)ULongToPtr( wow_teb->TlsExpansionSlots ))[index] = 0;
            }
            else
#endif
            if (teb->TlsExpansionSlots) teb->TlsExpansionSlots[index] = 0;
        }
        server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           virtual_set_tls_information_teb
 */
static NTSTATUS virtual_set_tls_information_teb( PROCESS_TLS_INFORMATION *t, unsigned int *idx, TEB *teb )
{
    __TRY
    {
#ifdef _WIN64
        if (t->Flags & PROCESS_TLS_INFORMATION_WOW64)
        {
            WOW_TEB *wow_teb = get_wow_teb( teb );
            ULONG *ptr;

            if (wow_teb && wow_teb->ThreadLocalStoragePointer)
            {
                if (t->OperationType == ProcessTlsReplaceVector)
                {
                    ptr = t->ThreadData[*idx].TlsVector;
                    memcpy( ptr, ULongToPtr( wow_teb->ThreadLocalStoragePointer ), sizeof(*ptr) * t->TlsVectorLength );
                    t->ThreadData[*idx].TlsVector = ULongToPtr( InterlockedExchange( (LONG *)&wow_teb->ThreadLocalStoragePointer, PtrToLong( ptr )));
                    t->ThreadData[*idx].ThreadId = wow_teb->ClientId.UniqueThread;
                }
                else
                {
                    ptr = ULongToPtr( wow_teb->ThreadLocalStoragePointer );
                    t->ThreadData[*idx].TlsModulePointer =
                            ULongToPtr( InterlockedExchange( (LONG *)&ptr[t->TlsIndex],
                                        PtrToLong( t->ThreadData[*idx].TlsModulePointer )));
                }
                t->ThreadData[*idx].Flags = THREAD_TLS_INFORMATION_ASSIGNED;
                ++*idx;
            }
        }
        else
#endif
        if (teb->ThreadLocalStoragePointer)
        {
            void **ptr;

            if (t->OperationType == ProcessTlsReplaceVector)
            {
                ptr = t->ThreadData[*idx].TlsVector;
                memcpy( ptr, teb->ThreadLocalStoragePointer, sizeof(*ptr) * t->TlsVectorLength );
                t->ThreadData[*idx].TlsVector = InterlockedExchangePointer( &teb->ThreadLocalStoragePointer, ptr );
                t->ThreadData[*idx].ThreadId = HandleToULong( teb->ClientId.UniqueThread );
#ifdef __x86_64__  /* macOS-specific hack */
                if (teb->Instrumentation[0]) ((TEB *)teb->Instrumentation[0])->ThreadLocalStoragePointer = ptr;
#endif
            }
            else
            {
                ptr = teb->ThreadLocalStoragePointer;
                t->ThreadData[*idx].TlsModulePointer = InterlockedExchangePointer( &ptr[t->TlsIndex],
                                                                                   t->ThreadData[*idx].TlsModulePointer );
            }
            t->ThreadData[*idx].Flags = THREAD_TLS_INFORMATION_ASSIGNED;
            ++*idx;
        }
    }
    __EXCEPT
    {
        return STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY

    return STATUS_SUCCESS;
}


/***********************************************************************
 *           virtual_set_tls_information
 */
NTSTATUS virtual_set_tls_information( PROCESS_TLS_INFORMATION *t )
{
    struct ntdll_thread_data *thread_data;
    NTSTATUS ret = STATUS_SUCCESS;
    unsigned int idx = 0;
    sigset_t sigset;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    LIST_FOR_EACH_ENTRY_REV( thread_data, &teb_list, struct ntdll_thread_data, entry )
    {
        TEB *teb = CONTAINING_RECORD( thread_data, TEB, GdiTebBatch );

        if (idx == t->ThreadDataCount) break;
        if ((ret = virtual_set_tls_information_teb( t, &idx, teb ))) break;
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return ret;
}


/***********************************************************************
 *           virtual_alloc_thread_stack
 */
NTSTATUS virtual_alloc_thread_stack( INITIAL_TEB *stack, ULONG_PTR limit_low, ULONG_PTR limit_high,
                                     SIZE_T reserve_size, SIZE_T commit_size, BOOL guard_page )
{
    struct file_view *view;
    NTSTATUS status;
    sigset_t sigset;
    SIZE_T size;
    unsigned int vprot = VPROT_READ | VPROT_WRITE | VPROT_COMMITTED;

#ifdef __PROSPERO__
    /* Each thread's stack (1 MiB at least, committed) would otherwise come
     * from the title's flexible memory, which the PsmKit children of a
     * Chromium client exhausted (mmap ENOMEM for exactly these 1 MiB views).
     * A committed direct view is backed when mapped, so the guard-page
     * changes below and in grow_thread_stack only change protection. */
    if (ps5_direct_enabled()) vprot |= VPROT_PS5_DIRECT;
#endif

    if (!reserve_size) reserve_size = main_image_info.MaximumStackSize;
    if (!commit_size) commit_size = main_image_info.CommittedStackSize;

    size = max( reserve_size, commit_size );
    if (size < 1024 * 1024) size = 1024 * 1024;  /* Xlib needs a large stack */
    size = ROUND_SIZE( 0, size, granularity_mask );

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );

    status = map_view( &view, NULL, size, 0, vprot, limit_low, limit_high, 0 );
    if (status != STATUS_SUCCESS) goto done;

#ifdef VALGRIND_STACK_REGISTER
    VALGRIND_STACK_REGISTER( view->base, (char *)view->base + view->size );
#endif

    /* setup no access guard page */
    if (guard_page)
    {
        set_page_vprot( view->base, host_page_size, 0 );
        set_page_vprot( (char *)view->base + host_page_size, host_page_size,
                        VPROT_READ | VPROT_WRITE | VPROT_COMMITTED | VPROT_GUARD );
        mprotect_range( view->base, 2 * host_page_size , 0, 0 );
    }
    else
    {
        /* setup kernel stack no access guard page */
        set_page_vprot( view->base, kernel_stack_guard_size, VPROT_COMMITTED | VPROT_READ );
        mprotect_range( view->base, kernel_stack_guard_size, 0, 0 );
    }
    VIRTUAL_DEBUG_DUMP_VIEW( view );

    /* note: limit is lower than base since the stack grows down */
    stack->OldStackBase = 0;
    stack->OldStackLimit = 0;
    stack->DeallocationStack = view->base;
    stack->StackBase = (char *)view->base + view->size;
    stack->StackLimit = (char *)view->base + (guard_page ? 2 * host_page_size : 0);
done:
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return status;
}


static const WCHAR shared_data_nameW[] = {'\\','K','e','r','n','e','l','O','b','j','e','c','t','s',
                                          '\\','_','_','w','i','n','e','_','u','s','e','r','_','s','h','a','r','e','d','_','d','a','t','a',0};

/***********************************************************************
 *           virtual_map_user_shared_data
 */
void virtual_map_user_shared_data(void)
{
    UNICODE_STRING name_str = RTL_CONSTANT_STRING( shared_data_nameW );
    OBJECT_ATTRIBUTES attr = { sizeof(attr), 0, &name_str };
    unsigned int status;
    HANDLE section;
    int res, fd, needs_close;

    if ((status = NtOpenSection( &section, SECTION_ALL_ACCESS, &attr )))
    {
        ERR( "failed to open the USD section: %08x\n", status );
        exit(1);
    }
    if ((res = server_get_unix_fd( section, 0, &fd, &needs_close, NULL, NULL )) ||
        (user_shared_data != mmap( user_shared_data, page_size, PROT_READ, MAP_SHARED|MAP_FIXED, fd, 0 )))
    {
        ERR( "failed to remap the process USD: %d\n", res );
        exit(1);
    }
    if (needs_close) close( fd );
    NtClose( section );
}


/******************************************************************
 *		virtual_init_user_shared_data
 *
 * Initialize user shared data before running wineboot.
 */
void virtual_init_user_shared_data(void)
{
    UNICODE_STRING name_str = RTL_CONSTANT_STRING( shared_data_nameW );
    OBJECT_ATTRIBUTES attr = { sizeof(attr), 0, &name_str };
    SYSTEM_BASIC_INFORMATION info;
    KUSER_SHARED_DATA *data;
    unsigned int status;
    HANDLE section;
    int res, fd, needs_close;

    if ((status = NtOpenSection( &section, SECTION_ALL_ACCESS, &attr )))
    {
        ERR( "failed to open the USD section: %08x\n", status );
        exit(1);
    }
    if ((res = server_get_unix_fd( section, 0, &fd, &needs_close, NULL, NULL )) ||
        (data = mmap( NULL, sizeof(*data), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 )) == MAP_FAILED)
    {
        ERR( "failed to remap the process USD: %d\n", res );
        exit(1);
    }
    if (needs_close) close( fd );
    NtClose( section );

    virtual_get_system_info( &info, FALSE );

    data->TickCountMultiplier   = 1 << 24;
    data->LargePageMinimum      = 2 * 1024 * 1024;
    data->SystemCall            = 1;
    data->NumberOfPhysicalPages = info.MmNumberOfPhysicalPages;
    data->NXSupportPolicy       = NX_SUPPORT_POLICY_OPTIN;
    data->ActiveProcessorCount  = peb->NumberOfProcessors;
    data->ActiveGroupCount      = 1;

    switch (native_machine)
    {
    case IMAGE_FILE_MACHINE_I386:  data->NativeProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL; break;
    case IMAGE_FILE_MACHINE_AMD64: data->NativeProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64; break;
    case IMAGE_FILE_MACHINE_ARMNT: data->NativeProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM; break;
    case IMAGE_FILE_MACHINE_ARM64: data->NativeProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64; break;
    }

    init_shared_data_cpuinfo( data );
    munmap( data, sizeof(*data) );
}


struct thread_stack_info
{
    char  *start;
    char  *limit;
    char  *end;
    SIZE_T guaranteed;
    BOOL   is_wow;
};

/***********************************************************************
 *           is_inside_thread_stack
 */
static BOOL is_inside_thread_stack( void *ptr, struct thread_stack_info *stack )
{
    TEB *teb = NtCurrentTeb();
    WOW_TEB *wow_teb = get_wow_teb( teb );
    size_t min_guaranteed = max( page_size * (is_win64 ? 2 : 1), host_page_size );

    stack->start = teb->DeallocationStack;
    stack->limit = teb->Tib.StackLimit;
    stack->end   = teb->Tib.StackBase;
    stack->guaranteed = max( teb->GuaranteedStackBytes, min_guaranteed );
    stack->is_wow = FALSE;
    if ((char *)ptr > stack->start && (char *)ptr <= stack->end) return TRUE;

    if (!wow_teb) return FALSE;
    stack->start = ULongToPtr( wow_teb->DeallocationStack );
    stack->limit = ULongToPtr( wow_teb->Tib.StackLimit );
    stack->end   = ULongToPtr( wow_teb->Tib.StackBase );
    stack->guaranteed = max( wow_teb->GuaranteedStackBytes, min_guaranteed );
    stack->is_wow = TRUE;
    return ((char *)ptr > stack->start && (char *)ptr <= stack->end);
}


/***********************************************************************
 *           grow_thread_stack
 */
static NTSTATUS grow_thread_stack( char *page, struct thread_stack_info *stack_info )
{
    NTSTATUS ret = 0;

    set_page_vprot_bits( page, host_page_size, VPROT_COMMITTED, VPROT_GUARD );
    mprotect_range( page, host_page_size, 0, 0 );
    if (page >= stack_info->start + host_page_size + stack_info->guaranteed)
    {
        set_page_vprot_bits( page - host_page_size, host_page_size, VPROT_COMMITTED | VPROT_GUARD, 0 );
        mprotect_range( page - host_page_size, host_page_size, 0, 0 );
    }
    else  /* inside guaranteed space -> overflow exception */
    {
        page = stack_info->start + host_page_size;
        set_page_vprot_bits( page, stack_info->guaranteed, VPROT_COMMITTED, VPROT_GUARD );
        mprotect_range( page, stack_info->guaranteed, 0, 0 );
        ret = STATUS_STACK_OVERFLOW;
    }
    if (stack_info->is_wow)
    {
        WOW_TEB *wow_teb = get_wow_teb( NtCurrentTeb() );
        wow_teb->Tib.StackLimit = PtrToUlong( page );
    }
    else NtCurrentTeb()->Tib.StackLimit = page;
    return ret;
}


/***********************************************************************
 *           virtual_handle_fault
 */
NTSTATUS virtual_handle_fault( EXCEPTION_RECORD *rec, void *stack )
{
    NTSTATUS ret = STATUS_ACCESS_VIOLATION;
    ULONG_PTR err = rec->ExceptionInformation[0];
    void *addr = (void *)rec->ExceptionInformation[1];
    char *page = ROUND_ADDR( addr, host_page_mask );
    BYTE vprot;

    mutex_lock( &virtual_mutex );  /* no need for signal masking inside signal handler */
    vprot = get_host_page_vprot( page );

#ifdef __APPLE__
    /* Rosetta on Apple Silicon misreports certain write faults as read faults. */
    if (err == EXCEPTION_READ_FAULT && (get_unix_prot( vprot ) & PROT_READ))
    {
        WARN( "treating read fault in a readable page as a write fault, addr %p\n", addr );
        err = EXCEPTION_WRITE_FAULT;
    }
#endif

#ifdef __PROSPERO__
    if (__wine_ps5_is_title() && host_page_size != page_size)
    {
        struct ntdll_thread_data *data = ntdll_get_thread_data();
        BYTE own = get_page_vprot( addr );
        struct file_view *view;

        close_opened_host_pages( page );
        if (err != EXCEPTION_WRITE_FAULT && (own & VPROT_COMMITTED) && !(own & VPROT_GUARD) &&
            (own & (err == EXCEPTION_EXECUTE_FAULT ? VPROT_EXEC : VPROT_READ | VPROT_EXEC)))
        {
            if (is_host_page_closed( page ))
            {
                char *beside = page;
                const char *why = NULL;

                while (beside < page + host_page_size)
                {
                    BYTE other = get_page_vprot( beside );
                    if ((other & VPROT_COMMITTED) && !(other & (VPROT_READ | VPROT_EXEC | VPROT_GUARD))) break;
                    beside += page_size;
                }
                /* Wine cannot call the program's handler recursively for a
                 * data access it makes. A program handler may likewise need
                 * readable image metadata while decrypting this neighbour.
                 * Those temporary data openings are safe only while NX.
                 *
                 * An execute access is different: opening the host page RX
                 * exposes every still-sealed Windows page to every thread.
                 * Always synthesize the neighbour fault first, even from a
                 * nested program handler; never run encrypted bytes merely to
                 * avoid recursive exception dispatch. */
                if (is_inside_signal_stack( stack ) || is_inside_syscall( (ULONG_PTR)stack ))
                    why = "from Wine";
                else if (data->ahead_page == beside && (char *)stack < (char *)data->ahead_stack)
                    why = "from the program's handler";
                else if (data->ahead_page == beside && data->ahead_stack == stack && ++data->ahead_count >= 8)
                    why = "its handler leaves as it is";

                if (why && wine_ps5_title_sealed_may_open( err == EXCEPTION_EXECUTE_FAULT ))
                {
                    int unix_prot = wine_ps5_title_sealed_open_prot( get_unix_prot( vprot ) );

                    TRACE( "opening the host page of %p for a data access to %p %s, prot %#x\n",
                           beside, addr, why, unix_prot );
                    if (!mprotect_exec( page, host_page_size, unix_prot ))
                    {
                        opened_host_pages[opened_host_pages_next++ % ARRAY_SIZE(opened_host_pages)] = page;
                        ret = STATUS_SUCCESS;
                    }
                }
                else
                {
                    if (why)
                        ERR( "title xo diagnostic=temporary-exec host=%p sealed=%p access=%p stack=%p "
                             "reason=%s action=synthetic-neighbour-fault\n",
                             page, beside, addr, stack, why );
                    if (data->ahead_page != beside || data->ahead_stack != stack) data->ahead_count = 0;
                    data->ahead_page = beside;
                    data->ahead_stack = stack;
                    TRACE( "access to %p reported as a read of the inaccessible page %p\n", addr, beside );
                    rec->ExceptionInformation[0] = EXCEPTION_READ_FAULT;
                    rec->ExceptionInformation[1] = (ULONG_PTR)beside;
                }
                mutex_unlock( &virtual_mutex );
                rec->ExceptionCode = ret;
                return ret;
            }
            /* Another thread may have opened the neighbour before we acquired
             * the mutex. Retry the allowed access, only for views we seal. */
            if ((view = find_view( addr, 0 )) && is_sealed_view( view ))
            {
                TRACE( "access to %p refused while its host page was closed, allowed now\n", addr );
                if (!mprotect_exec( page, host_page_size, get_unix_prot( vprot ) ))
                    ret = STATUS_SUCCESS;
                mutex_unlock( &virtual_mutex );
                rec->ExceptionCode = ret;
                return ret;
            }
        }
    }
#endif

    if (!is_inside_signal_stack( stack ) && (vprot & VPROT_GUARD))
    {
        struct thread_stack_info stack_info;
        if (!is_inside_thread_stack( page, &stack_info ))
        {
            set_page_vprot_bits( page, host_page_size, 0, VPROT_GUARD );
            mprotect_range( page, host_page_size, 0, 0 );
            ret = STATUS_GUARD_PAGE_VIOLATION;
        }
        else ret = grow_thread_stack( page, &stack_info );
    }
    else if (err == EXCEPTION_WRITE_FAULT)
    {
        if (vprot & VPROT_WRITEWATCH)
        {
            if (enable_write_exceptions && is_vprot_exec_write( vprot ) && !ntdll_get_thread_data()->allow_writes)
            {
                rec->NumberParameters = 3;
                rec->ExceptionInformation[2] = STATUS_EXECUTABLE_MEMORY_WRITE;
                ret = STATUS_IN_PAGE_ERROR;
            }
            else
            {
                set_page_vprot_bits( page, host_page_size, 0, VPROT_WRITEWATCH );
                mprotect_range( page, host_page_size, 0, 0 );
            }
        }
        /* ignore fault if page is writable now */
        if (get_unix_prot( get_host_page_vprot( page )) & PROT_WRITE)
        {
            if ((vprot & VPROT_WRITEWATCH) || is_write_watch_range( page, 1 ))
                ret = STATUS_SUCCESS;
#ifdef __PROSPERO__
            else
            {
                /* The console sometimes faults on writes to a page mprotect
                 * made writable: zero-filled tails of writable image sections
                 * (Battle.net-Setup.exe, then Battle.net's Update Agent writing
                 * its .data tail, 2026-10-03). Reapplying the protection lets
                 * the write through; a page that keeps faulting gets the
                 * access violation. */
                static char *last_page;
                static unsigned int retries;

                if (page != last_page)
                {
                    last_page = page;
                    retries = 0;
                }
                if (retries++ < 2)
                {
                    WARN( "write fault on writable page %p (vprot %#x), protection reapplied\n", page, vprot );
                    if (!mprotect_range( page, host_page_size, 0, 0 )) ret = STATUS_SUCCESS;
                }
            }
#endif
        }
    }
    mutex_unlock( &virtual_mutex );
    rec->ExceptionCode = ret;
    return ret;
}


/***********************************************************************
 *           virtual_setup_exception
 */
void *virtual_setup_exception( void *stack_ptr, size_t size, EXCEPTION_RECORD *rec )
{
    char *stack = stack_ptr;
    struct thread_stack_info stack_info;

    if (!is_inside_thread_stack( stack, &stack_info ))
    {
        if (is_inside_signal_stack( stack ))
        {
            ERR( "nested exception on signal stack addr %p stack %p\n", rec->ExceptionAddress, stack );
            abort_thread(1);
        }
        WARN( "exception outside of stack limits addr %p stack %p (%p-%p-%p)\n",
              rec->ExceptionAddress, stack, NtCurrentTeb()->DeallocationStack,
              NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
        return stack - size;
    }

    stack -= size;

    if (stack < stack_info.start + host_page_size)
    {
        /* stack overflow on last page, unrecoverable */
        UINT diff = stack_info.start + host_page_size - stack;
        ERR( "stack overflow %u bytes addr %p stack %p (%p-%p-%p)\n",
             diff, rec->ExceptionAddress, stack, stack_info.start, stack_info.limit, stack_info.end );
        abort_thread(1);
    }
    else if (stack < stack_info.limit)
    {
        char *page = ROUND_ADDR( stack, host_page_mask );
        mutex_lock( &virtual_mutex );  /* no need for signal masking inside signal handler */
        if ((get_host_page_vprot( page ) & VPROT_GUARD) && grow_thread_stack( page, &stack_info ))
        {
            rec->ExceptionCode = STATUS_STACK_OVERFLOW;
            rec->NumberParameters = 0;
        }
        mutex_unlock( &virtual_mutex );
    }
#if defined(VALGRIND_MAKE_MEM_UNDEFINED)
    VALGRIND_MAKE_MEM_UNDEFINED( stack, size );
#elif defined(VALGRIND_MAKE_WRITABLE)
    VALGRIND_MAKE_WRITABLE( stack, size );
#endif
    return stack;
}


/***********************************************************************
 *           check_write_access
 *
 * Check if the memory range is writable, temporarily disabling write watches if necessary.
 */
static NTSTATUS check_write_access( void *base, size_t size, BOOL *has_write_watch )
{
    size_t i;
    char *addr = ROUND_ADDR( base, host_page_mask );

    size = ROUND_SIZE( base, size, host_page_mask );
    for (i = 0; i < size; i += host_page_size)
    {
        BYTE vprot = get_host_page_vprot( addr + i );
        if (vprot & VPROT_WRITEWATCH) *has_write_watch = TRUE;
        if (!(get_unix_prot( vprot & ~VPROT_WRITEWATCH ) & PROT_WRITE))
            return STATUS_INVALID_USER_BUFFER;
    }
    if (*has_write_watch)
        mprotect_range( addr, size, 0, VPROT_WRITEWATCH );  /* temporarily enable write access */
    return STATUS_SUCCESS;
}

#ifdef __PROSPERO__
/* Caller holds virtual_mutex. Unlike get_host_page_vprot(), this checks every
 * Windows page. A readable page in a logically closed 16-KiB host page is not
 * touched: ReadProcessMemory must report the boundary instead of entering the
 * sealed-page fault/decryption path. */
static SIZE_T ps5_title_accessible_prefix( const void *ptr, SIZE_T size, BOOL write )
{
    const char *current = ptr;
    struct file_view *view = NULL;
    char *checked_host = NULL;
    BOOL host_accessible = TRUE, host_closed = FALSE;
    SIZE_T prefix = 0;

    if (!size || !ptr || (UINT_PTR)ptr > ~(UINT_PTR)0 - size) return 0;
    while (prefix < size)
    {
        BYTE vprot = get_page_vprot( current );
        BOOL accessible;
        SIZE_T block;

        if (!view || current >= (char *)view->base + view->size)
        {
            if (!(view = find_view( current, 1 )) || (view->protect & VPROT_SYSTEM)) break;
        }
        accessible = write ? vprot & (VPROT_WRITE | VPROT_WRITECOPY) : vprot & VPROT_READ;
        if (!write)
        {
            char *host = ROUND_ADDR( current, host_page_mask );

            if (host != checked_host)
            {
                checked_host = host;
                host_accessible = get_unix_prot( get_host_page_vprot( host )) & PROT_READ;
                host_closed = is_host_page_closed( host );
            }
        }
        block = wine_ps5_title_copy_page_size( (UINT_PTR)current, size - prefix, page_mask,
                                               vprot & VPROT_COMMITTED, vprot & VPROT_GUARD,
                                               accessible, host_accessible, host_closed );
        if (!block) break;
        current += block;
        prefix += block;
    }
    return prefix;
}

/* Keep the fault guard in the same live Windows-thread frame as the copy. An
 * unexpected host fault then returns to the caller, which still owns and
 * releases virtual_mutex, rather than dispatching to user mode with the lock
 * retained. The private server has no Windows TEB or signal dispatcher; its
 * callback uses the same complete prevalidation but cannot arm a Wine guard. */
static BOOL ps5_title_memmove_locked( void *dest, const void *src, SIZE_T size )
{
    BOOL copied = TRUE;

    if (!size) return TRUE;
    if (!ps5_title_current_thread_id())
    {
        memmove( dest, src, size );
        return TRUE;
    }
    if (ntdll_get_thread_data()->jmp_buf) return FALSE;
    __TRY
    {
        memmove( dest, src, size );
    }
    __EXCEPT
    {
        copied = FALSE;
    }
    __ENDTRY
    return copied;
}

static BOOL ps5_title_store_size_locked( SIZE_T *dest, SIZE_T value )
{
    BOOL has_write_watch = FALSE;
    BOOL stored;

    if (!dest) return TRUE;
    if (ps5_title_accessible_prefix( dest, sizeof(value), TRUE ) != sizeof(value) ||
        check_write_access( dest, sizeof(value), &has_write_watch ))
        return FALSE;
    stored = ps5_title_memmove_locked( dest, &value, sizeof(value) );
    if (has_write_watch)
        update_write_watches( dest, sizeof(value), stored ? sizeof(value) : 0 );
    return stored;
}

static NTSTATUS ps5_title_read_self_memory( const void *addr, void *buffer, SIZE_T size,
                                            SIZE_T *bytes_read )
{
    BOOL has_write_watch = FALSE;
    NTSTATUS status;
    sigset_t sigset;
    SIZE_T copied = 0;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (!size)
        status = STATUS_SUCCESS;
    /* Windows validates its local output buffer before reading the target. */
    else if (ps5_title_accessible_prefix( buffer, size, TRUE ) != size ||
             check_write_access( buffer, size, &has_write_watch ))
        status = STATUS_ACCESS_VIOLATION;
    else
    {
        copied = ps5_title_accessible_prefix( addr, size, FALSE );
        if (copied && !ps5_title_memmove_locked( buffer, addr, copied )) copied = 0;
        if (has_write_watch) update_write_watches( buffer, size, copied );
        status = copied == size ? STATUS_SUCCESS : STATUS_PARTIAL_COPY;
    }
    if (!ps5_title_store_size_locked( bytes_read, copied )) status = STATUS_ACCESS_VIOLATION;
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return status;
}

static NTSTATUS ps5_title_write_self_memory( void *addr, const void *buffer, SIZE_T size,
                                             SIZE_T *bytes_written )
{
    BOOL has_write_watch = FALSE;
    NTSTATUS status;
    sigset_t sigset;
    SIZE_T copied = 0, writable = 0;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (!size)
        status = STATUS_SUCCESS;
    /* The source is local to this process and must be wholly readable before
     * the target is changed, matching the existing probe-first ordering. */
    else if (ps5_title_accessible_prefix( buffer, size, FALSE ) != size)
        status = STATUS_PARTIAL_COPY;
    else
    {
        writable = ps5_title_accessible_prefix( addr, size, TRUE );
        copied = writable;
        if (copied && check_write_access( addr, copied, &has_write_watch )) copied = 0;
        else if (copied && !ps5_title_memmove_locked( addr, buffer, copied )) copied = 0;
        if (has_write_watch) update_write_watches( addr, writable, copied );
        status = copied == size ? STATUS_SUCCESS : STATUS_PARTIAL_COPY;
    }
    if (!ps5_title_store_size_locked( bytes_written, copied )) status = STATUS_ACCESS_VIOLATION;
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return status;
}
#endif


/***********************************************************************
 *           virtual_locked_server_call
 */
unsigned int virtual_locked_server_call( void *req_ptr )
{
    struct __server_request_info * const req = req_ptr;
    sigset_t sigset;
    void *addr = req->reply_data;
    data_size_t size = req->u.req.request_header.reply_size;
    BOOL has_write_watch = FALSE;
    unsigned int ret;

    if (!size) return wine_server_call( req_ptr );

#ifdef __PROSPERO__
    if (__wine_ps5_is_title())
    {
        server_enter_uninterrupted_section( &virtual_mutex, &sigset );
        ret = check_write_access( addr, size, &has_write_watch );
        server_leave_uninterrupted_section( &virtual_mutex, &sigset );
        if (ret)
        {
            memset( &req->u.reply, 0, sizeof(req->u.reply) );
            return ret;
        }
        ret = wine_server_call( req );
        if (has_write_watch)
        {
            server_enter_uninterrupted_section( &virtual_mutex, &sigset );
            update_write_watches( addr, size, wine_server_reply_size( req ));
            server_leave_uninterrupted_section( &virtual_mutex, &sigset );
        }
        return ret;
    }
#endif

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (!(ret = check_write_access( addr, size, &has_write_watch )))
    {
        ret = server_call_unlocked( req );
        if (has_write_watch) update_write_watches( addr, size, wine_server_reply_size( req ));
    }
    else memset( &req->u.reply, 0, sizeof(req->u.reply) );
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return ret;
}


/***********************************************************************
 *           virtual_locked_read
 */
ssize_t virtual_locked_read( int fd, void *addr, size_t size )
{
    sigset_t sigset;
    BOOL has_write_watch = FALSE;
    int err = EFAULT;

#ifdef __PROSPERO__
    ssize_t ret = ps5_title_profile_native_read( fd, addr, size );
#else
    ssize_t ret = read( fd, addr, size );
#endif
    if (ret != -1 || use_kernel_writewatch || errno != EFAULT) return ret;

#ifdef __PROSPERO__
    if (__wine_ps5_is_title())
    {
        server_enter_uninterrupted_section( &virtual_mutex, &sigset );
        err = check_write_access( addr, size, &has_write_watch ) ? EFAULT : 0;
        server_leave_uninterrupted_section( &virtual_mutex, &sigset );
        if (!err)
        {
            ret = ps5_title_profile_native_read( fd, addr, size );
            err = errno;
        }
        if (has_write_watch)
        {
            server_enter_uninterrupted_section( &virtual_mutex, &sigset );
            update_write_watches( addr, size, max( 0, ret ));
            server_leave_uninterrupted_section( &virtual_mutex, &sigset );
        }
        errno = err;
        return ret;
    }
#endif

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (!check_write_access( addr, size, &has_write_watch ))
    {
#ifdef __PROSPERO__
        ret = ps5_title_profile_native_read( fd, addr, size );
#else
        ret = read( fd, addr, size );
#endif
        err = errno;
        if (has_write_watch) update_write_watches( addr, size, max( 0, ret ));
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    errno = err;
    return ret;
}


/***********************************************************************
 *           virtual_locked_pread
 */
ssize_t virtual_locked_pread( int fd, void *addr, size_t size, off_t offset )
{
    sigset_t sigset;
    BOOL has_write_watch = FALSE;
    int err = EFAULT;

#ifdef __PROSPERO__
    ssize_t ret = ps5_title_profile_native_pread( fd, addr, size, offset );
#else
    ssize_t ret = pread( fd, addr, size, offset );
#endif
    if (ret != -1 || use_kernel_writewatch || errno != EFAULT) return ret;

#ifdef __PROSPERO__
    if (__wine_ps5_is_title())
    {
        server_enter_uninterrupted_section( &virtual_mutex, &sigset );
        err = check_write_access( addr, size, &has_write_watch ) ? EFAULT : 0;
        server_leave_uninterrupted_section( &virtual_mutex, &sigset );
        if (!err)
        {
            ret = ps5_title_profile_native_pread( fd, addr, size, offset );
            err = errno;
        }
        if (has_write_watch)
        {
            server_enter_uninterrupted_section( &virtual_mutex, &sigset );
            update_write_watches( addr, size, max( 0, ret ));
            server_leave_uninterrupted_section( &virtual_mutex, &sigset );
        }
        errno = err;
        return ret;
    }
#endif

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (!check_write_access( addr, size, &has_write_watch ))
    {
#ifdef __PROSPERO__
        ret = ps5_title_profile_native_pread( fd, addr, size, offset );
#else
        ret = pread( fd, addr, size, offset );
#endif
        err = errno;
        if (has_write_watch) update_write_watches( addr, size, max( 0, ret ));
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    errno = err;
    return ret;
}


/***********************************************************************
 *           virtual_locked_recvmsg
 */
ssize_t virtual_locked_recvmsg( int fd, struct msghdr *hdr, int flags )
{
    sigset_t sigset;
    size_t i;
    BOOL has_write_watch = FALSE;
    int err = EFAULT;

    ssize_t ret = recvmsg( fd, hdr, flags );
    if (ret != -1 || use_kernel_writewatch || errno != EFAULT) return ret;

#ifdef __PROSPERO__
    if (__wine_ps5_is_title())
    {
        server_enter_uninterrupted_section( &virtual_mutex, &sigset );
        for (i = 0; i < hdr->msg_iovlen; i++)
            if (check_write_access( hdr->msg_iov[i].iov_base, hdr->msg_iov[i].iov_len,
                                    &has_write_watch ))
                break;
        server_leave_uninterrupted_section( &virtual_mutex, &sigset );
        if (i == hdr->msg_iovlen)
        {
            ret = recvmsg( fd, hdr, flags );
            err = errno;
        }
        if (has_write_watch)
        {
            server_enter_uninterrupted_section( &virtual_mutex, &sigset );
            while (i--)
                update_write_watches( hdr->msg_iov[i].iov_base, hdr->msg_iov[i].iov_len, 0 );
            server_leave_uninterrupted_section( &virtual_mutex, &sigset );
        }
        errno = err;
        return ret;
    }
#endif

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    for (i = 0; i < hdr->msg_iovlen; i++)
        if (check_write_access( hdr->msg_iov[i].iov_base, hdr->msg_iov[i].iov_len, &has_write_watch ))
            break;
    if (i == hdr->msg_iovlen)
    {
        ret = recvmsg( fd, hdr, flags );
        err = errno;
    }
    if (has_write_watch)
        while (i--) update_write_watches( hdr->msg_iov[i].iov_base, hdr->msg_iov[i].iov_len, 0 );

    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    errno = err;
    return ret;
}


/***********************************************************************
 *           virtual_is_valid_code_address
 */
BOOL virtual_is_valid_code_address( const void *addr, SIZE_T size )
{
    struct file_view *view;
    BOOL ret = FALSE;
    sigset_t sigset;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if ((view = find_view( addr, size )))
        ret = !(view->protect & VPROT_SYSTEM);  /* system views are not visible to the app */
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return ret;
}


/***********************************************************************
 *           virtual_check_buffer_for_read
 *
 * Check if a memory buffer can be read, triggering page faults if needed for DIB section access.
 */
BOOL virtual_check_buffer_for_read( const void *ptr, SIZE_T size )
{
    if (!size) return TRUE;
    if (!ptr) return FALSE;

    __TRY
    {
        volatile const char *p = ptr;
        char dummy __attribute__((unused));
        SIZE_T count = size;

        while (count > host_page_size)
        {
            dummy = *p;
            p += host_page_size;
            count -= host_page_size;
        }
        dummy = p[0];
        dummy = p[count - 1];
    }
    __EXCEPT
    {
        return FALSE;
    }
    __ENDTRY
    return TRUE;
}


/***********************************************************************
 *           virtual_check_buffer_for_write
 *
 * Check if a memory buffer can be written to, triggering page faults if needed for write watches.
 */
BOOL virtual_check_buffer_for_write( void *ptr, SIZE_T size )
{
    if (!size) return TRUE;
    if (!ptr) return FALSE;

    __TRY
    {
        volatile char *p = ptr;
        SIZE_T count = size;

        while (count > host_page_size)
        {
            *p |= 0;
            p += host_page_size;
            count -= host_page_size;
        }
        p[0] |= 0;
        p[count - 1] |= 0;
    }
    __EXCEPT
    {
        return FALSE;
    }
    __ENDTRY
    return TRUE;
}


/***********************************************************************
 *           virtual_uninterrupted_read_memory
 *
 * Similar to NtReadVirtualMemory, but without wineserver calls. Moreover
 * permissions are checked before accessing each page, to ensure that no
 * exceptions can happen.
 */
SIZE_T virtual_uninterrupted_read_memory( const void *addr, void *buffer, SIZE_T size )
{
    struct file_view *view;
    sigset_t sigset;
    SIZE_T bytes_read = 0;

    if (!size) return 0;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
#ifdef __PROSPERO__
    if (__wine_ps5_is_title())
    {
        bytes_read = ps5_title_accessible_prefix( addr, size, FALSE );
        if (bytes_read && !ps5_title_memmove_locked( buffer, addr, bytes_read )) bytes_read = 0;
    }
    else
#endif
    if ((view = find_view( addr, size )))
    {
        if (!(view->protect & VPROT_SYSTEM))
        {
            while (bytes_read < size && (get_unix_prot( get_host_page_vprot( addr )) & PROT_READ))
            {
                SIZE_T block_size = min( size - bytes_read, host_page_size - ((UINT_PTR)addr & host_page_mask) );
                memcpy( buffer, addr, block_size );

                addr   = (const void *)((const char *)addr + block_size);
                buffer = (void *)((char *)buffer + block_size);
                bytes_read += block_size;
            }
        }
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return bytes_read;
}


/***********************************************************************
 *           virtual_uninterrupted_write_memory
 *
 * Similar to NtWriteVirtualMemory, but without wineserver calls. Moreover
 * permissions are checked before accessing each page, to ensure that no
 * exceptions can happen.
 */
NTSTATUS virtual_uninterrupted_write_memory( void *addr, const void *buffer, SIZE_T size )
{
    BOOL has_write_watch = FALSE;
    sigset_t sigset;
    NTSTATUS ret;

    if (!size) return STATUS_SUCCESS;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
#ifdef __PROSPERO__
    if (__wine_ps5_is_title())
    {
        if (ps5_title_accessible_prefix( addr, size, TRUE ) != size)
            ret = STATUS_INVALID_USER_BUFFER;
        else if (!(ret = check_write_access( addr, size, &has_write_watch )))
        {
            if (!ps5_title_memmove_locked( addr, buffer, size )) ret = STATUS_PARTIAL_COPY;
            if (has_write_watch) update_write_watches( addr, size, ret ? 0 : size );
        }
    }
    else
#endif
    if (!(ret = check_write_access( addr, size, &has_write_watch )))
    {
        memcpy( addr, buffer, size );
        if (has_write_watch) update_write_watches( addr, size, size );
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return ret;
}


/***********************************************************************
 *           virtual_set_force_exec
 *
 * Whether to force exec prot on all views.
 */
void virtual_set_force_exec( BOOL enable )
{
    struct file_view *view;
    sigset_t sigset;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (!force_exec_prot != !enable)  /* change all existing views */
    {
        force_exec_prot = enable;

        WINE_RB_FOR_EACH_ENTRY( view, &views_tree, struct file_view, entry )
        {
            /* file mappings are always accessible */
            BYTE commit;

            if (view->protect & VPROT_NATIVE)
                continue;

            commit = is_view_valloc( view ) ? 0 : VPROT_COMMITTED;

            mprotect_range( view->base, view->size, commit, 0 );
        }
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
}


/***********************************************************************
 *           virtual_manage_exec_writes
 */
void virtual_enable_write_exceptions( BOOL enable )
{
    struct file_view *view;
    sigset_t sigset;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (!enable_write_exceptions && enable)  /* change all existing views */
    {
        WINE_RB_FOR_EACH_ENTRY( view, &views_tree, struct file_view, entry )
            if (set_page_vprot_exec_write_protect( view->base, view->size ))
                mprotect_range( view->base, view->size, 0, 0 );
    }
    enable_write_exceptions = enable;
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
}


/* free reserved areas within a given range */
static void free_reserved_memory( char *base, char *limit )
{
    struct reserved_area *area;
#ifdef __PROSPERO__
    if (!title_direct_cache_evict_range( base, (UINT_PTR)limit - (UINT_PTR)base )) return;
#endif

    for (;;)
    {
        int removed = 0;

        LIST_FOR_EACH_ENTRY( area, &reserved_areas, struct reserved_area, entry )
        {
            char *area_base = area->base;
            char *area_end = area_base + area->size;

            if (area_end <= base) continue;
            if (area_base >= limit) return;
            if (area_base < base) area_base = base;
            if (area_end > limit) area_end = limit;
            remove_reserved_area( area_base, area_end - area_base );
            removed = 1;
            break;
        }
        if (!removed) return;
    }
}

#ifndef _WIN64

/***********************************************************************
 *           virtual_release_address_space
 *
 * Release some address space once we have loaded and initialized the app.
 */
static void virtual_release_address_space(void)
{
#ifndef __APPLE__  /* On macOS, we still want to free some of low memory, for OpenGL resources */
    if (user_space_limit > (void *)limit_2g && !release_reserved_memory_low_bound) return;
#endif
    if (release_reserved_memory_low_bound)
        ERR( "HACK: release_reserved_memory_low_bound %p.\n", release_reserved_memory_low_bound );
    free_reserved_memory( release_reserved_memory_low_bound ? release_reserved_memory_low_bound : (char *)0x20000000,
                          (char *)0x7f000000 );
}

#endif  /* _WIN64 */

static int need_override_large_address_aware(void)
{
    static int needs_override = -1;

    if (needs_override == -1)
    {
        const char *str = getenv( "WINE_LARGE_ADDRESS_AWARE" );

        needs_override = !str /* on by default */ || atoi(str) == 1;
    }
    return needs_override;
}

static BOOL is_large_address_aware(void)
{
    return (main_image_info.ImageCharacteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE)
           || need_override_large_address_aware();
}

/***********************************************************************
 *           virtual_set_large_address_space
 *
 * Enable use of a large address space when allowed by the application.
 */
void virtual_set_large_address_space(void)
{
    if (is_win64)
    {
        if (!is_wow64())
        {
            address_space_start = (void *)0x10000;
#ifndef __APPLE__  /* don't free the zerofill section on macOS */
            if ((main_image_info.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA) &&
                (main_image_info.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE))
                free_reserved_memory( 0, (char *)0x7ffe0000 );
#endif
        }
        else user_space_wow_limit = (is_large_address_aware() ? limit_4g : limit_2g) - 1;
    }
    else
    {
        if (!is_large_address_aware()) return;
        free_reserved_memory( (char *)0x80000000, address_space_limit );
    }
    user_space_limit = working_set_limit = address_space_limit;
}


/***********************************************************************
 *             allocate_virtual_memory
 *
 * NtAllocateVirtualMemory[Ex] implementation.
 */
#ifdef __PROSPERO__
static NTSTATUS title_allocate_virtual_memory( void **ret, SIZE_T *size_ptr, ULONG type, ULONG protect,
                                               ULONG_PTR limit_low, ULONG_PTR limit_high,
                                               ULONG_PTR align, ULONG attributes )
#else
static NTSTATUS allocate_virtual_memory( void **ret, SIZE_T *size_ptr, ULONG type, ULONG protect,
                                         ULONG_PTR limit_low, ULONG_PTR limit_high,
                                         ULONG_PTR align, ULONG attributes )
#endif
{
    void *base;
    unsigned int vprot;
    BOOL is_dos_memory = FALSE;
    struct file_view *view;
    sigset_t sigset;
    SIZE_T size = *size_ptr;
    NTSTATUS status = STATUS_SUCCESS;

    /* Round parameters to a page boundary */

    if (is_beyond_limit( 0, size, working_set_limit )) return STATUS_WORKING_SET_LIMIT_RANGE;

    if (*ret)
    {
        if (type & MEM_RESERVE && !(type & MEM_REPLACE_PLACEHOLDER)) /* Round down to 64k boundary */
            base = ROUND_ADDR( *ret, granularity_mask );
        else
            base = ROUND_ADDR( *ret, page_mask );
        size = (((UINT_PTR)*ret + size + page_mask) & ~page_mask) - (UINT_PTR)base;

        /* disallow low 64k, wrap-around and kernel space */
        if (((char *)base < (char *)0x10000) ||
            ((char *)base + size < (char *)base) ||
            is_beyond_limit( base, size, address_space_limit ))
        {
            /* address 1 is magic to mean DOS area */
            if (!base && *ret == (void *)1 && size == 0x110000) is_dos_memory = TRUE;
            else return STATUS_INVALID_PARAMETER;
        }
    }
    else
    {
        base = NULL;
        size = ROUND_SIZE( 0, size, page_mask );
    }

    /* Compute the alloc type flags */

    if (!(type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET))
        || (type & MEM_REPLACE_PLACEHOLDER && !(type & MEM_RESERVE)))
    {
        WARN("called with wrong alloc type flags (%08x) !\n", type);
        return STATUS_INVALID_PARAMETER;
    }

    if (type & MEM_RESERVE_PLACEHOLDER && (protect != PAGE_NOACCESS)) return STATUS_INVALID_PARAMETER;
    if (!arm64ec_view && (attributes & MEM_EXTENDED_PARAMETER_EC_CODE)) return STATUS_INVALID_PARAMETER;

    /* Reserve the memory */

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );

    if ((type & MEM_RESERVE) || !base)
    {
        if (!(status = get_vprot_flags( protect, &vprot, FALSE )))
        {
            if (type & MEM_COMMIT) vprot |= VPROT_COMMITTED;
            if (type & MEM_WRITE_WATCH) vprot |= VPROT_WRITEWATCH;
            if (type & MEM_RESERVE_PLACEHOLDER) vprot |= VPROT_PLACEHOLDER | VPROT_FREE_PLACEHOLDER;
            if (protect & PAGE_NOCACHE) vprot |= SEC_NOCACHE;
#ifdef __PROSPERO__
            if (ps5_direct_enabled() && !is_dos_memory &&
                !(type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER)))
                vprot |= VPROT_PS5_DIRECT;
#endif

            if (vprot & VPROT_WRITECOPY) status = STATUS_INVALID_PAGE_PROTECTION;
            else if (is_dos_memory) status = allocate_dos_memory( &view, vprot );
            else status = map_view( &view, base, size, type, vprot, limit_low, limit_high,
                                    align ? align - 1 : granularity_mask );

            if (status == STATUS_SUCCESS)
            {
                base = view->base;
                if (vprot & VPROT_EXEC || force_exec_prot) mprotect_range( base, size, 0, 0 );
            }
        }
    }
    else if (type & MEM_RESET)
    {
        if (!(view = find_view( base, size ))) status = STATUS_NOT_MAPPED_VIEW;
        else madvise( base, size, MADV_DONTNEED );
    }
    else  /* commit the pages */
    {
        if (!(view = find_view( base, size ))) status = STATUS_NOT_MAPPED_VIEW;
        else if (view->protect & SEC_FILE) status = STATUS_ALREADY_COMMITTED;
        else if (view->protect & VPROT_FREE_PLACEHOLDER) status = STATUS_CONFLICTING_ADDRESSES;
        else if (!(status = set_protection( view, base, size, protect )) && (view->protect & SEC_RESERVE))
        {
            SERVER_START_REQ( add_mapping_committed_range )
            {
                req->base   = wine_server_client_ptr( view->base );
                req->offset = (char *)base - (char *)view->base;
                req->size   = size;
                wine_server_call( req );
            }
            SERVER_END_REQ;
        }
    }

    if (!status && (attributes & MEM_EXTENDED_PARAMETER_EC_CODE))
    {
        commit_arm64ec_map( view );
        set_arm64ec_range( base, size );
    }

    if (!status)
    {
        VIRTUAL_DEBUG_DUMP_VIEW( view );
        dump_memory_statistics();
    }

    server_leave_uninterrupted_section( &virtual_mutex, &sigset );

    if (status == STATUS_SUCCESS)
    {
        *ret = base;
        *size_ptr = size;
    }
    else if (status == STATUS_NO_MEMORY)
        ERR( "out of memory for allocation, base %p size %08lx\n", base, size );

    return status;
}

#ifdef __PROSPERO__
static NTSTATUS allocate_virtual_memory( void **ret, SIZE_T *size_ptr, ULONG type, ULONG protect,
                                         ULONG_PTR limit_low, ULONG_PTR limit_high,
                                         ULONG_PTR align, ULONG attributes )
{
    struct title_vm_cpu_scope scope;
    uint64_t bytes;
    NTSTATUS status;

    if (!title_profile_is_enabled())
        return title_allocate_virtual_memory( ret, size_ptr, type, protect,
                                               limit_low, limit_high, align, attributes );
    scope = title_vm_cpu_begin();
    bytes = *size_ptr;
    status = title_allocate_virtual_memory( ret, size_ptr, type, protect,
                                            limit_low, limit_high, align, attributes );

    title_vm_cpu_end( TITLE_VM_ALLOCATE, scope, bytes, status );
    return status;
}
#endif



/***********************************************************************
 *             NtAllocateVirtualMemory   (NTDLL.@)
 *             ZwAllocateVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtAllocateVirtualMemory( HANDLE process, PVOID *ret, ULONG_PTR zero_bits,
                                         SIZE_T *size_ptr, ULONG type, ULONG protect )
{
    static const ULONG type_mask = MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_WRITE_WATCH | MEM_RESET;
    ULONG_PTR limit;

    TRACE("%p %p %08lx %x %08x\n", process, *ret, *size_ptr, type, protect );

    if (!*size_ptr) return STATUS_INVALID_PARAMETER;
    if (zero_bits > 21 && zero_bits < 32) return STATUS_INVALID_PARAMETER_3;
    if (zero_bits > 32 && zero_bits < granularity_mask) return STATUS_INVALID_PARAMETER_3;
#ifndef _WIN64
    if (!is_old_wow64() && zero_bits >= 32) return STATUS_INVALID_PARAMETER_3;
#endif
    if (type & ~type_mask) return STATUS_INVALID_PARAMETER;

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;
        unsigned int status;

        memset( &call, 0, sizeof(call) );

        call.virtual_alloc.type         = APC_VIRTUAL_ALLOC;
        call.virtual_alloc.addr         = wine_server_client_ptr( *ret );
        call.virtual_alloc.size         = *size_ptr;
        call.virtual_alloc.zero_bits    = zero_bits;
        call.virtual_alloc.op_type      = type;
        call.virtual_alloc.prot         = protect;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.virtual_alloc.status == STATUS_SUCCESS)
        {
            *ret      = wine_server_get_ptr( result.virtual_alloc.addr );
            *size_ptr = result.virtual_alloc.size;
        }
        else
        {
            WARN( "cross-process allocation failed, process=%p base=%p size=%08lx status=%08x",
                  process, *ret, *size_ptr, result.virtual_alloc.status );
        }
        return result.virtual_alloc.status;
    }

    if (!*ret)
        limit = get_zero_bits_limit( zero_bits );
    else
        limit = 0;

    return allocate_virtual_memory( ret, size_ptr, type, protect, 0, limit, 0, 0 );
}


static NTSTATUS get_extended_params( const MEM_EXTENDED_PARAMETER *parameters, ULONG count,
                                     ULONG_PTR *limit_low, ULONG_PTR *limit_high, ULONG_PTR *align,
                                     ULONG *attributes, USHORT *machine )
{
    ULONG i, present = 0;

    if (count && !parameters) return STATUS_INVALID_PARAMETER;

    for (i = 0; i < count; ++i)
    {
        if (parameters[i].Type >= 32) return STATUS_INVALID_PARAMETER;
        if (present & (1u << parameters[i].Type)) return STATUS_INVALID_PARAMETER;
        present |= 1u << parameters[i].Type;

        switch (parameters[i].Type)
        {
        case MemExtendedParameterAddressRequirements:
        {
            MEM_ADDRESS_REQUIREMENTS *r = parameters[i].Pointer;
            ULONG_PTR limit;

            if (is_wow64()) limit = get_wow_user_space_limit();
            else limit = (ULONG_PTR)user_space_limit;

            if (r->Alignment)
            {
                if ((r->Alignment & (r->Alignment - 1)) || r->Alignment - 1 < granularity_mask)
                {
                    WARN( "Invalid alignment %lu.\n", r->Alignment );
                    return STATUS_INVALID_PARAMETER;
                }
                *align = r->Alignment;
            }
            if (r->LowestStartingAddress)
            {
                *limit_low = (ULONG_PTR)r->LowestStartingAddress;
                if (*limit_low >= limit || (*limit_low & granularity_mask))
                {
                    WARN( "Invalid limit %p.\n", r->LowestStartingAddress );
                    return STATUS_INVALID_PARAMETER;
                }
            }
            if (r->HighestEndingAddress)
            {
                *limit_high = (ULONG_PTR)r->HighestEndingAddress;
                if (*limit_high > limit ||
                    *limit_high <= *limit_low ||
                    ((*limit_high + 1) & (page_mask - 1)))
                {
                    WARN( "Invalid limit %p.\n", r->HighestEndingAddress );
                    return STATUS_INVALID_PARAMETER;
                }
            }
            break;
        }

        case MemExtendedParameterAttributeFlags:
            *attributes = parameters[i].ULong;
            break;

        case MemExtendedParameterImageMachine:
            *machine = parameters[i].ULong;
            break;

        case MemExtendedParameterNumaNode:
        case MemExtendedParameterPartitionHandle:
        case MemExtendedParameterUserPhysicalHandle:
            FIXME( "Parameter type %d is not supported.\n", parameters[i].Type );
            break;

        default:
            WARN( "Invalid parameter type %u\n", parameters[i].Type );
            return STATUS_INVALID_PARAMETER;
        }
    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *             NtAllocateVirtualMemoryEx   (NTDLL.@)
 *             ZwAllocateVirtualMemoryEx   (NTDLL.@)
 */
NTSTATUS WINAPI NtAllocateVirtualMemoryEx( HANDLE process, PVOID *ret, SIZE_T *size_ptr, ULONG type,
                                           ULONG protect, MEM_EXTENDED_PARAMETER *parameters,
                                           ULONG count )
{
    static const ULONG type_mask = MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_WRITE_WATCH
                                   | MEM_RESET | MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER;
    ULONG_PTR limit_low = 0;
    ULONG_PTR limit_high = 0;
    ULONG_PTR align = 0;
    ULONG attributes = 0;
    USHORT machine = 0;
    unsigned int status;

    TRACE( "%p %p %08lx %x %08x %p %u\n",
          process, *ret, *size_ptr, type, protect, parameters, count );

    status = get_extended_params( parameters, count, &limit_low, &limit_high,
                                  &align, &attributes, &machine );
    if (status) return status;

    if (type & ~type_mask) return STATUS_INVALID_PARAMETER;
    if (*ret && (align || limit_low || limit_high)) return STATUS_INVALID_PARAMETER;
    if (!*size_ptr) return STATUS_INVALID_PARAMETER;

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.virtual_alloc_ex.type         = APC_VIRTUAL_ALLOC_EX;
        call.virtual_alloc_ex.addr         = wine_server_client_ptr( *ret );
        call.virtual_alloc_ex.size         = *size_ptr;
        call.virtual_alloc_ex.limit_low    = limit_low;
        call.virtual_alloc_ex.limit_high   = limit_high;
        call.virtual_alloc_ex.align        = align;
        call.virtual_alloc_ex.op_type      = type;
        call.virtual_alloc_ex.prot         = protect;
        call.virtual_alloc_ex.attributes   = attributes;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.virtual_alloc_ex.status == STATUS_SUCCESS)
        {
            *ret      = wine_server_get_ptr( result.virtual_alloc_ex.addr );
            *size_ptr = result.virtual_alloc_ex.size;
        }
        return result.virtual_alloc_ex.status;
    }

    return allocate_virtual_memory( ret, size_ptr, type, protect,
                                    limit_low, limit_high, align, attributes );
}


/***********************************************************************
 *             NtFreeVirtualMemory   (NTDLL.@)
 *             ZwFreeVirtualMemory   (NTDLL.@)
 */
#ifdef __PROSPERO__
static NTSTATUS title_free_virtual_memory( HANDLE process, PVOID *addr_ptr, SIZE_T *size_ptr, ULONG type )
#else
NTSTATUS WINAPI NtFreeVirtualMemory( HANDLE process, PVOID *addr_ptr, SIZE_T *size_ptr, ULONG type )
#endif
{
    struct file_view *view;
    char *base;
    sigset_t sigset;
    unsigned int status = STATUS_SUCCESS;
    LPVOID addr = *addr_ptr;
    SIZE_T size = *size_ptr;

    TRACE("%p %p %08lx %x\n", process, addr, size, type );

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.virtual_free.type      = APC_VIRTUAL_FREE;
        call.virtual_free.addr      = wine_server_client_ptr( addr );
        call.virtual_free.size      = size;
        call.virtual_free.op_type   = type;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.virtual_free.status == STATUS_SUCCESS)
        {
            *addr_ptr = wine_server_get_ptr( result.virtual_free.addr );
            *size_ptr = result.virtual_free.size;
        }
        return result.virtual_free.status;
    }

    /* Fix the parameters */

    if (size) size = ROUND_SIZE( addr, size, page_mask );
    base = ROUND_ADDR( addr, page_mask );

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );

    /* avoid freeing the DOS area when a broken app passes a NULL pointer */
    if (!base)
    {
#ifndef _WIN64
        /* address 1 is magic to mean release reserved space */
        if (addr == (void *)1 && !size && type == MEM_RELEASE) virtual_release_address_space();
        else
#endif
        status = STATUS_INVALID_PARAMETER;
    }
    else if (!(view = find_view( base, 0 ))) status = STATUS_MEMORY_NOT_ALLOCATED;
    else if (!is_view_valloc( view )) status = STATUS_INVALID_PARAMETER;
    else if (!size && base != view->base) status = STATUS_FREE_VM_NOT_AT_BASE;
    else if ((char *)view->base + view->size - base < size && !(type & MEM_COALESCE_PLACEHOLDERS))
             status = STATUS_UNABLE_TO_FREE_VM;
    else switch (type)
    {
    case MEM_DECOMMIT:
        status = decommit_pages( view, base, size );
        break;
    case MEM_RELEASE:
        if (!size) size = view->size;
        status = free_pages( view, base, size );
        break;
    case MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER:
        status = free_pages_preserve_placeholder( view, base, size );
        break;
    case MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS:
        status = coalesce_placeholders( view, base, size );
        break;
    case MEM_COALESCE_PLACEHOLDERS:
        status = STATUS_INVALID_PARAMETER_4;
        break;
    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }


    dump_memory_statistics();
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    if (status == STATUS_SUCCESS)
    {
        *addr_ptr = base;
        *size_ptr = size;
    }
    return status;
}

#ifdef __PROSPERO__
NTSTATUS WINAPI NtFreeVirtualMemory( HANDLE process, PVOID *addr_ptr, SIZE_T *size_ptr, ULONG type )
{
    struct title_vm_cpu_scope scope;
    NTSTATUS status;

    if (!title_profile_is_enabled()) return title_free_virtual_memory( process, addr_ptr, size_ptr, type );
    scope = title_vm_cpu_begin();
    status = title_free_virtual_memory( process, addr_ptr, size_ptr, type );

    title_vm_cpu_end( TITLE_VM_FREE, scope, status ? 0 : *size_ptr, status );
    return status;
}
#endif



/***********************************************************************
 *             NtProtectVirtualMemory   (NTDLL.@)
 *             ZwProtectVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtProtectVirtualMemory( HANDLE process, PVOID *addr_ptr, SIZE_T *size_ptr,
                                        ULONG new_prot, ULONG *old_prot )
{
    struct file_view *view;
    sigset_t sigset;
    unsigned int status = STATUS_SUCCESS;
    char *base;
    BYTE vprot;
    SIZE_T size = *size_ptr;
    LPVOID addr = *addr_ptr;
    DWORD old;

    TRACE("%p %p %08lx %08x\n", process, addr, size, new_prot );

    if (!old_prot)
        return STATUS_ACCESS_VIOLATION;

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.virtual_protect.type = APC_VIRTUAL_PROTECT;
        call.virtual_protect.addr = wine_server_client_ptr( addr );
        call.virtual_protect.size = size;
        call.virtual_protect.prot = new_prot;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.virtual_protect.status == STATUS_SUCCESS)
        {
            *addr_ptr = wine_server_get_ptr( result.virtual_protect.addr );
            *size_ptr = result.virtual_protect.size;
            *old_prot = result.virtual_protect.prot;
        }
        else *old_prot = PAGE_NOACCESS;
        return result.virtual_protect.status;
    }

    /* Fix the parameters */

    size = ROUND_SIZE( addr, size, page_mask );
    base = ROUND_ADDR( addr, page_mask );

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );

    if ((view = find_view( base, size )))
    {
        /* Make sure all the pages are committed */
        if (get_committed_size( view, base, size, &vprot, VPROT_COMMITTED ) >= size && (vprot & VPROT_COMMITTED))
        {
            old = get_win32_prot( vprot, view->protect );
            status = set_protection( view, base, size, new_prot );

            if (simulate_writecopy && status == STATUS_SUCCESS
                && ((old == PAGE_WRITECOPY || old == PAGE_EXECUTE_WRITECOPY)))
            {
                TRACE("Setting VPROT_COPIED.\n");

                set_page_vprot_bits(base, size, VPROT_COPIED, 0);
                vprot |= VPROT_COPIED;
                old = get_win32_prot( vprot, view->protect );
            }
            else if (status == STATUS_SUCCESS && (view->protect & SEC_IMAGE) &&
                    base == (void*)NtCurrentTeb()->Peb->ImageBaseAddress)
            {
                /* GTA5 HACK: Mark first page as copied. */
                const WCHAR gta5W[] = { 'g','t','a','5','.','e','x','e',0 };
                WCHAR *name, *p;

                name = NtCurrentTeb()->Peb->ProcessParameters->ImagePathName.Buffer;
                p = wcsrchr(name, '\\');
                p = p ? p+1 : name;

                if(!wcsicmp(p, gta5W))
                {
                    FIXME("HACK: changing GTA5.exe vprot\n");
                    set_page_vprot_bits(base, page_size, VPROT_COPIED, 0);
                }
            }
        }
        else status = STATUS_NOT_COMMITTED;
    }
    else status = STATUS_INVALID_PARAMETER;

    if (!status) VIRTUAL_DEBUG_DUMP_VIEW( view );

    server_leave_uninterrupted_section( &virtual_mutex, &sigset );

    if (status == STATUS_SUCCESS)
    {
        *addr_ptr = base;
        *size_ptr = size;
        *old_prot = old;
    }
    else *old_prot = PAGE_NOACCESS;
    return status;
}


static struct file_view *get_memory_region_size( char *base, char **region_start, char **region_end,
                                                 BOOL *fake_reserved )
{
    struct wine_rb_entry *ptr;
    struct file_view *view;

    *fake_reserved = FALSE;
    *region_start = NULL;
    *region_end = working_set_limit;

    ptr = views_tree.root;
    while (ptr)
    {
        view = WINE_RB_ENTRY_VALUE( ptr, struct file_view, entry );
        if ((char *)view->base > base)
        {
            *region_end = view->base;
            ptr = ptr->left;
        }
        else if ((char *)view->base + view->size <= base)
        {
            *region_start = (char *)view->base + view->size;
            ptr = ptr->right;
        }
        else
        {
            *region_start = view->base;
            *region_end = (char *)view->base + view->size;
            return view;
        }
    }
#ifdef __i386__
    {
        struct reserved_area *area;

        /* on i386, pretend that space outside of a reserved area is allocated,
         * so that the app doesn't believe it's fully available */
        LIST_FOR_EACH_ENTRY( area, &reserved_areas, struct reserved_area, entry )
        {
            char *area_start = area->base;
            char *area_end = area_start + area->size;

            if (area_end <= base)
            {
                if (*region_start < area_end) *region_start = area_end;
                continue;
            }
            if (area_start <= base || area_start <= (char *)address_space_start)
            {
                if (area_end < *region_end) *region_end = area_end;
                return NULL;
            }
            /* report the remaining part of the 64K after the view as free */
            if ((UINT_PTR)*region_start & granularity_mask)
            {
                char *next = (char *)ROUND_ADDR( *region_start, granularity_mask ) + granularity_mask + 1;

                if (base < next)
                {
                    *region_end = min( next, *region_end );
                    return NULL;
                }
                else *region_start = base;
            }
            /* pretend it's allocated */
            if (area_start < *region_end) *region_end = area_start;
            break;
        }
        *fake_reserved = TRUE;
    }
#endif
    return NULL;
}


static unsigned int fill_basic_memory_info( const void *addr, MEMORY_BASIC_INFORMATION *info )
{
    MEMORY_BASIC_INFORMATION result = {0};
    char *base, *alloc_base, *alloc_end;
    struct file_view *view;
    BOOL fake_reserved;
    sigset_t sigset;

    base = ROUND_ADDR( addr, page_mask );
    if (is_beyond_limit( base, 1, working_set_limit )) return STATUS_INVALID_PARAMETER;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    view = get_memory_region_size( base, &alloc_base, &alloc_end, &fake_reserved );
    result.BaseAddress = base;
    result.RegionSize = alloc_end - base;
    if (!view)
    {
        if (fake_reserved)
        {
            result.State             = MEM_RESERVE;
            result.Protect           = PAGE_NOACCESS;
            result.AllocationBase    = alloc_base;
            result.AllocationProtect = PAGE_NOACCESS;
            result.Type              = MEM_PRIVATE;
        }
        else
        {
            result.State             = MEM_FREE;
            result.Protect           = PAGE_NOACCESS;
        }
    }
    else
    {
        BYTE vprot;

        result.AllocationBase = alloc_base;
        result.RegionSize = get_committed_size( view, base, ~(size_t)0, &vprot, ~VPROT_WRITEWATCH );
        result.State = (vprot & VPROT_COMMITTED) ? MEM_COMMIT : MEM_RESERVE;
        result.Protect = (vprot & VPROT_COMMITTED) ? get_win32_prot( vprot, view->protect ) : 0;
        result.AllocationProtect = get_win32_prot( view->protect, view->protect );
        if (view->protect & SEC_IMAGE) result.Type = MEM_IMAGE;
        else if (view->protect & (SEC_FILE | SEC_RESERVE | SEC_COMMIT)) result.Type = MEM_MAPPED;
        else result.Type = MEM_PRIVATE;
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    *info = result;
    return STATUS_SUCCESS;
}

/* get basic information about a memory block */
static unsigned int get_basic_memory_info( HANDLE process, LPCVOID addr,
                                           MEMORY_BASIC_INFORMATION *info,
                                           SIZE_T len, SIZE_T *res_len )
{
    unsigned int status;

    if (len < sizeof(*info))
        return STATUS_INFO_LENGTH_MISMATCH;

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.virtual_query.type = APC_VIRTUAL_QUERY;
        call.virtual_query.addr = wine_server_client_ptr( addr );
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.virtual_query.status == STATUS_SUCCESS)
        {
            info->BaseAddress       = wine_server_get_ptr( result.virtual_query.base );
            info->AllocationBase    = wine_server_get_ptr( result.virtual_query.alloc_base );
            info->RegionSize        = result.virtual_query.size;
            info->Protect           = result.virtual_query.prot;
            info->AllocationProtect = result.virtual_query.alloc_prot;
            info->State             = (DWORD)result.virtual_query.state << 12;
            info->Type              = (DWORD)result.virtual_query.alloc_type << 16;
            if (info->RegionSize != result.virtual_query.size)  /* truncated */
                return STATUS_INVALID_PARAMETER;  /* FIXME */
            if (res_len) *res_len = sizeof(*info);
        }
        return result.virtual_query.status;
    }

    if ((status = fill_basic_memory_info( addr, info ))) return status;

    if (res_len) *res_len = sizeof(*info);
    return STATUS_SUCCESS;
}

static unsigned int get_memory_region_info( HANDLE process, LPCVOID addr, MEMORY_REGION_INFORMATION *info,
                                            SIZE_T len, SIZE_T *res_len )
{
    MEMORY_REGION_INFORMATION result = {0};
    char *base, *region_start, *region_end;
    struct file_view *view;
    BYTE vprot, vprot_mask;
    unsigned int status = STATUS_SUCCESS;
    BOOL fake_reserved;
    sigset_t sigset;
    SIZE_T size;

    if (len < FIELD_OFFSET(MEMORY_REGION_INFORMATION, CommitSize))
        return STATUS_INFO_LENGTH_MISMATCH;
    if (process != NtCurrentProcess())
    {
        FIXME("Unimplemented for other processes.\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    base = ROUND_ADDR( addr, page_mask );
    if (is_beyond_limit( base, 1, working_set_limit )) return STATUS_INVALID_PARAMETER;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if ((view = get_memory_region_size( base, &region_start, &region_end, &fake_reserved )))
    {
        result.AllocationBase = view->base;
        result.AllocationProtect = get_win32_prot( view->protect, view->protect );
        result.RegionType = 0; /* FIXME */
        result.RegionSize = view->size;
        if (len >= FIELD_OFFSET(MEMORY_REGION_INFORMATION, PartitionId))
        {
            base = region_start;
            vprot_mask = VPROT_COMMITTED;
            if (!is_view_valloc( view )) vprot_mask |= PAGE_WRITECOPY;
            while (base != region_end &&
                   (size = get_committed_size( view, base, ~(size_t)0, &vprot, vprot_mask )))
            {
                if ((vprot & vprot_mask) == vprot_mask) result.CommitSize += size;
                base += size;
            }
        }
    }
    else if (!fake_reserved)
        status = STATUS_INVALID_ADDRESS;
    else
    {
        result.AllocationBase = region_start;
        result.AllocationProtect = PAGE_NOACCESS;
        result.RegionType = 0; /* FIXME */
        result.RegionSize = region_end - region_start;
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );

    if (status) return status;
    info->AllocationBase = result.AllocationBase;
    info->AllocationProtect = result.AllocationProtect;
    info->RegionType = result.RegionType;
    info->RegionSize = result.RegionSize;
    if (len >= FIELD_OFFSET(MEMORY_REGION_INFORMATION, PartitionId))
        info->CommitSize = result.CommitSize;
    if (res_len) *res_len = sizeof(*info);
    return STATUS_SUCCESS;
}

struct working_set_info_ref
{
    char *addr;
    SIZE_T orig_index;
    MEMORY_WORKING_SET_EX_BLOCK attributes;
};

#if defined(HAVE_LIBPROCSTAT)
struct fill_working_set_info_data
{
    struct procstat *pstat;
    struct kinfo_proc *kip;
    unsigned int vmentry_count;
    struct kinfo_vmentry *vmentries;
};

static void init_fill_working_set_info_data( struct fill_working_set_info_data *d, char *end )
{
    unsigned int proc_count;

    d->kip = NULL;
    d->vmentry_count = 0;
    d->vmentries = NULL;

    if ((d->pstat = procstat_open_sysctl()))
        d->kip = procstat_getprocs( d->pstat, KERN_PROC_PID, getpid(), &proc_count );
    if (d->kip)
        d->vmentries = procstat_getvmmap( d->pstat, d->kip, &d->vmentry_count );
    if (!d->vmentries)
        WARN( "couldn't get process vmmap, errno %d\n", errno );
}

static void free_fill_working_set_info_data( struct fill_working_set_info_data *d )
{
    if (d->vmentries)
        procstat_freevmmap( d->pstat, d->vmentries );
    if (d->kip)
        procstat_freeprocs( d->pstat, d->kip );
    if (d->pstat)
        procstat_close( d->pstat );
}

static void fill_working_set_info( struct fill_working_set_info_data *d, struct file_view *view, BYTE vprot,
                                   struct working_set_info_ref *ref, SIZE_T count )
{
    SIZE_T i;
    int j;

    for (i = 0; i < count; ++i)
    {
        MEMORY_WORKING_SET_EX_BLOCK *p = &ref[i].attributes;
        struct kinfo_vmentry *entry = NULL;

        for (j = 0; j < d->vmentry_count; j++)
        {
            if (d->vmentries[j].kve_start <= (ULONG_PTR)ref[i].addr &&
                (ULONG_PTR)ref[i].addr <= d->vmentries[j].kve_end)
            {
                entry = &d->vmentries[j];
                break;
            }
        }

        p->Valid = !(vprot & VPROT_GUARD) && (vprot & 0x0f) &&
                   entry && entry->kve_type != KVME_TYPE_SWAP;
        p->Shared = !is_view_valloc( view );
        if (p->Shared && p->Valid) p->ShareCount = 1; /* FIXME */
        if (p->Valid) p->Win32Protection = get_win32_prot( vprot, view->protect );
    }
}
#else
static int pagemap_fd = -2;

struct fill_working_set_info_data
{
    UINT64 pm_buffer[256];
    SIZE_T buffer_start;
    ssize_t buffer_len;
    SIZE_T end_page;
};

static void init_fill_working_set_info_data( struct fill_working_set_info_data *d, char *end )
{
    d->buffer_start = 0;
    d->buffer_len = 0;
    d->end_page = (UINT_PTR)end / host_page_size;
    memset( d->pm_buffer, 0, sizeof(d->pm_buffer) );

    if (pagemap_fd != -2) return;

#ifdef O_CLOEXEC
    if ((pagemap_fd = open( "/proc/self/pagemap", O_RDONLY | O_CLOEXEC, 0 )) == -1 && errno == EINVAL)
#endif
        pagemap_fd = open( "/proc/self/pagemap", O_RDONLY, 0 );

    if (pagemap_fd == -1) WARN( "unable to open /proc/self/pagemap\n" );
    else fcntl(pagemap_fd, F_SETFD, FD_CLOEXEC);  /* in case O_CLOEXEC isn't supported */
}

static void free_fill_working_set_info_data( struct fill_working_set_info_data *d )
{
}

static void fill_working_set_info( struct fill_working_set_info_data *d, struct file_view *view, BYTE vprot,
                                   struct working_set_info_ref *ref, SIZE_T count )
{
    MEMORY_WORKING_SET_EX_BLOCK *p;
    UINT64 pagemap;
    SIZE_T i, page;
    ssize_t len;

    for (i = 0; i < count; ++i)
    {
        page = (UINT_PTR)ref[i].addr / host_page_size;
        p = &ref[i].attributes;

        assert(page >= d->buffer_start);
        if (page >= d->buffer_start + d->buffer_len)
        {
            d->buffer_start = page;
            len = min( sizeof(d->pm_buffer), (d->end_page - page) * sizeof(pagemap) );
            if (pagemap_fd != -1)
            {
                d->buffer_len = pread( pagemap_fd, d->pm_buffer, len, page * sizeof(pagemap) );
                if (d->buffer_len != len)
                {
                    d->buffer_len = max( d->buffer_len, 0 );
                    memset( d->pm_buffer + d->buffer_len / sizeof(pagemap), 0, len - d->buffer_len );
                }
            }
            d->buffer_len = len / sizeof(pagemap);
        }
        pagemap = d->pm_buffer[page - d->buffer_start];

        p->Valid = !(vprot & VPROT_GUARD) && (vprot & 0x0f) && (pagemap >> 63);
        p->Shared = !is_view_valloc( view ) && ((pagemap >> 61) & 1);
        if (p->Shared && p->Valid) p->ShareCount = 1; /* FIXME */
        if (p->Valid) p->Win32Protection = get_win32_prot( vprot, view->protect );
    }
}
#endif

static int compare_working_set_info_ref( const void *a, const void *b )
{
    const struct working_set_info_ref *r1 = a, *r2 = b;

    if (r1->addr < r2->addr) return -1;
    return r1->addr > r2->addr;
}

static NTSTATUS get_working_set_ex( HANDLE process, LPCVOID addr,
                                    MEMORY_WORKING_SET_EX_INFORMATION *info,
                                    SIZE_T len, SIZE_T *res_len )
{
    struct working_set_info_ref ref_buffer[256], *ref = ref_buffer, *r;
    struct fill_working_set_info_data data;
    char *start, *end;
    SIZE_T i, count;
    struct file_view *view, *prev_view;
    sigset_t sigset;
    BYTE vprot;

    if (process != NtCurrentProcess())
    {
        FIXME( "(process=%p,addr=%p) Unimplemented information class: MemoryWorkingSetExInformation\n", process, addr );
        return STATUS_INVALID_INFO_CLASS;
    }

    if (len < sizeof(*info)) return STATUS_INFO_LENGTH_MISMATCH;

    count = len / sizeof(*info);

    if (count > ARRAY_SIZE(ref_buffer) && !(ref = malloc( count * sizeof(*ref) )))
        return STATUS_NO_MEMORY;
    for (i = 0; i < count; ++i)
    {
        ref[i].orig_index = i;
        ref[i].addr = ROUND_ADDR( info[i].VirtualAddress, page_mask );
        ref[i].attributes.Flags = 0;
    }
    qsort( ref, count, sizeof(*ref), compare_working_set_info_ref );
    start = ref[0].addr;
    end = ref[count - 1].addr + page_size;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    init_fill_working_set_info_data( &data, end );

    view = find_view_range( start, end - start );
    while (view && (char *)view->base > start)
    {
        prev_view = RB_ENTRY_VALUE( rb_prev( &view->entry ), struct file_view, entry );
        if (!prev_view || (char *)prev_view->base + prev_view->size <= start) break;
        view = prev_view;
    }

    r = ref;
    while (view && (char *)view->base < end)
    {
        if (start < (char *)view->base) start = view->base;
        while (r != ref + count && r->addr < start) ++r;
        while (start != (char *)view->base + view->size && r != ref + count
               && r->addr < (char *)view->base + view->size)
        {
            start += get_committed_size( view, start, end - start, &vprot, ~VPROT_WRITEWATCH );
            i = 0;
            while (r + i != ref + count && r[i].addr < start) ++i;
            if (vprot & VPROT_COMMITTED) fill_working_set_info( &data, view, vprot, r, i );
            r += i;
        }
        if (r == ref + count) break;
        view = RB_ENTRY_VALUE( rb_next( &view->entry ), struct file_view, entry );
    }

    free_fill_working_set_info_data( &data );
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );

    for (i = 0; i < count; i++)
        info[ref[i].orig_index].VirtualAttributes = ref[i].attributes;
    if (ref != ref_buffer) free( ref );
    if (res_len) *res_len = len;
    return STATUS_SUCCESS;
}

static NTSTATUS read_nt_symlink( UNICODE_STRING *name, WCHAR *target, DWORD size )
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES attr;
    HANDLE handle;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.Attributes = OBJ_CASE_INSENSITIVE;
    attr.ObjectName = name;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    if (!(status = NtOpenSymbolicLinkObject( &handle, SYMBOLIC_LINK_QUERY, &attr )))
    {
        UNICODE_STRING targetW;
        targetW.Buffer = target;
        targetW.MaximumLength = (size - 1) * sizeof(WCHAR);
        status = NtQuerySymbolicLinkObject( handle, &targetW, NULL );
        if (!status) target[targetW.Length / sizeof(WCHAR)] = 0;
        NtClose( handle );
    }
    return status;
}

static NTSTATUS resolve_drive_symlink( UNICODE_STRING *name, SIZE_T max_name_len, SIZE_T *ret_len, NTSTATUS status )
{
    static int enabled = -1;

    static const WCHAR dosprefixW[] = {'\\','?','?','\\'};
    UNICODE_STRING device_name;
    SIZE_T required_length, symlink_len;
    WCHAR symlink[256];
    size_t offset = 0;

    if (enabled == -1)
    {
        const char *sgi = getenv("SteamGameId");

        enabled = sgi && !strcmp(sgi, "284160");
    }
    if (!enabled) return status;
    if (status == STATUS_INFO_LENGTH_MISMATCH)
    {
        /* FIXME */
        *ret_len += 64;
        return status;
    }
    if (status) return status;

    if (name->Length < sizeof(dosprefixW) ||
            memcmp( name->Buffer, dosprefixW, sizeof(dosprefixW) ))
        return STATUS_SUCCESS;

    offset = ARRAY_SIZE(dosprefixW);
    while (offset * sizeof(WCHAR) < name->Length && name->Buffer[ offset ] != '\\') offset++;

    device_name = *name;
    device_name.Length = offset * sizeof(WCHAR);
    if ((status = read_nt_symlink( &device_name, symlink, ARRAY_SIZE( symlink ))))
    {
        ERR("read_nt_symlink failed, status %#x.\n", (int)status);
        return status;
    }
    symlink_len = wcslen( symlink );
    required_length = symlink_len * sizeof(WCHAR) +
           name->Length - offset * sizeof(WCHAR) + sizeof(WCHAR);
    if (ret_len)
        *ret_len = sizeof(MEMORY_SECTION_NAME) + required_length;
    if (required_length > max_name_len)
        return STATUS_INFO_LENGTH_MISMATCH;

    memmove( name->Buffer + symlink_len, name->Buffer + offset, name->Length - offset * sizeof(WCHAR) );
    memcpy( name->Buffer, symlink, symlink_len * sizeof(WCHAR) );
    name->MaximumLength = required_length;
    name->Length = required_length - sizeof(WCHAR);
    name->Buffer[name->Length / sizeof(WCHAR)] = 0;
    return STATUS_SUCCESS;
}

static unsigned int get_memory_section_name( HANDLE process, LPCVOID addr,
                                             MEMORY_SECTION_NAME *info, SIZE_T len, SIZE_T *ret_len )
{
    unsigned int status;

    if (!info) return STATUS_ACCESS_VIOLATION;

    SERVER_START_REQ( get_mapping_filename )
    {
        req->process = wine_server_obj_handle( process );
        req->addr = wine_server_client_ptr( addr );
        if (len > sizeof(*info) + sizeof(WCHAR))
            wine_server_set_reply( req, info + 1, len - sizeof(*info) - sizeof(WCHAR) );
        status = wine_server_call( req );
        if (!status || status == STATUS_BUFFER_OVERFLOW)
        {
            if (ret_len) *ret_len = sizeof(*info) + reply->len + sizeof(WCHAR);
            if (len < sizeof(*info)) status = STATUS_INFO_LENGTH_MISMATCH;
            if (!status)
            {
                info->SectionFileName.Buffer = (WCHAR *)(info + 1);
                info->SectionFileName.Length = reply->len;
                info->SectionFileName.MaximumLength = reply->len + sizeof(WCHAR);
                info->SectionFileName.Buffer[reply->len / sizeof(WCHAR)] = 0;
            }
        }
    }
    SERVER_END_REQ;

    return resolve_drive_symlink( &info->SectionFileName, len - sizeof(*info), ret_len, status );
}

static unsigned int get_memory_image_info( HANDLE process, LPCVOID addr, MEMORY_IMAGE_INFORMATION *info,
                                           SIZE_T len, SIZE_T *res_len )
{
    unsigned int status;

    if (len < sizeof(*info)) return STATUS_INFO_LENGTH_MISMATCH;
    memset( info, 0, sizeof(*info) );

    SERVER_START_REQ( get_image_view_info )
    {
        req->process = wine_server_obj_handle( process );
        req->addr = wine_server_client_ptr( addr );
        status = wine_server_call( req );
        if (!status && reply->base)
        {
            info->ImageBase = wine_server_get_ptr( reply->base );
            info->SizeOfImage = reply->size;
            info->ImageSigningLevel = 12;
        }
    }
    SERVER_END_REQ;

    if (status == STATUS_NOT_MAPPED_VIEW)
    {
        MEMORY_BASIC_INFORMATION basic_info;

        status = get_basic_memory_info( process, addr, &basic_info, sizeof(basic_info), NULL );
        if (status || basic_info.State == MEM_FREE) status = STATUS_INVALID_ADDRESS;
    }

    if (!status && res_len) *res_len = sizeof(*info);
    return status;
}


/***********************************************************************
 *             NtQueryVirtualMemory   (NTDLL.@)
 *             ZwQueryVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryVirtualMemory( HANDLE process, LPCVOID addr,
                                      MEMORY_INFORMATION_CLASS info_class,
                                      PVOID buffer, SIZE_T len, SIZE_T *res_len )
{
    NTSTATUS status;

    TRACE("(%p, %p, info_class=%d, %p, %ld, %p)\n",
          process, addr, info_class, buffer, len, res_len);

    switch(info_class)
    {
        case MemoryBasicInformation:
            return get_basic_memory_info( process, addr, buffer, len, res_len );

        case MemoryWorkingSetExInformation:
            return get_working_set_ex( process, addr, buffer, len, res_len );

        case MemoryMappedFilenameInformation:
            return get_memory_section_name( process, addr, buffer, len, res_len );

        case MemoryRegionInformation:
            return get_memory_region_info( process, addr, buffer, len, res_len );

        case MemoryImageInformation:
            return get_memory_image_info( process, addr, buffer, len, res_len );

        case MemoryWineUnixFuncs:
        case MemoryWineUnixWow64Funcs:
            if (len != sizeof(unixlib_handle_t)) return STATUS_INFO_LENGTH_MISMATCH;
            if (process == GetCurrentProcess())
            {
                void *module = (void *)addr;
                const void *funcs = NULL;

                status = get_builtin_unix_funcs( module, info_class == MemoryWineUnixWow64Funcs, &funcs );
                if (!status) *(unixlib_handle_t *)buffer = (UINT_PTR)funcs;
                return status;
            }
            return STATUS_INVALID_HANDLE;

        default:
            FIXME("(%p,%p,info_class=%d,%p,%ld,%p) Unknown information class\n",
                  process, addr, info_class, buffer, len, res_len);
            return STATUS_INVALID_INFO_CLASS;
    }
}


/***********************************************************************
 *             NtLockVirtualMemory   (NTDLL.@)
 *             ZwLockVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtLockVirtualMemory( HANDLE process, PVOID *addr, SIZE_T *size, ULONG unknown )
{
    unsigned int status = STATUS_SUCCESS;

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.virtual_lock.type = APC_VIRTUAL_LOCK;
        call.virtual_lock.addr = wine_server_client_ptr( *addr );
        call.virtual_lock.size = *size;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.virtual_lock.status == STATUS_SUCCESS)
        {
            *addr = wine_server_get_ptr( result.virtual_lock.addr );
            *size = result.virtual_lock.size;
        }
        return result.virtual_lock.status;
    }

    *size = ROUND_SIZE( *addr, *size, page_mask );
    *addr = ROUND_ADDR( *addr, page_mask );

    if (mlock( ROUND_ADDR( *addr, host_page_mask ), ROUND_SIZE( *addr, *size, host_page_mask ) ))
        status = STATUS_ACCESS_DENIED;
    return status;
}


/***********************************************************************
 *             NtUnlockVirtualMemory   (NTDLL.@)
 *             ZwUnlockVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtUnlockVirtualMemory( HANDLE process, PVOID *addr, SIZE_T *size, ULONG unknown )
{
    unsigned int status = STATUS_SUCCESS;

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.virtual_unlock.type = APC_VIRTUAL_UNLOCK;
        call.virtual_unlock.addr = wine_server_client_ptr( *addr );
        call.virtual_unlock.size = *size;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.virtual_unlock.status == STATUS_SUCCESS)
        {
            *addr = wine_server_get_ptr( result.virtual_unlock.addr );
            *size = result.virtual_unlock.size;
        }
        return result.virtual_unlock.status;
    }

    *size = ROUND_SIZE( *addr, *size, page_mask );
    *addr = ROUND_ADDR( *addr, page_mask );

    if (munlock( ROUND_ADDR( *addr, host_page_mask ), ROUND_SIZE( *addr, *size, host_page_mask ) ))
        status = STATUS_ACCESS_DENIED;
    return status;
}


/***********************************************************************
 *             NtMapViewOfSection   (NTDLL.@)
 *             ZwMapViewOfSection   (NTDLL.@)
 */
NTSTATUS WINAPI NtMapViewOfSection( HANDLE handle, HANDLE process, PVOID *addr_ptr, ULONG_PTR zero_bits,
                                    SIZE_T commit_size, const LARGE_INTEGER *offset_ptr, SIZE_T *size_ptr,
                                    SECTION_INHERIT inherit, ULONG alloc_type, ULONG protect )
{
    unsigned int res;
    SIZE_T mask = granularity_mask;
    LARGE_INTEGER offset;

#ifdef __PROSPERO__
    if ((res = ps5_title_flush_coalesced_write( 0 ))) return res;
#endif
    offset.QuadPart = offset_ptr ? offset_ptr->QuadPart : 0;

    TRACE("handle=%p process=%p addr=%p off=%s size=0x%lx alloc_type=0x%x access=0x%x\n",
          handle, process, *addr_ptr, wine_dbgstr_longlong(offset.QuadPart), *size_ptr, alloc_type, protect );

    /* Check parameters */
    if (zero_bits > 21 && zero_bits < 32)
        return STATUS_INVALID_PARAMETER_4;

    /* If both addr_ptr and zero_bits are passed, they have match */
    if (zero_bits && zero_bits < 32 && ((UINT_PTR)*addr_ptr >> (32 - zero_bits)))
        return STATUS_INVALID_PARAMETER_4;
    if (zero_bits >= 32 && ((UINT_PTR)*addr_ptr & ~zero_bits))
        return STATUS_INVALID_PARAMETER_4;

    if (!is_win64 && !is_wow64())
    {
        if (zero_bits >= 32) return STATUS_INVALID_PARAMETER_4;
        if (alloc_type & AT_ROUND_TO_PAGE)
        {
            *addr_ptr = ROUND_ADDR( *addr_ptr, page_mask );
            mask = page_mask;
        }
    }
    else if (alloc_type & AT_ROUND_TO_PAGE) return STATUS_INVALID_PARAMETER_9;

    if (alloc_type & MEM_REPLACE_PLACEHOLDER) mask = page_mask;
    if (offset.u.LowPart & mask) return STATUS_MAPPED_ALIGNMENT;
    if ((UINT_PTR)*addr_ptr & mask) return STATUS_MAPPED_ALIGNMENT;
    if ((UINT_PTR)*addr_ptr & host_page_mask)
    {
        ERR( "unaligned placeholder at %p\n", *addr_ptr );
        return STATUS_MAPPED_ALIGNMENT;
    }

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.map_view.type         = APC_MAP_VIEW;
        call.map_view.handle       = wine_server_obj_handle( handle );
        call.map_view.addr         = wine_server_client_ptr( *addr_ptr );
        call.map_view.size         = *size_ptr;
        call.map_view.offset       = offset.QuadPart;
        call.map_view.zero_bits    = zero_bits;
        call.map_view.alloc_type   = alloc_type;
        call.map_view.prot         = protect;
        res = server_queue_process_apc( process, &call, &result );
        if (res != STATUS_SUCCESS) return res;

        if (NT_SUCCESS(result.map_view.status))
        {
            *addr_ptr = wine_server_get_ptr( result.map_view.addr );
            *size_ptr = result.map_view.size;
        }
        return result.map_view.status;
    }

    return virtual_map_section( handle, addr_ptr, 0, get_zero_bits_limit( zero_bits ), commit_size,
                                offset_ptr, size_ptr, alloc_type, protect, 0 );
}

/***********************************************************************
 *             NtMapViewOfSectionEx   (NTDLL.@)
 *             ZwMapViewOfSectionEx   (NTDLL.@)
 */
NTSTATUS WINAPI NtMapViewOfSectionEx( HANDLE handle, HANDLE process, PVOID *addr_ptr,
                                      const LARGE_INTEGER *offset_ptr, SIZE_T *size_ptr,
                                      ULONG alloc_type, ULONG protect,
                                      MEM_EXTENDED_PARAMETER *parameters, ULONG count )
{
    ULONG_PTR limit_low = 0, limit_high = 0, align = 0;
    ULONG attributes = 0;
    USHORT machine = 0;
    unsigned int status;
    SIZE_T mask = granularity_mask;
    LARGE_INTEGER offset;

    offset.QuadPart = offset_ptr ? offset_ptr->QuadPart : 0;

    TRACE( "handle=%p process=%p addr=%p off=%s size=0x%lx alloc_type=0x%x access=0x%x\n",
           handle, process, *addr_ptr, wine_dbgstr_longlong(offset.QuadPart), *size_ptr, alloc_type, protect );
#ifdef __PROSPERO__
    if ((status = ps5_title_flush_coalesced_write( 0 ))) return status;
#endif

    status = get_extended_params( parameters, count, &limit_low, &limit_high,
                                  &align, &attributes, &machine );
    if (status) return status;

    if (align) return STATUS_INVALID_PARAMETER;
    if (*addr_ptr && (limit_low || limit_high)) return STATUS_INVALID_PARAMETER;

    if (alloc_type & AT_ROUND_TO_PAGE)
    {
        if (is_win64 || is_wow64()) return STATUS_INVALID_PARAMETER;
        *addr_ptr = ROUND_ADDR( *addr_ptr, page_mask );
        mask = page_mask;
    }

    if (alloc_type & MEM_REPLACE_PLACEHOLDER) mask = page_mask;
    if (offset.u.LowPart & mask) return STATUS_MAPPED_ALIGNMENT;
    if ((UINT_PTR)*addr_ptr & mask) return STATUS_MAPPED_ALIGNMENT;
    if ((UINT_PTR)*addr_ptr & host_page_mask)
    {
        ERR( "unaligned placeholder at %p\n", *addr_ptr );
        return STATUS_MAPPED_ALIGNMENT;
    }

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.map_view_ex.type         = APC_MAP_VIEW_EX;
        call.map_view_ex.handle       = wine_server_obj_handle( handle );
        call.map_view_ex.addr         = wine_server_client_ptr( *addr_ptr );
        call.map_view_ex.size         = *size_ptr;
        call.map_view_ex.offset       = offset.QuadPart;
        call.map_view_ex.limit_low    = limit_low;
        call.map_view_ex.limit_high   = limit_high;
        call.map_view_ex.alloc_type   = alloc_type;
        call.map_view_ex.prot         = protect;
        call.map_view_ex.machine      = machine;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (NT_SUCCESS(result.map_view_ex.status))
        {
            *addr_ptr = wine_server_get_ptr( result.map_view_ex.addr );
            *size_ptr = result.map_view_ex.size;
        }
        return result.map_view_ex.status;
    }

    return virtual_map_section( handle, addr_ptr, limit_low, limit_high, 0,
                                offset_ptr, size_ptr, alloc_type, protect, machine );
}


/***********************************************************************
 *             unmap_view_of_section
 *
 * NtUnmapViewOfSection[Ex] implementation.
 */
static NTSTATUS unmap_view_of_section( HANDLE process, PVOID addr, ULONG flags )
{
    struct file_view *view;
    unsigned int status = STATUS_NOT_MAPPED_VIEW;
    sigset_t sigset;

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.unmap_view.type = APC_UNMAP_VIEW;
        call.unmap_view.addr = wine_server_client_ptr( addr );
        call.unmap_view.flags = flags;
        status = server_queue_process_apc( process, &call, &result );
        if (status == STATUS_SUCCESS) status = result.unmap_view.status;
        return status;
    }

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (!(view = find_view( addr, 0 )) || is_view_valloc( view )) goto done;

    if (flags & MEM_PRESERVE_PLACEHOLDER && !(view->protect & VPROT_PLACEHOLDER))
    {
        status = STATUS_CONFLICTING_ADDRESSES;
        goto done;
    }
    if (view->protect & VPROT_SYSTEM)
    {
        struct builtin_module *builtin;

        LIST_FOR_EACH_ENTRY( builtin, &builtin_modules, struct builtin_module, entry )
        {
            if (builtin->module != view->base) continue;
            if (builtin->refcount > 1)
            {
                TRACE( "not freeing in-use builtin %p\n", view->base );
                builtin->refcount--;
                server_leave_uninterrupted_section( &virtual_mutex, &sigset );
                return STATUS_SUCCESS;
            }
        }
    }

    SERVER_START_REQ( unmap_view )
    {
        req->base = wine_server_client_ptr( view->base );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    if (!status)
    {
        if (view->protect & SEC_IMAGE)
        {
            r_debug_remove_module( view->base );
            release_builtin_module( view->base );
        }
        if (flags & MEM_PRESERVE_PLACEHOLDER) free_pages_preserve_placeholder( view, view->base, view->size );
        else delete_view( view );
    }
    else FIXME( "failed to unmap %p %x\n", view->base, status );
done:
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return status;
}


/***********************************************************************
 *             NtUnmapViewOfSection   (NTDLL.@)
 *             ZwUnmapViewOfSection   (NTDLL.@)
 */
NTSTATUS WINAPI NtUnmapViewOfSection( HANDLE process, PVOID addr )
{
#ifdef __PROSPERO__
    NTSTATUS status = ps5_title_flush_coalesced_write( 0 );
    if (status) return status;
#endif
    return unmap_view_of_section( process, addr, 0 );
}

/***********************************************************************
 *             NtUnmapViewOfSectionEx   (NTDLL.@)
 *             ZwUnmapViewOfSectionEx   (NTDLL.@)
 */
NTSTATUS WINAPI NtUnmapViewOfSectionEx( HANDLE process, PVOID addr, ULONG flags )
{
    static const ULONG type_mask = MEM_UNMAP_WITH_TRANSIENT_BOOST | MEM_PRESERVE_PLACEHOLDER;
#ifdef __PROSPERO__
    NTSTATUS status = ps5_title_flush_coalesced_write( 0 );
    if (status) return status;
#endif

    if (flags & ~type_mask)
    {
        WARN( "Unsupported flags %#x.\n", flags );
        return STATUS_INVALID_PARAMETER;
    }
    if (flags & MEM_UNMAP_WITH_TRANSIENT_BOOST) FIXME( "Ignoring MEM_UNMAP_WITH_TRANSIENT_BOOST.\n" );
    return unmap_view_of_section( process, addr, flags );
}

/******************************************************************************
 *             virtual_fill_image_information
 *
 * Helper for NtQuerySection.
 */
void virtual_fill_image_information( const struct pe_image_info *pe_info, SECTION_IMAGE_INFORMATION *info )
{
    info->TransferAddress             = wine_server_get_ptr( pe_info->base + pe_info->entry_point );
    info->ZeroBits                    = pe_info->zerobits;
    info->MaximumStackSize            = pe_info->stack_size;
    info->CommittedStackSize          = pe_info->stack_commit;
    info->SubSystemType               = pe_info->subsystem;
    info->MinorSubsystemVersion       = pe_info->subsystem_minor;
    info->MajorSubsystemVersion       = pe_info->subsystem_major;
    info->MajorOperatingSystemVersion = pe_info->osversion_major;
    info->MinorOperatingSystemVersion = pe_info->osversion_minor;
    info->ImageCharacteristics        = pe_info->image_charact;
    info->DllCharacteristics          = pe_info->dll_charact;
    info->Machine                     = pe_info->machine;
    info->ImageContainsCode           = pe_info->contains_code;
    info->ImageFlags                  = pe_info->image_flags;
    info->LoaderFlags                 = pe_info->loader_flags;
    info->ImageFileSize               = pe_info->file_size;
    info->CheckSum                    = pe_info->checksum;
#ifndef _WIN64 /* don't return 64-bit values to 32-bit processes */
    if (is_machine_64bit( pe_info->machine ))
    {
        info->TransferAddress = (void *)0x81231234;  /* sic */
        info->MaximumStackSize = 0x100000;
        info->CommittedStackSize = 0x10000;
    }
#endif
}

/******************************************************************************
 *             NtQuerySection   (NTDLL.@)
 *             ZwQuerySection   (NTDLL.@)
 */
NTSTATUS WINAPI NtQuerySection( HANDLE handle, SECTION_INFORMATION_CLASS class, void *ptr,
                                SIZE_T size, SIZE_T *ret_size )
{
    unsigned int status;
    struct pe_image_info image_info;
#ifdef __PROSPERO__
    if ((status = ps5_title_flush_coalesced_write( 0 ))) return status;
#endif

    switch (class)
    {
    case SectionBasicInformation:
        if (size < sizeof(SECTION_BASIC_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        break;
    case SectionImageInformation:
        if (size < sizeof(SECTION_IMAGE_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        break;
    default:
	FIXME( "class %u not implemented\n", class );
	return STATUS_NOT_IMPLEMENTED;
    }
    if (!ptr) return STATUS_ACCESS_VIOLATION;

    SERVER_START_REQ( get_mapping_info )
    {
        req->handle = wine_server_obj_handle( handle );
        req->access = SECTION_QUERY;
        wine_server_set_reply( req, &image_info, sizeof(image_info) );
        if (!(status = wine_server_call( req )))
        {
            if (class == SectionBasicInformation)
            {
                SECTION_BASIC_INFORMATION *info = ptr;
                info->Attributes    = reply->flags;
                info->BaseAddress   = NULL;
                info->Size.QuadPart = reply->size;
                if (ret_size) *ret_size = sizeof(*info);
            }
            else if (reply->flags & SEC_IMAGE)
            {
                SECTION_IMAGE_INFORMATION *info = ptr;
                virtual_fill_image_information( &image_info, info );
                if (ret_size) *ret_size = sizeof(*info);
            }
            else status = STATUS_SECTION_NOT_IMAGE;
        }
    }
    SERVER_END_REQ;

    return status;
}


/***********************************************************************
 *             NtFlushVirtualMemory   (NTDLL.@)
 *             ZwFlushVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtFlushVirtualMemory( HANDLE process, LPCVOID *addr_ptr,
                                      SIZE_T *size_ptr, ULONG unknown )
{
    struct file_view *view;
    unsigned int status = STATUS_SUCCESS;
    sigset_t sigset;
    void *addr = ROUND_ADDR( *addr_ptr, page_mask );
    SIZE_T flush_size = *size_ptr;

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.virtual_flush.type = APC_VIRTUAL_FLUSH;
        call.virtual_flush.addr = wine_server_client_ptr( addr );
        call.virtual_flush.size = flush_size;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.virtual_flush.status == STATUS_SUCCESS)
        {
            *addr_ptr = wine_server_get_ptr( result.virtual_flush.addr );
            *size_ptr = result.virtual_flush.size;
        }
        return result.virtual_flush.status;
    }

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (!(view = find_view( addr, flush_size ))) status = STATUS_INVALID_PARAMETER;
    else
    {
        if (!flush_size) flush_size = view->size;
#ifdef MS_ASYNC
        if (msync( ROUND_ADDR( addr, host_page_mask ), ROUND_SIZE( addr, flush_size, host_page_mask ), MS_ASYNC ))
            status = STATUS_NOT_MAPPED_DATA;
#endif
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    if (!status)
    {
        *addr_ptr = addr;
        *size_ptr = flush_size;
    }
    return status;
}


/***********************************************************************
 *             NtGetWriteWatch   (NTDLL.@)
 *             ZwGetWriteWatch   (NTDLL.@)
 */
NTSTATUS WINAPI NtGetWriteWatch( HANDLE process, ULONG flags, PVOID base, SIZE_T size, PVOID *addresses,
                                 ULONG_PTR *count, ULONG *granularity )
{
    PVOID result_buffer[256], *results = result_buffer;
    ULONG_PTR capacity, result_count;
    NTSTATUS status = STATUS_SUCCESS;
    SIZE_T reset_size;
    sigset_t sigset;

    size = ROUND_SIZE( base, size, page_mask );
    base = ROUND_ADDR( base, page_mask );
    if (!count || !granularity) return STATUS_ACCESS_VIOLATION;
    capacity = *count;
    if (!capacity || !size) return STATUS_INVALID_PARAMETER;
    if (flags & ~WRITE_WATCH_FLAG_RESET) return STATUS_INVALID_PARAMETER;
    if (!addresses) return STATUS_ACCESS_VIOLATION;

    result_count = min( capacity, size / page_size );
    if (result_count > ARRAY_SIZE(result_buffer) &&
        !(results = malloc( result_count * sizeof(*results) )))
        return STATUS_NO_MEMORY;
    TRACE( "%p %x %p-%p %p %lu\n", process, flags, base, (char *)base + size,
           addresses, capacity );

    reset_size = size;
    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    if (is_write_watch_range( base, size ))
    {
        if (use_kernel_writewatch)
            kernel_get_write_watches( base, size, results, &result_count,
                                      flags & WRITE_WATCH_FLAG_RESET );
        else
        {
            ULONG_PTR pos = 0;
            char *addr = base;
            char *end = addr + size;

            while (pos < result_count && addr < end)
            {
                if (!(get_page_vprot( addr ) & VPROT_WRITEWATCH)) results[pos++] = addr;
                addr += page_size;
            }
            reset_size = addr - (char *)base;
            result_count = pos;
        }
        if (flags & WRITE_WATCH_FLAG_RESET && (enable_write_exceptions || !use_kernel_writewatch))
        {
            if (use_kernel_writewatch)
                set_page_vprot_exec_write_protect( base, reset_size );
            else
                set_page_vprot_bits( base, reset_size, VPROT_WRITEWATCH, 0 );
            mprotect_range( base, reset_size, 0, 0 );
        }
    }
    else status = STATUS_INVALID_PARAMETER;
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );

    if (!status)
    {
        if (result_count) memcpy( addresses, results, result_count * sizeof(*results) );
        *count = result_count;
        *granularity = page_size;
    }
    if (results != result_buffer) free( results );
    return status;
}


/***********************************************************************
 *             NtResetWriteWatch   (NTDLL.@)
 *             ZwResetWriteWatch   (NTDLL.@)
 */
NTSTATUS WINAPI NtResetWriteWatch( HANDLE process, PVOID base, SIZE_T size )
{
    NTSTATUS status = STATUS_SUCCESS;
    sigset_t sigset;

    size = ROUND_SIZE( base, size, page_mask );
    base = ROUND_ADDR( base, page_mask );

    TRACE( "%p %p-%p\n", process, base, (char *)base + size );

    if (!size) return STATUS_INVALID_PARAMETER;

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );

    if (is_write_watch_range( base, size ))
        reset_write_watches( base, size );
    else
        status = STATUS_INVALID_PARAMETER;

    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return status;
}


/***********************************************************************
 *             NtReadVirtualMemory   (NTDLL.@)
 *             ZwReadVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtReadVirtualMemory( HANDLE process, const void *addr, void *buffer,
                                     SIZE_T size, SIZE_T *bytes_read )
{
    unsigned int status;

#ifdef __PROSPERO__
    if (__wine_ps5_is_title() && process == GetCurrentProcess())
        return ps5_title_read_self_memory( addr, buffer, size, bytes_read );
#endif
    if (!virtual_check_buffer_for_write( buffer, size ))
    {
        status = STATUS_ACCESS_VIOLATION;
        size = 0;
    }
    else if (process == GetCurrentProcess())
    {
        __TRY
        {
            memmove( buffer, addr, size );
            status = STATUS_SUCCESS;
        }
        __EXCEPT
        {
            status = STATUS_PARTIAL_COPY;
            size = 0;
        }
        __ENDTRY
    }
    else
    {
        SERVER_START_REQ( read_process_memory )
        {
            req->handle = wine_server_obj_handle( process );
            req->addr   = wine_server_client_ptr( addr );
            wine_server_set_reply( req, buffer, size );
            if ((status = wine_server_call( req ))) size = 0;
        }
        SERVER_END_REQ;
    }
    if (bytes_read) *bytes_read = size;
    return status;
}


/***********************************************************************
 *             NtWriteVirtualMemory   (NTDLL.@)
 *             ZwWriteVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtWriteVirtualMemory( HANDLE process, void *addr, const void *buffer,
                                      SIZE_T size, SIZE_T *bytes_written )
{
    unsigned int status;

#ifdef __PROSPERO__
    if (__wine_ps5_is_title() && process == GetCurrentProcess())
        return ps5_title_write_self_memory( addr, buffer, size, bytes_written );
#endif
    if (virtual_check_buffer_for_read( buffer, size ))
    {
        SERVER_START_REQ( write_process_memory )
        {
            req->handle     = wine_server_obj_handle( process );
            req->addr       = wine_server_client_ptr( addr );
            wine_server_add_data( req, buffer, size );
            status = wine_server_call( req );
            size = reply->written;
        }
        SERVER_END_REQ;
    }
    else
    {
        status = STATUS_PARTIAL_COPY;
        size = 0;
    }
    if (bytes_written) *bytes_written = size;
    return status;
}


/***********************************************************************
 *             NtAreMappedFilesTheSame   (NTDLL.@)
 *             ZwAreMappedFilesTheSame   (NTDLL.@)
 */
NTSTATUS WINAPI NtAreMappedFilesTheSame(PVOID addr1, PVOID addr2)
{
    struct file_view *view1, *view2;
    unsigned int status;
    sigset_t sigset;

    TRACE("%p %p\n", addr1, addr2);

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );

    view1 = find_view( addr1, 0 );
    view2 = find_view( addr2, 0 );

    if (!view1 || !view2)
        status = STATUS_INVALID_ADDRESS;
    else if (is_view_valloc( view1 ) || is_view_valloc( view2 ))
        status = STATUS_CONFLICTING_ADDRESSES;
    else if (view1 == view2)
        status = STATUS_SUCCESS;
    else if ((view1->protect & VPROT_SYSTEM) || (view2->protect & VPROT_SYSTEM))
        status = STATUS_NOT_SAME_DEVICE;
    else
    {
        SERVER_START_REQ( is_same_mapping )
        {
            req->base1 = wine_server_client_ptr( view1->base );
            req->base2 = wine_server_client_ptr( view2->base );
            status = wine_server_call( req );
        }
        SERVER_END_REQ;
    }

    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    return status;
}


static NTSTATUS prefetch_memory( HANDLE process, ULONG_PTR count,
                                 PMEMORY_RANGE_ENTRY addresses, ULONG flags )
{
    ULONG_PTR i;
    PVOID base;
    SIZE_T size;
    static unsigned int once;

    if (!once++)
    {
        FIXME( "(process=%p,flags=%u) NtSetInformationVirtualMemory(VmPrefetchInformation) partial stub\n",
                process, flags );
    }

    for (i = 0; i < count; i++)
    {
        if (!addresses[i].NumberOfBytes) return STATUS_INVALID_PARAMETER_4;
    }

    if (process != NtCurrentProcess()) return STATUS_SUCCESS;

    for (i = 0; i < count; i++)
    {
        base = ROUND_ADDR( addresses[i].VirtualAddress, host_page_mask );
        size = ROUND_SIZE( addresses[i].VirtualAddress, addresses[i].NumberOfBytes, host_page_mask );
        madvise( base, size, MADV_WILLNEED );
    }

    return STATUS_SUCCESS;
}

static NTSTATUS set_dirty_state_information( ULONG_PTR count, MEMORY_RANGE_ENTRY *addresses )
{
    MEMORY_RANGE_ENTRY range_buffer[64], *ranges = range_buffer;
    ULONG_PTR i;
    sigset_t sigset;
    NTSTATUS ret = STATUS_SUCCESS;

    if (count > (size_t)-1 / sizeof(*ranges) ||
        !virtual_check_buffer_for_read( addresses, count * sizeof(*ranges) ))
        return STATUS_ACCESS_VIOLATION;
    if (count > ARRAY_SIZE(range_buffer) && !(ranges = malloc( count * sizeof(*ranges) )))
        return STATUS_NO_MEMORY;
    memcpy( ranges, addresses, count * sizeof(*ranges) );

    server_enter_uninterrupted_section( &virtual_mutex, &sigset );
    for (i = 0; i < count; i++)
    {
        void *base = ROUND_ADDR( ranges[i].VirtualAddress, page_mask );
        SIZE_T size = ROUND_SIZE( ranges[i].VirtualAddress, ranges[i].NumberOfBytes, page_mask );
        struct file_view *view = find_view( base, size );

        if (!view)
        {
            ret = STATUS_MEMORY_NOT_ALLOCATED;
            break;
        }
        if (use_kernel_writewatch) reset_write_watches( base, size );
        else if (set_page_vprot_exec_write_protect( base, size ))
            mprotect_range( base, size, 0, 0 );
    }
    server_leave_uninterrupted_section( &virtual_mutex, &sigset );
    if (ranges != range_buffer) free( ranges );
    return ret;
}

/***********************************************************************
 *           NtSetInformationVirtualMemory   (NTDLL.@)
 *           ZwSetInformationVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtSetInformationVirtualMemory( HANDLE process,
                                               VIRTUAL_MEMORY_INFORMATION_CLASS info_class,
                                               ULONG_PTR count, PMEMORY_RANGE_ENTRY addresses,
                                               PVOID ptr, ULONG size )
{
    TRACE("(%p, info_class=%d, %lu, %p, %p, %u)\n",
          process, info_class, count, addresses, ptr, size);

    switch (info_class)
    {
    case VmPrefetchInformation:
        if (!ptr) return STATUS_INVALID_PARAMETER_5;
        if (size != sizeof(ULONG)) return STATUS_INVALID_PARAMETER_6;
        if (!count) return STATUS_INVALID_PARAMETER_3;
        return prefetch_memory( process, count, addresses, *(ULONG *)ptr );

    case VmPageDirtyStateInformation:
        if (process != GetCurrentProcess()) return STATUS_NOT_SUPPORTED;
        if (!enable_write_exceptions) return STATUS_NOT_SUPPORTED;
        if (!ptr) return STATUS_INVALID_PARAMETER_5;
        if (size != sizeof(ULONG)) return STATUS_INVALID_PARAMETER_6;
        if (*(ULONG *)ptr) return STATUS_INVALID_PARAMETER_5;
        if (!count) return STATUS_INVALID_PARAMETER_3;
        return set_dirty_state_information( count, addresses );

    default:
        FIXME("(%p,info_class=%d,%lu,%p,%p,%u) Unknown information class\n",
              process, info_class, count, addresses, ptr, size);
        return STATUS_INVALID_PARAMETER_2;
    }
}


/**********************************************************************
 *           NtFlushInstructionCache  (NTDLL.@)
 */
NTSTATUS WINAPI NtFlushInstructionCache( HANDLE handle, const void *addr, SIZE_T size )
{
#if defined(__x86_64__) || defined(__i386__)
    /* no-op */
#elif defined(HAVE___CLEAR_CACHE)
    if (handle == GetCurrentProcess())
    {
        __clear_cache( (char *)addr, (char *)addr + size );
    }
    else
    {
        static int once;
        if (!once++) FIXME( "%p %p %ld other process not supported\n", handle, addr, size );
    }
#else
    static int once;
    if (!once++) FIXME( "%p %p %ld\n", handle, addr, size );
#endif
    return STATUS_SUCCESS;
}


#ifdef __APPLE__

static kern_return_t (*p_thread_get_register_pointer_values)( thread_t, uintptr_t*, size_t*, uintptr_t* );
static pthread_once_t tgrpvs_init_once = PTHREAD_ONCE_INIT;

static void tgrpvs_init(void)
{
    p_thread_get_register_pointer_values = dlsym( RTLD_DEFAULT, "thread_get_register_pointer_values" );
    if (!p_thread_get_register_pointer_values)
        FIXME( "thread_get_register_pointer_values not supported for NtFlushProcessWriteBuffers\n" );
}

/**********************************************************************
 *           NtFlushProcessWriteBuffers  (NTDLL.@)
 */
NTSTATUS WINAPI NtFlushProcessWriteBuffers(void)
{
    /* Taken from https://github.com/dotnet/runtime/blob/7be37908e5a1cbb83b1062768c1649827eeaceaa/src/coreclr/pal/src/thread/process.cpp#L2799 */
    mach_msg_type_number_t count, i;
    thread_act_array_t threads;

    pthread_once( &tgrpvs_init_once, tgrpvs_init );
    if (!p_thread_get_register_pointer_values) return STATUS_SUCCESS;

    /* Get references to all threads of this process */
    if (task_threads( mach_task_self(), &threads, &count )) return STATUS_SUCCESS;

    for (i = 0; i < count; i++)
    {
        uintptr_t reg_values[128];
        size_t reg_count = ARRAY_SIZE( reg_values );
        uintptr_t sp;

        /* Request the thread's register pointer values to force the thread to go through a memory barrier */
        p_thread_get_register_pointer_values( threads[i], &sp, &reg_count, reg_values );
        mach_port_deallocate( mach_task_self(), threads[i] );
    }
    vm_deallocate( mach_task_self(), (vm_address_t)threads, count * sizeof(threads[0]) );
    return STATUS_SUCCESS;
}

#elif defined(__linux__) && defined(__NR_membarrier)

#define MEMBARRIER_CMD_PRIVATE_EXPEDITED            0x08
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED   0x10

static pthread_once_t membarrier_init_once = PTHREAD_ONCE_INIT;

static int membarrier( int cmd, unsigned int flags, int cpu_id )
{
    return syscall( __NR_membarrier, cmd, flags, cpu_id );
}

static void membarrier_init(void)
{
    if (membarrier( MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0 ))
        FIXME( "membarrier not supported for NtFlushProcessWriteBuffers\n" );
}

/**********************************************************************
 *           NtFlushProcessWriteBuffers  (NTDLL.@)
 */
NTSTATUS WINAPI NtFlushProcessWriteBuffers(void)
{
    pthread_once( &membarrier_init_once, membarrier_init );
    membarrier( MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0 );
    return STATUS_SUCCESS;
}

#else /* __linux__ */

/**********************************************************************
 *           NtFlushProcessWriteBuffers  (NTDLL.@)
 */
NTSTATUS WINAPI NtFlushProcessWriteBuffers(void)
{
    static int once = 0;
    if (!once++) FIXME( "stub\n" );
    return STATUS_SUCCESS;
}

#endif

/**********************************************************************
 *           NtCreatePagingFile  (NTDLL.@)
 */
NTSTATUS WINAPI NtCreatePagingFile( UNICODE_STRING *name, LARGE_INTEGER *min_size,
                                    LARGE_INTEGER *max_size, LARGE_INTEGER *actual_size )
{
#ifdef __PROSPERO__
    NTSTATUS status = ps5_title_flush_coalesced_write( 0 );
    if (status) return status;
#endif
    FIXME( "(%s %p %p %p) stub\n", debugstr_us(name), min_size, max_size, actual_size );
    return STATUS_SUCCESS;
}

#ifndef _WIN64

/***********************************************************************
 *             NtWow64AllocateVirtualMemory64   (NTDLL.@)
 *             ZwWow64AllocateVirtualMemory64   (NTDLL.@)
 */
NTSTATUS WINAPI NtWow64AllocateVirtualMemory64( HANDLE process, ULONG64 *ret, ULONG64 zero_bits,
                                                ULONG64 *size_ptr, ULONG type, ULONG protect )
{
    void *base;
    SIZE_T size;
    unsigned int status;

    TRACE("%p %s %s %x %08x\n", process,
          wine_dbgstr_longlong(*ret), wine_dbgstr_longlong(*size_ptr), type, protect );

    if (!*size_ptr) return STATUS_INVALID_PARAMETER_4;
    if (zero_bits > 21 && zero_bits < 32) return STATUS_INVALID_PARAMETER_3;

    if (process != NtCurrentProcess())
    {
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );

        call.virtual_alloc.type         = APC_VIRTUAL_ALLOC;
        call.virtual_alloc.addr         = *ret;
        call.virtual_alloc.size         = *size_ptr;
        call.virtual_alloc.zero_bits    = zero_bits;
        call.virtual_alloc.op_type      = type;
        call.virtual_alloc.prot         = protect;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.virtual_alloc.status == STATUS_SUCCESS)
        {
            *ret      = result.virtual_alloc.addr;
            *size_ptr = result.virtual_alloc.size;
        }
        return result.virtual_alloc.status;
    }

    base = (void *)(ULONG_PTR)*ret;
    size = *size_ptr;
    if ((ULONG_PTR)base != *ret) return STATUS_CONFLICTING_ADDRESSES;
    if (size != *size_ptr) return STATUS_WORKING_SET_LIMIT_RANGE;

    status = NtAllocateVirtualMemory( process, &base, zero_bits, &size, type, protect );
    if (!status)
    {
        *ret = (ULONG_PTR)base;
        *size_ptr = size;
    }
    TRACE("status %#x.\n", status);
    return status;
}


/***********************************************************************
 *             NtWow64ReadVirtualMemory64   (NTDLL.@)
 *             ZwWow64ReadVirtualMemory64   (NTDLL.@)
 */
NTSTATUS WINAPI NtWow64ReadVirtualMemory64( HANDLE process, ULONG64 addr, void *buffer,
                                            ULONG64 size, ULONG64 *bytes_read )
{
    unsigned int status;

    if (size > MAXLONG) size = MAXLONG;

    if (virtual_check_buffer_for_write( buffer, size ))
    {
        SERVER_START_REQ( read_process_memory )
        {
            req->handle = wine_server_obj_handle( process );
            req->addr   = addr;
            wine_server_set_reply( req, buffer, size );
            if ((status = wine_server_call( req ))) size = 0;
        }
        SERVER_END_REQ;
    }
    else
    {
        status = STATUS_ACCESS_VIOLATION;
        size = 0;
    }
    if (bytes_read) *bytes_read = size;
    return status;
}


/***********************************************************************
 *             NtWow64WriteVirtualMemory64   (NTDLL.@)
 *             ZwWow64WriteVirtualMemory64   (NTDLL.@)
 */
NTSTATUS WINAPI NtWow64WriteVirtualMemory64( HANDLE process, ULONG64 addr, const void *buffer,
                                             ULONG64 size, ULONG64 *bytes_written )
{
    unsigned int status;

    if (size > MAXLONG) size = MAXLONG;

    if (virtual_check_buffer_for_read( buffer, size ))
    {
        SERVER_START_REQ( write_process_memory )
        {
            req->handle     = wine_server_obj_handle( process );
            req->addr       = addr;
            wine_server_add_data( req, buffer, size );
            if ((status = wine_server_call( req ))) size = 0;
        }
        SERVER_END_REQ;
    }
    else
    {
        status = STATUS_PARTIAL_COPY;
        size = 0;
    }
    if (bytes_written) *bytes_written = size;
    return status;
}


/***********************************************************************
 *             NtWow64GetNativeSystemInformation   (NTDLL.@)
 *             ZwWow64GetNativeSystemInformation   (NTDLL.@)
 */
NTSTATUS WINAPI NtWow64GetNativeSystemInformation( SYSTEM_INFORMATION_CLASS class, void *info,
                                                   ULONG len, ULONG *retlen )
{
    NTSTATUS status;

    switch (class)
    {
    case SystemCpuInformation:
        status = NtQuerySystemInformation( class, info, len, retlen );
        if (!status && is_old_wow64())
        {
            SYSTEM_CPU_INFORMATION *cpu = info;

            if (cpu->ProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
                cpu->ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
        }
        return status;
    case SystemBasicInformation:
    case SystemEmulationBasicInformation:
    case SystemEmulationProcessorInformation:
        return NtQuerySystemInformation( class, info, len, retlen );
    case SystemNativeBasicInformation:
        return NtQuerySystemInformation( SystemBasicInformation, info, len, retlen );
    default:
        if (is_old_wow64()) return STATUS_INVALID_INFO_CLASS;
        return NtQuerySystemInformation( class, info, len, retlen );
    }
}

/***********************************************************************
 *             NtWow64IsProcessorFeaturePresent   (NTDLL.@)
 *             ZwWow64IsProcessorFeaturePresent   (NTDLL.@)
 */
NTSTATUS WINAPI NtWow64IsProcessorFeaturePresent( UINT feature )
{
    return feature < PROCESSOR_FEATURE_MAX && user_shared_data->ProcessorFeatures[feature];
}

#endif  /* _WIN64 */
