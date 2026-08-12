/* SPDX-License-Identifier: MIT
 *
 * Fixed-point N64-style audio primitives. See NOTICE.md for provenance.
 */
#ifndef LIBOOT_AUDIO_MIXER_H
#define LIBOOT_AUDIO_MIXER_H

#include <stdbool.h>
#include <stdint.h>

#define LIBOOT_AUDIO_Q15_ONE 32767

typedef struct LibootAudioRamp
{
    int32_t current;
    int32_t target;
    int64_t step;
    int64_t remainder;
    int64_t error;
    uint32_t remaining;
    uint32_t denominator;
} LibootAudioRamp;

int16_t liboot_audio_saturate_s16( int64_t value );
int32_t liboot_audio_saturate_i32( int64_t value );
int32_t liboot_audio_q15_mul( int32_t value, int32_t gain );
int32_t liboot_audio_float_to_q15( float value );

void liboot_audio_ramp_start( LibootAudioRamp *ramp, int32_t current,
                              int32_t target, uint32_t frames );
int32_t liboot_audio_ramp_tick( LibootAudioRamp *ramp );

void liboot_audio_pan_q15( uint16_t pan, int32_t *outLeft, int32_t *outRight );

/* Four-tap, 64-phase N64 ABI resampler. positionQ16 is an absolute source
 * position. Samples outside a non-looping stream are zero; taps beyond a
 * looping end wrap to loopStart. This function does not advance position. */
int16_t liboot_audio_resample_s16( const int16_t *pcm, uint32_t sampleCount,
                                  uint32_t loopStart, bool looping,
                                  uint64_t positionQ16 );

typedef struct LibootAudioReverb
{
    int16_t *delay;               /* interleaved stereo, delayFrames * 2 */
    uint32_t delayFrames;
    uint32_t position;
    int32_t inputGainQ15;
    int32_t feedbackQ15;
    int32_t outputGainQ15;
} LibootAudioReverb;

void liboot_audio_reverb_stereo( LibootAudioReverb *reverb,
                                 int32_t dryLeft, int32_t dryRight,
                                 int32_t sendLeft, int32_t sendRight,
                                 int16_t *outLeft, int16_t *outRight );

#endif
