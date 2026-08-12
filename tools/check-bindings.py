#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0
"""Check that the C++ and C# bindings cover every oot_engine_* export."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
ENGINE_HEADER = ROOT / "src" / "liboot_engine.h"
CPP_BINDING = ROOT / "bindings" / "cpp" / "liboot.hpp"
CSHARP_BINDING = ROOT / "bindings" / "csharp" / "LibOot.cs"

IDENTIFIER = r"oot_engine_[A-Za-z0-9_]+"


def read(path):
    return path.read_text(encoding="utf-8")


def strip_comments(source, strip_literals=False):
    """Blank C-family comments and, optionally, quoted literals."""
    output = []
    index = 0
    state = "code"
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == "code":
            if char == "/" and following == "/":
                output.extend((" ", " "))
                index += 2
                state = "line_comment"
                continue
            if char == "/" and following == "*":
                output.extend((" ", " "))
                index += 2
                state = "block_comment"
                continue
            if char in ('"', "'"):
                state = "string" if char == '"' else "character"
                output.append(" " if strip_literals else char)
                index += 1
                continue
            output.append(char)
            index += 1
            continue

        if state == "line_comment":
            output.append(char if char in "\r\n" else " ")
            index += 1
            if char in "\r\n":
                state = "code"
            continue

        if state == "block_comment":
            if char == "*" and following == "/":
                output.extend((" ", " "))
                index += 2
                state = "code"
                continue
            output.append(char if char in "\r\n" else " ")
            index += 1
            continue

        delimiter = '"' if state == "string" else "'"
        output.append(" " if strip_literals and char not in "\r\n" else char)
        index += 1
        if char == "\\" and index < len(source):
            escaped = source[index]
            output.append(" " if strip_literals and escaped not in "\r\n" else escaped)
            index += 1
        elif char == delimiter:
            state = "code"

    return "".join(output)


def native_exports(source):
    source = strip_comments(source)
    pattern = re.compile(
        r"\bextern\s+OOT_LIB_FN\s+[^;]*?\b(" + IDENTIFIER + r")\s*\(",
        flags=re.DOTALL,
    )
    return set(pattern.findall(source))


def cpp_entry_points(source):
    source = strip_comments(source, strip_literals=True)
    return set(re.findall(r"\b(" + IDENTIFIER + r")\s*\(", source))


def csharp_entry_points(source):
    source = strip_comments(source)
    return set(
        re.findall(
            r"\bEntryPoint\s*=\s*\"(" + IDENTIFIER + r")\"",
            source,
        )
    )


def describe_difference(label, exports, bound):
    messages = []
    missing = sorted(exports - bound)
    stale = sorted(bound - exports)
    if missing:
        messages.append("{} missing: {}".format(label, ", ".join(missing)))
    if stale:
        messages.append("{} references non-exports: {}".format(label, ", ".join(stale)))
    return messages


def main():
    exports = native_exports(read(ENGINE_HEADER))
    cpp = cpp_entry_points(read(CPP_BINDING))
    csharp = csharp_entry_points(read(CSHARP_BINDING))

    if not exports:
        print("binding parity check failed: no engine exports found", file=sys.stderr)
        return 1

    errors = []
    errors.extend(describe_difference("C++", exports, cpp))
    errors.extend(describe_difference("C#", exports, csharp))
    if errors:
        print("binding parity check failed:", file=sys.stderr)
        for error in errors:
            print("  " + error, file=sys.stderr)
        return 1

    print(
        "binding parity ok: {} oot_engine_* exports covered by C++ and C#".format(
            len(exports)
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
