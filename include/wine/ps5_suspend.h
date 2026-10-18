/* Shared x64 title-thread suspension diagnostics in the Wine-private TEB area.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef __WINE_PS5_SUSPEND_H
#define __WINE_PS5_SUSPEND_H

#include <stdint.h>

#define WINE_PS5_SUSPEND_DIAGNOSTIC_MAGIC UINT32_C(0x53555350) /* "PSUS" */
#define WINE_PS5_SUSPEND_DIAGNOSTIC_TEB64_OFFSET UINT32_C(0x420)

#define WINE_PS5_HANG_SNAPSHOT_COUNT 3u
#define WINE_PS5_HANG_TOTAL_SNAPSHOT_COUNT 5u
#define WINE_PS5_HANG_THREAD_MAX     256u
#define WINE_PS5_HANG_STACK_SLOTS    256u
#define WINE_PS5_HANG_STACK_FRAMES   24u
#define WINE_PS5_HANG_MEMORY_REGS    3u
#define WINE_PS5_HANG_CODE_BYTES     32u
#define WINE_PS5_HANG_CODE_BEFORE_BYTES 64u
#define WINE_PS5_HANG_LOOP_RANGE     UINT64_C(0x10000)
#define WINE_PS5_HANG_VALID_MASK     ((1u << WINE_PS5_HANG_SNAPSHOT_COUNT) - 1)

static inline int wine_ps5_hang_sample_is_primary( unsigned int sample )
{
    return sample >= 1 && sample <= WINE_PS5_HANG_SNAPSHOT_COUNT;
}

static inline unsigned int wine_ps5_hang_sample_total( unsigned int sample )
{
    return wine_ps5_hang_sample_is_primary( sample )
           ? WINE_PS5_HANG_SNAPSHOT_COUNT : WINE_PS5_HANG_TOTAL_SNAPSHOT_COUNT;
}

#define WINE_PS5_PROFILE_HZ             20u
#define WINE_PS5_PROFILE_WINDOW_SAMPLES 200u
#define WINE_PS5_PROFILE_WINDOW_TICKS   (WINE_PS5_PROFILE_WINDOW_SAMPLES + 1)
#define WINE_PS5_PROFILE_BUCKETS        512u
#define WINE_PS5_PROFILE_TOP_BUCKETS    25u
#define WINE_PS5_PROFILE_TOP_THREADS    8u

/* Title alert words coalesce signals: 0 idle, 1 pending signal, 2 sleeper.
 * Preserve the established wake policy: wake idle or sleeping targets, never
 * issue another kernel wake for an already pending signal. */
static inline int wine_ps5_alert_signal( int32_t *word, int *sleeper )
{
    int32_t previous = __atomic_exchange_n( word, 1, __ATOMIC_SEQ_CST );

    *sleeper = previous == 2;
    return *sleeper || previous == 0;
}

static inline int wine_ps5_alert_prepare_wait( int32_t *word )
{
    int32_t expected = 0;

    if (__atomic_compare_exchange_n( word, &expected, 2, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST )) return 0;
    /* Only a real pending signal can produce STATUS_ALERTED. A signal racing
     * the 0->2 CAS is consumed here or on the next iteration after UMTX
     * rejects the changed word; a spurious kernel wake is never an alert. */
    return expected == 1 && __atomic_compare_exchange_n(
        word, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

static inline void wine_ps5_alert_cancel_wait( int32_t *word )
{
    int32_t expected = 2;

    /* A timeout must leave a concurrently published signal pending. */
    __atomic_compare_exchange_n( word, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

#define WINE_PS5_LATENCY_BUCKETS 8
/* Exclusive upper bounds: 2,5,10,20,50,100,500 us; last bucket >=500 us. */
static inline unsigned int wine_ps5_latency_bucket( uint64_t ns )
{
    static const uint64_t limits[] = { 2000, 5000, 10000, 20000, 50000, 100000, 500000 };
    unsigned int i;

    for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) if (ns < limits[i]) return i;
    return WINE_PS5_LATENCY_BUCKETS - 1;
}

struct wine_ps5_alert_timing
{
    uint64_t sleep_sequence, sleep_ns, alert_version, alert_sequence, alert_ns;
};

/* The block has unused tail padding when this layout does not divide 64 KiB.
 * Map the entire block, not just floor(block/entry_size) * entry_size. */
#define WINE_PS5_ALERT_BLOCK_BYTES 65536u
struct wine_ps5_alert_entry
{
    int32_t word;
    struct wine_ps5_alert_timing timing;
};

static inline void wine_ps5_alert_announce_time( struct wine_ps5_alert_timing *timing, uint64_t ns )
{
    __atomic_fetch_add( &timing->sleep_sequence, 1, __ATOMIC_SEQ_CST );
    __atomic_store_n( &timing->sleep_ns, ns, __ATOMIC_SEQ_CST );
    __atomic_fetch_add( &timing->sleep_sequence, 1, __ATOMIC_SEQ_CST );
}

static inline int wine_ps5_alert_sleep_time( const struct wine_ps5_alert_timing *timing,
                                            uint64_t *sequence, uint64_t *ns )
{
    *sequence = __atomic_load_n( &timing->sleep_sequence, __ATOMIC_SEQ_CST );
    /* The payload participates in the same SC order as both sequence reads.
     * Acquire sequence loads alone do not stop a relaxed payload load from
     * observing a different publication on weakly ordered hosts. */
    *ns = __atomic_load_n( &timing->sleep_ns, __ATOMIC_SEQ_CST );
    return *sequence && !(*sequence & 1) &&
           *sequence == __atomic_load_n( &timing->sleep_sequence, __ATOMIC_SEQ_CST );
}

static inline void wine_ps5_alert_publish_time( struct wine_ps5_alert_timing *timing,
                                               uint64_t sequence, uint64_t ns )
{
    uint64_t version = __atomic_load_n( &timing->alert_version, __ATOMIC_SEQ_CST );

    /* Different alerters can publish different sleep epochs concurrently.
     * A bounded diagnostic-only writer claim prevents mixing their fields;
     * contention drops this timestamp rather than delaying the actual wake. */
    if ((version & 1) || !__atomic_compare_exchange_n( &timing->alert_version, &version,
        version + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST )) return;
    __atomic_store_n( &timing->alert_ns, ns, __ATOMIC_SEQ_CST );
    __atomic_store_n( &timing->alert_sequence, sequence, __ATOMIC_SEQ_CST );
    __atomic_store_n( &timing->alert_version, version + 2, __ATOMIC_SEQ_CST );
}

static inline int wine_ps5_alert_return_latency( const struct wine_ps5_alert_timing *timing,
                                                uint64_t now, uint64_t *ns )
{
    uint64_t version = __atomic_load_n( &timing->alert_version, __ATOMIC_SEQ_CST );
    uint64_t sequence = __atomic_load_n( &timing->alert_sequence, __ATOMIC_SEQ_CST );
    uint64_t alerted = __atomic_load_n( &timing->alert_ns, __ATOMIC_SEQ_CST );

    if (!version || (version & 1) || !sequence || (sequence & 1) ||
        version != __atomic_load_n( &timing->alert_version, __ATOMIC_SEQ_CST ) ||
        sequence != __atomic_load_n( &timing->sleep_sequence, __ATOMIC_SEQ_CST ) || now < alerted)
        return 0;
    *ns = now - alerted;
    return 1;
}

struct wine_ps5_alert_stats
{
    uint64_t alerts, wakes, sleepers, wake_errors, total_ns, max_ns;
    uint64_t gap[WINE_PS5_LATENCY_BUCKETS], gap_unavailable;
    uint64_t wake[WINE_PS5_LATENCY_BUCKETS], wake_unavailable;
};

static inline void wine_ps5_alert_record( struct wine_ps5_alert_stats *stats,
                                         int wake, int sleeper, int error, uint64_t ns )
{
    uint64_t maximum = __atomic_load_n( &stats->max_ns, __ATOMIC_RELAXED );

    __atomic_fetch_add( &stats->alerts, 1, __ATOMIC_RELAXED );
    if (wake) __atomic_fetch_add( &stats->wakes, 1, __ATOMIC_RELAXED );
    if (sleeper) __atomic_fetch_add( &stats->sleepers, 1, __ATOMIC_RELAXED );
    if (error) __atomic_fetch_add( &stats->wake_errors, 1, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->total_ns, ns, __ATOMIC_RELAXED );
    while (maximum < ns && !__atomic_compare_exchange_n(
        &stats->max_ns, &maximum, ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED )) {}
}

/* Window boundaries are independent atomic exchanges: an alert concurrent
 * with the boundary can contribute fields to adjacent windows. */
static inline struct wine_ps5_alert_stats wine_ps5_alert_take_stats(
    struct wine_ps5_alert_stats *stats )
{
    struct wine_ps5_alert_stats result;
    unsigned int i;

    result.alerts = __atomic_exchange_n( &stats->alerts, 0, __ATOMIC_RELAXED );
    result.wakes = __atomic_exchange_n( &stats->wakes, 0, __ATOMIC_RELAXED );
    result.sleepers = __atomic_exchange_n( &stats->sleepers, 0, __ATOMIC_RELAXED );
    result.wake_errors = __atomic_exchange_n( &stats->wake_errors, 0, __ATOMIC_RELAXED );
    result.total_ns = __atomic_exchange_n( &stats->total_ns, 0, __ATOMIC_RELAXED );
    result.max_ns = __atomic_exchange_n( &stats->max_ns, 0, __ATOMIC_RELAXED );
    for (i = 0; i < WINE_PS5_LATENCY_BUCKETS; i++)
    {
        result.gap[i] = __atomic_exchange_n( &stats->gap[i], 0, __ATOMIC_RELAXED );
        result.wake[i] = __atomic_exchange_n( &stats->wake[i], 0, __ATOMIC_RELAXED );
    }
    result.gap_unavailable = __atomic_exchange_n( &stats->gap_unavailable, 0, __ATOMIC_RELAXED );
    result.wake_unavailable = __atomic_exchange_n( &stats->wake_unavailable, 0, __ATOMIC_RELAXED );
    return result;
}

enum wine_ps5_read_directory { WINE_PS5_READ_UNKNOWN, WINE_PS5_READ_CASC, WINE_PS5_READ_OTHER };
static inline unsigned int wine_ps5_read_size_bucket( uint64_t size )
{
    return size < 4096 ? 0 : size < 65536 ? 1 : size < 1048576 ? 2 : 3;
}

static inline unsigned int wine_ps5_read_directory_class( const char *path )
{
    const char *p;

    if (!path) return WINE_PS5_READ_UNKNOWN;
    for (p = path; *p; p++)
    {
        if (p != path && p[-1] != '/' && p[-1] != '\\') continue;
        if ((p[0] == 'D' || p[0] == 'd') && (p[1] == 'A' || p[1] == 'a') &&
            (p[2] == 'T' || p[2] == 't') && (p[3] == 'A' || p[3] == 'a') &&
            (p[4] == '/' || p[4] == '\\'))
        {
            const char *q = p + 5;
            if ((q[0] == 'D' || q[0] == 'd') && (q[1] == 'A' || q[1] == 'a') &&
                (q[2] == 'T' || q[2] == 't') && (q[3] == 'A' || q[3] == 'a') &&
                (q[4] == '/' || q[4] == '\\')) return WINE_PS5_READ_CASC;
        }
    }
    return WINE_PS5_READ_OTHER;
}

static inline uint64_t wine_ps5_read_class_key( uint32_t handle, unsigned int directory )
{
    return ((uint64_t)handle << 32) | directory;
}

static inline unsigned int wine_ps5_read_class_from_key( uint64_t key, uint32_t handle )
{
    return key >> 32 == handle ? (unsigned int)key : WINE_PS5_READ_UNKNOWN;
}

struct wine_ps5_read_stats
{
    uint64_t calls, bytes, total_ns, max_ns, lookup_ns, pread_ns, read_ns, native_calls;
    uint64_t sync_calls, pending_calls, failed_calls, unknown_backend;
    uint64_t sizes[4], classes[3];
};

struct wine_ps5_read_sample
{
    uint64_t bytes, lookup_ns, pread_ns, read_ns, native_calls;
    unsigned int synchronous, unknown_backend;
};

static inline void wine_ps5_read_record( struct wine_ps5_read_stats *stats,
    const struct wine_ps5_read_sample *sample, uint64_t requested, uint64_t ns,
    unsigned int directory, int pending, int failed )
{
    uint64_t maximum = __atomic_load_n( &stats->max_ns, __ATOMIC_RELAXED );

    __atomic_fetch_add( &stats->calls, 1, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->bytes, sample->bytes, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->total_ns, ns, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->lookup_ns, sample->lookup_ns, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->pread_ns, sample->pread_ns, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->read_ns, sample->read_ns, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->native_calls, sample->native_calls, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->sync_calls, sample->synchronous, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->pending_calls, !!pending, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->failed_calls, !!failed, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->unknown_backend, sample->unknown_backend, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->sizes[wine_ps5_read_size_bucket( requested )], 1, __ATOMIC_RELAXED );
    __atomic_fetch_add( &stats->classes[directory], 1, __ATOMIC_RELAXED );
    while (maximum < ns && !__atomic_compare_exchange_n(
        &stats->max_ns, &maximum, ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED )) {}
}

static inline struct wine_ps5_read_stats wine_ps5_read_take_stats( struct wine_ps5_read_stats *stats )
{
    struct wine_ps5_read_stats result;
    unsigned int i;

#define TAKE_READ(field) result.field = __atomic_exchange_n( &stats->field, 0, __ATOMIC_RELAXED )
    TAKE_READ(calls); TAKE_READ(bytes); TAKE_READ(total_ns); TAKE_READ(max_ns);
    TAKE_READ(lookup_ns); TAKE_READ(pread_ns); TAKE_READ(read_ns); TAKE_READ(native_calls);
    TAKE_READ(sync_calls); TAKE_READ(pending_calls); TAKE_READ(failed_calls); TAKE_READ(unknown_backend);
#undef TAKE_READ
    for (i = 0; i < 4; i++) result.sizes[i] = __atomic_exchange_n( &stats->sizes[i], 0, __ATOMIC_RELAXED );
    for (i = 0; i < 3; i++) result.classes[i] = __atomic_exchange_n( &stats->classes[i], 0, __ATOMIC_RELAXED );
    return result;
}

enum wine_ps5_profile_kind
{
    WINE_PS5_PROFILE_NONE,
    WINE_PS5_PROFILE_USER,
    WINE_PS5_PROFILE_UNIX_SYSCALL,
    WINE_PS5_PROFILE_UNIX_REQUEST,
    WINE_PS5_PROFILE_UNIX_CALL,
    WINE_PS5_PROFILE_WAIT
};

static inline enum wine_ps5_profile_kind wine_ps5_profile_classify(
    int blocking, int server_request, int user_context, int unix_call )
{
    if (blocking) return WINE_PS5_PROFILE_WAIT;
    if (server_request) return WINE_PS5_PROFILE_UNIX_REQUEST;
    if (user_context) return WINE_PS5_PROFILE_USER;
    if (unix_call) return WINE_PS5_PROFILE_UNIX_CALL;
    return WINE_PS5_PROFILE_UNIX_SYSCALL;
}

#define WINE_PS5_PROFILE_UNIX_CALL_FRAME UINT32_C(0x00020000)

static inline int wine_ps5_profile_is_unix_call( int in_syscall, uint32_t restore_flags )
{
    return in_syscall && (restore_flags & WINE_PS5_PROFILE_UNIX_CALL_FRAME);
}

static inline unsigned int wine_ps5_profile_cpu_permille(
    uint64_t cpu_first, uint64_t cpu_last, uint64_t wall_first, uint64_t wall_last )
{
    uint64_t cpu, wall;

    if (cpu_last < cpu_first || wall_last <= wall_first) return 0;
    cpu = cpu_last - cpu_first;
    wall = wall_last - wall_first;
    if (cpu >= wall) return 1000;
    return (unsigned int)((cpu * 1000 + wall / 2) / wall);
}

static inline uint64_t wine_ps5_profile_page( uint64_t pc )
{
    return pc & ~UINT64_C(0xfff);
}

static inline unsigned int wine_ps5_profile_permille( uint64_t count, uint64_t total )
{
    return total ? (unsigned int)((count * 1000 + total / 2) / total) : 0;
}

static inline unsigned int wine_ps5_profile_hash( uint64_t first, uint64_t second )
{
    uint64_t value = first ^ (second + UINT64_C(0x9e3779b97f4a7c15) +
                              (first << 6) + (first >> 2));

    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    return (unsigned int)value;
}

enum wine_ps5_hang_position_class
{
    WINE_PS5_HANG_POSITION_UNAVAILABLE,
    WINE_PS5_HANG_POSITION_CONSTANT,
    WINE_PS5_HANG_POSITION_VARYING,
    WINE_PS5_HANG_POSITION_PROGRESSING
};

enum wine_ps5_hang_address_class
{
    WINE_PS5_HANG_ADDRESS_UNBACKED,
    WINE_PS5_HANG_ADDRESS_WOW,
    WINE_PS5_HANG_ADDRESS_SYSTEM,
    WINE_PS5_HANG_ADDRESS_MODULE
};

static inline unsigned char wine_ps5_hang_ascii_lower( unsigned char ch )
{
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

static inline int wine_ps5_hang_name_equal( const char *left, const char *right )
{
    if (!left || !right) return 0;
    while (*left && *right)
    {
        if (wine_ps5_hang_ascii_lower( *left++ ) != wine_ps5_hang_ascii_lower( *right++ ))
            return 0;
    }
    return !*left && !*right;
}

/* All channel clients share one CPU diagnostic bucket, not one product. */
static inline int wine_ps5_wow_module( const char *name )
{
    const char *at;
    if (!name || !name[0] || !name[1] || !name[2] ||
        wine_ps5_hang_ascii_lower( name[0] ) != 'w' ||
        wine_ps5_hang_ascii_lower( name[1] ) != 'o' ||
        wine_ps5_hang_ascii_lower( name[2] ) != 'w') return 0;
    for (at = name + 3; (*at >= 'A' && *at <= 'Z') || (*at >= 'a' && *at <= 'z'); ++at);
    return wine_ps5_hang_name_equal( at, ".exe" );
}

static inline int wine_ps5_hang_system_module( const char *name )
{
    return wine_ps5_hang_name_equal( name, "ntdll.dll" ) ||
           wine_ps5_hang_name_equal( name, "kernelbase.dll" ) ||
           wine_ps5_hang_name_equal( name, "kernel32.dll" ) ||
           wine_ps5_hang_name_equal( name, "win32u.dll" ) ||
           wine_ps5_hang_name_equal( name, "d3d12.dll" ) ||
           wine_ps5_hang_name_equal( name, "d3d12core.dll" ) ||
           wine_ps5_hang_name_equal( name, "dxgi.dll" ) ||
           wine_ps5_hang_name_equal( name, "vulkan-1.dll" ) ||
           wine_ps5_hang_name_equal( name, "winevulkan.dll" ) ||
           wine_ps5_hang_name_equal( name, "ucrtbase.dll" ) ||
           wine_ps5_hang_name_equal( name, "msvcrt.dll" );
}

/* Half-open PE image membership without computing a potentially wrapped end. */
static inline int wine_ps5_hang_address_in_image( uint64_t address, uint64_t base,
                                                  uint64_t size )
{
    return size && address >= base && address - base < size;
}

/* Classify an address in a loaded PE image by its diagnostic role. */
static inline enum wine_ps5_hang_address_class wine_ps5_hang_classify_address(
    uint64_t address, uint64_t base, uint64_t size, const char *module )
{
    if (!wine_ps5_hang_address_in_image( address, base, size ))
        return WINE_PS5_HANG_ADDRESS_UNBACKED;
    if (wine_ps5_wow_module( module ))
        return WINE_PS5_HANG_ADDRESS_WOW;
    if (wine_ps5_hang_system_module( module ))
        return WINE_PS5_HANG_ADDRESS_SYSTEM;
    return WINE_PS5_HANG_ADDRESS_MODULE;
}

static inline const char *wine_ps5_hang_address_class_name(
    enum wine_ps5_hang_address_class kind )
{
    switch (kind)
    {
    case WINE_PS5_HANG_ADDRESS_WOW:      return "wow";
    case WINE_PS5_HANG_ADDRESS_SYSTEM:   return "system";
    case WINE_PS5_HANG_ADDRESS_MODULE:   return "module";
    default:                             return "unbacked";
    }
}

/* Three one-second samples distinguish an exact position from a bounded code
 * or stack loop. Larger movement is reported as progress; raw values remain in
 * the diagnostic so the classification never hides the sampled positions. */
static inline enum wine_ps5_hang_position_class wine_ps5_hang_classify_position(
    const uint64_t values[WINE_PS5_HANG_SNAPSHOT_COUNT], uint32_t valid )
{
    uint64_t low, high;
    unsigned int i;

    if ((valid & WINE_PS5_HANG_VALID_MASK) != WINE_PS5_HANG_VALID_MASK)
        return WINE_PS5_HANG_POSITION_UNAVAILABLE;
    low = high = values[0];
    if (!low) return WINE_PS5_HANG_POSITION_UNAVAILABLE;
    for (i = 1; i < WINE_PS5_HANG_SNAPSHOT_COUNT; i++)
    {
        if (!values[i]) return WINE_PS5_HANG_POSITION_UNAVAILABLE;
        if (values[i] < low) low = values[i];
        if (values[i] > high) high = values[i];
    }
    if (low == high) return WINE_PS5_HANG_POSITION_CONSTANT;
    if (high - low <= WINE_PS5_HANG_LOOP_RANGE) return WINE_PS5_HANG_POSITION_VARYING;
    return WINE_PS5_HANG_POSITION_PROGRESSING;
}

static inline const char *wine_ps5_hang_position_class_name(
    enum wine_ps5_hang_position_class kind )
{
    switch (kind)
    {
    case WINE_PS5_HANG_POSITION_CONSTANT:    return "constant";
    case WINE_PS5_HANG_POSITION_VARYING:     return "varying";
    case WINE_PS5_HANG_POSITION_PROGRESSING: return "progressing";
    default:                                 return "unavailable";
    }
}

#define WINE_PS5_HANG_TICKS_PER_MS INT64_C(10000)

static inline int64_t wine_ps5_hang_age_ms( int64_t now, int64_t then )
{
    if (then <= 0 || now < then) return -1;
    return (now - then) / WINE_PS5_HANG_TICKS_PER_MS;
}

/* Server wait deadlines are positive wall-clock ticks or negated monotonic
 * deadlines. Return the nonnegative duration left at the supplied clock
 * snapshot; callers retain their own infinite-time sentinel. */
static inline int64_t wine_ps5_hang_timeout_remaining_ms( int64_t deadline,
                                                          int64_t wall_now,
                                                          int64_t monotonic_now )
{
    uint64_t due, now, remaining;

    if (deadline < 0)
    {
        due = 0 - (uint64_t)deadline;
        now = monotonic_now > 0 ? (uint64_t)monotonic_now : 0;
    }
    else
    {
        due = (uint64_t)deadline;
        now = wall_now > 0 ? (uint64_t)wall_now : 0;
    }
    remaining = due > now ? due - now : 0;
    return remaining / WINE_PS5_HANG_TICKS_PER_MS;
}

struct wine_ps5_hang_registers
{
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, r8, r9;
    uint64_t r10, r11, r12, r13;
    uint64_t r14, r15, rbp;
};

struct wine_ps5_suspend_diagnostic
{
    uint32_t magic;
    volatile uint32_t waiting;
    volatile uint32_t inside_syscall;
    volatile uint32_t hang_snapshot;
    volatile uint64_t native_rip;
    volatile uint64_t native_rsp;
    volatile uint64_t user_rip;
    volatile uint64_t user_rsp;
    volatile uint32_t hang_epoch;
    volatile uint32_t hang_valid;
    uint32_t hang_reserved[2];
    volatile uint64_t hang_rip[WINE_PS5_HANG_SNAPSHOT_COUNT];
    volatile uint64_t hang_rsp[WINE_PS5_HANG_SNAPSHOT_COUNT];
    volatile struct wine_ps5_hang_registers user_regs;
    volatile uint32_t hang_pending;
    volatile uint32_t profile_pending;
    volatile uint32_t profile_completed;
    volatile uint32_t profile_kind;
    volatile uint64_t profile_where;
    volatile uint64_t profile_module_base;
    char profile_module[40];
    volatile uint64_t profile_cpu_ns;
    volatile uint64_t profile_wall_ns;
    volatile uint32_t profile_cpu_valid;
};

static inline struct wine_ps5_suspend_diagnostic *wine_ps5_suspend_diagnostic_from_teb64( void *teb )
{
    return (struct wine_ps5_suspend_diagnostic *)((char *)teb + WINE_PS5_SUSPEND_DIAGNOSTIC_TEB64_OFFSET);
}

static inline const struct wine_ps5_suspend_diagnostic *wine_ps5_suspend_diagnostic_from_const_teb64(
    const void *teb )
{
    return (const struct wine_ps5_suspend_diagnostic *)((const char *)teb +
                                                        WINE_PS5_SUSPEND_DIAGNOSTIC_TEB64_OFFSET);
}

static inline int wine_ps5_is_title_thread64( const void *teb )
{
    return teb && wine_ps5_suspend_diagnostic_from_const_teb64( teb )->magic ==
                  WINE_PS5_SUSPEND_DIAGNOSTIC_MAGIC;
}

static inline int wine_ps5_title_self_dump_disabled( int self_process, const void *teb )
{
    return self_process && wine_ps5_is_title_thread64( teb );
}

/* Per-thread running totals for per-hitch frame attribution (WINEPS5_PROFILE=1).
 * The block sits right after the suspend diagnostic at a fixed x64 TEB offset;
 * ps5-wow vkd3d-proton (libs/vkd3d/wineps5_hitch.h) mirrors the layout. Wine
 * stamps magic/tid/main_block at thread registration and clears tid at exit
 * (TEB memory is recycled, never unmapped). d3d12core arms the threads it
 * attributes and reads their totals from the thread that presents, so every
 * update is bracketed by an odd/even sequence (seqlock, atomic increments so
 * the profiler's SIGURG sample can safely nest). Unarmed threads accumulate
 * nothing. Timestamps are QueryPerformanceCounter nanoseconds (100-ns ticks
 * x 100), the clock d3d12core reads, so a reader can add a wait in progress.
 * The PS5 has no clock for another thread's CPU time (pthread_getcpuclockid
 * returns CLOCK_THREAD_CPUTIME_ID for every thread), so the owner samples its
 * own CPU clock at each outermost blocking transition and at each 20-Hz
 * profiler tick; the reader bounds CPU time between samples. */
#define WINE_PS5_HITCH_TEB64_OFFSET UINT32_C(0x568)
#define WINE_PS5_HITCH_MAGIC UINT32_C(0x48545350) /* "PSTH" */
#define WINE_PS5_HITCH_NO_CORE UINT32_C(0xffffffff)

struct wine_ps5_hitch_counters
{
    uint32_t magic;
    volatile uint32_t armed;
    volatile uint32_t tid;      /* owner thread id; 0 once the thread exited */
    volatile uint32_t sequence; /* odd while the fields below are updated */
    uint64_t wait_ns;           /* inside ps5_title_profile_set_blocking scopes */
    uint64_t wait_count;
    uint64_t read_ns;           /* NtReadFile, excluding nested blocked time */
    uint64_t read_count;
    uint64_t unwind_ns;         /* RtlWalkFrameChain (incl. RtlCaptureStackBackTrace), excluding nested wait/read */
    uint64_t unwind_count;
    uint64_t wait_start_ns;     /* nonzero while blocked */
    uint64_t main_block;        /* address of the process initial thread's block */
    uint64_t cpu_ns;            /* owner CPU time (CLOCK_THREAD_CPUTIME_ID) at cpu_sample_ns */
    uint64_t cpu_sample_ns;     /* 0: no sample yet */
    uint32_t cpu_core;          /* RDTSCP TSC_AUX at the sample, or WINE_PS5_HITCH_NO_CORE */
    uint32_t cpu_samples;
};

static inline struct wine_ps5_hitch_counters *wine_ps5_hitch_from_teb64( void *teb )
{
    return (struct wine_ps5_hitch_counters *)((char *)teb + WINE_PS5_HITCH_TEB64_OFFSET);
}

static inline int wine_ps5_hitch_armed( const struct wine_ps5_hitch_counters *counters )
{
    return counters->magic == WINE_PS5_HITCH_MAGIC && counters->armed;
}

static inline void wine_ps5_hitch_write_begin( struct wine_ps5_hitch_counters *counters )
{
    __atomic_fetch_add( &counters->sequence, 1, __ATOMIC_RELAXED );
    __atomic_thread_fence( __ATOMIC_RELEASE );
}

static inline void wine_ps5_hitch_write_end( struct wine_ps5_hitch_counters *counters )
{
    __atomic_fetch_add( &counters->sequence, 1, __ATOMIC_RELEASE );
}

static inline void wine_ps5_hitch_set_cpu( struct wine_ps5_hitch_counters *counters,
                                           uint64_t now, uint64_t cpu, uint32_t core )
{
    counters->cpu_ns = cpu;
    counters->cpu_sample_ns = now;
    counters->cpu_core = core;
    counters->cpu_samples++;
}

/* Outermost blocking-marker transitions only; a wait already in progress
 * when the thread was armed (no start) is not counted. Each transition also
 * records the owner's CPU clock when it could be read. */
static inline void wine_ps5_hitch_blocking( struct wine_ps5_hitch_counters *counters,
                                            int previous, int blocking, uint64_t now,
                                            int cpu_valid, uint64_t cpu, uint32_t core )
{
    if (!previous == !blocking) return;
    wine_ps5_hitch_write_begin( counters );
    if (blocking) counters->wait_start_ns = now;
    else if (counters->wait_start_ns)
    {
        if (now > counters->wait_start_ns) counters->wait_ns += now - counters->wait_start_ns;
        counters->wait_count++;
        counters->wait_start_ns = 0;
    }
    if (cpu_valid) wine_ps5_hitch_set_cpu( counters, now, cpu, core );
    wine_ps5_hitch_write_end( counters );
}

/* Profiler tick in the owner's SIGURG handler; skipped when the signal
 * interrupted an update of this block. */
static inline int wine_ps5_hitch_cpu_sample( struct wine_ps5_hitch_counters *counters,
                                             uint64_t now, uint64_t cpu, uint32_t core )
{
    if (__atomic_load_n( &counters->sequence, __ATOMIC_RELAXED ) & 1) return 0;
    wine_ps5_hitch_write_begin( counters );
    wine_ps5_hitch_set_cpu( counters, now, cpu, core );
    wine_ps5_hitch_write_end( counters );
    return 1;
}

/* Owner-side accumulation of an exclusive read or stack-walk duration. */
static inline void wine_ps5_hitch_add( struct wine_ps5_hitch_counters *counters,
                                       uint64_t *total, uint64_t *count, uint64_t ns )
{
    wine_ps5_hitch_write_begin( counters );
    *total += ns;
    (*count)++;
    wine_ps5_hitch_write_end( counters );
}

/* Duration of [started, ended] minus the blocked time recorded inside it. */
static inline uint64_t wine_ps5_hitch_exclusive( uint64_t started, uint64_t ended,
                                                 uint64_t wait_before, uint64_t wait_after )
{
    uint64_t total = ended > started ? ended - started : 0;
    uint64_t waited = wait_after > wait_before ? wait_after - wait_before : 0;

    return total > waited ? total - waited : 0;
}

#define WINE_PS5_PRESENT_HANG_NS INT64_C(15000000000)

static inline int wine_ps5_present_is_hung( int64_t now, int64_t last )
{
    return last > 0 && now >= last && now - last >= WINE_PS5_PRESENT_HANG_NS;
}

/* Title QueryPerformanceCounter from the TSC, in the same 100-ns ticks and on
 * the same timeline as the Unix monotonic_counter():
 *     ticks = (tsc * mul) >> 64 + bias,  mul = round(2^64 * 10^7 / tsc_hz).
 * mul fits in 64 bits for any tsc_hz above 10 MHz; the 64x128 product cannot
 * overflow, and the rounding of mul costs under one tick for every 64-bit TSC.
 * The conversion is non-decreasing in the TSC. Offsets only ever grow: a mode
 * switch steps the newly selected source forward to the old source's value so
 * that QPC never runs backwards for callers ordered after the switch. */
#define WINE_PS5_QPC_TICKS_PER_SEC UINT64_C(10000000)

enum wine_ps5_qpc_mode { WINE_PS5_QPC_OFF, WINE_PS5_QPC_SYSCALL, WINE_PS5_QPC_TSC };

struct wine_ps5_qpc_state
{
    uint64_t mul;               /* 0: TSC unusable; set once before publishing */
    uint64_t calibration_bias;  /* bias aligning the TSC with monotonic_counter() */
    uint64_t tsc_bias;          /* calibration_bias plus forward switch steps */
    uint64_t syscall_offset;    /* forward switch steps */
    int32_t mode;               /* enum wine_ps5_qpc_mode */
};

static inline uint64_t wine_ps5_qpc_multiplier( uint64_t tsc_hz )
{
    uint64_t quotient = 0, remainder = WINE_PS5_QPC_TICKS_PER_SEC, carry;
    unsigned int i;

    if (tsc_hz <= WINE_PS5_QPC_TICKS_PER_SEC) return 0;
    /* 2^64 * 10^7 / tsc_hz by long division; no 128-bit divide helper needed. */
    for (i = 0; i < 64; i++)
    {
        carry = remainder >> 63;
        remainder <<= 1;
        quotient <<= 1;
        if (carry || remainder >= tsc_hz)
        {
            remainder -= tsc_hz;
            quotient |= 1;
        }
    }
    return quotient + (remainder >= tsc_hz - remainder);
}

static inline uint64_t wine_ps5_qpc_scale( uint64_t tsc, uint64_t mul )
{
    return (uint64_t)(((unsigned __int128)tsc * mul) >> 64);
}

/* Bias making the converted TSC equal ticks at tsc; modular, either sign. */
static inline uint64_t wine_ps5_qpc_bias( uint64_t ticks, uint64_t tsc, uint64_t mul )
{
    return ticks - wine_ps5_qpc_scale( tsc, mul );
}

static inline uint64_t wine_ps5_qpc_pair_mid( uint64_t before, uint64_t after )
{
    return before + (after - before) / 2;
}

/* Converted TSC minus monotonic_counter() for a TSC/clock/TSC read pair,
 * relative to the calibration alignment (excludes switch steps). */
static inline int64_t wine_ps5_qpc_drift( const struct wine_ps5_qpc_state *state, uint64_t before,
                                          uint64_t mono, uint64_t after )
{
    return (int64_t)(wine_ps5_qpc_scale( wine_ps5_qpc_pair_mid( before, after ), state->mul ) +
                     state->calibration_bias - mono);
}

/* Current QPC minus monotonic_counter() for the active mode (switch steps included). */
static inline int64_t wine_ps5_qpc_offset( const struct wine_ps5_qpc_state *state, uint64_t before,
                                           uint64_t mono, uint64_t after )
{
    if (__atomic_load_n( &state->mode, __ATOMIC_ACQUIRE ) != WINE_PS5_QPC_TSC)
        return (int64_t)__atomic_load_n( &state->syscall_offset, __ATOMIC_RELAXED );
    return (int64_t)(wine_ps5_qpc_scale( wine_ps5_qpc_pair_mid( before, after ), state->mul ) +
                     __atomic_load_n( &state->tsc_bias, __ATOMIC_RELAXED ) - mono);
}

static inline uint64_t wine_ps5_qpc_from_tsc( const struct wine_ps5_qpc_state *state, uint64_t tsc )
{
    return wine_ps5_qpc_scale( tsc, state->mul ) + __atomic_load_n( &state->tsc_bias, __ATOMIC_RELAXED );
}

static inline uint64_t wine_ps5_qpc_from_syscall( const struct wine_ps5_qpc_state *state, uint64_t mono )
{
    return mono + __atomic_load_n( &state->syscall_offset, __ATOMIC_RELAXED );
}

/* Select mode from a read pair (TSC before, monotonic_counter(), TSC after).
 * The new source is stepped forward to the latest value the old source could
 * have returned by the time of the pair, then the mode is published with
 * release order after its offset. A single writer (the tune poll) is assumed.
 * Returns the mode now active; TSC requests without a usable TSC stay syscall. */
static inline int wine_ps5_qpc_select( struct wine_ps5_qpc_state *state, int mode, uint64_t before,
                                       uint64_t mono, uint64_t after )
{
    int old = __atomic_load_n( &state->mode, __ATOMIC_RELAXED );
    uint64_t syscall_now, tsc_now;

    if (old == WINE_PS5_QPC_OFF) return old;
    if (mode == WINE_PS5_QPC_TSC && !state->mul) mode = WINE_PS5_QPC_SYSCALL;
    if (mode == old) return old;
    syscall_now = wine_ps5_qpc_from_syscall( state, mono );
    if (mode == WINE_PS5_QPC_TSC)
    {
        tsc_now = wine_ps5_qpc_from_tsc( state, before );
        if ((int64_t)(syscall_now - tsc_now) > 0)
            __atomic_store_n( &state->tsc_bias, state->tsc_bias + (syscall_now - tsc_now), __ATOMIC_RELAXED );
    }
    else
    {
        tsc_now = wine_ps5_qpc_from_tsc( state, after );
        if ((int64_t)(tsc_now - syscall_now) > 0)
            __atomic_store_n( &state->syscall_offset, state->syscall_offset + (tsc_now - syscall_now),
                              __ATOMIC_RELAXED );
    }
    __atomic_store_n( &state->mode, mode, __ATOMIC_RELEASE );
    return mode;
}

#endif /* __WINE_PS5_SUSPEND_H */
