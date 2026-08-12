# liboot documentation

New to the project? Build and run the minimal host in
[Getting started](GETTING_STARTED.md). If you already have liboot running, use
the [usage cookbook](USAGE.md) for focused examples or the
[API reference](API_REFERENCE.md) for exact signatures and limits.

## Guides

- [Getting started](GETTING_STARTED.md): requirements, Make and CMake builds,
  the minimal host, timing, rendering, collision, scenes, and audio.
- [Usage cookbook](USAGE.md): examples organized by task and subsystem.
- [Engine integration](ENGINE_INTEGRATION.md): ownership, coordinates,
  threading, deployment, and adapter patterns for common engines and languages.
- [Engine API design](UNIVERSAL_SDK.md): ABI decisions, instance
  model, and roadmap.
- [Fidelity traces](FIDELITY.md): deterministic record/compare runs and the
  evidence required for a compatibility claim.
- [ROM compatibility](ROM_COMPATIBILITY.md): tested revisions, evidence levels,
  the local ROM identifier, and the process for adding a profile.
- [Development](DEVELOPMENT.md): validation, source boundaries, ABI changes,
  and vendored/generated files.
- [Releasing](RELEASING.md): version, provenance, test, and packaging gates.

## Reference

- [API reference](API_REFERENCE.md): exported functions, structures, enums, and
  constants from both public headers.
- [`src/liboot_engine.h`](../src/liboot_engine.h): recommended API for new
  integrations; this header is the normative contract.
- [`src/liboot.h`](../src/liboot.h): process-global compatibility API for
  features not yet available through the engine API.
- [Language bindings](../bindings/README.md): C++11 RAII and C# P/Invoke
  starters.
- [Changelog](../CHANGELOG.md): user-visible changes by version.

## Which API?

Start with `liboot_engine.h`. Use `liboot.h` only when you need direct AudioSeq
control, the SFX catalog, Ocarina tables, the direct skeleton-pose getter, actor
spawning, or an existing raw-API integration. Do not mix raw lifecycle calls
with an active `OoTEngine`.

The headers take precedence if a guide and the code disagree. Do not commit or
distribute a ROM or extracted game asset with liboot; the user supplies a
compatible ROM at runtime.
