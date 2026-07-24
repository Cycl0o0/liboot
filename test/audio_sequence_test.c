#include "../src/liboot.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t *read_file( const char *path, size_t *size )
{
    FILE *file = fopen( path, "rb" );
    if( !file ) return NULL;
    if( fseek( file, 0, SEEK_END ) != 0 ) { fclose( file ); return NULL; }
    long end = ftell( file );
    if( end <= 0 || fseek( file, 0, SEEK_SET ) != 0 ) { fclose( file ); return NULL; }
    uint8_t *data = malloc((size_t)end );
    if( !data || fread( data, 1, (size_t)end, file ) != (size_t)end ) {
        free( data ); fclose( file ); return NULL;
    }
    fclose( file );
    *size = (size_t)end;
    return data;
}

static double render_seconds( unsigned seconds, float *peak )
{
    enum { BLOCK = 257, RATE = 32000 };
    float stereo[BLOCK * 2];
    uint32_t remaining = RATE * seconds;
    double energy = 0.0;
    *peak = 0.0f;
    while( remaining ) {
        uint32_t frames = remaining < BLOCK ? remaining : BLOCK;
        assert( oot_audio_render_f32( stereo, frames, RATE ) == frames );
        for( uint32_t i = 0; i < frames * 2; ++i ) {
            assert( isfinite( stereo[i] ));
            float a = fabsf( stereo[i] );
            if( a > *peak ) *peak = a;
            energy += (double)stereo[i] * stereo[i];
        }
        remaining -= frames;
    }
    return energy;
}

static double render_frame_count( uint32_t remaining, float *peak )
{
    enum { BLOCK = 257, RATE = 32000 };
    float stereo[BLOCK * 2];
    double energy = 0.0;
    *peak = 0.0f;
    while( remaining ) {
        uint32_t frames = remaining < BLOCK ? remaining : BLOCK;
        assert( oot_audio_render_f32( stereo, frames, RATE ) == frames );
        for( uint32_t i = 0; i < frames * 2; ++i ) {
            assert( isfinite( stereo[i] ));
            float a = fabsf( stereo[i] );
            if( a > *peak ) *peak = a;
            energy += (double)stereo[i] * stereo[i];
        }
        remaining -= frames;
    }
    return energy;
}

struct PitchProbe {
    double hz;
    double rms;
    double madRatio;
    size_t periods;
};

#define TEST_TWO_PI 6.283185307179586476925286766559
#define MAX_SPECTRAL_WINDOW 8192u

static int compare_double( const void *left, const void *right )
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return ( a > b ) - ( a < b );
}

static double median( double *values, size_t count )
{
    qsort( values, count, sizeof( *values ), compare_double );
    return count & 1u ? values[count / 2u] :
                        0.5 * ( values[count / 2u - 1u] + values[count / 2u] );
}

static struct PitchProbe pitch_probe( const float *mono, size_t frames,
                                      unsigned rate )
{
    struct PitchProbe result = { NAN, 0.0, NAN, 0 };
    double mean = 0.0;
    for( size_t i = 0; i < frames; ++i ) mean += mono[i];
    mean /= frames;
    for( size_t i = 0; i < frames; ++i ) {
        double sample = mono[i] - mean;
        result.rms += sample * sample;
    }
    result.rms = sqrt( result.rms / frames );
    if( result.rms < 1e-5 ) return result;

    double periods[256];
    double previousCrossing = -1.0;
    double previous = mono[0] - mean;
    for( size_t i = 1; i < frames; ++i ) {
        double current = mono[i] - mean;
        if( previous <= 0.0 && current > 0.0 && current != previous ) {
            double crossing = (double)( i - 1u ) - previous / ( current - previous );
            if( previousCrossing >= 0.0 && result.periods < 256u ) {
                double period = crossing - previousCrossing;
                if( period >= rate / 2000.0 && period <= rate / 200.0 )
                    periods[result.periods++] = period;
            }
            previousCrossing = crossing;
        }
        previous = current;
    }
    if( result.periods < 8u ) return result;

    double period = median( periods, result.periods );
    double deviations[256];
    for( size_t i = 0; i < result.periods; ++i )
        deviations[i] = fabs( periods[i] - period );
    result.madRatio = median( deviations, result.periods ) / period;
    result.hz = rate / period;
    return result;
}

static double band_peak_hz( const float *mono, size_t frames, unsigned rate,
                            unsigned minHz, unsigned maxHz )
{
    assert( frames > 1 && frames <= MAX_SPECTRAL_WINDOW && minHz <= maxHz );
    double mean = 0.0;
    for( size_t i = 0; i < frames; ++i ) mean += mono[i];
    mean /= frames;

    double windowed[MAX_SPECTRAL_WINDOW];
    for( size_t i = 0; i < frames; ++i ) {
        double hann = 0.5 - 0.5 * cos( TEST_TWO_PI * i / ( frames - 1u ));
        windowed[i] = ( mono[i] - mean ) * hann;
    }

    double bestPower = -1.0;
    unsigned bestHz = minHz;
    for( unsigned hz = minHz; hz <= maxHz; ++hz ) {
        double coefficient = 2.0 * cos( TEST_TWO_PI * hz / rate );
        double previous = 0.0, beforePrevious = 0.0;
        for( size_t i = 0; i < frames; ++i ) {
            double current = windowed[i] + coefficient * previous - beforePrevious;
            beforePrevious = previous;
            previous = current;
        }
        double power = previous * previous + beforePrevious * beforePrevious -
                       coefficient * previous * beforePrevious;
        if( power > bestPower ) {
            bestPower = power;
            bestHz = hz;
        }
    }
    assert( bestPower > 0.0 );
    return bestHz;
}

static double spectral_proxy_hz( const float *mono, size_t frames,
                                 unsigned rate )
{
    assert( frames > 1 );
    double mean = 0.0;
    for( size_t i = 0; i < frames; ++i ) mean += mono[i];
    mean /= frames;

    double previous = 0.0;
    double energy = 0.0, differenceEnergy = 0.0;
    for( size_t i = 0; i < frames; ++i ) {
        double hann = 0.5 - 0.5 * cos( TEST_TWO_PI * i / ( frames - 1u ));
        double current = ( mono[i] - mean ) * hann;
        if( i != 0 ) {
            double difference = current - previous;
            differenceEnergy += difference * difference;
            energy += previous * previous;
        }
        previous = current;
    }
    assert( energy > 0.0 );
    return rate / TEST_TWO_PI * sqrt( differenceEnergy / energy );
}

static void capture_sfx_from_onset( float *mono, uint32_t captureFrames )
{
    enum { BLOCK = 16, RATE = 32000 };
    float stereo[BLOCK * 2];
    uint32_t waited = 0;
    uint32_t written = 0;
    while( written == 0 ) {
        assert( waited < RATE );
        assert( oot_audio_render_f32( stereo, BLOCK, RATE ) == BLOCK );
        waited += BLOCK;
        struct OoTAudioState state = {
            .structSize = sizeof( state ), .version = OOT_AUDIO_STATE_VERSION
        };
        assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &state ));
        if( state.activeVoices == 0 ) continue;
        for( uint32_t i = 0; i < BLOCK && written < captureFrames; ++i )
            mono[written++] = 0.5f * ( stereo[i * 2] + stereo[i * 2 + 1] );
    }
    while( written < captureFrames ) {
        uint32_t frames = captureFrames - written;
        if( frames > BLOCK ) frames = BLOCK;
        assert( oot_audio_render_f32( stereo, frames, RATE ) == frames );
        for( uint32_t i = 0; i < frames; ++i )
            mono[written + i] = 0.5f * ( stereo[i * 2] + stereo[i * 2 + 1] );
        written += frames;
    }
}

static void render_stereo_energy( unsigned seconds, double *left, double *right )
{
    enum { BLOCK = 257, RATE = 32000 };
    float stereo[BLOCK * 2];
    uint32_t remaining = RATE * seconds;
    *left = 0.0;
    *right = 0.0;
    while( remaining ) {
        uint32_t frames = remaining < BLOCK ? remaining : BLOCK;
        assert( oot_audio_render_f32( stereo, frames, RATE ) == frames );
        for( uint32_t i = 0; i < frames; ++i ) {
            assert( isfinite( stereo[i * 2] ) && isfinite( stereo[i * 2 + 1] ));
            *left += (double)stereo[i * 2] * stereo[i * 2];
            *right += (double)stereo[i * 2 + 1] * stereo[i * 2 + 1];
        }
        remaining -= frames;
    }
}

static double render_sfx_frames( uint32_t remaining, float *peak, uint32_t *maxVoices )
{
    enum { BLOCK = 257, RATE = 32000 };
    float stereo[BLOCK * 2];
    double energy = 0.0;
    *peak = 0.0f;
    *maxVoices = 0;
    while( remaining ) {
        uint32_t frames = remaining < BLOCK ? remaining : BLOCK;
        assert( oot_audio_render_f32( stereo, frames, RATE ) == frames );
        for( uint32_t i = 0; i < frames * 2; ++i ) {
            assert( isfinite( stereo[i] ));
            float a = fabsf( stereo[i] );
            if( a > *peak ) *peak = a;
            energy += (double)stereo[i] * stereo[i];
        }
        struct OoTAudioState state = {
            .structSize = sizeof( state ), .version = OOT_AUDIO_STATE_VERSION
        };
        assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &state ));
        if( state.activeVoices > *maxVoices ) *maxVoices = state.activeVoices;
        remaining -= frames;
    }
    return energy;
}

static double render_sfx_seconds( unsigned seconds, float *peak, uint32_t *maxVoices )
{
    return render_sfx_frames( 32000u * seconds, peak, maxVoices );
}

static bool sfx_is_retail_silent( uint16_t id )
{
    switch( id ) {
    case 0x28E5: case 0x28F5: case 0x38FE: case 0x4821: case 0x4822:
    case 0x6840: case 0x6841: case 0x6842: case 0x687C:
        return true;
    default:
        return false;
    }
}

int main( int argc, char **argv )
{
    if( argc != 2 ) {
        fprintf( stderr, "usage: %s <oot.z64>\n", argv[0] );
        return 2;
    }
    size_t romSize = 0;
    uint8_t *rom = read_file( argv[1], &romSize );
    assert( rom != NULL );
    oot_global_init( rom, romSize, NULL );

    assert( oot_audio_sequence_count() == OOT_AUDIO_SEQUENCE_COUNT );
    assert( !oot_audio_sequence_play( UINT8_MAX, OOT_AUDIO_NO_MUSIC, 0 ));
    struct OoTSequenceInfo info = {
        .structSize = sizeof( info ), .version = OOT_SEQUENCE_INFO_VERSION
    };
    assert( oot_audio_sequence_get_info( 87, &info ));
    assert( info.sequenceId == 87 && info.resolvedId == 40 && info.isAlias );
    assert( info.dataSize != 0 && info.fontCount != 0 );
    assert( !oot_audio_sequence_get_info( 110, &info ));
    for( uint16_t id = 0; id < OOT_AUDIO_SEQUENCE_COUNT; ++id )
        assert( oot_audio_sequence_prewarm( id ));
    assert( !oot_audio_sequence_prewarm( OOT_AUDIO_SEQUENCE_COUNT ));

    int32_t catalogCount = oot_audio_sfx_catalog_count();
    assert( catalogCount == 1259 );
    uint16_t previous = 0;
    for( int32_t i = 0; i < catalogCount; ++i ) {
        struct OoTSfxInfo sfx = {
            .structSize = sizeof( sfx ), .version = OOT_SFX_INFO_VERSION
        };
        assert( oot_audio_sfx_catalog_get( i, &sfx ));
        assert( sfx.name[0] != '\0' && sfx.bank < 7 );
        assert( i == 0 || sfx.sfxId > previous );
        previous = sfx.sfxId;
    }

    /* Every selector entry must be accepted by Sequence 0 and survive its
       initial dispatch ticks. Some retail rows are intentionally silent, so
       audibility is asserted below on representative scripted effects. */
    for( int32_t i = 0; i < catalogCount; ++i ) {
        struct OoTSfxInfo sfx = {
            .structSize = sizeof( sfx ), .version = OOT_SFX_INFO_VERSION
        };
        assert( oot_audio_sfx_catalog_get( i, &sfx ));
        assert( oot_audio_sfx_play( sfx.sfxId, 0.0f, 0.8f ));
        float selectorPeak;
        uint32_t selectorVoices;
        double selectorEnergy = render_sfx_frames( 32000, &selectorPeak,
                                                   &selectorVoices );
        struct OoTAudioState state = {
            .structSize = sizeof( state ), .version = OOT_AUDIO_STATE_VERSION
        };
        assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &state ));
        assert( state.playing && !state.finished && state.sequenceId == 0 );
        if( sfx_is_retail_silent( sfx.sfxId )) assert( selectorVoices == 0 );
        else if( selectorVoices == 0 || selectorEnergy <= 0.0 || selectorPeak <= 0.0f ) {
            fprintf( stderr, "SFX 0x%04X %s silent after 1 s (voices %u energy %.6f peak %.6f)\n",
                     sfx.sfxId, sfx.name, selectorVoices, selectorEnergy, selectorPeak );
            assert( selectorVoices > 0 && selectorEnergy > 0.0 && selectorPeak > 0.0f );
        }
        oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SFX, 0 );
    }

    /* Smoke every retail table entry long enough to execute its setup and
       first channel/layer commands. This catches an opcode-length mismatch
       anywhere in the 110-sequence catalog, not only in the map subset. */
    unsigned earlyFailures = 0;
    for( uint16_t id = 0; id < OOT_AUDIO_SEQUENCE_COUNT; ++id ) {
        if( !oot_audio_sequence_play( OOT_AUDIO_PLAYER_MAIN, id, 0 )) {
            fprintf( stderr, "seq %u failed to load\n", id );
            ++earlyFailures;
            continue;
        }
        oot_audio_sequence_set_io( OOT_AUDIO_PLAYER_MAIN, 0, 0 );
        oot_audio_channel_set_io( OOT_AUDIO_PLAYER_MAIN, 0, 0, 0 );
        float scratch[4096 * 2];
        assert( oot_audio_render_f32( scratch, 4096, 32000 ) == 4096 );
        struct OoTAudioState state = {
            .structSize = sizeof( state ), .version = OOT_AUDIO_STATE_VERSION
        };
        assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_MAIN, &state ));
        if( state.finished ) {
            fprintf( stderr, "seq %u (%s) terminated during setup\n",
                     id, oot_audio_sequence_name( id ));
            ++earlyFailures;
        }
    }
    assert( earlyFailures == 0 );

    /* A naturally ending sequence can still own release tails after it marks
       itself stopped. An explicit zero-fade stop must kill those voices too. */
    assert( oot_audio_sequence_play( OOT_AUDIO_PLAYER_MAIN, 4, 0 ));
    bool sawNaturalFinish = false, sawNaturalTail = false;
    for( uint32_t rendered = 0; rendered < 15u * 32000u; rendered += 257 ) {
        float tailScratch[257 * 2];
        assert( oot_audio_render_f32( tailScratch, 257, 32000 ) == 257 );
        struct OoTAudioState state = {
            .structSize = sizeof( state ), .version = OOT_AUDIO_STATE_VERSION
        };
        assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_MAIN, &state ));
        if( !state.playing ) {
            sawNaturalFinish = true;
            sawNaturalTail = state.activeVoices > 0;
            break;
        }
    }
    assert( sawNaturalFinish && sawNaturalTail );
    oot_audio_stop_all( 0 );
    struct OoTAudioState stoppedState = {
        .structSize = sizeof( stoppedState ), .version = OOT_AUDIO_STATE_VERSION
    };
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_MAIN, &stoppedState ));
    assert( stoppedState.activeChannels == 0 && stoppedState.activeVoices == 0 );

    /* Every directly referenced map theme must keep interpreting and produce
       finite ROM-sample audio.  Irregular 257-frame pulls exercise callback
       block-size independence. */
    static const uint8_t mapSequences[] = {
        24, 28, 31, 38, 39, 42, 44, 58, 60, 63, 80, 88, 91, 92
    };
    for( size_t i = 0; i < sizeof( mapSequences ); ++i ) {
        uint8_t id = mapSequences[i];
        assert( oot_audio_sequence_play( OOT_AUDIO_PLAYER_MAIN, id, 0 ));
        float peak;
        double energy = render_seconds( 2, &peak );
        struct OoTAudioState state = {
            .structSize = sizeof( state ), .version = OOT_AUDIO_STATE_VERSION
        };
        assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_MAIN, &state ));
        assert( state.playing && !state.finished && state.sequenceId == id );
        assert( state.activeChannels != 0 );
        assert( energy > 0.0 && peak > 0.0f );
        printf( "seq %02X %-20s energy %.3f peak %.3f channels %u voices %u\n",
                id, oot_audio_sequence_name( id ), energy, peak,
                state.activeChannels, state.activeVoices );
    }

    /* FIELD_LOGIC (scene sequence 2) loads its 3..23 child program through
       LDSEQ at runtime.  It must remain audible without allocating from the
       render callback. */
    oot_audio_stop_all( 0 );
    assert( oot_audio_sequence_play( OOT_AUDIO_PLAYER_MAIN, 2, 0 ));
    oot_audio_sequence_set_io( OOT_AUDIO_PLAYER_MAIN, 0, 0 );
    float peak;
    double fieldEnergy = render_seconds( 4, &peak );
    assert( fieldEnergy > 0.0 && peak > 0.0f );

    /* Sequence 1 is driven by the full retail preset IO table, not by the
       ambience id itself.  Exercise all scene-visible presets and require a
       representative continuous Kokiri preset to become audible. */
    assert( !oot_audio_nature_play( OOT_AUDIO_PLAYER_SUB,
                                    OOT_AUDIO_NATURE_NONE, 0 ));
    double kokiriEnergy = 0.0;
    for( uint8_t ambience = 0; ambience < OOT_AUDIO_NATURE_COUNT; ++ambience ) {
        assert( oot_audio_nature_play( OOT_AUDIO_PLAYER_SUB, ambience, 0 ));
        double energy = render_seconds( 2, &peak );
        struct OoTAudioState state = {
            .structSize = sizeof( state ), .version = OOT_AUDIO_STATE_VERSION
        };
        assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SUB, &state ));
        assert( state.playing && !state.finished && state.sequenceId == 1 );
        assert( state.activeChannels != 0 );
        if( ambience == 4 ) kokiriEnergy = energy;
    }
    assert( kokiriEnergy > 0.0 );

    /* Desert Colossus drives two indefinitely-looped samples with repeated
       legato notes.  A fresh allocation on each note used to orphan voices
       until the fixed 96-voice pool saturated; legitimate playback stays at
       two voices (the relaxed bound leaves room for synthesis refinements). */
    assert( oot_audio_nature_play( OOT_AUDIO_PLAYER_SUB, 11, 0 ));
    double legatoEnergy = render_seconds( 20, &peak );
    struct OoTAudioState legatoState = {
        .structSize = sizeof( legatoState ), .version = OOT_AUDIO_STATE_VERSION
    };
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SUB, &legatoState ));
    assert( legatoEnergy > 0.0 && legatoState.activeVoices >= 2 &&
            legatoState.activeVoices <= 8 );

    /* These looped effects expose the old fixed 50 ms note-off immediately.
       Their ROM-authored decay indexes retain the voice for a much longer,
       deterministic tail (PAL synthesis ticks are 200 Hz). */
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SFX, 0 );
    assert( oot_audio_sfx_play( 0x28CA, 0.0f, 1.0f )); /* WATER_BUBBLE, decay 200 */
    (void)render_frame_count( 40000, &peak );          /* 1.25 s */
    struct OoTAudioState bubbleTail = {
        .structSize = sizeof( bubbleTail ), .version = OOT_AUDIO_STATE_VERSION
    };
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &bubbleTail ));
    assert( bubbleTail.activeVoices >= 1 );
    (void)render_frame_count( 17600, &peak );          /* total 1.80 s */
    bubbleTail.structSize = sizeof( bubbleTail );
    bubbleTail.version = OOT_AUDIO_STATE_VERSION;
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &bubbleTail ));
    assert( bubbleTail.activeVoices == 0 );

    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SFX, 0 );
    assert( oot_audio_sfx_play( 0x2889, 0.0f, 1.0f )); /* DIVE_INTO_WATER, seq env + decay 127 */
    (void)render_frame_count( 64000, &peak );          /* 2.00 s */
    struct OoTAudioState diveTail = {
        .structSize = sizeof( diveTail ), .version = OOT_AUDIO_STATE_VERSION
    };
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &diveTail ));
    assert( diveTail.activeVoices >= 1 );
    (void)render_frame_count( 22400, &peak );          /* total 2.70 s */
    diveTail.structSize = sizeof( diveTail );
    diveTail.version = OOT_AUDIO_STATE_VERSION;
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &diveTail ));
    assert( diveTail.activeVoices == 0 );

    /* Channel instrument 84 supplies decay 200 while the layer keeps decay
       index zero (inherit). This catches regressions in channel ADSR loading,
       independently of explicit sequence envelope/release commands. */
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SFX, 0 );
    assert( oot_audio_sfx_play( 0x4817, 0.0f, 1.0f )); /* ATTENTION_ON_OLD */
    (void)render_frame_count( 16000, &peak );          /* 0.50 s */
    struct OoTAudioState inheritedTail = {
        .structSize = sizeof( inheritedTail ), .version = OOT_AUDIO_STATE_VERSION
    };
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &inheritedTail ));
    assert( inheritedTail.activeVoices >= 1 );
    (void)render_frame_count( 19200, &peak );          /* total 1.10 s */
    inheritedTail.structSize = sizeof( inheritedTail );
    inheritedTail.version = OOT_AUDIO_STATE_VERSION;
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &inheritedTail ));
    assert( inheritedTail.activeVoices == 0 );

    /* OBJECT_FALL is a single synthetic sine using C7 mode 0x81. It glides
       from roughly F6 toward F4; the old parser consumed C7 but stayed at the
       low endpoint for the complete note. A fresh engine makes the probe
       independent from any delayed reverb left by the earlier regressions. */
    oot_global_terminate();
    oot_global_init( rom, romSize, NULL );
    assert( oot_audio_sfx_play( 0x28A0, 0.0f, 1.0f ));
    enum { PORTAMENTO_RATE = 32000, PORTAMENTO_WINDOW = 2560,
           PORTAMENTO_CAPTURE = 36160 };
    float *portamentoAudio = malloc( sizeof( *portamentoAudio ) * PORTAMENTO_CAPTURE );
    assert( portamentoAudio != NULL );
    capture_sfx_from_onset( portamentoAudio, PORTAMENTO_CAPTURE );
    struct PitchProbe portamentoEarly =
        pitch_probe( portamentoAudio + 2560, PORTAMENTO_WINDOW, PORTAMENTO_RATE );
    struct PitchProbe portamentoMiddle =
        pitch_probe( portamentoAudio + 19200, PORTAMENTO_WINDOW, PORTAMENTO_RATE );
    struct PitchProbe portamentoLate =
        pitch_probe( portamentoAudio + 33600, PORTAMENTO_WINDOW, PORTAMENTO_RATE );
    printf( "portamento 0x28A0: early %.1f Hz middle %.1f Hz late %.1f Hz\n",
            portamentoEarly.hz, portamentoMiddle.hz, portamentoLate.hz );
    assert( portamentoEarly.rms > 1e-4 && portamentoEarly.madRatio < 0.08 );
    assert( portamentoMiddle.rms > 1e-4 && portamentoMiddle.madRatio < 0.08 );
    assert( portamentoLate.rms > 1e-4 && portamentoLate.madRatio < 0.08 );
    assert( portamentoEarly.hz >= 1250.0 && portamentoEarly.hz <= 1400.0 );
    assert( portamentoMiddle.hz >= 900.0 && portamentoMiddle.hz <= 1050.0 );
    assert( portamentoLate.hz >= 520.0 && portamentoLate.hz <= 660.0 );
    assert( portamentoEarly.hz > portamentoMiddle.hz * 1.20 );
    assert( portamentoMiddle.hz > portamentoLate.hz * 1.35 );
    assert( portamentoEarly.hz > portamentoLate.hz * 2.0 );
    free( portamentoAudio );
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SFX, 0 );

    /* FROG_GROW_UP keeps special mode 3 active across three notes. Each note
       must restart near the same low target and finish progressively higher. */
    oot_global_terminate();
    oot_global_init( rom, romSize, NULL );
    assert( oot_audio_sfx_play( 0x28CC, 0.0f, 1.0f ));
    float *mode3Audio = malloc( sizeof( *mode3Audio ) * 44800u );
    assert( mode3Audio != NULL );
    capture_sfx_from_onset( mode3Audio, 44800u );
    double mode3Early1 = band_peak_hz( mode3Audio + 320, 3520, 32000, 25, 60 );
    double mode3Late1 = band_peak_hz( mode3Audio + 10240, 4000, 32000, 65, 115 );
    double mode3Early2 = band_peak_hz( mode3Audio + 14880, 3040, 32000, 25, 60 );
    double mode3Late2 = band_peak_hz( mode3Audio + 25280, 3840, 32000, 90, 165 );
    double mode3Early3 = band_peak_hz( mode3Audio + 29600, 3040, 32000, 25, 60 );
    double mode3Late3 = band_peak_hz( mode3Audio + 40000, 4320, 32000, 125, 215 );
    printf( "portamento mode3 0x28CC: %.0f->%.0f %.0f->%.0f %.0f->%.0f Hz\n",
            mode3Early1, mode3Late1, mode3Early2, mode3Late2,
            mode3Early3, mode3Late3 );
    assert( mode3Early1 >= 25.0 && mode3Early1 <= 60.0 );
    assert( mode3Early2 >= 25.0 && mode3Early2 <= 60.0 );
    assert( mode3Early3 >= 25.0 && mode3Early3 <= 60.0 );
    assert( mode3Late1 >= 70.0 && mode3Late1 <= 110.0 );
    assert( mode3Late2 >= 105.0 && mode3Late2 <= 150.0 );
    assert( mode3Late3 >= 145.0 && mode3Late3 <= 200.0 );
    assert( mode3Late1 > mode3Early1 + 30.0 );
    assert( mode3Late2 > mode3Early2 + 55.0 );
    assert( mode3Late3 > mode3Early3 + 90.0 );
    assert( mode3Late2 > mode3Late1 + 20.0 );
    assert( mode3Late3 > mode3Late2 + 20.0 );
    free( mode3Audio );

    /* TRAP_OBJ_SLIDE uses persistent mode 4 in the opposite direction. Its
       noisy source is tracked with a spectral-frequency proxy instead of a
       fragile fundamental-frequency estimate. */
    oot_global_terminate();
    oot_global_init( rom, romSize, NULL );
    assert( oot_audio_sfx_play( 0x2858, 0.0f, 1.0f ));
    float mode4Audio[2048];
    capture_sfx_from_onset( mode4Audio, 2048 );
    double mode4Early = spectral_proxy_hz( mode4Audio + 320, 640, 32000 );
    double mode4Late = spectral_proxy_hz( mode4Audio + 1280, 640, 32000 );
    printf( "portamento mode4 0x2858: %.0f->%.0f Hz proxy\n",
            mode4Early, mode4Late );
    assert( mode4Early >= 180.0 && mode4Early <= 450.0 );
    assert( mode4Late >= 600.0 && mode4Late <= 1100.0 );
    assert( mode4Late > mode4Early * 1.8 );

    /* TOWER_BARRIER mode 5 chains its previous note into the next target.
       The first pair rises and the following pair falls. */
    oot_global_terminate();
    oot_global_init( rom, romSize, NULL );
    assert( oot_audio_sfx_play( 0x28EE, 0.0f, 1.0f ));
    float *mode5Audio = malloc( sizeof( *mode5Audio ) * 32000u );
    assert( mode5Audio != NULL );
    capture_sfx_from_onset( mode5Audio, 32000u );
    double mode5UpEarly = band_peak_hz( mode5Audio + 640, 5120, 32000, 25, 60 );
    double mode5UpLate = band_peak_hz( mode5Audio + 9600, 4800, 32000, 25, 60 );
    double mode5DownEarly = band_peak_hz( mode5Audio + 16000, 5760, 32000, 25, 60 );
    double mode5DownLate = band_peak_hz( mode5Audio + 25600, 4160, 32000, 25, 60 );
    printf( "portamento mode5 0x28EE: %.0f->%.0f then %.0f->%.0f Hz\n",
            mode5UpEarly, mode5UpLate, mode5DownEarly, mode5DownLate );
    assert( mode5UpEarly >= 31.0 && mode5UpEarly <= 40.0 );
    assert( mode5UpLate >= 41.0 && mode5UpLate <= 51.0 );
    assert( mode5DownEarly >= 41.0 && mode5DownEarly <= 51.0 );
    assert( mode5DownLate >= 31.0 && mode5DownLate <= 40.0 );
    assert( mode5UpLate >= mode5UpEarly + 6.0 );
    assert( mode5DownEarly >= mode5DownLate + 6.0 );
    free( mode5Audio );
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SFX, 0 );

    assert( oot_audio_sfx_play( 0x4808, 0.0f, 1.0f )); /* menu decide */
    uint32_t maxSfxVoices;
    double sfxEnergy = render_sfx_seconds( 1, &peak, &maxSfxVoices );
    assert( sfxEnergy > 0.0 && peak > 0.0f && maxSfxVoices >= 2 );
    oot_audio_sfx_stop_all();

    assert( oot_audio_sfx_play( 0x4802, 0.0f, 1.0f )); /* synthetic square: correct chime */
    sfxEnergy = render_sfx_seconds( 1, &peak, &maxSfxVoices );
    assert( sfxEnergy > 0.0 && peak > 0.0f && maxSfxVoices >= 1 );
    oot_audio_sfx_stop_all();

    /* A new event during a pending fade-out must revive/reload Sequence 0,
       rather than report success and disappear when the old fade completes. */
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SFX, 1000 );
    assert( oot_audio_sfx_play( 0x4808, 0.0f, 1.0f ));
    sfxEnergy = render_frame_count( 35200, &peak );
    struct OoTAudioState revivedState = {
        .structSize = sizeof( revivedState ), .version = OOT_AUDIO_STATE_VERSION
    };
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &revivedState ));
    assert( sfxEnergy > 0.0 && revivedState.playing && !revivedState.finished );
    oot_audio_sfx_stop_all();
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SFX, 0 );
    revivedState.structSize = sizeof( revivedState );
    revivedState.version = OOT_AUDIO_STATE_VERSION;
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &revivedState ));
    assert( !revivedState.playing && revivedState.finished &&
            revivedState.activeChannels == 0 && revivedState.activeVoices == 0 );

    /* Enemy-bank entry 0 is not one of the legacy direct voice mappings; it
       therefore validates Sequence 0 and the persistent host-pan override.
       Reinitializing between sides makes the bytecode RNG and reverb state
       identical, so the comparison is deterministic. */
    oot_audio_stop_all( 0 );
    oot_global_terminate();
    oot_global_init( rom, romSize, NULL );
    assert( oot_audio_sfx_play( 0x0809, 0.0f, 1.0f ));
    double naturalEnergy = render_frame_count( 3200, &peak );
    struct OoTAudioState naturalState = {
        .structSize = sizeof( naturalState ), .version = OOT_AUDIO_STATE_VERSION
    };
    assert( oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_SFX, &naturalState ));
    assert( naturalEnergy > 0.0 && naturalState.activeVoices >= 1 );

    oot_global_terminate();
    oot_global_init( rom, romSize, NULL );
    assert( oot_audio_sfx_play( 0x3800, -1.0f, 0.9f ));
    double leftHardLeft, leftHardRight;
    render_stereo_energy( 2, &leftHardLeft, &leftHardRight );
    assert( leftHardLeft > 0.0 && leftHardLeft > leftHardRight * 20.0 );

    oot_global_terminate();
    oot_global_init( rom, romSize, NULL );
    assert( oot_audio_sfx_play( 0x3800, 1.0f, 0.9f ));
    double rightHardLeft, rightHardRight;
    render_stereo_energy( 2, &rightHardLeft, &rightHardRight );
    assert( rightHardRight > 0.0 && rightHardRight > rightHardLeft * 20.0 );
    oot_audio_sfx_stop_all();
    oot_audio_stop_all( 0 );

    oot_global_terminate();
    /* A post-terminate render is silent and cannot dereference freed PCM. */
    float silent[64 * 2];
    assert( oot_audio_render_f32( silent, 64, 32000 ) == 64 );
    for( size_t i = 0; i < sizeof( silent ) / sizeof( silent[0] ); ++i ) assert( silent[i] == 0.0f );
    free( rom );
    puts( "audio sequence test: PASS" );
    return 0;
}
