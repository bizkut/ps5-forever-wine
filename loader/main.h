/*
 * Definitions for Wine main program
 *
 * Copyright 2004 Mike McCormack for CodeWeavers
 * Copyright 2004 Alexandre Julliard
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

#ifndef __WINE_LOADER_MAIN_H
#define __WINE_LOADER_MAIN_H

#include <string.h>

/* Shared with standalone path regressions. The selected platform resolver
 * owns allocation/normalization; Wine only trims its returned path in place. */
static inline char *wine_loader_realpath_dirname( const char *name,
                                                 char *(*resolve)( const char *, char * ))
{
    char *p, *fullpath = resolve( name, NULL );
    if (fullpath)
    {
        p = strrchr( fullpath, '/' );
        if (p == fullpath) p++;
        if (p) *p = 0;
    }
    return fullpath;
}

struct wine_preload_info
{
    void  *addr;
    size_t size;
};

#endif /* __WINE_LOADER_MAIN_H */
