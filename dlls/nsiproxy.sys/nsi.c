/*
 * nsiproxy.sys
 *
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
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <limits.h>
#ifdef HAVE_LINUX_RTNETLINK_H
#include <linux/rtnetlink.h>
#endif
#ifdef __APPLE__
#include <sys/ioctl.h>
#include <sys/kern_event.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "ifdef.h"
#define __WINE_INIT_NPI_MODULEID
#define USE_WS_PREFIX
#include "netiodef.h"
#include "wine/nsi.h"
#include "wine/debug.h"
#include "wine/unixlib.h"
#include "unix_private.h"
#include "nsiproxy_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(nsi);


static NTSTATUS unix_nsi_enumerate_all_ex( void *args )
{
    struct nsi_enumerate_all_ex *params = (struct nsi_enumerate_all_ex *)args;
    return nsi_enumerate_all_ex( params );
}

static NTSTATUS unix_nsi_get_all_parameters_ex( void *args )
{
    struct nsi_get_all_parameters_ex *params = (struct nsi_get_all_parameters_ex *)args;
    return nsi_get_all_parameters_ex( params );
}

static NTSTATUS unix_nsi_get_parameter_ex( void *args )
{
    struct nsi_get_parameter_ex *params = (struct nsi_get_parameter_ex *)args;
    return nsi_get_parameter_ex( params );
}

#if defined(HAVE_LINUX_RTNETLINK_H) || defined(__APPLE__)
static struct
{
    const NPI_MODULEID *module;
    UINT32 table;
}
queued_notifications[256];
static unsigned int queued_notification_count;

static NTSTATUS add_notification( const NPI_MODULEID *module, UINT32 table )
{
    unsigned int i;

    for (i = 0; i < queued_notification_count; ++i)
        if (queued_notifications[i].module == module && queued_notifications[i].table == table) return STATUS_SUCCESS;
    if (queued_notification_count == ARRAY_SIZE(queued_notifications))
    {
        ERR( "Notification queue full.\n" );
        return STATUS_NO_MEMORY;
    }
    queued_notifications[i].module = module;
    queued_notifications[i].table = table;
    ++queued_notification_count;
    return STATUS_SUCCESS;
}

#if defined(HAVE_LINUX_RTNETLINK_H)
static NTSTATUS poll_events(void)
{
    static int netlink_fd = -1;
    char buffer[PIPE_BUF];
    struct nlmsghdr *nlh;
    NTSTATUS status;
    int len;

    if (netlink_fd == -1)
    {
        struct sockaddr_nl addr;

        if ((netlink_fd = socket( PF_NETLINK, SOCK_RAW, NETLINK_ROUTE )) == -1)
        {
            ERR( "netlink socket creation failed, errno %d.\n", errno );
            return STATUS_NOT_IMPLEMENTED;
        }

        memset( &addr, 0, sizeof(addr) );
        addr.nl_family = AF_NETLINK;
        addr.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
        if (bind( netlink_fd, (struct sockaddr *)&addr, sizeof(addr) ) == -1)
        {
            close( netlink_fd );
            netlink_fd = -1;
            ERR( "bind failed, errno %d.\n", errno );
            return STATUS_NOT_IMPLEMENTED;
        }
    }

    while (1)
    {
        len = recv( netlink_fd, buffer, sizeof(buffer), 0 );
        if (len <= 0)
        {
            if (errno == EINTR) continue;
            ERR( "error receivng, len %d, errno %d.\n", len, errno );
            return STATUS_UNSUCCESSFUL;
        }
        for (nlh = (struct nlmsghdr *)buffer; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len))
        {
            if (nlh->nlmsg_type == NLMSG_DONE) break;
            if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR)
            {
                struct ifaddrmsg *addrmsg = (struct ifaddrmsg *)(nlh + 1);
                const NPI_MODULEID *module;

                if (addrmsg->ifa_family == AF_INET)       module = &NPI_MS_IPV4_MODULEID;
                else if (addrmsg->ifa_family == AF_INET6) module = &NPI_MS_IPV6_MODULEID;
                else
                {
                    WARN( "Unknown addrmsg->ifa_family %d.\n", addrmsg->ifa_family );
                    continue;
                }
                if ((status = add_notification( module, NSI_IP_UNICAST_TABLE))) return status;
            }
        }
        if (queued_notification_count) break;
    }
    return STATUS_SUCCESS;
}
#elif defined(__APPLE__)
static NTSTATUS poll_events(void)
{
    static int sock = -1;

    if (sock == -1)
    {
        static const struct kev_request req =
        {
            .vendor_code = KEV_VENDOR_APPLE,
            .kev_class = KEV_NETWORK_CLASS,
            .kev_subclass = KEV_ANY_SUBCLASS,
        };

        if ((sock = socket( PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT )) == -1)
        {
            ERR( "PF_SYSTEM socket creation failed, errno %d.\n", errno );
            return STATUS_NOT_IMPLEMENTED;
        }

        if (ioctl( sock, SIOCSKEVFILT, &req ) == -1)
        {
            close( sock );
            sock = -1;
            ERR( "SIOCSKEVFILT failed, errno %d.\n", errno );
            return STATUS_NOT_IMPLEMENTED;
        }
    }

    while (1)
    {
        struct kern_event_msg msg;
        NTSTATUS status;
        int len;

        len = recv( sock, &msg, sizeof(msg), 0 );
        if (len < sizeof(msg))
        {
            if (errno == EINTR) continue;
            ERR( "error receiving, len %d, errno %d.\n", len, errno );
            return STATUS_UNSUCCESSFUL;
        }

        if (msg.kev_subclass == KEV_INET_SUBCLASS)
        {
            switch (msg.event_code)
            {
                case KEV_INET_NEW_ADDR:
                case KEV_INET_CHANGED_ADDR:
                case KEV_INET_ADDR_DELETED:
                    if ((status = add_notification( &NPI_MS_IPV4_MODULEID, NSI_IP_UNICAST_TABLE))) return status;
                    break;
            }
        }
        else if (msg.kev_subclass == KEV_INET6_SUBCLASS)
        {
            switch (msg.event_code)
            {
                case KEV_INET6_NEW_USER_ADDR:
                case KEV_INET6_CHANGED_ADDR:
                case KEV_INET6_ADDR_DELETED:
                case KEV_INET6_NEW_LL_ADDR:
                case KEV_INET6_NEW_RTADV_ADDR:
                    if ((status = add_notification( &NPI_MS_IPV6_MODULEID, NSI_IP_UNICAST_TABLE))) return status;
                    break;
            }
        }
        if (queued_notification_count) break;
    }

    return STATUS_SUCCESS;
}
#endif

static NTSTATUS unix_nsi_get_notification( void *args )
{
    struct nsi_get_notification_params *params = (struct nsi_get_notification_params *)args;
    NTSTATUS status;

    if (!queued_notification_count && (status = poll_events())) return status;
    assert( queued_notification_count );
    params->module = *queued_notifications[0].module;
    params->table = queued_notifications[0].table;
    --queued_notification_count;
    memmove( queued_notifications, queued_notifications + 1, sizeof(*queued_notifications) * queued_notification_count );
    return STATUS_SUCCESS;
}
#else
static NTSTATUS unix_nsi_get_notification( void *args )
{
    return STATUS_NOT_IMPLEMENTED;
}
#endif

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    icmp_get_reply,
    icmp_listen,
    icmp_send_echo,
    unix_nsi_enumerate_all_ex,
    unix_nsi_get_all_parameters_ex,
    unix_nsi_get_parameter_ex,
    unix_nsi_get_notification,
};
