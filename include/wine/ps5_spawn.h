/*
 * Process start-up on the PS5: a spawn broker in place of execve
 *
 * Copyright 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The console's execve and posix_spawn refuse every file (ENOEXEC), and
 * dlopen fails in a forked child. Forking a running Wine process has also
 * panicked the kernel. A fresh process comes only from the ELF loader on
 * 127.0.0.1:9021, which runs the ELF it is sent with no arguments, no
 * environment and no descriptors of the sender.
 *
 * So the first Wine process starts a broker through the ELF loader; nothing
 * forks. To start a process, Wine sends the broker the loader's path, the
 * argument and environment strings, cwd and the descriptors the process
 * must find (by number); the broker queues the request and sends the loader
 * to port 9021. The loader, started with no arguments, asks the broker for the
 * oldest request and becomes that process. A request whose argv[0] ends in
 * "wineserver" runs the server, from wineserver.so beside that path.
 */

#ifndef __WINE_WINE_PS5_SPAWN_H
#define __WINE_WINE_PS5_SPAWN_H

#ifdef __PROSPERO__

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#define PS5_SPAWN_MAGIC   0x5053504e
#ifndef PS5_WINE_ROOT
#error "PS5_WINE_ROOT is not defined"
#endif
#define PS5_TITLE_WINE_ROOT "/app0/wine"

#define PS5_SPAWN_MAX_FDS 8

enum ps5_spawn_op
{
    PS5_SPAWN_REQUEST = 1, /* a process to start: header, strings, descriptors */
    PS5_SPAWN_PULL    = 2, /* a new process asks for its request */
};

struct ps5_spawn_message
{
    uint32_t magic;
    uint32_t op;
    uint32_t argc;
    uint32_t envc;
    uint32_t nfds;          /* descriptors sent: the working directory, then targets */
    uint32_t blob_size;     /* loader path, argv, then envp strings, each NUL-terminated */
    int32_t  target_fds[PS5_SPAWN_MAX_FDS]; /* number each descriptor gets in the new process */
};

static inline void ps5_spawn_path( char *buf, size_t size )
{
    snprintf( buf, size, "/system_tmp/.wine-%u/spawn", (unsigned int)getuid() );
}

static inline int ps5_spawn_write_all( int fd, const void *data, size_t size )
{
    const char *p = data;
    while (size)
    {
        ssize_t ret = write( fd, p, size );
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return -1;
        p += ret;
        size -= ret;
    }
    return 0;
}
/* Every Wine process on the console (loaders, the broker, the server) leaves
 * its pid here, so that ps5/probes/winekill can end a session: none of them
 * is a child of anything the host can reach. The file stays empty:
 * /system_tmp is a 256 KiB tmpfs of 16 KiB blocks shared with the system,
 * and a file with content takes a block; labelled pid files filled it after
 * 15 processes (2026-10-03). */
static inline void ps5_note_pid(void)
{
    char path[96];
    int fd;

    snprintf( path, sizeof(path), "/system_tmp/.wine-%u", (unsigned int)getuid() );
    mkdir( path, 0700 );
    snprintf( path, sizeof(path), "/system_tmp/.wine-%u/pids", (unsigned int)getuid() );
    mkdir( path, 0700 );
    snprintf( path, sizeof(path), "/system_tmp/.wine-%u/pids/%d", (unsigned int)getuid(), (int)getpid() );
    if ((fd = open( path, O_WRONLY | O_CREAT | O_TRUNC, 0600 )) >= 0) close( fd );
}

/* The broker and the desktop hub also leave their pid here: they outlive
 * the session's processes, so ps5/tools/ptguard does not count them as the
 * session (winekill still ends them through pids/). */
static inline void ps5_note_helper_pid(void)
{
    char path[96];
    int fd;

    snprintf( path, sizeof(path), "/system_tmp/.wine-%u/helpers", (unsigned int)getuid() );
    mkdir( path, 0700 );
    snprintf( path, sizeof(path), "/system_tmp/.wine-%u/helpers/%d", (unsigned int)getuid(), (int)getpid() );
    if ((fd = open( path, O_WRONLY | O_CREAT | O_TRUNC, 0600 )) >= 0) close( fd );
}


static inline int ps5_spawn_read_all( int fd, void *data, size_t size )
{
    char *p = data;
    while (size)
    {
        ssize_t ret = read( fd, p, size );
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return -1;
        p += ret;
        size -= ret;
    }
    return 0;
}

static inline int ps5_spawn_connect(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd = socket( AF_UNIX, SOCK_STREAM, 0 );

    if (fd < 0) return -1;
    ps5_spawn_path( addr.sun_path, sizeof(addr.sun_path) );
    if (connect( fd, (struct sockaddr *)&addr, sizeof(addr) ) < 0)
    {
        close( fd );
        return -1;
    }
    return fd;
}

/* Send a message with its strings and descriptors. */
static inline int ps5_spawn_send( int sock, struct ps5_spawn_message *msg, const char *blob,
                                  const int *fds, int nfds )
{
    char control[CMSG_SPACE( sizeof(int) * (PS5_SPAWN_MAX_FDS + 1) )];
    struct msghdr mh = { 0 };
    struct iovec iov = { msg, sizeof(*msg) };

    msg->magic = PS5_SPAWN_MAGIC;
    msg->nfds = nfds;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (nfds)
    {
        struct cmsghdr *cmsg;
        mh.msg_control = control;
        mh.msg_controllen = CMSG_SPACE( sizeof(int) * nfds );
        cmsg = CMSG_FIRSTHDR( &mh );
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN( sizeof(int) * nfds );
        memcpy( CMSG_DATA( cmsg ), fds, sizeof(int) * nfds );
    }
    if (sendmsg( sock, &mh, 0 ) != sizeof(*msg)) return -1;
    return ps5_spawn_write_all( sock, blob, msg->blob_size );
}

/* Receive a message; *blob is malloc'ed (NUL-terminated past blob_size). */
static inline int ps5_spawn_recv( int sock, struct ps5_spawn_message *msg, char **blob,
                                  int *fds, int *nfds )
{
    char control[CMSG_SPACE( sizeof(int) * (PS5_SPAWN_MAX_FDS + 1) )];
    struct msghdr mh = { 0 };
    struct iovec iov = { msg, sizeof(*msg) };
    struct cmsghdr *cmsg;

    *nfds = 0;
    *blob = NULL;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = control;
    mh.msg_controllen = sizeof(control);
    if (recvmsg( sock, &mh, 0 ) != sizeof(*msg) || msg->magic != PS5_SPAWN_MAGIC) return -1;
    for (cmsg = CMSG_FIRSTHDR( &mh ); cmsg; cmsg = CMSG_NXTHDR( &mh, cmsg ))
    {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        *nfds = (cmsg->cmsg_len - CMSG_LEN( 0 )) / sizeof(int);
        memcpy( fds, CMSG_DATA( cmsg ), *nfds * sizeof(int) );
    }
    if (msg->blob_size > (16 << 20) || !(*blob = malloc( msg->blob_size + 1 ))) return -1;
    (*blob)[msg->blob_size] = 0;
    return ps5_spawn_read_all( sock, *blob, msg->blob_size );
}

/* A title sees its installed tree at /app0, while payload processes see the
 * same files under PS5_WINE_ROOT. Rewrite only a complete path value, either
 * the whole string or the value after NAME= in an environment entry. */
static inline const char *ps5_spawn_title_path( const char *string, size_t *head )
{
    const char *value = strchr( string, '=' );
    const size_t length = sizeof(PS5_TITLE_WINE_ROOT) - 1;

    if (value) value++;
    else value = string;
    *head = value - string;
    if (strncmp( value, PS5_TITLE_WINE_ROOT, length )) return NULL;
    if (value[length] && value[length] != '/') return NULL;
    return value + length;
}

static inline size_t ps5_spawn_string_size( const char *string )
{
    size_t head;
    const char *tail = ps5_spawn_title_path( string, &head );

    if (!tail) return strlen( string ) + 1;
    return head + sizeof(PS5_WINE_ROOT) - 1 + strlen( tail ) + 1;
}

static inline char *ps5_spawn_copy_string( char *dest, const char *string )
{
    size_t head;
    const char *tail = ps5_spawn_title_path( string, &head );

    if (!tail) return stpcpy( dest, string ) + 1;
    memcpy( dest, string, head );
    dest += head;
    dest = stpcpy( dest, PS5_WINE_ROOT );
    return stpcpy( dest, tail ) + 1;
}

/* Start loader with argv and envp in a new process, in the directory cwd
 * (the current one if -1). fds[i] becomes descriptor number targets[i] there
 * (fds[i] itself if targets is NULL); descriptors that are -1 or closed are
 * left out, as across execve. The calling process is not changed. Returns 0
 * once the broker has sent the loader to the console's ELF loader, or -1 with
 * errno set. */
static inline int ps5_spawn( const char *loader, char * const *argv, char * const *envp,
                             int cwd, const int *fds, const int *targets, int nfds )
{
    struct ps5_spawn_message msg = { .op = PS5_SPAWN_REQUEST };
    int sent_fds[PS5_SPAWN_MAX_FDS + 1];
    char *blob, *p;
    int sock, own_cwd = -1, i, count = 0, ret = -1;
    int32_t reply;

    if (nfds > PS5_SPAWN_MAX_FDS) { errno = E2BIG; return -1; }
    msg.blob_size = ps5_spawn_string_size( loader );
    for (msg.argc = 0; argv[msg.argc]; msg.argc++)
        msg.blob_size += ps5_spawn_string_size( argv[msg.argc] );
    for (msg.envc = 0; envp && envp[msg.envc]; msg.envc++)
        msg.blob_size += ps5_spawn_string_size( envp[msg.envc] );
    if (!(blob = malloc( msg.blob_size ))) { errno = ENOMEM; return -1; }
    p = ps5_spawn_copy_string( blob, loader );
    for (i = 0; i < msg.argc; i++) p = ps5_spawn_copy_string( p, argv[i] );
    for (i = 0; i < msg.envc; i++) p = ps5_spawn_copy_string( p, envp[i] );

    if (cwd < 0 && (cwd = own_cwd = open( ".", O_RDONLY | O_DIRECTORY )) < 0) { free( blob ); return -1; }
    sent_fds[count++] = cwd;
    for (i = 0; i < nfds; i++)
    {
        if (fds[i] < 0 || fcntl( fds[i], F_GETFD ) == -1) continue;
        msg.target_fds[count - 1] = targets ? targets[i] : fds[i];
        sent_fds[count++] = fds[i];
    }

    if ((sock = ps5_spawn_connect()) >= 0)
    {
        if (!ps5_spawn_send( sock, &msg, blob, sent_fds, count ) &&
            !ps5_spawn_read_all( sock, &reply, sizeof(reply) ))
        {
            if (reply < 0) errno = -reply;
            else ret = 0;
        }
        close( sock );
    }
    if (own_cwd >= 0) close( own_cwd );
    free( blob );
    return ret;
}

#endif /* __PROSPERO__ */
#endif /* __WINE_WINE_PS5_SPAWN_H */
