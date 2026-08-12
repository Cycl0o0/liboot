# Release checklist

The release workflow packages tagged commits. Complete this checklist before
creating a tag; do not use a tag to discover whether the commit is releasable.

## Prepare

1. Decide the version under the pre-1.0 policy: minor for intentional ABI
   changes, patch for ABI-compatible fixes.
2. Update the version macros, string, CMake project version, documentation, and
   `CHANGELOG.md`.
3. Replace development-only version labels with the release version.
4. Confirm `NOTICE.md`, the upstream commit pin, vendored file list, and
   `tools/vendor-manifest.sha256` describe the exact tree being packaged.
5. Confirm the legal status of every third-party file. Passing CI does not grant
   redistribution permission.

## Validate

```sh
tools/check-version.sh vX.Y.Z
tools/check-vendor.py
tools/check-bindings.py
make check
make sanitizers
make fuzz-smoke
```

Then run the ROM-backed suite and compare a trace with the approved reference
runtime. Record the canonical ROM hash and validation level in
[ROM_COMPATIBILITY.md](ROM_COMPATIBILITY.md), without publishing ROM data.

Build and install shared and static variants through CMake. Test an external
consumer through both `find_package(liboot CONFIG)` and `pkg-config`, and run
the linked smoke executable. `tools/check-install.sh /absolute/path/to/stage`
performs both consumer checks against one installed tree.

## Publish

1. Create an annotated `vX.Y.Z` tag on the validated commit. A prerelease may
   append dot-separated identifiers such as `vX.Y.Z-rc.1`; its base version
   must still match the source version.
2. Let the release workflow rebuild and rerun the ROM-free gate before it
   uploads an archive.
3. Verify every archive checksum, target name, dynamic-library identity, license
   notice, and documentation link. Verify its GitHub build provenance with:

   ```sh
   gh attestation verify liboot-vX.Y.Z-PLATFORM.tar.gz \
     --repo Cycl0o0/liboot
   ```

4. Add release notes from the changelog. Do not claim platforms, ROM revisions,
   engine packages, or retail fidelity beyond the recorded evidence.
5. Preserve the ABI baseline and release artifacts for later comparison.
