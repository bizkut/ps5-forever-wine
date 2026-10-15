/* GPU-title XInput callbacks, resolved beside Vulkan callbacks, as WoWPS5 does.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __WINE_PS5_TITLE_GAMEPAD_H
#define __WINE_PS5_TITLE_GAMEPAD_H
#include <stdint.h>
struct wine_ps5_gamepad
{
    uint16_t buttons;
    uint8_t left_trigger, right_trigger;
    int16_t lx, ly, rx, ry;
};
struct wine_ps5_gamepad_state
{
    uint32_t packet;
    struct wine_ps5_gamepad pad;
};
/* Win32 error result; index zero is the initial user's pad. Poll is nonzero
 * only for GetState: capabilities queries must not activate gamepad mode. */
typedef unsigned (*wine_ps5_get_gamepad)(unsigned index, struct wine_ps5_gamepad_state *, int poll);
typedef unsigned (*wine_ps5_set_gamepad_vibration)(unsigned index, unsigned left, unsigned right);
typedef void (*wine_ps5_enable_gamepad)(int enabled);
#endif
