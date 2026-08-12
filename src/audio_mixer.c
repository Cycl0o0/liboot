/* SPDX-License-Identifier: MIT
 *
 * The resampling coefficients and command arithmetic are adapted from the
 * LibUltraShip/Ship of Harkinian portable N64 ABI mixer (Copyright (c) 2022
 * Kenix3). The surrounding cursor, ramp, pan, saturation, and reverb API is
 * a liboot integration. See NOTICE.md and LICENSES/LibUltraShip-MIT.txt.
 */
#include "audio_mixer.h"

#include <limits.h>
#include <stddef.h>

/* Coefficients used by the N64 audio ABI's four-tap, 64-phase resampler. */
static const int16_t sResampleTable[64][4] = {
    { 0x0C39, 0x66AD, 0x0D46, (int16_t)0xFFDF },
    { 0x0B39, 0x6696, 0x0E5F, (int16_t)0xFFD8 },
    { 0x0A44, 0x6669, 0x0F83, (int16_t)0xFFD0 },
    { 0x095A, 0x6626, 0x10B4, (int16_t)0xFFC8 },
    { 0x087D, 0x65CD, 0x11F0, (int16_t)0xFFBF },
    { 0x07AB, 0x655E, 0x1338, (int16_t)0xFFB6 },
    { 0x06E4, 0x64D9, 0x148C, (int16_t)0xFFAC },
    { 0x0628, 0x643F, 0x15EB, (int16_t)0xFFA1 },
    { 0x0577, 0x638F, 0x1756, (int16_t)0xFF96 },
    { 0x04D1, 0x62CB, 0x18CB, (int16_t)0xFF8A },
    { 0x0435, 0x61F3, 0x1A4C, (int16_t)0xFF7E },
    { 0x03A4, 0x6106, 0x1BD7, (int16_t)0xFF71 },
    { 0x031C, 0x6007, 0x1D6C, (int16_t)0xFF64 },
    { 0x029F, 0x5EF5, 0x1F0B, (int16_t)0xFF56 },
    { 0x022A, 0x5DD0, 0x20B3, (int16_t)0xFF48 },
    { 0x01BE, 0x5C9A, 0x2264, (int16_t)0xFF3A },
    { 0x015B, 0x5B53, 0x241E, (int16_t)0xFF2C },
    { 0x0101, 0x59FC, 0x25E0, (int16_t)0xFF1E },
    { 0x00AE, 0x5896, 0x27A9, (int16_t)0xFF10 },
    { 0x0063, 0x5720, 0x297A, (int16_t)0xFF02 },
    { 0x001F, 0x559D, 0x2B50, (int16_t)0xFEF4 },
    { (int16_t)0xFFE2, 0x540D, 0x2D2C, (int16_t)0xFEE8 },
    { (int16_t)0xFFAC, 0x5270, 0x2F0D, (int16_t)0xFEDB },
    { (int16_t)0xFF7C, 0x50C7, 0x30F3, (int16_t)0xFED0 },
    { (int16_t)0xFF53, 0x4F14, 0x32DC, (int16_t)0xFEC6 },
    { (int16_t)0xFF2E, 0x4D57, 0x34C8, (int16_t)0xFEBD },
    { (int16_t)0xFF0F, 0x4B91, 0x36B6, (int16_t)0xFEB6 },
    { (int16_t)0xFEF5, 0x49C2, 0x38A5, (int16_t)0xFEB0 },
    { (int16_t)0xFEDF, 0x47ED, 0x3A95, (int16_t)0xFEAC },
    { (int16_t)0xFECE, 0x4611, 0x3C85, (int16_t)0xFEAB },
    { (int16_t)0xFEC0, 0x4430, 0x3E74, (int16_t)0xFEAC },
    { (int16_t)0xFEB6, 0x424A, 0x4060, (int16_t)0xFEAF },
    { (int16_t)0xFEAF, 0x4060, 0x424A, (int16_t)0xFEB6 },
    { (int16_t)0xFEAC, 0x3E74, 0x4430, (int16_t)0xFEC0 },
    { (int16_t)0xFEAB, 0x3C85, 0x4611, (int16_t)0xFECE },
    { (int16_t)0xFEAC, 0x3A95, 0x47ED, (int16_t)0xFEDF },
    { (int16_t)0xFEB0, 0x38A5, 0x49C2, (int16_t)0xFEF5 },
    { (int16_t)0xFEB6, 0x36B6, 0x4B91, (int16_t)0xFF0F },
    { (int16_t)0xFEBD, 0x34C8, 0x4D57, (int16_t)0xFF2E },
    { (int16_t)0xFEC6, 0x32DC, 0x4F14, (int16_t)0xFF53 },
    { (int16_t)0xFED0, 0x30F3, 0x50C7, (int16_t)0xFF7C },
    { (int16_t)0xFEDB, 0x2F0D, 0x5270, (int16_t)0xFFAC },
    { (int16_t)0xFEE8, 0x2D2C, 0x540D, (int16_t)0xFFE2 },
    { (int16_t)0xFEF4, 0x2B50, 0x559D, 0x001F },
    { (int16_t)0xFF02, 0x297A, 0x5720, 0x0063 },
    { (int16_t)0xFF10, 0x27A9, 0x5896, 0x00AE },
    { (int16_t)0xFF1E, 0x25E0, 0x59FC, 0x0101 },
    { (int16_t)0xFF2C, 0x241E, 0x5B53, 0x015B },
    { (int16_t)0xFF3A, 0x2264, 0x5C9A, 0x01BE },
    { (int16_t)0xFF48, 0x20B3, 0x5DD0, 0x022A },
    { (int16_t)0xFF56, 0x1F0B, 0x5EF5, 0x029F },
    { (int16_t)0xFF64, 0x1D6C, 0x6007, 0x031C },
    { (int16_t)0xFF71, 0x1BD7, 0x6106, 0x03A4 },
    { (int16_t)0xFF7E, 0x1A4C, 0x61F3, 0x0435 },
    { (int16_t)0xFF8A, 0x18CB, 0x62CB, 0x04D1 },
    { (int16_t)0xFF96, 0x1756, 0x638F, 0x0577 },
    { (int16_t)0xFFA1, 0x15EB, 0x643F, 0x0628 },
    { (int16_t)0xFFAC, 0x148C, 0x64D9, 0x06E4 },
    { (int16_t)0xFFB6, 0x1338, 0x655E, 0x07AB },
    { (int16_t)0xFFBF, 0x11F0, 0x65CD, 0x087D },
    { (int16_t)0xFFC8, 0x10B4, 0x6626, 0x095A },
    { (int16_t)0xFFD0, 0x0F83, 0x6669, 0x0A44 },
    { (int16_t)0xFFD8, 0x0E5F, 0x6696, 0x0B39 },
    { (int16_t)0xFFDF, 0x0D46, 0x66AD, 0x0C39 }
};

int16_t liboot_audio_saturate_s16( int64_t value )
{
    if( value < INT16_MIN ) return INT16_MIN;
    if( value > INT16_MAX ) return INT16_MAX;
    return (int16_t)value;
}

int32_t liboot_audio_saturate_i32( int64_t value )
{
    if( value < INT32_MIN ) return INT32_MIN;
    if( value > INT32_MAX ) return INT32_MAX;
    return (int32_t)value;
}

/* C leaves right-shifting negative signed values implementation-defined.
 * Spell out floor((value + 0.5) / 2^15) so every host gets the command
 * arithmetic represented by the frozen vectors. Products of two int32_t
 * values leave enough room for the rounding bias in int64_t. */
static int64_t rounded_shift_q15( int64_t product )
{
    int64_t biased = product + INT64_C(0x4000);
    if( biased >= 0 ) return biased / INT64_C(0x8000);
    return -( ( -biased + INT64_C(0x7FFF) ) / INT64_C(0x8000) );
}

int32_t liboot_audio_q15_mul( int32_t value, int32_t gain )
{
    int64_t product = (int64_t)value * gain;
    return liboot_audio_saturate_i32( rounded_shift_q15( product ));
}

int32_t liboot_audio_float_to_q15( float value )
{
    if( !( value > 0.0f )) return 0;
    if( value >= 1.0f ) return LIBOOT_AUDIO_Q15_ONE;
    return (int32_t)( value * LIBOOT_AUDIO_Q15_ONE + 0.5f );
}

void liboot_audio_ramp_start( LibootAudioRamp *ramp, int32_t current,
                              int32_t target, uint32_t frames )
{
    if( ramp == NULL ) return;
    ramp->current = current;
    ramp->target = target;
    ramp->remaining = frames;
    ramp->denominator = frames;
    ramp->error = 0;
    if( frames == 0u ) {
        ramp->current = target;
        ramp->step = 0;
        ramp->remainder = 0;
        return;
    }
    int64_t delta = (int64_t)target - current;
    ramp->step = delta / (int64_t)frames;
    ramp->remainder = delta % (int64_t)frames;
}

int32_t liboot_audio_ramp_tick( LibootAudioRamp *ramp )
{
    if( ramp == NULL ) return 0;
    if( ramp->remaining == 0u ) return ramp->current;
    uint32_t divisor = ramp->denominator;
    int64_t next = (int64_t)ramp->current + ramp->step;
    ramp->error += ramp->remainder;
    if( ramp->error >= (int64_t)divisor ) {
        next++;
        ramp->error -= divisor;
    } else if( ramp->error <= -(int64_t)divisor ) {
        next--;
        ramp->error += divisor;
    }
    ramp->current = liboot_audio_saturate_i32( next );
    ramp->remaining--;
    if( ramp->remaining == 0u ) ramp->current = ramp->target;
    return ramp->current;
}

static uint32_t integer_sqrt_u64( uint64_t value )
{
    uint64_t result = 0u;
    uint64_t bit = UINT64_C(1) << 62;
    while( bit > value ) bit >>= 2;
    while( bit != 0u ) {
        if( value >= result + bit ) {
            value -= result + bit;
            result = ( result >> 1 ) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

void liboot_audio_pan_q15( uint16_t pan, int32_t *outLeft, int32_t *outRight )
{
    uint32_t p = pan > LIBOOT_AUDIO_Q15_ONE ? LIBOOT_AUDIO_Q15_ONE : pan;
    uint64_t leftPower = (uint64_t)( LIBOOT_AUDIO_Q15_ONE - p ) *
                         LIBOOT_AUDIO_Q15_ONE;
    uint64_t rightPower = (uint64_t)p * LIBOOT_AUDIO_Q15_ONE;
    if( outLeft != NULL ) *outLeft = (int32_t)integer_sqrt_u64( leftPower );
    if( outRight != NULL ) *outRight = (int32_t)integer_sqrt_u64( rightPower );
}

static int16_t source_sample( const int16_t *pcm, uint32_t sampleCount,
                              uint32_t loopStart, bool looping, int64_t index )
{
    if( pcm == NULL || sampleCount == 0u || index < 0 ) return 0;
    uint64_t at = (uint64_t)index;
    if( at < sampleCount ) return pcm[at];
    if( !looping || loopStart >= sampleCount ) return 0;
    uint32_t loopLength = sampleCount - loopStart;
    return pcm[loopStart + (uint32_t)(( at - loopStart ) % loopLength )];
}

int16_t liboot_audio_resample_s16( const int16_t *pcm, uint32_t sampleCount,
                                  uint32_t loopStart, bool looping,
                                  uint64_t positionQ16 )
{
    if( pcm == NULL || sampleCount == 0u ) return 0;
    uint32_t phase = (uint32_t)(( positionQ16 >> 10 ) & 0x3Fu );
    int64_t center = (int64_t)( positionQ16 >> 16 );
    int32_t mixed = 0;
    for( int tap = 0; tap < 4; ++tap ) {
        int32_t sample = source_sample( pcm, sampleCount, loopStart, looping,
                                        center + tap - 1 );
        int64_t product = (int64_t)sample * sResampleTable[phase][tap];
        mixed += (int32_t)rounded_shift_q15( product );
    }
    return liboot_audio_saturate_s16( mixed );
}

void liboot_audio_reverb_stereo( LibootAudioReverb *reverb,
                                 int32_t dryLeft, int32_t dryRight,
                                 int32_t sendLeft, int32_t sendRight,
                                 int16_t *outLeft, int16_t *outRight )
{
    int32_t wetLeft = 0;
    int32_t wetRight = 0;
    if( reverb != NULL && reverb->delay != NULL && reverb->delayFrames != 0u ) {
        uint32_t at = reverb->position * 2u;
        wetLeft = reverb->delay[at];
        wetRight = reverb->delay[at + 1u];
        int64_t nextLeft =
            (int64_t)liboot_audio_q15_mul( sendLeft, reverb->inputGainQ15 ) +
            liboot_audio_q15_mul( wetLeft, reverb->feedbackQ15 );
        int64_t nextRight =
            (int64_t)liboot_audio_q15_mul( sendRight, reverb->inputGainQ15 ) +
            liboot_audio_q15_mul( wetRight, reverb->feedbackQ15 );
        reverb->delay[at] = liboot_audio_saturate_s16( nextLeft );
        reverb->delay[at + 1u] = liboot_audio_saturate_s16( nextRight );
        reverb->position = ( reverb->position + 1u ) % reverb->delayFrames;
        dryLeft = liboot_audio_saturate_i32(
            (int64_t)dryLeft +
            liboot_audio_q15_mul( wetLeft, reverb->outputGainQ15 ));
        dryRight = liboot_audio_saturate_i32(
            (int64_t)dryRight +
            liboot_audio_q15_mul( wetRight, reverb->outputGainQ15 ));
    }
    if( outLeft != NULL ) *outLeft = liboot_audio_saturate_s16( dryLeft );
    if( outRight != NULL ) *outRight = liboot_audio_saturate_s16( dryRight );
}
