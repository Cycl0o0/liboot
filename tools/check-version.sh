#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0
#
# Assert the library and engine-ABI versions are pinned consistently in every
# maintained declaration, and (optionally) that a release tag agrees with them.
#
#   tools/check-version.sh            # header == CMake
#   tools/check-version.sh v0.8.0     # header == CMake == release tag
#   tools/check-version.sh v0.8.0-rc.1  # same base version, prerelease suffix
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)

header_major=$(awk '$2 == "LIBOOT_VERSION_MAJOR" { print $3 }' "$root/src/liboot.h")
header_minor=$(awk '$2 == "LIBOOT_VERSION_MINOR" { print $3 }' "$root/src/liboot.h")
header_patch=$(awk '$2 == "LIBOOT_VERSION_PATCH" { print $3 }' "$root/src/liboot.h")
header="$header_major.$header_minor.$header_patch"
header_string=$(awk '$2 == "LIBOOT_VERSION_STRING" { gsub(/"/, "", $3); print $3 }' \
  "$root/src/liboot.h")
cmake=$(grep -oE 'VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' \
  "$root/CMakeLists.txt" | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
docs_version=$(grep -A4 '^#define LIBOOT_VERSION_MAJOR' "$root/docs/API_REFERENCE.md" \
  | awk '
      $2 == "LIBOOT_VERSION_MAJOR" { major = $3 }
      $2 == "LIBOOT_VERSION_MINOR" { minor = $3 }
      $2 == "LIBOOT_VERSION_PATCH" { patch = $3 }
      END { print major "." minor "." patch }
    ')

engine_api=$(awk '$2 == "OOT_ENGINE_API_VERSION" { gsub(/u$/, "", $3); print $3 }' \
  "$root/src/liboot_engine.h")
csharp_api=$(awk '/public const uint EngineApiVersion =/ { gsub(/;/, "", $6); print $6 }' \
  "$root/bindings/csharp/LibOot.cs")
docs_api=$(grep -E '^\| `OOT_ENGINE_API_VERSION` \|' "$root/docs/API_REFERENCE.md" \
  | sed -E 's/.*\| `([0-9]+)` \|.*/\1/')
guide_api=$(grep -m1 -E '^`liboot_engine\.h` uses API version `[0-9]+`\.' \
  "$root/docs/ENGINE_INTEGRATION.md" | grep -oE '[0-9]+' | tail -1)

limits_version=$(awk '$2 == "OOT_ENGINE_LIMITS_VERSION" { gsub(/u$/, "", $3); print $3 }' \
  "$root/src/liboot_engine.h")
csharp_limits_version=$(awk '/public const uint EngineLimitsVersion =/ { gsub(/;/, "", $6); print $6 }' \
  "$root/bindings/csharp/LibOot.cs")
docs_limits_version=$(grep -E '^\| `OOT_ENGINE_LIMITS_VERSION` \|' \
  "$root/docs/API_REFERENCE.md" | sed -E 's/.*\| `([0-9]+)` \|.*/\1/')

if [ "$header" != "$header_string" ] || [ "$header" != "$cmake" ] || \
   [ "$header" != "$docs_version" ]; then
  echo "library version mismatch: macros=$header string=$header_string CMake=$cmake docs=$docs_version" >&2
  exit 1
fi

if [ -z "$engine_api" ] || [ "$engine_api" != "$csharp_api" ] || \
   [ "$engine_api" != "$docs_api" ] || [ "$engine_api" != "$guide_api" ]; then
  echo "engine API mismatch: header=$engine_api C#=$csharp_api docs=$docs_api guide=$guide_api" >&2
  exit 1
fi

if [ -z "$limits_version" ] || \
   [ "$limits_version" != "$csharp_limits_version" ] || \
   [ "$limits_version" != "$docs_limits_version" ]; then
  echo "limits API mismatch: header=$limits_version C#=$csharp_limits_version docs=$docs_limits_version" >&2
  exit 1
fi

if [ "$#" -ge 1 ] && [ -n "$1" ]; then
  tag=${1#v}
  prerelease_pattern="^${header//./\\.}-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*$"
  if [ "$tag" != "$header" ] && [[ ! "$tag" =~ $prerelease_pattern ]]; then
    echo "tag '$1' does not match pinned version '$header'" >&2
    exit 1
  fi
  echo "check-version: OK — library $header, engine API $engine_api, limits API $limits_version, tag $1"
else
  echo "check-version: OK — library $header, engine API $engine_api, limits API $limits_version"
fi
