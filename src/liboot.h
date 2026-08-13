/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#ifndef LIB_OOT_H
#define LIB_OOT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
    #if defined(OOT_LIB_STATIC)
        #define OOT_LIB_FN
    #elif defined(OOT_LIB_EXPORT)
        #define OOT_LIB_FN __declspec(dllexport)
    #else
        #define OOT_LIB_FN __declspec(dllimport)
    #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
    #ifdef OOT_LIB_EXPORT
        #define OOT_LIB_FN __attribute__ ((visibility("default")))
    #else
        #define OOT_LIB_FN
    #endif
#else
    #define OOT_LIB_FN
#endif

/* Compile-time ABI layout guard.  These fire in any translation unit that
   includes this header (including the CI public-header compile checks and
   every downstream binding build), turning a silent struct-reorder or an
   -fshort-enums miscompile into a hard build error instead of runtime
   corruption.  C++ provides static_assert; C11 provides _Static_assert;
   pre-C11 C compilers (e.g. MSVC in default C mode) silently skip the check,
   which is safe because those toolchains are not used to build the ABI. */
#if defined(__cplusplus)
    #define OOT_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define OOT_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
    #define OOT_STATIC_ASSERT(cond, msg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LIBOOT_VERSION_MAJOR 0
#define LIBOOT_VERSION_MINOR 8
#define LIBOOT_VERSION_PATCH 0
#define LIBOOT_VERSION_STRING "0.8.0"

/* liboot vNEXT: OoTSurface.type presets, mapped to real OoT SurfaceType data
   so the vendored collision code applies the right behavior per triangle.
   0 is the default (hookshot-attachable, like every prior release); unknown
   values fall back to the default, preserving the old "type ignored" contract. */
enum OoTSurfaceType
{
    OOT_SURFACE_DEFAULT = 0,   /* dirt-ish, hookshot-attachable (legacy behavior) */
    OOT_SURFACE_SAND,          /* sand footstep sfx */
    OOT_SURFACE_GRASS,         /* grass footstep sfx */
    OOT_SURFACE_STONE,         /* stone footstep sfx */
    OOT_SURFACE_DAMAGE,        /* damage floor (FLOOR_TYPE_2): hurts on contact */
    OOT_SURFACE_SLIPPERY,      /* slippery slope (FLOOR_TYPE_5): Link slides */
    OOT_SURFACE_CLIMB_WALL,    /* climbable wall (WALL_TYPE_2) */
    OOT_SURFACE_CONVEYOR,      /* moving floor (medium speed, +Z direction) */
    OOT_SURFACE_NO_HOOKSHOT,   /* solid but not hookshot-attachable */
    OOT_SURFACE_PRESET_COUNT
};

/* World-space collision triangle fed to the library (same convention as
   libsm64: integer vertices, y-up, OoT world units). */
struct OoTSurface
{
    uint16_t type;      /* enum OoTSurfaceType preset (0 = default) */
    int32_t vertices[3][3];
};

struct OoTLinkInputs
{
    float camLookX, camLookZ;   /* unit vector camera->link, xz plane */
    float stickX, stickY;       /* [-1, 1]; negative/positive = camera-left/right */
    uint8_t buttonA, buttonB, buttonZ, buttonR;
    /* liboot v0.6: the item button (the game's C-left). Hold to draw/aim the
       bow or hookshot, release to fire; press to (re)throw the boomerang or
       pull out another bomb. Items are placed on this button by
       oot_link_use_item. Appended field: zero-initialized callers keep the
       previous behavior. */
    uint8_t buttonItem;
    /* liboot vNEXT: C-up. Feeds BTN_CUP so the real Player code can enter its
       first-person / look-around and "talk to Navi" paths. Appended field:
       zero-initialized callers keep the previous behavior. */
    uint8_t buttonCUp;
};

/* Values mirror the game's own enums (item.h / save.h in the decomp). */
enum OoTAge    { OOT_AGE_ADULT = 0, OOT_AGE_CHILD = 1 };
enum OoTSword  { OOT_SWORD_NONE = 0, OOT_SWORD_KOKIRI, OOT_SWORD_MASTER, OOT_SWORD_BIGGORON };
enum OoTShield { OOT_SHIELD_NONE = 0, OOT_SHIELD_DEKU, OOT_SHIELD_HYLIAN, OOT_SHIELD_MIRROR };
enum OoTTunic  { OOT_TUNIC_KOKIRI = 0, OOT_TUNIC_GORON, OOT_TUNIC_ZORA };
enum OoTBoots  { OOT_BOOTS_KOKIRI = 0, OOT_BOOTS_IRON, OOT_BOOTS_HOVER };

/* Usable items for oot_link_use_item. Items obey the original age rules.
   Bow, bomb, hookshot, and boomerang use real helper actors; opt their meshes
   into the shared geometry output with oot_actor_set_render(true). */
enum OoTItem
{
    OOT_ITEM_NONE = 0,
    OOT_ITEM_OCARINA,        /* fairy ocarina / ocarina of time by age */
    OOT_ITEM_BOTTLE,         /* empty bottle swing */
    OOT_ITEM_HAMMER,         /* megaton hammer (adult) */
    OOT_ITEM_DEKU_STICK,     /* child */
    OOT_ITEM_BOOMERANG,      /* child */
    OOT_ITEM_BOW,            /* adult */
    OOT_ITEM_HOOKSHOT,       /* adult */
    OOT_ITEM_BOMB,           /* both ages */
};

/* liboot vNEXT: Link's current high-level action, mapped from the live
   Player action function. This is a curated set of the action functions the
   decomp gives meaningful names; every other (numerically named) action
   reports OOT_ACTION_OTHER. Reported in OoTLinkState.action. */
enum OoTAction
{
    OOT_ACTION_OTHER = 0,       /* a real action liboot does not label yet */
    OOT_ACTION_IDLE,
    OOT_ACTION_TURN_IN_PLACE,
    OOT_ACTION_ROLL,
    OOT_ACTION_TALK,
    OOT_ACTION_SWING_BOTTLE,
    OOT_ACTION_EXCHANGE_ITEM,
    OOT_ACTION_HOOKSHOT_FLY,
    OOT_ACTION_SLIDE_ON_SLOPE,
    OOT_ACTION_TRY_OPENING_DOOR,
    OOT_ACTION_EXIT_GROTTO,
    OOT_ACTION_CS_ACTION,       /* driven by a cutscene / SetCsAction */
    OOT_ACTION_WAIT_FOR_CUTSCENE,
    OOT_ACTION_WAIT_FOR_PUT_AWAY,
    OOT_ACTION_BLUE_WARP_ARRIVE,
    OOT_ACTION_FARORES_WIND_ARRIVE,
    OOT_ACTION_START_WARP_SONG_ARRIVE,
    OOT_ACTION_TIME_TRAVEL_END,
};

struct OoTLinkState
{
    float position[3];
    float velocity[3];
    int16_t faceAngle;          /* binary angle, y axis */
    float linearVelocity;
    int16_t health;             /* heart quarters (16 = one heart) */
    int16_t healthCapacity;
    int16_t magic;              /* 0..0x30 (level 1), 0..0x60 (level 2) */
    uint8_t magicLevel;         /* 0 = no meter, 1, 2 */
    uint8_t age;                /* enum OoTAge */
    uint8_t isDead;
    int8_t  heldItemAction;     /* PLAYER_IA_* currently in hand, -1 none */
    uint8_t meleeWeaponState;   /* nonzero while a sword/hammer swing is active */
    uint32_t action;            /* enum OoTAction: Link's current high-level action */
    int16_t animId;             /* stable 1-based link_animetion entry; 0 unknown */
    float animFrame;
    uint32_t stateFlags1;
    uint32_t stateFlags2;
    /* liboot v0.3: Z-targeting (real Attention system) */
    uint8_t lockOnActive;       /* nonzero while an attention target is locked on */
    float lockOnPos[3];         /* locked target's focus point (reticle anchor) */
    /* liboot v0.5: water. inWater = Link's position is inside a water box and
       below its surface (the game's BGCHECKFLAG_WATER); waterSurfaceY is that
       box's surface height, only meaningful while inWater. Actual swimming is
       reported by stateFlags1 bit 27 (0x08000000). */
    uint8_t inWater;
    float waterSurfaceY;
    /* liboot vNEXT: appended state (all fields above are unchanged). */
    uint8_t attackAnim;         /* PLAYER_MWA_* swing id; only meaningful while meleeWeaponState != 0 */
    uint8_t stateFlags3;        /* low-level Player state bits, bank 3 (diagnostics) */
    int16_t lookPitch;          /* head/aim pitch, binary angle (focus.rot.x) */
    int16_t lookYaw;            /* head/aim yaw, binary angle (focus.rot.y) */
    uint16_t floorSfxOffset;    /* floor-material sound group under Link (0 = default ground) */
    /* liboot vNEXT: underwater/air timer. Counts up 0..300 while Link is
       submerged (the real Player field). liboot never forces drowning, so a
       host drives its own air meter: full above 0, out of air at 300. */
    uint16_t underwaterTimer;
};

/* Filled per-tick by the display-list interpreter (F3DZEX2 -> triangles).
   oot_link_tick retains the historical OOT_GEO_MAX_TRIANGLES allocation;
   oot_link_tick_sized honors the appended triangleCapacity. Each triangle has
   3 vertices, 3 floats per position/normal/color vertex and 2 floats per UV. */

/* liboot vNEXT: per-triangle render flags (one uint8_t per triangle, parallel
   to triTexture), decoded from the display list's geometry/other mode so a host
   can render each triangle the way the game does instead of guessing one global
   policy. Front faces follow the emitted vertex winding (the ROM's DL order).
   CULL_BOTH (both bits) means the triangle is degenerate/never drawn. */
enum OoTTriangleFlags
{
    OOT_TRI_CULL_FRONT = 1u << 0,   /* G_CULL_FRONT: cull front-facing */
    OOT_TRI_CULL_BACK  = 1u << 1,   /* G_CULL_BACK:  cull back-facing  */
    OOT_TRI_ALPHA_TEST = 1u << 2,   /* alpha-compare cutout: discard low-alpha texels */
    OOT_TRI_DECAL      = 1u << 3    /* z-mode decal: depth-bias to avoid z-fighting */
};

/* A contiguous geometry range sharing one source entity and one render state.
   `materialKey` is stable for identical interpreter state within a process;
   `renderMode` preserves the N64 other-mode-low bits for hosts that need more
   detail than the normalized blend/depth fields. Actor sourceInstance values
   are opaque lifetime tokens and must never be dereferenced. */
enum OoTGeometrySourceKind
{
    OOT_GEOMETRY_SOURCE_LINK = 0,
    OOT_GEOMETRY_SOURCE_NAVI = 1,
    OOT_GEOMETRY_SOURCE_ACTOR = 2,
    OOT_GEOMETRY_SOURCE_SCENE_ROOM = 3
};

enum OoTGeometryRenderPass
{
    OOT_GEOMETRY_PASS_OPAQUE = 0,
    OOT_GEOMETRY_PASS_TRANSLUCENT = 1
};

enum OoTGeometryBlendMode
{
    OOT_GEOMETRY_BLEND_OPAQUE = 0,
    OOT_GEOMETRY_BLEND_ALPHA = 1,
    OOT_GEOMETRY_BLEND_CUSTOM = 2
};

enum OoTGeometryDepthMode
{
    OOT_GEOMETRY_DEPTH_OPAQUE = 0,
    OOT_GEOMETRY_DEPTH_INTERPENETRATING = 1,
    OOT_GEOMETRY_DEPTH_TRANSLUCENT = 2,
    OOT_GEOMETRY_DEPTH_DECAL = 3
};

enum OoTGeometryDepthFlags
{
    OOT_GEOMETRY_DEPTH_TEST = 1u << 0,
    OOT_GEOMETRY_DEPTH_WRITE = 1u << 1
};

struct OoTGeometryBatch
{
    uint32_t firstTriangle;
    uint32_t numTriangles;
    uint64_t sourceInstance;
    uint32_t materialKey;
    uint32_t renderMode;
    uint16_t textureIndex;       /* 0xFFFF = untextured */
    int16_t sourceId;            /* actor id or room index; 0 for Link/Navi */
    uint8_t sourceKind;          /* enum OoTGeometrySourceKind */
    uint8_t renderPass;          /* enum OoTGeometryRenderPass */
    uint8_t blendMode;           /* enum OoTGeometryBlendMode */
    uint8_t depthMode;           /* enum OoTGeometryDepthMode */
    uint8_t triangleFlags;       /* enum OoTTriangleFlags */
    uint8_t depthFlags;          /* enum OoTGeometryDepthFlags */
    uint8_t reserved[2];
    uint32_t combineModeHi;      /* exact G_SETCOMBINE command word 0 */
    uint32_t combineModeLo;      /* exact G_SETCOMBINE command word 1 */
    float primitiveColor[4];
    float environmentColor[4];
    float baseColor[3];           /* Link limb/material base tint */
    float reservedColor;
    uint32_t reservedTail;
};

struct OoTLinkGeometryBuffers
{
    float *position;
    float *normal;
    float *color;
    float *uv;                  /* normalized texture coords */
    uint16_t *triTexture;       /* per-triangle texture index, 0xFFFF = untextured (may be NULL) */
    uint16_t numTrianglesUsed;
    /* liboot vNEXT: optional 1 float/vertex shade alpha, parallel to color
       (which stays 3 floats RGB). Appended last so existing positional
       initializers leave it NULL; the interpreter writes it only when non-NULL. */
    float *alpha;
    /* liboot vNEXT: optional 1 byte/triangle render flags (enum OoTTriangleFlags),
       parallel to triTexture. Appended last so existing positional initializers
       leave it NULL; the interpreter writes it only when non-NULL. */
    uint8_t *triFlags;
    /* Appended capacity/count fields, enabled by oot_link_tick_sized. A zero
       triangleCapacity retains the historical OOT_GEO_MAX_TRIANGLES default.
       numTrianglesUsed32 mirrors
       numTrianglesUsed and is authoritative above UINT16_MAX. Batch storage
       is optional; at most one batch is needed per emitted triangle. */
    uint32_t triangleCapacity;
    uint32_t numTrianglesUsed32;
    struct OoTGeometryBatch *batches;
    uint32_t batchCapacity;
    uint32_t numBatchesUsed;
};

/* Textures decoded at runtime from the caller's ROM (RGBA32). The count is an
   index upper bound; scene changes can invalidate or replace scene-texture
   slots. Query each referenced index and re-upload when revision changes.
   wrap: 0 = repeat, 1 = mirror, 2 = clamp. */
struct OoTTextureInfo
{
    uint16_t width, height;
    uint8_t wrapS, wrapT;
    uint32_t revision;          /* bump = pixels changed, re-upload */
};
extern OOT_LIB_FN int32_t oot_get_texture_count( void );
extern OOT_LIB_FN bool oot_get_texture( int32_t index, struct OoTTextureInfo *info,
                                        const uint8_t **rgbaPixels );

/* Sound-effect requests made by the real Player code (jump, attack, hurt,
   footsteps...). ids are the game's NA_SE_* values. */
typedef void (*OoTSfxCallback)( uint16_t sfxId );
extern OOT_LIB_FN void oot_set_sfx_callback( OoTSfxCallback cb );

/* Full parameters supplied by the original audio call sites. position is in
   the game's projected/world coordinate space; freqScale and volume are
   multipliers. The legacy callback above remains supported. */
struct OoTSfxEvent
{
    uint16_t sfxId;
    uint8_t token;
    int8_t reverb;
    uint8_t action;              /* OOT_SFX_* below */
    uint8_t isRefresh;           /* SFX_FLAG was removed: update a continuing sound */
    uint8_t reserved[2];
    float position[3];
    float freqScale;
    float volume;
};
enum {
    OOT_SFX_PLAY,
    OOT_SFX_STOP_ID,
    OOT_SFX_STOP_POSITION,
};
typedef void (*OoTSfxCallbackEx)( const struct OoTSfxEvent *event );
extern OOT_LIB_FN void oot_set_sfx_callback_ex( OoTSfxCallbackEx cb );

/* Mapped gameplay SFX/voice lookup (extracted + decoded from the ROM at init
   when possible): returns mono PCM16 for supported footsteps, weapons,
   shields, damage/roll effects, and character voices. False when this sfxId
   has no extracted sample. */
extern OOT_LIB_FN bool oot_get_voice_sample( uint16_t sfxId, const int16_t **pcm,
                                             uint32_t *numSamples, uint32_t *sampleRate );

/* liboot v0.5: the five ocarina notes as the game plays them (font-0
   instrument 52, the default ocarina), decoded from the caller's ROM.
   noteIndex 0..4 = the A, C-down, C-right, C-left, C-up buttons = D4, F4,
   A4, B4, D5. All five notes share ONE decoded mono PCM16 clip — the game
   itself plays a single C4-authored sample at five rates — so only
   *sampleRate differs per note (A4/B4/D5 come out above 32 kHz; the host
   mixer must accept arbitrary source rates or resample). Held notes: play
   [0, numSamples) once, then repeat [loopStart, numSamples) while the key is
   held (numSamples is the loop end); on release apply a short ~50 ms fade.
   False if the ROM's audio tables were not extracted or noteIndex > 4. */
extern OOT_LIB_FN bool oot_get_ocarina_note( uint8_t noteIndex, const int16_t **pcm,
                                             uint32_t *numSamples, uint32_t *sampleRate,
                                             uint32_t *loopStart );

/* liboot vNEXT: native Zelda AudioSeq playback.  The four players mirror the
   retail engine: the main BGM, fanfare, general-SFX, and secondary BGM players.
   Sequence ids 0x00..0x6D come directly from the ROM's Audioseq table;
   0x7F is the scene-header NO_MUSIC sentinel and is therefore not a catalog
   entry.  All state is owned by liboot and reset by oot_global_init/terminate. */
#define OOT_AUDIO_SEQUENCE_COUNT 110
#define OOT_AUDIO_NO_MUSIC       0x7Fu
#define OOT_AUDIO_NATURE_RAIN    0x80u
#define OOT_AUDIO_NATURE_COUNT   19u
#define OOT_AUDIO_NATURE_NONE    0x13u

enum OoTAudioPlayer
{
    OOT_AUDIO_PLAYER_MAIN = 0,
    OOT_AUDIO_PLAYER_FANFARE,
    OOT_AUDIO_PLAYER_SFX,
    OOT_AUDIO_PLAYER_SUB,
    OOT_AUDIO_PLAYER_COUNT
};

#define OOT_SEQUENCE_INFO_VERSION 1u
struct OoTSequenceInfo
{
    uint32_t structSize;       /* caller sets sizeof(struct OoTSequenceInfo) */
    uint32_t version;          /* caller sets OOT_SEQUENCE_INFO_VERSION */
    uint16_t sequenceId;
    uint16_t resolvedId;       /* target id after resolving a table alias */
    uint32_t dataSize;
    uint8_t fontCount;
    uint8_t isAlias;
    uint8_t medium;
    uint8_t cachePolicy;
    uint8_t fontIds[8];        /* sequence font order, unused slots are 0xFF */
    uint32_t reserved;
};

#define OOT_AUDIO_STATE_VERSION 1u
struct OoTAudioState
{
    uint32_t structSize;       /* caller sets sizeof(struct OoTAudioState) */
    uint32_t version;          /* caller sets OOT_AUDIO_STATE_VERSION */
    uint16_t sequenceId;
    uint16_t resolvedId;
    uint8_t player;
    uint8_t playing;
    uint8_t paused;
    uint8_t finished;
    uint8_t activeChannels;
    uint8_t activeVoices;
    uint8_t reserved8[2];
    float volume;
    uint64_t framesRendered;
};

/* Catalog metadata is copied into the caller's fixed-size record, including
   its stable symbolic NA_SE_* name.  This avoids exposing ROM/decode storage
   or allocator ownership across the C ABI. */
#define OOT_SFX_INFO_VERSION 1u
struct OoTSfxInfo
{
    uint32_t structSize;       /* caller sets sizeof(struct OoTSfxInfo) */
    uint32_t version;          /* caller sets OOT_SFX_INFO_VERSION */
    uint16_t sfxId;
    uint8_t bank;              /* 0 player, 1 item, 2 env, 3 enemy, ... */
    uint8_t reserved8;
    uint16_t bankIndex;
    uint16_t reserved16;
    char name[48];
};

extern OOT_LIB_FN int32_t oot_audio_sequence_count( void );
extern OOT_LIB_FN const char *oot_audio_sequence_name( uint16_t sequenceId );
extern OOT_LIB_FN bool oot_audio_sequence_get_info( uint16_t sequenceId,
                                                     struct OoTSequenceInfo *outInfo );
/* Decode/cache every sample reachable by this sequence. This may allocate and
   should run before starting a real-time audio device, never in its callback. */
extern OOT_LIB_FN bool oot_audio_sequence_prewarm( uint16_t sequenceId );
extern OOT_LIB_FN bool oot_audio_sequence_play( uint8_t player, uint16_t sequenceId,
                                                uint16_t fadeInMs );
/* Start AudioSeq 1 with one of OoT's 19 retail nature presets.  Unlike a raw
   sequence play this also applies the preset's player/channel IO program
   (stream type, critters, pitch, layer count and reverb). */
extern OOT_LIB_FN bool oot_audio_nature_play( uint8_t player, uint8_t ambienceId,
                                              uint16_t fadeInMs );
extern OOT_LIB_FN void oot_audio_sequence_stop( uint8_t player, uint16_t fadeOutMs );
extern OOT_LIB_FN void oot_audio_sequence_pause( uint8_t player, bool paused );
extern OOT_LIB_FN void oot_audio_sequence_set_volume( uint8_t player, float volume );
extern OOT_LIB_FN void oot_audio_sequence_set_io( uint8_t player, uint8_t port,
                                                  int8_t value );
extern OOT_LIB_FN void oot_audio_channel_set_io( uint8_t player, uint8_t channel,
                                                 uint8_t port, int8_t value );
extern OOT_LIB_FN bool oot_audio_sequence_get_state( uint8_t player,
                                                     struct OoTAudioState *outState );
extern OOT_LIB_FN void oot_audio_set_master_volume( float volume );
extern OOT_LIB_FN void oot_audio_stop_all( uint16_t fadeOutMs );

/* Fill interleaved stereo signed 16-bit PCM at any host rate from 8 to
   192 kHz. This is the canonical fixed-point render path; the buffer is
   overwritten, not accumulated. Rendering performs no heap allocation.
   This portable mixer uses N64-style resampling and command arithmetic but is
   not a claim of bit-exact RSP emulation. Hosts must serialize every call that
   accesses mutable AudioSeq state (including oot_audio_sequence_get_state)
   against their audio callback (SDL_LockAudioDevice is sufficient). Returns
   frames written, or zero for invalid arguments. */
extern OOT_LIB_FN uint32_t oot_audio_render_s16( int16_t *stereo,
                                                 uint32_t frames,
                                                 uint32_t sampleRate );

/* Fill interleaved stereo F32 by converting the canonical S16 render above.
   Each sample is exactly s16 / 32768.0f; all state and serialization rules are
   the same as oot_audio_render_s16. */
extern OOT_LIB_FN uint32_t oot_audio_render_f32( float *stereo, uint32_t frames,
                                                 uint32_t sampleRate );

extern OOT_LIB_FN int32_t oot_audio_sfx_catalog_count( void );
extern OOT_LIB_FN bool oot_audio_sfx_catalog_get( int32_t catalogIndex,
                                                  struct OoTSfxInfo *outInfo );
extern OOT_LIB_FN bool oot_audio_sfx_play( uint16_t sfxId, float pan, float volume );
extern OOT_LIB_FN void oot_audio_sfx_stop( uint16_t sfxId );
extern OOT_LIB_FN void oot_audio_sfx_stop_all( void );

/* liboot vNEXT: proximity-driven enemy/battle BGM. OoT's own Player/actor code
   decides when a hostile enemy is near (its nearest hostile actor within a
   500-unit battle range). When this feature is enabled, liboot plays the OoT
   battle sequence on a dedicated SEQ player while such an enemy is in range,
   scales its volume with proximity (loudest at contact), and fades it out when
   none remain. It is DISABLED by default and layers over, rather than replaces,
   any scene BGM the host is playing on another player.

   `player` selects the SEQ player (0xFF keeps the default, OOT_AUDIO_PLAYER_SUB).
   `seqId` is the battle sequence (0 keeps the default, NA_BGM_ENEMY / 0x1A).
   `fadeMs` is the fade in/out. Returns false for an invalid player or seqId.
   Because the driver runs inside oot_link_tick and calls the sequence player,
   the host's usual AudioSeq serialization (tick vs oot_audio_render_f32) applies. */
extern OOT_LIB_FN bool oot_audio_set_enemy_bgm( bool enabled, uint8_t player,
                                                uint16_t seqId, uint16_t fadeMs );

/* liboot vNEXT: read the live enemy-BGM state. `outActive` receives whether the
   battle sequence is currently playing; `outDistance` receives the distance
   (world units) to the nearest battle-range enemy on the most recent tick, or
   the 500-unit range when none. Any out pointer may be NULL. Returns whether
   the feature is enabled. */
extern OOT_LIB_FN bool oot_audio_get_enemy_bgm( bool *outActive, float *outDistance );

/* liboot vNEXT: the twelve ocarina songs, in the game's own OcarinaSongId
   order. Each song is a sequence of note indices 0..4 (the same A/C-down/
   C-right/C-left/C-up indices oot_get_ocarina_note uses). The first six are
   the warp/teleport songs. */
enum OoTOcarinaSong
{
    OOT_SONG_MINUET = 0,   /* warp: Sacred Forest Meadow */
    OOT_SONG_BOLERO,       /* warp: Death Mountain Crater */
    OOT_SONG_SERENADE,     /* warp: Lake Hylia */
    OOT_SONG_REQUIEM,      /* warp: Desert Colossus */
    OOT_SONG_NOCTURNE,     /* warp: Graveyard */
    OOT_SONG_PRELUDE,      /* warp: Temple of Time */
    OOT_SONG_SARIAS,
    OOT_SONG_EPONAS,
    OOT_SONG_LULLABY,
    OOT_SONG_SUNS,
    OOT_SONG_TIME,
    OOT_SONG_STORMS,
    OOT_SONG_COUNT
};

/* Fill outNotes (capacity >= 8) with a song's note-index sequence and its
   length. Returns false for an out-of-range song. Lets a host render/teach a
   song or drive playback of oot_get_ocarina_note per note. */
extern OOT_LIB_FN bool oot_ocarina_song_notes( int32_t song, uint8_t outNotes[8],
                                               int32_t *outCount );

/* Recognize a played sequence: returns the enum OoTOcarinaSong whose pattern
   matches the tail of notes[0..count) (so a host feeds notes as Link plays and
   detects a completed song, warp songs included), or -1 if none match. Each
   note is a 0..4 index; other values never match. */
extern OOT_LIB_FN int32_t oot_ocarina_match( const uint8_t *notes, int32_t count );

#define OOT_GEO_MAX_TRIANGLES 2048
#define OOT_GEO_MAX_CONFIGURABLE_TRIANGLES 1048576u
/* Maximum number of live decoded texture slots. Scene reloads can recycle
   slots; revision numbers tell a host when an index's pixels changed. */
#define OOT_TEXTURE_MAX_COUNT 1024u

/* Legacy atlas dimensions retained for source compatibility. The atlas output
   was never implemented; query individual textures with oot_get_texture. */
#define OOT_TEXTURE_WIDTH  1024
#define OOT_TEXTURE_HEIGHT 1024

typedef void (*OoTDebugPrintFunctionPtr)( const char * );

/* rom: entire OoT ROM contents, romSize bytes (version fixed by build, see
   README). Buffers smaller than 0x1060 bytes or larger than 256 MiB are
   rejected. outTexture is a deprecated, ignored compatibility parameter;
   pass NULL. */
extern OOT_LIB_FN void oot_global_init( const uint8_t *rom, size_t romSize, uint8_t *outTexture );
extern OOT_LIB_FN void oot_global_terminate( void );

extern OOT_LIB_FN void oot_set_debug_print_function( OoTDebugPrintFunctionPtr fn );

extern OOT_LIB_FN void oot_static_surfaces_load( const struct OoTSurface *surfaces, uint32_t numSurfaces );

/* liboot v0.5: axis-aligned water volume for the static world (the game's own
   WaterBox shape). An XZ rectangle with a surface height; the water extends
   infinitely downward — any position below ySurface inside the rectangle is
   in water. Membership is strict: xMin < x < xMin + xLength and
   zMin < z < zMin + zLength. Same integer world units as OoTSurface. */
struct OoTWaterBox
{
    int16_t xMin, zMin;
    int16_t xLength, zLength;
    int16_t ySurface;
};

/* Load static collision plus water. Valid input replaces the previous world:
   invalid, degenerate, unrepresentable, or over-capacity input is ignored and
   leaves the previous world live. Both surfaces and water boxes are replaced
   together (oot_static_surfaces_load(s,n) is exactly
   oot_static_world_load(s,n,NULL,0), so legacy callers keep a dry world).
   Entering deep water hands control to the real Player swim code: surface
   swim + strokes, A to dive (depth capped by the game's scale upgrade), iron
   boots sink to the bottom and walk (re-equip lighter boots to float back
   up), leaving through rising ground or a climbable ledge exits the water.
   There is no air meter in liboot: Link never drowns. */
extern OOT_LIB_FN void oot_static_world_load( const struct OoTSurface *surfaces, uint32_t numSurfaces,
                                              const struct OoTWaterBox *waterBoxes, uint32_t numWaterBoxes );

/* Host-owned moving collision. Geometry is expressed in object-local integer
   coordinates and is transformed by OoTDynamicCollisionTransform. Handles are
   generation checked, survive static-world/scene replacement, and are rebound
   to the new native CollisionContext automatically. The retail dynapoly budget
   is shared by all live objects: at most 50 objects, 512 triangles, and 512
   unique local vertices. */
#define OOT_DYNAMIC_COLLISION_MAX_OBJECTS 50u
#define OOT_DYNAMIC_COLLISION_MAX_SURFACES 512u
#define OOT_DYNAMIC_COLLISION_MAX_VERTICES 512u
#define OOT_DYNAMIC_COLLISION_INVALID 0u

typedef uint32_t OoTDynamicCollision;

enum OoTDynamicCollisionFlags
{
    OOT_DYNAMIC_COLLISION_CARRY_POSITION = 1u << 0,
    OOT_DYNAMIC_COLLISION_CARRY_ROTATION_Y = 1u << 1
};

struct OoTDynamicCollisionTransform
{
    uint32_t structSize;
    float position[3];
    int16_t rotation[3];       /* binary angles, applied Y-X-Z like OoT */
    uint16_t reserved;
    float scale[3];
};

#define OOT_DYNAMIC_COLLISION_TRANSFORM_INIT \
    { sizeof(struct OoTDynamicCollisionTransform), { 0.0f, 0.0f, 0.0f }, \
      { 0, 0, 0 }, 0u, { 1.0f, 1.0f, 1.0f } }

struct OoTDynamicCollisionState
{
    uint32_t structSize;
    struct OoTDynamicCollisionTransform transform;
    int32_t nativeBgId;        /* diagnostic only; may change after world load */
    uint8_t enabled;
    uint8_t playerOnTop;
    uint8_t playerAbove;
    uint8_t actorOnTop;
};

/* Returns 0 on success; -1 invalid input, -2 no collision world, -3 retail
   dynapoly capacity exhausted, or -4 allocation failure. */
extern OOT_LIB_FN int32_t oot_dynamic_collision_create(
    const struct OoTSurface *surfaces, uint32_t numSurfaces,
    const struct OoTDynamicCollisionTransform *transform, uint32_t flags,
    OoTDynamicCollision *outHandle );
extern OOT_LIB_FN bool oot_dynamic_collision_set_transform(
    OoTDynamicCollision handle, const struct OoTDynamicCollisionTransform *transform );
extern OOT_LIB_FN bool oot_dynamic_collision_set_enabled(
    OoTDynamicCollision handle, bool enabled );
extern OOT_LIB_FN bool oot_dynamic_collision_get_state(
    OoTDynamicCollision handle, struct OoTDynamicCollisionState *outState );
extern OOT_LIB_FN bool oot_dynamic_collision_delete( OoTDynamicCollision handle );

/* A static world or ROM scene must be loaded before creating Link because the
   native Player initializer immediately queries the collision context. */
extern OOT_LIB_FN int32_t oot_link_create( float x, float y, float z );
extern OOT_LIB_FN void oot_link_tick( int32_t linkId,
                                      const struct OoTLinkInputs *inputs,
                                      struct OoTLinkState *outState,
                                      struct OoTLinkGeometryBuffers *outBuffers );
/* Capacity-aware form. geometryBuffersSize must be the caller's sizeof of
   OoTLinkGeometryBuffers; fields beyond that prefix are never read or written.
   The legacy oot_link_tick intentionally uses the original struct prefix and
   therefore retains the historical OOT_GEO_MAX_TRIANGLES capacity. */
extern OOT_LIB_FN void oot_link_tick_sized(
    int32_t linkId, const struct OoTLinkInputs *inputs,
    struct OoTLinkState *outState, struct OoTLinkGeometryBuffers *outBuffers,
    uint32_t geometryBuffersSize );
/* Number of otherwise valid triangles omitted by the most recent Link
   geometry walk because the caller's triangle capacity was reached. This includes
   optional Navi/actor meshes appended to the same output. */
extern OOT_LIB_FN uint32_t oot_link_get_geometry_dropped_triangles( void );
extern OOT_LIB_FN void oot_link_delete( int32_t linkId );

/* liboot vNEXT: move Link in place without the delete/recreate dance. Sets his
   world position and facing yaw (binary angle) directly, and snaps the
   previous-position/home anchors and clears velocity so the next tick does not
   interpolate a huge step or retain pre-warp momentum. Does NOT reset action
   state; combine with oot_link_freeze for a clean reposition. Returns false
   for a bad id or no live Link. */
extern OOT_LIB_FN bool oot_link_set_pose( int32_t linkId, float x, float y, float z, int16_t yaw );

/* liboot vNEXT: freeze/unfreeze Link's simulation. While frozen, oot_link_tick
   still reports state and regenerates geometry (so a paused Link renders) but
   does not advance the Player update, holding his current pose. */
extern OOT_LIB_FN void oot_link_freeze( int32_t linkId, bool frozen );

/* liboot vNEXT: set Link's invincibility timer (the game's own field). Positive
   = intangibility frames, negative = invulnerability frames, 0 = clear. The
   game decrements it, so re-apply each tick to hold a state indefinitely. */
extern OOT_LIB_FN void oot_link_set_invincible( int32_t linkId, int8_t frames );

/* liboot vNEXT: query the collision surface under a world point via a downward
   raycast against the live collision world (custom or ROM scene). Fills the
   ground height, the real floor-type and material ids from the surface's
   SurfaceType, and whether the surface is hookshot-attachable. Returns false
   when no floor is found below the point. Any out pointer may be NULL. */
struct OoTSurfaceInfo
{
    float groundY;          /* world Y of the floor hit */
    uint32_t floorType;     /* SurfaceType floor type (FLOOR_TYPE_*) */
    uint32_t material;      /* SurfaceType material/ground id (SURFACE_MATERIAL_*) */
    uint8_t hookshot;       /* nonzero if the surface is hookshot-attachable */
};
extern OOT_LIB_FN bool oot_scene_query_surface( float x, float y, float z,
                                                struct OoTSurfaceInfo *outInfo );

/* Switch adult/child. Re-initializes the Player in place (position kept,
   action state reset to idle). Returns false if the ROM lacks the object. */
extern OOT_LIB_FN bool oot_link_set_age( int32_t linkId, uint8_t age );

/* Equip gear. Invalid combinations for the current age (e.g. Master Sword as
   child) are clamped by the game's own rules. Sword lands on B: press B to
   swing with the real melee action set. */
extern OOT_LIB_FN void oot_link_set_equipment( int32_t linkId, uint8_t sword, uint8_t shield,
                                               uint8_t tunic, uint8_t boots );

/* Health in quarter-hearts (max 20 hearts = 0x140). Damage uses the real
   Player_InflictDamage: knockback, invincibility frames, death sequence. */
extern OOT_LIB_FN void oot_link_set_health( int32_t linkId, int16_t health, int16_t capacity );
extern OOT_LIB_FN void oot_link_damage( int32_t linkId, int16_t amount );
extern OOT_LIB_FN void oot_link_set_magic( int32_t linkId, uint8_t level, int16_t amount );

/* Put an item in Link's hands (see enum OoTItem caveats). OOT_ITEM_NONE puts
   the item away. */
extern OOT_LIB_FN void oot_link_use_item( int32_t linkId, uint8_t item );

/* liboot v0.3: Z-targeting attention targets. Each target is a real (fake-
   spawned) hostile actor the game's own Attention system can scan, hover and
   lock onto: press Z near one and the vendored Player code does the rest
   (Switch targeting, strafe/backflip move set, auto-face with stick neutral,
   leash release past ~429 units for the default range). (x,y,z) is the
   target's base position; radius is the height of the lock-on focus point
   above it (also keeps the line-of-sight ray off the floor — use ~30 for a
   ground-standing target). Lock state is reported per tick in OoTLinkState
   (lockOnActive / lockOnPos). Returns a target id, or -1 if the pool (16) is
   full. */
extern OOT_LIB_FN int32_t oot_target_create( float x, float y, float z, float radius );
extern OOT_LIB_FN void oot_target_move( int32_t targetId, float x, float y, float z );
/* Removing a locked target releases the lock through the game's own
   dead-actor paths on the next tick. */
extern OOT_LIB_FN void oot_target_remove( int32_t targetId );

/* Host-controlled actors -------------------------------------------------
 *
 * These actors participate in the native actor/attention lists while their
 * behavior, health and rendering stay under host control. They do not execute
 * retail MIPS actor overlays. State is pushed explicitly before a tick and can
 * be pulled back at any time. The raw integer id is process-local; the engine
 * wrapper below adds generation-checked handles.
 */
#define OOT_HOST_ACTOR_STATE_VERSION 1u
#define OOT_HOST_ACTOR_CONTACT_VERSION 1u
#define OOT_HOST_ACTOR_MAX 64u

enum OoTHostActorFlags
{
    OOT_HOST_ACTOR_ENABLED    = 1u << 0,
    OOT_HOST_ACTOR_TARGETABLE = 1u << 1,
    OOT_HOST_ACTOR_HOSTILE    = 1u << 2,
    OOT_HOST_ACTOR_HURT       = 1u << 3
};

enum OoTHostActorContactSource
{
    OOT_HOST_CONTACT_MELEE = 1,
    OOT_HOST_CONTACT_ARROW = 2,
    OOT_HOST_CONTACT_BOOMERANG = 3,
    OOT_HOST_CONTACT_HOOKSHOT = 4,
    OOT_HOST_CONTACT_BOMB = 5
};

struct OoTHostActorState
{
    uint32_t structSize;
    uint32_t version;
    uint64_t userTag;
    uint32_t typeId;              /* host-defined actor/archetype id */
    uint32_t flags;               /* enum OoTHostActorFlags */
    float position[3];
    float focusOffset[3];         /* lock-on/Navi focus relative to position */
    float hurtRadius;             /* world-space vertical-cylinder radius */
    float hurtHeight;
    float hurtYOffset;
    int16_t rotation[3];          /* OoT binary angles */
    int16_t room;                 /* -1 persists; otherwise room 0..127 */
    uint8_t attentionRange;       /* native AttentionRangeType, 0..9 */
    uint8_t reserved[3];
};

struct OoTHostActorContact
{
    uint32_t structSize;
    uint32_t version;
    int32_t actorId;              /* raw host-actor id */
    uint32_t source;              /* enum OoTHostActorContactSource */
    uint64_t userTag;
    uint32_t sourceActorId;       /* native OoT actor id, or ACTOR_PLAYER */
    uint32_t gameplayFrame;
    float position[3];
    uint32_t reserved;
};

extern OOT_LIB_FN int32_t oot_host_actor_create(
    const struct OoTHostActorState *state );
extern OOT_LIB_FN bool oot_host_actor_update(
    int32_t actorId, const struct OoTHostActorState *state );
extern OOT_LIB_FN bool oot_host_actor_get(
    int32_t actorId, struct OoTHostActorState *outState );
extern OOT_LIB_FN bool oot_host_actor_remove( int32_t actorId );
extern OOT_LIB_FN void oot_host_actor_clear( void );
/* Pops the oldest queued contact. Returns false when the queue is empty. */
extern OOT_LIB_FN bool oot_host_actor_poll_contact(
    struct OoTHostActorContact *outContact );

/* liboot v0.4: Navi. The real EnElf actor is auto-spawned by the vendored
   Player_Init (its Player_SpawnFairy path) and ticks its real update every
   frame: idle orbit over Link's head, dash toward the current attention /
   lock-on actor (Z-targeting), inner/outer color lerp by hovered-actor
   category — all driven by the game's own Attention system.

   outPos: world position. outInnerColor / outOuterColor: RGBA 0..1, the
   glow's core and fringe colors as computed by the real EnElf code.
   *outScale: actor scale (0.008 nominal; 0.0 while Navi is hidden, e.g.
   tucked under Link's hat). Any out pointer may be NULL. Returns false while
   no Navi exists (no Link yet, or the ROM's gameplay_keep assets were not
   found).

   Rendering: the in-game body "ball" is a camera-facing billboard; hosts
   should draw a soft sprite at outPos, radius ~1500 * scale world units,
   colored outInnerColor at the core fading to outOuterColor (pulse it
   slightly for the authentic look). The animated wing mesh, interpreted from
   the ROM's own display lists, can be appended to oot_link_tick's geometry
   buffers via oot_navi_set_render (default off: Link-only consumers keep
   identical buffers). */
extern OOT_LIB_FN bool oot_navi_get( float outPos[3], float outInnerColor[4],
                                     float outOuterColor[4], float *outScale );
extern OOT_LIB_FN void oot_navi_set_render( bool enabled );

/* Animated skeleton pose in world space, for callers that render Link
   themselves (bones as parent->joint segments). 21 limbs; parent 0xFF = root. */
#define OOT_SKELETON_MAX_JOINTS 21
struct OoTSkeletonPose
{
    uint8_t numJoints;
    uint8_t parent[OOT_SKELETON_MAX_JOINTS];
    float jointPos[OOT_SKELETON_MAX_JOINTS][3];
};
extern OOT_LIB_FN bool oot_link_get_skeleton( int32_t linkId, struct OoTSkeletonPose *out );

/* liboot v0.6: real projectiles. oot_link_use_item(BOW/BOMB/HOOKSHOT/
   BOOMERANG) now spawns the game's own EnArrow/EnBom/ArmsHook/EnBoom actors
   (vendored, updated by the real game loop) when driven with
   OoTLinkInputs.buttonItem; oot_actor_query snapshots every live non-Player
   actor so hosts can place them. `active` is 0 for an actor that died this
   tick (Actor_Kill'd, reaped next tick). Returns the number of entries
   written (<= maxCount). */
struct OoTActorInfo
{
    int16_t id;                 /* game actor id (ACTOR_EN_BOM=0x10, EN_ARROW=0x16,
                                   EN_ELF=0x18, EN_BOOM=0x32, ARMS_HOOK=0x66) */
    int16_t category;           /* ACTORCAT_* list the actor lives on */
    int16_t params;
    int16_t yaw;                /* shape.rot.y, binary angle */
    uint8_t active;             /* update != NULL */
    float pos[3];               /* world position */
};
extern OOT_LIB_FN int32_t oot_actor_query( struct OoTActorInfo *out, int32_t maxCount );

/* Opt-in: append the projectile actors' interpreted display lists (bomb
   body/cap, boomerang, arrow, hookshot tip+chain) to oot_link_tick's
   geometry buffers, exactly like oot_navi_set_render does for Navi's wings.
   Default off: Link-only consumers keep identical buffers. */
extern OOT_LIB_FN void oot_actor_set_render( bool enabled );

/* liboot vNEXT: place one of the vendored actors that is safe to spawn
   standalone. Currently only ACTOR_EN_BOM (a real bomb: it ticks, can be
   Link-carried, and explodes through the genuine EnBom code) is allowed; the
   other vendored actors are Player-coupled (arrows need a shooter, the hookshot
   hijacks Link, Navi is already auto-spawned) and are rejected without side
   effects. Requires a live Link, since the actor update path dereferences the
   Player. rot* are binary angles; params is the actor's spawn parameter word.
   Returns 0 on success, or negative on rejection (-1 no Link, -2 unsupported
   id, -3 spawn failed: arena full or object bank absent). The spawned actor is
   owned by the game's reaper; observe it through oot_actor_query. */
extern OOT_LIB_FN int32_t oot_actor_spawn( int16_t actorId, float x, float y, float z,
                                           int16_t rotX, int16_t rotY, int16_t rotZ,
                                           int16_t params );

/* liboot v0.7: real scene loading. The gSceneTable inside the ROM's 'code'
   file is located structurally at first use; oot_scene_load extracts the
   scene + room files, relocates the scene's own collision (polys, surface
   materials, conveyors, water boxes) into the live BgCheck world — replacing
   whatever oot_static_world_load installed — and interprets the room mesh
   display lists into lib-owned triangle arrays. oot_static_world_load keeps
   working unchanged for arena-style callers.

   sceneIndex follows the game's scene enum (PAL retail tables carry indices
   0x00..0x64; the 9 debug scenes above that are absent from retail ROMs).
   Convenience names below for the probed scenes. Returns 0 on success, a
   negative error code otherwise (<0: nothing about the previous world
   changed, except -9: collision committed but room mesh unavailable).

   roomIndex selects one room; pass roomIndex == -1 to load the WHOLE scene —
   every room's mesh is interpreted and concatenated into one geometry stream
   (opaque triangles for all rooms first, then translucent for all rooms), so a
   full multi-room dungeon renders in a single draw set. Scene collision is
   whole-scene regardless. The raw default is OOT_SCENE_MAX_TRIANGLES; a host
   can select a larger allocation before loading.

   oot_scene_load uses the child-day layer; oot_scene_load_ex selects the
   child/adult and day/night alternate scene and room headers. Exit and void
   conditions are delivered through oot_world_event_poll. Type-1 room images
   and unresolved animated-material references are exposed as borrowed source
   data and metadata; the host still owns image compositing, material animation,
   and loading a transition's destination. Cutscene header variants are outside
   the layer-selection contract. */
/* Every retail scene, straight from the game's own scene table (indices
   0x00..0x64; the 0x65+ debug/test scenes are absent from retail ROMs).
   0x00..0x1A and 0x4F are the dungeons, boss rooms, and Ganon's castle/tower;
   load a full multi-room dungeon with oot_scene_load(index, -1). The loader
   accepts any of these; individual scenes still depend on the host-side image,
   material, actor, and transition responsibilities described above. */
enum OoTSceneIndex
{
    OOT_SCENE_DEKU_TREE                      = 0x00,
    OOT_SCENE_DODONGOS_CAVERN                = 0x01,
    OOT_SCENE_JABU_JABU                      = 0x02,   /* Inside Jabu-Jabu's Belly */
    OOT_SCENE_FOREST_TEMPLE                  = 0x03,
    OOT_SCENE_FIRE_TEMPLE                    = 0x04,
    OOT_SCENE_WATER_TEMPLE                   = 0x05,
    OOT_SCENE_SPIRIT_TEMPLE                  = 0x06,
    OOT_SCENE_SHADOW_TEMPLE                  = 0x07,
    OOT_SCENE_BOTTOM_OF_THE_WELL             = 0x08,
    OOT_SCENE_ICE_CAVERN                     = 0x09,
    OOT_SCENE_GANONS_TOWER                   = 0x0A,
    OOT_SCENE_GERUDO_TRAINING_GROUND         = 0x0B,
    OOT_SCENE_THIEVES_HIDEOUT                = 0x0C,
    OOT_SCENE_INSIDE_GANONS_CASTLE           = 0x0D,
    OOT_SCENE_GANONS_TOWER_COLLAPSE_INTERIOR = 0x0E,
    OOT_SCENE_INSIDE_GANONS_CASTLE_COLLAPSE  = 0x0F,
    OOT_SCENE_TREASURE_BOX_SHOP              = 0x10,
    OOT_SCENE_DEKU_TREE_BOSS                 = 0x11,
    OOT_SCENE_DODONGOS_CAVERN_BOSS           = 0x12,
    OOT_SCENE_JABU_JABU_BOSS                 = 0x13,
    OOT_SCENE_FOREST_TEMPLE_BOSS             = 0x14,
    OOT_SCENE_FIRE_TEMPLE_BOSS               = 0x15,
    OOT_SCENE_WATER_TEMPLE_BOSS              = 0x16,
    OOT_SCENE_SPIRIT_TEMPLE_BOSS             = 0x17,
    OOT_SCENE_SHADOW_TEMPLE_BOSS             = 0x18,
    OOT_SCENE_GANONDORF_BOSS                 = 0x19,
    OOT_SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR = 0x1A,
    OOT_SCENE_MARKET_ENTRANCE_DAY            = 0x1B,
    OOT_SCENE_MARKET_ENTRANCE_NIGHT          = 0x1C,
    OOT_SCENE_MARKET_ENTRANCE_RUINS          = 0x1D,
    OOT_SCENE_BACK_ALLEY_DAY                 = 0x1E,
    OOT_SCENE_BACK_ALLEY_NIGHT               = 0x1F,
    OOT_SCENE_MARKET_DAY                     = 0x20,
    OOT_SCENE_MARKET_NIGHT                   = 0x21,
    OOT_SCENE_MARKET_RUINS                   = 0x22,
    OOT_SCENE_TEMPLE_OF_TIME_EXTERIOR_DAY    = 0x23,
    OOT_SCENE_TEMPLE_OF_TIME_EXTERIOR_NIGHT  = 0x24,
    OOT_SCENE_TEMPLE_OF_TIME_EXTERIOR_RUINS  = 0x25,
    OOT_SCENE_KNOW_IT_ALL_BROS_HOUSE         = 0x26,
    OOT_SCENE_TWINS_HOUSE                    = 0x27,
    OOT_SCENE_MIDOS_HOUSE                    = 0x28,
    OOT_SCENE_SARIAS_HOUSE                   = 0x29,
    OOT_SCENE_KAKARIKO_CENTER_GUEST_HOUSE    = 0x2A,
    OOT_SCENE_BACK_ALLEY_HOUSE               = 0x2B,
    OOT_SCENE_BAZAAR                         = 0x2C,
    OOT_SCENE_KOKIRI_SHOP                    = 0x2D,
    OOT_SCENE_GORON_SHOP                     = 0x2E,
    OOT_SCENE_ZORA_SHOP                      = 0x2F,
    OOT_SCENE_POTION_SHOP_KAKARIKO           = 0x30,
    OOT_SCENE_POTION_SHOP_MARKET             = 0x31,
    OOT_SCENE_BOMBCHU_SHOP                   = 0x32,
    OOT_SCENE_HAPPY_MASK_SHOP                = 0x33,
    OOT_SCENE_LINKS_HOUSE                    = 0x34,
    OOT_SCENE_DOG_LADY_HOUSE                 = 0x35,
    OOT_SCENE_STABLE                         = 0x36,
    OOT_SCENE_IMPAS_HOUSE                    = 0x37,
    OOT_SCENE_LAKESIDE_LABORATORY            = 0x38,
    OOT_SCENE_CARPENTERS_TENT                = 0x39,
    OOT_SCENE_GRAVEKEEPERS_HUT               = 0x3A,
    OOT_SCENE_GREAT_FAIRYS_FOUNTAIN_MAGIC    = 0x3B,
    OOT_SCENE_FAIRYS_FOUNTAIN                = 0x3C,
    OOT_SCENE_GREAT_FAIRYS_FOUNTAIN_SPELLS   = 0x3D,
    OOT_SCENE_GROTTOS                        = 0x3E,
    OOT_SCENE_REDEAD_GRAVE                   = 0x3F,
    OOT_SCENE_GRAVE_WITH_FAIRYS_FOUNTAIN     = 0x40,
    OOT_SCENE_ROYAL_FAMILYS_TOMB             = 0x41,
    OOT_SCENE_SHOOTING_GALLERY               = 0x42,
    OOT_SCENE_TEMPLE_OF_TIME                 = 0x43,
    OOT_SCENE_CHAMBER_OF_THE_SAGES           = 0x44,
    OOT_SCENE_CASTLE_COURTYARD_GUARDS_DAY    = 0x45,
    OOT_SCENE_CASTLE_COURTYARD_GUARDS_NIGHT  = 0x46,
    OOT_SCENE_CUTSCENE_MAP                   = 0x47,
    OOT_SCENE_WINDMILL_AND_DAMPES_GRAVE      = 0x48,
    OOT_SCENE_FISHING_POND                   = 0x49,
    OOT_SCENE_CASTLE_COURTYARD_ZELDA         = 0x4A,
    OOT_SCENE_BOMBCHU_BOWLING_ALLEY          = 0x4B,
    OOT_SCENE_LON_LON_BUILDINGS              = 0x4C,
    OOT_SCENE_MARKET_GUARD_HOUSE             = 0x4D,
    OOT_SCENE_POTION_SHOP_GRANNY             = 0x4E,
    OOT_SCENE_GANON_BOSS                     = 0x4F,
    OOT_SCENE_HOUSE_OF_SKULLTULA             = 0x50,
    OOT_SCENE_HYRULE_FIELD                   = 0x51,
    OOT_SCENE_KAKARIKO_VILLAGE               = 0x52,
    OOT_SCENE_GRAVEYARD                      = 0x53,
    OOT_SCENE_ZORAS_RIVER                    = 0x54,
    OOT_SCENE_KOKIRI_FOREST                  = 0x55,
    OOT_SCENE_SACRED_FOREST_MEADOW           = 0x56,
    OOT_SCENE_LAKE_HYLIA                     = 0x57,
    OOT_SCENE_ZORAS_DOMAIN                   = 0x58,
    OOT_SCENE_ZORAS_FOUNTAIN                 = 0x59,
    OOT_SCENE_GERUDO_VALLEY                  = 0x5A,
    OOT_SCENE_LOST_WOODS                     = 0x5B,
    OOT_SCENE_DESERT_COLOSSUS                = 0x5C,
    OOT_SCENE_GERUDOS_FORTRESS               = 0x5D,
    OOT_SCENE_HAUNTED_WASTELAND              = 0x5E,
    OOT_SCENE_HYRULE_CASTLE                  = 0x5F,
    OOT_SCENE_DEATH_MOUNTAIN_TRAIL           = 0x60,
    OOT_SCENE_DEATH_MOUNTAIN_CRATER          = 0x61,
    OOT_SCENE_GORON_CITY                     = 0x62,
    OOT_SCENE_LON_LON_RANCH                  = 0x63,
    OOT_SCENE_OUTSIDE_GANONS_CASTLE          = 0x64,
};

/* Raised for full multi-room dungeon loads (oot_scene_load roomIndex == -1);
   a single room stays well under this. Buffers are heap-allocated, so this is
   a memory/capacity knob, not a hard game limit. */
#define OOT_SCENE_MAX_TRIANGLES 16384

/* Select the allocation used by subsequent raw scene loads. Pass zero for the
   default. The call is rejected while a ROM scene is active; configure it
   after oot_global_init and before the first oot_scene_load. */
extern OOT_LIB_FN bool oot_scene_set_geometry_capacity( uint32_t triangleCapacity );

enum OoTSceneLayer
{
    OOT_SCENE_LAYER_CHILD_DAY = 0,
    OOT_SCENE_LAYER_CHILD_NIGHT = 1,
    OOT_SCENE_LAYER_ADULT_DAY = 2,
    OOT_SCENE_LAYER_ADULT_NIGHT = 3
};

/* Size-tagged scene loader. `layer` selects the matching scene and room
   alternate headers and sets the active day/night state. It does not recreate
   a live Link; callers select the matching age separately with
   oot_link_set_age. Cutscene layers are deliberately outside this contract. */
struct OoTSceneLoadOptions
{
    uint32_t structSize;
    int32_t sceneIndex;
    int32_t roomIndex;
    uint8_t layer;
    uint8_t reserved[3];
};
#define OOT_SCENE_LOAD_OPTIONS_INIT(scene, room, sceneLayer) \
    { sizeof(struct OoTSceneLoadOptions), (scene), (room), (sceneLayer), { 0u, 0u, 0u } }

extern OOT_LIB_FN int32_t oot_scene_load_ex( const struct OoTSceneLoadOptions *options );
extern OOT_LIB_FN int32_t oot_scene_load( int32_t sceneIndex, int32_t roomIndex );
extern OOT_LIB_FN bool oot_scene_get_active_layer( uint8_t *outLayer,
                                                    bool *outUsedFallback );

/* Borrowed prerender backgrounds from effective type-1 room shapes. Image and
   TLUT byte views remain valid until the next scene/room load or global
   termination. JPEG views contain the original encoded stream from SOI through
   the validated EOI marker; liboot does not decode them to pixels. Raw views
   contain exactly width*height at the N64 texel size. cameraIndex is -1 for a
   single-image room and the retail bg-camera index for a multi-image room. */
#define OOT_SCENE_BACKGROUND_VERSION 1u
enum OoTSceneBackgroundEncoding
{
    OOT_SCENE_BACKGROUND_NONE = 0,
    OOT_SCENE_BACKGROUND_RAW = 1,
    OOT_SCENE_BACKGROUND_JPEG = 2
};
struct OoTSceneBackground
{
    uint32_t structSize;
    uint32_t version;
    int32_t roomIndex;
    int16_t cameraIndex;
    uint8_t amountType;       /* RoomShapeImageAmountType: 1 single, 2 multi */
    uint8_t encoding;         /* enum OoTSceneBackgroundEncoding */
    uint16_t width;
    uint16_t height;
    uint8_t format;           /* raw N64 G_IM_FMT_* value */
    uint8_t size;             /* raw N64 G_IM_SIZ_* value */
    uint16_t tlutMode;        /* raw G_TT_* other-mode value */
    uint16_t tlutCount;       /* number of 16-bit TLUT entries */
    uint16_t entryFlags;      /* multi-entry unk_00; zero for single */
    uint32_t sourceAddress;   /* original segmented image address */
    uint32_t tlutAddress;     /* original segmented TLUT address */
    uint32_t sourceMetadata;  /* preserved unknown word beside source */
    const uint8_t *imageBytes;
    size_t imageByteCount;
    const uint8_t *tlutBytes;
    size_t tlutByteCount;
};
extern OOT_LIB_FN int32_t oot_scene_get_background_count( void );
extern OOT_LIB_FN bool oot_scene_get_background(
    int32_t index, struct OoTSceneBackground *outBackground );

/* Scene draw-config animation contract. The geometry interpreter records every
   unresolved draw-config segment (8..15) reached by a room display list. A
   host can key its canonical animation rules by drawConfigId, simulationFrame,
   segment/address, room, pass, and affected triangle range. Zero-length ranges
   identify unresolved display lists, vertex streams, or matrices whose geometry
   or state could not be executed by liboot. Texture-image ranges extend to the
   next texture-state command or the current root display-list end. */
#define OOT_SCENE_MATERIAL_STATE_VERSION 1u
#define OOT_SCENE_MATERIAL_REFERENCE_VERSION 1u
enum OoTSceneMaterialReferenceKind
{
    OOT_SCENE_MATERIAL_DISPLAY_LIST = 1,
    OOT_SCENE_MATERIAL_TEXTURE_IMAGE = 2,
    OOT_SCENE_MATERIAL_VERTEX_DATA = 3,
    OOT_SCENE_MATERIAL_MATRIX = 4
};
struct OoTSceneMaterialState
{
    uint32_t structSize;
    uint32_t version;
    uint64_t simulationFrame;
    uint32_t referenceCount;
    uint16_t segmentMask;       /* bit N means segment N was referenced */
    uint8_t drawConfigId;       /* enum SceneDrawConfig numeric value */
    uint8_t referencesTruncated;
    uint8_t reserved[8];
};
struct OoTSceneMaterialReference
{
    uint32_t structSize;
    uint32_t version;
    uint32_t segmentedAddress;
    uint32_t firstTriangle;
    uint32_t numTriangles;
    int16_t roomIndex;
    uint8_t segment;
    uint8_t kind;               /* enum OoTSceneMaterialReferenceKind */
    uint8_t renderPass;         /* enum OoTGeometryRenderPass */
    uint8_t reserved[3];
};
extern OOT_LIB_FN bool oot_scene_get_material_state(
    struct OoTSceneMaterialState *outState );
extern OOT_LIB_FN bool oot_scene_get_material_reference(
    int32_t index, struct OoTSceneMaterialReference *outReference );

/* Effective cmd 0x01 entries after alternate-header selection. Scene actors
   use room=-1/source=SCENE; room actors identify the source room. These are a
   ROM spawn catalog for host-side systems, not executable retail overlays. */
#define OOT_SCENE_ACTOR_ENTRY_VERSION 1u
enum OoTSceneActorSource
{
    OOT_SCENE_ACTOR_SOURCE_SCENE = 0,
    OOT_SCENE_ACTOR_SOURCE_ROOM = 1
};
struct OoTSceneActorEntry
{
    uint32_t structSize;
    uint32_t version;
    int16_t actorId;
    int16_t params;
    float position[3];
    int16_t rotation[3];
    int16_t room;
    uint8_t source;              /* enum OoTSceneActorSource */
    uint8_t layer;               /* enum OoTSceneLayer */
    uint16_t reserved;
};
extern OOT_LIB_FN int32_t oot_scene_get_actor_count( void );
extern OOT_LIB_FN bool oot_scene_get_actor(
    int32_t index, struct OoTSceneActorEntry *outEntry );

/* Exit-list entries referenced by the loaded header's collision surfaces.
   Exit indices in OoT surfaces are 1-based; this getter uses a conventional
   zero-based array index and returns the raw entrance-table value. */
extern OOT_LIB_FN int32_t oot_scene_get_exit_count( void );
extern OOT_LIB_FN bool oot_scene_get_exit( int32_t index, int16_t *outEntranceIndex );

enum OoTWorldEventKind
{
    OOT_WORLD_EVENT_SCENE_EXIT = 1,
    OOT_WORLD_EVENT_VOID_SURFACE = 2,
    OOT_WORLD_EVENT_VOID_Y = 3
};

#define OOT_WORLD_EVENT_VERSION 1u
struct OoTWorldEvent
{
    uint32_t structSize;
    uint32_t version;
    uint64_t sequence;
    uint32_t kind;
    int32_t sceneIndex;
    int32_t roomIndex;
    int16_t exitIndex;       /* raw 1-based surface exit index, or 0 */
    int16_t entranceIndex;   /* raw entrance-table value, or -1 for voids */
    uint8_t floorProperty;   /* original unmasked floor property */
    uint8_t layer;
    uint16_t reserved;
    float position[3];
};

/* Removes the oldest pending host transition event. Events are generated once
   per continuous contact; polling does not retrigger one until Link leaves and
   re-enters the exit/void condition. */
extern OOT_LIB_FN bool oot_world_event_poll( struct OoTWorldEvent *outEvent );

/* liboot vNEXT: room transitions. Swap the loaded scene's active room without
   re-picking the scene — the previous room's mesh is unloaded and roomIndex's
   is loaded (roomIndex == -1 re-loads the whole multi-room scene). Scene
   collision is unchanged. Returns 0 on success, or the oot_scene_load error
   code; -1 if no scene is currently loaded. This is the primitive a host uses
   to implement door-driven room switching (see the door list below). */
extern OOT_LIB_FN int32_t oot_scene_set_room( int32_t roomIndex );

/* liboot vNEXT: the scene's transition actors (doors). Each connects two rooms
   at a world position; a host detects Link crossing one and calls
   oot_scene_set_room(backRoom or frontRoom) to load the room on the far side.
   frontRoom/backRoom are the game's side[0]/side[1] rooms; actorId is the door
   actor's id; yaw is a binary angle. */
struct OoTDoor
{
    int16_t frontRoom;      /* side 0 room index */
    int16_t backRoom;       /* side 1 room index */
    int16_t actorId;        /* door actor id (game ACTOR_* value) */
    int16_t yaw;            /* facing, binary angle */
    float   pos[3];         /* world position */
};
extern OOT_LIB_FN int32_t oot_scene_get_door_count( void );
extern OOT_LIB_FN bool oot_scene_get_door( int32_t index, struct OoTDoor *outDoor );

/* liboot vNEXT: the loaded scene's background-music sequence id (the game's
   NA_BGM_* / SeqId, from the scene's sound-settings header command) and its
   nature-ambience id. These ids can be passed to the native audio API above.
   Returns -1 if no scene is loaded or the scene has no sound-settings command. */
extern OOT_LIB_FN int32_t oot_scene_get_sequence_id( void );
extern OOT_LIB_FN int32_t oot_scene_get_ambience_id( void );

/* liboot vNEXT: the loaded scene's active light/fog settings (scene cmd 0x0F,
   EnvLightSettings index 0). liboot already bakes this shade into the per-vertex
   color it emits, so scenes are lit like the real game; these values are also
   exported so a host can drive its own lighting/fog. Directions are unit-length
   and world-space, pointing toward the light (same space as emitted normals).
   fogNear is the N64 fog-coordinate near plane (0..1000) and fogFar is the far
   plane (zFar). valid is 0 (and fields are zeroed) when the scene declares no
   light settings — the renderer then falls back to a neutral daylight default.
   Returns false if no scene is loaded. */
struct OoTSceneEnvironment
{
    float   ambientColor[3];
    float   light1Dir[3];
    float   light1Color[3];
    float   light2Dir[3];
    float   light2Color[3];
    float   fogColor[3];
    float   fogNear;
    float   fogFar;
    uint8_t valid;
};
extern OOT_LIB_FN bool oot_scene_get_environment( struct OoTSceneEnvironment *out );

/* Live retail scene/room state copied from the fake PlayState after the scene
   commands have run. `geometryRoomIndex` is the caller's room selection and is
   -1 for an all-room mesh; `activeRoomIndex` remains the valid primary room 0
   in that mode, matching the segment-3 room used by gameplay. The structure is
   size/version tagged for bindings and future additive versions. The getter
   always initializes the tags and sentinel indices; it returns false when no
   ROM scene is active (for example after loading a custom static world). */
#define OOT_SCENE_RUNTIME_VERSION 1u
struct OoTSceneRuntime
{
    uint32_t structSize;
    uint32_t version;
    int32_t sceneIndex;
    int32_t activeRoomIndex;
    int32_t geometryRoomIndex;
    int32_t roomCount;
    int16_t worldMapArea;
    uint8_t roomType;
    uint8_t environmentType;
    int8_t echo;
    uint8_t lensMode;
    uint8_t warpSongsDisabled;
    uint8_t sceneCamType;
    uint8_t allRoomsLoaded;
    uint8_t roomMetadataValid;
    uint8_t reserved[2];
};
extern OOT_LIB_FN bool oot_scene_get_runtime( struct OoTSceneRuntime *out );

/* Interpreted room mesh: lib-owned arrays, valid until the next
   oot_scene_load / oot_global_terminate. Triangles are world-space; same
   per-vertex layout as OoTLinkGeometryBuffers (position/normal/color 3
   floats, uv 2 floats). Opaque pass first: [0, xluStart) opaque,
   [xluStart, numTriangles) translucent — draw the second range with
   blending. Textures land in the same cache as Link's
   (oot_get_texture; triTexture 0xFFFF = untextured). Any out pointer may be
   NULL. */
extern OOT_LIB_FN bool oot_scene_get_geometry( const float **position, const float **normal,
                                               const float **color, const float **uv,
                                               const uint16_t **triTexture,
                                               uint32_t *numTriangles, uint32_t *xluStartTriangle );
extern OOT_LIB_FN bool oot_scene_get_geometry_batches(
    const struct OoTGeometryBatch **batches, uint32_t *numBatches );
/* Number of otherwise valid triangles omitted by the most recent room-mesh
   interpretation because the configured scene capacity was reached. Link rendering
   does not change this value. */
extern OOT_LIB_FN uint32_t oot_scene_get_geometry_dropped_triangles( void );

/* liboot vNEXT: per-triangle render flags (enum OoTTriangleFlags), one byte per
   triangle, parallel to oot_scene_get_geometry's triTexture (same triangle
   count and order). Lets a host cull/alpha-test/decal each scene triangle the
   way the game does. Valid until the next oot_scene_load / oot_global_terminate.
   Returns false if no scene is loaded. */
extern OOT_LIB_FN bool oot_scene_get_triangle_flags( const uint8_t **outFlags );

/* Player spawn from the scene's own entrance data: outPos = spawn position,
   *outYaw = facing (binary angle). Spawn counts live in the game's entrance
   table (not parsed in v1): index 0 is always valid; higher indices are
   best-effort validated. */
extern OOT_LIB_FN bool oot_scene_spawn( int32_t spawnIndex, float outPos[3], int16_t *outYaw );

/* ABI layout guards for the raw shared structures (see OOT_STATIC_ASSERT).
   These structures contain no pointers or size_t, so their sizes are identical
   on 32- and 64-bit targets; the exact values are asserted unconditionally. */
OOT_STATIC_ASSERT(sizeof(struct OoTSurface) == 40, "OoTSurface ABI changed");
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
OOT_STATIC_ASSERT(sizeof(struct OoTGeometryBatch) == 96,
                  "OoTGeometryBatch ABI changed (64-bit)");
OOT_STATIC_ASSERT(offsetof(struct OoTGeometryBatch, primitiveColor) == 44,
                  "OoTGeometryBatch color offset changed (64-bit)");
#else
OOT_STATIC_ASSERT(sizeof(struct OoTGeometryBatch) == 96,
                  "OoTGeometryBatch ABI changed (32-bit)");
OOT_STATIC_ASSERT(offsetof(struct OoTGeometryBatch, primitiveColor) == 44,
                  "OoTGeometryBatch color offset changed (32-bit)");
#endif
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
OOT_STATIC_ASSERT(offsetof(struct OoTLinkGeometryBuffers, triangleCapacity) == 64,
                  "OoTLinkGeometryBuffers stable prefix changed");
OOT_STATIC_ASSERT(sizeof(struct OoTLinkGeometryBuffers) == 88,
                  "OoTLinkGeometryBuffers ABI changed (LP64/LLP64)");
#endif
OOT_STATIC_ASSERT(sizeof(struct OoTWaterBox) == 10, "OoTWaterBox ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTDynamicCollisionTransform) == 36,
                  "OoTDynamicCollisionTransform ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTDynamicCollisionState) == 48,
                  "OoTDynamicCollisionState ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTHostActorState) == 72,
                  "OoTHostActorState ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTHostActorContact) == 48,
                  "OoTHostActorContact ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTSceneActorEntry) == 36,
                  "OoTSceneActorEntry ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTSfxEvent) == 28, "OoTSfxEvent ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTSequenceInfo) == 32, "OoTSequenceInfo ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTAudioState) == 32, "OoTAudioState ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTSfxInfo) == 64, "OoTSfxInfo ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTActorInfo) == 24, "OoTActorInfo ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTSkeletonPose) == 276, "OoTSkeletonPose ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTSceneRuntime) == 36, "OoTSceneRuntime ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTSceneLoadOptions) == 16,
                  "OoTSceneLoadOptions ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTWorldEvent) == 48, "OoTWorldEvent ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTSceneMaterialState) == 32,
                  "OoTSceneMaterialState ABI changed");
OOT_STATIC_ASSERT(sizeof(struct OoTSceneMaterialReference) == 28,
                  "OoTSceneMaterialReference ABI changed");
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
OOT_STATIC_ASSERT(sizeof(struct OoTSceneBackground) == 72,
                  "OoTSceneBackground ABI changed (LP64/LLP64)");
#endif
OOT_STATIC_ASSERT(offsetof(struct OoTSceneRuntime, structSize) == 0,
                  "OoTSceneRuntime.structSize must be first");

#ifdef __cplusplus
}
#endif

#endif
