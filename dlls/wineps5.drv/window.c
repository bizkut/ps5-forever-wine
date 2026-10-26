/*
 * PS5 windows and shared-memory DIB surfaces
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ps5drv.h"
#include "wine/rbtree.h"
#include "wine/ps5_title.h"

WINE_DEFAULT_DEBUG_CHANNEL(ps5drv);

struct ps5_window_surface
{
    struct window_surface header;
    void *bits;  /* draw plane, followed by size bytes of published pixels */
    size_t size; /* bytes in one plane */
    UINT stride;
    char shm[PS5_DESKTOP_SHM_NAME];
    RECT clip;  /* bounding visible rect in surface coordinates */
    BOOL clipped;
};

struct ps5_win_data
{
    struct rb_entry entry;
    HWND hwnd;
    struct window_rects rects;
    UINT style, ex_style;
    BYTE alpha;
    UINT color_key;
    uint64_t insert_after;
    struct ps5_window_surface *surface; /* owns a reference, released outside win_data_mutex */
    struct ps5_desktop_window state;
    BOOL published;
};

static pthread_mutex_t win_data_mutex = PTHREAD_MUTEX_INITIALIZER;
static LONG surface_serial;

/* A top-level window's surface for other processes (PS5_GetForeignWindowSurface):
 * its creator's pid and serial, which name its shared memory with the window,
 * and its rectangle as left, top, width, height, 16 bits each. */
static const WCHAR surface_id_prop[] = {'_','_','w','i','n','e','_','p','s','5','_','s','u','r','f','a','c','e',0};
static const WCHAR surface_rect_prop[] = {'_','_','w','i','n','e','_','p','s','5','_','s','u','r','f','a','c','e','_','r','e','c','t',0};

static int win_data_compare( const void *key, const struct rb_entry *entry )
{
    const struct ps5_win_data *data = RB_ENTRY_VALUE( entry, const struct ps5_win_data, entry );
    UINT_PTR hwnd = (UINT_PTR)key, other = (UINT_PTR)data->hwnd;

    return hwnd < other ? -1 : hwnd > other;
}

static struct rb_tree win_data_tree = { win_data_compare };
static const struct window_surface_funcs ps5_surface_funcs;

static BOOL intersect_rect( RECT *dst, const RECT *left, const RECT *right )
{
    dst->left = max( left->left, right->left );
    dst->top = max( left->top, right->top );
    dst->right = min( left->right, right->right );
    dst->bottom = min( left->bottom, right->bottom );
    if (!IsRectEmpty( dst )) return TRUE;
    SetRectEmpty( dst );
    return FALSE;
}

static struct ps5_win_data *find_win_data( HWND hwnd )
{
    struct rb_entry *entry = rb_get( &win_data_tree, hwnd );
    return entry ? RB_ENTRY_VALUE( entry, struct ps5_win_data, entry ) : NULL;
}

static BOOL is_toplevel( HWND hwnd )
{
    HWND parent = NtUserGetAncestor( hwnd, GA_PARENT );
    return parent && parent == NtUserGetDesktopWindow();
}

/* All fields below are protected by win_data_mutex, including the surface's
 * clip bounding box. Never take a surface mutex while holding win_data_mutex:
 * win32u calls surface callbacks with the surface mutex already held. */
static void get_window_state( const struct ps5_win_data *data, struct ps5_desktop_window *state )
{
    const struct ps5_window_surface *surface = data->surface;
    RECT visible, bounds;

    memset( state, 0, sizeof(*state) );
    state->header.type = PS5_DESKTOP_WINDOW;
    state->header.size = sizeof(*state);
    state->id = (UINT_PTR)data->hwnd;
    state->insert_after = data->insert_after;
    state->alpha = data->alpha;
    state->color_key = data->color_key;
    /* A minimized window stays visible: with no taskbar, win32u keeps it as
     * a small title bar at the bottom of the screen (MinArrange). */
    if (data->style & WS_VISIBLE) state->flags |= PS5_DESKTOP_VISIBLE;
    if (data->ex_style & WS_EX_TOPMOST) state->flags |= PS5_DESKTOP_TOPMOST;
    if (data->ex_style & WS_EX_LAYERED) state->flags |= PS5_DESKTOP_LAYERED;
    state->x = data->rects.visible.left;
    state->y = data->rects.visible.top;
    if (!surface) return;
    /* UpdateLayeredWindow's surfaces: alpha_mask 0xff000000, the pixels
     * premultiplied by their alpha (a soft shadow around the window). */
    if ((data->ex_style & WS_EX_LAYERED) && surface->header.alpha_mask) state->flags |= PS5_DESKTOP_PREMULTIPLIED;

    state->x += surface->header.rect.left;
    state->y += surface->header.rect.top;
    state->width = surface->header.rect.right - surface->header.rect.left;
    state->height = surface->header.rect.bottom - surface->header.rect.top;
    state->stride = surface->stride;
    memcpy( state->shm, surface->shm, sizeof(state->shm) );
    bounds.left = bounds.top = 0;
    bounds.right = state->width;
    bounds.bottom = state->height;
    visible = data->rects.visible;
    OffsetRect( &visible, -state->x, -state->y );
    intersect_rect( &visible, &visible, &bounds );
    if (surface->clipped) intersect_rect( &visible, &visible, &surface->clip );
    state->visible_left = visible.left;
    state->visible_top = visible.top;
    state->visible_right = visible.right;
    state->visible_bottom = visible.bottom;
}

/* A state equal to the one last sent is skipped unless it moves the window
 * in the z-order: a layered window that animates calls in here every frame,
 * and the title redraws the whole window for each state it receives. */
static BOOL send_window_state( struct ps5_win_data *data, BOOL zorder )
{
    struct ps5_desktop_window state;

    get_window_state( data, &state );
    if (!zorder && data->published && !memcmp( &state, &data->state, sizeof(state) )) return TRUE;
    if (zorder) state.flags |= PS5_DESKTOP_ZORDER;
    if (!ps5_send_message( &state, sizeof(state) )) return FALSE;
    state.flags &= ~PS5_DESKTOP_ZORDER;
    data->state = state;
    data->published = TRUE;
    return TRUE;
}

static void ps5_surface_set_clip( struct window_surface *window_surface, const RECT *rects, UINT count )
{
    struct ps5_window_surface *surface = (struct ps5_window_surface *)window_surface;
    struct ps5_win_data *data;
    UINT i;

    pthread_mutex_lock( &win_data_mutex );
    surface->clipped = !!window_surface->clip_region;
    SetRectEmpty( &surface->clip );
    if (count) surface->clip = rects[0];
    for (i = 1; i < count; i++)
    {
        surface->clip.left = min( surface->clip.left, rects[i].left );
        surface->clip.top = min( surface->clip.top, rects[i].top );
        surface->clip.right = max( surface->clip.right, rects[i].right );
        surface->clip.bottom = max( surface->clip.bottom, rects[i].bottom );
    }
    OffsetRect( &surface->clip, -window_surface->rect.left, -window_surface->rect.top );
    if ((data = find_win_data( window_surface->hwnd )) && data->surface == surface)
        send_window_state( data, FALSE );
    pthread_mutex_unlock( &win_data_mutex );
}

static BOOL ps5_surface_flush( struct window_surface *window_surface, const RECT *rect, const RECT *dirty,
                               const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                               const BITMAPINFO *shape_info, const void *shape_bits )
{
    struct ps5_window_surface *surface = (struct ps5_window_surface *)window_surface;
    struct ps5_desktop_damage damage =
    {
        .header = { PS5_DESKTOP_DAMAGE, sizeof(damage) },
        .id = (UINT_PTR)window_surface->hwnd,
    };
    const RECT bounds = { 0, 0, surface->stride / 4, surface->size / surface->stride };
    struct ps5_win_data *data;
    const BYTE *src;
    BYTE *dst;
    RECT clipped;
    int y;
    BOOL ret = TRUE;

    if (!intersect_rect( &clipped, dirty, &bounds )) return TRUE;
    /* GDI and foreign processes share the first plane, including bitmap reads.
     * A private DIB must update those draw pixels before publishing, so a later
     * owner flush retains foreign drawing. dirty and color_bits are relative
     * to the DIB, not rect->left/top, which can be negative. */
    if (color_bits != surface->bits)
    {
        UINT src_stride = color_info->bmiHeader.biWidth * 4;
        int src_y, height = abs( color_info->bmiHeader.biHeight );

        dst = (BYTE *)surface->bits + clipped.top * surface->stride + clipped.left * 4;
        for (y = clipped.top; y < clipped.bottom; y++, dst += surface->stride)
        {
            src_y = color_info->bmiHeader.biHeight < 0 ? y : height - y - 1;
            src = (const BYTE *)color_bits + src_y * src_stride + clipped.left * 4;
            memcpy( dst, src, (clipped.right - clipped.left) * 4 );
        }
    }
    src = (const BYTE *)surface->bits + clipped.top * surface->stride + clipped.left * 4;
    dst = (BYTE *)surface->bits + surface->size + clipped.top * surface->stride + clipped.left * 4;
    for (y = clipped.top; y < clipped.bottom; y++, src += surface->stride, dst += surface->stride)
        memcpy( dst, src, (clipped.right - clipped.left) * 4 );
    damage.left = clipped.left;
    damage.top = clipped.top;
    damage.right = clipped.right;
    damage.bottom = clipped.bottom;

    pthread_mutex_lock( &win_data_mutex );
    if ((data = find_win_data( window_surface->hwnd )) && data->surface == surface)
    {
        /* win32u has already converted COLORREF to the DIB's 0x00RRGGBB. It
         * also stores UpdateLayeredWindow's color key and alpha mask on the
         * surface; an unchanged state is not sent again. */
        data->color_key = window_surface->color_key;
        ret = send_window_state( data, !data->published );
        if (ret) ret = ps5_send_message( &damage, sizeof(damage) );
    }
    pthread_mutex_unlock( &win_data_mutex );
    return ret;
}

static int unlink_surface( const char *name )
{
#ifdef __PROSPERO__
    if (ps5_title_desktop)
        return __wine_ps5_desktop_surface( name, 0, PS5_TITLE_SURFACE_UNLINK );
#endif
    return shm_unlink( name );
}

static void ps5_surface_destroy( struct window_surface *window_surface )
{
    struct ps5_window_surface *surface = (struct ps5_window_surface *)window_surface;

    if (surface->bits) munmap( surface->bits, 2 * surface->size );
    if (surface->shm[0]) unlink_surface( surface->shm );
}

static const struct window_surface_funcs ps5_surface_funcs =
{
    ps5_surface_set_clip,
    ps5_surface_flush,
    ps5_surface_destroy,
};

static struct window_surface *create_surface( HWND hwnd, const RECT *rect )
{
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    struct ps5_window_surface *surface;
    struct window_surface *window_surface;
    D3DKMT_CREATEDCFROMMEMORY desc = {0};
    char shm[PS5_DESKTOP_SHM_NAME];
    unsigned int serial;
    HBITMAP bitmap = 0;
    NTSTATUS status;
    size_t size;
    void *bits;
    int fd;

    if (width <= 0 || height <= 0 || width > UINT_MAX / 4 ||
        (size_t)height > UINT_MAX / (width * 4u)) return NULL;
    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = -height;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = width * height * 4u;
    size = info->bmiHeader.biSizeImage;
    if (size > SIZE_MAX / 2) return NULL;
    /* O_EXCL also handles names left by a killed process with a reused pid. */
    do
    {
        serial = InterlockedIncrement( &surface_serial );
        snprintf( shm, sizeof(shm), "/wine-ps5-%u-%lx-%u",
                  (unsigned int)getpid(), (unsigned long)(UINT_PTR)hwnd, serial );
#ifdef __PROSPERO__
        if (ps5_title_desktop)
            fd = __wine_ps5_desktop_surface( shm, 2 * size, PS5_TITLE_SURFACE_CREATE );
        else
#endif
            fd = shm_open( shm, O_RDWR | O_CREAT | O_EXCL, 0600 );
    } while (fd < 0 && errno == EEXIST);
    if (fd < 0)
    {
        ERR( "Cannot create the shared surface for %p: %s\n", hwnd, strerror( errno ) );
        return NULL;
    }
#ifdef __PROSPERO__
    if (ps5_title_desktop)
        bits = mmap( NULL, 2 * size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    else
#endif
    if (fchmod( fd, 0666 ) || ftruncate( fd, 2 * size ))
        bits = MAP_FAILED;
    else
        bits = mmap( NULL, 2 * size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (bits == MAP_FAILED)
    {
        ERR( "Cannot create the shared surface for %p: %s\n", hwnd, strerror( errno ) );
        close( fd );
        unlink_surface( shm );
        return NULL;
    }
    close( fd );
#ifdef __PROSPERO__
    if (ps5_title_desktop)
    {
        static LONG surface_logs;
        /* Bounded evidence of the sizes behind big Chromium buffers. */
        if (InterlockedIncrement( &surface_logs ) <= 32)
            ERR( "ps5-surface hwnd=%p rect=%d,%d-%d,%d size=%dx%d plane_bytes=%zu\n", hwnd,
                 (int)rect->left, (int)rect->top, (int)rect->right, (int)rect->bottom, width, height,
                 size );
    }
#endif

    /* GDI draws into the first plane; a flush copies dirty rows into the
     * second plane, the only pixels the title reads. */
    desc.Width = width;
    desc.Height = height;
    desc.Pitch = width * 4u;
    desc.Format = D3DDDIFMT_A8R8G8B8;
    desc.pMemory = bits;
    desc.hDeviceDc = NtUserGetDCEx( hwnd, 0, DCX_CACHE | DCX_WINDOW );
    if ((status = NtGdiDdDDICreateDCFromMemory( &desc )))
        WARN( "Cannot draw %p straight into its shared surface (%#x); flushes copy it\n", hwnd, (int)status );
    else
    {
        bitmap = desc.hBitmap;
        NtGdiDeleteObjectApp( desc.hDc );
    }
    if (desc.hDeviceDc) NtUserReleaseDC( hwnd, desc.hDeviceDc );

    /* It paints the bitmap white, the shared memory itself when it wraps it. */
    if (!(window_surface = window_surface_create( sizeof(*surface), &ps5_surface_funcs, hwnd, rect, info, bitmap )))
    {
        if (bitmap) NtGdiDeleteObjectApp( bitmap );
        munmap( bits, 2 * size );
        unlink_surface( shm );
        return NULL;
    }
    surface = (struct ps5_window_surface *)window_surface;
    surface->bits = bits;
    surface->size = size;
    surface->stride = width * 4u;
    memcpy( surface->shm, shm, sizeof(shm) );
    /* Match window_surface_create's initial white DIB, including pixels
     * outside the first dirty rectangle. */
    if (!bitmap) memset( bits, 0xff, size );
    memset( (BYTE *)bits + size, 0xff, size );
    if (width <= 0xffff && height <= 0xffff && rect->left == (INT16)rect->left && rect->top == (INT16)rect->top)
    {
        NtUserSetProp( hwnd, surface_id_prop, (HANDLE)(UINT_PTR)(((UINT64)getpid() << 32) | serial) );
        NtUserSetProp( hwnd, surface_rect_prop,
                       (HANDLE)(UINT_PTR)((UINT16)rect->left | (UINT64)(UINT16)rect->top << 16 |
                                          (UINT64)width << 32 | (UINT64)height << 48) );
    }
    return window_surface;
}

/* Another process's window surface, drawn into from this one: open both planes
 * by the name the owner published, and wrap the same draw plane as the owner.
 * A flush publishes its dirty rows and reports damage for the owner's window. */
struct ps5_foreign_surface
{
    struct window_surface header;
    void *bits;  /* draw plane, followed by size bytes of published pixels */
    size_t size; /* bytes in one plane */
    UINT stride;
};

static void foreign_surface_set_clip( struct window_surface *window_surface, const RECT *rects, UINT count )
{
    /* The owner reports what of its window shows. */
}

static BOOL foreign_surface_flush( struct window_surface *window_surface, const RECT *rect, const RECT *dirty,
                                   const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                   const BITMAPINFO *shape_info, const void *shape_bits )
{
    struct ps5_foreign_surface *surface = (struct ps5_foreign_surface *)window_surface;
    struct ps5_desktop_damage damage =
    {
        .header = { PS5_DESKTOP_DAMAGE, sizeof(damage) },
        .id = (UINT_PTR)window_surface->hwnd,
    };
    const RECT bounds = { 0, 0, surface->stride / 4, surface->size / surface->stride };
    int y;
    const BYTE *src;
    BYTE *dst;
    RECT clipped;

    if (!intersect_rect( &clipped, dirty, &bounds )) return TRUE;
    /* Keep the shared draw plane current when this process uses a private DIB. */
    if (color_bits != surface->bits)
    {
        UINT src_stride = color_info->bmiHeader.biWidth * 4;
        int src_y, height = abs( color_info->bmiHeader.biHeight );

        dst = (BYTE *)surface->bits + clipped.top * surface->stride + clipped.left * 4;
        for (y = clipped.top; y < clipped.bottom; y++, dst += surface->stride)
        {
            src_y = color_info->bmiHeader.biHeight < 0 ? y : height - y - 1;
            src = (const BYTE *)color_bits + src_y * src_stride + clipped.left * 4;
            memcpy( dst, src, (clipped.right - clipped.left) * 4 );
        }
    }
    src = (const BYTE *)surface->bits + clipped.top * surface->stride + clipped.left * 4;
    dst = (BYTE *)surface->bits + surface->size + clipped.top * surface->stride + clipped.left * 4;
    for (y = clipped.top; y < clipped.bottom; y++, src += surface->stride, dst += surface->stride)
        memcpy( dst, src, (clipped.right - clipped.left) * 4 );
    damage.left = clipped.left;
    damage.top = clipped.top;
    damage.right = clipped.right;
    damage.bottom = clipped.bottom;
    return ps5_send_message( &damage, sizeof(damage) );
}

static void foreign_surface_destroy( struct window_surface *window_surface )
{
    struct ps5_foreign_surface *surface = (struct ps5_foreign_surface *)window_surface;

    /* The shared memory is the owner's: it unlinks it. */
    if (surface->bits) munmap( surface->bits, 2 * surface->size );
}

static const struct window_surface_funcs foreign_surface_funcs =
{
    foreign_surface_set_clip,
    foreign_surface_flush,
    foreign_surface_destroy,
};

/* Each GetDC on a foreign window comes here: the surfaces opened last are
 * kept while their owner's surface stays the same. */
struct foreign_entry
{
    HWND hwnd;
    UINT64 id, rect;
    struct window_surface *surface;
};
static struct foreign_entry foreign_entries[8];
static unsigned int foreign_next;
static pthread_mutex_t foreign_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct window_surface *open_foreign_surface( HWND hwnd, UINT64 id, UINT64 packed )
{
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    D3DKMT_CREATEDCFROMMEMORY desc = {0};
    struct ps5_foreign_surface *surface;
    struct window_surface *window_surface;
    RECT rect;
    int width = (UINT16)(packed >> 32), height = (UINT16)(packed >> 48), fd;
    char shm[PS5_DESKTOP_SHM_NAME];
    size_t size;
    HBITMAP bitmap = 0;
    struct stat st;
    void *bits, *saved;

    if (!width || !height || (size_t)height > SIZE_MAX / (width * 8u)) return NULL;
    size = (size_t)width * height * 4;
    SetRect( &rect, (INT16)packed, (INT16)(packed >> 16), (INT16)packed + width, (INT16)(packed >> 16) + height );
    snprintf( shm, sizeof(shm), "/wine-ps5-%u-%lx-%u", (unsigned int)(id >> 32), (unsigned long)(UINT_PTR)hwnd,
              (unsigned int)id );
#ifdef __PROSPERO__
    if (ps5_title_desktop)
        fd = __wine_ps5_desktop_surface( shm, 2 * size, PS5_TITLE_SURFACE_OPEN );
    else
#endif
        fd = shm_open( shm, O_RDWR, 0 );
    if (fd < 0)
    {
        WARN( "Cannot open %s, the surface of %p: %s\n", shm, hwnd, strerror( errno ) );
        return NULL;
    }
#ifdef __PROSPERO__
    if (ps5_title_desktop)
        bits = mmap( NULL, 2 * size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    else
#endif
    if (fstat( fd, &st ) || st.st_size < 0 || (uint64_t)st.st_size < 2 * size)
        bits = MAP_FAILED;
    else
        bits = mmap( NULL, 2 * size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (bits == MAP_FAILED)
    {
        WARN( "Cannot map %s, the surface of %p: %s\n", shm, hwnd, strerror( errno ) );
        close( fd );
        return NULL;
    }
    close( fd );

    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = -height;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = size;
    desc.Width = width;
    desc.Height = height;
    desc.Pitch = width * 4u;
    desc.Format = D3DDDIFMT_A8R8G8B8;
    desc.pMemory = bits;
    desc.hDeviceDc = NtGdiCreateCompatibleDC( 0 );
    if (desc.hDeviceDc && !NtGdiDdDDICreateDCFromMemory( &desc ))
    {
        bitmap = desc.hBitmap;
        NtGdiDeleteObjectApp( desc.hDc );
    }
    if (desc.hDeviceDc) NtGdiDeleteObjectApp( desc.hDeviceDc );

    /* window_surface_create paints the bitmap white, here the owner's draw plane. */
    saved = bitmap ? malloc( size ) : NULL;
    if (saved) memcpy( saved, bits, size );
    window_surface = window_surface_create( sizeof(*surface), &foreign_surface_funcs, hwnd, &rect, info, bitmap );
    if (saved)
    {
        memcpy( bits, saved, size );
        free( saved );
    }
    if (!window_surface)
    {
        if (bitmap) NtGdiDeleteObjectApp( bitmap );
        munmap( bits, 2 * size );
        return NULL;
    }
    surface = (struct ps5_foreign_surface *)window_surface;
    surface->bits = bits;
    surface->size = size;
    surface->stride = width * 4u;
    TRACE( "drawing into %p from this process through %s\n", hwnd, shm );
    return window_surface;
}

struct window_surface *PS5_GetForeignWindowSurface( HWND hwnd )
{
    UINT64 id = (UINT_PTR)NtUserGetProp( hwnd, surface_id_prop );
    UINT64 packed = (UINT_PTR)NtUserGetProp( hwnd, surface_rect_prop );
    struct window_surface *surface = NULL;
    struct foreign_entry *entry = NULL;
    unsigned int i;

    if (!id || !packed) return NULL;
    pthread_mutex_lock( &foreign_mutex );
    for (i = 0; i < ARRAY_SIZE(foreign_entries); i++)
        if (foreign_entries[i].hwnd == hwnd) entry = &foreign_entries[i];
    if (!entry || entry->id != id || entry->rect != packed)
    {
        if ((surface = open_foreign_surface( hwnd, id, packed )))
        {
            if (!entry) entry = &foreign_entries[foreign_next++ % ARRAY_SIZE(foreign_entries)];
            if (entry->surface) window_surface_release( entry->surface );
            *entry = (struct foreign_entry){ .hwnd = hwnd, .id = id, .rect = packed, .surface = surface };
        }
    }
    else surface = entry->surface;
    if (surface) window_surface_add_ref( surface );
    pthread_mutex_unlock( &foreign_mutex );
    return surface;
}

BOOL PS5_WindowPosChanging( HWND hwnd, UINT flags, BOOL shaped, const struct window_rects *rects )
{
    struct ps5_win_data *data;
    UINT style, ex_style;

    /* Child windows use their ancestor's surface in win32u. The desktop and
     * HWND_MESSAGE do not need a surface of their own. */
    if (!is_toplevel( hwnd )) return TRUE;
    style = NtUserGetWindowLongW( hwnd, GWL_STYLE );
    ex_style = NtUserGetWindowLongW( hwnd, GWL_EXSTYLE );
    pthread_mutex_lock( &win_data_mutex );
    if (!(data = find_win_data( hwnd )))
    {
        if (!(data = calloc( 1, sizeof(*data) )))
        {
            pthread_mutex_unlock( &win_data_mutex );
            return FALSE;
        }
        data->hwnd = hwnd;
        data->alpha = 255;
        data->color_key = ~0u;
        data->rects = *rects;
        data->style = style;
        data->ex_style = ex_style;
        rb_put( &win_data_tree, hwnd, &data->entry );
    }
    pthread_mutex_unlock( &win_data_mutex );
    return TRUE;
}

BOOL PS5_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *rect, struct window_surface **surface )
{
    struct window_surface *previous = *surface, *created;

    if (!is_toplevel( hwnd )) return FALSE;
    if (previous && previous->funcs == &ps5_surface_funcs && EqualRect( &previous->rect, rect )) return TRUE;
    if (!(created = create_surface( hwnd, rect ))) return FALSE;
    *surface = created;
    if (previous) window_surface_release( previous );
    return TRUE;
}

void PS5_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner, UINT flags,
                           const struct window_rects *rects, struct window_surface *window_surface )
{
    struct ps5_window_surface *surface = NULL, *previous;
    struct ps5_win_data *data;
    UINT style = NtUserGetWindowLongW( hwnd, GWL_STYLE );
    UINT ex_style = NtUserGetWindowLongW( hwnd, GWL_EXSTYLE );
    BOOL zorder;

    /* Use the actual Wine z-order, not special HWND_TOPMOST/NOTOPMOST values
     * or a caller's insertion hint. HWNDs identify windows across processes. */
    insert_after = NtUserGetWindowRelative( hwnd, GW_HWNDPREV );
    if (window_surface && window_surface->funcs == &ps5_surface_funcs)
    {
        surface = (struct ps5_window_surface *)window_surface;
        window_surface_add_ref( window_surface );
    }
    pthread_mutex_lock( &win_data_mutex );
    if ((data = find_win_data( hwnd )))
    {
        previous = data->surface;
        zorder = !(flags & SWP_NOZORDER) || !data->published ||
                 ((data->ex_style ^ ex_style) & WS_EX_TOPMOST);
        data->surface = surface;
        data->rects = *rects;
        data->style = style;
        data->ex_style = ex_style;
        data->insert_after = (UINT_PTR)insert_after;
        send_window_state( data, zorder );
    }
    else previous = surface;
    pthread_mutex_unlock( &win_data_mutex );
    if (previous) window_surface_release( &previous->header );
}

void PS5_DestroyWindow( HWND hwnd )
{
    struct ps5_desktop_id destroy =
    {
        .header = { PS5_DESKTOP_DESTROY, sizeof(destroy) },
        .id = (UINT_PTR)hwnd,
    };
    struct ps5_win_data *data;
    struct ps5_window_surface *surface;

    pthread_mutex_lock( &win_data_mutex );
    if (!(data = find_win_data( hwnd )))
    {
        pthread_mutex_unlock( &win_data_mutex );
        return;
    }
    rb_remove( &win_data_tree, &data->entry );
    surface = data->surface;
    if (data->published) ps5_send_message( &destroy, sizeof(destroy) );
    pthread_mutex_unlock( &win_data_mutex );
    if (surface) window_surface_release( &surface->header );
    free( data );
}

UINT PS5_ShowWindow( HWND hwnd, INT cmd, RECT *rect, UINT flags )
{
    struct ps5_win_data *data;

    pthread_mutex_lock( &win_data_mutex );
    if ((data = find_win_data( hwnd )))
    {
        if (cmd == SW_HIDE) data->style &= ~WS_VISIBLE;
        else data->style |= WS_VISIBLE;
        send_window_state( data, FALSE );
    }
    pthread_mutex_unlock( &win_data_mutex );
    /* No window manager: keep win32u's placement. ~0 would move a minimized
     * window to -32000,-32000 for a taskbar to show; win32u's own position is
     * a title bar at the bottom left of the screen. */
    return flags;
}

void PS5_SetLayeredWindowAttributes( HWND hwnd, COLORREF key, BYTE alpha, DWORD flags )
{
    struct ps5_win_data *data;

    pthread_mutex_lock( &win_data_mutex );
    if ((data = find_win_data( hwnd )))
    {
        data->alpha = flags & LWA_ALPHA ? alpha : 255;
        data->color_key = flags & LWA_COLORKEY ? (GetRValue( key ) << 16) | (GetGValue( key ) << 8) | GetBValue( key ) : ~0u;
        send_window_state( data, FALSE );
    }
    pthread_mutex_unlock( &win_data_mutex );
}

void PS5_UpdateLayeredWindow( HWND hwnd, BYTE alpha, UINT flags )
{
    struct ps5_win_data *data;

    pthread_mutex_lock( &win_data_mutex );
    if ((data = find_win_data( hwnd )))
    {
        data->alpha = flags & ULW_ALPHA ? alpha : 255;
        if (!(flags & ULW_COLORKEY)) data->color_key = ~0u;
        send_window_state( data, FALSE );
    }
    pthread_mutex_unlock( &win_data_mutex );
}

void PS5_SetWindowStyle( HWND hwnd, INT offset, STYLESTRUCT *style )
{
    struct ps5_win_data *data;
    HWND insert_after = NtUserGetWindowRelative( hwnd, GW_HWNDPREV );
    BOOL zorder = offset == GWL_EXSTYLE && ((style->styleOld ^ style->styleNew) & WS_EX_TOPMOST);

    pthread_mutex_lock( &win_data_mutex );
    if ((data = find_win_data( hwnd )))
    {
        if (offset == GWL_STYLE) data->style = style->styleNew;
        else if (offset == GWL_EXSTYLE)
        {
            data->ex_style = style->styleNew;
            if (!(data->ex_style & WS_EX_LAYERED))
            {
                data->alpha = 255;
                data->color_key = ~0u;
            }
        }
        if (zorder) data->insert_after = (UINT_PTR)insert_after;
        send_window_state( data, zorder );
    }
    pthread_mutex_unlock( &win_data_mutex );
}

void PS5_SetParent( HWND hwnd, HWND parent, HWND old_parent )
{
    if (parent != old_parent && parent != NtUserGetDesktopWindow()) PS5_DestroyWindow( hwnd );
    /* Becoming a top-level window is handled by the ensuing position update. */
}
