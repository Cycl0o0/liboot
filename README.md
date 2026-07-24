# liboot

[![Build](https://github.com/Cycl0o0/liboot/actions/workflows/build.yml/badge.svg)](https://github.com/Cycl0o0/liboot/actions/workflows/build.yml)
[![License: AGPL v3 or later](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![Status: pre-1.0](https://img.shields.io/badge/status-pre--1.0-orange.svg)](#limitations)

liboot runs Link's real *Ocarina of Time* gameplay code as a standalone shared
library. It takes the actual Player actor (`z_player.c`), collision engine
(`z_bgcheck.c`), and skeleton/animation system (`z_skelanime.c`) from the
[zeldaret/oot](https://github.com/zeldaret/oot) decompilation, wraps them in a
thin shim that fakes the N64 OS, and exposes a C API you can embed in another
engine. This is the same approach [libsm64](https://github.com/libsm64/libsm64)
used to extract Mario from the SM64 decompilation.

Models, textures, animations, and audio are read at runtime from a ROM you
supply. **No game asset, and no ROM, ships in this repository.**

## What it is, and what it is not

liboot is a compatibility SDK for embedding original OoT Player gameplay in a
host engine. It is:

- the genuine Player state machine, collision, animation, and audio, compiled
  for a normal desktop toolchain;
- a renderer-neutral source of geometry, textures, skeleton poses, and PCM;
- a C ABI meant to sit under Unity, Godot, Unreal, or a custom engine.

It is **not** a PC port, an emulator, a complete game, or a source of assets.
It does not draw anything itself and it does not include a ROM.

## How it works

The decompilation targets the N64. To run 22 of its translation units on a PC,
liboot closes three gaps:

1. **Missing OS.** A shim (`src/shim/`) supplies fake `PlayState`, camera, and
   save-context instances plus no-op subsystems (audio thread, messages,
   effects). The rule is *real types, fake instances; real math, fake IO*.
2. **Missing assets.** Animation headers are generated from the decomp's asset
   XMLs as segment-7 tokens; model symbols become one-command display lists
   plus a bind manifest the loader fills from the ROM at runtime (`src/gen/`).
3. **Missing symbols.** Around 180 engine functions are stubbed or minimally
   implemented — synchronous message queues, a DMA service that resolves
   segment tokens and byte-swaps animation frames, and a follow camera driven
   from the public API.

A patched `SEGMENTED_TO_VIRTUAL` distinguishes 32-bit ROM segment tokens from
native pointers, so the same decomp code walks both ROM data and host structs.

## Requirements

- A C11 compiler (GCC or Clang) and GNU Make, or CMake 3.16+.
- Linux, macOS, or another POSIX host. CI builds Linux (x86-64 and ARM64) and
  macOS, each as a shared and a static library.
- A legally obtained, compatible OoT ROM (`.z64`, `.v64`, or `.n64`), supplied
  at runtime. PAL Europe Rev 1 is the currently exercised revision.
- SDL2 and OpenGL headers only for the optional playground. The library itself
  depends on neither.

## Build

```sh
make                 # -> dist/liboot.{so,dylib} and public headers in dist/include/
make -C examples     # -> examples/engine and examples/basic
./examples/engine /path/to/oot.z64
```

Or with CMake, which also installs `liboot::oot` package metadata and a
`pkg-config` file:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ./stage
```

Pass `-DBUILD_SHARED_LIBS=OFF` for a static library. See
[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) for the sanitizer build and
full options.

## Quick start

The recommended entry point is the versioned engine API in
[`liboot_engine.h`](src/liboot_engine.h). A minimal host:

```c
#include "liboot_engine.h"

/* A flat floor made of two triangles. */
static const struct OoTSurface floor[] = {
    { 0, {{ -1000, 0, -1000 }, { -1000, 0, 1000 }, { 1000, 0, 1000 }} },
    { 0, {{ -1000, 0, -1000 }, {  1000, 0, 1000 }, { 1000, 0, -1000 }} },
};

if (oot_engine_api_version() != OOT_ENGINE_API_VERSION)
    fail("liboot API version mismatch");

OoTEngineConfig config;
oot_engine_config_init(&config);   /* check the result in real code */
config.romData = rom;              /* your ROM bytes */
config.romSize = romSize;

OoTEngine *engine = NULL;
oot_engine_create(&config, &engine);
free(rom);                         /* create() copied the ROM synchronously */

oot_engine_static_world_load(engine, floor, 2, NULL, 0);  /* before Link */
oot_engine_link_create(engine, 0.0f, 0.0f, 0.0f);
oot_engine_link_set_equipment(engine, OOT_SWORD_MASTER, OOT_SHIELD_HYLIAN,
                              OOT_TUNIC_KOKIRI, OOT_BOOTS_KOKIRI);

OoTEngineInput input;
oot_engine_input_init(&input);
const OoTEngineFrame *frame = NULL;

for (int tick = 0; tick < 20; ++tick) {
    input.stickY = tick < 12 ? 1.0f : 0.0f;   /* walk forward, then stop */
    oot_engine_step(engine, &input, &frame);  /* one 20 Hz tick */
}

printf("Link at %.1f %.1f %.1f, %u triangles\n",
       frame->link.position[0], frame->link.position[1],
       frame->link.position[2], frame->geometry.numTriangles);

oot_engine_destroy(engine);
```

Load a world or a ROM scene **before** creating Link: `Player_Init` probes
collision immediately, and the wrapper rejects the unsafe ordering. The full
runnable version is [examples/engine.c](examples/engine.c);
[examples/basic.c](examples/basic.c) shows the low-level API.

For a task-by-task cookbook — rendering, textures, scenes, audio, actors,
targets, the Ocarina, and the C++/C# bindings — see
[docs/USAGE.md](docs/USAGE.md).

## What works today

**Initialization.** `oot_global_init` accepts `.z64/.v64/.n64`, finds
`link_animetion`, `object_link_boy`, and `object_link_child` through the ROM's
own DMADATA (not fixed indices), Yaz0-decompresses, relocates both flex
skeletons, and binds assets.

**Movement and state.** The real Player state machine: idle with fidget
animations, camera-relative running at authentic caps (adult 6.0, child 5.5
u/f) and accel/decel curves, runtime adult/child switching, save-context-backed
health and magic, damage through `Player_InflictDamage`, and the underwater
drown timer.

**Equipment and items.** Swords, shields, tunics, boots, the Ocarina, and the
real projectile actors — arrows, bombs, hookshot, and boomerang.

**Collision and scenes.** Host triangles and water boxes build a real
`CollisionHeader`; `z_bgcheck` handles floors, walls, ceilings, raycasts,
swimming, diving, and iron boots. ROM scenes load real collision and geometry,
support multi-room dungeon rendering and door-driven room transitions, and
report per-scene music and ambience.

**Rendering.** An F3DZEX2 display-list interpreter walks the flex skeleton with
the game's own limb-draw overrides, so the model follows equipment and age. The
texture pipeline (SETTIMG/SETTILE/LOADBLOCK/LOADTLUT; rgba16, ci4/ci8, ia, i
formats) decodes every referenced texture into an RGBA cache. Output is a
renderer-neutral triangle stream with per-vertex shade alpha and per-triangle
cull/alpha-test/decal flags.

**Audio.** Every SFX request the Player code makes is forwarded through a
callback with pitch, volume, reverb, and position. A native mixer renders the
ROM's 110 sequences through the four retail players, 38 soundfonts, and seven
sample banks to interleaved stereo F32 at the host rate, allocation-free, with
fades, all 19 nature-ambience presets, and a 1,259-entry SFX selector. An
opt-in proximity driver plays the OoT battle theme while a hostile enemy is
near Link, using the game's own enemy-proximity trigger.

**Targeting.** Host-owned Z-targets backed by OoT's real Attention system, with
lock, strafing, auto-facing, and release. The real EnElf actor supplies Navi.

**Ocarina.** Canonical note patterns for all twelve songs, longest-tail match
recognition of a played sequence, and the five ROM instrument samples.

For exact contracts, read the public headers and
[docs/API_REFERENCE.md](docs/API_REFERENCE.md).

## Documentation

| Document | Contents |
| --- | --- |
| [Getting started](docs/GETTING_STARTED.md) | Build, minimal host, fixed step, rendering, collision, audio. |
| [Usage cookbook](docs/USAGE.md) | Task-by-task examples for every subsystem, plus C++ and C#. |
| [API reference](docs/API_REFERENCE.md) | Every exported function, struct, enum, and constant. |
| [Universal SDK](docs/UNIVERSAL_SDK.md) | The engine-neutral wrapper, ABI rules, instance model, roadmap. |
| [Engine integration](docs/ENGINE_INTEGRATION.md) | Unity, Godot, Unreal, C/C++, C#, Rust, Python patterns. |
| [Fidelity traces](docs/FIDELITY.md) | The record/compare runner and what a trace does and does not prove. |
| [Bindings](bindings/README.md) | C++ RAII and C#/Unity P/Invoke starters. |
| [Notices](NOTICE.md) | AGPL scope, vendored-source status, ROM policy, attribution. |

## Two APIs

- [`liboot_engine.h`](src/liboot_engine.h) — the **recommended** API. An opaque
  `OoTEngine` handle, size-tagged config and input for ABI-stable growth,
  explicit `OoTResult` codes, engine-owned frame views, and one-tick or
  accumulated stepping. Start here.
- [`liboot.h`](src/liboot.h) — the low-level compatibility API. Process-global
  lifecycle and caller-owned geometry buffers. Use it only for a feature the
  wrapper has not yet surfaced.

## Playground

```sh
make -C test playground     # needs SDL2 + OpenGL
./test/playground /path/to/oot.z64
```

An interactive workbench, not a single demo: live health, magic, inputs,
loadout, actor and geometry counts, and a diagnostics panel with state flags,
animation data, and the recent SFX log. Press **F9** or **Tab** for the pages
(Play, Link, Items, World, Audio, Render); **F5**/**F6** switch between the test
arena and eight ROM scenes. Headless modes run before SDL/OpenGL init, so they
work in CI:

```sh
./test/playground rom.z64 --frames 3000        # scripted arena loop
./test/playground rom.z64 --scene-frames 1600  # walk every scene
./test/playground rom.z64 --suite 1000         # full API checks + N stress ticks
```

## Tests

ROM-free regressions run anywhere and gate CI:

```sh
make -C test engine_init_test rom_util_test audio_overflow_test fidelity_runner
./test/engine_init_test        # ABI/lifecycle
./test/rom_util_test           # DMADATA/Yaz0/byteswap
./test/audio_overflow_test     # mixer bounds
./test/fidelity_runner --self-test
```

ROM-backed tests (`headless`, `equip_test`, `audio_catalog_test`,
`audio_sequence_test`) take a ROM path. The fidelity runner records and replays
a deterministic numeric/hash trace:

```sh
./test/fidelity_runner rom.z64 --record local.trace
./test/fidelity_runner rom.z64 --compare local.trace
```

A trace is an anti-regression baseline, not proof of a match with retail OoT.
See [docs/FIDELITY.md](docs/FIDELITY.md).

## Limitations

- One engine and one Link per process. The decomp core still uses globals; the
  opaque handle is a migration boundary, not multi-instance support yet.
- No general enemy/actor runtime and no dynamic collision.
- First-pass scene loader: main headers only, no exits, alternate day/age
  headers, or animated materials; prerendered JPEG backgrounds are not drawn.
- Geometry is a triangle stream without entity, material, pass, blend, or depth
  metadata.
- Behavior is compiled against the NTSC 1.2 decomp paths even when structurally
  compatible assets come from another retail ROM. Do not claim untested ROM
  compatibility.

The [roadmap](docs/UNIVERSAL_SDK.md#roadmap) tracks multi-instance support, a
published compatibility matrix, richer draw batches, and packaged engine plugins.

## License and provenance

Original liboot code and documentation, by **Cycl0o0**, are licensed under the
[GNU Affero General Public License v3.0 or later](LICENSE).

The `src/decomp/` subtree is vendored from `zeldaret/oot`, which does not
currently declare a repository-wide license; liboot does not relicense it. Read
[NOTICE.md](NOTICE.md) before redistributing. No Nintendo ROM or asset is
included, and a ROM must never be committed to a project that uses liboot.
liboot is unaffiliated with Nintendo.
