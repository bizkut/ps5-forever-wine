/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Bounded PCM mixer for the PS5 Wine audio driver.
 *
 * Based on prospero-win src/pw_audio_mix.h (LGPL-2.1-or-later), adapted for
 * Wine's in-tree driver.  The caller owns storage and serialization; this
 * code performs no allocation or blocking.
 */
#ifndef __WINE_PS5_AUDIO_MIXER_H
#define __WINE_PS5_AUDIO_MIXER_H

#include <stdint.h>

enum
{
    PS5_AUDIO_RATE = 48000,
    PS5_AUDIO_GRAIN = 256,
    PS5_AUDIO_MAX_CHANNELS = 8,
    PS5_AUDIO_MIN_RATE = 1000,
    PS5_AUDIO_MAX_RATE = 384000,
    PS5_AUDIO_PCM = 1,
    PS5_AUDIO_FLOAT = 3,
};

enum ps5_audio_mix_result
{
    PS5_AUDIO_MIX_OK,
    PS5_AUDIO_MIX_PRECONDITION,
    PS5_AUDIO_MIX_UNSUPPORTED,
};

struct ps5_audio_format
{
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;
    uint16_t block_align;
    uint16_t encoding;
    uint32_t channel_mask;
};

struct ps5_audio_mix_stream
{
    struct ps5_audio_format format;
    const uint8_t *frames;
    uint32_t capacity;
    uint32_t offset, held;
    uint64_t consumed;
    float gain[PS5_AUDIO_MAX_CHANNELS];
    float left[PS5_AUDIO_MAX_CHANNELS], right[PS5_AUDIO_MAX_CHANNELS];
    uint64_t step, phase;
    float previous[2], current[2];
};

int ps5_audio_mix_format_check( const struct ps5_audio_format *format );
int ps5_audio_mix_stream_init( struct ps5_audio_mix_stream *stream,
                               const struct ps5_audio_format *format,
                               const uint8_t *frames, uint32_t capacity );
void ps5_audio_mix_stream_reset( struct ps5_audio_mix_stream *stream );
int ps5_audio_mix_set_rate( struct ps5_audio_mix_stream *stream, uint32_t rate );
int ps5_audio_mix_set_gain( struct ps5_audio_mix_stream *stream,
                            const float *gain, uint32_t count );
uint32_t ps5_audio_mix_add( struct ps5_audio_mix_stream *stream,
                            float *accumulator, uint32_t frames );
void ps5_audio_mix_store( const float *accumulator, int16_t *output, uint32_t frames );

#endif /* __WINE_PS5_AUDIO_MIXER_H */
