/*
 * PS5 title WoW64: 64-bit TEB fields shared with 32-bit PE code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * While 32-bit code runs in a PS5 title, libkernel's thread pointer occupies
 * WOW TEB bytes 0x10-0x17 (NT_TIB FiberData and ArbitraryUserPointer): its
 * signal wrapper reads %fs:0x10, and %fs is the WOW TEB. The title ntdll
 * keeps the genuine 32-bit FiberData in the 64-bit TEB and puts it back in
 * the WOW TEB whenever the thread runs 64-bit code
 * (dlls/ntdll/unix/signal_x86_64.c, struct amd64_thread_data).
 */

#ifndef __WINE_WINE_PS5_WOW_H
#define __WINE_WINE_PS5_WOW_H

/* Offsets in the 64-bit TEB. */
#define WINE_PS5_WOW_FIBER_DATA_OFFSET 0x348 /* DWORD: genuine WOW FiberData */
#define WINE_PS5_WOW_SHADOWED_OFFSET   0x360 /* BOOL: libkernel owns WOW TEB 0x10-0x17 */

#endif /* __WINE_WINE_PS5_WOW_H */
