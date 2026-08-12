/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "audio_mixer.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static void test_saturation_and_q15( void )
{
    assert( liboot_audio_saturate_s16( INT16_MIN - INT64_C(1) ) == INT16_MIN );
    assert( liboot_audio_saturate_s16( INT16_MAX + INT64_C(1) ) == INT16_MAX );
    assert( liboot_audio_saturate_s16( -1234 ) == -1234 );
    assert( liboot_audio_saturate_i32( INT64_MIN ) == INT32_MIN );
    assert( liboot_audio_saturate_i32( INT64_MAX ) == INT32_MAX );

    /* Frozen command-arithmetic vectors: Q15 is rounded, then shifted. */
    assert( liboot_audio_q15_mul( 32767, 32767 ) == 32766 );
    assert( liboot_audio_q15_mul( -32768, 32767 ) == -32767 );
    assert( liboot_audio_q15_mul( 10000, 16384 ) == 5000 );
    assert( liboot_audio_q15_mul( -10000, 16384 ) == -5000 );
    assert( liboot_audio_q15_mul( 12345, 0 ) == 0 );

    assert( liboot_audio_float_to_q15( -1.0f ) == 0 );
    assert( liboot_audio_float_to_q15( NAN ) == 0 );
    assert( liboot_audio_float_to_q15( 0.5f ) == 16384 );
    assert( liboot_audio_float_to_q15( 1.0f ) == 32767 );
}

static void test_exact_ramps( void )
{
    static const int32_t up[] = { 2, 5, 7, 10 };
    static const int32_t down[] = { 8, 5, 3, 0 };
    LibootAudioRamp ramp;

    liboot_audio_ramp_start( &ramp, 0, 10, 4u );
    for( size_t i = 0u; i < sizeof( up ) / sizeof( up[0] ); ++i )
        assert( liboot_audio_ramp_tick( &ramp ) == up[i] );
    assert( liboot_audio_ramp_tick( &ramp ) == 10 );

    liboot_audio_ramp_start( &ramp, 10, 0, 4u );
    for( size_t i = 0u; i < sizeof( down ) / sizeof( down[0] ); ++i )
        assert( liboot_audio_ramp_tick( &ramp ) == down[i] );
    assert( liboot_audio_ramp_tick( &ramp ) == 0 );

    liboot_audio_ramp_start( &ramp, -123, 456, 0u );
    assert( liboot_audio_ramp_tick( &ramp ) == 456 );

    liboot_audio_ramp_start( &ramp, INT32_MIN, INT32_MAX, 1u );
    assert( liboot_audio_ramp_tick( &ramp ) == INT32_MAX );
}

static void test_pan_vectors( void )
{
    int32_t left = -1;
    int32_t right = -1;
    liboot_audio_pan_q15( 0u, &left, &right );
    assert( left == 32767 && right == 0 );
    liboot_audio_pan_q15( 16384u, &left, &right );
    assert( left == 23169 && right == 23170 );
    liboot_audio_pan_q15( 32767u, &left, &right );
    assert( left == 0 && right == 32767 );
    liboot_audio_pan_q15( UINT16_MAX, &left, &right );
    assert( left == 0 && right == 32767 );
}

static void test_resample_vectors( void )
{
    static const int16_t impulse[] = { 0, 32767, 0, 0 };
    static const int16_t signedPcm[] = { 1000, -2000, 3000, -4000 };
    static const int16_t loopPcm[] = { 1000, 2000, 3000, 4000 };

    /* These values directly exercise three phases of the 64x4 ABI table. */
    assert( liboot_audio_resample_s16(
                impulse, 4u, 0u, false, UINT64_C(1) << 16 ) == 26284 );
    assert( liboot_audio_resample_s16(
                impulse, 4u, 0u, false,
                ( UINT64_C(1) << 16 ) | ( UINT64_C(32) << 10 )) == 16479 );
    assert( liboot_audio_resample_s16(
                impulse, 4u, 0u, false,
                ( UINT64_C(1) << 16 ) | ( UINT64_C(63) << 10 )) == 3398 );
    assert( liboot_audio_resample_s16(
                signedPcm, 4u, 0u, false, UINT64_C(1) << 16 ) == -1194 );

    /* At the stream end, looping taps wrap to loopStart; non-looping taps
       read zero. Keeping both vectors prevents an accidental clamp-to-end. */
    assert( liboot_audio_resample_s16(
                loopPcm, 4u, 2u, false, UINT64_C(3) << 16 ) == 3495 );
    assert( liboot_audio_resample_s16(
                loopPcm, 4u, 2u, true, UINT64_C(3) << 16 ) == 3802 );
    assert( liboot_audio_resample_s16( NULL, 4u, 0u, false, 0u ) == 0 );
}

static void test_reverb_vectors( void )
{
    int16_t delay[4] = { 0, 0, 0, 0 };
    LibootAudioReverb reverb = {
        delay, 2u, 0u, 16384, 16384, 16384
    };
    int16_t left = 0;
    int16_t right = 0;

    liboot_audio_reverb_stereo( &reverb, 10000, -10000, 10000, -10000,
                                &left, &right );
    assert( left == 10000 && right == -10000 );
    assert( delay[0] == 5000 && delay[1] == -5000 );
    assert( reverb.position == 1u );

    liboot_audio_reverb_stereo( &reverb, 0, 0, 0, 0, &left, &right );
    assert( left == 0 && right == 0 && reverb.position == 0u );
    liboot_audio_reverb_stereo( &reverb, 0, 0, 0, 0, &left, &right );
    assert( left == 2500 && right == -2500 );
    assert( delay[0] == 2500 && delay[1] == -2500 );

    liboot_audio_reverb_stereo( NULL, INT32_MAX, INT32_MIN, 0, 0,
                                &left, &right );
    assert( left == INT16_MAX && right == INT16_MIN );

    /* All command inputs are int32_t. Their sums must saturate through a
       wider intermediate instead of overflowing before the final S16 clamp. */
    delay[0] = INT16_MAX;
    delay[1] = INT16_MIN;
    reverb.position = 0u;
    reverb.inputGainQ15 = INT32_MAX;
    reverb.feedbackQ15 = INT32_MAX;
    reverb.outputGainQ15 = INT32_MAX;
    liboot_audio_reverb_stereo( &reverb, INT32_MAX, INT32_MIN,
                                INT32_MAX, INT32_MIN, &left, &right );
    assert( left == INT16_MAX && right == INT16_MIN );
    assert( delay[0] == INT16_MAX && delay[1] == INT16_MIN );
}

int main( void )
{
    test_saturation_and_q15();
    test_exact_ramps();
    test_pan_vectors();
    test_resample_vectors();
    test_reverb_vectors();
    puts( "audio fixed-mixer vectors: ok" );
    return 0;
}
