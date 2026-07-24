# Contributing to liboot

Thank you for helping improve liboot.

## Before opening a change

1. Do not commit ROMs, extracted game assets, audio captures, crash dumps, or
   other copyrighted/private runtime data.
2. Build from a clean checkout with `make`.
3. Run the ROM-backed checks locally with a legally obtained compatible ROM:

   ```sh
   make -C test playground
   ./test/playground /path/to/oot.z64 --suite 1000
   ./test/fidelity_runner /path/to/oot.z64 --compare /path/to/reference.trace
   ```

   Run `./test/fidelity_runner --self-test` without a ROM after changing its
   parser or trace format. A trace recorded by liboot is a regression baseline;
   only a trace produced by the pinned OoT reference runtime is a fidelity
   oracle. See [`docs/FIDELITY.md`](docs/FIDELITY.md).

4. Keep the public C ABI backwards compatible. New public structs should be
   size/version tagged, and engine-facing APIs must document ownership and
   threading rules.
5. Keep changes to `src/decomp/` minimal and mark integration patches with a
   `liboot` comment. Update provenance documentation when vendored files move.
6. Trace fixtures must contain numeric state/hashes only. Do not commit ROM
   data, decoded assets, geometry dumps, frame captures, or PCM samples.

## Licensing contributions

By submitting original work to this repository, you agree to license that
work under `AGPL-3.0-or-later`. Do not add third-party code unless its license
is documented and compatible. The unlicensed vendored `src/decomp/` subtree
has separate provenance described in [NOTICE.md](NOTICE.md).

The sole current contributor is listed in [CONTRIBUTORS.md](CONTRIBUTORS.md).
