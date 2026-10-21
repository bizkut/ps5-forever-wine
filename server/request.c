/*
 * Server-side request handling
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
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#include <unistd.h>
#include <poll.h>
#ifdef __APPLE__
# include <mach/mach_time.h>
#endif
#ifdef __PROSPERO__
# include <pthread.h>
# include <ps5platform/klog.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wincon.h"
#include "winternl.h"

#include "file.h"
#include "process.h"
#include "thread.h"

/* The PS5's file systems report owners and modes that say nothing about the
 * payload's user, and the console has one user: the multi-user checks below
 * are left out there. */
#ifdef __PROSPERO__
#define OWNED_BY_ME(st) 1
#define PRIVATE_MODE(st) 1
#else
#define OWNED_BY_ME(st) ((st).st_uid == getuid())
#define PRIVATE_MODE(st) (!((st).st_mode & 077))
#endif
#include "wine/ps5_spawn.h"
#include "security.h"
#include "handle.h"
#include "request_handlers.h"
#ifdef __PROSPERO__
#include "wine/ps5_request_profile.h"
#endif

/* Some versions of glibc don't define this */
#ifndef SCM_RIGHTS
#define SCM_RIGHTS 1
#endif

/* path names for server master Unix socket */
static const char * const server_socket_name = "socket";   /* name of the socket file */
static const char * const server_lock_name = "lock";       /* name of the server lock file */

struct master_socket
{
    struct object        obj;        /* object header */
    struct fd           *fd;         /* file descriptor of the master socket */
};

static void master_socket_dump( struct object *obj, int verbose );
static void master_socket_destroy( struct object *obj );
static void master_socket_poll_event( struct fd *fd, int event );

static const struct object_ops master_socket_ops =
{
    sizeof(struct master_socket),  /* size */
    &no_type,                      /* type */
    master_socket_dump,            /* dump */
    no_add_queue,                  /* add_queue */
    NULL,                          /* remove_queue */
    NULL,                          /* signaled */
    NULL,                          /* satisfied */
    no_signal,                     /* signal */
    no_get_fd,                     /* get_fd */
    default_get_sync,              /* get_sync */
    default_map_access,            /* map_access */
    default_get_sd,                /* get_sd */
    default_set_sd,                /* set_sd */
    no_get_full_name,              /* get_full_name */
    no_lookup_name,                /* lookup_name */
    no_link_name,                  /* link_name */
    NULL,                          /* unlink_name */
    no_open_file,                  /* open_file */
    no_kernel_obj_list,            /* get_kernel_obj_list */
    no_close_handle,               /* close_handle */
    master_socket_destroy          /* destroy */
};

static const struct fd_ops master_socket_fd_ops =
{
    NULL,                          /* get_poll_events */
    master_socket_poll_event,      /* poll_event */
    NULL,                          /* flush */
    NULL,                          /* get_fd_type */
    NULL,                          /* ioctl */
    NULL,                          /* queue_async */
    NULL                           /* reselect_async */
};


struct thread *current = NULL;  /* thread handling the current request */
unsigned int global_error = 0;  /* global error code for when no thread is current */
timeout_t server_start_time = 0;  /* server startup time */
char *server_dir = NULL;   /* server directory */
int server_dir_fd = -1;    /* file descriptor for the server dir */
int config_dir_fd = -1;    /* file descriptor for the config dir */

static struct master_socket *master_socket;  /* the master socket object */
static struct timeout_user *master_timeout;

/* complain about a protocol error and terminate the client connection */
void fatal_protocol_error( struct thread *thread, const char *err, ... )
{
    va_list args;

    va_start( args, err );
#ifdef __PROSPERO__
    if (inprocess_server)
    {
        char line[256];
        va_list copy;

        va_copy( copy, args );
        vsnprintf( line, sizeof(line), err, copy );
        va_end( copy );
        inprocess_trace( "protocol error:%04x: %s", thread->id, line );
    }
#endif
    fprintf( stderr, "Protocol error:%04x: ", thread->id );
    vfprintf( stderr, err, args );
    va_end( args );
    thread->exit_code = 1;
    kill_thread( thread, 1 );
}

/* die on a fatal error */
void fatal_error( const char *err, ... )
{
    va_list args;

    va_start( args, err );
#ifdef __PROSPERO__
    if (inprocess_server)
    {
        char line[1024];

        vsnprintf( line, sizeof(line), err, args );
        va_end( args );
        sceKernelDebugOutText( 0, "wineserver: " );
        sceKernelDebugOutText( 0, line );
        inprocess_exit( 1 );
    }
#endif
    fprintf( stderr, "wineserver: " );
    vfprintf( stderr, err, args );
    va_end( args );
    exit(1);
}
/* Internal PS5 server channels are AF_UNIX socketpairs. PsmKit children
 * reject post-creation non-blocking mode and ignore SOCK_NONBLOCK for plain
 * read/write, while MSG_DONTWAIT works. Keep pipes unchanged elsewhere. */
static ssize_t server_channel_send( int fd, const void *buffer, size_t size )
{
#ifdef __PROSPERO__
    return send( fd, buffer, size, MSG_DONTWAIT );
#else
    return write( fd, buffer, size );
#endif
}

static ssize_t server_channel_sendv( int fd, const struct iovec *iov, size_t count )
{
#ifdef __PROSPERO__
    struct msghdr message = {0};

    message.msg_iov = (struct iovec *)iov;
    message.msg_iovlen = count;
    return sendmsg( fd, &message, MSG_DONTWAIT );
#else
    return writev( fd, iov, count );
#endif
}

static ssize_t server_channel_recv( int fd, void *buffer, size_t size )
{
#ifdef __PROSPERO__
    return recv( fd, buffer, size, MSG_DONTWAIT );
#else
    return read( fd, buffer, size );
#endif
}

/* allocate the reply data */
void *set_reply_data_size( data_size_t size )
{
    assert( size <= get_reply_max_size() );
    if (size && !(current->reply_data = mem_alloc( size ))) size = 0;
    current->reply_size = size;
    return current->reply_data;
}

static const struct object_attributes empty_attributes;

/* return object attributes from the current request */
const struct object_attributes *get_req_object_attributes( const struct security_descriptor **sd,
                                                           struct unicode_str *name,
                                                           struct object **root )
{
    const struct object_attributes *attr = get_req_data();
    data_size_t size = get_req_data_size();

    if (root) *root = NULL;

    if (!size)
    {
        *sd = NULL;
        name->len = 0;
        return &empty_attributes;
    }

    if ((size < sizeof(*attr)) || (size - sizeof(*attr) < attr->sd_len) ||
        (size - sizeof(*attr) - attr->sd_len < attr->name_len))
    {
        set_error( STATUS_ACCESS_VIOLATION );
        return NULL;
    }
    if (attr->sd_len && !sd_is_valid( (const struct security_descriptor *)(attr + 1), attr->sd_len ))
    {
        set_error( STATUS_INVALID_SECURITY_DESCR );
        return NULL;
    }
    if ((attr->name_len & (sizeof(WCHAR) - 1)) || attr->name_len >= 65534)
    {
        set_error( STATUS_OBJECT_NAME_INVALID );
        return NULL;
    }
    if (root && attr->rootdir && attr->name_len)
    {
        if (!(*root = get_handle_obj( current->process, attr->rootdir, 0, NULL ))) return NULL;
    }
    *sd = attr->sd_len ? (const struct security_descriptor *)(attr + 1) : NULL;
    name->len = attr->name_len;
    name->str = (const WCHAR *)(attr + 1) + attr->sd_len / sizeof(WCHAR);
    return attr;
}

/* return a pointer to the request data following an object attributes structure */
const void *get_req_data_after_objattr( const struct object_attributes *attr, data_size_t *len )
{
    data_size_t size = (sizeof(*attr) + (attr->sd_len & ~1) + (attr->name_len & ~1) + 3) & ~3;

    if (attr == &empty_attributes || size >= get_req_data_size())
    {
        *len = 0;
        return NULL;
    }
    *len = get_req_data_size() - size;
    return (const char *)get_req_data() + size;
}

/* write the remaining part of the reply */
void write_reply( struct thread *thread )
{
    int ret;

    if ((ret = server_channel_send( get_unix_fd( thread->reply_fd ),
                      (char *)thread->reply_data + thread->reply_size - thread->reply_towrite,
                      thread->reply_towrite )) >= 0)
    {
        if (!(thread->reply_towrite -= ret))
        {
            free( thread->reply_data );
            thread->reply_data = NULL;
            /* sent everything, can go back to waiting for requests */
            set_fd_events( thread->request_fd, POLLIN );
            set_fd_events( thread->reply_fd, 0 );
        }
        return;
    }
    if (errno == EPIPE)
    {
#ifdef __PROSPERO__
        if (thread->process->running_threads == 1)
            inprocess_trace( "thread %04x reply channel EPIPE\n", thread->id );
#endif
        kill_thread( thread, 0 );  /* normal death */
    }
    else if (errno != EWOULDBLOCK && (EWOULDBLOCK == EAGAIN || errno != EAGAIN))
        fatal_protocol_error( thread, "reply write: %s\n", strerror( errno ));
}

/* send a reply to the current thread */
static void send_reply( union generic_reply *reply )
{
    int ret;

    if (!current->reply_size)
    {
        if ((ret = server_channel_send( get_unix_fd( current->reply_fd ),
                          reply, sizeof(*reply) )) != sizeof(*reply)) goto error;
    }
    else
    {
        struct iovec vec[2];

        vec[0].iov_base = (void *)reply;
        vec[0].iov_len  = sizeof(*reply);
        vec[1].iov_base = current->reply_data;
        vec[1].iov_len  = current->reply_size;

        if ((ret = server_channel_sendv( get_unix_fd( current->reply_fd ), vec, 2 )) < sizeof(*reply)) goto error;

        if ((current->reply_towrite = current->reply_size - (ret - sizeof(*reply))))
        {
            /* couldn't write it all, wait for POLLOUT */
            set_fd_events( current->reply_fd, POLLOUT );
            set_fd_events( current->request_fd, 0 );
            return;
        }
    }
    free( current->reply_data );
    current->reply_data = NULL;
    return;

 error:
    if (ret >= 0)
        fatal_protocol_error( current, "partial write %d\n", ret );
    else if (errno == EPIPE)
    {
#ifdef __PROSPERO__
        if (current->process->running_threads == 1)
            inprocess_trace( "thread %04x reply channel EPIPE\n", current->id );
#endif
        kill_thread( current, 0 );  /* normal death */
    }
    else
        fatal_protocol_error( current, "reply write: %s\n", strerror( errno ));
}

#ifdef __PROSPERO__
/* How busy the title's in-process server is, and with what: every 10 s of
 * activity, the requests handled, the time spent handling them, how many
 * waits (select) had to block, and the commonest request types. Wine's
 * server-side synchronization makes every wait and signal a request; this
 * says what a faster synchronization backend could save. */
static unsigned int census_count[REQ_NB_REQUESTS];
static unsigned long long census_us[REQ_NB_REQUESTS];
static unsigned int census_blocked;
static unsigned int census_direct, census_queued;
static unsigned long long census_max_us[REQ_NB_REQUESTS];
static unsigned int census_phase_count[CENSUS_PHASES];
static unsigned long long census_phase_us[CENSUS_PHASES], census_phase_max[CENSUS_PHASES];
static const char * const census_phase_name[CENSUS_PHASES] =
{
    "open", "open_create", "mkdir", "inode", "realpath", "sharing", "file_object", "park", "close", "unlink",
    "truncate"
};
static unsigned long long census_since;
struct request_queue_stat
{
    unsigned long long count, us, max, bins[32];
};
static struct request_queue_stat request_queue_stats[REQ_NB_REQUESTS];

static void request_queue_note( const union generic_request *req )
{
    struct request_queue_stat *stat;
    struct timespec now;
    unsigned long long started, ns, us;
    unsigned int bin = 0;

    if (!inprocess_wait_profile || !(started = wine_ps5_request_profile_started( req ))) return;
    clock_gettime( CLOCK_MONOTONIC, &now );
    ns = now.tv_sec * 1000000000ull + now.tv_nsec;
    if (ns < started) return;
    us = (ns - started) / 1000;
    stat = &request_queue_stats[req->request_header.req];
    while (bin < 31 && us > (1ull << bin)) bin++;
    stat->count++;
    stat->us += us;
    stat->bins[bin]++;
    if (us > stat->max) stat->max = us;
}

static unsigned long long request_queue_percentile( const struct request_queue_stat *stat, unsigned int pct )
{
    unsigned long long count = 0, target = (stat->count * pct + 99) / 100;
    unsigned int i;
    for (i = 0; i < 32; i++)
    {
        count += stat->bins[i];
        if (count >= target) return (1ull << i) < stat->max ? (1ull << i) : stat->max;
    }
    return stat->max;
}

static unsigned long long census_now_us(void)
{
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return now.tv_sec * 1000000ull + now.tv_nsec / 1000;
}

unsigned long long census_phase_start(void)
{
    return census_now_us();
}

void census_phase_note( enum census_phase phase, unsigned long long started, const char *name )
{
    static unsigned int slow_logged;
    int saved_errno = errno;
    unsigned long long took = census_now_us() - started;

    census_phase_count[phase]++;
    census_phase_us[phase] += took;
    if (took > census_phase_max[phase]) census_phase_max[phase] = took;
    if (took >= 20000 && slow_logged < 300)
    {
        slow_logged++;
        inprocess_trace( "server slow op=%s ms=%llu name=%s\n", census_phase_name[phase], took / 1000,
                         name ? name : "-" );
    }
    errno = saved_errno;
}

static void census_note( enum request req, unsigned long long started, unsigned int error, int direct )
{
    unsigned long long now = census_now_us(), total_us = 0;
    unsigned int i, j, total = 0, top[8] = { 0 }, tops = 0;
    char line[640];
    int len;

    if (req >= REQ_NB_REQUESTS) return;
    census_count[req]++;
    if (direct) census_direct++;
    else census_queued++;
    census_us[req] += now - started;
    if (now - started > census_max_us[req]) census_max_us[req] = now - started;
    if (req == REQ_select && error == STATUS_PENDING) census_blocked++;
    if (!census_since) census_since = now;
    if (now - census_since < 10000000) return;

    for (i = 0; i < REQ_NB_REQUESTS; i++)
    {
        if (!census_count[i]) continue;
        total += census_count[i];
        total_us += census_us[i];
        for (j = tops; j > 0 && census_count[top[j - 1]] < census_count[i]; j--)
            if (j < ARRAY_SIZE(top)) top[j] = top[j - 1];
        if (j < ARRAY_SIZE(top)) top[j] = i;
        if (tops < ARRAY_SIZE(top)) tops++;
    }
    len = snprintf( line, sizeof(line), "server requests window_ms=%llu total=%u direct=%u queued=%u handler_ms=%llu select_blocked=%u top",
                    (now - census_since) / 1000, total, census_direct, census_queued, total_us / 1000, census_blocked );
    for (j = 0; j < tops && len < (int)sizeof(line) - 48; j++)
        len += snprintf( line + len, sizeof(line) - len, " %s=%u/%llums", get_request_name( top[j] ),
                         census_count[top[j]], census_us[top[j]] / 1000 );
    inprocess_trace( "%s\n", line );
    {
        unsigned int by_time[5] = { 0 }, times = 0;

        for (i = 0; i < REQ_NB_REQUESTS; i++)
        {
            if (!census_us[i]) continue;
            for (j = times; j > 0 && census_us[by_time[j - 1]] < census_us[i]; j--)
                if (j < ARRAY_SIZE(by_time)) by_time[j] = by_time[j - 1];
            if (j < ARRAY_SIZE(by_time)) by_time[j] = i;
            if (times < ARRAY_SIZE(by_time)) times++;
        }
        len = snprintf( line, sizeof(line), "server time window_ms=%llu handler_ms=%llu top_ms",
                        (now - census_since) / 1000, total_us / 1000 );
        for (j = 0; j < times; j++)
            len += snprintf( line + len, sizeof(line) - len, " %s=%u/%llums/max%llums",
                             get_request_name( by_time[j] ), census_count[by_time[j]],
                             census_us[by_time[j]] / 1000, census_max_us[by_time[j]] / 1000 );
        inprocess_trace( "%s\n", line );
        len = snprintf( line, sizeof(line), "server ops window_ms=%llu", (now - census_since) / 1000 );
        for (j = 0; j < CENSUS_PHASES; j++)
        {
            if (!census_phase_count[j]) continue;
            len += snprintf( line + len, sizeof(line) - len, " %s=%u/%llums/max%llums", census_phase_name[j],
                             census_phase_count[j], census_phase_us[j] / 1000, census_phase_max[j] / 1000 );
        }
        inprocess_trace( "%s\n", line );
    }
    directory_entries_census_flush();
    for (i = 0; i < REQ_NB_REQUESTS; i++)
    {
        struct request_queue_stat *stat = &request_queue_stats[i];
        if (!stat->count) continue;
        inprocess_trace( "server queue window_ms=%llu type=%s count=%llu mean_us=%llu "
                         "p50_us=%llu p99_us=%llu max_us=%llu\n",
                         (now - census_since) / 1000, get_request_name( i ), stat->count,
                         stat->us / stat->count, request_queue_percentile( stat, 50 ),
                         request_queue_percentile( stat, 99 ), stat->max );
    }
    memset( request_queue_stats, 0, sizeof(request_queue_stats) );
    memset( census_count, 0, sizeof(census_count) );
    memset( census_us, 0, sizeof(census_us) );
    memset( census_max_us, 0, sizeof(census_max_us) );
    memset( census_phase_count, 0, sizeof(census_phase_count) );
    memset( census_phase_us, 0, sizeof(census_phase_us) );
    memset( census_phase_max, 0, sizeof(census_phase_max) );
    census_blocked = 0;
    census_direct = census_queued = 0;
    census_since = now;
}
#endif

/* call a request handler */
static void call_req_handler( struct thread *thread )
{
    union generic_reply reply;
    enum request req = thread->req.request_header.req;
#ifdef __PROSPERO__
    if (inprocess_server) request_queue_note( &thread->req );
#endif

    current = thread;
    current->reply_size = 0;
    clear_error();
    memset( &reply, 0, sizeof(reply) );

    if (debug_level) trace_request();

#ifdef __PROSPERO__
    if (inprocess_server && req == REQ_debug_process && current->req.debug_process_request.attach)
    {
        struct process *process = get_process_from_handle( current->req.debug_process_request.handle,
                                                          PROCESS_VM_READ | PROCESS_VM_WRITE |
                                                          PROCESS_SUSPEND_RESUME );
        if (process)
        {
            if (!inprocess_is_host_process( process )) set_error( STATUS_NOT_SUPPORTED );
            else req_handlers[req]( &current->req, &reply );
            release_object( process );
        }
    }
    else
#endif
#ifdef __PROSPERO__
    if (inprocess_server && req < REQ_NB_REQUESTS)
    {
        unsigned long long started = census_now_us();
        req_handlers[req]( &current->req, &reply );
        census_note( req, started, current ? current->error : 0, 0 );
    }
    else
#endif
    if (req < REQ_NB_REQUESTS)
        req_handlers[req]( &current->req, &reply );
    else
        set_error( STATUS_NOT_IMPLEMENTED );

    if (current)
    {
        if (current->reply_fd)
        {
            reply.reply_header.error = current->error;
            reply.reply_header.reply_size = current->reply_size;
            if (debug_level) trace_reply( req, &reply );
            send_reply( &reply );
        }
        else
        {
            current->exit_code = 1;
            kill_thread( current, 1 );  /* no way to continue without reply fd */
        }
    }
    current = NULL;
}

#ifdef __PROSPERO__
/* Deliberately small allowlist: these are the measured event-per-job
 * requests. Object creation uses ntdll-owned packed attributes, not an
 * application's name buffer. No process, file-open or debugger request may
 * execute here. Children cannot call across their process boundary. */
static int is_direct_request( enum request req )
{
    return req == REQ_create_event || req == REQ_event_op ||
           req == REQ_close_handle || req == REQ_select;
}

static void finish_direct_request( void *arg )
{
    struct thread *thread = arg;

    current = NULL;
    if (thread)
    {
        thread->req_data = NULL;  /* borrowed stack storage, never heap-owned */
        free( thread->reply_data );
        thread->reply_data = NULL;
        release_object( thread );
    }
    pthread_mutex_unlock( &inprocess_request_mutex );
}

/* 0: reply in info; 1: use the existing transport; 2: thread lost its reply
 * channel. The caller already masks server_block_set, including SIGQUIT.
 * A cleanup handler also releases the lock if a fatal title callback exits
 * this client thread while a handler is running. */
__attribute__((visibility("default")))
int wineserver_call_direct( unsigned int tid, client_ptr_t teb, struct __server_request_info *info )
{
    enum request req = info->u.req.request_header.req;
    data_size_t size = info->u.req.request_header.request_size, pos = 0;
    union { union generic_request alignment; char bytes[4096]; } data;
    union generic_reply reply;
    struct thread *thread;
    unsigned int i;
    unsigned long long started;
    int ret = 1;

    if (!WINE_PS5_DIRECT_REQUESTS || !inprocess_direct_enabled ||
        !__atomic_load_n( &inprocess_direct_ready, __ATOMIC_ACQUIRE ) ||
        !is_direct_request( req ) || size > sizeof(data.bytes) ||
        info->data_count > __SERVER_MAX_DATA) return 1;
    for (i = 0; i < info->data_count; i++)
    {
        if (info->data[i].size > size - pos) return 1;
        memcpy( data.bytes + pos, info->data[i].ptr, info->data[i].size );
        pos += info->data[i].size;
    }
    if (pos != size) return 1;

    /* Bounded spin as in the reference; a busy main loop eventually sleeps
     * in kqueue and lets both local direct callers and child requests run. */
    for (i = 0; i < 200; i++)
    {
        if (!pthread_mutex_trylock( &inprocess_request_mutex )) break;
#if defined(__i386__) || defined(__x86_64__)
        __asm__ __volatile__( "pause" );
#endif
    }
    if (i == 200) pthread_mutex_lock( &inprocess_request_mutex );
    thread = get_thread_from_id( tid );
    if (!inprocess_direct_ready || !thread || !inprocess_is_host_process( thread->process ) ||
        !teb || thread->teb != teb || thread->state == TERMINATED || !thread->reply_fd ||
        thread->req_toread || thread->reply_towrite || thread->req_data || thread->reply_data)
    {
        if (thread) release_object( thread );
        pthread_mutex_unlock( &inprocess_request_mutex );
        return 1;
    }
    if (req == REQ_close_handle)
    {
        struct event *event = get_event_obj( thread->process, info->u.req.close_handle_request.handle, 0 );

        /* Closing an arbitrary object can invoke debugger/process cleanup,
         * including callbacks into ntdll's virtual-memory lock. Only the
         * measured event handles are safe on an arbitrary client thread. */
        if (!event)
        {
            release_object( thread );
            pthread_mutex_unlock( &inprocess_request_mutex );
            return 1;
        }
        release_object( event );
    }
    pthread_cleanup_push( finish_direct_request, thread );
    {
        set_current_time();
        thread->req = info->u.req;
        thread->req_data = size ? data.bytes : NULL;
        current = thread;
        current->reply_size = 0;
        clear_error();
        memset( &reply, 0, sizeof(reply) );
        if (debug_level) trace_request();
        started = census_now_us();
        req_handlers[req]( &current->req, &reply );
        census_note( req, started, current ? current->error : 0, 1 );
        if (current && current->reply_fd)
        {
            reply.reply_header.error = current->error;
            reply.reply_header.reply_size = current->reply_size;
            if (debug_level) trace_reply( req, &reply );
            if (current->reply_size)
                memcpy( info->reply_data, current->reply_data, current->reply_size );
            info->u.reply = reply;
            ret = 0;
        }
        else
        {
            if (current)
            {
                current->exit_code = 1;
                kill_thread( current, 1 );
            }
            ret = 2;
        }
    }
    pthread_cleanup_pop( 1 );
    return ret;
}
#endif

/* read a request from a thread */
void read_request( struct thread *thread )
{
    int ret;

    if (!thread->req_toread)  /* no pending request */
    {
        if ((ret = server_channel_recv( get_unix_fd( thread->request_fd ), &thread->req,
                         sizeof(thread->req) )) != sizeof(thread->req)) goto error;
        if (!(thread->req_toread = thread->req.request_header.request_size))
        {
            /* no data, handle request at once */
            call_req_handler( thread );
            return;
        }
        if (!(thread->req_data = malloc( thread->req_toread )))
        {
            fatal_protocol_error( thread, "no memory for %u bytes request %d\n",
                                  thread->req_toread, thread->req.request_header.req );
            return;
        }
    }

    /* read the variable sized data */
    for (;;)
    {
        ret = server_channel_recv( get_unix_fd( thread->request_fd ),
                    (char *)thread->req_data + thread->req.request_header.request_size
                      - thread->req_toread,
                    thread->req_toread );
        if (ret <= 0) break;
        if (!(thread->req_toread -= ret))
        {
            call_req_handler( thread );
            free( thread->req_data );
            thread->req_data = NULL;
            return;
        }
    }

error:
    if (!ret)  /* closed pipe */
    {
#ifdef __PROSPERO__
        /* orderly worker exits close this too; only the last thread ends the client */
        if (thread->state != TERMINATED && thread->process->running_threads == 1)
            inprocess_trace( "thread %04x request channel EOF\n", thread->id );
#endif
        kill_thread( thread, 0 );
    }
    else if (ret > 0)
        fatal_protocol_error( thread, "partial read %d\n", ret );
    else if (errno != EWOULDBLOCK && (EWOULDBLOCK == EAGAIN || errno != EAGAIN))
        fatal_protocol_error( thread, "read: %s\n", strerror( errno ));
}

/* receive a file descriptor on the process socket */
int receive_fd( struct process *process )
{
    struct iovec vec;
    struct send_fd data;
    struct msghdr msghdr;
    char cmsg_buffer[256];
    int fd = -1, ret;

    msghdr.msg_name    = NULL;
    msghdr.msg_namelen = 0;
    msghdr.msg_iov     = &vec;
    msghdr.msg_iovlen  = 1;
    msghdr.msg_control = cmsg_buffer;
    msghdr.msg_controllen = sizeof(cmsg_buffer);
    msghdr.msg_flags   = 0;

    vec.iov_base = (void *)&data;
    vec.iov_len  = sizeof(data);

    ret = recvmsg( get_unix_fd( process->msg_fd ), &msghdr,
#ifdef __PROSPERO__
                   MSG_DONTWAIT
#else
                   0
#endif
                   );

    if (ret > 0)
    {
        struct cmsghdr *cmsg;
        for (cmsg = CMSG_FIRSTHDR( &msghdr ); cmsg; cmsg = CMSG_NXTHDR( &msghdr, cmsg ))
        {
            if (cmsg->cmsg_level != SOL_SOCKET) continue;
            if (cmsg->cmsg_type == SCM_RIGHTS) fd = *(int *)CMSG_DATA(cmsg);
        }
    }

    if (ret == sizeof(data))
    {
        struct thread *thread;

        if (data.tid) thread = get_thread_from_id( data.tid );
        else thread = (struct thread *)grab_object( get_process_first_thread( process ));

        if (!thread || thread->process != process || thread->state == TERMINATED)
        {
            if (debug_level)
                fprintf( stderr, "%04x: *fd* %d <- %d bad thread id\n",
                         data.tid, data.fd, fd );
            close( fd );
        }
        else
        {
            if (debug_level)
                fprintf( stderr, "%04x: *fd* %d <- %d\n",
                         thread->id, data.fd, fd );
            thread_add_inflight_fd( thread, data.fd, fd );
        }
        if (thread) release_object( thread );
        return 0;
    }

    if (!ret)
    {
#ifdef __PROSPERO__
        inprocess_trace( "process %04x socket EOF\n", process->id );
#endif
        kill_process( process, 0 );
    }
    else if (ret > 0)
    {
#ifdef __PROSPERO__
        inprocess_trace( "process %04x partial recvmsg %d for fd\n", process->id, ret );
#endif
        fprintf( stderr, "Protocol error: process %04x: partial recvmsg %d for fd\n",
                 process->id, ret );
        if (fd != -1) close( fd );
        kill_process( process, 1 );
    }
    else
    {
        if (errno != EWOULDBLOCK && (EWOULDBLOCK == EAGAIN || errno != EAGAIN))
        {
#ifdef __PROSPERO__
            inprocess_trace( "process %04x recvmsg: %s\n", process->id, strerror( errno ));
#endif
            fprintf( stderr, "Protocol error: process %04x: ", process->id );
            perror( "recvmsg" );
            kill_process( process, 1 );
        }
    }
    return -1;
}

/* send an fd to a client */
int send_client_fd( struct process *process, int fd, obj_handle_t handle )
{
    struct iovec vec;
    struct msghdr msghdr;
    char cmsg_buffer[256];
    struct cmsghdr *cmsg;
    int ret;

    msghdr.msg_name    = NULL;
    msghdr.msg_namelen = 0;
    msghdr.msg_iov     = &vec;
    msghdr.msg_iovlen  = 1;
    msghdr.msg_control = cmsg_buffer;
    msghdr.msg_controllen = sizeof(cmsg_buffer);
    msghdr.msg_flags   = 0;

    vec.iov_base = (void *)&handle;
    vec.iov_len  = sizeof(handle);

    cmsg = CMSG_FIRSTHDR( &msghdr );
    cmsg->cmsg_len   = CMSG_LEN( sizeof(fd) );
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    *(int *)CMSG_DATA(cmsg) = fd;
    msghdr.msg_controllen = cmsg->cmsg_len;

    if (debug_level)
        fprintf( stderr, "%04x: *fd* %04x -> %d\n", current ? current->id : process->id, handle, fd );

    ret = sendmsg( get_unix_fd( process->msg_fd ), &msghdr,
#ifdef __PROSPERO__
                   MSG_DONTWAIT
#else
                   0
#endif
                   );

    if (ret == sizeof(handle)) return 0;

    if (ret >= 0)
    {
#ifdef __PROSPERO__
        inprocess_trace( "process %04x partial sendmsg %d\n", process->id, ret );
#endif
        fprintf( stderr, "Protocol error: process %04x: partial sendmsg %d\n", process->id, ret );
        kill_process( process, 1 );
    }
    else if (errno == EPIPE)
    {
#ifdef __PROSPERO__
        inprocess_trace( "process %04x sendmsg EPIPE\n", process->id );
#endif
        kill_process( process, 0 );
    }
    else
    {
#ifdef __PROSPERO__
        inprocess_trace( "process %04x sendmsg: %s\n", process->id, strerror( errno ));
#endif
        fprintf( stderr, "Protocol error: process %04x: ", process->id );
        perror( "sendmsg" );
        kill_process( process, 1 );
    }
    return -1;
}

/* return a monotonic time counter */
timeout_t monotonic_counter(void)
{
#ifdef __APPLE__
    static mach_timebase_info_data_t timebase;

    if (!timebase.denom) mach_timebase_info( &timebase );
    return mach_continuous_time() * timebase.numer / timebase.denom / 100;
#elif defined(HAVE_CLOCK_GETTIME)
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    if (!clock_gettime( CLOCK_MONOTONIC_RAW, &ts ))
        return (timeout_t)ts.tv_sec * TICKS_PER_SEC + ts.tv_nsec / 100;
#endif
    if (!clock_gettime( CLOCK_MONOTONIC, &ts ))
        return (timeout_t)ts.tv_sec * TICKS_PER_SEC + ts.tv_nsec / 100;
#endif
    return current_time - server_start_time;
}

static void master_socket_dump( struct object *obj, int verbose )
{
    struct master_socket *sock = (struct master_socket *)obj;
    assert( obj->ops == &master_socket_ops );
    fprintf( stderr, "Master socket fd=%p\n", sock->fd );
}

static void master_socket_destroy( struct object *obj )
{
    struct master_socket *sock = (struct master_socket *)obj;
    assert( obj->ops == &master_socket_ops );
    release_object( sock->fd );
}

/* handle a socket event */
static void master_socket_poll_event( struct fd *fd, int event )
{
    struct master_socket *sock = get_fd_user( fd );
    assert( master_socket->obj.ops == &master_socket_ops );

    assert( sock == master_socket );  /* there is only one master socket */

    if (event & (POLLERR | POLLHUP))
    {
        /* this is not supposed to happen */
        fprintf( stderr, "wineserver: Error on master socket\n" );
        set_fd_events( sock->fd, -1 );
    }
    else if (event & POLLIN)
    {
        struct process *process;
        struct sockaddr_un dummy;
        socklen_t len = sizeof(dummy);
        struct thread *thread = NULL;

        int client = accept( get_unix_fd( master_socket->fd ), (struct sockaddr *) &dummy, &len );
        if (client == -1) return;
#ifndef __PROSPERO__
        fcntl( client, F_SETFL, O_NONBLOCK );
#endif
#ifdef SO_PEERCRED
        {
            struct ucred ucred;

            len = sizeof(ucred);
            if (!getsockopt( client, SOL_SOCKET, SO_PEERCRED, &ucred, &len ))
            {
                pid_t ppid;
                char s[256];
                FILE *f;

                sprintf( s, "/proc/%d/status", ucred.pid );
                if ((f = fopen( s, "r" )))
                {
                    while (fgets( s, sizeof(s), f ))
                    {
                        if (sscanf( s, "PPid:%d", &ppid ) != 1) continue;
                        thread = get_thread_from_pid( ppid );
                        break;
                    }
                    fclose( f );
                }
            }
        }
#endif
        if (thread) clear_error();
        if ((process = create_process( client, thread ? thread->process : NULL, 0, NULL, NULL, NULL, 0, NULL )))
        {
            create_thread( -1, process, NULL );
            release_object( process );
        }
    }
}

/* remove the socket upon exit */
static void socket_cleanup(void)
{
    static int do_it_once;
    if (!do_it_once++) unlink( server_socket_name );
}

/* create a directory and check its permissions */
static void create_dir( const char *name, struct stat *st )
{
    if (lstat( name, st ) == -1)
    {
        if (errno != ENOENT)
            fatal_error( "lstat %s: %s\n", name, strerror( errno ));
        if (mkdir( name, 0700 ) == -1 && errno != EEXIST)
            fatal_error( "mkdir %s: %s\n", name, strerror( errno ));
        if (lstat( name, st ) == -1)
            fatal_error( "lstat %s: %s\n", name, strerror( errno ));
    }
    if (!S_ISDIR(st->st_mode)) fatal_error( "%s is not a directory\n", name );
    if (!OWNED_BY_ME(*st)) fatal_error( "%s is not owned by you\n", name );
    if (!PRIVATE_MODE(*st)) fatal_error( "%s must not be accessible by other users\n", name );
}

/* create the server directory and chdir to it */
static char *create_server_dir( int force )
{
    const char *prefix = getenv( "WINEPREFIX" );
    char *p, *config_dir, *base_dir;
    struct stat st, st2;

    /* open the configuration directory */

    if (prefix)
    {
        if (!(config_dir = strdup( prefix ))) fatal_error( "out of memory\n" );
        for (p = config_dir + strlen(config_dir); p > config_dir; p--) if (p[-1] != '/') break;
        if (p > config_dir) *p = 0;
        if (config_dir[0] != '/')
            fatal_error( "invalid directory %s in WINEPREFIX: not an absolute path\n", prefix );
    }
    else
    {
        const char *home = getenv( "HOME" );
        if (!home)
        {
            struct passwd *pwd = getpwuid( getuid() );
            if (pwd) home = pwd->pw_dir;
        }
        if (!home) fatal_error( "could not determine your home directory\n" );
        if (home[0] != '/') fatal_error( "your home directory %s is not an absolute path\n", home );
        if (!(config_dir = malloc( strlen(home) + sizeof("/.wine") ))) fatal_error( "out of memory\n" );
        strcpy( config_dir, home );
        for (p = config_dir + strlen(config_dir); p > config_dir; p--) if (p[-1] != '/') break;
        strcpy( p, "/.wine" );
    }

    if (chdir( config_dir ) == -1)
    {
        if (errno != ENOENT || force) fatal_error( "chdir to %s: %s\n", config_dir, strerror( errno ));
        return NULL;
    }
    if ((config_dir_fd = open( ".", O_RDONLY )) == -1)
        fatal_error( "open %s: %s\n", config_dir, strerror( errno ));
    if (fstat( config_dir_fd, &st ) == -1)
        fatal_error( "stat %s: %s\n", config_dir, strerror( errno ));
    if (!OWNED_BY_ME(st))
        fatal_error( "%s is not owned by you\n", config_dir );

    /* create the base directory if needed */

#ifdef __ANDROID__  /* there's no /tmp dir on Android */
    if (asprintf( &base_dir, "%s/.wineserver", config_dir ) == -1)
        fatal_error( "out of memory\n" );
#elif defined(__PROSPERO__)  /* the PS5 has no /tmp, and binds AF_UNIX sockets only in /system_tmp */
    if (asprintf( &base_dir, "/system_tmp/.wine-%u", getuid() ) == -1)
        fatal_error( "out of memory\n" );
#else
    if (asprintf( &base_dir, "/tmp/.wine-%u", getuid() ) == -1)
        fatal_error( "out of memory\n" );
#endif
    create_dir( base_dir, &st2 );

    /* now create the server directory */

    if (asprintf( &server_dir, "%s/server-%llx-%llx", base_dir,
                  (unsigned long long)st.st_dev, (unsigned long long)st.st_ino ) == -1)
        fatal_error( "out of memory\n" );

    create_dir( server_dir, &st );

    if (chdir( server_dir ) == -1)
        fatal_error( "chdir %s: %s\n", server_dir, strerror( errno ));
    if ((server_dir_fd = open( ".", O_RDONLY )) == -1)
        fatal_error( "open %s: %s\n", server_dir, strerror( errno ));
    if (fstat( server_dir_fd, &st2 ) == -1)
        fatal_error( "stat %s: %s\n", server_dir, strerror( errno ));
    if (st.st_dev != st2.st_dev || st.st_ino != st2.st_ino)
        fatal_error( "chdir did not end up in %s\n", server_dir );

    free( base_dir );
    free( config_dir );
    return server_dir;
}

/* create the lock file and return its file descriptor */
static int create_server_lock(void)
{
    struct stat st;
    int fd;

    if (lstat( server_lock_name, &st ) == -1)
    {
        if (errno != ENOENT)
            fatal_error( "lstat %s/%s: %s\n", server_dir, server_lock_name, strerror( errno ));
    }
    else
    {
        if (!S_ISREG(st.st_mode))
            fatal_error( "%s/%s is not a regular file\n", server_dir, server_lock_name );
    }

    if ((fd = open( server_lock_name, O_CREAT|O_TRUNC|O_WRONLY, 0600 )) == -1)
        fatal_error( "error creating %s/%s: %s\n", server_dir, server_lock_name, strerror( errno ));
    return fd;
}

/* wait for the server lock */
int wait_for_lock(void)
{
    int fd, r;
    struct flock fl;

    server_dir = create_server_dir( 0 );
    if (!server_dir) return 0;  /* no server dir, so no lock to wait on */

    fd = create_server_lock();

    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 1;
    r = fcntl( fd, F_SETLKW, &fl );
    close(fd);

    return r;
}

/* kill the wine server holding the lock */
int kill_lock_owner( int sig )
{
    int fd, i, ret = 0;
    pid_t pid = 0;
    struct flock fl;

    server_dir = create_server_dir( 0 );
    if (!server_dir) return 0;  /* no server dir, nothing to do */

    fd = create_server_lock();

    for (i = 1; i <= 20; i++)
    {
        fl.l_type   = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start  = 0;
        fl.l_len    = 1;
        if (fcntl( fd, F_GETLK, &fl ) == -1) goto done;
        if (fl.l_type != F_WRLCK) goto done;  /* the file is not locked */
        if (!pid)  /* first time around */
        {
            if (!(pid = fl.l_pid)) goto done;  /* shouldn't happen */
            if (sig == -1)
            {
                if (kill( pid, SIGINT ) == -1) goto done;
                kill( pid, SIGCONT );
                ret = 1;
            }
            else  /* just send the specified signal and return */
            {
                ret = (kill( pid, sig ) != -1);
                goto done;
            }
        }
        else if (fl.l_pid != pid) goto done;  /* no longer the same process */
        usleep( 50000 * i );
    }
    /* waited long enough, now kill it */
    kill( pid, SIGKILL );

 done:
    close( fd );
    return ret;
}

/* acquire the main server lock */
static void acquire_lock(void)
{
    struct sockaddr_un addr;
    struct stat st;
    struct flock fl;
    int fd, slen, got_lock = 0;
#ifdef __PROSPERO__
    int backlog = inprocess_server ? SOMAXCONN : 5;
#else
    int backlog = 5;
#endif

    fd = create_server_lock();

    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 1;
    if (fcntl( fd, F_SETLK, &fl ) != -1)
    {
        /* check for crashed server */
        if (stat( server_socket_name, &st ) != -1 &&   /* there is a leftover socket */
            stat( "core", &st ) != -1 && st.st_size)   /* and there is a non-empty core file */
        {
            fprintf( stderr,
                     "Warning: a previous instance of the wine server seems to have crashed.\n"
                     "Please run 'gdb %s %s/core',\n"
                     "type 'backtrace' at the gdb prompt and report the results. Thanks.\n\n",
                     server_argv0, server_dir );
        }
        unlink( server_socket_name ); /* we got the lock, we can safely remove the socket */
        got_lock = 1;
        /* in that case we reuse fd without closing it, this ensures
         * that we hold the lock until the process exits */
    }
    else
    {
        switch(errno)
        {
        case ENOLCK:
            break;
        case EACCES:
            /* check whether locks work at all on this file system */
            if (fcntl( fd, F_GETLK, &fl ) == -1) break;
            /* fall through */
        case EAGAIN:
            exit(2); /* we didn't get the lock, exit with special status */
        default:
            fatal_error( "fcntl %s/%s: %s\n", server_dir, server_lock_name, strerror( errno ));
        }
        /* it seems we can't use locks on this fs, so we will use the socket existence as lock */
        close( fd );
    }

    if ((fd = socket( AF_UNIX, SOCK_STREAM, 0 )) == -1) fatal_error( "socket: %s\n", strerror( errno ));
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, server_socket_name );
    slen = sizeof(addr) - sizeof(addr.sun_path) + strlen(addr.sun_path) + 1;
#ifdef HAVE_STRUCT_SOCKADDR_UN_SUN_LEN
    addr.sun_len = slen;
#endif
    if (bind( fd, (struct sockaddr *)&addr, slen ) == -1)
    {
        if ((errno == EEXIST) || (errno == EADDRINUSE))
        {
            if (got_lock)
                fatal_error( "couldn't bind to the socket even though we hold the lock\n" );
            exit(2); /* we didn't get the lock, exit with special status */
        }
        fatal_error( "bind: %s\n", strerror( errno ));
    }
    atexit( socket_cleanup );
    chmod( server_socket_name, 0600 );  /* make sure no other user can connect */
    if (listen( fd, backlog ) == -1) fatal_error( "listen: %s\n", strerror( errno ));
#ifdef __PROSPERO__
    if (inprocess_server) inprocess_trace( "master socket listen backlog=%d\n", backlog );
#endif

    if (!(master_socket = alloc_object( &master_socket_ops )) ||
        !(master_socket->fd = create_anonymous_fd( &master_socket_fd_ops, fd, &master_socket->obj, 0 )))
        fatal_error( "out of memory\n" );
    set_fd_events( master_socket->fd, POLLIN );
    make_object_permanent( &master_socket->obj );
}

/* open the master server socket and start waiting for new clients */
void open_master_socket(void)
{
    int fd, pid, status, sync_pipe[2];
    char dummy;

    /* make sure no request is larger than the maximum size */
    assert( sizeof(union generic_request) == sizeof(struct request_max_size) );
    assert( sizeof(union generic_reply) == sizeof(struct request_max_size) );

    /* make sure the stdio fds are open */
    fd = open( "/dev/null", O_RDWR );
    while (fd >= 0 && fd <= 2) fd = dup( fd );

    server_dir = create_server_dir( 1 );

    if (!foreground)
    {
        if (pipe( sync_pipe ) == -1) fatal_error( "pipe: %s\n", strerror( errno ));
        pid = fork();
        switch( pid )
        {
        case 0:  /* child */
            setsid();
#ifdef __PROSPERO__
            ps5_note_pid();
#endif
            close( sync_pipe[0] );

            acquire_lock();

            /* close stdin and stdout */
            dup2( fd, 0 );
            dup2( fd, 1 );

            /* signal parent */
            dummy = 0;
            write( sync_pipe[1], &dummy, 1 );
            close( sync_pipe[1] );
            break;

        case -1:
            fatal_error( "fork: %s\n", strerror( errno ));
            break;

        default:  /* parent */
            close( sync_pipe[1] );

            /* wait for child to signal us and then exit */
            if (read( sync_pipe[0], &dummy, 1 ) == 1) _exit(0);

            /* child terminated, propagate exit status */
            waitpid( pid, &status, 0 );
            if (WIFEXITED(status)) _exit( WEXITSTATUS(status) );
            _exit(1);
        }
    }
    else  /* remain in the foreground */
    {
#ifdef __PROSPERO__
        /* The console starts the server with -f (ntdll's start_server): it
         * is already a fresh process, and a forked child there dies when a
         * caught signal is delivered. Detach as the forked child did. */
        setsid();
        dup2( fd, 0 );
        dup2( fd, 1 );
#endif
        acquire_lock();
    }

    /* init the process tracing mechanism */
    init_tracing_mechanism();
    close( fd );
}

/* master socket timer expiration handler */
static void close_socket_timeout( void *arg )
{
    master_timeout = NULL;
#ifdef __PROSPERO__
    if (inprocess_server)
    {
        /* Only host death schedules this in an in-process session. A child
         * disconnect never ends the parent server. The first requested
         * status wins, so the host's own exit is kept. */
        inprocess_trace( "host process stopped; requesting title exit\n" );
        inprocess_exit( 1 );
    }
#endif
    flush_registry();
    if (debug_level) fprintf( stderr, "wineserver: exiting (pid=%ld)\n", (long) getpid() );

#ifdef DEBUG_OBJECTS
    close_objects();  /* shut down everything properly */
#endif
    exit( 0 );
}

/* close the master socket and stop waiting for new clients */
void close_master_socket( timeout_t timeout )
{
    if (master_socket)
    {
        release_object( master_socket );
        master_socket = NULL;
    }
    if (master_timeout)  /* cancel previous timeout */
        remove_timeout_user( master_timeout );

    master_timeout = add_timeout_user( timeout, close_socket_timeout, NULL );
}
