#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0
#
# Exercise an installed liboot tree as an external pkg-config and CMake
# consumer. The checks deliberately use only installed files.
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: tools/check-install.sh <install-prefix>" >&2
  exit 2
fi

prefix=$(cd "$1" && pwd)
doc="$prefix/share/doc/liboot"
python=${PYTHON3:-python3}

find_installed_file() {
  local pattern=$1
  local label=$2
  local found=
  local candidate

  while IFS= read -r -d '' candidate; do
    if [ -n "$found" ]; then
      echo "check-install: multiple $label files under $prefix" >&2
      return 1
    fi
    found=$candidate
  done < <(find "$prefix" -type f -path "$pattern" -print0)

  if [ -z "$found" ]; then
    echo "check-install: missing $label under $prefix" >&2
    return 1
  fi
  printf '%s' "$found"
}

pc_file=$(find_installed_file '*/pkgconfig/liboot.pc' liboot.pc)
cmake_config=$(find_installed_file \
  '*/cmake/liboot/libootConfig.cmake' libootConfig.cmake)
pc_dir=${pc_file%/*}
cmake_dir=${cmake_config%/*}

required_files=(
  "$prefix/include/liboot/liboot.h"
  "$prefix/include/liboot/liboot_engine.h"
  "$pc_file"
  "$cmake_config"
  "$doc/LICENSE"
  "$doc/NOTICE.md"
  "$doc/README.md"
  "$doc/CHANGELOG.md"
  "$doc/CONTRIBUTING.md"
  "$doc/CONTRIBUTORS.md"
  "$doc/SECURITY.md"
  "$doc/docs/README.md"
  "$doc/docs/GETTING_STARTED.md"
  "$doc/docs/USAGE.md"
  "$doc/docs/API_REFERENCE.md"
  "$doc/docs/UNIVERSAL_SDK.md"
  "$doc/docs/ENGINE_INTEGRATION.md"
  "$doc/docs/FIDELITY.md"
  "$doc/docs/ROM_COMPATIBILITY.md"
  "$doc/docs/DEVELOPMENT.md"
  "$doc/docs/RELEASING.md"
  "$doc/bindings/README.md"
  "$doc/bindings/cpp/liboot.hpp"
  "$doc/bindings/csharp/LibOot.cs"
  "$doc/bindings/csharp/README.md"
  "$doc/examples/engine.c"
  "$doc/examples/basic.c"
  "$doc/fuzz/README.md"
  "$doc/tools/rom-profiles.json"
)

for path in "${required_files[@]}"; do
  if [ ! -f "$path" ]; then
    echo "check-install: missing $path" >&2
    exit 1
  fi
done
if [ ! -x "$doc/tools/identify-rom.py" ]; then
  echo "check-install: identify-rom.py is not executable" >&2
  exit 1
fi
if [ ! -x "$doc/tools/check-install.sh" ]; then
  echo "check-install: packaged check-install.sh is not executable" >&2
  exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/liboot-install-check.XXXXXX")
trap 'rm -rf "$work"' EXIT

printf '%s\n' \
  '#include <liboot_engine.h>' \
  'int main(void) {' \
  '    return oot_engine_api_version() == OOT_ENGINE_API_VERSION ? 0 : 1;' \
  '}' > "$work/main.c"

export PKG_CONFIG_PATH="$pc_dir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --validate liboot
libdir=$(pkg-config --variable=libdir liboot)

if ! command -v "$python" >/dev/null 2>&1; then
  echo "check-install: Python 3 is required to preserve pkg-config arguments" >&2
  exit 1
fi

# Parse pkg-config's shell-escaped output into argv without losing spaces in
# an install prefix. LIBOOT_CHECK_ARCHES optionally link-checks each requested
# macOS slice and verifies that the installed dylib contains them all.
"$python" - "$work/main.c" "$work/pkg-config-smoke" "$libdir" <<'PY'
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

source, output, libdir = sys.argv[1:4]
compiler = shlex.split(os.environ.get("CC", "cc"))
cflags_other = shlex.split(
    subprocess.check_output(
        ["pkg-config", "--cflags-only-other", "liboot"], text=True
    )
)
link_flags = shlex.split(
    subprocess.check_output(
        ["pkg-config", "--static", "--libs-only-l", "liboot"], text=True
    )
) + shlex.split(
    subprocess.check_output(
        ["pkg-config", "--static", "--libs-only-other", "liboot"], text=True
    )
)
includedir = subprocess.check_output(
    ["pkg-config", "--variable=includedir", "liboot"], text=True
).rstrip("\r\n")
common = [
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-I" + includedir,
    *cflags_other,
    source,
    "-L" + libdir,
    *link_flags,
    "-Wl,-rpath," + libdir,
]
subprocess.check_call([*compiler, *common, "-o", output])
subprocess.check_call([output])

architectures = os.environ.get("LIBOOT_CHECK_ARCHES", "").split()
for architecture in architectures:
    if re.fullmatch(r"[A-Za-z0-9_]+", architecture) is None:
        raise SystemExit("check-install: invalid architecture " + architecture)

if architectures:
    library = Path(libdir) / "liboot.dylib"
    subprocess.check_call(
        ["lipo", str(library), "-verify_arch", *architectures]
    )
    for architecture in architectures:
        subprocess.check_call(
            [
                *compiler,
                "-arch",
                architecture,
                *common,
                "-o",
                output + "-" + architecture,
            ]
        )
PY

printf '%s\n' \
  'cmake_minimum_required(VERSION 3.16)' \
  'project(liboot_consumer LANGUAGES C)' \
  'find_package(liboot CONFIG REQUIRED)' \
  'add_executable(liboot-cmake-smoke main.c)' \
  'target_link_libraries(liboot-cmake-smoke PRIVATE liboot::oot)' \
  > "$work/CMakeLists.txt"

cmake -S "$work" -B "$work/cmake-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$prefix" \
  -Dliboot_DIR="$cmake_dir" \
  -DCMAKE_BUILD_RPATH="$libdir"
cmake --build "$work/cmake-build" --config Release --parallel
"$work/cmake-build/liboot-cmake-smoke"

echo "check-install: OK — metadata, pkg-config, and CMake consumers"
