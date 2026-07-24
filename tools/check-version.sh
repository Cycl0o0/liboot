#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0
#
# Assert the version is pinned consistently in every place that declares it,
# and (optionally) that a release tag agrees with it.
#
#   tools/check-version.sh            # header == CMake
#   tools/check-version.sh v0.8.0     # header == CMake == tag (leading v optional)
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)

header=$(grep -oE 'LIBOOT_VERSION_STRING[[:space:]]+"[0-9]+\.[0-9]+\.[0-9]+"' \
  "$root/src/liboot.h" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
cmake=$(grep -oE 'VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' \
  "$root/CMakeLists.txt" | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')

if [ "$header" != "$cmake" ]; then
  echo "version mismatch: liboot.h=$header CMakeLists.txt=$cmake" >&2
  exit 1
fi

if [ "$#" -ge 1 ] && [ -n "$1" ]; then
  tag=${1#v}
  if [ "$tag" != "$header" ]; then
    echo "tag '$1' does not match pinned version '$header'" >&2
    exit 1
  fi
  echo "check-version: OK — tag, liboot.h, and CMakeLists.txt all agree on $header"
else
  echo "check-version: OK — liboot.h and CMakeLists.txt agree on $header"
fi
