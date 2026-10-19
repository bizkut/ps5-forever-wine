/* Standalone host regressions for the PS5 voice wire format and the shared
 * Wine input dispatcher. Build from the Wine root:
 * cc -std=c11 -Wall -Wextra -Werror -Iinclude loader/tests/ps5_input.c -o /tmp/ps5_input_test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define __WINESRC__
#define WINE_UNIX_LIB
#include "windef.h"
#include "winbase.h"
#include "ntuser.h"
#include "wine/ps5_title_input.h"

static struct
{
    HWND target;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
} posted[4096];
static unsigned int posted_count, hardware_count, focus_queries;
static HWND foreground, focus;
static DWORD foreground_thread;
static BOOL gui_available, change_focus;

static HWND test_foreground(void)
{
    return foreground;
}

static HANDLE test_query_window( HWND hwnd, WINDOWINFOCLASS info )
{
    assert( hwnd == foreground && info == WindowThread );
    return ULongToHandle( foreground_thread );
}

static BOOL test_gui_info( DWORD thread, GUITHREADINFO *info )
{
    assert( thread == foreground_thread && info->cbSize == sizeof(*info) );
    focus_queries++;
    info->hwndFocus = focus;
    return gui_available;
}

static BOOL test_post( HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam )
{
    assert( posted_count < ARRAY_SIZE(posted) );
    posted[posted_count].target = hwnd;
    posted[posted_count].message = message;
    posted[posted_count].wparam = wparam;
    posted[posted_count++].lparam = lparam;
    if (change_focus) focus = ULongToHandle( 0x2222 );
    return TRUE;
}

static BOOL test_hardware( HWND hwnd, UINT flags, const INPUT *input, LPARAM lparam )
{
    assert( !hwnd && !flags && !lparam );
    assert( input->type == INPUT_KEYBOARD && input->ki.wVk == 'A' );
    hardware_count++;
    return TRUE;
}

static BOOL test_async_state( BYTE state[256] )
{
    memset( state, 0, 256 );
    return TRUE;
}

#define NtUserGetForegroundWindow test_foreground
#define NtUserQueryWindow test_query_window
#define NtUserGetGUIThreadInfo test_gui_info
#define NtUserPostMessage test_post
#define NtUserSendHardwareInput test_hardware
#define NtUserGetAsyncKeyboardState test_async_state
#include "wine/ps5_input.h"

static void reset_queue(void)
{
    posted_count = hardware_count = focus_queries = 0;
    foreground = ULongToHandle( 0x1000 );
    focus = ULongToHandle( 0x1111 );
    foreground_thread = 17;
    gui_available = TRUE;
    change_focus = FALSE;
}

static void expect_key( unsigned int *index, HWND target, WORD vkey, WORD scan )
{
    unsigned int i = *index;
    assert( i + 1 < posted_count );
    assert( posted[i].target == target && posted[i + 1].target == target );
    assert( posted[i].message == WM_KEYDOWN && posted[i + 1].message == WM_KEYUP );
    assert( posted[i].wparam == vkey && posted[i + 1].wparam == vkey );
    assert( posted[i].lparam == (LPARAM)(1u | (scan << 16)) );
    assert( posted[i + 1].lparam == (LPARAM)(0xc0000001u | (scan << 16)) );
    *index += 2;
}

static void expect_text( unsigned int *index, HWND target, const uint16_t *text, unsigned int length )
{
    unsigned int j;
    for (j = 0; j < length; j++, (*index)++)
    {
        assert( *index < posted_count && posted[*index].target == target );
        assert( posted[*index].message == WM_CHAR && posted[*index].wparam == text[j] );
        assert( posted[*index].lparam == 1 );
    }
}

static void test_voice_wire(void)
{
    union ps5_title_input message;
    struct ps5_desktop_voice_edit *edit = (void *)&message;
    unsigned char unaligned[PS5_DESKTOP_VOICE_MAX_MESSAGE + 1];
    size_t i;

    _Static_assert( sizeof(*edit) == 20 && offsetof(struct ps5_desktop_voice_edit, text) == 20,
                    "voice wire prefix" );
    _Static_assert( PS5_DESKTOP_VOICE_EDIT == 13 && PS5_DESKTOP_CURSOR_SHAPE == 12,
                    "append-only desktop message ids" );
    _Static_assert( sizeof(message) >= PS5_DESKTOP_VOICE_MAX_MESSAGE, "native queue capacity" );
    _Static_assert( PS5_DESKTOP_MAX_MESSAGE >= PS5_DESKTOP_VOICE_MAX_MESSAGE, "hub capacity" );
    *edit = (struct ps5_desktop_voice_edit){ .header = { PS5_DESKTOP_VOICE_EDIT, sizeof(*edit) } };
    assert( ps5_desktop_voice_edit_valid( edit, sizeof(*edit) ) );
    for (i = 0; i < sizeof(*edit); i++) assert( !ps5_desktop_voice_edit_valid( edit, i ) );
    edit->flags = PS5_DESKTOP_VOICE_FLAGS;
    edit->erase = PS5_DESKTOP_VOICE_MAX_ERASE;
    edit->length = PS5_DESKTOP_MAX_TEXT;
    edit->header.size = PS5_DESKTOP_VOICE_MAX_MESSAGE;
    for (i = 0; i < edit->length; i++) edit->text[i] = 0x100 + i;
    assert( ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
    memcpy( unaligned + 1, edit, edit->header.size );
    assert( ps5_desktop_voice_edit_valid( unaligned + 1, edit->header.size ) );
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size - 1 ) );
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size + 1 ) );
    edit->header.size--;
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
    edit->header.size = PS5_DESKTOP_VOICE_MAX_MESSAGE;
    edit->length = UINT32_MAX;
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
    edit->length = PS5_DESKTOP_MAX_TEXT + 1;
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
    edit->length = PS5_DESKTOP_MAX_TEXT;
    edit->erase = UINT32_MAX;
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
    edit->erase = PS5_DESKTOP_VOICE_MAX_ERASE + 1;
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
    edit->erase = 0;
    edit->flags |= 0x10;
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
    edit->flags = 0x80000000u;
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
    edit->flags = 0;
    edit->header.type = PS5_DESKTOP_TEXT;
    assert( !ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
}

static void test_voice_order(void)
{
    static const uint16_t text[] = { '/', 'p', ' ', 'a', 0xd83d, 0xde00 };
    union ps5_title_input message;
    struct ps5_desktop_voice_edit *edit = (void *)&message;
    unsigned int flags, i, j;
    HWND target = ULongToHandle( 0x1111 );

    *edit = (struct ps5_desktop_voice_edit){
        .header = { PS5_DESKTOP_VOICE_EDIT, sizeof(*edit) + sizeof(text) },
        .erase = 2, .length = ARRAY_SIZE(text) };
    memcpy( edit->text, text, sizeof(text) );
    for (flags = 0; flags <= PS5_DESKTOP_VOICE_FLAGS; flags++)
    {
        reset_queue();
        edit->flags = flags;
        assert( ps5_desktop_voice_edit_valid( edit, edit->header.size ) );
        send_voice_edit( edit );
        assert( !hardware_count && focus_queries == 1 );
        i = 0;
        if (flags & PS5_DESKTOP_VOICE_ESCAPE_BEFORE) expect_key( &i, target, VK_ESCAPE, 0x01 );
        if (flags & PS5_DESKTOP_VOICE_ENTER_BEFORE) expect_key( &i, target, VK_RETURN, 0x1c );
        for (j = 0; j < edit->erase; j++) expect_key( &i, target, VK_BACK, 0x0e );
        expect_text( &i, target, text, ARRAY_SIZE(text) );
        if (flags & PS5_DESKTOP_VOICE_ENTER_AFTER) expect_key( &i, target, VK_RETURN, 0x1c );
        if (flags & PS5_DESKTOP_VOICE_ESCAPE_AFTER) expect_key( &i, target, VK_ESCAPE, 0x01 );
        assert( i == posted_count );
    }

    /* A competing focus change cannot split one batch over two windows. The
     * next batch snapshots the new focus and appends after the entire first. */
    reset_queue();
    edit->flags = PS5_DESKTOP_VOICE_ENTER_BEFORE | PS5_DESKTOP_VOICE_ENTER_AFTER;
    change_focus = TRUE;
    send_voice_edit( edit );
    j = posted_count;
    send_voice_edit( edit );
    assert( posted_count == j * 2 && focus_queries == 2 && !hardware_count );
    for (i = 0; i < j; i++) assert( posted[i].target == target );
    for (; i < posted_count; i++) assert( posted[i].target == ULongToHandle( 0x2222 ) );
    {
        /* This remains hardware input, not part of the posted batch. Wine's
         * server checks the posted FIFO first (server/queue.c:get_message). */
        struct ps5_desktop_key key = { { PS5_DESKTOP_KEY, sizeof(key) }, 0x04, 1, 0 };
        send_key( &key );
        assert( hardware_count == 1 && posted_count == j * 2 );
    }

    reset_queue();
    *edit = (struct ps5_desktop_voice_edit){
        .header = { PS5_DESKTOP_VOICE_EDIT, PS5_DESKTOP_VOICE_MAX_MESSAGE },
        .flags = PS5_DESKTOP_VOICE_FLAGS, .erase = PS5_DESKTOP_VOICE_MAX_ERASE,
        .length = PS5_DESKTOP_MAX_TEXT };
    for (i = 0; i < edit->length; i++) edit->text[i] = 0x100 + i;
    send_voice_edit( edit );
    assert( posted_count == 8 + 2 * edit->erase + edit->length && !hardware_count );
    i = 0;
    expect_key( &i, target, VK_ESCAPE, 0x01 );
    expect_key( &i, target, VK_RETURN, 0x1c );
    for (j = 0; j < edit->erase; j++) expect_key( &i, target, VK_BACK, 0x0e );
    expect_text( &i, target, edit->text, edit->length );
    expect_key( &i, target, VK_RETURN, 0x1c );
    expect_key( &i, target, VK_ESCAPE, 0x01 );
    assert( i == posted_count );
}

static void test_voice_missing_focus(void)
{
    struct ps5_desktop_voice_edit edit = {
        .header = { PS5_DESKTOP_VOICE_EDIT, sizeof(edit) },
        .flags = PS5_DESKTOP_VOICE_FLAGS, .erase = 1 };
    unsigned int scenario;
    for (scenario = 0; scenario < 4; scenario++)
    {
        reset_queue();
        switch (scenario)
        {
        case 0: foreground = 0; break;
        case 1: foreground_thread = 0; break;
        case 2: gui_available = FALSE; break;
        case 3: focus = 0; break;
        }
        send_voice_edit( &edit );
        assert( !posted_count && !hardware_count );
    }
}

int main(void)
{
    test_voice_wire();
    test_voice_order();
    test_voice_missing_focus();
    puts( "ps5 input: voice framing, bounds, focus, key scans and posted FIFO passed" );
    return 0;
}
