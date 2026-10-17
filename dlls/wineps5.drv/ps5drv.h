/*
 * PS5 user driver
 *
 * Copyright 2026
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

#ifndef __WINE_PS5DRV_H
#define __WINE_PS5DRV_H

#include <pthread.h>
#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "ntuser.h"
#include "wine/debug.h"
#include "wine/gdi_driver.h"
#include "wine/ps5_desktop.h"
#include "unixlib.h"

#ifdef __PROSPERO__
extern BOOL ps5_title_desktop;
#endif

extern BOOL ps5_send_message( const void *message, unsigned int size );
extern BOOL ps5_send_cursor_message( const void *message, unsigned int size );
extern void PS5_SetCursor( HWND hwnd, HCURSOR cursor );
extern void PS5_DestroyCursorIcon( HCURSOR cursor );
extern BOOL PS5_CreateWindow( HWND hwnd );
extern void PS5_SetDesktopWindow( HWND hwnd );
extern LRESULT PS5_DesktopWindowProc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam );
extern void PS5_DestroyWindow( HWND hwnd );
extern BOOL PS5_WindowPosChanging( HWND hwnd, UINT flags, BOOL shaped, const struct window_rects *rects );
extern BOOL PS5_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *rect, struct window_surface **surface );
extern struct window_surface *PS5_GetForeignWindowSurface( HWND hwnd );
extern void PS5_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner, UINT flags,
                                  const struct window_rects *rects, struct window_surface *surface );
extern UINT PS5_ShowWindow( HWND hwnd, INT cmd, RECT *rect, UINT flags );
extern void PS5_SetLayeredWindowAttributes( HWND hwnd, COLORREF key, BYTE alpha, DWORD flags );
extern void PS5_UpdateLayeredWindow( HWND hwnd, BYTE alpha, UINT flags );
extern void PS5_SetWindowStyle( HWND hwnd, INT offset, STYLESTRUCT *style );
extern void PS5_SetParent( HWND hwnd, HWND parent, HWND old_parent );

#endif /* __WINE_PS5DRV_H */
