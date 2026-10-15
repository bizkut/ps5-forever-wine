/* Native title XInput bridge, as WoWPS5 does; payload Wine keeps HID.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#if 0
#pragma makedep unix
#endif
#include "config.h"
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "xinput.h"
#include "unixlib.h"
#include "wine/ps5_title_gamepad.h"

#ifdef __PROSPERO__
extern int __wine_ps5_is_title(void);
extern void *__wine_ps5_get_vulkan_proc(const char *name);
/* The SDK linker does not page-align a BSS-only writable LOAD. Keep these
 * mutable callback slots on a fresh native page, separate from RELRO. */
_Alignas(0x4000) static wine_ps5_get_gamepad get_gamepad;
static wine_ps5_set_gamepad_vibration set_vibration;
static wine_ps5_enable_gamepad enable_gamepad;
#endif

static NTSTATUS probe(void *args)
{
    (void)args;
#ifdef __PROSPERO__
    if (__wine_ps5_is_title())
    {
        get_gamepad = __wine_ps5_get_vulkan_proc("wineGetGamepad");
        set_vibration = __wine_ps5_get_vulkan_proc("wineSetGamepadVibration");
        enable_gamepad = __wine_ps5_get_vulkan_proc("wineEnableGamepad");
        if (get_gamepad && set_vibration && enable_gamepad) return STATUS_SUCCESS;
    }
#endif
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS read_state(void *args)
{
#ifdef __PROSPERO__
    struct ps5_xinput_state_params *p = args;
    struct wine_ps5_gamepad_state state;
    p->result = get_gamepad(p->index, &state, p->poll);
    if (!p->result)
    {
        p->state.dwPacketNumber = state.packet;
        p->state.Gamepad.wButtons = state.pad.buttons;
        p->state.Gamepad.bLeftTrigger = state.pad.left_trigger;
        p->state.Gamepad.bRightTrigger = state.pad.right_trigger;
        p->state.Gamepad.sThumbLX = state.pad.lx;
        p->state.Gamepad.sThumbLY = state.pad.ly;
        p->state.Gamepad.sThumbRX = state.pad.rx;
        p->state.Gamepad.sThumbRY = state.pad.ry;
    }
    return STATUS_SUCCESS;
#else
    (void)args;
    return STATUS_NOT_SUPPORTED;
#endif
}

static NTSTATUS vibrate(void *args)
{
#ifdef __PROSPERO__
    struct ps5_xinput_vibrate_params *p = args;
    p->result = set_vibration(p->index, p->left, p->right);
    return STATUS_SUCCESS;
#else
    (void)args;
    return STATUS_NOT_SUPPORTED;
#endif
}

static NTSTATUS enable(void *args)
{
#ifdef __PROSPERO__
    enable_gamepad(*(BOOL *)args);
    return STATUS_SUCCESS;
#else
    (void)args;
    return STATUS_NOT_SUPPORTED;
#endif
}

const unixlib_entry_t __wine_unix_call_funcs[] = {probe, read_state, vibrate, enable};
