/* Native title controller bridge, as WoWPS5 does.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __WINE_XINPUT_UNIXLIB_H
#define __WINE_XINPUT_UNIXLIB_H
#include "wine/unixlib.h"
enum ps5_xinput_call { ps5_xinput_probe, ps5_xinput_state, ps5_xinput_vibrate, ps5_xinput_enable };
struct ps5_xinput_state_params { UINT index; BOOL poll; XINPUT_STATE state; DWORD result; };
struct ps5_xinput_vibrate_params { UINT index, left, right; DWORD result; };
#if defined(_WIN64) && defined(XINPUT_VER)
#define PS5_XINPUT_CALL WINE_UNIX_CALL
#else
static inline NTSTATUS ps5_xinput_unavailable(unsigned call, void *args)
{
    (void)call;
    (void)args;
    return (NTSTATUS)0xc00000bb;
}
#define PS5_XINPUT_CALL ps5_xinput_unavailable
#endif
#endif
