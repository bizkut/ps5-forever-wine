/*
 * nsiproxy.sys tcp module
 *
 * Copyright 2003, 2006, 2011 Juan Lang
 * Copyright 2007 TransGaming Technologies Inc.
 * Copyright 2021 Huw Davies
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
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#ifdef HAVE_SYS_SOCKETVAR_H
#include <sys/socketvar.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_NETINET_IP_VAR_H
#include <netinet/ip_var.h>
#endif

#ifdef HAVE_SYS_QUEUE_H
#include <sys/queue.h>
#endif

#ifdef HAVE_NETINET_TCP_VAR_H
#include <netinet/tcp_var.h>
#endif

#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#define USE_WS_PREFIX
#include "winsock2.h"
#include "ifdef.h"
#include "netiodef.h"
#include "ws2ipdef.h"
#include "tcpmib.h"
#include "wine/nsi.h"
#include "wine/debug.h"
#include "wine/server.h"

#include "unix_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(nsi);

static NTSTATUS tcp_stats_get_all_parameters( const void *key, UINT key_size, void *rw_data, UINT rw_size,
                                              void *dynamic_data, UINT dynamic_size, void *static_data, UINT static_size )
{
    struct nsi_tcp_stats_dynamic dyn;
    struct nsi_tcp_stats_static stat;
    const USHORT *family = key;

    TRACE( "%p %d %p %d %p %d %p %d\n", key, key_size, rw_data, rw_size, dynamic_data, dynamic_size,
           static_data, static_size );

    if (*family != WS_AF_INET && *family != WS_AF_INET6) return STATUS_NOT_SUPPORTED;

    memset( &dyn, 0, sizeof(dyn) );
    memset( &stat, 0, sizeof(stat) );

#ifdef __linux__
    {
        NTSTATUS status = STATUS_NOT_SUPPORTED;
        static const char hdr[] = "Tcp:";
        char buf[512], *ptr;
        FILE *fp;

        /* linux merges tcp4 and tcp6 stats, so simply supply that for either family */
        if (!(fp = fopen( "/proc/net/snmp", "r" ))) return STATUS_NOT_SUPPORTED;

        while ((ptr = fgets( buf, sizeof(buf), fp )))
        {
            if (ascii_strncasecmp( buf, hdr, sizeof(hdr) - 1 )) continue;
            /* last line was a header, get another */
            if (!(ptr = fgets( buf, sizeof(buf), fp ))) break;
            if (!ascii_strncasecmp( buf, hdr, sizeof(hdr) - 1 ))
            {
                UINT in_segs, out_segs;
                ptr += sizeof(hdr);
                sscanf( ptr, "%u %u %u %u %u %u %u %u %u %u %u %u %u %u",
                        &stat.rto_algo,
                        &stat.rto_min,
                        &stat.rto_max,
                        &stat.max_conns,
                        &dyn.active_opens,
                        &dyn.passive_opens,
                        &dyn.attempt_fails,
                        &dyn.est_rsts,
                        &dyn.cur_est,
                        &in_segs,
                        &out_segs,
                        &dyn.retrans_segs,
                        &dyn.in_errs,
                        &dyn.out_rsts );
                dyn.in_segs = in_segs;
                dyn.out_segs = out_segs;
                if (dynamic_data) *(struct nsi_tcp_stats_dynamic *)dynamic_data = dyn;
                if (static_data) *(struct nsi_tcp_stats_static *)static_data = stat;
                status = STATUS_SUCCESS;
                break;
            }
        }
        fclose( fp );
        return status;
    }
#elif defined(HAVE_SYS_SYSCTL_H) && defined(TCPCTL_STATS) && (defined(HAVE_STRUCT_TCPSTAT_TCPS_CONNATTEMPT) || defined(HAVE_STRUCT_TCP_STATS_TCPS_CONNATTEMPT))
    {
#ifndef TCPTV_MIN  /* got removed in Mac OS X for some reason */
#define TCPTV_MIN 2
#define TCPTV_REXMTMAX 128
#endif
        int mib[] = { CTL_NET, PF_INET, IPPROTO_TCP, TCPCTL_STATS };
#define hz 1000
#if defined(HAVE_STRUCT_TCPSTAT_TCPS_CONNATTEMPT)
        struct tcpstat tcp_stat;
#elif defined(HAVE_STRUCT_TCP_STATS_TCPS_CONNATTEMPT)
        struct tcp_stats tcp_stat;
#endif
        size_t needed = sizeof(tcp_stat);

        if (sysctl( mib, ARRAY_SIZE(mib), &tcp_stat, &needed, NULL, 0 ) == -1) return STATUS_NOT_SUPPORTED;

        stat.rto_algo = MIB_TCP_RTO_VANJ;
        stat.rto_min = TCPTV_MIN;
        stat.rto_max = TCPTV_REXMTMAX;
        stat.max_conns = -1;
        dyn.active_opens = tcp_stat.tcps_connattempt;
        dyn.passive_opens = tcp_stat.tcps_accepts;
        dyn.attempt_fails = tcp_stat.tcps_conndrops;
        dyn.est_rsts = tcp_stat.tcps_drops;
        dyn.cur_est = 0;
        dyn.pad = 0;
        dyn.in_segs = tcp_stat.tcps_rcvtotal;
        dyn.out_segs = tcp_stat.tcps_sndtotal - tcp_stat.tcps_sndrexmitpack;
        dyn.retrans_segs = tcp_stat.tcps_sndrexmitpack;
        dyn.out_rsts = tcp_stat.tcps_sndctrl - tcp_stat.tcps_closed;
        dyn.in_errs = tcp_stat.tcps_rcvbadsum + tcp_stat.tcps_rcvbadoff + tcp_stat.tcps_rcvmemdrop + tcp_stat.tcps_rcvshort;
        dyn.num_conns = tcp_stat.tcps_connects;
        if (dynamic_data) *(struct nsi_tcp_stats_dynamic *)dynamic_data = dyn;
        if (static_data) *(struct nsi_tcp_stats_static *)static_data = stat;
        return STATUS_SUCCESS;
    }
#else
    FIXME( "not implemented\n" );
    return STATUS_NOT_IMPLEMENTED;
#endif
}


#include "wine/nsi_tcp.h"

static NTSTATUS tcp_all_enumerate_all( void *key_data, UINT key_size, void *rw_data, UINT rw_size,
                                       void *dynamic_data, UINT dynamic_size,
                                       void *static_data, UINT static_size, UINT_PTR *count )
{
    TRACE( "%p %d %p %d %p %d %p %d %p\n", key_data, key_size, rw_data, rw_size,
           dynamic_data, dynamic_size, static_data, static_size, count );

    return tcp_conns_enumerate_all( 0, key_data, key_size, rw_data, rw_size,
                                    dynamic_data, dynamic_size, static_data, static_size, count );
}

static NTSTATUS tcp_estab_enumerate_all( void *key_data, UINT key_size, void *rw_data, UINT rw_size,
                                         void *dynamic_data, UINT dynamic_size,
                                         void *static_data, UINT static_size, UINT_PTR *count )
{
    TRACE( "%p %d %p %d %p %d %p %d %p\n", key_data, key_size, rw_data, rw_size,
           dynamic_data, dynamic_size, static_data, static_size, count );

    return tcp_conns_enumerate_all( MIB_TCP_STATE_ESTAB, key_data, key_size, rw_data, rw_size,
                                    dynamic_data, dynamic_size, static_data, static_size, count );
}

static NTSTATUS tcp_listen_enumerate_all( void *key_data, UINT key_size, void *rw_data, UINT rw_size,
                                          void *dynamic_data, UINT dynamic_size,
                                          void *static_data, UINT static_size, UINT_PTR *count )
{
    TRACE( "%p %d %p %d %p %d %p %d %p\n", key_data, key_size, rw_data, rw_size,
           dynamic_data, dynamic_size, static_data, static_size, count );

    return tcp_conns_enumerate_all( MIB_TCP_STATE_LISTEN, key_data, key_size, rw_data, rw_size,
                                    dynamic_data, dynamic_size, static_data, static_size, count );
}

static struct module_table tcp_tables[] =
{
    {
        NSI_TCP_STATS_TABLE,
        {
            sizeof(USHORT), 0,
            sizeof(struct nsi_tcp_stats_dynamic), sizeof(struct nsi_tcp_stats_static)
        },
        NULL,
        tcp_stats_get_all_parameters,
    },
    {
        NSI_TCP_ALL_TABLE,
        {
            sizeof(struct nsi_tcp_conn_key), 0,
            sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static)
        },
        tcp_all_enumerate_all,
    },
    {
        NSI_TCP_ESTAB_TABLE,
        {
            sizeof(struct nsi_tcp_conn_key), 0,
            sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static)
        },
        tcp_estab_enumerate_all,
    },
    {
        NSI_TCP_LISTEN_TABLE,
        {
            sizeof(struct nsi_tcp_conn_key), 0,
            sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static)
        },
        tcp_listen_enumerate_all,
    },
    {
        ~0u
    }
};

const struct module tcp_module =
{
    &NPI_MS_TCP_MODULEID,
    tcp_tables
};
