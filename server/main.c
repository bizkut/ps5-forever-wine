/*
 * Server main function
 *
 * Copyright (C) 1998 Alexandre Julliard
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

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif
#ifdef __PROSPERO__
# include <errno.h>
# include <stdarg.h>
# include <pthread.h>
# include <poll.h>
# include <sys/socket.h>
# include <ps5platform/klog.h>
# include <ps5platform/heap.h>
# include <ps5platform/libc.h>
# include "wine/ps5_title.h"
# include "wine/ps5_title_private.h"
# include "wine/ps5_suspend.h"
#endif

#include "object.h"
#include "file.h"
#include "thread.h"
#include "process.h"
#include "request.h"
#include "unicode.h"
#include "security.h"

#include "fsync.h"

/* command-line options */
int debug_level = 0;
int foreground = 0;
timeout_t master_socket_timeout = 0; /* master socket timeout, default is 3 seconds */
const char *server_argv0;

/* parse-line args */

static void usage( FILE *fh )
{
    fprintf(fh, "Usage: %s [options]\n\n", server_argv0);
    fprintf(fh, "Options:\n");
    fprintf(fh, "   -d[n], --debug[=n]       set debug level to n or +1 if n not specified\n");
    fprintf(fh, "   -f,    --foreground      remain in the foreground for debugging\n");
    fprintf(fh, "   -h,    --help            display this help message\n");
    fprintf(fh, "   -k[n], --kill[=n]        kill the current wineserver, optionally with signal n\n");
    fprintf(fh, "   -p[n], --persistent[=n]  make server persistent, optionally for n seconds\n");
    fprintf(fh, "   -v,    --version         display version information and exit\n");
    fprintf(fh, "   -w,    --wait            wait until the current wineserver terminates\n");
    fprintf(fh, "\n");
}

static void option_callback( int optc, char *optarg )
{
    int ret;

    switch (optc)
    {
    case 'd':
        if (optarg && isdigit(*optarg))
            debug_level = atoi( optarg );
        else
            debug_level++;
        break;
    case 'f':
        foreground = 1;
        break;
    case 'h':
        usage(stdout);
        exit(0);
        break;
    case 'k':
        if (optarg && isdigit(*optarg))
            ret = kill_lock_owner( atoi( optarg ) );
        else
            ret = kill_lock_owner(-1);
        exit( !ret );
    case 'p':
        if (optarg && isdigit(*optarg))
            master_socket_timeout = (timeout_t)atoi( optarg ) * -TICKS_PER_SEC;
        else
            master_socket_timeout = TIMEOUT_INFINITE;
        break;
    case 'v':
        fprintf( stderr, "%s\n", PACKAGE_STRING );
        exit(0);
    case 'w':
        wait_for_lock();
        exit(0);
    }
}

/* command-line option parsing */
/* partly based on the GLibc getopt() implementation */

static struct long_option
{
    const char *name;
    int has_arg;
    int val;
} long_options[] =
{
    {"debug",       2, 'd'},
    {"foreground",  0, 'f'},
    {"help",        0, 'h'},
    {"kill",        2, 'k'},
    {"persistent",  2, 'p'},
    {"version",     0, 'v'},
    {"wait",        0, 'w'},
    { NULL }
};

static void parse_options( int argc, char **argv, const char *short_opts,
                           const struct long_option *long_opts, void (*callback)( int, char* ) )
{
    const char *flag;
    char *start, *end;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] != '-' || !argv[i][1])  /* not an option */
            continue;
        if (!strcmp( argv[i], "--" ))
            break;
        start = argv[i] + 1 + (argv[i][1] == '-');

        if (argv[i][1] == '-')
        {
            /* handle long option */
            const struct long_option *opt, *found = NULL;
            int count = 0;

            if (!(end = strchr( start, '=' ))) end = start + strlen(start);
            for (opt = long_opts; opt && opt->name; opt++)
            {
                if (strncmp( opt->name, start, end - start )) continue;
                if (!opt->name[end - start])  /* exact match */
                {
                    found = opt;
                    count = 1;
                    break;
                }
                if (!found)
                {
                    found = opt;
                    count++;
                }
                else if (found->has_arg != opt->has_arg || found->val != opt->val)
                {
                    count++;
                }
            }

            if (count > 1) goto error;

            if (found)
            {
                if (*end)
                {
                    if (!found->has_arg) goto error;
                    end++;  /* skip '=' */
                }
                else if (found->has_arg == 1)
                {
                    if (i == argc - 1) goto error;
                    end = argv[++i];
                }
                else end = NULL;

                callback( found->val, end );
                continue;
            }
            goto error;
        }

        /* handle short option */
        for ( ; *start; start++)
        {
            if (!(flag = strchr( short_opts, *start ))) goto error;
            if (flag[1] == ':')
            {
                end = start + 1;
                if (!*end) end = NULL;
                if (flag[2] != ':' && !end)
                {
                    if (i == argc - 1) goto error;
                    end = argv[++i];
                }
                callback( *start, end );
                break;
            }
            callback( *start, NULL );
        }
    }
    return;

error:
    usage( stderr );
    exit(1);
}

static void sigterm_handler( int signum )
{
    exit(1);  /* make sure atexit functions get called */
}

static void init_limits(void)
{
#ifdef RLIMIT_NOFILE
    struct rlimit rlimit;

    if (!getrlimit( RLIMIT_NOFILE, &rlimit ))
    {
        rlimit.rlim_cur = rlimit.rlim_max;
        if (!setrlimit( RLIMIT_NOFILE, &rlimit )) return;
#ifdef __APPLE__
        {
            /* macOS before Big Sur fails if rlim_max is larger than maxfilesperproc */
            unsigned int nlimit = 0;
            size_t size = sizeof(nlimit);
            sysctlbyname("kern.maxfilesperproc", &nlimit, &size, NULL, 0);
            rlimit.rlim_cur = max( nlimit, OPEN_MAX );
            setrlimit( RLIMIT_NOFILE, &rlimit );
        }
#endif
    }
#endif
}

#ifdef __PROSPERO__
int inprocess_server;
int inprocess_wait_profile;
int inprocess_supervised;
unsigned int inprocess_desktop_requests;
const char *inprocess_config_dir;
const char *inprocess_nls_dir;
size_t (*inprocess_read_memory)( const void *, void *, size_t );
unsigned int (*inprocess_write_memory)( void *, const void *, size_t );
int (*inprocess_signal_thread)( unsigned int, void *, int );
int (*inprocess_signal_child_thread)( int, unsigned int, void *, int );
int (*inprocess_terminate_child)( int );
static pthread_t inprocess_thread;
static struct wine_ps5_title_runtime title_runtime;
static pthread_mutex_t inprocess_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t inprocess_request_mutex = PTHREAD_MUTEX_INITIALIZER;
int inprocess_direct_enabled, inprocess_direct_ready;
static int inprocess_started, inprocess_joined;
static int inprocess_client = -1, inprocess_stop_read = -1, inprocess_stop_write = -1;
static struct fd *inprocess_stop_fd;
static struct process *inprocess_process;

int inprocess_is_host_process( const struct process *process )
{
    return process == inprocess_process;
}

__attribute__((visibility("default")))
void wineserver_dump_title_threads( unsigned int snapshot, unsigned int total )
{
    char line[128];

    if (!inprocess_server) return;
    if (pthread_mutex_trylock( &inprocess_request_mutex ))
    {
        snprintf( line, sizeof(line),
                  "title hang wait diagnostic=unavailable sample=%u/%u server_lock=busy\n",
                  snapshot, total );
        sceKernelDebugOutText( 0, line );
        return;
    }
    set_current_time();
    dump_title_thread_states( snapshot, total );
    pthread_mutex_unlock( &inprocess_request_mutex );
}


/* The title's libc heap is much smaller than its flexible pool; a loaded
 * prefix exhausted it before the first client could be created. Only this
 * server instance uses the platform's separately reserved direct heap.
 * Foreign blocks from libc's strdup/asprintf/FILE remain libc-owned. */
extern void *__real_malloc( size_t );
extern void *__real_calloc( size_t, size_t );
extern void *__real_realloc( void *, size_t );
extern void __real_free( void * );
extern int __real_pthread_key_create( pthread_key_t *, void (*)( void * ));
extern void *__real_pthread_getspecific( pthread_key_t );
extern int __real_pthread_setspecific( pthread_key_t, const void * );
static const char *allocation_phase = "initialization";

static void log_allocation_failure( const char *operation, size_t count, size_t size )
{
    char line[256];
    int error = errno;

    snprintf( line, sizeof(line), "wine: title server allocation failed phase=%s operation=%s count=%zu size=%zu errno=%d\n",
              allocation_phase, operation, count, size, error );
    sceKernelDebugOutText( 0, line );
    errno = error;
}

/* A title child's stderr does not reach klog, so protocol errors and lost
 * clients of the private server would vanish. One klog line per event. */
void inprocess_trace( const char *format, ... )
{
    char line[768];
    va_list args;
    int error = errno, used;

    if (!inprocess_server) return;
    used = snprintf( line, sizeof(line), "wineserver: " );
    va_start( args, format );
    vsnprintf( line + used, sizeof(line) - used, format, args );
    va_end( args );
    sceKernelDebugOutText( 0, line );
    errno = error;
}


void *__wrap_malloc( size_t size )
{
    void *ptr = inprocess_server ? ps5_heap_malloc( size ) : __real_malloc( size );
    if (inprocess_server && !ptr && size) log_allocation_failure( "malloc", 1, size );
    return ptr;
}

void *__wrap_calloc( size_t count, size_t size )
{
    void *ptr = inprocess_server ? ps5_heap_calloc( count, size ) : __real_calloc( count, size );
    if (inprocess_server && !ptr && count && size) log_allocation_failure( "calloc", count, size );
    return ptr;
}

void *__wrap_realloc( void *ptr, size_t size )
{
    if (inprocess_server)
    {
        if (!ptr) return ps5_heap_malloc( size );
        if (ps5_heap_owns( ptr )) return ps5_heap_realloc( ptr, size );
    }
    return __real_realloc( ptr, size );
}

void __wrap_free( void *ptr )
{
    if (inprocess_server && ps5_heap_owns( ptr )) ps5_heap_free( ptr );
    else __real_free( ptr );
}

/* libkernel's lazy thread-specific storage must be entered from the native
 * FSELF context; the ordinary server ELF is not an OS-registered module. */
int __wrap_pthread_key_create( pthread_key_t *key, void (*destructor)( void * ))
{
    return inprocess_server ? title_runtime.create_key( key, destructor ) :
                              __real_pthread_key_create( key, destructor );
}

void *__wrap_pthread_getspecific( pthread_key_t key )
{
    return inprocess_server ? title_runtime.get_specific( key ) : __real_pthread_getspecific( key );
}

int __wrap_pthread_setspecific( pthread_key_t key, const void *value )
{
    return inprocess_server ? title_runtime.set_specific( key, value ) : __real_pthread_setspecific( key, value );
}

void inprocess_exit( int status )
{
    title_runtime.exit( status );
    __builtin_unreachable();
}

/* The control socket also interrupts kqueue when a direct wait adds an
 * earlier deadline. Per-operation nonblocking I/O works in a title, unlike
 * changing descriptor flags. EOF still belongs exclusively to shutdown. */
void inprocess_wake_server(void)
{
    char byte = 0;
    int ret;
    if (inprocess_stop_write != -1)
        do ret = send( inprocess_stop_write, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL );
        while (ret < 0 && errno == EINTR);
}

static void inprocess_stop_event( struct fd *fd, int event )
{
    char buffer[64];
    int ret;

    while ((ret = recv( get_unix_fd( fd ), buffer, sizeof(buffer), MSG_DONTWAIT )) > 0);
    if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    inprocess_trace( "private server stop channel event=%#x\n", event );
    pthread_exit( NULL );
}

static const struct fd_ops inprocess_stop_ops =
{
    NULL, inprocess_stop_event, NULL, NULL, NULL, NULL, NULL
};

static void inprocess_cleanup( void *arg )
{
    char line[160];

    /* Initialization, main-loop callbacks and cleanup share the same lock;
     * direct callers can never see half-initialized or destroyed objects. */
    __atomic_store_n( &inprocess_direct_ready, 0, __ATOMIC_RELEASE );
    if (inprocess_stop_fd) release_object( inprocess_stop_fd );
    inprocess_stop_fd = NULL;
    if (inprocess_stop_read != -1) close( inprocess_stop_read );
    inprocess_stop_read = -1;
    if (inprocess_client != -1) close( inprocess_client );
    inprocess_client = -1;
    if (inprocess_process)
    {
        /* Release reply/request descriptors too, so startup clients blocked
         * before installing their signal handlers can finish without SIGQUIT. */
        kill_process( inprocess_process, 0 );
        release_object( inprocess_process );
        inprocess_process = NULL;
    }
    /* Save before the server's objects go away; a title killed by the system
     * never gets here, the periodic save in registry.c covers that. */
    flush_registry();
    close( config_dir_fd );
    close( server_dir_fd );
    config_dir_fd = server_dir_fd = -1;
    snprintf( line, sizeof(line), "wine: in-process wineserver stopped; registry saved; desktop_requests=%u\n",
              inprocess_desktop_requests );
    sceKernelDebugOutText( 0, line );
    if (inprocess_direct_enabled) pthread_mutex_unlock( &inprocess_request_mutex );
}

static void *inprocess_main( void *arg )
{
    struct process *process;
    struct ps5_heap_stats heap;
    sigset_t blocked;
    char line[192];
    void *warmup;
    int client, control;

    /* This is not a Wine client thread and has no TEB. Leave ntdll's handlers
     * intact, and route asynchronous signals only to the client threads.
     * SIGSEGV/BUS/ILL/FPE/TRAP remain synchronous fatal faults if we have a bug. */
    sigfillset( &blocked );
    sigdelset( &blocked, SIGSEGV );
    sigdelset( &blocked, SIGBUS );
    sigdelset( &blocked, SIGILL );
    sigdelset( &blocked, SIGFPE );
    sigdelset( &blocked, SIGTRAP );
    pthread_sigmask( SIG_BLOCK, &blocked, NULL );
    pthread_cleanup_push( inprocess_cleanup, NULL );
    if (inprocess_direct_enabled) pthread_mutex_lock( &inprocess_request_mutex );
    allocation_phase = "server-thread";
    sceKernelDebugOutText( 0, "wine: title server thread heap pre-touch begin size=1\n" );
    if (!(warmup = ps5_heap_malloc( 1 )))
    {
        log_allocation_failure( "pre-touch", 1, 1 );
        fatal_error( "cannot initialize title server thread heap\n" );
    }
    ps5_heap_free( warmup );
    sceKernelDebugOutText( 0, "wine: title server thread heap pre-touch passed\n" );
    if ((config_dir_fd = open( inprocess_config_dir, O_RDONLY | O_DIRECTORY )) == -1 ||
        (server_dir_fd = open( server_dir, O_RDONLY | O_DIRECTORY )) == -1)
        fatal_error( "cannot open private title server directories: %s\n", strerror( errno ));
    sock_init();
    set_current_time();
    init_memory();
    init_user_sid();
    init_directories( load_intl_file() );
    init_threading();
    allocation_phase = "registry";
    init_registry();
    ps5_heap_stats( &heap );
    snprintf( line, sizeof(line), "wine: title server heap base=%p range_bytes=%zu mapped_bytes=%zu arenas=%u\n",
              (void *)heap.range_base, heap.range_bytes, heap.mapped_bytes, heap.arenas );
    sceKernelDebugOutText( 0, line );
    control = inprocess_stop_read;
    inprocess_stop_read = -1;  /* create_anonymous_fd owns it even on failure */
    if (!(inprocess_stop_fd = create_anonymous_fd( &inprocess_stop_ops, control, NULL, 0 )))
        fatal_error( "cannot register title server shutdown channel\n" );
    set_fd_events( inprocess_stop_fd, POLLIN );
    client = inprocess_client;
    inprocess_client = -1;  /* create_process consumes this descriptor */
    /* The process socket uses MSG_DONTWAIT per operation on PS5. */
    allocation_phase = "first-client";
    if (!(process = create_process( client, NULL, 0, NULL, NULL, NULL, 0, NULL )))
        fatal_error( "cannot create private title client\n" );
    inprocess_process = process;  /* cleanup owns this reference until shutdown */
    if (!create_thread( -1, process, NULL ))
    {
        fatal_error( "cannot create private title client thread\n" );
    }
    allocation_phase = "requests";
    main_loop();
    pthread_cleanup_pop( 1 );
    return NULL;
}

/* The host starts without a listener, lock or broker. New Windows children
 * join through descriptors handed to new_process. All initialization runs
 * on the server thread; a separate socketpair lets the native supervisor
 * stop it even if clients are still alive or the server itself fails. */
__attribute__((visibility("default")))
int wineserver_start_inprocess( int client, const char *prefix, const char *directory, const char *nls,
                               size_t (*read_memory)( const void *, void *, size_t ),
                               unsigned int (*write_memory)( void *, const void *, size_t ),
                               int (*signal_thread)( unsigned int, void *, int ),
                               const struct wine_ps5_title_runtime *runtime )
{
    pthread_attr_t attr;
    int control[2], ret;
    if (!runtime || runtime->version != WINE_PS5_TITLE_RUNTIME_VERSION)
    {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock( &inprocess_mutex );
    if (inprocess_server) { ret = EALREADY; goto failed; }
    title_runtime = *runtime;
    inprocess_supervised = !!runtime->spawn_process;
    inprocess_server = 1;
    inprocess_wait_profile = getenv( "WINEPS5_PROFILE" ) && !strcmp( getenv( "WINEPS5_PROFILE" ), "1" );
    inprocess_direct_enabled = WINE_PS5_DIRECT_REQUESTS &&
        !(getenv( "WINEPS5_DIRECT_REQUESTS" ) && !strcmp( getenv( "WINEPS5_DIRECT_REQUESTS" ), "0" ));
    inprocess_config_dir = prefix;
    /* nls was supplied from a caller's stack and must outlive startup. */
    if (!(server_dir = strdup( directory )) || !(inprocess_nls_dir = strdup( nls )))
    {
        ret = ENOMEM;
        goto failed;
    }
    if (socketpair( AF_UNIX, SOCK_STREAM, 0, control ) == -1) { ret = errno; goto failed; }
    inprocess_stop_read = control[1];
    inprocess_stop_write = control[0];
    fcntl( control[0], F_SETFD, FD_CLOEXEC );
    fcntl( control[1], F_SETFD, FD_CLOEXEC );
    inprocess_read_memory = read_memory;
    inprocess_write_memory = write_memory;
    inprocess_signal_thread = signal_thread;
    inprocess_signal_child_thread = title_runtime.signal_child_thread;
    inprocess_terminate_child = title_runtime.terminate_child;
    server_argv0 = "in-process wineserver";
    foreground = 1;
    inprocess_client = client;
    if ((ret = pthread_attr_init( &attr ))) goto failed;
    if (!(ret = pthread_attr_setstacksize( &attr, PS5_THREAD_STACK_BYTES )))
        ret = title_runtime.create_thread( &inprocess_thread, &attr, inprocess_main, NULL );
    pthread_attr_destroy( &attr );
    if (ret) goto failed;
    inprocess_started = 1;
    pthread_mutex_unlock( &inprocess_mutex );
    sceKernelDebugOutText( 0, "wine: in-process wineserver started; no signal handlers, registry saved every 30 s\n" );
    return 0;
failed:
    /* No fatal callback while holding this lock: the supervisor needs it. */
    if (!inprocess_started && ret != EALREADY)
    {
        if (inprocess_stop_read != -1) close( inprocess_stop_read );
        if (inprocess_stop_write != -1) close( inprocess_stop_write );
        inprocess_stop_read = inprocess_stop_write = -1;
        inprocess_client = -1;  /* failed startup leaves ownership with caller */
    }
    pthread_mutex_unlock( &inprocess_mutex );
    errno = ret;
    return -1;
}

/* Only the runtime-owning supervisor calls this. Claim the join before
 * releasing the lifecycle lock; no fatal requester waits for this function. */
__attribute__((visibility("default"))) void wineserver_stop_inprocess(void)
{
    pthread_t thread;
    char line[128];
    int control, ret;

    pthread_mutex_lock( &inprocess_mutex );
    if (!inprocess_started || inprocess_joined)
    {
        pthread_mutex_unlock( &inprocess_mutex );
        return;
    }
    inprocess_joined = 1;
    thread = inprocess_thread;
    control = inprocess_stop_write;
    pthread_mutex_unlock( &inprocess_mutex );
    /* EOF wakes the existing event loop, including a not-yet-started one. */
    shutdown( control, SHUT_RDWR );
    ret = title_runtime.join_thread( thread, NULL );
    /* Keep the descriptor stable until direct callers have quiesced under
     * the request lock; shutdown wakes kqueue without permitting fd reuse. */
    inprocess_stop_write = -1;
    close( control );
    snprintf( line, sizeof(line), "wine: private wineserver joined once; result=%d\n", ret );
    sceKernelDebugOutText( 0, line );
}
#endif

int main( int argc, char *argv[] )
{
    setvbuf( stderr, NULL, _IOLBF, 0 );
    server_argv0 = argv[0];
    parse_options( argc, argv, "d::fhk::p::vw", long_options, option_callback );

    /* setup temporary handlers before the real signal initialization is done */
    signal( SIGPIPE, SIG_IGN );
    signal( SIGHUP, sigterm_handler );
    signal( SIGINT, sigterm_handler );
    signal( SIGQUIT, sigterm_handler );
    signal( SIGTERM, sigterm_handler );
    signal( SIGABRT, sigterm_handler );
    init_limits();

    sock_init();
    open_master_socket();

    if (do_fsync())
        fsync_init();

    if (debug_level) fprintf( stderr, "wineserver: starting (pid=%ld)\n", (long) getpid() );
    set_current_time();
    init_signals();
    init_memory();
    init_user_sid();
    init_directories( load_intl_file() );
    init_threading();
    init_registry();
    main_loop();
    return 0;
}
