/* IPv6 scope lookup shared by the NSI IP/TCP/UDP providers.
 * Copyright 2003, 2006, 2011 Juan Lang
 * Copyright 2007 TransGaming Technologies Inc.
 * Copyright 2021 Huw Davies
 * This library is free software under the GNU Lesser General Public License,
 * version 2.1 or (at your option) any later version. */
#if 0
#pragma makedep unix
#endif
#include "config.h"
#if !defined(NSI_SERVICE_FREE) || defined(__PROSPERO__)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
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
#include "ws2ipdef.h"
#include "ifdef.h"
#include "netiodef.h"
#include "wine/nsi.h"
#include "wine/debug.h"
#include "unix_private.h"
WINE_DEFAULT_DEBUG_CHANNEL(nsi);

struct ipv6_addr_scope *get_ipv6_addr_scope_table( unsigned int *size )
{
    struct ipv6_addr_scope *table = NULL, *new_table;
    unsigned int table_size = 0, num = 0;
    *size = 0;

#ifdef __linux__
    {
        char buf[512], *ptr;
        FILE *fp;

        if (!(fp = fopen( "/proc/net/if_inet6", "r" ))) goto failed;

        while ((ptr = fgets( buf, sizeof(buf), fp )))
        {
            WORD a[8];
            UINT scope;
            struct ipv6_addr_scope *entry;
            unsigned int i;

            if (sscanf( ptr, "%4hx%4hx%4hx%4hx%4hx%4hx%4hx%4hx %*s %*s %x",
                        a, a + 1, a + 2, a + 3, a + 4, a + 5, a + 6, a + 7, &scope ) != 9)
                continue;

            if (++num > table_size)
            {
                if (!table_size) table_size = 4;
                else table_size *= 2;
                if (!(new_table = realloc( table, table_size * sizeof(table[0]) )))
                {
                    fclose( fp );
                    goto failed;
                }
                table = new_table;
            }

            entry = table + num - 1;
            for (i = 0; i < 8; i++)
                entry->addr.u.Word[i] = htons( a[i] );
            entry->scope = htons( scope );
        }

        fclose( fp );
    }
#elif defined(HAVE_GETIFADDRS)
    {
        struct ifaddrs *addrs, *cur;

        if (getifaddrs( &addrs ) == -1)  goto failed;

        for (cur = addrs; cur; cur = cur->ifa_next)
        {
            struct sockaddr_in6 *sin6;
            struct ipv6_addr_scope *entry;

            if (!cur->ifa_addr || cur->ifa_addr->sa_family != AF_INET6) continue;

            if (++num > table_size)
            {
                if (!table_size) table_size = 4;
                else table_size *= 2;
                if (!(new_table = realloc( table, table_size * sizeof(table[0]) )))
                {
                    freeifaddrs( addrs );
                    goto failed;
                }
                table = new_table;
            }

            sin6 = (struct sockaddr_in6 *)cur->ifa_addr;
            entry = table + num - 1;
            memcpy( &entry->addr, &sin6->sin6_addr, sizeof(entry->addr) );
            entry->scope = sin6->sin6_scope_id;
        }

        freeifaddrs( addrs );
    }
#else
    FIXME( "not implemented\n" );
    goto failed;
#endif

    *size = num;
    return table;

failed:
    free( table );
    return NULL;
}

UINT find_ipv6_addr_scope( const IN6_ADDR *addr, const struct ipv6_addr_scope *table, unsigned int size )
{
    const BYTE multicast_scope_mask = 0x0F;
    const BYTE multicast_scope_shift = 0;
    unsigned int i;

    if (WS_IN6_IS_ADDR_UNSPECIFIED( addr )) return 0;

    if (WS_IN6_IS_ADDR_MULTICAST( addr ))
        return htons( (addr->u.Byte[1] & multicast_scope_mask) >> multicast_scope_shift );

    if (!table) return -1;

    for (i = 0; i < size; i++)
        if (!memcmp( &table[i].addr, addr, sizeof(table[i].addr) ))
            return table[i].scope;

    return -1;
}
#endif
