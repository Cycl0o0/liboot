# Getting started

This guide starts with the versioned engine-neutral API in
`liboot_engine.h`. The raw compatibility API in `liboot.h` remains available
when an integration needs direct control of its geometry buffers. For Unity,
Godot, Unreal, Rust, Python, and custom-engine patterns, continue with
[ENGINE_INTEGRATION.md](ENGINE_INTEGRATION.md).

## Requirements

- A C11 compiler and GNU Make, or CMake 3.16 or newer.
- Linux for the currently tested shared-library build. The public headers are
  platform-neutral; macOS and Windows build validation is ongoing.
- A legally obtained, compatible Ocarina of Time ROM supplied at runtime.
  ROMs and extracted assets must not be committed to a project using liboot;
  the engine wrapper accepts buffers from `OOT_ENGINE_MIN_ROM_SIZE` (`0x1060`)
  bytes through
  `OOT_ENGINE_MAX_ROM_SIZE` (256 MiB), inclusive.
- SDL2 and OpenGL development packages only if you want the interactive
  playground. The core library itself does not depend on SDL or OpenGL.

On Debian or Ubuntu, the full playground toolchain is:

```sh
sudo apt install build-essential pkg-config libsdl2-dev libgl-dev
```

## Build

Using Make:

```sh
make
# dist/liboot.so
# dist/include/liboot.h
# dist/include/liboot_engine.h
# dist/include/liboot.hpp
```

Using CMake:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
cmake --install build-cmake --prefix "$PWD/stage"
```

Set `-DBUILD_SHARED_LIBS=OFF` for a static library. Installed CMake consumers
can link `liboot::oot`; installed `pkg-config` consumers can use `liboot`.

For a separate ASan/UBSan development build:

```sh
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_SHARED_LIBS=OFF \
  -DLIBOOT_ENABLE_SANITIZERS=ON
cmake --build build-sanitize
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-sanitize --output-on-failure
```

`LIBOOT_SANITIZERS` defaults to `address,undefined`; override it with a
comma-separated GCC/Clang sanitizer list when isolating one sanitizer. The
runtime must be available to the selected compiler, either system-wide or
through that compiler's configured library search path.

## Run the minimal host

```sh
make -C examples
./examples/engine /path/to/your/oot.z64
```

The preferred source is [examples/engine.c](../examples/engine.c).
[examples/basic.c](../examples/basic.c) demonstrates the raw compatibility
API. The engine-neutral lifecycle is:

1. Read the user-provided ROM into memory.
2. Require `oot_engine_api_version() == OOT_ENGINE_API_VERSION` before calling
   any initializer.
3. Initialize `OoTEngineConfig`, check the initializer result, call
   `oot_engine_create`, check that result, and release the ROM buffer after it
   returns.
4. Load host collision with `oot_engine_static_world_load` or a ROM scene with
   `oot_engine_scene_load`.
5. Create the single Link instance with `oot_engine_link_create`.
6. Call `oot_engine_step` at exactly 20 Hz, or use `oot_engine_advance`.
7. Render or inspect the borrowed `OoTEngineFrame`.
8. Call `oot_engine_destroy`; it also deletes an active Link.

Loading a world before Link is mandatory. `Player_Init` immediately queries
the real collision engine, so the wrapper rejects an unsafe ordering.

## Fixed simulation step

The gameplay core updates at 20 Hz: one simulation tick every 50 ms. Render
at any rate. The wrapper includes a capped accumulator and latches a button
tap received during a render frame that does not produce a simulation tick.

```c
void host_frame(float elapsed_seconds) {
    if (oot_engine_api_version() != OOT_ENGINE_API_VERSION) {
        report_api_mismatch();
        return;
    }

    OoTEngineInput input;
    OoTResult result = oot_engine_input_init(&input);
    if (result != OOT_ENGINE_RESULT_OK) {
        report_error(result);
        return;
    }
    sample_input(&input);

    uint32_t steps = 0;
    const OoTEngineFrame *frame = NULL;
    result = oot_engine_advance(
        engine, elapsed_seconds, &input, &steps, &frame);
    if (result == OOT_ENGINE_RESULT_OK && frame != NULL) {
        render_link(&frame->link, &frame->geometry,
                    frame->interpolationAlpha);
    }
}
```

In production, perform the API-version comparison once when loading the native
library rather than once per render frame. The C initializer name above is an
ergonomic macro over `oot_engine_input_init_sized`; it supplies `sizeof(input)`
and `OOT_ENGINE_API_VERSION`, and its `OoTResult` must still be checked.

Call `oot_engine_step` instead if the host already owns a fixed 20 Hz loop.
Raw `oot_link_tick` also advances exactly one tick and never accepts delta
time. `fixedStepSeconds` is a scheduling interval and accepts values from
`0.001` through `1.0` seconds; values other than the authentic `0.05` change
gameplay speed. `maxSubsteps` accepts 1 through 1000 (default 4). For either
field, zero selects the initialized default.

Buttons are sampled by the original game logic. A tap must be present for at
least one simulation tick. Hold/release edges matter for the bow, hookshot,
boomerang, bombs, shielding, and chargeable actions. Clamp stick axes to
`[-1, 1]`. `camLookX/camLookZ` represent the horizontal camera-to-Link
direction; the wrapper normalizes them and treats zero as `+Z`.

## Rendering

`OoTEngineFrame.geometry` and the raw `OoTLinkGeometryBuffers` are
renderer-neutral:

- positions and normals contain three floats per vertex;
- colors contain RGB floats per vertex;
- UVs contain two floats per vertex;
- `triTexture[t]` selects a texture for triangle `t`, or `0xFFFF` for none;
- the engine wrapper reports `numTriangles` and `triangleCapacity`; the raw
  buffer reports `numTrianglesUsed`, bounded by `OOT_GEO_MAX_TRIANGLES`.

Call `oot_engine_texture_count`/`oot_engine_texture_get` after ticks (or the
raw equivalents). Cache textures by index and upload again only when the
reported revision changes. Pixels are RGBA8. Honor `wrapS` and `wrapT`: `0`
repeat, `1` mirror, `2` clamp.

Navi wings and projectile geometry are opt-in and append to the same fixed
triangle buffer:

```c
oot_engine_set_render_flags(
    engine, OOT_ENGINE_RENDER_NAVI | OOT_ENGINE_RENDER_ACTORS);
```

The buffer does not currently identify which entity produced each triangle.
The wrapper includes projectile transforms in `OoTEngineFrame.actors`. Raw API
hosts use `oot_navi_set_render`, `oot_actor_set_render`, and `oot_actor_query`
directly.

## Collision and scenes

For a host-owned level, submit static integer triangles with
`oot_engine_static_world_load`. Coordinates are right-handed, Y-up OoT world
units and must fit signed 16-bit range. Water boxes are axis-aligned XZ
rectangles extending downward from their surface height. A world accepts at
most `OOT_ENGINE_MAX_STATIC_SURFACES` (2730) triangles and
`OOT_ENGINE_MAX_WATER_BOXES` (65,535) water boxes; water dimensions must be
positive.

For supported ROM scenes, call `oot_engine_scene_load(engine, scene, room,
&nativeResult)`. Scene loading replaces the current static world. Query
opaque and translucent room geometry with `oot_engine_scene_get_geometry`,
use `oot_engine_scene_get_spawn` for an entrance, and copy the live room
behavior with `oot_engine_scene_get_runtime`. The latter reports the real room
type/environment, echo, Lens behavior, warp restriction and camera type that
the vendored Player code is currently using. A custom-world load makes this
query return `OOT_ENGINE_RESULT_NOT_AVAILABLE`.

## Audio

Set `OoTEngineConfig.sfxCallback` before creation, or use
`oot_engine_set_callbacks`, to receive the original sound requests including
ID, pitch, volume, position, refresh, and stop events.
Despite its compatibility name, `oot_engine_voice_get` (and the raw
`oot_get_voice_sample`) returns decoded mono PCM16 for mapped gameplay SFX and
Link/Navi voice clips. `oot_engine_ocarina_note_get` supplies sample rates and
loop points for all five Ocarina notes.

For music and the full sound selector, use the process-global raw audio API
while the engine is alive. `oot_audio_sequence_play` addresses all 110 ROM
sequences on the main, fanfare, SFX, or sub player;
`oot_audio_nature_play` applies one of the 19 complete ambience IO presets;
`oot_audio_sfx_catalog_get` enumerates the seven named banks. Feed your audio
device by pulling interleaved stereo F32 with `oot_audio_render_f32`. The pull
function accepts host rates from 8 to 192 kHz and allocates no memory.
Call `oot_audio_sequence_prewarm` for every track your application may start
before opening the device (the playground prewarms all 110), so first-use
VADPCM decoding cannot block several callback buffers under the device lock.

Callbacks run synchronously inside `oot_link_tick`. Queue events for the
engine audio thread instead of calling the gameplay API recursively.
Likewise, serialize every mutable AudioSeq access, including state getters,
against the render callback.

## Current lifecycle and threading limits

- The implementation is process-global and supports one engine and one Link.
- A second `oot_engine_create` returns `OOT_ENGINE_RESULT_SINGLETON_IN_USE`.
- Calls must be serialized on one gameplay thread.
- Concurrent or callback-reentrant wrapper calls return
  `OOT_ENGINE_RESULT_BUSY`.
- Deleting/recreating Link or switching age despawns helper actors and host
  targets; recreate host attention targets afterward.
- `OoTLinkState.action` is populated from a curated set of named Player action
  functions; unclassified actions report `OOT_ACTION_OTHER`. `animId` is the
  stable one-based identity of the active `link_animetion` entry and
  `animFrame` is its current frame; zero means unknown.
- The raw `oot_global_init` has no fallible result value. Use the versioned
  wrapper for explicit result codes and owned frame buffers.

## Playground and validation

```sh
make -C test playground
make -C test engine_api_test
./test/playground /path/to/oot.z64
./test/playground /path/to/oot.z64 --suite 1000
./test/engine_api_test /path/to/oot.z64
```

Press F9 or Tab in the playground for equipment, items, worlds, audio,
rendering diagnostics, fixed-step control, and all supported ROM scenes.
