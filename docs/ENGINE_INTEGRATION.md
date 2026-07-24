# Integrating liboot into a game engine

This guide describes liboot v0.8's recommended, engine-neutral C ABI in
[`src/liboot_engine.h`](../src/liboot_engine.h) and how to consume it from an
engine or another language. The wrapper API version is
`OOT_ENGINE_API_VERSION == 1`. The public headers are the final authority when
this guide and a newer build differ.

liboot is not a complete *Ocarina of Time* runtime. It is a host-driven Link
simulation: the host supplies a legally obtained ROM, collision, controls, a
camera direction, rendering, audio playback, and any game-specific logic.
liboot supplies the original Player state machine, animation, Link and
supported projectile actors, geometry, ROM-derived textures and selected
audio data.

Start with `liboot_engine.h`. It adds checked results, versioned and size-tagged
structures, an opaque owner, fixed-step scheduling, button latching, generation-
checked target handles, and engine-owned output buffers around the original
core. [`src/liboot.h`](../src/liboot.h) remains available as the advanced and
compatibility API for existing hosts. Do not mix its raw lifecycle calls with
an active `OoTEngine`.

For the SDK's architecture and roadmap, see the
[universal SDK overview](UNIVERSAL_SDK.md). Maintained C++ and C# sources live
under [`bindings/`](../bindings/).

## What an integration gets today

| Area | Working API | Current boundary |
| --- | --- | --- |
| Lifecycle | `oot_engine_create`, `oot_engine_destroy` and `OoTResult` | Opaque checked handle, but the core is still a one-engine/one-Link process singleton |
| Link | Wrapper calls for create/delete, adult/child, equipment, health, magic, damage, items and direct pose/facing | No direct velocity or arbitrary action setter |
| Timing/input | `oot_engine_step` and capped `oot_engine_advance`; camera-relative stick and button mask | Native gameplay remains one OoT frame per step; the host still owns its camera and input mapping |
| World | `oot_engine_static_world_load` with triangle collision, surface presets and water boxes | Whole-world replacement only; no dynamic collision objects or arbitrary raw OoT `SurfaceType` payloads |
| Actors | Frame snapshots for Navi/projectiles plus generation-checked host targets | Not a general OoT actor runtime; target actors have no host-enemy behavior or health |
| Rendering | Engine-owned world-space geometry, skeleton, Navi state and ROM texture views | Fixed triangle caps; optional actor/Navi meshes share Link's geometry without per-actor ranges |
| Audio | SFX callback, mapped PCM/Ocarina views, four-player ROM AudioSeq mixer, 110 sequences, 19 nature presets and named 1,259-SFX selector | Host pulls stereo F32 and serializes controls against its callback; synthesis is not bit-exact RSP emulation |
| Scenes | Checked scene loading, copied room geometry and entrance spawn | Main headers only; no alternate age/day headers, exits, void-out transitions, animated materials or JPEG room backgrounds |

The wrapper exposes the curated high-level `OoTLinkState.action` mapping.
Unclassified Player action functions report `OOT_ACTION_OTHER`. `animId` is a
stable one-based identity for the exact `link_animetion` entry active on that
frame; zero is reserved for an unknown animation.

## Build and deployment

The Make build stages the shared library and both public headers:

```sh
make
# dist/liboot.so
# dist/include/liboot.h
# dist/include/liboot_engine.h
```

A native C program can link it with:

```sh
cc app.c -Idist/include -Ldist -loot -lm \
  -Wl,-rpath,'$ORIGIN/dist' -o app
```

The installable CMake build exports the `liboot::oot` package target:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
cmake --install build-cmake --prefix "$PWD/stage"
```

Consume that installation with:

```cmake
find_package(liboot 0.8 CONFIG REQUIRED)
target_link_libraries(my_game PRIVATE liboot::oot)
```

The exported target supplies the installed include path, so source uses
`#include <liboot_engine.h>`. Set `CMAKE_PREFIX_PATH` to the chosen prefix when
it is not a system location. The installation also provides `liboot.pc`, so a
non-CMake host can use `pkg-config --cflags --libs liboot`.

For an engine plugin, ship the native library beside the executable or in the
engine's platform-specific native-library directory. The CMake project has
platform-aware shared/static target definitions, but published prebuilt
binaries and CI coverage may not exist for every operating system and
architecture; validate the exact target you distribute.

The ABI uses native C layout:

- `float` is 32-bit; fixed-width integers have the widths in their names.
- `size_t` and pointers follow the target architecture. Use `size_t`,
  `nuint`/`UIntPtr`, or the equivalent rather than a language `long`.
- `OoTResult` is a signed C enum in the ABI; bind it as a 32-bit integer on
  current supported targets and translate it with `oot_engine_result_string`.
- Callbacks use the platform C calling convention. Keep managed callback
  delegates rooted for as long as native code can call them.
- Compare `oot_engine_api_version()` with the exact version expected by the
  binding before initializing any structure or creating an engine.
- In C/C++, initialize `OoTEngineConfig` and `OoTEngineInput` with the
  result-returning convenience macros. They pass `sizeof(*pointer)` and
  `OOT_ENGINE_API_VERSION` to the exported `*_init_sized` functions. Their
  `structSize` fields allow compatible extension; check each initializer result
  and do not invent sizes or omit the API version.
- A foreign-function binding calls
  `oot_engine_config_init_sized(pointer, structure_size, expected_api_version)`
  and `oot_engine_input_init_sized(pointer, structure_size,
  expected_api_version)` directly. The macro names are not exported symbols.
- Public calls use a non-blocking guard. Concurrent or callback-reentrant calls
  return `OOT_ENGINE_RESULT_BUSY`; normal integration should still keep all
  calls on one gameplay thread.

Use the maintained [`bindings/`](../bindings/) where possible. Bindings
generated from `liboot_engine.h` are safer than manually repeating layouts; a
handwritten binding should assert ABI sizes/offsets on every supported target.

## Lifecycle

The exact wrapper lifecycle is create, load a world/scene, create Link, step,
and destroy. `oot_engine_destroy` also deletes an active Link, so one cleanup
call is sufficient. This concise example uses the variable-frame accumulator
that most engines want:

```c
#include <liboot_engine.h>
#include <stdio.h>

static void debug_line(void *user, const char *message) {
    (void)user;
    fprintf(stderr, "%s\n", message != NULL ? message : "liboot");
}

OoTResult start_oot(const uint8_t *rom, size_t rom_size, OoTEngine **out) {
    static const struct OoTSurface ground[] = {
        { 0, {{-1000, 0, -1000}, {-1000, 0,  1000}, {1000, 0,  1000}} },
        { 0, {{-1000, 0, -1000}, { 1000, 0,  1000}, {1000, 0, -1000}} },
    };
    OoTEngineConfig config;
    OoTResult result;

    if (out == NULL)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    *out = NULL;
    if (oot_engine_api_version() != OOT_ENGINE_API_VERSION)
        return OOT_ENGINE_RESULT_API_VERSION;
    result = oot_engine_config_init(&config);
    if (result != OOT_ENGINE_RESULT_OK)
        return result;
    config.romData = rom;
    config.romSize = rom_size;
    config.debugCallback = debug_line;
    config.renderFlags = OOT_ENGINE_RENDER_NAVI | OOT_ENGINE_RENDER_ACTORS;

    result = oot_engine_create(&config, out);
    if (result != OOT_ENGINE_RESULT_OK)
        return result;
    result = oot_engine_static_world_load(*out, ground, 2u, NULL, 0u);
    if (result == OOT_ENGINE_RESULT_OK)
        result = oot_engine_link_create(*out, 0.0f, 0.0f, 0.0f);
    if (result != OOT_ENGINE_RESULT_OK) {
        (void)oot_engine_destroy(*out);
        *out = NULL;
    }
    return result;
}

OoTResult update_oot(OoTEngine *engine, float elapsed_seconds,
                     const OoTEngineInput *input,
                     const OoTEngineFrame **out_frame) {
    uint32_t steps = 0u;
    return oot_engine_advance(engine, elapsed_seconds, input,
                              &steps, out_frame);
}

OoTResult stop_oot(OoTEngine **engine) {
    OoTResult result;
    if (engine == NULL || *engine == NULL)
        return OOT_ENGINE_RESULT_INVALID_ARGUMENT;
    result = oot_engine_destroy(*engine);
    if (result == OOT_ENGINE_RESULT_OK)
        *engine = NULL;
    return result;
}
```

Check every returned `OoTResult`; a non-null output is not proof of success.
`oot_engine_result_string(result)` returns a diagnostic string. Creation copies
the `.z64`, `.v64` or `.n64` bytes synchronously, so the ROM input buffer may
be released when `oot_engine_create` returns. No copyrighted asset should be
packaged with a plugin; ask the user to select their own compatible ROM.

The C `oot_engine_config_init` and `oot_engine_input_init` convenience macros
forward the destination size and `OOT_ENGINE_API_VERSION` to the exported,
result-returning `*_init_sized` functions. Compare the runtime API version
first and check the initializer result. Dynamic-language FFIs must bind the
sized function symbols directly. Creation also returns
`OOT_ENGINE_RESULT_API_VERSION` when `config.apiVersion` does not match.

Creation accepts ROM buffers from `OOT_ENGINE_MIN_ROM_SIZE` (`0x1060`) bytes
through `OOT_ENGINE_MAX_ROM_SIZE` (256 MiB), inclusive. Smaller, larger, null,
or otherwise unsupported ROM input is rejected without creating an engine.

Collision must be installed before `oot_engine_link_create`; use either
`oot_engine_static_world_load` or `oot_engine_scene_load`. Creating Link without
a loaded world returns `OOT_ENGINE_RESULT_NOT_AVAILABLE` instead of entering an
uninitialized collision context.

The `OoTEngine` owns its frame geometry, actor snapshots, skeleton/Navi state,
copied scene geometry and bookkeeping. The `OoTEngineFrame` and all pointers
reachable from it are borrowed, read-only views valid only until the next
mutating call or destruction. Consume or copy them before another step,
setter, world/scene load or teardown. Texture pixels are borrowed and must be
re-queried across revisions/scene changes; PCM views remain valid until engine
destruction.

Debug and SFX callbacks execute synchronously while the wrapper's guard is
held. Copy callback payloads before returning, keep `userData` alive, and never
call any `oot_engine_*` or raw `oot_*` function from a callback. Re-entry and
concurrent calls return `OOT_ENGINE_RESULT_BUSY`. If destruction returns
`OOT_ENGINE_RESULT_BUSY`, wait for the owning call to finish and retry; the
handle was not destroyed.

Only one `OoTEngine` and one Link may exist in a process. A second creation
returns `OOT_ENGINE_RESULT_SINGLETON_IN_USE`. This remains true across language
bindings and engine plugins. Do not call `oot_global_init`,
`oot_global_terminate`, raw Link lifecycle, or raw callbacks while a wrapper
handle is active.

## The 20 Hz simulation contract

The original Player code advances one game frame per native tick. The default
wrapper schedule is exactly **20 Hz** (`OOT_ENGINE_DEFAULT_FIXED_STEP`, or
0.05 seconds). Keep that default for authentic speed.

- Use `oot_engine_step` when the host already invokes you exactly at 20 Hz. It
  always advances one native tick.
- Use `oot_engine_advance` from a variable-rate update. It accumulates elapsed
  time, runs at most `config.maxSubsteps` (default 4), and caps excess catch-up
  after a stall. Zero selects the default; configured nonzero values from 1
  through 1000 are accepted. `outSteps` says how many ticks ran.
- `config.fixedStepSeconds` changes only the wrapper's wall-clock scheduling
  interval; one native tick still advances one original game frame. The
  accepted nonzero range is `[0.001, 1.0]` seconds, while zero selects the
  default. Values other than the 0.05-second default intentionally change
  gameplay speed.
- `outFrame` may point to the most recently completed frame even when
  `outSteps == 0`; it is null until the first completed tick. Check the result,
  step count and pointer separately.
- `frame->interpolationAlpha` is the remaining accumulator fraction. It can
  drive host visual interpolation, subject to the topology warning below.
- Call `oot_engine_reset_clock` after a pause or discontinuity when old elapsed
  time and pending button latches should be discarded.

Buttons are levels, not one-shot commands. liboot derives presses and releases
by comparing each tick with the preceding tick. Hold a button high for every
tick it is held, and supply a later low tick so release actions can run. This
is especially important for drawing and releasing the bow or hookshot.

A keyboard, gamepad, or UI tap can begin and end between two 20 Hz ticks.
`oot_engine_advance` latches button bits observed during a no-tick call and
applies them to the next tick. A host using exact `oot_engine_step` must still
ensure every short tap is represented by at least one high step and a later
low step.

Rendering may run at any rate. The safest first integration renders the newest
geometry until the next tick. Vertex interpolation between the previous and
current buffers is possible only when triangle counts and `triTexture` values
match; equipment, items and appended actor meshes can change topology between
ticks. Link geometry is already in world space, so applying an interpolated
engine-node transform on top of it would double-transform the model.

## Raw compatibility API

`liboot_engine.h` includes the enums and shared structures from `liboot.h`, so
most adapters never need a second include. Use the raw API directly only for
an unwrapped feature, an existing integration, or specialized control over
caller-owned geometry buffers. Raw hosts must provide their own initialization
failure handling, one-Link ID, buffer allocation, actor queries, target-ID
invalidation, 20 Hz accumulator and button latching.

The detailed contracts below name the wrapper first and occasionally mention
the underlying raw call for readers maintaining compatibility code. The
standalone raw example remains in [`examples/basic.c`](../examples/basic.c);
new hosts should follow [`examples/engine.c`](../examples/engine.c).

## Input and camera direction

After checking the runtime API version, initialize `OoTEngineInput` with the
result-returning `oot_engine_input_init` C macro and check its result, then
replace the fields for the current host sample. The wrapper clamps stick
components to `[-1, 1]`, normalizes the camera vector, and uses `+Z` if it is
zero:

- `stickY > 0` moves forward relative to the supplied camera direction.
- `stickX > 0` moves right relative to it.
- `camLookX`, `camLookZ` are the normalized XZ direction **from the camera to
  Link**, not the direction Link is facing and not the camera's forward ray.
- `OOT_ENGINE_BUTTON_A`, `_B`, `_Z` and `_R` drive the corresponding original
  controls.
- `OOT_ENGINE_BUTTON_ITEM` drives the selected C-left item. Select that item
  separately with `oot_engine_link_use_item`.

Compute the camera direction in liboot coordinates:

```c
float dx = link_x - camera_x;
float dz = link_z - camera_z;
input.camLookX = dx; /* normalization is automatic */
input.camLookZ = dz;
```

Only yaw is communicated; the host still owns view/projection, camera pitch,
collision avoidance and visual smoothing. During Z-target strafing liboot
adds an internal battle-camera yaw correction so the original orbit behavior
does not spiral away, but the visual camera remains entirely host-controlled.

## Coordinates, units and winding

liboot exposes OoT world coordinates: `+Y` is up, and an actor yaw of zero
faces `+Z`. Viewed in that forward direction, semantic right is `-X` (and
semantic left is `+X`). This is why positive camera-relative `stickX` can
produce decreasing world X with a default `+Z` camera direction. Angles such
as `faceAngle`, actor `yaw` and scene spawn yaw are signed 16-bit binary angles:

```text
radians = binary_angle * pi / 32768
degrees = binary_angle * 180 / 32768
```

Do not assume an OoT unit is one metre or one centimetre. Choose an engine
scale and apply it everywhere: input collision, Link state, output vertices,
actor positions, Navi, scene geometry, SFX positions and lock-on points.

Represent an axis conversion as a matrix `M` plus a scalar `S`:

```text
engine_position = M * oot_position * S
oot_position    = inverse(M) * engine_position / S
```

Use the inverse conversion for collision sent into liboot and for the camera
to-Link vector. Apply `M` to normals without the position scale, then normalize.
If `determinant(M) < 0`, reverse each triangle's vertex order after conversion
or change the renderer's cull mode. A white fringe or missing face is usually
an alpha/culling problem, not bad Link geometry.

Examples of reasonable mappings are:

- Unity: `(x, y, z) -> (-x, y, z) * S` maps OoT right/up/forward to Unity
  `+X/+Y/+Z`. The reflection reverses triangle winding.
- Godot: `(x, y, z) -> (-x, y, -z) * S` maps OoT right/up/forward to Godot
  `+X/+Y/-Z`. Its two sign changes preserve triangle winding.
- Unreal: `(x, y, z) -> (z, -x, y) * S` maps OoT forward/right/up to Unreal
  `+X/+Y/+Z`. The transform reverses triangle winding.

These are adapter conventions, not requirements. Keeping `(x, y, z)` unchanged
is perfectly valid if the rest of the host uses that convention.

## Collision and water

`oot_engine_static_world_load` replaces both triangle collision and water; pass
`NULL, 0` for its water arguments to create a dry world. liboot copies the
supplied arrays during the call, so temporary host buffers may be released
after it returns. The raw equivalents are `oot_static_surfaces_load` and
`oot_static_world_load`.

Order triangle vertices so
`cross(vertex1 - vertex0, vertex2 - vertex0)` points out of the solid side. The
ground example above produces a `+Y` normal. Include floors, walls and ceilings
that Link or projectiles must hit; a visual mesh is not automatically collision.

Although `OoTSurface.vertices` uses `int32_t` for ABI compatibility, the current
OoT collision backend stores vertices as signed 16-bit values. Convert from
engine coordinates, round deliberately, and keep each component within
`[-32768, 32767]`. The wrapper rejects out-of-range or unrepresentable planes,
empty worlds, degenerate triangles, non-positive water dimensions, more than
`OOT_ENGINE_MAX_STATIC_SURFACES` (2730) triangles, and geometry that cannot fit
the native collision lookup budget. Rejected input leaves the previously loaded
world intact. Removing degenerate and duplicate triangles in the host remains
useful because it preserves more of that fixed lookup budget for real geometry.

`OoTSurface.type` selects one of the stable `OoTSurfaceType` presets. Current
presets cover dirt/default, sand, grass, stone, damage, slippery, climbable,
conveyor and non-hookshot collision. Unknown values fall back to the default
hookshot-attachable surface. These are deliberately curated presets rather
than arbitrary raw OoT `SurfaceType` words, so the wrapper can reject behaviors
that require scene exits or other unavailable game state.

An `OoTWaterBox` is an XZ rectangle with an integer surface height. Membership
is strict at the rectangle edges and extends infinitely downwards. It is not a
closed physics volume, so construct collision for the basin floor and walls as
well. `xLength` and `zLength` should be positive and all fields must fit
`int16_t`. A single world accepts at most `OOT_ENGINE_MAX_WATER_BOXES` (65,535)
water boxes. There is currently no air meter or drowning.

For host-engine physics, keep the original host collision mesh as the source
for the engine collider and create a separately quantized `OoTSurface` copy for
liboot. This avoids trying to recover collision from Link's render geometry.
ROM scene collision is installed internally but is not exported through the
public API; scene room render triangles are not guaranteed to equal its
collision polygons.

## Link state and game state

Each completed `OoTEngineFrame` embeds `OoTEngineLinkState`, reporting position,
velocity, facing, horizontal speed, age, health, magic, held item, melee state,
flags, lock-on and water. Useful details:

- Health uses OoT's native units: `0x10` is one heart and `0x140` is the
  documented 20-heart maximum. `oot_engine_link_damage` goes through the real
  damage, knockback, invulnerability and death path.
- Magic levels are 0, 1 and 2 with capacities `0`, `0x30` and `0x60`.
- `meleeWeaponState != 0` indicates a live sword/hammer swing. It is useful for
  host-side enemy hit tests, because liboot does not own the host's enemies.
- Swimming is reflected both by water state and native state flags. Treat the
  named fields as stable API and state flag bits as low-level diagnostics.
- `lockOnPos` is meaningful only while `lockOnActive != 0`.
- `action` maps named Player action functions to stable `OoTAction` values;
  unclassified functions report `OOT_ACTION_OTHER`.
- `animId` identifies the active generated `LinkAnimationHeader`, while
  `animFrame` reports its current frame. IDs are ordered by segment-7 data
  offset and remain independent of host pointer addresses and ASLR.

`oot_engine_link_set_pose` directly changes position and facing without
recreating Link. It intentionally preserves the current action state; pair it
with `oot_engine_link_freeze` when the host needs a clean warp. Deleting and
recreating Link remains useful for a complete gameplay reset and invalidates
helper actors and host targets.

Equipment combinations are clamped by the game. In particular, adult and
child have different valid swords, shields and items. The wrapper returns
`OOT_ENGINE_RESULT_AGE_RESTRICTED` for an item forbidden to the current age.
`oot_engine_link_set_age` preserves position but reinitializes Player action
state and destroys non-Player actors. It invalidates every wrapper target
handle and can return `OOT_ENGINE_RESULT_NOT_AVAILABLE` when the child object
was not found.

## Rendering Link, Navi and projectiles

The wrapper allocates and owns all Link output buffers. Read them from the
borrowed frame after a successful step or advance:

```c
const OoTEngineFrame *frame = NULL;
OoTResult result = oot_engine_step(engine, &input, &frame);
if (result == OOT_ENGINE_RESULT_OK && frame != NULL) {
    const OoTEngineGeometry *geometry = &frame->geometry;
    upload_mesh(geometry->position, geometry->normal, geometry->color,
                geometry->uv, geometry->triTexture,
                geometry->numTriangles);
}
```

Triangle `t` uses vertices
`t*3` through `t*3+2`, so the output is already de-indexed. Position, normal
and color have three floats per vertex; UV has two. Colors are normalized RGB
material tints with no exported alpha; the host chooses the appropriate GPU
color-space treatment. Positions and the 21-joint skeleton pose are already in
world space.

For each triangle:

1. Resolve `triTexture[t]`; `0xFFFF` means untextured.
2. Multiply the sampled RGBA by vertex RGB.
3. Apply host lighting with the supplied world-space normal if desired.
4. For Link and opaque scene geometry, alpha-test/discard low texture alpha
   (a threshold around 0.5 is a practical starting point). Do not replace
   transparent texels with white.

The hard cap is `geometry.triangleCapacity` (currently
`OOT_GEO_MAX_TRIANGLES`, 2048). Enable `OOT_ENGINE_RENDER_NAVI` and/or
`OOT_ENGINE_RENDER_ACTORS` in the config or with
`oot_engine_set_render_flags` to append those meshes to the **same** geometry.
There are no ranges identifying which actor emitted which triangles, and the
combined output can reach the cap. Keep these flags off if the integration
needs Link-only topology, and use frame actor snapshots plus host proxy meshes.

Navi's glowing body is deliberately not in the triangle output. When
`frame->navi.available` is nonzero, draw a camera-facing soft sprite at its
position, with the inner color in the core fading to the outer color. A scale
of zero means hidden. The suggested world radius is approximately
`1500 * scale`.

When `frame->skeletonAvailable` is nonzero, `frame->skeleton` contains 21
world-space joints and parent indices. It is suited to debug rendering,
attachments and host-side hit volumes. It is not a skinned-mesh bone palette:
Link's display-list geometry is already posed.

### Texture cache

Geometry generation and scene loading add decoded RGBA8 textures to a
library-owned cache. After those operations:

1. Call `oot_engine_texture_count`.
2. Call `oot_engine_texture_get` for each referenced/new index.
3. Upload `width * height * 4` bytes as RGBA8.
4. Apply `wrapS` and `wrapT`: 0 repeat, 1 mirror, 2 clamp.
5. Remember `revision` and re-upload when it changes.

`OoTEngineTexture.rgbaPixels` is borrowed; never free or write it. Its byte
length is supplied as `rgbaSize`. Copy/upload promptly and do not retain the
pointer across mutating calls, scene loads or destruction. Texture indices used
by geometry stay meaningful through the accompanying cache/revision contract,
but a scene transition may evict scene-backed pixels and reuse storage, which
is why revision checks are required.

Link's eye and mouth texture selection is animation-driven and is resolved on
each generated frame. Treat those texture indices like every other geometry
texture and honor cache revisions; they are not a static-face limitation.

Upload pixels and UVs as a pair without independently flipping one. If an
engine's image-origin convention makes the result upside down, flip V or the
pixel rows exactly once in the adapter. Use texture alpha for cutouts and
translucency; ignoring it causes the characteristic white rectangles around
hair, equipment and effects.

### Scene room geometry

After `oot_engine_scene_load` succeeds,
`oot_engine_scene_get_geometry` returns wrapper-owned arrays copied from the
core, with the same vertex layout and texture cache as Link. Treat the view as
borrowed and consume it before replacing the world/scene or destroying the
engine. Its `triangleCapacity` is currently `OOT_SCENE_MAX_TRIANGLES` (16384).

Draw `[0, xluStartTriangle)` as the opaque/cutout pass with depth writes.
Draw `[xluStartTriangle, numTriangles)` afterwards with source-alpha blending
and depth writes disabled. Combine texture alpha with the exported scene vertex
alpha. Scene RGB colors already carry the interpreted material/lighting color,
so avoid accidentally applying strong lighting twice.

`oot_engine_scene_get_runtime` copies the live `OoTSceneRuntime`; unlike the
borrowed geometry view, the caller owns this small value. It reports the scene
and active room that the vendored Player actually sees, including room type,
environment (cold/hot behavior), echo, Lens mode, warp-song restriction,
scene-camera type and map area. `geometryRoomIndex == -1` means every room mesh
was concatenated, while `activeRoomIndex == 0` remains valid for gameplay.
Loading a custom static world ends the ROM scene and makes the query return
`OOT_ENGINE_RESULT_NOT_AVAILABLE`. Calls use the same non-blocking engine guard
as the rest of this API.

## Actors, host enemies and targeting

Every completed frame contains up to `frame->actorCapacity` non-Player actor
snapshots at `frame->actors`, including Navi, projectiles and host-created
target actors. `actorListTruncated` says that the configured capacity was too
small; raise `config.actorCapacity` on the next engine creation if needed.
`active == 0` denotes an actor killed during that tick. Actor IDs are original
game actor IDs, not stable per-instance handles. Match instances conservatively
using actor type plus temporal/positional tracking.

Attention targets are the bridge from a host enemy system into Link's real
Z-targeting behavior:

```c
OoTEngineTarget target = OOT_ENGINE_INVALID_TARGET;
OoTResult result = oot_engine_target_create(
    engine, enemy_x, enemy_y, enemy_z, enemy_height, &target);

/* Before each Link step: */
result = oot_engine_target_move(
    engine, target, enemy_x, enemy_y, enemy_z);

/* When the host enemy dies or leaves range: */
result = oot_engine_target_remove(engine, target);
```

There are at most 16 target slots. `focusHeight` is the focus height above the
base position, not a collision radius; around 30 OoT units is appropriate for
a ground-standing target. Wrapper handles carry a generation, so a stale
handle returns `OOT_ENGINE_RESULT_TARGET_NOT_FOUND` instead of moving a reused
slot. Removal releases an existing lock through the normal dead-actor path on
the following tick. Link deletion and a successful age change invalidate all
handles.

Targets do not make a host enemy participate in OoT combat. The host still
owns enemy health, hurt boxes, AI and visual effects. A common adapter uses
Link's skeleton and `meleeWeaponState` for sword hit volumes, and tracks arrow,
bomb, hookshot and boomerang transforms from the frame actor list for host-side
overlap tests. Avoid applying damage every tick of the same swing: assign a
swing or projectile contact ID and remember which enemies it already hit.

## Audio contract

Set `OoTEngineConfig.sfxCallback` during creation or replace callbacks later
with `oot_engine_set_callbacks`. The preferred callback includes `userData`:

```c
static void on_sfx(void *user_data, const struct OoTSfxEvent *event) {
    /* Copy the value into a lock-free or mutex-protected host queue. */
    audio_queue_push(user_data, *event);
}
```

It is invoked synchronously from the simulation thread. Handle:

- `OOT_SFX_PLAY`: start or update a sound using `sfxId`, `position`,
  `freqScale`, `volume`, `reverb` and `token`.
- `isRefresh != 0`: refresh/update a continuing sound instead of stacking a
  new copy every tick.
- `OOT_SFX_STOP_ID`: stop matching SFX/voice instances.
- `OOT_SFX_STOP_POSITION`: stop instances attached to the supplied position.

The event pointer is valid only during the callback. The callback runs while
the wrapper guard is held, so copy and enqueue only; an engine API call made
there returns `OOT_ENGINE_RESULT_BUSY`. Raw compatibility hosts can still use
`oot_set_sfx_callback_ex`, while the older ID-only callback does not receive
stop actions.

Despite its compatibility name, `oot_engine_voice_get` and the raw
`oot_get_voice_sample` serve mapped gameplay SFX (including footsteps, weapon
swings, shields, rolls, and damage) as well as Link/Navi voice clips. The
wrapper fills an `OoTEnginePcm` view with library-owned mono signed PCM16 plus
its native sample rate, or returns `OOT_ENGINE_RESULT_NOT_AVAILABLE` when that
ID has no mapping. Do not assume a fixed 32 kHz rate. Apply `freqScale` in
addition to resampling from the returned source rate to the engine's mixer
rate, then apply event volume and host spatial attenuation. PCM views remain
valid until `oot_engine_destroy`.

`oot_engine_ocarina_note_get(0..4)` returns the five logical notes. They share
source PCM but have different sample rates. Play `[0, sampleCount)` once, loop
`[loopStart, sampleCount)` while held, and fade for roughly 50 ms on release.

The raw audio API also runs OoT's ROM-backed three-level AudioSeq programs. It
is initialized by `oot_engine_create`/`oot_global_init` and reset at teardown:

```c
/* Startup/control thread, before the real-time device starts. */
for (uint16_t id = 0; id < OOT_AUDIO_SEQUENCE_COUNT; ++id)
    oot_audio_sequence_prewarm(id);

/* Use scene command 0x15 values after a successful scene load. */
int32_t sequence = oot_scene_get_sequence_id();
int32_t ambience = oot_scene_get_ambience_id();
if (sequence >= 0 && sequence < OOT_AUDIO_SEQUENCE_COUNT)
    oot_audio_sequence_play(OOT_AUDIO_PLAYER_MAIN, (uint16_t)sequence, 250);
else if (ambience >= 0 && ambience < OOT_AUDIO_NATURE_COUNT)
    oot_audio_nature_play(OOT_AUDIO_PLAYER_MAIN, (uint8_t)ambience, 250);
else
    oot_audio_sequence_stop(OOT_AUDIO_PLAYER_MAIN, 250);

/* Audio callback: output is overwritten with interleaved stereo F32. */
oot_audio_render_f32(output, frame_count, sample_rate);
```

Use `oot_audio_sequence_count/name/get_info` for the 110-entry music selector
and `oot_audio_sfx_catalog_count/get` plus `oot_audio_sfx_play` for all seven
SFX banks. The four players mirror main BGM, fanfare, SFX and secondary BGM.
AudioSeq calls and rendering share process-global mutable state; lock the audio
device around controls and state getters such as `oot_audio_sequence_get_state`.
The render path allocates nothing. It interprets the retail sequence data and
samples, but does not claim bit-exact N64 RSP output.
Nature ambience replaces the MAIN BGM in retail OoT; an environment/time-of-day
system may switch between the scene BGM and its nature preset, but should not
automatically layer both.

## Loading ROM scenes

Load a scene and room before creating Link, then query a spawn:

```c
int32_t native_result = 0;
OoTResult result = oot_engine_scene_load(
    engine, OOT_SCENE_KOKIRI_FOREST, 0, &native_result);
if (result == OOT_ENGINE_RESULT_OK) {
    float spawn[3];
    int16_t yaw = 0;
    result = oot_engine_scene_get_spawn(engine, 0, spawn, &yaw);
    if (result == OOT_ENGINE_RESULT_OK)
        result = oot_engine_link_create(engine, spawn[0], spawn[1], spawn[2]);
}
```

`OOT_ENGINE_RESULT_OK` means collision and copied room geometry were loaded.
`OOT_ENGINE_RESULT_SCENE_LOAD_FAILED` means the native load failed;
`outNativeResult` preserves its detailed negative code.
`OOT_ENGINE_RESULT_SCENE_GEOMETRY_UNAVAILABLE` is the special partial result:
scene collision was committed but room mesh output is unavailable (native
result `-9`). The eight values in `OoTSceneIndex` are convenience constants;
retail scene tables contain more valid indices.

Spawn index 0 is the reliable entrance. Higher indices are best-effort because
the entrance-table count is not parsed yet. Scene loading does not itself move
an existing Link. For a scene transition, delete Link, load the new scene,
query its spawn and create Link again. Within one scene,
`oot_engine_scene_get_door` exposes transition actors and
`oot_engine_scene_set_room` synchronously swaps the active room. The host still
detects door crossings and routes scene exits/void-outs itself.

## Custom C and C++ engines

The versioned C API is already suitable for a custom renderer. A useful adapter
owns:

- the sole `OoTEngine *` and its destruction path;
- host input state and the latest borrowed `OoTEngineFrame` only until the next
  mutation;
- a GPU texture cache keyed by `(texture index, revision)`;
- `OoTEngineTarget` handles paired with enemy entity IDs;
- a thread-safe SFX event queue;
- coordinate conversion functions used at every boundary.

One complete tick looks like:

```c
if (oot_engine_api_version() != OOT_ENGINE_API_VERSION)
    return OOT_ENGINE_RESULT_API_VERSION;

OoTEngineInput in;
OoTResult result = oot_engine_input_init(&in);
if (result != OOT_ENGINE_RESULT_OK)
    return result;
in.camLookX = camera_to_link_x;
in.camLookZ = camera_to_link_z;
in.stickX = move_right;
in.stickY = move_forward;
if (jump_roll_held) in.buttons |= OOT_ENGINE_BUTTON_A;
if (attack_held)    in.buttons |= OOT_ENGINE_BUTTON_B;
if (target_held)    in.buttons |= OOT_ENGINE_BUTTON_Z;
if (shield_held)    in.buttons |= OOT_ENGINE_BUTTON_R;
if (item_held)      in.buttons |= OOT_ENGINE_BUTTON_ITEM;

uint32_t steps = 0u;
const OoTEngineFrame *frame = NULL;
result = oot_engine_advance(
    engine, elapsed_seconds, &in, &steps, &frame);
if (result == OOT_ENGINE_RESULT_OK && frame != NULL) {
    upload_changed_textures(engine);
    update_host_transform_from_state(&frame->link);
    rebuild_or_stream_link_mesh(&frame->geometry);
    consume_actor_snapshots(frame->actors, frame->actorCount);
}
```

For C++, use the maintained C++11 RAII binding in
[`bindings/cpp/liboot.hpp`](../bindings/cpp/liboot.hpp). It translates failed
results to `liboot::Error`, is move-only, and keeps borrowed frame semantics:

```cpp
#include <liboot.hpp>

if (oot_engine_api_version() != OOT_ENGINE_API_VERSION)
    throw liboot::Error(OOT_ENGINE_RESULT_API_VERSION);

liboot::Engine engine(rom.data(), rom.size());
engine.load_world(surfaces.data(), (uint32_t)surfaces.size());
engine.create_link(0.0f, 0.0f, 0.0f);

OoTEngineInput input = liboot::default_input();
input.stickY = 1.0f;
liboot::AdvanceResult result = engine.advance(elapsed_seconds, &input);
if (result.frame != nullptr)
    render(result.frame->geometry);
```

Only one `liboot::Engine` may be live, because the RAII class cannot change the
native singleton limit. Do not retain its returned frame reference or pointer
after a mutating method. Use `engine.close()` when teardown errors must be
observed or retried. Never destroy from a callback: the noexcept destructor
terminates on native teardown failure rather than losing the singleton handle.

## Unity (C# P/Invoke)

Create a native plugin folder such as `Assets/Plugins/x86_64/`, place the
platform liboot binary there, and add the maintained
[`bindings/csharp/LibOot.cs`](../bindings/csharp/LibOot.cs) to a Unity assembly.
It mirrors `liboot_engine.h`; do not maintain a second raw `liboot.h` P/Invoke
layer unless compatibility code requires it. Enable `Allow 'unsafe' Code`.
`LibOot.cs` supplies declarations, not a managed lifetime owner: wrap the
returned `IntPtr`, call `EngineDestroy` exactly once from the owning
`MonoBehaviour`/service, and handle Unity domain reload before unloading the
plugin.

Initialize size/version-tagged values through the native helpers and pin the
ROM only across creation:

```csharp
unsafe Result Create(byte[] rom, out IntPtr engine)
{
    engine = IntPtr.Zero;
    if (Native.EngineApiVersionGet() != Native.EngineApiVersion)
        return Result.ApiVersion;

    EngineConfig config = default;
    Result result = Native.EngineConfigInit(ref config);
    if (result != Result.Ok)
        return result;
    fixed (byte* bytes = rom)
    {
        config.RomData = (IntPtr)bytes;
        config.RomSize = new UIntPtr((uint)rom.Length);
        return Native.EngineCreate(ref config, out engine);
    } // Native creation has copied the ROM.
}
```

Drive the wrapper accumulator from `Update`; it handles 20 Hz scheduling and
short-press latching. The returned frame and its arrays are borrowed:

```csharp
if (Native.EngineApiVersionGet() != Native.EngineApiVersion)
    throw new InvalidOperationException("liboot API version mismatch");

EngineInput input = default;
Result inputResult = Native.EngineInputInit(ref input);
if (inputResult != Result.Ok)
    throw new InvalidOperationException(Native.EngineResultString(inputResult));
input.StickY = move.y;
input.Buttons = attackHeld ? Buttons.B : Buttons.None;

uint steps;
IntPtr framePointer;
Result result;
unsafe
{
    result = Native.EngineAdvance(engine, Time.unscaledDeltaTime,
                                  &input, out steps, out framePointer);
}
if (result == Result.Ok && framePointer != IntPtr.Zero) {
    EngineFrame frame = Marshal.PtrToStructure<EngineFrame>(framePointer);
    UploadBorrowedGeometry(frame.Geometry); // before another native mutation
}
```

`Marshal.PtrToStructure` copies only the frame structure; its geometry, actor
and texture pointers still refer to native memory. Copy or upload them before
the next mutating call. Update `Mesh` and create/update `Texture2D` objects on
Unity's main thread. Group de-indexed triangles by texture into submeshes or a
texture-array material. Apply the Unity reflection described in the coordinate
section and reverse winding. Set vertex colors and discard low sample alpha in
the shader; ignoring alpha creates white artifacts.

The maintained binding declares `DebugCallback` and `SfxCallback`. Store each
delegate in a field until after `EngineDestroy`, obtain its pointer with
`Marshal.GetFunctionPointerForDelegate`, copy callback data immediately, and
enqueue it for `AudioSource` work on the main thread. Never call Unity APIs or
liboot from the callback.

For IL2CPP/AOT targets, callback entry points also need the platform's AOT
callback annotation. Assert the maintained binding's structure sizes in a
development build for each architecture and test the plugin under IL2CPP, not
only Mono.

## Godot 4 (C++ GDExtension)

The most direct Godot integration is a C++ GDExtension that links liboot and
exposes a `Node3D` such as `LibootLink`. Use
[`bindings/cpp/liboot.hpp`](../bindings/cpp/liboot.hpp) inside the extension;
it consumes `liboot_engine.h` without a handwritten ABI, while the extension
translates Godot resources and signals.

At initialization:

1. Read the ROM with `FileAccess` into a `PackedByteArray`.
2. Require `oot_engine_api_version() == OOT_ENGINE_API_VERSION` before the
   binding initializes a config or input structure.
3. Construct one `liboot::Engine(bytes.ptr(), bytes.size())`.
4. Convert a level collision mesh to signed-in-range `OoTSurface` triangles,
   call `load_world`, and call `create_link`.
5. Keep the RAII owner alive until extension shutdown; the wrapper owns output
   buffers.

A simplified tick method is:

```cpp
void LibootLink::_process(double delta) {
    OoTEngineInput in = liboot::default_input();
    Vector2 move = Input::get_singleton()->get_vector(
        "move_left", "move_right", "move_forward", "move_back");
    in.stickX = move.x;
    in.stickY = -move.y;
    fill_camera_to_link(in.camLookX, in.camLookZ);
    if (Input::get_singleton()->is_action_pressed("oot_a"))
        in.buttons |= OOT_ENGINE_BUTTON_A;
    if (Input::get_singleton()->is_action_pressed("oot_b"))
        in.buttons |= OOT_ENGINE_BUTTON_B;
    if (Input::get_singleton()->is_action_pressed("oot_z"))
        in.buttons |= OOT_ENGINE_BUTTON_Z;
    if (Input::get_singleton()->is_action_pressed("oot_r"))
        in.buttons |= OOT_ENGINE_BUTTON_R;
    if (Input::get_singleton()->is_action_pressed("oot_item"))
        in.buttons |= OOT_ENGINE_BUTTON_ITEM;

    liboot::AdvanceResult advanced = engine->advance((float)delta, &in);
    if (advanced.frame != nullptr)
        rebuild_array_mesh(advanced.frame->geometry); // borrowed this call
}
```

Use `ArrayMesh` surface arrays (`ARRAY_VERTEX`, `ARRAY_NORMAL`, `ARRAY_COLOR`,
`ARRAY_TEX_UV`, `ARRAY_INDEX`) grouped by texture. Cache each decoded image as
`Image::create_from_data(width, height, false, Image::FORMAT_RGBA8, bytes)` and
create an `ImageTexture` on the main thread. A spatial shader should multiply
albedo by vertex color and texture, with alpha scissor for Link/cutout passes.

Godot's visual forward axis is `-Z`; the sample mapping in the coordinate
section flips both X and Z, so preserve winding when filling the mesh. Keep the liboot
world-space vertices under an identity `MeshInstance3D`, or subtract Link's
position before parenting a local mesh—do not also set the node to Link's
position while retaining world-space vertices.

Expose host enemies as Godot nodes paired with `OoTEngineTarget` handles, move
them before every step, and remove them in `_exit_tree`. Emit copied SFX events
as deferred signals; create or control `AudioStreamPlayer3D` nodes on the main
thread. Destroy the `liboot::Engine` owner before the GDExtension/native library
is unloaded. Centralize that owner rather than letting several scene nodes
construct competing singleton instances.

## Unreal Engine 5 (C++)

Create a runtime module or plugin with `liboot_engine.h` in a third-party
include directory. In `Build.cs`, add that directory, link the platform liboot
binary, and stage its runtime shared library. The C++ binding uses exceptions;
because many Unreal targets disable them, a `UWorldSubsystem` will usually own
the checked C `OoTEngine *` directly.

Call `oot_engine_advance` from subsystem `Tick`. The wrapper owns its arrays;
consume the returned frame before another native mutation:

```cpp
if (oot_engine_api_version() != OOT_ENGINE_API_VERSION)
    return;

OoTEngineInput Input;
OoTResult Result = oot_engine_input_init(&Input);
if (Result != OOT_ENGINE_RESULT_OK)
    return;
FillInputFromUnreal(Input);

uint32 Steps = 0;
const OoTEngineFrame* Frame = nullptr;
Result = oot_engine_advance(
    Engine, DeltaSeconds, &Input, &Steps, &Frame);
if (Result == OOT_ENGINE_RESULT_OK && Frame != nullptr)
    UpdateUnrealMesh(Frame->geometry); // borrowed until the next mutation
```

A convenient axis mapping is:

```cpp
static FVector ToUnreal(float X, float Y, float Z, float Scale) {
    return FVector(Z, -X, Y) * Scale; // OoT forward/right/up -> UE X/Y/Z
}
```

Use the inverse mapping for level collision and camera-to-Link input. The
transform reverses winding. Choose `Scale` for the project rather than
assuming OoT units are centimetres.

For a prototype, group triangles by texture into sections of
`UProceduralMeshComponent`. For production, a dynamic mesh/Render Hardware
Interface path or a texture array avoids rebuilding many procedural sections
every 50 ms. Create transient `UTexture2D` resources in `PF_R8G8B8A8`, apply
wrap modes, and update when a liboot texture revision changes. Materials need
masked blend mode for Link/cutouts and translucent mode for the scene XLU
pass.

Run UObject, procedural mesh, texture and audio operations on the game thread.
If liboot simulation is moved to a worker thread, it must be the sole owner of
all liboot calls, and callbacks should copy into a thread-safe Unreal queue.
Pair enemy actors/components with `OoTEngineTarget` handles for Z-targeting and
invalidate those pairs after Link deletion or an age change.
Multiple PIE worlds cannot each create an engine in the same editor process;
use one process-wide subsystem or run those sessions in separate processes.

## Rust

There is no official Rust crate yet. Generate a `-sys` layer from the installed
wrapper header so Rust follows the exact C layout:

```sh
bindgen dist/include/liboot_engine.h \
  --allowlist-function 'oot_engine_.*' \
  --allowlist-type 'OoT.*' \
  --allowlist-var 'OOT_.*' \
  --output src/liboot_sys.rs
```

A minimal `build.rs` for a checkout-relative build is:

```rust
fn main() {
    println!("cargo:rustc-link-search=native=../liboot/dist");
    println!("cargo:rustc-link-lib=dylib=oot");
    println!("cargo:rerun-if-changed=../liboot/src/liboot_engine.h");
}
```

Keep raw calls in a small `liboot-sys` layer and expose one safe, non-`Send`,
non-`Sync` owner around the opaque pointer. The wrapper owns geometry, so the
safe layer should borrow a frame from `&mut self` rather than allocate native
output arrays:

```rust
const EXPECTED_API_VERSION: u32 = 1;
let actual_api_version = unsafe { oot_engine_api_version() };
if actual_api_version != EXPECTED_API_VERSION {
    return Err(OoTResult_OOT_ENGINE_RESULT_API_VERSION);
}

let mut config = std::mem::MaybeUninit::<OoTEngineConfig>::uninit();
let init_result = unsafe {
    oot_engine_config_init_sized(
        config.as_mut_ptr(),
        std::mem::size_of::<OoTEngineConfig>() as u32,
        EXPECTED_API_VERSION,
    )
};
if init_result != OoTResult_OOT_ENGINE_RESULT_OK {
    return Err(init_result);
}
let mut config = unsafe { config.assume_init() };
config.romData = rom.as_ptr();
config.romSize = rom.len();

let mut engine: *mut OoTEngine = std::ptr::null_mut();
let result = unsafe { oot_engine_create(&config, &mut engine) };
if result != OoTResult_OOT_ENGINE_RESULT_OK {
    return Err(result);
}

let mut input = std::mem::MaybeUninit::<OoTEngineInput>::uninit();
let init_result = unsafe {
    oot_engine_input_init_sized(
        input.as_mut_ptr(),
        std::mem::size_of::<OoTEngineInput>() as u32,
        EXPECTED_API_VERSION,
    )
};
if init_result != OoTResult_OOT_ENGINE_RESULT_OK {
    unsafe { oot_engine_destroy(engine) };
    return Err(init_result);
}
let input = unsafe { input.assume_init() };
```

Generated enum constant names vary with bindgen settings; wrap them instead of
leaking them into application code. `oot_engine_config_init` and
`oot_engine_input_init` are C preprocessor macros, not exported symbols, so an
FFI must call the sized functions with its actual structure size and expected
API version as above, and check each result. The owner's `Drop` calls only
`oot_engine_destroy`—it already deletes Link and terminates the core. Give a
returned frame lifetime no longer than the mutable owner borrow, turn native
arrays into checked slices from `numTriangles`, and prevent another mutation
while those slices exist. For callbacks use `unsafe extern "C" fn`, copy the
event value, send it through a channel, and never unwind across C. A dynamic
loader such as `libloading` is also viable, but its `Library` must outlive the
engine and every function pointer.

Engines such as Bevy can treat the safe owner as an exclusive resource, call
`oot_engine_advance` in one system, copy required frame data, and update
meshes/textures in later systems after releasing the native borrow.

## Python (`ctypes`)

Python is useful for tools, automated tests and prototypes. It is not an ideal
place to rebuild a large render mesh at high frequency, but the 20 Hz native
simulation itself is straightforward. Bind `liboot_engine.h`, not the raw
buffer API. This minimal `ctypes` example exercises checked creation, world
loading, accumulation and destruction; generate or mirror the remaining frame
structures before dereferencing `frame` in a real tool.

```python
import ctypes as C
from pathlib import Path

EXPECTED_API_VERSION = 1

class Config(C.Structure):
    _fields_ = [
        ("structSize", C.c_uint32), ("apiVersion", C.c_uint32),
        ("romData", C.c_void_p), ("romSize", C.c_size_t),
        ("actorCapacity", C.c_uint32), ("maxSubsteps", C.c_uint32),
        ("fixedStepSeconds", C.c_float), ("renderFlags", C.c_uint32),
        ("debugCallback", C.c_void_p), ("debugUserData", C.c_void_p),
        ("sfxCallback", C.c_void_p), ("sfxUserData", C.c_void_p),
    ]

class Input(C.Structure):
    _fields_ = [
        ("structSize", C.c_uint32),
        ("camLookX", C.c_float), ("camLookZ", C.c_float),
        ("stickX", C.c_float), ("stickY", C.c_float),
        ("buttons", C.c_uint32),
    ]

class Surface(C.Structure):
    _fields_ = [
        ("type", C.c_uint16),
        ("vertices", (C.c_int32 * 3) * 3),
    ]

class WaterBox(C.Structure):
    _fields_ = [
        ("xMin", C.c_int16), ("zMin", C.c_int16),
        ("xLength", C.c_int16), ("zLength", C.c_int16),
        ("ySurface", C.c_int16),
    ]

def surface(a, b, c):
    result = Surface()
    for row, point in enumerate((a, b, c)):
        for axis, value in enumerate(point):
            result.vertices[row][axis] = value
    return result

lib = C.CDLL("./dist/liboot.so")
expected_config_size = 72 if C.sizeof(C.c_void_p) == 8 else 48
assert C.sizeof(Config) == expected_config_size
assert C.sizeof(Input) == 24
assert C.sizeof(Surface) == 40
assert C.sizeof(WaterBox) == 10
lib.oot_engine_api_version.argtypes = []
lib.oot_engine_api_version.restype = C.c_uint32
lib.oot_engine_result_string.argtypes = [C.c_int]
lib.oot_engine_result_string.restype = C.c_char_p
lib.oot_engine_config_init_sized.argtypes = [
    C.POINTER(Config), C.c_uint32, C.c_uint32,
]
lib.oot_engine_config_init_sized.restype = C.c_int
lib.oot_engine_input_init_sized.argtypes = [
    C.POINTER(Input), C.c_uint32, C.c_uint32,
]
lib.oot_engine_input_init_sized.restype = C.c_int
lib.oot_engine_create.argtypes = [C.POINTER(Config), C.POINTER(C.c_void_p)]
lib.oot_engine_create.restype = C.c_int
lib.oot_engine_static_world_load.argtypes = [
    C.c_void_p, C.POINTER(Surface), C.c_uint32,
    C.POINTER(WaterBox), C.c_uint32,
]
lib.oot_engine_static_world_load.restype = C.c_int
lib.oot_engine_link_create.argtypes = [C.c_void_p, C.c_float, C.c_float,
                                       C.c_float]
lib.oot_engine_link_create.restype = C.c_int
lib.oot_engine_advance.argtypes = [
    C.c_void_p, C.c_float, C.POINTER(Input),
    C.POINTER(C.c_uint32), C.POINTER(C.c_void_p),
]
lib.oot_engine_advance.restype = C.c_int
lib.oot_engine_destroy.argtypes = [C.c_void_p]
lib.oot_engine_destroy.restype = C.c_int

def check(result):
    if result != 0:
        message = lib.oot_engine_result_string(result).decode("utf-8")
        raise RuntimeError(message)

actual_api_version = lib.oot_engine_api_version()
if actual_api_version != EXPECTED_API_VERSION:
    raise RuntimeError(
        f"liboot API mismatch: expected {EXPECTED_API_VERSION}, "
        f"loaded {actual_api_version}"
    )

rom_bytes = Path("oot.z64").read_bytes()
rom = (C.c_uint8 * len(rom_bytes)).from_buffer_copy(rom_bytes)
config = Config()
check(lib.oot_engine_config_init_sized(
    C.byref(config), C.sizeof(Config), EXPECTED_API_VERSION
))
config.romData = C.cast(rom, C.c_void_p)
config.romSize = len(rom_bytes)
engine = C.c_void_p()
check(lib.oot_engine_create(C.byref(config), C.byref(engine)))
# The wrapper has copied `rom`; it no longer needs to stay alive.
del rom, rom_bytes

try:
    ground = (Surface * 2)(
        surface((-1000, 0, -1000), (-1000, 0, 1000), (1000, 0, 1000)),
        surface((-1000, 0, -1000), (1000, 0, 1000), (1000, 0, -1000)),
    )
    check(lib.oot_engine_static_world_load(engine, ground, len(ground), None, 0))
    check(lib.oot_engine_link_create(engine, 0.0, 0.0, 0.0))

    inputs = Input()
    check(lib.oot_engine_input_init_sized(
        C.byref(inputs), C.sizeof(Input), EXPECTED_API_VERSION
    ))
    inputs.stickY = 1.0
    steps = C.c_uint32()
    frame = C.c_void_p()
    for _ in range(4):
        check(lib.oot_engine_advance(engine, 1.0 / 60.0, C.byref(inputs),
                                     C.byref(steps), C.byref(frame)))
    print("frame available:", bool(frame.value),
          "steps in last update:", steps.value)
finally:
    destroy_result = lib.oot_engine_destroy(engine)
    if destroy_result != 0:
        print("destroy failed:",
              lib.oot_engine_result_string(destroy_result).decode("utf-8"))
```

The surface declaration mirrors `OoTSurface`; generate the complete binding
from the header for a larger adapter. Define every called function's
`argtypes` and `restype`; a missing declaration can truncate pointers on
64-bit systems. Validate `Config`, `Input` and frame structure sizes against a
small C probe in CI.

A `CFUNCTYPE(None, c_void_p, ...)` callback object must be stored in a live
Python variable until after destruction. Copy callback values into a
`queue.Queue`; do not retain the message/event pointer, re-enter liboot, or
allow exceptions to cross the native boundary.

## Production checklist

- [ ] The user supplies a compatible ROM; no ROM data is distributed with the
      game/plugin.
- [ ] One object owns the sole `OoTEngine`; wrapper and raw lifecycle calls are
      never mixed.
- [ ] Every `OoTResult` is checked and translated for the user.
- [ ] The wrapper API version is checked; `OoTEngineConfig` and every input are
      initialized with the public helper.
- [ ] Simulation uses `oot_engine_step` at 20 Hz or the capped/latching
      `oot_engine_advance` accumulator.
- [ ] Camera input is normalized camera-to-Link direction in liboot space.
- [ ] Coordinate conversion and scale are used consistently in both directions.
- [ ] Reflected coordinate mappings reverse triangle winding.
- [ ] Collision is non-degenerate and fits the current signed 16-bit backend.
- [ ] Borrowed frame and texture pointers are consumed before invalidation and
      never freed by the host.
- [ ] Link world-space geometry is not transformed twice.
- [ ] Texture alpha, wrap modes and revision updates are honored.
- [ ] Scene opaque and translucent ranges use the appropriate depth/blend
      state.
- [ ] Managed callback delegates remain rooted and callback payloads are copied.
- [ ] PCM is resampled from its reported rate; continuing SFX instances are
      refreshed rather than stacked.
- [ ] AudioSeq control calls are serialized against `oot_audio_render_f32`;
      scene ambience uses `oot_audio_nature_play`, not raw sequence 1 alone.
- [ ] Generation-checked target handles are discarded after Link deletion or
      age changes.
- [ ] No callback re-enters liboot.
- [ ] `oot_engine_destroy` succeeds before unloading the shared library.

[`examples/engine.c`](../examples/engine.c) is the minimal wrapper reference.
The feature playground in `test/playground.c` remains the executable reference
for collision, camera input, texture revisions, cutout/translucent rendering,
audio, items, targets, actors and scene transitions.
