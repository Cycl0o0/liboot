# API reference

Every exported symbol in the two public headers. The engine-neutral API in
[`liboot_engine.h`](../src/liboot_engine.h) is the recommended surface; the
low-level compatibility API in [`liboot.h`](../src/liboot.h) is process-global
and uses integer handles. Both headers are wrapped in `extern "C"` and every
function carries the `OOT_LIB_FN` linkage macro.

For worked examples see [USAGE.md](USAGE.md). The headers themselves remain the
normative source; this document tracks them but the compiler does not.

- [Engine API (`liboot_engine.h`)](#engine-api-liboot_enginehh)
  - [Constants](#engine-constants) · [Results](#ootresult) ·
    [Enums](#engine-enums) · [Structs](#engine-structs) ·
    [Functions](#engine-functions)
- [Low-level API (`liboot.h`)](#low-level-api-liboothh)
  - [Constants](#low-level-constants) · [Enums](#low-level-enums) ·
    [Structs](#low-level-structs) · [Functions](#low-level-functions)
- [Notes on the two APIs](#notes-on-the-two-apis)

---

# Engine API (`liboot_engine.h`)

Only one `OoTEngine` may exist per process. Do not mix this API with the raw
`oot_*` lifecycle calls. Load a static world or ROM scene before creating Link:
native Player init needs a live collision context. Public calls use a
non-blocking guard; concurrent or callback-reentrant calls return
`OOT_ENGINE_RESULT_BUSY`, and callbacks must copy any event data and must not
re-enter the engine.

## Engine constants

| Macro | Value |
| --- | --- |
| `OOT_ENGINE_API_VERSION` | `1` |
| `OOT_ENGINE_DEFAULT_FIXED_STEP` | `1.0f / 20.0f` |
| `OOT_ENGINE_DEFAULT_ACTOR_CAPACITY` | `64` |
| `OOT_ENGINE_DEFAULT_MAX_SUBSTEPS` | `4` |
| `OOT_ENGINE_MAX_ACTOR_CAPACITY` | `4096` |
| `OOT_ENGINE_MAX_STATIC_SURFACES` | `2730` |
| `OOT_ENGINE_MAX_WATER_BOXES` | `65535` |
| `OOT_ENGINE_MIN_ROM_SIZE` | `0x1060` |
| `OOT_ENGINE_MAX_ROM_SIZE` | `256 * 1024 * 1024` |
| `OOT_ENGINE_MAX_SUBSTEPS` | `1000` |
| `OOT_ENGINE_MIN_FIXED_STEP_SECONDS` | `0.001f` |
| `OOT_ENGINE_MAX_FIXED_STEP_SECONDS` | `1.0f` |
| `OOT_ENGINE_INVALID_TARGET` | `0` |

Opaque types and callbacks:

```c
typedef struct OoTEngine OoTEngine;
typedef uint32_t OoTEngineTarget;
typedef void (*OoTEngineDebugCallback)(void *userData, const char *message);
typedef void (*OoTEngineSfxCallback)(void *userData, const struct OoTSfxEvent *event);
```

Native C/C++ callers should use the convenience initializers
`oot_engine_config_init(&config)` and `oot_engine_input_init(&input)` (macros
over the `_sized` functions). The static initializers `OOT_ENGINE_CONFIG_INIT`
and `OOT_ENGINE_INPUT_INIT` are also provided.

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

### OoTEngineConfig

```c
typedef struct OoTEngineConfig {
    uint32_t structSize;
    uint32_t apiVersion;
    const uint8_t *romData;       /* required */
    size_t romSize;               /* required, MIN_ROM_SIZE..MAX_ROM_SIZE */
    uint32_t actorCapacity;       /* 0/default 64; max 4096 */
    uint32_t maxSubsteps;         /* 0/default 4; else 1..1000 */
    float fixedStepSeconds;       /* 0/default 1/20; else 0.001..1 */
    uint32_t renderFlags;         /* enum OoTEngineRenderFlags */
    OoTEngineDebugCallback debugCallback;
    void *debugUserData;
    OoTEngineSfxCallback sfxCallback;
    void *sfxUserData;
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
    uint8_t reserved[2];
    struct OoTSkeletonPose skeleton;
    OoTEngineNaviState navi;
} OoTEngineFrame;               /* sizeof == 544 on 64-bit */
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
} OoTEngineSceneGeometry;
```

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

## Engine functions

### Lifecycle and configuration

```c
uint32_t     oot_engine_api_version(void);
const char  *oot_engine_result_string(OoTResult result);
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
OoTResult oot_engine_scene_load(OoTEngine *engine, int32_t sceneIndex, int32_t roomIndex,
                                int32_t *outNativeResult);       /* roomIndex -1 = whole scene */
OoTResult oot_engine_scene_get_geometry(OoTEngine *engine,
                                        const OoTEngineSceneGeometry **outGeometry);
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
OoTResult oot_engine_scene_query_surface(OoTEngine *engine, float x, float y, float z,
                                         struct OoTSurfaceInfo *outInfo);
```

`scene_get_sequence_id` / `scene_get_ambience_id` return `-1` in their out
parameter when the scene declares no sound settings. `scene_get_runtime` and
`scene_get_environment` return NOT_AVAILABLE/empty after a custom-world load.

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

### Targets and render flags

```c
OoTResult oot_engine_target_create(OoTEngine *engine, float x, float y, float z,
                                   float focusHeight, OoTEngineTarget *outTarget);
OoTResult oot_engine_target_move(OoTEngine *engine, OoTEngineTarget target,
                                 float x, float y, float z);
OoTResult oot_engine_target_remove(OoTEngine *engine, OoTEngineTarget target);
OoTResult oot_engine_targets_clear(OoTEngine *engine);
OoTResult oot_engine_set_render_flags(OoTEngine *engine, uint32_t renderFlags);
OoTResult oot_engine_get_render_flags(OoTEngine *engine, uint32_t *outRenderFlags);
```

### Textures and audio

```c
OoTResult oot_engine_texture_count(OoTEngine *engine, uint32_t *outCount);
OoTResult oot_engine_texture_get(OoTEngine *engine, uint32_t index, OoTEngineTexture *outTexture);
OoTResult oot_engine_voice_get(OoTEngine *engine, uint16_t sfxId, OoTEnginePcm *outPcm);
OoTResult oot_engine_ocarina_note_get(OoTEngine *engine, uint8_t noteIndex, OoTEnginePcm *outPcm);
```

Music playback, the AudioSeq mixer, the SFX catalog, and the Ocarina song table
are on the low-level API and are safe to call while an engine is alive.

---

# Low-level API (`liboot.h`)

Process-global, single-instance. Link and targets are integer handles. New
integrations should prefer the engine API and drop here only for a feature the
wrapper does not surface.

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
#define OOT_GEO_MAX_TRIANGLES    2048   /* per-tick Link geometry cap */
#define OOT_SKELETON_MAX_JOINTS  21
#define OOT_SCENE_MAX_TRIANGLES  16384  /* full multi-room dungeon cap */
```

Also: `OOT_SEQUENCE_INFO_VERSION`, `OOT_AUDIO_STATE_VERSION`,
`OOT_SFX_INFO_VERSION`, `OOT_SCENE_RUNTIME_VERSION` (all `1`), and the legacy
`OOT_TEXTURE_WIDTH`/`OOT_TEXTURE_HEIGHT` (`1024`, retained for source
compatibility).

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

struct OoTSurfaceInfo {        /* oot_scene_query_surface out; any field pointer may be NULL */
    float groundY;
    uint32_t floorType;        /* FLOOR_TYPE_* */
    uint32_t material;         /* SURFACE_MATERIAL_* */
    uint8_t hookshot;
};
```

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
};
```

Allocate `position`/`normal`/`color`/`uv` for `OOT_GEO_MAX_TRIANGLES * 3`
vertices (three floats each, two for `uv`).

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
bool    oot_scene_query_surface(float x, float y, float z, struct OoTSurfaceInfo *outInfo);
int32_t oot_scene_load(int32_t sceneIndex, int32_t roomIndex);   /* 0 ok; <0 error; -1 = -1 room whole scene */
int32_t oot_scene_set_room(int32_t roomIndex);
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
bool    oot_scene_get_triangle_flags(const uint8_t **outFlags);
bool    oot_scene_spawn(int32_t spawnIndex, float outPos[3], int16_t *outYaw);
```

`oot_scene_load` replaces the static world; a negative result other than `-9`
leaves the previous world live (`-9` commits collision but the room mesh is
unavailable). `roomIndex == -1` concatenates all rooms (opaque then translucent).

### Link

```c
int32_t oot_link_create(float x, float y, float z);   /* <0 on failure; load a world first */
void    oot_link_delete(int32_t linkId);
void    oot_link_tick(int32_t linkId, const struct OoTLinkInputs *inputs,
                      struct OoTLinkState *outState, struct OoTLinkGeometryBuffers *outBuffers);
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

### Targets, actors, Navi

```c
int32_t oot_target_create(float x, float y, float z, float radius); /* radius = focus height; pool 16, -1 if full */
void    oot_target_move(int32_t targetId, float x, float y, float z);
void    oot_target_remove(int32_t targetId);
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
uint32_t    oot_audio_render_f32(float *stereo, uint32_t frames, uint32_t sampleRate); /* interleaved stereo F32; 8..192 kHz */
int32_t     oot_audio_sfx_catalog_count(void);
bool        oot_audio_sfx_catalog_get(int32_t catalogIndex, struct OoTSfxInfo *outInfo);
bool        oot_audio_sfx_play(uint16_t sfxId, float pan, float volume);
void        oot_audio_sfx_stop(uint16_t sfxId);
void        oot_audio_sfx_stop_all(void);
```

`oot_audio_render_f32` overwrites (does not accumulate) its buffer and allocates
nothing. Serialize every mutable AudioSeq call, including `get_state`, against
your audio callback.

### Ocarina songs

```c
bool    oot_ocarina_song_notes(int32_t song, uint8_t outNotes[8], int32_t *outCount);
int32_t oot_ocarina_match(const uint8_t *notes, int32_t count);   /* enum OoTOcarinaSong, or -1 */
```

---

# Notes on the two APIs

- The engine API is the recommended surface; every function returns `OoTResult`.
  The low-level API returns raw `int32_t`/`bool`/`void` and uses a process-wide
  singleton with integer `linkId` / `targetId` handles.
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
