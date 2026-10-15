/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __WINE_MMDEVAPI_PS5AUDIO_H
#define __WINE_MMDEVAPI_PS5AUDIO_H

/* The native title publishes exactly one ASCII digit. Unknown/absent values
 * retain ordinary Wine registry behavior; only exact '1' enables title audio. */
static inline int ps5_title_audio_mode_from_value( unsigned int length, unsigned int first )
{
    if (length != 1) return -1;
    return first == '1';
}

#endif /* __WINE_MMDEVAPI_PS5AUDIO_H */
