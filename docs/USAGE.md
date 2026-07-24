# Usage cookbook

Task-by-task examples against the recommended engine API in
[`liboot_engine.h`](../src/liboot_engine.h). Every snippet uses real function
signatures; error checks are shortened to `require_ok(...)` for readability but
must be kept in production code — no call should be assumed to succeed. A
complete `require_ok` helper appears in
[examples/engine.c](../examples/engine.c).

For build instructions see [GETTING_STARTED.md](GETTING_STARTED.md); for the
full symbol list see [API_REFERENCE.md](API_REFERENCE.md).

## Contents

- [Create and destroy an engine](#create-and-destroy-an-engine)
- [Step the simulation](#step-the-simulation)
- [Equipment, items, age, health, magic](#equipment-items-age-health-magic)
- [Render Link's geometry](#render-links-geometry)
- [Upload textures](#upload-textures)
- [Host collision and water](#host-collision-and-water)
- [Load a ROM scene](#load-a-rom-scene)
- [Rooms and door transitions](#rooms-and-door-transitions)
- [Query the surface under a point](#query-the-surface-under-a-point)
- [Z-targeting](#z-targeting)
- [Actors and Navi](#actors-and-navi)
- [Sound effects and voice](#sound-effects-and-voice)
- [Music and ambience](#music-and-ambience)
- [The Ocarina](#the-ocarina)
- [Reposition and freeze Link](#reposition-and-freeze-link)
- [Error handling](#error-handling)
- [C++ binding](#c-binding)
- [C# and Unity binding](#c-and-unity-binding)
- [Threading rules](#threading-rules)

## Create and destroy an engine

The ROM is copied during `oot_engine_create`, so the caller may free its buffer
as soon as the call returns.

```c
#include "liboot_engine.h"

if (oot_engine_api_version() != OOT_ENGINE_API_VERSION)
    fatal("liboot ABI mismatch: rebuild the host against this header");

OoTEngineConfig config;
require_ok(oot_engine_config_init(&config));   /* fills structSize + defaults */
config.romData     = romBytes;
config.romSize     = romSize;
config.renderFlags = OOT_ENGINE_RENDER_NAVI | OOT_ENGINE_RENDER_ACTORS;

OoTEngine *engine = NULL;
require_ok(oot_engine_create(&config, &engine));
free(romBytes);   /* the ROM was copied synchronously */

/* ... use the engine ... */

require_ok(oot_engine_destroy(engine));   /* also deletes an active Link */
```

`oot_engine_create` returns `OOT_ENGINE_RESULT_SINGLETON_IN_USE` if an engine
already exists in the process, and `OOT_ENGINE_RESULT_ROM_UNSUPPORTED` if the
ROM cannot be parsed. Configuration knobs on `OoTEngineConfig`:

| Field | Default | Range |
| --- | --- | --- |
| `actorCapacity` | 64 | 1..4096 |
| `maxSubsteps` | 4 | 1..1000 |
| `fixedStepSeconds` | 1/20 | 0.001..1.0 |

Setting any of the three to `0` selects its default. A value of
`fixedStepSeconds` other than `0.05` changes gameplay speed.

## Step the simulation

The gameplay core runs at a fixed 20 Hz — one tick every 50 ms. Render at any
rate. Use `oot_engine_step` if your host already owns a 20 Hz loop:

```c
OoTEngineInput input;
require_ok(oot_engine_input_init(&input));
input.stickY   = 1.0f;                 /* full forward on the camera-relative stick */
input.buttons  = OOT_ENGINE_BUTTON_A;  /* jump/roll/context action this tick */
input.camLookX = 0.0f;                 /* camera-to-Link direction in XZ; (0,0) -> +Z */
input.camLookZ = 1.0f;

const OoTEngineFrame *frame = NULL;
require_ok(oot_engine_step(engine, &input, &frame));

printf("tick %llu  pos %.1f %.1f %.1f\n",
       (unsigned long long)frame->simulationTick,
       frame->link.position[0], frame->link.position[1], frame->link.position[2]);
```

Or let the engine accumulate real time and run 0..`maxSubsteps` ticks per call,
which is the right choice when driving from a variable-rate render loop:

```c
void on_render_frame(float elapsedSeconds) {
    OoTEngineInput input;
    oot_engine_input_init(&input);
    sample_host_input(&input);

    uint32_t steps = 0;
    const OoTEngineFrame *frame = NULL;
    if (oot_engine_advance(engine, elapsedSeconds, &input, &steps, &frame)
            == OOT_ENGINE_RESULT_OK && frame) {
        /* interpolationAlpha in [0,1) is how far we are into the next tick. */
        render(frame, frame->interpolationAlpha);
    }
}
```

Buttons are sampled by the original game logic, so a tap must be present for at
least one tick, and hold/release edges matter for the bow, hookshot, boomerang,
bombs, shielding, and chargeable actions. Stick axes are clamped to `[-1, 1]`.
`oot_engine_get_frame` re-reads the last frame without stepping; `frame` and
everything it points to are owned by the engine and valid only until the next
mutating call.

## Equipment, items, age, health, magic

```c
require_ok(oot_engine_link_set_equipment(engine, OOT_SWORD_MASTER,
    OOT_SHIELD_HYLIAN, OOT_TUNIC_KOKIRI, OOT_BOOTS_KOKIRI));

require_ok(oot_engine_link_use_item(engine, OOT_ITEM_BOW));    /* draw the bow */
require_ok(oot_engine_link_set_age(engine, OOT_AGE_CHILD));    /* re-inits on the child skeleton */

require_ok(oot_engine_link_set_health(engine, 16 * 3, 16 * 3));/* 3 hearts, full */
require_ok(oot_engine_link_damage(engine, 8));                 /* half-heart hit via Player_InflictDamage */
require_ok(oot_engine_link_set_magic(engine, 1, 48));          /* magic meter level 1, 48 units */
```

Items obey the original age rules: `OOT_ITEM_BOW`, `OOT_ITEM_HOOKSHOT`, and
`OOT_ITEM_HAMMER` are adult-only; `OOT_ITEM_DEKU_STICK` and `OOT_ITEM_BOOMERANG`
are child-only; a wrong-age request returns `OOT_ENGINE_RESULT_AGE_RESTRICTED`.
Firing a projectile is a hold-then-release input gesture on the item button, not
a single call — see [examples/engine.c](../examples/engine.c) and the playground
for the exact input pattern. Changing age or recreating Link despawns helper
actors and host targets; recreate targets afterward.

## Render Link's geometry

`frame->geometry` is renderer-neutral. Positions, normals, and colors are three
floats per vertex; UVs are two; there are three vertices per triangle:

```c
const OoTEngineGeometry *g = &frame->geometry;
for (uint32_t t = 0; t < g->numTriangles; ++t) {
    for (int v = 0; v < 3; ++v) {
        uint32_t i = (t * 3 + v);
        const float *p = &g->position[i * 3];   /* x, y, z */
        const float *n = &g->normal[i * 3];     /* nx, ny, nz */
        const float *c = &g->color[i * 3];      /* r, g, b */
        const float  a = g->alpha ? g->alpha[i] : 1.0f;
        const float *uv = &g->uv[i * 2];        /* u, v */
        push_vertex(p, n, c, a, uv);
    }
    uint16_t tex = g->triTexture[t];            /* 0xFFFF means untextured */
    uint8_t  flags = g->triFlags ? g->triFlags[t] : 0;  /* cull/alpha-test/decal */
    set_triangle_material(tex, flags);
}
```

Coordinates are right-handed, Y-up, in OoT world units. Navi wings and
projectile meshes append to the same buffer when their render flags are set:

```c
require_ok(oot_engine_set_render_flags(engine,
    OOT_ENGINE_RENDER_NAVI | OOT_ENGINE_RENDER_ACTORS));
```

The buffer does not yet tag which entity produced each triangle. Actor
transforms are available separately in `frame->actors` (see
[Actors and Navi](#actors-and-navi)).

## Upload textures

Textures live in a core-owned RGBA8 cache. Cache them on the host by index and
re-upload only when the reported `revision` changes:

```c
uint32_t count = 0;
require_ok(oot_engine_texture_count(engine, &count));
for (uint32_t i = 0; i < count; ++i) {
    OoTEngineTexture tex;
    if (oot_engine_texture_get(engine, i, &tex) != OOT_ENGINE_RESULT_OK)
        continue;
    if (host_cached_revision(i) == tex.revision)
        continue;                          /* unchanged; skip the upload */
    host_upload_rgba8(i, tex.rgbaPixels, tex.width, tex.height,
                      tex.wrapS, tex.wrapT);  /* wrap: 0 repeat, 1 mirror, 2 clamp */
    host_store_revision(i, tex.revision);
}
```

`rgbaPixels` is borrowed; copy or upload before the next mutating engine call.

## Host collision and water

Submit static integer triangles for a host-built level. Coordinates must fit
signed 16-bit range. Water boxes are axis-aligned XZ rectangles that extend
downward from their surface height:

```c
static const struct OoTSurface floor[] = {
    { 0, {{ -1000, 0, -1000 }, { -1000, 0, 1000 }, { 1000, 0, 1000 }} },
    { 0, {{ -1000, 0, -1000 }, {  1000, 0, 1000 }, { 1000, 0, -1000 }} },
};
static const struct OoTWaterBox pool[] = {
    { -200, -200, 400, 400, /*surfaceY=*/ -50 },  /* see OoTWaterBox for field order */
};
require_ok(oot_engine_static_world_load(engine, floor, 2, pool, 1));
```

Load a world (or a scene) **before** `oot_engine_link_create`: `Player_Init`
probes collision immediately, so the wrapper rejects the unsafe ordering with
`OOT_ENGINE_RESULT_NOT_AVAILABLE`. A world accepts up to
`OOT_ENGINE_MAX_STATIC_SURFACES` (2730) triangles and
`OOT_ENGINE_MAX_WATER_BOXES` (65535) water boxes.

## Load a ROM scene

Scene loading replaces the current static world and copies the room geometry
into engine-owned storage. `roomIndex = -1` loads every room of a multi-room
scene at once.

```c
int32_t nativeResult = 0;
require_ok(oot_engine_scene_load(engine, OOT_SCENE_DEKU_TREE, -1, &nativeResult));

const OoTEngineSceneGeometry *sg = NULL;
require_ok(oot_engine_scene_get_geometry(engine, &sg));

/* Opaque triangles are [0, xluStartTriangle); translucent are the rest. */
draw_opaque(sg, 0, sg->xluStartTriangle);
draw_translucent(sg, sg->xluStartTriangle, sg->numTriangles);

float spawnPos[3];
int16_t spawnYaw = 0;
if (oot_engine_scene_get_spawn(engine, 0, spawnPos, &spawnYaw) == OOT_ENGINE_RESULT_OK)
    oot_engine_link_create(engine, spawnPos[0], spawnPos[1], spawnPos[2]);
```

The scene's real light/fog is already baked into the emitted vertex colors;
`oot_engine_scene_get_environment` and `oot_engine_scene_get_runtime` expose the
underlying values (room type, echo, Lens behavior, camera type) if the host
wants to drive its own lighting. Both return an empty/NOT_AVAILABLE result after
a custom-world load.

## Rooms and door transitions

```c
uint32_t doorCount = 0;
oot_engine_scene_get_door_count(engine, &doorCount);
for (uint32_t i = 0; i < doorCount; ++i) {
    struct OoTDoor door;
    if (oot_engine_scene_get_door(engine, i, &door) != OOT_ENGINE_RESULT_OK)
        continue;
    if (link_crossed(&door)) {
        int32_t nativeResult = 0;
        oot_engine_scene_set_room(engine, door_target_room(&door), &nativeResult);
        break;
    }
}
```

`oot_engine_scene_set_room` unloads the previous room and loads the next; pass
`-1` to switch back to the whole-scene render.

## Query the surface under a point

```c
struct OoTSurfaceInfo info;
if (oot_engine_scene_query_surface(engine, x, y, z, &info) == OOT_ENGINE_RESULT_OK) {
    /* info carries floor height, floor type, mapped material, and whether the
       surface is hookshot-attachable. NOT_AVAILABLE means no floor below. */
    place_on_floor(&info);
}
```

## Z-targeting

Host-owned targets are backed by OoT's real Attention system: lock, strafing,
auto-facing, and release all come from the game code.

```c
OoTEngineTarget target = OOT_ENGINE_INVALID_TARGET;
require_ok(oot_engine_target_create(engine, ex, ey, ez, /*focusHeight=*/ 40.0f, &target));

/* follow a moving enemy every tick */
oot_engine_target_move(engine, target, ex, ey, ez);

/* Press Z (OOT_ENGINE_BUTTON_Z) to lock on; read frame->link.lockOnActive and
   frame->link.lockOnPos to drive the reticle and camera. */

oot_engine_target_remove(engine, target);   /* or oot_engine_targets_clear(engine) */
```

Targets are dropped when Link is recreated or changes age; recreate them after.

## Actors and Navi

Enabling `OOT_ENGINE_RENDER_ACTORS` appends projectile meshes to the geometry
buffer; the transforms are always available in the frame:

```c
for (uint32_t i = 0; i < frame->actorCount; ++i) {
    const struct OoTActorInfo *a = &frame->actors[i];
    place_marker(a);   /* id, position, rotation, scale — see OoTActorInfo */
}
if (frame->actorListTruncated)
    log("raise config.actorCapacity to see all actors");

if (frame->navi.available)
    draw_navi(frame->navi.position, frame->navi.innerColor, frame->navi.outerColor);
```

## Sound effects and voice

Every sound the Player code requests is delivered through a callback that runs
synchronously inside the tick. Copy the event and hand it to your audio thread;
never call back into liboot from the callback.

```c
static void on_sfx(uint16_t id, float pitch, float volume, float pan, void *user) {
    struct AudioEvent ev = { id, pitch, volume, pan };
    audio_queue_push((AudioQueue *)user, &ev);   /* copy out; do not touch liboot here */
}

config.sfxCallback = on_sfx;
config.sfxUserData = &myAudioQueue;
/* or oot_engine_set_callbacks(engine, dbg, dbgUser, on_sfx, &myAudioQueue) later */
```

Despite its historical name, `oot_engine_voice_get` returns decoded mono PCM16
for mapped gameplay SFX as well as Link/Navi voice clips:

```c
OoTEnginePcm pcm;
if (oot_engine_voice_get(engine, sfxId, &pcm) == OOT_ENGINE_RESULT_OK) {
    /* pcm.samples (PCM16), pcm.sampleCount, pcm.sampleRate, pcm.loopStart.
       loopStart == sampleCount means a non-looping clip. The view is
       core-owned and valid until engine destruction. */
    play_pcm(&pcm);
}
```

## Music and ambience

Full-song playback and the native mixer are on the process-global raw audio API
in [`liboot.h`](../src/liboot.h). They are safe to call while an engine is
alive; serialize every mutable AudioSeq call against your render callback.

```c
#include "liboot.h"

/* Decode every sample a track uses before opening the audio device, so
   first-use VADPCM decoding cannot stall an audio callback under the lock. */
oot_audio_sequence_prewarm(seqId);

/* enum OoTAudioPlayer: MAIN=0, FANFARE=1, SFX=2, SUB=3. */
oot_audio_sequence_play(OOT_AUDIO_PLAYER_MAIN, seqId, /*fadeInMs=*/ 500);
oot_audio_nature_play(OOT_AUDIO_PLAYER_MAIN, ambienceId, /*fadeInMs=*/ 500);  /* one of 19 presets */

/* Pull interleaved stereo F32 from your device callback. Allocation-free;
   host rates 8..192 kHz. Returns the number of frames written. */
uint32_t got = oot_audio_render_f32(outStereo, requestedFrames, deviceSampleRate);
```

A scene reports the music and ambience it wants; feed those ids straight in:

```c
int32_t seqId = -1, ambienceId = -1;
oot_engine_scene_get_sequence_id(engine, &seqId);
oot_engine_scene_get_ambience_id(engine, &ambienceId);
if (seqId >= 0)      oot_audio_sequence_play(OOT_AUDIO_PLAYER_MAIN, (uint16_t)seqId, 1000);
if (ambienceId >= 0) oot_audio_nature_play(OOT_AUDIO_PLAYER_MAIN, (uint8_t)ambienceId, 1000);
```

`oot_audio_sfx_catalog_get` enumerates the seven named banks with their
symbolic `NA_SE_*` names for building a sound browser.

## Battle music when an enemy is near

OoT itself decides when an enemy is near: the vendored Player/actor code tracks
the nearest hostile actor within a 500-unit battle range and, each tick one is
in range, calls its enemy-BGM hook. liboot forwards that signal to an opt-in
driver that plays the OoT battle theme (`NA_BGM_ENEMY`) on a dedicated SEQ
player, scales its volume with proximity, and fades it out when no enemy
remains. It is disabled by default and layers over any scene BGM you drive on
another player.

```c
#include "liboot.h"

/* Enable with defaults: OOT_AUDIO_PLAYER_SUB, NA_BGM_ENEMY (0x1C), 400 ms fade.
   0xFF keeps the default player; 0 keeps the default sequence. */
oot_audio_set_enemy_bgm(true, 0xFF, 0, 400);

/* ... run oot_link_tick / oot_engine_step as usual; the driver runs per tick.
   The Attention system sees hostile actors, including host Z-targets. */

/* Optional: read the live state to duck your own scene BGM, draw a UI, etc. */
bool active = false;
float distance = 0.0f;
bool enabled = oot_audio_get_enemy_bgm(&active, &distance);
/* active = battle theme playing; distance = units to the nearest in-range
   enemy this tick (or 500 when none). */

oot_audio_set_enemy_bgm(false, 0xFF, 0, 400);   /* fades out any active theme */
```

From the engine API, `oot_engine_set_enemy_bgm(engine, 1)` enables it with those
defaults; use the raw calls above for the player/sequence/fade overrides and the
live state. Because the driver runs inside the tick and touches the sequence
player, keep your usual AudioSeq serialization (tick vs `oot_audio_render_f32`)
in place.

## The Ocarina

```c
uint8_t notes[8];
int32_t noteCount = 0;
if (oot_ocarina_song_notes(OOT_SONG_LULLABY, notes, &noteCount)) {
    /* canonical note pattern for the song, note ids 0..4 */
}

/* Recognize a sequence the player just performed (longest-tail match). */
int32_t song = oot_ocarina_match(playedNotes, playedCount);
if (song >= 0)
    on_song_recognized(song);
```

`oot_engine_ocarina_note_get(engine, noteIndex, &pcm)` returns the sample rate
and loop points for each of the five instrument notes, for synthesizing the
performance yourself.

## Reposition and freeze Link

```c
oot_engine_link_freeze(engine, 1);                       /* stop simulating */
oot_engine_link_set_pose(engine, x, y, z, /*yaw=*/ 0);   /* teleport + face */
oot_engine_link_freeze(engine, 0);                       /* resume */

oot_engine_link_set_invincible(engine, 60);   /* re-apply each tick to hold it */
```

A frozen Link still renders. `set_invincible` takes a positive intangible or
negative invulnerable frame count; `0` clears it.

## Error handling

Every call returns an `OoTResult`. Never infer success from a non-null output
pointer alone. A minimal helper:

```c
static int require_ok(OoTResult r, const char *what) {
    if (r == OOT_ENGINE_RESULT_OK) return 1;
    fprintf(stderr, "%s: %s\n", what, oot_engine_result_string(r));
    return 0;
}
```

Codes you will actually branch on:

| Result | Meaning |
| --- | --- |
| `OOT_ENGINE_RESULT_SINGLETON_IN_USE` | An engine already exists in this process. |
| `OOT_ENGINE_RESULT_ROM_UNSUPPORTED` | The ROM could not be parsed. |
| `OOT_ENGINE_RESULT_NOT_AVAILABLE` | Wrong ordering, or a scene-only query with no scene loaded. |
| `OOT_ENGINE_RESULT_AGE_RESTRICTED` | Item or action not allowed for the current age. |
| `OOT_ENGINE_RESULT_BUSY` | A concurrent or callback-reentrant call was rejected. |
| `OOT_ENGINE_RESULT_LINK_NOT_FOUND` | An operation needs a Link that does not exist. |

## C++ binding

Header-only RAII over the engine API. Add `bindings/cpp` and the installed
liboot include directory to the include path and link `liboot`:

```cpp
#include <liboot.hpp>

liboot::Engine engine(rom.data(), rom.size());   // throws on failure

const OoTSurface ground[] = {
    {0, {{-1000, 0, -1000}, {-1000, 0, 1000}, {1000, 0, 1000}}},
    {0, {{-1000, 0, -1000}, { 1000, 0, 1000}, {1000, 0, -1000}}},
};
engine.load_world(ground, 2);
engine.create_link(0.0f, 0.0f, 0.0f);

OoTEngineInput input = liboot::default_input();
input.stickY = 1.0f;
const OoTEngineFrame &frame = engine.step(&input);   // borrowed; do not retain
```

Failures throw an exception carrying the `OoTResult`. Destroy the engine outside
any liboot callback; call `engine.close()` explicitly when a teardown error must
be reported rather than terminating.

## C# and Unity binding

Blittable layouts and P/Invoke declarations for a .NET or Unity host. Compile
with unsafe code enabled and place the native library (`liboot.so`,
`liboot.dylib`, or `liboot.dll`) where the runtime can resolve `liboot`. Check
every initializer:

```csharp
LibOot.EngineConfig config = default;
if (LibOot.Native.EngineConfigInit(ref config) != LibOot.Result.Ok)
    throw new InvalidOperationException("incompatible liboot config ABI");
config.romData = romPtr;    // pin only for EngineCreate; it copies synchronously
config.romSize = (nuint)rom.Length;

if (LibOot.Native.EngineCreate(ref config, out var engine) != LibOot.Result.Ok)
    throw new InvalidOperationException("engine create failed");
```

Keep managed callback delegates rooted until after `EngineDestroy`, convert them
with `Marshal.GetFunctionPointerForDelegate`, copy event data immediately, and
never let a managed exception cross the native boundary. The returned frame,
texture, and PCM pointers are borrowed; copy before a later mutating call or
before handing data to another thread. See
[bindings/csharp/README.md](../bindings/csharp/README.md).

## Threading rules

- One `OoTEngine` and one Link per process. A second `oot_engine_create`
  returns `OOT_ENGINE_RESULT_SINGLETON_IN_USE`.
- Serialize all gameplay calls on one thread. Concurrent or callback-reentrant
  calls return `OOT_ENGINE_RESULT_BUSY`.
- SFX and debug callbacks run inside the tick. Copy the payload and return; do
  not call liboot from a callback.
- The audio mixer (`oot_audio_render_f32`) runs on the device thread. Serialize
  every mutable AudioSeq call, including state getters, against it.
- Frame, texture, and PCM pointers are borrowed and short-lived. Treat them as
  read-only and copy anything you keep.
