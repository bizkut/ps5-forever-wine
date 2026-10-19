/* Shared HID/UTF-16 hardware-input conversion for PS5 drivers. */
#ifndef __WINE_PS5_INPUT_H
#define __WINE_PS5_INPUT_H
#include "wine/ps5_desktop.h"

struct hid_key
{
    WORD vkey;
    WORD scan;  /* E0 in the high byte for an extended PC key */
};

static const struct hid_key hid_keys[0xe8] =
{
    [0x04] = { 'A', 0x1e }, [0x05] = { 'B', 0x30 }, [0x06] = { 'C', 0x2e },
    [0x07] = { 'D', 0x20 }, [0x08] = { 'E', 0x12 }, [0x09] = { 'F', 0x21 },
    [0x0a] = { 'G', 0x22 }, [0x0b] = { 'H', 0x23 }, [0x0c] = { 'I', 0x17 },
    [0x0d] = { 'J', 0x24 }, [0x0e] = { 'K', 0x25 }, [0x0f] = { 'L', 0x26 },
    [0x10] = { 'M', 0x32 }, [0x11] = { 'N', 0x31 }, [0x12] = { 'O', 0x18 },
    [0x13] = { 'P', 0x19 }, [0x14] = { 'Q', 0x10 }, [0x15] = { 'R', 0x13 },
    [0x16] = { 'S', 0x1f }, [0x17] = { 'T', 0x14 }, [0x18] = { 'U', 0x16 },
    [0x19] = { 'V', 0x2f }, [0x1a] = { 'W', 0x11 }, [0x1b] = { 'X', 0x2d },
    [0x1c] = { 'Y', 0x15 }, [0x1d] = { 'Z', 0x2c },
    [0x1e] = { '1', 0x02 }, [0x1f] = { '2', 0x03 }, [0x20] = { '3', 0x04 },
    [0x21] = { '4', 0x05 }, [0x22] = { '5', 0x06 }, [0x23] = { '6', 0x07 },
    [0x24] = { '7', 0x08 }, [0x25] = { '8', 0x09 }, [0x26] = { '9', 0x0a },
    [0x27] = { '0', 0x0b }, [0x28] = { VK_RETURN, 0x1c }, [0x29] = { VK_ESCAPE, 0x01 },
    [0x2a] = { VK_BACK, 0x0e }, [0x2b] = { VK_TAB, 0x0f }, [0x2c] = { VK_SPACE, 0x39 },
    [0x2d] = { VK_OEM_MINUS, 0x0c }, [0x2e] = { VK_OEM_PLUS, 0x0d },
    [0x2f] = { VK_OEM_4, 0x1a }, [0x30] = { VK_OEM_6, 0x1b },
    [0x31] = { VK_OEM_5, 0x2b }, [0x32] = { VK_OEM_5, 0x2b },
    [0x33] = { VK_OEM_1, 0x27 }, [0x34] = { VK_OEM_7, 0x28 }, [0x35] = { VK_OEM_3, 0x29 },
    [0x36] = { VK_OEM_COMMA, 0x33 }, [0x37] = { VK_OEM_PERIOD, 0x34 }, [0x38] = { VK_OEM_2, 0x35 },
    [0x39] = { VK_CAPITAL, 0x3a },
    [0x3a] = { VK_F1, 0x3b }, [0x3b] = { VK_F2, 0x3c }, [0x3c] = { VK_F3, 0x3d },
    [0x3d] = { VK_F4, 0x3e }, [0x3e] = { VK_F5, 0x3f }, [0x3f] = { VK_F6, 0x40 },
    [0x40] = { VK_F7, 0x41 }, [0x41] = { VK_F8, 0x42 }, [0x42] = { VK_F9, 0x43 },
    [0x43] = { VK_F10, 0x44 }, [0x44] = { VK_F11, 0x57 }, [0x45] = { VK_F12, 0x58 },
    [0x46] = { VK_SNAPSHOT, 0xe037 }, [0x47] = { VK_SCROLL, 0x46 }, [0x48] = { VK_PAUSE, 0x45 },
    [0x49] = { VK_INSERT, 0xe052 }, [0x4a] = { VK_HOME, 0xe047 }, [0x4b] = { VK_PRIOR, 0xe049 },
    [0x4c] = { VK_DELETE, 0xe053 }, [0x4d] = { VK_END, 0xe04f }, [0x4e] = { VK_NEXT, 0xe051 },
    [0x4f] = { VK_RIGHT, 0xe04d }, [0x50] = { VK_LEFT, 0xe04b },
    [0x51] = { VK_DOWN, 0xe050 }, [0x52] = { VK_UP, 0xe048 },
    [0x53] = { VK_NUMLOCK, 0xe045 }, [0x54] = { VK_DIVIDE, 0xe035 },
    [0x55] = { VK_MULTIPLY, 0x37 }, [0x56] = { VK_SUBTRACT, 0x4a }, [0x57] = { VK_ADD, 0x4e },
    [0x58] = { VK_RETURN, 0xe01c },
    [0x59] = { VK_NUMPAD1, 0x4f }, [0x5a] = { VK_NUMPAD2, 0x50 }, [0x5b] = { VK_NUMPAD3, 0x51 },
    [0x5c] = { VK_NUMPAD4, 0x4b }, [0x5d] = { VK_NUMPAD5, 0x4c }, [0x5e] = { VK_NUMPAD6, 0x4d },
    [0x5f] = { VK_NUMPAD7, 0x47 }, [0x60] = { VK_NUMPAD8, 0x48 }, [0x61] = { VK_NUMPAD9, 0x49 },
    [0x62] = { VK_NUMPAD0, 0x52 }, [0x63] = { VK_DECIMAL, 0x53 },
    [0x64] = { VK_OEM_102, 0x56 }, [0x65] = { VK_APPS, 0xe05d },
    [0xe0] = { VK_LCONTROL, 0x1d }, [0xe1] = { VK_LSHIFT, 0x2a },
    [0xe2] = { VK_LMENU, 0x38 }, [0xe3] = { VK_LWIN, 0xe05b },
    [0xe4] = { VK_RCONTROL, 0xe01d }, [0xe5] = { VK_RSHIFT, 0x36 },
    [0xe6] = { VK_RMENU, 0xe038 }, [0xe7] = { VK_RWIN, 0xe05c },
};

static WORD pressed_vkeys[ARRAY_SIZE(hid_keys)];

static void send_key( const struct ps5_desktop_key *key )
{
    static const WORD keypad_nav[] =
    {
        VK_END, VK_DOWN, VK_NEXT, VK_LEFT, VK_CLEAR, VK_RIGHT,
        VK_HOME, VK_UP, VK_PRIOR, VK_INSERT, VK_DELETE,
    };
    INPUT input = { .type = INPUT_KEYBOARD };
    BYTE state[256];
    WORD vkey, scan;

    if (key->usage >= ARRAY_SIZE(hid_keys) || !(vkey = hid_keys[key->usage].vkey)) return;
    scan = hid_keys[key->usage].scan;
    if (pressed_vkeys[key->usage]) vkey = pressed_vkeys[key->usage];
    else if (key->usage >= 0x59 && key->usage <= 0x63 && NtUserGetAsyncKeyboardState( state ))
    {
        if (!(state[VK_NUMLOCK] & 1) != !!(state[VK_SHIFT] & 0x80))
            vkey = keypad_nav[key->usage - 0x59];
    }
    pressed_vkeys[key->usage] = key->down ? vkey : 0;
    input.ki.wVk = vkey;
    input.ki.wScan = scan & 0xff;
    if (scan >> 8 == 0xe0) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!key->down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    NtUserSendHardwareInput( 0, 0, &input, 0 );
}

static inline void send_text( const struct ps5_desktop_text *text )
{
    INPUT input = { .type = INPUT_KEYBOARD };
    unsigned int i;

    for (i = 0; i < text->length; i++)
    {
        input.ki.wScan = text->text[i];
        input.ki.dwFlags = KEYEVENTF_UNICODE;
        NtUserSendHardwareInput( 0, SEND_HWMSG_NO_RAW, &input, 0 );
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
        NtUserSendHardwareInput( 0, SEND_HWMSG_NO_RAW, &input, 0 );
    }
}

static inline void post_voice_key( HWND target, unsigned int usage )
{
    LPARAM lparam = 1 | (hid_keys[usage].scan << 16);
    NtUserPostMessage( target, WM_KEYDOWN, hid_keys[usage].vkey, lparam );
    NtUserPostMessage( target, WM_KEYUP, hid_keys[usage].vkey, lparam | 0xc0000000u );
}

/* The caller validates the complete frame before reading it. Both input
 * drains serialize dispatch, so the entire edit is posted before the next
 * batch or hardware event. server/queue.c reads posted messages before
 * hardware input. Never send these controls through send_key(), or text
 * can overtake them even when the native producer kept perfect FIFO order.
 *
 * Preserve control key messages: WM_CHAR alone is not an Enter/Escape key
 * press. Text is already committed, so do not send VK_PACKET too; WoW
 * does not TranslateMessage those keys and would lose the characters. */
static inline void send_voice_edit( const struct ps5_desktop_voice_edit *edit )
{
    HWND foreground = NtUserGetForegroundWindow(), target;
    GUITHREADINFO info = {.cbSize = sizeof(info)};
    DWORD thread = HandleToULong( NtUserQueryWindow( foreground, WindowThread ));
    unsigned int i;

    target = foreground && thread && NtUserGetGUIThreadInfo( thread, &info ) ? info.hwndFocus : 0;
    if (!target) return;
    if (edit->flags & PS5_DESKTOP_VOICE_ESCAPE_BEFORE) post_voice_key( target, 0x29 );
    if (edit->flags & PS5_DESKTOP_VOICE_ENTER_BEFORE) post_voice_key( target, 0x28 );
    for (i = 0; i < edit->erase; i++) post_voice_key( target, 0x2a );
    for (i = 0; i < edit->length; i++) NtUserPostMessage( target, WM_CHAR, edit->text[i], 1 );
    if (edit->flags & PS5_DESKTOP_VOICE_ENTER_AFTER) post_voice_key( target, 0x28 );
    if (edit->flags & PS5_DESKTOP_VOICE_ESCAPE_AFTER) post_voice_key( target, 0x29 );
}
#endif
