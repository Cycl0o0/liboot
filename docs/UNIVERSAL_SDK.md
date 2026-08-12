# Engine API design

[`src/liboot_engine.h`](../src/liboot_engine.h) is the C boundary for new host
engines and foreign-function bindings. It owns lifecycle checks, scheduling,
and output buffers without adding a dependency on Unity, Godot, Unreal, or any
other host.

The API is pre-1.0. Check `oot_engine_api_version()` before initializing a
structure, use the supplied result-returning initializers, zero direct FFI
outputs, and handle every `OoTResult`.

## What the engine API provides

- An opaque `OoTEngine` owner instead of exposing the raw global lifecycle.
- Size-tagged configuration and input structures for ABI-compatible growth.
- Explicit result codes and human-readable `oot_engine_result_string` output.
- Engine-owned Link geometry, actor snapshots, skeleton state, and Navi state.
- Exact one-tick stepping plus a capped PAL 60 ms elapsed-time accumulator.
- User-data-aware debug and SFX callbacks.
- A versioned limits/capability query for dynamic hosts.
- Static and dynamic collision, water, layered ROM scenes, transition events,
  host actors, spawn points, targets, equipment, items, health, magic, texture
  views, and mapped gameplay-SFX/voice PCM views.
- A C boundary usable from C++, C#, Rust, Python, and engine plugins.

[`src/liboot.h`](../src/liboot.h) remains available for existing raw-API hosts
and for features that the engine API has not exposed. Do not mix its lifecycle
calls with an active `OoTEngine`.

## Minimal lifecycle

```c
#include <liboot_engine.h>

if (oot_engine_api_version() != OOT_ENGINE_API_VERSION) {
    fail("liboot API version mismatch");
}

OoTEngineConfig config;
OoTResult result = oot_engine_config_init(&config);
if (result != OOT_ENGINE_RESULT_OK) {
    fail(oot_engine_result_string(result));
}
config.romData = rom_bytes;
config.romSize = rom_size;

OoTEngine *engine = NULL;
result = oot_engine_create(&config, &engine);
if (result != OOT_ENGINE_RESULT_OK) {
    fail(oot_engine_result_string(result));
}

const struct OoTSurface floor[] = {
    { 0, {{-1000, 0, -1000}, {-1000, 0, 1000}, {1000, 0, 1000}} },
    { 0, {{-1000, 0, -1000}, { 1000, 0, 1000}, {1000, 0, -1000}} },
};
result = oot_engine_static_world_load(engine, floor, 2, NULL, 0);
if (result != OOT_ENGINE_RESULT_OK) {
    oot_engine_destroy(engine);
    fail(oot_engine_result_string(result));
}

result = oot_engine_link_create(engine, 0.0f, 0.0f, 0.0f);
if (result != OOT_ENGINE_RESULT_OK) {
    oot_engine_destroy(engine);
    fail(oot_engine_result_string(result));
}

OoTEngineInput input;
result = oot_engine_input_init(&input);
if (result != OOT_ENGINE_RESULT_OK) {
    oot_engine_destroy(engine);
    fail(oot_engine_result_string(result));
}
input.stickY = 1.0f;

const OoTEngineFrame *frame = NULL;
result = oot_engine_step(engine, &input, &frame);
if (result == OOT_ENGINE_RESULT_OK) {
    upload_triangles(&frame->geometry);
}

oot_engine_destroy(engine);
```

The ROM is copied during `oot_engine_create`, so the host may release its input
buffer when that function returns. Accepted buffers are
`OOT_ENGINE_MIN_ROM_SIZE` (`0x1060`) bytes through `OOT_ENGINE_MAX_ROM_SIZE`
(256 MiB), inclusive. Configured
`fixedStepSeconds` values must be in `[0.001, 1.0]` seconds and
`maxSubsteps` in `[1, 1000]`; setting either field to zero selects its default.
A custom world accepts no more than `OOT_ENGINE_MAX_STATIC_SURFACES` (2730)
triangles and `OOT_ENGINE_MAX_WATER_BOXES` (65,535) water boxes. All
pointers in an `OoTEngineFrame` belong to the engine and remain valid only
until the next mutating call. Texture and PCM views are also borrowed; copy or
upload data before retaining it beyond the documented lifetime.

Always load static collision or a successful ROM scene before creating Link.
The original Player initialization immediately probes collision; the wrapper
returns `OOT_ENGINE_RESULT_NOT_AVAILABLE` for the unsafe ordering.

## Threading and instance model

The decompiled code still declares writable globals. Shared-library builds
isolate that writable module image per `OoTEngine` and switch images under the
wrapper guard. Engines can therefore own independent worlds and Links, but
their calls remain serialized rather than executing concurrently. Static
archives cannot isolate only liboot's data after the linker merges it into the
host executable, so they advertise `OOT_ENGINE_CAPABILITY_PROCESS_SINGLETON`.

- Query `oot_engine_get_limits` and inspect `MULTI_INSTANCE` or
  `PROCESS_SINGLETON` instead of inferring the model from a filename.
- Send normal calls from one gameplay thread; concurrent calls return `BUSY`.
- Callbacks must copy their payload and must not call liboot recursively.
- Do not mix raw lifecycle calls with calls on an `OoTEngine`.
- With more than one engine, use the handle-guarded engine audio functions.
  Raw mutable `oot_audio_*` calls have no engine selector.

## Engine adapter shape

A thin adapter in any host engine normally owns five pieces:

1. A ROM picker/loader that passes bytes to `oot_engine_create`.
2. A fixed-update node that translates host input to `OoTEngineInput`.
3. A collision exporter that quantizes host triangles into `OoTSurface`.
4. A renderer that uploads frame arrays and RGBA textures.
5. An audio queue that copies synchronous SFX events for the mixer thread.

Keep coordinate conversion in that adapter. liboot uses a right-handed,
Y-up coordinate system and OoT world units. The host supplies a normalized
horizontal camera-to-Link direction in `camLookX/camLookZ`.

See [ENGINE_INTEGRATION.md](ENGINE_INTEGRATION.md) for detailed Unity, Godot,
Unreal, C/C++, C#, Rust, and Python patterns. Starter C++ and C# bindings live
under [`bindings/`](../bindings/).

## ABI rules for bindings

- Call `oot_engine_api_version()` and require the exact flat API version your
  binding was generated for, before initializing any structure.
- In C/C++, use the result-returning `oot_engine_config_init()` and
  `oot_engine_input_init()` convenience macros instead of assuming zeroes will
  always select the defaults. They forward the destination size and
  `OOT_ENGINE_API_VERSION` to the exported sized functions.
- Foreign-function bindings call the exported
  `oot_engine_config_init_sized(config, sizeof_config, expected_api_version)`
  and `oot_engine_input_init_sized(input, sizeof_input,
  expected_api_version)` functions directly, after the runtime version check,
  and check both returned `OoTResult` values. The convenience macro names are
  not exported function symbols.
- Preserve native pointer and `size_t` widths in foreign-function bindings.
- Keep managed callback delegates rooted for the lifetime of the engine.
- Copy callback event values immediately; callback pointers are borrowed.
- Treat frame pointers as read-only and short-lived.
- Translate `OoTResult`; never infer success from a non-null output alone.

## Limits and truncation

`oot_engine_get_limits` is process-independent and does not require an engine.
Initialize `OoTEngineLimits` with `OOT_ENGINE_LIMITS_INIT`; its capability bits
let dynamically loaded hosts distinguish available contracts without guessing
from a library filename.

The limits are capacities, not observed counts. When the combined Link, Navi,
and optional actor stream exceeds its triangle capacity,
`OoTEngineFrame.linkGeometryTruncated` is nonzero. For a loaded scene,
`oot_engine_scene_get_dropped_triangles` returns the exact number of otherwise
valid triangles omitted from its copied geometry. Existing actor-list
truncation remains separately reported by `actorListTruncated`.

Choose frame and scene triangle capacities in `OoTEngineConfig`. Geometry
batches split those arrays at source-entity and render-state boundaries, with
material, texture, render-pass, blend, depth, culling, and original RDP mode
metadata available to renderer integrations.

## Remaining engineering work

The main unresolved work is evidence and breadth: reference-runtime fidelity
traces, gameplay validation for ROM revisions other than PAL 1.1, more native
actor overlays, GPU helpers for room-image compositing and animated materials,
and maintained packages for specific game engines. The public headers and the
[engine integration guide](ENGINE_INTEGRATION.md) define the current boundary.
