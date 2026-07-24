#ifndef LIBOOT_AUDIO_EXTRACT_H
#define LIBOOT_AUDIO_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Internal, ROM-backed audio catalogue.  All pointers remain valid until
   liboot_audio_terminate(); callers must not free or modify them. */
typedef struct {
    const uint8_t *data;
    size_t size;
    const uint8_t *fontIds;
    uint16_t requestedId;
    uint16_t resolvedId;
    uint8_t numFonts;
    uint8_t cachePolicy;
} LibootAudioSequenceView;

typedef struct {
    const uint8_t *data;
    size_t size;
    uint16_t fontId;
    uint16_t numSoundEffects;
    uint8_t sampleBankId1;
    uint8_t sampleBankId2;
    uint8_t numInstruments;
    uint8_t numDrums;
    uint8_t cachePolicy;
} LibootAudioSoundFontView;

typedef struct {
    const uint8_t *envelope;
    size_t size;
    uint8_t decayIndex;
} LibootAudioAdsrView;

typedef struct {
    const int16_t *pcm;
    const uint8_t *tunedSample;
    uint32_t numSamples;
    uint32_t sampleRate;
    uint32_t loopStart;
    uint32_t loopCount;
    LibootAudioAdsrView adsr;
    uint8_t pan;
    uint8_t hasPan;
} LibootAudioSampleView;

void liboot_audio_init( const uint8_t *rom, size_t romSize );
void liboot_audio_terminate( void );

/* liboot vNEXT: proximity-driven enemy/battle BGM. The vendored Player code
   calls Audio_SetBgmEnemyVolume() every tick a hostile enemy is within OoT's
   500-unit battle range of Link; the shim forwards that distance through
   liboot_enemy_bgm_signal(). liboot_enemy_bgm_tick() runs once per
   oot_link_tick to start, stop, and volume-scale the battle sequence. */
void liboot_enemy_bgm_signal( float dist );
void liboot_enemy_bgm_tick( void );

uint16_t liboot_audio_sequence_count( void );
uint16_t liboot_audio_soundfont_count( void );
uint16_t liboot_audio_samplebank_count( void );

bool liboot_audio_sequence_get( uint16_t sequenceId, LibootAudioSequenceView *out );
bool liboot_audio_soundfont_get( uint16_t fontId, LibootAudioSoundFontView *out );

/* Decode/cache every sample reachable from the sequence's soundfonts.  Call
   this before entering a real-time audio callback. */
bool liboot_audio_prewarm_sequence( uint16_t sequenceId );
bool liboot_audio_prewarm_soundfont( uint16_t fontId );

/* PCM getters are lookup-only: they never allocate or decode.  A matching
   sequence/soundfont must have been prewarmed first (font 0 is prewarmed at
   init for compatibility with the existing public SFX/ocarina helpers). */
bool liboot_audio_instrument_sample_get( uint16_t fontId, uint8_t instrumentId,
                                         uint8_t semitone, LibootAudioSampleView *out );
bool liboot_audio_instrument_sample_get_for_pitch( uint16_t fontId,
                                                   uint8_t instrumentId,
                                                   uint8_t sampleSemitone,
                                                   uint8_t pitchSemitone,
                                                   LibootAudioSampleView *out );
bool liboot_audio_instrument_adsr_get( uint16_t fontId, uint8_t instrumentId,
                                      LibootAudioAdsrView *out );
bool liboot_audio_drum_sample_get( uint16_t fontId, uint8_t drumId,
                                   LibootAudioSampleView *out );
bool liboot_audio_font_sfx_sample_get( uint16_t fontId, uint16_t sfxIndex,
                                       LibootAudioSampleView *out );

#endif
