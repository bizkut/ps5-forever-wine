/* Service-free NSI TCP queries for sandboxed Wine titles.
 *
 * Copyright 2026 RuneScape PS5 contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#if 0
#pragma makedep unix
#endif
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef __PROSPERO__
#include <ps5platform/klog.h>
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
#define __WINE_INIT_NPI_MODULEID
#include "netiodef.h"
#include "tcpmib.h"
#include "wine/nsi.h"
#include "wine/server.h"
#include "wine/unixlib.h"
#include "wine/ps5_title.h"
#include "wine/nsi_tcp.h"
#ifdef __PROSPERO__
#include "../nsiproxy.sys/unix_private.h"
#endif

static NTSTATUS unix_enumerate_tcp( void *args )
{
#ifdef __PROSPERO__
    struct nsi_enumerate_all_ex *params = args;
    UINT filter;
    NTSTATUS status;
    static LONG queries;

    if (!__wine_ps5_is_title()) return STATUS_NOT_SUPPORTED;
    if (!NmrIsEqualNpiModuleId( params->module, &NPI_MS_TCP_MODULEID )) return STATUS_NOT_SUPPORTED;
    switch (params->table)
    {
    case NSI_TCP_ALL_TABLE: filter = 0; break;
    case NSI_TCP_ESTAB_TABLE: filter = MIB_TCP_STATE_ESTAB; break;
    case NSI_TCP_LISTEN_TABLE: filter = MIB_TCP_STATE_LISTEN; break;
    default: return STATUS_NOT_SUPPORTED;
    }
    if ((params->key_size && (params->key_size != sizeof(struct nsi_tcp_conn_key) || !params->key_data)) ||
        params->rw_size ||
        (params->dynamic_size && (params->dynamic_size != sizeof(struct nsi_tcp_conn_dynamic) || !params->dynamic_data)) ||
        (params->static_size && (params->static_size != sizeof(struct nsi_tcp_conn_static) || !params->static_data)))
        return STATUS_INVALID_PARAMETER;

    status = tcp_conns_enumerate_all( filter, params->key_size ? params->key_data : NULL, params->key_size,
                                    NULL, 0, params->dynamic_size ? params->dynamic_data : NULL, params->dynamic_size,
                                    params->static_size ? params->static_data : NULL, params->static_size, &params->count );
    /* Bound output to 32 queries/process and 16 loopback rows/query. No remote
     * addresses, executable paths, URLs, endpoint tokens or user data. */
    if (InterlockedIncrement( &queries ) <= 32)
    {
        UINT_PTR i, shown = 0;
        char line[256];
        snprintf( line, sizeof(line), "wine: nsi tcp query pid=%04x filter=%u status=%08x rows=%u owners=%u\n",
                 (unsigned int)(ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess, filter,
                 (unsigned int)status, (unsigned int)params->count, !!params->static_size );
        sceKernelDebugOutText( 0, line );
        if (!status && params->key_size && params->static_size)
            for (i = 0; i < params->count && shown < 16; i++)
            {
                const struct nsi_tcp_conn_key *key = (const struct nsi_tcp_conn_key *)params->key_data + i;
                const struct nsi_tcp_conn_static *stat = (const struct nsi_tcp_conn_static *)params->static_data + i;
                if (key->local.si_family != WS_AF_INET || key->local.Ipv4.sin_addr.WS_s_addr != 0x0100007f) continue;
                snprintf( line, sizeof(line), "wine: nsi tcp loopback owner=%04x local_port=%u remote_port=%u\n",
                         stat->pid, RtlUshortByteSwap( key->local.Ipv4.sin_port ),
                         RtlUshortByteSwap( key->remote.Ipv4.sin_port ) );
                sceKernelDebugOutText( 0, line );
                shown++;
            }
    }
    return status;
#else
    return STATUS_NOT_SUPPORTED;
#endif
}

#ifdef __PROSPERO__
static BOOL network_module( const NPI_MODULEID *module )
{
    return module && (NmrIsEqualNpiModuleId( module, &NPI_MS_NDIS_MODULEID ) ||
                      NmrIsEqualNpiModuleId( module, &NPI_MS_IPV4_MODULEID ) ||
                      NmrIsEqualNpiModuleId( module, &NPI_MS_IPV6_MODULEID ));
}

static NTSTATUS refresh_network_module( const NPI_MODULEID *module )
{
    return NmrIsEqualNpiModuleId( module, &NPI_MS_NDIS_MODULEID ) ? STATUS_SUCCESS : nsi_ndis_refresh();
}
#endif

static NTSTATUS unix_enumerate_network( void *args )
{
#ifdef __PROSPERO__
    struct nsi_enumerate_all_ex *params = args;
    NTSTATUS status;
    if (!__wine_ps5_is_title()) return STATUS_NOT_IMPLEMENTED;
    if (!params || !network_module( params->module )) return STATUS_INVALID_PARAMETER;
    if ((status = refresh_network_module( params->module ))) return status;
    return nsi_enumerate_all_ex( params );
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS unix_get_all_network( void *args )
{
#ifdef __PROSPERO__
    struct nsi_get_all_parameters_ex *params = args;
    NTSTATUS status;
    if (!__wine_ps5_is_title()) return STATUS_NOT_IMPLEMENTED;
    if (!params || !network_module( params->module )) return STATUS_INVALID_PARAMETER;
    if ((status = refresh_network_module( params->module ))) return status;
    return nsi_get_all_parameters_ex( params );
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS unix_get_network_parameter( void *args )
{
#ifdef __PROSPERO__
    struct nsi_get_parameter_ex *params = args;
    NTSTATUS status;
    if (!__wine_ps5_is_title()) return STATUS_NOT_IMPLEMENTED;
    if (!params || !network_module( params->module )) return STATUS_INVALID_PARAMETER;
    if ((status = refresh_network_module( params->module ))) return status;
    return nsi_get_parameter_ex( params );
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    unix_enumerate_tcp, unix_enumerate_network, unix_get_all_network, unix_get_network_parameter
};

#ifdef _WIN64
static NTSTATUS wow64_enumerate( void *args, unixlib_entry_t enumerate )
{
    struct params32
    {
        UINT unknown[2], module, table, first_arg, second_arg;
        UINT key_data, key_size, rw_data, rw_size, dynamic_data, dynamic_size, static_data, static_size, count;
    } *params32 = args;
    struct nsi_enumerate_all_ex params = {0};
    NTSTATUS status;

    params.module = ULongToPtr( params32->module );
    params.table = params32->table;
    params.first_arg = params32->first_arg;
    params.second_arg = params32->second_arg;
    params.key_data = ULongToPtr( params32->key_data );
    params.key_size = params32->key_size;
    params.rw_data = ULongToPtr( params32->rw_data );
    params.rw_size = params32->rw_size;
    params.dynamic_data = ULongToPtr( params32->dynamic_data );
    params.dynamic_size = params32->dynamic_size;
    params.static_data = ULongToPtr( params32->static_data );
    params.static_size = params32->static_size;
    params.count = params32->count;
    status = enumerate( &params );
    params32->count = params.count;
    return status;
}
static NTSTATUS wow64_enumerate_tcp( void *args ) { return wow64_enumerate( args, unix_enumerate_tcp ); }
static NTSTATUS wow64_enumerate_network( void *args ) { return wow64_enumerate( args, unix_enumerate_network ); }

static NTSTATUS wow64_get_all_network( void *args )
{
    const struct params32
    {
        UINT unknown[2], module, table, first_arg, unknown2, key, key_size;
        UINT rw_data, rw_size, dynamic_data, dynamic_size, static_data, static_size;
    } *input = args;
    struct nsi_get_all_parameters_ex params = {0};
    params.module = ULongToPtr( input->module );
    params.table = input->table;
    params.first_arg = input->first_arg;
    params.unknown2 = input->unknown2;
    params.key = ULongToPtr( input->key );
    params.key_size = input->key_size;
    params.rw_data = ULongToPtr( input->rw_data );
    params.rw_size = input->rw_size;
    params.dynamic_data = ULongToPtr( input->dynamic_data );
    params.dynamic_size = input->dynamic_size;
    params.static_data = ULongToPtr( input->static_data );
    params.static_size = input->static_size;
    return unix_get_all_network( &params );
}

static NTSTATUS wow64_get_network_parameter( void *args )
{
    const struct params32
    {
        UINT unknown[2], module, table, first_arg, unknown2, key, key_size;
        UINT param_type, data, data_size, data_offset;
    } *input = args;
    struct nsi_get_parameter_ex params = {0};
    params.module = ULongToPtr( input->module );
    params.table = input->table;
    params.first_arg = input->first_arg;
    params.unknown2 = input->unknown2;
    params.key = ULongToPtr( input->key );
    params.key_size = input->key_size;
    params.param_type = input->param_type;
    params.data = ULongToPtr( input->data );
    params.data_size = input->data_size;
    params.data_offset = input->data_offset;
    return unix_get_network_parameter( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_enumerate_tcp, wow64_enumerate_network, wow64_get_all_network, wow64_get_network_parameter
};
#endif
