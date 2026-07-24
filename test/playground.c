/* liboot feature playground: an interactive workbench for every public API.
 *
 *   WASD move   SPACE jump/roll (A)   J attack (B)   L shield (R)   K Z-target
 *   1..4 sword: none/Kokiri/Master/Biggoron
 *   5 bow   6 bomb   7 hookshot   8 boomerang (child)   I = item button
 *   SHIFT+5/6/7 shield: Deku/Hylian/Mirror   SHIFT+8 9 0 tunic: Kokiri/Goron/Zora
 *   F1 F2 F3 boots: Kokiri/Iron/Hover
 *   T toggle adult/child   E ocarina out/away    H hurt self   M fill magic
 *   R respawn Link         ESC quit       right-mouse camera / wheel zoom
 *   F5 arena/scene   F6 cycle scene   [/] room   F9/TAB workbench   F11 diagnostics
 *
 * The arena includes stairs, ramps, a balance beam, a deep-water basin and a
 * hookshot tower. The enemy cube chases Link: sword swings within reach damage it, its lunge
 * damages Link (real Player_InflictDamage). Kill it, or die trying. The v0.6
 * projectiles (real EnArrow/EnBom/ArmsHook/EnBoom actors) render through the
 * shared geometry buffers (oot_actor_set_render) and damage the Stalchild:
 * arrows/boomerang on contact, bombs in a 100-unit blast radius.
 */
#include <SDL2/SDL.h>
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "liboot.h"
#include "playground_ui.h"

#define ARENA       900.0f
#define WALL_H      200.0f
/* NE-corner pool: 500x400 carve, floor at -120, ramp back up on the west
   side, real WaterBox surface at -10 (integer world units, shared between
   collision load and rendering) */
#define POOL_X0     350
#define POOL_X1     850
#define POOL_Z0     350
#define POOL_Z1     750
#define POOL_FLOOR  (-120)
#define POOL_RAMP_X 550     /* ramp: y=0 at POOL_X0 down to POOL_FLOOR here */
#define WATER_Y     (-10)
#define SIM_DT_MS   50          /* the real game updates gameplay at 20 Hz */
#define AUDIO_RATE  48000
#define PI_F        3.14159265358979323846f
#define J_L_HAND    15          /* pose joint indices (PLAYER_LIMB_* - 1)   */
#define J_L_FOREARM 14
#define J_HEAD      10

static struct OoTLinkInputs gIn;
static struct OoTLinkState gSt;
static struct OoTSkeletonPose gPose;
static int gPoseValid;
static float gGeoPos[OOT_GEO_MAX_TRIANGLES*9], gGeoNrm[OOT_GEO_MAX_TRIANGLES*9],
             gGeoCol[OOT_GEO_MAX_TRIANGLES*9], gGeoUv[OOT_GEO_MAX_TRIANGLES*6];
static uint16_t gGeoTex[OOT_GEO_MAX_TRIANGLES];
static float gGeoAlpha[OOT_GEO_MAX_TRIANGLES*3];   /* liboot vNEXT: per-vertex shade alpha */
static uint8_t gGeoFlags[OOT_GEO_MAX_TRIANGLES];   /* liboot vNEXT: per-triangle render flags */
static struct OoTLinkGeometryBuffers gGeo = {
    gGeoPos, gGeoNrm, gGeoCol, gGeoUv, gGeoTex, 0, gGeoAlpha, gGeoFlags };
static GLuint *gGlTex;
static uint32_t *gGlTexRevision;
static int gGlTexCapacity, gGlTexCount;

static int reserve_gl_textures( int count )
{
    if( count <= gGlTexCapacity ) return 1;
    int capacity = gGlTexCapacity ? gGlTexCapacity : 128;
    while( capacity < count ) {
        if( capacity > INT_MAX / 2 ) return 0;
        capacity *= 2;
    }
    if((size_t)capacity > SIZE_MAX / sizeof( *gGlTex ) ||
       (size_t)capacity > SIZE_MAX / sizeof( *gGlTexRevision )) return 0;
    GLuint *textures = calloc((size_t)capacity, sizeof( *textures ));
    uint32_t *revisions = calloc((size_t)capacity, sizeof( *revisions ));
    if( !textures || !revisions ) {
        free( textures );
        free( revisions );
        return 0;
    }
    if( gGlTexCount > 0 ) {
        memcpy( textures, gGlTex, (size_t)gGlTexCount * sizeof( *textures ));
        memcpy( revisions, gGlTexRevision, (size_t)gGlTexCount * sizeof( *revisions ));
    }
    free( gGlTex );
    free( gGlTexRevision );
    gGlTex = textures;
    gGlTexRevision = revisions;
    gGlTexCapacity = capacity;
    return 1;
}

static int gl_texture_ready( int index )
{
    return index >= 0 && index < gGlTexCount && gGlTex && gGlTex[index] != 0;
}

static void discard_gl_texture( int index )
{
    if( index < 0 || index >= gGlTexCapacity || !gGlTex ) return;
    if( gGlTex[index] ) glDeleteTextures( 1, &gGlTex[index] );
    gGlTex[index] = 0;
    gGlTexRevision[index] = 0;
}

static void upload_new_textures( void )
{
    int n = oot_get_texture_count();
    if( n < 0 || !reserve_gl_textures( n )) {
        fprintf( stderr, "[gfx] cannot reserve texture cache for %d entries\n", n );
        return;
    }
    GLint maxTextureSize = 0;
    glGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxTextureSize );
    for( int i = 0; i < n; ++i ) {
        struct OoTTextureInfo ti;
        const uint8_t *px;
        if( !oot_get_texture( i, &ti, &px )) {
            discard_gl_texture( i );
            continue;
        }
        if( gGlTex[i] && gGlTexRevision[i] == ti.revision ) continue;
        if( !px || !ti.width || !ti.height || maxTextureSize <= 0 ||
            ti.width > maxTextureSize || ti.height > maxTextureSize ) {
            fprintf( stderr, "[gfx] texture %d has invalid dimensions/data\n", i );
            discard_gl_texture( i );
            continue;
        }
        while( glGetError() != GL_NO_ERROR ) {}
        if( !gGlTex[i] ) glGenTextures( 1, &gGlTex[i] );
        if( !gGlTex[i] ) {
            fprintf( stderr, "[gfx] glGenTextures failed for texture %d\n", i );
            continue;
        }
        glBindTexture( GL_TEXTURE_2D, gGlTex[i] );
        glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, ti.width, ti.height, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, px );
        GLint ws = ti.wrapS == 1 ? GL_MIRRORED_REPEAT : ti.wrapS == 2 ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        GLint wt = ti.wrapT == 1 ? GL_MIRRORED_REPEAT : ti.wrapT == 2 ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
        GLenum error = glGetError();
        if( error != GL_NO_ERROR ) {
            fprintf( stderr, "[gfx] texture %d upload failed (GL error 0x%04x)\n",
                     i, (unsigned)error );
            discard_gl_texture( i );
            continue;
        }
        gGlTexRevision[i] = ti.revision;
    }
    for( int i = n; i < gGlTexCount; ++i ) {
        discard_gl_texture( i );
    }
    gGlTexCount = n;
}

static void release_gl_textures( void )
{
    if( gGlTex ) {
        for( int i = 0; i < gGlTexCount; ++i )
            if( gGlTex[i] ) glDeleteTextures( 1, &gGlTex[i] );
    }
    free( gGlTex );
    free( gGlTexRevision );
    gGlTex = NULL;
    gGlTexRevision = NULL;
    gGlTexCapacity = gGlTexCount = 0;
}

static void forget_gl_textures( void )
{
    free( gGlTex );
    free( gGlTexRevision );
    gGlTex = NULL;
    gGlTexRevision = NULL;
    gGlTexCapacity = gGlTexCount = 0;
}
static int gShowSkeleton;
static int32_t gLink = -1;
static int gOcarinaOut, gDeadTimer, gKills;
static int gLibootInitialized, gFatalError;

/* Authoritative host-side loadout. Link recreation, scene changes and age
   changes all restore this state instead of silently going back to defaults. */
static uint8_t gSword = OOT_SWORD_MASTER;
static uint8_t gShield = OOT_SHIELD_HYLIAN;
static uint8_t gTunic = OOT_TUNIC_KOKIRI;
static uint8_t gBoots = OOT_BOOTS_KOKIRI;

/* Interactive feature-lab state. None of this is touched by the headless
   simulation paths except the deterministic counters. */
enum PgCameraMode { PG_CAM_ORBIT, PG_CAM_CHASE, PG_CAM_TARGET, PG_CAM_COUNT };
enum PgTab { PG_TAB_PLAY, PG_TAB_LINK, PG_TAB_ITEMS, PG_TAB_WORLD,
             PG_TAB_AUDIO, PG_TAB_RENDER, PG_TAB_HELP, PG_TAB_COUNT };

static int gWorkbenchOpen, gHudVisible = 1, gDiagnosticsVisible;
static int gWorkbenchTab, gWorkbenchRow;
static int gSnapshotRefresh;
static int gManualPaused, gSingleStep, gEnemyAi = 1;
static int gShowCollision, gShowActorBounds, gShowSceneMesh = 1;
static int gActorMeshesEnabled = 1, gNaviMeshEnabled = 1;
/* backface culling from per-triangle flags: 0 = off (draw both sides),
   1 = cull, front=CCW, 2 = cull, front=CW. B key cycles it (the emitted winding
   should make CCW correct; CW is the escape hatch if a build looks inside-out). */
static int gCullMode = 1;
static const char *const kCullNames[] = { "OFF", "ON (CCW front)", "ON (CW front)" };
static int gCameraMode = PG_CAM_ORBIT, gMouseLook;
static float gCameraYaw, gCameraPitch = 0.30f, gCameraDistance = 330.0f;
static float gMasterVolume = 1.0f, gTimeScale = 1.0f;
static float gRenderFps, gFrameMs;
static uint32_t gWallClockMs;
static uint64_t gSimTicks;
static int gCatchupLast;
static int gItemChoice = OOT_ITEM_BOW;
static int gWorldChoice;
static int gArenaSpawnChoice;
static uint8_t gLatchA, gLatchB, gLatchZ, gLatchR, gLatchItem;

#define PG_TOASTS 5
static struct { char text[112]; uint32_t until; } gToasts[PG_TOASTS];
static int gToastHead;

#define PG_SFX_LOG 10
static struct {
    uint16_t id;
    uint8_t action, refresh, hasSample;
    float volume, pitch;
    uint64_t tick;
} gSfxLog[PG_SFX_LOG];
static int gSfxLogHead;

static void pg_notify( const char *format, ... )
{
    char *dst = gToasts[gToastHead].text;
    va_list args;
    va_start( args, format );
    vsnprintf( dst, sizeof( gToasts[gToastHead].text ), format, args );
    va_end( args );
    dst[sizeof( gToasts[gToastHead].text ) - 1] = '\0';
    gToasts[gToastHead].until = gWallClockMs + 4200;
    gToastHead = ( gToastHead + 1 ) % PG_TOASTS;
}

/* ---- v0.6 items: real projectile actors --------------------------------- */
#define SFX_ARROW_SHOT      0x1804      /* NA_SE_IT_ARROW_SHOT */
#define SFX_BOOMERANG_THROW 0x1805      /* NA_SE_IT_BOOMERANG_THROW */
#define SFX_BOMB_EXPLOSION  0x180E      /* NA_SE_IT_BOMB_EXPLOSION */
#define AID_EN_BOM          0x10
#define AID_EN_ARROW        0x16
#define AID_EN_BOOM         0x32
#define STATE1_BOOMERANG_THROWN (1u << 25)

static uint8_t gItemOut;                /* OOT_ITEM_* currently on the item button */
static int gArrowAmmo = 30, gBombAmmo = 20;
static struct OoTActorInfo gActors[64];
static int gActorN;
static float gLastBombPos[3];           /* last live EN_BOM position (flash anchor) */
static int gBombAlive, gBoomAlive;
static int gExplodePending;             /* set by sfx_cb mid-tick, drained after */
static struct { int timer; float pos[3]; } gFlash;   /* explosion billboard, ~20 frames */

typedef enum { FOE_CHASE, FOE_WINDUP, FOE_RECOVER, FOE_HURT, FOE_DEAD } FoeState;
static struct {
    float x, y, z, yaw;
    int hp, timer, invuln;
    FoeState state;
} gFoe;
static int32_t gFoeTarget = -1;     /* attention-target id for the Stalchild */
static uint8_t gPrevLockOn;

/* the foe dies: unregister its lock-on target so the game's own dead-actor
   path releases Link's Z-lock next tick */
static void foe_die( void )
{
    gFoe.state = FOE_DEAD;
    gFoe.timer = 100;
    gKills++;
    if( gFoeTarget >= 0 ) { oot_target_remove( gFoeTarget ); gFoeTarget = -1; }
    printf( "[foe] Stalchild destroyed! kills: %d\n", gKills );
}

/* a projectile connects: same damage path as the sword check in foe_tick
   (FOE_HURT knockback; foe_tick's HURT timer calls foe_die at hp <= 0) */
static void foe_projectile_hit( int dmg, const char *what )
{
    if( gFoe.state == FOE_DEAD || gFoe.invuln > 0 ) return;
    gFoe.hp -= dmg;
    gFoe.invuln = 16;
    gFoe.state = FOE_HURT;
    gFoe.timer = 10;
    printf( "[foe] %s hits the Stalchild! %d hp left\n", what, gFoe.hp );
}

/* ---- ocarina synth + voice sample mixer -------------------------------- */
static float gNoteFreq, gNotePhase, gNoteEnv;
/* live song recognition via the real oot_ocarina_match API: a sliding window of
   the last note indices (0..4) played, and the last song it matched (-1 none). */
static uint8_t gNoteIdx[8];
static int gNoteIdxLen;
static int gLastSong = -1;
static SDL_AudioDeviceID gAudioDev;
static int gAudioInitialized;
static int gSceneMusicAuto = 1;
static int gMusicSequenceChoice = 28; /* INSIDE_DEKU_TREE: audible manual default */
static int gMusicActiveSequence = -1;
static int gMusicPlaying;
static int gNatureChoice;
static int gAmbiencePreset = -1;
static int gAmbiencePlaying;
static float gMusicVolume = 1.0f;
static int gSfxCatalogChoice;

#define PG_AMBIENCE_NONE 0x13

static uint32_t audio_step( uint32_t sampleRate, float scale )
{
    if( !isfinite( scale ) || scale <= 0.0f ) scale = 1.0f;
    double step = (double)sampleRate * scale * 65536.0 / AUDIO_RATE;
    if( step < 1.0 ) return 1;
    if( step > UINT32_MAX ) return UINT32_MAX;
    return (uint32_t)step;
}

static float audio_gain( float volume )
{
    if( !isfinite( volume ) || volume <= 0.0f ) return 0.0f;
    if( volume > 4.0f ) volume = 4.0f;
    return 0.85f * volume;
}

/* real ocarina instrument: one shared ROM PCM clip played at per-note rates
   (oot_get_ocarina_note); loop-sustains [loopStart,len) while the key is
   held, ~50 ms fade on release. The additive synth below stays as fallback
   when the ROM's audio tables were not extracted. */
static struct {
    const int16_t *pcm;
    uint32_t len, loopStart;
    uint64_t pos1616;        /* 16.16 resampling position */
    uint32_t step1616;
    int held;                /* loop-sustain while nonzero */
    float rel;               /* release gain, fades ~50 ms after key-up */
} gOcNote;

#define VOICE_CH 4
static struct {
    const int16_t *pcm;
    uint32_t len;
    uint64_t pos1616;        /* playback position 16.16 for resampling
                                (64-bit: clips over 65535 samples must not wrap) */
    uint32_t step1616;
    float gain;
    uint16_t sfxId;
    float position[3];
} gVoice[VOICE_CH];

static void audio_cb( void *ud, Uint8 *stream, int len )
{
    if( !stream || len <= 0 ) return;
    SDL_memset( stream, 0, (size_t)len );
    float *out = (float *)stream;
    int frames = len / (int)( sizeof( *out ) * 2 );
    (void)ud;
    /* The library owns the sequenced-music/SFX mix. It writes interleaved
       stereo F32 and performs no allocation from this real-time callback. */
    oot_audio_render_f32( out, (uint32_t)frames, AUDIO_RATE );
    for( int i = 0; i < frames; ++i ) {
        float v = 0.0f;
        /* real ocarina PCM: loop-sustain while held, short fade on release */
        if( gOcNote.pcm ) {
            uint64_t idx = gOcNote.pos1616 >> 16;
            if( idx >= gOcNote.len ) {
                if( gOcNote.held && gOcNote.loopStart < gOcNote.len ) {
                    uint64_t loopStart = (uint64_t)gOcNote.loopStart << 16;
                    uint64_t loopLength = (uint64_t)( gOcNote.len - gOcNote.loopStart ) << 16;
                    gOcNote.pos1616 = loopStart + ( gOcNote.pos1616 - loopStart ) % loopLength;
                    idx = gOcNote.pos1616 >> 16;
                } else
                    gOcNote.pcm = NULL;
            }
            if( gOcNote.pcm ) {
                v += gOcNote.pcm[idx] / 32768.0f * 0.6f * gOcNote.rel;
                gOcNote.pos1616 += gOcNote.step1616;
                if( !gOcNote.held ) {
                    gOcNote.rel -= 1.0f / ( 0.05f * AUDIO_RATE );   /* ~50 ms */
                    if( gOcNote.rel <= 0.0f ) gOcNote.pcm = NULL;
                }
            }
        }
        /* synth fallback (only fed when oot_get_ocarina_note fails) */
        float target = gNoteFreq > 0 ? 0.22f : 0.0f;
        gNoteEnv += ( target - gNoteEnv ) * 0.0015f;
        if( gNoteFreq > 0 )
            gNotePhase += gNoteFreq * ( 2.0f * PI_F / AUDIO_RATE );
        if( gNotePhase > 2.0f * PI_F ) gNotePhase -= 2.0f * PI_F;
        v += ( sinf( gNotePhase ) + 0.28f * sinf( gNotePhase * 2 )
                    + 0.08f * sinf( gNotePhase * 3 )) * gNoteEnv;
        for( int c = 0; c < VOICE_CH; ++c ) {
            if( !gVoice[c].pcm ) continue;
            uint64_t idx = gVoice[c].pos1616 >> 16;
            if( idx >= gVoice[c].len ) { gVoice[c].pcm = NULL; gVoice[c].sfxId = 0; continue; }
            v += gVoice[c].pcm[idx] / 32768.0f * gVoice[c].gain;
            gVoice[c].pos1616 += gVoice[c].step1616;
        }
        /* Legacy gameplay voices and the ocarina are mono sources. Mix them
           equally into both channels after the library's stereo render. */
        float left = ( out[i * 2] + v ) * gMasterVolume;
        float right = ( out[i * 2 + 1] + v ) * gMasterVolume;
        if( !isfinite( left )) left = 0.0f;
        if( !isfinite( right )) right = 0.0f;
        if( left > 1.0f ) left = 1.0f;
        else if( left < -1.0f ) left = -1.0f;
        if( right > 1.0f ) right = 1.0f;
        else if( right < -1.0f ) right = -1.0f;
        out[i * 2] = left;
        out[i * 2 + 1] = right;
    }
}

/* every sfx request the real Player code makes lands here; play the ROM's
   own sample when the extractor has one */
static void sfx_cb( const struct OoTSfxEvent *event )
{
    if( !event ) return;
    uint16_t sfxId = event->sfxId;
    const int16_t *pcm; uint32_t numSamples, rate;
    int logIndex = gSfxLogHead;
    gSfxLog[logIndex].id = sfxId;
    gSfxLog[logIndex].action = event->action;
    gSfxLog[logIndex].refresh = event->isRefresh;
    gSfxLog[logIndex].hasSample = 0;
    gSfxLog[logIndex].volume = event->volume;
    gSfxLog[logIndex].pitch = event->freqScale;
    gSfxLog[logIndex].tick = gSimTicks;
    gSfxLogHead = ( gSfxLogHead + 1 ) % PG_SFX_LOG;
    if( event->action == OOT_SFX_STOP_ID || event->action == OOT_SFX_STOP_POSITION ) {
        uint16_t canonical = sfxId | 0x0800u;
        if( gAudioDev ) SDL_LockAudioDevice( gAudioDev );
        oot_audio_sfx_stop( sfxId );
        for( int c = 0; c < VOICE_CH; ++c ) {
            int samePosition = fabsf( gVoice[c].position[0] - event->position[0] ) < 0.01f &&
                               fabsf( gVoice[c].position[1] - event->position[1] ) < 0.01f &&
                               fabsf( gVoice[c].position[2] - event->position[2] ) < 0.01f;
            if(( event->action == OOT_SFX_STOP_ID && gVoice[c].sfxId == canonical ) ||
               ( event->action == OOT_SFX_STOP_POSITION && samePosition )) {
                gVoice[c].pcm = NULL;
                gVoice[c].sfxId = 0;
            }
        }
        if( gAudioDev ) SDL_UnlockAudioDevice( gAudioDev );
        return;
    }
    if( event->action != OOT_SFX_PLAY ) return;
    if( sfxId >= 0x6840 && sfxId <= 0x684F )   /* NA_SE_VO_NAVY_* */
        printf( "[navi] voice event 0x%04x%s\n", sfxId,
                sfxId <= 0x6842 ? " (retail-silent)" : "" );
    if( sfxId == SFX_ARROW_SHOT ) {
        if( gArrowAmmo > 0 ) gArrowAmmo--;
        printf( "[item] arrow fired! (%d/30 left)\n", gArrowAmmo );
    }
    if( sfxId == SFX_BOOMERANG_THROW )
        puts( "[item] boomerang away!" );
    if( sfxId == SFX_BOMB_EXPLOSION )
        gExplodePending = 1;    /* fires mid-oot_link_tick: drained by
                                   projectile_tick, no library re-entry here */
    if( !gAudioDev ) return;   /* no device: LockAudioDevice(0) is a silent no-op */
    if( !oot_get_voice_sample( sfxId, &pcm, &numSamples, &rate ) ||
        !pcm || !numSamples || !rate ) {
        /* The compatibility PCM map is intentionally curated. Route every
           other retail event through Sequence 0, which covers all seven SFX
           banks used by the workbench selector. Refreshes update an existing
           gameplay sound and must not stack a fresh instance each tick. */
        if( !event->isRefresh ) {
            SDL_LockAudioDevice( gAudioDev );
            gSfxLog[logIndex].hasSample =
                oot_audio_sfx_play( sfxId, 0.0f, audio_gain( event->volume ));
            SDL_UnlockAudioDevice( gAudioDev );
        }
        return;
    }
    gSfxLog[logIndex].hasSample = 1;
    SDL_LockAudioDevice( gAudioDev );
    if( event->isRefresh ) {
        uint16_t canonical = sfxId | 0x0800u;
        for( int c = 0; c < VOICE_CH; ++c ) {
            if( gVoice[c].pcm && gVoice[c].sfxId == canonical ) {
                gVoice[c].step1616 = audio_step( rate, event->freqScale );
                gVoice[c].gain = audio_gain( event->volume );
                memcpy( gVoice[c].position, event->position, sizeof( gVoice[c].position ));
                SDL_UnlockAudioDevice( gAudioDev );
                return;
            }
        }
    }
    int slot = -1;
    uint64_t shortest = UINT64_MAX;
    for( int c = 0; c < VOICE_CH; ++c ) {
        if( !gVoice[c].pcm ) { slot = c; break; }
        uint64_t end = (uint64_t)gVoice[c].len << 16;
        uint64_t remaining = gVoice[c].pos1616 < end ? end - gVoice[c].pos1616 : 0;
        uint64_t frames = remaining / ( gVoice[c].step1616 ? gVoice[c].step1616 : 1 );
        if( frames < shortest ) { shortest = frames; slot = c; }
    }
    gVoice[slot].pcm = pcm;
    gVoice[slot].len = numSamples;
    gVoice[slot].pos1616 = 0;
    gVoice[slot].step1616 = audio_step( rate, event->freqScale );
    gVoice[slot].gain = audio_gain( event->volume );
    gVoice[slot].sfxId = sfxId | 0x0800u;
    memcpy( gVoice[slot].position, event->position, sizeof( gVoice[slot].position ));
    SDL_UnlockAudioDevice( gAudioDev );
}

/* Open the audio device, falling back through SDL's backends; make failure
   impossible to miss and report the working driver + obtained spec. */
static void open_audio( void )
{
    SDL_AudioSpec want = { .freq = AUDIO_RATE, .format = AUDIO_F32SYS, .channels = 2,
                           .samples = 512, .callback = audio_cb };
    static const char *drivers[] = { "pipewire", "pulseaudio", "alsa" };
    char lastError[256] = "no SDL audio driver was available";

    for( unsigned attempt = 0; attempt <= sizeof( drivers ) / sizeof( drivers[0] ); ++attempt ) {
        const char *requested = attempt == 0 ? NULL : drivers[attempt - 1];
        if( SDL_AudioInit( requested ) != 0 ) {
            snprintf( lastError, sizeof( lastError ), "%s", SDL_GetError());
            fprintf( stderr, "[audio] driver %s: init failed: %s\n",
                     requested ? requested : "(default)", lastError );
            continue;
        }
        SDL_AudioSpec have = { 0 };
        gAudioDev = SDL_OpenAudioDevice( NULL, 0, &want, &have, 0 );
        if( gAudioDev ) {
            const char *active = SDL_GetCurrentAudioDriver();
            gAudioInitialized = 1;
            SDL_LockAudioDevice( gAudioDev );
            oot_audio_sequence_set_volume( OOT_AUDIO_PLAYER_MAIN, gMusicVolume );
            oot_audio_sequence_set_volume( OOT_AUDIO_PLAYER_SUB, gMusicVolume );
            SDL_UnlockAudioDevice( gAudioDev );
            printf( "[audio] device open: driver=%s %d Hz format=0x%04x ch=%d buf=%d samples\n",
                    active ? active : "(unknown)", have.freq, have.format,
                    have.channels, have.samples );
            puts( "[audio] if this stays silent, check system mutes:"
                  " wpctl set-mute @DEFAULT_AUDIO_SINK@ 0 (and the ALSA sink)" );
            return;
        }
        snprintf( lastError, sizeof( lastError ), "%s", SDL_GetError());
        fprintf( stderr, "[audio] driver %s: SDL_OpenAudioDevice failed: %s\n",
                 SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver()
                                             : ( requested ? requested : "(default)" ),
                 lastError );
        SDL_AudioQuit();
        gAudioInitialized = 0;
    }
    fprintf( stderr, "\n[audio] ************************************************************\n"
                     "[audio] ** NO AUDIO DEVICE COULD BE OPENED -- RUNNING SILENT       **\n"
                     "[audio] ** tried: default, pipewire, pulseaudio, alsa             **\n"
                     "[audio] ** last SDL error: %s\n"
                     "[audio] ************************************************************\n\n",
             lastError );
}

static void audio_lock( void )
{
    if( gAudioDev ) SDL_LockAudioDevice( gAudioDev );
}

static void audio_unlock( void )
{
    if( gAudioDev ) SDL_UnlockAudioDevice( gAudioDev );
}

static int audio_sequence_count_safe( void )
{
    int32_t count = oot_audio_sequence_count();
    return count > 0 ? count : 0;
}

static const char *audio_sequence_name_safe( int sequenceId )
{
    const char *name = sequenceId >= 0 && sequenceId < audio_sequence_count_safe()
        ? oot_audio_sequence_name((uint16_t)sequenceId ) : NULL;
    return name && name[0] ? name : "(unnamed)";
}

static void audio_music_play_selected( int notify )
{
    int count = audio_sequence_count_safe();
    if( count <= 0 ) {
        if( notify ) pg_notify( "NO AUDIO SEQUENCES EXTRACTED" );
        return;
    }
    if( gMusicSequenceChoice < 0 || gMusicSequenceChoice >= count )
        gMusicSequenceChoice = 0;
    audio_lock();
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SUB, 0 );
    int played = oot_audio_sequence_play( OOT_AUDIO_PLAYER_MAIN,
                                          (uint16_t)gMusicSequenceChoice, 0 );
    if( played )
        oot_audio_sequence_set_volume( OOT_AUDIO_PLAYER_MAIN, gMusicVolume );
    audio_unlock();
    if( played ) {
        gMusicActiveSequence = gMusicSequenceChoice;
        gMusicPlaying = 1;
        gAmbiencePlaying = 0;
        gAmbiencePreset = -1;
    }
    if( notify && played )
        pg_notify( "PLAY SEQ 0X%02X: %s", gMusicSequenceChoice,
                   audio_sequence_name_safe( gMusicSequenceChoice ));
    else if( notify )
        pg_notify( "SEQUENCE 0X%02X FAILED TO START", gMusicSequenceChoice );
}

static void audio_nature_play_selected( void )
{
    if( gNatureChoice < 0 || gNatureChoice >= (int)OOT_AUDIO_NATURE_COUNT )
        gNatureChoice = 0;
    audio_lock();
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SUB, 0 );
    int played = oot_audio_nature_play( OOT_AUDIO_PLAYER_MAIN,
                                        (uint8_t)gNatureChoice, 0 );
    if( played ) oot_audio_sequence_set_volume( OOT_AUDIO_PLAYER_MAIN, gMusicVolume );
    audio_unlock();
    gMusicPlaying = played;
    gMusicActiveSequence = played ? 1 : -1;
    gAmbiencePlaying = played;
    gAmbiencePreset = played ? gNatureChoice : -1;
    if( played ) pg_notify( "PLAY NATURE PRESET 0X%02X", gNatureChoice );
    else pg_notify( "NATURE PRESET 0X%02X FAILED", gNatureChoice );
}

static void audio_music_stop( int notify )
{
    audio_lock();
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_MAIN, 0 );
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SUB, 0 );
    audio_unlock();
    gMusicPlaying = 0;
    gMusicActiveSequence = -1;
    gAmbiencePlaying = 0;
    gAmbiencePreset = -1;
    if( notify ) pg_notify( "MUSIC STOPPED" );
}

static void audio_music_refresh_state( void )
{
    struct OoTAudioState mainState;
    memset( &mainState, 0, sizeof( mainState ));
    mainState.structSize = sizeof( mainState );
    mainState.version = OOT_AUDIO_STATE_VERSION;
    audio_lock();
    int mainValid = oot_audio_sequence_get_state( OOT_AUDIO_PLAYER_MAIN, &mainState );
    audio_unlock();
    if( mainValid ) {
        gMusicPlaying = mainState.playing != 0;
        gMusicActiveSequence = mainState.playing ? mainState.sequenceId : -1;
        gAmbiencePlaying = mainState.playing && mainState.sequenceId == 1 &&
                           gAmbiencePreset >= 0;
    }
}

/* Apply cmd 0x15 only after a scene load has committed. A failed scene load
   therefore leaves the currently playing track untouched. Room swaps never
   call this helper, matching OoT's scene-level sequence lifetime. */
static void audio_music_apply_scene( void )
{
    if( !gSceneMusicAuto ) return;
    int sequenceId = oot_scene_get_sequence_id();
    int ambienceId = oot_scene_get_ambience_id();
    int count = audio_sequence_count_safe();
    if( sequenceId >= 0 && sequenceId < count )
        gMusicSequenceChoice = sequenceId;

    int hasAmbience = ambienceId >= 0 && ambienceId < (int)OOT_AUDIO_NATURE_COUNT &&
                      ambienceId != PG_AMBIENCE_NONE;
    int useNature = hasAmbience &&
                    ( sequenceId == OOT_AUDIO_NO_MUSIC || sequenceId < 0 ||
                      sequenceId >= count );

    audio_lock();
    oot_audio_sequence_stop( OOT_AUDIO_PLAYER_SUB, 0 );
    if( useNature ) {
        int played = oot_audio_nature_play( OOT_AUDIO_PLAYER_MAIN,
                                            (uint8_t)ambienceId, 0 );
        if( played ) oot_audio_sequence_set_volume( OOT_AUDIO_PLAYER_MAIN, gMusicVolume );
        gMusicPlaying = played;
        gMusicActiveSequence = played ? 1 : -1;
        gAmbiencePlaying = played;
        gAmbiencePreset = played ? ambienceId : -1;
    } else if( sequenceId == OOT_AUDIO_NO_MUSIC || sequenceId < 0 || sequenceId >= count ) {
        oot_audio_sequence_stop( OOT_AUDIO_PLAYER_MAIN, 0 );
        gMusicPlaying = 0;
        gMusicActiveSequence = -1;
        gAmbiencePlaying = 0;
        gAmbiencePreset = -1;
    } else {
        int played = oot_audio_sequence_play( OOT_AUDIO_PLAYER_MAIN,
                                              (uint16_t)sequenceId, 0 );
        if( played )
            oot_audio_sequence_set_volume( OOT_AUDIO_PLAYER_MAIN, gMusicVolume );
        gMusicPlaying = played;
        gMusicActiveSequence = played ? sequenceId : -1;
        gAmbiencePlaying = 0;
        gAmbiencePreset = -1;
    }
    audio_unlock();

    if( useNature )
        pg_notify( "AUTO NATURE 0X%02X (REPLACES SCENE BGM)", ambienceId );
    else if( sequenceId == OOT_AUDIO_NO_MUSIC )
        pg_notify( "AUTO MUSIC: NO_MUSIC" );
    else if( sequenceId >= 0 && sequenceId < count )
        pg_notify( "AUTO SEQ 0X%02X: %s", sequenceId,
                   audio_sequence_name_safe( sequenceId ));
}

static int audio_sfx_catalog_count_safe( void )
{
    int32_t count = oot_audio_sfx_catalog_count();
    return count > 0 ? count : 0;
}

static int audio_sfx_info_at( int catalogIndex, struct OoTSfxInfo *out )
{
    int count = audio_sfx_catalog_count_safe();
    if( !out || catalogIndex < 0 || catalogIndex >= count ) return 0;
    memset( out, 0, sizeof( *out ));
    out->structSize = sizeof( *out );
    out->version = OOT_SFX_INFO_VERSION;
    return oot_audio_sfx_catalog_get( catalogIndex, out ) != 0;
}

static int audio_sfx_current( struct OoTSfxInfo *out )
{
    int count = audio_sfx_catalog_count_safe();
    if( count <= 0 ) return 0;
    gSfxCatalogChoice %= count;
    if( gSfxCatalogChoice < 0 ) gSfxCatalogChoice += count;
    return audio_sfx_info_at( gSfxCatalogChoice, out );
}

static const char *audio_sfx_bank_name( uint8_t bank )
{
    static const char *const names[] = {
        "PLAYER", "ITEM", "ENVIRONMENT", "ENEMY", "SYSTEM", "OCARINA", "VOICE"
    };
    return bank < sizeof( names ) / sizeof( names[0] ) ? names[bank] : "UNKNOWN";
}

static void audio_sfx_change_bank( int delta )
{
    struct OoTSfxInfo current, candidate;
    int count = audio_sfx_catalog_count_safe();
    if( !delta || count <= 1 || !audio_sfx_current( &current )) return;
    int direction = delta < 0 ? -1 : 1;
    int steps = delta < 0 ? -delta : delta;
    while( steps-- > 0 ) {
        int start = gSfxCatalogChoice;
        for( int distance = 1; distance < count; ++distance ) {
            int index = ( start + direction * distance ) % count;
            if( index < 0 ) index += count;
            if( audio_sfx_info_at( index, &candidate ) &&
                candidate.bank != current.bank ) {
                gSfxCatalogChoice = index;
                current = candidate;
                break;
            }
        }
    }
}

static void audio_sfx_change_entry( int delta )
{
    struct OoTSfxInfo current, candidate;
    int count = audio_sfx_catalog_count_safe();
    if( !delta || count <= 1 || !audio_sfx_current( &current )) return;
    int direction = delta < 0 ? -1 : 1;
    int steps = delta < 0 ? -delta : delta;
    while( steps-- > 0 ) {
        int start = gSfxCatalogChoice;
        for( int distance = 1; distance <= count; ++distance ) {
            int index = ( start + direction * distance ) % count;
            if( index < 0 ) index += count;
            if( audio_sfx_info_at( index, &candidate ) &&
                candidate.bank == current.bank ) {
                gSfxCatalogChoice = index;
                current = candidate;
                break;
            }
        }
    }
}

static void audio_sfx_play_selected( void )
{
    struct OoTSfxInfo info;
    if( !audio_sfx_current( &info )) {
        pg_notify( "NO SFX CATALOG EXTRACTED" );
        return;
    }
    audio_lock();
    int played = oot_audio_sfx_play( info.sfxId, 0.0f, 1.0f );
    audio_unlock();
    if( played )
        pg_notify( "PLAY SFX 0X%04X: %s", info.sfxId,
                   info.name[0] ? info.name : "(unnamed)" );
    else
        pg_notify( "SFX 0X%04X HAS NO PLAYABLE SAMPLE", info.sfxId );
}

static void audio_sfx_stop_all( void )
{
    audio_lock();
    oot_audio_sfx_stop_all();
    memset( gVoice, 0, sizeof( gVoice ));
    audio_unlock();
    pg_notify( "ALL SFX STOPPED" );
}

/* All twelve songs, indexed by the real OoTOcarinaSong enum (the six warp/
   teleport songs come first). Recognition is done live by oot_ocarina_match;
   `effect` drives the playground's little demo hooks (heal/stun/hurt). */
static const struct { const char *name; int effect; } gSongs[OOT_SONG_COUNT] = {
    [OOT_SONG_MINUET]   = { "Minuet of Forest (warp)",   0 },
    [OOT_SONG_BOLERO]   = { "Bolero of Fire (warp)",     0 },
    [OOT_SONG_SERENADE] = { "Serenade of Water (warp)",  0 },
    [OOT_SONG_REQUIEM]  = { "Requiem of Spirit (warp)",  0 },
    [OOT_SONG_NOCTURNE] = { "Nocturne of Shadow (warp)", 0 },
    [OOT_SONG_PRELUDE]  = { "Prelude of Light (warp)",   0 },
    [OOT_SONG_SARIAS]   = { "Saria's Song",              0 },
    [OOT_SONG_EPONAS]   = { "Epona's Song",              0 },
    [OOT_SONG_LULLABY]  = { "Zelda's Lullaby",           1 },
    [OOT_SONG_SUNS]     = { "Sun's Song",                2 },
    [OOT_SONG_TIME]     = { "Song of Time",              0 },
    [OOT_SONG_STORMS]   = { "Song of Storms",            3 },
};

static void ocarina_note( char btn, float freq )
{
    /* button -> oot_get_ocarina_note index: A, C-down, C-right, C-left, C-up */
    static const char btns[5] = { 'A', 'D', 'R', 'L', 'U' };
    const int16_t *pcm; uint32_t num, rate, loop;
    int idx = -1;
    for( int i = 0; i < 5; ++i ) if( btns[i] == btn ) idx = i;
    if( gAudioDev ) SDL_LockAudioDevice( gAudioDev );
    if( idx >= 0 && oot_get_ocarina_note( (uint8_t)idx, &pcm, &num, &rate, &loop ) &&
        pcm && num && rate ) {
        gOcNote.pcm = pcm;
        gOcNote.len = num;
        gOcNote.loopStart = loop;
        gOcNote.pos1616 = 0;
        gOcNote.step1616 = audio_step( rate, 1.0f );
        gOcNote.held = 1;
        gOcNote.rel = 1.0f;
        gNoteFreq = 0;                 /* real PCM plays: keep synth quiet */
    } else {
        gOcNote.pcm = NULL;
        gNoteFreq = freq;              /* synth fallback */
    }
    if( gAudioDev ) SDL_UnlockAudioDevice( gAudioDev );
    /* Feed the played note index into liboot's real recognizer. It matches the
       tail of the window, so every one of the twelve songs (warp songs too) is
       caught as soon as its pattern completes. */
    if( idx >= 0 ) {
        if( gNoteIdxLen < (int)sizeof gNoteIdx )
            gNoteIdx[gNoteIdxLen++] = (uint8_t)idx;
        else {
            memmove( gNoteIdx, gNoteIdx + 1, sizeof gNoteIdx - 1 );
            gNoteIdx[sizeof gNoteIdx - 1] = (uint8_t)idx;
        }
        int song = oot_ocarina_match( gNoteIdx, gNoteIdxLen );
        if( song >= 0 ) {
            gLastSong = song;
            gNoteIdxLen = 0;                     /* start fresh after a hit */
            printf( "[ocarina] you played %s!\n", gSongs[song].name );
            pg_notify( "SONG: %s", gSongs[song].name );
            if( gSongs[song].effect == 1 ) {        /* lullaby heals */
                oot_link_set_health( gLink, 0x60, 0 );
                puts( "[ocarina] ...you feel restored" );
            } else if( gSongs[song].effect == 2 ) { /* sun's song stuns */
                if( gFoe.state != FOE_DEAD ) { gFoe.state = FOE_RECOVER; gFoe.timer = 160; }
                puts( "[ocarina] ...the Stalchild freezes" );
            } else if( gSongs[song].effect == 3 ) { /* storms hurts it */
                if( gFoe.state != FOE_DEAD ) { gFoe.hp -= 2; gFoe.state = FOE_HURT; gFoe.timer = 12;
                    if( gFoe.hp <= 0 ) foe_die(); }
                puts( "[ocarina] ...thunder rolls" );
            }
        }
    }
}

static void ocarina_release( void )
{
    if( gAudioDev ) SDL_LockAudioDevice( gAudioDev );
    gNoteFreq = 0;
    gOcNote.held = 0;      /* audio_cb finishes the loop pass and fades out */
    if( gAudioDev ) SDL_UnlockAudioDevice( gAudioDev );
}
static float gCamPos[3] = { 0, 120, -260 };
static float gCameraAt[3] = { 0, 60, 0 };


static void foe_spawn( void )
{
    float a = (float)( rand() % 628 ) / 100.0f;
    gFoe.x = cosf( a ) * 400.0f;
    gFoe.z = sinf( a ) * 400.0f;
    gFoe.y = 0;
    gFoe.hp = 6;
    gFoe.state = FOE_CHASE;
    gFoe.timer = gFoe.invuln = 0;
    if( gFoeTarget >= 0 ) oot_target_remove( gFoeTarget );   /* respawn: drop stale target */
    gFoeTarget = oot_target_create( gFoe.x, gFoe.y, gFoe.z, 30.0f );
    if( gFoeTarget < 0 )
        fprintf( stderr, "[foe] no attention-target slot available; Z-lock disabled\n" );
    puts( "[foe] a Stalchild rises..." );
}

static int link_spawn_at( float x, float y, float z )
{
    int hadTarget = 0;
    if( gLink >= 0 ) {
        /* oot_link_delete despawns every non-Player actor and releases any
           attention-target pool slots, so the foe's target id dies with the
           old Link; re-create it after the respawn (also covers the
           R-respawn key). */
        if( gFoeTarget >= 0 ) { oot_target_remove( gFoeTarget ); gFoeTarget = -1; hadTarget = 1; }
        oot_link_delete( gLink );
    }
    gLink = oot_link_create( x, y, z );
    if( gLink < 0 ) {
        fprintf( stderr, "[link] create failed\n" );
        gFatalError = 1;
        return 0;
    }
    oot_link_set_health( gLink, 0x30, 0x60 );      /* 3 of 6 hearts */
    oot_link_set_magic( gLink, 1, 0x30 );
    oot_link_set_equipment( gLink, gSword, gShield, gTunic, gBoots );
    gOcarinaOut = 0;
    gItemOut = OOT_ITEM_NONE;
    gDeadTimer = 0;
    if( hadTarget ) {
        gFoeTarget = oot_target_create( gFoe.x, gFoe.y, gFoe.z, 30.0f );
        if( gFoeTarget < 0 )
            fprintf( stderr, "[foe] could not restore the Z-lock target\n" );
    }
    return 1;
}

static int link_spawn( void )
{
    return link_spawn_at( 0.0f, 0.0f, 0.0f );
}

/* item hotkeys 5..8: put the item on the item button / put it away again */
static void item_toggle( uint8_t item, const char *name )
{
    if( item == OOT_ITEM_NONE ) {
        gOcarinaOut = 0;
        gItemOut = OOT_ITEM_NONE;
        oot_link_use_item( gLink, OOT_ITEM_NONE );
        pg_notify( "ITEM CLEARED" );
        if( gWorkbenchOpen ) gSnapshotRefresh = 1;
        return;
    }
    if( item == OOT_ITEM_OCARINA ) {
        gOcarinaOut = !gOcarinaOut;
        gItemOut = gOcarinaOut ? OOT_ITEM_OCARINA : OOT_ITEM_NONE;
        oot_link_use_item( gLink, gItemOut );
        pg_notify( "OCARINA %s", gOcarinaOut ? "OUT" : "AWAY" );
        if( gWorkbenchOpen ) gSnapshotRefresh = 1;
        return;
    }
    if( gOcarinaOut ) {
        gOcarinaOut = 0;
        gItemOut = OOT_ITEM_NONE;
        oot_link_use_item( gLink, OOT_ITEM_NONE );
        puts( "[link] ocarina away" );
    }
    if( gItemOut == item ) {
        gItemOut = OOT_ITEM_NONE;
        oot_link_use_item( gLink, OOT_ITEM_NONE );
        printf( "[item] %s away\n", name );
        pg_notify( "%s AWAY", name );
        if( gWorkbenchOpen ) gSnapshotRefresh = 1;
        return;
    }
    int childOnly = item == OOT_ITEM_DEKU_STICK || item == OOT_ITEM_BOOMERANG;
    int adultOnly = item == OOT_ITEM_HAMMER || item == OOT_ITEM_BOW || item == OOT_ITEM_HOOKSHOT;
    if(( childOnly && gSt.age != OOT_AGE_CHILD ) ||
       ( adultOnly && gSt.age != OOT_AGE_ADULT )) {
        printf( "[item] %s: wrong age (press T to switch)\n", name );
        pg_notify( "%s REQUIRES %s LINK", name,
                   childOnly ? "CHILD" : "ADULT" );
        return;
    }
    gItemOut = item;
    if( item == OOT_ITEM_BOW )  gArrowAmmo = 30;    /* use_item refills the quiver */
    if( item == OOT_ITEM_BOMB ) gBombAmmo = 20;     /* ...and the bomb bag */
    oot_link_use_item( gLink, item );
    if( gWorkbenchOpen ) gSnapshotRefresh = 1;
    if( item == OOT_ITEM_BOMB )
        printf( "[item] %s out -- press I for another bomb, A throws it\n", name );
    else if( item == OOT_ITEM_BOOMERANG )
        printf( "[item] %s out -- press I to throw it\n", name );
    else if( item == OOT_ITEM_BOW || item == OOT_ITEM_HOOKSHOT )
        printf( "[item] %s out -- hold I to draw/aim, release to fire\n", name );
    else
        printf( "[item] %s equipped -- press I to use it\n", name );
    pg_notify( "%s EQUIPPED", name );
}

enum ArenaMaterial {
    ARENA_GRASS, ARENA_STONE, ARENA_POOL, ARENA_STEPS,
    ARENA_HOOKSHOT, ARENA_RAMP, ARENA_MATERIAL_COUNT
};

#define ARENA_SURFACE_CAP 192
static struct OoTSurface gArenaSurfaces[ARENA_SURFACE_CAP];
static uint8_t gArenaSurfaceMaterial[ARENA_SURFACE_CAP];
static int gArenaSurfaceCount;

static const float gArenaMaterialColor[ARENA_MATERIAL_COUNT][3] = {
    { .28f, .50f, .29f }, { .48f, .44f, .39f }, { .14f, .19f, .27f },
    { .58f, .53f, .42f }, { .50f, .32f, .18f }, { .38f, .48f, .36f },
};

static void arena_triangle( int material,
                            int ax, int ay, int az, int bx, int by, int bz,
                            int cx, int cy, int cz )
{
    if( gArenaSurfaceCount >= ARENA_SURFACE_CAP ) {
        gFatalError = 1;
        return;
    }
    struct OoTSurface *out = &gArenaSurfaces[gArenaSurfaceCount];
    memset( out, 0, sizeof( *out ));
    /* type is retained for the host renderer/debugger. The current public
       static-world loader intentionally maps every custom surface to one
       hookshot-enabled OoT SurfaceType. */
    out->type = (uint16_t)material;
    int values[9] = { ax,ay,az, bx,by,bz, cx,cy,cz };
    for( int v = 0; v < 3; ++v )
        for( int axis = 0; axis < 3; ++axis )
            out->vertices[v][axis] = values[v * 3 + axis];
    gArenaSurfaceMaterial[gArenaSurfaceCount] = (uint8_t)material;
    gArenaSurfaceCount++;
}

static void arena_quad( int material,
                        int ax, int ay, int az, int bx, int by, int bz,
                        int cx, int cy, int cz, int dx, int dy, int dz )
{
    arena_triangle( material, ax,ay,az, bx,by,bz, cx,cy,cz );
    arena_triangle( material, ax,ay,az, cx,cy,cz, dx,dy,dz );
}

static void arena_box( int material, int x0, int y0, int z0,
                       int x1, int y1, int z1 )
{
    arena_quad( material, x0,y1,z0, x0,y1,z1, x1,y1,z1, x1,y1,z0 ); /* top */
    arena_quad( material, x0,y0,z1, x0,y1,z1, x0,y1,z0, x0,y0,z0 );
    arena_quad( material, x1,y0,z0, x1,y1,z0, x1,y1,z1, x1,y0,z1 );
    arena_quad( material, x0,y0,z0, x0,y1,z0, x1,y1,z0, x1,y0,z0 );
    arena_quad( material, x1,y0,z1, x1,y1,z1, x0,y1,z1, x0,y0,z1 );
    arena_quad( material, x0,y0,z1, x0,y0,z0, x1,y0,z0, x1,y0,z1 ); /* underside */
}

static void arena_ramp_x( int material, int x0, int x1, int z0, int z1,
                          int y0, int y1 )
{
    arena_quad( material, x0,y0,z0, x0,y0,z1, x1,y1,z1, x1,y1,z0 );
    if( y0 != 0 )
        arena_triangle( material, x0,0,z0, x0,y0,z0, x1,y1,z0 );
    arena_triangle( material, x0,0,z0, x1,y1,z0, x1,0,z0 );
    arena_triangle( material, x1,0,z1, x1,y1,z1, x0,y0,z1 );
    if( y0 != 0 )
        arena_triangle( material, x1,0,z1, x0,y0,z1, x0,0,z1 );
    arena_quad( material, x1,0,z0, x1,y1,z0, x1,y1,z1, x1,0,z1 );
}

static void build_arena_geometry( void )
{
    gArenaSurfaceCount = 0;
    int A = (int)ARENA, H = (int)WALL_H;
    /* Central combat field with the north-east water cutout. */
    arena_quad( ARENA_GRASS, -A,0,-A, -A,0,A, POOL_X0,0,A, POOL_X0,0,-A );
    arena_quad( ARENA_GRASS, POOL_X0,0,-A, POOL_X0,0,POOL_Z0,
                A,0,POOL_Z0, A,0,-A );
    arena_quad( ARENA_GRASS, POOL_X0,0,POOL_Z1, POOL_X0,0,A,
                A,0,A, A,0,POOL_Z1 );
    arena_quad( ARENA_GRASS, POOL_X1,0,POOL_Z0, POOL_X1,0,POOL_Z1,
                A,0,POOL_Z1, A,0,POOL_Z0 );

    /* Deep-water and iron-boots test basin with a walk-out ramp. */
    arena_quad( ARENA_POOL, POOL_X0,0,POOL_Z0, POOL_X0,0,POOL_Z1,
                POOL_RAMP_X,POOL_FLOOR,POOL_Z1, POOL_RAMP_X,POOL_FLOOR,POOL_Z0 );
    arena_quad( ARENA_POOL, POOL_RAMP_X,POOL_FLOOR,POOL_Z0,
                POOL_RAMP_X,POOL_FLOOR,POOL_Z1, POOL_X1,POOL_FLOOR,POOL_Z1,
                POOL_X1,POOL_FLOOR,POOL_Z0 );
    arena_quad( ARENA_POOL, POOL_X0,POOL_FLOOR,POOL_Z1, POOL_X0,0,POOL_Z1,
                POOL_X1,0,POOL_Z1, POOL_X1,POOL_FLOOR,POOL_Z1 );
    arena_quad( ARENA_POOL, POOL_X1,POOL_FLOOR,POOL_Z0, POOL_X1,0,POOL_Z0,
                POOL_X0,0,POOL_Z0, POOL_X0,POOL_FLOOR,POOL_Z0 );
    arena_quad( ARENA_POOL, POOL_X1,POOL_FLOOR,POOL_Z1, POOL_X1,0,POOL_Z1,
                POOL_X1,0,POOL_Z0, POOL_X1,POOL_FLOOR,POOL_Z0 );

    /* North-west movement course: discrete stairs into a broad platform. */
    for( int i = 0; i < 5; ++i ) {
        int x0 = -790 + i * 92;
        arena_box( ARENA_STEPS, x0, 0, 470, x0 + 86, ( i + 1 ) * 26, 650 );
    }
    arena_box( ARENA_STEPS, -330, 0, 450, -120, 142, 670 );

    /* South-west slope/ledge course for turning, rolls and drop tests. */
    arena_ramp_x( ARENA_RAMP, -790, -510, -700, -520, 0, 135 );
    arena_box( ARENA_RAMP, -510, 0, -700, -280, 135, -520 );
    arena_box( ARENA_STEPS, -240, 0, -680, -180, 70, -540 );

    /* South-east hookshot wall and elevated landing. Every custom arena
       polygon is hookshot-enabled by oot_static_world_load. */
    arena_box( ARENA_HOOKSHOT, 500, 0, -760, 735, 230, -580 );
    arena_box( ARENA_HOOKSHOT, 410, 0, -535, 730, 58, -475 );

    /* Narrow balance beam between the combat ring and projectile range. */
    arena_box( ARENA_STONE, 190, 0, -250, 530, 38, -205 );

    /* Boundary walls. */
    arena_quad( ARENA_STONE, -A,0,A, -A,H,A, A,H,A, A,0,A );
    arena_quad( ARENA_STONE, A,0,-A, A,H,-A, -A,H,-A, -A,0,-A );
    arena_quad( ARENA_STONE, A,0,A, A,H,A, A,H,-A, A,0,-A );
    arena_quad( ARENA_STONE, -A,0,-A, -A,H,-A, -A,H,A, -A,0,A );
}

static void load_arena( void )
{
    build_arena_geometry();
    if( gFatalError ) return;
    static const struct OoTWaterBox wb = {
        POOL_X0, POOL_Z0, POOL_X1 - POOL_X0, POOL_Z1 - POOL_Z0, WATER_Y
    };
    oot_static_world_load( gArenaSurfaces, (uint32_t)gArenaSurfaceCount, &wb, 1 );
}

/* ---- v0.7 scene mode: real ROM scenes ----------------------------------
   F5 toggles arena <-> the first scene, F6 cycles the list below. The scene's
   own collision replaces the arena world (oot_scene_load) and the room mesh
   renders through the same textured-triangle path as Link. The foe + its
   attention target are arena-only in this host; Navi stays. */
static const struct { int32_t idx; const char *name; } kScenes[] = {
    { OOT_SCENE_DEKU_TREE,      "Deku Tree" },
    { OOT_SCENE_DODONGOS_CAVERN, "Dodongo's Cavern" },
    { OOT_SCENE_JABU_JABU,      "Inside Jabu-Jabu" },
    { OOT_SCENE_FOREST_TEMPLE,  "Forest Temple" },
    { OOT_SCENE_FIRE_TEMPLE,    "Fire Temple" },
    { OOT_SCENE_WATER_TEMPLE,   "Water Temple" },
    { OOT_SCENE_SPIRIT_TEMPLE,  "Spirit Temple" },
    { OOT_SCENE_SHADOW_TEMPLE,  "Shadow Temple" },
    { OOT_SCENE_BOTTOM_OF_THE_WELL, "Bottom of the Well" },
    { OOT_SCENE_ICE_CAVERN,     "Ice Cavern" },
    { OOT_SCENE_GERUDO_TRAINING_GROUND, "Gerudo Training Ground" },
    { OOT_SCENE_KOKIRI_FOREST,  "Kokiri Forest" },
    { OOT_SCENE_LINKS_HOUSE,    "Link's House" },
    { OOT_SCENE_TEMPLE_OF_TIME, "Temple of Time" },
    { OOT_SCENE_HYRULE_FIELD,   "Hyrule Field" },
    { OOT_SCENE_KAKARIKO_VILLAGE, "Kakariko Village" },
    { OOT_SCENE_ZORAS_DOMAIN,   "Zora's Domain" },
    { OOT_SCENE_OUTSIDE_GANONS_CASTLE, "Outside Ganon's Castle" },
};
#define N_SCENES ( (int)( sizeof( kScenes ) / sizeof( kScenes[0] )))
static int gSceneMode;              /* 0 = arena, 1 = scene */
static int gSceneSel;               /* index into kScenes while gSceneMode */
static struct OoTSceneEnvironment gSceneEnv;  /* liboot vNEXT: active scene light/fog */
static int gSceneEnvValid;                    /* 1 once fetched for the current world */
static struct OoTSceneRuntime gSceneRuntime;  /* live PlayState scene/room commands */
static int gSceneRuntimeValid;
static int gSceneRoomChoice = -1;             /* -1 = aggregate every room mesh */

static int scene_runtime_refresh( void )
{
    gSceneRuntimeValid = oot_scene_get_runtime( &gSceneRuntime );
    return gSceneRuntimeValid;
}

static const char *scene_room_type_name( uint8_t type )
{
    static const char *const names[] = {
        "NORMAL", "DUNGEON", "INDOORS", "UNUSED 3", "UNUSED 4", "BOSS"
    };
    return type < sizeof( names ) / sizeof( names[0] ) ? names[type] : "UNKNOWN";
}

static const char *scene_room_env_name( uint8_t environment )
{
    static const char *const names[] = {
        "DEFAULT", "COLD", "WARM", "HOT", "STRETCH 1", "STRETCH 2", "STRETCH 3"
    };
    return environment < sizeof( names ) / sizeof( names[0] )
        ? names[environment] : "UNKNOWN";
}

static int scene_room_apply( int room )
{
    if( !gSceneMode || !scene_runtime_refresh()) {
        pg_notify( "LOAD A ROM SCENE FIRST" );
        return 0;
    }
    if( room < -1 || room >= gSceneRuntime.roomCount ) {
        pg_notify( "ROOM %d IS OUT OF RANGE", room );
        return 0;
    }
    int32_t rc = oot_scene_set_room( room );
    if( rc != 0 && rc != -9 ) {
        fprintf( stderr, "[scene] set_room(%d) failed (rc=%d)\n", room, rc );
        pg_notify( "ROOM LOAD FAILED RC %d", rc );
        return 0;
    }
    gSceneRoomChoice = room;
    scene_runtime_refresh();
    gSnapshotRefresh = 1;
    if( rc == -9 ) pg_notify( "ROOM %d ACTIVE - MESH UNAVAILABLE", room );
    else if( room == -1 ) pg_notify( "ALL ROOMS LOADED - ACTIVE ROOM 0" );
    else pg_notify( "ROOM %d LOADED", room );
    printf( "[scene] runtime: scene=%d active=%d geometry=%d/%d type=%u(%s) "
            "env=%u(%s) echo=%d lens=%u warp=%u cam=%u map=%d\n",
            gSceneRuntime.sceneIndex, gSceneRuntime.activeRoomIndex,
            gSceneRuntime.geometryRoomIndex, gSceneRuntime.roomCount,
            gSceneRuntime.roomType, scene_room_type_name( gSceneRuntime.roomType ),
            gSceneRuntime.environmentType,
            scene_room_env_name( gSceneRuntime.environmentType ),
            gSceneRuntime.echo, gSceneRuntime.lensMode,
            gSceneRuntime.warpSongsDisabled, gSceneRuntime.sceneCamType,
            gSceneRuntime.worldMapArea );
    return 1;
}

static void scene_room_step( int delta )
{
    if( !gSceneMode || !scene_runtime_refresh()) {
        pg_notify( "LOAD A ROM SCENE FIRST" );
        return;
    }
    int count = gSceneRuntime.roomCount + 1; /* aggregate sentinel + real rooms */
    int current = gSceneRuntime.geometryRoomIndex + 1;
    int next = ( current + delta ) % count;
    if( next < 0 ) next += count;
    next -= 1;
    scene_room_apply( next );
}

/* Load kScenes[sel] room 0 and respawn Link at the scene's spawn 0.
   Returns 1 on success. On failure (rc < 0, world untouched, except -9:
   collision live but no mesh -> proceed with an empty mesh) the current
   world stays. */
static int scene_enter( int sel )
{
    if( sel < 0 || sel >= N_SCENES ) {
        fprintf( stderr, "[scene] invalid selection %d\n", sel );
        return 0;
    }
    /* liboot vNEXT: load the whole dungeon (all rooms) so the playground shows
       the full scene, not just room 0. Collision is whole-scene either way. */
    int32_t rc = oot_scene_load( kScenes[sel].idx, -1 );
    if( rc != 0 ) {
        printf( "[scene] load %s failed (rc=%d)%s\n", kScenes[sel].name, rc,
                rc == -9 ? " -- collision live, mesh unavailable" : "" );
        if( rc != -9 ) return 0;
    }
    /* foe + target disabled in scene mode v1 */
    if( gFoeTarget >= 0 ) { oot_target_remove( gFoeTarget ); gFoeTarget = -1; }
    gFoe.state = FOE_DEAD;
    gFoe.timer = 0x7FFFFFFF;
    float sp[3]; int16_t yaw = 0;
    if( !oot_scene_spawn( 0, sp, &yaw )) { sp[0] = sp[2] = 0.0f; sp[1] = 100.0f; }
    if( gLink >= 0 ) oot_link_delete( gLink );
    gLink = oot_link_create( sp[0], sp[1], sp[2] );
    if( gLink < 0 ) {
        fprintf( stderr, "[scene] Link create failed in %s\n", kScenes[sel].name );
        gFatalError = 1;
        return 0;
    }
    gSceneMode = 1;
    gSceneSel = sel;
    gSceneRoomChoice = -1;
    gWorldChoice = sel + 1;
    oot_link_set_health( gLink, 0x30, 0x60 );
    oot_link_set_magic( gLink, 1, 0x30 );
    oot_link_set_equipment( gLink, gSword, gShield, gTunic, gBoots );
    gOcarinaOut = 0;
    gItemOut = OOT_ITEM_NONE;
    gDeadTimer = 0;
    gCamPos[0] = sp[0];
    gCamPos[1] = sp[1] + 120.0f;
    gCamPos[2] = sp[2] - 260.0f;
    uint32_t nTri = 0, xlu = 0;
    if( !oot_scene_get_geometry( NULL, NULL, NULL, NULL, NULL, &nTri, &xlu ) ||
        xlu > nTri || nTri > OOT_SCENE_MAX_TRIANGLES )
        nTri = xlu = 0;
    printf( "[scene] %s all rooms (active room 0): %u tris (%u opa + %u xlu), spawn (%.0f, %.0f, %.0f) yaw %d\n",
            kScenes[sel].name, nTri, xlu, nTri - xlu, sp[0], sp[1], sp[2], yaw );
    if( scene_runtime_refresh())
        printf( "[scene] runtime: scene=%d active=%d geometry=%d/%d type=%u(%s) "
                "env=%u(%s) echo=%d lens=%u warp=%u cam=%u map=%d\n",
                gSceneRuntime.sceneIndex, gSceneRuntime.activeRoomIndex,
                gSceneRuntime.geometryRoomIndex, gSceneRuntime.roomCount,
                gSceneRuntime.roomType, scene_room_type_name( gSceneRuntime.roomType ),
                gSceneRuntime.environmentType,
                scene_room_env_name( gSceneRuntime.environmentType ),
                gSceneRuntime.echo, gSceneRuntime.lensMode,
                gSceneRuntime.warpSongsDisabled, gSceneRuntime.sceneCamType,
                gSceneRuntime.worldMapArea );
    /* liboot vNEXT: the scene's sound-settings command -> host sequenced audio. */
    printf( "[scene] %s sound: bgm seqId=%d  nature ambienceId=%d\n",
            kScenes[sel].name, oot_scene_get_sequence_id(), oot_scene_get_ambience_id() );
    /* liboot vNEXT: the scene's light/fog settings. Shade is already baked into
       the vertex colors we render; we additionally drive GL fog from this. */
    gSceneEnvValid = oot_scene_get_environment( &gSceneEnv );
    if( gSceneEnvValid && gSceneEnv.valid )
        printf( "[scene] %s light: ambient (%.2f %.2f %.2f)  fog color (%.2f %.2f %.2f) near %.0f far %.0f\n",
                kScenes[sel].name, gSceneEnv.ambientColor[0], gSceneEnv.ambientColor[1],
                gSceneEnv.ambientColor[2], gSceneEnv.fogColor[0], gSceneEnv.fogColor[1],
                gSceneEnv.fogColor[2], gSceneEnv.fogNear, gSceneEnv.fogFar );
    else
        printf( "[scene] %s light: no scene light settings (renderer uses daylight default)\n",
                kScenes[sel].name );
    audio_music_apply_scene();
    return 1;
}

/* back to the arena: rebuild the static world, respawn Link + foe */
static int scene_leave( void )
{
    audio_music_stop( 0 );
    gSceneMode = 0;
    gWorldChoice = 0;
    gSceneEnvValid = 0;   /* arena uses the renderer's daylight default */
    gSceneRuntimeValid = 0;
    gSceneRoomChoice = -1;
    load_arena();
    if( !link_spawn()) return 0;
    foe_spawn();
    gCamPos[0] = 0; gCamPos[1] = 120; gCamPos[2] = -260;
    puts( "[scene] back to the arena" );
    return 1;
}

/* ---- feature-lab workbench -------------------------------------------- */

static const char *const kTabNames[PG_TAB_COUNT] = {
    "PLAY", "LINK", "ITEMS", "WORLD", "AUDIO", "RENDER", "HELP"
};
static const char *const kAgeNames[] = { "ADULT", "CHILD" };
static const char *const kSwordNames[] = { "NONE", "KOKIRI", "MASTER", "BIGGORON" };
static const char *const kShieldNames[] = { "NONE", "DEKU", "HYLIAN", "MIRROR" };
static const char *const kTunicNames[] = { "KOKIRI", "GORON", "ZORA" };
static const char *const kBootNames[] = { "KOKIRI", "IRON", "HOVER" };
static const char *const kItemNames[] = {
    "NONE", "OCARINA", "BOTTLE", "HAMMER", "DEKU STICK",
    "BOOMERANG", "BOW", "HOOKSHOT", "BOMB"
};
static const char *const kCameraNames[] = { "ORBIT", "CHASE", "TARGET" };
static const char *const kFoeStateNames[] = { "CHASE", "WINDUP", "RECOVER", "HURT", "DEAD" };
static const struct { const char *name; float x, y, z; } kArenaSpawns[] = {
    { "COMBAT RING", 0, 0, 0 },
    { "STAIR COURSE", -760, 12, 390 },
    { "RAMP COURSE", -820, 12, -610 },
    { "PROJECTILE RANGE", 250, 12, -610 },
    { "WATER BASIN", 650, 25, 520 },
};
#define N_ARENA_SPAWNS ((int)( sizeof( kArenaSpawns ) / sizeof( kArenaSpawns[0] )))

static const char *pg_on_off( int value ) { return value ? "ON" : "OFF"; }

static void apply_loadout( void )
{
    oot_link_set_equipment( gLink, gSword, gShield, gTunic, gBoots );
    if( gWorkbenchOpen ) gSnapshotRefresh = 1;
    pg_notify( "REQUESTED LOADOUT APPLIED" );
}

static void set_link_age( uint8_t age )
{
    if( age > OOT_AGE_CHILD || age == gSt.age ) return;
    int restoreTarget = !gSceneMode && gFoe.state != FOE_DEAD;
    if( gFoeTarget >= 0 ) {
        oot_target_remove( gFoeTarget );
        gFoeTarget = -1;
    }
    gItemOut = OOT_ITEM_NONE;
    gOcarinaOut = 0;
    if( !oot_link_set_age( gLink, age )) {
        fprintf( stderr, "[link] age switch unavailable for this ROM\n" );
        if( restoreTarget )
            gFoeTarget = oot_target_create( gFoe.x, gFoe.y, gFoe.z, 30.0f );
        pg_notify( "AGE SWITCH FAILED" );
        return;
    }
    gSt.age = age; /* immediate UI feedback; next real tick is authoritative */
    if( gWorkbenchOpen ) gSnapshotRefresh = 1;
    apply_loadout();
    if( restoreTarget ) {
        gFoeTarget = oot_target_create( gFoe.x, gFoe.y, gFoe.z, 30.0f );
        if( gFoeTarget < 0 )
            fprintf( stderr, "[foe] could not restore the Z-lock target\n" );
    }
    pg_notify( "%s LINK", kAgeNames[age] );
}

static int workbench_row_count( int tab )
{
    static const int rows[PG_TAB_COUNT] = { 7, 8, 5, 7, 13, 7, 1 };
    return tab >= 0 && tab < PG_TAB_COUNT ? rows[tab] : 0;
}

static int wrap_value( int value, int count )
{
    if( count <= 0 ) return 0;
    value %= count;
    if( value < 0 ) value += count;
    return value;
}

static void workbench_change_tab( int delta )
{
    gWorkbenchTab = wrap_value( gWorkbenchTab + delta, PG_TAB_COUNT );
    gWorkbenchRow = 0;
}

static void workbench_adjust( int delta, int activate )
{
    if( !delta && !activate ) return;
    switch( gWorkbenchTab ) {
    case PG_TAB_PLAY:
        switch( gWorkbenchRow ) {
        case 0: gManualPaused = !gManualPaused; break;
        case 1: gSingleStep = 1; gManualPaused = 1; break;
        case 2: {
            static const float speeds[] = { .25f, .5f, 1.0f, 2.0f, 4.0f };
            int nearest = 0;
            for( int i = 1; i < 5; ++i )
                if( fabsf( speeds[i] - gTimeScale ) < fabsf( speeds[nearest] - gTimeScale )) nearest = i;
            nearest = wrap_value( nearest + ( delta ? delta : 1 ), 5 );
            gTimeScale = speeds[nearest];
            break;
        }
        case 3:
            gCameraMode = wrap_value( gCameraMode + ( delta ? delta : 1 ), PG_CAM_COUNT );
            break;
        case 4: gEnemyAi = !gEnemyAi; break;
        case 5:
            if( activate && ( gSceneMode ? scene_enter( gSceneSel ) : link_spawn())) {
                pg_notify( "LINK RESPAWNED" );
                gSnapshotRefresh = 1;
            }
            break;
        case 6:
            if( activate && !gSceneMode ) { foe_spawn(); pg_notify( "ENEMY RESPAWNED" ); }
            break;
        }
        break;
    case PG_TAB_LINK:
        switch( gWorkbenchRow ) {
        case 0: set_link_age((uint8_t)wrap_value( gSt.age + ( delta ? delta : 1 ), 2 )); break;
        case 1: gSword = (uint8_t)wrap_value( gSword + ( delta ? delta : 1 ), 4 ); apply_loadout(); break;
        case 2: gShield = (uint8_t)wrap_value( gShield + ( delta ? delta : 1 ), 4 ); apply_loadout(); break;
        case 3: gTunic = (uint8_t)wrap_value( gTunic + ( delta ? delta : 1 ), 3 ); apply_loadout(); break;
        case 4: gBoots = (uint8_t)wrap_value( gBoots + ( delta ? delta : 1 ), 3 ); apply_loadout(); break;
        case 5: if( activate ) { oot_link_set_health( gLink, gSt.healthCapacity, 0 ); gSnapshotRefresh=1; pg_notify( "HEALTH FILLED" ); } break;
        case 6: if( activate ) { oot_link_damage( gLink, 8 ); gSnapshotRefresh=1; pg_notify( "DAMAGE INFLICTED" ); } break;
        case 7: if( activate ) { oot_link_set_magic( gLink, 2, 0x60 ); gSnapshotRefresh=1; pg_notify( "MAGIC FILLED" ); } break;
        }
        break;
    case PG_TAB_ITEMS:
        switch( gWorkbenchRow ) {
        case 0: gItemChoice = wrap_value( gItemChoice + ( delta ? delta : 1 ), 9 ); break;
        case 1:
            if( activate ) item_toggle((uint8_t)gItemChoice, kItemNames[gItemChoice] );
            break;
        case 2: if( activate ) { gArrowAmmo = 30; pg_notify( "ARROWS REFILLED" ); } break;
        case 3: if( activate ) { gBombAmmo = 20; pg_notify( "BOMBS REFILLED" ); } break;
        case 4:
            if( activate ) { gItemOut = OOT_ITEM_NONE; gOcarinaOut = 0; oot_link_use_item( gLink, OOT_ITEM_NONE ); gSnapshotRefresh=1; pg_notify( "ITEM CLEARED" ); }
            break;
        }
        break;
    case PG_TAB_WORLD:
        switch( gWorkbenchRow ) {
        case 0: gWorldChoice = wrap_value( gWorldChoice + ( delta ? delta : 1 ), N_SCENES + 1 ); break;
        case 1:
            if( activate ) {
                int loaded = gWorldChoice == 0 ? scene_leave() : scene_enter( gWorldChoice - 1 );
                if( loaded ) gSnapshotRefresh = 1;
            }
            break;
        case 2:
            if( gSceneMode && scene_runtime_refresh()) {
                int count = gSceneRuntime.roomCount + 1;
                int selected = gSceneRoomChoice + 1;
                selected = wrap_value( selected + ( delta ? delta : 1 ), count );
                gSceneRoomChoice = selected - 1;
            }
            break;
        case 3:
            if( activate && gSceneMode ) scene_room_apply( gSceneRoomChoice );
            break;
        case 4: gArenaSpawnChoice = wrap_value( gArenaSpawnChoice + ( delta ? delta : 1 ), N_ARENA_SPAWNS ); break;
        case 5:
            if( activate && !gSceneMode ) {
                const int s = gArenaSpawnChoice;
                if( link_spawn_at( kArenaSpawns[s].x, kArenaSpawns[s].y,
                                   kArenaSpawns[s].z )) {
                    gSnapshotRefresh = 1;
                    pg_notify( "TELEPORTED: %s", kArenaSpawns[s].name );
                }
            }
            break;
        case 6:
            if( activate && !gSceneMode ) { load_arena(); gSnapshotRefresh=1; pg_notify( "ARENA COLLISION RELOADED" ); }
            break;
        }
        break;
    case PG_TAB_AUDIO:
        switch( gWorkbenchRow ) {
        case 0:
            audio_lock();
            gMasterVolume = fmaxf( 0.0f, fminf( 1.5f, gMasterVolume + ( delta ? delta : 1 ) * .1f ));
            audio_unlock();
            break;
        case 1:
            gSceneMusicAuto = !gSceneMusicAuto;
            if( gSceneMusicAuto ) {
                if( gSceneMode ) audio_music_apply_scene();
                else audio_music_stop( 0 );
            }
            pg_notify( "SCENE MUSIC MODE: %s", gSceneMusicAuto ? "AUTO" : "MANUAL" );
            break;
        case 2: {
            int count = audio_sequence_count_safe();
            if( count > 0 ) {
                int step = delta ? delta : 1;
                gMusicSequenceChoice = wrap_value( gMusicSequenceChoice + step, count );
            }
            break;
        }
        case 3: if( activate ) audio_music_play_selected( 1 ); break;
        case 4: if( activate ) audio_music_stop( 1 ); break;
        case 5:
            audio_lock();
            gMusicVolume = fmaxf( 0.0f, fminf( 1.0f,
                gMusicVolume + ( delta ? delta : 1 ) * .1f ));
            oot_audio_sequence_set_volume( OOT_AUDIO_PLAYER_MAIN, gMusicVolume );
            oot_audio_sequence_set_volume( OOT_AUDIO_PLAYER_SUB, gMusicVolume );
            audio_unlock();
            break;
        case 6:
            gNatureChoice = wrap_value( gNatureChoice + ( delta ? delta : 1 ),
                                        OOT_AUDIO_NATURE_COUNT );
            break;
        case 7: if( activate ) audio_nature_play_selected(); break;
        case 8: audio_sfx_change_bank( delta ? delta : 1 ); break;
        case 9: audio_sfx_change_entry( delta ? delta : 1 ); break;
        case 10: if( activate ) audio_sfx_play_selected(); break;
        case 11: if( activate ) audio_sfx_stop_all(); break;
        case 12:
            if( activate ) {
                gOcarinaOut = !gOcarinaOut;
                gItemOut = gOcarinaOut ? OOT_ITEM_OCARINA : OOT_ITEM_NONE;
                oot_link_use_item( gLink, gOcarinaOut ? OOT_ITEM_OCARINA : OOT_ITEM_NONE );
                gSnapshotRefresh = 1;
            }
            break;
        }
        break;
    case PG_TAB_RENDER:
        switch( gWorkbenchRow ) {
        case 0: gShowSkeleton = !gShowSkeleton; break;
        case 1: gShowSceneMesh = !gShowSceneMesh; break;
        case 2:
            gActorMeshesEnabled = !gActorMeshesEnabled;
            oot_actor_set_render( gActorMeshesEnabled != 0 );
            gSnapshotRefresh = 1;
            break;
        case 3:
            gNaviMeshEnabled = !gNaviMeshEnabled;
            oot_navi_set_render( gNaviMeshEnabled != 0 );
            gSnapshotRefresh = 1;
            break;
        case 4: gShowCollision = !gShowCollision; break;
        case 5: gShowActorBounds = !gShowActorBounds; break;
        case 6: gDiagnosticsVisible = !gDiagnosticsVisible; break;
        }
        break;
    default: break;
    }
}

static void workbench_key( SDL_Keycode key, SDL_Keymod mod )
{
    if( key == SDLK_ESCAPE || key == SDLK_TAB ) {
        if( key == SDLK_TAB && ( mod & KMOD_SHIFT )) workbench_change_tab( -1 );
        else if( key == SDLK_TAB ) workbench_change_tab( 1 );
        else gWorkbenchOpen = 0;
        return;
    }
    int rows = workbench_row_count( gWorkbenchTab );
    if( key == SDLK_UP ) gWorkbenchRow = wrap_value( gWorkbenchRow - 1, rows );
    else if( key == SDLK_DOWN ) gWorkbenchRow = wrap_value( gWorkbenchRow + 1, rows );
    else if( key == SDLK_HOME ) gWorkbenchRow = 0;
    else if( key == SDLK_END ) gWorkbenchRow = rows > 0 ? rows - 1 : 0;
    else if( key == SDLK_PAGEUP || key == SDLK_PAGEDOWN ) {
        int direction = key == SDLK_PAGEUP ? -1 : 1;
        if( gWorkbenchTab == PG_TAB_AUDIO &&
            ( gWorkbenchRow == 2 || gWorkbenchRow == 6 ||
              gWorkbenchRow == 8 || gWorkbenchRow == 9 ))
            workbench_adjust( direction * 10, 0 );
        else
            gWorkbenchRow = wrap_value( gWorkbenchRow + direction * 5, rows );
    }
    else if( key == SDLK_LEFT ) workbench_adjust( -1, 0 );
    else if( key == SDLK_RIGHT ) workbench_adjust( 1, 0 );
    else if( key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE )
        workbench_adjust( 0, 1 );
}

/* ---- tiny GL helpers (fixed pipeline) ---------------------------------- */

static void frustum( float fovDeg, float aspect, float nearZ, float farZ )
{
    float t = nearZ * tanf( fovDeg * PI_F / 360.0f );
    glFrustum( -t * aspect, t * aspect, -t, t, nearZ, farZ );
}

static void look_at( const float *eye, const float *at )
{
    float f[3] = { at[0]-eye[0], at[1]-eye[1], at[2]-eye[2] };
    float fl = sqrtf( f[0]*f[0]+f[1]*f[1]+f[2]*f[2] );
    if( !isfinite( fl ) || fl < 1e-5f ) {
        f[0] = 0.0f; f[1] = 0.0f; f[2] = 1.0f;
    } else {
        f[0]/=fl; f[1]/=fl; f[2]/=fl;
    }
    float s[3] = { -f[2], 0, f[0] };                       /* f x up(0,1,0) */
    float sl = sqrtf( s[0]*s[0]+s[2]*s[2] );
    if( !isfinite( sl ) || sl < 1e-5f ) {
        s[0] = 1.0f; s[2] = 0.0f;
    } else {
        s[0]/=sl; s[2]/=sl;
    }
    float u[3] = { -s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1] };
    float m[16] = { s[0],u[0],-f[0],0, s[1],u[1],-f[1],0, s[2],u[2],-f[2],0, 0,0,0,1 };
    m[4]=0; m[5]=u[1]; m[6]=-f[1];
    glMultMatrixf( m );
    glTranslatef( -eye[0], -eye[1], -eye[2] );
}

static void cube( float x, float y, float z, float sx, float sy, float sz )
{
    glPushMatrix();
    glTranslatef( x, y, z );
    glScalef( sx, sy, sz );
    glBegin( GL_QUADS );
    /* 6 faces, unit cube centered on origin (y from 0 to 1) */
    glVertex3f(-.5f,0,-.5f); glVertex3f(-.5f,1,-.5f); glVertex3f(.5f,1,-.5f); glVertex3f(.5f,0,-.5f);
    glVertex3f(-.5f,0, .5f); glVertex3f(.5f,0,.5f);   glVertex3f(.5f,1,.5f);  glVertex3f(-.5f,1,.5f);
    glVertex3f(-.5f,0,-.5f); glVertex3f(-.5f,0,.5f);  glVertex3f(-.5f,1,.5f); glVertex3f(-.5f,1,-.5f);
    glVertex3f( .5f,0,-.5f); glVertex3f(.5f,1,-.5f);  glVertex3f(.5f,1,.5f);  glVertex3f(.5f,0,.5f);
    glVertex3f(-.5f,1,-.5f); glVertex3f(-.5f,1,.5f);  glVertex3f(.5f,1,.5f);  glVertex3f(.5f,1,-.5f);
    glVertex3f(-.5f,0,-.5f); glVertex3f(.5f,0,-.5f);  glVertex3f(.5f,0,.5f);  glVertex3f(-.5f,0,.5f);
    glEnd();
    glPopMatrix();
}

static void wire_box( float x, float y, float z, float sx, float sy, float sz )
{
    float x0=x-sx*.5f, x1=x+sx*.5f, y0=y, y1=y+sy, z0=z-sz*.5f, z1=z+sz*.5f;
    glBegin( GL_LINES );
    glVertex3f(x0,y0,z0); glVertex3f(x1,y0,z0); glVertex3f(x1,y0,z0); glVertex3f(x1,y0,z1);
    glVertex3f(x1,y0,z1); glVertex3f(x0,y0,z1); glVertex3f(x0,y0,z1); glVertex3f(x0,y0,z0);
    glVertex3f(x0,y1,z0); glVertex3f(x1,y1,z0); glVertex3f(x1,y1,z0); glVertex3f(x1,y1,z1);
    glVertex3f(x1,y1,z1); glVertex3f(x0,y1,z1); glVertex3f(x0,y1,z1); glVertex3f(x0,y1,z0);
    glVertex3f(x0,y0,z0); glVertex3f(x0,y1,z0); glVertex3f(x1,y0,z0); glVertex3f(x1,y1,z0);
    glVertex3f(x1,y0,z1); glVertex3f(x1,y1,z1); glVertex3f(x0,y0,z1); glVertex3f(x0,y1,z1);
    glEnd();
}

/* ---- simulation -------------------------------------------------------- */

static float pg_wrap_angle( float angle )
{
    while( angle > PI_F ) angle -= 2.0f * PI_F;
    while( angle < -PI_F ) angle += 2.0f * PI_F;
    return angle;
}

static void camera_tick( void )
{
    float target[3] = { gSt.position[0], gSt.position[1] + 62.0f, gSt.position[2] };
    if( gCameraMode == PG_CAM_CHASE ) {
        float facing = gSt.faceAngle * PI_F / 32768.0f;
        gCameraYaw += pg_wrap_angle( facing - gCameraYaw ) * 0.16f;
    } else if( gCameraMode == PG_CAM_TARGET && gSt.lockOnActive ) {
        float mx = ( gSt.position[0] + gSt.lockOnPos[0] ) * 0.5f;
        float mz = ( gSt.position[2] + gSt.lockOnPos[2] ) * 0.5f;
        float facing = atan2f( gSt.lockOnPos[0] - gSt.position[0],
                               gSt.lockOnPos[2] - gSt.position[2] );
        gCameraYaw += pg_wrap_angle( facing - gCameraYaw ) * 0.12f;
        target[0] = mx;
        target[2] = mz;
        target[1] = ( gSt.position[1] + gSt.lockOnPos[1] ) * 0.5f + 25.0f;
    }
    gCameraPitch = fmaxf( -0.10f, fminf( 1.10f, gCameraPitch ));
    gCameraDistance = fmaxf( 120.0f, fminf( 900.0f, gCameraDistance ));
    float horizontal = cosf( gCameraPitch ) * gCameraDistance;
    float desired[3] = {
        target[0] - sinf( gCameraYaw ) * horizontal,
        target[1] + sinf( gCameraPitch ) * gCameraDistance,
        target[2] - cosf( gCameraYaw ) * horizontal,
    };
    float cameraEase = gCameraMode == PG_CAM_ORBIT ? 0.24f : 0.14f;
    for( int axis = 0; axis < 3; ++axis ) {
        gCamPos[axis] += ( desired[axis] - gCamPos[axis] ) * cameraEase;
        gCameraAt[axis] += ( target[axis] - gCameraAt[axis] ) * 0.25f;
    }
    float dx = gSt.position[0] - gCamPos[0];
    float dz = gSt.position[2] - gCamPos[2];
    float d = hypotf( dx, dz );
    gIn.camLookX = d > 1.0f ? dx / d : 0.0f;
    gIn.camLookZ = d > 1.0f ? dz / d : 1.0f;
}

static void foe_tick( void )
{
    if( gFoe.invuln > 0 ) gFoe.invuln--;

    float dx = gSt.position[0] - gFoe.x, dz = gSt.position[2] - gFoe.z;
    float dist = sqrtf( dx*dx + dz*dz );

    switch( gFoe.state ) {
    case FOE_CHASE:
        if( dist > 1.0f ) {
            gFoe.yaw = atan2f( dx, dz );
            gFoe.x += sinf( gFoe.yaw ) * 2.5f;
            gFoe.z += cosf( gFoe.yaw ) * 2.5f;
        }
        if( dist < 55.0f && !gSt.isDead ) { gFoe.state = FOE_WINDUP; gFoe.timer = 14; }
        break;
    case FOE_WINDUP:
        if( --gFoe.timer <= 0 ) {
            if( dist < 75.0f && !gSt.isDead ) {
                oot_link_damage( gLink, 8 );    /* half a heart, real knockback path */
                puts( "[foe] the Stalchild claws you!" );
            }
            gFoe.state = FOE_RECOVER;
            gFoe.timer = 50;
        }
        break;
    case FOE_RECOVER:
        if( --gFoe.timer <= 0 ) gFoe.state = FOE_CHASE;
        break;
    case FOE_HURT:
        gFoe.x -= sinf( gFoe.yaw ) * 6.0f;     /* knockback away from Link */
        gFoe.z -= cosf( gFoe.yaw ) * 6.0f;
        if( --gFoe.timer <= 0 ) {
            if( gFoe.hp > 0 ) gFoe.state = FOE_CHASE;
            else foe_die();
        }
        break;
    case FOE_DEAD:
        if( --gFoe.timer <= 0 ) foe_spawn();
        break;
    }

    /* sword connects: active swing + in reach + roughly facing the foe */
    if( gFoe.state != FOE_DEAD && gFoe.invuln == 0 && gSt.meleeWeaponState != 0 ) {
        float faceX = sinf( gSt.faceAngle * PI_F / 32768.0f );
        float faceZ = cosf( gSt.faceAngle * PI_F / 32768.0f );
        float toFoeX = gFoe.x - gSt.position[0];
        float toFoeZ = gFoe.z - gSt.position[2];
        float hitDist = hypotf( toFoeX, toFoeZ );
        if( hitDist < 85.0f && hitDist > 1.0f &&
            ( toFoeX * faceX + toFoeZ * faceZ ) / hitDist > 0.34f ) {
            gFoe.hp--;
            gFoe.invuln = 16;
            gFoe.state = FOE_HURT;
            gFoe.timer = 10;
            printf( "[foe] hit! %d hp left\n", gFoe.hp );
        }
    }

    if( gFoe.x >  ARENA-30 ) gFoe.x =  ARENA-30;
    if( gFoe.x < -ARENA+30 ) gFoe.x = -ARENA+30;
    if( gFoe.z >  ARENA-30 ) gFoe.z =  ARENA-30;
    if( gFoe.z < -ARENA+30 ) gFoe.z = -ARENA+30;

    /* keep the attention target glued to the foe */
    if( gFoeTarget >= 0 )
        oot_target_move( gFoeTarget, gFoe.x, gFoe.y, gFoe.z );
}

/* v0.6 spawned actors: snapshot them, land projectile hits on the foe,
   drive the explosion flash and the fire/explode/catch console events */
static void projectile_tick( void )
{
    int bombSeen = 0, boomSeen = 0;

    gActorN = oot_actor_query( gActors, 64 );
    for( int i = 0; i < gActorN; ++i ) {
        const struct OoTActorInfo *a = &gActors[i];
        if( !a->active ) continue;
        float fdx = a->pos[0] - gFoe.x, fdy = a->pos[1] - ( gFoe.y + 30.0f ),
              fdz = a->pos[2] - gFoe.z;
        float fdist = sqrtf( fdx*fdx + fdy*fdy + fdz*fdz );
        float ldx = a->pos[0] - gSt.position[0], ldz = a->pos[2] - gSt.position[2];
        float ldist = sqrtf( ldx*ldx + ldz*ldz );
        switch( a->id ) {
        case AID_EN_BOM:                 /* remember where it is for the blast */
            bombSeen = 1;
            gLastBombPos[0] = a->pos[0];
            gLastBombPos[1] = a->pos[1];
            gLastBombPos[2] = a->pos[2];
            break;
        case AID_EN_ARROW:               /* in flight only: nocked arrow rides Link */
            if( ldist > 60.0f && fdist < 45.0f )
                foe_projectile_hit( 1, "the arrow" );
            break;
        case AID_EN_BOOM:                /* exists only while thrown */
            boomSeen = 1;
            if( fdist < 50.0f )
                foe_projectile_hit( 1, "the boomerang" );
            break;
        default: break;
        }
    }

    if( bombSeen && !gBombAlive ) {
        if( gBombAmmo > 0 ) gBombAmmo--;
        printf( "[item] bomb out! (%d/20 left) -- throw with A, then run\n", gBombAmmo );
    }
    if( !boomSeen && gBoomAlive && !( gSt.stateFlags1 & STATE1_BOOMERANG_THROWN ))
        puts( "[item] boomerang caught!" );
    gBombAlive = bombSeen;
    gBoomAlive = boomSeen;

    if( gExplodePending ) {              /* raised by sfx_cb during the tick */
        gExplodePending = 0;
        gFlash.timer = 20;
        gFlash.pos[0] = gLastBombPos[0];
        gFlash.pos[1] = gLastBombPos[1] + 20.0f;
        gFlash.pos[2] = gLastBombPos[2];
        printf( "[item] KABOOM! at (%.0f, %.0f, %.0f)\n",
                gLastBombPos[0], gLastBombPos[1], gLastBombPos[2] );
        float dx = gLastBombPos[0] - gFoe.x, dz = gLastBombPos[2] - gFoe.z;
        if( sqrtf( dx*dx + dz*dz ) < 100.0f )       /* blast radius 100 */
            foe_projectile_hit( 2, "the blast" );
    }
    if( gFlash.timer > 0 ) gFlash.timer--;
}

static void sim_tick( void )
{
    if( gLink < 0 ) {
        gFatalError = 1;
        return;
    }
    camera_tick();

    oot_link_tick( gLink, &gIn, &gSt, &gGeo );
    gSimTicks++;
    if( gGeo.numTrianglesUsed > OOT_GEO_MAX_TRIANGLES ) {
        fprintf( stderr, "[gfx] Link geometry exceeded the host buffer (%u triangles)\n",
                 gGeo.numTrianglesUsed );
        gGeo.numTrianglesUsed = 0;
    }
    gPoseValid = oot_link_get_skeleton( gLink, &gPose ) &&
                 gPose.numJoints > 0 && gPose.numJoints <= OOT_SKELETON_MAX_JOINTS;
    if( gPoseValid ) {
        for( int i = 0; i < gPose.numJoints && gPoseValid; ++i ) {
            for( int axis = 0; axis < 3; ++axis )
                if( !isfinite( gPose.jointPos[i][axis] )) gPoseValid = 0;
        }
    }

    static uint8_t prevInWater;
    if( gSt.inWater != prevInWater ) {
        if( gSt.inWater )
            printf( "[water] splash! Link is in the water (surface y=%.1f)\n",
                    gSt.waterSurfaceY );
        else
            puts( "[water] Link climbed out of the water" );
        prevInWater = gSt.inWater;
    }

    if( gSt.lockOnActive != gPrevLockOn ) {
        if( gSt.lockOnActive )
            printf( "[target] lock-on acquired at (%.0f, %.0f, %.0f)\n",
                    gSt.lockOnPos[0], gSt.lockOnPos[1], gSt.lockOnPos[2] );
        else
            puts( "[target] lock-on released" );
        gPrevLockOn = gSt.lockOnActive;
    }

    projectile_tick();
    if( !gSceneMode && gEnemyAi )
        foe_tick();

    if( gSt.isDead && gDeadTimer == 0 ) {
        gDeadTimer = 90;
        puts( "[link] YOU DIED. respawning..." );
    }
    if( gDeadTimer > 0 && --gDeadTimer == 1 ) {
        if( gSceneMode ) {
            scene_enter( gSceneSel );   /* respawn at the scene's spawn 0 */
        } else {
            if( link_spawn()) foe_spawn();
        }
    }
}

/* ---- rendering --------------------------------------------------------- */

static void draw_link_model( void )
{
    /* real Link mesh + real ROM textures, simple diffuse */
    const float L[3] = { 0.35f, 0.8f, 0.48f };
    const float L2[3] = { -0.45f, 0.5f, -0.6f };
    upload_new_textures();
    glEnable( GL_ALPHA_TEST );
    glAlphaFunc( GL_GREATER, 0.5f );
    glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

    int curTex = -2;
    for( int t = 0; t < gGeo.numTrianglesUsed; ++t ) {
        int want = gGeoTex[t] == 0xFFFF ? -1 : gGeoTex[t];
        if( want != curTex ) {
            if( curTex != -2 ) glEnd();
            if( gl_texture_ready( want )) {
                glEnable( GL_TEXTURE_2D );
                glBindTexture( GL_TEXTURE_2D, gGlTex[want] );
            } else {
                glDisable( GL_TEXTURE_2D );
            }
            curTex = want;
            glBegin( GL_TRIANGLES );
        }
        for( int k = 0; k < 3; ++k ) {
            int i = t*3 + k;
            const float *n = gGeoNrm + i*3, *c = gGeoCol + i*3, *pv = gGeoPos + i*3;
            float d1 = n[0]*L[0] + n[1]*L[1] + n[2]*L[2];
            float d2 = n[0]*L2[0] + n[1]*L2[1] + n[2]*L2[2];
            float b = 0.62f + 0.27f * ( d1 > 0 ? d1 : 0 ) + 0.16f * ( d2 > 0 ? d2 : 0 );
            float a = gGeoAlpha[i];   /* liboot vNEXT: per-vertex shade alpha */
            if( gDeadTimer > 0 ) glColor4f( b * 0.6f, b * 0.1f, b * 0.1f, a );
            else glColor4f( c[0]*b, c[1]*b, c[2]*b, a );
            glTexCoord2fv( gGeoUv + i*2 );
            glVertex3fv( pv );
        }
    }
    if( curTex != -2 ) glEnd();
    glDisable( GL_TEXTURE_2D );
    glDisable( GL_ALPHA_TEST );
}

static void draw_link( void )
{
    if( !gShowSkeleton && gGeo.numTrianglesUsed > 0 ) {
        draw_link_model();
        return;
    }
    if( !gPoseValid ) return;
    float tunic[3][3] = {{ .18f,.65f,.25f }, { .75f,.25f,.15f }, { .2f,.35f,.8f }};
    uint8_t curTunic = gTunic < 3 ? gTunic : 0;

    if( gDeadTimer > 0 ) glColor3f( .45f, .1f, .1f );
    else glColor3fv( tunic[curTunic] );

    glLineWidth( gSt.age == OOT_AGE_CHILD ? 4.0f : 5.0f );
    glBegin( GL_LINES );
    for( int i = 1; i < gPose.numJoints; ++i ) {
        if( gPose.parent[i] == 0xFF || gPose.parent[i] >= gPose.numJoints ) continue;
        glVertex3fv( gPose.jointPos[gPose.parent[i]] );
        glVertex3fv( gPose.jointPos[i] );
    }
    glEnd();

    /* head */
    if( gPose.numJoints > J_HEAD ) {
        float *h = gPose.jointPos[J_HEAD];
        glColor3f( .95f, .8f, .6f );
        cube( h[0], h[1]-4, h[2], 12, 12, 12 );
    }

    /* sword beam from the left hand while swinging */
    if( gSt.meleeWeaponState != 0 && gPose.numJoints > J_L_HAND ) {
        float *hand = gPose.jointPos[J_L_HAND], *fore = gPose.jointPos[J_L_FOREARM];
        float vx = hand[0]-fore[0], vy = hand[1]-fore[1], vz = hand[2]-fore[2];
        float vl = sqrtf( vx*vx+vy*vy+vz*vz ) + 1e-5f;
        glColor3f( .9f, .9f, 1.0f );
        glLineWidth( 3.0f );
        glBegin( GL_LINES );
        glVertex3fv( hand );
        glVertex3f( hand[0]+vx/vl*45, hand[1]+vy/vl*45, hand[2]+vz/vl*45 );
        glEnd();
    }
}

/* Room mesh from oot_scene_get_geometry, drawn through the same textured-
   triangle path as Link. xluPass 0: [0, xluStart) with alpha-test cutout
   (leaves, fences...); xluPass 1: [xluStart, n) blended, depth writes off
   (translucency = texture alpha, per the v1 scene API). */
static void draw_scene_mesh( int xluPass )
{
    const float *pos, *nrm, *col, *uv;
    const uint16_t *tex;
    uint32_t nTri, xluStart;
    if( !oot_scene_get_geometry( &pos, &nrm, &col, &uv, &tex, &nTri, &xluStart ))
        return;
    if( !pos || !nrm || !col || !uv || !tex || nTri > OOT_SCENE_MAX_TRIANGLES ||
        xluStart > nTri ) {
        fprintf( stderr, "[scene] rejected invalid geometry (%u triangles, xlu=%u)\n",
                 nTri, xluStart );
        return;
    }
    uint32_t t0 = xluPass ? xluStart : 0;
    uint32_t t1 = xluPass ? nTri : xluStart;
    if( t0 >= t1 ) return;
    (void)nrm;                          /* scene colors are baked lighting */
    /* liboot vNEXT: per-triangle render flags drive real backface culling.
       Front faces follow the emitted winding; GL default (CCW) matches. */
    const uint8_t *flags = NULL;
    oot_scene_get_triangle_flags( &flags );
    glFrontFace( gCullMode == 2 ? GL_CW : GL_CCW );
    upload_new_textures();
    glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
    if( xluPass ) {
        glEnable( GL_BLEND );
        glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
        glDepthMask( GL_FALSE );
    } else {
        glEnable( GL_ALPHA_TEST );
        glAlphaFunc( GL_GREATER, 0.5f );
    }
    int curTex = -2, curCull = -2, curDecal = -1, batchOpen = 0;
    for( uint32_t t = t0; t < t1; ++t ) {
        uint8_t f = flags ? flags[t] : 0u;
        /* CULL_BOTH marks a degenerate/never-drawn triangle. */
        if(( f & ( OOT_TRI_CULL_FRONT | OOT_TRI_CULL_BACK )) ==
           ( OOT_TRI_CULL_FRONT | OOT_TRI_CULL_BACK )) continue;
        int wantCull = gCullMode == 0 ? 0
                     : ( f & OOT_TRI_CULL_BACK ) ? 1 : ( f & OOT_TRI_CULL_FRONT ) ? 2 : 0;
        int wantDecal = ( f & OOT_TRI_DECAL ) ? 1 : 0;
        int want = tex[t] == 0xFFFF ? -1 : tex[t];
        if( want != curTex || wantCull != curCull || wantDecal != curDecal ) {
            if( batchOpen ) glEnd();
            if( gl_texture_ready( want )) {
                glEnable( GL_TEXTURE_2D );
                glBindTexture( GL_TEXTURE_2D, gGlTex[want] );
            } else {
                glDisable( GL_TEXTURE_2D );
            }
            if( wantCull == 0 ) glDisable( GL_CULL_FACE );
            else { glEnable( GL_CULL_FACE ); glCullFace( wantCull == 1 ? GL_BACK : GL_FRONT ); }
            /* DECAL: bias toward the camera so overlays win the depth test
               against the surface they decal instead of z-fighting it. */
            if( wantDecal ) { glEnable( GL_POLYGON_OFFSET_FILL ); glPolygonOffset( -1.0f, -1.0f ); }
            else glDisable( GL_POLYGON_OFFSET_FILL );
            curTex = want; curCull = wantCull; curDecal = wantDecal;
            glBegin( GL_TRIANGLES );
            batchOpen = 1;
        }
        for( int k = 0; k < 3; ++k ) {
            uint32_t i = t * 3 + k;
            glColor4f( col[i*3], col[i*3+1], col[i*3+2], 1.0f );
            glTexCoord2fv( uv + i*2 );
            glVertex3fv( pos + i*3 );
        }
    }
    if( batchOpen ) glEnd();
    glDisable( GL_TEXTURE_2D );
    glDisable( GL_CULL_FACE );
    glDisable( GL_POLYGON_OFFSET_FILL );
    if( xluPass ) {
        glDisable( GL_BLEND );
        glDepthMask( GL_TRUE );
    } else {
        glDisable( GL_ALPHA_TEST );
    }
}

static void draw_arena_geometry( void )
{
    glDisable( GL_TEXTURE_2D );
    glBegin( GL_TRIANGLES );
    for( int i = 0; i < gArenaSurfaceCount; ++i ) {
        int material = gArenaSurfaceMaterial[i];
        if( material < 0 || material >= ARENA_MATERIAL_COUNT ) material = ARENA_STONE;
        glColor3fv( gArenaMaterialColor[material] );
        for( int v = 0; v < 3; ++v )
            glVertex3f((float)gArenaSurfaces[i].vertices[v][0],
                       (float)gArenaSurfaces[i].vertices[v][1],
                       (float)gArenaSurfaces[i].vertices[v][2] );
    }
    glEnd();

    /* A subtle 100-unit grid makes movement speed, attack reach and item
       range easy to judge without obscuring the actual collision course. */
    glColor4f( .12f, .24f, .15f, .55f );
    glLineWidth( 1.0f );
    glBegin( GL_LINES );
    for( int i = -(int)ARENA; i <= (int)ARENA; i += 100 ) {
        if( i > POOL_X0 && i < POOL_X1 ) {
            glVertex3f((float)i,1,-ARENA); glVertex3f((float)i,1,POOL_Z0);
            glVertex3f((float)i,1,POOL_Z1); glVertex3f((float)i,1,ARENA);
        } else {
            glVertex3f((float)i,1,-ARENA); glVertex3f((float)i,1,ARENA);
        }
        if( i > POOL_Z0 && i < POOL_Z1 ) {
            glVertex3f(-ARENA,1,(float)i); glVertex3f(POOL_X0,1,(float)i);
            glVertex3f(POOL_X1,1,(float)i); glVertex3f(ARENA,1,(float)i);
        } else {
            glVertex3f(-ARENA,1,(float)i); glVertex3f(ARENA,1,(float)i);
        }
    }
    glEnd();

    if( gShowCollision ) {
        glDisable( GL_DEPTH_TEST );
        glColor4f( .10f, 1.0f, .85f, .85f );
        glLineWidth( 1.5f );
        glBegin( GL_LINES );
        for( int i = 0; i < gArenaSurfaceCount; ++i ) {
            for( int e = 0; e < 3; ++e ) {
                int n = ( e + 1 ) % 3;
                glVertex3f((float)gArenaSurfaces[i].vertices[e][0],
                           (float)gArenaSurfaces[i].vertices[e][1] + 1.5f,
                           (float)gArenaSurfaces[i].vertices[e][2] );
                glVertex3f((float)gArenaSurfaces[i].vertices[n][0],
                           (float)gArenaSurfaces[i].vertices[n][1] + 1.5f,
                           (float)gArenaSurfaces[i].vertices[n][2] );
            }
        }
        glEnd();
        glEnable( GL_DEPTH_TEST );
    }
}

static void workbench_row_strings( int tab, int row, char *label, size_t labelSize,
                                   char *value, size_t valueSize )
{
    label[0] = value[0] = '\0';
    switch( tab ) {
    case PG_TAB_PLAY:
        switch( row ) {
        case 0: snprintf( label,labelSize,"SIMULATION" ); snprintf( value,valueSize,"%s",gManualPaused?"PAUSED":"RUNNING" ); break;
        case 1: snprintf( label,labelSize,"SINGLE STEP" ); snprintf( value,valueSize,"[ENTER]" ); break;
        case 2: snprintf( label,labelSize,"TIME SCALE" ); snprintf( value,valueSize,"%.2fX",gTimeScale ); break;
        case 3: snprintf( label,labelSize,"CAMERA" ); snprintf( value,valueSize,"%s",kCameraNames[gCameraMode] ); break;
        case 4: snprintf( label,labelSize,"ENEMY AI" ); snprintf( value,valueSize,"%s",pg_on_off(gEnemyAi) ); break;
        case 5: snprintf( label,labelSize,"RESPAWN LINK" ); snprintf( value,valueSize,"[ENTER]" ); break;
        case 6: snprintf( label,labelSize,"RESPAWN ENEMY" ); snprintf( value,valueSize,"%s",gSceneMode?"ARENA ONLY":"[ENTER]" ); break;
        }
        break;
    case PG_TAB_LINK:
        switch( row ) {
        case 0: snprintf(label,labelSize,"AGE"); snprintf(value,valueSize,"%s",kAgeNames[gSt.age<=1?gSt.age:0]); break;
        case 1: snprintf(label,labelSize,"SWORD REQUEST"); snprintf(value,valueSize,"%s",kSwordNames[gSword]); break;
        case 2: snprintf(label,labelSize,"SHIELD REQUEST"); snprintf(value,valueSize,"%s",kShieldNames[gShield]); break;
        case 3: snprintf(label,labelSize,"TUNIC REQUEST"); snprintf(value,valueSize,"%s",kTunicNames[gTunic]); break;
        case 4: snprintf(label,labelSize,"BOOTS REQUEST"); snprintf(value,valueSize,"%s",kBootNames[gBoots]); break;
        case 5: snprintf(label,labelSize,"FILL HEALTH"); snprintf(value,valueSize,"[ENTER]"); break;
        case 6: snprintf(label,labelSize,"INFLICT HALF HEART"); snprintf(value,valueSize,"[ENTER]"); break;
        case 7: snprintf(label,labelSize,"FILL DOUBLE MAGIC"); snprintf(value,valueSize,"[ENTER]"); break;
        }
        break;
    case PG_TAB_ITEMS:
        switch( row ) {
        case 0: snprintf(label,labelSize,"ITEM SELECT"); snprintf(value,valueSize,"%s",kItemNames[gItemChoice]); break;
        case 1: snprintf(label,labelSize,"EQUIP / TOGGLE"); snprintf(value,valueSize,"[ENTER]"); break;
        case 2: snprintf(label,labelSize,"REFILL ARROWS"); snprintf(value,valueSize,"%d / 30",gArrowAmmo); break;
        case 3: snprintf(label,labelSize,"REFILL BOMBS"); snprintf(value,valueSize,"%d / 20",gBombAmmo); break;
        case 4: snprintf(label,labelSize,"PUT ITEM AWAY"); snprintf(value,valueSize,"[ENTER]"); break;
        }
        break;
    case PG_TAB_WORLD:
        switch( row ) {
        case 0:
            snprintf(label,labelSize,"WORLD SELECT");
            snprintf(value,valueSize,"%s",gWorldChoice? kScenes[gWorldChoice-1].name:"FEATURE ARENA");
            break;
        case 1: snprintf(label,labelSize,"LOAD WORLD"); snprintf(value,valueSize,"[ENTER]"); break;
        case 2:
            snprintf(label,labelSize,"SCENE ROOM MESH");
            if( !gSceneMode ) snprintf(value,valueSize,"LOAD SCENE FIRST");
            else if( gSceneRoomChoice < 0 ) snprintf(value,valueSize,"ALL ROOMS (ACTIVE 0)");
            else snprintf(value,valueSize,"ROOM %d / %d",gSceneRoomChoice,
                          gSceneRuntimeValid?gSceneRuntime.roomCount-1:0);
            break;
        case 3: snprintf(label,labelSize,"APPLY SCENE ROOM"); snprintf(value,valueSize,"%s",gSceneMode?"[ENTER]":"SCENE ONLY"); break;
        case 4: snprintf(label,labelSize,"ARENA TEST ZONE"); snprintf(value,valueSize,"%s",kArenaSpawns[gArenaSpawnChoice].name); break;
        case 5: snprintf(label,labelSize,"TELEPORT TO ZONE"); snprintf(value,valueSize,"%s",gSceneMode?"ARENA ONLY":"[ENTER]"); break;
        case 6: snprintf(label,labelSize,"RELOAD COLLISION"); snprintf(value,valueSize,"%s",gSceneMode?"ARENA ONLY":"[ENTER]"); break;
        }
        break;
    case PG_TAB_AUDIO:
        switch( row ) {
        case 0: snprintf(label,labelSize,"MASTER VOLUME"); snprintf(value,valueSize,"%d%%",(int)(gMasterVolume*100+.5f)); break;
        case 1: snprintf(label,labelSize,"SCENE MUSIC MODE"); snprintf(value,valueSize,"%s",gSceneMusicAuto?"AUTO":"MANUAL"); break;
        case 2:
            snprintf(label,labelSize,"MUSIC SEQUENCE");
            snprintf(value,valueSize,"0X%02X  %s",gMusicSequenceChoice,
                     audio_sequence_name_safe(gMusicSequenceChoice));
            break;
        case 3:
            snprintf(label,labelSize,"PLAY SELECTED MUSIC");
            if( gMusicPlaying ) snprintf(value,valueSize,"PLAYING 0X%02X",gMusicActiveSequence);
            else snprintf(value,valueSize,"[ENTER]");
            break;
        case 4: snprintf(label,labelSize,"STOP MUSIC"); snprintf(value,valueSize,"[ENTER]"); break;
        case 5: snprintf(label,labelSize,"MUSIC VOLUME"); snprintf(value,valueSize,"%d%%",(int)(gMusicVolume*100+.5f)); break;
        case 6:
            snprintf(label,labelSize,"NATURE PRESET");
            snprintf(value,valueSize,"0X%02X / 0X%02X",gNatureChoice,
                     (unsigned)( OOT_AUDIO_NATURE_COUNT - 1 ));
            break;
        case 7:
            snprintf(label,labelSize,"PLAY NATURE (REPLACES BGM)");
            snprintf(value,valueSize,"%s",gAmbiencePlaying?"PLAYING":"[ENTER]");
            break;
        case 8: {
            struct OoTSfxInfo info;
            snprintf(label,labelSize,"SFX BANK");
            if( audio_sfx_current(&info) )
                snprintf(value,valueSize,"BANK %u  %s",(unsigned)info.bank,
                         audio_sfx_bank_name(info.bank));
            else snprintf(value,valueSize,"NO CATALOG");
            break;
        }
        case 9: {
            struct OoTSfxInfo info;
            snprintf(label,labelSize,"SFX ENTRY");
            if( audio_sfx_current(&info) )
                snprintf(value,valueSize,"%u / 0X%04X  %s",(unsigned)info.bankIndex,
                         (unsigned)info.sfxId,info.name[0]?info.name:"(unnamed)");
            else snprintf(value,valueSize,"NO CATALOG");
            break;
        }
        case 10: snprintf(label,labelSize,"PLAY SELECTED SFX"); snprintf(value,valueSize,"[ENTER]"); break;
        case 11: snprintf(label,labelSize,"STOP ALL SFX"); snprintf(value,valueSize,"[ENTER]"); break;
        case 12: snprintf(label,labelSize,"OCARINA"); snprintf(value,valueSize,"%s",gOcarinaOut?"OUT":"AWAY"); break;
        }
        break;
    case PG_TAB_RENDER:
        switch( row ) {
        case 0: snprintf(label,labelSize,"LINK VIEW"); snprintf(value,valueSize,"%s",gShowSkeleton?"SKELETON":"TEXTURED MODEL"); break;
        case 1: snprintf(label,labelSize,"SCENE MESH"); snprintf(value,valueSize,"%s",pg_on_off(gShowSceneMesh)); break;
        case 2: snprintf(label,labelSize,"PROJECTILE MESHES"); snprintf(value,valueSize,"%s",pg_on_off(gActorMeshesEnabled)); break;
        case 3: snprintf(label,labelSize,"NAVI WING MESH"); snprintf(value,valueSize,"%s",pg_on_off(gNaviMeshEnabled)); break;
        case 4: snprintf(label,labelSize,"ARENA COLLISION"); snprintf(value,valueSize,"%s",pg_on_off(gShowCollision)); break;
        case 5: snprintf(label,labelSize,"ACTOR BOUNDS"); snprintf(value,valueSize,"%s",pg_on_off(gShowActorBounds)); break;
        case 6: snprintf(label,labelSize,"DIAGNOSTICS PANEL"); snprintf(value,valueSize,"%s",pg_on_off(gDiagnosticsVisible)); break;
        }
        break;
    default: break;
    }
}

static void draw_workbench( int w, int h )
{
    float scale = h >= 800 ? 1.65f : 1.35f;
    float pw = fminf((float)w - 48.0f, 920.0f);
    float ph = fminf((float)h - 62.0f, 650.0f);
    float x = ( w - pw ) * .5f, y = ( h - ph ) * .5f;
    pg_ui_rect( 0, 0, (float)w, (float)h, .01f, .02f, .035f, .58f );
    pg_ui_rect( x, y, pw, ph, .025f, .045f, .075f, .97f );
    pg_ui_outline( x, y, pw, ph, 2, .22f, .72f, .86f, 1 );
    pg_ui_text( x + 22, y + 18, scale * 1.25f, .68f, .95f, 1, 1, "LIBOOT FEATURE WORKBENCH" );
    pg_ui_text( x + 22, y + 48, scale * .8f, .55f, .65f, .73f, 1,
                "TAB: CATEGORY   ARROWS: NAVIGATE / CHANGE   PGUP/DN: X10   ENTER: ACTIVATE" );

    float tabY = y + 82, tabX = x + 18;
    float each = ( pw - 36 ) / PG_TAB_COUNT;
    for( int i = 0; i < PG_TAB_COUNT; ++i ) {
        int active = i == gWorkbenchTab;
        pg_ui_rect( tabX + i * each, tabY, each - 3, 30,
                    active?.12f:.04f, active?.42f:.09f, active?.52f:.13f, 1 );
        pg_ui_text( tabX + i * each + 7, tabY + 9, scale * .75f,
                    active?.85f:.5f, active?1.0f:.62f, active?1.0f:.7f, 1, kTabNames[i] );
    }

    float contentY = tabY + 48;
    if( gWorkbenchTab == PG_TAB_HELP ) {
        static const char *lines[] = {
            "MOVEMENT: WASD     A: SPACE     B / ATTACK: J     Z: K     SHIELD: L",
            "ITEM: I (HOLD TO AIM, RELEASE TO FIRE)     OCARINA: E",
            "CAMERA: RIGHT MOUSE + MOVE, WHEEL ZOOM, C CYCLES MODE",
            "QUICK LOADOUT: 1-4 SWORDS, 5-8 ITEMS, SHIFT+5-8 SHIELDS / TUNIC",
            "F1-F3 BOOTS   F5/F6 SCENE   F7/F8 OR [ / ] PREV/NEXT ROOM",
            "F9 WORKBENCH     F10 HUD     F11 DIAGNOSTICS     P PAUSE     N STEP",
            "AUDIO TAB: AUTO MAP MUSIC / MANUAL SEQ + NATURE + BANKED SFX; PGUP/DN = 10",
            "CONTROLLER: LEFT STICK MOVE, A/X GAME BUTTONS, Y ITEM, LB Z, RB SHIELD",
            "ARENA: STAIRS NW, WATER NE, RAMP SW, HOOKSHOT TOWER SE, COMBAT CENTER",
            "TIP: OPEN THE ITEMS TAB TO TEST EVERY PUBLIC ITEM WITHOUT MEMORIZING KEYS.",
        };
        for( unsigned i = 0; i < sizeof(lines)/sizeof(lines[0]); ++i )
            pg_ui_text( x + 32, contentY + i * 34, scale * .88f, .78f, .86f, .91f, 1, lines[i] );
    } else {
        int rows = workbench_row_count( gWorkbenchTab );
        float available = y + ph - 52.0f - contentY;
        float rowH = rows > 0 ? fminf( 42.0f, available / rows ) : 42.0f;
        if( rowH < 28.0f ) rowH = 28.0f;
        for( int i = 0; i < rows; ++i ) {
            char label[64], value[96];
            workbench_row_strings( gWorkbenchTab, i, label, sizeof(label), value, sizeof(value) );
            float ry = contentY + i * rowH;
            if( i == gWorkbenchRow ) {
                pg_ui_rect( x + 24, ry - 5, pw - 48, rowH - 3, .11f, .28f, .35f, .95f );
                pg_ui_text( x + 34, ry + 7, scale, .95f, 1, .66f, 1, ">" );
            }
            pg_ui_text( x + 57, ry + 7, scale, .8f, .88f, .92f, 1, label );
            float valueWidth = pg_ui_text_width( value, scale );
            pg_ui_text( x + pw - 42 - valueWidth, ry + 7, scale, .45f, .94f, .95f, 1, value );
        }
        if( gWorkbenchTab == PG_TAB_WORLD && gSceneRuntimeValid ) {
            float infoY = contentY + rows * rowH + 2;
            pg_ui_textf( x+32,infoY,scale*.72f,.95f,.74f,.35f,1,
                         "LIVE: SCENE 0X%02X ACTIVE %d GEOMETRY %d / %d ROOMS",
                         gSceneRuntime.sceneIndex,gSceneRuntime.activeRoomIndex,
                         gSceneRuntime.geometryRoomIndex,gSceneRuntime.roomCount );
            pg_ui_textf( x+32,infoY+20,scale*.72f,.62f,.86f,.92f,1,
                         "TYPE %s  ENV %s  ECHO %d  LENS %u",
                         scene_room_type_name(gSceneRuntime.roomType),
                         scene_room_env_name(gSceneRuntime.environmentType),
                         gSceneRuntime.echo,gSceneRuntime.lensMode );
            pg_ui_text( x+32,infoY+40,scale*.68f,.55f,.7f,.75f,1,
                        "F7/F8 OR [ / ] CHANGES ROOM IMMEDIATELY" );
        }
    }
    pg_ui_text( x + 22, y + ph - 25, scale * .72f, .42f, .55f, .62f, 1,
                "REQUESTED EQUIPMENT MAY BE CLAMPED BY OOT AGE RULES" );
}

static void draw_feature_ui( int w, int h )
{
    audio_music_refresh_state();
    pg_ui_begin( w, h );
    if( gSceneMode ) scene_runtime_refresh();
    float s = h >= 800 ? 1.5f : 1.25f;
    if( gHudVisible ) {
        /* Vital stats. */
        pg_ui_rect( 12, 12, 285, 74, .015f, .025f, .04f, .78f );
        pg_ui_outline( 12, 12, 285, 74, 1, .18f, .35f, .42f, .85f );
        pg_ui_textf( 24, 22, s, 1, .45f, .48f, 1, "HEALTH  %d / %d", gSt.health, gSt.healthCapacity );
        float hp = gSt.healthCapacity > 0 ? (float)gSt.health / gSt.healthCapacity : 0;
        hp = fmaxf( 0, fminf( 1, hp ));
        pg_ui_rect( 24, 43, 248, 9, .18f, .035f, .05f, 1 );
        pg_ui_rect( 24, 43, 248 * hp, 9, .95f, .10f, .18f, 1 );
        if( gSt.magicLevel ) {
            float cap = 48.0f * gSt.magicLevel;
            float magic = fmaxf( 0, fminf( 1, gSt.magic / cap ));
            pg_ui_textf( 24, 61, s * .78f, .5f, 1, .65f, 1, "MAGIC %d/%d", gSt.magic, (int)cap );
            pg_ui_rect( 120, 64, 152, 7, .03f, .14f, .08f, 1 );
            pg_ui_rect( 120, 64, 152 * magic, 7, .12f, .82f, .32f, 1 );
        }

        const char *where = gSceneMode ? kScenes[gSceneSel].name : "FEATURE ARENA";
        float titleW = pg_ui_text_width( where, s * 1.05f );
        pg_ui_rect(( w - titleW )*.5f - 15, 12, titleW + 30, 35, .015f, .025f, .04f, .76f );
        pg_ui_text(( w - titleW )*.5f, 23, s * 1.05f, .75f, .95f, 1, 1, where );

        pg_ui_rect( w - 292.0f, 12, 280, 74, .015f, .025f, .04f, .78f );
        pg_ui_textf( w - 280.0f, 22, s*.9f, .76f, .88f, .93f, 1,
                     "FPS %.0f   FRAME %.1fMS", gRenderFps, gFrameMs );
        pg_ui_textf( w - 280.0f, 43, s*.9f, .5f, .84f, .9f, 1,
                     "CAM %s   %s", kCameraNames[gCameraMode], gManualPaused?"PAUSED":"LIVE" );
        pg_ui_text( w - 280.0f, 64, s*.72f, .95f, .83f, .35f, 1, "F9 WORKBENCH   F11 DIAGNOSTICS" );

        pg_ui_rect( 12, h - 91.0f, 380, 79, .015f, .025f, .04f, .78f );
        pg_ui_textf( 24, h - 79.0f, s*.9f, .75f, .86f, .92f, 1,
                     "STICK %+0.1f %+0.1f   A%d B%d Z%d R%d I%d", gIn.stickX, gIn.stickY,
                     gIn.buttonA,gIn.buttonB,gIn.buttonZ,gIn.buttonR,gIn.buttonItem );
        pg_ui_textf( 24, h - 55.0f, s*.82f, .52f, .78f, .84f, 1,
                     "POS %.0f %.0f %.0f   SPEED %.1f", gSt.position[0],gSt.position[1],gSt.position[2],gSt.linearVelocity );
        pg_ui_textf( 24, h - 32.0f, s*.82f, .52f, .78f, .84f, 1,
                     "WATER %s   AIR %u/300   LOCK %s   ENEMY %s", pg_on_off(gSt.inWater),
                     gSt.underwaterTimer, pg_on_off(gSt.lockOnActive),
                     gSceneMode?"OFF":kFoeStateNames[gFoe.state] );

        pg_ui_rect( w - 405.0f, h - 91.0f, 393, 79, .015f, .025f, .04f, .78f );
        pg_ui_textf( w - 393.0f, h - 79.0f, s*.88f, .83f, .9f, .94f, 1,
                     "%s | %s | %s | %s", kSwordNames[gSword],kShieldNames[gShield],kTunicNames[gTunic],kBootNames[gBoots] );
        pg_ui_textf( w - 393.0f, h - 54.0f, s*.88f, .6f, .9f, 1, 1,
                     "ITEM %s   ARROWS %d   BOMBS %d", kItemNames[gItemOut<=8?gItemOut:0],gArrowAmmo,gBombAmmo );
        pg_ui_textf( w - 393.0f, h - 30.0f, s*.78f, .7f, .72f, .76f, 1,
                     "TRIS %u   TEXTURES %d   ACTORS %d",gGeo.numTrianglesUsed,oot_get_texture_count(),gActorN );

        float toastY = 102;
        for( int n = 0; n < PG_TOASTS; ++n ) {
            int i = ( gToastHead - 1 - n + PG_TOASTS ) % PG_TOASTS;
            if( !gToasts[i].text[0] || (int32_t)( gToasts[i].until - gWallClockMs ) <= 0 ) continue;
            float tw = pg_ui_text_width( gToasts[i].text, s*.9f );
            pg_ui_rect((w-tw)*.5f-10,toastY-5,tw+20,24,.03f,.12f,.15f,.82f );
            pg_ui_text((w-tw)*.5f,toastY,s*.9f,.72f,1,.92f,1,gToasts[i].text );
            toastY += 28;
        }
    }

    if( gDiagnosticsVisible && !gWorkbenchOpen ) {
        float x = 12, y = 105, pw = 360, ph = 372;
        pg_ui_rect( x,y,pw,ph,.015f,.02f,.035f,.9f );
        pg_ui_outline( x,y,pw,ph,1,.16f,.55f,.65f,.9f );
        pg_ui_text( x+12,y+12,s,.55f,.95f,1,1,"LIVE DIAGNOSTICS" );
        pg_ui_textf(x+12,y+38,s*.8f,.72f,.8f,.84f,1,"SIM TICKS %llu  CATCHUP %d",(unsigned long long)gSimTicks,gCatchupLast);
        pg_ui_textf(x+12,y+58,s*.8f,.72f,.8f,.84f,1,"VEL %.2f %.2f %.2f",gSt.velocity[0],gSt.velocity[1],gSt.velocity[2]);
        pg_ui_textf(x+12,y+78,s*.8f,.72f,.8f,.84f,1,"FACE %d  ANIM FRAME %.2f",gSt.faceAngle,gSt.animFrame);
        pg_ui_textf(x+12,y+98,s*.8f,.72f,.8f,.84f,1,"FLAGS1 %08X",gSt.stateFlags1);
        pg_ui_textf(x+12,y+118,s*.8f,.72f,.8f,.84f,1,"FLAGS2 %08X",gSt.stateFlags2);
        pg_ui_textf(x+12,y+138,s*.8f,.72f,.8f,.84f,1,"MELEE %u  HELD ACTION %d",gSt.meleeWeaponState,gSt.heldItemAction);
        pg_ui_textf(x+12,y+158,s*.8f,.72f,.8f,.84f,1,"FOE HP %d  TARGET ID %d",gFoe.hp,(int)gFoeTarget);
        pg_ui_textf(x+12,y+178,s*.8f,.62f,.85f,.95f,1,"AIR %u/300  BGM SEQ %d  AMB %d",
                    gSt.underwaterTimer, oot_scene_get_sequence_id(), oot_scene_get_ambience_id());
        pg_ui_textf(x+12,y+198,s*.72f,.62f,.85f,.95f,1,
                    "MUSIC %s MAIN %s 0X%02X  NATURE %s 0X%02X",
                    gSceneMusicAuto?"AUTO":"MANUAL",gMusicPlaying?"PLAY":"STOP",
                    gMusicPlaying?gMusicActiveSequence:0,
                    gAmbiencePlaying?"PLAY":"STOP",
                    gAmbiencePlaying?gAmbiencePreset:0);
        pg_ui_textf(x+12,y+218,s*.8f,.62f,.85f,.95f,1,"LAST SONG %s",
                    gLastSong>=0 ? gSongs[gLastSong].name : "(none yet)");
        pg_ui_text( x+12,y+246,s*.82f,.55f,.95f,1,1,"RECENT GAMEPLAY SFX" );
        for( int n=0;n<6;++n ) {
            int i=(gSfxLogHead-1-n+PG_SFX_LOG)%PG_SFX_LOG;
            if(!gSfxLog[i].id) continue;
            pg_ui_textf(x+12,y+267+n*19,s*.72f,.7f,.8f,.85f,1,
                        "0X%04X %s VOL %.2f PITCH %.2f",gSfxLog[i].id,
                        gSfxLog[i].action==OOT_SFX_PLAY?(gSfxLog[i].hasSample?"PCM":"EVENT"):"STOP",
                        gSfxLog[i].volume,gSfxLog[i].pitch);
        }
        if( gSceneRuntimeValid && w >= 760 ) {
            float sx = 384, sy = 105, sw = fminf( 465.0f, w - sx - 12.0f ), sh = 174;
            pg_ui_rect( sx,sy,sw,sh,.015f,.02f,.035f,.9f );
            pg_ui_outline( sx,sy,sw,sh,1,.55f,.32f,.12f,.95f );
            pg_ui_text( sx+12,sy+12,s,.98f,.72f,.30f,1,"OOT SCENE RUNTIME" );
            pg_ui_textf(sx+12,sy+38,s*.8f,.82f,.86f,.9f,1,
                        "SCENE 0X%02X  ACTIVE %d  GEOMETRY %d  ROOMS %d",
                        gSceneRuntime.sceneIndex, gSceneRuntime.activeRoomIndex,
                        gSceneRuntime.geometryRoomIndex, gSceneRuntime.roomCount);
            pg_ui_textf(sx+12,sy+60,s*.8f,.82f,.86f,.9f,1,
                        "TYPE %u %s   ENV %u %s",gSceneRuntime.roomType,
                        scene_room_type_name(gSceneRuntime.roomType),
                        gSceneRuntime.environmentType,
                        scene_room_env_name(gSceneRuntime.environmentType));
            pg_ui_textf(sx+12,sy+82,s*.8f,.82f,.86f,.9f,1,
                        "ECHO %d   LENS %u %s",gSceneRuntime.echo,
                        gSceneRuntime.lensMode,
                        gSceneRuntime.lensMode?"HIDE ACTORS":"SHOW ACTORS");
            pg_ui_textf(sx+12,sy+104,s*.8f,.82f,.86f,.9f,1,
                        "WARP DISABLED %u   CAM 0X%02X   MAP %d",
                        gSceneRuntime.warpSongsDisabled,gSceneRuntime.sceneCamType,
                        gSceneRuntime.worldMapArea);
            pg_ui_textf(sx+12,sy+126,s*.76f,.55f,.9f,.95f,1,
                        "HEADER METADATA %s   ALL ROOMS %s",
                        gSceneRuntime.roomMetadataValid?"VALID":"MISSING",
                        gSceneRuntime.allRoomsLoaded?"YES":"NO");
            pg_ui_text( sx+12,sy+149,s*.72f,.95f,.83f,.35f,1,
                        "[ / ] CHANGE ROOM   F6 NEXT SCENE" );
        }
    }
    if( gWorkbenchOpen ) draw_workbench( w, h );
    pg_ui_end();
}

static void draw_scene( int w, int hgt )
{
    if( w <= 0 || hgt <= 0 ) return;
    glViewport( 0, 0, w, hgt );
    /* liboot vNEXT: drive GL fog from the scene's real light settings so depth
       cueing matches the game. Clear to the fog color so distant geometry fades
       into a matching horizon (the playground has no skybox). */
    int fogOn = gSceneMode && gSceneEnvValid && gSceneEnv.valid;
    if( fogOn )      glClearColor( gSceneEnv.fogColor[0], gSceneEnv.fogColor[1], gSceneEnv.fogColor[2], 1 );
    else if( gSceneMode ) glClearColor( .55f, .72f, .92f, 1 );   /* soft sky blue */
    else             glClearColor( .35f, .55f, .8f, 1 );
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
    glEnable( GL_DEPTH_TEST );
    if( fogOn ) {
        float end = gSceneEnv.fogFar > 1.0f ? gSceneEnv.fogFar : 6000.0f;
        float start = end * ( gSceneEnv.fogNear / 1000.0f );
        if( start >= end ) start = end * 0.25f;
        GLfloat fc[4] = { gSceneEnv.fogColor[0], gSceneEnv.fogColor[1], gSceneEnv.fogColor[2], 1.0f };
        glFogi( GL_FOG_MODE, GL_LINEAR );
        glFogfv( GL_FOG_COLOR, fc );
        glFogf( GL_FOG_START, start );
        glFogf( GL_FOG_END, end );
        glEnable( GL_FOG );
    } else {
        glDisable( GL_FOG );
    }

    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    /* scenes span thousands of units (Kokiri ~ -3000..5200): push far clip */
    frustum( 55.0f, (float)w / hgt, 5.0f, gSceneMode ? 12000.0f : 5000.0f );
    glMatrixMode( GL_MODELVIEW );
    glLoadIdentity();
    look_at( gCamPos, gCameraAt );

    if( gSceneMode ) {
        if( gShowSceneMesh ) draw_scene_mesh( 0 ); /* opaque room geometry */
        goto after_arena;
    }
    draw_arena_geometry();

after_arena:
    /* enemy (FOE_DEAD in scene mode: never drawn there) */
    if( gFoe.state != FOE_DEAD ) {
        if( gFoe.invuln > 0 && ( gFoe.invuln & 2 )) glColor3f( 1, 1, 1 );
        else if( gFoe.state == FOE_WINDUP ) glColor3f( 1.0f, .55f, .1f );
        else glColor3f( .8f, .15f, .15f );
        glPushMatrix();
        glTranslatef( gFoe.x, 0, gFoe.z );
        glRotatef( gFoe.yaw * 180.0f / PI_F, 0, 1, 0 );
        cube( 0, 0, 0, 40, 60, 30 );
        cube( 0, 62, 0, 24, 24, 24 );        /* skull */
        glPopMatrix();
        /* hp pips */
        glDisable( GL_DEPTH_TEST );
        glPointSize( 6 );
        glColor3f( 1, .2f, .2f );
        glBegin( GL_POINTS );
        for( int i = 0; i < gFoe.hp; ++i )
            glVertex3f( gFoe.x + ( i - gFoe.hp/2.0f ) * 10, 100, gFoe.z );
        glEnd();
        glEnable( GL_DEPTH_TEST );
    }

    if( gShowActorBounds ) {
        glDisable( GL_DEPTH_TEST );
        glLineWidth( 1.5f );
        glColor3f( .25f, 1.0f, .55f );
        wire_box( gSt.position[0], gSt.position[1], gSt.position[2],
                  38.0f, gSt.age == OOT_AGE_CHILD ? 65.0f : 90.0f, 38.0f );
        if( gFoe.state != FOE_DEAD ) {
            glColor3f( 1.0f, .25f, .25f );
            wire_box( gFoe.x, gFoe.y, gFoe.z, 52, 92, 52 );
        }
        glColor3f( 1.0f, .8f, .15f );
        for( int i = 0; i < gActorN; ++i )
            if( gActors[i].active )
                wire_box( gActors[i].pos[0], gActors[i].pos[1]-10,
                          gActors[i].pos[2], 28, 28, 28 );
        glEnable( GL_DEPTH_TEST );
    }

    /* lock-on reticle: spinning yellow diamond over the locked foe */
    if( gSt.lockOnActive ) {
        glDisable( GL_DEPTH_TEST );
        glPushMatrix();
        glTranslatef( gSt.lockOnPos[0], gSt.lockOnPos[1], gSt.lockOnPos[2] );
        glRotatef( SDL_GetTicks() * 0.25f, 0, 1, 0 );
        glColor3f( 1.0f, 0.85f, 0.1f );
        glLineWidth( 3.0f );
        for( int p = 0; p < 2; ++p ) {          /* two crossed diamonds */
            glPushMatrix();
            glRotatef( 90.0f * p, 0, 1, 0 );
            glBegin( GL_LINE_LOOP );
            glVertex3f( 0, 28, 0 ); glVertex3f( 17, 0, 0 );
            glVertex3f( 0, -28, 0 ); glVertex3f( -17, 0, 0 );
            glEnd();
            glPopMatrix();
        }
        glPopMatrix();
        glEnable( GL_DEPTH_TEST );
    }

    draw_link();

    if( gSceneMode ) {
        if( gShowSceneMesh ) {
            /* translucent room geometry (pond water, windows...) after Link so
               he shows through it, exactly like the arena's water quad */
            draw_scene_mesh( 1 );
        }
    } else {
        /* water surface: translucent blue quad at the WaterBox height, drawn
           after all opaque geometry so the basin and a submerged Link show
           through; depth writes off so Navi's glow still blends over it */
        glEnable( GL_BLEND );
        glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
        glDepthMask( GL_FALSE );
        glColor4f( .18f, .45f, .85f, .5f );
        glBegin( GL_QUADS );
        glVertex3f( POOL_X0, WATER_Y, POOL_Z0 ); glVertex3f( POOL_X0, WATER_Y, POOL_Z1 );
        glVertex3f( POOL_X1, WATER_Y, POOL_Z1 ); glVertex3f( POOL_X1, WATER_Y, POOL_Z0 );
        glEnd();
        glDepthMask( GL_TRUE );
        glDisable( GL_BLEND );
    }

    /* Navi: glowing camera-facing billboard (radial-fade fan, core -> halo),
       pulsing; her animated wing mesh rides in the Link geometry buffers via
       oot_navi_set_render and is drawn by draw_link_model above */
    {
        float npos[3], ic[4], oc[4], nscale;
        if( oot_navi_get( npos, ic, oc, &nscale ) && nscale > 0.0f ) {
            float mv[16];
            glGetFloatv( GL_MODELVIEW_MATRIX, mv );
            float rx[3] = { mv[0], mv[4], mv[8] };   /* camera right */
            float ux[3] = { mv[1], mv[5], mv[9] };   /* camera up    */
            float pulse = 1.0f + 0.15f * sinf( SDL_GetTicks() * 0.012f );
            float r = 1500.0f * nscale * pulse;
            glDepthMask( GL_FALSE );
            glEnable( GL_BLEND );
            glBlendFunc( GL_SRC_ALPHA, GL_ONE );     /* additive glow */
            for( int layer = 0; layer < 2; ++layer ) {
                float rad = layer ? r * 0.4f : r;    /* halo, then core */
                glBegin( GL_TRIANGLE_FAN );
                glColor4f( ic[0], ic[1], ic[2], layer ? 0.95f : 0.5f );
                glVertex3fv( npos );
                glColor4f( oc[0], oc[1], oc[2], 0.0f );
                for( int i = 0; i <= 12; ++i ) {
                    float a = (float)i * PI_F / 6.0f;
                    float cx = cosf( a ) * rad, cy = sinf( a ) * rad;
                    glVertex3f( npos[0] + rx[0]*cx + ux[0]*cy,
                                npos[1] + rx[1]*cx + ux[1]*cy,
                                npos[2] + rx[2]*cx + ux[2]*cy );
                }
                glEnd();
            }
            glDisable( GL_BLEND );
            glDepthMask( GL_TRUE );
        }
    }

    /* bomb explosion: white->orange expanding billboard flash (~20 frames)
       at the bomb's last live position (from oot_actor_query) */
    if( gFlash.timer > 0 ) {
        float mv[16];
        glGetFloatv( GL_MODELVIEW_MATRIX, mv );
        float rx[3] = { mv[0], mv[4], mv[8] };   /* camera right */
        float ux[3] = { mv[1], mv[5], mv[9] };   /* camera up    */
        float t = 1.0f - gFlash.timer / 20.0f;   /* 0 -> 1 over the flash */
        float r = 40.0f + 160.0f * t;            /* expanding sphere radius */
        float fade = 1.0f - t;
        glDepthMask( GL_FALSE );
        glEnable( GL_BLEND );
        glBlendFunc( GL_SRC_ALPHA, GL_ONE );     /* additive fireball */
        for( int layer = 0; layer < 2; ++layer ) {
            float rad = layer ? r * 0.55f : r;   /* orange shell, then white core */
            glBegin( GL_TRIANGLE_FAN );
            if( layer ) glColor4f( 1.0f, 1.0f, 0.92f, 0.9f * fade );
            else        glColor4f( 1.0f, 0.55f, 0.10f, 0.55f * fade );
            glVertex3fv( gFlash.pos );
            glColor4f( 1.0f, 0.30f, 0.05f, 0.0f );
            for( int i = 0; i <= 12; ++i ) {
                float a = (float)i * PI_F / 6.0f;
                float cx = cosf( a ) * rad, cy = sinf( a ) * rad;
                glVertex3f( gFlash.pos[0] + rx[0]*cx + ux[0]*cy,
                            gFlash.pos[1] + rx[1]*cx + ux[1]*cy,
                            gFlash.pos[2] + rx[2]*cx + ux[2]*cy );
            }
            glEnd();
        }
        glDisable( GL_BLEND );
        glDepthMask( GL_TRUE );
    }

    glDisable( GL_FOG );   /* never fog the 2D HUD/overlay */
    draw_feature_ui( w, hgt );
}

/* ---- main -------------------------------------------------------------- */

static void print_usage( const char *program )
{
    fprintf( stderr, "usage: %s <oot-rom.z64> [--frames N | --scene-frames N | --suite N | --features N | --ui-frames N]\n", program );
}

static int parse_frame_count( const char *text, long *out )
{
    char *end = NULL;
    errno = 0;
    long value = strtol( text, &end, 10 );
    if( errno || !end || *end != '\0' || value <= 0 ) return 0;
    *out = value;
    return 1;
}

/* N64 stick X follows the ordinary controller convention: negative is left,
   positive is right. Keep the keyboard path in one pure helper so headless
   validation catches an accidental sign flip. */
static float playground_stick_x( int leftHeld, int rightHeld )
{
    return ( rightHeld ? 1.0f : 0.0f ) - ( leftHeld ? 1.0f : 0.0f );
}

static uint8_t *read_rom( const char *path, size_t *outSize )
{
    *outSize = 0;
    FILE *file = fopen( path, "rb" );
    if( !file ) {
        fprintf( stderr, "rom: %s: %s\n", path, strerror( errno ));
        return NULL;
    }
    if( fseek( file, 0, SEEK_END ) != 0 ) {
        fprintf( stderr, "rom: cannot seek %s: %s\n", path, strerror( errno ));
        fclose( file );
        return NULL;
    }
    long fileSize = ftell( file );
    if( fileSize <= 0 || (uintmax_t)fileSize > SIZE_MAX || fseek( file, 0, SEEK_SET ) != 0 ) {
        fprintf( stderr, "rom: invalid size for %s\n", path );
        fclose( file );
        return NULL;
    }
    uint8_t *rom = malloc((size_t)fileSize );
    if( !rom ) {
        fprintf( stderr, "rom: cannot allocate %ld bytes\n", fileSize );
        fclose( file );
        return NULL;
    }
    size_t got = fread( rom, 1, (size_t)fileSize, file );
    if( got != (size_t)fileSize ) {
        fprintf( stderr, "rom: short read from %s (%zu/%ld bytes)%s%s\n", path, got, fileSize,
                 ferror( file ) ? ": " : "", ferror( file ) ? strerror( errno ) : "" );
        free( rom );
        fclose( file );
        return NULL;
    }
    if( fclose( file ) != 0 ) {
        fprintf( stderr, "rom: close failed for %s: %s\n", path, strerror( errno ));
        free( rom );
        return NULL;
    }
    *outSize = (size_t)fileSize;
    return rom;
}

static void close_audio( void )
{
    if( gAudioDev ) {
        SDL_PauseAudioDevice( gAudioDev, 1 );
        SDL_CloseAudioDevice( gAudioDev );
        gAudioDev = 0;
    }
    memset( gVoice, 0, sizeof( gVoice ));
    memset( &gOcNote, 0, sizeof( gOcNote ));
    if( gAudioInitialized ) {
        SDL_AudioQuit();
        gAudioInitialized = 0;
    }
}

static void shutdown_liboot( void )
{
    if( !gLibootInitialized ) return;
    oot_set_sfx_callback_ex( NULL );
    if( gFoeTarget >= 0 ) {
        oot_target_remove( gFoeTarget );
        gFoeTarget = -1;
    }
    if( gLink >= 0 ) {
        oot_link_delete( gLink );
        gLink = -1;
    }
    oot_global_terminate();
    gLibootInitialized = 0;
}

static void debug_print( const char *message )
{
    if( message ) puts( message );
}

static int suite_check_state( const char *phase )
{
    if( gLink < 0 || gGeo.numTrianglesUsed == 0 ||
        gGeo.numTrianglesUsed > OOT_GEO_MAX_TRIANGLES ) {
        fprintf( stderr, "[suite] %s: invalid Link/geometry (%d, %u tris)\n",
                 phase, (int)gLink, gGeo.numTrianglesUsed );
        return 0;
    }
    for( int axis = 0; axis < 3; ++axis )
        if( !isfinite( gSt.position[axis] ) || !isfinite( gSt.velocity[axis] )) {
            fprintf( stderr, "[suite] %s: non-finite Link state\n", phase );
            return 0;
        }
    int textureCount = oot_get_texture_count();
    for( int t = 0; t < gGeo.numTrianglesUsed; ++t ) {
        if( gGeoTex[t] != 0xFFFF && gGeoTex[t] >= textureCount ) {
            fprintf( stderr, "[suite] %s: invalid texture index %u/%d\n",
                     phase, gGeoTex[t], textureCount );
            return 0;
        }
        for( int v = 0; v < 3; ++v ) {
            int i = t * 3 + v;
            for( int a = 0; a < 3; ++a )
                if( !isfinite( gGeoPos[i*3+a] ) || !isfinite( gGeoNrm[i*3+a] ) ||
                    !isfinite( gGeoCol[i*3+a] )) {
                    fprintf( stderr, "[suite] %s: non-finite vertex data\n", phase );
                    return 0;
                }
            if( !isfinite( gGeoUv[i*2] ) || !isfinite( gGeoUv[i*2+1] )) {
                fprintf( stderr, "[suite] %s: non-finite UV data\n", phase );
                return 0;
            }
        }
    }
    if( !gPoseValid || gPose.numJoints == 0 || gPose.numJoints > OOT_SKELETON_MAX_JOINTS ) {
        fprintf( stderr, "[suite] %s: invalid skeleton\n", phase );
        return 0;
    }
    return 1;
}

static unsigned suite_tick_scan( int ticks, int holdItem, int pressA )
{
    unsigned seen = 0;
    for( int tick = 0; tick < ticks && !gFatalError; ++tick ) {
        memset( &gIn, 0, sizeof( gIn ));
        gIn.buttonItem = holdItem;
        gIn.buttonA = pressA && tick == 0;
        sim_tick();
        for( int i = 0; i < gActorN; ++i ) {
            if( !gActors[i].active ) continue;
            if( gActors[i].id == AID_EN_ARROW ) seen |= 1u << 0;
            if( gActors[i].id == AID_EN_BOM )   seen |= 1u << 1;
            if( gActors[i].id == 0x66 )         seen |= 1u << 2;
            if( gActors[i].id == AID_EN_BOOM )  seen |= 1u << 3;
        }
    }
    return seen;
}

/* liboot vNEXT: deterministic self-test for the newly wired single features
   (Link action/attackAnim/look/floor state, set_pose, freeze, set_invincible,
   scene_query_surface, C-up input). Prints one [features] line per check and
   returns 1 on full pass. Uses a local input so it fully controls each case. */
static int run_feature_suite( long stressFrames )
{
    int failures = 0;
    struct OoTLinkInputs in;

    if( !link_spawn()) { fprintf( stderr, "[features] link_spawn failed\n" ); return 0; }
    oot_link_set_equipment( gLink, OOT_SWORD_KOKIRI, OOT_SHIELD_DEKU,
                            OOT_TUNIC_KOKIRI, OOT_BOOTS_KOKIRI );

    printf( "[features] --- liboot single-feature inspection ---\n" );

    /* action: an untouched Link settles into OOT_ACTION_IDLE. */
    memset( &in, 0, sizeof in );
    for( int i = 0; i < 8; ++i ) oot_link_tick( gLink, &in, &gSt, &gGeo );
    printf( "[features] state: action=%u (idle=%u) look=(%d,%d) floorSfx=%u stateFlags3=0x%02X\n",
            gSt.action, OOT_ACTION_IDLE, gSt.lookPitch, gSt.lookYaw,
            gSt.floorSfxOffset, gSt.stateFlags3 );
    if( gSt.action != OOT_ACTION_IDLE ) {
        fprintf( stderr, "[features] idle action = %u, expected OOT_ACTION_IDLE(%u)\n",
                 gSt.action, OOT_ACTION_IDLE );
        failures++;
    }
    if( gSt.action > OOT_ACTION_TIME_TRAVEL_END ) {
        fprintf( stderr, "[features] action %u out of enum range\n", gSt.action );
        failures++;
    }

    /* attackAnim: a sword swing reports a live meleeWeaponState + an anim id. */
    memset( &in, 0, sizeof in ); in.buttonB = 1;
    int sawSwing = 0;
    uint8_t swingAnim = 0;
    for( int i = 0; i < 24; ++i ) {
        oot_link_tick( gLink, &in, &gSt, &gGeo );
        if( gSt.meleeWeaponState ) { sawSwing = 1; swingAnim = gSt.attackAnim; }
        in.buttonB = ( i < 2 );
    }
    printf( "[features] attack: swing detected=%d attackAnim(PLAYER_MWA)=%u\n", sawSwing, swingAnim );
    if( !sawSwing ) { fprintf( stderr, "[features] no sword swing detected\n" ); failures++; }

    /* scene_query_surface: the arena floor is directly under the origin. */
    {
        struct OoTSurfaceInfo si;
        if( !oot_scene_query_surface( 0.0f, 200.0f, 0.0f, &si ) ) {
            fprintf( stderr, "[features] query_surface found no floor under origin\n" );
            failures++;
        } else if( !( si.groundY > -1e9f && si.groundY < 1e9f ) ) {
            fprintf( stderr, "[features] query_surface returned non-finite groundY\n" );
            failures++;
        }
    }

    /* set_pose: the documented clean-reposition idiom is freeze + set_pose, so
       the next tick reports the exact spot instead of a physics-settled one. */
    memset( &in, 0, sizeof in );
    oot_link_freeze( gLink, true );
    oot_link_set_pose( gLink, 400.0f, 0.0f, -250.0f, 0x4000 );
    oot_link_tick( gLink, &in, &gSt, &gGeo );
    oot_link_freeze( gLink, false );
    printf( "[features] set_pose: landed (%.1f,%.1f,%.1f) face=0x%04X\n",
            gSt.position[0], gSt.position[1], gSt.position[2], (unsigned short)gSt.faceAngle );
    if( fabsf( gSt.position[0] - 400.0f ) > 2.0f ||
        fabsf( gSt.position[2] - (-250.0f) ) > 2.0f ||
        (unsigned short)gSt.faceAngle != 0x4000 ) {
        fprintf( stderr, "[features] set_pose landed off-target\n" );
        failures++;
    }

    /* freeze: forward input does not move a frozen Link; unfreeze resumes. */
    {
        float sx = gSt.position[0], sz = gSt.position[2];
        oot_link_freeze( gLink, true );
        memset( &in, 0, sizeof in ); in.stickY = 1.0f;
        for( int i = 0; i < 10; ++i ) oot_link_tick( gLink, &in, &gSt, &gGeo );
        float d2 = ( gSt.position[0]-sx )*( gSt.position[0]-sx ) +
                   ( gSt.position[2]-sz )*( gSt.position[2]-sz );
        oot_link_freeze( gLink, false );
        for( int i = 0; i < 12; ++i ) oot_link_tick( gLink, &in, &gSt, &gGeo );
        float d2b = ( gSt.position[0]-sx )*( gSt.position[0]-sx ) +
                    ( gSt.position[2]-sz )*( gSt.position[2]-sz );
        printf( "[features] freeze: frozen moved^2=%.1f (want ~0), unfrozen moved^2=%.1f (want >0)\n", d2, d2b );
        if( d2 > 1.0f ) { fprintf( stderr, "[features] frozen Link moved\n" ); failures++; }
        if( d2b < 1.0f ) { fprintf( stderr, "[features] unfrozen Link did not move\n" ); failures++; }
    }

    /* set_invincible + C-up: must not crash and the sim keeps running. */
    oot_link_set_invincible( gLink, -30 );
    memset( &in, 0, sizeof in ); in.buttonCUp = 1;
    for( int i = 0; i < 8; ++i ) oot_link_tick( gLink, &in, &gSt, &gGeo );
    printf( "[features] invincible+C-up: health=%d action=%u (sim alive)\n", gSt.health, gSt.action );
    if( gSt.health <= 0 ) { fprintf( stderr, "[features] Link unexpectedly dead\n" ); failures++; }

    /* stress: hammer all the new setters under motion for stability. */
    for( long i = 0; i < stressFrames; ++i ) {
        memset( &in, 0, sizeof in );
        in.stickY = 1.0f;
        in.buttonB = ( i % 30 ) < 3;
        in.buttonCUp = ( i % 40 ) >= 30;
        if( ( i % 90 ) == 0 ) oot_link_set_pose( gLink, 0.0f, 0.0f, 0.0f, 0 );
        if( ( i % 50 ) == 25 ) oot_link_freeze( gLink, true );
        if( ( i % 50 ) == 45 ) oot_link_freeze( gLink, false );
        oot_link_set_invincible( gLink, -1 );
        oot_link_tick( gLink, &in, &gSt, &gGeo );
        if( gFatalError ) { fprintf( stderr, "[features] fatal during stress at %ld\n", i ); failures++; break; }
        if( !( gSt.position[0] > -1e9f && gSt.position[0] < 1e9f ) ) {
            fprintf( stderr, "[features] non-finite position during stress at %ld\n", i );
            failures++; break;
        }
    }
    oot_link_freeze( gLink, false );

    /* vNEXT deferred trio -------------------------------------------------- */

    /* per-vertex alpha: poison the buffer to a sentinel first, then require the
       tick to OVERWRITE every emitted vertex (proves emission actually ran, not
       just zero-init) AND land in [0,1]. Without the sentinel this check would
       pass on an all-zero (never-written) buffer too. */
    memset( &in, 0, sizeof in );
    for( int i = 0; i < OOT_GEO_MAX_TRIANGLES * 3; ++i ) gGeoAlpha[i] = -1.0f;
    oot_link_tick( gLink, &in, &gSt, &gGeo );
    {
        int bad = 0; float amin = 9.0f, amax = -9.0f;
        for( int i = 0; i < gGeo.numTrianglesUsed * 3; ++i ) {
            float a = gGeoAlpha[i];
            if( a < amin ) amin = a;
            if( a > amax ) amax = a;
            if( a == -1.0f || !( a >= 0.0f && a <= 1.0f ) ) bad = 1;
        }
        printf( "[features] alpha: %u verts, range [%.2f,%.2f]\n", gGeo.numTrianglesUsed * 3, amin, amax );
        if( gGeo.numTrianglesUsed == 0 || bad ) {
            fprintf( stderr, "[features] vertex alpha not emitted / out of range (tris=%u)\n",
                     gGeo.numTrianglesUsed );
            failures++;
        }
    }

    /* actor spawn: a bomb (ACTOR_EN_BOM = 0x10) places, a bad id is rejected. */
    {
        int spawned = oot_actor_spawn( 0x10, gSt.position[0], gSt.position[1],
                                       gSt.position[2] + 40.0f, 0, 0, 0, 0 );
        int rejected = oot_actor_spawn( 0x7FFF, 0, 0, 0, 0, 0, 0, 0 );
        struct OoTActorInfo ai[32];
        int na = oot_actor_query( ai, 32 ), bombs = 0;
        for( int i = 0; i < na; ++i ) if( ai[i].id == 0x10 ) bombs++;
        printf( "[features] actor_spawn(EN_BOM)=%d (bombs live=%d), reject(bad id)=%d\n",
                spawned, bombs, rejected );
        if( spawned != 0 ) { fprintf( stderr, "[features] oot_actor_spawn(EN_BOM) failed\n" ); failures++; }
        if( rejected != -2 ) {
            fprintf( stderr, "[features] oot_actor_spawn did not reject an unsupported id\n" );
            failures++;
        }
    }

    /* OoTSurface.type materials: a typed flat floor reports the mapped
       SurfaceType through oot_scene_query_surface. */
    {
        struct OoTSurfaceInfo si;
        struct OoTSurface tri[2] = {
            { 0, {{-1000,0,-1000},{-1000,0,1000},{1000,0,1000}} },
            { 0, {{-1000,0,-1000},{1000,0,1000},{1000,0,-1000}} },
        };
        static const struct { int type; const char *name; } kMat[] = {
            { OOT_SURFACE_DEFAULT, "DEFAULT" }, { OOT_SURFACE_SAND, "SAND" },
            { OOT_SURFACE_DAMAGE, "DAMAGE" },   { OOT_SURFACE_SLIPPERY, "SLIPPERY" },
            { OOT_SURFACE_CONVEYOR, "CONVEYOR" },{ OOT_SURFACE_NO_HOOKSHOT, "NO_HOOKSHOT" },
        };
        for( size_t m = 0; m < sizeof( kMat ) / sizeof( kMat[0] ); ++m ) {
            tri[0].type = tri[1].type = (uint16_t)kMat[m].type;
            oot_static_world_load( tri, 2, NULL, 0 );
            if( oot_scene_query_surface( 0, 100, 0, &si ) )
                printf( "[features] surface %-11s -> floorType=%u material=%u hookshot=%u\n",
                        kMat[m].name, si.floorType, si.material, si.hookshot );
            else { fprintf( stderr, "[features] surface %s: no floor\n", kMat[m].name ); failures++; }
        }
        /* Assert the discriminating mappings, not just that a value printed. */
        tri[0].type = tri[1].type = OOT_SURFACE_NO_HOOKSHOT;
        oot_static_world_load( tri, 2, NULL, 0 );
        if( !oot_scene_query_surface( 0, 100, 0, &si ) || si.hookshot != 0 ) {
            fprintf( stderr, "[features] NO_HOOKSHOT surface still hookshot-attachable\n" ); failures++; }
        tri[0].type = tri[1].type = OOT_SURFACE_DAMAGE;
        oot_static_world_load( tri, 2, NULL, 0 );
        if( !oot_scene_query_surface( 0, 100, 0, &si ) || si.floorType != 2 ) {
            fprintf( stderr, "[features] DAMAGE surface floorType=%u, expected 2\n", si.floorType ); failures++; }
        tri[0].type = tri[1].type = OOT_SURFACE_DEFAULT;
        oot_static_world_load( tri, 2, NULL, 0 );
        if( !oot_scene_query_surface( 0, 100, 0, &si ) || si.hookshot != 1 ) {
            fprintf( stderr, "[features] DEFAULT surface not hookshot-attachable\n" ); failures++; }
    }

    /* door transitions: a dungeon exposes transition actors (doors), and
       oot_scene_set_room swaps the active room (unload previous, load next). */
    {
        int rc = oot_scene_load( OOT_SCENE_DEKU_TREE, 0 );
        if( rc != 0 ) { fprintf( stderr, "[features] Deku Tree load rc=%d\n", rc ); failures++; }
        else {
            int nd = oot_scene_get_door_count();
            struct OoTDoor d;
            struct OoTSceneRuntime runtime;
            if( !oot_scene_get_runtime( &runtime ) || runtime.sceneIndex != OOT_SCENE_DEKU_TREE ||
                runtime.activeRoomIndex != 0 || runtime.geometryRoomIndex != 0 ||
                !runtime.roomMetadataValid ) {
                fprintf( stderr, "[features] initial scene runtime mismatch\n" );
                failures++;
            }
            printf( "[features] doors: Deku Tree has %d transition actors\n", nd );
            if( nd <= 0 || !oot_scene_get_door( 0, &d )) {
                fprintf( stderr, "[features] no doors / get_door failed\n" ); failures++;
            } else {
                int target = ( d.backRoom >= 0 ) ? d.backRoom : d.frontRoom;
                int sr = oot_scene_set_room( target );
                const float *p; const uint16_t *t; uint32_t n = 0, x = 0;
                oot_scene_get_geometry( &p, &p, &p, &p, &t, &n, &x );
                printf( "[features] set_room(%d) via door 0 (rooms %d<->%d): rc=%d tris=%u\n",
                        target, d.frontRoom, d.backRoom, sr, n );
                if( sr != 0 || n == 0 ) { fprintf( stderr, "[features] room transition failed\n" ); failures++; }
                if( !oot_scene_get_runtime( &runtime ) || runtime.activeRoomIndex != target ||
                    runtime.geometryRoomIndex != target ) {
                    fprintf( stderr, "[features] transitioned room runtime mismatch\n" ); failures++;
                }
                if( oot_scene_set_room( -1 ) != 0 ) { fprintf( stderr, "[features] full re-load failed\n" ); failures++; }
                else if( !oot_scene_get_runtime( &runtime ) || runtime.activeRoomIndex != 0 ||
                         runtime.geometryRoomIndex != -1 || !runtime.allRoomsLoaded ) {
                    fprintf( stderr, "[features] aggregate room runtime mismatch\n" ); failures++;
                }
            }
        }
    }

    /* ocarina songs: every one of the twelve (the six warp/teleport songs come
       first) has a canonical note pattern, matches itself exactly, and is still
       recognised by the longest-tail matcher after junk lead-in notes.  Notes
       are 0..4 (A / C-down / C-right / C-left / C-up). */
    {
        static const char *kSongName[OOT_SONG_COUNT] = {
            "Minuet", "Bolero", "Serenade", "Requiem", "Nocturne", "Prelude",
            "Saria", "Epona", "Lullaby", "Suns", "Time", "Storms",
        };
        int allMatched = 1, warpMatched = 1;
        for( int s = 0; s < OOT_SONG_COUNT; ++s ) {
            uint8_t notes[8]; int32_t n = 0;
            if( !oot_ocarina_song_notes( s, notes, &n ) || n < 3 || n > 8 ) {
                fprintf( stderr, "[features] ocarina: %s bad pattern (n=%d)\n",
                         kSongName[s], n );
                failures++; allMatched = 0; if( s < 6 ) warpMatched = 0; continue;
            }
            for( int i = 0; i < n; ++i )
                if( notes[i] > 4 ) {
                    fprintf( stderr, "[features] ocarina: %s note %d out of 0..4\n",
                             kSongName[s], notes[i] );
                    failures++;
                }
            if( oot_ocarina_match( notes, n ) != s ) {
                fprintf( stderr, "[features] ocarina: %s did not self-match\n", kSongName[s] );
                failures++; allMatched = 0; if( s < 6 ) warpMatched = 0;
            }
            /* longest-tail match: a "0,3" lead-in cannot start any real song's
               longer pattern (Bolero begins with 1, Nocturne with 3-2-2), so the
               matcher must still land on this song. */
            uint8_t buf[10]; buf[0] = 0; buf[1] = 3;
            memcpy( buf + 2, notes, (size_t)n );
            if( oot_ocarina_match( buf, n + 2 ) != s ) {
                fprintf( stderr, "[features] ocarina: %s tail-match failed\n", kSongName[s] );
                failures++; if( s < 6 ) warpMatched = 0;
            }
        }
        /* negative + degenerate inputs must not false-positive. */
        uint8_t allCup[8] = { 4, 4, 4, 4, 4, 4, 4, 4 };
        if( oot_ocarina_match( allCup, 8 ) != -1 ) {
            fprintf( stderr, "[features] ocarina: junk sequence wrongly matched\n" ); failures++; }
        if( oot_ocarina_match( allCup, 0 ) != -1 || oot_ocarina_match( NULL, 5 ) != -1 ) {
            fprintf( stderr, "[features] ocarina: empty/NULL not rejected\n" ); failures++; }
        printf( "[features] ocarina: 12 songs, warp/teleport songs matched=%s, all matched=%s\n",
                warpMatched ? "yes" : "no", allMatched ? "yes" : "no" );
    }

    /* scene sound settings (cmd 0x15): dungeons expose a background-music
       sequence id and a nature-ambience id for a host's sequenced-audio player.
       -1 means the scene declares none. */
    {
        static const struct { int scene; const char *name; } kSnd[] = {
            { OOT_SCENE_DEKU_TREE,       "Deku Tree" },
            { OOT_SCENE_DODONGOS_CAVERN, "Dodongo"   },
            { OOT_SCENE_JABU_JABU,       "Jabu-Jabu" },
            { OOT_SCENE_FOREST_TEMPLE,   "Forest"    },
            { OOT_SCENE_FIRE_TEMPLE,     "Fire"      },
            { OOT_SCENE_WATER_TEMPLE,    "Water"     },
            { OOT_SCENE_SHADOW_TEMPLE,   "Shadow"    },
        };
        int sawBgm = 0;
        for( size_t i = 0; i < sizeof( kSnd ) / sizeof( kSnd[0] ); ++i ) {
            if( oot_scene_load( kSnd[i].scene, -1 ) != 0 ) {
                fprintf( stderr, "[features] sound: %s load failed\n", kSnd[i].name );
                failures++; continue;
            }
            int seq = oot_scene_get_sequence_id();
            int amb = oot_scene_get_ambience_id();
            printf( "[features] sound: %-9s seqId=%d ambienceId=%d\n",
                    kSnd[i].name, seq, amb );
            if( seq < -1 || seq > 255 || amb < -1 || amb > 255 ) {
                fprintf( stderr, "[features] sound: %s id out of range (%d/%d)\n",
                         kSnd[i].name, seq, amb );
                failures++;
            }
            if( seq >= 0 ) sawBgm = 1;
        }
        if( !sawBgm ) {
            fprintf( stderr, "[features] no dungeon reported a bgm sequence id\n" );
            failures++;
        }
    }

    /* scene lighting/fog: the scene's EnvLightSettings are exported AND baked
       into the emitted vertex colors, so a lit dungeon is neither fullbright
       (all ~1.0) nor uniform — the shade varies with surface orientation. */
    {
        if( oot_scene_load( OOT_SCENE_DEKU_TREE, -1 ) != 0 ) {
            fprintf( stderr, "[features] lighting: Deku Tree load failed\n" ); failures++;
        } else {
            struct OoTSceneEnvironment env;
            int haveEnv = oot_scene_get_environment( &env );
            const float *pos = NULL, *nrm = NULL, *col = NULL, *uv = NULL;
            const uint16_t *tex = NULL; uint32_t n = 0, x = 0;
            oot_scene_get_geometry( &pos, &nrm, &col, &uv, &tex, &n, &x );
            float cmin = 9.0f, cmax = -9.0f; int allWhite = 1;
            if( col && n ) {
                for( uint32_t i = 0; i < n * 9; ++i ) {
                    float c = col[i];
                    if( c < cmin ) cmin = c;
                    if( c > cmax ) cmax = c;
                    if( c < 0.999f ) allWhite = 0;
                }
            }
            printf( "[features] lighting: env valid=%d ambient=(%.2f %.2f %.2f) fog=(%.2f %.2f %.2f) near=%.0f far=%.0f; color range [%.2f,%.2f]\n",
                    haveEnv && env.valid, env.ambientColor[0], env.ambientColor[1], env.ambientColor[2],
                    env.fogColor[0], env.fogColor[1], env.fogColor[2], env.fogNear, env.fogFar, cmin, cmax );
            if( !haveEnv || !env.valid ) {
                fprintf( stderr, "[features] lighting: Deku Tree reported no environment\n" ); failures++;
            }
            if( n == 0 || col == NULL ) {
                fprintf( stderr, "[features] lighting: no scene colors emitted\n" ); failures++;
            } else if( allWhite ) {
                fprintf( stderr, "[features] lighting: scene still fullbright (all colors ~1.0)\n" ); failures++;
            } else if( cmax - cmin < 0.02f ) {
                fprintf( stderr, "[features] lighting: scene shade does not vary\n" ); failures++;
            }
            if( haveEnv && env.valid &&
                ( env.fogNear < 0.0f || env.fogNear > 1023.0f || env.fogFar <= 0.0f ) ) {
                fprintf( stderr, "[features] lighting: fog params out of range\n" ); failures++;
            }
            /* per-triangle render flags: parallel to the triangles, only known
               bits set, and a real dungeon uses culling on at least some faces. */
            const uint8_t *tflags = NULL;
            int haveFlags = oot_scene_get_triangle_flags( &tflags );
            uint32_t culled = 0, atest = 0, decal = 0, badbits = 0;
            if( haveFlags && tflags && n ) {
                for( uint32_t i = 0; i < n; ++i ) {
                    uint8_t f = tflags[i];
                    if( f & ( OOT_TRI_CULL_FRONT | OOT_TRI_CULL_BACK )) culled++;
                    if( f & OOT_TRI_ALPHA_TEST ) atest++;
                    if( f & OOT_TRI_DECAL ) decal++;
                    if( f & ~( OOT_TRI_CULL_FRONT | OOT_TRI_CULL_BACK |
                               OOT_TRI_ALPHA_TEST | OOT_TRI_DECAL )) badbits++;
                }
            }
            printf( "[features] triflags: present=%d, culled %u/%u, alpha-test %u, decal %u, %u bad-bit\n",
                    haveFlags && tflags != NULL, culled, n, atest, decal, badbits );
            if( !haveFlags || tflags == NULL ) {
                fprintf( stderr, "[features] triflags: not exposed for a loaded scene\n" ); failures++;
            } else if( badbits ) {
                fprintf( stderr, "[features] triflags: undefined bits set on %u triangles\n", badbits ); failures++;
            } else if( culled == 0 ) {
                fprintf( stderr, "[features] triflags: no culling flags on any triangle (expected some)\n" ); failures++;
            }
        }
    }

    /* drown/underwater timer: with iron boots Link sinks in the deep basin and
       the submerged-air timer (Player.underwaterTimer) climbs from 0; back on
       dry land it must sit at 0. */
    {
        uint8_t savedBoots = gBoots;
        load_arena();
        gBoots = OOT_BOOTS_IRON;
        uint16_t peak = 0; int rose = 0;
        if( !link_spawn_at( 680.0f, -90.0f, 560.0f )) { failures++; }
        else {
            memset( &in, 0, sizeof in );
            for( int i = 0; i < 120; ++i ) {
                oot_link_tick( gLink, &in, &gSt, &gGeo );
                if( gSt.underwaterTimer > peak ) peak = gSt.underwaterTimer;
                if( gSt.underwaterTimer > 0 ) rose = 1;
                if( gSt.underwaterTimer > 300 ) {
                    fprintf( stderr, "[features] underwaterTimer overflow %u\n",
                             gSt.underwaterTimer );
                    failures++; break;
                }
            }
        }
        printf( "[features] underwater: submerged peak timer=%u inWater=%u (want >0)\n",
                peak, gSt.inWater );
        if( !rose ) {
            fprintf( stderr, "[features] underwaterTimer never rose while submerged\n" );
            failures++;
        }
        gBoots = OOT_BOOTS_KOKIRI;
        if( link_spawn_at( 0.0f, 0.0f, 0.0f )) {
            memset( &in, 0, sizeof in );
            for( int i = 0; i < 20; ++i ) oot_link_tick( gLink, &in, &gSt, &gGeo );
            printf( "[features] underwater: dry-land timer=%u (want 0)\n", gSt.underwaterTimer );
            if( gSt.underwaterTimer != 0 ) {
                fprintf( stderr, "[features] dry-land underwaterTimer nonzero (%u)\n",
                         gSt.underwaterTimer );
                failures++;
            }
        }
        gBoots = savedBoots;
    }

    printf( "[features] done: %s, checks over %ld stress ticks\n",
            failures ? "FAILED" : "PASS", stressFrames );
    return failures == 0;
}

static int run_headless_suite( long stressFrames )
{
    int failures = 0;
    unsigned itemSeen = 0;

    if( playground_stick_x( 1, 0 ) >= 0.0f ||
        playground_stick_x( 0, 1 ) <= 0.0f ) {
        fprintf( stderr, "[suite] playground left/right input mapping is reversed\n" );
        failures++;
    }

    /* Verify the mapping through the real Player movement, not just helper
       signs. With the default camera behind Link looking along +Z, screen
       right is world -X and screen left is world +X. */
    {
        struct OoTLinkInputs lateral = { .camLookZ = 1.0f };
        float rightX = 0.0f, leftX = 0.0f;
        if( !link_spawn()) {
            failures++;
        } else {
            lateral.stickX = playground_stick_x( 0, 1 );
            for( int i = 0; i < 24; ++i )
                oot_link_tick( gLink, &lateral, &gSt, &gGeo );
            rightX = gSt.position[0];
        }
        if( !link_spawn()) {
            failures++;
        } else {
            lateral.stickX = playground_stick_x( 1, 0 );
            for( int i = 0; i < 24; ++i )
                oot_link_tick( gLink, &lateral, &gSt, &gGeo );
            leftX = gSt.position[0];
        }
        if( rightX >= -10.0f || leftX <= 10.0f ) {
            fprintf( stderr,
                     "[suite] lateral movement failed (right x=%.1f, left x=%.1f)\n",
                     rightX, leftX );
            failures++;
        }
        if( !link_spawn()) failures++;
    }

    /* Playground combat is intentionally directional: an active sword swing
       may hit a nearby foe in front of Link, never the same foe behind him. */
    {
        int initialHp = 3;
        gSt.position[0] = gSt.position[2] = 0.0f;
        gSt.faceAngle = 0; /* +Z */
        gSt.meleeWeaponState = 1;
        gSt.isDead = 0;
        gFoe.state = FOE_WINDUP;
        gFoe.timer = 100;
        gFoe.invuln = 0;
        gFoe.hp = initialHp;
        gFoe.x = 0.0f;
        gFoe.z = -50.0f;
        foe_tick();
        if( gFoe.hp != initialHp ) {
            fprintf( stderr, "[suite] sword incorrectly hit a foe behind Link\n" );
            failures++;
        }
        gFoe.state = FOE_WINDUP;
        gFoe.timer = 100;
        gFoe.invuln = 0;
        gFoe.x = 0.0f;
        gFoe.z = 50.0f;
        foe_tick();
        if( gFoe.hp != initialHp - 1 ) {
            fprintf( stderr, "[suite] sword did not hit a nearby foe in front of Link\n" );
            failures++;
        }
        gSt.meleeWeaponState = 0;
        foe_spawn();
        if( !link_spawn()) failures++;
    }

    gEnemyAi = 0;
    memset( &gIn, 0, sizeof( gIn ));
    for( int i = 0; i < 8; ++i ) sim_tick();
    if( !suite_check_state( "initial arena" )) failures++;

    /* Validate every decoded texture currently exposed by the ROM cache. */
    int textureCount = oot_get_texture_count();
    for( int i = 0; i < textureCount; ++i ) {
        struct OoTTextureInfo info;
        const uint8_t *pixels = NULL;
        if( !oot_get_texture( i, &info, &pixels ) || !pixels || !info.width || !info.height ||
            info.wrapS > 2 || info.wrapT > 2 ) {
            fprintf( stderr, "[suite] texture %d is invalid\n", i );
            failures++;
            break;
        }
    }

    /* Audio extraction contract: two known Link/Navi voices and all notes. */
    const int16_t *voice = NULL;
    uint32_t voiceCount = 0, voiceRate = 0;
    if( !oot_get_voice_sample( 0x6800, &voice, &voiceCount, &voiceRate ) ||
        !voice || !voiceCount || !voiceRate ) {
        fprintf( stderr, "[suite] Link voice sample 0x6800 unavailable\n" );
        failures++;
    }
    if( !oot_get_voice_sample( 0x6844, &voice, &voiceCount, &voiceRate ) ||
        !voice || !voiceCount || !voiceRate ) {
        fprintf( stderr, "[suite] Navi voice sample 0x6844 unavailable\n" );
        failures++;
    }
    uint32_t previousRate = 0;
    for( uint8_t note = 0; note < 5; ++note ) {
        const int16_t *pcm = NULL; uint32_t count = 0, rate = 0, loop = 0;
        if( !oot_get_ocarina_note( note, &pcm, &count, &rate, &loop ) || !pcm ||
            !count || !rate || loop >= count || ( note && rate <= previousRate )) {
            fprintf( stderr, "[suite] ocarina note %u is invalid\n", note );
            failures++;
            break;
        }
        previousRate = rate;
    }

    /* Adult item actors. Edges are deliberately separated by multiple game
       ticks so this is deterministic even on slow CI hosts. */
    set_link_age( OOT_AGE_ADULT );
    item_toggle( OOT_ITEM_BOW, "bow" );
    itemSeen |= suite_tick_scan( 16, 1, 0 );
    itemSeen |= suite_tick_scan( 30, 0, 0 );
    item_toggle( OOT_ITEM_BOMB, "bomb" );
    itemSeen |= suite_tick_scan( 18, 0, 0 );
    itemSeen |= suite_tick_scan( 8, 0, 1 );
    item_toggle( OOT_ITEM_HOOKSHOT, "hookshot" );
    itemSeen |= suite_tick_scan( 14, 1, 0 );
    itemSeen |= suite_tick_scan( 26, 0, 0 );
    set_link_age( OOT_AGE_CHILD );
    item_toggle( OOT_ITEM_BOOMERANG, "boomerang" );
    itemSeen |= suite_tick_scan( 2, 1, 0 );
    itemSeen |= suite_tick_scan( 36, 0, 0 );
    if(( itemSeen & 0x0Fu ) != 0x0Fu ) {
        fprintf( stderr, "[suite] projectile coverage missing (seen mask 0x%X, wanted 0xF)\n",
                 itemSeen );
        failures++;
    }
    if( !suite_check_state( "items" )) failures++;

    /* Water membership and boots path in the custom collision world. */
    gItemOut = OOT_ITEM_NONE;
    oot_link_use_item( gLink, OOT_ITEM_NONE );
    if( !link_spawn_at( 680.0f, -45.0f, 560.0f )) failures++;
    itemSeen |= suite_tick_scan( 6, 0, 0 );
    if( !gSt.inWater || fabsf( gSt.waterSurfaceY - WATER_Y ) > .1f ) {
        fprintf( stderr, "[suite] water box not detected (inWater=%u surface=%.1f)\n",
                 gSt.inWater, gSt.waterSurfaceY );
        failures++;
    }
    gBoots = OOT_BOOTS_IRON;
    apply_loadout();
    suite_tick_scan( 6, 0, 0 );

    /* Every public scene must provide a finite spawn and bounded geometry. */
    for( int scene = 0; scene < N_SCENES; ++scene ) {
        if( !scene_enter( scene )) { failures++; continue; }
        const float *pos = NULL, *nrm = NULL, *col = NULL, *uv = NULL;
        const uint16_t *tex = NULL; uint32_t count = 0, xlu = 0;
        if( !oot_scene_get_geometry( &pos,&nrm,&col,&uv,&tex,&count,&xlu ) ||
            !pos || !nrm || !col || !uv || !tex || !count ||
            count > OOT_SCENE_MAX_TRIANGLES || xlu > count ) {
            fprintf( stderr, "[suite] scene %s geometry invalid\n", kScenes[scene].name );
            failures++;
            continue;
        }
        int sceneTextureCount = oot_get_texture_count();
        for( uint32_t t = 0; t < count; ++t ) {
            if( tex[t] != 0xFFFF && tex[t] >= sceneTextureCount ) {
                fprintf( stderr, "[suite] scene %s has invalid texture %u/%d\n",
                         kScenes[scene].name, tex[t], sceneTextureCount );
                failures++;
                break;
            }
            if( tex[t] != 0xFFFF ) {
                struct OoTTextureInfo info; const uint8_t *pixels = NULL;
                if( !oot_get_texture( tex[t], &info, &pixels ) || !pixels ||
                    !info.width || !info.height || info.wrapS > 2 || info.wrapT > 2 ) {
                    fprintf( stderr, "[suite] scene %s references invalid texture %u\n",
                             kScenes[scene].name, tex[t] );
                    failures++;
                    break;
                }
            }
        }
        for( uint32_t i = 0; i < count * 3; ++i ) {
            if( !isfinite( pos[i*3] ) || !isfinite( pos[i*3+1] ) || !isfinite( pos[i*3+2] ) ||
                !isfinite( nrm[i*3] ) || !isfinite( nrm[i*3+1] ) || !isfinite( nrm[i*3+2] ) ||
                !isfinite( col[i*3] ) || !isfinite( col[i*3+1] ) || !isfinite( col[i*3+2] ) ||
                !isfinite( uv[i*2] ) || !isfinite( uv[i*2+1] )) {
                fprintf( stderr, "[suite] scene %s contains non-finite geometry\n", kScenes[scene].name );
                failures++;
                break;
            }
        }
        suite_tick_scan( 2, 0, 0 );
        if( !suite_check_state( kScenes[scene].name )) failures++;
    }
    textureCount = oot_get_texture_count(); /* cache may contain unreferenced sparse slots */
    if( !scene_leave()) failures++;
    set_link_age( OOT_AGE_ADULT );
    gBoots = OOT_BOOTS_KOKIRI;
    apply_loadout();

    /* Deterministic mixed-input stress pass, retaining the requested count. */
    for( long i = 0; i < stressFrames && !gFatalError; ++i ) {
        memset( &gIn, 0, sizeof( gIn ));
        gIn.stickY = ( i % 120 ) < 45 ? 1.0f : 0.0f;
        gIn.stickX = ( i % 120 ) >= 90 ? .55f : 0.0f;
        gIn.buttonA = ( i % 97 ) == 30;
        gIn.buttonB = ( i % 73 ) == 15;
        gIn.buttonZ = ( i % 160 ) >= 120;
        sim_tick();
        if(( i % 100 ) == 0 && !suite_check_state( "stress" )) { failures++; break; }
    }
    if( gFatalError ) failures++;
    printf( "[suite] done: %s, stress=%ld ticks, items=0x%X, textures=%d, scenes=%d\n",
            failures ? "FAILED" : "PASS", stressFrames, itemSeen & 0xF, textureCount, N_SCENES );
    return failures == 0;
}

int main( int argc, char **argv )
{
    if( argc < 2 ) { print_usage( argv[0] ); return 1; }
    long benchFrames = 0, sceneFrames = 0, suiteFrames = 0, uiFrames = 0, featureFrames = 0;
    if( argc != 2 && argc != 4 ) {
        print_usage( argv[0] );
        return 1;
    }
    if( argc == 4 ) {
        long *destination = NULL;
        if( !strcmp( argv[2], "--frames" )) destination = &benchFrames;
        else if( !strcmp( argv[2], "--scene-frames" )) destination = &sceneFrames;
        else if( !strcmp( argv[2], "--suite" )) destination = &suiteFrames;
        else if( !strcmp( argv[2], "--features" )) destination = &featureFrames;
        else if( !strcmp( argv[2], "--ui-frames" )) destination = &uiFrames;
        if( !destination || !parse_frame_count( argv[3], destination )) {
            fprintf( stderr, "invalid playground option/count\n" );
            print_usage( argv[0] );
            return 1;
        }
    }

    int exitCode = 1;
    SDL_Window *win = NULL;
    SDL_GLContext ctx = NULL;
    SDL_GameController *controller = NULL;
    size_t romSize = 0;
    uint8_t *rom = read_rom( argv[1], &romSize );
    if( !rom ) return 1;

    oot_set_debug_print_function( debug_print );
    oot_global_init( rom, romSize, NULL );
    gLibootInitialized = 1;
    int audioWarmCount = 0;
    for( uint16_t id = 0; id < OOT_AUDIO_SEQUENCE_COUNT; ++id )
        audioWarmCount += oot_audio_sequence_prewarm( id ) ? 1 : 0;
    printf( "[audio] prewarmed %d/%u ROM sequences before device start\n",
            audioWarmCount, (unsigned)OOT_AUDIO_SEQUENCE_COUNT );
    free( rom );
    load_arena();
    if( !link_spawn()) {
        fprintf( stderr, "link_create failed (bad or unsupported ROM?)\n" );
        goto cleanup;
    }
    foe_spawn();
    oot_navi_set_render( true );      /* wing mesh -> geometry buffers */
    oot_actor_set_render( true );     /* v0.6 projectile meshes -> geometry buffers */
    oot_set_sfx_callback_ex( sfx_cb ); /* preserve pitch/volume from Player */

    if( benchFrames > 0 ) {           /* headless self-test: no window */
        for( long i = 0; i < benchFrames; ++i ) {
            /* 150-frame cycle: run, then two standing slashes, then a Z
               press-edge well clear of any melee action.  Run only while the
               foe is far and in front: the attention wedge is +-60 degrees of
               Link's facing, so outrunning the chasing foe (leaving it behind
               him) would make every Z press whiff. */
            float fdx = gFoe.x - gSt.position[0], fdz = gFoe.z - gSt.position[2];
            float fdist = sqrtf( fdx*fdx + fdz*fdz ) + 1e-6f;
            float ffwd = ( fdx * gIn.camLookX + fdz * gIn.camLookZ ) / fdist;
            gIn.stickY = (( i % 150 ) < 40 && fdist > 250.0f && ffwd > 0.5f ) ? 1.0f : 0.0f;
            gIn.buttonB = ( i % 150 ) == 60 || ( i % 150 ) == 75;
            gIn.buttonZ = ( i % 150 ) >= 110;       /* exercise Z-lock */
            sim_tick();
            if( gFatalError ) break;
        }
        printf( "bench done: link hp=%d foe hp=%d kills=%d pos=(%.0f,%.0f)\n",
                gSt.health, gFoe.hp, gKills, gSt.position[0], gSt.position[2] );
        exitCode = gFatalError ? 1 : 0;
        goto cleanup;
    }

    if( sceneFrames > 0 ) {           /* headless scene-mode self-test */
        if( !scene_enter( 0 )) {
            fprintf( stderr, "scene bench: could not enter %s\n", kScenes[0].name );
            goto cleanup;
        }
        int attemptedScene = 0;
        for( long i = 0; i < sceneFrames; ++i ) {
            int desiredScene = (int)((long double)i * N_SCENES / sceneFrames );
            if( desiredScene >= N_SCENES ) desiredScene = N_SCENES - 1;
            if( desiredScene != attemptedScene ) {
                attemptedScene = desiredScene;
                if( !scene_enter( desiredScene ))
                    printf( "[scene] cycle to %s failed, staying in %s\n",
                            kScenes[desiredScene].name, kScenes[gSceneSel].name );
            }
            /* scripted stroll: walk forward with brief pauses, a hop each
               cycle, a slight veer near the end of every 100 frames */
            gIn.stickY = (( i % 50 ) < 35 ) ? 1.0f : 0.0f;
            gIn.stickX = (( i % 100 ) >= 80 ) ? 0.6f : 0.0f;
            gIn.buttonA = ( i % 50 ) == 40;
            gIn.buttonB = gIn.buttonZ = gIn.buttonR = gIn.buttonItem = 0;
            sim_tick();
            if( gFatalError ) break;
            if( gSt.position[1] < -2000.0f ) {
                fprintf( stderr, "scene bench: fell through the world at frame %ld (y=%.0f)\n",
                         i, gSt.position[1] );
                goto cleanup;
            }
        }
        printf( "scene bench done: %s pos=(%.0f, %.0f, %.0f) hp=%d inWater=%d\n",
                kScenes[gSceneSel].name, gSt.position[0], gSt.position[1],
                gSt.position[2], gSt.health, gSt.inWater );
        exitCode = gFatalError ? 1 : 0;
        goto cleanup;
    }

    if( suiteFrames > 0 ) {           /* broad deterministic API/playground suite */
        exitCode = run_headless_suite( suiteFrames ) ? 0 : 1;
        goto cleanup;
    }

    if( featureFrames > 0 ) {         /* vNEXT single-feature self-test */
        exitCode = run_feature_suite( featureFrames ) ? 0 : 1;
        goto cleanup;
    }

    if( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER ) != 0 ) {
        fprintf( stderr, "SDL_Init(VIDEO/GAMECONTROLLER) failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    if( SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 ) != 0 ||
        SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 ) != 0 ||
        SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 2 ) != 0 ||
        SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 1 ) != 0 ) {
        fprintf( stderr, "SDL OpenGL attribute setup failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    win = SDL_CreateWindow( "liboot feature playground",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1152, 768,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI );
    if( !win ) {
        fprintf( stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    SDL_SetWindowMinimumSize( win, 900, 600 );
    ctx = SDL_GL_CreateContext( win );
    if( !ctx ) {
        fprintf( stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    if( SDL_GL_MakeCurrent( win, ctx ) != 0 ) {
        fprintf( stderr, "SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    if( SDL_GL_SetSwapInterval( 1 ) != 0 )
        fprintf( stderr, "[gfx] vertical sync unavailable: %s\n", SDL_GetError());
    if( !pg_ui_init()) {
        fprintf( stderr, "[gfx] playground UI font initialization failed\n" );
        goto cleanup;
    }
    for( int i = 0; i < SDL_NumJoysticks() && !controller; ++i )
        if( SDL_IsGameController( i )) controller = SDL_GameControllerOpen( i );
    if( controller )
        printf( "[input] controller: %s\n", SDL_GameControllerName( controller ));
    open_audio();
    if( gAudioDev ) SDL_PauseAudioDevice( gAudioDev, 0 );

    puts( "\n=== liboot feature playground ===\n"
          "F9 or TAB opens the full workbench; F10 HUD; F11 diagnostics; F12 help\n"
          "AUDIO tab: scene music, 110 AudioSeq tracks, 19 nature presets and 1,259 SFX\n"
          "WASD move | SPACE A | J attack | L shield | K Z | E ocarina | T age | V view\nocarina out: J + arrow keys play notes (try Zelda's Lullaby: left up right x2)\n"
          "1-4 swords | 5 bow  6 bomb  7 hookshot (adult) | 8 boomerang (child)\n"
          "I = item button: hold to draw/aim (bow/hookshot), release to fire;\n"
          "    press for another bomb / boomerang throw; bomb: A throws, then RUN\n"
          "SHIFT+5/6/7 shields Deku/Hylian/Mirror | SHIFT+8,9,0 tunics Kokiri/Goron/Zora\n"
          "F1-F3 boots | H hurt | M magic | R respawn\n"
          "F5 scene mode: arena <-> ROM scene | F6 cycles all 18 test scenes\n"
          "F7/F8 (or [ / ]) previous/next room; F11 shows live OoT room metadata\n"
          "arrows/boomerang hit the Stalchild on contact, bombs in a 100-unit blast\n"
          "pool in the NE corner: swim with the stick, A dives, iron boots (F2) sink and walk,\n"
          "walk up the west ramp to get out; NW stairs, SW ramp and SE hookshot tower\n"
          "right mouse rotates the orbit camera, wheel zooms, C cycles camera modes\n" );

    int running = 1;
    long renderedFrames = 0;
    int controllerTriggerDown = 0;
    uint32_t lastFrame = SDL_GetTicks();
    double simAccumulator = 0.0;
    gWallClockMs = lastFrame;
    if( uiFrames > 0 ) {
        gWorkbenchOpen = 1;
        gWorkbenchTab = PG_TAB_RENDER;
        gDiagnosticsVisible = 1;
    }

    while( running ) {
        SDL_Event ev;
        while( SDL_PollEvent( &ev )) {
            if( ev.type == SDL_QUIT ) running = 0;
            if( ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST ) {
                memset( &gIn, 0, sizeof( gIn ));
                gLatchA = gLatchB = gLatchZ = gLatchR = gLatchItem = 0;
                gMouseLook = 0;
                SDL_SetRelativeMouseMode( SDL_FALSE );
                ocarina_release();
            }
            if( ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT ) {
                gMouseLook = 1;
                SDL_SetRelativeMouseMode( SDL_TRUE );
            }
            if( ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_RIGHT ) {
                gMouseLook = 0;
                SDL_SetRelativeMouseMode( SDL_FALSE );
            }
            if( ev.type == SDL_MOUSEMOTION && gMouseLook && !gWorkbenchOpen ) {
                gCameraYaw -= ev.motion.xrel * .006f;
                gCameraPitch -= ev.motion.yrel * .005f;
                gCameraMode = PG_CAM_ORBIT;
            }
            if( ev.type == SDL_MOUSEWHEEL && !gWorkbenchOpen )
                gCameraDistance -= ev.wheel.y * 30.0f;
            if( ev.type == SDL_CONTROLLERBUTTONDOWN ) {
                if( ev.cbutton.button == SDL_CONTROLLER_BUTTON_START ) {
                    gWorkbenchOpen = !gWorkbenchOpen;
                    gWorkbenchTab = PG_TAB_PLAY;
                    gWorkbenchRow = 0;
                    if( gWorkbenchOpen ) gLatchA = gLatchB = gLatchZ = gLatchR = gLatchItem = 0;
                } else if( gWorkbenchOpen ) {
                    switch( ev.cbutton.button ) {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP: workbench_key( SDLK_UP, KMOD_NONE ); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: workbench_key( SDLK_DOWN, KMOD_NONE ); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: workbench_key( SDLK_LEFT, KMOD_NONE ); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: workbench_key( SDLK_RIGHT, KMOD_NONE ); break;
                    case SDL_CONTROLLER_BUTTON_A: workbench_key( SDLK_RETURN, KMOD_NONE ); break;
                    case SDL_CONTROLLER_BUTTON_B: workbench_key( SDLK_ESCAPE, KMOD_NONE ); break;
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: workbench_key( SDLK_TAB, KMOD_SHIFT ); break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: workbench_key( SDLK_TAB, KMOD_NONE ); break;
                    default: break;
                    }
                } else {
                    if( ev.cbutton.button == SDL_CONTROLLER_BUTTON_A ) gLatchA = 1;
                    if( ev.cbutton.button == SDL_CONTROLLER_BUTTON_X ) gLatchB = 1;
                    if( ev.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ) gLatchZ = 1;
                    if( ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ) gLatchR = 1;
                    if( ev.cbutton.button == SDL_CONTROLLER_BUTTON_Y ) gLatchItem = 1;
                }
            }
            if( ev.type == SDL_CONTROLLERAXISMOTION &&
                ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ) {
                int down = ev.caxis.value > 8000;
                if( down && !controllerTriggerDown && !gWorkbenchOpen ) gLatchZ = 1;
                controllerTriggerDown = down;
            }
            if( ev.type == SDL_KEYDOWN && !ev.key.repeat ) {
                SDL_Keycode k = ev.key.keysym.sym;
                uint8_t needEquip = 0;
                if( gWorkbenchOpen ) {
                    if( k == SDLK_F9 ) gWorkbenchOpen = 0;
                    else if( k == SDLK_F7 || k == SDLK_LEFTBRACKET ) scene_room_step( -1 );
                    else if( k == SDLK_F8 || k == SDLK_RIGHTBRACKET ) scene_room_step( 1 );
                    else workbench_key( k, (SDL_Keymod)ev.key.keysym.mod );
                    continue;
                }
                if( k == SDLK_F9 || k == SDLK_TAB ) {
                    gWorkbenchOpen = 1;
                    gWorkbenchRow = 0;
                    gLatchA = gLatchB = gLatchZ = gLatchR = gLatchItem = 0;
                    continue;
                }
                if( k == SDLK_F12 ) {
                    gWorkbenchOpen = 1;
                    gWorkbenchTab = PG_TAB_HELP;
                    gWorkbenchRow = 0;
                    gLatchA = gLatchB = gLatchZ = gLatchR = gLatchItem = 0;
                    continue;
                }
                if( ev.key.keysym.scancode == SDL_SCANCODE_SPACE ) gLatchA = 1;
                if( ev.key.keysym.scancode == SDL_SCANCODE_J ) gLatchB = 1;
                if( ev.key.keysym.scancode == SDL_SCANCODE_K ) gLatchZ = 1;
                if( ev.key.keysym.scancode == SDL_SCANCODE_L ) gLatchR = 1;
                if( ev.key.keysym.scancode == SDL_SCANCODE_I ) gLatchItem = 1;
                switch( k ) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_F10: gHudVisible = !gHudVisible; break;
                case SDLK_F11: gDiagnosticsVisible = !gDiagnosticsVisible; break;
                case SDLK_p:
                    gManualPaused = !gManualPaused;
                    if( gManualPaused ) gLatchA = gLatchB = gLatchZ = gLatchR = gLatchItem = 0;
                    pg_notify( "SIMULATION %s", gManualPaused?"PAUSED":"RUNNING" );
                    break;
                case SDLK_n: gSingleStep = 1; gManualPaused = 1; break;
                case SDLK_c:
                    gCameraMode = ( gCameraMode + 1 ) % PG_CAM_COUNT;
                    pg_notify( "%s CAMERA", kCameraNames[gCameraMode] );
                    break;
                case SDLK_1: gSword = OOT_SWORD_NONE;     needEquip = 1; break;
                case SDLK_2: gSword = OOT_SWORD_KOKIRI;   needEquip = 1; break;
                case SDLK_3: gSword = OOT_SWORD_MASTER;   needEquip = 1; break;
                case SDLK_4: gSword = OOT_SWORD_BIGGORON; needEquip = 1; break;
                case SDLK_5:
                    if( ev.key.keysym.mod & KMOD_SHIFT ) { gShield = OOT_SHIELD_DEKU; needEquip = 1; }
                    else item_toggle( OOT_ITEM_BOW, "bow" );
                    break;
                case SDLK_6:
                    if( ev.key.keysym.mod & KMOD_SHIFT ) { gShield = OOT_SHIELD_HYLIAN; needEquip = 1; }
                    else item_toggle( OOT_ITEM_BOMB, "bomb" );
                    break;
                case SDLK_7:
                    if( ev.key.keysym.mod & KMOD_SHIFT ) { gShield = OOT_SHIELD_MIRROR; needEquip = 1; }
                    else item_toggle( OOT_ITEM_HOOKSHOT, "hookshot" );
                    break;
                case SDLK_8:
                    if( ev.key.keysym.mod & KMOD_SHIFT ) { gTunic = OOT_TUNIC_KOKIRI; needEquip = 1; }
                    else item_toggle( OOT_ITEM_BOOMERANG, "boomerang" );
                    break;
                case SDLK_9: gTunic = OOT_TUNIC_GORON;   needEquip = 1; break;
                case SDLK_0: gTunic = OOT_TUNIC_ZORA;    needEquip = 1; break;
                case SDLK_F1: gBoots = OOT_BOOTS_KOKIRI;  needEquip = 1; break;
                case SDLK_F2: gBoots = OOT_BOOTS_IRON;    needEquip = 1; break;
                case SDLK_F3: gBoots = OOT_BOOTS_HOVER;   needEquip = 1; break;
                case SDLK_t: set_link_age((uint8_t)!gSt.age ); break;
                case SDLK_e:
                    gOcarinaOut = !gOcarinaOut;
                    gItemOut = gOcarinaOut ? OOT_ITEM_OCARINA : OOT_ITEM_NONE;
                    oot_link_use_item( gLink, gOcarinaOut ? OOT_ITEM_OCARINA : OOT_ITEM_NONE );
                    puts( gOcarinaOut ? "[link] ocarina out" : "[link] ocarina away" );
                    break;
                case SDLK_h: oot_link_damage( gLink, 8 ); puts( "[link] ouch (self)" ); break;
                case SDLK_m: oot_link_set_magic( gLink, 2, 0x60 ); puts( "[link] magic filled" ); break;
                case SDLK_r:
                    if( gSceneMode ? scene_enter( gSceneSel ) : link_spawn())
                        puts( "[link] respawned" );
                    break;
                case SDLK_F5:
                    if( gSceneMode ) scene_leave();
                    else scene_enter( 0 );
                    break;
                case SDLK_F6:
                    if( !gSceneMode ) scene_enter( 0 );
                    else scene_enter( ( gSceneSel + 1 ) % N_SCENES );
                    break;
                case SDLK_F7:
                case SDLK_LEFTBRACKET: scene_room_step( -1 ); break;
                case SDLK_F8:
                case SDLK_RIGHTBRACKET: scene_room_step( 1 ); break;
                case SDLK_v: gShowSkeleton = !gShowSkeleton;
                    puts( gShowSkeleton ? "[view] skeleton" : "[view] model" ); break;
                case SDLK_b: gCullMode = ( gCullMode + 1 ) % 3;
                    pg_notify( "BACKFACE CULL %s", kCullNames[gCullMode] ); break;
                default: break;
                }
                if( needEquip ) {
                    apply_loadout();
                    printf( "[link] equip: sword=%d shield=%d tunic=%d boots=%d\n",
                            gSword, gShield, gTunic, gBoots );
                }
            }
        }

        const Uint8 *keys = SDL_GetKeyboardState( NULL );
        uint8_t heldA = 0, heldB = 0, heldZ = 0, heldR = 0, heldItem = 0;
        static char ocHeld;
        if( gWorkbenchOpen ) {
            memset( &gIn, 0, sizeof( gIn ));
            if( ocHeld ) { ocHeld = 0; ocarina_release(); }
        } else if( gOcarinaOut ) {
            /* play like the game: J = A (D4), arrows = C buttons */
            gIn.stickX = gIn.stickY = 0;
            gIn.buttonA = gIn.buttonB = gIn.buttonZ = gIn.buttonR = 0;
            gIn.buttonItem = 0;
            char want = 0; float freq = 0;
            if( keys[SDL_SCANCODE_J] )          { want = 'A'; freq = 293.66f; }
            else if( keys[SDL_SCANCODE_DOWN] )  { want = 'D'; freq = 349.23f; }
            else if( keys[SDL_SCANCODE_RIGHT] ) { want = 'R'; freq = 440.00f; }
            else if( keys[SDL_SCANCODE_LEFT] )  { want = 'L'; freq = 493.88f; }
            else if( keys[SDL_SCANCODE_UP] )    { want = 'U'; freq = 587.33f; }
            if( want != ocHeld ) {
                ocHeld = want;
                if( want ) ocarina_note( want, freq );
                else ocarina_release();
            }
        } else {
            if( ocHeld ) { ocHeld = 0; ocarina_release(); }
            gIn.stickX = playground_stick_x( keys[SDL_SCANCODE_A],
                                             keys[SDL_SCANCODE_D] );
            gIn.stickY = ( keys[SDL_SCANCODE_W] ? 1.0f : 0.0f ) + ( keys[SDL_SCANCODE_S] ? -1.0f : 0.0f );
            heldA = keys[SDL_SCANCODE_SPACE] != 0;
            heldB = keys[SDL_SCANCODE_J] != 0;
            heldZ = keys[SDL_SCANCODE_K] != 0;
            heldR = keys[SDL_SCANCODE_L] != 0;
            heldItem = keys[SDL_SCANCODE_I] != 0;
            if( controller && SDL_GameControllerGetAttached( controller )) {
                const float invAxis = 1.0f / 32767.0f;
                float lx = SDL_GameControllerGetAxis( controller, SDL_CONTROLLER_AXIS_LEFTX ) * invAxis;
                float ly = SDL_GameControllerGetAxis( controller, SDL_CONTROLLER_AXIS_LEFTY ) * invAxis;
                float rx = SDL_GameControllerGetAxis( controller, SDL_CONTROLLER_AXIS_RIGHTX ) * invAxis;
                float ry = SDL_GameControllerGetAxis( controller, SDL_CONTROLLER_AXIS_RIGHTY ) * invAxis;
                if( fabsf( lx ) > .18f ) gIn.stickX = lx;
                if( fabsf( ly ) > .18f ) gIn.stickY = -ly;
                if( fabsf( rx ) > .18f || fabsf( ry ) > .18f ) {
                    gCameraYaw -= rx * .055f;
                    gCameraPitch -= ry * .04f;
                    gCameraMode = PG_CAM_ORBIT;
                }
                heldA |= SDL_GameControllerGetButton( controller, SDL_CONTROLLER_BUTTON_A );
                heldB |= SDL_GameControllerGetButton( controller, SDL_CONTROLLER_BUTTON_X );
                heldZ |= SDL_GameControllerGetButton( controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER ) ||
                         SDL_GameControllerGetAxis( controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT ) > 8000;
                heldR |= SDL_GameControllerGetButton( controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER );
                heldItem |= SDL_GameControllerGetButton( controller, SDL_CONTROLLER_BUTTON_Y );
            }
            gIn.buttonA = heldA || gLatchA;
            gIn.buttonB = heldB || gLatchB;
            gIn.buttonZ = heldZ || gLatchZ;
            gIn.buttonR = heldR || gLatchR;
            gIn.buttonItem = heldItem || gLatchItem;
        }

        if( gWorkbenchOpen && gSnapshotRefresh ) {
            int savedAi = gEnemyAi;
            gEnemyAi = 0;
            sim_tick();                 /* refresh state/geometry after menu mutations */
            gEnemyAi = savedAi;
            gSnapshotRefresh = 0;
        }
        if( gFatalError ) break;
        uint32_t now = SDL_GetTicks();
        gWallClockMs = now;
        uint32_t elapsed = now - lastFrame;
        lastFrame = now;
        if( elapsed > 250 ) elapsed = 250;
        float instantMs = elapsed ? (float)elapsed : 0.1f;
        if( gFrameMs <= 0 ) gFrameMs = instantMs;
        else gFrameMs += ( instantMs - gFrameMs ) * .08f;
        gRenderFps = gFrameMs > 0 ? 1000.0f / gFrameMs : 0;

        int catchupSteps = 0;
        int consumedLatches = 0;
        if( !gManualPaused && !gWorkbenchOpen ) simAccumulator += elapsed * gTimeScale;
        else simAccumulator = 0;
        if( gSingleStep ) {
            sim_tick();
            catchupSteps++;
            gSingleStep = 0;
            consumedLatches = 1;
            gLatchA = gLatchB = gLatchZ = gLatchR = gLatchItem = 0;
            gIn.buttonA=heldA; gIn.buttonB=heldB; gIn.buttonZ=heldZ;
            gIn.buttonR=heldR; gIn.buttonItem=heldItem;
        }
        while( simAccumulator >= SIM_DT_MS && catchupSteps < 5 ) {
            sim_tick();
            simAccumulator -= SIM_DT_MS;
            catchupSteps++;
            if( !consumedLatches ) {
                consumedLatches = 1;
                gLatchA = gLatchB = gLatchZ = gLatchR = gLatchItem = 0;
                gIn.buttonA=heldA; gIn.buttonB=heldB; gIn.buttonZ=heldZ;
                gIn.buttonR=heldR; gIn.buttonItem=heldItem;
            }
        }
        if( simAccumulator >= SIM_DT_MS ) simAccumulator = 0;
        gCatchupLast = catchupSteps;
        if( gFatalError ) break;

        int w, hgt;
        SDL_GL_GetDrawableSize( win, &w, &hgt );
        if( uiFrames > 0 ) {
            long menuFrames = uiFrames > 4 ? uiFrames * 4 / 5 : uiFrames;
            if( renderedFrames < menuFrames ) {
                gWorkbenchOpen = 1;
                gWorkbenchTab = (int)( renderedFrames * PG_TAB_COUNT / ( menuFrames ? menuFrames : 1 ));
                if( gWorkbenchTab >= PG_TAB_COUNT ) gWorkbenchTab = PG_TAB_COUNT - 1;
            } else {
                gWorkbenchOpen = 0;      /* finish by exercising HUD + diagnostics */
                gDiagnosticsVisible = 1;
            }
        }
        draw_scene( w, hgt );
        if( uiFrames > 0 ) {
            GLenum error = glGetError();
            if( error != GL_NO_ERROR ) {
                fprintf( stderr, "[gfx] UI smoke frame %ld failed (GL error 0x%04x)\n",
                         renderedFrames, (unsigned)error );
                gFatalError = 1;
                break;
            }
        }
        SDL_GL_SwapWindow( win );
        if( uiFrames > 0 && ++renderedFrames >= uiFrames ) running = 0;
    }

    if( uiFrames > 0 && !gFatalError )
        printf( "[gfx] UI smoke PASS: %ld frames, all workbench tabs + diagnostics\n",
                renderedFrames );
    exitCode = gFatalError ? 1 : 0;

cleanup:
    /* PCM pointers in the SDL callback are owned by liboot. Stop that thread
       before releasing the library, and release GL objects while the context
       that created them is still current. */
    close_audio();
    if( controller ) {
        SDL_GameControllerClose( controller );
        controller = NULL;
    }
    if( ctx ) {
        if( win && SDL_GL_MakeCurrent( win, ctx ) == 0 ) {
            pg_ui_shutdown();
            release_gl_textures();
        } else {
            fprintf( stderr, "[gfx] context unavailable during cleanup: %s\n", SDL_GetError());
            forget_gl_textures();
        }
    } else {
        forget_gl_textures();
    }
    shutdown_liboot();
    if( ctx ) SDL_GL_DeleteContext( ctx );
    if( win ) SDL_DestroyWindow( win );
    SDL_Quit();
    return exitCode;
}
