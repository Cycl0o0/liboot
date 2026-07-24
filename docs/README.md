# liboot documentation

Start with the [project README](../README.md) for what liboot is, how it works,
and a quick-start. This directory holds the reference material.

## Guides

- **[Getting started](GETTING_STARTED.md)** — requirements, the Make and CMake
  builds (including the sanitizer build), the minimal host, the fixed
  simulation step, rendering, collision, scenes, and audio.
- **[Usage cookbook](USAGE.md)** — task-by-task, copy-pasteable examples for
  every subsystem: lifecycle, stepping, equipment, geometry, textures,
  collision, scenes, rooms, targeting, actors, audio, the Ocarina, and the
  C++/C# bindings.
- **[Engine integration](ENGINE_INTEGRATION.md)** — Unity, Godot, Unreal,
  C/C++, C#, Rust, Python, and custom-engine adapter patterns, with coordinate
  conversion and threading.
- **[Universal SDK](UNIVERSAL_SDK.md)** — the design of the engine-neutral
  layer, ABI rules for bindings, the instance model, and the roadmap.
- **[Fidelity traces](FIDELITY.md)** — the deterministic record/compare runner
  and what a trace does and does not prove.

## Reference

- **[API reference](API_REFERENCE.md)** — every exported function, struct,
  enum, and constant in both public headers.
- [`src/liboot_engine.h`](../src/liboot_engine.h) — the engine-neutral API
  (recommended), normative.
- [`src/liboot.h`](../src/liboot.h) — the low-level compatibility API,
  normative.
- [Language bindings](../bindings/README.md) — C++ RAII and C#/Unity P/Invoke
  starters.

## Which API do I use?

Use the **engine API** (`liboot_engine.h`) for anything new. It gives you an
opaque handle, explicit error codes, engine-owned frame buffers, and a stable
ABI. Drop to the **low-level API** (`liboot.h`) only for a feature the engine
wrapper has not surfaced yet — full music playback and the AudioSeq mixer, the
SFX catalog, the Ocarina song table, the skeleton pose getter, and direct
actor spawning currently live there.

## A note on the docs

The public headers are the normative contract. These documents track the
headers, but only the compiler enforces them — when a signature here disagrees
with the header, the header wins. No ROM or extracted game asset is included in
this repository or required to build the library; a ROM is supplied by the user
at runtime and must never be committed to a project that uses liboot.
