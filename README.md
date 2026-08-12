# liboot

[![Build](https://github.com/Cycl0o0/liboot/actions/workflows/build.yml/badge.svg)](https://github.com/Cycl0o0/liboot/actions/workflows/build.yml)
[![License: AGPL v3 or later](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![Status: pre-1.0](https://img.shields.io/badge/status-pre--1.0-orange.svg)](#limitations)

liboot embeds Link's *Ocarina of Time* gameplay in another engine. It compiles
selected Player, collision, animation, actor, and math code from the
[zeldaret/oot](https://github.com/zeldaret/oot) decompilation into a C11 library,
then adapts its rendering and audio requests for a host application.

The host supplies a ROM, controls, collision or a ROM scene, and the rendering
and audio backends. liboot advances the simulation in fixed ticks (20 Hz by
default) and returns Link's state, triangle and texture data, actor snapshots,
and audio output.

No ROM or extracted game asset is included. liboot is not an emulator, PC port,
complete game, or renderer.

## Try it

You need a C11 compiler, GNU Make or CMake 3.16+, and a legally obtained
compatible ROM (`.z64`, `.v64`, or `.n64`). Linux and macOS are tested in CI;
the optional playground also needs SDL2 and OpenGL.

```sh
make
make -C examples
./examples/engine /path/to/oot.z64
```

The example loads a flat collision floor, creates Link, runs 20 simulation
ticks, and prints the resulting position and geometry count. Its source is in
[`examples/engine.c`](examples/engine.c).

For an interactive renderer and diagnostics UI:

```sh
make -C test playground
./test/playground /path/to/oot.z64
```

The playground can also run headlessly. `--frames`, `--scene-frames`, `--suite`,
and `--features` select its scripted checks.

The CMake build installs a `liboot::oot` package target and a `liboot.pc` file:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ./stage
```

Set `-DBUILD_SHARED_LIBS=OFF` for a static library.

## Host and library responsibilities

| The host owns | liboot supplies |
| --- | --- |
| ROM selection and storage | ROM validation, byte-order handling, DMADATA lookup, Yaz0 decompression, and runtime asset binding |
| Input and camera direction | One Player update per fixed tick (20 Hz by default), including movement, equipment, items, health, magic, and targeting |
| Static collision or scene selection | OoT floor, wall, ceiling, water, and raycast handling; room state, door metadata, and host-requested room swaps |
| GPU resources and draw submission | Renderer-independent triangles, UVs, shade color/alpha, texture views, and render flags |
| Audio device and scheduling | SFX events, decoded voice and instrument samples, and interleaved stereo F32 sequence mixing |
| Game-specific enemies and rules | Link, Navi, selected projectile actors, and snapshots of supported actors |

Borrowed frame and texture pointers are short-lived. Upload or copy their data
before the next mutating call. Serialize engine/gameplay calls on one thread,
and do not re-enter the library from callbacks. The raw AudioSeq control and
render calls may use an audio thread when the host locks their shared state.

## Current capabilities

| Area | Available now |
| --- | --- |
| Player | Adult and child movement, fidget states, equipment, usable items, damage, health, magic, swimming, diving, iron boots, and the underwater timer |
| Items and actors | Swords, shields, tunics, boots, Ocarina input, arrows, bombs, hookshot, boomerang, Navi, and host-owned Z-targets |
| World | Host triangle collision and water boxes; ROM scene collision, geometry, spawn points, rooms, doors, music, ambience, lighting, and fog metadata |
| Rendering | Link, Navi, projectile, and room geometry; RGBA texture decoding; per-vertex alpha and per-triangle cull, alpha-test, and decal flags |
| Audio | Engine API: parameterized SFX callbacks and voice/Ocarina PCM views. Raw API: 110 ROM sequences, four sequence players, 38 soundfonts, seven sample banks, 19 nature presets, and a 1,259-entry SFX catalog. |
| Integration | C ABI, C++11 RAII wrapper, C# P/Invoke binding, CMake package metadata, and `pkg-config` metadata |

The public headers define the exact contracts and limits. See the
[API reference](docs/API_REFERENCE.md) for the exported surface.

## Choose an API

| API | Use it for |
| --- | --- |
| [`liboot_engine.h`](src/liboot_engine.h) | New integrations. It provides an opaque owner, result codes, size-tagged inputs, engine-owned frame buffers, and fixed or accumulated stepping. |
| [`liboot.h`](src/liboot.h) | Existing integrations or features not yet exposed by the engine API, including direct AudioSeq control, the SFX catalog, Ocarina tables, the direct skeleton-pose getter, and actor spawning. |

Only one API may own the process lifecycle at a time. Do not mix raw lifecycle
calls with an active `OoTEngine`.

## Documentation

| If you need to... | Read |
| --- | --- |
| Build the library and run a first host | [Getting started](docs/GETTING_STARTED.md) |
| Copy a focused example for one subsystem | [Usage cookbook](docs/USAGE.md) |
| Look up a function, struct, enum, or limit | [API reference](docs/API_REFERENCE.md) |
| Integrate with C/C++, C#, Unity, Godot, Unreal, Rust, or Python | [Engine integration](docs/ENGINE_INTEGRATION.md) |
| Understand ABI and ownership decisions | [Engine API design](docs/UNIVERSAL_SDK.md) |
| Record or compare deterministic runs | [Fidelity traces](docs/FIDELITY.md) |
| Use the C++ or C# starters | [Language bindings](bindings/README.md) |

The [documentation index](docs/README.md) separates guides from reference
material.

## How it works

The selected decompilation units still expect N64 services and segmented
addresses. liboot supplies the minimum host environment needed to run them:

1. `src/shim/` provides the `PlayState`, camera, save context, message queues,
   DMA behavior, and placeholders for systems outside liboot's scope.
2. `src/gen/` turns decompilation asset declarations into ROM-backed symbols.
   The loader resolves and binds those symbols when an engine is created.
3. `src/gfx_adapter.c` interprets F3DZEX2 display lists into host-readable
   triangles and textures instead of sending commands to an N64 renderer.

A patched `SEGMENTED_TO_VIRTUAL` distinguishes 32-bit ROM segment tokens from
native pointers, allowing the selected game code to traverse both ROM data and
host structures.

The architecture follows the same broad extraction model as
[libsm64](https://github.com/libsm64/libsm64), but liboot contains no libsm64
source.

## Tests

ROM-free checks run in public CI:

```sh
make -C test engine_init_test rom_util_test audio_overflow_test fidelity_runner
./test/engine_init_test
./test/rom_util_test
./test/audio_overflow_test
./test/fidelity_runner --self-test
```

ROM-backed tests cover initialization, equipment, scenes, rendering, the audio
catalog and sequencer, and longer headless runs. A fidelity trace can detect a
regression between liboot builds:

```sh
./test/fidelity_runner oot.z64 --record local.trace
./test/fidelity_runner oot.z64 --compare local.trace
```

A liboot-generated trace is not proof of agreement with retail OoT. The
[fidelity guide](docs/FIDELITY.md) explains the distinction.

## Limitations

- The decompilation core is process-global: one engine and one Link may exist in
  a process.
- PAL 1.1 is the currently exercised ROM revision. Gameplay code is compiled
  from the NTSC 1.2 decompilation paths, so other ROM revisions are not claimed
  compatible.
- Static host collision is replaced as one world; dynamic collision objects are
  not supported.
- The actor support is limited to Link, Navi, selected projectiles, and
  host-provided targeting. This is not a general enemy or actor runtime.
- Scene loading handles main headers and rooms, but not alternate age/day
  headers, exits, void-out transitions, animated materials, or prerendered JPEG
  backgrounds.
- Link and actor geometry uses fixed capacities and has no source-entity or
  material ranges. Scene geometry exposes one opaque/translucent split, not
  per-batch material, blend, or depth metadata.
- The audio mixer is a native approximation, not bit-exact N64 RSP emulation.
- Windows is not currently a supported build or release target.

The [roadmap](docs/UNIVERSAL_SDK.md#roadmap) covers context isolation, ROM
profiles, richer draw batches, host callbacks, audio fidelity, and packaged
engine integrations.

## License and provenance

Code and documentation written for liboot by **Cycl0o0** are licensed under the
[GNU Affero General Public License v3.0 or later](LICENSE).

The selected files under `src/decomp/` are vendored from `zeldaret/oot`, which
does not currently declare a repository-wide license. liboot does not relicense
that material. Read [NOTICE.md](NOTICE.md) before redistributing.

No Nintendo ROM or extracted asset is included. Users must supply their own
legally obtained compatible ROM. liboot is an independent project and is not
affiliated with or endorsed by Nintendo.
