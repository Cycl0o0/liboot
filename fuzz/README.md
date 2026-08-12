# ROM parser fuzzing

These targets call the small parsers in `src/rom_util.c` without starting the
game runtime or using game data.

Building them requires a source checkout because they compile the internal
`src/rom_util.c` parser directly. Binary packages include this guide for
reference, not a standalone fuzzing SDK.

- `rom_util_fuzz.c` covers short and padded byte-order normalization, dmadata
  signature scans, arbitrary and discovered table entries, and bounded raw or
  Yaz0-backed extraction from a generated ROM envelope.
- `yaz0_fuzz.c` feeds arbitrary streams to `yaz0_decode`. Its output buffer is
  capped at 64 KiB so malformed length fields cannot exhaust the fuzz runner.

The 4 MiB harness input cap is a fuzzing resource limit, not a change to the
public 256 MiB ROM limit.

## Finite smoke run

From the repository root, with a Clang build that includes libFuzzer:

```sh
make fuzz-smoke
```

The smoke target runs each fuzzer 1,000 times. Override the finite count with
`make fuzz-smoke SMOKE_RUNS=10000`. `make -C fuzz all` builds without running,
and `make -C fuzz clean` removes the generated tree.

Binaries, corpora, and crash artifacts stay under the ignored `build/fuzz/`
directory. The Makefile keeps fuzz compiler and linker flags separate from the
library build; customize them with `FUZZ_CC`, `FUZZ_CPPFLAGS`, `FUZZ_CFLAGS`,
`FUZZ_LDFLAGS`, or `FUZZ_LDLIBS`. Keep ROMs out of the corpus. A minimized
synthetic reproducer is safe to commit; a retail ROM or extracted game asset is
not.

## Next parser boundaries

Scene parsing is not covered by these targets. Its byte readers are currently
`static` functions in `src/scene.c`, while the public entry point also depends
on the process-wide ROM, `PlayState`, collision, renderer, and segment shims.
The next step is to move the scene-header, collision-header, room-list, and
mesh-header readers behind internal functions that accept `(data, size)` and
return owned parser results. Those functions can then receive independent fuzz
targets before runtime state is touched.

Audio-table parsing is also not covered here. The table validators in
`src/audio_extract.c` are `static` and read sizes from process-global extracted
audio buffers. The next boundary is a stateless validator that accepts the code,
Audiobank, Audioseq, and Audiotable spans explicitly. Sample-header and ADPCM
book/loop parsing should be split into bounded `(data, size)` helpers and fuzzed
separately before decoding or cache allocation.
