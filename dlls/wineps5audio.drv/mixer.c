/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Bounded PCM mixer for the PS5 Wine audio driver.
 *
 * Based on prospero-win src/pw_audio_mix.c (LGPL-2.1-or-later), adapted for
 * Wine's in-tree driver.
 */
#if 0
#pragma makedep unix
#endif

#include <string.h>

#include "mixer.h"

#define PHASE_ONE ((uint64_t)1 << 32)
#define HALF_POWER 0.70710678f

enum
{
    FRONT_LEFT = 0x1,
    FRONT_RIGHT = 0x2,
    FRONT_CENTER = 0x4,
    LOW_FREQUENCY = 0x8,
    BACK_LEFT = 0x10,
    BACK_RIGHT = 0x20,
    FRONT_LEFT_OF_CENTER = 0x40,
    FRONT_RIGHT_OF_CENTER = 0x80,
    BACK_CENTER = 0x100,
    SIDE_LEFT = 0x200,
    SIDE_RIGHT = 0x400,
    TOP_CENTER = 0x800,
    TOP_FRONT_LEFT = 0x1000,
    TOP_FRONT_CENTER = 0x2000,
    TOP_FRONT_RIGHT = 0x4000,
    TOP_BACK_LEFT = 0x8000,
    TOP_BACK_CENTER = 0x10000,
    TOP_BACK_RIGHT = 0x20000,
};

#define LEFT_SIDE (BACK_LEFT | FRONT_LEFT_OF_CENTER | SIDE_LEFT | TOP_FRONT_LEFT | TOP_BACK_LEFT)
#define RIGHT_SIDE (BACK_RIGHT | FRONT_RIGHT_OF_CENTER | SIDE_RIGHT | TOP_FRONT_RIGHT | TOP_BACK_RIGHT)
#define CENTER (FRONT_CENTER | BACK_CENTER | TOP_CENTER | TOP_FRONT_CENTER | TOP_BACK_CENTER)

static const uint32_t usual_mask[PS5_AUDIO_MAX_CHANNELS + 1] =
{
    0,
    FRONT_CENTER,
    FRONT_LEFT | FRONT_RIGHT,
    FRONT_LEFT | FRONT_RIGHT | LOW_FREQUENCY,
    FRONT_LEFT | FRONT_RIGHT | BACK_LEFT | BACK_RIGHT,
    FRONT_LEFT | FRONT_RIGHT | BACK_LEFT | BACK_RIGHT | LOW_FREQUENCY,
    FRONT_LEFT | FRONT_RIGHT | FRONT_CENTER | LOW_FREQUENCY | BACK_LEFT | BACK_RIGHT,
    FRONT_LEFT | FRONT_RIGHT | FRONT_CENTER | LOW_FREQUENCY | BACK_LEFT | BACK_RIGHT | BACK_CENTER,
    FRONT_LEFT | FRONT_RIGHT | FRONT_CENTER | LOW_FREQUENCY | BACK_LEFT | BACK_RIGHT | SIDE_LEFT | SIDE_RIGHT,
};

static unsigned int bit_count( uint32_t mask )
{
    unsigned int count = 0;

    for (; mask; mask &= mask - 1) count++;
    return count;
}

int ps5_audio_mix_format_check( const struct ps5_audio_format *format )
{
    if (!format) return PS5_AUDIO_MIX_PRECONDITION;
    if (format->channels < 1 || format->channels > PS5_AUDIO_MAX_CHANNELS ||
        format->rate < PS5_AUDIO_MIN_RATE || format->rate > PS5_AUDIO_MAX_RATE)
        return PS5_AUDIO_MIX_UNSUPPORTED;
    if (format->encoding == PS5_AUDIO_PCM)
    {
        if (format->bits != 8 && format->bits != 16 && format->bits != 24 && format->bits != 32)
            return PS5_AUDIO_MIX_UNSUPPORTED;
    }
    else if (format->encoding != PS5_AUDIO_FLOAT || format->bits != 32)
        return PS5_AUDIO_MIX_UNSUPPORTED;
    return format->block_align == format->channels * (format->bits / 8u)
           ? PS5_AUDIO_MIX_OK : PS5_AUDIO_MIX_UNSUPPORTED;
}

static void fold_weights( struct ps5_audio_mix_stream *stream )
{
    const struct ps5_audio_format *format = &stream->format;
    uint32_t mask = format->channel_mask;
    unsigned int channel;

    if (bit_count( mask ) < format->channels) mask = usual_mask[format->channels];
    for (channel = 0; channel < format->channels; channel++)
    {
        uint32_t speaker = mask & -mask;
        float left = 0.0f, right = 0.0f;

        mask &= mask - 1;
        if (format->channels == 1) left = right = 1.0f;
        else if (speaker == FRONT_LEFT) left = 1.0f;
        else if (speaker == FRONT_RIGHT) right = 1.0f;
        else if (speaker & LEFT_SIDE) left = HALF_POWER;
        else if (speaker & RIGHT_SIDE) right = HALF_POWER;
        else if (speaker & CENTER) left = right = HALF_POWER;
        stream->left[channel] = left * stream->gain[channel];
        stream->right[channel] = right * stream->gain[channel];
    }
}

int ps5_audio_mix_stream_init( struct ps5_audio_mix_stream *stream,
                               const struct ps5_audio_format *format,
                               const uint8_t *frames, uint32_t capacity )
{
    int status;
    unsigned int channel;

    if (!stream || !format || !frames || !capacity) return PS5_AUDIO_MIX_PRECONDITION;
    if ((status = ps5_audio_mix_format_check( format )) != PS5_AUDIO_MIX_OK) return status;
    memset( stream, 0, sizeof(*stream) );
    stream->format = *format;
    stream->frames = frames;
    stream->capacity = capacity;
    for (channel = 0; channel < PS5_AUDIO_MAX_CHANNELS; channel++) stream->gain[channel] = 1.0f;
    fold_weights( stream );
    ps5_audio_mix_stream_reset( stream );
    return ps5_audio_mix_set_rate( stream, format->rate );
}

void ps5_audio_mix_stream_reset( struct ps5_audio_mix_stream *stream )
{
    if (!stream) return;
    stream->offset = stream->held = 0;
    stream->consumed = 0;
    stream->phase = PHASE_ONE;
    stream->previous[0] = stream->previous[1] = 0.0f;
    stream->current[0] = stream->current[1] = 0.0f;
}

int ps5_audio_mix_set_rate( struct ps5_audio_mix_stream *stream, uint32_t rate )
{
    if (!stream) return PS5_AUDIO_MIX_PRECONDITION;
    if (rate < PS5_AUDIO_MIN_RATE || rate > PS5_AUDIO_MAX_RATE) return PS5_AUDIO_MIX_UNSUPPORTED;
    stream->format.rate = rate;
    stream->step = ((uint64_t)rate << 32) / PS5_AUDIO_RATE;
    return PS5_AUDIO_MIX_OK;
}

int ps5_audio_mix_set_gain( struct ps5_audio_mix_stream *stream,
                            const float *gain, uint32_t count )
{
    uint32_t channel;

    if (!stream || !gain || count != stream->format.channels) return PS5_AUDIO_MIX_PRECONDITION;
    for (channel = 0; channel < count; channel++)
    {
        float value = gain[channel];
        stream->gain[channel] = value > 1.0f ? 1.0f : value > 0.0f ? value : 0.0f;
    }
    fold_weights( stream );
    return PS5_AUDIO_MIX_OK;
}

static float read_sample( const uint8_t *at, const struct ps5_audio_format *format )
{
    int32_t value;
    float result;

    switch (format->bits)
    {
    case 8:
        return ((int)at[0] - 128) * (1.0f / 128.0f);
    case 16:
        return (int16_t)(uint16_t)(at[0] | at[1] << 8) * (1.0f / 32768.0f);
    case 24:
        value = (int32_t)((uint32_t)at[0] << 8 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 24);
        return (float)(value >> 8) * (1.0f / 8388608.0f);
    default:
        if (format->encoding == PS5_AUDIO_FLOAT)
        {
            memcpy( &result, at, sizeof(result) );
            return result > 1.0f ? 1.0f : result < -1.0f ? -1.0f : result == result ? result : 0.0f;
        }
        value = (int32_t)((uint32_t)at[0] | (uint32_t)at[1] << 8 |
                          (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24);
        return (float)value * (1.0f / 2147483648.0f);
    }
}

static void take_frame( struct ps5_audio_mix_stream *stream )
{
    const struct ps5_audio_format *format = &stream->format;
    const uint8_t *at = stream->frames + (size_t)stream->offset * format->block_align;
    float left = 0.0f, right = 0.0f;
    unsigned int channel;

    for (channel = 0; channel < format->channels; channel++)
    {
        float value = read_sample( at + channel * (format->bits / 8u), format );
        left += value * stream->left[channel];
        right += value * stream->right[channel];
    }
    stream->previous[0] = stream->current[0];
    stream->previous[1] = stream->current[1];
    stream->current[0] = left;
    stream->current[1] = right;
    if (++stream->offset == stream->capacity) stream->offset = 0;
    stream->held--;
    stream->consumed++;
}

uint32_t ps5_audio_mix_add( struct ps5_audio_mix_stream *stream,
                            float *accumulator, uint32_t frames )
{
    uint32_t done;

    if (!stream || !accumulator) return 0;
    for (done = 0; done < frames; done++)
    {
        float fraction;

        while (stream->phase >= PHASE_ONE)
        {
            if (!stream->held) return done;
            take_frame( stream );
            stream->phase -= PHASE_ONE;
        }
        fraction = (float)(uint32_t)stream->phase * (1.0f / 4294967296.0f);
        accumulator[2 * done] += stream->previous[0] +
                                (stream->current[0] - stream->previous[0]) * fraction;
        accumulator[2 * done + 1] += stream->previous[1] +
                                    (stream->current[1] - stream->previous[1]) * fraction;
        stream->phase += stream->step;
    }
    return done;
}

void ps5_audio_mix_store( const float *accumulator, int16_t *output, uint32_t frames )
{
    uint32_t sample;

    if (!accumulator || !output) return;
    for (sample = 0; sample < 2 * frames; sample++)
    {
        float value = accumulator[sample];
        int converted;

        value = value > 1.0f ? 1.0f : value < -1.0f ? -1.0f : value;
        converted = (int)(value * 32767.0f + (value >= 0.0f ? 0.5f : -0.5f));
        output[sample] = converted;
    }
}
