/* voicetest: extract + VADPCM-decode every Link voice clip (NA_SE_VO_LI_*,
   0x6800-0x683F) from a user-supplied OoT ROM through the public liboot API.
   No copyrighted samples are embedded; everything comes from the caller's ROM.

   usage: voicetest ROM [OUTPUT.wav]  (default output: voice0.wav in cwd)
   exit 0 when at least 10 clips decode with sane values and the WAV wrote. */

#include "liboot.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void le16( uint8_t *p, uint16_t v ) { p[0] = v; p[1] = v >> 8; }
static void le32( uint8_t *p, uint32_t v )
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static int write_wav( const char *path, const int16_t *pcm, uint32_t count, uint32_t rate )
{
    if( count > ( UINT32_MAX - 44 ) / 2 ) return 0;
    FILE *f = fopen( path, "wb" );
    if( !f ) return 0;
    uint32_t bytes = count * 2;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E',
                      'f','m','t',' ', 16,0,0,0, 1,0, 1,0 };
    le32( h + 4, 36 + bytes ); le32( h + 24, rate ); le32( h + 28, rate * 2 );
    le16( h + 32, 2 ); le16( h + 34, 16 );
    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a'; le32( h + 40, bytes );
    int ok = fwrite( h, 1, sizeof( h ), f ) == sizeof( h );
    for( uint32_t i = 0; ok && i < count; ++i ) {
        uint8_t s[2]; le16( s, (uint16_t)pcm[i] );
        ok = fwrite( s, 1, 2, f ) == 2;
    }
    return fclose( f ) == 0 && ok;
}

int main( int argc, char **argv )
{
    if( argc < 2 || argc > 3 ) {
        fprintf( stderr, "usage: %s ROM [OUTPUT.wav]\n", argv[0] );
        return 2;
    }
    const char *wavPath = argc == 3 ? argv[2] : "voice0.wav";
    FILE *f = fopen( argv[1], "rb" );
    if( !f ) return 1;
    if( fseek( f, 0, SEEK_END ) != 0 ) { fclose( f ); return 1; }
    long size = ftell( f );
    if( size <= 0 || (uintmax_t)size > SIZE_MAX ||
        fseek( f, 0, SEEK_SET ) != 0 ) { fclose( f ); return 1; }
    uint8_t *rom = malloc( (size_t)size );
    if( !rom || fread( rom, 1, (size_t)size, f ) != (size_t)size ) {
        free( rom ); fclose( f ); return 1;
    }
    fclose( f );
    oot_global_init( rom, (size_t)size, NULL );
    free( rom );

    int available = 0, sane = 0, wrote = 0, errors = 0;
    for( uint16_t i = 0; i < 64; ++i ) {
        const int16_t *pcm;
        uint32_t count, rate;
        uint16_t id = 0x6800 + i;
        if( !oot_get_voice_sample( id, &pcm, &count, &rate ) || !pcm || !count || !rate )
            continue;
        int32_t peak = 0;
        for( uint32_t s = 0; s < count; ++s ) {
            int32_t v = pcm[s] < 0 ? -(int32_t)pcm[s] : pcm[s];
            if( v > peak ) peak = v;
        }
        double duration = (double)count / rate;
        /* Timed DOWN/SNEEZE/SWEAT/RELAX chains intentionally span several
           seconds; the upper bound catches corrupt tables, not long seq_0. */
        int ok = rate >= 8000 && rate <= 32000 && peak > 1000 &&
                 duration >= 0.05 && duration <= 12.0;
        printf( "sfx=0x%04X numSamples=%u sampleRate=%u peak=%d dur=%.3fs%s\n",
                id, count, rate, peak, duration, ok ? "" : " (outlier)" );
        ++available;
        if( ok ) ++sane;
        if( !wrote ) {
            wrote = write_wav( wavPath, pcm, count, rate );
            if( !wrote ) fprintf( stderr, "could not write WAV: %s\n", wavPath );
        }
    }
    {
        const int16_t *pcm;
        uint32_t count, rate;
        if( oot_get_voice_sample( 0x6840, &pcm, &count, &rate )) {
            fprintf( stderr, "retail-silent Navi id 0x6840 decoded unexpectedly\n" );
            ++errors;
        }
        if( !oot_get_voice_sample( 0x6000, &pcm, &count, &rate )) {
            fprintf( stderr, "continuous voice alias 0x6000 did not decode\n" );
            ++errors;
        }
    }
    oot_global_terminate();
    printf( "voices: %d available, %d sane, wav=%s\n", available, sane,
            wrote ? wavPath : "(failed)" );
    return sane == available && available == 64 && wrote && !errors ? 0 : 1;
}
