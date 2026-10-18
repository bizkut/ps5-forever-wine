/* Native title lifecycle helpers. These have no PS5 imports so that their
 * concurrency and descriptor-lifetime contracts can also be tested on a host.
 */
#ifndef __WINE_PS5_TITLE_PRIVATE_H
#define __WINE_PS5_TITLE_PRIVATE_H

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wine/ps5_title.h"
#include "wine/ps5_writebroker.h"

/* Half-open GPU windows, independent of the host pointer width. Return the
 * first intersected window so a caller can split its ordinary free search. */
static inline int wine_ps5_title_radv_overlap( uint64_t start, uint64_t size,
                                              uint64_t *low, uint64_t *high )
{
    static const uint64_t windows[][2] = {
        { UINT64_C(0x200000000), UINT64_C(0x300000000) },
        { UINT64_C(0x4000000000), UINT64_C(0x8000000000) },
    };
    unsigned int i;

    if (!size) return 0;
    for (i = 0; i < sizeof(windows) / sizeof(windows[0]); i++)
        if (start < windows[i][1] && (start >= windows[i][0] || size > windows[i][0] - start))
        {
            if (low) *low = windows[i][0];
            if (high) *high = windows[i][1];
            return 1;
        }
    return 0;
}

/* A temporary opening of a 16 KiB host page containing any sealed 4 KiB
 * neighbour is always non-executable. An allowed execute access must first
 * synthesize a fault for the sealed neighbour; granting host execute access
 * would let encrypted bytes run without a hardware fault. */
static inline int wine_ps5_title_sealed_open_prot( int prot )
{
    return prot & ~PROT_EXEC;
}

static inline int wine_ps5_title_sealed_may_open( int execute_fault )
{
    return !execute_fault;
}

/* Return the fault-free part of one Windows page for a native self-memory
 * copy. The caller walks one page at a time and stops on zero, so a denied,
 * guarded, or PS5-sealed host page becomes the exact partial-copy boundary. */
static inline size_t wine_ps5_title_copy_page_size( uintptr_t address, size_t size,
                                                    uintptr_t page_mask, int committed,
                                                    int guarded, int accessible,
                                                    int host_accessible, int host_closed )
{
    size_t page_left;

    if (!size || !committed || guarded || !accessible || !host_accessible || host_closed)
        return 0;
    page_left = (page_mask + 1) - (address & page_mask);
    return size < page_left ? size : page_left;
}

/* A diagnostic signal may interrupt a syscall that already owns the one Wine
 * fault guard. A nested fault may jump only to a guard created on the active
 * signal stack; otherwise it would abandon the signal handler and its locks. */
static inline int wine_ps5_title_use_signal_fault_guard( int fault_on_signal_stack,
                                                         int guard_on_signal_stack )
{
    return fault_on_signal_stack && guard_on_signal_stack;
}

/* Run 40, run 105-run109, and run 114 measured budget-exhausted small writes
 * at 0.76-0.95 s, while runs 112/117 measured 0.5-0.65 s self-contention
 * stalls. Run 120 measured 0.668-0.674-s report writes while several MiB
 * arrived as <=64-KiB local writes rather than one >=1-MiB write. Require
 * three >=700-ms small writes in five seconds, mask evidence after 1 MiB of
 * cumulative local output within two seconds, and cap cumulative local output
 * at 256 MiB. */
#define WINE_PS5_WRITE_SMALL_MAX          (64u * 1024u)
#define WINE_PS5_WRITE_CONTENTION_BYTES   (1u * 1024u * 1024u)
#define WINE_PS5_WRITE_THROTTLE_US        UINT64_C(700000)
#define WINE_PS5_WRITE_CONTENTION_US      UINT64_C(2000000)
#define WINE_PS5_WRITE_SLOW_WINDOW_US     UINT64_C(5000000)
#define WINE_PS5_WRITE_BROKER_HOLD_US     UINT64_C(60000000)
#define WINE_PS5_WRITE_LOCAL_LIMIT        (UINT64_C(256) << 20)
#define WINE_PS5_WRITE_SLOW_COUNT         3u
#define WINE_PS5_WRITE_LOCAL_NONE         UINT64_MAX

/* Run 175 isolated recurrent 86–553-ms ADB stalls inside local pwrite on an
 * exclusive RW handle. One >=200-ms completion promotes only that handle,
 * not every small write in the process. State lives until handle close. */
#define WINE_PS5_WRITE_STICKY_US UINT64_C(200000)
#define WINE_PS5_WRITE_STICKY_BUCKETS 64u
#define WINE_PS5_WRITE_LOCAL_PROBE 1u
#define WINE_PS5_WRITE_STICKY 2u

struct wine_ps5_sticky_write
{
    struct wine_ps5_sticky_write *next;
    uint32_t handle;
};

struct wine_ps5_sticky_writes
{
    struct wine_ps5_sticky_write *buckets[WINE_PS5_WRITE_STICKY_BUCKETS];
};

/* The caller serializes this sparse set. Only promoted handles allocate a
 * node; hash collisions never evict a live handle's sticky decision. */
static inline struct wine_ps5_sticky_write **wine_ps5_sticky_write_slot(
    struct wine_ps5_sticky_writes *writes, uint32_t handle )
{
    struct wine_ps5_sticky_write **slot =
        &writes->buckets[(handle >> 2) % WINE_PS5_WRITE_STICKY_BUCKETS];

    while (*slot && (*slot)->handle != handle) slot = &(*slot)->next;
    return slot;
}

static inline int wine_ps5_sticky_write_contains( struct wine_ps5_sticky_writes *writes, uint32_t handle )
{
    return !!*wine_ps5_sticky_write_slot( writes, handle );
}

static inline int wine_ps5_sticky_write_candidate( uint64_t local_us, int64_t written, int regular )
{
    return regular && written > 0 && local_us >= WINE_PS5_WRITE_STICKY_US;
}

/* Return 1 for a newly promoted handle, 0 if already present/ineligible, -1
 * on allocation failure (ordinary routing remains available). */
static inline int wine_ps5_sticky_write_promote( struct wine_ps5_sticky_writes *writes, uint32_t handle,
                                                const char *path )
{
    struct wine_ps5_sticky_write **slot;

    if (!wb_path_root( path )) return 0;
    slot = wine_ps5_sticky_write_slot( writes, handle );
    if (*slot) return 0;
    if (!(*slot = malloc( sizeof(**slot) ))) return -1;
    (*slot)->handle = handle;
    (*slot)->next = NULL;
    return 1;
}

static inline int wine_ps5_sticky_write_forget( struct wine_ps5_sticky_writes *writes, uint32_t handle )
{
    struct wine_ps5_sticky_write **slot = wine_ps5_sticky_write_slot( writes, handle ), *old = *slot;

    if (!old) return 0;
    *slot = old->next;
    free( old );
    return 1;
}

static inline uint64_t wine_ps5_title_write_local_total( uint64_t total, uint64_t written )
{
    if (total >= WINE_PS5_WRITE_LOCAL_LIMIT ||
        written >= WINE_PS5_WRITE_LOCAL_LIMIT - total)
        return WINE_PS5_WRITE_LOCAL_LIMIT;
    return total + written;
}

enum wine_ps5_title_write_mode
{
    WINE_PS5_WRITE_NORMAL,
    WINE_PS5_WRITE_THROTTLED,
    WINE_PS5_WRITE_PROBING,
    WINE_PS5_WRITE_VOLUME
};

enum wine_ps5_title_write_route
{
    WINE_PS5_WRITE_LOCAL,
    WINE_PS5_WRITE_BROKER
};

enum wine_ps5_title_write_transition
{
    WINE_PS5_WRITE_ROUTE_UNCHANGED,
    WINE_PS5_WRITE_SMALL_TO_BROKER,
    WINE_PS5_WRITE_SMALL_TO_LOCAL,
    WINE_PS5_WRITE_VOLUME_TO_BROKER
};

struct wine_ps5_title_write_state
{
    enum wine_ps5_title_write_mode mode;
    uint64_t broker_until_us;
    uint64_t slow_first_us;
    uint64_t slow_second_us;
    uint64_t local_window_first_us;
    uint64_t local_window_bytes;
    uint64_t contention_us;
    unsigned int slow_count;
};

#define WINE_PS5_TITLE_WRITE_STATE_INIT { .mode = WINE_PS5_WRITE_NORMAL }

struct wine_ps5_title_write_decision
{
    struct wine_ps5_title_write_state state;
    enum wine_ps5_title_write_route route;
    enum wine_ps5_title_write_transition transition;
    int probe;
};

static inline int wine_ps5_title_write_slow_within( uint64_t now, uint64_t earlier )
{
    return now >= earlier && now - earlier <= WINE_PS5_WRITE_SLOW_WINDOW_US;
}

/* Pure routing transition. WINE_PS5_WRITE_LOCAL_NONE means this call only
 * selects a route. last_local_bytes is the successful output from the
 * completed local write, and local_total_bytes is the process's cumulative
 * successful local output. flags identifies an intentional local probe and/or
 * a sticky handle. Sticky writes must not consume the process's local probe. */
static inline struct wine_ps5_title_write_decision wine_ps5_title_write_route(
    struct wine_ps5_title_write_state state, size_t size, uint64_t last_local_us,
    uint64_t last_local_bytes, uint64_t local_total_bytes, uint64_t now_us,
    int broker_available, unsigned int flags )
{
    struct wine_ps5_title_write_decision ret =
    {
        .state = state,
        .transition = WINE_PS5_WRITE_ROUTE_UNCHANGED
    };
    int slow, self_contention;

    if (ret.state.mode != WINE_PS5_WRITE_VOLUME &&
        last_local_us != WINE_PS5_WRITE_LOCAL_NONE && last_local_bytes)
    {
        if (!ret.state.local_window_bytes ||
            now_us < ret.state.local_window_first_us ||
            now_us - ret.state.local_window_first_us > WINE_PS5_WRITE_CONTENTION_US)
        {
            ret.state.local_window_first_us = now_us;
            ret.state.local_window_bytes = 0;
        }

        if (last_local_bytes >= WINE_PS5_WRITE_CONTENTION_BYTES -
                                ret.state.local_window_bytes)
        {
            ret.state.local_window_first_us = 0;
            ret.state.local_window_bytes = 0;
            ret.state.contention_us = now_us;
            ret.state.slow_first_us = ret.state.slow_second_us = 0;
            ret.state.slow_count = 0;
        }
        else
            ret.state.local_window_bytes += last_local_bytes;
    }

    if (ret.state.mode != WINE_PS5_WRITE_VOLUME &&
        local_total_bytes >= WINE_PS5_WRITE_LOCAL_LIMIT)
    {
        ret.state.mode = WINE_PS5_WRITE_VOLUME;
        ret.state.broker_until_us = 0;
        ret.state.slow_first_us = ret.state.slow_second_us = 0;
        ret.state.local_window_first_us = ret.state.local_window_bytes = 0;
        ret.state.slow_count = 0;
        ret.transition = WINE_PS5_WRITE_VOLUME_TO_BROKER;
    }
    else if (ret.state.mode != WINE_PS5_WRITE_VOLUME &&
             last_local_us != WINE_PS5_WRITE_LOCAL_NONE)
    {
        slow = last_local_us >= WINE_PS5_WRITE_THROTTLE_US;
        self_contention = ret.state.contention_us && now_us >= ret.state.contention_us &&
                          now_us - ret.state.contention_us <= WINE_PS5_WRITE_CONTENTION_US;
        if (ret.state.mode == WINE_PS5_WRITE_PROBING)
        {
            ret.state.slow_first_us = ret.state.slow_second_us = 0;
            ret.state.slow_count = 0;
            if (slow && size <= WINE_PS5_WRITE_SMALL_MAX && !self_contention)
            {
                ret.state.mode = WINE_PS5_WRITE_THROTTLED;
                ret.state.broker_until_us =
                    now_us > UINT64_MAX - WINE_PS5_WRITE_BROKER_HOLD_US
                    ? UINT64_MAX : now_us + WINE_PS5_WRITE_BROKER_HOLD_US;
            }
            else if (flags & WINE_PS5_WRITE_LOCAL_PROBE)
            {
                ret.state.mode = WINE_PS5_WRITE_NORMAL;
                ret.state.broker_until_us = 0;
                ret.transition = WINE_PS5_WRITE_SMALL_TO_LOCAL;
            }
        }
        if (ret.state.mode == WINE_PS5_WRITE_NORMAL && slow &&
            size <= WINE_PS5_WRITE_SMALL_MAX && !self_contention)
        {
            if (!ret.state.slow_count)
            {
                ret.state.slow_first_us = now_us;
                ret.state.slow_count = 1;
            }
            else if (ret.state.slow_count == 1)
            {
                if (wine_ps5_title_write_slow_within( now_us, ret.state.slow_first_us ))
                {
                    ret.state.slow_second_us = now_us;
                    ret.state.slow_count = 2;
                }
                else
                    ret.state.slow_first_us = now_us;
            }
            else if (ret.state.slow_count + 1 == WINE_PS5_WRITE_SLOW_COUNT &&
                     wine_ps5_title_write_slow_within( now_us, ret.state.slow_first_us ))
            {
                ret.state.mode = WINE_PS5_WRITE_THROTTLED;
                ret.state.broker_until_us =
                    now_us > UINT64_MAX - WINE_PS5_WRITE_BROKER_HOLD_US
                    ? UINT64_MAX : now_us + WINE_PS5_WRITE_BROKER_HOLD_US;
                ret.state.slow_first_us = ret.state.slow_second_us = 0;
                ret.state.slow_count = 0;
                ret.transition = WINE_PS5_WRITE_SMALL_TO_BROKER;
            }
            else if (wine_ps5_title_write_slow_within( now_us, ret.state.slow_second_us ))
            {
                ret.state.slow_first_us = ret.state.slow_second_us;
                ret.state.slow_second_us = now_us;
                ret.state.slow_count = 2;
            }
            else
            {
                ret.state.slow_first_us = now_us;
                ret.state.slow_second_us = 0;
                ret.state.slow_count = 1;
            }
        }
    }

    if (!(flags & WINE_PS5_WRITE_STICKY) && ret.state.mode == WINE_PS5_WRITE_THROTTLED &&
        now_us >= ret.state.broker_until_us && size <= WINE_PS5_WRITE_SMALL_MAX)
    {
        ret.state.mode = WINE_PS5_WRITE_PROBING;
        ret.state.slow_first_us = ret.state.slow_second_us = 0;
        ret.state.slow_count = 0;
        ret.probe = 1;
    }

    if (!broker_available || ret.probe ||
        (!(flags & WINE_PS5_WRITE_STICKY) &&
         ret.state.mode == WINE_PS5_WRITE_NORMAL && size <= WINE_PS5_WRITE_SMALL_MAX))
        ret.route = WINE_PS5_WRITE_LOCAL;
    else
        ret.route = WINE_PS5_WRITE_BROKER;
    return ret;
}

/* Opt-in write diagnostics: bounded per-handle history, not file contents or
 * names. Counters describe API attempts between routed writes, including a
 * call that drains the coalescer before performing its own operation. */
enum wine_ps5_write_trace_op
{
    WINE_PS5_WRITE_TRACE_FLUSH,
    WINE_PS5_WRITE_TRACE_EOF,
    WINE_PS5_WRITE_TRACE_ALLOC,
    WINE_PS5_WRITE_TRACE_READ,
    WINE_PS5_WRITE_TRACE_SEEK,
    WINE_PS5_WRITE_TRACE_QUERY,
    WINE_PS5_WRITE_TRACE_OPS
};

struct wine_ps5_write_trace_history
{
    uint32_t calls[WINE_PS5_WRITE_TRACE_OPS];
    uint32_t inflight;
    uint64_t previous_us;
    int64_t previous_end;
};

static inline void wine_ps5_write_trace_note( struct wine_ps5_write_trace_history *history,
                                             enum wine_ps5_write_trace_op op )
{
    if (history->calls[op] != UINT32_MAX) history->calls[op]++;
}

static inline struct wine_ps5_write_trace_history wine_ps5_write_trace_take(
    struct wine_ps5_write_trace_history *history )
{
    struct wine_ps5_write_trace_history previous = *history;

    memset( history->calls, 0, sizeof(history->calls) );
    history->inflight++;
    return previous;
}

static inline void wine_ps5_write_trace_complete( struct wine_ps5_write_trace_history *history,
                                                 uint64_t now_us, int64_t end )
{
    if (history->inflight) history->inflight--;
    history->previous_us = now_us;
    history->previous_end = end;
}

/* Case-insensitive components; classifying a path never prints or retains it. */
static inline int wine_ps5_write_trace_component( const char *path, const char *component )
{
    while (*component)
    {
        unsigned char ch = *path++;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != *component++) return 0;
    }
    return !*path || *path == '/';
}

static inline const char *wine_ps5_write_trace_class( const char *path )
{
    const char *p;
    size_t root;

    if (!path) return "unknown";
    if (!(root = wb_path_root( path ))) return "outside-title";
    for (p = path + root; *p; p++)
    {
        if (p[-1] != '/' || !wine_ps5_write_trace_component( p, "cache" )) continue;
        p += 5;
        if (*p == '/' && wine_ps5_write_trace_component( p + 1, "adb" )) return "title-adb";
        return "title-cache";
    }
    return "title-other";
}

/* Bounded process-wide extent slots, only for exact WoW CASC data.NNN archives. */
#define WINE_PS5_TITLE_BUFFER_MAX (1024u * 1024u)
#define WINE_PS5_TITLE_BUFFER_RUNS 8u
#define WINE_PS5_TITLE_BUFFER_EXTENTS_PER_HANDLE 4u
#define WINE_PS5_TITLE_BUFFER_TOTAL_MAX \
    (WINE_PS5_TITLE_BUFFER_RUNS * WINE_PS5_TITLE_BUFFER_MAX)
#define WINE_PS5_TITLE_BUFFER_RESIDENCE_US UINT64_C(40000)

enum wine_ps5_title_buffer_barrier
{
    WINE_PS5_TITLE_BUFFER_BARRIER_OTHER,
    WINE_PS5_TITLE_BUFFER_BARRIER_READ,
    WINE_PS5_TITLE_BUFFER_BARRIER_QUERY,
    WINE_PS5_TITLE_BUFFER_BARRIER_NONDATA_WRITE,
    WINE_PS5_TITLE_BUFFER_BARRIER_CLOSE,
    WINE_PS5_TITLE_BUFFER_BARRIER_SECTION,
    WINE_PS5_TITLE_BUFFER_BARRIER_PROCESS,
    WINE_PS5_TITLE_BUFFER_BARRIER_CREATE_OPEN,
    WINE_PS5_TITLE_BUFFER_BARRIER_ATTRIBUTES,
    WINE_PS5_TITLE_BUFFER_BARRIER_DIRECTORY,
    WINE_PS5_TITLE_BUFFER_BARRIER_VOLUME,
    WINE_PS5_TITLE_BUFFER_BARRIER_LOCK_FLUSH,
    WINE_PS5_TITLE_BUFFER_BARRIER_CONTROL,
    WINE_PS5_TITLE_BUFFER_BARRIER_CANCEL,
    WINE_PS5_TITLE_BUFFER_BARRIER_DUPLICATE,
    WINE_PS5_TITLE_BUFFER_BARRIER_NAMESPACE,
    WINE_PS5_TITLE_BUFFER_BARRIER_VIRTUAL,
    WINE_PS5_TITLE_BUFFER_EVENT_NONFILE_WRITE,
    WINE_PS5_TITLE_BUFFER_EVENT_CLOSE_UNRELATED,
    WINE_PS5_TITLE_BUFFER_BARRIERS
};

struct wine_ps5_title_buffer_state
{
    uintptr_t handle;
    uint64_t end;
    size_t length;
};

enum wine_ps5_title_buffer_action
{
    WINE_PS5_TITLE_BUFFER_START,
    WINE_PS5_TITLE_BUFFER_APPEND,
    WINE_PS5_TITLE_BUFFER_FLUSH_DIRECT
};

static inline unsigned char wine_ps5_title_buffer_lower( unsigned char ch )
{
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

static inline int wine_ps5_title_buffer_component( const char *text, size_t length,
                                                   const char *expected )
{
    size_t i;
    for (i = 0; i < length && expected[i]; i++)
        if (wine_ps5_title_buffer_lower( text[i] ) != (unsigned char)expected[i]) return 0;
    return i == length && !expected[i];
}

static inline int wine_ps5_title_buffer_under_root( const char *path, const char *root )
{
    size_t length;
    if (!path || !root || !(length = strlen( root )) || strncmp( path, root, length )) return 0;
    return !path[length] || path[length] == '/';
}

static inline int wine_ps5_title_buffer_path( const char *path, const char *c_root,
                                              const char *w_root )
{
    const char *part, *end, *next, *next_end, *leaf, *leaf_end, *name, *name_end;

    if (!wine_ps5_title_buffer_under_root( path, c_root ) &&
        !wine_ps5_title_buffer_under_root( path, w_root )) return 0;
    for (part = path; *part; part = *end ? end + 1 : end)
    {
        while (*part == '/') part++;
        for (end = part; *end && *end != '/'; end++);
        if (!wine_ps5_title_buffer_component( part, end - part, "world of warcraft" )) continue;
        next = end;
        while (*next == '/') next++;
        for (next_end = next; *next_end && *next_end != '/'; next_end++);
        if (next_end - next >= 3 && next[0] == '_' && next_end[-1] == '_')
        {
            next = next_end;
            while (*next == '/') next++;
            for (next_end = next; *next_end && *next_end != '/'; next_end++);
        }
        if (!wine_ps5_title_buffer_component( next, next_end - next, "data" )) continue;
        leaf = next_end;
        while (*leaf == '/') leaf++;
        for (leaf_end = leaf; *leaf_end && *leaf_end != '/'; leaf_end++);
        if (!wine_ps5_title_buffer_component( leaf, leaf_end - leaf, "data" )) continue;
        name = leaf_end;
        while (*name == '/') name++;
        for (name_end = name; *name_end && *name_end != '/'; name_end++);
        return !*name_end && name_end - name == 8 &&
               wine_ps5_title_buffer_lower( name[0] ) == 'd' &&
               wine_ps5_title_buffer_lower( name[1] ) == 'a' &&
               wine_ps5_title_buffer_lower( name[2] ) == 't' &&
               wine_ps5_title_buffer_lower( name[3] ) == 'a' &&
               name[4] == '.' && name[5] >= '0' && name[5] <= '9' &&
               name[6] >= '0' && name[6] <= '9' && name[7] >= '0' && name[7] <= '9';
    }
    return 0;
}

static inline enum wine_ps5_title_buffer_action wine_ps5_title_buffer_decide(
    const struct wine_ps5_title_buffer_state *state, uintptr_t handle, uint64_t offset,
    size_t size, int eligible )
{
    if (!eligible || !size || size > WINE_PS5_TITLE_BUFFER_MAX)
        return WINE_PS5_TITLE_BUFFER_FLUSH_DIRECT;
    if (!state->length) return WINE_PS5_TITLE_BUFFER_START;
    if (state->handle == handle && state->end == offset &&
        size <= WINE_PS5_TITLE_BUFFER_MAX - state->length)
        return WINE_PS5_TITLE_BUFFER_APPEND;
    return WINE_PS5_TITLE_BUFFER_FLUSH_DIRECT;
}

static inline int wine_ps5_title_buffer_idle( size_t length, uint64_t first_us, uint64_t now_us )
{
    return length && now_us >= first_us &&
           now_us - first_us >= WINE_PS5_TITLE_BUFFER_RESIDENCE_US;
}

static inline int wine_ps5_title_buffer_disable_for_section( int image, int resolved, int scoped )
{
    return !image && (!resolved || scoped);
}

static inline int wine_ps5_title_buffer_single_link( uint64_t links )
{
    /* Prospero may report zero for an ordinary title-storage file. Links
     * created in this process disable buffering before creation; pre-existing
     * or cross-process aliases are the documented residual exclusion. */
    return links <= 1;
}


static inline int wine_ps5_title_buffer_rename_may_enter_scope(
    const char *path, const char *c_root, const char *w_root, int directory )
{
    if (!wine_ps5_title_buffer_under_root( path, c_root ) &&
        !wine_ps5_title_buffer_under_root( path, w_root )) return 0;
    /*
     * A renamed directory may contain an unseen World of Warcraft subtree;
     * without an identity table, every directory rename on C:/W: is the
     * conservative boundary. Files require the exact archive destination.
     */
    return directory || wine_ps5_title_buffer_path( path, c_root, w_root );
}

static inline int wine_ps5_title_buffer_same_identity( uint64_t left_dev, uint64_t left_ino,
                                                       uint64_t right_dev, uint64_t right_ino )
{
    return left_dev == right_dev && left_ino == right_ino;
}

static inline int wine_ps5_title_buffer_admit( int shutdown, int disabled, int enabled )
{
    return !shutdown && !disabled && enabled;
}

/* Like server_block_set, a title's native critical sections must defer SIGQUIT
 * (Wine's thread-termination handler). They also defer every other asynchronous
 * terminating/exception signal. Synchronous faults remain observable. */
static inline void wine_ps5_title_critical_signals( sigset_t *signals )
{
    sigfillset( signals );
    sigdelset( signals, SIGSEGV );
    sigdelset( signals, SIGBUS );
    sigdelset( signals, SIGILL );
    sigdelset( signals, SIGFPE );
    sigdelset( signals, SIGTRAP );
#ifdef SIGURG
    sigdelset( signals, SIGURG );
#endif
#ifdef SIGSYS
    sigdelset( signals, SIGSYS );
#endif
}

struct wine_ps5_title_lock
{
    pthread_mutex_t *mutex;
    sigset_t signals;
};

static inline void wine_ps5_title_lock_mutex( struct wine_ps5_title_lock *state,
                                             pthread_mutex_t *mutex, const sigset_t *blocked )
{
    state->mutex = mutex;
    pthread_sigmask( SIG_BLOCK, blocked, &state->signals );
    pthread_mutex_lock( mutex );
}

/* Also usable as a pthread cleanup handler when a server disconnect ends
 * a Wine client without returning from the request made under this lock. */
static inline void wine_ps5_title_unlock_mutex( void *arg )
{
    struct wine_ps5_title_lock *state = arg;

    pthread_mutex_unlock( state->mutex );
    pthread_sigmask( SIG_SETMASK, &state->signals, NULL );
}

#if defined(WINE_UNIX_LIB) && defined(__TRY)
/* The Wine Unix fault guard returns to this live frame, not to the syscall
 * dispatcher. Callers never need pthread cleanup to handle a user fault. */
static inline int wine_ps5_title_fault_safe_copy( void *dest, const void *src, size_t size )
{
    __TRY
    {
        memcpy( dest, src, size );
    }
    __EXCEPT
    {
        return -1;
    }
    __ENDTRY
    return 0;
}
#endif
/* Match the final component of a UTF-16 image path against the trusted
 * profile's semicolon-separated ASCII basenames. Return the profile spelling
 * so a caller can log it without exposing the path or command line. */
static inline const char *wine_ps5_title_refused_program( const uint16_t *image, size_t length,
                                                          const char *programs, size_t *match_length )
{
    const uint16_t *base, *end, *p;
    const char *entry, *entry_end;
    size_t image_length, entry_length, i;

    if (match_length) *match_length = 0;
    if (!image || !length || !programs || !*programs) return NULL;
    base = image;
    end = image + length;
    for (p = image; p < end; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    image_length = end - base;

    for (entry = programs; *entry; entry = entry_end + 1)
    {
        for (entry_end = entry; *entry_end && *entry_end != ';'; entry_end++);
        entry_length = entry_end - entry;
        if (entry_length && entry_length <= 255 && entry_length == image_length)
        {
            for (i = 0; i < entry_length; i++)
            {
                unsigned int wide = base[i], narrow = (unsigned char)entry[i];

                if (wide >= 'A' && wide <= 'Z') wide += 'a' - 'A';
                if (narrow >= 'A' && narrow <= 'Z') narrow += 'a' - 'A';
                if (wide != narrow) break;
            }
            if (i == entry_length)
            {
                if (match_length) *match_length = entry_length;
                return entry;
            }
        }
        if (!*entry_end) break;
    }
    return NULL;
}

/* Restrict the scan workspace optimization to Agent.exe. Reuse
 * the existing exact, case-insensitive UTF-16 image-leaf rule, not argv[0]. */
static inline int wine_ps5_title_direct_cache_image( const uint16_t *image, size_t length )
{
    return wine_ps5_title_refused_program( image, length, "Agent.exe", NULL ) != NULL;
}

/* Battle.net's real WoW game images are drive-qualified Wow*.exe files with
 * a sibling <name>_loader.dll. The caller supplies the translated Unix image
 * and performs the filesystem check, keeping this predicate host-testable. */
static inline int wine_ps5_title_game_image( const char *dos, const char *unix_image,
                                             int (*exists)( const char * ))
{
    const char *base, *dot, *slash;
    char loader[4096];
    size_t prefix, length;

    if (!dos || !unix_image || !exists ||
        !((dos[0] == 'C' || dos[0] == 'c' || dos[0] == 'W' || dos[0] == 'w') &&
          dos[1] == ':' && (dos[2] == '\\' || dos[2] == '/')))
        return 0;
    base = dos;
    for (const char *p = dos; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    length = strlen( base );
    if (length < 7 || !((base[0] | 32) == 'w' && (base[1] | 32) == 'o' &&
                        (base[2] | 32) == 'w')) return 0;
    dot = base + length - 4;
    if ((dot[0] | 32) != '.' || (dot[1] | 32) != 'e' ||
        (dot[2] | 32) != 'x' || (dot[3] | 32) != 'e') return 0;
    slash = strrchr( unix_image, '/' );
    prefix = slash ? (size_t)(slash + 1 - unix_image) : 0;
    length = strlen( slash ? slash + 1 : unix_image );
    if (length < 4 || prefix + length - 4 + sizeof("_loader.dll") > sizeof(loader)) return 0;
    memcpy( loader, unix_image, prefix + length - 4 );
    memcpy( loader + prefix + length - 4, "_loader.dll", sizeof("_loader.dll") );
    if (exists( loader )) return 1;
    /* Blizzard ships the final WoW.exe beside lowercase wow_loader.dll,
     * while beta uses WowB_loader.dll. Try the derived spelling first, then
     * a lowercase basename without changing the directory path. */
    for (size_t i = prefix; loader[i]; ++i)
        if (loader[i] >= 'A' && loader[i] <= 'Z') loader[i] += 'a' - 'A';
    return exists( loader );
}

/* Title-only encoding in get_directory_entries.max_count. The ordinary Wine
 * protocol has no restart bit because the server enumeration is index-based. */
#define WINE_PS5_DIRECTORY_RESTART_FLAG UINT32_C(0x80000000)
#define WINE_PS5_DIRECTORY_COUNT_MASK   UINT32_C(0x7fffffff)

#define WINE_PS5_DIRQUERY_SPIN_THRESHOLD 10000
#define WINE_PS5_DIRQUERY_SPIN_DELAY_US 1000
#define WINE_PS5_DIRQUERY_SPIN_REPORT 0x01
#define WINE_PS5_DIRQUERY_SPIN_DELAY 0x02
#define WINE_PS5_DIRQUERY_MITIGATION_REPORT 0x04

struct wine_ps5_title_dirquery_spin
{
    uintptr_t handle;
    uint32_t index;
    uint32_t calls;
    unsigned int reported : 1;
    unsigned int mitigation_reported : 1;
};

/* Track one thread's consecutive input key. Reporting is lifetime-once for
 * that thread, while changing either key immediately stops the delay. */
static inline unsigned int wine_ps5_title_dirquery_spin( struct wine_ps5_title_dirquery_spin *state,
                                                         uintptr_t handle, uint32_t index )
{
    unsigned int action = 0;

    if (!state->calls || state->handle != handle || state->index != index)
    {
        state->handle = handle;
        state->index = index;
        state->calls = 1;
    }
    else if (state->calls != UINT32_MAX) state->calls++;

    if (state->calls < WINE_PS5_DIRQUERY_SPIN_THRESHOLD) return 0;
    action |= WINE_PS5_DIRQUERY_SPIN_DELAY;
    if (!state->reported)
    {
        state->reported = 1;
        action |= WINE_PS5_DIRQUERY_SPIN_REPORT;
    }
    if (!state->mitigation_reported)
    {
        state->mitigation_reported = 1;
        action |= WINE_PS5_DIRQUERY_MITIGATION_REPORT;
    }
    return action;
}


struct wine_ps5_title_directory_output
{
    size_t used, previous;
    int have_entry;
};

/* Directory structures all start with a 32-bit NextEntryOffset. Assembly is
 * native-only; guarded copies take place after leaving the shared cwd lock.
 * Commit the cursor only when every user write for this entry succeeded. */
static inline int wine_ps5_title_append_directory_output( struct wine_ps5_title_directory_output *state,
                                                          void *dest, void *information,
                                                          const void *entry, size_t size, size_t alignment,
                                                          int (*copy)( void *, const void *, size_t ))
{
    size_t start = (state->used + alignment - 1) & ~(alignment - 1), end = start + size;
    uint32_t next = start - state->previous;

    if (copy( (char *)dest + start, entry, size ) || copy( information, &end, sizeof(end) )) return -1;
    if (state->have_entry && copy( (char *)dest + state->previous, &next, sizeof(next) )) return -1;
    state->used = end;
    state->previous = start;
    state->have_entry = 1;
    return 0;
}

struct wine_ps5_title_exit
{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int requested;
    int status;
};

#define WINE_PS5_TITLE_EXIT_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 }

/* The first request in mutex order owns the terminal status. Requesters never
 * wait for the server; only the runtime-owning supervisor performs shutdown. */
static inline void wine_ps5_title_request_exit( struct wine_ps5_title_exit *state, int status )
{
    pthread_mutex_lock( &state->mutex );
    if (!state->requested)
    {
        state->status = status;
        state->requested = 1;
        pthread_cond_signal( &state->condition );
    }
    pthread_mutex_unlock( &state->mutex );
}

static inline int wine_ps5_title_wait_exit( struct wine_ps5_title_exit *state )
{
    int status;

    pthread_mutex_lock( &state->mutex );
    while (!state->requested) pthread_cond_wait( &state->condition, &state->mutex );
    status = state->status;
    pthread_mutex_unlock( &state->mutex );
    return status;
}

static inline int wine_ps5_title_supervise_exit( struct wine_ps5_title_exit *state, void (*stop)(void) )
{
    int status = wine_ps5_title_wait_exit( state );

    /* Never retain the status lock while stopping/joining its producer. */
    stop();
    return status;
}

/* Lives in ntdll, not the loader ELF. Closing entry and waiting for every
 * return to ntdll makes the loader's publish callback safe to unmap. */
struct wine_ps5_title_callbacks
{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned int active;
    int closed;
};

#define WINE_PS5_TITLE_CALLBACKS_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 }

static inline int wine_ps5_title_enter_callback( struct wine_ps5_title_callbacks *state )
{
    int entered;

    pthread_mutex_lock( &state->mutex );
    if ((entered = !state->closed)) state->active++;
    pthread_mutex_unlock( &state->mutex );
    return entered;
}

static inline void wine_ps5_title_leave_callback( struct wine_ps5_title_callbacks *state )
{
    pthread_mutex_lock( &state->mutex );
    if (!--state->active) pthread_cond_broadcast( &state->condition );
    pthread_mutex_unlock( &state->mutex );
}

static inline void wine_ps5_title_close_callbacks( struct wine_ps5_title_callbacks *state )
{
    pthread_mutex_lock( &state->mutex );
    state->closed = 1;
    pthread_cond_broadcast( &state->condition );
    while (state->active) pthread_cond_wait( &state->condition, &state->mutex );
    pthread_mutex_unlock( &state->mutex );
}

struct wine_ps5_title_lifetimes
{
    pthread_mutex_t mutex;
    uint64_t next;
};

#define WINE_PS5_TITLE_LIFETIMES_INIT { PTHREAD_MUTEX_INITIALIZER, 1 }

/* Zero means exhausted/unregistered. Even failed native thread creation
 * consumes its ticket; a retired lifetime can never be assigned again. */
static inline uint64_t wine_ps5_title_new_lifetime( struct wine_ps5_title_lifetimes *state )
{
    uint64_t lifetime;

    pthread_mutex_lock( &state->mutex );
    lifetime = state->next;
    if (lifetime) state->next++;
    pthread_mutex_unlock( &state->mutex );
    return lifetime;
}

/* Wine may reap its initial Windows thread before process shutdown. Its
 * loader frame and SDK stack must nevertheless be joined exactly once,
 * whether that reaper or the supervisor gets here first. */
struct wine_ps5_title_join
{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    uint64_t lifetime;
    int joining, joined, result;
};

#define WINE_PS5_TITLE_JOIN_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0 }

static inline int wine_ps5_title_join_once( struct wine_ps5_title_join *state,
                                           int (*join)( pthread_t, void ** ))
{
    int result;

    pthread_mutex_lock( &state->mutex );
    while (state->joining) pthread_cond_wait( &state->condition, &state->mutex );
    if (state->joined)
    {
        result = state->result;
        pthread_mutex_unlock( &state->mutex );
        return result;
    }
    state->joining = 1;
    pthread_mutex_unlock( &state->mutex );
    result = join( state->thread, NULL );
    pthread_mutex_lock( &state->mutex );
    state->result = result;
    state->joined = 1;
    state->joining = 0;
    pthread_cond_broadcast( &state->condition );
    pthread_mutex_unlock( &state->mutex );
    return result;
}

enum wine_ps5_title_startup_channel
{
    WINE_PS5_TITLE_STARTUP_MESSAGE,
    WINE_PS5_TITLE_STARTUP_REQUEST,
    WINE_PS5_TITLE_STARTUP_REPLY_READ,
    WINE_PS5_TITLE_STARTUP_REPLY_WRITE,
    WINE_PS5_TITLE_STARTUP_WAIT_READ,
    WINE_PS5_TITLE_STARTUP_WAIT_WRITE,
    WINE_PS5_TITLE_STARTUP_CHANNEL_COUNT
};

struct wine_ps5_title_startup
{
    pthread_mutex_t mutex;
    int cancelled, complete;
    int channel[WINE_PS5_TITLE_STARTUP_CHANNEL_COUNT];
};

#define WINE_PS5_TITLE_STARTUP_INIT { PTHREAD_MUTEX_INITIALIZER, 0, 0, { -1, -1, -1, -1, -1, -1 } }

/* Own duplicates, not the client's descriptor numbers. shutdown affects all
 * aliases of each socket, including a reply writer retained by the client. */
static inline int wine_ps5_title_publish_startup_channel( struct wine_ps5_title_startup *state,
                                                          unsigned int channel, int fd )
{
    int copy, error;

    pthread_mutex_lock( &state->mutex );
    if (state->cancelled)
    {
        shutdown( fd, SHUT_RDWR );
        pthread_mutex_unlock( &state->mutex );
        errno = ECANCELED;
        return -1;
    }
    if (state->complete)
    {
        pthread_mutex_unlock( &state->mutex );
        return 0;
    }
    if ((copy = dup( fd )) == -1)
    {
        error = errno;
        pthread_mutex_unlock( &state->mutex );
        errno = error;
        return -1;
    }
    fcntl( copy, F_SETFD, FD_CLOEXEC );
    if (state->channel[channel] != -1) close( state->channel[channel] );
    state->channel[channel] = copy;
    pthread_mutex_unlock( &state->mutex );
    return 0;
}

static inline void wine_ps5_title_cancel_startup( struct wine_ps5_title_startup *state )
{
    unsigned int i;

    pthread_mutex_lock( &state->mutex );
    state->cancelled = 1;
    for (i = 0; i < WINE_PS5_TITLE_STARTUP_CHANNEL_COUNT; i++)
        if (state->channel[i] != -1)
        {
            shutdown( state->channel[i], SHUT_RDWR );
            close( state->channel[i] );
            state->channel[i] = -1;
        }
    pthread_mutex_unlock( &state->mutex );
}

/* Only complete after the initial client has installed its signal handlers
 * and registered its lifetime. Later shutdown can then signal it safely. */
static inline int wine_ps5_title_complete_startup( struct wine_ps5_title_startup *state )
{
    unsigned int i;
    int cancelled;

    pthread_mutex_lock( &state->mutex );
    cancelled = state->cancelled;
    state->complete = 1;
    for (i = 0; i < WINE_PS5_TITLE_STARTUP_CHANNEL_COUNT; i++)
        if (state->channel[i] != -1)
        {
            close( state->channel[i] );
            state->channel[i] = -1;
        }
    pthread_mutex_unlock( &state->mutex );
    if (!cancelled) return 0;
    errno = ECANCELED;
    return -1;
}

/* Entries are embedded in live client thread data. The TEB is an opaque key,
 * never dereferenced by lookup, and remains unique until its native thread is
 * joined. A Windows ID may be recycled before that thread finishes its exit
 * destructor, so signals use the exact (ID, TEB) pair. */
struct wine_ps5_title_thread
{
    struct wine_ps5_title_thread *next;
    const void *teb;
    unsigned int id;
    uint64_t lifetime;
    pthread_t thread;
};

static inline int wine_ps5_title_join_client( const struct wine_ps5_title_thread *entry, pthread_t thread,
                                             struct wine_ps5_title_join *initial,
                                             int (*native_join)( pthread_t, void ** ),
                                             int (*regular_join)( pthread_t, void ** ))
{
    if (entry->lifetime && entry->lifetime == initial->lifetime)
        return wine_ps5_title_join_once( initial, native_join );
    return regular_join( thread, NULL );
}

struct wine_ps5_title_threads
{
    pthread_mutex_t mutex;
    struct wine_ps5_title_thread *head;
};

#define WINE_PS5_TITLE_THREADS_INIT { PTHREAD_MUTEX_INITIALIZER, NULL }

static inline int wine_ps5_title_register_thread( struct wine_ps5_title_threads *registry,
                                                  struct wine_ps5_title_thread *entry,
                                                  unsigned int id, const void *teb, pthread_t thread )
{
    struct wine_ps5_title_thread *ptr;
    int ret = 0;

    pthread_mutex_lock( &registry->mutex );
    for (ptr = registry->head; ptr; ptr = ptr->next)
        if (ptr == entry || ptr->teb == teb) { ret = EEXIST; break; }
    if (!ret)
    {
        entry->teb = teb;
        entry->id = id;
        entry->thread = thread;
        entry->next = registry->head;
        registry->head = entry;
    }
    pthread_mutex_unlock( &registry->mutex );
    return ret;
}

static inline void wine_ps5_title_unregister_thread( struct wine_ps5_title_threads *registry,
                                                     struct wine_ps5_title_thread *entry )
{
    struct wine_ps5_title_thread **ptr;

    pthread_mutex_lock( &registry->mutex );
    for (ptr = &registry->head; *ptr; ptr = &(*ptr)->next)
        if (*ptr == entry)
        {
            *ptr = entry->next;
            entry->next = NULL;
            break;
        }
    pthread_mutex_unlock( &registry->mutex );
}

/* Install the exit destructor before the TEB is published to the server.
 * Roll back the registry entry if native TLS setup fails. */
static inline int wine_ps5_title_register_current_thread( struct wine_ps5_title_threads *registry,
                                                          struct wine_ps5_title_thread *entry,
                                                          unsigned int id, const void *teb,
                                                          pthread_key_t key,
                                                          int (*set_specific)( pthread_key_t, const void * ))
{
    int ret = wine_ps5_title_register_thread( registry, entry, id, teb, pthread_self() );

    if (!ret && (ret = set_specific( key, entry ))) wine_ps5_title_unregister_thread( registry, entry );
    return ret;
}

static inline int wine_ps5_title_signal_thread( struct wine_ps5_title_threads *registry,
                                                unsigned int id, const void *teb, int sig )
{
    struct wine_ps5_title_thread *ptr;
    int ret = ESRCH;

    /* Keep unregister/exit from invalidating the native identity until kill
     * returns. No client holds this mutex across any server request. */
    pthread_mutex_lock( &registry->mutex );
    for (ptr = registry->head; ptr; ptr = ptr->next)
        if (ptr->id == id && ptr->teb == teb) { ret = pthread_kill( ptr->thread, sig ); break; }
    pthread_mutex_unlock( &registry->mutex );
    if (!ret) return 0;
    errno = ret;
    return -1;
}

static inline int wine_ps5_title_signal_lifetime( struct wine_ps5_title_threads *registry,
                                                 uint64_t lifetime, int sig )
{
    struct wine_ps5_title_thread *ptr;
    int ret = ESRCH;

    pthread_mutex_lock( &registry->mutex );
    if (lifetime)
        for (ptr = registry->head; ptr; ptr = ptr->next)
            if (ptr->lifetime == lifetime) { ret = pthread_kill( ptr->thread, sig ); break; }
    pthread_mutex_unlock( &registry->mutex );
    if (!ret) return 0;
    errno = ret;
    return -1;
}

/* A title can neither change its working directory (chdir is refused; fchdir
 * and the *at calls are exported only by libkernel_sys, which a title does
 * not import) nor ask for a descriptor's path. A directory descriptor is used
 * through the absolute path its object was opened by, and only while that
 * path still names the same object: a held directory which was renamed or
 * replaced fails instead of resolving to whatever now has its old name. */
static inline int wine_ps5_title_check_directory( int fd, const char *path )
{
    struct stat by_fd, by_path;

    if (path[0] != '/')
    {
        errno = EINVAL;
        return -1;
    }
    if (fstat( fd, &by_fd ) == -1 || stat( path, &by_path ) == -1) return -1;
    if (by_fd.st_dev != by_path.st_dev || by_fd.st_ino != by_path.st_ino)
    {
        errno = ENOENT;
        return -1;
    }
    if (!S_ISDIR( by_path.st_mode ))
    {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

/* name below an absolute directory, or name itself when it is absolute;
 * malloc'd, NULL with errno ENOMEM. */
static inline char *wine_ps5_title_join_path( const char *directory, const char *name )
{
    size_t directory_len = name[0] == '/' ? 0 : strlen( directory ), name_len = strlen( name );
    char *path;

    if (!(path = malloc( directory_len + name_len + 2 ))) return NULL;
    memcpy( path, directory, directory_len );
    if (directory_len && directory[directory_len - 1] != '/') path[directory_len++] = '/';
    memcpy( path + directory_len, name, name_len + 1 );
    return path;
}

/* Code that upstream runs after fchdir(dir) uses names relative to dir; in a
 * title they resolve against *current, which is only set under mutex. */
struct wine_ps5_title_directory
{
    sigset_t signals;
};

static inline int wine_ps5_title_enter_directory( pthread_mutex_t *mutex, const sigset_t *blocked,
                                                   const char **current, int fd, const char *path,
                                                   struct wine_ps5_title_directory *state )
{
    int error;

    pthread_sigmask( SIG_BLOCK, blocked, &state->signals );
    pthread_mutex_lock( mutex );
    if (!wine_ps5_title_check_directory( fd, path ))
    {
        *current = path;
        return 0;
    }
    error = errno;
    pthread_mutex_unlock( mutex );
    pthread_sigmask( SIG_SETMASK, &state->signals, NULL );
    errno = error;
    return -1;
}

static inline void wine_ps5_title_leave_directory( pthread_mutex_t *mutex, const char **current,
                                                   struct wine_ps5_title_directory *state )
{
    *current = NULL;
    pthread_mutex_unlock( mutex );
    pthread_sigmask( SIG_SETMASK, &state->signals, NULL );
}

#endif /* __WINE_PS5_TITLE_PRIVATE_H */
