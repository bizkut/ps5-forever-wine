/* Native GPU-title input, drained only by Wine message-pump threads.
 * The callback is resolved beside Vulkan callbacks, not a module import. */
#ifndef __WINE_PS5_TITLE_INPUT_H
#define __WINE_PS5_TITLE_INPUT_H
#include "wine/ps5_desktop.h"

#define PS5_TITLE_INPUT_RESET 0x10000u
#define PS5_TITLE_MOUSE_RELATIVE 0x80000000u
union ps5_title_input
{
    struct ps5_desktop_header header;
    struct ps5_desktop_mouse mouse;
    struct ps5_desktop_key key;
    unsigned char bytes[PS5_DESKTOP_VOICE_MAX_MESSAGE];
};
/* Returns one framed protocol event, 0 when empty. Cursor coordinates are
 * desktop pixels; the producer uses them to rebase the pad's absolute pointer.
 * RESET releases previously delivered keys/buttons after queue overflow. */
typedef int (*wine_ps5_read_input)(union ps5_title_input *, unsigned, int, int, int, int);
#endif
