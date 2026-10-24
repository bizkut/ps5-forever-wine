/*
 * Emulator initialisation code
 *
 * Copyright 2000 Alexandre Julliard
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

#include "config.h"

#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <limits.h>
#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif
#ifdef __APPLE__
# include <mach-o/dyld.h>
#endif

#include "main.h"

#ifdef __PROSPERO__
#include <ps5platform/klog.h>
#include <ps5platform/libc.h>
#endif

#if defined(__APPLE__) && defined(__x86_64__) && !defined(HAVE_WINE_PRELOADER)

/* Not using the preloader on x86_64:
 * Reserve the same areas as the preloader does, but using zero-fill sections
 * (the only way to prevent system frameworks from using them, including allocations
 * before main() runs).
 */
__asm__(".zerofill WINE_RESERVE,WINE_RESERVE");
static char __wine_reserve[0x1fffff000] __attribute__((section("WINE_RESERVE, WINE_RESERVE")));

__asm__(".zerofill WINE_TOP_DOWN,WINE_TOP_DOWN");
static char __wine_top_down[0x001ff0000] __attribute__((section("WINE_TOP_DOWN, WINE_TOP_DOWN")));

static const struct wine_preload_info preload_info[] =
{
    { __wine_reserve,  sizeof(__wine_reserve)  }, /*         0x1000 -    0x200000000: low 8GB */
    { __wine_top_down, sizeof(__wine_top_down) }, /* 0x7ff000000000 - 0x7ff001ff0000: top-down allocations + virtual heap */
    { 0, 0 }                                      /* end of list */
};

const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = preload_info;

static void init_reserved_areas(void)
{
    int i;

    for (i = 0; wine_main_preload_info[i].size != 0; i++)
    {
        /* Match how the preloader maps reserved areas: */
        mmap(wine_main_preload_info[i].addr, wine_main_preload_info[i].size, PROT_NONE,
             MAP_FIXED | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1, 0);
    }
}

#else

/* the preloader will set these variables */
__attribute((visibility("default"))) struct r_debug *wine_r_debug = NULL;
const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = NULL;

static void init_reserved_areas(void)
{
}

#endif

/* canonicalize path and return its directory name */
static char *realpath_dirname( const char *name )
{
#ifdef WINEPS5_PUBLIC_TITLE_ONLY
    return wine_loader_realpath_dirname( name, ps5_realpath );
#else
    return wine_loader_realpath_dirname( name, realpath );
#endif
}

/* if string ends with tail, remove it */
static char *remove_tail( const char *str, const char *tail )
{
    size_t len = strlen( str );
    size_t tail_len = strlen( tail );
    char *ret;

    if (len < tail_len) return NULL;
    if (strcmp( str + len - tail_len, tail )) return NULL;
    ret = malloc( len - tail_len + 1 );
    memcpy( ret, str, len - tail_len );
    ret[len - tail_len] = 0;
    return ret;
}

/* build a path from the specified dir and name */
static char *build_path( const char *dir, const char *name )
{
    size_t len = strlen( dir );
    char *ret = malloc( len + strlen( name ) + 2 );

    memcpy( ret, dir, len );
    if (len && ret[len - 1] != '/') ret[len++] = '/';
    strcpy( ret + len, name );
    return ret;
}

static const char *get_self_exe(void)
{
#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__NetBSD__)
    return "/proc/self/exe";
#elif defined (__FreeBSD__) || defined(__DragonFly__)
    static int pathname[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t path_size = PATH_MAX;
    char *path = malloc( path_size );
    if (path && !sysctl( pathname, sizeof(pathname)/sizeof(pathname[0]), path, &path_size, NULL, 0 ))
        return path;
    free( path );
#elif defined(__APPLE__)
    uint32_t path_size = PATH_MAX;
    char *path = malloc( path_size );
    if (path && !_NSGetExecutablePath( path, &path_size ))
        return path;
    free( path );
#endif
    return NULL;
}

#ifdef __PROSPERO__
/* The console's loader gives every library whose DT_NEEDED names ntdll.so
 * a copy of its own, unless ntdll.so is already loaded with RTLD_GLOBAL
 * (ps5/probes/needed): win32u.so registered its system calls in such a copy. */
#define NTDLL_DLOPEN_FLAGS (RTLD_NOW | RTLD_GLOBAL)
#else
#define NTDLL_DLOPEN_FLAGS RTLD_NOW
#endif

static void *try_dlopen( const char *argv0 )
{
    char *dir, *path, *p;
    void *handle;

    if (!argv0) return NULL;

    if ((p = remove_tail( argv0, "i386-unix/wine64")))
    {
        path = build_path( p, "x86_64-unix/ntdll.so" );
        free( p );
        handle = dlopen( path, NTDLL_DLOPEN_FLAGS );
        free( path );
        return handle;
    }

    if (!(dir = realpath_dirname( argv0 )))
    {
#ifdef WINEPS5_PUBLIC_TITLE_ONLY
        int saved_errno = errno;
        char line[640];
        snprintf( line, sizeof(line), "wine: public loader path resolution failed errno=%d path=%.512s\n",
                  saved_errno, argv0 );
        sceKernelDebugOutText( 0, line );
        errno = saved_errno;
#endif
        return NULL;
    }

    if ((p = remove_tail( dir, "/loader" )))
        path = build_path( p, "dlls/ntdll/ntdll.so" );
    else
        path = build_path( dir, "ntdll.so" );

#ifdef __PROSPERO__
    /* the console's dladdr reports nothing, so ntdll learns its path here */
    setenv( "WINE_NTDLL_PATH", path, 1 );
#endif
    handle = dlopen( path, NTDLL_DLOPEN_FLAGS );
    free( p );
    free( dir );
    free( path );
    return handle;
}

#ifdef __PROSPERO__

#include <stdint.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <dirent.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ps5/kernel.h>
#include <ps5/rtld.h>
#include "wine/ps5_spawn.h"
#include "wine/ps5_desktop.h"
#include "wine/ps5_title.h"
#include "wine/ps5_title_private.h"

extern char **environ;

/* The first process is started by the console's ELF loader, which passes no
 * arguments and no environment. They come from this file, one per line:
 * "arg <argument>" (the first is the loader's own path, which locates
 * ntdll.so), "env NAME=VALUE" and "cwd <directory>". PS5_WINE_ROOT, where
 * Wine is installed on the console, comes from ps5/paths.sh through the
 * header ps5/build-wine.sh force-includes. */
#ifndef PS5_WINE_ROOT
#error "PS5_WINE_ROOT is not defined: build with ps5/build-wine.sh"
#endif
#define PS5_BOOT_FILE PS5_WINE_ROOT "/boot.txt"
#define PS5_TITLE_BOOT_FILE "/app0/wine/boot.txt"

static void ps5_read_boot_file( const char *path, int *argc, char ***argv )
{
    char line[4096], **args;
    int count = 0, size = 16;
    FILE *f;

    if (*argc > 1 || !(f = fopen( path, "r" ))) return;
    args = malloc( size * sizeof(*args) );
    while (fgets( line, sizeof(line), f ))
    {
        line[strcspn( line, "\r\n" )] = 0;
        if (!strncmp( line, "arg ", 4 ))
        {
            if (count + 2 > size) args = realloc( args, (size *= 2) * sizeof(*args) );
            args[count++] = strdup( line + 4 );
        }
        else if (!strncmp( line, "env ", 4 )) putenv( strdup( line + 4 ) );
        else if (!strncmp( line, "cwd ", 4 )) chdir( line + 4 );
    }
    fclose( f );
    args[count] = NULL;
    if (count)
    {
        *argc = count;
        *argv = args;
    }
}

static void ps5_read_boot( int *argc, char ***argv )
{
    ps5_read_boot_file( PS5_BOOT_FILE, argc, argv );
}

/* Send the loader at path to the console's ELF loader, which starts it as a
 * new process with no arguments. Returns the connection, which must stay
 * open while the ELF loader starts the process (it streams the process's
 * output back on it), or -errno. */
static int ps5_send_loader( const char *path )
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons( 9021 ) };
    struct stat st;
    char *elf;
    int fd, sock, ret;

    if ((fd = open( path, O_RDONLY )) < 0) return -errno;
    if (fstat( fd, &st ) || !(elf = malloc( st.st_size )) || ps5_spawn_read_all( fd, elf, st.st_size ))
    {
        close( fd );
        return -EIO;
    }
    close( fd );
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    if ((sock = socket( AF_INET, SOCK_STREAM, 0 )) < 0) ret = -errno;
    else if (connect( sock, (struct sockaddr *)&addr, sizeof(addr) ) ||
             ps5_spawn_write_all( sock, elf, st.st_size ) || shutdown( sock, SHUT_WR ))
    {
        ret = -errno;
        close( sock );
    }
    else ret = sock;
    free( elf );
    return ret;
}

struct ps5_pending
{
    struct ps5_spawn_message msg;
    char *blob;
    int fds[PS5_SPAWN_MAX_FDS + 1];
    int nfds;
    struct ps5_pending *next;
};

static void ps5_free_pending( struct ps5_pending *req )
{
    int i;
    for (i = 0; i < req->nfds; i++) close( req->fds[i] );
    free( req->blob );
    free( req );
}

/* A started loader that has not pulled its request after this long is given
 * up on, and its request dropped. */
#define PS5_LAUNCH_TIMEOUT 20

/* The broker: queue requests and start a loader for each, one at a time. A
 * process the console's ELF loader is still starting was found stopped for
 * good, unkillable, when the next launch overlapped it (2026-10-01): so the
 * next loader is sent only once the previous one has pulled its request,
 * and the ELF loader's connection stays open until then. The broker itself
 * is a fresh loader process (ps5_start_helper), never a fork of Wine. */
static void __attribute__((noreturn)) ps5_broker_run( int listener )
{
    struct ps5_pending *head = NULL, **tail = &head, *req;
    struct ps5_spawn_message msg;
    int fds[PS5_SPAWN_MAX_FDS + 1], nfds, conn, i, launch = -1;
    time_t launched = 0;
    struct pollfd pfd = { .fd = listener, .events = POLLIN };
    char *blob;
    int32_t reply;

    for (;;)
    {
        /* start the oldest request's loader when none is starting */
        while (head && launch < 0)
        {
            if ((launch = ps5_send_loader( head->blob )) >= 0)  /* the blob starts with the loader's path */
            {
                launched = time( NULL );
                break;
            }
            req = head;
            if (!(head = req->next)) tail = &head;
            ps5_free_pending( req );
        }
        if (launch >= 0 && time( NULL ) - launched > PS5_LAUNCH_TIMEOUT)
        {
            close( launch );
            launch = -1;
            req = head;
            if (!(head = req->next)) tail = &head;
            ps5_free_pending( req );
            continue;
        }
        if (poll( &pfd, 1, 1000 ) <= 0) continue;
        if ((conn = accept( listener, NULL, NULL )) < 0) continue;
        if (ps5_spawn_recv( conn, &msg, &blob, fds, &nfds ))
        {
            for (i = 0; i < nfds; i++) close( fds[i] );
            free( blob );
        }
        else if (msg.op == PS5_SPAWN_REQUEST && nfds == msg.nfds && nfds >= 1)
        {
            req = calloc( 1, sizeof(*req) );
            req->msg = msg;
            req->blob = blob;
            req->nfds = nfds;
            memcpy( req->fds, fds, nfds * sizeof(int) );
            *tail = req;
            tail = &req->next;
            reply = 0;  /* queued; started in order */
            ps5_spawn_write_all( conn, &reply, sizeof(reply) );
        }
        else if (msg.op == PS5_SPAWN_PULL)
        {
            free( blob );
            if (launch >= 0 && (req = head))
            {
                if (!(head = req->next)) tail = &head;
                ps5_spawn_send( conn, &req->msg, req->blob, req->fds, req->nfds );
                ps5_free_pending( req );
                close( launch );  /* the new process runs: the next one may start */
                launch = -1;
            }
            else
            {
                memset( &msg, 0, sizeof(msg) );
                msg.op = PS5_SPAWN_PULL;
                ps5_spawn_send( conn, &msg, "", NULL, 0 );
            }
        }
        else
        {
            for (i = 0; i < nfds; i++) close( fds[i] );
            free( blob );
        }
        close( conn );
    }
}

/* The broker and the desktop hub are loaders too, started through the
 * console's ELF loader as every Wine process is. Forking a running Wine
 * process brought the kernel down (2026-10-02), and the console still trapped
 * at the end of sessions whose two helpers had been forked (2026-10-03). The
 * starter leaves a claim file and sends the loader; the new process takes the
 * role by removing the file, before it would read boot.txt. */
static void ps5_claim_path( char *buf, size_t size, const char *role )
{
    snprintf( buf, size, "/system_tmp/.wine-%u/%s.claim", (unsigned int)getuid(), role );
}

static int ps5_claim( const char *role )
{
    char path[96];

    ps5_claim_path( path, sizeof(path), role );
    return !unlink( path );
}

/* Start a helper unless running() finds it, and wait up to 5 s for it: no
 * other launch may overlap its start (see the broker). */
static void ps5_start_helper( const char *role, const char *loader, int (*running)(void) )
{
    char dir[64], path[96];
    struct stat st;
    int fd, launch = -1, created = 0, i;

    if (running()) return;
    snprintf( dir, sizeof(dir), "/system_tmp/.wine-%u", (unsigned int)getuid() );
    mkdir( dir, 0700 );
    ps5_claim_path( path, sizeof(path), role );
    /* a claim nobody took for 10 s is left from a start that failed */
    if (!stat( path, &st ) && time( NULL ) - st.st_mtime > 10) unlink( path );
    if ((fd = open( path, O_WRONLY | O_CREAT | O_EXCL, 0600 )) >= 0)  /* else another loader starts it */
    {
        close( fd );
        created = 1;
        if ((launch = ps5_send_loader( loader )) < 0)
        {
            fprintf( stderr, "wine: could not start the %s: %s\n", role, strerror( -launch ));
            unlink( path );
            return;
        }
    }
    for (i = 0; i < 500 && !running(); i++) poll( NULL, 0, 10 );
    if (launch >= 0) close( launch );
    if (!running())
    {
        fprintf( stderr, "wine: the %s did not start\n", role );
        if (created) unlink( path );
    }
}

/* A helper outlives the session's processes: it must not keep the ELF
 * loader's connection open as its standard descriptors, or whoever started
 * Wine never sees the output end. */
static void ps5_null_stdio(void)
{
    int null_fd = open( "/dev/null", O_RDWR );

    if (null_fd >= 0)
    {
        dup2( null_fd, 0 );
        dup2( null_fd, 1 );
        dup2( null_fd, 2 );
        if (null_fd > 2) close( null_fd );
    }
    else
    {
        close( 0 );
        close( 1 );
        close( 2 );
    }
}

static int ps5_broker_running(void)
{
    int conn = ps5_spawn_connect();

    if (conn < 0) return 0;
    close( conn );
    return 1;
}

static void __attribute__((noreturn)) ps5_broker_main(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int listener;

    setsid();
    signal( SIGPIPE, SIG_IGN );
    ps5_spawn_path( addr.sun_path, sizeof(addr.sun_path) );
    unlink( addr.sun_path );
    if ((listener = socket( AF_UNIX, SOCK_STREAM, 0 )) < 0 ||
        bind( listener, (struct sockaddr *)&addr, sizeof(addr) ) < 0 || listen( listener, 16 ) < 0)
    {
        fprintf( stderr, "wine: no spawn broker at %s: %s\n", addr.sun_path, strerror( errno ));
        _exit(1);
    }
    ps5_note_pid();
    ps5_note_helper_pid();
    ps5_null_stdio();
    ps5_broker_run( listener );
}

/* The title can only reach a payload over TCP. The desktop hub owns no Wine
 * state and loads no libraries; like the broker it is a loader started
 * through the ELF loader (ps5_start_helper). */
#define PS5_HUB_MAX_FRAME PS5_DESKTOP_MAX_MESSAGE
#define PS5_HUB_MAX_OUTPUT (1024 * 1024)

struct ps5_hub_peer
{
    struct ps5_hub_peer *next;
    int fd, is_title, hello;
    unsigned int sink_order;    /* when it last became the input sink; 0: it is not one */
    uint32_t pid;
    unsigned int poll_index, input_size;
    union
    {
        struct ps5_desktop_header header;
        struct ps5_desktop_hello hello;
        struct ps5_desktop_window window;
        struct ps5_desktop_id id;
        uint64_t alignment;
        unsigned char bytes[PS5_HUB_MAX_FRAME];
    } input;
    char *output;
    size_t output_begin, output_end, output_capacity;
};

struct ps5_hub_window
{
    struct ps5_hub_window *above, *below;
    struct ps5_hub_peer *owner;
    struct ps5_desktop_window state;
    struct ps5_desktop_cursor_shape *cursor;
    int replay_cursor, replay_cursor_changed;
};

struct ps5_desktop_hub
{
    struct ps5_hub_peer *peers, *title, *input_sink;
    unsigned int sink_serial;
    int input_dropped;          /* "input dropped" was logged since the sink went */
    struct ps5_hub_window *top, *bottom;
    struct ps5_desktop_cursor_shape *desktop_cursor;
    int replay_cursors, replay_desktop_cursor, replay_desktop_changed;
    int session;                /* wineserver running: 1, exited: 0, not checked yet: -1 */
    time_t session_checked;
    int title_seen;             /* a title connected since the hub started */
    time_t title_left;          /* when the last title disconnected, 0 while one is connected */
};

static void ps5_hub_drop_peer( struct ps5_hub_peer *peer )
{
    if (peer->fd >= 0) close( peer->fd );
    peer->fd = -1;
}

/* A slow title must never stop the drivers, nor may a slow input sink stop
 * the title. Bound pending bytes and disconnect a peer that cannot keep up;
 * the title can reconnect and get a fresh snapshot of the window cache. */
static int ps5_hub_queue( struct ps5_hub_peer *peer, const void *data, size_t size )
{
    size_t pending, capacity;
    char *output;

    if (!peer) return 1;
    if (peer->fd < 0) return 0;
    pending = peer->output_end - peer->output_begin;
    if (pending + size > PS5_HUB_MAX_OUTPUT) goto failed;
    if (peer->output_capacity - peer->output_end < size)
    {
        if (pending + size > peer->output_capacity)
        {
            capacity = peer->output_capacity ? peer->output_capacity : 4096;
            while (capacity < pending + size) capacity *= 2;
            if (!(output = realloc( peer->output, capacity ))) goto failed;
            peer->output = output;
            peer->output_capacity = capacity;
        }
        if (pending) memmove( peer->output, peer->output + peer->output_begin, pending );
        peer->output_begin = 0;
        peer->output_end = pending;
    }
    memcpy( peer->output + peer->output_end, data, size );
    peer->output_end += size;
    return 1;

failed:
    ps5_hub_drop_peer( peer );
    return 0;
}

static void ps5_hub_flush( struct ps5_hub_peer *peer )
{
    size_t size, sent = 0;
    ssize_t ret;

    while (peer->output_begin < peer->output_end && sent < 65536)
    {
        size = peer->output_end - peer->output_begin;
        if (size > 65536 - sent) size = 65536 - sent;
        ret = send( peer->fd, peer->output + peer->output_begin, size, MSG_NOSIGNAL );
        if (ret > 0)
        {
            peer->output_begin += ret;
            sent += ret;
        }
        else if (ret < 0 && errno == EINTR) continue;
        else if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        else
        {
            ps5_hub_drop_peer( peer );
            return;
        }
    }
    if (peer->output_begin == peer->output_end) peer->output_begin = peer->output_end = 0;
}

static struct ps5_hub_window *ps5_hub_find_window( struct ps5_desktop_hub *hub, uint64_t id )
{
    struct ps5_hub_window *window;

    for (window = hub->top; window; window = window->below)
        if (window->state.id == id) return window;
    return NULL;
}

static void ps5_hub_unlink_window( struct ps5_desktop_hub *hub, struct ps5_hub_window *window )
{
    if (window->above) window->above->below = window->below;
    else if (hub->top == window) hub->top = window->below;
    if (window->below) window->below->above = window->above;
    else if (hub->bottom == window) hub->bottom = window->above;
    window->above = window->below = NULL;
}

static void ps5_hub_remove_window( struct ps5_desktop_hub *hub, struct ps5_hub_window *window )
{
    ps5_hub_unlink_window( hub, window );
    if (window->state.shm[0]) shm_unlink( window->state.shm );
    free( window->cursor );
    free( window );
}

/* Maintain top-to-bottom order, with the topmost group always first. WINDOW
 * without ZORDER updates its pixels/geometry but must not raise the window. */
static void ps5_hub_restack( struct ps5_desktop_hub *hub, struct ps5_hub_window *window, uint64_t after )
{
    struct ps5_hub_window *other, *position = NULL, *last_topmost = NULL;
    int topmost = !!(window->state.flags & PS5_DESKTOP_TOPMOST);

    ps5_hub_unlink_window( hub, window );
    for (other = hub->top; other; other = other->below)
        if (other->state.flags & PS5_DESKTOP_TOPMOST) last_topmost = other;
    if (!topmost) position = last_topmost; /* top of the normal group */
    if (after == PS5_DESKTOP_Z_BOTTOM)
    {
        for (other = hub->top; other; other = other->below)
            if (!!(other->state.flags & PS5_DESKTOP_TOPMOST) == topmost) position = other;
    }
    else if (after != PS5_DESKTOP_Z_TOP && (other = ps5_hub_find_window( hub, after )))
    {
        if (!!(other->state.flags & PS5_DESKTOP_TOPMOST) == topmost) position = other;
        else position = last_topmost;
    }
    window->above = position;
    window->below = position ? position->below : hub->top;
    if (window->below) window->below->above = window;
    else hub->bottom = window;
    if (position) position->below = window;
    else hub->top = window;
}

static void ps5_hub_replay_cursors( struct ps5_desktop_hub *hub )
{
    struct ps5_hub_window *window;
    size_t budget = 65536, pending;
    if (!hub->replay_cursors) return;
    if (!hub->title || hub->title->fd < 0) { hub->replay_cursors = 0; return; }
    for (window = hub->top; window; window = window->below)
    {
        size_t size;
        if (!window->replay_cursor) continue;
        if (!window->cursor) { window->replay_cursor = 0; continue; }
        size = window->cursor->header.size;
        pending = hub->title->output_end - hub->title->output_begin;
        if (size > budget || pending + size + 65536 > PS5_HUB_MAX_OUTPUT) return;
        if (!ps5_hub_queue( hub->title, window->cursor, size )) return;
        window->replay_cursor = 0;
        budget -= size;
    }
    if (hub->replay_desktop_cursor && hub->desktop_cursor)
    {
        size_t size = hub->desktop_cursor->header.size;
        pending = hub->title->output_end - hub->title->output_begin;
        if (size > budget || pending + size + 65536 > PS5_HUB_MAX_OUTPUT) return;
        if (!ps5_hub_queue( hub->title, hub->desktop_cursor, size )) return;
    }
    hub->replay_desktop_cursor = hub->replay_desktop_changed;
    hub->replay_desktop_changed = 0;
    hub->replay_cursors = hub->replay_desktop_cursor;
    for (window = hub->top; window; window = window->below)
    {
        window->replay_cursor = window->replay_cursor_changed;
        window->replay_cursor_changed = 0;
        hub->replay_cursors |= window->replay_cursor;
    }
}

static void ps5_hub_replay( struct ps5_desktop_hub *hub )
{
    struct ps5_hub_window *window;
    struct ps5_desktop_window state;
    struct ps5_desktop_damage damage = { .header = { PS5_DESKTOP_DAMAGE, sizeof(damage) } };
    uint64_t previous = PS5_DESKTOP_Z_TOP;

    hub->replay_cursors = 1;
    hub->replay_desktop_cursor = hub->desktop_cursor != NULL;
    hub->replay_desktop_changed = 0;
    for (window = hub->top; window; window = window->below)
    {
        window->replay_cursor = window->cursor != NULL;
        window->replay_cursor_changed = 0;
    }
    for (window = hub->top; window; window = window->below)
    {
        state = window->state;
        state.flags |= PS5_DESKTOP_ZORDER;
        state.insert_after = previous;
        if (!ps5_hub_queue( hub->title, &state, sizeof(state) )) return;
        previous = state.id;
    }
    for (window = hub->top; window; window = window->below)
    {
        damage.id = window->state.id;
        damage.left = damage.top = 0;
        damage.right = window->state.width;
        damage.bottom = window->state.height;
        if (!ps5_hub_queue( hub->title, &damage, sizeof(damage) )) return;
    }
    ps5_hub_replay_cursors( hub );
}

/* The input sink after the current one stops being one: the connected peer
 * that became one most recently. A desktop process that comes and goes (a
 * program started on a desktop of its own) must not leave input with no one. */
static struct ps5_hub_peer *ps5_hub_previous_sink( struct ps5_desktop_hub *hub )
{
    struct ps5_hub_peer *peer, *best = NULL;

    for (peer = hub->peers; peer; peer = peer->next)
        if (peer->fd >= 0 && peer->sink_order && (!best || peer->sink_order > best->sink_order)) best = peer;
    return best;
}

/* Input goes to one desktop process; klog says which, as nothing else shows
 * where clicks go. */
static void ps5_hub_set_sink( struct ps5_desktop_hub *hub, struct ps5_hub_peer *peer )
{
    if (hub->input_sink == peer) return;
    hub->input_sink = peer;
    if (peer)
    {
        hub->input_dropped = 0;
        fprintf( stderr, "wine: desktop hub: input goes to pid %u\n", (unsigned int)peer->pid );
    }
    else fprintf( stderr, "wine: desktop hub: no desktop process takes input\n" );
}

static int ps5_hub_driver_message( struct ps5_desktop_hub *hub, struct ps5_hub_peer *peer )
{
    const struct ps5_desktop_header *header = &peer->input.header;
    const struct ps5_desktop_window *state = &peer->input.window;
    const struct ps5_desktop_hello *hello = &peer->input.hello;
    struct ps5_hub_window *window;
    int restack;

    if (header->type == PS5_DESKTOP_HELLO)
    {
        if (header->size != sizeof(*hello) || !hello->pid || (peer->hello && peer->pid != hello->pid)) return 0;
        peer->pid = hello->pid;
        peer->hello = 1;
        if (hello->flags & PS5_DESKTOP_INPUT_SINK)
        {
            peer->sink_order = ++hub->sink_serial;
            ps5_hub_set_sink( hub, peer );
        }
        else
        {
            peer->sink_order = 0;
            if (hub->input_sink == peer) ps5_hub_set_sink( hub, ps5_hub_previous_sink( hub ) );
        }
        if (hello->flags & PS5_DESKTOP_CURSOR_CAPABLE)
        {
            struct ps5_desktop_hello reply = {
                .header = {PS5_DESKTOP_HELLO, sizeof(reply)}, .flags = PS5_DESKTOP_CURSOR_CAPABLE};
            ps5_hub_queue( peer, &reply, sizeof(reply) );
        }
        return 1;
    }
    if (!peer->hello) return 0;
    switch (header->type)
    {
    case PS5_DESKTOP_WINDOW:
        if (header->size != sizeof(*state) || state->id <= PS5_DESKTOP_Z_BOTTOM ||
            !memchr( state->shm, 0, sizeof(state->shm) ) ||
            (state->shm[0] && strncmp( state->shm, "/wine-ps5-", 10 ))) return 0;
        window = ps5_hub_find_window( hub, state->id );
        restack = !window || (state->flags & PS5_DESKTOP_ZORDER);
        if (!window)
        {
            if (!(window = calloc( 1, sizeof(*window) ))) return 0;
        }
        else
        {
            restack |= !!((window->state.flags ^ state->flags) & PS5_DESKTOP_TOPMOST);
            if (strcmp( window->state.shm, state->shm ) && window->state.shm[0])
                shm_unlink( window->state.shm );
        }
        window->state = *state;
        window->owner = peer;
        if (restack) ps5_hub_restack( hub, window, state->insert_after );
        ps5_hub_queue( hub->title, header, header->size );
        return 1;
    case PS5_DESKTOP_DESTROY:
        if (header->size != sizeof(struct ps5_desktop_id)) return 0;
        if ((window = ps5_hub_find_window( hub, peer->input.id.id )) && window->owner == peer)
        {
            ps5_hub_queue( hub->title, header, header->size );
            ps5_hub_remove_window( hub, window );
        }
        return 1;
    case PS5_DESKTOP_DAMAGE:
        /* From any process: one may draw into another's window (wineps5.drv's
         * foreign surfaces). Damage only asks the title to redraw a window it
         * knows; changing or destroying the window stays its owner's. */
        if (header->size != sizeof(struct ps5_desktop_damage)) return 0;
        if (ps5_hub_find_window( hub, peer->input.id.id ))
            ps5_hub_queue( hub->title, header, header->size );
        return 1;
    case PS5_DESKTOP_CURSOR_POS:
        if (header->size != sizeof(struct ps5_desktop_point)) return 0;
        ps5_hub_queue( hub->title, header, header->size );
        return 1;
    case PS5_DESKTOP_CURSOR_SHAPE:
    {
        const struct ps5_desktop_cursor_shape *shape = (const void *)header;
        struct ps5_desktop_cursor_shape **saved;
        if (!ps5_desktop_cursor_shape_valid( header, header->size )) return 0;
        window = NULL;
        if (shape->id)
        {
            if (!(window = ps5_hub_find_window( hub, shape->id ))) return 1;
            saved = &window->cursor;
        }
        else saved = &hub->desktop_cursor;
        if (*saved && (*saved)->header.size == header->size &&
            !memcmp( *saved, header, header->size )) return 1;
        if (!*saved && !(*saved = malloc( PS5_DESKTOP_MAX_MESSAGE ))) return 0;
        memcpy( *saved, header, header->size );
        if (hub->replay_cursors)
        {
            if (window)
            {
                if (!window->replay_cursor) window->replay_cursor_changed = 1;
            }
            else if (!hub->replay_desktop_cursor) hub->replay_desktop_changed = 1;
            return 1;
        }
        ps5_hub_queue( hub->title, header, header->size );
        return 1;
    }
    case PS5_DESKTOP_TEXT_INPUT:
        if (header->size != sizeof(struct ps5_desktop_text_input)) return 0;
        ps5_hub_queue( hub->title, header, header->size );
        return 1;
    default:
        return 0;
    }
}

/* The title's art (icon0.png, pic0.dds, pic1.dds), written by the title into
 * its own sce_sys/, copied over the copies its registration left where the
 * home screen reads them, which only a payload can reach. Only these files
 * of the title this Wine tree belongs to, each only if the title has it,
 * only into folders that already exist, never through a link. */
#define PS5_ART_MAX (16 * 1024 * 1024)

static int ps5_art_write( const char *dir, const char *name, const unsigned char *data, size_t size )
{
    char temp[PATH_MAX], path[PATH_MAX];
    struct stat st;
    size_t done = 0;
    ssize_t ret;
    int fd, error = 0;

    if (lstat( dir, &st ) < 0) return errno;
    if (!S_ISDIR( st.st_mode )) return ENOTDIR;
    snprintf( temp, sizeof(temp), "%s/.%s.new", dir, name );
    snprintf( path, sizeof(path), "%s/%s", dir, name );
    if ((fd = open( temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0666 )) < 0) return errno;
    while (done < size)
    {
        if ((ret = write( fd, data + done, size - done )) < 0)
        {
            if (errno == EINTR) continue;
            error = errno;
            break;
        }
        done += ret;
    }
    if (!error && fchmod( fd, 0666 ) < 0) error = errno;
    if (close( fd ) < 0 && !error) error = errno;
    if (!error && rename( temp, path ) < 0) error = errno;
    if (error) unlink( temp );
    return error;
}

/* One file of the title's sce_sys/: 0 and *data when it is there and has the
 * expected start, ENOENT when the title has none, another errno otherwise */
static int ps5_art_read( const char *path, const void *magic, size_t magic_size,
                         unsigned char **data, size_t *size )
{
    struct stat st;
    ssize_t ret;
    int fd, error = 0;

    *data = NULL;
    *size = 0;
    if ((fd = open( path, O_RDONLY | O_NOFOLLOW )) < 0) return errno;
    if (fstat( fd, &st ) < 0) error = errno;
    else if (!S_ISREG( st.st_mode ) || st.st_size < (off_t)magic_size || st.st_size > PS5_ART_MAX) error = EINVAL;
    else if (!(*data = malloc( st.st_size ))) error = ENOMEM;
    while (!error && *size < (size_t)st.st_size)
    {
        if ((ret = read( fd, *data + *size, st.st_size - *size )) <= 0)
        {
            if (ret < 0 && errno == EINTR) continue;
            error = ret < 0 ? errno : EIO;
        }
        else *size += ret;
    }
    close( fd );
    if (!error && memcmp( *data, magic, magic_size )) error = EINVAL;
    if (error)
    {
        free( *data );
        *data = NULL;
    }
    return error;
}

static void ps5_hub_install_art( struct ps5_desktop_hub *hub )
{
    static const unsigned char png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    static const struct
    {
        const char *name;
        const void *magic;
        size_t magic_size;
        unsigned int places;  /* 1: /user/app/<id>, 2: its sce_sys, 4: /user/appmeta/<id> */
    } files[] =
    {
        { "icon0.png", png, sizeof(png), 1 | 2 | 4 },
        { "pic0.dds", "DDS ", 4, 2 | 4 },
        { "pic1.dds", "DDS ", 4, 2 | 4 },
    };
    struct ps5_desktop_art answer = { { PS5_DESKTOP_ART, sizeof(answer) } };
    char title[PATH_MAX], source[PATH_MAX], dirs[3][PATH_MAX];
    const char *id, *slash;
    unsigned char *data;
    size_t size;
    unsigned int i, j;
    int error;

    /* PS5_WINE_ROOT is <title folder>/wine; the folder's name is the title id */
    snprintf( title, sizeof(title), "%s", PS5_WINE_ROOT );
    if (!(slash = strrchr( title, '/' )) || slash == title)
    {
        answer.result = EINVAL;
        goto done;
    }
    title[slash - title] = 0;
    id = strrchr( title, '/' ) + 1;
    snprintf( dirs[0], sizeof(dirs[0]), "/user/app/%s", id );
    snprintf( dirs[1], sizeof(dirs[1]), "/user/app/%s/sce_sys", id );
    snprintf( dirs[2], sizeof(dirs[2]), "/user/appmeta/%s", id );

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        snprintf( source, sizeof(source), "%s/sce_sys/%s", title, files[i].name );
        if ((error = ps5_art_read( source, files[i].magic, files[i].magic_size, &data, &size )))
        {
            if (error != ENOENT && !answer.result) answer.result = error;
            continue;
        }
        for (j = 0; j < 3; j++)
        {
            if (!(files[i].places & (1u << j))) continue;
            if (!(error = ps5_art_write( dirs[j], files[i].name, data, size ))) answer.copied++;
            else if (!answer.result) answer.result = error;
        }
        free( data );
    }

done:
    ps5_hub_queue( hub->title, &answer, sizeof(answer) );
}

static int ps5_hub_title_message( struct ps5_desktop_hub *hub, struct ps5_hub_peer *peer )
{
    const struct ps5_desktop_header *header = &peer->input.header;
    const struct ps5_desktop_text *text = (const void *)peer->input.bytes;

    switch (header->type)
    {
    case PS5_DESKTOP_MOUSE:
        if (header->size != sizeof(struct ps5_desktop_mouse)) return 0;
        break;
    case PS5_DESKTOP_KEY:
        if (header->size != sizeof(struct ps5_desktop_key)) return 0;
        break;
    case PS5_DESKTOP_TEXT:
        if (header->size < sizeof(*text) || text->length > PS5_DESKTOP_MAX_TEXT ||
            header->size != sizeof(*text) + text->length * sizeof(text->text[0])) return 0;
        break;
    case PS5_DESKTOP_VOICE_EDIT:
        if (!ps5_desktop_voice_edit_valid( header, header->size )) return 0;
        break;
    case PS5_DESKTOP_ART:
        if (header->size != sizeof(struct ps5_desktop_art)) return 0;
        ps5_hub_install_art( hub );
        return 1;
    default:
        return 0;
    }
    if (!hub->input_sink && !hub->input_dropped)
    {
        fprintf( stderr, "wine: desktop hub: input dropped: no desktop process takes it\n" );
        hub->input_dropped = 1;
    }
    ps5_hub_queue( hub->input_sink, header, header->size );
    return 1;
}

static void ps5_hub_read( struct ps5_desktop_hub *hub, struct ps5_hub_peer *peer )
{
    unsigned int wanted, count = 0;
    ssize_t ret;

    /* Limit work per poll turn as well as queued bytes: a busy driver must
     * not starve the title or another process's windows. */
    while (count < 64)
    {
        wanted = sizeof(peer->input.header);
        if (peer->input_size >= wanted)
        {
            wanted = peer->input.header.size;
            if (wanted < sizeof(peer->input.header) || wanted > sizeof(peer->input.bytes)) break;
            if (peer->input_size == wanted)
            {
                if (!(peer->is_title ? ps5_hub_title_message( hub, peer ) : ps5_hub_driver_message( hub, peer ))) break;
                peer->input_size = 0;
                count++;
                continue;
            }
        }
        ret = recv( peer->fd, peer->input.bytes + peer->input_size, wanted - peer->input_size, 0 );
        if (ret > 0) peer->input_size += ret;
        else if (ret < 0 && errno == EINTR) continue;
        else if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        else break;
    }
    if (count < 64)
    {
        if (peer->hello && peer->input_size >= sizeof(peer->input.header))
            fprintf( stderr, "wine: desktop hub: pid %u: unexpected message type %u size %u\n",
                     (unsigned int)peer->pid, peer->input.header.type, peer->input.header.size );
        else if (peer->hello)
            fprintf( stderr, "wine: desktop hub: pid %u disconnected\n", (unsigned int)peer->pid );
        ps5_hub_drop_peer( peer );
    }
}

static void ps5_hub_cleanup_peers( struct ps5_desktop_hub *hub )
{
    struct ps5_desktop_id destroy = { .header = { PS5_DESKTOP_DESTROY, sizeof(destroy) } };
    struct ps5_hub_peer **ptr, *peer;
    struct ps5_hub_window *window, *next;
    int removed;

    do
    {
        removed = 0;
        for (ptr = &hub->peers; (peer = *ptr); )
        {
            if (peer->fd >= 0)
            {
                ptr = &peer->next;
                continue;
            }
            if (hub->title == peer) hub->title = NULL;
            if (hub->input_sink == peer) ps5_hub_set_sink( hub, ps5_hub_previous_sink( hub ) );
            for (window = hub->top; window; window = next)
            {
                next = window->below;
                if (window->owner != peer) continue;
                destroy.id = window->state.id;
                ps5_hub_queue( hub->title, &destroy, sizeof(destroy) );
                ps5_hub_remove_window( hub, window );
            }
            *ptr = peer->next;
            free( peer->output );
            free( peer );
            removed = 1;
        }
    } while (removed); /* cleanup may have disconnected a backlogged title */
}

static int ps5_desktop_nonblock( int fd )
{
    int flags = fcntl( fd, F_GETFL );
    return flags < 0 || fcntl( fd, F_SETFL, flags | O_NONBLOCK ) < 0 || fcntl( fd, F_SETFD, FD_CLOEXEC ) < 0;
}

static void ps5_hub_send_session( struct ps5_desktop_hub *hub );

static void ps5_hub_accept( struct ps5_desktop_hub *hub, int listener, int is_title )
{
    struct ps5_hub_peer *peer;
    int fd, count;

    for (count = 0; count < 32; count++)
    {
        if ((fd = accept( listener, NULL, NULL )) < 0)
        {
            if (errno == EINTR) count--;
            else return;
            continue;
        }
        if (ps5_desktop_nonblock( fd ) || !(peer = calloc( 1, sizeof(*peer) )))
        {
            close( fd );
            continue;
        }
        peer->fd = fd;
        peer->is_title = is_title;
        peer->next = hub->peers;
        hub->peers = peer;
        if (is_title)
        {
            if (hub->title) ps5_hub_drop_peer( hub->title );
            hub->title = peer;
            hub->title_seen = 1;
            ps5_hub_replay( hub );
            ps5_hub_send_session( hub );
        }
    }
}

/* Whether wineserver runs: it holds a write lock on its server directory's
 * lock file until it exits (server/request.c, acquire_lock), which F_GETLK
 * reads without disturbing it. Where locks do not work, wineserver uses its
 * socket's existence instead, and so does this. A probe connection would
 * be a new client to wineserver and restart its exit delay. */
static int ps5_wineserver_running(void)
{
    const char *prefix = getenv( "WINEPREFIX" ), *home = getenv( "HOME" );
    char config[PATH_MAX], path[PATH_MAX];
    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
    struct stat st;
    int fd, ret;

    if (prefix) snprintf( config, sizeof(config), "%s", prefix );
    else if (home) snprintf( config, sizeof(config), "%s/.wine", home );
    else return 0;
    if (stat( config, &st ) < 0) return 0;  /* no prefix yet: no server either */
    snprintf( path, sizeof(path), "/system_tmp/.wine-%u/server-%llx-%llx/lock", (unsigned int)getuid(),
              (unsigned long long)st.st_dev, (unsigned long long)st.st_ino );
    if ((fd = open( path, O_RDONLY )) < 0) return 0;
    ret = fcntl( fd, F_GETLK, &lock );
    close( fd );
    if (ret == 0) return lock.l_type == F_WRLCK;
    strcpy( strrchr( path, '/' ) + 1, "socket" );
    return stat( path, &st ) == 0;
}

static void ps5_hub_send_session( struct ps5_desktop_hub *hub )
{
    struct ps5_desktop_session session = { { PS5_DESKTOP_SESSION, sizeof(session) } };

    if (hub->session < 0) return;
    session.running = hub->session;
    ps5_hub_queue( hub->title, &session, sizeof(session) );
}

/* Once a second at most: tell the title when wineserver starts or exits. */
static void ps5_hub_check_session( struct ps5_desktop_hub *hub )
{
    time_t now = time( NULL );
    int running;

    if (now == hub->session_checked) return;
    hub->session_checked = now;
    if ((running = ps5_wineserver_running()) == hub->session) return;
    hub->session = running;
    ps5_hub_send_session( hub );
}

/* Whether pid still runs a payload, as ps5/probes/winekill checks: pid files
 * outlive their processes, and a pid may since belong to a system process. */
static int ps5_is_payload( pid_t pid )
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    char buffer[4096];
    struct kinfo_proc kp;
    size_t size = sizeof(buffer);

    /* The console's kinfo_proc is larger than the SDK's: read the start. */
    if (pid <= 1 || pid == getpid() || sysctl( mib, 4, buffer, &size, NULL, 0 ) || size < sizeof(kp)) return 0;
    memcpy( &kp, buffer, sizeof(kp) );
    return kp.ki_pid == pid && !strcmp( kp.ki_comm, "payload.elf" );
}

/* One at a time, each gone (2 s at most) before the next: the kernel trapped
 * after Battle.net's processes died together (ps5/probes/winekill). */
static void ps5_kill_payload( pid_t pid )
{
    int waited = 0;

    if (!ps5_is_payload( pid ) || kill( pid, SIGKILL )) return;
    while (waited < 2000 && ps5_is_payload( pid ))
    {
        usleep( 10000 );
        waited += 10;
    }
    usleep( 300000 );
}

static int ps5_compare_pids_down( const void *a, const void *b )
{
    pid_t x = *(const pid_t *)a, y = *(const pid_t *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* The pids named by the empty files in dir (ps5_note_pid), newest first:
 * children before what started them, wineserver near the end. */
static int ps5_read_pids( const char *dir, pid_t *pids, int max )
{
    DIR *d = opendir( dir );
    struct dirent *e;
    int count = 0;
    char *end;
    long pid;

    if (!d) return 0;
    while (count < max && (e = readdir( d )))
    {
        pid = strtol( e->d_name, &end, 10 );
        if (*end || pid <= 1) continue;
        pids[count++] = pid;
    }
    closedir( d );
    qsort( pids, count, sizeof(*pids), ps5_compare_pids_down );
    return count;
}

/* The session's directory in /system_tmp: files and directories are removed,
 * a link is unlinked itself and never followed. */
static void ps5_remove_tree( const char *path, int depth )
{
    DIR *d = opendir( path );
    struct dirent *e;
    struct stat st;
    char child[PATH_MAX];

    if (!d) return;
    while ((e = readdir( d )))
    {
        if (!strcmp( e->d_name, "." ) || !strcmp( e->d_name, ".." )) continue;
        snprintf( child, sizeof(child), "%s/%s", path, e->d_name );
        if (lstat( child, &st )) continue;
        if (S_ISDIR( st.st_mode ) && depth < 3)
        {
            ps5_remove_tree( child, depth + 1 );
            rmdir( child );
        }
        else unlink( child );
    }
    closedir( d );
}

/* The title is gone and stayed away: nothing shows the session any more.
 * End its processes, then the spawn broker, then this hub, so no Wine payload
 * stays behind holding memory and the descriptors payloads share with the
 * console's services (about 900 open files, 2026-10-04). */
static void __attribute__((noreturn)) ps5_hub_end_session(void)
{
    char base[64], dir[96];
    pid_t pids[512], helpers[16];
    int count, helper_count, i, j;

    snprintf( base, sizeof(base), "/system_tmp/.wine-%u", (unsigned int)getuid() );
    snprintf( dir, sizeof(dir), "%s/helpers", base );
    helper_count = ps5_read_pids( dir, helpers, sizeof(helpers) / sizeof(helpers[0]) );
    snprintf( dir, sizeof(dir), "%s/pids", base );
    count = ps5_read_pids( dir, pids, sizeof(pids) / sizeof(pids[0]) );
    for (i = 0; i < count; i++)
    {
        for (j = 0; j < helper_count && helpers[j] != pids[i]; j++) ;
        if (j == helper_count) ps5_kill_payload( pids[i] );
    }
    for (j = 0; j < helper_count; j++) ps5_kill_payload( helpers[j] );
    ps5_remove_tree( base, 0 );
    rmdir( base );
    _exit(0);
}

/* A title that reconnects within this many seconds keeps the session. */
#define PS5_HUB_TITLE_GRACE 5
static void __attribute__((noreturn)) ps5_desktop_run( int drivers, int tcp_drivers,
                                                       int title_listener )
{
    struct ps5_desktop_hub hub = { .session = -1 };
    struct ps5_hub_peer *peer;
    struct pollfd *pollfds = NULL, *new_pollfds;
    unsigned int count, capacity = 0;
    int driver_events, tcp_driver_events, title_events, ret, events;

    {
        /* A helper's stderr goes nowhere: the hub's lines (where input goes,
         * connections it drops) go to the session directory, removed with it. */
        char log_path[96];
        int log_fd;
        snprintf( log_path, sizeof(log_path), "/system_tmp/.wine-%u/hub.log", (unsigned int)getuid() );
        if ((log_fd = open( log_path, O_WRONLY | O_CREAT | O_APPEND, 0600 )) >= 0)
        {
            dup2( log_fd, 2 );
            close( log_fd );
        }
    }
    for (;;)
    {
        ps5_hub_replay_cursors( &hub );
        count = 3;
        for (peer = hub.peers; peer; peer = peer->next) count++;
        if (count > capacity)
        {
            capacity = capacity ? capacity * 2 : 16;
            while (capacity < count) capacity *= 2;
            if (!(new_pollfds = realloc( pollfds, capacity * sizeof(*pollfds) ))) break;
            pollfds = new_pollfds;
        }
        pollfds[0] = (struct pollfd){ .fd = drivers, .events = POLLIN };
        pollfds[1] = (struct pollfd){ .fd = tcp_drivers, .events = POLLIN };
        pollfds[2] = (struct pollfd){ .fd = title_listener, .events = POLLIN };
        count = 3;
        for (peer = hub.peers; peer; peer = peer->next)
        {
            peer->poll_index = count;
            pollfds[count++] = (struct pollfd)
            {
                .fd = peer->fd, .events = POLLIN | (peer->output_end > peer->output_begin ? POLLOUT : 0),
            };
        }
        ps5_hub_check_session( &hub );
        if ((ret = poll( pollfds, count, 1000 )) < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        driver_events = pollfds[0].revents;
        tcp_driver_events = pollfds[1].revents;
        title_events = pollfds[2].revents;
        for (peer = hub.peers; peer; peer = peer->next)
        {
            if (peer->fd < 0) continue;
            events = pollfds[peer->poll_index].revents;
            if (events & (POLLIN | POLLHUP)) ps5_hub_read( &hub, peer );
            if (peer->fd >= 0 && (events & POLLOUT)) ps5_hub_flush( peer );
            if (events & (POLLERR | POLLNVAL)) ps5_hub_drop_peer( peer );
        }
        ps5_hub_cleanup_peers( &hub );
        if (driver_events & POLLIN) ps5_hub_accept( &hub, drivers, 0 );
        if (tcp_driver_events & POLLIN) ps5_hub_accept( &hub, tcp_drivers, 0 );
        if (title_events & POLLIN) ps5_hub_accept( &hub, title_listener, 1 );
        ps5_hub_cleanup_peers( &hub );
        /* Development runs never connect a title and keep their helpers. */
        if (hub.title) hub.title_left = 0;
        else if (hub.title_seen)
        {
            time_t now = time( NULL );
            if (!hub.title_left) hub.title_left = now;
            else if (now - hub.title_left >= PS5_HUB_TITLE_GRACE) ps5_hub_end_session();
        }
    }
    for (peer = hub.peers; peer; peer = peer->next) ps5_hub_drop_peer( peer );
    ps5_hub_cleanup_peers( &hub );
    free( pollfds );
    close( drivers );
    close( tcp_drivers );
    close( title_listener );
    _exit(1);
}

static int ps5_desktop_connect( const struct sockaddr_un *addr )
{
    int fd = socket( AF_UNIX, SOCK_STREAM, 0 );

    if (fd < 0) return -1;
    if (connect( fd, (const struct sockaddr *)addr, sizeof(*addr) ) < 0)
    {
        close( fd );
        return -1;
    }
    return fd;
}

static void ps5_desktop_socket_path( struct sockaddr_un *addr )
{
    snprintf( addr->sun_path, sizeof(addr->sun_path), "/system_tmp/.wine-%u/%s", (unsigned int)getuid(),
              PS5_DESKTOP_SOCKET );
}

static int ps5_desktop_running(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int conn;

    ps5_desktop_socket_path( &addr );
    if ((conn = ps5_desktop_connect( &addr )) < 0) return 0;
    close( conn );
    return 1;
}

static void __attribute__((noreturn)) ps5_desktop_main(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct sockaddr_in tcp_drivers = { .sin_family = AF_INET,
                                       .sin_port = htons( PS5_DESKTOP_DRIVER_PORT ) };
    struct sockaddr_in tcp_title = { .sin_family = AF_INET,
                                     .sin_port = htons( PS5_DESKTOP_PORT ) };
    char lock_path[96];
    int lock_fd, drivers = -1, tcp_driver_listener = -1, title_listener = -1, option = 1;

    ps5_desktop_socket_path( &addr );
    snprintf( lock_path, sizeof(lock_path), "/system_tmp/.wine-%u/desktop.lock", (unsigned int)getuid() );
    /* Held for the hub's life: no second hub unlinks the socket this one
     * is binding. */
    if ((lock_fd = open( lock_path, O_RDWR | O_CREAT, 0600 )) < 0 || flock( lock_fd, LOCK_EX ) < 0) _exit(1);
    if (ps5_desktop_running()) _exit(0);
    setsid();
    signal( SIGPIPE, SIG_IGN );
    unlink( addr.sun_path );
    tcp_drivers.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    tcp_title.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    if ((drivers = socket( AF_UNIX, SOCK_STREAM, 0 )) < 0 || ps5_desktop_nonblock( drivers ) ||
        bind( drivers, (struct sockaddr *)&addr, sizeof(addr) ) < 0 || listen( drivers, 16 ) < 0 ||
        (tcp_driver_listener = socket( AF_INET, SOCK_STREAM, 0 )) < 0 ||
        setsockopt( tcp_driver_listener, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option) ) < 0 ||
        ps5_desktop_nonblock( tcp_driver_listener ) ||
        bind( tcp_driver_listener, (struct sockaddr *)&tcp_drivers, sizeof(tcp_drivers) ) < 0 ||
        listen( tcp_driver_listener, 16 ) < 0 ||
        (title_listener = socket( AF_INET, SOCK_STREAM, 0 )) < 0 ||
        setsockopt( title_listener, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option) ) < 0 ||
        ps5_desktop_nonblock( title_listener ) ||
        bind( title_listener, (struct sockaddr *)&tcp_title, sizeof(tcp_title) ) < 0 ||
        listen( title_listener, 4 ) < 0)
    {
        fprintf( stderr, "wine: cannot start the PS5 desktop hub: %s\n", strerror( errno ));
        unlink( addr.sun_path );
        _exit(1);
    }
    ps5_note_pid();
    ps5_note_helper_pid();
    ps5_null_stdio();
    ps5_desktop_run( drivers, tcp_driver_listener, title_listener );
}

/* A loader started by the broker takes the oldest request: its arguments,
 * environment, working directory and descriptors. Returns 0 when there is
 * none (the first process, started by hand). */
static int ps5_pull_request( int *argc, char ***argv )
{
    struct ps5_spawn_message msg = { .op = PS5_SPAWN_PULL };
    int fds[PS5_SPAWN_MAX_FDS + 1], nfds, sock, i;
    char *blob, *p, **args, **env;

    if ((sock = ps5_spawn_connect()) < 0) return 0;
    if (ps5_spawn_send( sock, &msg, "", NULL, 0 ) || ps5_spawn_recv( sock, &msg, &blob, fds, &nfds ))
    {
        close( sock );
        return 0;
    }
    close( sock );
    if (!msg.argc || nfds < 1)
    {
        for (i = 0; i < nfds; i++) close( fds[i] );
        free( blob );
        return 0;
    }
    /* move every descriptor out of the way, then to its number */
    for (i = 0; i < nfds; i++)
    {
        int high = fcntl( fds[i], F_DUPFD, 256 );
        close( fds[i] );
        fds[i] = high;
    }
    fchdir( fds[0] );
    close( fds[0] );
    for (i = 1; i < nfds; i++)
    {
        dup2( fds[i], msg.target_fds[i - 1] );
        close( fds[i] );
    }
    args = malloc( (msg.argc + 1) * sizeof(*args) );
    env = malloc( (msg.envc + 1) * sizeof(*env) );
    p = blob + strlen( blob ) + 1;  /* past the loader's path */
    for (i = 0; i < msg.argc; i++, p += strlen( p ) + 1) args[i] = p;
    args[msg.argc] = NULL;
    for (i = 0; i < msg.envc; i++, p += strlen( p ) + 1) env[i] = p;
    env[msg.envc] = NULL;
    environ = env;
    *argc = msg.argc;
    *argv = args;
    return 1;
}

/* Only the single-threaded wineserver calls this callback. Keep the SDK's
 * initialized kernel-copy state in the loader, and prevent asynchronous
 * signal handlers from running while its process credentials are elevated. */
static int ps5_ptrace( int request, pid_t pid, caddr_t addr, int data )
{
    sigset_t blocked, previous;
    uint8_t caps[16], privileged_caps[16];
    uint64_t authid;
    pid_t self = getpid();
    int ret = -1, err = EIO, restore_authid, restore_caps;

    sigfillset( &blocked );
    sigdelset( &blocked, SIGSEGV );
    sigdelset( &blocked, SIGBUS );
    sigdelset( &blocked, SIGILL );
    sigdelset( &blocked, SIGFPE );
    sigdelset( &blocked, SIGTRAP );
    sigdelset( &blocked, SIGEMT );
    sigdelset( &blocked, SIGSYS );
    if (sigprocmask( SIG_BLOCK, &blocked, &previous ) == -1) return -1;

    /* The SDK returns sentinels on credential failures, not reliable errno. */
    if (!(authid = kernel_get_ucred_authid( self )) || kernel_get_ucred_caps( self, caps ))
        goto done;

    memset( privileged_caps, 0xff, sizeof(privileged_caps) );
    if (kernel_set_ucred_authid( self, 0x4800000000010003ULL ) ||
        kernel_set_ucred_caps( self, privileged_caps ))
        err = EPERM;
    else
    {
        errno = 0;
        ret = (int)syscall( SYS_ptrace, request, pid, addr, data );
        err = errno;
    }

    /* Either setter may have mutated credentials before reporting failure.
     * Always attempt both restores, even when the first one fails. */
    restore_authid = kernel_set_ucred_authid( self, authid );
    restore_caps = kernel_set_ucred_caps( self, caps );
    if (restore_authid || restore_caps)
    {
        fputs( "wine: could not restore wineserver ptrace credentials\n", stderr );
        _exit(1);
    }

done:
    if (sigprocmask( SIG_SETMASK, &previous, NULL ) == -1)
    {
        fputs( "wine: could not restore wineserver ptrace signal mask\n", stderr );
        _exit(1);
    }
    errno = err;
    return ret;
}

/* A request whose argv[0] ends in "wineserver" runs the server, built as
 * wineserver.so beside that path (ps5/build-wine.sh). */
static void ps5_run_server( int argc, char **argv )
{
    const char *name = strrchr( argv[0], '/' );
    int (*server_main)( int, char ** );
    void (*server_set_ptrace)( int (*)( int, pid_t, caddr_t, int ));
    char *path;
    void *handle;

    if (strcmp( name ? name + 1 : argv[0], "wineserver" )) return;
    asprintf( &path, "%s.so", argv[0] );
    if (!(handle = dlopen( path, RTLD_NOW | RTLD_LOCAL )) ||
        !(server_main = dlsym( handle, "wineserver_main" )) ||
        !(server_set_ptrace = dlsym( handle, "wineserver_set_ptrace" )))
    {
        fprintf( stderr, "wine: could not load %s: %s\n", path, dlerror() );
        exit(1);
    }
    server_set_ptrace( ps5_ptrace );
    /* the console's crash reports give bare addresses and its dladdr reports
     * nothing: this line places wineserver.so for llvm-symbolizer */
    if (getenv( "WINEDEBUG" )) fprintf( stderr, "wine: %s wineserver_main=%p\n", path, server_main );
    exit( server_main( argc, argv ));
}

extern void payload_exit( int );
static int title_loader;
static struct wine_ps5_title_exit title_exit = WINE_PS5_TITLE_EXIT_INIT;

/* Native parent service threads have no Wine TEB and must never enter ntdll
 * merely to request shutdown. This only publishes to the loader-owned
 * condition; the SDK-owning supervisor performs shutdown and final exit. */
void wine_ps5_title_native_request_exit( int status )
{
    wine_ps5_title_request_exit( &title_exit, status );
}

/* Startup errors are still on the SDK-owning thread. After Wine starts, all
 * client/server exits use the request callback instead of jumping here. */
static void __attribute__((noreturn)) ps5_loader_exit( int status )
{
    if (title_loader)
    {
        payload_exit( status );
        __builtin_unreachable();
    }
    exit( status );
}

static void ps5_title_publish_exit( int status )
{
#ifdef WINE_PS5_TITLE_B1_PARENT
    /* The first process exiting ends nothing else on Windows, whatever its
     * exit code: start.exe /exec waits only for its immediate child, and
     * Steam's bootstrapper exits with 42 after starting the updated client.
     * Keep this title's server alive until the native PsmKit supervisor has
     * reaped the session's remaining children (none: returns at once). */
    extern void wine_ps5_title_wait_children(void);
    wine_ps5_title_wait_children();
#endif
    /* ntdll acknowledges this return before the supervisor may unmap us. */
    wine_ps5_title_request_exit( &title_exit, status );
}

struct ps5_title_startup
{
    struct wine_ps5_title_runtime runtime;
    void (*set_runtime)( const struct wine_ps5_title_runtime * );
    void (*init)( int, char ** );
    int argc;
    char **argv;
    sigset_t signals;
};

static void *ps5_title_initial_thread( void *arg )
{
    struct ps5_title_startup *startup = arg;
    sigset_t blocked;

    pthread_sigmask( SIG_SETMASK, &startup->signals, NULL );
    startup->set_runtime( &startup->runtime );
    startup->init( startup->argc, startup->argv );
    wine_ps5_title_critical_signals( &blocked );
    pthread_sigmask( SIG_BLOCK, &blocked, NULL );
    ps5_title_publish_exit( 1 );  /* __wine_main must not return */
    pthread_exit( NULL );        /* the supervisor joins this initial thread */
}

static void __attribute__((noreturn)) ps5_run_title( void *module, void (*init)( int, char ** ),
                                                    int argc, char **argv,
                                                    const struct wine_ps5_title_runtime *native )
{
    struct ps5_title_startup startup;
    void (*shutdown_runtime)(void) = dlsym( module, "__wine_ps5_shutdown_title_runtime" );
    pthread_attr_t attr;
    pthread_t thread;
    sigset_t blocked;
    char line[160];
    int ret, status;

    startup.set_runtime = dlsym( module, "__wine_ps5_set_title_runtime" );
    if (!init || !startup.set_runtime || !shutdown_runtime)
    {
        sceKernelDebugOutText( 0, "wine: missing trusted native title runtime callbacks\n" );
        ps5_loader_exit( 1 );
    }
    startup.runtime = *native;
    startup.runtime.exit = ps5_title_publish_exit;
    startup.init = init;
    startup.argc = argc;
    startup.argv = argv;
    /* This thread has no TEB and must outlive every Windows thread, including
     * an initial thread which calls ExitThread while workers keep running.
     * Keep asynchronous ntdll handlers off its SDK startup stack. */
    wine_ps5_title_critical_signals( &blocked );
    pthread_sigmask( SIG_BLOCK, &blocked, &startup.signals );
    if ((ret = pthread_attr_init( &attr ))) goto failed;
    if (!(ret = pthread_attr_setstacksize( &attr, PS5_THREAD_STACK_BYTES )))
        ret = native->create_thread( &thread, &attr, ps5_title_initial_thread, &startup );
    pthread_attr_destroy( &attr );
    if (ret) goto failed;
    /* Shutdown closes callback entry, waits for returns into ntdll, stops the
     * server, and joins the initial native thread before loader unmapping. */
    status = wine_ps5_title_supervise_exit( &title_exit, shutdown_runtime );
    snprintf( line, sizeof(line), "wine: title supervisor exit_code=%d; private wineserver joined\n", status );
    sceKernelDebugOutText( 0, line );
    payload_exit( status );  /* the sole post-startup SDK jump, on its owner */
    __builtin_unreachable();
failed:
    pthread_sigmask( SIG_SETMASK, &startup.signals, NULL );
    snprintf( line, sizeof(line), "wine: cannot start title Windows thread: %s\n", strerror( ret ));
    sceKernelDebugOutText( 0, line );
    ps5_loader_exit( 1 );
}

/* Only main below uses this startup exit route; payload helpers above keep
 * their ordinary libc exit behavior. ps5_run_title never returns to main. */
#define exit ps5_loader_exit

#endif  /* __PROSPERO__ */


/**********************************************************************
 *           main
 */
int main( int argc, char *argv[] )
{
    void *handle;
#ifdef __PROSPERO__
#ifdef WINEPS5_PUBLIC_TITLE_ONLY
    /* Native PsmKit entry has no payload/broker/credential startup path. */
    int in_title = 1;
#else
    const char *title_mode = getenv( "WINEPS5_IN_TITLE" );
    int in_title = title_mode && !strcmp( title_mode, "1" );
#endif
    const char *test_mode = getenv( "WINEPS5_DIRECT_TEST" );
    int direct_test = in_title && test_mode && !strcmp( test_mode, "1" );
    const char *runtime_marker = getenv( "WINEPS5_TITLE_RUNTIME" );
    const struct wine_ps5_title_runtime *native_runtime =
        in_title && runtime_marker ? (void *)(uintptr_t)strtoull( runtime_marker, NULL, 16 ) : NULL;
    title_loader = in_title;
#endif

    init_reserved_areas();

#ifdef __PROSPERO__
    /* Neither boot.txt nor a payload's inherited environment may enable
     * direct memory. Only the original title context can hand it to ntdll. */
    if (unsetenv( "WINEPS5_DIRECT_MEMORY" ) || unsetenv( "WINEPS5_DIRECT_TEST" ) ||
        unsetenv( "WINEPS5_TITLE_RUNTIME" ))
    {
        perror( "wine: cannot clear direct-memory startup context" );
        exit(1);
    }
    /* The title's SDK runtime sees the title's original argc/argv. It must
     * neither claim a helper role nor pull a request meant for a new payload.
     * Consume the marker before spawning, so every child stays a payload. */
    if (in_title)
    {
        unsetenv( "WINEPS5_IN_TITLE" );
#ifndef WINEPS5_PUBLIC_TITLE_ONLY
        argc = 0;
        argv = NULL;
        ps5_read_boot_file( PS5_TITLE_BOOT_FILE, &argc, &argv );
#endif
        unsetenv( "WINEPS5_IN_TITLE" );  /* reserved even if boot.txt supplies it */
        if (argc < 2)
        {
            fputs( "wine: in-title startup needs arg LOADER and arg PROGRAM in boot.txt\n", stderr );
            exit(1);
        }
        /* The process is already the right loader: never try to exec a
         * preloader or a renamed-wrapper executable inside a BigApp. */
        if (setenv( "WINELOADERNOEXEC", "1", 1 ))
        {
            perror( "wine: cannot disable in-title loader exec" );
            exit(1);
        }
        fprintf( stderr, "wine: in-title startup pid=%d argc=%d; private wineserver, no child processes\n",
                 (int)getpid(), argc );
    }
    else
    {
        /* a loader sent to become the broker or desktop hub (ps5_start_helper) */
        if (argc <= 1 && ps5_claim( "broker" )) ps5_broker_main();
        if (argc <= 1 && ps5_claim( "desktop" )) ps5_desktop_main();
        if (argc > 1 || !ps5_pull_request( &argc, &argv )) ps5_read_boot( &argc, &argv );
    }
    /* The boot file and broker request may have supplied reserved names. */
    if (unsetenv( "WINEPS5_IN_TITLE" ) || unsetenv( "WINEPS5_DIRECT_MEMORY" ) ||
        unsetenv( "WINEPS5_DIRECT_TEST" ) || unsetenv( "WINEPS5_TITLE_RUNTIME" ))
    {
        perror( "wine: cannot clear untrusted direct-memory context" );
        exit(1);
    }
    if (!in_title)
    {
        ps5_note_pid();
        ps5_start_helper( "broker", argv[0], ps5_broker_running );
        ps5_start_helper( "desktop", argv[0], ps5_desktop_running );
        ps5_run_server( argc, argv );
    }
    if (in_title)
    {
        int prefix_error;

        /* B3 desktop, B2 payload_spawn and title open_url callbacks are optional;
         * G3 and headless runtimes still satisfy the title runtime ABI without them. */
        if (!native_runtime || !native_runtime->create_thread || !native_runtime->join_thread ||
            !native_runtime->create_key || !native_runtime->get_specific || !native_runtime->set_specific ||
            !native_runtime->delete_key)
        {
            sceKernelDebugOutText( 0, "wine: missing trusted native title runtime callbacks\n" );
            ps5_loader_exit( 1 );
        }
        errno = 0;
        if (!wine_ps5_title_stdout_valid( native_runtime ))
        {
            char line[160];
            snprintf( line, sizeof(line), "wine: invalid native stdout version=%u fd=%d errno=%d\n",
                      native_runtime->version, native_runtime->stdout_fd, errno );
            sceKernelDebugOutText( 0, line );
            ps5_loader_exit( 1 );
        }
        /* ntdll consumes both markers before starting its private server.
         * A title runtime with its own root (B1) maps Wine's payload sonames,
         * such as SONAME_LIBGNUTLS, into that root. */
#ifdef WINE_PS5_TITLE_ROOT
        prefix_error = ps5_rtld_register_prefix( PS5_WINE_ROOT, WINE_PS5_TITLE_ROOT );
#else
        prefix_error = ps5_rtld_register_prefix( PS5_WINE_ROOT, "/app0/wine" );
#endif
        if (prefix_error)
        {
            fprintf( stderr, "wine: cannot register title loader path prefix: %d\n", prefix_error );
            exit(1);
        }
        if (setenv( "WINEPS5_DIRECT_MEMORY", "1", 1 ) ||
            (direct_test && setenv( "WINEPS5_DIRECT_TEST", "1", 1 )))
        {
            perror( "wine: cannot set title direct-memory context" );
            exit(1);
        }
    }
    /* the process's own path is the ELF loader's: argv[0] names this loader */
    if ((handle = try_dlopen( argv[0] )))
#else
    if ((handle = try_dlopen( get_self_exe() )) ||
        (handle = try_dlopen( argv[0] )))
#endif
    {
        void (*init_func)(int, char **) = dlsym( handle, "__wine_main" );
#ifdef __PROSPERO__
        /* Keep the module placement for symbolization, but never print
         * title argv: launcher-provided arguments may contain credentials. */
        if (in_title)
            fprintf( stderr, "wine: pid %d ntdll.so __wine_main=%p argc=%d argv=REDACTED\n",
                     (int)getpid(), init_func, argc );
        else
            fprintf( stderr, "wine: pid %d ntdll.so __wine_main=%p %s %.60s\n", (int)getpid(), init_func,
                     argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "" );
        if (in_title) ps5_run_title( handle, init_func, argc, argv, native_runtime );
#endif
        if (init_func) init_func( argc, argv );
        fprintf( stderr, "wine: __wine_main function not found in ntdll.so\n" );
        exit(1);
    }

    fprintf( stderr, "wine: could not load ntdll.so: %s\n", dlerror() );
    pthread_detach( pthread_self() );  /* force importing libpthread for OpenGL */
    exit(1);
}
