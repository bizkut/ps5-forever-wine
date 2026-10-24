/* Shared NSI TCP connection enumeration backed by wineserver socket handles.
 *
 * Copyright 2021 Huw Davies
 * Copyright 2026 RuneScape PS5 contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#ifndef __WINE_NSI_TCP_H
#define __WINE_NSI_TCP_H

/* Callers include wine/nsi.h and wine/server.h. No host TCP-table scan is
 * needed: the shared wineserver knows the Windows owner of every socket,
 * including descriptors transported between native title processes. */
static NTSTATUS tcp_conns_enumerate_all( UINT filter, struct nsi_tcp_conn_key *key_data, UINT key_size,
                                         void *rw, UINT rw_size,
                                         struct nsi_tcp_conn_dynamic *dynamic_data, UINT dynamic_size,
                                         struct nsi_tcp_conn_static *static_data, UINT static_size, UINT_PTR *count )
{
    BOOL want_data = key_size || rw_size || dynamic_size || static_size;
    union tcp_connection *connections = NULL;
    NTSTATUS status;
    UINT_PTR i;

    if (want_data && *count)
    {
        if (*count > ~(size_t)0 / sizeof(*connections)) return STATUS_NO_MEMORY;
        if (!(connections = malloc( sizeof(*connections) * *count ))) return STATUS_NO_MEMORY;
    }

    SERVER_START_REQ( get_tcp_connections )
    {
        req->state_filter = filter;
        wine_server_set_reply( req, connections, want_data ? sizeof(*connections) * *count : 0 );
        status = wine_server_call( req );
        if (!status || (status == STATUS_BUFFER_TOO_SMALL && !want_data)) *count = reply->count;
    }
    SERVER_END_REQ;

    if (status)
    {
        free( connections );
        if (status != STATUS_BUFFER_TOO_SMALL) return status;
        return want_data ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }

    for (i = 0; i < *count; i++)
    {
        const union tcp_connection *conn = &connections[i];

        if (key_data)
        {
            memset( key_data, 0, sizeof(*key_data) );
            if (conn->common.family == WS_AF_INET)
            {
                key_data->local.Ipv4.sin_family = key_data->remote.Ipv4.sin_family = WS_AF_INET;
                key_data->local.Ipv4.sin_addr.WS_s_addr = conn->ipv4.local_addr;
                key_data->local.Ipv4.sin_port = conn->ipv4.local_port;
                key_data->remote.Ipv4.sin_addr.WS_s_addr = conn->ipv4.remote_addr;
                key_data->remote.Ipv4.sin_port = conn->ipv4.remote_port;
            }
            else
            {
                key_data->local.Ipv6.sin6_family = key_data->remote.Ipv6.sin6_family = WS_AF_INET6;
                memcpy( &key_data->local.Ipv6.sin6_addr, conn->ipv6.local_addr, 16 );
                key_data->local.Ipv6.sin6_port = conn->ipv6.local_port;
                key_data->local.Ipv6.sin6_scope_id = conn->ipv6.local_scope_id;
                memcpy( &key_data->remote.Ipv6.sin6_addr, conn->ipv6.remote_addr, 16 );
                key_data->remote.Ipv6.sin6_port = conn->ipv6.remote_port;
                key_data->remote.Ipv6.sin6_scope_id = conn->ipv6.remote_scope_id;
            }
            key_data++;
        }
        if (dynamic_data)
        {
            memset( dynamic_data, 0, sizeof(*dynamic_data) );
            dynamic_data++->state = conn->common.state;
        }
        if (static_data)
        {
            memset( static_data, 0, sizeof(*static_data) );
            static_data++->pid = conn->common.owner;
        }
    }
    free( connections );
    return STATUS_SUCCESS;
}

#endif
