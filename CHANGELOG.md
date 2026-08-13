# Changelog

This file records user-visible changes to liboot. The project uses semantic
version numbers before 1.0: a minor release may change the native ABI, while a
patch release must remain ABI-compatible with its minor line.

## Unreleased

### Added

- Isolated shared-library engine contexts, with interleaved multi-engine
  execution and a capability-reported static-library singleton fallback.
- Host-defined dynamic collision objects with generation-checked transforms,
  enable/disable state, scene/world rebinding, and moving-platform carry.
- Host actor records in the native attention lists, scene actor catalogs, and
  queued melee/projectile contact events.
- Child/adult and day/night scene layers, exit and void events, room-image
  records, and animated-material segment metadata.
- Configurable Link and scene geometry storage with 32-bit counts and
  source/material/render-state batches.
- Deterministic interleaved S16 audio output and engine-handle AudioSeq controls.
- Windows x86-64 UCRT64 shared/static CI, install checks, and release archives.
- Identity profiles for all eight N64 retail region/revision combinations.
- A single ROM-free validation entry point for local and release builds.
- Reproducibility checks for the pinned `src/decomp` snapshot.
- Parser fuzzing entry points and binding/header parity checks.
- A published ROM-compatibility matrix with an exact PAL 1.1 identity profile.
- A maintainer release checklist.
- A local, byte-order-independent ROM identity and profile-matching tool.
- Signed GitHub build-provenance attestations for release archives.
- Installed-package smoke tests through both `pkg-config` and CMake consumers.
- Prerelease tag validation and SHA-based archive names for manual release
  workflow runs.
- Versioned runtime limits/capability discovery and explicit Link/scene
  geometry truncation reporting without changing existing ABI sizes.

### Changed

- The selected decompilation configuration and default 60 ms simulation step
  now match the exercised PAL 1.1 ROM.
- Internal libultra compatibility functions now use liboot-prefixed symbols,
  preventing collisions when a static host supplies the same N64 functions,
  including the incompatible `sins` and `coss` helpers used by SM64 ports.
- Public documentation now describes liboot as a host-driven Link runtime and
  separates current capabilities from host responsibilities and limitations.

### Fixed

- The checked engine SFX player now accepts the documented negative pan range,
  so callers can place sounds left of center.
- Repositioning Link now clears pre-warp momentum and synchronizes his internal
  facing fields, preventing movement from leaking across host-owned warps.
- Relocatable `pkg-config` metadata and installed-package checks now support
  multi-component library directories, custom include and documentation roots,
  install-prefix overrides, and multi-config CMake generators.

## 0.8.0

The current source and ABI version is 0.8.0. This repository did not contain a
corresponding Git tag when this changelog was introduced; the first published
release must follow [the release checklist](docs/RELEASING.md) and must not
retroactively imply that an earlier binary was verified.
