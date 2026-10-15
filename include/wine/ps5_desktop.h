/*
 * The PS5 desktop: Wine's windows on the screen of the launcher title
 *
 * Copyright 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * What the console allows (measured, ps5/probes/d7 and d8 in the Proton tree):
 * only a title (the launcher, PPSA99030) shows on the screen and reads the
 * pad, keyboards and mice; Wine's processes are payloads. A title cannot
 * listen on a socket (bind: EACCES, TCP and AF_UNIX) and cannot connect to an
 * AF_UNIX socket (EPERM); it can connect to a payload over TCP loopback and
 * open a payload's POSIX shared-memory object by name when the object is
 * readable by every user.
 *
 * So the desktop hub listens on 127.0.0.1:PS5_DESKTOP_PORT for the title,
 * on an AF_UNIX socket in /system_tmp for payload Wine processes, and on
 * 127.0.0.1:PS5_DESKTOP_DRIVER_PORT for a Wine process hosted by the title.
 * wineps5.drv tries AF_UNIX first and falls back to driver TCP when the
 * title's jail rejects AF_UNIX. Messages from the drivers go to the title;
 * input from the title goes to one driver, the input sink (the process that
 * owns the desktop window, explorer), which injects it: wineserver then
 * delivers it to the window under the pointer or with the focus, as it does
 * on any driver.
 *
 * Window pixels: each top-level window's surface is a shared-memory object
 * the driver creates (mode 0666) and names in a WINDOW message; the title
 * opens it by name, read-only. The pixels are 32-bit B,G,R,X, top-down, as
 * win32u's DIB surfaces and the title's scan-out are.
 *
 * Every message starts with struct ps5_desktop_header; size counts the
 * header. Integers are the console's (little-endian); the two ends are built
 * from this header.
 */

#ifndef __WINE_WINE_PS5_DESKTOP_H
#define __WINE_WINE_PS5_DESKTOP_H

#include <stdint.h>

#define PS5_DESKTOP_PORT       58420
#define PS5_DESKTOP_DRIVER_PORT 58421
#define PS5_DESKTOP_SOCKET     "desktop"   /* in /system_tmp/.wine-<uid>/, beside the broker's */
#define PS5_DESKTOP_SHM_NAME   64          /* bytes for a shared-memory object's name, with its NUL */
#define PS5_DESKTOP_MAX_TEXT   512         /* UTF-16 units in one TEXT message */
#define PS5_DESKTOP_WIDTH      1920        /* the one monitor, the title's output */
#define PS5_DESKTOP_HEIGHT     1080

enum ps5_desktop_type
{
    /* driver -> hub */
    PS5_DESKTOP_HELLO = 1,      /* struct ps5_desktop_hello: first message of a connection */
    /* driver -> hub -> title */
    PS5_DESKTOP_WINDOW,         /* struct ps5_desktop_window: a top-level window's state */
    PS5_DESKTOP_DESTROY,        /* struct ps5_desktop_id: the window is gone */
    PS5_DESKTOP_DAMAGE,         /* struct ps5_desktop_damage: pixels changed */
    PS5_DESKTOP_CURSOR_POS,     /* struct ps5_desktop_point: a program moved the pointer */
    /* title -> hub -> input sink */
    PS5_DESKTOP_MOUSE,          /* struct ps5_desktop_mouse */
    PS5_DESKTOP_KEY,            /* struct ps5_desktop_key */
    PS5_DESKTOP_TEXT,           /* struct ps5_desktop_text: committed on-screen keyboard text */
    /* hub -> title */
    PS5_DESKTOP_SESSION,        /* struct ps5_desktop_session: wineserver started or exited */
    /* title -> hub, and its answer hub -> title */
    PS5_DESKTOP_ART,            /* struct ps5_desktop_art: install the title's new icon and backgrounds */
    /* driver -> title */
    PS5_DESKTOP_TEXT_INPUT,     /* struct ps5_desktop_text_input: a text field took or lost the focus */
};

struct ps5_desktop_header
{
    uint32_t type;
    uint32_t size;
};

/* HELLO flags */
#define PS5_DESKTOP_INPUT_SINK 0x1  /* send the title's input to this connection */

struct ps5_desktop_hello
{
    struct ps5_desktop_header header;
    uint32_t pid;
    uint32_t flags;
};

struct ps5_desktop_id
{
    struct ps5_desktop_header header;
    uint64_t id;                /* the window's HWND */
};

/* WINDOW flags */
#define PS5_DESKTOP_VISIBLE    0x01
#define PS5_DESKTOP_TOPMOST    0x02 /* stays above windows without it */
#define PS5_DESKTOP_ZORDER     0x04 /* insert_after says where the window now is */
#define PS5_DESKTOP_LAYERED    0x08 /* alpha and color_key apply */
#define PS5_DESKTOP_PREMULTIPLIED 0x10 /* the pixels carry premultiplied alpha (UpdateLayeredWindow) */

/* insert_after, besides another window's id */
#define PS5_DESKTOP_Z_TOP      0    /* above every window */
#define PS5_DESKTOP_Z_BOTTOM   1    /* below every window */

struct ps5_desktop_window
{
    struct ps5_desktop_header header;
    uint64_t id;
    uint64_t insert_after;      /* the window goes right below this one (or Z_TOP, Z_BOTTOM) */
    int32_t  x, y;              /* the surface's top-left corner on the screen */
    uint32_t width, height;     /* of the surface, in pixels */
    uint32_t stride;            /* bytes from one row to the next */
    uint32_t flags;
    uint32_t alpha;             /* LAYERED: 0-255 for the whole window */
    uint32_t color_key;         /* LAYERED: 0x00RRGGBB drawn transparent, or ~0u for none */
    int32_t  visible_left, visible_top, visible_right, visible_bottom; /* in the surface: what shows */
    char     shm[PS5_DESKTOP_SHM_NAME]; /* the pixels' object, empty when there are none */
};

struct ps5_desktop_damage
{
    struct ps5_desktop_header header;
    uint64_t id;
    int32_t  left, top, right, bottom; /* in the surface */
};

struct ps5_desktop_point
{
    struct ps5_desktop_header header;
    int32_t x, y;
};

/* MOUSE kinds */
#define PS5_DESKTOP_MOUSE_MOVE   1  /* x, y */
#define PS5_DESKTOP_MOUSE_BUTTON 2  /* x, y, button, down */
#define PS5_DESKTOP_MOUSE_WHEEL  3  /* x, y, wheel_x, wheel_y (120 a detent; positive: up, right) */

/* MOUSE buttons */
#define PS5_DESKTOP_BUTTON_LEFT   1
#define PS5_DESKTOP_BUTTON_RIGHT  2
#define PS5_DESKTOP_BUTTON_MIDDLE 3
#define PS5_DESKTOP_BUTTON_X1     4
#define PS5_DESKTOP_BUTTON_X2     5

struct ps5_desktop_mouse
{
    struct ps5_desktop_header header;
    uint32_t kind;
    int32_t  x, y;              /* the pointer on the screen, after the event */
    uint32_t button;
    uint32_t down;
    int32_t  wheel_x, wheel_y;
};

struct ps5_desktop_key
{
    struct ps5_desktop_header header;
    uint32_t usage;             /* USB HID keyboard usage (page 7); modifiers 0xe0-0xe7 */
    uint32_t down;
    uint32_t repeat;            /* an autorepeat of a held key */
};

struct ps5_desktop_text
{
    struct ps5_desktop_header header;
    uint32_t length;            /* UTF-16 units that follow */
    uint16_t text[];            /* header.size = sizeof(struct ps5_desktop_text) + 2 * length */
};

/* Sent when the title connects and whenever wineserver starts or exits. A
 * session's first process may end long before the session does (a launcher
 * that starts the real program and exits, a program that updates itself and
 * starts again), and the hub and the broker outlive every session, so
 * wineserver's own lifetime is what tells the title that Wine is done. */
struct ps5_desktop_session
{
    struct ps5_desktop_header header;
    uint32_t running;           /* 1 while wineserver runs, 0 once it has exited */
    uint32_t reserved;
};

/* The home screen draws a homebrew title's icon and backgrounds from the
 * copies its registration made (ShadowMountPlus copies sce_sys/ into
 * /user/app/<id>/ and /user/appmeta/<id>/ once), which a title cannot see
 * (ps5/probes/appmeta in the Proton tree). The title writes its new
 * sce_sys/icon0.png, pic0.dds or pic1.dds, then asks: the hub, a payload,
 * copies each of those the title has over the existing copies of the title
 * this Wine tree belongs to (PS5_WINE_ROOT's parent folder), and answers with
 * the same message. The home screen shows them after the console restarts. */
struct ps5_desktop_art
{
    struct ps5_desktop_header header;
    uint32_t result;            /* answer: 0, or the first failure's errno */
    uint32_t copied;            /* answer: how many existing copies were replaced */
};

/* A program enabled the IME on a window as one of its text fields took the
 * focus (active 1), or disabled it as the focus left (active 0): the title
 * may open its on-screen keyboard. Chromium does not enable it for password
 * fields. */
struct ps5_desktop_text_input
{
    struct ps5_desktop_header header;
    uint32_t active;
    uint32_t reserved;
};

#endif /* __WINE_WINE_PS5_DESKTOP_H */
