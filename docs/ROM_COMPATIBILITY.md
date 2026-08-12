# ROM compatibility

liboot discovers assets through a ROM's own DMADATA and accepts the three common
byte orders, but structural discovery is not proof that gameplay, scene, and
audio layouts match the compiled decompilation paths.

## Current matrix

| ROM revision | Accepted byte orders | Validation level | Status |
| --- | --- | --- | --- |
| PAL Europe Rev 1 (PAL 1.1) | `.z64`, `.v64`, `.n64` | Levels 1–5: identification, initialization, Player, scenes, and audio | Profiled and exercised on 2026-08-12; retail fidelity is pending |
| Any other retail revision | Parser may accept it | None published | Unsupported until profiled and tested |

## PAL 1.1 evidence

The `oot-pal-1.1` profile records a 33,554,432-byte canonical ROM with SHA-256
`74f9266fd7fa23cc700b5b46a21fbe99cdc6ea10438bc4b48eeb625763b8611c`.
Its header identifies `THE LEGEND OF ZELDA`, game code `NZLP`, Europe region
code `P`, and revision byte `1`.

The 2026-08-12 maintainer run covered:

- engine initialization, both Link ages, equipment combinations, asset and
  texture extraction, and the checked engine API;
- the 18 scene presets exposed by the playground, including aggregate-room
  geometry and a separate 1,000-frame scene walk;
- an audio catalog containing 110 sequences, successful prewarming of all 110,
  selected sequencer behavior checks, and all 64 Link voice IDs exercised by
  `voicetest`;
- 1,000-tick broad and feature runs plus a 1,000-frame combat run; and
- a 1,000-tick local trace round-trip matching 58,000 fields.

The local trace is regression evidence only. Retail fidelity remains unclaimed
until the same scenario passes against a trace exported by the pinned OoT
reference runtime. This run also does not cover every retail scene, alternate
header, spawn, or room combination.

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

Downstream applications should ask the user for a ROM and match it locally
against the profile database. They must not bundle ROM data or claim support
for unprofiled revisions.

The versioned profile database is `tools/rom-profiles.json`. Entries contain
identity metadata only; they do not contain ROM bytes or extracted assets.
