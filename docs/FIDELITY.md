# Fidelity traces

`test/fidelity_runner` records and compares deterministic, ROM-backed liboot
executions. It is the measurement layer for claims about frame-by-frame OoT
compatibility; ordinary smoke tests can prove that output is finite and
bounded, but cannot prove that it matches the game.

Trace files contain numeric state and fixed-endian FNV-1a hashes only. They do
not contain ROM bytes, decoded textures, geometry arrays, framebuffer captures,
or PCM samples.

## Build and self-test

```sh
make
make -C test fidelity_runner
./test/fidelity_runner --self-test
```

The self-test exercises the versioned trace parser, float round-trip, hashes,
and mismatch detection without reading a ROM. It runs in public CI.

## Record and compare

Record the default 300-tick `core` scenario in Hyrule Field:

```sh
./test/fidelity_runner oot.z64 --record local.trace
```

Replay it against the current library:

```sh
./test/fidelity_runner oot.z64 --compare local.trace
```

Useful recording options are:

```text
--scenario core|idle
--ticks N
--scene INDEX
--room INDEX
--spawn INDEX
--age adult|child
```

Scene and room indices accept decimal or C-style hexadecimal values. Comparison
uses the scenario, tick count, scene, room, spawn, age, ROM hash, and default
float tolerance stored in the trace. `--tolerance FLOAT` overrides only the
comparison tolerance.

The raw ROM FNV-1a hash is an identity check, not a security primitive. A
byte-swapped dump of the same revision intentionally has a different identity:
generate a separate trace or canonicalize it before recording.

## What is compared

Every completed 20 Hz tick records:

- Link position, velocity, facing, action, canonical animation id + frame,
  health, magic, targeting, water, aim, floor-SFX, attack, and state flags;
- the live scene id plus active/geometry room indices, room type, environment,
  echo, Lens behavior, warp restriction, scene-camera type, and map area;
- triangle count and a canonical hash of positions, normals, colors, UVs,
  alpha, texture indices, and render flags;
- Link skeleton, actor list, Navi state, and synchronous SFX-event hashes.

Discrete values and counts compare exactly. A float matches when its absolute
difference is no larger than `tolerance * max(1, abs(reference))`. Component
hashes normally compare exactly; an external reference exporter may write a
zero hash to mark a component it cannot observe yet, while the associated
availability/count fields remain checked.

Trace format version 3 adds the live scene/room command state. Version 2 added
`animId`; its one-based values follow segment-7 animation-data order, so an OoT
reference exporter can produce the same IDs without depending on native
addresses. Zero means that the animation is not in the canonical 561-entry
table.

The built-in `core` input script covers idle, forward/diagonal movement, roll,
sword inputs, Z movement, shielding, and C-up. `idle` holds neutral input for
the complete run. Both use exact engine steps rather than elapsed-time
accumulation.

## Baseline versus OoT oracle

A trace recorded by liboot is a regression baseline: it detects future changes
but does **not** establish fidelity to OoT. A 1:1 claim requires a second trace
exporter in the pinned OoT reference runtime that writes the same format from
the same ROM revision, scene, spawn, age, equipment, input script, and RNG
state. Compare that reference trace with this runner and classify results as:

1. gameplay state equality;
2. geometry/material command equality;
3. framebuffer and mixed-audio equality, once those reference hashes exist.

The native audio path executes the ROM's sequence/channel/layer programs and
uses its decoded VADPCM samples, but it is a host software mixer rather than an
RSP emulator. ROM-authored ADSR envelopes, decay/release indexes, sustain,
short-note gate tables, drum pan and all five layer-portamento modes now follow
the PAL retail state machine at its 200 Hz synthesis cadence, including
special-time sweeps and continuous-note sample identity. Vibrato, filters,
reverb, ABI resampling and the final envelope/pan mixer remain host
approximations; catalog coverage therefore does not imply sample-exact N64
output.

A bit-perfect audio oracle must compare the native PAL 1.1 AI buffers as
interleaved S16, including their variable chunk lengths and deterministic AI,
RNG and task timing. The arbitrary-rate F32 convenience renderer cannot be
that contract. A future exact backend therefore needs the original audio CPU
state machine plus execution of the ROM's `aspMain` RSP microcode; F32 output
can then be derived from, but is not itself identical to, those reference
buffers.

Keep reference tooling and trace provenance explicit. Do not commit ROMs,
extracted assets, audio captures, or private crash data. Review numeric trace
fixtures before publishing them and identify the exact ROM revision by hash.
