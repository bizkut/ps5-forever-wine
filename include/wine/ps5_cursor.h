/* PS5 cursor bitmap conversion and handle cache, shared by win32u's native
 * GPU title driver and wineps5.drv's desktop transport.
 *
 * Copyright 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#ifndef __WINE_PS5_CURSOR_H
#define __WINE_PS5_CURSOR_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "wine/ps5_desktop.h"

/* The caller supplies Wine's GDI/user declarations and default debug channel. */
typedef int (*wine_ps5_cursor_send)( const void *, unsigned int );
struct wine_ps5_cursor_entry
{
    struct wine_ps5_cursor_entry *next;
    HCURSOR cursor;
    uint32_t width, height, visible_pixels, reserved; /* keeps bytes[] 8-byte aligned */
    unsigned char bytes[];
};
struct wine_ps5_cursor_cache
{
    pthread_mutex_t mutex;
    struct wine_ps5_cursor_entry *entries;
};
#define WINE_PS5_CURSOR_CACHE_INIT { PTHREAD_MUTEX_INITIALIZER, NULL }

static inline BOOL wine_ps5_cursor_read_bitmap( HDC dc, HBITMAP bitmap, unsigned int width,
                                               unsigned int height, unsigned int depth, void *pixels )
{
    struct { BITMAPINFOHEADER header; RGBQUAD colors[2]; } dib = {0};
    BITMAPINFO *info = (BITMAPINFO *)&dib;
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = -(int)height;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = depth;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = ((width * depth + 31) / 32 * 4) * height;
    if (depth == 1) dib.colors[1].rgbRed = dib.colors[1].rgbGreen = dib.colors[1].rgbBlue = 255;
    return NtGdiGetDIBitsInternal( dc, bitmap, 0, height, pixels, info, DIB_RGB_COLORS, 0, 0 ) == (int)height;
}

static inline BOOL wine_ps5_cursor_build_shape( HCURSOR cursor, struct ps5_desktop_cursor_shape *shape )
{
    unsigned char mask[PS5_DESKTOP_CURSOR_SIDE / 8 * PS5_DESKTOP_CURSOR_SIDE * 2];
    ICONINFO icon = {0};
    BITMAP bitmap;
    HDC dc = 0;
    unsigned int width, height, stride, x, y;
    BOOL alpha = FALSE, result = FALSE;

    *shape = (struct ps5_desktop_cursor_shape){.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape)}};
    if (!cursor) return TRUE;
    /* NtUserGetIconInfo returns frame zero for animated cursors. */
    if (!NtUserGetIconInfo( cursor, &icon, NULL, NULL, NULL, 0 )) return FALSE;
    if (!icon.hbmMask || !NtGdiExtGetObjectW( icon.hbmColor ? icon.hbmColor : icon.hbmMask,
                                            sizeof(bitmap), &bitmap )) goto done;
    if (bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0) goto done;
    width = bitmap.bmWidth;
    height = icon.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
    if (!width || !height || width > PS5_DESKTOP_CURSOR_SIDE || height > PS5_DESKTOP_CURSOR_SIDE)
    {
        WARN( "Cursor %p exceeds the %ux%u plane image limit\n", cursor,
              PS5_DESKTOP_CURSOR_SIDE, PS5_DESKTOP_CURSOR_SIDE );
        goto done;
    }
    stride = (width + 31) / 32 * 4;
    if (!(dc = NtGdiCreateCompatibleDC( 0 ))) goto done;
    if (icon.hbmColor)
    {
        if (!wine_ps5_cursor_read_bitmap( dc, icon.hbmColor, width, height, 32, shape->pixels )) goto done;
        for (x = 0; x < width * height; x++)
            if (shape->pixels[x] >> 24) { alpha = TRUE; break; }
    }
    if (!alpha && !wine_ps5_cursor_read_bitmap( dc, icon.hbmMask, width,
            height * (icon.hbmColor ? 1 : 2), 1, mask )) goto done;
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
        {
            uint32_t *pixel = &shape->pixels[y * width + x];
            if (alpha)
            {
                if (!(*pixel >> 24)) *pixel = 0;
            }
            else
            {
                unsigned int and_bit = !!(mask[y * stride + x / 8] & (0x80 >> (x % 8)));
                if (icon.hbmColor) *pixel = and_bit ? 0 : *pixel | 0xff000000u;
                else
                {
                    unsigned int xor_bit = !!(mask[(height + y) * stride + x / 8] & (0x80 >> (x % 8)));
                    /* The overlay cannot invert the scene: invert is black, as on macdrv. */
                    *pixel = and_bit && !xor_bit ? 0 :
                             !and_bit && xor_bit ? 0xffffffffu : 0xff000000u;
                }
            }
        }
    shape->width = width;
    shape->height = height;
    shape->hotspot_x = icon.xHotspot < width ? icon.xHotspot : width / 2;
    shape->hotspot_y = icon.yHotspot < height ? icon.yHotspot : height / 2;
    shape->visible = 1;
    shape->header.size += width * height * sizeof(shape->pixels[0]);
    result = TRUE;
done:
    if (dc) NtGdiDeleteObjectApp( dc );
    if (icon.hbmColor) NtGdiDeleteObjectApp( icon.hbmColor );
    if (icon.hbmMask) NtGdiDeleteObjectApp( icon.hbmMask );
    return result;
}

static inline void wine_ps5_cursor_destroy( struct wine_ps5_cursor_cache *cache, HCURSOR cursor )
{
    struct wine_ps5_cursor_entry **link, *entry;
    pthread_mutex_lock( &cache->mutex );
    for (link = &cache->entries; (entry = *link); link = &entry->next)
        if (entry->cursor == cursor)
        {
            *link = entry->next;
            free( entry );
            break;
        }
    pthread_mutex_unlock( &cache->mutex );
}

/* Cache lookup/convert with the mutex held. visible_pixels counts non-transparent pixels. */
static inline struct wine_ps5_cursor_entry *wine_ps5_cursor_lookup( struct wine_ps5_cursor_cache *cache,
                                                                     HCURSOR cursor )
{
    union
    {
        struct ps5_desktop_cursor_shape shape;
        unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
    } message;
    struct wine_ps5_cursor_entry *entry;
    unsigned int i, visible = 0;

    for (entry = cache->entries; entry; entry = entry->next)
        if (entry->cursor == cursor) return entry;
    if (!wine_ps5_cursor_build_shape( cursor, &message.shape )) return NULL;
    if (!(entry = malloc( sizeof(*entry) + message.shape.header.size ))) return NULL;
    for (i = 0; i < message.shape.width * message.shape.height; i++)
        if (message.shape.pixels[i] >> 24) visible++;
    entry->cursor = cursor;
    entry->width = message.shape.width;
    entry->height = message.shape.height;
    entry->visible_pixels = visible;
    entry->reserved = 0;
    memcpy( entry->bytes, &message, message.shape.header.size );
    entry->next = cache->entries;
    cache->entries = entry;
    return entry;
}

/* The converted size and number of non-transparent pixels of a cursor. */
static inline BOOL wine_ps5_cursor_info( struct wine_ps5_cursor_cache *cache, HCURSOR cursor,
                                        uint32_t *width, uint32_t *height, uint32_t *visible_pixels )
{
    struct wine_ps5_cursor_entry *entry;
    BOOL result = FALSE;

    pthread_mutex_lock( &cache->mutex );
    if ((entry = wine_ps5_cursor_lookup( cache, cursor )))
    {
        *width = entry->width;
        *height = entry->height;
        *visible_pixels = entry->visible_pixels;
        result = TRUE;
    }
    pthread_mutex_unlock( &cache->mutex );
    return result;
}

static inline BOOL wine_ps5_cursor_publish( struct wine_ps5_cursor_cache *cache, uint64_t id,
                                           HCURSOR cursor, wine_ps5_cursor_send send )
{
    struct wine_ps5_cursor_entry *entry;
    struct ps5_desktop_cursor_shape *shape;
    BOOL result = FALSE;

    if (!send) return FALSE;
    pthread_mutex_lock( &cache->mutex );
    if (!(entry = wine_ps5_cursor_lookup( cache, cursor ))) goto done;
    shape = (struct ps5_desktop_cursor_shape *)entry->bytes;
    shape->id = id;
    /* Cache conversion only: every SetCursor republishes, since another
     * process may have changed the desktop cursor since the previous call. */
    result = send( shape, shape->header.size );
done:
    pthread_mutex_unlock( &cache->mutex );
    return result;
}
#endif /* __WINE_PS5_CURSOR_H */
