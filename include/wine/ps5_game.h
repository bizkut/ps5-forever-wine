/* RuneScape product game descriptors shared by interception and handoff.
 * Other products retain their existing image policy; they do not call this
 * allowlist. No command line, session value or install location belongs here. */
#ifndef WINE_PS5_GAME_H
#define WINE_PS5_GAME_H
#include <stddef.h>
#include <strings.h>

enum wine_ps5_game_kind { WINE_PS5_GAME_OSRS, WINE_PS5_GAME_RUNELITE };
enum wine_ps5_game_graphics { WINE_PS5_GAME_WGL_ZINK };
enum wine_ps5_game_pad { WINE_PS5_GAME_PAD_OSRS };
struct wine_ps5_game_profile
{
    enum wine_ps5_game_kind kind;
    const char *image;
    const char *name;
    enum wine_ps5_game_graphics graphics;
    enum wine_ps5_game_pad pad;
    const char *launch_mode;
};
static inline const struct wine_ps5_game_profile *wine_ps5_game_profile(const char *image)
{
    static const struct wine_ps5_game_profile games[] = {
        { WINE_PS5_GAME_OSRS, "osclient.exe", "osrs", WINE_PS5_GAME_WGL_ZINK, WINE_PS5_GAME_PAD_OSRS, NULL },
        { WINE_PS5_GAME_RUNELITE, "RuneLite.exe", "runelite", WINE_PS5_GAME_WGL_ZINK, WINE_PS5_GAME_PAD_OSRS, "REFLECT" }
    };
    const char *base;
    if (!image || (image[0] != 'C' && image[0] != 'c') || image[1] != ':' ||
        (image[2] != '\\' && image[2] != '/')) return NULL;
    base = image;
    for (const char *p = image; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    for (unsigned i = 0; i < sizeof(games) / sizeof(games[0]); ++i)
        if (!strcasecmp(base, games[i].image)) return &games[i];
    return NULL;
}
#endif
