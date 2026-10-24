/* RuneScape's user data is shared with other titles and FTP. Windows access
 * checks stay in Wine; Unix owner/group ACL projections must not hide this
 * explicitly shared tree. Other products and paths keep their existing mode. */
#ifndef __WINE_PS5_SHARED_MODE_H
#define __WINE_PS5_SHARED_MODE_H
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef WINE_PS5_SHARED_ROOT
#define WINE_PS5_SHARED_ROOT "/data/runescape"
#endif
#define WINE_PS5_PRIVATE_HANDOFF_DIRECTORY WINE_PS5_SHARED_ROOT "/handoff"

/* The only private exception inside the shared root. Normalize harmless slash
 * and dot spellings, but never authorize a parent traversal. */
static inline int wine_ps5_private_handoff_path( const char *path )
{
#ifdef WINE_PS5_RUNESCAPE
    static const char root[] = WINE_PS5_SHARED_ROOT;
    const char *first, *scan;
    if (!path || strncmp( path, root, sizeof(root) - 1 ) ||
        (path[sizeof(root) - 1] && path[sizeof(root) - 1] != '/')) return 0;
    first = scan = path + sizeof(root) - 1;
    while (*scan)
    {
        if (*scan++ != '/') continue;
        if (scan[0] == '.' && scan[1] == '.' && (!scan[2] || scan[2] == '/')) return 0;
    }
    for (;;)
    {
        while (*first == '/') ++first;
        if (first[0] == '.' && (!first[1] || first[1] == '/')) { ++first;continue; }
        break;
    }
    return !strncmp( first, "handoff", 7 ) && (!first[7] || first[7] == '/');
#else
    (void)path;
    return 0;
#endif
}

static inline int wine_ps5_shared_path( const char *path )
{
#ifdef WINE_PS5_RUNESCAPE
    static const char root[] = WINE_PS5_SHARED_ROOT;
    const char *component;
    if (!path || strncmp( path, root, sizeof(root) - 1 ) ||
        (path[sizeof(root) - 1] && path[sizeof(root) - 1] != '/')) return 0;
    if (wine_ps5_private_handoff_path( path )) return 0;
    component = path + sizeof(root) - 1;
    while (*component)
    {
        if (*component++ != '/') continue;
        if (component[0] == '.' && component[1] == '.' &&
            (!component[2] || component[2] == '/')) return 0;
    }
    return 1;
#else
    (void)path;
    return 0;
#endif
}

/* PS5's existing realpath provider normalizes stat-able names but does not
 * resolve symbolic links. Independently reject links at every component
 * before widening an inode's Unix permissions. */
static inline int wine_ps5_shared_no_links( char *path )
{
    char *at = path + 1;
    for (;; ++at)
    {
        int fd, valid;
        char saved;
        struct stat status;
        if (*at && *at != '/') continue;
        saved = *at;
        *at = 0;
        fd = open( path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW );
        *at = saved;
        if (fd < 0) return 0;
        valid = !fstat( fd, &status ) && (!saved || S_ISDIR(status.st_mode));
        close( fd );
        if (!valid) return 0;
        if (!saved) return 1;
    }
}

static inline int wine_ps5_shared_existing_path( const char *path )
{
    char checked[4096];
    size_t length;
    int saved, selected;
    if (!wine_ps5_shared_path(path) || (length = strlen(path)) >= sizeof(checked)) return 0;
    memcpy( checked, path, length + 1 );
    saved = errno;
    selected = wine_ps5_shared_no_links( checked );
    errno = saved;
    return selected;
}

/* Select by physical object/parent, not a lexical symlink path. The caller
 * supplies its existing host or PS5 realpath provider, without new imports. */
static inline int wine_ps5_shared_resolve( const char *path, char resolved[4096],
                                         char *(*resolve)(const char *, char *) )
{
    char parent[4096], *slash;
    size_t length;
    int saved = errno, selected = 0;
    if (!wine_ps5_shared_path(path)) return 0;
    if (resolve(path, resolved))
        selected = wine_ps5_shared_path(resolved) && wine_ps5_shared_no_links(resolved);
    else if (errno == ENOENT && (length = strlen(path)) < sizeof(parent))
    {
        memcpy(parent, path, length + 1);
        slash = strrchr(parent, '/');
        if (slash && slash[1])
        {
            const char *leaf = path + (slash - parent) + 1;
            *slash = 0;
            if (resolve(parent, resolved) && wine_ps5_shared_path(resolved) &&
                wine_ps5_shared_no_links(resolved))
            {
                length = strlen(resolved);
                if (length + 1 + strlen(leaf) < 4096)
                {
                    resolved[length++] = '/';
                    strcpy(resolved + length, leaf);
                    selected = wine_ps5_shared_path(resolved);
                }
            }
        }
    }
    errno = saved;
    return selected;
}

static inline mode_t wine_ps5_shared_permissions( mode_t current )
{
    if (S_ISDIR(current)) return (current & ~07777) | 0777;
    if (S_ISREG(current)) return (current & ~07777) | 0666 | (current & 0111);
    return current;
}

static inline mode_t wine_ps5_shared_mode( const char *path, mode_t current, mode_t requested )
{
    if (!S_ISREG(current) && !S_ISDIR(current)) return requested;
    return wine_ps5_shared_path( path ) ? wine_ps5_shared_permissions( current ) : requested;
}
#endif
