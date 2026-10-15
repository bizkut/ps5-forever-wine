/*
 * Exception unwinding
 *
 * Copyright 1999, 2005, 2019, 2024 Alexandre Julliard
 * Copyright 2021 Martin Storsjö
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

#ifndef __i386__

#include <stdlib.h>
#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/exception.h"
#include "wine/list.h"
#include "ntdll_misc.h"
#include "unwind.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(unwind);

#if defined(__x86_64__) && !defined(__arm64ec__)
/* PS5 C2: not a donor port. Wine master and Proton bleeding-edge still use
 * LdrFindEntryForAddress + PE directory parsing on every lookup (2026-10-12).
 * Cache only image metadata, never dynamic/callback/growable function tables.
 * As with RtlLookupFunctionEntry, callers must keep the image mapped while
 * using the returned function entry; this cache does not pin DLLs. */
ULONG64 ps5_module_generation = 1;
BOOL ps5_unwind_profile;
static BOOL ps5_unwind_title;
static LONG ps5_unwind_cache;
static ULONG ps5_unwind_poll, ps5_unwind_report;
static ULONG64 ps5_unwind_counts[PS5_UNWIND_EVENTS];
/* "xmm" is no longer produced (XMM saves compile into plans); the counter and
 * its fast_fallback_xmm field stay for log-format stability. */
enum ps5_fast_reason { PS5_FAST_STORAGE, PS5_FAST_IMAGE, PS5_FAST_ENTRY, PS5_FAST_FLAGS,
                       PS5_FAST_CHAIN, PS5_FAST_EPILOG, PS5_FAST_CODES, PS5_FAST_XMM,
                       PS5_FAST_MACHINE, PS5_FAST_METADATA, PS5_FAST_LIMIT, PS5_FAST_REASONS };
static const char *ps5_fast_names[] = { "storage", "non_image", "no_pdata_entry", "flags",
                                      "chain", "epilog", "opcodes", "xmm", "machine", "metadata", "limit" };
static ULONG64 ps5_fast_hits, ps5_fast_fallbacks[PS5_FAST_REASONS];
static ULONG ps5_unwind_tls_index = ~0u;
BOOL ps5_unwind_verify;
static LONG ps5_unwind_failed;
static struct
{
    ULONG64 sequence, generation;
    ULONG_PTR base, end, table;
    ULONG size;
} ps5_unwind_modules[8];
static unsigned int ps5_unwind_next;

/* Keys are published once and never recycled. Counting and draining are
 * atomic; a writer preempted while publishing cannot block another caller. */
struct ps5_unwind_arguments
{
    ULONG state;
    ULONG skip, frames;
    ULONG64 count;
};
struct ps5_unwind_caller_entry
{
    ULONG state;
    ULONG_PTR caller;
    unsigned int event;
    ULONG64 count;
    struct ps5_unwind_arguments arguments[8];
};
static struct ps5_unwind_caller_entry ps5_unwind_callers[64];
static ULONG64 ps5_unwind_caller_overflow, ps5_unwind_argument_overflow;
static ULONG64 ps5_unwind_depths[2][7];
static ULONG_PTR ps5_unwind_ntdll_base, ps5_unwind_ntdll_end;
static ULONG ps5_unwind_started;
static struct
{
    ULONG state;  /* 0 unused, 1 selecting, 2 sampling, 3 samples complete */
    ULONG_PTR caller;
    ULONG captures;
    struct
    {
        ULONG state;  /* 0 unused, 1 writing, 2 ready, 3 reported */
        ULONG frames;
        ULONG_PTR addresses[8];
    } chains[16];
} ps5_unwind_sites[3];

/* Check every memory region in the requested code span. NtReadVirtualMemory
 * handles protection/unmap races without dispatching a diagnostic exception. */
static BOOL ps5_unwind_read_code( ULONG_PTR address, BYTE *buffer, SIZE_T size )
{
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T copied, chunk;
    DWORD protection;

    if (address + size < address) return FALSE;
    while (size)
    {
        if (NtQueryVirtualMemory( NtCurrentProcess(), (void *)address, MemoryBasicInformation,
                                  &mbi, sizeof(mbi), NULL ) || mbi.State != MEM_COMMIT) return FALSE;
        protection = mbi.Protect & 0xff;
        if ((mbi.Protect & PAGE_GUARD) ||
            (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
             protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)) return FALSE;
        if ((ULONG_PTR)mbi.BaseAddress + mbi.RegionSize <= address) return FALSE;
        chunk = min( size, (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize - address );
        if (NtReadVirtualMemory( NtCurrentProcess(), (void *)address, buffer, chunk, &copied ) ||
            copied != chunk) return FALSE;
        buffer += chunk;
        address += chunk;
        size -= chunk;
    }
    return TRUE;
}

static BOOL ps5_unwind_address_name( ULONG_PTR pc, char *name, SIZE_T capacity,
                                     ULONG_PTR *base, ULONG_PTR *offset )
{
    LDR_DATA_TABLE_ENTRY *module;
    unsigned int i;
    if (LdrFindEntryForAddress( (void *)pc, &module )) return FALSE;
    for (i = 0; i < min( module->BaseDllName.Length / sizeof(WCHAR), capacity - 1 ); i++)
        name[i] = module->BaseDllName.Buffer[i] < 128 ? module->BaseDllName.Buffer[i] : '?';
    name[i] = 0;
    *base = (ULONG_PTR)module->DllBase;
    *offset = pc - *base;
    return TRUE;
}

static void ps5_unwind_dump_site( ULONG_PTR caller )
{
    static const char hex[] = "0123456789abcdef";
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    IMAGE_SECTION_HEADER section;
    SIZE_T copied;
    ULONG_PTR cookie, base = 0, offset = 0, section_start, section_end;
    ULONG disposition;
    unsigned int i, j;
    BOOL valid = FALSE;
    char name[96] = "unknown", section_name[9] = "", bytes[129];
    BYTE code[0x1c0];

    if (!LdrLockLoaderLock( 2, &disposition, &cookie ) && disposition == 1)
    {
        if (!ps5_unwind_address_name( caller, name, sizeof(name), &base, &offset )) goto done;
        /* Header reads are only for image/section metadata, never dumped. */
        if (NtReadVirtualMemory( NtCurrentProcess(), (void *)base, &dos, sizeof(dos), &copied ) ||
            copied != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || (LONG)dos.e_lfanew < 0) goto done;
        if (NtReadVirtualMemory( NtCurrentProcess(), (void *)(base + dos.e_lfanew), &nt, sizeof(nt), &copied ) ||
            copied != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.NumberOfSections > 96) goto done;
        for (i = 0; i < nt.FileHeader.NumberOfSections; i++)
        {
            ULONG_PTR section_address = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader) +
                                        nt.FileHeader.SizeOfOptionalHeader + i * sizeof(section);
            if (NtReadVirtualMemory( NtCurrentProcess(), (void *)section_address, &section,
                                      sizeof(section), &copied ) || copied != sizeof(section)) goto done;
            section_start = base + section.VirtualAddress;
            section_end = section_start + section.Misc.VirtualSize;
            if (caller < section_start || caller >= section_end) continue;
            memcpy( section_name, section.Name, sizeof(section.Name) );
            if (!(section.Characteristics & IMAGE_SCN_CNT_CODE) ||
                !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                caller < section_start + 0x180 || caller + 0x40 < caller ||
                caller + 0x40 > section_end) goto done;
            valid = ps5_unwind_read_code( caller - 0x180, code, sizeof(code) );
            break;
        }
done:
        LdrUnlockLoaderLock( 0, cookie );
    }
    MESSAGE( "wine: title unwind site where=%s+0x%Ix image_base=%p section=%s status=%s\n",
             name, offset, (void *)base, *section_name ? section_name : "unknown",
             valid ? "code" : "unavailable" );
    if (!valid) return;
    for (i = 0; i < sizeof(code); i += 64)
    {
        for (j = 0; j < 64; j++)
        {
            bytes[2 * j] = hex[code[i + j] >> 4];
            bytes[2 * j + 1] = hex[code[i + j] & 15];
        }
        bytes[128] = 0;
        MESSAGE( "wine: title unwind site where=%s+0x%Ix off=%s0x%x bytes=%s\n",
                 name, offset, i < 0x180 ? "-" : "+", i < 0x180 ? 0x180 - i : i - 0x180, bytes );
    }
}

static void ps5_unwind_select_site( const struct ps5_unwind_caller_entry *entry )
{
    unsigned int i;
    ULONG state, expected;
    if (entry->event != PS5_UNWIND_CAPTURE ||
        (ULONG)(NtGetTickCount() - ps5_unwind_started) < 30000) return;
    for (i = 0; i < ARRAY_SIZE(ps5_unwind_sites); i++)
    {
        state = __atomic_load_n( &ps5_unwind_sites[i].state, __ATOMIC_ACQUIRE );
        if (state == 1) return;
        if (state && ps5_unwind_sites[i].caller == entry->caller) return;
    }
    for (i = 0; i < ARRAY_SIZE(ps5_unwind_sites); i++)
    {
        expected = 0;
        if (!__atomic_compare_exchange_n( &ps5_unwind_sites[i].state, &expected, 1, FALSE,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED )) continue;
        ps5_unwind_sites[i].caller = entry->caller;
        ps5_unwind_dump_site( entry->caller );
        __atomic_store_n( &ps5_unwind_sites[i].state, 2, __ATOMIC_RELEASE );
        return;
    }
}

void ps5_unwind_sample_capture( void *caller, void **buffer, ULONG frames )
{
    ULONG captures, sample;
    SIZE_T copied;
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(ps5_unwind_sites); i++)
    {
        if (__atomic_load_n( &ps5_unwind_sites[i].state, __ATOMIC_ACQUIRE ) != 2 ||
            ps5_unwind_sites[i].caller != (ULONG_PTR)caller) continue;
        captures = __atomic_add_fetch( &ps5_unwind_sites[i].captures, 1, __ATOMIC_RELAXED );
        if (captures % 1000 || captures > 16000) return;
        sample = captures / 1000 - 1;
        ps5_unwind_sites[i].chains[sample].frames = min( frames, 8 );
        /* Copy only this API's returned address array, not stack memory. */
        if (frames && (NtReadVirtualMemory( NtCurrentProcess(), buffer,
                      ps5_unwind_sites[i].chains[sample].addresses,
                      min( frames, 8 ) * sizeof(ULONG_PTR), &copied ) ||
                      copied != min( frames, 8 ) * sizeof(ULONG_PTR)))
            ps5_unwind_sites[i].chains[sample].frames = 0;
        __atomic_store_n( &ps5_unwind_sites[i].chains[sample].state, 2, __ATOMIC_RELEASE );
        if (captures == 16000) __atomic_store_n( &ps5_unwind_sites[i].state, 3, __ATOMIC_RELEASE );
        return;
    }
}

static void ps5_unwind_report_chains(void)
{
    char frames[1024], name[96], address[128];
    BYTE byte;
    ULONG_PTR base, offset, cookie;
    ULONG disposition, expected;
    unsigned int i, j, k;
    for (i = 0; i < ARRAY_SIZE(ps5_unwind_sites); i++)
    {
        if (__atomic_load_n( &ps5_unwind_sites[i].state, __ATOMIC_ACQUIRE ) < 2) continue;
        for (j = 0; j < ARRAY_SIZE(ps5_unwind_sites[i].chains); j++)
        {
            expected = 2;
            if (!__atomic_compare_exchange_n( &ps5_unwind_sites[i].chains[j].state, &expected, 3, FALSE,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED )) continue;
            frames[0] = 0;
            for (k = 0; k < ps5_unwind_sites[i].chains[j].frames; k++)
            {
                strcpy( address, "unavailable" );
                offset = ps5_unwind_sites[i].chains[j].addresses[k];
                if (ps5_unwind_read_code( offset, &byte, 1 ) &&
                    !LdrLockLoaderLock( 2, &disposition, &cookie ) && disposition == 1)
                {
                    if (ps5_unwind_address_name( offset, name, sizeof(name), &base, &offset ))
                        snprintf( address, sizeof(address), "%s+0x%Ix", name, offset );
                    LdrUnlockLoaderLock( 0, cookie );
                }
                if (k) strcat( frames, "," );
                strcat( frames, address );
            }
            MESSAGE( "wine: title unwind chain n=%u/16 frames=%s caller=%p\n",
                     j + 1, *frames ? frames : "unavailable", (void *)ps5_unwind_sites[i].caller );
        }
    }
}

void ps5_unwind_record_caller( unsigned int event, void *caller, ULONG skip, ULONG frames )
{
    ULONG_PTR pc = (ULONG_PTR)caller;
    unsigned int hash = ((pc >> 4) ^ (pc >> 12) ^ event) % ARRAY_SIZE(ps5_unwind_callers);
    unsigned int i, j;
    ULONG state, expected;
    struct ps5_unwind_caller_entry *entry;
    struct ps5_unwind_arguments *args;

    if (event == PS5_UNWIND_VIRTUAL2 && pc >= ps5_unwind_ntdll_base && pc < ps5_unwind_ntdll_end) return;
    for (i = 0; i < ARRAY_SIZE(ps5_unwind_callers); i++)
    {
        entry = &ps5_unwind_callers[(hash + i) % ARRAY_SIZE(ps5_unwind_callers)];
        state = __atomic_load_n( &entry->state, __ATOMIC_ACQUIRE );
        if (!state)
        {
            expected = 0;
            if (!__atomic_compare_exchange_n( &entry->state, &expected, 1, FALSE,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED )) break;
            entry->caller = pc;
            entry->event = event;
            __atomic_store_n( &entry->state, 2, __ATOMIC_RELEASE );
        }
        else if (state == 1) break;  /* bounded, accounted drop; never spin */
        else if (entry->caller != pc || entry->event != event) continue;

        __atomic_add_fetch( &entry->count, 1, __ATOMIC_RELAXED );
        for (j = 0; j < ARRAY_SIZE(entry->arguments); j++)
        {
            args = &entry->arguments[j];
            state = __atomic_load_n( &args->state, __ATOMIC_ACQUIRE );
            if (!state)
            {
                expected = 0;
                if (!__atomic_compare_exchange_n( &args->state, &expected, 1, FALSE,
                                                 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED )) break;
                args->skip = skip;
                args->frames = frames;
                __atomic_store_n( &args->state, 2, __ATOMIC_RELEASE );
            }
            else if (state == 1) break;
            else if (args->skip != skip || args->frames != frames) continue;
            __atomic_add_fetch( &args->count, 1, __ATOMIC_RELAXED );
            return;
        }
        __atomic_add_fetch( &ps5_unwind_argument_overflow, 1, __ATOMIC_RELAXED );
        return;
    }
    __atomic_add_fetch( &ps5_unwind_caller_overflow, 1, __ATOMIC_RELAXED );
}

void ps5_unwind_record_depth( unsigned int event, ULONG frames )
{
    unsigned int bucket = !frames ? 0 : frames <= 4 ? 1 : frames <= 8 ? 2 :
                          frames <= 16 ? 3 : frames <= 32 ? 4 : frames <= 62 ? 5 : 6;
    __atomic_add_fetch( &ps5_unwind_depths[event == PS5_UNWIND_WALK][bucket], 1, __ATOMIC_RELAXED );
}

static void ps5_unwind_report_callers( ULONG elapsed )
{
    static const char *events[PS5_UNWIND_EVENTS] = { "lookup", "virtual", "virtual2", "capture", "walk", "dispatch" };
    struct
    {
        ULONG64 count, mode_count;
        ULONG skip, frames;
    } snapshot[ARRAY_SIZE(ps5_unwind_callers)] = {{0}};
    ULONG64 count, depths[7], overflow, argument_overflow;
    ULONG_PTR cookie, offset;
    ULONG disposition;
    unsigned int i, j, rank, best;
    LDR_DATA_TABLE_ENTRY *module;
    char name[96];
    struct ps5_unwind_caller_entry *entry;
    struct ps5_unwind_arguments *args;

    for (i = 0; i < ARRAY_SIZE(ps5_unwind_callers); i++)
    {
        entry = &ps5_unwind_callers[i];
        if (__atomic_load_n( &entry->state, __ATOMIC_ACQUIRE ) != 2) continue;
        snapshot[i].count = __atomic_exchange_n( &entry->count, 0, __ATOMIC_RELAXED );
        for (j = 0; j < ARRAY_SIZE(entry->arguments); j++)
        {
            args = &entry->arguments[j];
            if (__atomic_load_n( &args->state, __ATOMIC_ACQUIRE ) != 2) continue;
            count = __atomic_exchange_n( &args->count, 0, __ATOMIC_RELAXED );
            if (count > snapshot[i].mode_count)
            {
                snapshot[i].mode_count = count;
                snapshot[i].skip = args->skip;
                snapshot[i].frames = args->frames;
            }
        }
    }
    overflow = __atomic_exchange_n( &ps5_unwind_caller_overflow, 0, __ATOMIC_RELAXED );
    argument_overflow = __atomic_exchange_n( &ps5_unwind_argument_overflow, 0, __ATOMIC_RELAXED );
    MESSAGE( "wine: title unwind callers window_ms=%lu overflow=%I64u argument_overflow=%I64u scope=process\n",
             elapsed, overflow, argument_overflow );
    for (rank = 1; rank <= 8; rank++)
    {
        best = 0;
        for (i = 1; i < ARRAY_SIZE(snapshot); i++)
            if (snapshot[i].count > snapshot[best].count) best = i;
        if (!snapshot[best].count) break;
        entry = &ps5_unwind_callers[best];
        strcpy( name, "unknown" );
        offset = entry->caller;
        /* Try-only: reporting from exception/loader/dynamic-table paths must
         * not introduce an inverse loader-lock ordering. Names are copied
         * while locked; only code addresses and module names are inspected. */
        if (!LdrLockLoaderLock( 2, &disposition, &cookie ) && disposition == 1)
        {
            if (!LdrFindEntryForAddress( (void *)entry->caller, &module ))
            {
                for (i = 0; i < min( module->BaseDllName.Length / sizeof(WCHAR), sizeof(name) - 1 ); i++)
                    name[i] = module->BaseDllName.Buffer[i] < 128 ? module->BaseDllName.Buffer[i] : '?';
                name[i] = 0;
                offset -= (ULONG_PTR)module->DllBase;
            }
            LdrUnlockLoaderLock( 0, cookie );
        }
        MESSAGE( "wine: title unwind callers window_ms=%lu rank=%u count=%I64u where=%s+0x%Ix "
                 "skip=%lu frames=%lu event=%s mode_count=%I64u\n",
                 elapsed, rank, snapshot[best].count, name, offset, snapshot[best].skip,
                 snapshot[best].frames, events[entry->event], snapshot[best].mode_count );
        ps5_unwind_select_site( entry );
        snapshot[best].count = 0;
    }
    for (i = 0; i < ARRAY_SIZE(ps5_unwind_depths); i++)
    {
        for (j = 0; j < ARRAY_SIZE(depths); j++)
            depths[j] = __atomic_exchange_n( &ps5_unwind_depths[i][j], 0, __ATOMIC_RELAXED );
        MESSAGE( "wine: title unwind depths window_ms=%lu event=%s zero=%I64u "
                 "frames_1_4=%I64u frames_5_8=%I64u frames_9_16=%I64u frames_17_32=%I64u "
                 "frames_33_62=%I64u frames_63_plus=%I64u\n",
                 elapsed, i ? "walk" : "capture", depths[0], depths[1], depths[2], depths[3],
                 depths[4], depths[5], depths[6] );
    }
    ps5_unwind_report_chains();
}

static BOOL ps5_unwind_env( const WCHAR *key, WCHAR expected )
{
    UNICODE_STRING name, value;
    WCHAR buffer[2];
    RtlInitUnicodeString( &name, key );
    value.Buffer = buffer;
    value.Length = 0;
    value.MaximumLength = sizeof(buffer);
    return !RtlQueryEnvironmentVariable_U( NULL, &name, &value ) &&
           value.Length == sizeof(WCHAR) && buffer[0] == expected;
}

void ps5_unwind_init(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    IMAGE_NT_HEADERS *nt;

    /* Native game runtime's existing PE-visible title marker. Unlike the
     * stripped WINEPS5_IN_TITLE marker, this survives process-parameter init. */
    ps5_unwind_title = ps5_unwind_env( L"WINEPS5_TITLE_AUDIO", L'1' );
    ps5_unwind_profile = ps5_unwind_title && ps5_unwind_env( L"WINEPS5_PROFILE", L'1' );
    ps5_unwind_verify = ps5_unwind_title && ps5_unwind_env( L"WINEPS5_UNWIND_VERIFY", L'1' );
    ps5_unwind_cache = ps5_unwind_title && !ps5_unwind_env( L"WINEPS5_UNWIND_CACHE", L'0' );
    ps5_unwind_poll = ps5_unwind_report = NtGetTickCount();
    ps5_unwind_started = ps5_unwind_poll;
    if (ps5_unwind_profile &&
        !NtQueryVirtualMemory( NtCurrentProcess(), LdrInitializeThunk, MemoryBasicInformation,
                               &mbi, sizeof(mbi), NULL ) && (nt = RtlImageNtHeader( mbi.AllocationBase )))
    {
        ps5_unwind_ntdll_base = (ULONG_PTR)mbi.AllocationBase;
        ps5_unwind_ntdll_end = ps5_unwind_ntdll_base + nt->OptionalHeader.SizeOfImage;
    }
    if (ps5_unwind_title)
    {
        MESSAGE( "wine: title unwind cache=%s source=environment\n", ps5_unwind_cache ? "on" : "off" );
        ps5_qpc_init();
    }
}

void ps5_unwind_count( unsigned int event )
{
    ULONG now = NtGetTickCount(), last = __atomic_load_n( &ps5_unwind_report, __ATOMIC_RELAXED );
    ULONG64 counts[PS5_UNWIND_EVENTS];
    unsigned int i, top = 0;
    ULONG64 hits, fallbacks = 0, reasons[PS5_FAST_REASONS];
    __atomic_add_fetch( &ps5_unwind_counts[event], 1, __ATOMIC_RELAXED );
    if ((ULONG)(now - last) < 10000 ||
        !__atomic_compare_exchange_n( &ps5_unwind_report, &last, now, FALSE,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED )) return;
    for (i = 0; i < PS5_UNWIND_EVENTS; i++)
        counts[i] = __atomic_exchange_n( &ps5_unwind_counts[i], 0, __ATOMIC_RELAXED );
    hits = __atomic_exchange_n( &ps5_fast_hits, 0, __ATOMIC_RELAXED );
    for (i = 0; i < PS5_FAST_REASONS; i++)
    {
        reasons[i] = __atomic_exchange_n( &ps5_fast_fallbacks[i], 0, __ATOMIC_RELAXED );
        fallbacks += reasons[i];
        if (reasons[i] > reasons[top]) top = i;
    }
    MESSAGE( "wine: title unwind window_ms=%lu reporter_tid=%04lx scope=process "
             "lookup=%I64u virtual=%I64u virtual2=%I64u capture=%I64u walk=%I64u dispatch=%I64u "
             "fast_hits=%I64u fast_fallbacks=%I64u fast_reason_top=%s "
             "fast_fallback_storage=%I64u fast_fallback_image=%I64u fast_fallback_entry=%I64u "
             "fast_fallback_flags=%I64u fast_fallback_chain=%I64u fast_fallback_epilog=%I64u "
             "fast_fallback_opcodes=%I64u fast_fallback_xmm=%I64u fast_fallback_machine=%I64u "
             "fast_fallback_metadata=%I64u fast_fallback_limit=%I64u\n",
             now - last, GetCurrentThreadId(), counts[0], counts[1], counts[2],
             counts[3], counts[4], counts[5], hits, fallbacks, fallbacks ? ps5_fast_names[top] : "none",
             reasons[PS5_FAST_STORAGE], reasons[PS5_FAST_IMAGE], reasons[PS5_FAST_ENTRY],
             reasons[PS5_FAST_FLAGS], reasons[PS5_FAST_CHAIN], reasons[PS5_FAST_EPILOG],
             reasons[PS5_FAST_CODES], reasons[PS5_FAST_XMM], reasons[PS5_FAST_MACHINE],
             reasons[PS5_FAST_METADATA], reasons[PS5_FAST_LIMIT] );
    ps5_unwind_report_callers( now - last );
}

static void ps5_unwind_poll_tune(void)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING( L"\\??\\C:\\wineps5-tune.txt" );
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    LARGE_INTEGER offset = { .QuadPart = 0 };
    HANDLE file;
    char buffer[1024], *line, *end;
    LONG enabled = -1, verify = -1, qpc = -1;
    ULONG now = NtGetTickCount(), last = __atomic_load_n( &ps5_unwind_poll, __ATOMIC_RELAXED );

    if ((ULONG)(now - last) < 1000 ||
        !__atomic_compare_exchange_n( &ps5_unwind_poll, &last, now, FALSE,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED )) return;
    InitializeObjectAttributes( &attr, &path, OBJ_CASE_INSENSITIVE, NULL, NULL );
    if (NtOpenFile( &file, FILE_READ_DATA | SYNCHRONIZE, &attr, &io,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE )) return;
    if (!NtReadFile( file, NULL, NULL, NULL, &io, buffer, sizeof(buffer) - 1, &offset, NULL ))
    {
        buffer[io.Information] = 0;
        for (line = buffer; *line; line = end)
        {
            end = strchr( line, '\n' );
            if (end) *end++ = 0;
            else end = line + strlen(line);
            if (*line && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = 0;
            if (!strcmp( line, "unwind_cache=on" )) enabled = 1;
            if (!strcmp( line, "unwind_cache=off" )) enabled = 0;
            if (!strcmp( line, "unwind_verify=on" )) verify = 1;
            if (!strcmp( line, "unwind_verify=off" )) verify = 0;
            if (!strcmp( line, "qpc=tsc" )) qpc = 1;
            if (!strcmp( line, "qpc=syscall" )) qpc = 0;
        }
    }
    NtClose( file );
    if (enabled >= 0 && __atomic_exchange_n( &ps5_unwind_cache, enabled, __ATOMIC_RELAXED ) != enabled)
        MESSAGE( "wine: title unwind cache=%s source=C:\\wineps5-tune.txt\n", enabled ? "on" : "off" );
    if (verify >= 0 && __atomic_exchange_n( &ps5_unwind_verify, verify, __ATOMIC_RELAXED ) != verify)
        MESSAGE( "wine: title unwind verify=%s source=C:\\wineps5-tune.txt\n", verify ? "on" : "off" );
    if (qpc >= 0) ps5_qpc_tune( qpc );
}

static RUNTIME_FUNCTION *ps5_lookup_function_table( ULONG_PTR pc, ULONG_PTR *base, ULONG *size )
{
    ULONG64 generation, sequence;
    ULONG_PTR cached_base, end, table, cookie;
    ULONG cached_size, disposition;
    LDR_DATA_TABLE_ENTRY *module;
    RUNTIME_FUNCTION *ret = NULL;
    unsigned int i;

    ps5_unwind_poll_tune();
    if (!__atomic_load_n( &ps5_unwind_cache, __ATOMIC_RELAXED ))
        return RtlLookupFunctionTable( pc, base, size );
    generation = __atomic_load_n( &ps5_module_generation, __ATOMIC_ACQUIRE );
    for (i = 0; i < ARRAY_SIZE(ps5_unwind_modules); i++)
    {
        sequence = __atomic_load_n( &ps5_unwind_modules[i].sequence, __ATOMIC_ACQUIRE );
        if (sequence & 1) continue;
        if (__atomic_load_n( &ps5_unwind_modules[i].generation, __ATOMIC_RELAXED ) != generation) continue;
        cached_base = __atomic_load_n( &ps5_unwind_modules[i].base, __ATOMIC_RELAXED );
        end = __atomic_load_n( &ps5_unwind_modules[i].end, __ATOMIC_RELAXED );
        table = __atomic_load_n( &ps5_unwind_modules[i].table, __ATOMIC_RELAXED );
        cached_size = __atomic_load_n( &ps5_unwind_modules[i].size, __ATOMIC_RELAXED );
        if (pc < cached_base || pc >= end) continue;
        __atomic_thread_fence( __ATOMIC_ACQUIRE );
        if (sequence != __atomic_load_n( &ps5_unwind_modules[i].sequence, __ATOMIC_RELAXED ) ||
            generation != __atomic_load_n( &ps5_module_generation, __ATOMIC_ACQUIRE )) continue;
        *base = cached_base;
        *size = cached_size;
        return (RUNTIME_FUNCTION *)table;
    }
    /* Population is serialized with module insertion/removal. All shared
     * fields are atomic: overlapping readers never observe a torn entry. */
    if (LdrLockLoaderLock( 0, &disposition, &cookie )) return RtlLookupFunctionTable( pc, base, size );
    if (!LdrFindEntryForAddress( (void *)pc, &module ))
    {
        *base = (ULONG_PTR)module->DllBase;
        *size = 0;
        ret = RtlImageDirectoryEntryToData( module->DllBase, TRUE, IMAGE_DIRECTORY_ENTRY_EXCEPTION, size );
        i = ps5_unwind_next++ % ARRAY_SIZE(ps5_unwind_modules);
        __atomic_add_fetch( &ps5_unwind_modules[i].sequence, 1, __ATOMIC_SEQ_CST );
        __atomic_store_n( &ps5_unwind_modules[i].base, *base, __ATOMIC_RELAXED );
        __atomic_store_n( &ps5_unwind_modules[i].end, *base + module->SizeOfImage, __ATOMIC_RELAXED );
        __atomic_store_n( &ps5_unwind_modules[i].table, (ULONG_PTR)ret, __ATOMIC_RELAXED );
        __atomic_store_n( &ps5_unwind_modules[i].size, *size, __ATOMIC_RELAXED );
        __atomic_store_n( &ps5_unwind_modules[i].generation,
                          __atomic_load_n( &ps5_module_generation, __ATOMIC_RELAXED ), __ATOMIC_RELAXED );
        __atomic_add_fetch( &ps5_unwind_modules[i].sequence, 1, __ATOMIC_RELEASE );
    }
    LdrUnlockLoaderLock( 0, cookie );
    return ret;
}

void ps5_unwind_reserve_tls(void)
{
    if (!ps5_unwind_title) return;
    RtlAcquirePebLock();
    ps5_unwind_tls_index = RtlFindClearBitsAndSet( NtCurrentTeb()->Peb->TlsBitmap, 1, WOW64_TLS_MAX_NUMBER );
    RtlReleasePebLock();
}

void ps5_unwind_free_thread(void)
{
    void *address;
    SIZE_T size = 0;
    if (ps5_unwind_tls_index == ~0u) return;
    address = NtCurrentTeb()->TlsSlots[ps5_unwind_tls_index];
    NtCurrentTeb()->TlsSlots[ps5_unwind_tls_index] = (void *)~(ULONG_PTR)0;
    if (address && address != (void *)~(ULONG_PTR)0)
        NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
}

BOOL ps5_unwind_walk_enabled(void)
{
    if (!ps5_unwind_title) return FALSE;
    ps5_unwind_poll_tune();
    return __atomic_load_n( &ps5_unwind_cache, __ATOMIC_RELAXED ) &&
           !__atomic_load_n( &ps5_unwind_failed, __ATOMIC_RELAXED );
}
#endif


/***********************************************************************
 * C specific handler
 */
extern LONG __C_ExecuteExceptionFilter( EXCEPTION_POINTERS *ptrs, void *frame,
                                        PEXCEPTION_FILTER filter, BYTE *nonvolatile );

#define DUMP_SCOPE_TABLE(base,table) do { \
        for (unsigned int i = 0; i < table->Count; i++) \
            TRACE( "  %u: %p-%p handler %p target %p\n", i, \
                   (char *)base + table->ScopeRecord[i].BeginAddress,   \
                   (char *)base + table->ScopeRecord[i].EndAddress,     \
                   (char *)base + table->ScopeRecord[i].HandlerAddress, \
                   (char *)base + table->ScopeRecord[i].JumpTarget );   \
    } while(0)


/***********************************************************************
 * Dynamic unwind tables
 */

struct dynamic_unwind_entry
{
    struct list       entry;
    ULONG_PTR         base;
    ULONG_PTR         end;
    RUNTIME_FUNCTION *table;
    DWORD             count;
    DWORD             max_count;
    PGET_RUNTIME_FUNCTION_CALLBACK callback;
    PVOID             context;
};

static struct list dynamic_unwind_list = LIST_INIT(dynamic_unwind_list);

static RTL_CRITICAL_SECTION dynamic_unwind_section;
static RTL_CRITICAL_SECTION_DEBUG dynamic_unwind_debug =
{
    0, 0, &dynamic_unwind_section,
    { &dynamic_unwind_debug.ProcessLocksList, &dynamic_unwind_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": dynamic_unwind_section") }
};
static RTL_CRITICAL_SECTION dynamic_unwind_section = { &dynamic_unwind_debug, -1, 0, 0, 0, 0 };

struct module_exception_dir_entry
{
    void *exception_dir;
    void *dllbase;
    DWORD size_of_image;
    DWORD exception_dir_size;
};

#define MAX_MODULE_EXCEPTION_DIR_ENTRIES 512

struct module_exception_dir_table
{
    DWORD count;
    DWORD max_count;
    DWORD unk;
    DWORD unk2;
    struct module_exception_dir_entry entries[MAX_MODULE_EXCEPTION_DIR_ENTRIES];
};
struct module_exception_dir_table DECLSPEC_ALLOCATE(".mrdata") exception_dir_table = { 1, MAX_MODULE_EXCEPTION_DIR_ENTRIES };

C_ASSERT( sizeof(struct module_exception_dir_entry) == 0x18 );
C_ASSERT( offsetof(struct module_exception_dir_table, entries) == 0x10 );

void register_module_exception_directory( void *module )
{
    SIZE_T size = sizeof(exception_dir_table);
    struct module_exception_dir_entry e;
    void *addr = &exception_dir_table;
    IMAGE_NT_HEADERS *nt;
    ULONG old_prot;
    unsigned int i;

    if (!(nt = RtlImageNtHeader( module ))) return;
    e.exception_dir = RtlImageDirectoryEntryToData( module, TRUE, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &e.exception_dir_size );
    if (!e.exception_dir) return;
    e.size_of_image = nt->OptionalHeader.SizeOfImage;
    e.dllbase = module;

    RtlEnterCriticalSection( &dynamic_unwind_section );

    if (exception_dir_table.count == MAX_MODULE_EXCEPTION_DIR_ENTRIES) goto done;

    NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, PAGE_READWRITE, &old_prot );
    /* First entry is reserved for ntdll regardless of base address order. */
    if (exception_dir_table.count <= 2)
    {
        MEMORY_BASIC_INFORMATION mbi;

        NtQueryVirtualMemory( GetCurrentProcess(), LdrInitializeThunk, MemoryBasicInformation, &mbi, sizeof(mbi), NULL );
        if (module == mbi.AllocationBase)
        {
            exception_dir_table.entries[0] = e;
            goto done;
        }
    }
    for (i = 1; i < exception_dir_table.count; ++i)
        if ((ULONG_PTR)module < (ULONG_PTR)exception_dir_table.entries[i].dllbase) break;
    memmove( &exception_dir_table.entries[i + 1], &exception_dir_table.entries[i],
             sizeof(*exception_dir_table.entries) * (exception_dir_table.count - i) );
    exception_dir_table.entries[i] = e;
    ++exception_dir_table.count;
done:
    RtlLeaveCriticalSection( &dynamic_unwind_section );
}

void unregister_module_exception_directory( void *module )
{
    SIZE_T size = sizeof(exception_dir_table);
    void *addr = &exception_dir_table;
    ULONG old_prot;
    unsigned int i;

    RtlEnterCriticalSection( &dynamic_unwind_section );
    for (i = 1; i < exception_dir_table.count; ++i)
    {
        if (module == exception_dir_table.entries[i].dllbase)
        {
            NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, PAGE_READWRITE, &old_prot );
            memmove( &exception_dir_table.entries[i], &exception_dir_table.entries[i + 1],
                     sizeof(*exception_dir_table.entries) * (exception_dir_table.count - i - 1) );
            --exception_dir_table.count;
            break;
        }
    }
    RtlLeaveCriticalSection( &dynamic_unwind_section );
}

static RUNTIME_FUNCTION *lookup_dynamic_function_table( ULONG_PTR pc, ULONG_PTR *base, ULONG *count )
{
    struct dynamic_unwind_entry *entry;
    RUNTIME_FUNCTION *ret = NULL;

    RtlEnterCriticalSection( &dynamic_unwind_section );
    LIST_FOR_EACH_ENTRY( entry, &dynamic_unwind_list, struct dynamic_unwind_entry, entry )
    {
        if (pc >= entry->base && pc < entry->end)
        {
            *base = entry->base;
            if (entry->callback)
            {
                ret = entry->callback( pc, entry->context );
                *count = 1;
            }
            else
            {
                ret = entry->table;
                *count = entry->count;
            }
            break;
        }
    }
    RtlLeaveCriticalSection( &dynamic_unwind_section );
    return ret;
}


/**********************************************************************
 *              RtlInstallFunctionTableCallback   (NTDLL.@)
 */
BOOLEAN CDECL RtlInstallFunctionTableCallback( ULONG_PTR table, ULONG_PTR base, DWORD length,
                                               PGET_RUNTIME_FUNCTION_CALLBACK callback, PVOID context,
                                               PCWSTR dll )
{
    struct dynamic_unwind_entry *entry;

    TRACE( "%Ix %Ix %ld %p %p %s\n", table, base, length, callback, context, wine_dbgstr_w(dll) );

    /* NOTE: Windows doesn't check if the provided callback is a NULL pointer */

    /* both low-order bits must be set */
    if ((table & 0x3) != 0x3)
        return FALSE;

    entry = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*entry) );
    if (!entry)
        return FALSE;

    entry->base      = base;
    entry->end       = base + length;
    entry->table     = (RUNTIME_FUNCTION *)table;
    entry->count     = 0;
    entry->max_count = 0;
    entry->callback  = callback;
    entry->context   = context;

    RtlEnterCriticalSection( &dynamic_unwind_section );
    list_add_tail( &dynamic_unwind_list, &entry->entry );
    RtlLeaveCriticalSection( &dynamic_unwind_section );

    return TRUE;
}


/*************************************************************************
 *              RtlAddGrowableFunctionTable   (NTDLL.@)
 */
NTSTATUS WINAPI RtlAddGrowableFunctionTable( void **table, RUNTIME_FUNCTION *functions, DWORD count,
                                             DWORD max_count, ULONG_PTR base, ULONG_PTR end )
{
    struct dynamic_unwind_entry *entry;

    TRACE( "%p, %p, %lu, %lu, %Ix, %Ix\n", table, functions, count, max_count, base, end );

    entry = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*entry) );
    if (!entry)
        return STATUS_NO_MEMORY;

    entry->base      = base;
    entry->end       = end;
    entry->table     = functions;
    entry->count     = count;
    entry->max_count = max_count;
    entry->callback  = NULL;
    entry->context   = NULL;

    RtlEnterCriticalSection( &dynamic_unwind_section );
    list_add_tail( &dynamic_unwind_list, &entry->entry );
    RtlLeaveCriticalSection( &dynamic_unwind_section );

    *table = entry;

    return STATUS_SUCCESS;
}


/*************************************************************************
 *              RtlGrowFunctionTable   (NTDLL.@)
 */
void WINAPI RtlGrowFunctionTable( void *table, DWORD count )
{
    struct dynamic_unwind_entry *entry;

    TRACE( "%p, %lu\n", table, count );

    RtlEnterCriticalSection( &dynamic_unwind_section );
    LIST_FOR_EACH_ENTRY( entry, &dynamic_unwind_list, struct dynamic_unwind_entry, entry )
    {
        if (entry == table)
        {
            if (count > entry->count && count <= entry->max_count)
                entry->count = count;
            break;
        }
    }
    RtlLeaveCriticalSection( &dynamic_unwind_section );
}


/*************************************************************************
 *              RtlDeleteGrowableFunctionTable   (NTDLL.@)
 */
void WINAPI RtlDeleteGrowableFunctionTable( void *table )
{
    struct dynamic_unwind_entry *entry, *to_free = NULL;

    TRACE( "%p\n", table );

    RtlEnterCriticalSection( &dynamic_unwind_section );
    LIST_FOR_EACH_ENTRY( entry, &dynamic_unwind_list, struct dynamic_unwind_entry, entry )
    {
        if (entry == table)
        {
            to_free = entry;
            list_remove( &entry->entry );
            break;
        }
    }
    RtlLeaveCriticalSection( &dynamic_unwind_section );

    RtlFreeHeap( GetProcessHeap(), 0, to_free );
}


/**********************************************************************
 *              RtlDeleteFunctionTable   (NTDLL.@)
 */
BOOLEAN CDECL RtlDeleteFunctionTable( RUNTIME_FUNCTION *table )
{
    struct dynamic_unwind_entry *entry, *to_free = NULL;

    TRACE( "%p\n", table );

    RtlEnterCriticalSection( &dynamic_unwind_section );
    LIST_FOR_EACH_ENTRY( entry, &dynamic_unwind_list, struct dynamic_unwind_entry, entry )
    {
        if (entry->table == table)
        {
            to_free = entry;
            list_remove( &entry->entry );
            break;
        }
    }
    RtlLeaveCriticalSection( &dynamic_unwind_section );

    if (!to_free) return FALSE;

    RtlFreeHeap( GetProcessHeap(), 0, to_free );
    return TRUE;
}


/**********************************************************************
 *              RtlLookupFunctionTable   (NTDLL.@)
 */
PRUNTIME_FUNCTION WINAPI RtlLookupFunctionTable( ULONG_PTR pc, ULONG_PTR *base, ULONG *len )
{
    LDR_DATA_TABLE_ENTRY *module;

    if (LdrFindEntryForAddress( (void *)pc, &module )) return NULL;
    *base = (ULONG_PTR)module->DllBase;
#ifdef __arm64ec__
    if (RtlIsEcCode( pc ))
    {
        IMAGE_ARM64EC_METADATA *metadata = arm64ec_get_module_metadata( module->DllBase );
        if (!metadata) return NULL;
        *len = metadata->ExtraRFETableSize;
        return (RUNTIME_FUNCTION *)(*base + metadata->ExtraRFETable);
    }
#endif
    return RtlImageDirectoryEntryToData( module->DllBase, TRUE, IMAGE_DIRECTORY_ENTRY_EXCEPTION, len );
}


/***********************************************************************
 * ARM64 support
 */
#if defined(__aarch64__) || defined(__arm64ec__)

struct unwind_info_ext
{
    WORD epilog;
    BYTE codes;
    BYTE reserved;
};

struct unwind_info_epilog
{
    DWORD offset : 18;
    DWORD res : 4;
    DWORD index : 10;
};

static const BYTE unwind_code_len[256] =
{
/* 00 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 20 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 40 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 60 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 80 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* a0 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* c0 */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
/* e0 */ 4,1,2,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
};

static unsigned int get_sequence_len( BYTE *ptr, BYTE *end )
{
    unsigned int ret = 0;

    while (ptr < end)
    {
        if (*ptr == 0xe4 || *ptr == 0xe5) break;
        if ((*ptr & 0xf8) != 0xe8) ret++;  /* custom stack frames don't count */
        ptr += unwind_code_len[*ptr];
    }
    return ret;
}

static void restore_regs( int reg, int count, int pos, ARM64_NT_CONTEXT *context,
                          KNONVOLATILE_CONTEXT_POINTERS_ARM64 *ptrs )
{
    int i, offset = max( 0, pos );
    for (i = 0; i < count; i++)
    {
        if (ptrs && reg + i >= 19) (&ptrs->X19)[reg + i - 19] = (DWORD64 *)context->Sp + i + offset;
        context->X[reg + i] = ((DWORD64 *)context->Sp)[i + offset];
    }
    if (pos < 0) context->Sp += -8 * pos;
}

static void restore_fpregs( int reg, int count, int pos, ARM64_NT_CONTEXT *context,
                            KNONVOLATILE_CONTEXT_POINTERS_ARM64 *ptrs )
{
    int i, offset = max( 0, pos );
    for (i = 0; i < count; i++)
    {
        if (ptrs && reg + i >= 8) (&ptrs->D8)[reg + i - 8] = (DWORD64 *)context->Sp + i + offset;
        context->V[reg + i].D[0] = ((double *)context->Sp)[i + offset];
    }
    if (pos < 0) context->Sp += -8 * pos;
}

static void restore_qregs( int reg, int count, int pos, ARM64_NT_CONTEXT *context,
                           KNONVOLATILE_CONTEXT_POINTERS_ARM64 *ptrs )
{
    int i, offset = max( 0, pos );
    for (i = 0; i < count; i++)
    {
        if (ptrs && reg + i >= 8) (&ptrs->D8)[reg + i - 8] = (DWORD64 *)context->Sp + 2 * (i + offset);
        context->V[reg + i].Low  = ((DWORD64 *)context->Sp)[2 * (i + offset)];
        context->V[reg + i].High = ((DWORD64 *)context->Sp)[2 * (i + offset) + 1];
    }
    if (pos < 0) context->Sp += -16 * pos;
}

static void restore_any_reg( int reg, int count, int type, int pos, ARM64_NT_CONTEXT *context,
                             KNONVOLATILE_CONTEXT_POINTERS_ARM64 *ptrs )
{
    if (reg & 0x20) pos = -pos - 1;

    switch (type)
    {
    case 0:
        if (count > 1 || pos < 0) pos *= 2;
        restore_regs( reg & 0x1f, count, pos, context, ptrs );
        break;
    case 1:
        if (count > 1 || pos < 0) pos *= 2;
        restore_fpregs( reg & 0x1f, count, pos, context, ptrs );
        break;
    case 2:
        restore_qregs( reg & 0x1f, count, pos, context, ptrs );
        break;
    }
}

static void do_pac_auth( ARM64_NT_CONTEXT *context )
{
    register DWORD64 x17 __asm__( "x17" ) = context->Lr;
    register DWORD64 x16 __asm__( "x16" ) = context->Sp;

    /* This is the autib1716 instruction. The hint instruction is used here
     * as gcc does not assemble autib1716 for pre armv8.3a targets. For
     * pre-armv8.3a targets, this is just treated as a hint instruction, which
     * is ignored. */
    __asm__( "hint 0xe" : "+r"(x17) : "r"(x16) );

    context->Lr = x17;
}

static void process_unwind_codes( BYTE *ptr, BYTE *end, ARM64_NT_CONTEXT *context,
                                  KNONVOLATILE_CONTEXT_POINTERS_ARM64 *ptrs, int skip,
                                  BOOLEAN *final_pc_from_lr )
{
    unsigned int i, val, len, save_next = 2;

    /* skip codes */
    while (ptr < end && skip)
    {
        if (*ptr == 0xe4) break;
        ptr += unwind_code_len[*ptr];
        skip--;
    }

    while (ptr < end)
    {
        if ((len = unwind_code_len[*ptr]) > 1)
        {
            if (ptr + len > end) break;
            val = ptr[0] * 0x100 + ptr[1];
        }
        else val = *ptr;

        if (*ptr < 0x20)  /* alloc_s */
            context->Sp += 16 * (val & 0x1f);
        else if (*ptr < 0x40)  /* save_r19r20_x */
            restore_regs( 19, save_next, -(val & 0x1f), context, ptrs );
        else if (*ptr < 0x80) /* save_fplr */
            restore_regs( 29, 2, val & 0x3f, context, ptrs );
        else if (*ptr < 0xc0)  /* save_fplr_x */
            restore_regs( 29, 2, -(val & 0x3f) - 1, context, ptrs );
        else if (*ptr < 0xc8)  /* alloc_m */
            context->Sp += 16 * (val & 0x7ff);
        else if (*ptr < 0xcc)  /* save_regp */
            restore_regs( 19 + ((val >> 6) & 0xf), save_next, val & 0x3f, context, ptrs );
        else if (*ptr < 0xd0)  /* save_regp_x */
            restore_regs( 19 + ((val >> 6) & 0xf), save_next, -(val & 0x3f) - 1, context, ptrs );
        else if (*ptr < 0xd4)  /* save_reg */
            restore_regs( 19 + ((val >> 6) & 0xf), 1, val & 0x3f, context, ptrs );
        else if (*ptr < 0xd6)  /* save_reg_x */
            restore_regs( 19 + ((val >> 5) & 0xf), 1, -(val & 0x1f) - 1, context, ptrs );
        else if (*ptr < 0xd8)  /* save_lrpair */
        {
            restore_regs( 19 + 2 * ((val >> 6) & 0x7), 1, val & 0x3f, context, ptrs );
            restore_regs( 30, 1, (val & 0x3f) + 1, context, ptrs );
        }
        else if (*ptr < 0xda)  /* save_fregp */
            restore_fpregs( 8 + ((val >> 6) & 0x7), save_next, val & 0x3f, context, ptrs );
        else if (*ptr < 0xdc)  /* save_fregp_x */
            restore_fpregs( 8 + ((val >> 6) & 0x7), save_next, -(val & 0x3f) - 1, context, ptrs );
        else if (*ptr < 0xde)  /* save_freg */
            restore_fpregs( 8 + ((val >> 6) & 0x7), 1, val & 0x3f, context, ptrs );
        else if (*ptr == 0xde)  /* save_freg_x */
            restore_fpregs( 8 + ((val >> 5) & 0x7), 1, -(val & 0x3f) - 1, context, ptrs );
        else if (*ptr == 0xe0)  /* alloc_l */
            context->Sp += 16 * ((ptr[1] << 16) + (ptr[2] << 8) + ptr[3]);
        else if (*ptr == 0xe1)  /* set_fp */
            context->Sp = context->Fp;
        else if (*ptr == 0xe2)  /* add_fp */
            context->Sp = context->Fp - 8 * (val & 0xff);
        else if (*ptr == 0xe3)  /* nop */
            /* nop */ ;
        else if (*ptr == 0xe4)  /* end */
            break;
        else if (*ptr == 0xe5)  /* end_c */
            /* ignore */;
        else if (*ptr == 0xe6)  /* save_next */
        {
            save_next += 2;
            ptr += len;
            continue;
        }
        else if (*ptr == 0xe7)  /* save_any_reg */
        {
            restore_any_reg( ptr[1], (ptr[1] & 0x40) ? save_next : 1,
                             ptr[2] >> 6, ptr[2] & 0x3f, context, ptrs );
        }
        else if (*ptr == 0xe9)  /* MSFT_OP_MACHINE_FRAME */
        {
            context->Pc = ((DWORD64 *)context->Sp)[1];
            context->Sp = ((DWORD64 *)context->Sp)[0];
            context->ContextFlags &= ~CONTEXT_UNWOUND_TO_CALL;
            *final_pc_from_lr = FALSE;
        }
        else if (*ptr == 0xea)  /* MSFT_OP_CONTEXT */
        {
            DWORD flags = context->ContextFlags & ~CONTEXT_UNWOUND_TO_CALL;
            ARM64_NT_CONTEXT *src_ctx = (ARM64_NT_CONTEXT *)context->Sp;
            *context = *src_ctx;
            context->ContextFlags = flags | (src_ctx->ContextFlags & CONTEXT_UNWOUND_TO_CALL);
            if (ptrs)
            {
                for (i = 19; i < 29; i++) (&ptrs->X19)[i - 19] = &src_ctx->X[i];
                for (i = 8; i < 16; i++) (&ptrs->D8)[i - 8] = &src_ctx->V[i].Low;
            }
            *final_pc_from_lr = FALSE;
        }
        else if (*ptr == 0xeb)  /* MSFT_OP_EC_CONTEXT */
        {
            DWORD flags = context->ContextFlags & ~CONTEXT_UNWOUND_TO_CALL;
            ARM64EC_NT_CONTEXT *src_ctx = (ARM64EC_NT_CONTEXT *)context->Sp;
            context_x64_to_arm( context, src_ctx );
            context->ContextFlags = flags | (src_ctx->ContextFlags & CONTEXT_UNWOUND_TO_CALL);
            if (ptrs) for (i = 8; i < 16; i++) (&ptrs->D8)[i - 8] = &src_ctx->V[i].Low;
            *final_pc_from_lr = FALSE;
        }
        else if (*ptr == 0xec)  /* MSFT_OP_CLEAR_UNWOUND_TO_CALL */
        {
            context->Pc = context->Lr;
            context->ContextFlags &= ~CONTEXT_UNWOUND_TO_CALL;
            *final_pc_from_lr = FALSE;
        }
        else if (*ptr == 0xfc)  /* pac_sign_lr */
        {
            do_pac_auth( context );
        }
        else
        {
            WARN( "unsupported code %02x\n", *ptr );
            return;
        }
        save_next = 2;
        ptr += len;
    }
}

static void *unwind_packed_data( ULONG_PTR base, ULONG_PTR pc, ARM64_RUNTIME_FUNCTION *func,
                                 ARM64_NT_CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS_ARM64 *ptrs )
{
    int i;
    unsigned int len, offset;
    unsigned int int_size = func->RegI * 8, fp_size = func->RegF * 8, regsave, local_size;
    unsigned int int_regs, fp_regs, saved_regs, homing = func->H;
    BYTE prologue[40], *prologue_end, epilogue[40], *epilogue_end;
    unsigned int ppos = 0, epos = 0;
    BOOLEAN final_pc_from_lr;

    TRACE( "function %I64x-%I64x: len=%#x flag=%x regF=%u regI=%u H=%u CR=%u frame=%x\n",
           base + func->BeginAddress, base + func->BeginAddress + func->FunctionLength * 4,
           func->FunctionLength, func->Flag, func->RegF, func->RegI, func->H, func->CR, func->FrameSize );

    if (func->CR == 1) int_size += 8;
    if (func->RegF) fp_size += 8;

    regsave = ((int_size + fp_size + 8 * 8 * func->H) + 0xf) & ~0xf;
    local_size = func->FrameSize * 16 - regsave;

    int_regs = int_size / 8;
    fp_regs = fp_size / 8;
    saved_regs = regsave / 8;

    offset = ((pc - base) - func->BeginAddress) / 4;

    if (func->H && func->RegI == 0 && func->RegF == 0 && func->CR != 1)
    {
        local_size += regsave;
        homing = 0;
    }

    /* Synthesize prologue opcodes */
#define WRITE_ONE(x) do { prologue[ppos++] = epilogue[epos++] = (x); } while (0)
#define WRITE_TWO(x) do { WRITE_ONE((x) >> 8); WRITE_ONE((x) & 0xff); } while (0)
    if (func->CR == 2 || func->CR == 3)
    {
        WRITE_ONE(0xe1); /* set_fp */
        if (local_size <= 512)
        {
            WRITE_ONE(0x80 | (local_size/8 - 1)); /* save_fplr_x */
        }
        else
        {
            WRITE_ONE(0x40); /* save_fplr */
        }
    }
    if ((func->CR <= 1 && local_size > 0) || local_size > 512)
    {
        if (local_size <= 512)
        {
            WRITE_ONE(local_size/16); /* alloc_s */
        }
        else if (local_size <= 4080)
        {
            WRITE_TWO(0xc000 | (local_size/16)); /* alloc_m */
        }
        else
        {
            WRITE_ONE((local_size - 4080)/16); /* alloc_s */
            WRITE_TWO(0xc000 | (4080/16)); /* alloc_m */
        }
    }
    if (homing)
    {
        prologue[ppos++] = 0xe3; /* nop */
        prologue[ppos++] = 0xe3; /* nop */
        prologue[ppos++] = 0xe3; /* nop */
        prologue[ppos++] = 0xe3; /* nop */
    }
    if (func->RegF > 0)
    {
        if (func->RegF % 2 == 0)
        {
            WRITE_TWO(0xdc00 | ((func->RegF) << 6) | (int_regs + fp_regs - 1)); /* save_freg */
        }
        for (i = (func->RegF + 1) / 2 - 1; i >= 0; i--)
        {
            if (!i && !int_size)
                WRITE_TWO(0xda00 | ((0) << 6) | (saved_regs - 1)); /* save_fregp_x */
            else
                WRITE_TWO(0xd800 | ((2*i) << 6) | (int_regs + 2*i)); /* save_fregp d(8+2*i), int_regs + 2*i */
        }
    }
    if (func->CR == 1 && func->RegI % 2 == 0)
    {
        if (func->RegI == 0)
            WRITE_TWO(0xd400 | ((30 - 19) << 5) | (saved_regs - 1)); /* save_reg_x x30 */
        else
            WRITE_TWO(0xd000 | ((30 - 19) << 6) | (int_regs - 1)); /* save_reg x30 */
    }
    if (func->RegI > 0)
    {
        if (func->RegI % 2)
        {
            if (func->CR == 1)
            {
                WRITE_TWO(0xd600 | ((func->RegI - 1)/2) << 6 | (int_regs - 2)); /* save_lrpair x(19+RegI-1), (int_regs-2) */
                if (func->RegI == 1)
                    WRITE_ONE(saved_regs/2); /* alloc_s */
            }
            else
            {
                if (func->RegI == 1)
                    WRITE_TWO(0xd400 | ((0) << 5) | (saved_regs - 1)); /* save_reg_x x19 */
                else
                    WRITE_TWO(0xd000 | ((int_regs - 1) << 6) | (int_regs - 1)); /* save_reg x(19+int_regs-1) */
            }
        }
        for (i = func->RegI / 2 - 1; i >= 0; i--)
        {
            if (i)
                WRITE_TWO(0xc800 | ((2*i) << 6) | (2*i)); /* save_regp x(19+2*i), 2*i */
            else
                WRITE_TWO(0xcc00 | ((0) << 6) | (saved_regs - 1)); /* save_regp_x x19, saved_regs */
        }
    }
    if (func->CR == 2)
        WRITE_ONE(0xfc); /* pac_sign_lr */
    WRITE_ONE(0xe4); /* end */
    prologue_end = &prologue[ppos];
    epilogue_end = &epilogue[epos];

    if (func->Flag == 1)
    {
        if (offset < (prologue_end - prologue) || offset >= func->FunctionLength - (epilogue_end - epilogue))
        {
            /* Check prologue */
            len = get_sequence_len( prologue, prologue_end );
            if (offset < len)
            {
                process_unwind_codes( prologue, prologue_end, context, ptrs, len - offset, &final_pc_from_lr );
                return NULL;
            }
            /* Check epilogue */
            len = get_sequence_len( epilogue, epilogue_end );
            if (offset >= func->FunctionLength - (len + 1))  /* epilog */
            {
                process_unwind_codes( epilogue, epilogue_end, context, ptrs, offset - (func->FunctionLength - (len + 1)), &final_pc_from_lr );
                return NULL;
            }
        }
    }

    /* Execute full prologue */
    process_unwind_codes( prologue, prologue_end, context, ptrs, 0, &final_pc_from_lr );
    return NULL;
}

static void *unwind_full_data( ULONG_PTR base, ULONG_PTR pc, ARM64_RUNTIME_FUNCTION *func,
                               ARM64_NT_CONTEXT *context, PVOID *handler_data,
                               KNONVOLATILE_CONTEXT_POINTERS_ARM64 *ptrs,
                               BOOLEAN *final_pc_from_lr )
{
    IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY_XDATA *info;
    struct unwind_info_epilog *info_epilog;
    unsigned int i, codes, epilogs, len, offset;
    void *data;
    BYTE *end;

    info = (IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY_XDATA *)((char *)base + func->UnwindData);
    data = info + 1;
    epilogs = info->EpilogCount;
    codes = info->CodeWords;
    if (!codes && !epilogs)
    {
        struct unwind_info_ext *infoex = data;
        codes = infoex->codes;
        epilogs = infoex->epilog;
        data = infoex + 1;
    }
    info_epilog = data;
    if (!info->EpilogInHeader) data = info_epilog + epilogs;

    offset = ((pc - base) - func->BeginAddress) / 4;
    end = (BYTE *)data + codes * 4;

    TRACE( "function %I64x-%I64x: len=%#x ver=%u X=%u E=%u epilogs=%u codes=%u\n",
           base + func->BeginAddress, base + func->BeginAddress + info->FunctionLength * 4,
           info->FunctionLength, info->Version, info->ExceptionDataPresent, info->EpilogInHeader,
           epilogs, codes * 4 );

    /* check for prolog */
    if (offset < codes * 4)
    {
        len = get_sequence_len( data, end );
        if (offset < len)
        {
            process_unwind_codes( data, end, context, ptrs, len - offset, final_pc_from_lr );
            return NULL;
        }
    }

    /* check for epilog */
    if (!info->EpilogInHeader)
    {
        for (i = 0; i < epilogs; i++)
        {
            if (offset < info_epilog[i].offset) break;
            if (offset - info_epilog[i].offset < codes * 4 - info_epilog[i].index)
            {
                BYTE *ptr = (BYTE *)data + info_epilog[i].index;
                len = get_sequence_len( ptr, end );
                if (offset <= info_epilog[i].offset + len)
                {
                    process_unwind_codes( ptr, end, context, ptrs, offset - info_epilog[i].offset, final_pc_from_lr );
                    return NULL;
                }
            }
        }
    }
    else if (info->FunctionLength - offset <= codes * 4 - epilogs)
    {
        BYTE *ptr = (BYTE *)data + epilogs;
        len = get_sequence_len( ptr, end ) + 1;
        if (offset >= info->FunctionLength - len)
        {
            process_unwind_codes( ptr, end, context, ptrs, offset - (info->FunctionLength - len), final_pc_from_lr );
            return NULL;
        }
    }

    process_unwind_codes( data, end, context, ptrs, 0, final_pc_from_lr );

    /* get handler since we are inside the main code */
    if (info->ExceptionDataPresent)
    {
        DWORD *handler_rva = (DWORD *)data + codes;
        *handler_data = handler_rva + 1;
        return (char *)base + *handler_rva;
    }
    return NULL;
}

static ARM64_RUNTIME_FUNCTION *find_function_info_arm64( ULONG_PTR pc, ULONG_PTR base,
                                                         ARM64_RUNTIME_FUNCTION *func, ULONG count )
{
    int min = 0;
    int max = count - 1;

    while (min <= max)
    {
        int pos = (min + max) / 2;
        ULONG_PTR start = base + func[pos].BeginAddress;

        if (pc >= start)
        {
            ULONG len = func[pos].Flag ? func[pos].FunctionLength :
                ((IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY_XDATA *)(base + func[pos].UnwindData))->FunctionLength;
            if (pc < start + 4 * len) return func + pos;
            min = pos + 1;
        }
        else max = pos - 1;
    }
    return NULL;
}

#ifdef __arm64ec__
#define RtlVirtualUnwind RtlVirtualUnwind_arm64
#define RtlVirtualUnwind2 RtlVirtualUnwind2_arm64
#define RtlLookupFunctionEntry RtlLookupFunctionEntry_arm64
#define __C_specific_handler __C_specific_handler_arm64
#endif

/**********************************************************************
 *              RtlVirtualUnwind2   (NTDLL.@)
 */
NTSTATUS WINAPI RtlVirtualUnwind2( ULONG type, ULONG_PTR base, ULONG_PTR pc,
                                   ARM64_RUNTIME_FUNCTION *func, ARM64_NT_CONTEXT *context,
                                   BOOLEAN *mach_frame_unwound, void **handler_data,
                                   ULONG_PTR *frame_ret, KNONVOLATILE_CONTEXT_POINTERS_ARM64 *ctx_ptr,
                                   ULONG_PTR *limit_low, ULONG_PTR *limit_high,
                                   PEXCEPTION_ROUTINE *handler_ret, ULONG flags )
{
    BOOLEAN final_pc_from_lr = TRUE;
    PEXCEPTION_ROUTINE handler;
    void *data = NULL;

    TRACE( "type %lx base %I64x pc %I64x rva %I64x sp %I64x\n", type, base, pc, pc - base, context->Sp );
    if (limit_low || limit_high) FIXME( "limits not supported\n" );

    if (!func && pc == context->Lr) return STATUS_BAD_FUNCTION_TABLE;  /* invalid leaf function */

    context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;

    if (!func)  /* leaf function */
    {
        context->Pc = context->Lr;
        *frame_ret = context->Sp;
        if (handler_ret) *handler_ret = NULL;
        *handler_data = NULL;
        return STATUS_SUCCESS;
    }

    __TRY
    {
        if (func->Flag)
            handler = unwind_packed_data( base, pc, func, context, ctx_ptr );
        else
            handler = unwind_full_data( base, pc, func, context, &data, ctx_ptr, &final_pc_from_lr );

        if (final_pc_from_lr) context->Pc = context->Lr;
        *frame_ret = context->Sp;

        if (handler_ret) *handler_ret = handler;
        *handler_data = data;
    }
    __EXCEPT_PAGE_FAULT
    {
        WARN( "Access violation.\n" );
        return STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY

    TRACE( "ret: pc=%I64x lr=%I64x sp=%I64x handler=%p\n", context->Pc, context->Lr, context->Sp, handler );
    return STATUS_SUCCESS;
}


/**********************************************************************
 *              RtlVirtualUnwind   (NTDLL.@)
 */
PEXCEPTION_ROUTINE WINAPI RtlVirtualUnwind( ULONG type, ULONG_PTR base, ULONG_PTR pc,
                                            ARM64_RUNTIME_FUNCTION *func, ARM64_NT_CONTEXT *context,
                                            PVOID *handler_data, ULONG_PTR *frame_ret,
                                            KNONVOLATILE_CONTEXT_POINTERS_ARM64 *ctx_ptr )
{
    PEXCEPTION_ROUTINE handler;

    if (!RtlVirtualUnwind2( type, base, pc, func, context, NULL, handler_data,
                           frame_ret, ctx_ptr, NULL, NULL, &handler, 0 ))
        return handler;

    context->Pc = 0;
    return NULL;
}


/*******************************************************************
 *		__C_specific_handler (NTDLL.@)
 */
EXCEPTION_DISPOSITION WINAPI __C_specific_handler( EXCEPTION_RECORD *rec, void *frame,
                                                   ARM64_NT_CONTEXT *context,
                                                   DISPATCHER_CONTEXT_ARM64 *dispatch )
{
    const SCOPE_TABLE *table = dispatch->HandlerData;
    ULONG_PTR base = dispatch->ImageBase;
    ULONG_PTR pc = dispatch->ControlPc;
    unsigned int i;
    void *handler;

    TRACE( "%p %p %p %p pc %Ix\n", rec, frame, context, dispatch, pc );
    if (TRACE_ON(unwind)) DUMP_SCOPE_TABLE( base, table );

    if (dispatch->ControlPcIsUnwound) pc -= 4;

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    {
        for (i = dispatch->ScopeIndex; i < table->Count; i++)
        {
            if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
            if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
            if (table->ScopeRecord[i].JumpTarget) continue;

            if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND &&
                dispatch->TargetPc >= base + table->ScopeRecord[i].BeginAddress &&
                dispatch->TargetPc < base + table->ScopeRecord[i].EndAddress)
            {
                break;
            }
            handler = (void *)(base + table->ScopeRecord[i].HandlerAddress);
            dispatch->ScopeIndex = i + 1;
            TRACE( "scope %u calling __finally %p frame %p\n", i, handler, frame );
            __C_ExecuteExceptionFilter( ULongToPtr(TRUE), frame, handler, dispatch->NonVolatileRegisters );
        }
    }
    else
    {
        for (i = dispatch->ScopeIndex; i < table->Count; i++)
        {
            if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
            if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
            if (!table->ScopeRecord[i].JumpTarget) continue;

            if (table->ScopeRecord[i].HandlerAddress != EXCEPTION_EXECUTE_HANDLER)
            {
                EXCEPTION_POINTERS ptrs = { rec, (CONTEXT *)context };

                handler = (void *)(base + table->ScopeRecord[i].HandlerAddress);
                TRACE( "scope %u calling filter %p ptrs %p frame %p\n", i, handler, &ptrs, frame );
                switch (__C_ExecuteExceptionFilter( &ptrs, frame, handler, dispatch->NonVolatileRegisters ))
                {
                case EXCEPTION_EXECUTE_HANDLER:
                    break;
                case EXCEPTION_CONTINUE_SEARCH:
                    continue;
                case EXCEPTION_CONTINUE_EXECUTION:
                    return ExceptionContinueExecution;
                }
            }
            TRACE( "unwinding to target %Ix\n", base + table->ScopeRecord[i].JumpTarget );
            RtlUnwindEx( frame, (char *)base + table->ScopeRecord[i].JumpTarget,
                         rec, ULongToPtr(rec->ExceptionCode), (CONTEXT *)dispatch->ContextRecord,
                         dispatch->HistoryTable );
        }
    }
    return ExceptionContinueSearch;
}


/**********************************************************************
 *              RtlLookupFunctionEntry   (NTDLL.@)
 */
PARM64_RUNTIME_FUNCTION WINAPI RtlLookupFunctionEntry( ULONG_PTR pc, ULONG_PTR *base,
                                                       UNWIND_HISTORY_TABLE *table )
{
    ARM64_RUNTIME_FUNCTION *func;
    ULONG_PTR dynbase;
    ULONG size;

    if ((func = (ARM64_RUNTIME_FUNCTION *)RtlLookupFunctionTable( pc, base, &size )))
        return find_function_info_arm64( pc, *base, func, size / sizeof(*func));

    if ((func = (ARM64_RUNTIME_FUNCTION *)lookup_dynamic_function_table( pc, &dynbase, &size )))
    {
        ARM64_RUNTIME_FUNCTION *ret = find_function_info_arm64( pc, dynbase, func, size );
        if (ret) *base = dynbase;
        return ret;
    }

    *base = 0;
    return NULL;
}


/**********************************************************************
 *              RtlAddFunctionTable   (NTDLL.@)
 */
#ifndef __arm64ec__
BOOLEAN CDECL RtlAddFunctionTable( RUNTIME_FUNCTION *table, DWORD count, ULONG_PTR base )
{
    ULONG_PTR end = base;
    void *ret;

    if (count)
    {
        RUNTIME_FUNCTION *func = table + count - 1;
        ULONG len = func->Flag ? func->FunctionLength :
            ((IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY_XDATA *)(base + func->UnwindData))->FunctionLength;
        end += func->BeginAddress + 4 * len;
    }
    return !RtlAddGrowableFunctionTable( &ret, table, count, 0, base, end );
}
#else
#undef RtlVirtualUnwind
#undef RtlVirtualUnwind2
#undef RtlLookupFunctionEntry
#undef __C_specific_handler
#endif

#endif  /* __aarch64__ */


/***********************************************************************
 * ARM support
 */
#ifdef __arm__

struct unwind_info
{
    DWORD function_length : 18;
    DWORD version : 2;
    DWORD x : 1;
    DWORD e : 1;
    DWORD f : 1;
    DWORD epilog : 5;
    DWORD codes : 4;
};

struct unwind_info_ext
{
    WORD epilog;
    BYTE codes;
    BYTE reserved;
};

struct unwind_info_epilog
{
    DWORD offset : 18;
    DWORD res : 2;
    DWORD cond : 4;
    DWORD index : 8;
};

static const BYTE unwind_code_len[256] =
{
/* 00 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 20 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 40 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 60 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 80 */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
/* a0 */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
/* c0 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* e0 */ 1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,3,4,3,4,1,1,1,1,1
};

static const BYTE unwind_instr_len[256] =
{
/* 00 */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
/* 20 */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
/* 40 */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
/* 60 */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
/* 80 */ 4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
/* a0 */ 4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
/* c0 */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,4,4,4,4,4,4,4,4,
/* e0 */ 4,4,4,4,4,4,4,4,4,4,4,4,2,2,0,4,0,0,0,0,0,4,4,2,2,4,4,2,4,2,4,0
};

static unsigned int get_sequence_len( BYTE *ptr, BYTE *end, int include_end )
{
    unsigned int ret = 0;

    while (ptr < end)
    {
        if (*ptr >= 0xfd)
        {
            if (*ptr <= 0xfe && include_end)
                ret += unwind_instr_len[*ptr];
            break;
        }
        ret += unwind_instr_len[*ptr];
        ptr += unwind_code_len[*ptr];
    }
    return ret;
}

static void pop_regs_mask( int mask, CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ptrs )
{
    int i;
    for (i = 0; i <= 12; i++)
    {
        if (!(mask & (1 << i))) continue;
        if (ptrs && i >= 4 && i <= 11) (&ptrs->R4)[i - 4] = (DWORD *)context->Sp;
        if (i >= 4) (&context->R0)[i] = *(DWORD *)context->Sp;
        context->Sp += 4;
    }
}

static void pop_regs_range( int last, CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ptrs )
{
    int i;
    for (i = 4; i <= last; i++)
    {
        if (ptrs) (&ptrs->R4)[i - 4] = (DWORD *)context->Sp;
        (&context->R0)[i] = *(DWORD *)context->Sp;
        context->Sp += 4;
    }
}

static void pop_lr( int increment, CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ptrs )
{
    if (ptrs) ptrs->Lr = (DWORD *)context->Sp;
    context->Lr = *(DWORD *)context->Sp;
    context->Sp += increment;
}

static void pop_fpregs_range( int first, int last, CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ptrs )
{
    int i;
    for (i = first; i <= last; i++)
    {
        if (ptrs && i >= 8 && i <= 15) (&ptrs->D8)[i - 8] = (ULONGLONG *)context->Sp;
        context->D[i] = *(ULONGLONG *)context->Sp;
        context->Sp += 8;
    }
}

static void ms_opcode( BYTE opcode, CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ptrs )
{
    switch (opcode)
    {
    case 1:  /* MSFT_OP_MACHINE_FRAME */
        context->Pc = ((DWORD *)context->Sp)[1];
        context->Sp = ((DWORD *)context->Sp)[0];
        context->ContextFlags &= ~CONTEXT_UNWOUND_TO_CALL;
        break;
    case 2:  /* MSFT_OP_CONTEXT */
    {
        int i;
        CONTEXT *src = (CONTEXT *)context->Sp;

        *context = *src;
        if (!ptrs) break;
        for (i = 0; i < 8; i++) (&ptrs->R4)[i] = &src->R4 + i;
        ptrs->Lr = &src->Lr;
        for (i = 0; i < 8; i++) (&ptrs->D8)[i] = &src->D[i + 8];
        break;
    }
    default:
        WARN( "unsupported code %02x\n", opcode );
        break;
    }
}

static void process_unwind_codes( BYTE *ptr, BYTE *end, CONTEXT *context,
                                  KNONVOLATILE_CONTEXT_POINTERS *ptrs, int skip )
{
    unsigned int val, len;
    unsigned int i;

    /* skip codes */
    while (ptr < end && skip)
    {
        if (*ptr >= 0xfd) break;
        skip -= unwind_instr_len[*ptr];
        ptr += unwind_code_len[*ptr];
    }

    while (ptr < end)
    {
        len = unwind_code_len[*ptr];
        if (ptr + len > end) break;
        val = 0;
        for (i = 0; i < len; i++)
            val = (val << 8) | ptr[i];

        if (*ptr <= 0x7f)      /* add sp, sp, #x */
            context->Sp += 4 * (val & 0x7f);
        else if (*ptr <= 0xbf) /* pop {r0-r12,lr} */
        {
            pop_regs_mask( val & 0x1fff, context, ptrs );
            if (val & 0x2000)
                pop_lr( 4, context, ptrs );
        }
        else if (*ptr <= 0xcf) /* mov sp, rX */
            context->Sp = (&context->R0)[val & 0x0f];
        else if (*ptr <= 0xd7) /* pop {r4-rX,lr} */
        {
            pop_regs_range( (val & 0x03) + 4, context, ptrs );
            if (val & 0x04)
                pop_lr( 4, context, ptrs );
        }
        else if (*ptr <= 0xdf) /* pop {r4-rX,lr} */
        {
            pop_regs_range( (val & 0x03) + 8, context, ptrs );
            if (val & 0x04)
                pop_lr( 4, context, ptrs );
        }
        else if (*ptr <= 0xe7) /* vpop {d8-dX} */
            pop_fpregs_range( 8, (val & 0x07) + 8, context, ptrs );
        else if (*ptr <= 0xeb) /* add sp, sp, #x */
            context->Sp += 4 * (val & 0x3ff);
        else if (*ptr <= 0xed) /* pop {r0-r12,lr} */
        {
            pop_regs_mask( val & 0xff, context, ptrs );
            if (val & 0x100)
                pop_lr( 4, context, ptrs );
        }
        else if (*ptr <= 0xee) /* Microsoft-specific 0x00-0x0f, Available 0x10-0xff */
            ms_opcode( val & 0xff, context, ptrs );
        else if (*ptr <= 0xef && ((val & 0xff) <= 0x0f)) /* ldr lr, [sp], #x */
            pop_lr( 4 * (val & 0x0f), context, ptrs );
        else if (*ptr <= 0xf4) /* Available */
            WARN( "unsupported code %02x\n", *ptr );
        else if (*ptr <= 0xf5) /* vpop {dS-dE} */
            pop_fpregs_range( (val & 0xf0) >> 4, (val & 0x0f), context, ptrs );
        else if (*ptr <= 0xf6) /* vpop {dS-dE} */
            pop_fpregs_range( ((val & 0xf0) >> 4) + 16, (val & 0x0f) + 16, context, ptrs );
        else if (*ptr == 0xf7 || *ptr == 0xf9) /* add sp, sp, #x */
            context->Sp += 4 * (val & 0xffff);
        else if (*ptr == 0xf8 || *ptr == 0xfa) /* add sp, sp, #x */
            context->Sp += 4 * (val & 0xffffff);
        else if (*ptr <= 0xfc)  /* nop */
            /* nop */ ;
        else                    /* end */
            break;

        ptr += len;
    }
}

static void *unwind_packed_data( ULONG_PTR base, ULONG_PTR pc, RUNTIME_FUNCTION *func,
                                 CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ptrs )
{
    int i, pos = 0;
    int pf = 0, ef = 0, fpoffset = 0, stack = func->StackAdjust;
    int prologue_regmask = 0;
    int epilogue_regmask = 0;
    unsigned int offset, len;
    BYTE prologue[10], *prologue_end, epilogue[20], *epilogue_end;

    TRACE( "function %lx-%lx: len=%#x flag=%x ret=%u H=%u reg=%u R=%u L=%u C=%u stackadjust=%x\n",
           base + func->BeginAddress, base + func->BeginAddress + func->FunctionLength * 2,
           func->FunctionLength, func->Flag, func->Ret,
           func->H, func->Reg, func->R, func->L, func->C, func->StackAdjust );

    offset = (pc - base) - func->BeginAddress;
    if (func->StackAdjust >= 0x03f4)
    {
        pf = func->StackAdjust & 0x04;
        ef = func->StackAdjust & 0x08;
        stack = (func->StackAdjust & 3) + 1;
    }

    if (!func->R || pf)
    {
        int first = 4, last = func->Reg + 4;
        if (pf)
        {
            first = (~func->StackAdjust) & 3;
            if (func->R)
                last = 3;
        }
        for (i = first; i <= last; i++)
            prologue_regmask |= 1 << i;
        fpoffset = last + 1 - first;
    }

    if (!func->R || ef)
    {
        int first = 4, last = func->Reg + 4;
        if (ef)
        {
            first = (~func->StackAdjust) & 3;
            if (func->R)
                last = 3;
        }
        for (i = first; i <= last; i++)
            epilogue_regmask |= 1 << i;
    }

    if (func->C)
    {
        prologue_regmask |= 1 << 11;
        epilogue_regmask |= 1 << 11;
    }

    if (func->L)
    {
        prologue_regmask |= 1 << 14; /* lr */
        if (func->Ret != 0)
            epilogue_regmask |= 1 << 14; /* lr */
        else if (!func->H)
            epilogue_regmask |= 1 << 15; /* pc */
    }

    /* Synthesize prologue opcodes */
    if (stack && !pf)
    {
        if (stack <= 0x7f)
        {
            prologue[pos++] = stack; /* sub sp, sp, #x */
        }
        else
        {
            prologue[pos++] = 0xe8 | (stack >> 8); /* sub.w sp, sp, #x */
            prologue[pos++] = stack & 0xff;
        }
    }

    if (func->R && func->Reg != 7)
        prologue[pos++] = 0xe0 | func->Reg; /* vpush {d8-dX} */

    if (func->C && fpoffset == 0)
        prologue[pos++] = 0xfb; /* mov r11, sp - handled as nop16 */
    else if (func->C)
        prologue[pos++] = 0xfc; /* add r11, sp, #x - handled as nop32 */

    if (prologue_regmask & 0xf00) /* r8-r11 set */
    {
        int bitmask = prologue_regmask & 0x1fff;
        if (prologue_regmask & (1 << 14)) /* lr */
            bitmask |= 0x2000;
        prologue[pos++] = 0x80 | (bitmask >> 8); /* push.w {r0-r12,lr} */
        prologue[pos++] = bitmask & 0xff;
    }
    else if (prologue_regmask) /* r0-r7, lr set */
    {
        int bitmask = prologue_regmask & 0xff;
        if (prologue_regmask & (1 << 14)) /* lr */
            bitmask |= 0x100;
        prologue[pos++] = 0xec | (bitmask >> 8); /* push {r0-r7,lr} */
        prologue[pos++] = bitmask & 0xff;
    }

    if (func->H)
        prologue[pos++] = 0x04; /* push {r0-r3} - handled as sub sp, sp, #16 */

    prologue[pos++] = 0xff; /* end */
    prologue_end = &prologue[pos];

    /* Synthesize epilogue opcodes */
    pos = 0;
    if (stack && !ef)
    {
        if (stack <= 0x7f)
        {
            epilogue[pos++] = stack; /* sub sp, sp, #x */
        }
        else
        {
            epilogue[pos++] = 0xe8 | (stack >> 8); /* sub.w sp, sp, #x */
            epilogue[pos++] = stack & 0xff;
        }
    }

    if (func->R && func->Reg != 7)
        epilogue[pos++] = 0xe0 | func->Reg; /* vpush {d8-dX} */

    if (epilogue_regmask & 0x7f00) /* r8-r11, lr set */
    {
        int bitmask = epilogue_regmask & 0x1fff;
        if (epilogue_regmask & (3 << 14)) /* lr or pc */
            bitmask |= 0x2000;
        epilogue[pos++] = 0x80 | (bitmask >> 8); /* push.w {r0-r12,lr} */
        epilogue[pos++] = bitmask & 0xff;
    }
    else if (epilogue_regmask) /* r0-r7, pc set */
    {
        int bitmask = epilogue_regmask & 0xff;
        if (epilogue_regmask & (1 << 15)) /* pc */
            bitmask |= 0x100; /* lr */
        epilogue[pos++] = 0xec | (bitmask >> 8); /* push {r0-r7,lr} */
        epilogue[pos++] = bitmask & 0xff;
    }

    if (func->H && !(func->L && func->Ret == 0))
        epilogue[pos++] = 0x04; /* add sp, sp, #16 */
    else if (func->H && (func->L && func->Ret == 0))
    {
        epilogue[pos++] = 0xef; /* ldr lr, [sp], #20 */
        epilogue[pos++] = 5;
    }

    if (func->Ret == 1)
        epilogue[pos++] = 0xfd; /* bx lr */
    else if (func->Ret == 2)
        epilogue[pos++] = 0xfe; /* b address */
    else
        epilogue[pos++] = 0xff; /* end */
    epilogue_end = &epilogue[pos];

    if (func->Flag == 1 && offset < 4 * (prologue_end - prologue)) {
        /* Check prologue */
        len = get_sequence_len( prologue, prologue_end, 0 );
        if (offset < len)
        {
            process_unwind_codes( prologue, prologue_end, context, ptrs, len - offset );
            return NULL;
        }
    }

    if (func->Ret != 3 && 2 * func->FunctionLength - offset <= 4 * (epilogue_end - epilogue)) {
        /* Check epilogue */
        len = get_sequence_len( epilogue, epilogue_end, 1 );
        if (offset >= 2 * func->FunctionLength - len)
        {
            process_unwind_codes( epilogue, epilogue_end, context, ptrs, offset - (2 * func->FunctionLength - len) );
            return NULL;
        }
    }

    /* Execute full prologue */
    process_unwind_codes( prologue, prologue_end, context, ptrs, 0 );

    return NULL;
}

static void *unwind_full_data( ULONG_PTR base, ULONG_PTR pc, RUNTIME_FUNCTION *func,
                               CONTEXT *context, PVOID *handler_data, KNONVOLATILE_CONTEXT_POINTERS *ptrs )
{
    struct unwind_info *info;
    struct unwind_info_epilog *info_epilog;
    unsigned int i, codes, epilogs, len, offset;
    void *data;
    BYTE *end;

    info = (struct unwind_info *)((char *)base + func->UnwindData);
    data = info + 1;
    epilogs = info->epilog;
    codes = info->codes;
    if (!codes && !epilogs)
    {
        struct unwind_info_ext *infoex = data;
        codes = infoex->codes;
        epilogs = infoex->epilog;
        data = infoex + 1;
    }
    info_epilog = data;
    if (!info->e) data = info_epilog + epilogs;

    offset = (pc - base) - func->BeginAddress;
    end = (BYTE *)data + codes * 4;

    TRACE( "function %lx-%lx: len=%#x ver=%u X=%u E=%u F=%u epilogs=%u codes=%u\n",
           base + func->BeginAddress, base + func->BeginAddress + info->function_length * 2,
           info->function_length, info->version, info->x, info->e, info->f, epilogs, codes * 4 );

    /* check for prolog */
    if (offset < codes * 4 * 4 && !info->f)
    {
        len = get_sequence_len( data, end, 0 );
        if (offset < len)
        {
            process_unwind_codes( data, end, context, ptrs, len - offset );
            return NULL;
        }
    }

    /* check for epilog */
    if (!info->e)
    {
        for (i = 0; i < epilogs; i++)
        {
            /* TODO: Currently not checking epilogue conditions. */
            if (offset < 2 * info_epilog[i].offset) break;
            if (offset - 2 * info_epilog[i].offset < (codes * 4 - info_epilog[i].index) * 4)
            {
                BYTE *ptr = (BYTE *)data + info_epilog[i].index;
                len = get_sequence_len( ptr, end, 1 );
                if (offset <= 2 * info_epilog[i].offset + len)
                {
                    process_unwind_codes( ptr, end, context, ptrs, offset - 2 * info_epilog[i].offset );
                    return NULL;
                }
            }
        }
    }
    else if (2 * info->function_length - offset <= (codes * 4 - epilogs) * 4)
    {
        BYTE *ptr = (BYTE *)data + epilogs;
        len = get_sequence_len( ptr, end, 1 );
        if (offset >= 2 * info->function_length - len)
        {
            process_unwind_codes( ptr, end, context, ptrs, offset - (2 * info->function_length - len) );
            return NULL;
        }
    }

    process_unwind_codes( data, end, context, ptrs, 0 );

    /* get handler since we are inside the main code */
    if (info->x)
    {
        DWORD *handler_rva = (DWORD *)data + codes;
        *handler_data = handler_rva + 1;
        return (char *)base + *handler_rva;
    }
    return NULL;
}

static RUNTIME_FUNCTION *find_function_info( ULONG_PTR pc, ULONG_PTR base,
                                             RUNTIME_FUNCTION *func, ULONG count )
{
    int min = 0;
    int max = count - 1;

    while (min <= max)
    {
        int pos = (min + max) / 2;
        ULONG_PTR start = base + (func[pos].BeginAddress & ~1);

        if (pc >= start)
        {
            struct unwind_info *info = (struct unwind_info *)(base + func[pos].UnwindData);
            if (pc < start + 2 * (func[pos].Flag ? func[pos].FunctionLength : info->function_length))
                return func + pos;
            min = pos + 1;
        }
        else max = pos - 1;
    }
    return NULL;
}


/**********************************************************************
 *              RtlVirtualUnwind2   (NTDLL.@)
 */
NTSTATUS WINAPI RtlVirtualUnwind2( ULONG type, ULONG_PTR base, ULONG_PTR pc,
                                   RUNTIME_FUNCTION *func, CONTEXT *context,
                                   BOOLEAN *mach_frame_unwound, void **handler_data,
                                   ULONG_PTR *frame_ret, KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr,
                                   ULONG_PTR *limit_low, ULONG_PTR *limit_high,
                                   PEXCEPTION_ROUTINE *handler_ret, ULONG flags )
{
    TRACE( "type %lx base %Ix pc %Ix rva %Ix sp %lx\n", type, base, pc, pc - base, context->Sp );
    if (limit_low || limit_high) FIXME( "limits not supported\n" );

    context->Pc = 0;

    if (!func && pc == context->Lr) return STATUS_BAD_FUNCTION_TABLE;  /* invalid leaf function */

    *handler_data = NULL;

    if (!func)  /* leaf function */
        *handler_ret = NULL;
    else if (func->Flag)
        *handler_ret = unwind_packed_data( base, pc, func, context, ctx_ptr );
    else
        *handler_ret = unwind_full_data( base, pc, func, context, handler_data, ctx_ptr );

    TRACE( "ret: pc=%lx lr=%lx sp=%lx handler=%p\n", context->Pc, context->Lr, context->Sp, *handler_ret );
    if (!context->Pc)
    {
        context->Pc = context->Lr;
        context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
    }
    *frame_ret = context->Sp;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *            RtlVirtualUnwind  (NTDLL.@)
 */
PEXCEPTION_ROUTINE WINAPI RtlVirtualUnwind( ULONG type, ULONG_PTR base, ULONG_PTR pc,
                                            RUNTIME_FUNCTION *func, CONTEXT *context,
                                            PVOID *handler_data, ULONG_PTR *frame_ret,
                                            KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr )
{
    PEXCEPTION_ROUTINE handler;

    if (!RtlVirtualUnwind2( type, base, pc, func, context, NULL, handler_data,
                           frame_ret, ctx_ptr, NULL, NULL, &handler, 0 ))
        return handler;

    context->Pc = 0;
    return NULL;
}


/*******************************************************************
 *              __C_specific_handler  (NTDLL.@)
 */
EXCEPTION_DISPOSITION WINAPI __C_specific_handler( EXCEPTION_RECORD *rec, void *frame,
                                                   CONTEXT *context, DISPATCHER_CONTEXT *dispatch )
{
    const SCOPE_TABLE *table = dispatch->HandlerData;
    ULONG_PTR base = dispatch->ImageBase;
    ULONG_PTR pc = dispatch->ControlPc;
    unsigned int i;
    void *handler;

    TRACE( "%p %p %p %p pc %Ix\n", rec, frame, context, dispatch, pc );
    if (TRACE_ON(unwind)) DUMP_SCOPE_TABLE( base, table );

    if (dispatch->ControlPcIsUnwound) pc -= 2;

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    {
        for (i = dispatch->ScopeIndex; i < table->Count; i++)
        {
            if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
            if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
            if (table->ScopeRecord[i].JumpTarget) continue;

            if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND &&
                dispatch->TargetPc >= base + table->ScopeRecord[i].BeginAddress &&
                dispatch->TargetPc < base + table->ScopeRecord[i].EndAddress)
            {
                break;
            }
            handler = (void *)(base + table->ScopeRecord[i].HandlerAddress);
            dispatch->ScopeIndex = i + 1;
            TRACE( "scope %u calling __finally %p frame %p\n", i, handler, frame );
            __C_ExecuteExceptionFilter( ULongToPtr(TRUE), frame, handler, dispatch->NonVolatileRegisters );
        }
    }
    else
    {
        for (i = dispatch->ScopeIndex; i < table->Count; i++)
        {
            if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
            if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
            if (!table->ScopeRecord[i].JumpTarget) continue;

            if (table->ScopeRecord[i].HandlerAddress != EXCEPTION_EXECUTE_HANDLER)
            {
                EXCEPTION_POINTERS ptrs = { rec, context };

                handler = (void *)(base + table->ScopeRecord[i].HandlerAddress);
                TRACE( "scope %u calling filter %p ptrs %p frame %p\n", i, handler, &ptrs, frame );
                switch (__C_ExecuteExceptionFilter( &ptrs, frame, handler, dispatch->NonVolatileRegisters ))
                {
                case EXCEPTION_EXECUTE_HANDLER:
                    break;
                case EXCEPTION_CONTINUE_SEARCH:
                    continue;
                case EXCEPTION_CONTINUE_EXECUTION:
                    return ExceptionContinueExecution;
                }
            }
            TRACE( "unwinding to target %lx\n", base + table->ScopeRecord[i].JumpTarget );
            RtlUnwindEx( frame, (char *)base + table->ScopeRecord[i].JumpTarget,
                         rec, ULongToPtr(rec->ExceptionCode), dispatch->ContextRecord,
                         dispatch->HistoryTable );
        }
    }
    return ExceptionContinueSearch;
}


/**********************************************************************
 *              RtlLookupFunctionEntry   (NTDLL.@)
 */
PRUNTIME_FUNCTION WINAPI RtlLookupFunctionEntry( ULONG_PTR pc, ULONG_PTR *base,
                                                 UNWIND_HISTORY_TABLE *table )
{
    RUNTIME_FUNCTION *func;
    ULONG_PTR dynbase;
    ULONG size;

    if ((func = RtlLookupFunctionTable( pc, base, &size )))
        return find_function_info( pc, *base, func, size / sizeof(*func));

    if ((func = lookup_dynamic_function_table( pc, &dynbase, &size )))
    {
        RUNTIME_FUNCTION *ret = find_function_info( pc, dynbase, func, size );
        if (ret) *base = dynbase;
        return ret;
    }

    *base = 0;
    return NULL;
}


/**********************************************************************
 *              RtlAddFunctionTable   (NTDLL.@)
 */
BOOLEAN CDECL RtlAddFunctionTable( RUNTIME_FUNCTION *table, DWORD count, ULONG_PTR base )
{
    ULONG_PTR end = base;
    void *ret;

    if (count)
    {
        RUNTIME_FUNCTION *func = table + count - 1;
        struct unwind_info *info = (struct unwind_info *)(base + func->UnwindData);
        end += func->BeginAddress + 2 * (func->Flag ? func->FunctionLength : info->function_length);
    }
    return !RtlAddGrowableFunctionTable( &ret, table, count, 0, base, end );
}

#endif  /* __arm__ */


/***********************************************************************
 * x86-64 support
 */
#ifdef __x86_64__

union handler_data
{
    RUNTIME_FUNCTION chain;
    ULONG handler;
};

struct opcode
{
    BYTE offset;
    BYTE code : 4;
    BYTE info : 4;
};

struct UNWIND_INFO
{
    BYTE version : 3;
    BYTE flags : 5;
    BYTE prolog;
    BYTE count;
    BYTE frame_reg : 4;
    BYTE frame_offset : 4;
    struct opcode opcodes[1];  /* info->count entries */
    /* followed by handler_data */
};

#define UWOP_PUSH_NONVOL     0
#define UWOP_ALLOC_LARGE     1
#define UWOP_ALLOC_SMALL     2
#define UWOP_SET_FPREG       3
#define UWOP_SAVE_NONVOL     4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_EPILOG          6
#define UWOP_SAVE_XMM128     8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME  10

static void dump_unwind_info( ULONG64 base, RUNTIME_FUNCTION *function )
{
    static const char * const reg_names[16] =
        { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
          "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15" };

    union handler_data *handler_data;
    struct UNWIND_INFO *info;
    unsigned int i, count;

    TRACE( "**** func %lx-%lx\n", function->BeginAddress, function->EndAddress );
    for (;;)
    {
        if (function->UnwindData & 1)
        {
            RUNTIME_FUNCTION *next = (RUNTIME_FUNCTION *)((char *)base + (function->UnwindData & ~1));
            TRACE( "unwind info for function %p-%p chained to function %p-%p\n",
                   (char *)base + function->BeginAddress, (char *)base + function->EndAddress,
                   (char *)base + next->BeginAddress, (char *)base + next->EndAddress );
            function = next;
            continue;
        }
        info = (struct UNWIND_INFO *)((char *)base + function->UnwindData);

        TRACE( "unwind info at %p flags %x prolog 0x%x bytes function %p-%p\n",
               info, info->flags, info->prolog,
               (char *)base + function->BeginAddress, (char *)base + function->EndAddress );

        if (info->frame_reg)
            TRACE( "    frame register %s offset 0x%x(%%rsp)\n",
                   reg_names[info->frame_reg], info->frame_offset * 16 );

        for (i = 0; i < info->count; i++)
        {
            TRACE( "    0x%x: ", info->opcodes[i].offset );
            switch (info->opcodes[i].code)
            {
            case UWOP_PUSH_NONVOL:
                TRACE( "pushq %%%s\n", reg_names[info->opcodes[i].info] );
                break;
            case UWOP_ALLOC_LARGE:
                if (info->opcodes[i].info)
                {
                    count = *(DWORD *)&info->opcodes[i+1];
                    i += 2;
                }
                else
                {
                    count = *(USHORT *)&info->opcodes[i+1] * 8;
                    i++;
                }
                TRACE( "subq $0x%x,%%rsp\n", count );
                break;
            case UWOP_ALLOC_SMALL:
                count = (info->opcodes[i].info + 1) * 8;
                TRACE( "subq $0x%x,%%rsp\n", count );
                break;
            case UWOP_SET_FPREG:
                TRACE( "leaq 0x%x(%%rsp),%s\n",
                     info->frame_offset * 16, reg_names[info->frame_reg] );
                break;
            case UWOP_SAVE_NONVOL:
                count = *(USHORT *)&info->opcodes[i+1] * 8;
                TRACE( "movq %%%s,0x%x(%%rsp)\n", reg_names[info->opcodes[i].info], count );
                i++;
                break;
            case UWOP_SAVE_NONVOL_FAR:
                count = *(DWORD *)&info->opcodes[i+1];
                TRACE( "movq %%%s,0x%x(%%rsp)\n", reg_names[info->opcodes[i].info], count );
                i += 2;
                break;
            case UWOP_SAVE_XMM128:
                count = *(USHORT *)&info->opcodes[i+1] * 16;
                TRACE( "movaps %%xmm%u,0x%x(%%rsp)\n", info->opcodes[i].info, count );
                i++;
                break;
            case UWOP_SAVE_XMM128_FAR:
                count = *(DWORD *)&info->opcodes[i+1];
                TRACE( "movaps %%xmm%u,0x%x(%%rsp)\n", info->opcodes[i].info, count );
                i += 2;
                break;
            case UWOP_PUSH_MACHFRAME:
                TRACE( "PUSH_MACHFRAME %u\n", info->opcodes[i].info );
                break;
            case UWOP_EPILOG:
                if (info->version == 2)
                {
                    unsigned int offset;
                    if (info->opcodes[i].info)
                        offset = info->opcodes[i].offset;
                    else
                        offset = (info->opcodes[i+1].info << 8) + info->opcodes[i+1].offset;
                    TRACE("epilog %p-%p\n", (char *)base + function->EndAddress - offset,
                            (char *)base + function->EndAddress - offset + info->opcodes[i].offset );
                    i += 1;
                    break;
                }
            default:
                FIXME( "unknown code %u\n", info->opcodes[i].code );
                break;
            }
        }

        handler_data = (union handler_data *)&info->opcodes[(info->count + 1) & ~1];
        if (info->flags & UNW_FLAG_CHAININFO)
        {
            TRACE( "    chained to function %p-%p\n",
                   (char *)base + handler_data->chain.BeginAddress,
                   (char *)base + handler_data->chain.EndAddress );
            function = &handler_data->chain;
            continue;
        }
        if (info->flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER))
            TRACE( "    handler %p data at %p\n",
                   (char *)base + handler_data->handler, &handler_data->handler + 1 );
        break;
    }
}

static ULONG64 get_int_reg( CONTEXT *context, int reg )
{
    return *(&context->Rax + reg);
}

static void set_int_reg( CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr, int reg, ULONG64 *val )
{
    *(&context->Rax + reg) = *val;
    if (ctx_ptr) ctx_ptr->IntegerContext[reg] = val;
}

static void set_float_reg( CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr, int reg, M128A *val )
{
    /* Use a memcpy() to avoid issues if val is misaligned. */
    memcpy(&context->Xmm0 + reg, val, sizeof(*val));
    if (ctx_ptr) ctx_ptr->FloatingContext[reg] = val;
}

static int get_opcode_size( struct opcode op )
{
    switch (op.code)
    {
    case UWOP_ALLOC_LARGE:
        return 2 + (op.info != 0);
    case UWOP_SAVE_NONVOL:
    case UWOP_SAVE_XMM128:
    case UWOP_EPILOG:
        return 2;
    case UWOP_SAVE_NONVOL_FAR:
    case UWOP_SAVE_XMM128_FAR:
        return 3;
    default:
        return 1;
    }
}

static void *get_main_function_start( const RUNTIME_FUNCTION *f, ULONG_PTR base )
{
    const struct UNWIND_INFO *info;
    const union handler_data *data;

    while (f->UnwindData)
    {
        info = (const struct UNWIND_INFO *)((char *)base + f->UnwindData);
        if (!(info->flags & UNW_FLAG_CHAININFO)) break;

        data = (union handler_data *)&info->opcodes[(info->count + 1) & ~1];
        f = &data->chain;
    }
    return (char *)base + f->BeginAddress;
}

static BOOL is_address_in_function( BYTE *pc, ULONG64 base, const RUNTIME_FUNCTION *function )
{
    RUNTIME_FUNCTION *f;
    ULONG_PTR pc_base;

    if (pc - (BYTE *)base >= function->BeginAddress && pc - (BYTE *)base < function->EndAddress) return TRUE;
    if (!(f = RtlLookupFunctionEntry( (ULONG_PTR)pc, &pc_base, NULL ))) return FALSE;
    return get_main_function_start( f, pc_base ) == get_main_function_start( function, base );
}

static BOOL is_inside_epilog( BYTE *pc, ULONG64 base, const RUNTIME_FUNCTION *function )
{
    /* add or lea must be the first instruction, and it must have a rex.W prefix */
    if ((pc[0] & 0xf8) == 0x48)
    {
        switch (pc[1])
        {
        case 0x81: /* add $nnnn,%rsp */
            if (pc[0] == 0x48 && pc[2] == 0xc4)
            {
                pc += 7;
                break;
            }
            return FALSE;
        case 0x83: /* add $n,%rsp */
            if (pc[0] == 0x48 && pc[2] == 0xc4)
            {
                pc += 4;
                break;
            }
            return FALSE;
        case 0x8d: /* lea n(reg),%rsp */
            if (pc[0] & 0x06) return FALSE;  /* rex.RX must be cleared */
            if (((pc[2] >> 3) & 7) != 4) return FALSE;  /* dest reg mus be %rsp */
            if ((pc[2] & 7) == 4) return FALSE;  /* no SIB byte allowed */
            if ((pc[2] >> 6) == 1)  /* 8-bit offset */
            {
                pc += 4;
                break;
            }
            if ((pc[2] >> 6) == 2)  /* 32-bit offset */
            {
                pc += 7;
                break;
            }
            return FALSE;
        }
    }

    /* now check for various pop instructions */

    for (;;)
    {
        BYTE rex = 0;

        if ((*pc & 0xf0) == 0x40) rex = *pc++ & 0x0f;  /* rex prefix */

        switch (*pc)
        {
        case 0x58: /* pop %rax/%r8 */
        case 0x59: /* pop %rcx/%r9 */
        case 0x5a: /* pop %rdx/%r10 */
        case 0x5b: /* pop %rbx/%r11 */
        case 0x5c: /* pop %rsp/%r12 */
        case 0x5d: /* pop %rbp/%r13 */
        case 0x5e: /* pop %rsi/%r14 */
        case 0x5f: /* pop %rdi/%r15 */
            pc++;
            continue;
        case 0xc2: /* ret $nn */
        case 0xc3: /* ret */
            return TRUE;
        case 0xe9: /* jmp nnnn */
            pc += 5 + *(LONG *)(pc + 1);
            return !is_address_in_function( pc, base, function );
        case 0xeb: /* jmp n */
            pc += 2 + (signed char)pc[1];
            return !is_address_in_function( pc, base, function );
        case 0xf3: /* rep; ret (for amd64 prediction bug) */
            return pc[1] == 0xc3;
        case 0xff: /* jmp */
            if (rex && rex != 8) return FALSE;
            if (pc[1] == 0x25) return TRUE;
            return rex && ((pc[1] >> 3) & 7) == 4;
        }
        return FALSE;
    }
}

/* execute a function epilog, which must have been validated with is_inside_epilog() */
static void interpret_epilog( BYTE *pc, CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr )
{
    for (;;)
    {
        BYTE rex = 0;

        if ((*pc & 0xf0) == 0x40) rex = *pc++ & 0x0f;  /* rex prefix */

        switch (*pc)
        {
        case 0x58: /* pop %rax/r8 */
        case 0x59: /* pop %rcx/r9 */
        case 0x5a: /* pop %rdx/r10 */
        case 0x5b: /* pop %rbx/r11 */
        case 0x5c: /* pop %rsp/r12 */
        case 0x5d: /* pop %rbp/r13 */
        case 0x5e: /* pop %rsi/r14 */
        case 0x5f: /* pop %rdi/r15 */
            set_int_reg( context, ctx_ptr, *pc - 0x58 + (rex & 1) * 8, (ULONG64 *)context->Rsp );
            context->Rsp += sizeof(ULONG64);
            pc++;
            continue;
        case 0x81: /* add $nnnn,%rsp */
            context->Rsp += *(LONG *)(pc + 2);
            pc += 2 + sizeof(LONG);
            continue;
        case 0x83: /* add $n,%rsp */
            context->Rsp += (signed char)pc[2];
            pc += 3;
            continue;
        case 0x8d:
            if ((pc[1] >> 6) == 1)  /* lea n(reg),%rsp */
            {
                context->Rsp = get_int_reg( context, (pc[1] & 7) + (rex & 1) * 8 ) + (signed char)pc[2];
                pc += 3;
            }
            else  /* lea nnnn(reg),%rsp */
            {
                context->Rsp = get_int_reg( context, (pc[1] & 7) + (rex & 1) * 8 ) + *(LONG *)(pc + 2);
                pc += 2 + sizeof(LONG);
            }
            continue;
        case 0xc2: /* ret $nn */
            context->Rip = *(ULONG64 *)context->Rsp;
            context->Rsp += sizeof(ULONG64) + *(WORD *)(pc + 1);
            return;
        case 0xe9: /* jmp nnnn */
        case 0xeb: /* jmp n */
        case 0xc3: /* ret */
        case 0xf3: /* rep; ret */
        case 0xff: /* jmp */
            context->Rip = *(ULONG64 *)context->Rsp;
            context->Rsp += sizeof(ULONG64);
            return;
        }
        return;
    }
}

static RUNTIME_FUNCTION *find_function_info( ULONG_PTR pc, ULONG_PTR base,
                                             RUNTIME_FUNCTION *func, ULONG count, BOOL resolve_indirect )
{
    int min = 0;
    int max = count - 1;

    while (min <= max)
    {
        int pos = (min + max) / 2;
        if (pc < base + func[pos].BeginAddress) max = pos - 1;
        else if (pc >= base + func[pos].EndAddress) min = pos + 1;
        else
        {
            func += pos;
            while (resolve_indirect && (func->UnwindData & 1))  /* follow chained entry */
                func = (RUNTIME_FUNCTION *)(base + (func->UnwindData & ~1));
            return func;
        }
    }
    return NULL;
}

#ifndef __arm64ec__
struct ps5_unwind_step
{
    RUNTIME_FUNCTION *function, *image_function;
    RUNTIME_FUNCTION function_copy;
    struct UNWIND_INFO *info;
    DWORD header;
    BYTE codes[72];
    DWORD rsp_delta;
    BYTE frame_reg, frame_offset, use_frame_rsp, saves, code_count;
    struct { DWORD offset; BYTE reg, frame; } saved[8];
};

/* is_inside_epilog() outcome for one exact control PC. SKIP: the interpreter
 * does not test (prolog, no codes). NO/YES depend only on the code bytes it
 * reads, kept in code[]. JMP: those bytes end in a jmp leaving the function
 * range, so the verdict also depends on is_address_in_function( target ). */
enum ps5_epilog_kind { PS5_EPILOG_SKIP, PS5_EPILOG_NO, PS5_EPILOG_YES, PS5_EPILOG_JMP, PS5_EPILOG_LIMIT };

struct ps5_unwind_epilog
{
    ULONG_PTR target;
    BYTE kind, length;
    BYTE code[30];
};

/* Keep the common one-step plan small. Only 64 chain sidecars are retained,
 * not eight metadata copies in each of the 512 ordinary cache entries. */
struct ps5_unwind_plan
{
    ULONG_PTR pc, base;
    ULONG64 generation;
    BYTE count;
    struct ps5_unwind_epilog epilog;
    struct ps5_unwind_step steps[8];
};

struct ps5_unwind_cached_plan
{
    ULONG_PTR pc, base;
    ULONG64 generation;
    BYTE count, chain_slot;
    struct ps5_unwind_epilog epilog;
    struct ps5_unwind_step first;
};

struct ps5_unwind_thread
{
    BOOL busy;
    BOOL verify_next;
    ULONG captures;
    struct ps5_unwind_cached_plan plans[512];
    struct
    {
        ULONG_PTR pc;
        ULONG64 generation;
        struct ps5_unwind_step steps[7];
    } chains[64];
};

static struct ps5_unwind_thread *ps5_unwind_thread_cache(void)
{
    struct ps5_unwind_thread *cache;
    void *address = NULL;
    SIZE_T size = sizeof(*cache);
    if (ps5_unwind_tls_index == ~0u) return NULL;
    cache = NtCurrentTeb()->TlsSlots[ps5_unwind_tls_index];
    if (cache == (void *)~(ULONG_PTR)0) return NULL;
    if (!cache)
    {
        /* A capture may itself occur in allocator hooks. Publish a sentinel
         * before native VM allocation, never acquire the process heap here. */
        NtCurrentTeb()->TlsSlots[ps5_unwind_tls_index] = (void *)~(ULONG_PTR)0;
        if (NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE )) return NULL;
        cache = address;
        NtCurrentTeb()->TlsSlots[ps5_unwind_tls_index] = cache;
    }
    return cache;
}

void ps5_unwind_prepare_capture(void)
{
    struct ps5_unwind_thread *cache;
    if (!ps5_unwind_walk_enabled() ||
        !__atomic_load_n( &ps5_unwind_verify, __ATOMIC_RELAXED )) return;
    cache = ps5_unwind_thread_cache();
    if (cache && !cache->busy) cache->verify_next = !(++cache->captures & 1023);
}

/* Mirror is_inside_epilog() decision for decision, reading no byte it would
 * not read. Report the leading code bytes it depends on and, for a final jmp,
 * the target it hands to is_address_in_function(). code[] spans every byte up
 * to the last one read; skipped immediates are included, which only makes the
 * hit-time comparison stricter. */
static unsigned int ps5_unwind_scan_epilog( const BYTE *start, struct ps5_unwind_epilog *epilog )
{
    const BYTE *pc = start, *last = start;
    unsigned int kind = PS5_EPILOG_NO;
    BYTE rex;

    if ((pc[0] & 0xf8) == 0x48)
    {
        last = pc + 1;
        switch (pc[1])
        {
        case 0x81: /* add $nnnn,%rsp */
        case 0x83: /* add $n,%rsp */
            if (pc[0] != 0x48) goto done;
            last = pc + 2;
            if (pc[2] != 0xc4) goto done;
            pc += pc[1] == 0x81 ? 7 : 4;
            break;
        case 0x8d: /* lea n(reg),%rsp */
            if (pc[0] & 0x06) goto done;
            last = pc + 2;
            if (((pc[2] >> 3) & 7) != 4 || (pc[2] & 7) == 4) goto done;
            if ((pc[2] >> 6) == 1) pc += 4;
            else if ((pc[2] >> 6) == 2) pc += 7;
            else goto done;
            break;
        }
    }

    for (;;)
    {
        if (pc + 2 - start > sizeof(epilog->code)) return PS5_EPILOG_LIMIT;
        rex = 0;
        if ((*pc & 0xf0) == 0x40) rex = *pc++ & 0x0f;
        if (pc > last) last = pc;
        switch (*pc)
        {
        case 0x58: case 0x59: case 0x5a: case 0x5b:
        case 0x5c: case 0x5d: case 0x5e: case 0x5f: /* pop */
            pc++;
            continue;
        case 0xc2: /* ret $nn */
        case 0xc3: /* ret */
            kind = PS5_EPILOG_YES;
            break;
        case 0xe9: /* jmp nnnn */
            last = pc + 4;
            epilog->target = (ULONG_PTR)(pc + 5 + *(const LONG *)(pc + 1));
            kind = PS5_EPILOG_JMP;
            break;
        case 0xeb: /* jmp n */
            last = pc + 1;
            epilog->target = (ULONG_PTR)(pc + 2 + (signed char)pc[1]);
            kind = PS5_EPILOG_JMP;
            break;
        case 0xf3: /* rep; ret */
            last = pc + 1;
            if (pc[1] == 0xc3) kind = PS5_EPILOG_YES;
            break;
        case 0xff: /* jmp */
            if (rex && rex != 8) break;
            last = pc + 1;
            if (pc[1] == 0x25 || (rex && ((pc[1] >> 3) & 7) == 4)) kind = PS5_EPILOG_YES;
            break;
        }
        break;
    }
done:
    if (last - start >= sizeof(epilog->code)) return PS5_EPILOG_LIMIT;
    epilog->length = last - start + 1;
    memcpy( epilog->code, start, epilog->length );
    return kind;
}

/* Classify the initial record's control PC once. The authoritative
 * is_inside_epilog() runs too; any disagreement with the scan falls back. */
static unsigned int ps5_unwind_compile_epilog( struct ps5_unwind_epilog *epilog, ULONG_PTR pc,
                                               ULONG_PTR base, RUNTIME_FUNCTION *function )
{
    unsigned int kind = ps5_unwind_scan_epilog( (const BYTE *)pc, epilog );
    BOOL inside;

    if (kind == PS5_EPILOG_LIMIT) return PS5_FAST_LIMIT;
    /* is_address_in_function()'s own range test: pure in the validated entry */
    if (kind == PS5_EPILOG_JMP && epilog->target - base >= function->BeginAddress &&
        epilog->target - base < function->EndAddress) kind = PS5_EPILOG_NO;
    inside = is_inside_epilog( (BYTE *)pc, base, function );
    if (kind == PS5_EPILOG_JMP ? inside == is_address_in_function( (BYTE *)epilog->target, base, function )
                               : inside != (kind == PS5_EPILOG_YES)) return PS5_FAST_EPILOG;
    epilog->kind = kind;
    return PS5_FAST_REASONS;
}

static unsigned int ps5_unwind_compile_plan( struct ps5_unwind_plan *plan, ULONG_PTR pc,
                                             ULONG_PTR base, RUNTIME_FUNCTION *function,
                                             RUNTIME_FUNCTION *table, ULONG table_count )
{
    const struct UNWIND_INFO *info;
    struct ps5_unwind_step *step;
    RUNTIME_FUNCTION *image_function = function;
    unsigned int i, size, reg, reason;
    ULONG prolog_offset;
    ULONG64 delta;
    DWORD save_offset;
    BOOL from_frame;

    plan->pc = pc;
    plan->base = base;
    plan->generation = __atomic_load_n( &ps5_module_generation, __ATOMIC_ACQUIRE );
    plan->count = 0;
    plan->epilog.kind = PS5_EPILOG_SKIP;
    for (;;)
    {
        if (plan->count == ARRAY_SIZE(plan->steps)) return PS5_FAST_CHAIN;
        info = (void *)(base + function->UnwindData);
        /* EH/UH flags have no effect for UNW_FLAG_NHANDLER. Their handler
         * fields are never inspected by the no-dispatch interpreter. */
        if ((info->version != 1 && info->version != 2) ||
            (info->flags & ~(UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER | UNW_FLAG_CHAININFO)))
            return PS5_FAST_FLAGS;
        step = &plan->steps[plan->count];
        prolog_offset = ~0u;
        if (pc >= base + function->BeginAddress && pc < base + function->BeginAddress + info->prolog)
            prolog_offset = pc - base - function->BeginAddress;

        memset( step, 0, sizeof(*step) );
        step->function = function;
        step->image_function = image_function;
        step->function_copy = *function;
        step->info = (void *)info;
        memcpy( &step->header, info, sizeof(step->header) );
        step->frame_reg = info->frame_reg;
        step->frame_offset = info->frame_offset;
        if (!plan->count && prolog_offset == ~0u && info->count)
        {
            if ((reason = ps5_unwind_compile_epilog( &plan->epilog, pc, base, function )) != PS5_FAST_REASONS)
                return reason;
            /* The interpreter's epilog emulation reads only the header, never
             * the codes or chained records: no code snapshot, one step. */
            if (plan->epilog.kind == PS5_EPILOG_YES)
            {
                plan->count = 1;
                return PS5_FAST_REASONS;
            }
        }
        if (info->count > sizeof(step->codes) / sizeof(struct opcode)) return PS5_FAST_CODES;
        step->code_count = info->count;
        memcpy( step->codes, info->opcodes, info->count * sizeof(struct opcode) );
        delta = 0;
        for (i = 0; i < info->count; i += size)
        {
            struct opcode op = info->opcodes[i];
            size = get_opcode_size( op );
            if (i + size > info->count) return PS5_FAST_CODES;
            if (prolog_offset < op.offset) continue;
            reg = op.info;
            switch (op.code)
            {
            case UWOP_ALLOC_SMALL:
                delta += (op.info + 1) * 8;
                break;
            case UWOP_ALLOC_LARGE:  /* any nonzero info is the 32-bit form, as in the interpreter */
                delta += op.info ? *(DWORD *)&info->opcodes[i + 1] : *(USHORT *)&info->opcodes[i + 1] * 8;
                break;
            case UWOP_SET_FPREG:
                /* Without a frame register the interpreter reuses the initial
                 * or inherited frame base, exactly what *frame then holds. */
                delta = 0;
                step->use_frame_rsp = TRUE;
                break;
            case UWOP_PUSH_NONVOL:
            case UWOP_SAVE_NONVOL:
            case UWOP_SAVE_NONVOL_FAR:
                /* Restoring RSP would move later RSP-relative slots. Other
                 * registers, volatile or not, only feed later records. */
                if (reg == 4) return PS5_FAST_CODES;
                if (step->saves == ARRAY_SIZE(step->saved) || delta > MAXDWORD) return PS5_FAST_LIMIT;
                if (op.code == UWOP_PUSH_NONVOL)
                {
                    save_offset = delta;
                    from_frame = step->use_frame_rsp;
                    delta += 8;
                }
                else
                {
                    save_offset = op.code == UWOP_SAVE_NONVOL ? *(USHORT *)&info->opcodes[i + 1] * 8 :
                                                              *(DWORD *)&info->opcodes[i + 1];
                    from_frame = TRUE;
                }
                step->saved[step->saves].offset = save_offset;
                step->saved[step->saves].reg = reg;
                step->saved[step->saves++].frame = from_frame;
                break;
            case UWOP_SAVE_XMM128:
            case UWOP_SAVE_XMM128_FAR:
                /* The interpreter only copies 16 bytes into context->Xmm<n>
                 * (ctx_ptr is NULL here). No later frame reads XMM state:
                 * RtlVirtualUnwind2, is_inside_epilog(), interpret_epilog()
                 * and the walk use RIP, RSP, GP registers and the frame
                 * base, none of which these codes change. The walk context
                 * is private and discarded, so skipping them is exact. */
                break;
            case UWOP_EPILOG:  /* version-2 epilog descriptors are interpreter no-ops */
                if (info->version != 2) return PS5_FAST_CODES;
                break;
            case UWOP_PUSH_MACHFRAME:
                return PS5_FAST_MACHINE;
            default:
                return PS5_FAST_CODES;
            }
            if (delta > MAXDWORD) return PS5_FAST_LIMIT;
        }
        step->rsp_delta = delta;
        ++plan->count;
        if (!(info->flags & UNW_FLAG_CHAININFO)) return PS5_FAST_REASONS;
        function = (RUNTIME_FUNCTION *)&info->opcodes[(info->count + 1) & ~1];
        if (function->UnwindData & 1) return PS5_FAST_CHAIN;
        image_function = find_function_info( base + function->BeginAddress, base, table, table_count, FALSE );
        if (!image_function || memcmp( image_function, function, sizeof(*function) )) return PS5_FAST_CHAIN;
    }
}

static BOOL ps5_unwind_step_valid( const struct ps5_unwind_step *step )
{
    return !memcmp( step->function, &step->function_copy, sizeof(step->function_copy) ) &&
           (step->image_function == step->function ||
            !memcmp( step->image_function, &step->function_copy, sizeof(step->function_copy) )) &&
           !memcmp( step->info, &step->header, sizeof(step->header) ) &&
           !memcmp( step->info->opcodes, step->codes, step->code_count * sizeof(struct opcode) );
}

static void ps5_unwind_execute_step( const struct ps5_unwind_step *step, CONTEXT *context,
                                     ULONG_PTR *frame, ULONG_PTR *frame_ret )
{
    ULONG_PTR rsp = context->Rsp;
    unsigned int i;
    /* A chain recomputes FP bases after earlier saved registers have been
     * restored. A frameless chained record inherits the preceding frame. */
    if (step->frame_reg) *frame = get_int_reg( context, step->frame_reg ) - step->frame_offset * 16;
    for (i = 0; i < step->saves; i++)
        *(&context->Rax + step->saved[i].reg) =
            *(ULONG64 *)((step->saved[i].frame ? *frame : rsp) + step->saved[i].offset);
    context->Rsp = (step->use_frame_rsp ? *frame : rsp) + step->rsp_delta;
    if (step->use_frame_rsp) *frame_ret = *frame;
}


static unsigned int ps5_unwind_fast_frame( struct ps5_unwind_thread *cache, CONTEXT *context,
                                           ULONG_PTR *frame_ret )
{
    struct ps5_unwind_cached_plan *plan = &cache->plans[(context->Rip >> 4 ^ context->Rip >> 12) & 511];
    struct ps5_unwind_plan compiled;
    RUNTIME_FUNCTION *table, *function;
    ULONG_PTR base, frame;
    ULONG size;
    unsigned int reason, i, slot;
    ULONG64 generation = __atomic_load_n( &ps5_module_generation, __ATOMIC_ACQUIRE );

    if (plan->pc != context->Rip || plan->generation != generation ||
        (plan->count > 1 && (cache->chains[plan->chain_slot].pc != plan->pc ||
                            cache->chains[plan->chain_slot].generation != generation)))
    {
        if (!(table = ps5_lookup_function_table( context->Rip, &base, &size ))) return PS5_FAST_IMAGE;
        if (!(function = find_function_info( context->Rip, base, table, size / sizeof(*table), FALSE )))
            return PS5_FAST_ENTRY;
        if (function->UnwindData & 1) return PS5_FAST_CHAIN;
        reason = ps5_unwind_compile_plan( &compiled, context->Rip, base, function, table, size / sizeof(*table) );
        if (reason != PS5_FAST_REASONS) return reason;
        plan->pc = compiled.pc;
        plan->base = compiled.base;
        plan->generation = compiled.generation;
        plan->count = compiled.count;
        plan->epilog = compiled.epilog;
        plan->first = compiled.steps[0];
        if (compiled.count > 1)
        {
            slot = (context->Rip >> 5 ^ context->Rip >> 13) & 63;
            plan->chain_slot = slot;
            cache->chains[slot].pc = compiled.pc;
            cache->chains[slot].generation = compiled.generation;
            memcpy( cache->chains[slot].steps, &compiled.steps[1],
                    (compiled.count - 1) * sizeof(compiled.steps[0]) );
        }
    }
    else
    {
        if (!ps5_unwind_step_valid( &plan->first ))
        {
            plan->pc = 0;
            return PS5_FAST_METADATA;
        }
        for (i = 1; i < plan->count; i++)
            if (!ps5_unwind_step_valid( &cache->chains[plan->chain_slot].steps[i - 1] ))
            {
                plan->pc = 0;
                return PS5_FAST_METADATA;
            }
        /* The epilog verdict depends on these code bytes (and a jmp target). */
        if (plan->epilog.kind != PS5_EPILOG_SKIP &&
            memcmp( (const void *)plan->pc, plan->epilog.code, plan->epilog.length ))
        {
            plan->pc = 0;
            return PS5_FAST_METADATA;
        }
    }
    frame = *frame_ret = context->Rsp;
    if (plan->epilog.kind == PS5_EPILOG_YES ||
        (plan->epilog.kind == PS5_EPILOG_JMP &&
         !is_address_in_function( (BYTE *)plan->epilog.target, plan->base, plan->first.function )))
    {
        /* RtlVirtualUnwind2's own emulation on the validated bytes; its frame
         * result is the post-return RSP - 8 with a frame register, else RSP. */
        interpret_epilog( (BYTE *)context->Rip, context, NULL );
        if (plan->first.frame_reg) *frame_ret = context->Rsp - 8;
        return PS5_FAST_REASONS;
    }
    ps5_unwind_execute_step( &plan->first, context, &frame, frame_ret );
    for (i = 1; i < plan->count; i++)
        ps5_unwind_execute_step( &cache->chains[plan->chain_slot].steps[i - 1], context, &frame, frame_ret );
    context->Rip = *(ULONG64 *)context->Rsp;
    context->Rsp += 8;
    return PS5_FAST_REASONS;
}

static ULONG ps5_unwind_walk_context( CONTEXT *context, void **buffer, ULONG count, ULONG flags,
                                      struct ps5_unwind_thread *cache, ULONG compare_count, BOOL *mismatch )
{
    RUNTIME_FUNCTION *function;
    PEXCEPTION_ROUTINE handler;
    ULONG_PTR frame, base;
    void *data;
    ULONG i, skip = flags >> 8, entries = 0;
    unsigned int reason;
    for (i = 0; i < count; i++)
    {
        reason = cache ? ps5_unwind_fast_frame( cache, context, &frame ) : PS5_FAST_STORAGE;
        if (reason != PS5_FAST_REASONS)
        {
            if (cache && ps5_unwind_profile)
                __atomic_add_fetch( &ps5_fast_fallbacks[reason], 1, __ATOMIC_RELAXED );
            function = RtlLookupFunctionEntry( context->Rip, &base, NULL );
            if (!function) break;  /* preserve Wine's existing leaf-frame stop */
            if (RtlVirtualUnwind2( UNW_FLAG_NHANDLER, base, context->Rip, function, context, NULL,
                                   &data, &frame, NULL, NULL, NULL, &handler, 0 )) break;
        }
        else if (ps5_unwind_profile) __atomic_add_fetch( &ps5_fast_hits, 1, __ATOMIC_RELAXED );
        if (!context->Rip || !frame || !is_valid_frame( frame ) ||
            context->Rsp == (ULONG_PTR)NtCurrentTeb()->Tib.StackBase) break;
        if (i >= skip)
        {
            if (mismatch && (entries >= compare_count || buffer[entries] != (void *)context->Rip)) *mismatch = TRUE;
            buffer[entries++] = (void *)context->Rip;
        }
    }
    return entries;
}

ULONG ps5_unwind_walk_frames( CONTEXT *context, void **buffer, ULONG count, ULONG flags )
{
    struct ps5_unwind_thread *cache;
    CONTEXT original;
    ULONG entries, reference;
    BOOL verify = FALSE, mismatch = FALSE;
    if (!count) return 0;
    cache = ps5_unwind_thread_cache();
    if (!cache || cache->busy)
    {
        if (ps5_unwind_profile) __atomic_add_fetch( &ps5_fast_fallbacks[PS5_FAST_STORAGE], 1, __ATOMIC_RELAXED );
        return ps5_unwind_walk_context( context, buffer, count, flags, NULL, 0, NULL );
    }
    cache->busy = TRUE;  /* reentrant captures use the full interpreter */
    verify = __atomic_load_n( &ps5_unwind_verify, __ATOMIC_RELAXED ) && cache->verify_next;
    cache->verify_next = FALSE;
    if (verify) original = *context;
    entries = ps5_unwind_walk_context( context, buffer, count, flags, cache, 0, NULL );
    if (verify)
    {
        /* Replay the same captured machine state, not a new helper frame.
         * Overwrite with reference results so even the mismatch call is safe. */
        reference = ps5_unwind_walk_context( &original, buffer, count, flags, NULL, entries, &mismatch );
        if (reference != entries) mismatch = TRUE;
        if (mismatch && !__atomic_exchange_n( &ps5_unwind_failed, 1, __ATOMIC_RELAXED ))
            MESSAGE( "wine: title unwind verify mismatch tid=%04lx fast_count=%lu slow_count=%lu cache=disabled\n",
                     GetCurrentThreadId(), entries, reference );
        entries = reference;
    }
    cache->busy = FALSE;
    return entries;
}
#endif


/**********************************************************************
 *              RtlVirtualUnwind2   (NTDLL.@)
 */
NTSTATUS WINAPI RtlVirtualUnwind2( ULONG type, ULONG_PTR base, ULONG_PTR pc,
                                   RUNTIME_FUNCTION *function, CONTEXT *context,
                                   BOOLEAN *mach_frame_unwound, void **data,
                                   ULONG_PTR *frame_ret, KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr,
                                   ULONG_PTR *limit_low, ULONG_PTR *limit_high,
                                   PEXCEPTION_ROUTINE *handler_ret, ULONG flags )
{
    union handler_data *handler_data;
    ULONG64 frame, off;
    struct UNWIND_INFO *info;
    unsigned int i, prolog_offset;
    BOOL mach_frame = FALSE, chained = FALSE;
    ps5_unwind_event( PS5_UNWIND_VIRTUAL2 );
#if defined(__x86_64__) && !defined(__arm64ec__)
    if (ps5_unwind_profile)
    {
        ULONG_PTR caller = (ULONG_PTR)__builtin_return_address(0);
        /* Millions of walk-internal calls need only this range check, not a
         * histogram helper call or atomic updates. */
        if (caller < ps5_unwind_ntdll_base || caller >= ps5_unwind_ntdll_end)
            ps5_unwind_record_caller( PS5_UNWIND_VIRTUAL2, (void *)caller, 0, 0 );
    }
#endif

#ifdef __arm64ec__
    if (RtlIsEcCode( pc ))
    {
        DWORD flags = context->ContextFlags & ~CONTEXT_UNWOUND_TO_CALL;
        ARM64_NT_CONTEXT arm_context;
        NTSTATUS status;

        context_x64_to_arm( &arm_context, (ARM64EC_NT_CONTEXT *)context );
        status = RtlVirtualUnwind2_arm64( type, base, pc, (ARM64_RUNTIME_FUNCTION *)function,
                                          &arm_context, NULL, data, frame_ret, NULL,
                                          limit_low, limit_high, handler_ret, flags );
        context_arm_to_x64( (ARM64EC_NT_CONTEXT *)context, &arm_context );
        context->ContextFlags = flags | (arm_context.ContextFlags & CONTEXT_UNWOUND_TO_CALL);
        return status;
    }
#endif

    TRACE( "type %lx base %I64x rip %I64x rva %I64x rsp %I64x\n", type, base, pc, pc - base, context->Rsp );
    if (limit_low || limit_high) FIXME( "limits not supported\n" );

    frame = *frame_ret = context->Rsp;

    if (!function)  /* leaf function */
    {
        context->Rip = *(ULONG64 *)context->Rsp;
        context->Rsp += sizeof(ULONG64);
        if (type) *data = NULL;
        if (handler_ret) *handler_ret = NULL;
        return STATUS_SUCCESS;
    }

    if (TRACE_ON(unwind)) dump_unwind_info( base, function );

    for (;;)
    {
        info = (struct UNWIND_INFO *)((char *)base + function->UnwindData);
        handler_data = (union handler_data *)&info->opcodes[(info->count + 1) & ~1];

        if (info->version != 1 && info->version != 2)
        {
            FIXME( "unknown unwind info version %u at %p\n", info->version, info );
            return STATUS_BAD_FUNCTION_TABLE;
        }

        if (info->frame_reg)
            frame = get_int_reg( context, info->frame_reg ) - info->frame_offset * 16;

        /* check if in prolog */
        if (pc >= base + function->BeginAddress && pc < base + function->BeginAddress + info->prolog)
        {
            TRACE("inside prolog.\n");
            prolog_offset = pc - base - function->BeginAddress;
        }
        else
        {
            prolog_offset = ~0;
            /* Since Win10 1809 epilogue does not have a special treatment in case of zero opcode count. */
            if (!chained && info->count && is_inside_epilog( (BYTE *)pc, base, function ))
            {
                TRACE("inside epilog.\n");
                interpret_epilog( (BYTE *)pc, context, ctx_ptr );
                *frame_ret = info->frame_reg ? context->Rsp - 8 : frame;
                if (handler_ret) *handler_ret = NULL;
                return STATUS_SUCCESS;
            }
        }

        for (i = 0; i < info->count; i += get_opcode_size(info->opcodes[i]))
        {
            if (prolog_offset < info->opcodes[i].offset) continue; /* skip it */

            switch (info->opcodes[i].code)
            {
            case UWOP_PUSH_NONVOL:  /* pushq %reg */
                set_int_reg( context, ctx_ptr, info->opcodes[i].info, (ULONG64 *)context->Rsp );
                context->Rsp += sizeof(ULONG64);
                break;
            case UWOP_ALLOC_LARGE:  /* subq $nn,%rsp */
                if (info->opcodes[i].info) context->Rsp += *(DWORD *)&info->opcodes[i+1];
                else context->Rsp += *(USHORT *)&info->opcodes[i+1] * 8;
                break;
            case UWOP_ALLOC_SMALL:  /* subq $n,%rsp */
                context->Rsp += (info->opcodes[i].info + 1) * 8;
                break;
            case UWOP_SET_FPREG:  /* leaq nn(%rsp),%framereg */
                context->Rsp = *frame_ret = frame;
                break;
            case UWOP_SAVE_NONVOL:  /* movq %reg,n(%rsp) */
                off = frame + *(USHORT *)&info->opcodes[i+1] * 8;
                set_int_reg( context, ctx_ptr, info->opcodes[i].info, (ULONG64 *)off );
                break;
            case UWOP_SAVE_NONVOL_FAR:  /* movq %reg,nn(%rsp) */
                off = frame + *(DWORD *)&info->opcodes[i+1];
                set_int_reg( context, ctx_ptr, info->opcodes[i].info, (ULONG64 *)off );
                break;
            case UWOP_SAVE_XMM128:  /* movaps %xmmreg,n(%rsp) */
                off = frame + *(USHORT *)&info->opcodes[i+1] * 16;
                set_float_reg( context, ctx_ptr, info->opcodes[i].info, (M128A *)off );
                break;
            case UWOP_SAVE_XMM128_FAR:  /* movaps %xmmreg,nn(%rsp) */
                off = frame + *(DWORD *)&info->opcodes[i+1];
                set_float_reg( context, ctx_ptr, info->opcodes[i].info, (M128A *)off );
                break;
            case UWOP_PUSH_MACHFRAME:
                if (info->flags & UNW_FLAG_CHAININFO)
                {
                    FIXME("PUSH_MACHFRAME with chained unwind info.\n");
                    break;
                }
                if (i + get_opcode_size(info->opcodes[i]) < info->count )
                {
                    FIXME("PUSH_MACHFRAME is not the last opcode.\n");
                    break;
                }

                if (info->opcodes[i].info)
                    context->Rsp += 0x8;

                context->Rip = *(ULONG64 *)context->Rsp;
                context->Rsp = *(ULONG64 *)(context->Rsp + 24);
                mach_frame = TRUE;
                break;
            case UWOP_EPILOG:
                if (info->version == 2)
                    break; /* nothing to do */
            default:
                FIXME( "unknown code %u\n", info->opcodes[i].code );
                break;
            }
        }

        if (!(info->flags & UNW_FLAG_CHAININFO)) break;
        function = &handler_data->chain;  /* restart with the chained info */
        chained = TRUE;
    }

    if (!mach_frame)
    {
        /* now pop return address */
        context->Rip = *(ULONG64 *)context->Rsp;
        context->Rsp += sizeof(ULONG64);
    }

    if (handler_ret) *handler_ret = NULL;

    if (!(info->flags & type)) return STATUS_SUCCESS;  /* no matching handler */
    if (prolog_offset != ~0) return STATUS_SUCCESS;  /* inside prolog */

    if (handler_ret) *handler_ret = (PEXCEPTION_ROUTINE)((char *)base + handler_data->handler);
    *data = &handler_data->handler + 1;
    return STATUS_SUCCESS;
}


/**********************************************************************
 *              RtlVirtualUnwind   (NTDLL.@)
 */
PEXCEPTION_ROUTINE WINAPI RtlVirtualUnwind( ULONG type, ULONG64 base, ULONG64 pc,
                                            RUNTIME_FUNCTION *func, CONTEXT *context,
                                            PVOID *handler_data, ULONG64 *frame_ret,
                                            KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr )
{
    PEXCEPTION_ROUTINE handler;
    ps5_unwind_event( PS5_UNWIND_VIRTUAL );

    if (!RtlVirtualUnwind2( type, base, pc, func, context, NULL, handler_data,
                           frame_ret, ctx_ptr, NULL, NULL, &handler, 0 ))
        return handler;

    context->Rip = 0;
    return NULL;
}


/*******************************************************************
 *		__C_specific_handler (NTDLL.@)
 */
EXCEPTION_DISPOSITION WINAPI __C_specific_handler( EXCEPTION_RECORD *rec, void *frame,
                                                   CONTEXT *context, DISPATCHER_CONTEXT *dispatch )
{
    const SCOPE_TABLE *table = dispatch->HandlerData;
    ULONG_PTR base = dispatch->ImageBase;
    ULONG_PTR pc = dispatch->ControlPc;
    unsigned int i;

#ifdef __arm64ec__
    if (RtlIsEcCode( pc ))
        return __C_specific_handler_arm64( rec, frame, (ARM64_NT_CONTEXT *)context,
                                           (DISPATCHER_CONTEXT_ARM64 *)dispatch );
#endif

    TRACE( "%p %p %p %p pc %Ix\n", rec, frame, context, dispatch, pc );
    if (TRACE_ON(unwind)) DUMP_SCOPE_TABLE( base, table );

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    {
        for (i = dispatch->ScopeIndex; i < table->Count; i++)
        {
            if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
            if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
            if (table->ScopeRecord[i].JumpTarget) continue;

            if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND &&
                dispatch->TargetIp >= base + table->ScopeRecord[i].BeginAddress &&
                dispatch->TargetIp < base + table->ScopeRecord[i].EndAddress)
            {
                break;
            }
            else
            {
                PTERMINATION_HANDLER handler = (void *)(base + table->ScopeRecord[i].HandlerAddress);
                dispatch->ScopeIndex = i + 1;
                TRACE( "scope %u calling __finally %p frame %p\n", i, handler, frame );
                handler( TRUE, frame );
            }
        }
    }
    else
    {
        for (i = dispatch->ScopeIndex; i < table->Count; i++)
        {
            if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
            if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
            if (!table->ScopeRecord[i].JumpTarget) continue;

            if (table->ScopeRecord[i].HandlerAddress != EXCEPTION_EXECUTE_HANDLER)
            {
                EXCEPTION_POINTERS ptrs = { rec, context };
                PEXCEPTION_FILTER filter = (void *)(base + table->ScopeRecord[i].HandlerAddress);

                TRACE( "scope %u calling filter %p ptrs %p frame %p\n", i, filter, &ptrs, frame );
                switch (filter( &ptrs, frame ))
                {
                case EXCEPTION_EXECUTE_HANDLER:
                    break;
                case EXCEPTION_CONTINUE_SEARCH:
                    continue;
                case EXCEPTION_CONTINUE_EXECUTION:
                    return ExceptionContinueExecution;
                }
            }
            TRACE( "unwinding to target %Ix\n", base + table->ScopeRecord[i].JumpTarget );
            RtlUnwindEx( frame, (char *)base + table->ScopeRecord[i].JumpTarget,
                         rec, ULongToPtr(rec->ExceptionCode), dispatch->ContextRecord,
                         dispatch->HistoryTable );
        }
    }
    return ExceptionContinueSearch;
}


/**********************************************************************
 *              RtlLookupFunctionEntry   (NTDLL.@)
 */
PRUNTIME_FUNCTION WINAPI RtlLookupFunctionEntry( ULONG_PTR pc, ULONG_PTR *base,
                                                 UNWIND_HISTORY_TABLE *table )
{
    RUNTIME_FUNCTION *func;
    ULONG_PTR dynbase;
    ULONG size;
    ps5_unwind_event( PS5_UNWIND_LOOKUP );

#ifdef __arm64ec__
    if (RtlIsEcCode( pc ))
        return (RUNTIME_FUNCTION *)RtlLookupFunctionEntry_arm64( pc, base, table );
#endif

#if defined(__x86_64__) && !defined(__arm64ec__)
    if (ps5_unwind_title) func = ps5_lookup_function_table( pc, base, &size );
    else
#endif
        func = RtlLookupFunctionTable( pc, base, &size );
    if (func) return find_function_info( pc, *base, func, size / sizeof(*func), TRUE );

    if ((func = lookup_dynamic_function_table( pc, &dynbase, &size )))
    {
        RUNTIME_FUNCTION *ret = find_function_info( pc, dynbase, func, size, TRUE );
        if (ret) *base = dynbase;
        return ret;
    }

    *base = 0;
    return NULL;
}


/**********************************************************************
 *              RtlAddFunctionTable   (NTDLL.@)
 */
BOOLEAN CDECL RtlAddFunctionTable( RUNTIME_FUNCTION *table, DWORD count, ULONG_PTR base )
{
    ULONG_PTR end = base;
    void *ret;

    if (count) end += table[count - 1].EndAddress;

    return !RtlAddGrowableFunctionTable( &ret, table, count, 0, base, end );
}

#endif  /* __x86_64__ */


/***********************************************************************
 * Generic unwind functions
 */


/***********************************************************************
 *            RtlUnwind  (NTDLL.@)
 */
void WINAPI RtlUnwind( void *frame, void *target_ip, EXCEPTION_RECORD *rec, void *retval )
{
    CONTEXT context;
    RtlUnwindEx( frame, target_ip, rec, retval, &context, NULL );
}

__ASM_GLOBAL_IMPORT(RtlUnwind)


/*******************************************************************
 *		_local_unwind (NTDLL.@)
 */
void WINAPI _local_unwind( void *frame, void *target_ip )
{
    RtlUnwind( frame, target_ip, NULL, NULL );
}

#else

void register_module_exception_directory( void *module )
{
}

void unregister_module_exception_directory( void *module )
{
}

#endif  /* !__i386__ */
