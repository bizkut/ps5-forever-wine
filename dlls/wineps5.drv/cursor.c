/* Windows cursor images for the PS5 desktop transport.
 *
 * Copyright 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#if 0
#pragma makedep unix
#endif

#include "ps5drv.h"
WINE_DEFAULT_DEBUG_CHANNEL(ps5drv);
#include "wine/ps5_cursor.h"

static struct wine_ps5_cursor_cache cursor_cache = WINE_PS5_CURSOR_CACHE_INIT;

void PS5_DestroyCursorIcon( HCURSOR cursor )
{
    wine_ps5_cursor_destroy( &cursor_cache, cursor );
}

void PS5_SetCursor( HWND hwnd, HCURSOR cursor )
{
    HWND root = hwnd ? NtUserGetAncestor( hwnd, GA_ROOT ) : 0;
    if (root && root == NtUserGetDesktopWindow()) root = 0;
    wine_ps5_cursor_publish( &cursor_cache, (UINT_PTR)root, cursor, ps5_send_cursor_message );
}
