#include "liboot.h"
#include "audio_extract.h"
#include "audio_math.h"
#include "rom_util.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AUDIOBANK_DMA_INDEX  3
#define AUDIOSEQ_DMA_INDEX   4
#define AUDIOTABLE_DMA_INDEX 5
#define AUDIO_TABLE_MAX      256u
#define CODE_SCAN_DMA_MAX    64u
#define CODE_SCAN_MIN_SIZE   0x80000u
#define CODE_SCAN_MAX_SIZE   0x300000u

typedef struct {
    uint32_t romAddr, size;
    uint8_t medium, cachePolicy;
    uint16_t shortData1, shortData2, shortData3;
} RomAudioEntry;

typedef struct {
    size_t fontOff, sequenceFontOff, sequenceFontSize, sequenceOff, sampleOff;
    uint16_t numFonts, numSequences, numBanks;
} AudioTableLayout;

typedef struct {
    int16_t *pcm;
    uint32_t numSamples, sampleRate;
    uint32_t loopStart, loopCount;
    bool attempted;
} DecodedSample;

/* Decoded instrument sample, cached per soundfont + sample-header offset (the
   same sample serves many notes; only the playback rate differs per note). */
typedef struct {
    uint16_t fontId;
    uint32_t sampleOff;
    DecodedSample sample;
} CachedSample;

/* liboot v0.4: 64 Link grunt ids (0x6800-0x683F) + 16 Navi/fairy voice ids
   (0x6840-0x684F) */
#define LINK_VOICE_COUNT 80u

static struct {
    uint8_t *fontData, *sequenceData, *sampleData, *sequenceFontData;
    uint8_t *prewarmedFonts;
    size_t fontDataSize, sequenceDataSize, sampleDataSize, sequenceFontDataSize;
    uint32_t font0Offset, font0Size, sfxOffset;
    uint16_t numSfx, numSoundFonts, numSequences, numSampleBanks, numInstruments;
    uint8_t sampleBankId1, sampleBankId2;
    RomAudioEntry *soundFonts, *sequenceEntries, *sampleBanks;
    DecodedSample *decoded;
    DecodedSample voiceSequences[LINK_VOICE_COUNT];
    uint8_t lastPick[LINK_VOICE_COUNT];
    CachedSample *sampleCache;
    uint32_t sampleCacheCount, sampleCacheCap;
    bool isPal;
    bool ready;
} s_audio;

static bool prewarm_font( uint16_t fontId );

static uint16_t be16( const uint8_t *p )
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static int16_t bes16( const uint8_t *p )
{
    return (int16_t)be16( p );
}

static uint32_t be32( const uint8_t *p )
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static float bef32( const uint8_t *p )
{
    uint32_t bits = be32( p );
    float value;
    memcpy( &value, &bits, sizeof( value ));
    return value;
}

static size_t align16( size_t value )
{
    return ( value + 15u ) & ~(size_t)15u;
}

static bool span_ok( size_t size, uint32_t offset, size_t length )
{
    return offset <= size && length <= size - offset;
}

static RomAudioEntry audio_entry( const uint8_t *p )
{
    RomAudioEntry e;
    e.romAddr = be32( p );
    e.size = be32( p + 4 );
    e.medium = p[8];
    e.cachePolicy = p[9];
    e.shortData1 = be16( p + 10 );
    e.shortData2 = be16( p + 12 );
    e.shortData3 = be16( p + 14 );
    return e;
}

static void audio_clear( void )
{
    /* decoded[] aliases the owning sampleCache entries. */
    for( uint32_t i = 0; i < LINK_VOICE_COUNT; ++i ) free( s_audio.voiceSequences[i].pcm );
    for( uint32_t i = 0; i < s_audio.sampleCacheCount; ++i ) free( s_audio.sampleCache[i].sample.pcm );
    free( s_audio.sampleCache );
    free( s_audio.decoded );
    free( s_audio.sampleBanks );
    free( s_audio.sequenceEntries );
    free( s_audio.soundFonts );
    free( s_audio.prewarmedFonts );
    free( s_audio.sequenceFontData );
    free( s_audio.sampleData );
    free( s_audio.sequenceData );
    free( s_audio.fontData );
    memset( &s_audio, 0, sizeof( s_audio ));
}

void liboot_audio_terminate( void )
{
    audio_clear();
}

static bool table_header_ok( const uint8_t *code, size_t codeSize, size_t off, uint16_t *count )
{
    if( off > codeSize || codeSize - off < 0x20 ) return false;
    uint16_t n = be16( code + off );
    if( n == 0 || n > AUDIO_TABLE_MAX || (size_t)n * 16 > codeSize - off - 16 ||
        be16( code + off + 2 ) != 0 || be32( code + off + 4 ) != 0 ) return false;
    for( size_t i = 8; i < 16; ++i ) if( code[off + i] != 0 ) return false;
    *count = n;
    return true;
}

static bool is_soundfont_table( const uint8_t *code, size_t codeSize, size_t off,
                                size_t bankSize, uint16_t *count )
{
    uint16_t n;
    if( !table_header_ok( code, codeSize, off, &n ) || bankSize > UINT32_MAX ||
        bankSize > SIZE_MAX - 15u ) return false;
    size_t cursor = 0;
    for( uint16_t i = 0; i < n; ++i ) {
        RomAudioEntry e = audio_entry( code + off + 16 + (size_t)i * 16 );
        if( e.medium != 2 || e.size == 0 || e.romAddr != cursor ||
            cursor > bankSize || e.size > bankSize - cursor ) return false;
        cursor += align16( e.size );
    }
    /* Retail Audiobank files can contain a small linker-alignment tail. */
    if( cursor > bankSize || bankSize - cursor > 0x100 ) return false;
    *count = n;
    return true;
}

static bool is_samplebank_table( const uint8_t *code, size_t codeSize, size_t off,
                                 size_t tableSize, uint16_t *count )
{
    uint16_t n;
    if( !table_header_ok( code, codeSize, off, &n ) || tableSize > UINT32_MAX ||
        tableSize > SIZE_MAX - 15u ) return false;
    size_t cursor = 0;
    bool physical = false;
    for( uint16_t i = 0; i < n; ++i ) {
        RomAudioEntry e = audio_entry( code + off + 16 + (size_t)i * 16 );
        if( e.medium != 2 ) return false;
        if( e.size == 0 ) {
            if( e.romAddr >= n ) return false;
        } else {
            if( e.romAddr != cursor || cursor > tableSize || e.size > tableSize - cursor ) return false;
            cursor += align16( e.size );
            physical = true;
        }
    }
    if( !physical || cursor != tableSize ) return false;
    *count = n;
    return true;
}

static bool is_sequence_table( const uint8_t *code, size_t codeSize, size_t off,
                               size_t sequenceSize, uint16_t *count )
{
    uint16_t n;
    if( !table_header_ok( code, codeSize, off, &n ) || sequenceSize > UINT32_MAX ||
        sequenceSize > SIZE_MAX - 15u ) return false;
    size_t cursor = 0;
    bool physical = false;
    for( uint16_t i = 0; i < n; ++i ) {
        RomAudioEntry e = audio_entry( code + off + 16 + (size_t)i * 16 );
        if( e.medium != 2 ) return false;
        if( e.size == 0 ) {
            /* Sequence aliases store another sequence id in romAddr. */
            if( e.romAddr >= n ) return false;
        } else {
            if( e.romAddr != cursor || cursor > sequenceSize || e.size > sequenceSize - cursor )
                return false;
            cursor += align16( e.size );
            physical = true;
        }
    }
    /* Like Audiobank, Audioseq can retain a small final alignment tail. */
    if( !physical || cursor > sequenceSize || sequenceSize - cursor > 0x100 ) return false;
    *count = n;
    return true;
}

static bool sequence_font_table_ok( const uint8_t *code, size_t start, size_t size,
                                    uint16_t numSequences, uint16_t numFonts )
{
    if( size < (size_t)numSequences * 2 ) return false;
    for( uint16_t i = 0; i < numSequences; ++i ) {
        uint16_t offset = be16( code + start + (size_t)i * 2 );
        if( offset < (size_t)numSequences * 2 || offset >= size ) return false;
        uint8_t count = code[start + offset];
        if( count == 0 || (size_t)count > size - offset - 1 ) return false;
        for( uint8_t j = 0; j < count; ++j )
            if( code[start + offset + 1 + j] >= numFonts ) return false;
    }
    return true;
}

static bool find_tables( const uint8_t *code, size_t codeSize, AudioTableLayout *layout )
{
    memset( layout, 0, sizeof( *layout ));

    /* Audiobank and Audioseq tables are adjacent, separated by the compact
       sequence-id -> soundfont-id table.  Validating all three together
       avoids accepting unrelated AudioTable-shaped data in the code file. */
    for( size_t fontOff = 0; fontOff + 0x20 <= codeSize; fontOff += 4 ) {
        uint16_t numFonts;
        if( !is_soundfont_table( code, codeSize, fontOff, s_audio.fontDataSize, &numFonts ))
            continue;
        size_t fontEnd = fontOff + 16 + (size_t)numFonts * 16;
        for( size_t sequenceOff = fontEnd; sequenceOff + 0x20 <= codeSize; sequenceOff += 4 ) {
            uint16_t numSequences;
            if( !is_sequence_table( code, codeSize, sequenceOff, s_audio.sequenceDataSize,
                                    &numSequences )) continue;
            size_t sequenceFontSize = sequenceOff - fontEnd;
            if( sequenceFontSize > UINT16_MAX + 1u ||
                !sequence_font_table_ok( code, fontEnd, sequenceFontSize,
                                         numSequences, numFonts )) continue;
            layout->fontOff = fontOff;
            layout->sequenceFontOff = fontEnd;
            layout->sequenceFontSize = sequenceFontSize;
            layout->sequenceOff = sequenceOff;
            layout->numFonts = numFonts;
            layout->numSequences = numSequences;
            break;
        }
        if( layout->numFonts ) break;
    }

    if( !layout->numFonts ) return false;
    for( size_t off = 0; off + 0x20 <= codeSize; off += 4 ) {
        uint16_t numBanks;
        if( is_samplebank_table( code, codeSize, off, s_audio.sampleDataSize, &numBanks )) {
            layout->sampleOff = off;
            layout->numBanks = numBanks;
            return true;
        }
    }
    return false;
}

static bool safe_dma_get( const uint8_t *rom, size_t romSize, const uint8_t *dmadata,
                          uint32_t index, LibootDmaEntry *entry )
{
    return dma_get( rom, romSize, dmadata, index, entry ) &&
           entry->vromEnd >= entry->vromStart;
}

static uint8_t *find_code( const uint8_t *rom, size_t romSize, const uint8_t *dmadata,
                           AudioTableLayout *layout )
{
    LibootDmaEntry e;
    for( uint32_t i = 3; i < CODE_SCAN_DMA_MAX && safe_dma_get( rom, romSize, dmadata, i, &e ); ++i ) {
        size_t size = e.vromEnd - e.vromStart;
        /* PAL 1.1's code is ~1 MiB; other layouts can be over 2 MiB. */
        if( size < CODE_SCAN_MIN_SIZE || size > CODE_SCAN_MAX_SIZE ) continue;
        uint8_t *code = rom_read_file( rom, romSize, i, &size );
        if( !code ) continue;
        if( find_tables( code, size, layout )) return code;
        free( code );
    }
    return NULL;
}

void liboot_audio_init( const uint8_t *rom, size_t romSize )
{
    audio_clear();
    if( !rom || romSize < 0x1060 ) return;
    switch( rom[0x3E] ) {
    case 'D': case 'F': case 'I': case 'P': case 'S':
    case 'U': case 'X': case 'Y':
        s_audio.isPal = true;
        break;
    default:
        s_audio.isPal = false;
        break;
    }
    const uint8_t *dmadata = rom_find_dmadata( rom, romSize );
    LibootDmaEntry unused;
    if( !dmadata || !safe_dma_get( rom, romSize, dmadata, AUDIOBANK_DMA_INDEX, &unused ) ||
        !safe_dma_get( rom, romSize, dmadata, AUDIOSEQ_DMA_INDEX, &unused ) ||
        !safe_dma_get( rom, romSize, dmadata, AUDIOTABLE_DMA_INDEX, &unused )) return;

    s_audio.fontData = rom_read_file( rom, romSize, AUDIOBANK_DMA_INDEX, &s_audio.fontDataSize );
    s_audio.sequenceData = rom_read_file( rom, romSize, AUDIOSEQ_DMA_INDEX,
                                          &s_audio.sequenceDataSize );
    s_audio.sampleData = rom_read_file( rom, romSize, AUDIOTABLE_DMA_INDEX, &s_audio.sampleDataSize );
    if( !s_audio.fontData || !s_audio.sequenceData || !s_audio.sampleData ) {
        audio_clear();
        return;
    }

    AudioTableLayout layout;
    uint8_t *code = find_code( rom, romSize, dmadata, &layout );
    if( !code ) { audio_clear(); return; }

    s_audio.soundFonts = calloc( layout.numFonts, sizeof( *s_audio.soundFonts ));
    s_audio.sequenceEntries = calloc( layout.numSequences, sizeof( *s_audio.sequenceEntries ));
    s_audio.sampleBanks = calloc( layout.numBanks, sizeof( *s_audio.sampleBanks ));
    s_audio.prewarmedFonts = calloc( layout.numFonts, sizeof( *s_audio.prewarmedFonts ));
    s_audio.sequenceFontData = malloc( layout.sequenceFontSize );
    if( !s_audio.soundFonts || !s_audio.sequenceEntries || !s_audio.sampleBanks ||
        !s_audio.prewarmedFonts || !s_audio.sequenceFontData ) {
        free( code );
        audio_clear();
        return;
    }
    s_audio.numSoundFonts = layout.numFonts;
    s_audio.numSequences = layout.numSequences;
    s_audio.numSampleBanks = layout.numBanks;
    s_audio.sequenceFontDataSize = layout.sequenceFontSize;
    memcpy( s_audio.sequenceFontData, code + layout.sequenceFontOff, layout.sequenceFontSize );
    for( uint16_t i = 0; i < layout.numFonts; ++i )
        s_audio.soundFonts[i] = audio_entry( code + layout.fontOff + 16 + (size_t)i * 16 );
    for( uint16_t i = 0; i < layout.numSequences; ++i )
        s_audio.sequenceEntries[i] = audio_entry( code + layout.sequenceOff + 16 + (size_t)i * 16 );
    for( uint16_t i = 0; i < layout.numBanks; ++i )
        s_audio.sampleBanks[i] = audio_entry( code + layout.sampleOff + 16 + (size_t)i * 16 );
    free( code );

    RomAudioEntry font0 = s_audio.soundFonts[0];
    if( font0.size < 8 || !span_ok( s_audio.fontDataSize, font0.romAddr, font0.size )) {
        audio_clear();
        return;
    }
    s_audio.font0Offset = font0.romAddr;
    s_audio.font0Size = font0.size;
    s_audio.sampleBankId1 = (uint8_t)( font0.shortData1 >> 8 );
    s_audio.sampleBankId2 = (uint8_t)font0.shortData1;
    s_audio.numSfx = font0.shortData3;
    /* Instrument pointer array lives at font+8 (after the drum/sfx list offsets). */
    s_audio.numInstruments = (uint16_t)( font0.shortData2 >> 8 );
    if( !span_ok( s_audio.font0Size, 8, (size_t)s_audio.numInstruments * 4 ))
        s_audio.numInstruments = 0;
    s_audio.sfxOffset = be32( s_audio.fontData + s_audio.font0Offset + 4 );
    if( s_audio.numSfx == 0 || s_audio.sfxOffset == 0 ||
        !span_ok( s_audio.font0Size, s_audio.sfxOffset, (size_t)s_audio.numSfx * 8 )) {
        audio_clear();
        return;
    }

    s_audio.decoded = calloc( s_audio.numSfx, sizeof( *s_audio.decoded ));
    if( !s_audio.decoded ) { audio_clear(); return; }
    memset( s_audio.lastPick, 0xFF, sizeof( s_audio.lastPick ));
    s_audio.ready = true;

    /* Preserve the existing public SFX and ocarina getters: their font is
       available immediately, while music renderers explicitly prewarm the
       fonts attached to the sequence they are about to play. */
    (void)prewarm_font( 0 );
}

static bool resolve_bank( uint8_t id, RomAudioEntry *out )
{
    if( id >= s_audio.numSampleBanks ) return false;
    for( uint16_t depth = 0; depth < s_audio.numSampleBanks; ++depth ) {
        RomAudioEntry e = s_audio.sampleBanks[id];
        if( e.size != 0 ) {
            if( !span_ok( s_audio.sampleDataSize, e.romAddr, e.size )) return false;
            *out = e;
            return true;
        }
        if( e.romAddr >= s_audio.numSampleBanks ) return false;
        id = (uint8_t)e.romAddr;
    }
    return false;
}

static bool soundfont_entry_get( uint16_t fontId, RomAudioEntry *out, const uint8_t **font )
{
    if( !s_audio.ready || fontId >= s_audio.numSoundFonts ) return false;
    RomAudioEntry e = s_audio.soundFonts[fontId];
    if( e.size < 8 || !span_ok( s_audio.fontDataSize, e.romAddr, e.size )) return false;
    if( out ) *out = e;
    if( font ) *font = s_audio.fontData + e.romAddr;
    return true;
}

static bool sequence_resolve( uint16_t sequenceId, uint16_t *resolved )
{
    if( !s_audio.ready || sequenceId >= s_audio.numSequences ) return false;
    uint16_t id = sequenceId;
    for( uint16_t depth = 0; depth < s_audio.numSequences; ++depth ) {
        RomAudioEntry e = s_audio.sequenceEntries[id];
        if( e.size != 0 ) {
            if( !span_ok( s_audio.sequenceDataSize, e.romAddr, e.size )) return false;
            *resolved = id;
            return true;
        }
        if( e.romAddr >= s_audio.numSequences ) return false;
        id = (uint16_t)e.romAddr;
    }
    return false;
}

uint16_t liboot_audio_sequence_count( void )
{
    return s_audio.ready ? s_audio.numSequences : 0;
}

uint16_t liboot_audio_soundfont_count( void )
{
    return s_audio.ready ? s_audio.numSoundFonts : 0;
}

uint16_t liboot_audio_samplebank_count( void )
{
    return s_audio.ready ? s_audio.numSampleBanks : 0;
}

bool liboot_audio_sequence_get( uint16_t sequenceId, LibootAudioSequenceView *out )
{
    if( out ) memset( out, 0, sizeof( *out ));
    if( !out || sequenceId >= s_audio.numSequences ) return false;

    uint16_t resolved;
    if( !sequence_resolve( sequenceId, &resolved )) return false;
    RomAudioEntry e = s_audio.sequenceEntries[resolved];
    size_t offsetTableSize = (size_t)s_audio.numSequences * 2;
    if( s_audio.sequenceFontDataSize < offsetTableSize ) return false;
    uint16_t fontListOff = be16( s_audio.sequenceFontData + (size_t)sequenceId * 2 );
    if( fontListOff < offsetTableSize || fontListOff >= s_audio.sequenceFontDataSize ) return false;
    uint8_t numFonts = s_audio.sequenceFontData[fontListOff];
    if( numFonts == 0 || (size_t)numFonts > s_audio.sequenceFontDataSize - fontListOff - 1 )
        return false;
    for( uint8_t i = 0; i < numFonts; ++i )
        if( s_audio.sequenceFontData[fontListOff + 1 + i] >= s_audio.numSoundFonts ) return false;

    out->data = s_audio.sequenceData + e.romAddr;
    out->size = e.size;
    out->fontIds = s_audio.sequenceFontData + fontListOff + 1;
    out->requestedId = sequenceId;
    out->resolvedId = resolved;
    out->numFonts = numFonts;
    out->cachePolicy = s_audio.sequenceEntries[sequenceId].cachePolicy;
    return true;
}

bool liboot_audio_soundfont_get( uint16_t fontId, LibootAudioSoundFontView *out )
{
    if( out ) memset( out, 0, sizeof( *out ));
    RomAudioEntry e;
    const uint8_t *font;
    if( !out || !soundfont_entry_get( fontId, &e, &font )) return false;

    out->data = font;
    out->size = e.size;
    out->fontId = fontId;
    out->numSoundEffects = e.shortData3;
    out->sampleBankId1 = (uint8_t)( e.shortData1 >> 8 );
    out->sampleBankId2 = (uint8_t)e.shortData1;
    out->numInstruments = (uint8_t)( e.shortData2 >> 8 );
    out->numDrums = (uint8_t)e.shortData2;
    out->cachePolicy = e.cachePolicy;
    return true;
}

static int16_t clamp16( int32_t value )
{
    if( value < -32768 ) return -32768;
    if( value > 32767 ) return 32767;
    return (int16_t)value;
}

static int32_t inner_product( const int32_t *a, const int32_t *b, int count )
{
    int64_t sum = 0;
    for( int i = 0; i < count; ++i ) sum += (int64_t)a[i] * b[i];
    int64_t value = sum / 2048;
    if( sum < 0 && sum % 2048 ) --value;
    if( value < INT32_MIN ) return INT32_MIN;
    if( value > INT32_MAX ) return INT32_MAX;
    return (int32_t)value;
}

static bool vadpcm_decode( const uint8_t *input, uint32_t inputSize, const uint8_t *book,
                           size_t bookAvailable, int frameSize, uint32_t numSamples, int16_t **out )
{
    if( bookAvailable < 8 || numSamples == 0 || ( frameSize != 5 && frameSize != 9 )) return false;
    uint32_t order = be32( book ), predictors = be32( book + 4 );
    size_t coefficientCount = (size_t)8 * order * predictors;
    if( order == 0 || order > 8 || predictors == 0 || predictors > 16 ||
        coefficientCount > ( bookAvailable - 8 ) / 2 ) return false;
    /* Avoid wrapping numSamples + 15 for a synthetic UINT32_MAX request. */
    uint32_t frames = numSamples / 16 + ( numSamples % 16 != 0 );
    if( frames > inputSize / (uint32_t)frameSize ) return false;
#if SIZE_MAX <= UINT32_MAX
    if((size_t)numSamples > SIZE_MAX / sizeof( int16_t )) return false;
#endif

    int columns = (int)order + 8;
    int32_t *table = calloc( (size_t)predictors * 8 * (size_t)columns, sizeof( *table ));
    int16_t *pcm = malloc( (size_t)numSamples * sizeof( *pcm ));
    if( !table || !pcm ) { free( table ); free( pcm ); return false; }
#define C(pred,row,col) table[((size_t)(pred) * 8 + (row)) * (size_t)columns + (col)]
    const uint8_t *src = book + 8;
    for( uint32_t pred = 0; pred < predictors; ++pred ) {
        for( uint32_t col = 0; col < order; ++col )
            for( int row = 0; row < 8; ++row, src += 2 ) C( pred, row, col ) = bes16( src );
        for( int row = 1; row < 8; ++row ) C( pred, row, order ) = C( pred, row - 1, order - 1 );
        C( pred, 0, order ) = 2048;
        for( int shift = 1; shift < 8; ++shift )
            for( int row = shift; row < 8; ++row )
                C( pred, row, shift + order ) = C( pred, row - shift, order );
    }

    int32_t state[16] = { 0 };
    uint32_t pos = 0;
    for( uint32_t f = 0; f < frames; ++f ) {
        const uint8_t *frame = input + (size_t)f * frameSize;
        uint32_t shift = frame[0] >> 4, predictor = frame[0] & 15;
        if( predictor >= predictors ) { free( table ); free( pcm ); return false; }
        int32_t residual[16];
        for( int i = 0; i < 16; ++i ) {
            int value;
            if( frameSize == 9 ) {
                uint8_t packed = frame[1 + i / 2];
                value = ( i & 1 ) ? packed & 15 : packed >> 4;
                if( value >= 8 ) value -= 16;
            } else {
                uint8_t packed = frame[1 + i / 4];
                value = packed >> ( 6 - 2 * ( i & 3 )) & 3;
                if( value >= 2 ) value -= 4;
            }
            residual[i] = value * ( 1 << shift );
        }
        for( int half = 0; half < 2; ++half ) {
            int32_t vector[16] = { 0 };
            for( uint32_t i = 0; i < order; ++i )
                vector[i] = state[( 2 - half ) * 8 - (int)order + (int)i];
            for( int row = 0; row < 8; ++row ) {
                int index = half * 8 + row;
                vector[order + row] = residual[index];
                int32_t prediction = inner_product( &C( predictor, row, 0 ), vector,
                                                    (int)order + row );
                state[index] = liboot_audio_add_saturate_i32( prediction, residual[index] );
            }
        }
        for( int i = 0; i < 16 && pos < numSamples; ++i ) pcm[pos++] = clamp16( state[i] );
    }
#undef C
    free( table );
    *out = pcm;
    return true;
}

/* Decode the VADPCM sample whose header sits at soundfont+sampleOff.  Looped
   samples are cut at their loop end; the renderer repeats that PCM span. */
static bool decode_sample_at( uint16_t fontId, uint32_t sampleOff, int16_t **outPcm,
                              uint32_t *outNumSamples, uint32_t *outLoopStart,
                              uint32_t *outLoopCount )
{
    RomAudioEntry fontEntry;
    const uint8_t *font;
    if( !soundfont_entry_get( fontId, &fontEntry, &font ) || sampleOff == 0 ||
        !span_ok( fontEntry.size, sampleOff, 16 )) return false;

    const uint8_t *sample = font + sampleOff;
    uint32_t bits = be32( sample );
    uint32_t codec = bits >> 28, medium = bits >> 26 & 3, size = bits & 0xFFFFFF;
    uint32_t address = be32( sample + 4 ), loopOff = be32( sample + 8 ), bookOff = be32( sample + 12 );
    int frameSize = codec == 0 ? 9 : codec == 3 ? 5 : 0;
    if( frameSize == 0 || size == 0 || medium > 1 ||
        !span_ok( fontEntry.size, loopOff, 16 ) || !span_ok( fontEntry.size, bookOff, 8 ))
        return false;

    RomAudioEntry bank;
    uint8_t bankId = medium == 0 ? (uint8_t)( fontEntry.shortData1 >> 8 )
                                 : (uint8_t)fontEntry.shortData1;
    if( !resolve_bank( bankId, &bank ) || !span_ok( bank.size, address, size )) return false;
    uint32_t frames = size / (uint32_t)frameSize;
    if( frames == 0 || frames > UINT32_MAX / 16 ) return false;
    uint32_t capacity = frames * 16;
    uint32_t loopStart = be32( font + loopOff );
    uint32_t loopEnd = be32( font + loopOff + 4 );
    uint32_t loopCount = be32( font + loopOff + 8 );
    uint32_t samples = loopEnd != 0 && loopEnd <= capacity ? loopEnd : capacity;
    if( !vadpcm_decode( s_audio.sampleData + bank.romAddr + address, size, font + bookOff,
                        fontEntry.size - bookOff, frameSize, samples, outPcm )) return false;
    *outNumSamples = samples;
    if( outLoopStart ) *outLoopStart = loopStart < samples ? loopStart : 0;
    if( outLoopCount ) *outLoopCount = loopCount;
    return true;
}

static DecodedSample *sample_cache_find( uint16_t fontId, uint32_t sampleOff )
{
    for( uint32_t i = 0; i < s_audio.sampleCacheCount; ++i ) {
        CachedSample *entry = &s_audio.sampleCache[i];
        if( entry->fontId == fontId && entry->sampleOff == sampleOff ) return &entry->sample;
    }
    return NULL;
}

/* Allocation/decoding entry point used only by init/prewarm, never by a PCM getter. */
static DecodedSample *sample_cache_ensure( uint16_t fontId, uint32_t sampleOff )
{
    DecodedSample *found = sample_cache_find( fontId, sampleOff );
    if( found ) return found;
    if( s_audio.sampleCacheCount == s_audio.sampleCacheCap ) {
        if( s_audio.sampleCacheCap > UINT32_MAX / 2 ) return NULL;
        uint32_t cap = s_audio.sampleCacheCap ? s_audio.sampleCacheCap * 2 : 32;
#if SIZE_MAX <= UINT32_MAX
        if((size_t)cap > SIZE_MAX / sizeof( *s_audio.sampleCache )) return NULL;
#endif
        CachedSample *grown = realloc( s_audio.sampleCache, (size_t)cap * sizeof( *grown ));
        if( !grown ) return NULL;
        s_audio.sampleCache = grown;
        s_audio.sampleCacheCap = cap;
    }
    CachedSample *entry = &s_audio.sampleCache[s_audio.sampleCacheCount++];
    memset( entry, 0, sizeof( *entry ));
    entry->fontId = fontId;
    entry->sampleOff = sampleOff;
    entry->sample.attempted = true;
    (void)decode_sample_at( fontId, sampleOff, &entry->sample.pcm, &entry->sample.numSamples,
                            &entry->sample.loopStart, &entry->sample.loopCount );
    return &entry->sample;
}

static bool tuned_sample_read( uint16_t fontId, uint32_t tunedOff, uint32_t *sampleOff,
                               float *tuning )
{
    RomAudioEntry fontEntry;
    const uint8_t *font;
    if( !soundfont_entry_get( fontId, &fontEntry, &font ) ||
        !span_ok( fontEntry.size, tunedOff, 8 )) return false;
    *sampleOff = be32( font + tunedOff );
    *tuning = bef32( font + tunedOff + 4 );
    return *sampleOff != 0 && isfinite( *tuning ) && *tuning > 0.0f && *tuning <= 32.0f;
}

static uint32_t tuned_sample_rate( float tuning, uint8_t semitone, bool transpose )
{
    double rate = 32000.0 * tuning;
    if( transpose ) rate *= exp2(( (double)semitone - 39.0 ) / 12.0 );
    if( rate < 1.0 ) return 1;
    if( rate >= UINT32_MAX ) return UINT32_MAX;
    return (uint32_t)llround( rate );
}

static bool sample_view_fill( uint16_t fontId, uint32_t sampleOff, float tuning,
                              uint8_t semitone, bool transpose, LibootAudioSampleView *out )
{
    DecodedSample *sample = sample_cache_find( fontId, sampleOff );
    if( !sample || !sample->pcm ) return false;
    out->pcm = sample->pcm;
    out->numSamples = sample->numSamples;
    out->sampleRate = tuned_sample_rate( tuning, semitone, transpose );
    out->loopStart = sample->loopStart;
    out->loopCount = sample->loopCount;
    return true;
}

static bool adsr_view_fill( const RomAudioEntry *fontEntry, const uint8_t *font,
                            uint32_t ownerOff, uint32_t envelopeField,
                            uint32_t decayField, LibootAudioAdsrView *out )
{
    if( out ) memset( out, 0, sizeof( *out ));
    if( !out || !fontEntry || !font ||
        !span_ok( fontEntry->size, ownerOff, envelopeField + 4u )) return false;
    uint32_t envelopeOff = be32( font + ownerOff + envelopeField );
    if( envelopeOff == 0 || !span_ok( fontEntry->size, envelopeOff, 4 )) return false;
    out->envelope = font + envelopeOff;
    out->size = fontEntry->size - envelopeOff;
    out->decayIndex = font[ownerOff + decayField];
    return true;
}

bool liboot_audio_instrument_adsr_get( uint16_t fontId, uint8_t instrumentId,
                                       LibootAudioAdsrView *out )
{
    if( out ) memset( out, 0, sizeof( *out ));
    RomAudioEntry fontEntry;
    const uint8_t *font;
    if( !out || !soundfont_entry_get( fontId, &fontEntry, &font )) return false;
    uint8_t numInstruments = (uint8_t)( fontEntry.shortData2 >> 8 );
    if( instrumentId >= numInstruments ||
        !span_ok( fontEntry.size, 8, (size_t)numInstruments * 4 )) return false;
    uint32_t instrumentOff = be32( font + 8 + (size_t)instrumentId * 4 );
    if( instrumentOff == 0 || !span_ok( fontEntry.size, instrumentOff, 32 )) return false;
    return adsr_view_fill( &fontEntry, font, instrumentOff, 4, 3, out );
}

bool liboot_audio_instrument_sample_get_for_pitch( uint16_t fontId,
                                                   uint8_t instrumentId,
                                                   uint8_t sampleSemitone,
                                                   uint8_t pitchSemitone,
                                                   LibootAudioSampleView *out )
{
    if( out ) memset( out, 0, sizeof( *out ));
    RomAudioEntry fontEntry;
    const uint8_t *font;
    if( !out || !soundfont_entry_get( fontId, &fontEntry, &font )) return false;
    uint8_t numInstruments = (uint8_t)( fontEntry.shortData2 >> 8 );
    if( instrumentId >= numInstruments ||
        !span_ok( fontEntry.size, 8, (size_t)numInstruments * 4 )) return false;
    uint32_t instrumentOff = be32( font + 8 + (size_t)instrumentId * 4 );
    if( instrumentOff == 0 || !span_ok( fontEntry.size, instrumentOff, 32 )) return false;
    uint8_t rangeLo = font[instrumentOff + 1], rangeHi = font[instrumentOff + 2];
    uint32_t tunedOff = instrumentOff +
        ( sampleSemitone < rangeLo ? 8u : sampleSemitone <= rangeHi ? 16u : 24u );
    uint32_t sampleOff;
    float tuning;
    if( !tuned_sample_read( fontId, tunedOff, &sampleOff, &tuning )) return false;
    if( !sample_view_fill( fontId, sampleOff, tuning, pitchSemitone, true, out )) return false;
    out->tunedSample = font + tunedOff;
    (void)adsr_view_fill( &fontEntry, font, instrumentOff, 4, 3, &out->adsr );
    return true;
}

bool liboot_audio_instrument_sample_get( uint16_t fontId, uint8_t instrumentId,
                                         uint8_t semitone, LibootAudioSampleView *out )
{
    return liboot_audio_instrument_sample_get_for_pitch( fontId, instrumentId,
                                                         semitone, semitone, out );
}

bool liboot_audio_drum_sample_get( uint16_t fontId, uint8_t drumId,
                                   LibootAudioSampleView *out )
{
    if( out ) memset( out, 0, sizeof( *out ));
    RomAudioEntry fontEntry;
    const uint8_t *font;
    if( !out || !soundfont_entry_get( fontId, &fontEntry, &font )) return false;
    uint8_t numDrums = (uint8_t)fontEntry.shortData2;
    uint32_t drumListOff = be32( font );
    if( drumId >= numDrums || drumListOff == 0 ||
        !span_ok( fontEntry.size, drumListOff, (size_t)numDrums * 4 )) return false;
    uint32_t drumOff = be32( font + drumListOff + (size_t)drumId * 4 );
    if( drumOff == 0 || !span_ok( fontEntry.size, drumOff, 16 )) return false;
    uint32_t sampleOff;
    float tuning;
    if( !tuned_sample_read( fontId, drumOff + 4, &sampleOff, &tuning )) return false;
    if( !sample_view_fill( fontId, sampleOff, tuning, 39, false, out )) return false;
    out->tunedSample = font + drumOff + 4;
    (void)adsr_view_fill( &fontEntry, font, drumOff, 12, 0, &out->adsr );
    out->pan = font[drumOff + 1];
    out->hasPan = 1;
    return true;
}

bool liboot_audio_font_sfx_sample_get( uint16_t fontId, uint16_t sfxIndex,
                                       LibootAudioSampleView *out )
{
    if( out ) memset( out, 0, sizeof( *out ));
    RomAudioEntry fontEntry;
    const uint8_t *font;
    if( !out || !soundfont_entry_get( fontId, &fontEntry, &font ) ||
        sfxIndex >= fontEntry.shortData3 ) return false;
    uint32_t sfxListOff = be32( font + 4 );
    if( sfxListOff == 0 ||
        !span_ok( fontEntry.size, sfxListOff, (size_t)fontEntry.shortData3 * 8 )) return false;
    uint32_t sampleOff;
    float tuning;
    if( !tuned_sample_read( fontId, sfxListOff + (size_t)sfxIndex * 8,
                            &sampleOff, &tuning )) return false;
    if( !sample_view_fill( fontId, sampleOff, tuning, 39, false, out )) return false;
    out->tunedSample = font + sfxListOff + (size_t)sfxIndex * 8;
    return true;
}

static bool prewarm_tuned_sample( uint16_t fontId, uint32_t tunedOff )
{
    RomAudioEntry fontEntry;
    const uint8_t *font;
    if( !soundfont_entry_get( fontId, &fontEntry, &font ) ||
        !span_ok( fontEntry.size, tunedOff, 8 )) return false;
    uint32_t sampleOff = be32( font + tunedOff );
    float tuning = bef32( font + tunedOff + 4 );
    /* Empty low/high splits are normal and need no cache entry. */
    if( sampleOff == 0 ) return true;
    if( !isfinite( tuning ) || tuning <= 0.0f || tuning > 32.0f ) return false;
    DecodedSample *sample = sample_cache_ensure( fontId, sampleOff );
    return sample && sample->pcm;
}

static bool prewarm_font( uint16_t fontId )
{
    RomAudioEntry fontEntry;
    const uint8_t *font;
    if( !soundfont_entry_get( fontId, &fontEntry, &font )) return false;
    if( s_audio.prewarmedFonts[fontId] ) return true;
    bool ok = true;

    uint8_t numInstruments = (uint8_t)( fontEntry.shortData2 >> 8 );
    if( !span_ok( fontEntry.size, 8, (size_t)numInstruments * 4 )) return false;
    for( uint8_t i = 0; i < numInstruments; ++i ) {
        uint32_t instrumentOff = be32( font + 8 + (size_t)i * 4 );
        if( instrumentOff == 0 ) continue;
        if( !span_ok( fontEntry.size, instrumentOff, 32 )) {
            ok = false;
            continue;
        }
        for( uint32_t tuned = 8; tuned <= 24; tuned += 8 )
            if( !prewarm_tuned_sample( fontId, instrumentOff + tuned )) ok = false;
    }

    uint8_t numDrums = (uint8_t)fontEntry.shortData2;
    uint32_t drumListOff = be32( font );
    if( numDrums != 0 ) {
        if( drumListOff == 0 ||
            !span_ok( fontEntry.size, drumListOff, (size_t)numDrums * 4 )) {
            ok = false;
        } else {
            for( uint8_t i = 0; i < numDrums; ++i ) {
                uint32_t drumOff = be32( font + drumListOff + (size_t)i * 4 );
                if( drumOff == 0 ) continue;
                if( !span_ok( fontEntry.size, drumOff, 16 ) ||
                    !prewarm_tuned_sample( fontId, drumOff + 4 )) ok = false;
            }
        }
    }

    uint16_t numSfx = fontEntry.shortData3;
    uint32_t sfxListOff = be32( font + 4 );
    if( numSfx != 0 ) {
        if( sfxListOff == 0 || !span_ok( fontEntry.size, sfxListOff, (size_t)numSfx * 8 )) {
            ok = false;
        } else {
            for( uint16_t i = 0; i < numSfx; ++i )
                if( !prewarm_tuned_sample( fontId, sfxListOff + (size_t)i * 8 )) ok = false;
        }
    }
    if( ok ) s_audio.prewarmedFonts[fontId] = 1;
    return ok;
}

bool liboot_audio_prewarm_soundfont( uint16_t fontId )
{
    return prewarm_font( fontId );
}

bool liboot_audio_prewarm_sequence( uint16_t sequenceId )
{
    LibootAudioSequenceView sequence;
    if( !liboot_audio_sequence_get( sequenceId, &sequence )) return false;
    bool ok = true;
    for( uint8_t i = 0; i < sequence.numFonts; ++i )
        if( !liboot_audio_prewarm_soundfont( sequence.fontIds[i] )) ok = false;
    return ok;
}

static bool decode_effect( uint16_t index )
{
    if( !s_audio.ready || index >= s_audio.numSfx ) return false;
    DecodedSample *decoded = &s_audio.decoded[index];
    if( decoded->attempted ) return decoded->pcm != NULL;
    decoded->attempted = true;

    LibootAudioSampleView view;
    if( !liboot_audio_font_sfx_sample_get( 0, index, &view )) return false;
    decoded->pcm = (int16_t *)view.pcm;
    decoded->numSamples = view.numSamples;
    decoded->sampleRate = view.sampleRate;
    decoded->loopStart = view.loopStart;
    decoded->loopCount = view.loopCount;
    return true;
}

/* How Sequence 0's voicebank channel scripts pick font-0 sfx entries for each
   NA_SE_VO_LI_* id (0x6800-0x683F): a fixed entry, a random pick that avoids
   immediate repeats, two alternating entries, or a timed multi-note sequence.
   Read from the decomp's
   seq_0 disassembly; the id -> effect-index mapping is version-independent. */
enum {
    VOICE_NONE,      /* the retail sequence channel is silent         */
    VOICE_FIXED,     /* always effect[0] (also single looped clips)   */
    VOICE_RANDOM,    /* uniform over effect[0..count-1], no repeat    */
    VOICE_ALTERNATE, /* cycles through effect[0..count-1]             */
    VOICE_SEQUENCE,  /* effect[0..count-1] played back to back        */
};

typedef struct {
    uint8_t kind, count;
    uint8_t effect[4];
    uint16_t delayNtsc[4], delayPal[4];
    uint8_t velocity[4];
} LinkVoiceMap;

static const LinkVoiceMap s_linkVoice[LINK_VOICE_COUNT] = {
    /* adult, 0x6800.. */
    { VOICE_RANDOM,    4, { 0, 1, 2, 3 }},     /* SWORD_N            */
    { VOICE_RANDOM,    2, { 4, 5 }},           /* SWORD_L            */
    { VOICE_RANDOM,    2, { 21, 22 }},         /* LASH               */
    { VOICE_ALTERNATE, 2, { 6, 25 }},          /* HANG               */
    { VOICE_RANDOM,    2, { 7, 8 }},           /* CLIMB_END          */
    { VOICE_RANDOM,    3, { 9, 10, 11 }},      /* DAMAGE_S           */
    { VOICE_RANDOM,    3, { 12, 13, 14 }},     /* FREEZE             */
    { VOICE_RANDOM,    2, { 17, 18 }},         /* FALL_S             */
    { VOICE_RANDOM,    2, { 15, 16 }},         /* FALL_L             */
    { VOICE_RANDOM,    2, { 19, 23 }},         /* BREATH_REST        */
    { VOICE_FIXED,     1, { 56 }},             /* BREATH_DRINK       */
    { VOICE_SEQUENCE,  3, { 77, 78, 79 }, { 87, 97, 71 }, { 104, 116, 85 }, { 100, 100, 100 }}, /* DOWN */
    { VOICE_RANDOM,    2, { 15, 16 }},         /* TAKEN_AWAY         */
    { VOICE_RANDOM,    3, { 9, 10, 11 }},      /* HELD               */
    { VOICE_SEQUENCE,  3, { 80, 81, 82 }, { 127, 280, 318 }, { 152, 336, 382 }, { 100, 100, 100 }}, /* SNEEZE */
    { VOICE_SEQUENCE,  3, { 83, 84, 85 }, { 290, 163, 53 }, { 378, 196, 64 }, { 100, 100, 100 }}, /* SWEAT */
    { VOICE_FIXED,     1, { 55 }},             /* DRINK (looped)     */
    { VOICE_SEQUENCE,  3, { 86, 87, 88 }, { 185, 134, 116 }, { 222, 161, 116 }, { 100, 100, 100 }}, /* RELAX */
    { VOICE_FIXED,     1, { 0 }},              /* SWORD_PUTAWAY      */
    { VOICE_FIXED,     1, { 67 }},             /* GROAN              */
    { VOICE_ALTERNATE, 2, { 26, 27 }},         /* AUTO_JUMP          */
    { VOICE_FIXED,     1, { 5 }},              /* MAGIC_NALE         */
    { VOICE_FIXED,     1, { 134 }},            /* SURPRISE           */
    { VOICE_FIXED,     1, { 4 }},              /* MAGIC_FROL         */
    { VOICE_FIXED,     1, { 7 }},              /* PUSH               */
    { VOICE_FIXED,     1, { 6 }},              /* HOOKSHOT_HANG      */
    { VOICE_FIXED,     1, { 24 }},             /* LAND_DAMAGE_S      */
    { VOICE_FIXED,     1, { 60 }},             /* NULL_0x1b          */
    { VOICE_FIXED,     1, { 61 }},             /* MAGIC_ATTACK       */
    { VOICE_RANDOM,    2, { 15, 16 }},         /* BL_DOWN            */
    { VOICE_FIXED,     1, { 13 }},             /* DEMO_DAMAGE        */
    { VOICE_RANDOM,    4, { 0, 1, 2, 3 }},     /* ELECTRIC_SHOCK_LV  */
    /* child, 0x6820.. */
    { VOICE_RANDOM,    4, { 28, 29, 30, 31 }}, /* SWORD_N_KID        */
    { VOICE_RANDOM,    2, { 32, 33 }},         /* ROLLING_CUT_KID    */
    { VOICE_RANDOM,    2, { 21, 22 }},         /* LASH_KID           */
    { VOICE_ALTERNATE, 2, { 34, 50 }},         /* HANG_KID           */
    { VOICE_RANDOM,    2, { 35, 36 }},         /* CLIMB_END_KID      */
    { VOICE_RANDOM,    3, { 37, 38, 39 }},     /* DAMAGE_S_KID       */
    { VOICE_RANDOM,    3, { 40, 41, 42 }},     /* FREEZE_KID         */
    { VOICE_RANDOM,    2, { 45, 46 }},         /* FALL_S_KID         */
    { VOICE_RANDOM,    2, { 43, 44 }},         /* FALL_L_KID         */
    { VOICE_RANDOM,    2, { 47, 48 }},         /* BREATH_REST_KID    */
    { VOICE_FIXED,     1, { 52 }},             /* BREATH_DRINK_KID   */
    { VOICE_SEQUENCE,  3, { 64, 65, 66 }, { 98, 106, 58 }, { 118, 127, 58 }, { 100, 100, 100 }}, /* DOWN_KID */
    { VOICE_RANDOM,    2, { 43, 44 }},         /* TAKEN_AWAY_KID     */
    { VOICE_FIXED,     1, { 20 }},             /* HELD_KID           */
    { VOICE_SEQUENCE,  4, { 67, 67, 69, 70 }, { 267, 197, 135, 33 }, { 320, 236, 162, 33 }, { 50, 50, 100, 100 }}, /* SNEEZE_KID */
    { VOICE_SEQUENCE,  3, { 71, 72, 73 }, { 217, 98, 265 }, { 260, 118, 265 }, { 100, 100, 100 }}, /* SWEAT_KID */
    { VOICE_FIXED,     1, { 51 }},             /* DRINK_KID (looped) */
    { VOICE_SEQUENCE,  3, { 74, 75, 76 }, { 65, 266, 83 }, { 78, 319, 83 }, { 100, 100, 100 }}, /* RELAX_KID */
    { VOICE_FIXED,     1, { 28 }},             /* SWORD_PUTAWAY_KID  */
    { VOICE_FIXED,     1, { 67 }},             /* GROAN_KID          */
    { VOICE_ALTERNATE, 2, { 53, 54 }},         /* AUTO_JUMP_KID      */
    { VOICE_FIXED,     1, { 33 }},             /* MAGIC_NALE_KID     */
    { VOICE_FIXED,     1, { 135 }},            /* SURPRISE_KID       */
    { VOICE_FIXED,     1, { 32 }},             /* MAGIC_FROL_KID     */
    { VOICE_FIXED,     1, { 35 }},             /* PUSH_KID           */
    { VOICE_FIXED,     1, { 34 }},             /* HOOKSHOT_HANG_KID  */
    { VOICE_FIXED,     1, { 49 }},             /* LAND_DAMAGE_S_KID  */
    { VOICE_FIXED,     1, { 62 }},             /* NULL_0x1b_KID      */
    { VOICE_FIXED,     1, { 63 }},             /* MAGIC_ATTACK_KID   */
    { VOICE_RANDOM,    2, { 43, 44 }},         /* BL_DOWN_KID        */
    { VOICE_FIXED,     1, { 44 }},             /* DEMO_DAMAGE_KID    */
    { VOICE_RANDOM,    4, { 0, 1, 2, 3 }},     /* ELECTRIC_SHOCK_KID */
    /* Navi/fairy, 0x6840.. Retail routes the first three ids to the empty
       CHAN_612C. NAVY_CALL/dummies use effects 57..59; NA_HELLO_3 addresses
       sound-effect slot 132 directly (layer transpose 2, pitch DF1). */
    { VOICE_NONE,      0, { 0 }},              /* NAVY_ENEMY (silent) */
    { VOICE_NONE,      0, { 0 }},              /* NAVY_HELLO (silent) */
    { VOICE_NONE,      0, { 0 }},              /* NAVY_HEAR  (silent) */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* NAVY_CALL          */
    { VOICE_FIXED,     1, { 132 }},            /* NA_HELLO_3         */
    /* 0x6845-0x684F: table dummies aliased onto the NAVY_CALL script */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x45         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x46         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x47         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x48         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x49         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x4a         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x4b         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x4c         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x4d         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x4e         */
    { VOICE_RANDOM,    3, { 57, 58, 59 }},     /* DUMMY_0x4f         */
};

static uint32_t voice_random( void )
{
    static uint32_t state = 0x2F6E2B1u;
    state = state * 1664525u + 1013904223u;
    return state >> 16;
}

/* Render a seq_0 grunt chain at the first note's sample rate. Each notedv
   delay is a 96 Hz sequence tick duration: a short source naturally falls
   silent, while a looped source repeats until the next note. */
static bool build_sequence( const LinkVoiceMap *map, DecodedSample *out )
{
    uint32_t rate = 0;
    uint64_t total = 0;
    for( uint8_t i = 0; i < map->count; ++i ) {
        if( !decode_effect( map->effect[i] )) return false;
        const DecodedSample *part = &s_audio.decoded[map->effect[i]];
        if( i == 0 ) rate = part->sampleRate;
        uint16_t delay = s_audio.isPal ? map->delayPal[i] : map->delayNtsc[i];
        total += ((uint64_t)delay * rate + 48u ) / 96u;
    }
    if( rate == 0 || total == 0 || total > 1u << 24 ) return false;
    int16_t *pcm = malloc( (size_t)total * sizeof( *pcm ));
    if( !pcm ) return false;
    uint64_t pos = 0;
    for( uint8_t i = 0; i < map->count; ++i ) {
        const DecodedSample *part = &s_audio.decoded[map->effect[i]];
        uint16_t delay = s_audio.isPal ? map->delayPal[i] : map->delayNtsc[i];
        uint64_t count = ((uint64_t)delay * rate + 48u ) / 96u;
        uint8_t velocity = map->velocity[i] ? map->velocity[i] : 100;
        for( uint64_t j = 0; j < count; ++j ) {
            uint64_t step = j * part->sampleRate;
            uint64_t source = step / rate;
            if( source >= part->numSamples ) {
                if( part->loopCount && part->loopStart < part->numSamples )
                    source = part->loopStart + ( source - part->loopStart ) %
                             ( part->numSamples - part->loopStart );
                else {
                    pcm[pos++] = 0;
                    continue;
                }
            }
            uint32_t src = (uint32_t)source;
            int32_t frac = (int32_t)( step % rate );
            int32_t a = part->pcm[src];
            int32_t b = part->pcm[src + 1 < part->numSamples ? src + 1 : src];
            int32_t sample = a + (int32_t)((int64_t)( b - a ) * frac / rate );
            sample = (int32_t)((int64_t)sample * velocity * velocity / 10000 );
            pcm[pos++] = (int16_t)sample;
        }
    }
    out->pcm = pcm;
    out->numSamples = (uint32_t)pos;
    out->sampleRate = rate;
    return true;
}

/* ---- general sfx banks (player / item / event / system / occasion) ------
 *
 * Outside the voice bank, seq_0's per-sfx channel scripts play notes on
 * font-0 INSTRUMENTS rather than font-0 sfx-list entries.  Each sfx id maps
 * to (instrument, semitone) taken from the seq_0 disassembly (first event of
 * the lowest layer).  The engine's pitch rule gives the playback rate:
 *     rate = 32000 * tuning * 2^((semitone - 39) / 12)
 * with the instrument's normal TunedSample when semitone <= normalRangeHi,
 * else its high one.  Floor-material variants (walk/jump/land/bound/slip
 * +0..15, adult forms at +0x80) are all listed explicitly. */
typedef struct {
    uint16_t sfxId;
    uint8_t instrument, semitone;
} EffectNote;

static const EffectNote s_effectNotes[] = {
    { 0x0800,   0,  31 },
    { 0x0801,   1,  36 },
    { 0x0802,   2,  33 },
    { 0x0803,   3,  36 },
    { 0x0804,   4,  36 },
    { 0x0805,   4,  32 },
    { 0x0806,   5,  32 },
    { 0x0807,   0,  27 },
    { 0x0808,   5,  36 },
    { 0x0809,   6,  45 },
    { 0x080A,  11,  35 },
    { 0x080B,  22,  27 },
    { 0x080C,   1,  28 },
    { 0x080D,  24,  31 },
    { 0x080E,   1,  24 },
    { 0x080F,  25,  36 },
    { 0x0810,   0,  31 },
    { 0x0811,   1,  36 },
    { 0x0812,   2,  33 },
    { 0x0813,   3,  32 },
    { 0x0814,   4,  36 },
    { 0x0815,   4,  32 },
    { 0x0816,   5,  31 },
    { 0x0817,   0,  25 },
    { 0x0818,   5,  34 },
    { 0x0819,   6,  39 },
    { 0x081A,  11,  35 },
    { 0x081B,  22,  32 },
    { 0x081C,   0,  31 },
    { 0x081D,  24,  30 },
    { 0x081E,   1,  24 },
    { 0x081F,  25,  31 },
    { 0x0820,   0,  43 },
    { 0x0821,   1,  48 },
    { 0x0822,   2,  45 },
    { 0x0823,   3,  44 },
    { 0x0824,   4,  48 },
    { 0x0825,   4,  44 },
    { 0x0826,   5,  36 },
    { 0x0827,   0,  26 },
    { 0x0828,   5,  43 },
    { 0x0829,   6,  50 },
    { 0x082A,  11,  47 },
    { 0x082B,  22,  44 },
    { 0x082C,   0,  31 },
    { 0x082D,  24,  38 },
    { 0x082E,   1,  36 },
    { 0x082F,  25,  46 },
    { 0x0830,  10,  41 },
    { 0x0831,   9,  48 },
    { 0x0832,   5,  24 },
    { 0x0833,   9,  39 },
    { 0x0834,  28,  31 },
    { 0x0835,   9,  34 },
    { 0x0836,   0,  32 },
    { 0x0839,  20,  27 },
    { 0x083A,  27,  15 },
    { 0x083B,  21,  26 },
    { 0x083C,  27,  17 },
    { 0x083D,  27,   9 },
    { 0x083E,  65,  35 },
    { 0x083F,  30,   4 },
    { 0x0840,  12,  39 },
    { 0x0841,  13,  39 },
    { 0x0842,  14,  44 },
    { 0x0843,  16,  39 },
    { 0x0844,  16,  39 },
    { 0x0845,  16,  39 },
    { 0x0846,  16,  39 },
    { 0x0847,  16,  39 },
    { 0x0848,  17,  39 },
    { 0x0849,  12,  39 },
    { 0x084A,  14,  35 },
    { 0x084B,  14,  35 },
    { 0x084C,   4,  36 },
    { 0x084D,  14,  35 },
    { 0x084E,  13,  28 },
    { 0x084F,  14,  44 },
    { 0x0850,  21,  37 },
    { 0x0851,  21,  37 },
    { 0x0852,  21,  37 },
    { 0x0853,  21,  37 },
    { 0x0854,  21,  37 },
    { 0x0855,  21,  37 },
    { 0x0856,  21,  37 },
    { 0x0857,  21,  37 },
    { 0x0858,  21,  37 },
    { 0x0859,  21,  37 },
    { 0x085A,  21,  37 },
    { 0x085B,  21,  37 },
    { 0x085C,   0,  31 },
    { 0x085D,  21,  37 },
    { 0x085E,  21,  37 },
    { 0x085F,  21,  37 },
    { 0x0863,  62,  51 },
    { 0x0864,  72,  34 },
    { 0x0868,  70,  34 },
    { 0x0870,  21,  37 },
    { 0x0871,  47,  30 },
    { 0x0872,  19,  27 },
    { 0x0874,  15,  31 },
    { 0x0875,  79,  80 },
    { 0x0877,   9,  32 },
    { 0x087B,  60,  53 },
    { 0x0880,   0,  29 },
    { 0x0881,   1,  34 },
    { 0x0882,   2,  31 },
    { 0x0883,   3,  34 },
    { 0x0884,   4,  34 },
    { 0x0885,   4,  30 },
    { 0x0886,   5,  30 },
    { 0x0887,   0,  25 },
    { 0x0888,   5,  34 },
    { 0x0889,   6,  43 },
    { 0x088A,  11,  33 },
    { 0x088B,  22,  25 },
    { 0x088C,   1,  26 },
    { 0x088D,  24,  31 },
    { 0x088E,   0,  31 },
    { 0x088F,  25,  34 },
    { 0x0890,   0,  29 },
    { 0x0891,   1,  34 },
    { 0x0892,   2,  31 },
    { 0x0893,   3,  30 },
    { 0x0894,   4,  34 },
    { 0x0895,   4,  30 },
    { 0x0896,   5,  29 },
    { 0x0897,   0,  23 },
    { 0x0898,   5,  32 },
    { 0x0899,   6,  37 },
    { 0x089A,  11,  33 },
    { 0x089B,  22,  30 },
    { 0x089C,   0,  31 },
    { 0x089D,  24,  30 },
    { 0x089E,   0,  31 },
    { 0x089F,  25,  29 },
    { 0x08A0,   0,  41 },
    { 0x08A1,   1,  46 },
    { 0x08A2,   2,  43 },
    { 0x08A3,   3,  42 },
    { 0x08A4,   4,  46 },
    { 0x08A5,   4,  42 },
    { 0x08A6,   5,  34 },
    { 0x08A7,   0,  24 },
    { 0x08A8,   5,  41 },
    { 0x08A9,   6,  48 },
    { 0x08AA,  11,  45 },
    { 0x08AB,  22,  42 },
    { 0x08AC,   0,  29 },
    { 0x08AD,  24,  38 },
    { 0x08AE,   0,  31 },
    { 0x08AF,  25,  44 },
    { 0x08C1,  12,  44 },
    { 0x08C8,  68,  71 },
    { 0x08C9,  68,  55 },
    { 0x08D0,  12,  39 },
    { 0x08D1,  13,  39 },
    { 0x08D2,  14,  44 },
    { 0x08D3,  16,  39 },
    { 0x08D4,  16,  39 },
    { 0x08D5,  16,  39 },
    { 0x08D6,  16,  39 },
    { 0x08D7,  16,  39 },
    { 0x08D8,  17,  39 },
    { 0x08D9,  12,  39 },
    { 0x08DA,  14,  35 },
    { 0x08DB,  14,  35 },
    { 0x08DC,   4,  36 },
    { 0x08DD,  14,  35 },
    { 0x08DE,  13,  28 },
    { 0x08DF,  14,  44 },
    { 0x1800,  26,  44 },  /* NA_SE_IT_SWORD_IMPACT */
    { 0x1801,  27,  17 },  /* NA_SE_IT_SWORD_SWING */
    { 0x1802,  28,  36 },  /* NA_SE_IT_SWORD_PUTAWAY */
    { 0x1803,  29,  36 },  /* NA_SE_IT_SWORD_PICKOUT */
    { 0x1804,  30,  36 },  /* NA_SE_IT_ARROW_SHOT */
    { 0x1805,  31,  36 },  /* NA_SE_IT_BOOMERANG_THROW */
    { 0x1806,  32,  38 },  /* NA_SE_IT_SHIELD_BOUND */
    { 0x1807,  33,  45 },  /* NA_SE_IT_BOW_DRAW */
    { 0x1808,  32,  39 },  /* NA_SE_IT_SHIELD_REFLECT_SW */
    { 0x1809,  38,  39 },  /* NA_SE_IT_ARROW_STICK_HRAD */
    { 0x180A,  26,   4 },  /* NA_SE_IT_HAMMER_HIT */
    { 0x180B,  34,  50 },  /* NA_SE_IT_HOOKSHOT_CHAIN */
    { 0x180C,  32,  44 },  /* NA_SE_IT_SHIELD_REFLECT_MG */
    { 0x180D,  35,  36 },  /* NA_SE_IT_BOMB_IGNIT */
    { 0x180E,  47,  44 },  /* NA_SE_IT_BOMB_EXPLOSION */
    { 0x180F,  35,  43 },  /* NA_SE_IT_BOMB_UNEXPLOSION */
    { 0x1810,  30,  20 },  /* NA_SE_IT_BOOMERANG_FLY */
    { 0x1811,  43,  21 },  /* NA_SE_IT_SWORD_STRIKE */
    { 0x1812,  27,   7 },  /* NA_SE_IT_HAMMER_SWING */
    { 0x1813,  26,  13 },  /* NA_SE_IT_HOOKSHOT_REFLECT */
    { 0x1814,  30,   7 },  /* NA_SE_IT_ARROW_STICK_CRE */
    { 0x1815,  38,  39 },  /* NA_SE_IT_ARROW_STICK_OBJ */
    { 0x1816,  30,  13 },  /* NA_SE_IT_DUMMY */
    { 0x1817,  30,  24 },  /* NA_SE_IT_DUMMY2 */
    { 0x1818,  27,  20 },  /* NA_SE_IT_SWORD_SWING_HARD */
    { 0x1819,  36,  27 },  /* NA_SE_IT_DUMMY3 */
    { 0x181A,  26,  19 },  /* NA_SE_IT_WALL_HIT_HARD */
    { 0x181B,  26,  36 },  /* NA_SE_IT_WALL_HIT_SOFT */
    { 0x181C,  26,  12 },  /* NA_SE_IT_STONE_HIT */
    { 0x181D,  40,  23 },  /* NA_SE_IT_WOODSTICK_BROKEN */
    { 0x181E,  30,  36 },  /* NA_SE_IT_LASH */
    { 0x181F,  28,  32 },  /* NA_SE_IT_SHIELD_POSTURE */
    { 0x1820,  30,   0 },  /* NA_SE_IT_SLING_SHOT */
    { 0x1821,  33,  50 },  /* NA_SE_IT_SLING_DRAW */
    { 0x1822,  42,  67 },  /* NA_SE_IT_SWORD_CHARGE */
    { 0x1823,  39,  32 },  /* NA_SE_IT_ROLLING_CUT */
    { 0x1824,  43,  24 },  /* NA_SE_IT_SWORD_STRIKE_HARD */
    { 0x1825,  26,  72 },  /* NA_SE_IT_SLING_REFLECT */
    { 0x1826,  28,  36 },  /* NA_SE_IT_SHIELD_REMOVE */
    { 0x1827,  28,  42 },  /* NA_SE_IT_HOOKSHOT_READY */
    { 0x1828,  34,  53 },  /* NA_SE_IT_HOOKSHOT_RECEIVE */
    { 0x1829,  47,  26 },  /* NA_SE_IT_HOOKSHOT_STICK_OBJ */
    { 0x182A,  42,  56 },  /* NA_SE_IT_SWORD_REFLECT_MG */
    { 0x182B,  26,  69 },  /* NA_SE_IT_DEKU */
    { 0x182C,  81,  23 },  /* NA_SE_IT_WALL_HIT_BUYO */
    { 0x182D,  29,  34 },  /* NA_SE_IT_SWORD_PUTAWAY_STN */
    { 0x182E,  39,  30 },  /* NA_SE_IT_ROLLING_CUT_LV1 */
    { 0x182F,  39,  32 },  /* NA_SE_IT_ROLLING_CUT_LV2 */
    { 0x1830,  33,  95 },  /* NA_SE_IT_BOW_FLICK */
    { 0x1831,  35,  91 },  /* NA_SE_IT_BOMBCHU_MOVE */
    { 0x1832,   8,  24 },  /* NA_SE_IT_SHIELD_CHARGE_LV1 */
    { 0x1833,   8,  24 },  /* NA_SE_IT_SHIELD_CHARGE_LV2 */
    { 0x1834,   8,  24 },  /* NA_SE_IT_SHIELD_CHARGE_LV3 */
    { 0x1835,  33, 100 },  /* NA_SE_IT_SLING_FLICK */
    { 0x1836,  47,  20 },  /* NA_SE_IT_SWORD_STICK_STN */
    { 0x1837,  63,  45 },  /* NA_SE_IT_REFLECTION_WOOD */
    { 0x1838,  32,  35 },  /* NA_SE_IT_SHIELD_REFLECT_MG2 */
    { 0x1839,  39,  44 },  /* NA_SE_IT_MAGIC_ARROW_SHOT */
    { 0x183A,  67,  27 },  /* NA_SE_IT_EXPLOSION_FRAME */
    { 0x183B,  68,  61 },  /* NA_SE_IT_EXPLOSION_ICE */
    { 0x183C,  64,  48 },  /* NA_SE_IT_EXPLOSION_LIGHT */
    { 0x183D,  33,  44 },  /* NA_SE_IT_FISHING_REEL_SLOW */
    { 0x183E,  33,  48 },  /* NA_SE_IT_FISHING_REEL_HIGH */
    { 0x183F,  33,  45 },  /* NA_SE_IT_PULL_FISHING_ROD */
    { 0x1840,  46,  31 },  /* NA_SE_IT_DM_FLYING_GOD_PASS */
    { 0x1841,  46,  19 },  /* NA_SE_IT_DM_FLYING_GOD_DASH */
    { 0x1842,  46,  24 },  /* NA_SE_IT_DM_RING_EXPLOSION */
    { 0x1843,  47,  50 },  /* NA_SE_IT_DM_RING_GATHER */
    { 0x1844,  50,  39 },  /* NA_SE_IT_INGO_HORSE_NEIGH */
    { 0x1845,  47,  50 },  /* NA_SE_IT_EARTHQUAKE */
    { 0x1846,  47,  50 },  /* NA_SE_IT_DUMMY4 */
    { 0x1847,  33,  95 },  /* NA_SE_IT_KAKASHI_JUMP */
    { 0x1848,  18,  13 },  /* NA_SE_IT_FLAME */
    { 0x1849,  68,  68 },  /* NA_SE_IT_SHIELD_BEAM */
    { 0x184A,  41,  46 },  /* NA_SE_IT_FISHING_HIT */
    { 0x184B,  47,  34 },  /* NA_SE_IT_GOODS_APPEAR */
    { 0x184C,  26,  67 },  /* NA_SE_IT_MAJIN_SWORD_BROKEN */
    { 0x184D,  41,  34 },  /* NA_SE_IT_HAND_CLAP */
    { 0x184E,  27,   7 },  /* NA_SE_IT_MASTER_SWORD_SWING */
    { 0x184F,  35,  43 },  /* NA_SE_IT_DUMMY5 */
    { 0x2800,  45,  36 },  /* NA_SE_EV_DOOR_OPEN */
    { 0x2801,  45,  84 },  /* NA_SE_EV_DOOR_CLOSE */
    { 0x2802,  47,  48 },  /* NA_SE_EV_EXPLOSION */
    { 0x2803,  48,  34 },  /* NA_SE_EV_HORSE_WALK */
    { 0x2804,  48,  39 },  /* NA_SE_EV_HORSE_RUN */
    { 0x2805,  50,  39 },  /* NA_SE_EV_HORSE_NEIGH */
    { 0x2806,  51,  27 },  /* NA_SE_EV_RIVER_STREAM */
    { 0x2807,  51,  87 },  /* NA_SE_EV_WATER_WALL_BIG */
    { 0x2808,  19,  50 },  /* NA_SE_EV_OUT_OF_WATER */
    { 0x2809,  19,  24 },  /* NA_SE_EV_DIVE_WATER */
    { 0x280A,  55,  27 },  /* NA_SE_EV_ROCK_SLIDE */
    { 0x280B,  56,  31 },  /* NA_SE_EV_MAGMA_LEVEL */
    { 0x280C,  34,  32 },  /* NA_SE_EV_BRIDGE_OPEN */
    { 0x280D,  57,  24 },  /* NA_SE_EV_BRIDGE_CLOSE */
    { 0x280E,  59,   5 },  /* NA_SE_EV_BRIDGE_OPEN_STOP */
    { 0x280F,  59,  11 },  /* NA_SE_EV_BRIDGE_CLOSE_STOP */
    { 0x2810,  47,  39 },  /* NA_SE_EV_WALL_BROKEN */
    { 0x2811,  58,  40 },  /* NA_SE_EV_CHICKEN_CRY_N */
    { 0x2812,  61,  35 },  /* NA_SE_EV_CHICKEN_CRY_A */
    { 0x2813,  61,  39 },  /* NA_SE_EV_CHICKEN_CRY_M */
    { 0x2814,  54,  38 },  /* NA_SE_EV_SLIDE_DOOR_OPEN */
    { 0x2815,  63,  39 },  /* NA_SE_EV_FOOT_SWITCH */
    { 0x2816,  50,  83 },  /* NA_SE_EV_HORSE_GROAN */
    { 0x2817,  62,  44 },  /* NA_SE_EV_BOMB_DROP_WATER */
    { 0x2818,  48,  41 },  /* NA_SE_EV_HORSE_JUMP */
    { 0x2819,  48,  44 },  /* NA_SE_EV_HORSE_LAND */
    { 0x281A,  12,  27 },  /* NA_SE_EV_HORSE_SLIP */
    { 0x281B,  69,  62 },  /* NA_SE_EV_FAIRY_DASH */
    { 0x281C,  54,  32 },  /* NA_SE_EV_SLIDE_DOOR_CLOSE */
    { 0x281D,  77,  43 },  /* NA_SE_EV_STONE_BOUND */
    { 0x281E,  54,  24 },  /* NA_SE_EV_STONE_STATUE_OPEN */
    { 0x281F,  66,  34 },  /* NA_SE_EV_TBOX_UNLOCK */
    { 0x2820,  66,  29 },  /* NA_SE_EV_TBOX_OPEN */
    { 0x2821,  33,  86 },  /* NA_SE_SY_TIMER */
    { 0x2822,  67,  39 },  /* NA_SE_EV_FLAME_IGNITION */
    { 0x2823,  26,  39 },  /* NA_SE_EV_SPEAR_HIT */
    { 0x2824,  54,  36 },  /* NA_SE_EV_ELEVATOR_MOVE */
    { 0x2825,  68,  47 },  /* NA_SE_EV_WARP_HOLE */
    { 0x2826,  68,  53 },  /* NA_SE_EV_LINK_WARP */
    { 0x2827,  47,  40 },  /* NA_SE_EV_PILLAR_SINK */
    { 0x2828,  51,  36 },  /* NA_SE_EV_WATER_WALL */
    { 0x2829,  51,  22 },  /* NA_SE_EV_RIVER_STREAM_S */
    { 0x282A,  51,  34 },  /* NA_SE_EV_RIVER_STREAM_F */
    { 0x282B,  48,  38 },  /* NA_SE_EV_HORSE_LAND2 */
    { 0x282C,  12,  39 },  /* NA_SE_EV_HORSE_SANDDUST */
    { 0x282D,  70,  46 },  /* NA_SE_EV_DUMMY45 */
    { 0x282E,  74,  82 },  /* NA_SE_EV_LIGHTNING */
    { 0x282F,  36,  27 },  /* NA_SE_EV_BOMB_BOUND */
    { 0x2830,  71,  39 },  /* NA_SE_EV_WATERDROP */
    { 0x2831,  73,  37 },  /* NA_SE_EV_TORCH */
    { 0x2832,  56,  36 },  /* NA_SE_EV_MAGMA_LEVEL_M */
    { 0x2833,  74,  32 },  /* NA_SE_EV_FIRE_PILLAR */
    { 0x2834,  74,  39 },  /* NA_SE_EV_FIRE_PLATE */
    { 0x2835,  65,  45 },  /* NA_SE_EV_BLOCK_BOUND */
    { 0x2836,  75,  29 },  /* NA_SE_EV_METALDOOR_SLIDE */
    { 0x2837,  76,  36 },  /* NA_SE_EV_METALDOOR_STOP */
    { 0x2838,  65,  27 },  /* NA_SE_EV_BLOCK_SHAKE */
    { 0x2839,  47,  51 },  /* NA_SE_EV_BOX_BREAK */
    { 0x283A,  65,  52 },  /* NA_SE_EV_HAMMER_SWITCH */
    { 0x283B,  56,  42 },  /* NA_SE_EV_MAGMA_LEVEL_L */
    { 0x283C,  26,  65 },  /* NA_SE_EV_SPEAR_FENCE */
    { 0x283D,  50,  41 },  /* NA_SE_EV_GANON_HORSE_NEIGH */
    { 0x283E,  50,  85 },  /* NA_SE_EV_GANON_HORSE_GROAN */
    { 0x283F,  68,  44 },  /* NA_SE_EV_FANTOM_WARP_S */
    { 0x2840,  68,  44 },  /* NA_SE_EV_FANTOM_WARP_L */
    { 0x2841,  51,  93 },  /* NA_SE_EV_FOUNTAIN */
    { 0x2842,  48,  39 },  /* NA_SE_EV_KID_HORSE_WALK */
    { 0x2843,  48,  43 },  /* NA_SE_EV_KID_HORSE_RUN */
    { 0x2844,  50,  43 },  /* NA_SE_EV_KID_HORSE_NEIGH */
    { 0x2845,  50,  87 },  /* NA_SE_EV_KID_HORSE_GROAN */
    { 0x2846,  64,  63 },  /* NA_SE_EV_WHITE_OUT */
    { 0x2847,  46,  99 },  /* NA_SE_EV_LIGHT_GATHER */
    { 0x2848,   7,  36 },  /* NA_SE_EV_TREE_CUT */
    { 0x2849,  77,  44 },  /* NA_SE_EV_VOLCANO */
    { 0x284A,  34,  43 },  /* NA_SE_EV_GUILLOTINE_UP */
    { 0x284B,  34,  48 },  /* NA_SE_EV_GUILLOTINE_BOUND */
    { 0x284C,  75,  24 },  /* NA_SE_EV_ROLLCUTTER_MOTOR */
    { 0x284D,  34,  46 },  /* NA_SE_EV_CHINETRAP_DOWN */
    { 0x284E,   7,  31 },  /* NA_SE_EV_PLANT_BROKEN */
    { 0x284F,  87,  84 },  /* NA_SE_EV_SHIP_BELL */
    { 0x2850,  78,  34 },  /* NA_SE_EV_FLUTTER_FLAG */
    { 0x2851,  76,  36 },  /* NA_SE_EV_TRAP_BOUND */
    { 0x2852,  47,  42 },  /* NA_SE_EV_ROCK_BROKEN */
    { 0x2853,  52,  17 },  /* NA_SE_EV_FANTOM_WARP_S2 */
    { 0x2854,  52,  15 },  /* NA_SE_EV_FANTOM_WARP_L2 */
    { 0x2855,  45,  27 },  /* NA_SE_EV_COFFIN_CAP_OPEN */
    { 0x2856,  63,  16 },  /* NA_SE_EV_COFFIN_CAP_BOUND */
    { 0x2857,  60,  62 },  /* NA_SE_EV_WIND_TRAP */
    { 0x2858,  66,   5 },  /* NA_SE_EV_TRAP_OBJ_SLIDE */
    { 0x2859,  75,  38 },  /* NA_SE_EV_METALDOOR_OPEN */
    { 0x285A,  75,  32 },  /* NA_SE_EV_METALDOOR_CLOSE */
    { 0x285B,  73,  44 },  /* NA_SE_EV_BURN_OUT */
    { 0x285C,  77,  56 },  /* NA_SE_EV_BLOCKSINK */
    { 0x285D,  79,  39 },  /* NA_SE_EV_CROWD */
    { 0x285E,  13,  32 },  /* NA_SE_EV_WATER_LEVEL_DOWN */
    { 0x285F,  69,  56 },  /* NA_SE_EV_NAVY_VANISH */
    { 0x2860,  63,  66 },  /* NA_SE_EV_LADDER_DOUND */
    { 0x2861,  80,  20 },  /* NA_SE_EV_WEB_VIBRATION */
    { 0x2862,  80,  26 },  /* NA_SE_EV_WEB_BROKEN */
    { 0x2863,  52,  12 },  /* NA_SE_EV_ROLL_STAND */
    { 0x2866,  66,  14 },  /* NA_SE_EV_WOODDOOR_OPEN */
    { 0x2867,  75,  31 },  /* NA_SE_EV_METALGATE_OPEN */
    { 0x2868,  62,  36 },  /* NA_SE_IT_SCOOP_UP_WATER */
    { 0x2869,  62,  48 },  /* NA_SE_EV_FISH_LEAP */
    { 0x286A,   7,  32 },  /* NA_SE_EV_KAKASHI_SWING */
    { 0x286B,   7,  41 },  /* NA_SE_EV_KAKASHI_ROLL */
    { 0x286C,  41,  84 },  /* NA_SE_EV_BOTTLE_CAP_OPEN */
    { 0x286D,  60,  51 },  /* NA_SE_EV_JABJAB_BREATHE */
    { 0x286E,  68,  63 },  /* NA_SE_EV_SPIRIT_STONE */
    { 0x286F,  42,  78 },  /* NA_SE_EV_TRIFORCE_FLASH */
    { 0x2870,  16,  39 },  /* NA_SE_EV_FALL_DOWN_DIRT */
    { 0x2871,  69,  53 },  /* NA_SE_EV_NAVY_FLY */
    { 0x2872,  11,  36 },  /* NA_SE_EV_NAVY_CRASH */
    { 0x2873,  63,  71 },  /* NA_SE_EV_WOOD_HIT */
    { 0x2874,  51,  92 },  /* NA_SE_EV_SCOOPUP_WATER */
    { 0x2875,  71,  46 },  /* NA_SE_EV_DROP_FALL */
    { 0x2876,  66,  15 },  /* NA_SE_EV_WOOD_GEAR */
    { 0x2877,   7,  26 },  /* NA_SE_EV_TREE_SWING */
    { 0x2878,  48,  39 },  /* NA_SE_EV_HORSE_RUN_LEVEL */
    { 0x2879,  77,  79 },  /* NA_SE_EV_ELEVATOR_MOVE2 */
    { 0x287A,  76,  32 },  /* NA_SE_EV_ELEVATOR_STOP */
    { 0x287B,  64,  46 },  /* NA_SE_EV_TRE_BOX_APPEAR */
    { 0x287C,  34,  45 },  /* NA_SE_EV_CHAIN_KEY_UNLOCK */
    { 0x287D,  29,  25 },  /* NA_SE_EV_SPINE_TRAP_MOVE */
    { 0x287E,  46,  44 },  /* NA_SE_EV_HEALING */
    { 0x287F,  64,  58 },  /* NA_SE_EV_GREAT_FAIRY_APPEAR */
    { 0x2880,  64,  58 },  /* NA_SE_EV_GREAT_FAIRY_VANISH */
    { 0x2881,   8,  35 },  /* NA_SE_EV_RED_EYE */
    { 0x2882,  54,  32 },  /* NA_SE_EV_ROLL_STAND_2 */
    { 0x2883,  54,  31 },  /* NA_SE_EV_WALL_SLIDE */
    { 0x2884,  68,  65 },  /* NA_SE_EV_TRE_BOX_FLASH */
    { 0x2885,  66,  63 },  /* NA_SE_EV_WINDMILL_LEVEL */
    { 0x2886,  68,  60 },  /* NA_SE_EV_GOTO_HEAVEN */
    { 0x2887,  70,  73 },  /* NA_SE_EV_POT_BROKEN */
    { 0x2888,  70,  63 },  /* NA_SE_PL_PUT_DOWN_POT */
    { 0x2889,  19,  32 },  /* NA_SE_EV_DIVE_INTO_WATER */
    { 0x288A,  19,  36 },  /* NA_SE_EV_JUMP_OUT_WATER */
    { 0x288B,  77,  53 },  /* NA_SE_EV_GOD_FLYING */
    { 0x288C,  46,  47 },  /* NA_SE_EV_TRIFORCE */
    { 0x288D,  68,  68 },  /* NA_SE_EV_AURORA */
    { 0x288E,  77,  29 },  /* NA_SE_EV_DEKU_DEATH */
    { 0x288F,  74,  29 },  /* NA_SE_EV_BUYOSTAND_RISING */
    { 0x2890,  74,  29 },  /* NA_SE_EV_BUYOSTAND_FALL */
    { 0x2891,   8,   5 },  /* NA_SE_EV_BUYOSHUTTER_OPEN */
    { 0x2892,   8,   5 },  /* NA_SE_EV_BUYOSHUTTER_CLOSE */
    { 0x2893,  77,  43 },  /* NA_SE_EV_STONEDOOR_STOP */
    { 0x2894,  68,  66 },  /* NA_SE_EV_S_STONE_REVIVAL */
    { 0x2895,  46,  93 },  /* NA_SE_EV_MEDAL_APPEAR_S */
    { 0x2896,  21,  23 },  /* NA_SE_EV_HUMAN_BOUND */
    { 0x2897,  46,  80 },  /* NA_SE_EV_MEDAL_APPEAR_L */
    { 0x2898,  77,  48 },  /* NA_SE_EV_EARTHQUAKE */
    { 0x2899,   8,  15 },  /* NA_SE_EV_SHUT_BY_CRYSTAL */
    { 0x289A,  46, 102 },  /* NA_SE_EV_GOD_LIGHTBALL_2 */
    { 0x289B,  74,  62 },  /* NA_SE_EV_RUN_AROUND */
    { 0x289C,  74,  62 },  /* NA_SE_EV_CONSENTRATION */
    { 0x289D,  68,  72 },  /* NA_SE_EV_TIMETRIP_LIGHT */
    { 0x289E,  65,  36 },  /* NA_SE_EV_BUYOSTAND_STOP_A */
    { 0x289F,  77,  43 },  /* NA_SE_EV_BUYOSTAND_STOP_U */
    { 0x28A1,   2,  33 },  /* NA_SE_EV_JUMP_CONC */
    { 0x28A2,  35,  24 },  /* NA_SE_EV_ICE_MELT */
    { 0x28A3,  18,  13 },  /* NA_SE_EV_FIRE_PILLAR_S */
    { 0x28A4,  54,  33 },  /* NA_SE_EV_BLOCK_RISING */
    { 0x28A5,  64,  55 },  /* NA_SE_EV_NABALL_VANISH */
    { 0x28A6,  69,  62 },  /* NA_SE_EV_SARIA_MELODY */
    { 0x28A7,  69,  62 },  /* NA_SE_EV_LINK_WARP_OUT */
    { 0x28A8,  46,  99 },  /* NA_SE_EV_FIATY_HEAL */
    { 0x28A9,  26,   7 },  /* NA_SE_EV_CHAIN_KEY_UNLOCK_B */
    { 0x28AA,  47,  39 },  /* NA_SE_EV_WOODBOX_BREAK */
    { 0x28AB,  63,  27 },  /* NA_SE_EV_PUT_DOWN_WOODBOX */
    { 0x28AC,   3,  44 },  /* NA_SE_EV_LAND_DIRT */
    { 0x28AD,  77,  33 },  /* NA_SE_EV_FLOOR_ROLLING */
    { 0x28AE,  61,  82 },  /* NA_SE_EV_DOG_CRY_EVENING */
    { 0x28AF,  81,  17 },  /* NA_SE_EV_JABJAB_HICCUP */
    { 0x28B0,  27,  17 },  /* NA_SE_EV_NALE_MAGIC */
    { 0x28B1,  58,  75 },  /* NA_SE_EV_FROG_JUMP */
    { 0x28B2,  15,  15 },  /* NA_SE_EV_ICE_FREEZE */
    { 0x28B3,  78,  34 },  /* NA_SE_EV_BURNING */
    { 0x28B4,  63,  33 },  /* NA_SE_EV_WOODPLATE_BOUND */
    { 0x28B5,  71,  33 },  /* NA_SE_EV_GORON_WATER_DROP */
    { 0x28B6,  81,  15 },  /* NA_SE_EV_JABJAB_GROAN */
    { 0x28B7,  47,  51 },  /* NA_SE_EV_DARUMA_VANISH */
    { 0x28B8,  55,  27 },  /* NA_SE_EV_BIGBALL_ROLL */
    { 0x28B9,  77,  56 },  /* NA_SE_EV_ELEVATOR_MOVE3 */
    { 0x28BA,  26,  39 },  /* NA_SE_EV_DIAMOND_SWITCH */
    { 0x28BB,  78,  34 },  /* NA_SE_EV_FLAME_OF_FIRE */
    { 0x28BC,  46,  47 },  /* NA_SE_EV_RAINBOW_SHOWER */
    { 0x28BD,  78,  34 },  /* NA_SE_EV_FLYING_AIR */
    { 0x28BE,  60,  53 },  /* NA_SE_EV_PASS_AIR */
    { 0x28BF,  47,  48 },  /* NA_SE_EV_COME_UP_DEKU_JR */
    { 0x28C0,  15,  98 },  /* NA_SE_EV_SAND_STORM */
    { 0x28C1,  42,  78 },  /* NA_SE_EV_TRIFORCE_MARK */
    { 0x28C2,  47,  39 },  /* NA_SE_EV_GRAVE_EXPLOSION */
    { 0x28C3,  20,  27 },  /* NA_SE_EV_LURE_MOVE_W */
    { 0x28C4,  70,  65 },  /* NA_SE_EV_POT_MOVE_START */
    { 0x28C5,  62,  50 },  /* NA_SE_EV_DIVE_INTO_WATER_L */
    { 0x28C6,  62,  50 },  /* NA_SE_EV_OUT_OF_WATER_L */
    { 0x28C7,  78,  32 },  /* NA_SE_EV_GANON_MANTLE */
    { 0x28C8,   1,  34 },  /* NA_SE_EV_DIG_UP */
    { 0x28C9,  63,  27 },  /* NA_SE_EV_WOOD_BOUND */
    { 0x28CA,  72,  34 },  /* NA_SE_EV_WATER_BUBBLE */
    { 0x28CB,  79,  87 },  /* NA_SE_EV_ICE_BROKEN */
    { 0x28CD,  13,  30 },  /* NA_SE_EV_WATER_CONVECTION */
    { 0x28CE,  54,  75 },  /* NA_SE_EV_GROUND_GATE_OPEN */
    { 0x28CF,  47,  39 },  /* NA_SE_EV_FACE_BREAKDOWN */
    { 0x28D0,  47,  39 },  /* NA_SE_EV_FACE_EXPLOSION */
    { 0x28D1,  47,  24 },  /* NA_SE_EV_FACE_CRUMBLE_SLOW */
    { 0x28D2,  29,  22 },  /* NA_SE_EV_ROUND_TRAP_MOVE */
    { 0x28D3,  87,  87 },  /* NA_SE_EV_HIT_SOUND */
    { 0x28D4,  79,  84 },  /* NA_SE_EV_ICE_SWING */
    { 0x28D5,  68,  60 },  /* NA_SE_EV_DOWN_TO_GROUND */
    { 0x28D6,  71,   2 },  /* NA_SE_EV_KENJA_ENVIROMENT_0 */
    { 0x28D7,  60,  27 },  /* NA_SE_EV_KENJA_ENVIROMENT_1 */
    { 0x28D8,  81,  87 },  /* NA_SE_EV_SMALL_DOG_BARK */
    { 0x28D9,  64,  51 },  /* NA_SE_EV_ZELDA_POWER */
    { 0x28DB,  54,  89 },  /* NA_SE_EV_IRON_DOOR_OPEN */
    { 0x28DC,  54,  82 },  /* NA_SE_EV_IRON_DOOR_CLOSE */
    { 0x28DD,  13,  38 },  /* NA_SE_EV_WHIRLPOOL */
    { 0x28DE,  47,  39 },  /* NA_SE_EV_TOWER_PARTS_BROKEN */
    { 0x28DF,  73,  87 },  /* NA_SE_EV_COW_CRY */
    { 0x28E0,  54, 106 },  /* NA_SE_EV_METAL_BOX_BOUND */
    { 0x28E1,  47,  39 },  /* NA_SE_EV_ELECTRIC_EXPLOSION */
    { 0x28E2,  47,  48 },  /* NA_SE_EV_HEAVY_THROW */
    { 0x28E3,  58,  80 },  /* NA_SE_EV_FROG_CRY_0 */
    { 0x28E4,  58,  80 },  /* NA_SE_EV_FROG_CRY_1 */
    { 0x28E5,  73,  87 },  /* NA_SE_EV_COW_CRY_LV */
    { 0x28E6,  26,  19 },  /* NA_SE_EV_RONRON_DOOR_CLOSE */
    { 0x28E7,  46,  99 },  /* NA_SE_EV_BUTTERFRY_TO_FAIRY */
    { 0x28EA,  74,  39 },  /* NA_SE_EV_STONE_LAUNCH */
    { 0x28EB,  54,  32 },  /* NA_SE_EV_STONE_ROLLING */
    { 0x28EC,  57,  28 },  /* NA_SE_EV_TOGE_STICK_ROLLING */
    { 0x28ED,  52,  27 },  /* NA_SE_EV_TOWER_ENERGY */
    { 0x28EE,  68,  46 },  /* NA_SE_EV_TOWER_BARRIER */
    { 0x28EF,   0,  19 },  /* NA_SE_EV_CHIBI_WALK */
    { 0x28F0,  26,  44 },  /* NA_SE_EV_KNIGHT_WALK */
    { 0x28F1,  65,  45 },  /* NA_SE_EV_PILLAR_MOVE_STOP */
    { 0x28F2,  47,  48 },  /* NA_SE_EV_ERUPTION_CLOUD */
    { 0x28F3,  58,  80 },  /* NA_SE_EV_LINK_WARP_OUT_LV */
    { 0x28F4,  58,  80 },  /* NA_SE_EV_LINK_WARP_IN */
    { 0x28F5,  73,  87 },  /* NA_SE_EV_OCARINA_BMELO_0 */
    { 0x28F6,  26,  19 },  /* NA_SE_EV_OCARINA_BMELO_1 */
    { 0x28F7,  46,  99 },  /* NA_SE_EV_EXPLOSION_FOR_RENZOKU */
    { 0x4800,  84,  49 },  /* NA_SE_SY_WIN_OPEN */
    { 0x4801,  84,  56 },  /* NA_SE_SY_WIN_CLOSE */
    { 0x4803,  91,  51 },  /* NA_SE_SY_GET_RUPY */
    { 0x4808,  84,  48 },  /* NA_SE_SY_DECIDE */
    { 0x480C,  84,  60 },  /* NA_SE_SY_ATTENTION_ON */
    { 0x480D,  84,  57 },  /* NA_SE_SY_DUMMY_13 */
    { 0x480E,  26,  39 },  /* NA_SE_SY_DUMMY_14 */
    { 0x4810,  84,  41 },  /* NA_SE_SY_LOCK_ON_HUMAN */
    { 0x4811,  84,  57 },  /* NA_SE_SY_DUMMY_17 */
    { 0x4812,  84,  48 },  /* NA_SE_SY_DUMMY_18 */
    { 0x4813,  84,  53 },  /* NA_SE_SY_CAMERA_ZOOM_UP */
    { 0x4814,  84,  48 },  /* NA_SE_SY_CAMERA_ZOOM_DOWN */
    { 0x4815,  84,  53 },  /* NA_SE_SY_DUMMY_21 */
    { 0x4816,  84,  53 },  /* NA_SE_SY_DUMMY_22 */
    { 0x4817,  84,  42 },  /* NA_SE_SY_ATTENTION_ON_OLD */
    { 0x4818,  84,  46 },  /* NA_SE_SY_MESSAGE_PASS */
    { 0x481C,  47,  40 },  /* NA_SE_SY_DUMMY_28 */
    { 0x481D,  47,  40 },  /* NA_SE_SY_DEMO_CUT */
    { 0x481E,  69,  48 },  /* NA_SE_SY_NAVY_CALL */
    { 0x4825,  84,  44 },  /* NA_SE_SY_WIN_SCROLL_LEFT */
    { 0x4826,  84,  44 },  /* NA_SE_SY_WIN_SCROLL_RIGHT */
    { 0x4827,  87,  46 },  /* NA_SE_SY_OCARINA_ERROR */
    { 0x4828,  84,  36 },  /* NA_SE_SY_CAMERA_ZOOM_UP_2 */
    { 0x4829,  84,  48 },  /* NA_SE_SY_CAMERA_ZOOM_DOWN_2 */
    { 0x482A,  44,  39 },  /* NA_SE_SY_GLASSMODE_ON */
    { 0x482B,  44,  39 },  /* NA_SE_SY_GLASSMODE_OFF */
    { 0x482C,  88,  42 },  /* NA_SE_SY_FOUND */
    { 0x482D,  87,  87 },  /* NA_SE_SY_HIT_SOUND */
    { 0x482E,  84,  43 },  /* NA_SE_SY_MESSAGE_END */
    { 0x482F,  91,  58 },  /* NA_SE_SY_RUPY_COUNT */
    { 0x4830,  90,  87 },  /* NA_SE_SY_LOCK_ON */
    { 0x4832,  14,  74 },  /* NA_SE_SY_WHITE_OUT_L */
    { 0x4833,  14,  74 },  /* NA_SE_SY_WHITE_OUT_S */
    { 0x4834,  68,  86 },  /* NA_SE_SY_WHITE_OUT_T */
    { 0x4835,  21,  87 },  /* NA_SE_SY_START_SHOT */
    { 0x4837,  90,  39 },  /* NA_SE_SY_ATTENTION_URGENCY */
    { 0x4839,  68,  63 },  /* NA_SE_SY_FSEL_CURSOR */
    { 0x483A,  68,  67 },  /* NA_SE_SY_FSEL_DECIDE_S */
    { 0x483B,  68,  72 },  /* NA_SE_SY_FSEL_DECIDE_L */
    { 0x483C,  68,  60 },  /* NA_SE_SY_FSEL_CLOSE */
    { 0x483E,   8,  15 },  /* NA_SE_SY_SET_FIRE_ARROW */
    { 0x483F,   8,  15 },  /* NA_SE_SY_SET_ICE_ARROW */
    { 0x4840,   8,  15 },  /* NA_SE_SY_SET_LIGHT_ARROW */
    { 0x4841,   8,  27 },  /* NA_SE_SY_SYNTH_MAGIC_ARROW */
    { 0x4843,  84,  56 },  /* NA_SE_SY_KINSTA_MARK_APPEAR */
    { 0x4847,  90,  39 },  /* NA_SE_SY_DUMMY_71 */
    { 0x5800,  52,  39 },  /* NA_SE_OC_OCARINA */
    { 0x5801,  46,  64 },  /* NA_SE_OC_ABYSS */
    { 0x5802,  45,  36 },  /* NA_SE_OC_DOOR_OPEN */
    { 0x5803,  64,  58 },  /* NA_SE_OC_SECRET_WARP_IN */
    { 0x5804,  64,  58 },  /* NA_SE_OC_SECRET_WARP_OUT */
    { 0x5805,  68,  60 },  /* NA_SE_OC_SECRET_HOLE_OUT */
    { 0x5806,  46,  64 },  /* NA_SE_OC_REVENGE */
    { 0x5807,  52,  39 },  /* NA_SE_OC_HINT_MOVIE */
};

static const EffectNote *find_effect_note( uint16_t sfxId )
{
    size_t lo = 0, hi = sizeof( s_effectNotes ) / sizeof( s_effectNotes[0] );
    while( lo < hi ) {
        size_t mid = lo + ( hi - lo ) / 2;
        if( s_effectNotes[mid].sfxId == sfxId ) return &s_effectNotes[mid];
        if( s_effectNotes[mid].sfxId < sfxId ) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static bool effect_note_sample( uint8_t instrument, uint8_t semitone, const int16_t **pcm,
                                uint32_t *numSamples, uint32_t *sampleRate )
{
    LibootAudioSampleView view;
    if( !liboot_audio_instrument_sample_get( 0, instrument, semitone, &view )) return false;
    *pcm = view.pcm;
    *numSamples = view.numSamples;
    *sampleRate = view.sampleRate;
    return true;
}

/* ---- ocarina (liboot v0.5) ---------------------------------------------
 *
 * NA_SE_OC_OCARINA routes to a seq_0 channel script that plays font-0
 * INSTRUMENT 52 (the default ocarina) at PITCH_C4 (semitone 39) transposed
 * by the button's OcarinaPitch, so the five playable notes are single
 * semitones on one shared C4-authored sample:
 *     A = D4 (39+2), C-down = F4 (+5), C-right = A4 (+9),
 *     C-left = B4 (+11), C-up = D5 (+14).
 * One decode serves all five notes (the game itself plays one sample at five
 * rates, and pre-resampling would put the loop seam on fractional sample
 * positions); the per-note playback rate follows the engine's 12-TET rule
 * rate = 32000 * tuning * 2^((semitone-39)/12). The sample loops forever
 * (loopCount 0xFFFFFFFF), so decode_sample_at already cut the PCM at the
 * loop end: numSamples == loopEnd, and sustaining a held note is just
 * repeating [loopStart, numSamples). */
#define OCARINA_INSTRUMENT 52u

static const uint8_t sOcarinaSemi[5] = { 41, 44, 48, 50, 53 }; /* D4 F4 A4 B4 D5 */

bool oot_get_ocarina_note( uint8_t noteIndex, const int16_t **pcm, uint32_t *numSamples,
                           uint32_t *sampleRate, uint32_t *loopStart )
{
    if( pcm ) *pcm = NULL;
    if( numSamples ) *numSamples = 0;
    if( sampleRate ) *sampleRate = 0;
    if( loopStart ) *loopStart = 0;
    if( noteIndex >= 5 || !pcm || !numSamples || !sampleRate || !loopStart || !s_audio.ready )
        return false;
    if( !effect_note_sample( OCARINA_INSTRUMENT, sOcarinaSemi[noteIndex],
                             pcm, numSamples, sampleRate ))
        return false;

    /* Loop start from the shared sample's own header. Instrument 52 covers
       the full range with its NORMAL TunedSample only (rangeLo 0 / rangeHi
       127), the same slot effect_note_sample just resolved. */
    const uint8_t *font = s_audio.fontData + s_audio.font0Offset;
    uint32_t instrOff = be32( font + 8 + (size_t)OCARINA_INSTRUMENT * 4 );
    if( instrOff != 0 && span_ok( s_audio.font0Size, instrOff, 32 )) {
        uint32_t sampleOff = be32( font + instrOff + 16 );
        if( sampleOff != 0 && span_ok( s_audio.font0Size, sampleOff, 16 )) {
            uint32_t loopOff = be32( font + sampleOff + 8 );
            if( span_ok( s_audio.font0Size, loopOff, 8 )) {
                uint32_t start = be32( font + loopOff );
                if( start < *numSamples ) *loopStart = start;
            }
        }
    }
    return true;
}

/* Public API: serves the ROM's own PCM for a sfx id — Link's voice bank
   (0x6800..) and, via the instrument mapping above, the general effect
   banks (footsteps, sword whooshes, shield, damage, rolls, ...). */
bool oot_get_voice_sample( uint16_t sfxId, const int16_t **pcm,
                           uint32_t *numSamples, uint32_t *sampleRate )
{
    if( pcm ) *pcm = NULL;
    if( numSamples ) *numSamples = 0;
    if( sampleRate ) *sampleRate = 0;
    if( !pcm || !numSamples || !sampleRate || !s_audio.ready || sfxId == 0 ) return false;

    /* Continuous sounds are refreshed with SFX_FLAG subtracted. Restore the
       canonical table id before looking up either general effects or voices. */
    uint16_t canonicalId = sfxId | 0x0800u;
    if( canonicalId < 0x6800u || canonicalId > 0x684Fu ) {
        const EffectNote *note = find_effect_note( canonicalId );
        return note && effect_note_sample( note->instrument, note->semitone,
                                           pcm, numSamples, sampleRate );
    }
    uint16_t voice = canonicalId - 0x6800u;
    if( voice >= LINK_VOICE_COUNT ) return false;
    const LinkVoiceMap *map = &s_linkVoice[voice];
    if( map->kind == VOICE_NONE || map->count == 0 ) return false;
    const DecodedSample *clip = NULL;

    if( map->kind == VOICE_SEQUENCE ) {
        DecodedSample *seq = &s_audio.voiceSequences[voice];
        if( !seq->attempted ) {
            seq->attempted = true;
            build_sequence( map, seq );
        }
        if( seq->pcm ) clip = seq;
    } else {
        uint8_t slot = 0;
        if( map->count > 1 ) {
            uint8_t previous = s_audio.lastPick[voice];
            if( map->kind == VOICE_ALTERNATE ) {
                slot = previous < map->count ? (uint8_t)(( previous + 1 ) % map->count ) : 0;
            } else if( previous < map->count ) {
                slot = (uint8_t)( voice_random() % ( map->count - 1u ));
                if( slot >= previous ) ++slot;
            } else {
                slot = (uint8_t)( voice_random() % map->count );
            }
        }
        /* If the selected variant fails to decode, fall back to any that works. */
        for( uint8_t tries = 0; tries < map->count; ++tries ) {
            uint8_t candidate = (uint8_t)(( slot + tries ) % map->count );
            if( decode_effect( map->effect[candidate] )) {
                s_audio.lastPick[voice] = candidate;
                clip = &s_audio.decoded[map->effect[candidate]];
                break;
            }
        }
    }
    if( !clip ) return false;
    *pcm = clip->pcm;
    *numSamples = clip->numSamples;
    *sampleRate = clip->sampleRate;
    return true;
}
