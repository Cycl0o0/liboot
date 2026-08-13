# API reference

Every exported symbol in the two public headers. The engine-neutral API in
[`liboot_engine.h`](../src/liboot_engine.h) is the recommended surface; the
low-level compatibility API in [`liboot.h`](../src/liboot.h) is process-global
and uses integer handles. Both headers are wrapped in `extern "C"` and every
function carries the `OOT_LIB_FN` linkage macro.

For worked examples see [USAGE.md](USAGE.md). The headers themselves remain the
normative source; this document tracks them but the compiler does not.

- [Engine API (`liboot_engine.h`)](#engine-api-liboot_engineh)
  - [Constants](#engine-constants) · [Results](#ootresult) ·
    [Enums](#engine-enums) · [Structs](#engine-structs) ·
    [Functions](#engine-functions)
- [Low-level API (`liboot.h`)](#low-level-api-libooth)
  - [Constants](#low-level-constants) · [Enums](#low-level-enums) ·
    [Structs](#low-level-structs) · [Functions](#low-level-functions)
- [Notes on the two APIs](#notes-on-the-two-apis)

---

# Engine API (`liboot_engine.h`)

Shared-library builds isolate multiple engines; static archives allow one.
Query the capability flags rather than assuming a linkage model. Do not mix
this API with raw `oot_*` lifecycle calls. Load a static world or ROM scene
before creating Link: native Player init needs a live collision context. Public
calls use a non-blocking guard; concurrent or callback-reentrant calls return
`OOT_ENGINE_RESULT_BUSY`, and callbacks must not re-enter the engine.

## Engine constants

| Macro | Value |
| --- | --- |
| `OOT_ENGINE_API_VERSION` | `1` |
| `OOT_ENGINE_DEFAULT_FIXED_STEP` | `3.0f / 50.0f` |
| `OOT_ENGINE_DEFAULT_ACTOR_CAPACITY` | `64` |
| `OOT_ENGINE_DEFAULT_MAX_SUBSTEPS` | `4` |
| `OOT_ENGINE_DEFAULT_LINK_TRIANGLE_CAPACITY` | `2048` |
| `OOT_ENGINE_DEFAULT_SCENE_TRIANGLE_CAPACITY` | `16384` |
| `OOT_ENGINE_MAX_GEOMETRY_TRIANGLE_CAPACITY` | `1048576` |
| `OOT_ENGINE_MAX_ACTOR_CAPACITY` | `4096` |
| `OOT_ENGINE_MAX_STATIC_SURFACES` | `2730` |
| `OOT_ENGINE_MAX_WATER_BOXES` | `65535` |
| `OOT_ENGINE_MAX_TARGETS` | `16` |
| `OOT_ENGINE_MAX_HOST_ACTORS` | `64` |
| `OOT_ENGINE_MAX_TEXTURES` | `1024` |
| `OOT_ENGINE_MIN_ROM_SIZE` | `0x1060` |
| `OOT_ENGINE_MAX_ROM_SIZE` | `256 * 1024 * 1024` |
| `OOT_ENGINE_MAX_SUBSTEPS` | `1000` |
| `OOT_ENGINE_MIN_FIXED_STEP_SECONDS` | `0.001f` |
| `OOT_ENGINE_MAX_FIXED_STEP_SECONDS` | `1.0f` |
| `OOT_ENGINE_INVALID_TARGET` | `0` |
| `OOT_ENGINE_INVALID_HOST_ACTOR` | `0` |
| `OOT_ENGINE_LIMITS_VERSION` | `1` |

Opaque types and callbacks:

```c
typedef struct OoTEngine OoTEngine;
typedef uint32_t OoTEngineTarget;
typedef uint32_t OoTEngineHostActor;
typedef void (*OoTEngineDebugCallback)(void *userData, const char *message);
typedef void (*OoTEngineSfxCallback)(void *userData, const struct OoTSfxEvent *event);
```

Native C/C++ callers should use the convenience initializers
`oot_engine_config_init(&config)` and `oot_engine_input_init(&input)` (macros
over the `_sized` functions). The static initializers `OOT_ENGINE_CONFIG_INIT`
and `OOT_ENGINE_INPUT_INIT` are also provided.

```c
enum OoTEngineCapabilities {
    OOT_ENGINE_CAPABILITY_STATIC_WORLD        = 1u << 0,
    OOT_ENGINE_CAPABILITY_ROM_SCENE_LOADING   = 1u << 1,
    OOT_ENGINE_CAPABILITY_LINK_GEOMETRY       = 1u << 2,
    OOT_ENGINE_CAPABILITY_SCENE_GEOMETRY      = 1u << 3,
    OOT_ENGINE_CAPABILITY_GEOMETRY_TRUNCATION = 1u << 4,
    OOT_ENGINE_CAPABILITY_ACTOR_QUERY         = 1u << 5,
    OOT_ENGINE_CAPABILITY_TARGETS             = 1u << 6,
    OOT_ENGINE_CAPABILITY_TEXTURES            = 1u << 7,
    OOT_ENGINE_CAPABILITY_FIXED_STEP          = 1u << 8,
    OOT_ENGINE_CAPABILITY_AUDIO               = 1u << 9,
    OOT_ENGINE_CAPABILITY_PROCESS_SINGLETON   = 1u << 10,
    OOT_ENGINE_CAPABILITY_DYNAMIC_COLLISION   = 1u << 11,
    OOT_ENGINE_CAPABILITY_GEOMETRY_BATCHES    = 1u << 12,
    OOT_ENGINE_CAPABILITY_HOST_ACTORS         = 1u << 13,
    OOT_ENGINE_CAPABILITY_SCENE_ACTOR_CATALOG = 1u << 14,
    OOT_ENGINE_CAPABILITY_MULTI_INSTANCE      = 1u << 15,
    OOT_ENGINE_CAPABILITY_SCENE_LAYERS        = 1u << 16,
    OOT_ENGINE_CAPABILITY_SCENE_TRANSITIONS   = 1u << 17,
    OOT_ENGINE_CAPABILITY_SCENE_BACKGROUNDS   = 1u << 18,
    OOT_ENGINE_CAPABILITY_SCENE_MATERIAL_METADATA = 1u << 19
};
```

The bit mask lets dynamic hosts test a contract directly instead of inferring
features from a library filename. Shared builds advertise `MULTI_INSTANCE` and
may own several independent `OoTEngine` handles. Static archives advertise
`PROCESS_SINGLETON`, because their writable sections are merged into the host.

## OoTResult

Crosses the ABI as a 32-bit signed integer. `0` is success; all errors are
negative.

| Code | Value | Meaning |
| --- | --- | --- |
| `OOT_ENGINE_RESULT_OK` | 0 | Success. |
| `OOT_ENGINE_RESULT_INVALID_ARGUMENT` | -1 | A null or out-of-range argument. |
| `OOT_ENGINE_RESULT_API_VERSION` | -2 | Struct API version mismatch. |
| `OOT_ENGINE_RESULT_OUT_OF_MEMORY` | -3 | Allocation failed. |
| `OOT_ENGINE_RESULT_SINGLETON_IN_USE` | -4 | An engine already exists. |
| `OOT_ENGINE_RESULT_ROM_UNSUPPORTED` | -5 | The ROM could not be parsed. |
| `OOT_ENGINE_RESULT_NOT_INITIALIZED` | -6 | The engine is not initialized. |
| `OOT_ENGINE_RESULT_BUSY` | -7 | Concurrent or reentrant call rejected. |
| `OOT_ENGINE_RESULT_LINK_ALREADY_EXISTS` | -8 | Link already created. |
| `OOT_ENGINE_RESULT_LINK_NOT_FOUND` | -9 | Operation needs a Link. |
| `OOT_ENGINE_RESULT_AGE_RESTRICTED` | -10 | Item/action not allowed for the age. |
| `OOT_ENGINE_RESULT_TARGET_CAPACITY` | -11 | Target pool full. |
| `OOT_ENGINE_RESULT_TARGET_NOT_FOUND` | -12 | Unknown target handle. |
| `OOT_ENGINE_RESULT_SCENE_LOAD_FAILED` | -13 | Scene load failed. |
| `OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE` | -14 | Collision committed, mesh uninterpretable. |
| `OOT_ENGINE_RESULT_NO_FRAME` | -15 | No frame produced yet. |
| `OOT_ENGINE_RESULT_NOT_AVAILABLE` | -16 | Unsupported ordering or query. |
| `OOT_ENGINE_RESULT_DYNAMIC_COLLISION_CAPACITY` | -17 | Native dynamic-collision budget exhausted. |
| `OOT_ENGINE_RESULT_DYNAMIC_COLLISION_NOT_FOUND` | -18 | Stale or unknown dynamic-collision handle. |
| `OOT_ENGINE_RESULT_HOST_ACTOR_CAPACITY` | -19 | Host-actor pool exhausted. |
| `OOT_ENGINE_RESULT_HOST_ACTOR_NOT_FOUND` | -20 | Stale or unknown host-actor handle. |

`oot_engine_result_string(result)` returns a human-readable name.

## Engine enums

```c
enum OoTEngineRenderFlags {
    OOT_ENGINE_RENDER_NAVI   = 1u << 0,
    OOT_ENGINE_RENDER_ACTORS = 1u << 1
};

enum OoTEngineButtons {          /* OoTEngineInput.buttons bit mask */
    OOT_ENGINE_BUTTON_A    = 1u << 0,
    OOT_ENGINE_BUTTON_B    = 1u << 1,
    OOT_ENGINE_BUTTON_Z    = 1u << 2,
    OOT_ENGINE_BUTTON_R    = 1u << 3,
    OOT_ENGINE_BUTTON_ITEM = 1u << 4,
    OOT_ENGINE_BUTTON_CUP  = 1u << 5   /* C-up: first-person / look */
};
```

Age, sword, shield, tunic, boots, item, action, surface-type, triangle-flag,
audio-player, ocarina-song, and scene-index enums are shared with the low-level
header; see [Low-level enums](#low-level-enums).

## Engine structs

Each struct begins with `structSize` for versioned FFI growth. Field comments
below are from the header.

### OoTEngineLimits

```c
typedef struct OoTEngineLimits {
    uint32_t structSize;
    uint32_t version;
    uint64_t capabilityFlags;
    uint32_t linkTriangleCapacity;
    uint32_t sceneTriangleCapacity;
    uint32_t staticSurfaceCapacity;
    uint32_t waterBoxCapacity;
    uint32_t maxActorCapacity;
    uint32_t targetCapacity;
    uint32_t textureCapacity;
    uint32_t maxSubsteps;
    float minFixedStepSeconds;
    float maxFixedStepSeconds;
    float defaultFixedStepSeconds;
    uint32_t defaultMaxSubsteps;
    uint32_t maxLinkTriangleCapacity;
    uint32_t maxSceneTriangleCapacity;
} OoTEngineLimits;            /* sizeof == 72 */
```

Initialize with `OOT_ENGINE_LIMITS_INIT`. The getter accepts the required
prefix through `capabilityFlags`, preserves the caller's `structSize`, and
never writes past it.

### OoTEngineConfig

```c
typedef struct OoTEngineConfig {
    uint32_t structSize;
    uint32_t apiVersion;
    const uint8_t *romData;       /* required */
    size_t romSize;               /* required, MIN_ROM_SIZE..MAX_ROM_SIZE */
    uint32_t actorCapacity;       /* 0/default 64; max 4096 */
    uint32_t maxSubsteps;         /* 0/default 4; else 1..1000 */
    float fixedStepSeconds;       /* 0/default 3/50; else 0.001..1 */
    uint32_t renderFlags;         /* enum OoTEngineRenderFlags */
    OoTEngineDebugCallback debugCallback;
    void *debugUserData;
    OoTEngineSfxCallback sfxCallback;
    void *sfxUserData;
    uint32_t linkTriangleCapacity;  /* 0/default 2048; max 1048576 */
    uint32_t sceneTriangleCapacity; /* 0/default 16384; max 1048576 */
} OoTEngineConfig;
```

The ROM is copied during `oot_engine_create` and need not outlive it.

### OoTEngineInput

```c
typedef struct OoTEngineInput {
    uint32_t structSize;
    float camLookX;   /* camera-to-Link direction, XZ; normalized; (0,0) -> +Z */
    float camLookZ;
    float stickX;     /* [-1,1]; -/+ = camera-left/right */
    float stickY;     /* [-1,1] */
    uint32_t buttons; /* enum OoTEngineButtons */
} OoTEngineInput;     /* sizeof == 24 */
```

### OoTEngineGeometry

```c
typedef struct OoTEngineGeometry {
    uint32_t structSize;
    const float *position;      /* numTriangles * 3 verts * xyz */
    const float *normal;
    const float *color;
    const float *uv;            /* numTriangles * 3 verts * uv */
    const uint16_t *triTexture; /* one texture index per triangle; 0xFFFF none */
    uint32_t numTriangles;
    uint32_t triangleCapacity;
    const float *alpha;         /* 1 float/vertex shade alpha, parallel to color */
    const uint8_t *triFlags;    /* 1 byte/triangle, enum OoTTriangleFlags */
} OoTEngineGeometry;
```

### OoTEngineLinkState

```c
typedef struct OoTEngineLinkState {
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
    uint32_t action;          /* enum OoTAction */
    int16_t lookPitch;        /* aim pitch, binary angle */
    int16_t lookYaw;          /* aim yaw, binary angle */
    uint16_t floorSfxOffset;  /* floor-material sound group */
    uint8_t attackAnim;       /* PLAYER_MWA_* swing id; valid while meleeWeaponState != 0 */
    uint8_t stateFlags3;
    uint16_t underwaterTimer; /* 0..300 while submerged */
    int16_t animId;           /* stable 1-based link_animetion entry; 0 unknown */
} OoTEngineLinkState;         /* sizeof == 92; animId at offset 90 */
```

Note: this layout differs from the low-level `struct OoTLinkState`. Do not
assume the two are interchangeable.

### OoTEngineNaviState

```c
typedef struct OoTEngineNaviState {
    uint32_t structSize;
    uint8_t available;
    uint8_t reserved[3];
    float position[3];
    float innerColor[4];
    float outerColor[4];
    float scale;
} OoTEngineNaviState;         /* sizeof == 56 */
```

### OoTEngineFrame

Owned by the engine; every pointer is valid only until the next mutating call
or destruction.

```c
typedef struct OoTEngineFrame {
    uint32_t structSize;
    uint64_t simulationTick;
    float fixedStepSeconds;
    float interpolationAlpha;   /* [0,1) progress into the next tick */
    OoTEngineLinkState link;
    OoTEngineGeometry geometry;
    const struct OoTActorInfo *actors;
    uint32_t actorCount;
    uint32_t actorCapacity;
    uint8_t actorListTruncated; /* more actors existed than actorCapacity */
    uint8_t skeletonAvailable;
    uint8_t linkGeometryTruncated; /* valid triangles omitted at the cap */
    uint8_t reserved0;
    struct OoTSkeletonPose skeleton;
    OoTEngineNaviState navi;
    const struct OoTGeometryBatch *geometryBatches;
    uint32_t geometryBatchCount;
    uint32_t geometryBatchCapacity;
} OoTEngineFrame;               /* sizeof == 560 on 64-bit */
```

### OoTEngineSceneGeometry

```c
typedef struct OoTEngineSceneGeometry {
    uint32_t structSize;
    const float *position;
    const float *normal;
    const float *color;
    const float *uv;
    const uint16_t *triTexture;
    uint32_t numTriangles;
    uint32_t xluStartTriangle;  /* opaque [0,xluStart); translucent [xluStart,num) */
    uint32_t triangleCapacity;
    const float *alpha;
    const uint8_t *triFlags;
    const struct OoTGeometryBatch *batches;
    uint32_t batchCount;
    uint32_t batchCapacity;
} OoTEngineSceneGeometry;
```

`OoTGeometryBatch` gives each contiguous range a source (Link, Navi, actor, or
scene room), texture/material key, opaque/translucent pass, normalized blend
and depth modes, depth-test/write flags, triangle flags, and the original RDP
render/combine words plus primitive/environment/base colors. This lets hosts preserve draw boundaries instead of
treating a scene as one opaque and one translucent material.

### OoTEngineTexture

```c
typedef struct OoTEngineTexture {
    uint32_t structSize;
    uint16_t width;
    uint16_t height;
    uint8_t wrapS;              /* 0 repeat, 1 mirror, 2 clamp */
    uint8_t wrapT;
    uint16_t reserved;
    uint32_t revision;         /* re-upload when this changes */
    const uint8_t *rgbaPixels; /* borrowed; RGBA8 */
    size_t rgbaSize;
} OoTEngineTexture;
```

### OoTEnginePcm

```c
typedef struct OoTEnginePcm {
    uint32_t structSize;
    const int16_t *samples;    /* core-owned; valid until engine destruction */
    uint32_t sampleCount;
    uint32_t sampleRate;
    uint32_t loopStart;        /* == sampleCount for a non-looping clip */
} OoTEnginePcm;
```

### OoTEngineActorContact

```c
typedef struct OoTEngineActorContact {
    uint32_t structSize;
    uint32_t version;
    OoTEngineHostActor actor; /* generation-checked host-actor handle */
    uint32_t source;          /* enum OoTHostActorContactSource */
    uint64_t userTag;
    uint32_t sourceActorId;   /* native OoT actor id, or ACTOR_PLAYER */
    uint32_t simulationTick;
    float position[3];
    uint32_t reserved;
} OoTEngineActorContact;      /* sizeof == 48 */
```

Before `oot_engine_host_actor_poll_contact`, set `structSize` to
`sizeof(OoTEngineActorContact)` and `version` to
`OOT_HOST_ACTOR_CONTACT_VERSION`. Invalid tags return
`OOT_ENGINE_RESULT_INVALID_ARGUMENT` without consuming a contact.

## Engine functions

### Lifecycle and configuration

```c
uint32_t     oot_engine_api_version(void);
const char  *oot_engine_result_string(OoTResult result);
OoTResult    oot_engine_get_limits(OoTEngineLimits *outLimits);
OoTResult    oot_engine_config_init_sized(OoTEngineConfig *config,
                                          uint32_t structSize, uint32_t apiVersion);
OoTResult    oot_engine_input_init_sized(OoTEngineInput *input,
                                         uint32_t structSize, uint32_t apiVersion);
OoTResult    oot_engine_create(const OoTEngineConfig *config, OoTEngine **outEngine);
OoTResult    oot_engine_destroy(OoTEngine *engine);   /* also deletes an active Link */
OoTResult    oot_engine_set_callbacks(OoTEngine *engine,
                                      OoTEngineDebugCallback debugCallback, void *debugUserData,
                                      OoTEngineSfxCallback sfxCallback, void *sfxUserData);
```

The `_sized` initializers validate the caller's compiled struct size and API
version before writing, and never write beyond the smaller of `structSize` and
the library's own struct. FFI bindings call them directly; C/C++ callers use the
`oot_engine_config_init` / `oot_engine_input_init` macros.

### Stepping

```c
OoTResult oot_engine_step(OoTEngine *engine, const OoTEngineInput *input,
                          const OoTEngineFrame **outFrame);      /* exactly one tick */
OoTResult oot_engine_advance(OoTEngine *engine, float elapsedSeconds,
                             const OoTEngineInput *input,
                             uint32_t *outSteps, const OoTEngineFrame **outFrame);
OoTResult oot_engine_get_frame(OoTEngine *engine, const OoTEngineFrame **outFrame);
OoTResult oot_engine_reset_clock(OoTEngine *engine);
```

`advance` accumulates host time and runs at most `maxSubsteps` ticks, discarding
excess catch-up. A button seen during a no-tick advance is latched for the next
tick.

### World and scene

```c
OoTResult oot_engine_static_world_load(OoTEngine *engine,
                                       const struct OoTSurface *surfaces, uint32_t numSurfaces,
                                       const struct OoTWaterBox *waterBoxes, uint32_t numWaterBoxes);
OoTResult oot_engine_dynamic_collision_create(
    OoTEngine *engine, const struct OoTSurface *surfaces, uint32_t numSurfaces,
    const struct OoTDynamicCollisionTransform *transform, uint32_t flags,
    OoTDynamicCollision *outHandle);
OoTResult oot_engine_dynamic_collision_set_transform(
    OoTEngine *engine, OoTDynamicCollision handle,
    const struct OoTDynamicCollisionTransform *transform);
OoTResult oot_engine_dynamic_collision_set_enabled(
    OoTEngine *engine, OoTDynamicCollision handle, uint8_t enabled);
OoTResult oot_engine_dynamic_collision_get_state(
    OoTEngine *engine, OoTDynamicCollision handle,
    struct OoTDynamicCollisionState *outState);
OoTResult oot_engine_dynamic_collision_delete(
    OoTEngine *engine, OoTDynamicCollision handle);
OoTResult oot_engine_scene_load(OoTEngine *engine, int32_t sceneIndex, int32_t roomIndex,
                                int32_t *outNativeResult);       /* roomIndex -1 = whole scene */
OoTResult oot_engine_scene_load_ex(OoTEngine *engine,
                                   const struct OoTSceneLoadOptions *options,
                                   int32_t *outNativeResult);
OoTResult oot_engine_scene_get_active_layer(OoTEngine *engine,
                                            uint8_t *outLayer,
                                            uint8_t *outUsedFallback);
OoTResult oot_engine_scene_get_geometry(OoTEngine *engine,
                                        const OoTEngineSceneGeometry **outGeometry);
OoTResult oot_engine_scene_get_dropped_triangles(OoTEngine *engine,
                                                  uint32_t *outDroppedTriangles);
OoTResult oot_engine_scene_set_room(OoTEngine *engine, int32_t roomIndex,
                                    int32_t *outNativeResult);
OoTResult oot_engine_scene_get_door_count(OoTEngine *engine, uint32_t *outCount);
OoTResult oot_engine_scene_get_door(OoTEngine *engine, uint32_t index, struct OoTDoor *outDoor);
OoTResult oot_engine_scene_get_spawn(OoTEngine *engine, int32_t spawnIndex,
                                     float outPosition[3], int16_t *outYaw);
OoTResult oot_engine_scene_get_sequence_id(OoTEngine *engine, int32_t *outSeqId);
OoTResult oot_engine_scene_get_ambience_id(OoTEngine *engine, int32_t *outAmbienceId);
OoTResult oot_engine_scene_get_environment(OoTEngine *engine, struct OoTSceneEnvironment *outEnv);
OoTResult oot_engine_scene_get_runtime(OoTEngine *engine, struct OoTSceneRuntime *outRuntime);
OoTResult oot_engine_scene_get_actor_count(OoTEngine *engine, uint32_t *outCount);
OoTResult oot_engine_scene_get_actor(OoTEngine *engine, uint32_t index,
                                     struct OoTSceneActorEntry *outEntry);
OoTResult oot_engine_scene_get_exit_count(OoTEngine *engine, uint32_t *outCount);
OoTResult oot_engine_scene_get_exit(OoTEngine *engine, uint32_t index,
                                    int16_t *outEntranceIndex);
OoTResult oot_engine_world_event_poll(OoTEngine *engine,
                                      struct OoTWorldEvent *outEvent);
OoTResult oot_engine_scene_get_background_count(OoTEngine *engine,
                                                uint32_t *outCount);
OoTResult oot_engine_scene_get_background(OoTEngine *engine, uint32_t index,
                                          struct OoTSceneBackground *outBackground);
OoTResult oot_engine_scene_get_material_state(
    OoTEngine *engine, struct OoTSceneMaterialState *outState);
OoTResult oot_engine_scene_get_material_reference(
    OoTEngine *engine, uint32_t index,
    struct OoTSceneMaterialReference *outReference);
OoTResult oot_engine_scene_query_surface(OoTEngine *engine, float x, float y, float z,
                                         struct OoTSurfaceInfo *outInfo);
```

`scene_get_sequence_id` / `scene_get_ambience_id` return `-1` in their out
parameter when the scene declares no sound settings. After a custom-world load,
`scene_get_runtime` returns `NOT_AVAILABLE`; `scene_get_environment` returns
`OK` with a zeroed record whose `valid` field is zero. Layer values select
child/adult and day/night headers. World-event polling reports scene exits and
void contacts without loading a destination. Background records contain
borrowed image/TLUT pointers valid until the next successful scene, room, or
static-world load, or engine/global termination. Material state and reference
records are copied snapshots; query material state again after simulation
ticks and query both again after a scene or room change.

### Link control

```c
OoTResult oot_engine_link_create(OoTEngine *engine, float x, float y, float z);
OoTResult oot_engine_link_delete(OoTEngine *engine);
OoTResult oot_engine_link_set_age(OoTEngine *engine, uint8_t age);
OoTResult oot_engine_link_set_equipment(OoTEngine *engine, uint8_t sword, uint8_t shield,
                                        uint8_t tunic, uint8_t boots);
OoTResult oot_engine_link_use_item(OoTEngine *engine, uint8_t item);
OoTResult oot_engine_link_set_health(OoTEngine *engine, int16_t health, int16_t capacity);
OoTResult oot_engine_link_damage(OoTEngine *engine, int16_t amount);
OoTResult oot_engine_link_set_magic(OoTEngine *engine, uint8_t level, int16_t amount);
OoTResult oot_engine_link_set_pose(OoTEngine *engine, float x, float y, float z, int16_t yaw);
OoTResult oot_engine_link_freeze(OoTEngine *engine, uint8_t frozen);   /* frozen Link still renders */
OoTResult oot_engine_link_set_invincible(OoTEngine *engine, int8_t frames); /* +intangible/-invuln/0 clear */
```

Link geometry, skeleton, actors, and Navi are delivered inside
`OoTEngineFrame`; there are no separate getters.

### Targets, host actors, and render flags

```c
OoTResult oot_engine_target_create(OoTEngine *engine, float x, float y, float z,
                                   float focusHeight, OoTEngineTarget *outTarget);
OoTResult oot_engine_target_move(OoTEngine *engine, OoTEngineTarget target,
                                 float x, float y, float z);
OoTResult oot_engine_target_remove(OoTEngine *engine, OoTEngineTarget target);
OoTResult oot_engine_targets_clear(OoTEngine *engine);
OoTResult oot_engine_host_actor_create(OoTEngine *engine,
                                       const struct OoTHostActorState *state,
                                       OoTEngineHostActor *outActor);
OoTResult oot_engine_host_actor_update(OoTEngine *engine,
                                       OoTEngineHostActor actor,
                                       const struct OoTHostActorState *state);
OoTResult oot_engine_host_actor_get(OoTEngine *engine,
                                    OoTEngineHostActor actor,
                                    struct OoTHostActorState *outState);
OoTResult oot_engine_host_actor_remove(OoTEngine *engine,
                                       OoTEngineHostActor actor);
OoTResult oot_engine_host_actors_clear(OoTEngine *engine);
OoTResult oot_engine_host_actor_poll_contact(OoTEngine *engine,
                                             OoTEngineActorContact *outContact);
OoTResult oot_engine_set_render_flags(OoTEngine *engine, uint32_t renderFlags);
OoTResult oot_engine_get_render_flags(OoTEngine *engine, uint32_t *outRenderFlags);
```

Host actors are native attention-list entries whose behavior and presentation
remain host-owned. Their generation-checked handles are separate from the
legacy 16-slot target facade. Contact polling returns the oldest queued melee,
arrow, boomerang, hookshot, or bomb overlap.

### Textures and audio

```c
OoTResult oot_engine_texture_count(OoTEngine *engine, uint32_t *outCount);
OoTResult oot_engine_texture_get(OoTEngine *engine, uint32_t index, OoTEngineTexture *outTexture);
OoTResult oot_engine_voice_get(OoTEngine *engine, uint16_t sfxId, OoTEnginePcm *outPcm);
OoTResult oot_engine_ocarina_note_get(OoTEngine *engine, uint8_t noteIndex, OoTEnginePcm *outPcm);
OoTResult oot_engine_audio_sequence_prewarm(OoTEngine *engine, uint16_t sequenceId);
OoTResult oot_engine_audio_sequence_play(OoTEngine *engine, uint8_t player,
                                         uint16_t sequenceId, uint16_t fadeInMs);
OoTResult oot_engine_audio_nature_play(OoTEngine *engine, uint8_t player,
                                      uint8_t ambienceId, uint16_t fadeInMs);
OoTResult oot_engine_audio_sequence_stop(OoTEngine *engine, uint8_t player,
                                         uint16_t fadeOutMs);
OoTResult oot_engine_audio_sequence_pause(OoTEngine *engine, uint8_t player,
                                          uint8_t paused);
OoTResult oot_engine_audio_sequence_set_volume(OoTEngine *engine, uint8_t player,
                                               float volume);
OoTResult oot_engine_audio_sequence_set_io(OoTEngine *engine, uint8_t player,
                                           uint8_t port, int8_t value);
OoTResult oot_engine_audio_channel_set_io(OoTEngine *engine, uint8_t player,
                                          uint8_t channel, uint8_t port,
                                          int8_t value);
OoTResult oot_engine_audio_sequence_get_state(OoTEngine *engine, uint8_t player,
                                              struct OoTAudioState *outState);
OoTResult oot_engine_audio_set_master_volume(OoTEngine *engine, float volume);
OoTResult oot_engine_audio_stop_all(OoTEngine *engine, uint16_t fadeOutMs);
OoTResult oot_engine_audio_render_s16(OoTEngine *engine, int16_t *stereo,
                                     uint32_t frames, uint32_t sampleRate,
                                     uint32_t *outFrames);
OoTResult oot_engine_audio_render_f32(OoTEngine *engine, float *stereo,
                                     uint32_t frames, uint32_t sampleRate,
                                     uint32_t *outFrames);
OoTResult oot_engine_audio_sfx_play(OoTEngine *engine, uint16_t sfxId,
                                    float pan, float volume);
OoTResult oot_engine_audio_sfx_stop(OoTEngine *engine, uint16_t sfxId);
OoTResult oot_engine_audio_sfx_stop_all(OoTEngine *engine);
OoTResult oot_engine_set_enemy_bgm(OoTEngine *engine, uint8_t enabled); /* opt-in proximity battle BGM */
OoTResult oot_engine_get_enemy_bgm(OoTEngine *engine, uint8_t *outEnabled,
                                   uint8_t *outActive, float *outDistance);
```

Immutable sequence/SFX catalogs and the Ocarina song table remain on the
low-level API. Raw mutable audio calls have no engine selector and must not be
used as controls for an `OoTEngine`.

`oot_engine_audio_sfx_play` uses pan `-1.0` for full left, `0.0` for center,
and `1.0` for full right; volume is `0.0..1.0`.

---

# Low-level API (`liboot.h`)

Process-global, single-instance. Link, targets, host actors, and dynamic
collision use integer handles. New integrations should prefer the engine API
and use this surface only when compatibility with existing raw callers matters.

## Low-level constants

```c
#define LIBOOT_VERSION_MAJOR 0
#define LIBOOT_VERSION_MINOR 8
#define LIBOOT_VERSION_PATCH 0
#define LIBOOT_VERSION_STRING "0.8.0"

#define OOT_AUDIO_SEQUENCE_COUNT 110
#define OOT_AUDIO_NO_MUSIC       0x7F   /* scene-header NO_MUSIC sentinel */
#define OOT_AUDIO_NATURE_RAIN    0x80
#define OOT_AUDIO_NATURE_COUNT   19
#define OOT_AUDIO_NATURE_NONE    0x13
#define OOT_GEO_MAX_TRIANGLES    2048   /* legacy/default Link capacity */
#define OOT_GEO_MAX_CONFIGURABLE_TRIANGLES 1048576
#define OOT_TEXTURE_MAX_COUNT    1024   /* decoded texture slots */
#define OOT_SKELETON_MAX_JOINTS  21
#define OOT_SCENE_MAX_TRIANGLES  16384  /* default scene capacity */

#define OOT_DYNAMIC_COLLISION_MAX_OBJECTS  50
#define OOT_DYNAMIC_COLLISION_MAX_SURFACES 512
#define OOT_DYNAMIC_COLLISION_MAX_VERTICES 512
#define OOT_DYNAMIC_COLLISION_INVALID      0
#define OOT_HOST_ACTOR_MAX                 64
```

Also: `OOT_SEQUENCE_INFO_VERSION`, `OOT_AUDIO_STATE_VERSION`,
`OOT_SFX_INFO_VERSION`, `OOT_SCENE_RUNTIME_VERSION`,
`OOT_HOST_ACTOR_STATE_VERSION`, `OOT_HOST_ACTOR_CONTACT_VERSION`,
`OOT_SCENE_BACKGROUND_VERSION`, `OOT_SCENE_MATERIAL_STATE_VERSION`,
`OOT_SCENE_MATERIAL_REFERENCE_VERSION`, `OOT_SCENE_ACTOR_ENTRY_VERSION`, and
`OOT_WORLD_EVENT_VERSION` are all `1`. The header also provides
`OOT_DYNAMIC_COLLISION_TRANSFORM_INIT` and `OOT_SCENE_LOAD_OPTIONS_INIT`, plus
the legacy `OOT_TEXTURE_WIDTH`/`OOT_TEXTURE_HEIGHT` (`1024`, retained for
source compatibility).

Callback types:

```c
typedef void (*OoTSfxCallback)(uint16_t sfxId);                    /* id only */
typedef void (*OoTSfxCallbackEx)(const struct OoTSfxEvent *event); /* full event */
typedef void (*OoTDebugPrintFunctionPtr)(const char *);
```

## Low-level enums

```c
enum OoTAge    { OOT_AGE_ADULT = 0, OOT_AGE_CHILD = 1 };
enum OoTSword  { OOT_SWORD_NONE = 0, OOT_SWORD_KOKIRI, OOT_SWORD_MASTER, OOT_SWORD_BIGGORON };
enum OoTShield { OOT_SHIELD_NONE = 0, OOT_SHIELD_DEKU, OOT_SHIELD_HYLIAN, OOT_SHIELD_MIRROR };
enum OoTTunic  { OOT_TUNIC_KOKIRI = 0, OOT_TUNIC_GORON, OOT_TUNIC_ZORA };
enum OoTBoots  { OOT_BOOTS_KOKIRI = 0, OOT_BOOTS_IRON, OOT_BOOTS_HOVER };

enum OoTItem {                 /* oot_link_use_item; obeys age rules */
    OOT_ITEM_NONE = 0,
    OOT_ITEM_OCARINA,
    OOT_ITEM_BOTTLE,
    OOT_ITEM_HAMMER,           /* adult */
    OOT_ITEM_DEKU_STICK,       /* child */
    OOT_ITEM_BOOMERANG,        /* child */
    OOT_ITEM_BOW,              /* adult */
    OOT_ITEM_HOOKSHOT,         /* adult */
    OOT_ITEM_BOMB              /* both */
};

enum OoTSurfaceType {          /* OoTSurface.type preset; 0 = default */
    OOT_SURFACE_DEFAULT = 0,   /* hookshot-attachable */
    OOT_SURFACE_SAND, OOT_SURFACE_GRASS, OOT_SURFACE_STONE,
    OOT_SURFACE_DAMAGE,        /* hurts on contact */
    OOT_SURFACE_SLIPPERY,      /* Link slides */
    OOT_SURFACE_CLIMB_WALL, OOT_SURFACE_CONVEYOR,
    OOT_SURFACE_NO_HOOKSHOT,   /* solid, not hookshot-attachable */
    OOT_SURFACE_PRESET_COUNT
};

enum OoTTriangleFlags {        /* one uint8_t per triangle */
    OOT_TRI_CULL_FRONT = 1u << 0,
    OOT_TRI_CULL_BACK  = 1u << 1,
    OOT_TRI_ALPHA_TEST = 1u << 2,  /* alpha-compare cutout */
    OOT_TRI_DECAL      = 1u << 3   /* depth-bias decal */
};

enum OoTGeometrySourceKind {
    OOT_GEOMETRY_SOURCE_LINK = 0, OOT_GEOMETRY_SOURCE_NAVI,
    OOT_GEOMETRY_SOURCE_ACTOR, OOT_GEOMETRY_SOURCE_SCENE_ROOM
};
enum OoTGeometryRenderPass {
    OOT_GEOMETRY_PASS_OPAQUE = 0, OOT_GEOMETRY_PASS_TRANSLUCENT
};
enum OoTGeometryBlendMode {
    OOT_GEOMETRY_BLEND_OPAQUE = 0, OOT_GEOMETRY_BLEND_ALPHA,
    OOT_GEOMETRY_BLEND_CUSTOM
};
enum OoTGeometryDepthMode {
    OOT_GEOMETRY_DEPTH_OPAQUE = 0, OOT_GEOMETRY_DEPTH_INTERPENETRATING,
    OOT_GEOMETRY_DEPTH_TRANSLUCENT, OOT_GEOMETRY_DEPTH_DECAL
};
enum OoTGeometryDepthFlags {
    OOT_GEOMETRY_DEPTH_TEST = 1u << 0,
    OOT_GEOMETRY_DEPTH_WRITE = 1u << 1
};

enum OoTDynamicCollisionFlags {
    OOT_DYNAMIC_COLLISION_CARRY_POSITION = 1u << 0,
    OOT_DYNAMIC_COLLISION_CARRY_ROTATION_Y = 1u << 1
};

enum OoTHostActorFlags {
    OOT_HOST_ACTOR_ENABLED = 1u << 0, OOT_HOST_ACTOR_TARGETABLE = 1u << 1,
    OOT_HOST_ACTOR_HOSTILE = 1u << 2, OOT_HOST_ACTOR_HURT = 1u << 3
};
enum OoTHostActorContactSource {
    OOT_HOST_CONTACT_MELEE = 1, OOT_HOST_CONTACT_ARROW,
    OOT_HOST_CONTACT_BOOMERANG, OOT_HOST_CONTACT_HOOKSHOT,
    OOT_HOST_CONTACT_BOMB
};

enum OoTSceneLayer {
    OOT_SCENE_LAYER_CHILD_DAY = 0, OOT_SCENE_LAYER_CHILD_NIGHT,
    OOT_SCENE_LAYER_ADULT_DAY, OOT_SCENE_LAYER_ADULT_NIGHT
};
enum OoTSceneBackgroundEncoding {
    OOT_SCENE_BACKGROUND_NONE = 0, OOT_SCENE_BACKGROUND_RAW,
    OOT_SCENE_BACKGROUND_JPEG
};
enum OoTSceneMaterialReferenceKind {
    OOT_SCENE_MATERIAL_DISPLAY_LIST = 1,
    OOT_SCENE_MATERIAL_TEXTURE_IMAGE,
    OOT_SCENE_MATERIAL_VERTEX_DATA,
    OOT_SCENE_MATERIAL_MATRIX
};
enum OoTSceneActorSource {
    OOT_SCENE_ACTOR_SOURCE_SCENE = 0, OOT_SCENE_ACTOR_SOURCE_ROOM
};
enum OoTWorldEventKind {
    OOT_WORLD_EVENT_SCENE_EXIT = 1,
    OOT_WORLD_EVENT_VOID_SURFACE,
    OOT_WORLD_EVENT_VOID_Y
};

enum OoTAudioPlayer {
    OOT_AUDIO_PLAYER_MAIN = 0,     /* main BGM */
    OOT_AUDIO_PLAYER_FANFARE,
    OOT_AUDIO_PLAYER_SFX,
    OOT_AUDIO_PLAYER_SUB,          /* secondary BGM */
    OOT_AUDIO_PLAYER_COUNT
};

enum OoTOcarinaSong {          /* first six are warp songs */
    OOT_SONG_MINUET = 0, OOT_SONG_BOLERO, OOT_SONG_SERENADE,
    OOT_SONG_REQUIEM, OOT_SONG_NOCTURNE, OOT_SONG_PRELUDE,
    OOT_SONG_SARIAS, OOT_SONG_EPONAS, OOT_SONG_LULLABY,
    OOT_SONG_SUNS, OOT_SONG_TIME, OOT_SONG_STORMS, OOT_SONG_COUNT
};
```

`enum OoTAction` (reported in `OoTLinkState.action`) labels a curated set of
Player actions — `OOT_ACTION_IDLE`, `OOT_ACTION_ROLL`, `OOT_ACTION_TALK`,
`OOT_ACTION_HOOKSHOT_FLY`, `OOT_ACTION_SLIDE_ON_SLOPE`, the cutscene/warp-arrival
actions, and others; anything unlabeled reports `OOT_ACTION_OTHER` (`0`).

`enum OoTSceneIndex` names every retail scene from `0x00`
(`OOT_SCENE_DEKU_TREE`) to `0x64` (`OOT_SCENE_OUTSIDE_GANONS_CASTLE`) — dungeons
and boss rooms (`0x00`–`0x1A`, `0x4F`), interiors and shops (`0x1B`–`0x50`), and
overworld areas (`0x51`–`0x64`). See the header for the full list.

## Low-level structs

### Collision and world

```c
struct OoTSurface {            /* sizeof == 40 */
    uint16_t type;             /* enum OoTSurfaceType */
    int32_t vertices[3][3];    /* integer, y-up, OoT world units */
};

struct OoTWaterBox {           /* sizeof == 10; axis-aligned, extends downward */
    int16_t xMin, zMin;
    int16_t xLength, zLength;
    int16_t ySurface;
};

typedef uint32_t OoTDynamicCollision;

struct OoTDynamicCollisionTransform { /* sizeof == 36 */
    uint32_t structSize;
    float position[3];
    int16_t rotation[3];       /* binary angles, Y-X-Z application order */
    uint16_t reserved;
    float scale[3];
};

struct OoTDynamicCollisionState {     /* sizeof == 48 */
    uint32_t structSize;
    struct OoTDynamicCollisionTransform transform;
    int32_t nativeBgId;        /* diagnostic; can change after a world load */
    uint8_t enabled, playerOnTop, playerAbove, actorOnTop;
};

struct OoTSurfaceInfo {        /* oot_scene_query_surface out; any field pointer may be NULL */
    float groundY;
    uint32_t floorType;        /* FLOOR_TYPE_* */
    uint32_t material;         /* SURFACE_MATERIAL_* */
    uint8_t hookshot;
};
```

Dynamic geometry is local-space `OoTSurface` data. All objects share the
retail 50-object, 512-surface, and 512-unique-vertex DynaPoly budgets. Valid
handles survive static-world and ROM-scene replacement and are rebound to the
new collision context. Carry flags move Link with a translated or yaw-rotated
platform. Initialize transforms with `OOT_DYNAMIC_COLLISION_TRANSFORM_INIT`:
create and set-transform require the full `structSize`, finite values, and a
nonzero scale on every axis. Before get-state, set only
`outState.structSize = sizeof(outState)`; this record has no version field.

### Input, state, geometry

```c
struct OoTLinkInputs {
    float camLookX, camLookZ;  /* unit vector camera->Link, XZ */
    float stickX, stickY;      /* [-1,1] */
    uint8_t buttonA, buttonB, buttonZ, buttonR;
    uint8_t buttonItem;        /* hold to draw/aim */
    uint8_t buttonCUp;         /* first-person / talk to Navi */
};

struct OoTLinkState {          /* NOTE: different layout from OoTEngineLinkState */
    float position[3];
    float velocity[3];
    int16_t faceAngle;         /* binary angle, y */
    float linearVelocity;
    int16_t health;            /* quarter-hearts (16 = 1 heart) */
    int16_t healthCapacity;
    int16_t magic;
    uint8_t magicLevel;
    uint8_t age;               /* enum OoTAge */
    uint8_t isDead;
    int8_t  heldItemAction;    /* PLAYER_IA_*, -1 none */
    uint8_t meleeWeaponState;
    uint32_t action;           /* enum OoTAction */
    int16_t animId;            /* stable 1-based link_animetion entry; 0 unknown */
    float animFrame;
    uint32_t stateFlags1;      /* bit 27 (0x08000000) = swimming */
    uint32_t stateFlags2;
    uint8_t lockOnActive;
    float lockOnPos[3];
    uint8_t inWater;
    float waterSurfaceY;
    uint8_t attackAnim;        /* PLAYER_MWA_* */
    uint8_t stateFlags3;
    int16_t lookPitch, lookYaw;
    uint16_t floorSfxOffset;
    uint16_t underwaterTimer;  /* 0..300; liboot never forces drowning */
};

struct OoTLinkGeometryBuffers {  /* caller-allocated, filled per tick */
    float *position;
    float *normal;
    float *color;
    float *uv;
    uint16_t *triTexture;      /* 0xFFFF untextured; may be NULL */
    uint16_t numTrianglesUsed;
    float *alpha;              /* optional, 1 float/vertex; NULL-safe */
    uint8_t *triFlags;         /* optional, enum OoTTriangleFlags; NULL-safe */
    uint32_t triangleCapacity; /* zero selects OOT_GEO_MAX_TRIANGLES */
    uint32_t numTrianglesUsed32; /* authoritative above UINT16_MAX */
    struct OoTGeometryBatch *batches; /* optional */
    uint32_t batchCapacity;
    uint32_t numBatchesUsed;
};

struct OoTGeometryBatch {      /* sizeof == 96 */
    uint32_t firstTriangle, numTriangles;
    uint64_t sourceInstance;   /* opaque token; never dereference */
    uint32_t materialKey;      /* stable only within this process */
    uint32_t renderMode;
    uint16_t textureIndex;
    int16_t sourceId;
    uint8_t sourceKind, renderPass, blendMode, depthMode;
    uint8_t triangleFlags, depthFlags, reserved[2];
    uint32_t combineModeHi, combineModeLo;
    float primitiveColor[4], environmentColor[4], baseColor[3];
    float reservedColor;
    uint32_t reservedTail;
};
```

Legacy `oot_link_tick` uses `OOT_GEO_MAX_TRIANGLES`. With
`oot_link_tick_sized`, allocate the vertex arrays for
`triangleCapacity * 3` vertices, up to
`OOT_GEO_MAX_CONFIGURABLE_TRIANGLES`; positions, normals, and colors use three
floats per vertex and UVs use two. Batch storage is optional and needs at most
one slot per triangle.

`oot_link_get_geometry_dropped_triangles()` returns the exact number of valid
triangles omitted by the most recent Link/Navi/actor walk. The corresponding
scene query is `oot_scene_get_geometry_dropped_triangles()`; the two counters
are independent.

### Host actors and contacts

```c
struct OoTHostActorState {     /* sizeof == 72 */
    uint32_t structSize, version;
    uint64_t userTag;
    uint32_t typeId, flags;
    float position[3], focusOffset[3];
    float hurtRadius, hurtHeight, hurtYOffset;
    int16_t rotation[3];
    int16_t room;              /* -1 persists; otherwise 0..127 */
    uint8_t attentionRange;    /* native AttentionRangeType, 0..9 */
    uint8_t reserved[3];
};

struct OoTHostActorContact {   /* sizeof == 48 */
    uint32_t structSize, version;
    int32_t actorId;
    uint32_t source;
    uint64_t userTag;
    uint32_t sourceActorId, gameplayFrame;
    float position[3];
    uint32_t reserved;
};
```

Host actors join the native actor and attention lists, but the host owns their
AI, health, animation, and rendering. A live Link is required before creation.
`userTag` is caller-supplied identity echoed in contacts. Attack contacts are
edge-triggered and removed from the FIFO when polled.

### Textures, skeleton, actors, doors

```c
struct OoTTextureInfo {
    uint16_t width, height;
    uint8_t wrapS, wrapT;      /* 0 repeat, 1 mirror, 2 clamp */
    uint32_t revision;         /* re-upload when changed */
};

struct OoTSkeletonPose {       /* sizeof == 276 */
    uint8_t numJoints;
    uint8_t parent[OOT_SKELETON_MAX_JOINTS];   /* 0xFF = root */
    float jointPos[OOT_SKELETON_MAX_JOINTS][3];
};

struct OoTActorInfo {          /* sizeof == 24 */
    int16_t id;                /* EN_BOM=0x10, EN_ARROW=0x16, EN_ELF=0x18, EN_BOOM=0x32, ARMS_HOOK=0x66 */
    int16_t category;          /* ACTORCAT_* */
    int16_t params;
    int16_t yaw;               /* shape.rot.y, binary angle */
    uint8_t active;            /* 0 for an actor that died this tick */
    float pos[3];
};

struct OoTDoor {
    int16_t frontRoom;         /* side 0 room index */
    int16_t backRoom;          /* side 1 room index */
    int16_t actorId;
    int16_t yaw;
    float pos[3];
};
```

### Audio and scene metadata

```c
struct OoTSfxEvent {           /* sizeof == 28; oot_set_sfx_callback_ex */
    uint16_t sfxId;
    uint8_t token;
    int8_t reverb;
    uint8_t action;            /* OOT_SFX_PLAY / STOP_ID / STOP_POSITION */
    uint8_t isRefresh;
    uint8_t reserved[2];
    float position[3];
    float freqScale;
    float volume;
};

struct OoTSceneEnvironment {   /* active light/fog; already baked into vertex color */
    float ambientColor[3];
    float light1Dir[3], light1Color[3];
    float light2Dir[3], light2Color[3];
    float fogColor[3];
    float fogNear, fogFar;
    uint8_t valid;
};

struct OoTSceneRuntime {       /* sizeof == 36; size/version tagged */
    uint32_t structSize, version;
    int32_t sceneIndex, activeRoomIndex, geometryRoomIndex, roomCount;
    int16_t worldMapArea;
    uint8_t roomType, environmentType;
    int8_t echo;
    uint8_t lensMode, warpSongsDisabled, sceneCamType, allRoomsLoaded, roomMetadataValid;
    uint8_t reserved[2];
};
```

Layered loading and host-owned scene catalogs use these records:

```c
struct OoTSceneLoadOptions {   /* sizeof == 16 */
    uint32_t structSize;
    int32_t sceneIndex, roomIndex;
    uint8_t layer;             /* enum OoTSceneLayer */
    uint8_t reserved[3];
};

struct OoTSceneBackground {    /* sizeof == 72 on 64-bit */
    uint32_t structSize, version;
    int32_t roomIndex;
    int16_t cameraIndex;
    uint8_t amountType, encoding;
    uint16_t width, height;
    uint8_t format, size;
    uint16_t tlutMode, tlutCount, entryFlags;
    uint32_t sourceAddress, tlutAddress, sourceMetadata;
    const uint8_t *imageBytes;
    size_t imageByteCount;
    const uint8_t *tlutBytes;
    size_t tlutByteCount;
};

struct OoTSceneMaterialState { /* sizeof == 32 */
    uint32_t structSize, version;
    uint64_t simulationFrame;
    uint32_t referenceCount;
    uint16_t segmentMask;
    uint8_t drawConfigId, referencesTruncated;
    uint8_t reserved[8];
};

struct OoTSceneMaterialReference { /* sizeof == 28 */
    uint32_t structSize, version;
    uint32_t segmentedAddress, firstTriangle, numTriangles;
    int16_t roomIndex;
    uint8_t segment, kind, renderPass, reserved[3];
};

struct OoTSceneActorEntry {    /* sizeof == 36 */
    uint32_t structSize, version;
    int16_t actorId, params;
    float position[3];
    int16_t rotation[3], room;
    uint8_t source, layer;
    uint16_t reserved;
};

struct OoTWorldEvent {         /* sizeof == 48 */
    uint32_t structSize, version;
    uint64_t sequence;
    uint32_t kind;
    int32_t sceneIndex, roomIndex;
    int16_t exitIndex, entranceIndex;
    uint8_t floorProperty, layer;
    uint16_t reserved;
    float position[3];
};
```

Use `OOT_SCENE_LOAD_OPTIONS_INIT(scene, room, layer)`, or set the complete
`structSize` and a layer from `OOT_SCENE_LAYER_CHILD_DAY` through
`OOT_SCENE_LAYER_ADULT_NIGHT` manually.

JPEG backgrounds expose the validated encoded byte stream; liboot does not
decode or composite it. Material references identify unresolved segment data
for the host renderer; liboot does not evaluate draw-config animations.
The background, material, scene-actor, world-event, and host-actor getters or
polls validate their output record's full `structSize` and documented
`version`; host-actor create/update validate the same tags on input. The engine
actor-contact poll has the same rule. An invalid contact or world-event output
does not consume the queued record. `oot_scene_get_runtime` is different: it
initializes and overwrites its own tags.

`struct OoTSequenceInfo`, `struct OoTAudioState`, and `struct OoTSfxInfo`
(each `structSize`/`version` tagged) carry sequence metadata, live player state,
and SFX-catalog entries with their symbolic `NA_SE_*` names. See the header for
their fields.

## Low-level functions

### Lifecycle

```c
void oot_global_init(const uint8_t *rom, size_t romSize, uint8_t *outTexture); /* outTexture: pass NULL */
void oot_global_terminate(void);
void oot_set_debug_print_function(OoTDebugPrintFunctionPtr fn);
```

### World, scene, and surface query

```c
void    oot_static_surfaces_load(const struct OoTSurface *surfaces, uint32_t numSurfaces);
void    oot_static_world_load(const struct OoTSurface *surfaces, uint32_t numSurfaces,
                              const struct OoTWaterBox *waterBoxes, uint32_t numWaterBoxes);
int32_t oot_dynamic_collision_create(
            const struct OoTSurface *surfaces, uint32_t numSurfaces,
            const struct OoTDynamicCollisionTransform *transform, uint32_t flags,
            OoTDynamicCollision *outHandle);
bool    oot_dynamic_collision_set_transform(
            OoTDynamicCollision handle,
            const struct OoTDynamicCollisionTransform *transform);
bool    oot_dynamic_collision_set_enabled(OoTDynamicCollision handle, bool enabled);
bool    oot_dynamic_collision_get_state(OoTDynamicCollision handle,
                                         struct OoTDynamicCollisionState *outState);
bool    oot_dynamic_collision_delete(OoTDynamicCollision handle);
bool    oot_scene_query_surface(float x, float y, float z, struct OoTSurfaceInfo *outInfo);
bool    oot_scene_set_geometry_capacity(uint32_t triangleCapacity);
int32_t oot_scene_load_ex(const struct OoTSceneLoadOptions *options);
int32_t oot_scene_load(int32_t sceneIndex, int32_t roomIndex); /* child-day */
bool    oot_scene_get_active_layer(uint8_t *outLayer, bool *outUsedFallback);
int32_t oot_scene_set_room(int32_t roomIndex);
int32_t oot_scene_get_actor_count(void);
bool    oot_scene_get_actor(int32_t index, struct OoTSceneActorEntry *outEntry);
int32_t oot_scene_get_exit_count(void);
bool    oot_scene_get_exit(int32_t index, int16_t *outEntranceIndex);
bool    oot_world_event_poll(struct OoTWorldEvent *outEvent);
int32_t oot_scene_get_background_count(void);
bool    oot_scene_get_background(int32_t index,
                                 struct OoTSceneBackground *outBackground);
bool    oot_scene_get_material_state(struct OoTSceneMaterialState *outState);
bool    oot_scene_get_material_reference(
            int32_t index, struct OoTSceneMaterialReference *outReference);
int32_t oot_scene_get_door_count(void);
bool    oot_scene_get_door(int32_t index, struct OoTDoor *outDoor);
int32_t oot_scene_get_sequence_id(void);   /* -1 if none */
int32_t oot_scene_get_ambience_id(void);   /* -1 if none */
bool    oot_scene_get_environment(struct OoTSceneEnvironment *out);
bool    oot_scene_get_runtime(struct OoTSceneRuntime *out);
bool    oot_scene_get_geometry(const float **position, const float **normal,
                               const float **color, const float **uv,
                               const uint16_t **triTexture,
                               uint32_t *numTriangles, uint32_t *xluStartTriangle);
bool    oot_scene_get_geometry_batches(const struct OoTGeometryBatch **batches,
                                        uint32_t *numBatches);
uint32_t oot_scene_get_geometry_dropped_triangles(void);
bool    oot_scene_get_triangle_flags(const uint8_t **outFlags);
bool    oot_scene_spawn(int32_t spawnIndex, float outPos[3], int16_t *outYaw);
```

`oot_scene_load` selects child-day. `oot_scene_load_ex` selects child/adult and
day/night effective headers; `outUsedFallback` reports when an alternate header
was absent. Cutscene headers are outside this selector. Both loaders replace a
static world; a negative result other than `-9` leaves the previous world live
(`-9` commits collision but the room mesh is unavailable). `roomIndex == -1`
concatenates all rooms. Call `oot_scene_set_geometry_capacity` before a ROM
scene is active; zero restores the 16,384-triangle default.

Scene actor entries are a spawn catalog, not running overlays. Exit getter
indices are zero-based; a world event preserves the surface's raw 1-based exit
index. `oot_world_event_poll` removes the oldest event and does not dequeue it
when the caller supplies invalid size/version tags.

### Link

```c
int32_t oot_link_create(float x, float y, float z);   /* <0 on failure; load a world first */
void    oot_link_delete(int32_t linkId);
void    oot_link_tick(int32_t linkId, const struct OoTLinkInputs *inputs,
                      struct OoTLinkState *outState, struct OoTLinkGeometryBuffers *outBuffers);
void    oot_link_tick_sized(int32_t linkId, const struct OoTLinkInputs *inputs,
                            struct OoTLinkState *outState,
                            struct OoTLinkGeometryBuffers *outBuffers,
                            uint32_t geometryBuffersSize);
uint32_t oot_link_get_geometry_dropped_triangles(void);
bool    oot_link_set_pose(int32_t linkId, float x, float y, float z, int16_t yaw);
void    oot_link_freeze(int32_t linkId, bool frozen);
void    oot_link_set_invincible(int32_t linkId, int8_t frames);
bool    oot_link_set_age(int32_t linkId, uint8_t age);   /* false if ROM lacks the object */
void    oot_link_set_equipment(int32_t linkId, uint8_t sword, uint8_t shield,
                               uint8_t tunic, uint8_t boots);
void    oot_link_set_health(int32_t linkId, int16_t health, int16_t capacity);
void    oot_link_damage(int32_t linkId, int16_t amount);  /* real Player_InflictDamage */
void    oot_link_set_magic(int32_t linkId, uint8_t level, int16_t amount);
void    oot_link_use_item(int32_t linkId, uint8_t item);
bool    oot_link_get_skeleton(int32_t linkId, struct OoTSkeletonPose *out);
```

### Targets, host actors, retail helpers, and Navi

```c
int32_t oot_target_create(float x, float y, float z, float radius); /* radius = focus height; pool 16, -1 if full */
void    oot_target_move(int32_t targetId, float x, float y, float z);
void    oot_target_remove(int32_t targetId);
int32_t oot_host_actor_create(const struct OoTHostActorState *state);
bool    oot_host_actor_update(int32_t actorId, const struct OoTHostActorState *state);
bool    oot_host_actor_get(int32_t actorId, struct OoTHostActorState *outState);
bool    oot_host_actor_remove(int32_t actorId);
void    oot_host_actor_clear(void);
bool    oot_host_actor_poll_contact(struct OoTHostActorContact *outContact);
int32_t oot_actor_query(struct OoTActorInfo *out, int32_t maxCount);
void    oot_actor_set_render(bool enabled);
int32_t oot_actor_spawn(int16_t actorId, float x, float y, float z,
                        int16_t rotX, int16_t rotY, int16_t rotZ, int16_t params); /* currently ACTOR_EN_BOM only */
bool    oot_navi_get(float outPos[3], float outInnerColor[4], float outOuterColor[4], float *outScale);
void    oot_navi_set_render(bool enabled);
```

### Textures and voice

```c
int32_t oot_get_texture_count(void);
bool    oot_get_texture(int32_t index, struct OoTTextureInfo *info, const uint8_t **rgbaPixels);
void    oot_set_sfx_callback(OoTSfxCallback cb);
void    oot_set_sfx_callback_ex(OoTSfxCallbackEx cb);
bool    oot_get_voice_sample(uint16_t sfxId, const int16_t **pcm,
                             uint32_t *numSamples, uint32_t *sampleRate);
bool    oot_get_ocarina_note(uint8_t noteIndex, const int16_t **pcm,
                             uint32_t *numSamples, uint32_t *sampleRate, uint32_t *loopStart);
```

### Audio sequences and SFX

```c
int32_t     oot_audio_sequence_count(void);
const char *oot_audio_sequence_name(uint16_t sequenceId);
bool        oot_audio_sequence_get_info(uint16_t sequenceId, struct OoTSequenceInfo *outInfo);
bool        oot_audio_sequence_prewarm(uint16_t sequenceId);   /* may allocate; not in the audio callback */
bool        oot_audio_sequence_play(uint8_t player, uint16_t sequenceId, uint16_t fadeInMs);
bool        oot_audio_nature_play(uint8_t player, uint8_t ambienceId, uint16_t fadeInMs);
void        oot_audio_sequence_stop(uint8_t player, uint16_t fadeOutMs);
void        oot_audio_sequence_pause(uint8_t player, bool paused);
void        oot_audio_sequence_set_volume(uint8_t player, float volume);
void        oot_audio_sequence_set_io(uint8_t player, uint8_t port, int8_t value);
void        oot_audio_channel_set_io(uint8_t player, uint8_t channel, uint8_t port, int8_t value);
bool        oot_audio_sequence_get_state(uint8_t player, struct OoTAudioState *outState);
void        oot_audio_set_master_volume(float volume);
void        oot_audio_stop_all(uint16_t fadeOutMs);
uint32_t    oot_audio_render_s16(int16_t *stereo, uint32_t frames, uint32_t sampleRate); /* canonical interleaved stereo S16 */
uint32_t    oot_audio_render_f32(float *stereo, uint32_t frames, uint32_t sampleRate); /* interleaved stereo F32; 8..192 kHz */
int32_t     oot_audio_sfx_catalog_count(void);
bool        oot_audio_sfx_catalog_get(int32_t catalogIndex, struct OoTSfxInfo *outInfo);
bool        oot_audio_sfx_play(uint16_t sfxId, float pan, float volume);
void        oot_audio_sfx_stop(uint16_t sfxId);
void        oot_audio_sfx_stop_all(void);

/* Opt-in proximity battle BGM (disabled by default). Plays NA_BGM_ENEMY while a
   hostile enemy is within OoT's 500-unit battle range of Link. player 0xFF keeps
   the default (OOT_AUDIO_PLAYER_SUB); seqId 0 keeps NA_BGM_ENEMY (0x1A). */
bool        oot_audio_set_enemy_bgm(bool enabled, uint8_t player, uint16_t seqId, uint16_t fadeMs);
bool        oot_audio_get_enemy_bgm(bool *outActive, float *outDistance); /* returns enabled */
```

Both render calls overwrite rather than accumulate and allocate nothing. F32
output is an exact `s16 / 32768.0f` conversion of the canonical fixed-point S16
stream. Serialize every mutable AudioSeq call, including `get_state`, against
the raw audio callback.

With more than one `OoTEngine`, use the handle-taking
`oot_engine_audio_*` control and S16/F32 render functions. The raw mutable
`oot_audio_*` API has no context selector and therefore must not be used while
multiple engine handles are live; it acts on whichever native context was most
recently activated by an engine call. Immutable catalog/name/info queries are
the exception.

### Ocarina songs

```c
bool    oot_ocarina_song_notes(int32_t song, uint8_t outNotes[8], int32_t *outCount);
int32_t oot_ocarina_match(const uint8_t *notes, int32_t count);   /* enum OoTOcarinaSong, or -1 */
```

---

# Notes on the two APIs

- The engine API is the recommended surface; every function returns `OoTResult`.
  The low-level API returns raw `int32_t`/`bool`/`void` and uses a process-wide
  singleton with integer Link, target, host-actor, and dynamic-collision
  handles.
- `oot_engine_target_create`'s `focusHeight` maps to the low-level
  `oot_target_create`'s `radius`: both are the lock-on focus height above the
  base position.
- Several engine structs deliberately mirror a low-level struct with a leading
  `structSize` tag for versioned FFI growth (for example `OoTEngineLinkState`
  vs `OoTLinkState`). Their field orders differ — do not assume identical
  layout.
- `OOT_ENGINE_MAX_STATIC_SURFACES` (2730) and `OOT_ENGINE_MAX_WATER_BOXES`
  (65535) cap `oot_engine_static_world_load`; the low-level
  `oot_static_world_load` silently ignores over-capacity input and keeps the
  previous world live.
