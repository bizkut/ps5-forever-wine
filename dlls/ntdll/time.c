/*
 * Nt time functions.
 *
 * RtlTimeToTimeFields, RtlTimeFieldsToTime and defines are taken from ReactOS and
 * adapted to wine with special permissions of the author. This code is
 * Copyright 2002 Rex Jolliff (rex@lvcablemodem.com)
 *
 * Copyright 1999 Juergen Schmied
 * Copyright 2007 Dmitry Timoshkov
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

#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/exception.h"
#include "wine/debug.h"
#include "ntdll_misc.h"

WINE_DEFAULT_DEBUG_CHANNEL(ntdll);

#define TICKSPERSEC        10000000
#define TICKSPERMSEC       10000
#define SECSPERDAY         86400
#define SECSPERHOUR        3600
#define SECSPERMIN         60
#define MINSPERHOUR        60
#define HOURSPERDAY        24
#define EPOCHWEEKDAY       1  /* Jan 1, 1601 was Monday */
#define DAYSPERWEEK        7
#define MONSPERYEAR        12
#define DAYSPERQUADRICENTENNIUM (365 * 400 + 97)
#define DAYSPERNORMALQUADRENNIUM (365 * 4 + 1)

/* 1601 to 1970 is 369 years plus 89 leap days */
#define SECS_1601_TO_1970  ((369 * 365 + 89) * (ULONGLONG)SECSPERDAY)
#define TICKS_1601_TO_1970 (SECS_1601_TO_1970 * TICKSPERSEC)
/* 1601 to 1980 is 379 years plus 91 leap days */
#define SECS_1601_TO_1980  ((379 * 365 + 91) * (ULONGLONG)SECSPERDAY)
#define TICKS_1601_TO_1980 (SECS_1601_TO_1980 * TICKSPERSEC)


static const int MonthLengths[2][MONSPERYEAR] =
{
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

static inline BOOL IsLeapYear(int Year)
{
    return Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
}


/******************************************************************************
 *       RtlTimeToTimeFields [NTDLL.@]
 *
 * Convert a time into a TIME_FIELDS structure.
 *
 * PARAMS
 *   liTime     [I] Time to convert.
 *   TimeFields [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
VOID WINAPI RtlTimeToTimeFields(
	const LARGE_INTEGER *liTime,
	PTIME_FIELDS TimeFields)
{
	int SecondsInDay;
        long int cleaps, years, yearday, months;
	long int Days;
	LONGLONG Time;

	/* Extract millisecond from time and convert time into seconds */
	TimeFields->Milliseconds =
            (CSHORT) (( liTime->QuadPart % TICKSPERSEC) / TICKSPERMSEC);
	Time = liTime->QuadPart / TICKSPERSEC;

	/* The native version of RtlTimeToTimeFields does not take leap seconds
	 * into account */

	/* Split the time into days and seconds within the day */
	Days = Time / SECSPERDAY;
	SecondsInDay = Time % SECSPERDAY;

	/* compute time of day */
	TimeFields->Hour = (CSHORT) (SecondsInDay / SECSPERHOUR);
	SecondsInDay = SecondsInDay % SECSPERHOUR;
	TimeFields->Minute = (CSHORT) (SecondsInDay / SECSPERMIN);
	TimeFields->Second = (CSHORT) (SecondsInDay % SECSPERMIN);

	/* compute day of week */
	TimeFields->Weekday = (CSHORT) ((EPOCHWEEKDAY + Days) % DAYSPERWEEK);

        /* compute year, month and day of month. */
        cleaps=( 3 * ((4 * Days + 1227) / DAYSPERQUADRICENTENNIUM) + 3 ) / 4;
        Days += 28188 + cleaps;
        years = (20 * Days - 2442) / (5 * DAYSPERNORMALQUADRENNIUM);
        yearday = Days - (years * DAYSPERNORMALQUADRENNIUM)/4;
        months = (64 * yearday) / 1959;
        /* the result is based on a year starting on March.
         * To convert take 12 from Januari and Februari and
         * increase the year by one. */
        if( months < 14 ) {
            TimeFields->Month = months - 1;
            TimeFields->Year = years + 1524;
        } else {
            TimeFields->Month = months - 13;
            TimeFields->Year = years + 1525;
        }
        /* calculation of day of month is based on the wonderful
         * sequence of INT( n * 30.6): it reproduces the 
         * 31-30-31-30-31-31 month lengths exactly for small n's */
        TimeFields->Day = yearday - (1959 * months) / 64 ;
        return;
}

/******************************************************************************
 *       RtlTimeFieldsToTime [NTDLL.@]
 *
 * Convert a TIME_FIELDS structure into a time.
 *
 * PARAMS
 *   ftTimeFields [I] TIME_FIELDS structure to convert.
 *   Time         [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE.
 */
BOOLEAN WINAPI RtlTimeFieldsToTime(
	PTIME_FIELDS tfTimeFields,
	PLARGE_INTEGER Time)
{
        int month, year, cleaps, day;

	/* FIXME: normalize the TIME_FIELDS structure here */
        /* No, native just returns 0 (error) if the fields are not */
        if( tfTimeFields->Milliseconds< 0 || tfTimeFields->Milliseconds > 999 ||
                tfTimeFields->Second < 0 || tfTimeFields->Second > 59 ||
                tfTimeFields->Minute < 0 || tfTimeFields->Minute > 59 ||
                tfTimeFields->Hour < 0 || tfTimeFields->Hour > 23 ||
                tfTimeFields->Month < 1 || tfTimeFields->Month > 12 ||
                tfTimeFields->Day < 1 ||
                tfTimeFields->Day > MonthLengths
                    [ tfTimeFields->Month ==2 || IsLeapYear(tfTimeFields->Year)]
                    [ tfTimeFields->Month - 1] ||
                tfTimeFields->Year < 1601 )
            return FALSE;

        /* now calculate a day count from the date
         * First start counting years from March. This way the leap days
         * are added at the end of the year, not somewhere in the middle.
         * Formula's become so much less complicate that way.
         * To convert: add 12 to the month numbers of Jan and Feb, and 
         * take 1 from the year */
        if(tfTimeFields->Month < 3) {
            month = tfTimeFields->Month + 13;
            year = tfTimeFields->Year - 1;
        } else {
            month = tfTimeFields->Month + 1;
            year = tfTimeFields->Year;
        }
        cleaps = (3 * (year / 100) + 3) / 4;   /* nr of "century leap years"*/
        day =  (36525 * year) / 100 - cleaps + /* year * dayperyr, corrected */
                 (1959 * month) / 64 +         /* months * daypermonth */
                 tfTimeFields->Day -          /* day of the month */
                 584817 ;                      /* zero that on 1601-01-01 */
        /* done */
        
        Time->QuadPart = (((((LONGLONG) day * HOURSPERDAY +
            tfTimeFields->Hour) * MINSPERHOUR +
            tfTimeFields->Minute) * SECSPERMIN +
            tfTimeFields->Second ) * 1000 +
            tfTimeFields->Milliseconds ) * TICKSPERMSEC;

        return TRUE;
}


/******************************************************************************
 *        RtlLocalTimeToSystemTime [NTDLL.@]
 *
 * Convert a local time into system time.
 *
 * PARAMS
 *   LocalTime  [I] Local time to convert.
 *   SystemTime [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlLocalTimeToSystemTime( const LARGE_INTEGER *LocalTime,
                                          PLARGE_INTEGER SystemTime)
{
    SYSTEM_TIMEOFDAY_INFORMATION info;

    TRACE("(%p, %p)\n", LocalTime, SystemTime);

    NtQuerySystemInformation( SystemTimeOfDayInformation, &info, sizeof(info), NULL );
    SystemTime->QuadPart = LocalTime->QuadPart + info.TimeZoneBias.QuadPart;
    return STATUS_SUCCESS;
}

/******************************************************************************
 *       RtlSystemTimeToLocalTime [NTDLL.@]
 *
 * Convert a system time into a local time.
 *
 * PARAMS
 *   SystemTime [I] System time to convert.
 *   LocalTime  [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlSystemTimeToLocalTime( const LARGE_INTEGER *SystemTime,
                                          PLARGE_INTEGER LocalTime )
{
    SYSTEM_TIMEOFDAY_INFORMATION info;

    TRACE("(%p, %p)\n", SystemTime, LocalTime);

    NtQuerySystemInformation( SystemTimeOfDayInformation, &info, sizeof(info), NULL );
    LocalTime->QuadPart = SystemTime->QuadPart - info.TimeZoneBias.QuadPart;
    return STATUS_SUCCESS;
}

/******************************************************************************
 *       RtlTimeToSecondsSince1970 [NTDLL.@]
 *
 * Convert a time into a count of seconds since 1970.
 *
 * PARAMS
 *   Time    [I] Time to convert.
 *   Seconds [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE, if the resulting value will not fit in a DWORD.
 */
BOOLEAN WINAPI RtlTimeToSecondsSince1970( const LARGE_INTEGER *Time, LPDWORD Seconds )
{
    ULONGLONG tmp = Time->QuadPart / TICKSPERSEC - SECS_1601_TO_1970;
    if (tmp > 0xffffffff) return FALSE;
    *Seconds = tmp;
    return TRUE;
}

/******************************************************************************
 *       RtlTimeToSecondsSince1980 [NTDLL.@]
 *
 * Convert a time into a count of seconds since 1980.
 *
 * PARAMS
 *   Time    [I] Time to convert.
 *   Seconds [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE, if the resulting value will not fit in a DWORD.
 */
BOOLEAN WINAPI RtlTimeToSecondsSince1980( const LARGE_INTEGER *Time, LPDWORD Seconds )
{
    ULONGLONG tmp = Time->QuadPart / TICKSPERSEC - SECS_1601_TO_1980;
    if (tmp > 0xffffffff) return FALSE;
    *Seconds = tmp;
    return TRUE;
}

/******************************************************************************
 *       RtlSecondsSince1970ToTime [NTDLL.@]
 *
 * Convert a count of seconds since 1970 to a time.
 *
 * PARAMS
 *   Seconds [I] Time to convert.
 *   Time    [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
void WINAPI RtlSecondsSince1970ToTime( DWORD Seconds, LARGE_INTEGER *Time )
{
    Time->QuadPart = Seconds * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1970;
}

/******************************************************************************
 *       RtlSecondsSince1980ToTime [NTDLL.@]
 *
 * Convert a count of seconds since 1980 to a time.
 *
 * PARAMS
 *   Seconds [I] Time to convert.
 *   Time    [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
void WINAPI RtlSecondsSince1980ToTime( DWORD Seconds, LARGE_INTEGER *Time )
{
    Time->QuadPart = Seconds * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1980;
}

/******************************************************************************
 *       RtlTimeToElapsedTimeFields [NTDLL.@]
 *
 * Convert a time to a count of elapsed seconds.
 *
 * PARAMS
 *   Time       [I] Time to convert.
 *   TimeFields [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
void WINAPI RtlTimeToElapsedTimeFields( const LARGE_INTEGER *Time, PTIME_FIELDS TimeFields )
{
    LONGLONG time;
    INT rem;

    time = Time->QuadPart / TICKSPERSEC;
    TimeFields->Milliseconds = (Time->QuadPart % TICKSPERSEC) / TICKSPERMSEC;

    /* time is now in seconds */
    TimeFields->Year  = 0;
    TimeFields->Month = 0;
    TimeFields->Day   = time / SECSPERDAY;

    /* rem is now the remaining seconds in the last day */
    rem = time % SECSPERDAY;
    TimeFields->Second = rem % 60;
    rem /= 60;
    TimeFields->Minute = rem % 60;
    TimeFields->Hour = rem / 60;
}

/***********************************************************************
 *       RtlGetSystemTimePrecise [NTDLL.@]
 *
 * Get a more accurate current system time.
 *
 * RETURNS
 *   The current system time.
 */
LONGLONG WINAPI RtlGetSystemTimePrecise( void )
{
    LONGLONG ret;

    WINE_UNIX_CALL( unix_system_time_precise, &ret );
    return ret;
}

#if defined(__x86_64__) && !defined(__arm64ec__) && defined(__WINE_PS5__)
/* PS5 title QPC from the TSC, switched by `qpc=tsc|syscall` in the tune file
 * (default syscall). One Unix call at init returns the calibrated conversion;
 * after that the TSC path makes no syscall. KUSER_SHARED_DATA's QpcBypass
 * fields are not used: they define QPC as (rdtsc + QpcBias) >> QpcShift at
 * QpcFrequency, which would change QPC's units away from TICKSPERSEC, and that
 * page is server-owned and shared with every process. */
static struct wine_ps5_qpc_state ps5_qpc;
static ULONG64 ps5_qpc_tsc_hz, ps5_qpc_calls;
static ULONG ps5_qpc_report;
static LONG ps5_qpc_requested = WINE_PS5_QPC_SYSCALL;

static inline ULONG64 ps5_qpc_rdtscp(void)
{
    ULONG low, high, aux;

    __asm__ __volatile__( "rdtscp" : "=a"(low), "=d"(high), "=c"(aux) );
    return ((ULONG64)high << 32) | low;
}

/* Minimum-latency TSC/monotonic_counter()/TSC pair of four. */
static void ps5_qpc_read_pair( ULONG64 *before, ULONG64 *mono, ULONG64 *after )
{
    LARGE_INTEGER now;
    ULONG64 first, last;
    unsigned int i;

    for (i = 0; i < 4; i++)
    {
        first = ps5_qpc_rdtscp();
        NtQueryPerformanceCounter( &now, NULL );
        last = ps5_qpc_rdtscp();
        if (!i || last - first < *after - *before)
        {
            *before = first;
            *mono = now.QuadPart;
            *after = last;
        }
    }
}

static const char *ps5_qpc_mode_name( int mode )
{
    return mode == WINE_PS5_QPC_TSC ? "tsc" : "syscall";
}

void ps5_qpc_init(void)
{
    struct ps5_qpc_calibrate_params params;

    if (!WINE_UNIX_CALL( unix_ps5_qpc_calibrate, &params ))
    {
        ps5_qpc_tsc_hz = params.tsc_hz;
        ps5_qpc.mul = params.mul;
        ps5_qpc.calibration_bias = ps5_qpc.tsc_bias = params.bias;
    }
    ps5_qpc_report = NtGetTickCount();
    __atomic_store_n( &ps5_qpc.mode, WINE_PS5_QPC_SYSCALL, __ATOMIC_RELEASE );
    MESSAGE( "wine: title qpc mode=syscall source=default freq_hz=%I64u\n", ps5_qpc_tsc_hz );
}

/* Live switch from the tune poll; see wine_ps5_qpc_select() for ordering. */
void ps5_qpc_tune( BOOL tsc )
{
    LONG requested = tsc ? WINE_PS5_QPC_TSC : WINE_PS5_QPC_SYSCALL;
    ULONG64 before = 0, mono, after = 0;
    LARGE_INTEGER now;
    int mode;

    if (__atomic_load_n( &ps5_qpc.mode, __ATOMIC_RELAXED ) == WINE_PS5_QPC_OFF ||
        __atomic_exchange_n( &ps5_qpc_requested, requested, __ATOMIC_RELAXED ) == requested) return;
    if (ps5_qpc.mul) ps5_qpc_read_pair( &before, &mono, &after );
    else
    {
        NtQueryPerformanceCounter( &now, NULL );
        mono = now.QuadPart;
    }
    mode = wine_ps5_qpc_select( &ps5_qpc, requested, before, mono, after );
    MESSAGE( "wine: title qpc mode=%s source=C:\\wineps5-tune.txt freq_hz=%I64u%s\n",
             ps5_qpc_mode_name( mode ), ps5_qpc_tsc_hz, mode != requested ? " refused=no_tsc" : "" );
}

static const char *ps5_qpc_us( char *buffer, size_t size, LONG64 ticks )
{
    ULONG64 value = ticks < 0 ? -(ULONG64)ticks : (ULONG64)ticks;

    snprintf( buffer, size, "%s%I64u.%u", ticks < 0 ? "-" : "", value / 10, (unsigned int)(value % 10) );
    return buffer;
}

/* WINEPS5_PROFILE=1: call volume and TSC-vs-CLOCK_MONOTONIC drift per 10 s. */
static void DECLSPEC_NOINLINE ps5_qpc_count(void)
{
    ULONG now = NtGetTickCount(), last = __atomic_load_n( &ps5_qpc_report, __ATOMIC_RELAXED );
    ULONG64 calls, before, mono, after;
    char drift[32], offset[32];

    __atomic_add_fetch( &ps5_qpc_calls, 1, __ATOMIC_RELAXED );
    if ((ULONG)(now - last) < 10000 ||
        !__atomic_compare_exchange_n( &ps5_qpc_report, &last, now, FALSE,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED )) return;
    calls = __atomic_exchange_n( &ps5_qpc_calls, 0, __ATOMIC_RELAXED );
    if (!ps5_qpc.mul)
    {
        MESSAGE( "wine: title qpc window mode=syscall calls=%I64u drift_us=unavailable window_ms=%lu\n",
                 calls, now - last );
        return;
    }
    ps5_qpc_read_pair( &before, &mono, &after );
    MESSAGE( "wine: title qpc window mode=%s calls=%I64u drift_us=%s offset_us=%s pair_ns=%I64u window_ms=%lu\n",
             ps5_qpc_mode_name( __atomic_load_n( &ps5_qpc.mode, __ATOMIC_RELAXED ) ), calls,
             ps5_qpc_us( drift, sizeof(drift), wine_ps5_qpc_drift( &ps5_qpc, before, mono, after ) ),
             ps5_qpc_us( offset, sizeof(offset), wine_ps5_qpc_offset( &ps5_qpc, before, mono, after ) ),
             (after - before) * 1000000000 / ps5_qpc_tsc_hz, now - last );
}
#endif

/******************************************************************************
 *  RtlQueryPerformanceCounter   [NTDLL.@]
 */
BOOL WINAPI DECLSPEC_HOTPATCH RtlQueryPerformanceCounter( LARGE_INTEGER *counter )
{
#if defined(__x86_64__) && !defined(__arm64ec__) && defined(__WINE_PS5__)
    int mode = __atomic_load_n( &ps5_qpc.mode, __ATOMIC_ACQUIRE );

    if (mode != WINE_PS5_QPC_OFF)
    {
        if (ps5_unwind_profile) ps5_qpc_count();
        if (mode == WINE_PS5_QPC_TSC)
        {
            counter->QuadPart = wine_ps5_qpc_from_tsc( &ps5_qpc, ps5_qpc_rdtscp() );
            return TRUE;
        }
        NtQueryPerformanceCounter( counter, NULL );
        counter->QuadPart = wine_ps5_qpc_from_syscall( &ps5_qpc, counter->QuadPart );
        return TRUE;
    }
#endif
    NtQueryPerformanceCounter( counter, NULL );
    return TRUE;
}

/******************************************************************************
 *  RtlQueryPerformanceFrequency   [NTDLL.@]
 */
BOOL WINAPI DECLSPEC_HOTPATCH RtlQueryPerformanceFrequency( LARGE_INTEGER *frequency )
{
    frequency->QuadPart = TICKSPERSEC;
    return TRUE;
}

/******************************************************************************
 * NtGetTickCount   (NTDLL.@)
 * ZwGetTickCount   (NTDLL.@)
 */
ULONG WINAPI DECLSPEC_HOTPATCH NtGetTickCount(void)
{
    /* note: we ignore TickCountMultiplier */
    return user_shared_data->TickCount.LowPart;
}

/***********************************************************************
 *      RtlQueryTimeZoneInformation [NTDLL.@]
 *
 * Get information about the current timezone.
 *
 * PARAMS
 *   tzinfo [O] Destination for the retrieved timezone info.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlQueryTimeZoneInformation(RTL_TIME_ZONE_INFORMATION *ret)
{
    return NtQuerySystemInformation( SystemCurrentTimeZoneInformation, ret, sizeof(*ret), NULL );
}

/***********************************************************************
 *      RtlQueryDynamicTimeZoneInformation [NTDLL.@]
 *
 * Get information about the current timezone.
 *
 * PARAMS
 *   tzinfo [O] Destination for the retrieved timezone info.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlQueryDynamicTimeZoneInformation(RTL_DYNAMIC_TIME_ZONE_INFORMATION *ret)
{
    return NtQuerySystemInformation( SystemDynamicTimeZoneInformation, ret, sizeof(*ret), NULL );
}

/***********************************************************************
 *       RtlSetTimeZoneInformation [NTDLL.@]
 *
 * Set the current time zone information.
 *
 * PARAMS
 *   tzinfo [I] Timezone information to set.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 *
 */
NTSTATUS WINAPI RtlSetTimeZoneInformation( const RTL_TIME_ZONE_INFORMATION *tzinfo )
{
    return STATUS_PRIVILEGE_NOT_HELD;
}

/***********************************************************************
 *        RtlQueryUnbiasedInterruptTime [NTDLL.@]
 */
BOOL WINAPI RtlQueryUnbiasedInterruptTime(ULONGLONG *time)
{
    ULONG high, low;

    if (!time)
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus( STATUS_INVALID_PARAMETER );
        return FALSE;
    }

    do
    {
        high = user_shared_data->InterruptTime.High1Time;
        low = user_shared_data->InterruptTime.LowPart;
    }
    while (high != user_shared_data->InterruptTime.High2Time);
    /* FIXME: should probably subtract InterruptTimeBias */
    *time = (ULONGLONG)high << 32 | low;
    return TRUE;
}
