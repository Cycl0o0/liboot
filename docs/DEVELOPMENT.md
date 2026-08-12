# Development guide

## Validation layers

Run the ROM-free gate before every change:

```sh
make check
```

This builds the library, checks version and binding declarations, verifies the
vendored-source manifest, and runs the synthetic tests that require no
copyrighted data.

Use `tools/identify-rom.py --json /path/to/rom` when recording compatibility
evidence. It prints only identity metadata and never writes canonicalized bytes;
still inspect header text before sharing and treat the resulting hash as
user-provided local data until it is approved for the public profile database.

Clang users can also run a finite, generated-input parser fuzz smoke:

```sh
make fuzz-smoke
```

Changes that touch Player behavior, scenes, rendering, or audio also need the
ROM-backed suite with a legally obtained compatible ROM:

```sh
make -C test playground
./test/playground /path/to/oot.z64 --suite 1000
```

Use the fidelity runner only with a baseline whose origin is known. A trace
recorded by liboot catches regressions but is not a retail oracle.

After installing a CMake build, verify the exact staged tree through both
supported package-discovery paths:

```sh
tools/check-install.sh /absolute/path/to/stage
```

The check compiles and runs independent `pkg-config` and CMake consumers; it
does not fall back to headers or libraries in the source tree.

## Source boundaries

- `src/liboot_engine.*` is the checked, size-tagged API for new hosts.
- `src/liboot.*` is the process-global compatibility layer.
- `src/shim/` supplies the N64 services needed by selected decompilation code.
- `src/decomp/` is pinned third-party material plus documented integration
  patches. Keep unrelated formatting and refactors out of this subtree.
- `bindings/` contains maintained wrappers; the parity check distinguishes missing
  declarations from deliberately unwrapped convenience methods.

## Public ABI changes

Before adding an exported function or field:

1. Prefer the engine API and return `OoTResult` for operations that can fail.
2. Append to size-tagged structures; never reorder existing fields.
3. Document ownership, pointer lifetime, thread, and callback re-entry rules.
4. Update `tools/public-symbols.txt`, C++ and C# declarations, API reference,
   and ABI layout assertions together.
5. Run `tools/check-symbols.sh` on the shared library and
   `tools/check-bindings.py` before committing.

A pre-1.0 minor version may intentionally change ABI. Patch versions must not.

## Vendored and generated files

`tools/vendor.sh` accepts only a clean zeldaret/oot checkout at the commit named
in `NOTICE.md`. It refreshes the selected translation units but does not reapply
liboot's integration patches or regenerate the header closure.

After an intentional vendor update:

1. Review the upstream diff and licenses.
2. Reapply each integration patch listed in `NOTICE.md`.
3. Regenerate asset declarations with the scripts in `tools/` when their XML
   inputs changed.
4. Run the ROM-backed suite.
5. Update the snapshot with `tools/check-vendor.py --write` and review every
   manifest change.

## Completion criteria

A runtime feature is complete only when it has a checked C contract, ownership
and threading documentation, a ROM-free failure-path test where possible, a
ROM-backed behavior test, binding coverage, and no silent overflow or ignored
host event. Do not mark a stubbed path complete because it links.
