/*
 * Wine server requests
 *
 * Copyright (C) 1999 Alexandre Julliard
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

#ifndef __WINE_SERVER_REQUEST_H
#define __WINE_SERVER_REQUEST_H

#include <assert.h>

#include "thread.h"
#include "wine/server_protocol.h"

/* max request length */
#define MAX_REQUEST_LENGTH  8192

/* request handler definition */
#define DECL_HANDLER(name) \
    void req_##name( const struct name##_request *req, struct name##_reply *reply )

/* request functions */

#ifdef __GNUC__
extern void fatal_protocol_error( struct thread *thread,
                                  const char *err, ... ) __attribute__((format (printf,2,3)));
extern void fatal_error( const char *err, ... )  __attribute__((noreturn,format(printf,1,2)));
#else
extern void fatal_protocol_error( struct thread *thread, const char *err, ... );
extern void fatal_error( const char *err, ... );
#endif

extern const char *get_config_dir(void);
extern void *set_reply_data_size( data_size_t size );
extern const struct object_attributes *get_req_object_attributes( const struct security_descriptor **sd,
                                                                  struct unicode_str *name,
                                                                  struct object **root );
extern const void *get_req_data_after_objattr( const struct object_attributes *attr, data_size_t *len );
extern int receive_fd( struct process *process );
extern int send_client_fd( struct process *process, int fd, obj_handle_t handle );
extern void read_request( struct thread *thread );
extern void write_reply( struct thread *thread );
extern timeout_t monotonic_counter(void);
extern void open_master_socket(void);
extern void close_master_socket( timeout_t timeout );
extern void shutdown_master_socket(void);
extern int wait_for_lock(void);
extern int kill_lock_owner( int sig );
extern char *server_dir;
extern int server_dir_fd, config_dir_fd;

#ifdef __PROSPERO__
#include "wine/ps5_title_private.h"
#include <pthread.h>
#include "wine/server.h"
#ifndef WINE_PS5_DIRECT_REQUESTS
#define WINE_PS5_DIRECT_REQUESTS 1
#endif
extern pthread_mutex_t inprocess_request_mutex;
extern int inprocess_direct_enabled, inprocess_direct_ready;
extern void inprocess_wake_server(void);
extern int inprocess_server;
extern int inprocess_wait_profile;
extern int inprocess_supervised;
extern unsigned int inprocess_desktop_requests;
extern const char *inprocess_config_dir;
extern const char *inprocess_nls_dir;
extern size_t (*inprocess_read_memory)( const void *, void *, size_t );
extern unsigned int (*inprocess_write_memory)( void *, const void *, size_t );
extern int (*inprocess_signal_thread)( unsigned int, void *, int );
extern int (*inprocess_signal_child_thread)( int, unsigned int, void *, int );
extern int (*inprocess_terminate_child)( int );
struct process;
extern int inprocess_is_host_process( const struct process *process );
extern void inprocess_exit( int status ) __attribute__((noreturn));
extern void inprocess_trace( const char *format, ... ) __attribute__((format(printf,1,2)));
#endif

extern void trace_request(void);
extern void trace_reply( enum request req, const union generic_reply *reply );
extern const char *get_request_name( enum request req );

/* get current tick count to return to client */
static inline unsigned int get_tick_count(void)
{
    return monotonic_counter() / 10000;
}

/* get the request vararg data */
static inline const void *get_req_data(void)
{
    return current->req_data;
}

/* get the request vararg size */
static inline data_size_t get_req_data_size(void)
{
    return current->req.request_header.request_size;
}

/* get the request vararg as unicode string */
static inline struct unicode_str get_req_unicode_str(void)
{
    struct unicode_str ret;
    ret.str = get_req_data();
    ret.len = (get_req_data_size() / sizeof(WCHAR)) * sizeof(WCHAR);
    return ret;
}

/* get the reply maximum vararg size */
static inline data_size_t get_reply_max_size(void)
{
    return current->req.request_header.reply_size;
}

/* allocate and fill the reply data */
static inline void *set_reply_data( const void *data, data_size_t size )
{
    void *ret = set_reply_data_size( size );
    if (ret) memcpy( ret, data, size );
    return ret;
}

/* set the reply data pointer directly (will be freed by request code) */
static inline void set_reply_data_ptr( void *data, data_size_t size )
{
    assert( size <= get_reply_max_size() );
    current->reply_size = size;
    current->reply_data = data;
}

#endif  /* __WINE_SERVER_REQUEST_H */
