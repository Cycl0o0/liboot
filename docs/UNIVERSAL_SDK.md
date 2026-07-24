# Universal engine SDK

liboot's universal layer is a small, engine-neutral C ABI around the current
Ocarina of Time Player core. It is designed to sit below Unity, Godot, Unreal,
custom C/C++ engines, and foreign-function interfaces without making any of
those engines a dependency of the library.

The foundation is available in [`src/liboot_engine.h`](../src/liboot_engine.h).
It is intentionally a pre-1.0 API: integrations should check
`oot_engine_api_version()` against `OOT_ENGINE_API_VERSION` before any
initializer, initialize configuration and input with the supplied
result-returning helpers, zero output structures before direct FFI calls, and
handle every `OoTResult`.

## What the foundation provides

- An opaque `OoTEngine` owner instead of exposing the raw global lifecycle.
- Size-tagged configuration and input structures for ABI-compatible growth.
- Explicit result codes and human-readable `oot_engine_result_string` output.
- Engine-owned Link geometry, actor snapshots, skeleton state, and Navi state.
- Exact one-tick stepping plus a capped 20 Hz elapsed-time accumulator.
- User-data-aware debug and SFX callbacks.
- Static collision, water, ROM scenes, spawn points, targets, equipment,
  items, health, magic, texture views, and mapped gameplay-SFX/voice PCM views.
- A stable C boundary suitable for C++, C#, Rust, Python, and engine plugins.

The original functions in [`src/liboot.h`](../src/liboot.h) remain available
as the low-level compatibility API. New integrations should begin with
`liboot_engine.h` and drop to `liboot.h` only when they need a feature that the
wrapper has not exposed yet.

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

The wrapper serializes access and rejects callback re-entry, but the current
decompiled core still stores game state in process-global objects. Therefore:

- only one `OoTEngine` may exist in a process;
- all normal calls should come from one gameplay thread;
- callbacks must copy their payload and must not call liboot recursively;
- do not mix raw lifecycle calls with calls on an `OoTEngine`.

The opaque handle is a migration boundary, not a claim that the current core
already supports independent simultaneous worlds.

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

See [ENGINE_INTEGRATION.md](ENGINE_INTEGRATION.md) for complete Unity, Godot,
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

## Roadmap

The next universal-SDK milestones are:

1. Move every decomp/shim global behind a true context so multiple engines and
   Links can coexist safely.
2. Add generated ROM profiles and a published compatibility matrix instead of
   relying on structural asset discovery alone.
3. Replace fixed geometry limits with capacity queries and draw batches that
   identify entity, material, pass, alpha, blend, depth, and culling state.
4. Add allocator, logger, filesystem, dynamic-collision, hit-volume, and host
   world-event interfaces.
5. Improve the internal AudioSeq mixer's envelopes, filters, effects and
   reference-capture fidelity, and expose richer spatial sound-instance
   metadata to hosts.
6. Validate and package native builds for Linux, macOS, Windows, x86-64, and
   ARM64, followed by maintained Unity, Godot, Unreal, Rust, and Python
   packages.

These are roadmap items, not promises about the current release. The exact
working boundary is documented in the public headers and the
[engine integration guide](ENGINE_INTEGRATION.md).
