# Changelog

This file records user-visible changes to liboot. The project uses semantic
version numbers before 1.0: a minor release may change the native ABI, while a
patch release must remain ABI-compatible with its minor line.

## Unreleased

### Added

- A single ROM-free validation entry point for local and release builds.
- Reproducibility checks for the pinned `src/decomp` snapshot.
- Parser fuzzing entry points and binding/header parity checks.
- A published ROM-compatibility matrix and maintainer release checklist.
- A local, byte-order-independent ROM identity and profile-matching tool.
- Signed GitHub build-provenance attestations for release archives.
- Installed-package smoke tests through both `pkg-config` and CMake consumers.
- Prerelease tag validation and SHA-based archive names for manual release
  workflow runs.
- Versioned runtime limits/capability discovery and explicit Link/scene
  geometry truncation reporting without changing existing ABI sizes.

### Changed

- Public documentation now describes liboot as a host-driven Link runtime and
  separates current capabilities from host responsibilities and limitations.

## 0.8.0

The current source and ABI version is 0.8.0. This repository did not contain a
corresponding Git tag when this changelog was introduced; the first published
release must follow [the release checklist](docs/RELEASING.md) and must not
retroactively imply that an earlier binary was verified.
