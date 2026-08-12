# liboot

[![Build](https://github.com/Cycl0o0/liboot/actions/workflows/build.yml/badge.svg)](https://github.com/Cycl0o0/liboot/actions/workflows/build.yml)
[![License: AGPL v3 or later](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![Status: pre-1.0](https://img.shields.io/badge/status-pre--1.0-orange.svg)](#scope-and-fidelity-boundaries)

liboot runs selected *Ocarina of Time* gameplay systems inside another engine.
It compiles Player, collision, animation, actor, scene, and math code from the
[zeldaret/oot](https://github.com/zeldaret/oot) decompilation into a C11 library,
then exposes host-readable state, geometry, collision, events, and audio.

The host supplies a legally obtained PAL 1.1 ROM, controls, a world or ROM
scene, and rendering and audio devices. The authentic PAL schedule is one
gameplay tick every 60 ms. Shared-library builds can isolate several engines;
each engine owns one Link and its native gameplay state.

No ROM or extracted game asset is included. liboot is not an emulator, PC port,
complete game, or renderer.

## Try it

You need a C11 compiler, GNU Make or CMake 3.16+, and a legally obtained PAL
1.1 ROM (`.z64`, `.v64`, or `.n64`). CI builds Linux, macOS, and Windows
UCRT64 artifacts. The optional playground also needs SDL2 and OpenGL. ROM-free
checks require Python 3.8+; pass `-DBUILD_TESTING=OFF` to CMake for a
library-only build without Python.

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
| Input and camera direction | Player updates, equipment, items, health, magic, targeting, and a fixed-step accumulator |
| Static world, moving objects, or scene selection | Floor, wall, ceiling, water, raycasts, dynamic collision, room state, and transition events |
| GPU resources and draw submission | Triangles, textures, source/material batches, render state, room images, and animated-material references |
| Audio device and scheduling | SFX events, decoded samples, and portable interleaved stereo mixing |
| Enemy AI, health, and game rules | Native attention entries, arbitrary host actor records, scene actor catalogs, and weapon-contact events |

Borrowed frame and texture pointers are short-lived. Upload or copy their data
before the next mutating call. Serialize calls that use an `OoTEngine`, including
audio rendering, and do not re-enter the library from callbacks. Raw AudioSeq
controls belong to the raw lifecycle and have no engine selector.

## Current capabilities

| Area | Available now |
| --- | --- |
| Player | Adult and child movement, fidget states, equipment, usable items, damage, health, magic, swimming, diving, iron boots, and the underwater timer |
| Items and actors | Swords, shields, tunics, boots, Ocarina input, arrows, bombs, hookshot, boomerang, Navi, scene actor entries, and up to 64 host-controlled actors with attention and hit contacts |
| World | Host static collision, water boxes, and transformed dynamic collision objects; ROM scene collision, rooms, doors, spawn points, exits, void-out events, music, ambience, lighting, and fog |
| Scenes | Child/adult and day/night headers, explicit layer selection, room swaps, scene transition events, prerender-image records, and animated-material references |
| Rendering | Configurable Link and scene buffers; Link, Navi, projectile, actor, and room source ranges; material, texture, pass, blend, depth, cull, alpha-test, combine-mode, and color metadata |
| Audio | Parameterized SFX callbacks, voice/Ocarina PCM, 110 ROM sequences, four sequence players, 38 soundfonts, seven sample banks, 19 nature presets, and a 1,259-entry SFX catalog |
| Integration | C ABI, C++11 RAII, C# P/Invoke, CMake and `pkg-config` packages, Linux/macOS shared libraries, and Windows UCRT64 DLLs |

The public headers define the exact contracts and limits. See the
[API reference](docs/API_REFERENCE.md) for the exported surface.

## Choose an API

| API | Use it for |
| --- | --- |
| [`liboot_engine.h`](src/liboot_engine.h) | New integrations. It provides an opaque owner, result codes, size-tagged inputs, engine-owned frame buffers, and fixed or accumulated stepping. |
| [`liboot.h`](src/liboot.h) | Existing raw-lifecycle integrations and low-level helpers, including immutable audio catalogs, Ocarina tables, the direct skeleton-pose getter, and actor spawning. |

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
| Check a ROM revision or add a profile | [ROM compatibility](docs/ROM_COMPATIBILITY.md) |
| Work on the library or prepare a release | [Development](docs/DEVELOPMENT.md) and [releasing](docs/RELEASING.md) |

The [documentation index](docs/README.md) separates guides from reference
material.

To identify a local ROM without uploading or rewriting it:

```sh
tools/identify-rom.py --json /path/to/oot.z64
```

The tool reports header metadata and a SHA-256 over canonical big-endian bytes,
so equivalent `.z64`, `.v64`, and `.n64` dumps share one identity.

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

The local gate matches the ROM-free checks used by CI and release builds:

```sh
make check
make sanitizers
make fuzz-smoke
```

The parser fuzz targets use generated or arbitrary bytes and are documented in
[`fuzz/README.md`](fuzz/README.md).

ROM-backed tests cover initialization, equipment, scenes, rendering, the audio
catalog and sequencer, and longer headless runs. A fidelity trace can detect a
regression between liboot builds:

```sh
./test/fidelity_runner oot.z64 --record local.trace
./test/fidelity_runner oot.z64 --compare local.trace
```

A liboot-generated trace is not proof of agreement with retail OoT. The
[fidelity guide](docs/FIDELITY.md) explains the distinction.

## Scope and fidelity boundaries

- PAL 1.1 is the compiled and ROM-backed tested revision. The ROM identifier
  recognizes all eight N64 retail region/revision hashes, but recognition is
  not a gameplay-compatibility claim.
- Shared libraries isolate writable decompilation state per `OoTEngine`.
  Static archives are still single-instance because their writable sections
  are linked into the host executable. The raw lifecycle API remains
  process-global and must not be mixed with active engine owners.
- Host actors participate in attention, room filtering, targeting, and Link or
  projectile contact detection. Their AI, health, animation, and rendering are
  host-owned; liboot does not execute arbitrary retail actor overlays.
- Scene exit and void events are reported to the host. Loading the destination
  scene, choosing a spawn, and applying game-specific save rules remain host
  responsibilities. Cutscene header variants are not selected automatically.
- Room-image and animated-material APIs expose validated source data and render
  metadata. Hosts still own camera compositing and GPU-side material animation.
- The sample-mixing stage uses fixed-point resampling, gain, pan, saturation,
  and delay reverb, with canonical S16 output and exact F32 conversion.
  Sequence timing, envelope targets, fades, portamento, and procedural
  oscillators still use floating-point control logic. Output has not been
  verified as bit-exact N64 RSP microcode emulation.
- Windows support targets x86-64 MSYS2 UCRT64/MinGW. An MSVC build is not part
  of the release matrix.

## License and provenance

Code and documentation written for liboot by **Cycl0o0** are licensed under the
[GNU Affero General Public License v3.0 or later](LICENSE).

The selected files under `src/decomp/` are vendored from `zeldaret/oot`, which
does not currently declare a repository-wide license. liboot does not relicense
that material. Read [NOTICE.md](NOTICE.md) before redistributing.

No Nintendo ROM or extracted asset is included. Users must supply their own
legally obtained compatible ROM. liboot is an independent project and is not
affiliated with or endorsed by Nintendo.
