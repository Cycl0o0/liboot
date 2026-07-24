#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0
#
# Verify that a built liboot shared library exports exactly the public ABI
# recorded in tools/public-symbols.txt, on every platform, from ONE allowlist.
#
#   tools/check-symbols.sh <shared-library> [allowlist]
#
# The public ABI is the set of exported `oot_*` symbols.  Symbol decoration
# differs per object format (Mach-O prefixes a leading underscore; PE export
# tables are read with a different tool), so each platform is normalized back
# to the bare `oot_*` names that public-symbols.txt stores.  The OS is taken
# from $RUNNER_OS (set by GitHub Actions) and falls back to `uname` locally.
set -euo pipefail

lib=${1:?usage: check-symbols.sh <shared-library> [allowlist]}
here=$(cd "$(dirname "$0")" && pwd)
allow=${2:-"$here/public-symbols.txt"}
os=${RUNNER_OS:-$(uname -s)}

# Emit one candidate symbol name per line for the current platform.
extract() {
  case "$os" in
    Linux|linux)
      # ELF: `value type name`; defined-only removes undefined rows.
      nm -D --defined-only "$lib" | awk '{ print $NF }'
      ;;
    macOS|Darwin)
      # Mach-O: external + defined only, then strip the leading underscore.
      nm --defined-only --extern-only "$lib" | awk '{ print $NF }' | sed 's/^_//'
      ;;
    Windows|MINGW*|MSYS*|CYGWIN*)
      # PE: read the DLL export directory. Prefer MSVC dumpbin, then LLVM.
      if command -v dumpbin >/dev/null 2>&1; then
        dumpbin //nologo //exports "$lib"
      elif command -v llvm-readobj >/dev/null 2>&1; then
        llvm-readobj --coff-exports "$lib"
      elif command -v llvm-nm >/dev/null 2>&1; then
        llvm-nm --defined-only --extern-only "$lib"
      else
        echo "check-symbols: need dumpbin, llvm-readobj, or llvm-nm on PATH" >&2
        exit 2
      fi | grep -oE 'oot_[A-Za-z0-9_]+'
      ;;
    *)
      echo "check-symbols: unsupported OS '$os'" >&2
      exit 2
      ;;
  esac
}

got=$(mktemp)
trap 'rm -f "$got"' EXIT
extract | grep -E '^oot_' | LC_ALL=C sort -u > "$got"

if ! diff -u "$allow" "$got"; then
  echo "check-symbols: exported ABI drifted from $(basename "$allow") on $os" >&2
  echo "  (regenerate with: tools/check-symbols.sh <lib> && cp the new list, if intended)" >&2
  exit 1
fi

echo "check-symbols: OK — $(wc -l < "$got" | tr -d ' ') exported oot_* symbols on $os"
