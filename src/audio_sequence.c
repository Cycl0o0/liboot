/* SPDX-License-Identifier: AGPL-3.0-or-later
 * ROM-backed Zelda AudioSeq interpreter and software mixer.
 *
 * This is deliberately independent from an N64 audio device/RSP.  It keeps
 * the retail three-level bytecode model (sequence -> channel -> layer), uses
 * the ROM's own soundfonts and VADPCM samples, and exposes a pull mixer that a
 * host can feed directly to SDL, WASAPI, CoreAudio, etc.
 */
#include "liboot.h"
#include "audio_extract.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SEQ_PLAYERS       OOT_AUDIO_PLAYER_COUNT
#define SEQ_CHANNELS      16
#define SEQ_LAYERS        4
#define SEQ_IO_PORTS      8
#define SEQ_STACK_DEPTH   4
#define SEQ_DATA_MAX      65536u
#define AUDIO_VOICES      96
#define SCRIPT_BUDGET     1024
#define REVERB_FRAMES     8192u
#define SEQTICKS_PER_BEAT 48.0
#define DEFAULT_TEMPO     120.0
#define SEQ_IO_NONE       (-1)

/* PAL OoT runs four synthesis updates per 50 Hz video frame.  ADSR state is
   advanced once per synthesis update, then the ABI envelope mixer ramps to
   the new target over that update's PCM chunk. */
#define RETAIL_ADSR_TICKS_PER_SECOND 200.0
#define RETAIL_ADSR_TICKS_PER_UPDATE 4.0f
#define RETAIL_MAX_TEMPO_PAL         11981u
/* 32000 * PAL unk_2870, where aiFrequency=31995, ticksPerUpdate=4 and
   maxTempo=11981. The sample view already folds its tuning/pitch into a
   32000-Hz rate, so natural-note timing uses this equivalent scale. */
#define RETAIL_NATURAL_DELAY_SCALE_PAL 0.01669570431113243

#define PORTAMENTO_MODE(mode) ((mode) & 0x7Fu)
#define PORTAMENTO_SPECIAL(mode) ((mode) & 0x80u)

#define FLOW_CONTINUE 0
#define FLOW_END     (-1)
#define FLOW_ERROR   (-2)

typedef struct {
    uint32_t pc;
    uint32_t stack[SEQ_STACK_DEPTH];
    uint8_t loop[SEQ_STACK_DEPTH];
    uint8_t depth;
    int16_t value;
    uint8_t valid;
} Script;

typedef struct {
    const uint8_t *envelope;       /* big-endian EnvelopePoint array */
    size_t envelopeSize;
    uint8_t decayIndex;
    uint8_t sustain;
} AdsrSettings;

typedef struct {
    uint16_t cur, speed;
    float extent;
    uint8_t mode;
} PortamentoState;

typedef struct {
    Script script;
    uint16_t delay, lastDelay, shortDelay, gateDelay;
    uint8_t enabled, finished, muted, legato, ignoreDrumPan;
    uint8_t gate, velocity, pan;
    uint8_t portamentoTarget;
    int8_t transpose;
    int16_t instrument;            /* -1 inherits channel */
    float bend, velocitySquare;
    uint16_t portamentoTime;
    int voice;
    uint32_t voiceSerial;
    AdsrSettings adsr;
    PortamentoState portamento;
} SeqLayer;

typedef struct {
    Script script;
    SeqLayer layers[SEQ_LAYERS];
    int16_t io[SEQ_IO_PORTS];
    uint32_t dynTable;
    uint32_t ptr;
    uint16_t delay;
    uint8_t enabled, finished, stopScript, largeNotes;
    uint8_t font, instrument, hasInstrument;
    uint8_t volume, volumeScale, pan, panWeight;
    uint8_t reverb;
    uint8_t velocityRandom, gateRandom;
    int8_t transpose;
    float freqScale;
    AdsrSettings adsr;
} SeqChannel;

typedef struct {
    uint8_t data[SEQ_DATA_MAX];
    uint32_t dataSize;
    Script script;
    SeqChannel channels[SEQ_CHANNELS];
    int16_t io[SEQ_IO_PORTS];
    uint8_t fonts[8], fontCount, defaultFont;
    uint8_t index, playing, paused, finished, stopScript;
    uint8_t pendingStop;
    uint16_t sequenceId, resolvedId, delay;
    int8_t transpose;
    uint8_t shortVel[16], shortGate[16];
    uint32_t shortVelOff, shortGateOff;
    float tempo, tempoChange, seqVolume, userVolume;
    float fadeGain, fadeTarget, fadeSeconds;
    double samplesUntilTick;
    uint64_t framesRendered;
} SeqPlayer;

typedef struct {
    const int16_t *pcm;
    const uint8_t *tunedSample;
    uint32_t numSamples, loopStart, loopCount;
    double sourceRate;
    double position;
    const uint8_t *adsrEnvelope;
    size_t adsrEnvelopeSize;
    double adsrSamplesUntilTick;
    double portamentoSamplesUntilTick;
    float env, envTarget, envStep;
    float adsrCurrent, adsrTarget, adsrVelocity, adsrFadeOutVel, adsrSustain;
    float volume, pan, reverb;
    float oscPhase, oscStep;
    float portamentoScale;
    int16_t adsrDelay;
    uint16_t sfxId;
    uint8_t active, releasing, oscillator, wave;
    uint8_t adsrState, adsrEnvIndex, adsrDecay, adsrRelease;
    uint8_t adsrDisableAfterRamp;
    PortamentoState portamento;
    uint8_t player, channel, layer;
    uint32_t serial;
} AudioVoice;

enum {
    VOICE_ADSR_DISABLED,
    VOICE_ADSR_INITIAL,
    VOICE_ADSR_START_LOOP,
    VOICE_ADSR_LOOP,
    VOICE_ADSR_FADE,
    VOICE_ADSR_HANG,
    VOICE_ADSR_DECAY,
    VOICE_ADSR_RELEASE,
    VOICE_ADSR_SUSTAIN
};

static const uint8_t s_defaultEnvelope[] = {
    0x00, 0x01, 0x7D, 0x00, /* { 1, 32000 } */
    0x03, 0xE8, 0x7D, 0x00, /* { 1000, 32000 } */
    0xFF, 0xFF, 0x00, 0x00, /* ADSR_HANG */
    0x00, 0x00, 0x00, 0x00  /* ADSR_DISABLE */
};

static const uint8_t s_defaultShortVelocity[16] = {
    12, 25, 38, 51, 57, 64, 71, 76, 83, 89, 96, 102, 109, 115, 121, 127
};

static const uint8_t s_defaultShortGate[16] = {
    229, 203, 177, 151, 139, 126, 113, 100, 87, 74, 61, 48, 36, 23, 10, 0
};

static SeqPlayer s_players[SEQ_PLAYERS];
static AudioVoice s_voices[AUDIO_VOICES];
static float s_reverb[REVERB_FRAMES * 2u];
static uint32_t s_reverbPos, s_voiceSerial = 1, s_random = 0x12345678u;
static float s_masterVolume = 0.85f;
static uint8_t s_sfxRotation[7];
static uint16_t s_sfxChannelId[SEQ_CHANNELS];
static float s_sfxChannelPan[SEQ_CHANNELS];

static void sfx_channel_state_reset( void )
{
    memset( s_sfxChannelId, 0, sizeof( s_sfxChannelId ));
    for( int i = 0; i < SEQ_CHANNELS; ++i ) s_sfxChannelPan[i] = 0.5f;
}

typedef struct {
    uint8_t channel, port, value;
} NatureIo;

typedef struct {
    uint16_t playerIo, channelMask;
    const NatureIo *channelIo;
    uint8_t channelIoCount;
} NaturePreset;

#define NIO(channel, port, value) { (channel), (port), (value) }
#define NATURE_COUNT(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

/* Retail sNatureAmbienceDataIO, expressed as compact triples.  Sequence 1 is
   a programmable ambience engine: scene ids select these IO programs rather
   than separate Audioseq entries. */
static const NatureIo s_natureGeneralNight[] = {
    NIO(0,2,0), NIO(0,3,0),
    NIO(1,2,9), NIO(1,3,64), NIO(1,4,0), NIO(1,5,32),
    NIO(2,2,4), NIO(2,3,0), NIO(2,4,1), NIO(2,5,16),
    NIO(3,2,10), NIO(3,3,112), NIO(3,4,1), NIO(3,5,48),
    NIO(4,2,14), NIO(4,3,127), NIO(4,4,0), NIO(4,5,16),
    NIO(5,2,0), NIO(5,3,127), NIO(5,4,1), NIO(5,5,16),
    NIO(6,2,1), NIO(6,3,127), NIO(6,4,3), NIO(6,5,16),
    NIO(7,2,17), NIO(7,3,127), NIO(7,4,1), NIO(7,5,16),
};

static const NatureIo s_natureMarketEntrance[] = {
    NIO(0,2,0), NIO(0,3,0),
    NIO(1,2,4), NIO(1,3,0), NIO(1,4,1), NIO(1,5,16),
    NIO(3,2,11), NIO(3,3,112), NIO(3,4,1), NIO(3,5,48),
    NIO(4,2,14), NIO(4,3,127), NIO(4,4,0), NIO(4,5,16),
    NIO(5,2,0), NIO(5,3,127), NIO(5,4,1), NIO(5,5,16),
    NIO(6,2,1), NIO(6,3,127), NIO(6,4,3), NIO(6,5,16),
    NIO(7,2,17), NIO(7,3,127), NIO(7,4,1), NIO(7,5,16),
};

static const NatureIo s_natureKakariko[] = {
    NIO(0,2,0), NIO(0,3,0),
    NIO(2,2,11), NIO(2,3,48), NIO(2,4,1), NIO(2,5,32),
};

static const NatureIo s_natureMarketRuins[] = {
    NIO(0,2,1), NIO(0,3,32),
    NIO(2,2,11), NIO(2,3,48), NIO(2,4,1), NIO(2,5,32),
};

static const NatureIo s_natureKokiri[] = {
    NIO(0,2,0), NIO(0,3,47),
    NIO(1,2,13), NIO(1,3,0), NIO(1,4,1), NIO(1,5,16),
    NIO(2,2,16), NIO(2,3,0), NIO(2,4,1), NIO(2,5,32),
    NIO(3,2,14), NIO(3,3,0), NIO(3,4,0), NIO(3,5,44),
    NIO(4,2,11), NIO(4,3,63), NIO(4,4,1), NIO(4,5,44),
};

static const NatureIo s_natureCommon[] = {
    NIO(0,2,0), NIO(0,3,0),
    NIO(1,2,4), NIO(1,3,0), NIO(1,4,1), NIO(1,5,16),
};

static const NatureIo s_natureGanonsLair[] = {
    NIO(0,2,1), NIO(0,3,32),
};

static const NatureIo s_natureWasteland[] = {
    NIO(0,2,2), NIO(0,3,0), NIO(0,4,0),
};

static const NatureIo s_natureColossus[] = {
    NIO(0,2,2), NIO(0,3,0), NIO(0,4,0),
    NIO(1,2,10), NIO(1,3,64), NIO(1,4,0), NIO(1,5,32),
    NIO(2,2,15), NIO(2,3,112), NIO(2,4,1), NIO(2,5,48),
    NIO(3,2,14), NIO(3,3,127), NIO(3,4,0), NIO(3,5,16),
    NIO(5,2,4), NIO(5,3,127), NIO(5,4,0), NIO(5,5,16),
};

static const NatureIo s_natureDeathMountain[] = {
    NIO(0,2,0), NIO(0,3,0), NIO(0,4,0),
    NIO(1,2,10), NIO(1,3,64), NIO(1,4,0), NIO(1,5,32),
    NIO(2,2,11), NIO(2,3,112), NIO(2,4,1), NIO(2,5,48),
    NIO(3,2,12), NIO(3,3,127), NIO(3,4,0), NIO(3,5,16),
    NIO(4,2,6), NIO(4,3,0), NIO(4,4,0), NIO(4,5,16),
    NIO(5,2,0), NIO(5,3,0), NIO(5,4,0), NIO(5,5,16),
    NIO(6,2,1), NIO(6,3,0), NIO(6,4,0), NIO(6,5,16),
};

static const NatureIo s_naturePreset0F[] = {
    NIO(0,2,0), NIO(0,3,0),
    NIO(1,2,0), NIO(1,3,80), NIO(1,4,1), NIO(1,5,8),
    NIO(2,2,10), NIO(2,3,80), NIO(2,4,1), NIO(2,5,48),
    NIO(3,2,6), NIO(3,3,0), NIO(3,4,0), NIO(3,5,0),
    NIO(4,2,11), NIO(4,3,96), NIO(4,4,0), NIO(4,5,32),
};

#define NATURE_PRESET(player, mask, io) \
    { (player), (mask), (io), NATURE_COUNT(io) }
static const NaturePreset s_naturePresets[OOT_AUDIO_NATURE_COUNT] = {
    NATURE_PRESET(0xC0FF, 0xC0FE, s_natureGeneralNight),
    NATURE_PRESET(0xC0FB, 0xC0FA, s_natureMarketEntrance),
    NATURE_PRESET(0xC001, 0x4000, s_natureKakariko),
    NATURE_PRESET(0xC005, 0x4000, s_natureMarketRuins),
    NATURE_PRESET(0xC01F, 0xC000, s_natureKokiri),
    NATURE_PRESET(0xC003, 0xC000, s_natureCommon),
    NATURE_PRESET(0xC0FB, 0xC0FA, s_natureMarketEntrance),
    NATURE_PRESET(0x8001, 0x0000, s_natureGanonsLair),
    NATURE_PRESET(0xC003, 0xC000, s_natureCommon),
    NATURE_PRESET(0xC003, 0xC000, s_natureCommon),
    NATURE_PRESET(0xC001, 0xC000, s_natureWasteland),
    NATURE_PRESET(0xC02F, 0xC02E, s_natureColossus),
    NATURE_PRESET(0xC07F, 0xC07E, s_natureDeathMountain),
    NATURE_PRESET(0xC003, 0xC000, s_natureCommon),
    NATURE_PRESET(0xC003, 0xC000, s_natureCommon),
    NATURE_PRESET(0xC01F, 0xC000, s_naturePreset0F),
    NATURE_PRESET(0xC003, 0xC000, s_natureCommon),
    NATURE_PRESET(0xC003, 0xC000, s_natureCommon),
    NATURE_PRESET(0xC003, 0xC000, s_natureCommon),
};
#undef NATURE_PRESET
#undef NATURE_COUNT
#undef NIO

static const char *const s_sequenceNames[OOT_AUDIO_SEQUENCE_COUNT] = {
    "GENERAL_SFX", "NATURE_AMBIENCE", "FIELD_LOGIC", "FIELD_INIT",
    "FIELD_DEFAULT_1", "FIELD_DEFAULT_2", "FIELD_DEFAULT_3", "FIELD_DEFAULT_4",
    "FIELD_DEFAULT_5", "FIELD_DEFAULT_6", "FIELD_DEFAULT_7", "FIELD_DEFAULT_8",
    "FIELD_DEFAULT_9", "FIELD_DEFAULT_A", "FIELD_DEFAULT_B", "FIELD_ENEMY_INIT",
    "FIELD_ENEMY_1", "FIELD_ENEMY_2", "FIELD_ENEMY_3", "FIELD_ENEMY_4",
    "FIELD_STILL_1", "FIELD_STILL_2", "FIELD_STILL_3", "FIELD_STILL_4",
    "DUNGEON", "KAKARIKO_ADULT", "ENEMY", "BOSS", "INSIDE_DEKU_TREE",
    "MARKET", "TITLE", "LINK_HOUSE", "GAME_OVER", "BOSS_CLEAR", "ITEM_GET",
    "OPENING_GANON", "HEART_GET", "OCA_LIGHT", "JABU_JABU", "KAKARIKO_KID",
    "GREAT_FAIRY", "ZELDA_THEME", "FIRE_TEMPLE", "OPEN_TRE_BOX", "FOREST_TEMPLE",
    "COURTYARD", "GANON_TOWER", "LONLON", "GORON_CITY", "FIELD_MORNING",
    "SPIRITUAL_STONE", "OCA_BOLERO", "OCA_MINUET", "OCA_SERENADE", "OCA_REQUIEM",
    "OCA_NOCTURNE", "MINI_BOSS", "SMALL_ITEM_GET", "TEMPLE_OF_TIME", "EVENT_CLEAR",
    "KOKIRI", "OCA_FAIRY_GET", "SARIA_THEME", "SPIRIT_TEMPLE", "HORSE", "HORSE_GOAL",
    "INGO", "MEDALLION_GET", "OCA_SARIA", "OCA_EPONA", "OCA_ZELDA", "OCA_SUNS",
    "OCA_TIME", "OCA_STORM", "NAVI_OPENING", "DEKU_TREE_CS", "WINDMILL", "HYRULE_CS",
    "MINI_GAME", "SHEIK", "ZORA_DOMAIN", "APPEAR", "ADULT_LINK", "MASTER_SWORD",
    "INTRO_GANON", "SHOP", "CHAMBER_OF_SAGES", "FILE_SELECT", "ICE_CAVERN",
    "DOOR_OF_TIME", "OWL", "SHADOW_TEMPLE", "WATER_TEMPLE", "BRIDGE_TO_GANONS",
    "SEAL_OF_SAGES", "GERUDO_VALLEY", "POTION_SHOP", "KOTAKE_KOUME", "ESCAPE",
    "UNDERGROUND", "GANONDORF_BOSS", "GANON_BOSS", "OCARINA_OF_TIME", "STAFF_1",
    "STAFF_2", "STAFF_3", "STAFF_4", "FIRE_BOSS", "TIMED_MINI_GAME", "CUTSCENE_EFFECTS"
};

#define DEFINE_SFX(_chan, name, _importance, _dist, _rand, _flags) #name,
static const char *const s_sfxPlayerNames[] = {
#include "decomp/include/tables/sfx/playerbank_table.h"
};
static const char *const s_sfxItemNames[] = {
#include "decomp/include/tables/sfx/itembank_table.h"
};
static const char *const s_sfxEnvironmentNames[] = {
#include "decomp/include/tables/sfx/environmentbank_table.h"
};
static const char *const s_sfxEnemyNames[] = {
#include "decomp/include/tables/sfx/enemybank_table.h"
};
static const char *const s_sfxSystemNames[] = {
#include "decomp/include/tables/sfx/systembank_table.h"
};
static const char *const s_sfxOcarinaNames[] = {
#include "decomp/include/tables/sfx/ocarinabank_table.h"
};
static const char *const s_sfxVoiceNames[] = {
#include "decomp/include/tables/sfx/voicebank_table.h"
};
#undef DEFINE_SFX

typedef struct {
    const char *const *names;
    uint16_t count, base;
} SfxCatalogBank;

#define COUNT_OF(a) ((uint16_t)(sizeof(a) / sizeof((a)[0])))
static const SfxCatalogBank s_sfxBanks[7] = {
    { s_sfxPlayerNames,      COUNT_OF(s_sfxPlayerNames),      0x0800 },
    { s_sfxItemNames,        COUNT_OF(s_sfxItemNames),        0x1800 },
    { s_sfxEnvironmentNames, COUNT_OF(s_sfxEnvironmentNames), 0x2800 },
    { s_sfxEnemyNames,       COUNT_OF(s_sfxEnemyNames),       0x3800 },
    { s_sfxSystemNames,      COUNT_OF(s_sfxSystemNames),      0x4800 },
    { s_sfxOcarinaNames,     COUNT_OF(s_sfxOcarinaNames),     0x5800 },
    { s_sfxVoiceNames,       COUNT_OF(s_sfxVoiceNames),       0x6800 },
};

static float clamp01( float v )
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static uint32_t next_random( void )
{
    s_random = s_random * 1664525u + 1013904223u;
    return s_random;
}

static int sfx_bank_for_id( uint16_t id, uint16_t *index )
{
    uint16_t canonical = id | 0x0800u;
    for( int bank = 0; bank < 7; ++bank ) {
        const SfxCatalogBank *b = &s_sfxBanks[bank];
        if( canonical >= b->base && canonical - b->base < b->count ) {
            if( index ) *index = (uint16_t)( canonical - b->base );
            return bank;
        }
    }
    return -1;
}

int32_t oot_audio_sequence_count( void )
{
    return liboot_audio_sequence_count();
}

const char *oot_audio_sequence_name( uint16_t sequenceId )
{
    if( sequenceId == OOT_AUDIO_NO_MUSIC ) return "NO_MUSIC";
    if( sequenceId == OOT_AUDIO_NATURE_RAIN ) return "NATURE_SFX_RAIN";
    return sequenceId < OOT_AUDIO_SEQUENCE_COUNT ? s_sequenceNames[sequenceId] : "UNKNOWN";
}

bool oot_audio_sequence_get_info( uint16_t sequenceId, struct OoTSequenceInfo *outInfo )
{
    if( !outInfo || outInfo->structSize < sizeof( *outInfo ) ||
        outInfo->version != OOT_SEQUENCE_INFO_VERSION ) return false;
    uint32_t size = outInfo->structSize, version = outInfo->version;
    memset( outInfo, 0, sizeof( *outInfo ));
    outInfo->structSize = size;
    outInfo->version = version;
    memset( outInfo->fontIds, 0xFF, sizeof( outInfo->fontIds ));

    LibootAudioSequenceView view;
    if( !liboot_audio_sequence_get( sequenceId, &view )) return false;
    outInfo->sequenceId = view.requestedId;
    outInfo->resolvedId = view.resolvedId;
    outInfo->dataSize = view.size > UINT32_MAX ? UINT32_MAX : (uint32_t)view.size;
    outInfo->fontCount = view.numFonts > sizeof( outInfo->fontIds ) ?
                         (uint8_t)sizeof( outInfo->fontIds ) : view.numFonts;
    outInfo->isAlias = view.requestedId != view.resolvedId;
    outInfo->medium = 2; /* retail Audioseq is cartridge-backed */
    outInfo->cachePolicy = view.cachePolicy;
    if( outInfo->fontCount ) memcpy( outInfo->fontIds, view.fontIds, outInfo->fontCount );
    return true;
}

static bool prewarm_sequence_reachable( uint16_t sequenceId )
{
    if( sequenceId >= OOT_AUDIO_SEQUENCE_COUNT ||
        !liboot_audio_prewarm_sequence( sequenceId )) return false;
    /* FIELD_LOGIC copies one of 3..23 into its mutable program with LDSEQ. */
    if( sequenceId == 2 ) {
        bool ok = true;
        for( uint16_t child = 3; child <= 23; ++child )
            ok = liboot_audio_prewarm_sequence( child ) && ok;
        return ok;
    }
    return true;
}

bool oot_audio_sequence_prewarm( uint16_t sequenceId )
{
    return prewarm_sequence_reachable( sequenceId );
}

int32_t oot_audio_sfx_catalog_count( void )
{
    int32_t count = 0;
    for( int bank = 0; bank < 7; ++bank ) count += s_sfxBanks[bank].count;
    return count;
}

bool oot_audio_sfx_catalog_get( int32_t catalogIndex, struct OoTSfxInfo *outInfo )
{
    if( !outInfo || outInfo->structSize < sizeof( *outInfo ) ||
        outInfo->version != OOT_SFX_INFO_VERSION || catalogIndex < 0 ) return false;
    uint32_t size = outInfo->structSize, version = outInfo->version;
    for( int bank = 0; bank < 7; ++bank ) {
        const SfxCatalogBank *b = &s_sfxBanks[bank];
        if( catalogIndex < b->count ) {
            const char *name = b->names[catalogIndex];
            memset( outInfo, 0, sizeof( *outInfo ));
            outInfo->structSize = size;
            outInfo->version = version;
            outInfo->sfxId = (uint16_t)( b->base + catalogIndex );
            outInfo->bank = (uint8_t)bank;
            outInfo->bankIndex = (uint16_t)catalogIndex;
            if( name ) {
                strncpy( outInfo->name, name, sizeof( outInfo->name ) - 1 );
                outInfo->name[sizeof( outInfo->name ) - 1] = '\0';
            }
            return true;
        }
        catalogIndex -= b->count;
    }
    return false;
}

static void script_start( Script *s, uint32_t pc )
{
    memset( s, 0, sizeof( *s ));
    s->pc = pc;
    s->value = 0;
    s->valid = 1;
}

static uint8_t read_u8( SeqPlayer *p, Script *s )
{
    if( !s->valid || s->pc >= p->dataSize ) {
        s->valid = 0;
        return 0;
    }
    return p->data[s->pc++];
}

static int16_t read_s16( SeqPlayer *p, Script *s )
{
    uint8_t hi = read_u8( p, s );
    uint8_t lo = read_u8( p, s );
    return (int16_t)((uint16_t)hi << 8 | lo);
}

static uint16_t read_var( SeqPlayer *p, Script *s )
{
    uint16_t value = read_u8( p, s );
    if( value & 0x80u ) value = (uint16_t)((( value << 8 ) & 0x7F00u ) | read_u8( p, s ));
    return value;
}

static bool script_jump( SeqPlayer *p, Script *s, int32_t offset )
{
    if( offset < 0 || (uint32_t)offset >= p->dataSize ) {
        s->valid = 0;
        return false;
    }
    s->pc = (uint32_t)offset;
    return true;
}

/* Shared AudioSeq control-flow opcodes.  A positive result is a script delay;
   FLOW_END terminates this script, FLOW_ERROR rejects malformed bytecode. */
static int script_flow( SeqPlayer *p, Script *s, uint8_t cmd )
{
    int16_t absolute;
    int8_t relative;
    switch( cmd ) {
    case 0xFF: /* end / return */
        if( s->depth == 0 ) return FLOW_END;
        s->pc = s->stack[--s->depth];
        return FLOW_CONTINUE;
    case 0xFE: return 1;
    case 0xFD: {
        uint16_t delay = read_var( p, s );
        return s->valid ? ( delay ? delay : 1 ) : FLOW_ERROR;
    }
    case 0xFC: /* call absolute */
        absolute = read_s16( p, s );
        if( !s->valid || s->depth >= SEQ_STACK_DEPTH ) return FLOW_ERROR;
        s->stack[s->depth++] = s->pc;
        return script_jump( p, s, (uint16_t)absolute ) ? FLOW_CONTINUE : FLOW_ERROR;
    case 0xFB: /* jump absolute */
        absolute = read_s16( p, s );
        return script_jump( p, s, (uint16_t)absolute ) ? FLOW_CONTINUE : FLOW_ERROR;
    case 0xFA: /* beqz */
    case 0xF9: /* bltz */
    case 0xF5: /* bgez */
        absolute = read_s16( p, s );
        if( !s->valid ) return FLOW_ERROR;
        if(( cmd == 0xFA && s->value == 0 ) || ( cmd == 0xF9 && s->value < 0 ) ||
           ( cmd == 0xF5 && s->value >= 0 ))
            return script_jump( p, s, (uint16_t)absolute ) ? FLOW_CONTINUE : FLOW_ERROR;
        return FLOW_CONTINUE;
    case 0xF8: /* loop */
        if( s->depth >= SEQ_STACK_DEPTH ) return FLOW_ERROR;
        s->loop[s->depth] = read_u8( p, s );
        if( !s->valid ) return FLOW_ERROR;
        s->stack[s->depth++] = s->pc;
        return FLOW_CONTINUE;
    case 0xF7: /* loopend */
        if( s->depth == 0 ) return FLOW_ERROR;
        if( --s->loop[s->depth - 1] != 0 ) s->pc = s->stack[s->depth - 1];
        else --s->depth;
        return FLOW_CONTINUE;
    case 0xF6: /* break */
        if( s->depth == 0 ) return FLOW_ERROR;
        --s->depth;
        return FLOW_CONTINUE;
    case 0xF4: /* relative jump */
    case 0xF3: /* relative beqz */
    case 0xF2: /* relative bltz */
        relative = (int8_t)read_u8( p, s );
        if( !s->valid ) return FLOW_ERROR;
        if( cmd == 0xF4 || ( cmd == 0xF3 && s->value == 0 ) ||
            ( cmd == 0xF2 && s->value < 0 ))
            return script_jump( p, s, (int32_t)s->pc + relative ) ? FLOW_CONTINUE : FLOW_ERROR;
        return FLOW_CONTINUE;
    default:
        return FLOW_ERROR;
    }
}

static bool voice_matches_layer( const SeqLayer *layer )
{
    return layer->voice >= 0 && layer->voice < AUDIO_VOICES &&
           s_voices[layer->voice].active &&
           s_voices[layer->voice].serial == layer->voiceSerial;
}

static int16_t adsr_be16( const uint8_t *data )
{
    return (int16_t)((uint16_t)data[0] << 8 | data[1]);
}

static float retail_adsr_decay_rate( uint8_t index )
{
    float scaleInv;
    if( index == 0 ) return 0.0f;
    if( index == 255 ) scaleInv = 0.25f;
    else if( index == 254 ) scaleInv = 0.33f;
    else if( index == 253 ) scaleInv = 0.5f;
    else if( index == 252 ) scaleInv = 0.66f;
    else if( index == 251 ) scaleInv = 0.75f;
    else if( index >= 128 ) scaleInv = (float)( 251 - index );
    else if( index >= 16 ) scaleInv = (float)( 4 * ( 143 - index ));
    else scaleInv = (float)( 60 * ( 23 - index ));
    return ( 1.0f / RETAIL_ADSR_TICKS_PER_UPDATE ) / scaleInv;
}

static void voice_adsr_init( AudioVoice *voice, const AdsrSettings *settings )
{
    voice->adsrEnvelope = settings && settings->envelope ?
                          settings->envelope : s_defaultEnvelope;
    voice->adsrEnvelopeSize = settings && settings->envelope ?
                              settings->envelopeSize : sizeof( s_defaultEnvelope );
    voice->adsrSamplesUntilTick = 0.0;
    voice->env = 0.0f;
    voice->envTarget = 0.0f;
    voice->envStep = 0.0f;
    voice->adsrCurrent = 0.0f;
    voice->adsrTarget = 0.0f;
    voice->adsrVelocity = 0.0f;
    voice->adsrFadeOutVel = 0.0f;
    voice->adsrSustain = 0.0f;
    voice->adsrDelay = 0;
    voice->adsrState = VOICE_ADSR_INITIAL;
    voice->adsrEnvIndex = 0;
    voice->adsrDecay = 0;
    voice->adsrRelease = 0;
    voice->adsrDisableAfterRamp = 0;
}

static bool voice_adsr_point( const AudioVoice *voice, uint8_t index,
                              int16_t *delay, int16_t *arg )
{
    size_t offset = (size_t)index * 4u;
    if( !voice->adsrEnvelope || offset > voice->adsrEnvelopeSize ||
        voice->adsrEnvelopeSize - offset < 4 ) return false;
    *delay = adsr_be16( voice->adsrEnvelope + offset );
    *arg = adsr_be16( voice->adsrEnvelope + offset + 2 );
    return true;
}

static float voice_adsr_update_tick( AudioVoice *voice )
{
    uint8_t state = voice->adsrState;
    if( state == VOICE_ADSR_DISABLED ) return 0.0f;

    if( state == VOICE_ADSR_INITIAL ) voice->adsrState = VOICE_ADSR_START_LOOP;
    if( voice->adsrState == VOICE_ADSR_START_LOOP ) {
        voice->adsrEnvIndex = 0;
        voice->adsrState = VOICE_ADSR_LOOP;
    }
    if( voice->adsrState == VOICE_ADSR_LOOP ) {
        for( int redirects = 0; redirects < 64; ++redirects ) {
            int16_t delay, arg;
            if( !voice_adsr_point( voice, voice->adsrEnvIndex, &delay, &arg )) {
                voice->adsrState = VOICE_ADSR_DISABLED;
                break;
            }
            voice->adsrDelay = delay;
            if( delay == 0 ) {
                voice->adsrState = VOICE_ADSR_DISABLED;
                break;
            }
            if( delay == -1 ) {
                voice->adsrState = VOICE_ADSR_HANG;
                break;
            }
            if( delay == -2 ) {
                voice->adsrEnvIndex = (uint8_t)arg;
                continue;
            }
            if( delay == -3 ) {
                voice->adsrState = VOICE_ADSR_INITIAL;
                break;
            }
            if( delay < 0 ) {
                voice->adsrState = VOICE_ADSR_DISABLED;
                break;
            }
            /* PAL has four synthesis ticks per update, so the retail
               ticksPerUpdateScaled multiplier is exactly one. */
            if( voice->adsrDelay == 0 ) voice->adsrDelay = 1;
            voice->adsrTarget = arg / 32767.0f;
            voice->adsrTarget *= voice->adsrTarget;
            voice->adsrVelocity =
                ( voice->adsrTarget - voice->adsrCurrent ) / voice->adsrDelay;
            voice->adsrState = VOICE_ADSR_FADE;
            ++voice->adsrEnvIndex;
            break;
        }
        if( voice->adsrState == VOICE_ADSR_LOOP )
            voice->adsrState = VOICE_ADSR_DISABLED;
    }

    state = voice->adsrState;
    if( state == VOICE_ADSR_FADE ) {
        voice->adsrCurrent += voice->adsrVelocity;
        --voice->adsrDelay;
        if( voice->adsrDelay <= 0 ) voice->adsrState = VOICE_ADSR_LOOP;
    } else if( state == VOICE_ADSR_DECAY || state == VOICE_ADSR_RELEASE ) {
        voice->adsrCurrent -= voice->adsrFadeOutVel;
        if( voice->adsrSustain != 0.0f && state == VOICE_ADSR_DECAY ) {
            if( voice->adsrCurrent < voice->adsrSustain ) {
                voice->adsrCurrent = voice->adsrSustain;
                voice->adsrDelay = 128;
                voice->adsrState = VOICE_ADSR_SUSTAIN;
            }
        } else if( voice->adsrCurrent < 0.00001f ) {
            voice->adsrCurrent = 0.0f;
            voice->adsrState = VOICE_ADSR_DISABLED;
        }
    } else if( state == VOICE_ADSR_SUSTAIN ) {
        --voice->adsrDelay;
        if( voice->adsrDelay == 0 ) voice->adsrState = VOICE_ADSR_RELEASE;
    }

    if( voice->adsrDecay ) {
        voice->adsrState = VOICE_ADSR_DECAY;
        voice->adsrDecay = 0;
    }
    if( voice->adsrRelease ) {
        voice->adsrState = VOICE_ADSR_RELEASE;
        voice->adsrRelease = 0;
    }
    return clamp01( voice->adsrCurrent );
}

static bool voice_adsr_sample( AudioVoice *voice, uint32_t sampleRate )
{
    if( voice->adsrSamplesUntilTick <= 0.0 ) {
        double interval = sampleRate / RETAIL_ADSR_TICKS_PER_SECOND;
        if( interval < 1.0 ) interval = 1.0;
        float target = voice_adsr_update_tick( voice );
        voice->envTarget = target;
        voice->envStep = ( target - voice->env ) / (float)interval;
        voice->adsrSamplesUntilTick += interval;
        voice->adsrDisableAfterRamp = voice->adsrState == VOICE_ADSR_DISABLED;
    }
    voice->env += voice->envStep;
    voice->adsrSamplesUntilTick -= 1.0;
    if( voice->adsrSamplesUntilTick <= 0.0 ) {
        voice->env = voice->envTarget;
        if( voice->adsrDisableAfterRamp ) {
            voice->active = 0;
            return false;
        }
    }
    return true;
}

static void voice_portamento_init( AudioVoice *voice,
                                   const PortamentoState *portamento )
{
    if( portamento ) voice->portamento = *portamento;
    else memset( &voice->portamento, 0, sizeof( voice->portamento ));
    voice->portamentoScale = 1.0f;
    voice->portamentoSamplesUntilTick = 0.0;
}

static float voice_portamento_update_tick( AudioVoice *voice )
{
    uint32_t cur = (uint32_t)voice->portamento.cur + voice->portamento.speed;
    voice->portamento.cur = (uint16_t)cur;
    uint32_t tableIndex = ( cur >> 8 ) & 0xFFu;
    if( tableIndex >= 127 ) {
        tableIndex = 127;
        voice->portamento.mode = 0;
    }
    /* gBendPitchOneOctaveFrequencies[128 + n] is 2^(n/127). */
    float bend = exp2f((float)tableIndex / 127.0f );
    return 1.0f + voice->portamento.extent * ( bend - 1.0f );
}

static void voice_portamento_sample( AudioVoice *voice, uint32_t sampleRate )
{
    if( voice->portamento.mode == 0 ) return;
    if( voice->portamentoSamplesUntilTick <= 0.0 ) {
        voice->portamentoScale = voice_portamento_update_tick( voice );
        voice->portamentoSamplesUntilTick +=
            sampleRate / RETAIL_ADSR_TICKS_PER_SECOND;
    }
    voice->portamentoSamplesUntilTick -= 1.0;
}

static void voice_decay( AudioVoice *voice, uint8_t decayIndex, uint8_t sustain )
{
    if( !voice->active || voice->adsrState == VOICE_ADSR_DECAY ) return;
    voice->releasing = 1;
    voice->adsrFadeOutVel = retail_adsr_decay_rate( decayIndex );
    voice->adsrSustain = sustain * voice->adsrCurrent / 256.0f;
    voice->adsrDecay = 1;
}

static void voice_release( AudioVoice *voice )
{
    if( !voice->active ) return;
    voice->releasing = 1;
    voice->adsrFadeOutVel = 1.0f / RETAIL_ADSR_TICKS_PER_UPDATE;
    voice->adsrSustain = 0.0f;
    voice->adsrRelease = 1;
}

static void layer_decay( SeqChannel *channel, SeqLayer *layer )
{
    if( voice_matches_layer( layer )) {
        uint8_t decayIndex = layer->adsr.decayIndex ?
                             layer->adsr.decayIndex : channel->adsr.decayIndex;
        voice_decay( &s_voices[layer->voice], decayIndex, channel->adsr.sustain );
    }
}

static void voices_stop_player( uint8_t player, bool release )
{
    for( int i = 0; i < AUDIO_VOICES; ++i ) {
        if( !s_voices[i].active || s_voices[i].player != player ) continue;
        if( release ) voice_release( &s_voices[i] );
        else s_voices[i].active = 0;
    }
}

static AudioVoice *voice_allocate( uint8_t player, uint8_t channel, uint8_t layer )
{
    int selected = -1;
    float quietest = 2.0f;
    for( int i = 0; i < AUDIO_VOICES; ++i ) {
        if( !s_voices[i].active ) { selected = i; break; }
        if( s_voices[i].releasing && s_voices[i].env < quietest ) {
            quietest = s_voices[i].env;
            selected = i;
        }
    }
    if( selected < 0 ) selected = (int)( s_voiceSerial % AUDIO_VOICES );
    AudioVoice *voice = &s_voices[selected];
    memset( voice, 0, sizeof( *voice ));
    voice->active = 1;
    voice->player = player;
    voice->channel = channel;
    voice->layer = layer;
    voice->serial = ++s_voiceSerial;
    if( s_voiceSerial == 0 ) s_voiceSerial = 1;
    return voice;
}

static int voice_index( const AudioVoice *voice )
{
    return (int)( voice - s_voices );
}

static const AdsrSettings *layer_effective_adsr( const SeqChannel *channel,
                                                 const SeqLayer *layer )
{
    return layer->adsr.decayIndex == 0 ? &channel->adsr : &layer->adsr;
}

static bool voice_start_sample( SeqPlayer *p, uint8_t channelIndex, uint8_t layerIndex,
                                SeqLayer *layer, const LibootAudioSampleView *sample,
                                float volume, float pan, float pitchScale, float reverb )
{
    if( !sample || !sample->pcm || sample->numSamples == 0 || sample->sampleRate == 0 ) return false;
    AudioVoice *voice = NULL;
    bool initializeAdsr = false;
    if( layer->legato && voice_matches_layer( layer )) {
        AudioVoice *current = &s_voices[layer->voice];
        if( !current->releasing && !current->oscillator &&
            current->tunedSample == sample->tunedSample ) {
            voice = current;
            voice->releasing = 0;
        }
    }
    if( !voice ) {
        layer_decay( &p->channels[channelIndex], layer );
        voice = voice_allocate( p->index, channelIndex, layerIndex );
        voice->loopCount = sample->loopCount;
        initializeAdsr = true;
    }
    voice->pcm = sample->pcm;
    voice->tunedSample = sample->tunedSample;
    voice->numSamples = sample->numSamples;
    voice->sourceRate = sample->sampleRate;
    voice->loopStart = sample->loopStart < sample->numSamples ? sample->loopStart : 0;
    voice->volume = clamp01( volume );
    voice->pan = clamp01( pan );
    voice->reverb = clamp01( reverb );
    voice->sfxId = p->index == OOT_AUDIO_PLAYER_SFX && channelIndex < SEQ_CHANNELS ?
                   s_sfxChannelId[channelIndex] : 0;
    if( pitchScale > 0.0f && isfinite( pitchScale ))
        voice->sourceRate = fmax( 1.0, voice->sourceRate * pitchScale );
    if( initializeAdsr )
        voice_adsr_init( voice, layer_effective_adsr( &p->channels[channelIndex], layer ));
    voice_portamento_init( voice, &layer->portamento );
    layer->voice = voice_index( voice );
    layer->voiceSerial = voice->serial;
    return true;
}

static void voice_start_wave( SeqPlayer *p, uint8_t channelIndex, uint8_t layerIndex,
                              SeqLayer *layer, uint8_t wave, int pitch,
                              float volume, float pan, float pitchScale, float reverb )
{
    AudioVoice *voice = NULL;
    bool initializeAdsr = false;
    if( layer->legato && voice_matches_layer( layer )) {
        AudioVoice *current = &s_voices[layer->voice];
        /* Retail synthetic waves have no TunedSample identity, so continuous
           notes reuse the voice even when the wave selector changes. */
        if( !current->releasing && current->oscillator ) {
            voice = current;
            voice->releasing = 0;
        }
    }
    if( !voice ) {
        layer_decay( &p->channels[channelIndex], layer );
        voice = voice_allocate( p->index, channelIndex, layerIndex );
        initializeAdsr = true;
    }
    voice->oscillator = 1;
    voice->wave = wave;
    voice->oscStep = 440.0f * exp2f(( pitch - 48 ) / 12.0f );
    if( pitchScale > 0.0f && isfinite( pitchScale )) voice->oscStep *= pitchScale;
    voice->volume = clamp01( volume );
    voice->pan = clamp01( pan );
    voice->reverb = clamp01( reverb );
    voice->sfxId = p->index == OOT_AUDIO_PLAYER_SFX && channelIndex < SEQ_CHANNELS ?
                   s_sfxChannelId[channelIndex] : 0;
    if( initializeAdsr )
        voice_adsr_init( voice, layer_effective_adsr( &p->channels[channelIndex], layer ));
    voice_portamento_init( voice, &layer->portamento );
    layer->voice = voice_index( voice );
    layer->voiceSerial = voice->serial;
}

static void layer_reset( SeqLayer *layer )
{
    memset( layer, 0, sizeof( *layer ));
    layer->finished = 1;
    layer->gate = 0x80;
    layer->velocity = 0x7F;
    layer->pan = 0x40;
    layer->instrument = -1;
    layer->bend = 1.0f;
    layer->velocitySquare = 1.0f;
    layer->voice = -1;
}

static void channel_reset( SeqChannel *channel, uint8_t defaultFont )
{
    memset( channel, 0, sizeof( *channel ));
    channel->finished = 1;
    channel->font = defaultFont;
    channel->volume = 0x7F;
    channel->volumeScale = 0x80;
    channel->pan = 0x40;
    channel->panWeight = 0x80;
    channel->freqScale = 1.0f;
    channel->adsr.envelope = s_defaultEnvelope;
    channel->adsr.envelopeSize = sizeof( s_defaultEnvelope );
    channel->adsr.decayIndex = 0xF0;
    channel->adsr.sustain = 0;
    for( int i = 0; i < SEQ_IO_PORTS; ++i ) channel->io[i] = SEQ_IO_NONE;
    for( int i = 0; i < SEQ_LAYERS; ++i ) layer_reset( &channel->layers[i] );
}

static void layer_enable( SeqPlayer *p, SeqChannel *channel, uint8_t index, uint32_t pc )
{
    if( index >= SEQ_LAYERS ) return;
    SeqLayer *layer = &channel->layers[index];
    layer_decay( channel, layer );
    layer_reset( layer );
    if( pc >= p->dataSize ) return;
    layer->enabled = 1;
    layer->finished = 0;
    layer->adsr = channel->adsr;
    layer->adsr.decayIndex = 0;
    script_start( &layer->script, pc );
}

static void channel_enable( SeqPlayer *p, uint8_t index, uint32_t pc )
{
    if( index >= SEQ_CHANNELS || pc >= p->dataSize ) return;
    SeqChannel *channel = &p->channels[index];
    /* Retail reuses the preallocated channel object. LDCHAN/RLDCHAN restarts
       only its script and layers; controllers, instrument, dynamic tables and
       externally queued IO remain intact. */
    for( int i = 0; i < SEQ_LAYERS; ++i ) {
        layer_decay( channel, &channel->layers[i] );
        layer_reset( &channel->layers[i] );
    }
    channel->enabled = 1;
    channel->finished = 0;
    channel->delay = 0;
    script_start( &channel->script, pc );
}

static void channel_disable_mode( SeqChannel *channel, bool forceRelease )
{
    for( int i = 0; i < SEQ_LAYERS; ++i ) {
        SeqLayer *layer = &channel->layers[i];
        if( forceRelease && voice_matches_layer( layer ))
            voice_release( &s_voices[layer->voice] );
        else
            layer_decay( channel, layer );
        layer->enabled = 0;
        layer->finished = 1;
    }
    channel->enabled = 0;
    channel->finished = 1;
}

static void channel_disable( SeqChannel *channel )
{
    channel_disable_mode( channel, false );
}

static bool player_load( uint8_t playerIndex, uint16_t sequenceId, uint16_t fadeMs,
                         bool prewarm )
{
    if( playerIndex >= SEQ_PLAYERS || sequenceId >= OOT_AUDIO_SEQUENCE_COUNT ) return false;
    LibootAudioSequenceView view;
    if( !liboot_audio_sequence_get( sequenceId, &view ) || !view.data ||
        view.size == 0 || view.size > SEQ_DATA_MAX ) return false;
    if( prewarm && !prewarm_sequence_reachable( sequenceId )) return false;

    SeqPlayer *p = &s_players[playerIndex];
    float userVolume = isfinite( p->userVolume ) ? p->userVolume : 1.0f;
    voices_stop_player( playerIndex, false );
    if( playerIndex == OOT_AUDIO_PLAYER_SFX ) sfx_channel_state_reset();
    memset( p, 0, sizeof( *p ));
    p->index = playerIndex;
    p->sequenceId = sequenceId;
    p->resolvedId = view.resolvedId;
    p->dataSize = (uint32_t)view.size;
    memcpy( p->data, view.data, view.size );
    p->fontCount = view.numFonts > sizeof( p->fonts ) ? (uint8_t)sizeof( p->fonts ) : view.numFonts;
    memset( p->fonts, 0xFF, sizeof( p->fonts ));
    if( p->fontCount ) memcpy( p->fonts, view.fontIds, p->fontCount );
    p->defaultFont = p->fontCount ? p->fonts[p->fontCount - 1] : 0;
    p->tempo = DEFAULT_TEMPO;
    p->seqVolume = 1.0f;
    p->userVolume = userVolume;
    p->fadeGain = fadeMs ? 0.0f : 1.0f;
    p->fadeTarget = 1.0f;
    p->fadeSeconds = fadeMs / 1000.0f;
    p->playing = 1;
    p->samplesUntilTick = 0.0;
    for( int i = 0; i < SEQ_IO_PORTS; ++i ) p->io[i] = SEQ_IO_NONE;
    memcpy( p->shortVel, s_defaultShortVelocity, sizeof( p->shortVel ));
    memcpy( p->shortGate, s_defaultShortGate, sizeof( p->shortGate ));
    for( int i = 0; i < SEQ_CHANNELS; ++i ) channel_reset( &p->channels[i], p->defaultFont );
    script_start( &p->script, 0 );
    return true;
}

bool oot_audio_sequence_play( uint8_t player, uint16_t sequenceId, uint16_t fadeInMs )
{
    if( player >= SEQ_PLAYERS ) return false;
    if( sequenceId == OOT_AUDIO_NO_MUSIC ) {
        oot_audio_sequence_stop( player, fadeInMs );
        return true;
    }
    return player_load( player, sequenceId, fadeInMs, true );
}

bool oot_audio_nature_play( uint8_t player, uint8_t ambienceId, uint16_t fadeInMs )
{
    if( player >= SEQ_PLAYERS || ambienceId >= OOT_AUDIO_NATURE_COUNT ) return false;
    if( !player_load( player, 1, fadeInMs, true )) return false;

    SeqPlayer *p = &s_players[player];
    const NaturePreset *preset = &s_naturePresets[ambienceId];
    p->io[0] = 1;
    p->io[4] = (int8_t)( preset->playerIo >> 8 );
    p->io[5] = (int8_t)( preset->playerIo & 0xFF );

    for( uint8_t channel = 0; channel < SEQ_CHANNELS; ++channel ) {
        uint16_t bit = (uint16_t)( 1u << channel );
        if(( preset->playerIo & bit ) && !( preset->channelMask & bit ))
            p->channels[channel].io[1] = 1;
    }
    for( uint8_t i = 0; i < preset->channelIoCount; ++i ) {
        const NatureIo *io = &preset->channelIo[i];
        p->channels[io->channel].io[io->port] = io->value;
    }
    /* Retail writes the current sound-output mode to channel 13/IO7.  Zero is
       stereo in the ROM scripts and matches this mixer's output contract. */
    p->channels[13].io[7] = 0;
    return true;
}

void oot_audio_sequence_stop( uint8_t player, uint16_t fadeOutMs )
{
    if( player >= SEQ_PLAYERS ) return;
    SeqPlayer *p = &s_players[player];
    if( fadeOutMs == 0 ) {
        for( int i = 0; i < SEQ_CHANNELS; ++i ) channel_disable( &p->channels[i] );
        voices_stop_player( player, false );
        if( player == OOT_AUDIO_PLAYER_SFX ) sfx_channel_state_reset();
        p->playing = 0;
        p->finished = 1;
        p->pendingStop = 0;
        p->fadeGain = 0.0f;
        p->fadeTarget = 0.0f;
        p->fadeSeconds = 0.0f;
    } else {
        if( !p->playing ) return;
        p->pendingStop = 1;
        p->fadeTarget = 0.0f;
        p->fadeSeconds = fadeOutMs / 1000.0f;
    }
}

void oot_audio_sequence_pause( uint8_t player, bool paused )
{
    if( player < SEQ_PLAYERS ) s_players[player].paused = paused ? 1 : 0;
}

void oot_audio_sequence_set_volume( uint8_t player, float volume )
{
    if( player < SEQ_PLAYERS && isfinite( volume )) s_players[player].userVolume = clamp01( volume );
}

void oot_audio_sequence_set_io( uint8_t player, uint8_t port, int8_t value )
{
    if( player < SEQ_PLAYERS && port < SEQ_IO_PORTS ) s_players[player].io[port] = value;
}

void oot_audio_channel_set_io( uint8_t player, uint8_t channel, uint8_t port, int8_t value )
{
    if( player < SEQ_PLAYERS && channel < SEQ_CHANNELS && port < SEQ_IO_PORTS )
        s_players[player].channels[channel].io[port] = value;
}

void oot_audio_set_master_volume( float volume )
{
    if( isfinite( volume )) s_masterVolume = clamp01( volume );
}

void oot_audio_stop_all( uint16_t fadeOutMs )
{
    for( uint8_t i = 0; i < SEQ_PLAYERS; ++i ) oot_audio_sequence_stop( i, fadeOutMs );
}

bool oot_audio_sequence_get_state( uint8_t player, struct OoTAudioState *outState )
{
    if( !outState || outState->structSize < sizeof( *outState ) ||
        outState->version != OOT_AUDIO_STATE_VERSION || player >= SEQ_PLAYERS ) return false;
    uint32_t size = outState->structSize, version = outState->version;
    memset( outState, 0, sizeof( *outState ));
    outState->structSize = size;
    outState->version = version;
    SeqPlayer *p = &s_players[player];
    outState->sequenceId = p->sequenceId;
    outState->resolvedId = p->resolvedId;
    outState->player = player;
    outState->playing = p->playing;
    outState->paused = p->paused;
    outState->finished = p->finished;
    outState->volume = p->userVolume * p->seqVolume * p->fadeGain;
    outState->framesRendered = p->framesRendered;
    for( int i = 0; i < SEQ_CHANNELS; ++i ) outState->activeChannels += p->channels[i].enabled != 0;
    for( int i = 0; i < AUDIO_VOICES; ++i ) outState->activeVoices +=
        s_voices[i].active && s_voices[i].player == player;
    return true;
}

static uint8_t player_font_by_ordinal( const SeqPlayer *p, uint8_t ordinal )
{
    if( ordinal < p->fontCount ) return p->fonts[p->fontCount - 1u - ordinal];
    return p->defaultFont;
}

static void adsr_settings_apply_view( AdsrSettings *settings,
                                      const LibootAudioAdsrView *view )
{
    if( !settings || !view || !view->envelope || view->size < 4 ) return;
    settings->envelope = view->envelope;
    settings->envelopeSize = view->size;
    settings->decayIndex = view->decayIndex;
}

static bool channel_set_instrument( SeqChannel *channel, uint8_t instrument )
{
    channel->instrument = instrument;
    if( instrument < 0x7E ) {
        LibootAudioAdsrView adsr;
        if( !liboot_audio_instrument_adsr_get( channel->font, instrument, &adsr )) {
            channel->hasInstrument = 0;
            return false;
        }
        adsr_settings_apply_view( &channel->adsr, &adsr );
    }
    channel->hasInstrument = 1;
    return true;
}

static void layer_set_instrument( SeqChannel *channel, SeqLayer *layer,
                                  uint8_t instrument )
{
    layer->instrument = instrument == 0xFF ? -1 : instrument;
    if( instrument < 0x7E ) {
        LibootAudioAdsrView adsr;
        if( liboot_audio_instrument_adsr_get( channel->font, instrument, &adsr ))
            adsr_settings_apply_view( &layer->adsr, &adsr );
    } else if( instrument == 0xFF ) {
        layer->adsr.decayIndex = 0;
    }
}

static uint16_t natural_sample_delay( const SeqPlayer *p,
                                      const LibootAudioSampleView *sample,
                                      float pitchScale )
{
    double rate = sample->sampleRate;
    if( pitchScale > 0.0f && isfinite( pitchScale )) rate *= pitchScale;
    /* Retail intentionally ignores tempoChange here even though the sequence
       scheduler applies it. p->tempo is stored as BPM; the native field is
       BPM * SEQTICKS_PER_BEAT. */
    double tempo = p->tempo * SEQTICKS_PER_BEAT;
    if( tempo < 1.0 ) tempo = 1.0;
    if( !isfinite( rate ) || rate <= 0.0 ) return 1;
    double ticks = sample->numSamples / rate * tempo * RETAIL_NATURAL_DELAY_SCALE_PAL;
    if( !isfinite( ticks ) || ticks >= 0x7FFE ) return 0x7FFF;
    return (uint16_t)ticks + 1;
}

static void layer_portamento_notes( SeqLayer *layer, uint8_t note,
                                    uint8_t *baseNote, uint8_t *sampleNote )
{
    uint8_t target = layer->portamentoTarget;
    uint8_t mode = PORTAMENTO_MODE( layer->portamento.mode );
    *sampleNote = note > target ? note : target;
    uint8_t endNote;
    switch( mode ) {
    case 1: case 3: case 5:
        *baseNote = target;
        endNote = note;
        break;
    case 2: case 4:
        *baseNote = note;
        endNote = target;
        break;
    default:
        *baseNote = note;
        endNote = note;
        break;
    }
    layer->portamento.extent =
        exp2f(((int)endNote - (int)*baseNote ) / 12.0f ) - 1.0f;
    layer->portamento.cur = 0;
    if( mode == 5 ) layer->portamentoTarget = note;
}

static void layer_portamento_speed( const SeqPlayer *p, SeqLayer *layer )
{
    if( layer->portamento.mode == 0 ) {
        layer->portamento.speed = 0;
        return;
    }
    uint32_t time = layer->portamentoTime;
    uint32_t speed;
    if( PORTAMENTO_SPECIAL( layer->portamento.mode )) {
        uint32_t tempo = (uint32_t)fmax( 1.0, p->tempo ) * (uint32_t)SEQTICKS_PER_BEAT;
        speed = tempo * 0x8000u / RETAIL_MAX_TEMPO_PAL;
        if( layer->delay != 0 ) {
            uint64_t denominator = (uint64_t)layer->delay * time;
            speed = denominator ?
                (uint32_t)((uint64_t)speed * 0x100u / denominator ) : 0x7FFFu;
        }
    } else {
        uint32_t denominator = time * (uint32_t)RETAIL_ADSR_TICKS_PER_UPDATE;
        speed = denominator ? 0x20000u / denominator : 0x7FFFu;
    }
    if( speed > 0x7FFFu ) speed = 0x7FFFu;
    if( speed < 1u ) speed = 1u;
    layer->portamento.speed = (uint16_t)speed;
}

static void layer_note( SeqPlayer *p, uint8_t channelIndex, uint8_t layerIndex,
                        SeqChannel *channel, SeqLayer *layer, uint8_t pitch )
{
    if( !layer->legato ) layer_decay( channel, layer );
    int note = pitch + p->transpose + channel->transpose + layer->transpose;
    if( note < 0 || note >= 128 ) return;

    uint8_t instrument = layer->instrument >= 0 ? (uint8_t)layer->instrument : channel->instrument;
    if( layer->instrument < 0 && !channel->hasInstrument ) return;
    float volume = layer->velocitySquare * ( channel->volume / 127.0f ) *
                   ( channel->volumeScale / 128.0f );
    float panWeight = clamp01( channel->panWeight / 128.0f );
    float pan = ( channel->pan * panWeight + layer->pan * ( 1.0f - panWeight )) / 127.0f;
    if( p->index == OOT_AUDIO_PLAYER_SFX && s_sfxChannelId[channelIndex] )
        pan = s_sfxChannelPan[channelIndex];
    float reverb = channel->reverb / 255.0f;
    float pitchScale = channel->freqScale * layer->bend;
    uint8_t baseNote = (uint8_t)note;
    uint8_t sampleNote = (uint8_t)note;
    bool usePortamento = instrument != 0x7E && instrument != 0x7F &&
                         layer->portamento.mode != 0;
    if( usePortamento )
        layer_portamento_notes( layer, (uint8_t)note, &baseNote, &sampleNote );

    if( instrument >= 0x80 ) {
        if( layer->delay == 0 ) {
            layer->delay = 1;
            layer->gateDelay = 0;
        }
        if( usePortamento ) layer_portamento_speed( p, layer );
        voice_start_wave( p, channelIndex, layerIndex, layer, instrument, baseNote,
                          volume, pan, pitchScale, reverb );
        return;
    }
    LibootAudioSampleView sample;
    bool ok;
    if( instrument == 0x7F ) {
        int drum = pitch + channel->transpose + layer->transpose;
        ok = drum >= 0 && drum < 128 &&
             liboot_audio_drum_sample_get( channel->font, (uint8_t)drum, &sample );
        pitchScale = channel->freqScale * layer->bend;
    } else if( instrument == 0x7E ) {
        uint16_t sfx = (uint16_t)(((uint8_t)layer->transpose << 6 ) + pitch );
        ok = liboot_audio_font_sfx_sample_get( channel->font, sfx, &sample );
    } else {
        ok = liboot_audio_instrument_sample_get_for_pitch( channel->font, instrument,
                                                           sampleNote, baseNote, &sample );
    }
    if( ok ) {
        if( instrument == 0x7F ) {
            adsr_settings_apply_view( &layer->adsr, &sample.adsr );
            if( sample.hasPan && !layer->ignoreDrumPan ) {
                layer->pan = sample.pan;
                pan = ( channel->pan * panWeight +
                        layer->pan * ( 1.0f - panWeight )) / 127.0f;
            }
        }
        if( p->index == OOT_AUDIO_PLAYER_SFX && s_sfxChannelId[channelIndex] )
            pan = s_sfxChannelPan[channelIndex];
        if( layer->delay == 0 ) {
            /* The retail natural-note duration is based on the layer pitch
               (including bend), before the channel frequency scale is mixed
               into the final note playback rate. */
            layer->delay = natural_sample_delay( p, &sample, layer->bend );
            layer->gateDelay = 0;
        }
        if( usePortamento ) layer_portamento_speed( p, layer );
        voice_start_sample( p, channelIndex, layerIndex, layer, &sample,
                            volume, pan, pitchScale, reverb );
    }
}

static void process_layer( SeqPlayer *p, uint8_t channelIndex, uint8_t layerIndex )
{
    SeqChannel *channel = &p->channels[channelIndex];
    SeqLayer *layer = &channel->layers[layerIndex];
    if( !layer->enabled ) return;
    if( layer->delay > 1 ) {
        --layer->delay;
        if( !layer->muted && layer->delay <= layer->gateDelay ) {
            layer_decay( channel, layer );
            layer->muted = 1;
        }
        return;
    }
    if( !layer->legato ) layer_decay( channel, layer );
    uint8_t portamentoMode = PORTAMENTO_MODE( layer->portamento.mode );
    if( portamentoMode == 1 || portamentoMode == 2 )
        layer->portamento.mode = 0;

    for( int budget = 0; budget < SCRIPT_BUDGET && layer->script.valid; ++budget ) {
        uint8_t cmd = read_u8( p, &layer->script );
        if( !layer->script.valid ) break;
        if( cmd >= 0xF2 ) {
            int flow = script_flow( p, &layer->script, cmd );
            if( flow == FLOW_CONTINUE ) continue;
            if( flow > 0 ) { layer->delay = (uint16_t)flow; return; }
            layer_decay( channel, layer );
            layer->enabled = 0;
            layer->finished = 1;
            return;
        }
        if( cmd <= 0xC0 ) {
            if( cmd == 0xC0 ) {
                layer->delay = read_var( p, &layer->script );
                if( layer->delay == 0 ) layer->delay = 1;
                layer_decay( channel, layer );
                layer->muted = 1;
                return;
            }
            uint16_t delay;
            uint8_t velocity = layer->velocity, gate = layer->gate;
            if( channel->largeNotes ) {
                switch( cmd & 0xC0 ) {
                case 0x00:
                    delay = read_var( p, &layer->script );
                    velocity = read_u8( p, &layer->script );
                    gate = read_u8( p, &layer->script );
                    layer->lastDelay = delay;
                    break;
                case 0x40:
                    delay = read_var( p, &layer->script );
                    velocity = read_u8( p, &layer->script );
                    gate = 0;
                    layer->lastDelay = delay;
                    break;
                default:
                    delay = layer->lastDelay;
                    velocity = read_u8( p, &layer->script );
                    gate = read_u8( p, &layer->script );
                    break;
                }
            } else {
                switch( cmd & 0xC0 ) {
                case 0x00: delay = read_var( p, &layer->script ); layer->lastDelay = delay; break;
                case 0x40: delay = layer->shortDelay; break;
                default:   delay = layer->lastDelay; break;
                }
            }
            if( !layer->script.valid ) break;
            layer->velocity = velocity > 0x7F ? 0x7F : velocity;
            layer->gate = gate;
            layer->delay = delay;
            layer->gateDelay = delay ?
                (uint16_t)(((uint32_t)gate * delay ) >> 8 ) : 0;
            uint32_t noteRandom = next_random();
            float velocityNorm = layer->velocity / 127.0f;
            layer->velocitySquare = velocityNorm * velocityNorm;
            if( channel->velocityRandom ) {
                float delta = layer->velocitySquare *
                    ( noteRandom % channel->velocityRandom ) / 100.0f;
                if( noteRandom & 0x8000u ) delta = -delta;
                layer->velocitySquare = clamp01( layer->velocitySquare + delta );
            }
            if( channel->gateRandom && channel->velocityRandom ) {
                int32_t delta = (int32_t)((uint32_t)layer->gateDelay *
                    ( noteRandom % channel->velocityRandom ) / 100u );
                if( noteRandom & 0x4000u ) delta = -delta;
                int32_t randomized = (int32_t)layer->gateDelay + delta;
                if( randomized < 0 ) randomized = 0;
                if( randomized > delay ) randomized = delay;
                layer->gateDelay = (uint16_t)randomized;
            }
            layer->muted = 0;
            layer_note( p, channelIndex, layerIndex, channel, layer, cmd & 0x3F );
            if( layer->delay == 0 ) layer->delay = 1;
            return;
        }

        switch( cmd ) {
        case 0xC1: layer->velocity = read_u8( p, &layer->script ); break;
        case 0xC2: layer->transpose = (int8_t)read_u8( p, &layer->script ); break;
        case 0xC3: layer->shortDelay = read_var( p, &layer->script ); break;
        case 0xC4: layer->legato = 1; layer_decay( channel, layer ); break;
        case 0xC5: layer->legato = 0; layer_decay( channel, layer ); break;
        case 0xC6:
            layer_set_instrument( channel, layer, read_u8( p, &layer->script ));
            break;
        case 0xC7: {
            uint8_t mode = read_u8( p, &layer->script );
            uint8_t target = read_u8( p, &layer->script );
            target = (uint8_t)( target + channel->transpose );
            target = (uint8_t)( target + layer->transpose );
            target = (uint8_t)( target + p->transpose );
            if( target >= 0x80 ) target = 0;
            layer->portamento.mode = mode;
            layer->portamentoTarget = target;
            layer->portamentoTime = PORTAMENTO_SPECIAL( mode ) ?
                                     read_u8( p, &layer->script ) :
                                     read_var( p, &layer->script );
            break;
        }
        case 0xC8: layer->portamento.mode = 0; break;
        case 0xC9: layer->gate = read_u8( p, &layer->script ); break;
        case 0xCA: layer->pan = read_u8( p, &layer->script ); break;
        case 0xCB: {
            uint16_t offset = (uint16_t)read_s16( p, &layer->script );
            uint8_t decayIndex = read_u8( p, &layer->script );
            if( offset <= p->dataSize && p->dataSize - offset >= 4 ) {
                layer->adsr.envelope = p->data + offset;
                layer->adsr.envelopeSize = p->dataSize - offset;
                layer->adsr.decayIndex = decayIndex;
            } else layer->script.valid = 0;
            break;
        }
        case 0xCC: layer->ignoreDrumPan = 1; break;
        case 0xCD: read_u8( p, &layer->script ); break;
        case 0xCE: {
            int8_t bend = (int8_t)read_u8( p, &layer->script );
            layer->bend = exp2f( bend / ( 128.0f * 6.0f ));
            break;
        }
        case 0xCF: layer->adsr.decayIndex = read_u8( p, &layer->script ); break;
        case 0xF0: case 0xF1: break;
        default:
            if(( cmd & 0xF0 ) == 0xD0 ) layer->velocity = p->shortVel[cmd & 15];
            else if(( cmd & 0xF0 ) == 0xE0 ) layer->gate = p->shortGate[cmd & 15];
            else { layer->script.valid = 0; }
            break;
        }
    }
    if( !layer->script.valid ) {
        layer_decay( channel, layer );
        layer->enabled = 0;
        layer->finished = 1;
    }
}

/* abc... argument descriptor used by channel opcodes B0..F1: low two bits are
   the argument count; 0x80/0x40/0x20 select signed 16-bit arguments. */
static const uint8_t s_channelArgSpec[0x42] = {
    0x81,0,0x81,1,0,0,0,0x81,1,1,1,0x42,0x81,0xC2,0,0,
    0,1,0x81,0,0,0,1,0x42,1,1,1,0x81,1,1,0x81,0x81,
    1,1,1,1,1,1,1,1,1,1,0x81,1,1,1,0x81,1,
    1,3,3,1,0,1,1,0x81,3,1,0,2,0,1,1,0x82,
    0,1
};

static uint16_t data_be16( const SeqPlayer *p, uint32_t offset, bool *ok )
{
    if( offset > p->dataSize || p->dataSize - offset < 2 ) {
        if( ok ) *ok = false;
        return 0;
    }
    return (uint16_t)((uint16_t)p->data[offset] << 8 | p->data[offset + 1]);
}

static void process_channel_command( SeqPlayer *p, uint8_t channelIndex, uint8_t cmd,
                                     uint32_t arg[3] )
{
    SeqChannel *c = &p->channels[channelIndex];
    Script *s = &c->script;
    switch( cmd ) {
    case 0xB0: break; /* filter address; synthesis filter is optional */
    case 0xB1: break;
    case 0xB2: {
        bool ok = true;
        c->ptr = data_be16( p, (uint16_t)arg[0] + (uint16_t)s->value * 2u, &ok );
        if( !ok ) s->valid = 0;
        break;
    }
    case 0xB3: break;
    case 0xB4: c->dynTable = c->ptr; break;
    case 0xB5: {
        bool ok = true;
        c->ptr = data_be16( p, c->dynTable + (uint16_t)s->value * 2u, &ok );
        if( !ok ) s->valid = 0;
        break;
    }
    case 0xB6:
        if( c->dynTable + (uint16_t)s->value < p->dataSize )
            s->value = p->data[c->dynTable + (uint16_t)s->value];
        else s->valid = 0;
        break;
    case 0xB7: c->ptr = arg[0] ? next_random() % arg[0] : next_random() & 0xFFFFu; break;
    case 0xB8: s->value = (int16_t)( arg[0] ? next_random() % arg[0] : next_random() & 0xFFFFu ); break;
    case 0xB9: c->velocityRandom = (uint8_t)arg[0]; break;
    case 0xBA: c->gateRandom = (uint8_t)arg[0]; break;
    case 0xBB: break;
    case 0xBC: c->ptr = (uint16_t)( c->ptr + (int16_t)arg[0] ); break;
    case 0xBD: {
        uint16_t value = (uint16_t)(( arg[0] ? next_random() % arg[0] :
                                      next_random() & 0xFFFFu ) + arg[1]);
        /* RANDPTR stores its random value in the sequence's segmented-pointer
           form: high byte biased by 0x80, low byte preserved. */
        c->ptr = (uint16_t)(((( value >> 8 ) + 0x80u ) << 8 ) | ( value & 0xFFu ));
        break;
    }
    case 0xC0: break;
    case 0xC1: (void)channel_set_instrument( c, (uint8_t)arg[0] ); break;
    case 0xC2: c->dynTable = (uint16_t)arg[0]; break;
    case 0xC3: c->largeNotes = 0; break;
    case 0xC4: c->largeNotes = 1; break;
    case 0xC5: {
        if( s->value == -1 ) break;
        bool ok = true;
        if( s->value < 0 ) { s->valid = 0; break; }
        uint16_t target = data_be16( p, c->dynTable + (uint16_t)s->value * 2u, &ok );
        if( ok ) c->dynTable = target; else s->valid = 0;
        break;
    }
    case 0xC6: c->font = player_font_by_ordinal( p, (uint8_t)arg[0] ); break;
    case 0xC7: {
        uint32_t off = (uint16_t)arg[1];
        if( off < p->dataSize ) p->data[off] = (uint8_t)( s->value + (uint8_t)arg[0] );
        else s->valid = 0;
        break;
    }
    case 0xC8: s->value -= (int8_t)arg[0]; break;
    case 0xC9: s->value &= (uint8_t)arg[0]; break;
    case 0xCA: break;
    case 0xCB: {
        int32_t off = (uint16_t)arg[0] + s->value;
        if( off >= 0 && (uint32_t)off < p->dataSize ) s->value = p->data[off];
        else s->valid = 0;
        break;
    }
    case 0xCC: s->value = (int8_t)arg[0]; break;
    case 0xCD: if( arg[0] < SEQ_CHANNELS ) channel_disable( &p->channels[arg[0]] ); break;
    case 0xCE: c->ptr = (uint16_t)arg[0]; break;
    case 0xCF: {
        uint32_t off = (uint16_t)arg[0];
        if( off + 1 < p->dataSize ) {
            p->data[off] = (uint8_t)( c->ptr >> 8 ); p->data[off + 1] = (uint8_t)c->ptr;
        } else s->valid = 0;
        break;
    }
    case 0xD0: break;
    case 0xD1: break;
    case 0xD2: c->adsr.sustain = (uint8_t)arg[0]; break;
    case 0xD3:
        c->freqScale = exp2f((int8_t)(uint8_t)arg[0] / 128.0f );
        break;
    case 0xD4: c->reverb = (uint8_t)arg[0]; break;
    case 0xD5: case 0xD6: break;
    case 0xD7: case 0xD8: break; /* vibrato target retained by retail; mixer applies bend */
    case 0xD9: c->adsr.decayIndex = (uint8_t)arg[0]; break;
    case 0xDA: {
        uint16_t offset = (uint16_t)arg[0];
        if( offset <= p->dataSize && p->dataSize - offset >= 4 ) {
            c->adsr.envelope = p->data + offset;
            c->adsr.envelopeSize = p->dataSize - offset;
        } else s->valid = 0;
        break;
    }
    case 0xDB: c->transpose = (int8_t)arg[0]; break;
    case 0xDC: c->panWeight = (uint8_t)arg[0]; break;
    case 0xDD: c->pan = (uint8_t)arg[0]; break;
    case 0xDE: c->freqScale = (uint16_t)arg[0] / 32768.0f; break;
    case 0xDF: c->volume = (uint8_t)arg[0]; break;
    case 0xE0: c->volumeScale = (uint8_t)arg[0]; break;
    case 0xE1: case 0xE2: case 0xE3: break;
    case 0xE4: {
        bool ok = true;
        if( s->value >= 0 && s->depth < SEQ_STACK_DEPTH ) {
            uint16_t target = data_be16( p, c->dynTable + (uint16_t)s->value * 2u, &ok );
            if( ok && target < p->dataSize ) {
                s->stack[s->depth++] = s->pc;
                s->pc = target;
            } else s->valid = 0;
        }
        break;
    }
    case 0xE5: case 0xE6: break;
    case 0xE7: {
        uint32_t off = (uint16_t)arg[0];
        if( off + 8 <= p->dataSize ) {
            c->transpose = (int8_t)p->data[off + 3];
            c->pan = p->data[off + 4];
            c->panWeight = p->data[off + 5];
            c->reverb = p->data[off + 6];
        } else s->valid = 0;
        break;
    }
    case 0xE8:
        c->transpose = (int8_t)read_u8( p, s );
        c->pan = read_u8( p, s );
        c->panWeight = read_u8( p, s );
        c->reverb = read_u8( p, s );
        read_u8( p, s );
        break;
    case 0xE9: break;
    case 0xEA: c->stopScript = 1; break;
    case 0xEB:
        c->font = player_font_by_ordinal( p, (uint8_t)arg[0] );
        (void)channel_set_instrument( c, (uint8_t)arg[1] );
        break;
    case 0xEC:
        c->adsr.sustain = 0;
        c->velocityRandom = 0;
        c->gateRandom = 0;
        c->freqScale = 1.0f;
        break;
    case 0xED: break;
    case 0xEE:
        c->freqScale = exp2f((int8_t)(uint8_t)arg[0] / ( 128.0f * 6.0f ));
        break;
    case 0xEF: break;
    case 0xF0: case 0xF1: break;
    default: s->valid = 0; break;
    }
}

static void process_channel( SeqPlayer *p, uint8_t channelIndex )
{
    SeqChannel *c = &p->channels[channelIndex];
    if( !c->enabled ) return;
    if( !c->stopScript ) {
        if( c->delay >= 2 ) --c->delay;
        else {
            for( int budget = 0; budget < SCRIPT_BUDGET && c->script.valid; ++budget ) {
                uint8_t cmd = read_u8( p, &c->script );
                if( !c->script.valid ) break;
                if( cmd >= 0xF2 ) {
                    int flow = script_flow( p, &c->script, cmd );
                    if( flow == FLOW_CONTINUE ) continue;
                    if( flow > 0 ) c->delay = (uint16_t)flow;
                    else channel_disable( c );
                    break;
                }
                if( cmd >= 0xB0 ) {
                    uint8_t spec = s_channelArgSpec[cmd - 0xB0];
                    uint8_t count = spec & 3;
                    uint32_t arg[3] = { 0, 0, 0 };
                    for( uint8_t i = 0; i < count; ++i, spec <<= 1 )
                        arg[i] = spec & 0x80 ? (uint16_t)read_s16( p, &c->script ) :
                                              read_u8( p, &c->script );
                    if( !c->script.valid ) break;
                    process_channel_command( p, channelIndex, cmd, arg );
                    if( c->stopScript ) break;
                    continue;
                }
                if( cmd >= 0x70 ) {
                    uint8_t low = cmd & 7;
                    uint8_t op = cmd & 0xF8;
                    /* The encoding reserves three low bits, but OoT has four
                       layers. Retail aliases 4..7 to layer 0 for every layer
                       opcode; STIO alone uses all eight values as IO ports. */
                    if( op != 0x70 && low >= SEQ_LAYERS ) low = 0;
                    switch( op ) {
                    case 0x78: {
                        int16_t rel = read_s16( p, &c->script );
                        layer_enable( p, c, low, (uint32_t)((int32_t)c->script.pc + rel ));
                        break;
                    }
                    case 0x80: c->script.value = c->layers[low].enabled ? c->layers[low].finished : -1; break;
                    case 0x88: layer_enable( p, c, low, (uint16_t)read_s16( p, &c->script )); break;
                    case 0x90: layer_decay( c, &c->layers[low] ); c->layers[low].enabled = 0; break;
                    case 0x98: {
                        if( c->script.value == -1 ) break;
                        if( c->script.value < 0 ) { c->script.valid = 0; break; }
                        bool ok = true;
                        uint16_t target = data_be16( p, c->dynTable + (uint16_t)c->script.value * 2u, &ok );
                        if( ok ) layer_enable( p, c, low, target ); else c->script.valid = 0;
                        break;
                    }
                    case 0x70: c->io[low] = c->script.value; break;
                    default: c->script.valid = 0; break;
                    }
                    continue;
                }

                uint8_t low = cmd & 15;
                switch( cmd & 0xF0 ) {
                case 0x00: c->delay = low ? low : 1; budget = SCRIPT_BUDGET; break;
                case 0x10: c->io[low & 7] = SEQ_IO_NONE; break;
                case 0x20: channel_enable( p, low, (uint16_t)read_s16( p, &c->script )); break;
                case 0x30: {
                    uint8_t port = read_u8( p, &c->script );
                    if( low < SEQ_CHANNELS && port < SEQ_IO_PORTS ) p->channels[low].io[port] = c->script.value;
                    break;
                }
                case 0x40: {
                    uint8_t port = read_u8( p, &c->script );
                    c->script.value = low < SEQ_CHANNELS && port < SEQ_IO_PORTS ?
                                      p->channels[low].io[port] : SEQ_IO_NONE;
                    break;
                }
                case 0x50: c->script.value -= c->io[low & 7]; break;
                case 0x60:
                    c->script.value = c->io[low & 7];
                    if(( low & 7 ) < 2 ) c->io[low & 7] = SEQ_IO_NONE;
                    break;
                default: c->script.valid = 0; break;
                }
            }
            if( !c->script.valid ) channel_disable( c );
        }
    }
    if( c->enabled ) for( uint8_t i = 0; i < SEQ_LAYERS; ++i ) process_layer( p, channelIndex, i );
}

static void player_finish( SeqPlayer *p )
{
    for( int i = 0; i < SEQ_CHANNELS; ++i )
        channel_disable_mode( &p->channels[i], true );
    if( p->index == OOT_AUDIO_PLAYER_SFX ) sfx_channel_state_reset();
    p->playing = 0;
    p->finished = 1;
}

static void sequence_dynamic_call( SeqPlayer *p, uint16_t table )
{
    if( p->script.value < 0 || p->script.depth >= SEQ_STACK_DEPTH ) return;
    bool ok = true;
    uint16_t target = data_be16( p, table + (uint16_t)p->script.value * 2u, &ok );
    if( !ok || target >= p->dataSize ) { p->script.valid = 0; return; }
    p->script.stack[p->script.depth++] = p->script.pc;
    p->script.pc = target;
}

static void process_sequence( SeqPlayer *p )
{
    if( !p->playing || p->paused || p->stopScript ) return;
    if( p->delay > 1 ) --p->delay;
    else {
        for( int budget = 0; budget < SCRIPT_BUDGET && p->script.valid; ++budget ) {
            uint8_t cmd = read_u8( p, &p->script );
            if( !p->script.valid ) break;
            if( cmd >= 0xF2 ) {
                int flow = script_flow( p, &p->script, cmd );
                if( flow == FLOW_CONTINUE ) continue;
                if( flow > 0 ) p->delay = (uint16_t)flow;
                else player_finish( p );
                break;
            }
            if( cmd >= 0xC0 ) {
                switch( cmd ) {
                case 0xC4: {
                    uint8_t targetPlayer = read_u8( p, &p->script );
                    uint8_t seqId = read_u8( p, &p->script );
                    if( targetPlayer == 0xFF ) targetPlayer = p->index;
                    if( targetPlayer < SEQ_PLAYERS && seqId < OOT_AUDIO_SEQUENCE_COUNT ) {
                        bool same = targetPlayer == p->index;
                        player_load( targetPlayer, seqId, 0, false );
                        if( same ) return;
                    }
                    break;
                }
                case 0xC5: read_s16( p, &p->script ); break;
                case 0xC6: p->stopScript = 1; break;
                case 0xC7: {
                    uint8_t add = read_u8( p, &p->script );
                    uint16_t off = (uint16_t)read_s16( p, &p->script );
                    if( off < p->dataSize ) p->data[off] = (uint8_t)( p->script.value + add );
                    else p->script.valid = 0;
                    break;
                }
                case 0xC8: p->script.value -= (int8_t)read_u8( p, &p->script ); break;
                case 0xC9: p->script.value &= read_u8( p, &p->script ); break;
                case 0xCC: p->script.value = (int8_t)read_u8( p, &p->script ); break;
                case 0xCD: sequence_dynamic_call( p, (uint16_t)read_s16( p, &p->script )); break;
                case 0xCE: {
                    uint8_t max = read_u8( p, &p->script );
                    p->script.value = (int16_t)( max ? next_random() % max : ( next_random() >> 2 ) & 0xFF );
                    break;
                }
                case 0xD0: read_u8( p, &p->script ); break;
                case 0xD1:
                case 0xD2: {
                    uint16_t off = (uint16_t)read_s16( p, &p->script );
                    if((uint32_t)off + 16u <= p->dataSize ) {
                        if( cmd == 0xD1 ) { p->shortGateOff = off; memcpy( p->shortGate, p->data + off, 16 ); }
                        else { p->shortVelOff = off; memcpy( p->shortVel, p->data + off, 16 ); }
                    } else p->script.valid = 0;
                    break;
                }
                case 0xD3: read_u8( p, &p->script ); break;
                case 0xD4: break;
                case 0xD5: read_u8( p, &p->script ); break;
                case 0xD6: read_s16( p, &p->script ); break;
                case 0xD7: read_s16( p, &p->script ); break;
                case 0xD9: read_u8( p, &p->script ); break;
                case 0xDA: read_u8( p, &p->script ); read_s16( p, &p->script ); break;
                case 0xDB: p->seqVolume = read_u8( p, &p->script ) / 127.0f; break;
                case 0xDC: p->tempoChange = (int8_t)read_u8( p, &p->script ); break;
                case 0xDD: p->tempo = fmaxf( 1.0f, read_u8( p, &p->script )); break;
                case 0xDE: p->transpose += (int8_t)read_u8( p, &p->script ); break;
                case 0xDF: p->transpose = (int8_t)read_u8( p, &p->script ); break;
                case 0xEF: read_s16( p, &p->script ); read_u8( p, &p->script ); break;
                case 0xF0: break;
                case 0xF1: read_u8( p, &p->script ); break;
                default: p->script.valid = 0; break;
                }
                if( p->stopScript || !p->script.valid ) break;
                continue;
            }

            uint8_t low = cmd & 15;
            switch( cmd & 0xF0 ) {
            case 0x00: p->script.value = p->channels[low].enabled ? 0 : 1; break;
            case 0x40: channel_disable( &p->channels[low] ); break;
            case 0x50: p->script.value -= p->io[low & 7]; break;
            case 0x60: {
                read_u8( p, &p->script ); read_u8( p, &p->script );
                p->io[low & 7] = 0; /* synchronous ROM resources are already loaded */
                break;
            }
            case 0x70: p->io[low & 7] = p->script.value; break;
            case 0x80:
                p->script.value = p->io[low & 7];
                if(( low & 7 ) < 2 ) p->io[low & 7] = SEQ_IO_NONE;
                break;
            case 0x90: channel_enable( p, low, (uint16_t)read_s16( p, &p->script )); break;
            case 0xA0: {
                int16_t rel = read_s16( p, &p->script );
                channel_enable( p, low, (uint32_t)((int32_t)p->script.pc + rel ));
                break;
            }
            case 0xB0: {
                uint8_t childId = read_u8( p, &p->script );
                uint16_t destination = (uint16_t)read_s16( p, &p->script );
                LibootAudioSequenceView child;
                if( p->script.valid && liboot_audio_sequence_get( childId, &child ) &&
                    child.data && destination <= p->dataSize &&
                    child.size <= p->dataSize - destination ) {
                    memcpy( p->data + destination, child.data, child.size );
                    p->io[low & 7] = 0;
                } else {
                    p->io[low & 7] = SEQ_IO_NONE;
                }
                break;
            }
            default: p->script.valid = 0; break;
            }
        }
        if( !p->script.valid ) player_finish( p );
    }

    if( p->playing ) for( uint8_t i = 0; i < SEQ_CHANNELS; ++i ) process_channel( p, i );
}

bool oot_audio_sfx_play( uint16_t sfxId, float pan, float volume )
{
    uint16_t index;
    int bank = sfx_bank_for_id( sfxId, &index );
    if( bank < 0 || !isfinite( pan ) || !isfinite( volume )) return false;
    pan = pan < -1.0f ? -1.0f : pan > 1.0f ? 1.0f : pan;
    volume = clamp01( volume );

    if( !s_players[OOT_AUDIO_PLAYER_SFX].playing ||
        s_players[OOT_AUDIO_PLAYER_SFX].sequenceId != 0 ||
        s_players[OOT_AUDIO_PLAYER_SFX].pendingStop ) {
        if( !player_load( OOT_AUDIO_PLAYER_SFX, 0, 0, true )) return false;
        s_players[OOT_AUDIO_PLAYER_SFX].io[0] = 0; /* standard channel layout */
    }

    /* Inject the same IO values used by OoT's SFX manager into Sequence 0.
       Every catalog entry follows its complete ROM script, retaining layered
       notes, pans, random pitch and multi-note effects.  The older raw-PCM
       accessor remains available separately as a compatibility API. */
    static const uint8_t firstChannel[7] = { 0, 3, 5, 8, 11, 13, 14 };
    static const uint8_t channelCount[7] = { 3, 2, 3, 3, 2, 1, 2 };
    uint8_t channelIndex = (uint8_t)( firstChannel[bank] +
        s_sfxRotation[bank]++ % channelCount[bank] );
    SeqChannel *channel = &s_players[OOT_AUDIO_PLAYER_SFX].channels[channelIndex];
    channel->io[0] = 1;
    channel->io[2] = (int16_t)lroundf( volume * 127.0f );
    channel->io[3] = 0;
    channel->io[4] = index & 0xFF;
    channel->io[5] = index >> 8;
    channel->pan = (uint8_t)lroundf(( pan + 1.0f ) * 63.5f );
    s_sfxChannelPan[channelIndex] = ( pan + 1.0f ) * 0.5f;
    s_sfxChannelId[channelIndex] = sfxId | 0x0800u;
    return true;
}

void oot_audio_sfx_stop( uint16_t sfxId )
{
    uint16_t canonical = sfxId | 0x0800u;
    for( int i = 0; i < AUDIO_VOICES; ++i )
        if( s_voices[i].active && s_voices[i].sfxId == canonical )
            voice_release( &s_voices[i] );
    for( int i = 0; i < SEQ_CHANNELS; ++i ) {
        if( s_sfxChannelId[i] == canonical ) {
            s_players[OOT_AUDIO_PLAYER_SFX].channels[i].io[0] = 0;
            s_sfxChannelId[i] = 0;
            s_sfxChannelPan[i] = 0.5f;
        }
    }
}

void oot_audio_sfx_stop_all( void )
{
    for( int i = 0; i < AUDIO_VOICES; ++i )
        if( s_voices[i].active && s_voices[i].player == OOT_AUDIO_PLAYER_SFX )
            voice_release( &s_voices[i] );
    for( int i = 0; i < SEQ_CHANNELS; ++i ) {
        s_players[OOT_AUDIO_PLAYER_SFX].channels[i].io[0] = 0;
    }
    sfx_channel_state_reset();
}

static float noise_wave_sample( float phase, uint32_t bins, uint32_t seed )
{
    uint32_t index = (uint32_t)( phase * bins );
    if( index >= bins ) index = bins - 1;
    uint32_t value = index + seed;
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return ( value & 0xFFFFu ) / 32767.5f - 1.0f;
}

static float oscillator_sample( AudioVoice *voice, uint32_t sampleRate )
{
    float phase = voice->oscPhase;
    float angle = phase * 6.2831853071795864769f;
    float sample;
    switch( voice->wave ) {
    case 0x80: sample = phase * 2.0f - 1.0f; break;                    /* saw */
    case 0x81: sample = 1.0f - 4.0f * fabsf( phase - 0.5f ); break;   /* triangle */
    case 0x82: sample = sinf( angle ); break;
    case 0x83: sample = phase < 0.5f ? 1.0f : -1.0f; break;
    case 0x84: sample = noise_wave_sample( phase, 64, 0xA511E9B3u ); break;
    case 0x85:                                                            /* bell */
        sample = 0.55f * sinf( angle ) + 0.30f * sinf( angle * 2.0f ) +
                 0.15f * sinf( angle * 3.0f );
        break;
    case 0x86: sample = phase < 0.125f ? 1.0f : -1.0f; break;        /* 1/8 pulse */
    case 0x87: sample = phase < 0.25f ? 1.0f : -1.0f; break;         /* 1/4 pulse */
    case 0x88: sample = noise_wave_sample( phase, 32, 0x63D83595u ); break;
    default: sample = sinf( angle ); break;
    }
    phase += voice->oscStep * voice->portamentoScale / sampleRate;
    phase -= floorf( phase );
    voice->oscPhase = phase;
    return sample * 0.65f;
}

static bool voice_pcm_sample( AudioVoice *voice, uint32_t sampleRate, float *out )
{
    if( !voice->pcm || voice->numSamples == 0 ) return false;
    uint32_t i0 = (uint32_t)voice->position;
    if( i0 >= voice->numSamples ) return false;
    uint32_t i1 = i0 + 1 < voice->numSamples ? i0 + 1 : i0;
    float frac = (float)( voice->position - i0 );
    *out = ( voice->pcm[i0] + ( voice->pcm[i1] - voice->pcm[i0] ) * frac ) / 32768.0f;
    voice->position += voice->sourceRate * voice->portamentoScale / sampleRate;
    if( voice->position >= voice->numSamples ) {
        bool loop = voice->loopStart < voice->numSamples &&
                    ( voice->loopCount == UINT32_MAX || voice->loopCount > 0 );
        if( loop ) {
            double excess = voice->position - voice->numSamples;
            double length = voice->numSamples - voice->loopStart;
            if( voice->loopCount != UINT32_MAX ) --voice->loopCount;
            voice->position = voice->loopStart + fmod( excess, length );
        } else {
            voice->active = 0;
        }
    }
    return true;
}

static void player_update_fade( SeqPlayer *p, uint32_t sampleRate )
{
    if( p->fadeSeconds > 0.0f ) {
        float frames = p->fadeSeconds * sampleRate;
        p->fadeGain += ( p->fadeTarget - p->fadeGain ) / fmaxf( 1.0f, frames );
        p->fadeSeconds -= 1.0f / sampleRate;
        if( p->fadeSeconds <= 0.0f ) {
            p->fadeSeconds = 0.0f;
            p->fadeGain = p->fadeTarget;
        }
    }
    if( p->pendingStop && p->fadeGain <= 0.00001f ) {
        for( int i = 0; i < SEQ_CHANNELS; ++i ) channel_disable( &p->channels[i] );
        voices_stop_player( p->index, false );
        if( p->index == OOT_AUDIO_PLAYER_SFX ) sfx_channel_state_reset();
        p->playing = 0;
        p->finished = 1;
        p->pendingStop = 0;
    }
}

uint32_t oot_audio_render_f32( float *stereo, uint32_t frames, uint32_t sampleRate )
{
    if( !stereo || frames == 0 || sampleRate < 8000 || sampleRate > 192000 ) return 0;
    memset( stereo, 0, (size_t)frames * 2u * sizeof( *stereo ));

    for( uint32_t frame = 0; frame < frames; ++frame ) {
        for( uint8_t player = 0; player < SEQ_PLAYERS; ++player ) {
            SeqPlayer *p = &s_players[player];
            if( !p->playing ) continue;
            player_update_fade( p, sampleRate );
            if( !p->playing || p->paused ) continue;
            int tickBudget = 8;
            while( p->samplesUntilTick <= 0.0 && tickBudget-- > 0 && p->playing ) {
                process_sequence( p );
                double tempo = p->tempo + p->tempoChange;
                if( tempo < 1.0 ) tempo = 1.0;
                p->samplesUntilTick += sampleRate * 60.0 / ( tempo * SEQTICKS_PER_BEAT );
            }
            p->samplesUntilTick -= 1.0;
            ++p->framesRendered;
        }

        float left = 0.0f, right = 0.0f, sendL = 0.0f, sendR = 0.0f;
        for( int i = 0; i < AUDIO_VOICES; ++i ) {
            AudioVoice *voice = &s_voices[i];
            if( !voice->active || voice->player >= SEQ_PLAYERS ) continue;
            SeqPlayer *p = &s_players[voice->player];
            if( p->paused ) continue;
            if( !voice_adsr_sample( voice, sampleRate )) continue;
            voice_portamento_sample( voice, sampleRate );

            float sample = 0.0f;
            bool valid = voice->oscillator ?
                ( sample = oscillator_sample( voice, sampleRate ), true ) :
                voice_pcm_sample( voice, sampleRate, &sample );
            if( !valid ) { voice->active = 0; continue; }

            float playerGain = p->seqVolume * p->userVolume * p->fadeGain;
            float gain = sample * voice->volume * voice->env * playerGain;
            float gainL = sqrtf( 1.0f - voice->pan );
            float gainR = sqrtf( voice->pan );
            left += gain * gainL;
            right += gain * gainR;
            sendL += gain * gainL * voice->reverb;
            sendR += gain * gainR * voice->reverb;
        }

        uint32_t rp = s_reverbPos * 2u;
        float wetL = s_reverb[rp], wetR = s_reverb[rp + 1];
        s_reverb[rp] = sendL * 0.35f + wetL * 0.62f;
        s_reverb[rp + 1] = sendR * 0.35f + wetR * 0.62f;
        s_reverbPos = ( s_reverbPos + 1u ) % REVERB_FRAMES;
        left = ( left + wetL * 0.28f ) * s_masterVolume;
        right = ( right + wetR * 0.28f ) * s_masterVolume;
        stereo[frame * 2u] = left < -1.0f ? -1.0f : left > 1.0f ? 1.0f : left;
        stereo[frame * 2u + 1] = right < -1.0f ? -1.0f : right > 1.0f ? 1.0f : right;
    }
    return frames;
}

void liboot_audio_sequence_reset( void )
{
    memset( s_players, 0, sizeof( s_players ));
    memset( s_voices, 0, sizeof( s_voices ));
    memset( s_reverb, 0, sizeof( s_reverb ));
    memset( s_sfxRotation, 0, sizeof( s_sfxRotation ));
    sfx_channel_state_reset();
    s_reverbPos = 0;
    s_voiceSerial = 1;
    s_random = 0x12345678u;
    s_masterVolume = 0.85f;
    for( uint8_t p = 0; p < SEQ_PLAYERS; ++p ) {
        s_players[p].index = p;
        s_players[p].userVolume = 1.0f;
        for( int c = 0; c < SEQ_CHANNELS; ++c ) channel_reset( &s_players[p].channels[c], 0 );
    }
}

/* -------------------------------------------------------------------------- */
/* liboot vNEXT: proximity-driven enemy/battle BGM.                           */
/*                                                                            */
/* OoT already decides "enemy near": the vendored z_actor.c tracks the        */
/* nearest hostile actor within SQ(500) as actorCtx.attention.bgmEnemy, and   */
/* the vendored z_player.c calls Audio_SetBgmEnemyVolume(dist) each tick one   */
/* is in range. liboot's shim forwards that distance to liboot_enemy_bgm_     */
/* signal(); this driver, run once per oot_link_tick, plays the battle        */
/* sequence on a dedicated player while an enemy is in range, scales its      */
/* volume with proximity, and fades it out when none remain. Opt-in.          */

#define LIBOOT_BGM_ENEMY    0x1Cu   /* NA_BGM_ENEMY (sequence_table.h Sequence_26) */
#define ENEMY_BGM_RANGE     500.0f  /* sqrt of OoT's SQ(500) battle range */

static struct {
    bool     enabled;
    uint8_t  player;    /* SEQ player carrying the battle theme */
    uint16_t seqId;     /* battle sequence id */
    uint16_t fadeMs;    /* fade in/out */
    bool     signaled;  /* Audio_SetBgmEnemyVolume fired this tick */
    float    tickDist;  /* nearest in-range enemy distance accumulated this tick */
    float    lastDist;  /* distance reported by the most recent tick (for the getter) */
    bool     active;    /* battle sequence currently playing */
} s_enemyBgm = { false, OOT_AUDIO_PLAYER_SUB, LIBOOT_BGM_ENEMY, 400u,
                 false, ENEMY_BGM_RANGE, ENEMY_BGM_RANGE, false };

void liboot_enemy_bgm_signal( float dist )
{
    s_enemyBgm.signaled = true;
    if( dist < s_enemyBgm.tickDist ) s_enemyBgm.tickDist = dist; /* nearest wins */
}

void liboot_enemy_bgm_tick( void )
{
    bool  enemyNear = s_enemyBgm.signaled;
    float dist      = enemyNear ? s_enemyBgm.tickDist : ENEMY_BGM_RANGE;

    /* Reset the per-tick heartbeat before the next Player update. */
    s_enemyBgm.signaled = false;
    s_enemyBgm.tickDist = ENEMY_BGM_RANGE;
    s_enemyBgm.lastDist = dist;

    if( !s_enemyBgm.enabled ) {
        if( s_enemyBgm.active ) {                 /* disabled mid-battle: fade out */
            oot_audio_sequence_stop( s_enemyBgm.player, s_enemyBgm.fadeMs );
            s_enemyBgm.active = false;
        }
        return;
    }

    if( enemyNear && !s_enemyBgm.active ) {
        s_enemyBgm.active =
            oot_audio_sequence_play( s_enemyBgm.player, s_enemyBgm.seqId, s_enemyBgm.fadeMs );
    } else if( !enemyNear && s_enemyBgm.active ) {
        oot_audio_sequence_stop( s_enemyBgm.player, s_enemyBgm.fadeMs );
        s_enemyBgm.active = false;
    }

    if( s_enemyBgm.active ) {
        /* Louder as the enemy closes in: full at contact, ~0.35 at max range. */
        float t = dist / ENEMY_BGM_RANGE;
        if( t < 0.0f ) t = 0.0f; else if( t > 1.0f ) t = 1.0f;
        oot_audio_sequence_set_volume( s_enemyBgm.player, 1.0f - 0.65f * t );
    }
}

bool oot_audio_set_enemy_bgm( bool enabled, uint8_t player, uint16_t seqId, uint16_t fadeMs )
{
    if( player == 0xFFu ) player = OOT_AUDIO_PLAYER_SUB;   /* 0xFF keeps the default player */
    if( player >= SEQ_PLAYERS ) return false;
    if( seqId != 0u && seqId >= OOT_AUDIO_SEQUENCE_COUNT ) return false;

    s_enemyBgm.player = player;
    s_enemyBgm.seqId  = ( seqId != 0u ) ? seqId : LIBOOT_BGM_ENEMY; /* 0 keeps NA_BGM_ENEMY */
    s_enemyBgm.fadeMs = fadeMs;
    s_enemyBgm.enabled = enabled;
    return true;
}

bool oot_audio_get_enemy_bgm( bool *outActive, float *outDistance )
{
    if( outActive )   *outActive = s_enemyBgm.active;
    if( outDistance ) *outDistance = s_enemyBgm.lastDist;
    return s_enemyBgm.enabled;
}
