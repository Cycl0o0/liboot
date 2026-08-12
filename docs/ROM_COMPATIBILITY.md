# ROM compatibility

liboot discovers assets through a ROM's own DMADATA and accepts the three common
byte orders, but structural discovery is not proof that gameplay, scene, and
audio layouts match the compiled decompilation paths.

## Current matrix

| ROM revision | Accepted byte orders | Validation level | Status |
| --- | --- | --- | --- |
| PAL Europe Rev 1 (PAL 1.1) | `.z64`, `.v64`, `.n64` | Maintainer development and ROM-backed tests | Exercised; publish the exact SHA-256 before making a release compatibility claim |
| Any other retail revision | Parser may accept it | None published | Unsupported until profiled and tested |

Gameplay code is compiled with the NTSC 1.2 decompilation configuration even
when compatible assets are read from PAL 1.1. A successful engine creation only
proves that the required tables and Link objects were found and parsed.

Public CI contains no ROM and therefore cannot validate either row. It checks
synthetic parser inputs, ABI behavior, arithmetic bounds, and the trace format.

## Validation levels

A compatibility entry should report each level separately:

1. **Identification:** canonical big-endian SHA-256, game code, region, and
   revision byte.
2. **Initialization:** engine creation, adult/child objects, animations, and
   texture extraction.
3. **Player:** the headless engine suite for both ages and each supported item.
4. **Scenes:** every claimed scene/room loads without invalid geometry and its
   runtime metadata is checked.
5. **Audio:** catalog counts, sample decoding, sequences, ambience, and bounded
   mixer output.
6. **Fidelity:** comparison against a trace exported by the pinned OoT reference
   runtime, not a trace recorded by liboot itself.

Do not collapse these levels into a single “compatible” flag. A ROM may parse
while still selecting different behavior or table layouts.

## Adding a profile

1. Work from a legally obtained ROM; never commit or upload it.
2. Run `tools/identify-rom.py --json /path/to/rom`. It canonicalizes the three
   accepted byte orders while hashing and never writes transformed ROM data.
   The output includes the ROM header title and game code; inspect it before
   sharing results from a modified or private build.
3. Record only identification metadata and hashes in the repository.
4. Run the ROM-backed suite and capture which subsystems and scenes passed.
5. Add a profile only after a maintainer has reproduced the result.

Until the first exact hash is published, downstream applications should ask the
user for a ROM and report unsupported input without claiming revision coverage.

The versioned profile database is `tools/rom-profiles.json`. It is intentionally
empty until a maintainer can publish and reproduce an exact hash.
