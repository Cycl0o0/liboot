#include "../src/audio_extract.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t *read_file( const char *path, size_t *size )
{
    FILE *file = fopen( path, "rb" );
    if( !file ) return NULL;
    if( fseek( file, 0, SEEK_END ) != 0 ) { fclose( file ); return NULL; }
    long end = ftell( file );
    if( end <= 0 || fseek( file, 0, SEEK_SET ) != 0 ) { fclose( file ); return NULL; }
    uint8_t *data = malloc( (size_t)end );
    if( !data || fread( data, 1, (size_t)end, file ) != (size_t)end ) {
        free( data );
        fclose( file );
        return NULL;
    }
    fclose( file );
    *size = (size_t)end;
    return data;
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
    liboot_audio_init( rom, romSize );

    /* PAL Rev 1: retail audio table cardinalities. */
    assert( liboot_audio_sequence_count() == 110 );
    assert( liboot_audio_soundfont_count() == 38 );
    assert( liboot_audio_samplebank_count() == 7 );

    LibootAudioSequenceView alias, target;
    assert( liboot_audio_sequence_get( 87, &alias ));
    assert( alias.requestedId == 87 && alias.resolvedId == 40 );
    assert( liboot_audio_sequence_get( 40, &target ));
    assert( alias.data == target.data && alias.size == target.size );

    bool usedFonts[256] = { false };
    for( uint16_t id = 0; id < liboot_audio_sequence_count(); ++id ) {
        LibootAudioSequenceView sequence;
        assert( liboot_audio_sequence_get( id, &sequence ));
        assert( sequence.data != NULL && sequence.size != 0 && sequence.numFonts != 0 );
        for( uint8_t i = 0; i < sequence.numFonts; ++i ) {
            assert( sequence.fontIds[i] < liboot_audio_soundfont_count() );
            usedFonts[sequence.fontIds[i]] = true;
        }
        assert( liboot_audio_prewarm_sequence( id ));
    }

    unsigned fontsWithPcm = 0;
    for( uint16_t fontId = 0; fontId < liboot_audio_soundfont_count(); ++fontId ) {
        LibootAudioSoundFontView font;
        assert( liboot_audio_soundfont_get( fontId, &font ));
        assert( font.fontId == fontId && font.data != NULL && font.size >= 8 );
        bool prewarmed = liboot_audio_prewarm_soundfont( fontId );
        /* PAL font 37 is not referenced by any sequence and contains an
           unused malformed predictor index, which the safe decoder rejects. */
        assert( prewarmed || !usedFonts[fontId] );
        bool found = false;
        LibootAudioSampleView sample;
        for( uint16_t i = 0; i < font.numInstruments && !found; ++i ) {
            found = liboot_audio_instrument_sample_get( fontId, (uint8_t)i, 39, &sample ) ||
                    liboot_audio_instrument_sample_get( fontId, (uint8_t)i, 0, &sample ) ||
                    liboot_audio_instrument_sample_get( fontId, (uint8_t)i, 127, &sample );
        }
        for( uint16_t i = 0; i < font.numDrums && !found; ++i )
            found = liboot_audio_drum_sample_get( fontId, (uint8_t)i, &sample );
        for( uint16_t i = 0; i < font.numSoundEffects && !found; ++i )
            found = liboot_audio_font_sfx_sample_get( fontId, i, &sample );
        if( found ) {
            assert( sample.pcm != NULL && sample.numSamples != 0 && sample.sampleRate != 0 );
            ++fontsWithPcm;
        }
        assert( !usedFonts[fontId] || found );
    }

    LibootAudioSampleView ocarina;
    assert( liboot_audio_instrument_sample_get( 0, 52, 41, &ocarina ));
    assert( ocarina.loopCount != 0 && ocarina.loopStart < ocarina.numSamples );

    /* PAL font 3/drum 33 has a distinctive metadata tuple; this pins the raw
       Drum offsets (+0 decay, +1 pan, +0xC envelope) used by the sequencer. */
    LibootAudioSampleView drum;
    assert( liboot_audio_drum_sample_get( 3, 33, &drum ));
    assert( drum.hasPan && drum.pan == 74 );
    assert( drum.adsr.envelope != NULL && drum.adsr.size >= 4 &&
            drum.adsr.decayIndex == 242 );
    assert( drum.adsr.envelope[0] == 0 && drum.adsr.envelope[1] == 2 );

    LibootAudioSequenceView invalidSequence = { (const uint8_t *)1, 1, NULL, 1, 1, 1, 1 };
    assert( !liboot_audio_sequence_get( 110, &invalidSequence ));
    assert( invalidSequence.data == NULL && invalidSequence.size == 0 );

    printf( "audio catalogue ok: 110 sequences, 38 fonts, %u fonts with PCM\n", fontsWithPcm );
    liboot_audio_terminate();
    assert( liboot_audio_sequence_count() == 0 );
    free( rom );
    return 0;
}
