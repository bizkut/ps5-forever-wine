/*
 * Ntdll Unix interface
 *
 * Copyright (C) 2020 Alexandre Julliard
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

#ifndef __NTDLL_UNIXLIB_H
#define __NTDLL_UNIXLIB_H

#include "wine/unixlib.h"

struct _DISPATCHER_CONTEXT;

struct wine_dbg_write_params
{
    const char  *str;
    unsigned int len;
};

struct wine_server_fd_to_handle_params
{
    int          fd;
    unsigned int access;
    unsigned int attributes;
    HANDLE      *handle;
};

struct wine_server_handle_to_fd_params
{
    HANDLE        handle;
    unsigned int  access;
    int          *unix_fd;
    unsigned int *options;
};

struct wine_spawnvp_params
{
    char       **argv;
    int          wait;
};

struct load_so_dll_params
{
    UNICODE_STRING              nt_name;
    void                      **module;
};

struct unwind_builtin_dll_params
{
    ULONG                       type;
    struct _DISPATCHER_CONTEXT *dispatch;
    CONTEXT                    *context;
};

struct wine_get_unix_env_params
{
    const char *name;
    char *val;
    unsigned int buffer_len;
};

struct wine_set_unix_env_params
{
    const char *name;
    const char *val;
};

struct wine_dbg_ftrace_params
{
    char *str;
    unsigned int len;
    unsigned int ctx;
};


struct steamclient_setup_trampolines_params
{
    HMODULE src_mod;
    HMODULE tgt_mod;
};

struct debugstr_pc_args
{
    void *pc;
    char *buffer;
    unsigned int size;
};

struct compat_wine_nt_to_unix_file_name_params
{
    const OBJECT_ATTRIBUTES *attr;
    char *nameA;
    ULONG *size;
    unsigned int disposition;
};

/* PS5 title QPC from the TSC; tsc_hz 0 when the TSC is unusable. */
struct ps5_qpc_calibrate_params
{
    ULONGLONG tsc_hz;
    ULONGLONG mul;
    ULONGLONG bias;
};

enum ps5_title_startup_step
{
    PS5_TITLE_STARTUP_PE_LOADER,
    PS5_TITLE_STARTUP_EXE_IMPORTS,
    PS5_TITLE_STARTUP_PROCESS_ATTACH,
    PS5_TITLE_STARTUP_ENTRY
};

/* No pointers: shared by the native and wow64 Unix call tables. */
struct ps5_title_startup_params
{
    unsigned int step;
    unsigned int enabled;
};

enum ps5_title_dll_op
{
    PS5_TITLE_DLL_MAP,
    PS5_TITLE_DLL_IMPORTS,
    PS5_TITLE_DLL_ATTACH,
    PS5_TITLE_DLL_LOG,
    PS5_TITLE_DLL_SUMMARY,
    PS5_TITLE_DLL_UNTRACKED
};

/* Pointer-free and identically laid out on PE32/PE64/Unix. Times are us. */
struct ps5_title_dll_params
{
    ULONGLONG map, reloc, imports, attach;
    unsigned int op, size_kib, relocated, map_known;
    char name[96];
};

enum ntdll_unix_funcs
{
    unix_load_so_dll,
    unix_unwind_builtin_dll,
    unix_wine_dbg_write,
    unix_wine_server_call,
    unix_wine_server_fd_to_handle,
    unix_wine_server_handle_to_fd,
    unix_wine_spawnvp,
    unix_system_time_precise,
    unix___wine_get_unix_env,
    unix___wine_set_unix_env,
    unix_wine_dbg_ftrace,
    unix_steamclient_setup_trampolines,
    unix_debugstr_pc,
    unix_compat_wine_nt_to_unix_file_name,
    unix_ps5_qpc_calibrate,
    unix_ps5_title_startup,
    unix_ps5_title_dll,
};

extern unixlib_handle_t __wine_unixlib_handle;

#define WINE_BACKTRACE_LOG_ON() WARN_ON(seh)

#define WINE_BACKTRACE_LOG(args...) do { \
        WARN_(seh)("backtrace: " args); \
    } while (0)

#endif /* __NTDLL_UNIXLIB_H */
