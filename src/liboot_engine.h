/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Cycl0o0
 */

#ifndef LIBOOT_ENGINE_H
#define LIBOOT_ENGINE_H

#include "liboot.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Engine-neutral integration layer
 * --------------------------------
 *
 * This API owns the transient buffers needed by a typical game-engine
 * integration and turns liboot's process-wide singleton into an opaque,
 * checked handle.  There can still only be one OoTEngine in a process at a
 * time.  Do not mix calls through this API with direct oot_* lifecycle calls.
 * Load a static world or ROM scene before creating Link; the native Player
 * initialization requires a live collision context.
 *
 * Public calls are protected by a non-blocking guard.  Concurrent or
 * callback-reentrant calls return OOT_ENGINE_RESULT_BUSY; callbacks must copy
 * any event data they want to retain and must not call back into the engine.
 */

#define OOT_ENGINE_API_VERSION 1u
#define OOT_ENGINE_DEFAULT_FIXED_STEP (1.0f / 20.0f)
#define OOT_ENGINE_DEFAULT_ACTOR_CAPACITY 64u
#define OOT_ENGINE_DEFAULT_MAX_SUBSTEPS 4u
#define OOT_ENGINE_MAX_ACTOR_CAPACITY 4096u
#define OOT_ENGINE_MAX_STATIC_SURFACES 2730u
#define OOT_ENGINE_MAX_WATER_BOXES 65535u
#define OOT_ENGINE_MIN_ROM_SIZE 0x1060u
#define OOT_ENGINE_MAX_ROM_SIZE (256u * 1024u * 1024u)
#define OOT_ENGINE_MAX_SUBSTEPS 1000u
#define OOT_ENGINE_MIN_FIXED_STEP_SECONDS 0.001f
#define OOT_ENGINE_MAX_FIXED_STEP_SECONDS 1.0f
#define OOT_ENGINE_INVALID_TARGET 0u

typedef struct OoTEngine OoTEngine;
typedef uint32_t OoTEngineTarget;

typedef enum OoTResult
{
    OOT_ENGINE_RESULT_OK = 0,
    OOT_ENGINE_RESULT_INVALID_ARGUMENT = -1,
    OOT_ENGINE_RESULT_API_VERSION = -2,
    OOT_ENGINE_RESULT_OUT_OF_MEMORY = -3,
    OOT_ENGINE_RESULT_SINGLETON_IN_USE = -4,
    OOT_ENGINE_RESULT_ROM_UNSUPPORTED = -5,
    OOT_ENGINE_RESULT_NOT_INITIALIZED = -6,
    OOT_ENGINE_RESULT_BUSY = -7,
    OOT_ENGINE_RESULT_LINK_ALREADY_EXISTS = -8,
    OOT_ENGINE_RESULT_LINK_NOT_FOUND = -9,
    OOT_ENGINE_RESULT_AGE_RESTRICTED = -10,
    OOT_ENGINE_RESULT_TARGET_CAPACITY = -11,
    OOT_ENGINE_RESULT_TARGET_NOT_FOUND = -12,
    OOT_ENGINE_RESULT_SCENE_LOAD_FAILED = -13,
    /* The scene collision was committed, but its display-list mesh could not
       be interpreted (the native oot_scene_load result is -9). */
    OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE = -14,
    OOT_ENGINE_RESULT_NO_FRAME = -15,
    OOT_ENGINE_RESULT_NOT_AVAILABLE = -16
} OoTResult;

typedef void (*OoTEngineDebugCallback)(void *userData, const char *message);
typedef void (*OoTEngineSfxCallback)(void *userData, const struct OoTSfxEvent *event);

enum OoTEngineRenderFlags
{
    OOT_ENGINE_RENDER_NAVI = 1u << 0,
    OOT_ENGINE_RENDER_ACTORS = 1u << 1
};

/* Only structSize, apiVersion, romData, and romSize are required. Fields not
   covered by structSize receive the defaults documented below. The ROM is
   copied by liboot during creation, need not outlive oot_engine_create, and
   must be OOT_ENGINE_MIN_ROM_SIZE..OOT_ENGINE_MAX_ROM_SIZE bytes. */
typedef struct OoTEngineConfig
{
    uint32_t structSize;
    uint32_t apiVersion;
    const uint8_t *romData;
    size_t romSize;
    uint32_t actorCapacity;       /* 0/default 64; maximum 4096 */
    uint32_t maxSubsteps;         /* 0/default 4; otherwise 1..1000 */
    float fixedStepSeconds;       /* 0/default 1/20; otherwise 0.001..1 */
    uint32_t renderFlags;         /* enum OoTEngineRenderFlags */
    OoTEngineDebugCallback debugCallback;
    void *debugUserData;
    OoTEngineSfxCallback sfxCallback;
    void *sfxUserData;
} OoTEngineConfig;

enum OoTEngineButtons
{
    OOT_ENGINE_BUTTON_A = 1u << 0,
    OOT_ENGINE_BUTTON_B = 1u << 1,
    OOT_ENGINE_BUTTON_Z = 1u << 2,
    OOT_ENGINE_BUTTON_R = 1u << 3,
    OOT_ENGINE_BUTTON_ITEM = 1u << 4,
    OOT_ENGINE_BUTTON_CUP = 1u << 5   /* liboot vNEXT: C-up (first-person/look) */
};

/* Host input for one simulation interval.  camLook is the camera-to-Link
   direction in the XZ plane.  It is normalized by the wrapper; (0,0) uses
   the +Z default.  Stick components are clamped to [-1,1], with negative and
   positive stickX meaning camera-left and camera-right respectively. */
typedef struct OoTEngineInput
{
    uint32_t structSize;
    float camLookX;
    float camLookZ;
    float stickX;
    float stickY;
    uint32_t buttons;             /* enum OoTEngineButtons bit mask */
} OoTEngineInput;

typedef struct OoTEngineGeometry
{
    uint32_t structSize;
    const float *position;        /* numTriangles * 3 vertices * xyz */
    const float *normal;
    const float *color;
    const float *uv;              /* numTriangles * 3 vertices * uv */
    const uint16_t *triTexture;   /* one texture index per triangle */
    uint32_t numTriangles;
    uint32_t triangleCapacity;
    const float *alpha;           /* liboot vNEXT: 1 float/vertex shade alpha, parallel to color */
    const uint8_t *triFlags;      /* liboot vNEXT: 1 byte/triangle render flags (OoTTriangleFlags) */
} OoTEngineGeometry;

/* Mirrors the state the core populates. `action` is Link's current high-level
   action (enum OoTAction); `animId` identifies the exact link_animetion entry. */
typedef struct OoTEngineLinkState
{
    uint32_t structSize;
    float position[3];
    float velocity[3];
    int16_t faceAngle;
    int16_t health;
    int16_t healthCapacity;
    int16_t magic;
    float linearVelocity;
    float animFrame;
    uint32_t stateFlags1;
    uint32_t stateFlags2;
    uint8_t magicLevel;
    uint8_t age;
    uint8_t isDead;
    int8_t heldItemAction;
    uint8_t meleeWeaponState;
    uint8_t lockOnActive;
    uint8_t inWater;
    uint8_t reserved0;
    float lockOnPos[3];
    float waterSurfaceY;
    /* liboot vNEXT: appended state (all fields above are unchanged). */
    uint32_t action;            /* enum OoTAction: Link's current high-level action */
    int16_t lookPitch;          /* head/aim pitch, binary angle (focus.rot.x) */
    int16_t lookYaw;            /* head/aim yaw, binary angle (focus.rot.y) */
    uint16_t floorSfxOffset;    /* floor-material sound group under Link */
    uint8_t attackAnim;         /* PLAYER_MWA_* swing id; valid while meleeWeaponState != 0 */
    uint8_t stateFlags3;        /* low-level Player state bits, bank 3 */
    uint16_t underwaterTimer;   /* 0..300 while submerged; host drives its own air meter */
    /* Occupies the two bytes that were tail padding before animId was exposed,
       preserving sizeof(OoTEngineLinkState) and every enclosing ABI offset. */
    int16_t animId;             /* stable 1-based link_animetion entry; 0 unknown */
} OoTEngineLinkState;

typedef struct OoTEngineNaviState
{
    uint32_t structSize;
    uint8_t available;
    uint8_t reserved[3];
    float position[3];
    float innerColor[4];
    float outerColor[4];
    float scale;
} OoTEngineNaviState;

/* The frame and everything referenced by it are owned by OoTEngine.  Pointers
   remain valid until the next mutating call on that engine or its destruction.
   actorListTruncated is set only when more actors existed than actorCapacity. */
typedef struct OoTEngineFrame
{
    uint32_t structSize;
    uint64_t simulationTick;
    float fixedStepSeconds;
    float interpolationAlpha;
    OoTEngineLinkState link;
    OoTEngineGeometry geometry;
    const struct OoTActorInfo *actors;
    uint32_t actorCount;
    uint32_t actorCapacity;
    uint8_t actorListTruncated;
    uint8_t skeletonAvailable;
    uint8_t reserved[2];
    struct OoTSkeletonPose skeleton;
    OoTEngineNaviState navi;
} OoTEngineFrame;

/* Scene arrays are copied into engine-owned storage after a successful load.
   The opaque/translucent split is [0,xluStartTriangle) and
   [xluStartTriangle,numTriangles). */
typedef struct OoTEngineSceneGeometry
{
    uint32_t structSize;
    const float *position;
    const float *normal;
    const float *color;
    const float *uv;
    const uint16_t *triTexture;
    uint32_t numTriangles;
    uint32_t xluStartTriangle;
    uint32_t triangleCapacity;
    const float *alpha;           /* liboot vNEXT: 1 float/vertex shade alpha, parallel to color */
    const uint8_t *triFlags;      /* liboot vNEXT: 1 byte/triangle render flags (OoTTriangleFlags) */
} OoTEngineSceneGeometry;

/* Texture pixels are borrowed from the core cache.  Query again after scene
   changes and whenever revision changes; copy/upload pixels before retaining
   them beyond another mutating engine call. */
typedef struct OoTEngineTexture
{
    uint32_t structSize;
    uint16_t width;
    uint16_t height;
    uint8_t wrapS;
    uint8_t wrapT;
    uint16_t reserved;
    uint32_t revision;
    const uint8_t *rgbaPixels;
    size_t rgbaSize;
} OoTEngineTexture;

/* Decoded PCM views are core-owned and valid until engine destruction. */
typedef struct OoTEnginePcm
{
    uint32_t structSize;
    const int16_t *samples;
    uint32_t sampleCount;
    uint32_t sampleRate;
    uint32_t loopStart;           /* sampleCount for a non-looping PCM clip */
} OoTEnginePcm;

#define OOT_ENGINE_CONFIG_INIT \
    { sizeof(OoTEngineConfig), OOT_ENGINE_API_VERSION, NULL, 0u, \
      OOT_ENGINE_DEFAULT_ACTOR_CAPACITY, OOT_ENGINE_DEFAULT_MAX_SUBSTEPS, \
      OOT_ENGINE_DEFAULT_FIXED_STEP, 0u, NULL, NULL, NULL, NULL }

#define OOT_ENGINE_INPUT_INIT \
    { sizeof(OoTEngineInput), 0.0f, 1.0f, 0.0f, 0.0f, 0u }

/* ABI layout guards (see OOT_STATIC_ASSERT in liboot.h).
 *
 * OoTResult crosses the ABI as a plain 32-bit signed integer; the first guard
 * fails loudly under -fshort-enums or an ABI that narrows the enum, which is
 * the exact assumption every binding makes (e.g. C# `enum Result : int`).
 *
 * Every size-tagged structure keeps `structSize` first so oot_engine_*_init_sized
 * and the versioned-growth contract keep working; the offset guards enforce it.
 *
 * Structures free of pointers/size_t have a fixed size on all targets and are
 * asserted unconditionally.  Structures containing pointers are asserted only on
 * the LP64/LLP64 (64-bit) targets liboot ships prebuilt, so a 32-bit host build
 * is never blocked by a size that legitimately differs by pointer width. */
OOT_STATIC_ASSERT(sizeof(OoTResult) == 4, "OoTResult must cross the ABI as int32");

OOT_STATIC_ASSERT(sizeof(OoTEngineInput) == 24, "OoTEngineInput ABI changed");
OOT_STATIC_ASSERT(sizeof(OoTEngineLinkState) == 92, "OoTEngineLinkState ABI changed");
OOT_STATIC_ASSERT(offsetof(OoTEngineLinkState, animId) == 90,
                  "OoTEngineLinkState.animId must occupy the former tail padding");
OOT_STATIC_ASSERT(sizeof(OoTEngineNaviState) == 56, "OoTEngineNaviState ABI changed");

OOT_STATIC_ASSERT(offsetof(OoTEngineConfig, structSize) == 0, "structSize must be first");
OOT_STATIC_ASSERT(offsetof(OoTEngineInput, structSize) == 0, "structSize must be first");
OOT_STATIC_ASSERT(offsetof(OoTEngineGeometry, structSize) == 0, "structSize must be first");
OOT_STATIC_ASSERT(offsetof(OoTEngineLinkState, structSize) == 0, "structSize must be first");
OOT_STATIC_ASSERT(offsetof(OoTEngineNaviState, structSize) == 0, "structSize must be first");
OOT_STATIC_ASSERT(offsetof(OoTEngineFrame, structSize) == 0, "structSize must be first");
OOT_STATIC_ASSERT(offsetof(OoTEngineSceneGeometry, structSize) == 0, "structSize must be first");
OOT_STATIC_ASSERT(offsetof(OoTEngineTexture, structSize) == 0, "structSize must be first");
OOT_STATIC_ASSERT(offsetof(OoTEnginePcm, structSize) == 0, "structSize must be first");

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
OOT_STATIC_ASSERT(sizeof(OoTEngineConfig) == 72, "OoTEngineConfig ABI changed (LP64/LLP64)");
OOT_STATIC_ASSERT(sizeof(OoTEngineGeometry) == 72, "OoTEngineGeometry ABI changed (LP64/LLP64)");
OOT_STATIC_ASSERT(sizeof(OoTEngineSceneGeometry) == 80, "OoTEngineSceneGeometry ABI changed (LP64/LLP64)");
OOT_STATIC_ASSERT(sizeof(OoTEngineTexture) == 32, "OoTEngineTexture ABI changed (LP64/LLP64)");
OOT_STATIC_ASSERT(sizeof(OoTEnginePcm) == 32, "OoTEnginePcm ABI changed (LP64/LLP64)");
OOT_STATIC_ASSERT(sizeof(OoTEngineFrame) == 544, "OoTEngineFrame ABI changed (LP64/LLP64)");
#endif

extern OOT_LIB_FN uint32_t oot_engine_api_version(void);
extern OOT_LIB_FN const char *oot_engine_result_string(OoTResult result);

/* Safe initializers for dynamic-language/FFI consumers. They validate the
   caller's compiled struct size and API version before writing anything, and
   never write beyond the smaller of structSize and this library's structure.
   Native C/C++ callers should normally use the convenience macros below. */
extern OOT_LIB_FN OoTResult oot_engine_config_init_sized(
    OoTEngineConfig *config, uint32_t structSize, uint32_t apiVersion);
extern OOT_LIB_FN OoTResult oot_engine_input_init_sized(
    OoTEngineInput *input, uint32_t structSize, uint32_t apiVersion);

#define oot_engine_config_init(config) \
    oot_engine_config_init_sized((config), (uint32_t)sizeof(*(config)), \
                                 OOT_ENGINE_API_VERSION)
#define oot_engine_input_init(input) \
    oot_engine_input_init_sized((input), (uint32_t)sizeof(*(input)), \
                                OOT_ENGINE_API_VERSION)

extern OOT_LIB_FN OoTResult oot_engine_create(const OoTEngineConfig *config,
                                               OoTEngine **outEngine);
extern OOT_LIB_FN OoTResult oot_engine_destroy(OoTEngine *engine);
extern OOT_LIB_FN OoTResult oot_engine_set_callbacks(OoTEngine *engine,
                                                      OoTEngineDebugCallback debugCallback,
                                                      void *debugUserData,
                                                      OoTEngineSfxCallback sfxCallback,
                                                      void *sfxUserData);

extern OOT_LIB_FN OoTResult oot_engine_link_create(OoTEngine *engine,
                                                    float x, float y, float z);
extern OOT_LIB_FN OoTResult oot_engine_link_delete(OoTEngine *engine);
extern OOT_LIB_FN OoTResult oot_engine_link_set_age(OoTEngine *engine, uint8_t age);
extern OOT_LIB_FN OoTResult oot_engine_link_set_equipment(OoTEngine *engine,
                                                          uint8_t sword, uint8_t shield,
                                                          uint8_t tunic, uint8_t boots);
extern OOT_LIB_FN OoTResult oot_engine_link_use_item(OoTEngine *engine, uint8_t item);
extern OOT_LIB_FN OoTResult oot_engine_link_set_health(OoTEngine *engine,
                                                       int16_t health, int16_t capacity);
extern OOT_LIB_FN OoTResult oot_engine_link_damage(OoTEngine *engine, int16_t amount);
extern OOT_LIB_FN OoTResult oot_engine_link_set_magic(OoTEngine *engine,
                                                      uint8_t level, int16_t amount);

/* liboot vNEXT: move Link in place (position + facing yaw) without recreating
   him; combine with oot_engine_link_freeze for a clean reposition. */
extern OOT_LIB_FN OoTResult oot_engine_link_set_pose(OoTEngine *engine,
                                                     float x, float y, float z, int16_t yaw);
/* liboot vNEXT: freeze/unfreeze Link's simulation; a frozen Link still renders. */
extern OOT_LIB_FN OoTResult oot_engine_link_freeze(OoTEngine *engine, uint8_t frozen);
/* liboot vNEXT: set the invincibility timer (positive intangible / negative
   invulnerable / 0 clear); re-apply each tick to hold it. */
extern OOT_LIB_FN OoTResult oot_engine_link_set_invincible(OoTEngine *engine, int8_t frames);
/* liboot vNEXT: raycast the live collision world under a point; fills real
   floor type/material/hookshot from the surface. NOT_AVAILABLE if no floor. */
extern OOT_LIB_FN OoTResult oot_engine_scene_query_surface(OoTEngine *engine,
                                                           float x, float y, float z,
                                                           struct OoTSurfaceInfo *outInfo);

/* step advances exactly one native simulation tick.  advance accumulates host
   time and runs at most maxSubsteps ticks, discarding excess catch-up time.
   A button observed during a no-tick advance is latched for the next tick. */
extern OOT_LIB_FN OoTResult oot_engine_step(OoTEngine *engine,
                                            const OoTEngineInput *input,
                                            const OoTEngineFrame **outFrame);
extern OOT_LIB_FN OoTResult oot_engine_advance(OoTEngine *engine,
                                               float elapsedSeconds,
                                               const OoTEngineInput *input,
                                               uint32_t *outSteps,
                                               const OoTEngineFrame **outFrame);
extern OOT_LIB_FN OoTResult oot_engine_get_frame(OoTEngine *engine,
                                                 const OoTEngineFrame **outFrame);
extern OOT_LIB_FN OoTResult oot_engine_reset_clock(OoTEngine *engine);

extern OOT_LIB_FN OoTResult oot_engine_static_world_load(
    OoTEngine *engine, const struct OoTSurface *surfaces, uint32_t numSurfaces,
    const struct OoTWaterBox *waterBoxes, uint32_t numWaterBoxes);
/* roomIndex selects one room; pass -1 to load the whole scene (all rooms
   concatenated into one geometry stream). See oot_scene_load in liboot.h. */
extern OOT_LIB_FN OoTResult oot_engine_scene_load(OoTEngine *engine,
                                                  int32_t sceneIndex, int32_t roomIndex,
                                                  int32_t *outNativeResult);
extern OOT_LIB_FN OoTResult oot_engine_scene_get_geometry(
    OoTEngine *engine, const OoTEngineSceneGeometry **outGeometry);
/* liboot vNEXT: door-driven room transitions. set_room swaps the active room
   (roomIndex -1 = whole scene); get_door_count/get_door expose the scene's
   transition actors so a host can detect a crossing and call set_room. */
extern OOT_LIB_FN OoTResult oot_engine_scene_set_room(OoTEngine *engine,
                                                      int32_t roomIndex,
                                                      int32_t *outNativeResult);
extern OOT_LIB_FN OoTResult oot_engine_scene_get_door_count(OoTEngine *engine,
                                                            uint32_t *outCount);
extern OOT_LIB_FN OoTResult oot_engine_scene_get_door(OoTEngine *engine, uint32_t index,
                                                      struct OoTDoor *outDoor);
extern OOT_LIB_FN OoTResult oot_engine_scene_get_spawn(OoTEngine *engine,
                                                       int32_t spawnIndex,
                                                       float outPosition[3],
                                                       int16_t *outYaw);

/* liboot vNEXT: the loaded scene's sound settings (cmd 0x15).  outSeqId is the
   background-music sequence id and outAmbienceId the nature-ambience id for the
   current scene; both are set to -1 when the scene declares no sound settings.
   A host feeds these to its own sequenced-audio player. */
extern OOT_LIB_FN OoTResult oot_engine_scene_get_sequence_id(OoTEngine *engine,
                                                             int32_t *outSeqId);
extern OOT_LIB_FN OoTResult oot_engine_scene_get_ambience_id(OoTEngine *engine,
                                                             int32_t *outAmbienceId);

/* liboot vNEXT: the loaded scene's active light/fog settings (see
   oot_scene_get_environment). liboot already bakes this shade into emitted
   vertex colors; the values let a host drive its own lighting/fog. Returns
   OOT_ENGINE_RESULT_NOT_FOUND-style empty (valid=0, fields zeroed) when no
   scene is loaded. */
extern OOT_LIB_FN OoTResult oot_engine_scene_get_environment(OoTEngine *engine,
                                                             struct OoTSceneEnvironment *outEnv);

/* Snapshot of the real scene/room fields currently driving Player behavior.
   The caller owns outRuntime; the function copies into it while holding the
   engine guard. Returns NOT_AVAILABLE after a custom world load or before any
   ROM scene is active. See OoTSceneRuntime in liboot.h. */
extern OOT_LIB_FN OoTResult oot_engine_scene_get_runtime(OoTEngine *engine,
                                                        struct OoTSceneRuntime *outRuntime);

extern OOT_LIB_FN OoTResult oot_engine_target_create(OoTEngine *engine,
                                                      float x, float y, float z,
                                                      float focusHeight,
                                                      OoTEngineTarget *outTarget);
extern OOT_LIB_FN OoTResult oot_engine_target_move(OoTEngine *engine,
                                                    OoTEngineTarget target,
                                                    float x, float y, float z);
extern OOT_LIB_FN OoTResult oot_engine_target_remove(OoTEngine *engine,
                                                      OoTEngineTarget target);
extern OOT_LIB_FN OoTResult oot_engine_targets_clear(OoTEngine *engine);

extern OOT_LIB_FN OoTResult oot_engine_set_render_flags(OoTEngine *engine,
                                                         uint32_t renderFlags);
extern OOT_LIB_FN OoTResult oot_engine_get_render_flags(OoTEngine *engine,
                                                         uint32_t *outRenderFlags);

extern OOT_LIB_FN OoTResult oot_engine_texture_count(OoTEngine *engine,
                                                      uint32_t *outCount);
extern OOT_LIB_FN OoTResult oot_engine_texture_get(OoTEngine *engine,
                                                    uint32_t index,
                                                    OoTEngineTexture *outTexture);
extern OOT_LIB_FN OoTResult oot_engine_voice_get(OoTEngine *engine,
                                                 uint16_t sfxId,
                                                 OoTEnginePcm *outPcm);
extern OOT_LIB_FN OoTResult oot_engine_ocarina_note_get(OoTEngine *engine,
                                                        uint8_t noteIndex,
                                                        OoTEnginePcm *outPcm);

/* liboot vNEXT: opt-in proximity-driven enemy/battle BGM. When enabled, liboot
   plays the OoT battle sequence on OOT_AUDIO_PLAYER_SUB while the vendored
   Player/actor code reports a hostile enemy within its 500-unit battle range,
   scaling volume with proximity and fading out when none remain. Disabled by
   default; layers over any scene BGM the host drives on another player. For the
   player/sequence/fade overrides and the live state, use the raw
   oot_audio_set_enemy_bgm / oot_audio_get_enemy_bgm. */
extern OOT_LIB_FN OoTResult oot_engine_set_enemy_bgm(OoTEngine *engine, uint8_t enabled);

#ifdef __cplusplus
}
#endif

#endif
