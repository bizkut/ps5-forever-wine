/*
 * PS5 driver initialization, desktop connection and input
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

#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __PROSPERO__
#include <ps5platform/klog.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ps5drv.h"
#include "wine/server.h"
#include "wine/ps5_title.h"

WINE_DEFAULT_DEBUG_CHANNEL(ps5drv);

static pthread_mutex_t connection_mutex = PTHREAD_MUTEX_INITIALIZER;
static int desktop_fd = -1;
static DWORD desktop_tid;
static BOOL queue_registered;
static BOOL processing_events;
#ifdef __PROSPERO__
BOOL ps5_title_desktop;
#endif

static union
{
    struct ps5_desktop_header header;
    struct ps5_desktop_mouse mouse;
    struct ps5_desktop_key key;
    uint64_t alignment;
    unsigned char bytes[sizeof(struct ps5_desktop_text) + PS5_DESKTOP_MAX_TEXT * sizeof(uint16_t)];
} input_event;
static unsigned int input_size;

/* The queue holds its own reference to the socket, and set_queue_fd can only
 * be called once on a thread. Like the Wayland driver, we must stop on a lost
 * established connection, not reconnect onto a socket the queue cannot watch. */
static void connection_lost(void)
{
    ERR( "Connection to the PS5 desktop hub was lost\n" );
    NtTerminateProcess( 0, 1 );
}

static BOOL write_message( int fd, const void *message, unsigned int size )
{
    const char *ptr = message;
    ssize_t ret;

    while (size)
    {
        ret = send( fd, ptr, size, MSG_NOSIGNAL );
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

static void register_queue(void)
{
    HANDLE handle;
    NTSTATUS status;

    if (queue_registered || !desktop_tid || desktop_tid != GetCurrentThreadId()) return;
    if ((status = wine_server_fd_to_handle( desktop_fd, GENERIC_READ | SYNCHRONIZE, 0, &handle )))
    {
        ERR( "Cannot allocate a handle for the desktop socket, status %#x\n", status );
        NtTerminateProcess( 0, 1 );
        return;
    }
    SERVER_START_REQ( set_queue_fd )
    {
        req->handle = wine_server_obj_handle( handle );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    NtClose( handle );
    if (status)
    {
        ERR( "Cannot register the desktop queue socket, status %#x\n", status );
        NtTerminateProcess( 0, 1 );
        return;
    }
    queue_registered = TRUE;
}

static BOOL connect_desktop(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct sockaddr_in tcp = { .sin_family = AF_INET,
                               .sin_port = htons( PS5_DESKTOP_DRIVER_PORT ),
                               .sin_addr.s_addr = htonl( INADDR_LOOPBACK ) };
    struct ps5_desktop_hello hello =
    {
        .header = { PS5_DESKTOP_HELLO, sizeof(hello) },
        .pid = getpid(),
        .flags = desktop_tid ? PS5_DESKTOP_INPUT_SINK : 0,
    };
    int fd = -1, unix_error, tcp_error;

    if (desktop_fd >= 0) return TRUE;
#ifdef __PROSPERO__
    if (ps5_title_desktop)
    {
        if ((fd = __wine_ps5_desktop_connect()) < 0)
        {
            ERR( "Cannot connect to the title desktop hub: %s\n", strerror( errno ) );
            return FALSE;
        }
        /* PsmKit child descriptors may reject F_SETFD; the connected peer
         * remains usable without it. */
        if (fcntl( fd, F_SETFD, FD_CLOEXEC ) < 0)
            WARN( "Cannot set close-on-exec on the title desktop socket: %s\n", strerror( errno ) );
        goto connected;
    }
#endif
    snprintf( addr.sun_path, sizeof(addr.sun_path), "/system_tmp/.wine-%u/%s",
              (unsigned int)getuid(), PS5_DESKTOP_SOCKET );
    if ((fd = socket( AF_UNIX, SOCK_STREAM, 0 )) >= 0 &&
        fcntl( fd, F_SETFD, FD_CLOEXEC ) == 0 &&
        connect( fd, (struct sockaddr *)&addr, sizeof(addr) ) == 0)
        goto connected;
    unix_error = errno;
    if (fd >= 0) close( fd );

    if ((fd = socket( AF_INET, SOCK_STREAM, 0 )) >= 0 &&
        fcntl( fd, F_SETFD, FD_CLOEXEC ) == 0 &&
        connect( fd, (struct sockaddr *)&tcp, sizeof(tcp) ) == 0)
        goto connected;
    tcp_error = errno;
    if (fd >= 0) close( fd );
    ERR( "Cannot connect to %s (%s) or driver TCP port %u (%s)\n", addr.sun_path,
         strerror(unix_error), PS5_DESKTOP_DRIVER_PORT, strerror(tcp_error) );
    return FALSE;

connected:
    if (!write_message( fd, &hello, sizeof(hello) ))
    {
        close( fd );
        return FALSE;
    }
    desktop_fd = fd;
    register_queue();
    return TRUE;
}

BOOL ps5_send_message( const void *message, unsigned int size )
{
    BOOL ret;

    pthread_mutex_lock( &connection_mutex );
    if ((ret = connect_desktop()) && !write_message( desktop_fd, message, size )) connection_lost();
    pthread_mutex_unlock( &connection_mutex );
    return ret;
}

void PS5_SetDesktopWindow( HWND hwnd )
{
    DWORD pid = 0, tid = NtUserGetWindowThread( hwnd, &pid );
    struct ps5_desktop_hello hello =
    {
        .header = { PS5_DESKTOP_HELLO, sizeof(hello) },
        .pid = getpid(),
        .flags = PS5_DESKTOP_INPUT_SINK,
    };

    if (pid != GetCurrentProcessId() || tid != GetCurrentThreadId()) return;
    {
        /* klog names the process input goes to: the hub knows only its pid. */
        WCHAR desktop[64] = {0};
        const UNICODE_STRING *image = &NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;
        NtUserGetObjectInformation( NtUserGetThreadDesktop( tid ), UOI_NAME, desktop, sizeof(desktop) - sizeof(WCHAR), NULL );
        fprintf( stderr, "wineps5.drv: pid %d (%s) takes input for desktop %s\n", (int)getpid(),
                 debugstr_wn( image->Buffer, image->Length / sizeof(WCHAR) ), debugstr_w( desktop ) );
    }
    pthread_mutex_lock( &connection_mutex );
    if (!queue_registered)
    {
        desktop_tid = tid;
        if (desktop_fd < 0) connect_desktop();
        else
        {
            register_queue();
            if (!write_message( desktop_fd, &hello, sizeof(hello) )) connection_lost();
        }
    }
    pthread_mutex_unlock( &connection_mutex );
}

BOOL PS5_CreateWindow( HWND hwnd )
{
    if (hwnd == NtUserGetDesktopWindow())
    {
        PS5_SetDesktopWindow( hwnd );
        if (desktop_tid == GetCurrentThreadId()) return queue_registered;
    }
    return TRUE;
}

LRESULT PS5_DesktopWindowProc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_NCCREATE || msg == WM_CREATE) PS5_SetDesktopWindow( hwnd );
    return NtUserMessageCall( hwnd, msg, wparam, lparam, 0, NtUserDefWindowProc, FALSE );
}

#include "wine/ps5_input.h"

static void send_mouse( const struct ps5_desktop_mouse *mouse )
{
    INPUT input = { .type = INPUT_MOUSE };

    /* Wine 11's hardware-input entry point takes absolute screen pixels. Only
     * NtUserSendInput converts normalized 0..65535 coordinates; using it here
     * would also incorrectly mark the physical device input as injected. */
    input.mi.dx = max( 0, min( mouse->x, PS5_DESKTOP_WIDTH - 1 ) );
    input.mi.dy = max( 0, min( mouse->y, PS5_DESKTOP_HEIGHT - 1 ) );
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    switch (mouse->kind)
    {
    case PS5_DESKTOP_MOUSE_MOVE:
        break;
    case PS5_DESKTOP_MOUSE_BUTTON:
        switch (mouse->button)
        {
        case PS5_DESKTOP_BUTTON_LEFT:
            input.mi.dwFlags |= mouse->down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case PS5_DESKTOP_BUTTON_RIGHT:
            input.mi.dwFlags |= mouse->down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case PS5_DESKTOP_BUTTON_MIDDLE:
            input.mi.dwFlags |= mouse->down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        case PS5_DESKTOP_BUTTON_X1:
        case PS5_DESKTOP_BUTTON_X2:
            input.mi.dwFlags |= mouse->down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            input.mi.mouseData = mouse->button == PS5_DESKTOP_BUTTON_X1 ? XBUTTON1 : XBUTTON2;
            break;
        default:
            return;
        }
        break;
    case PS5_DESKTOP_MOUSE_WHEEL:
        if (mouse->wheel_y)
        {
            input.mi.dwFlags |= MOUSEEVENTF_WHEEL;
            input.mi.mouseData = mouse->wheel_y;
            NtUserSendHardwareInput( 0, SEND_HWMSG_NO_RAW, &input, 0 );
            input.mi.dwFlags &= ~MOUSEEVENTF_WHEEL;
        }
        if (mouse->wheel_x)
        {
            input.mi.dwFlags |= MOUSEEVENTF_HWHEEL;
            input.mi.mouseData = mouse->wheel_x;
        }
        else if (mouse->wheel_y) return;
        break;
    default:
        return;
    }
    NtUserSendHardwareInput( 0, SEND_HWMSG_NO_RAW, &input, 0 );
}


/* Read exactly one framed event. Keep partial frames until the queue fd
 * becomes readable again; never consume the beginning of the next event. */
static int read_event(void)
{
    unsigned int wanted;
    ssize_t ret;

    for (;;)
    {
        wanted = sizeof(input_event.header);
        if (input_size >= wanted)
        {
            wanted = input_event.header.size;
            if (wanted < sizeof(input_event.header) || wanted > sizeof(input_event.bytes)) return -1;
            if (input_size == wanted) return 1;
        }
        ret = recv( desktop_fd, input_event.bytes + input_size, wanted - input_size, MSG_DONTWAIT );
        if (ret > 0) input_size += ret;
        else if (ret < 0 && errno == EINTR) continue;
        else if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        else return -1;
    }
}

static BOOL PS5_ProcessEvents( DWORD mask )
{
    const struct ps5_desktop_text *text = (const void *)input_event.bytes;
    int ret;

    if (GetCurrentThreadId() != desktop_tid || processing_events) return FALSE;
    processing_events = TRUE;
    for (;;)
    {
        pthread_mutex_lock( &connection_mutex );
        if (desktop_fd < 0 && !connect_desktop()) ret = 0;
        else ret = read_event();
        pthread_mutex_unlock( &connection_mutex );
        if (ret < 0) connection_lost();
        if (ret <= 0) break;
        switch (input_event.header.type)
        {
        case PS5_DESKTOP_MOUSE:
            if (input_event.header.size == sizeof(input_event.mouse)) send_mouse( &input_event.mouse );
            else connection_lost();
            break;
        case PS5_DESKTOP_KEY:
            if (input_event.header.size == sizeof(input_event.key)) send_key( &input_event.key );
            else connection_lost();
            break;
        case PS5_DESKTOP_TEXT:
            if (input_event.header.size >= sizeof(*text) && text->length <= PS5_DESKTOP_MAX_TEXT &&
                input_event.header.size == sizeof(*text) + text->length * sizeof(text->text[0]))
                send_text( text );
            else connection_lost();
            break;
        default:
            connection_lost();
            break;
        }
        input_size = 0;
    }
    processing_events = FALSE;
    /* All input is routed through wineserver, which applies the caller's
     * message filter. The transport must be drained regardless of mask. */
    return TRUE;
}

static BOOL PS5_SetCursorPos( INT x, INT y )
{
    struct ps5_desktop_point point =
    {
        .header = { PS5_DESKTOP_CURSOR_POS, sizeof(point) },
        .x = x, .y = y,
    };
    return ps5_send_message( &point, sizeof(point) );
}

/* The GPU name as RADV shows it under the Linux the console boots, where
 * Wine names the GPU after the Vulkan device: libdrm has no amdgpu.ids entry
 * for 1002:13fb, so for this APU it takes the trimmed CPUID brand string
 * (from "Radeon" on if present, "AMD " in front unless it starts with it),
 * and RADV appends its chip name, GFX1013. The PS5 RADV builds it alike. */
static void ps5_gpu_name( char *name, size_t size )
{
    unsigned int brand[13] = {0};
    char *p, *end;
    unsigned int i;

    for (i = 0; i < 3; i++)
        __cpuid( 0x80000002 + i, brand[4 * i], brand[4 * i + 1], brand[4 * i + 2], brand[4 * i + 3] );
    p = (char *)brand;
    while (*p == ' ') p++;
    end = p + strlen( p );
    while (end > p && end[-1] == ' ') *--end = 0;
    if (strstr( p, "Radeon" )) p = strstr( p, "Radeon" );
    snprintf( name, size, "%s%s (RADV GFX1013)", strncmp( p, "AMD", 3 ) ? "AMD " : "", p );
}


#ifdef __PROSPERO__
static DEVMODEW title_modes[8];
static unsigned int title_mode_count;
static pthread_once_t title_modes_once = PTHREAD_ONCE_INIT;

static void init_title_modes(void)
{
    struct wine_ps5_display_mode modes[ARRAY_SIZE(title_modes)];
    unsigned int (*get_modes)( struct wine_ps5_display_mode *, unsigned int ) =
        __wine_ps5_get_vulkan_proc( "wineGetDisplayModes" );
    if (!get_modes) return;
    title_mode_count = get_modes( modes, ARRAY_SIZE(modes) );
    if (title_mode_count > ARRAY_SIZE(title_modes)) title_mode_count = 0;
    for (unsigned int i = 0; i < title_mode_count; i++)
    {
        title_modes[i].dmSize = sizeof(DEVMODEW);
        title_modes[i].dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT |
                                 DM_BITSPERPEL | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
        title_modes[i].dmPelsWidth = modes[i].width;
        title_modes[i].dmPelsHeight = modes[i].height;
        title_modes[i].dmDisplayFrequency = (modes[i].refresh_millihz + 500) / 1000;
        title_modes[i].dmBitsPerPel = 32;
    }
}
#endif
static UINT PS5_UpdateDisplayDevices( const struct gdi_device_manager *manager, void *param )
{
    static const UINT source_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP |
                                     DISPLAY_DEVICE_PRIMARY_DEVICE | DISPLAY_DEVICE_VGA_COMPATIBLE;
    /* The console's own GPU function, 1002:13fb, revision 0 (lspci under
     * Linux on a PS5, where amdgpu drives it as Cyan Skillfish), as RADV
     * reports it too, so DXGI and Vulkan agree. */
    struct pci_id pci_id = { .vendor = 0x1002, .device = 0x13fb };
    struct gdi_monitor monitor =
    {
        .rc_monitor = { 0, 0, PS5_DESKTOP_WIDTH, PS5_DESKTOP_HEIGHT },
        .rc_work = { 0, 0, PS5_DESKTOP_WIDTH, PS5_DESKTOP_HEIGHT },
    };
    const DEVMODEW mode =
    {
        .dmSize = sizeof(mode),
        .dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                    DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY,
        .dmPelsWidth = PS5_DESKTOP_WIDTH, .dmPelsHeight = PS5_DESKTOP_HEIGHT,
        .dmBitsPerPel = 32, .dmDisplayFrequency = 60,
    };
    DEVMODEW current = mode;
    char gpu_name[128];

#ifdef __PROSPERO__
    if (!ps5_title_desktop) pthread_once( &title_modes_once, init_title_modes );
    if (title_mode_count)
    {
        current = title_modes[0];
        monitor.rc_monitor.right = monitor.rc_work.right = current.dmPelsWidth;
        monitor.rc_monitor.bottom = monitor.rc_work.bottom = current.dmPelsHeight;
    }
#endif
    ps5_gpu_name( gpu_name, sizeof(gpu_name) );
    manager->add_gpu( gpu_name, &pci_id, NULL, param );
    manager->add_source( "Default", source_flags, 96, param );
    manager->add_monitor( &monitor, param );
    current.dmFields |= DM_POSITION;
#ifdef __PROSPERO__
    if (title_mode_count) manager->add_modes( &current, title_mode_count, title_modes, param );
    else
#endif
    manager->add_modes( &current, 1, &mode, param );
    return STATUS_SUCCESS;
}

static LONG PS5_ChangeDisplaySettings( DEVMODEW *displays, const WCHAR *primary, HWND hwnd,
                                      DWORD flags, void *param )
{
    DEVMODEW *mode;
    unsigned int count = 0;

    if (!displays) return DISP_CHANGE_SUCCESSFUL;
    for (mode = displays; mode->dmSize; mode = (DEVMODEW *)((char *)(mode + 1) + mode->dmDriverExtra))
    {
#ifdef __PROSPERO__
        if (!ps5_title_desktop) pthread_once( &title_modes_once, init_title_modes );
        if (title_mode_count)
        {
            unsigned int i;
            for (i = 0; i < title_mode_count; i++)
                if (!(mode->dmFields & DM_PELSWIDTH) || mode->dmPelsWidth == title_modes[i].dmPelsWidth)
                    if (!(mode->dmFields & DM_PELSHEIGHT) || mode->dmPelsHeight == title_modes[i].dmPelsHeight)
                        if (!(mode->dmFields & DM_DISPLAYFREQUENCY) || mode->dmDisplayFrequency == title_modes[i].dmDisplayFrequency)
                            break;
            if (i == title_mode_count) return DISP_CHANGE_BADMODE;
        }
        else
#endif
        if (((mode->dmFields & DM_PELSWIDTH) && mode->dmPelsWidth != PS5_DESKTOP_WIDTH) ||
            ((mode->dmFields & DM_PELSHEIGHT) && mode->dmPelsHeight != PS5_DESKTOP_HEIGHT) ||
            ((mode->dmFields & DM_DISPLAYFREQUENCY) && mode->dmDisplayFrequency != 60))
            return DISP_CHANGE_BADMODE;
        if (++count > 1 ||
            ((mode->dmFields & DM_BITSPERPEL) && mode->dmBitsPerPel != 32) ||
            ((mode->dmFields & DM_DISPLAYORIENTATION) && mode->dmDisplayOrientation != DMDO_DEFAULT) ||
            ((mode->dmFields & DM_DISPLAYFLAGS) && mode->dmDisplayFlags) ||
            ((mode->dmFields & DM_POSITION) && (mode->dmPosition.x || mode->dmPosition.y)))
            return DISP_CHANGE_BADMODE;
    }
    return DISP_CHANGE_SUCCESSFUL;
}

/* A text field took or lost the focus: the title may open its on-screen
 * keyboard (it does so only right after a click). */
static void PS5_SetTextInputFocus( HWND hwnd, BOOL focused )
{
    struct ps5_desktop_text_input message =
    {
        .header = { PS5_DESKTOP_TEXT_INPUT, sizeof(message) },
        .active = !!focused,
    };
    ps5_send_message( &message, sizeof(message) );
}

static const struct user_driver_funcs ps5drv_funcs =
{
    .pSetTextInputFocus = PS5_SetTextInputFocus,
    .pSetCursorPos = PS5_SetCursorPos,
    .pChangeDisplaySettings = PS5_ChangeDisplaySettings,
    .pUpdateDisplayDevices = PS5_UpdateDisplayDevices,
    .pCreateWindow = PS5_CreateWindow,
    .pSetDesktopWindow = PS5_SetDesktopWindow,
    .pDesktopWindowProc = PS5_DesktopWindowProc,
    .pDestroyWindow = PS5_DestroyWindow,
    .pProcessEvents = PS5_ProcessEvents,
    .pSetParent = PS5_SetParent,
    .pSetLayeredWindowAttributes = PS5_SetLayeredWindowAttributes,
    .pUpdateLayeredWindow = PS5_UpdateLayeredWindow,
    .pSetWindowStyle = PS5_SetWindowStyle,
    .pShowWindow = PS5_ShowWindow,
    .pWindowPosChanging = PS5_WindowPosChanging,
    .pCreateWindowSurface = PS5_CreateWindowSurface,
    .pGetForeignWindowSurface = PS5_GetForeignWindowSurface,
    .pWindowPosChanged = PS5_WindowPosChanged,
};

#ifdef __PROSPERO__
/* A title without the B3 desktop callbacks uses native Vulkan's display
 * modes and owns no desktop hub or shared-memory compositor. Unimplemented
 * user callbacks retain win32u's defaults rather than attempting the payload
 * desktop transport. */
static const struct user_driver_funcs title_funcs =
{
    .pChangeDisplaySettings = PS5_ChangeDisplaySettings,
    .pUpdateDisplayDevices = PS5_UpdateDisplayDevices,
    .pCreateWindow = PS5_CreateWindow,
};
#endif

static NTSTATUS ps5drv_init( void *arg )
{
#ifdef __PROSPERO__
    if (__wine_ps5_is_title())
    {
        char line[128];

        ps5_title_desktop = __wine_ps5_desktop_available();
        if (ps5_title_desktop)
        {
            sceKernelDebugOutText( 0, "wine: title wineps5.drv desktop transport=runtime\n" );
            __wine_set_user_driver( &ps5drv_funcs, WINE_GDI_DRIVER_VERSION );
            return STATUS_SUCCESS;
        }

        pthread_once( &title_modes_once, init_title_modes );
        snprintf( line, sizeof(line), "wine: title wineps5.drv RADV display modes=%u\n", title_mode_count );
        sceKernelDebugOutText( 0, line );
        if (!title_mode_count) return STATUS_DEVICE_NOT_READY;
        __wine_set_user_driver( &title_funcs, WINE_GDI_DRIVER_VERSION );
        return STATUS_SUCCESS;
    }
#endif

    /* Leave unimplemented callbacks to win32u, including the US keyboard
     * layout, software non-client frames, DIB drawing and cursor tracking. */
    __wine_set_user_driver( &ps5drv_funcs, WINE_GDI_DRIVER_VERSION );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] = { ps5drv_init };
C_ASSERT( ARRAY_SIZE(__wine_unix_call_funcs) == ps5drv_unix_func_count );

#ifdef _WIN64
const unixlib_entry_t __wine_unix_call_wow64_funcs[] = { ps5drv_init };
C_ASSERT( ARRAY_SIZE(__wine_unix_call_wow64_funcs) == ps5drv_unix_func_count );
#endif
