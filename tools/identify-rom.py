#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0
"""Identify an N64 ROM without copying or rewriting it."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
from typing import BinaryIO, Optional


MIN_ROM_SIZE = 0x1060
MAX_ROM_SIZE = 256 * 1024 * 1024
READ_SIZE = 1024 * 1024
HEADER_SIZE = 0x40
PROFILE_SCHEMA_VERSION = 1
MAX_PROFILE_SIZE = 1024 * 1024
DEFAULT_PROFILES = Path(__file__).resolve().with_name("rom-profiles.json")
ALLOWED_EXTENSIONS = {".z64", ".v64", ".n64"}

ORDER_BY_MAGIC = {
    b"\x80\x37\x12\x40": ("z64", "big-endian", 1),
    b"\x37\x80\x40\x12": ("v64", "byte-swapped (16-bit)", 2),
    b"\x40\x12\x37\x80": ("n64", "little-endian (32-bit word-swapped)", 4),
}

REGIONS = {
    0x37: "Beta",
    0x41: "Asia (NTSC)",
    0x42: "Brazil",
    0x43: "China",
    0x44: "Germany",
    0x45: "North America",
    0x46: "France",
    0x47: "Gateway 64 (NTSC)",
    0x48: "Netherlands",
    0x49: "Italy",
    0x4A: "Japan",
    0x4B: "Korea",
    0x4C: "Gateway 64 (PAL)",
    0x4E: "Canada",
    0x50: "Europe",
    0x53: "Spain",
    0x55: "Australia",
    0x57: "Scandinavia",
    0x58: "Europe",
    0x59: "Europe",
}


class IdentifyError(Exception):
    """A user-facing input or profile error that omits file contents."""


def _os_error(action: str, error: OSError) -> IdentifyError:
    detail = error.strerror or error.__class__.__name__
    return IdentifyError(f"{action}: {detail}")


def _canonicalize(block: bytes, rom_format: str) -> bytes:
    if rom_format == "z64":
        return block

    converted = bytearray(len(block))
    if rom_format == "v64":
        converted[0::2] = block[1::2]
        converted[1::2] = block[0::2]
    else:
        converted[0::4] = block[3::4]
        converted[1::4] = block[2::4]
        converted[2::4] = block[1::4]
        converted[3::4] = block[0::4]
    return bytes(converted)


def _hash_canonical(
    stream: BinaryIO, first: bytes, expected_size: int, rom_format: str, unit: int
) -> tuple[str, str, bytes]:
    sha256_digest = hashlib.sha256()
    # MD5 is retained only for matching the decompilation project's published
    # ROM identities; SHA-256 remains the collision-resistant reported digest.
    md5_digest = hashlib.md5()  # nosec B324
    header = bytearray()
    pending = b""
    total = 0
    chunk = first

    while chunk:
        total += len(chunk)
        if total > MAX_ROM_SIZE:
            raise IdentifyError(f"ROM is larger than {MAX_ROM_SIZE} bytes")
        pending += chunk
        usable = len(pending) - (len(pending) % unit)
        if usable:
            canonical = _canonicalize(pending[:usable], rom_format)
            sha256_digest.update(canonical)
            md5_digest.update(canonical)
            if len(header) < HEADER_SIZE:
                header.extend(canonical[: HEADER_SIZE - len(header)])
            pending = pending[usable:]
        chunk = stream.read(READ_SIZE)

    if pending:
        raise IdentifyError(
            f"ROM size is not aligned to the detected {unit}-byte storage unit"
        )
    if total != expected_size:
        raise IdentifyError("ROM size changed while it was being read")
    if len(header) < HEADER_SIZE:
        raise IdentifyError("ROM header is incomplete")
    return sha256_digest.hexdigest(), md5_digest.hexdigest(), bytes(header)


def _safe_ascii(raw: bytes, trim_padding: bool = False) -> str:
    if trim_padding:
        raw = raw.rstrip(b"\x00 ")
    return "".join(chr(value) if 0x20 <= value <= 0x7E else "?" for value in raw)


def identify(path: Path) -> dict[str, object]:
    if path.suffix.lower() not in ALLOWED_EXTENSIONS:
        raise IdentifyError("unsupported ROM extension; expected .z64, .v64, or .n64")

    try:
        stream = path.open("rb")
    except OSError as error:
        raise _os_error("cannot open ROM", error) from error

    with stream:
        try:
            opened_stat = os.fstat(stream.fileno())
        except OSError as error:
            raise _os_error("cannot inspect ROM", error) from error
        if not stat.S_ISREG(opened_stat.st_mode):
            raise IdentifyError("ROM input is not a regular file")
        size = opened_stat.st_size
        if size < MIN_ROM_SIZE:
            raise IdentifyError(f"ROM is smaller than {MIN_ROM_SIZE} bytes")
        if size > MAX_ROM_SIZE:
            raise IdentifyError(f"ROM is larger than {MAX_ROM_SIZE} bytes")

        try:
            first = stream.read(4)
            order = ORDER_BY_MAGIC.get(first)
            if order is None:
                raise IdentifyError("unrecognized N64 byte-order magic")
            rom_format, byte_order, unit = order
            canonical_sha256, canonical_md5, header = _hash_canonical(
                stream, first, size, rom_format, unit
            )
        except OSError as error:
            raise _os_error("cannot read ROM", error) from error
        try:
            ending_size = os.fstat(stream.fileno()).st_size
        except OSError as error:
            raise _os_error("cannot recheck ROM", error) from error
        if ending_size != size:
            raise IdentifyError("ROM size changed while it was being read")

    region_value = header[0x3E]
    region_code = (
        chr(region_value) if 0x20 <= region_value <= 0x7E else f"0x{region_value:02X}"
    )
    return {
        "size_bytes": size,
        "format": rom_format,
        "byte_order": byte_order,
        "canonical_sha256": canonical_sha256,
        "canonical_md5": canonical_md5,
        "title": _safe_ascii(header[0x20:0x34], trim_padding=True),
        "game_code": _safe_ascii(header[0x3B:0x3F]),
        "region_code": region_code,
        "region": REGIONS.get(region_value, "Unknown"),
        "revision": header[0x3F],
    }


def _is_display_string(value: object, maximum: int) -> bool:
    return (
        isinstance(value, str)
        and 0 < len(value) <= maximum
        and all(0x20 <= ord(char) <= 0x7E for char in value)
    )


def _profile_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise IdentifyError("profile database contains a duplicate object key")
        result[key] = value
    return result


def _profile_int(token: str) -> int:
    digits = token[1:] if token.startswith("-") else token
    if len(digits) > 20:
        raise IdentifyError("profile database contains an invalid JSON value")
    return int(token)


def _profile_constant(_token: str) -> object:
    # Python's JSON decoder accepts NaN and infinities unless parse_constant is
    # supplied. They are not JSON and no profile field permits them.
    raise IdentifyError("profile database contains a non-standard JSON number")


def load_profiles(path: Path) -> list[dict[str, object]]:
    try:
        with path.open("rb") as stream:
            profile_stat = os.fstat(stream.fileno())
            if not stat.S_ISREG(profile_stat.st_mode):
                raise IdentifyError("profile database is not a regular file")
            if profile_stat.st_size > MAX_PROFILE_SIZE:
                raise IdentifyError(
                    f"profile database exceeds {MAX_PROFILE_SIZE} bytes"
                )
            encoded = stream.read(MAX_PROFILE_SIZE + 1)
            if len(encoded) > MAX_PROFILE_SIZE:
                raise IdentifyError(
                    f"profile database exceeds {MAX_PROFILE_SIZE} bytes"
                )
        document = json.loads(
            encoded.decode("utf-8"),
            object_pairs_hook=_profile_object,
            parse_int=_profile_int,
            parse_constant=_profile_constant,
        )
    except json.JSONDecodeError as error:
        raise IdentifyError(
            "profile database is not valid JSON "
            f"(line {error.lineno}, column {error.colno})"
        ) from error
    except RecursionError as error:
        raise IdentifyError("profile database JSON nesting is too deep") from error
    except UnicodeError as error:
        raise IdentifyError("profile database is not valid UTF-8") from error
    except ValueError as error:
        # Python rejects extremely long integer tokens before returning a JSON
        # value. Convert that implementation exception into the same bounded,
        # user-facing failure as the other malformed profile cases.
        raise IdentifyError("profile database contains an invalid JSON value") from error
    except OSError as error:
        raise _os_error("cannot read profile database", error) from error

    if not isinstance(document, dict):
        raise IdentifyError("profile database root must be an object")
    if set(document) != {"schema_version", "profiles"}:
        raise IdentifyError(
            "profile database must contain only schema_version and profiles"
        )
    if (
        type(document["schema_version"]) is not int
        or document["schema_version"] != PROFILE_SCHEMA_VERSION
    ):
        raise IdentifyError(
            f"unsupported profile schema_version; expected {PROFILE_SCHEMA_VERSION}"
        )
    raw_profiles = document["profiles"]
    if not isinstance(raw_profiles, list):
        raise IdentifyError("profile database profiles must be an array")

    profiles: list[dict[str, object]] = []
    seen_ids: set[str] = set()
    seen_sha256: set[str] = set()
    seen_md5: set[str] = set()
    allowed = {"id", "label", "canonical_sha256", "canonical_md5", "size_bytes"}
    for index, raw_profile in enumerate(raw_profiles):
        location = f"profile {index}"
        if not isinstance(raw_profile, dict) or not set(raw_profile) <= allowed:
            raise IdentifyError(f"{location} is not a valid profile object")
        if "id" not in raw_profile or not (
            "canonical_sha256" in raw_profile or "canonical_md5" in raw_profile
        ):
            raise IdentifyError(
                f"{location} is missing id or a canonical digest"
            )

        profile_id = raw_profile["id"]
        if not isinstance(profile_id, str) or re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", profile_id
        ) is None:
            raise IdentifyError(f"{location} has an invalid id")
        label = raw_profile.get("label", profile_id)
        if not _is_display_string(label, 128):
            raise IdentifyError(f"{location} has an invalid label")
        raw_sha256 = raw_profile.get("canonical_sha256")
        if raw_sha256 is not None and (
            not isinstance(raw_sha256, str)
            or re.fullmatch(r"[0-9A-Fa-f]{64}", raw_sha256) is None
        ):
            raise IdentifyError(f"{location} has an invalid canonical_sha256")
        canonical_sha256 = raw_sha256.lower() if raw_sha256 is not None else None

        raw_md5 = raw_profile.get("canonical_md5")
        if raw_md5 is not None and (
            not isinstance(raw_md5, str)
            or re.fullmatch(r"[0-9A-Fa-f]{32}", raw_md5) is None
        ):
            raise IdentifyError(f"{location} has an invalid canonical_md5")
        canonical_md5 = raw_md5.lower() if raw_md5 is not None else None

        profile_size_value = raw_profile.get("size_bytes")
        if profile_size_value is not None and (
            type(profile_size_value) is not int
            or not MIN_ROM_SIZE <= profile_size_value <= MAX_ROM_SIZE
        ):
            raise IdentifyError(f"{location} has an invalid size_bytes")
        if profile_id in seen_ids:
            raise IdentifyError(f"profile database has duplicate id {profile_id}")
        if canonical_sha256 is not None and canonical_sha256 in seen_sha256:
            raise IdentifyError("profile database has a duplicate canonical_sha256")
        if canonical_md5 is not None and canonical_md5 in seen_md5:
            raise IdentifyError("profile database has a duplicate canonical_md5")
        seen_ids.add(profile_id)
        if canonical_sha256 is not None:
            seen_sha256.add(canonical_sha256)
        if canonical_md5 is not None:
            seen_md5.add(canonical_md5)
        profiles.append(
            {
                "id": profile_id,
                "label": label,
                "canonical_sha256": canonical_sha256,
                "canonical_md5": canonical_md5,
                "size_bytes": profile_size_value,
            }
        )
    return profiles


def match_profile(
    result: dict[str, object], profiles: list[dict[str, object]]
) -> Optional[dict[str, str]]:
    for profile in profiles:
        sha256_matches = (
            profile["canonical_sha256"] is None
            or profile["canonical_sha256"] == result["canonical_sha256"]
        )
        md5_matches = (
            profile["canonical_md5"] is None
            or profile["canonical_md5"] == result["canonical_md5"]
        )
        if not sha256_matches or not md5_matches:
            continue
        declared_size = profile["size_bytes"]
        if declared_size is not None and declared_size != result["size_bytes"]:
            raise IdentifyError("matching profile has an inconsistent size_bytes")
        return {"id": str(profile["id"]), "label": str(profile["label"])}
    return None


def print_human(result: dict[str, object]) -> None:
    print(f"Size: {result['size_bytes']} bytes")
    print(f"Format: {result['format']}")
    print(f"Byte order: {result['byte_order']}")
    print(f"Canonical SHA-256: {result['canonical_sha256']}")
    print(f"Canonical MD5: {result['canonical_md5']}")
    print(f"Title: {result['title']}")
    print(f"Game code: {result['game_code']}")
    print(f"Region: {result['region']} ({result['region_code']})")
    print(f"Revision: {result['revision']}")
    profile = result["profile"]
    if isinstance(profile, dict):
        print(f"Profile: {profile['label']} ({profile['id']})")
    elif result["profile_checked"]:
        print("Profile: no match")
    else:
        print("Profile: not checked")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Report N64 ROM identity metadata without writing ROM data."
    )
    parser.add_argument("rom", type=Path, help="path to a .z64, .v64, or .n64 ROM")
    parser.add_argument("--json", action="store_true", help="write machine-readable JSON")
    profile_group = parser.add_mutually_exclusive_group()
    profile_group.add_argument(
        "--profiles",
        type=Path,
        default=DEFAULT_PROFILES,
        help="versioned profile database (default: tools/rom-profiles.json)",
    )
    profile_group.add_argument(
        "--no-profiles",
        dest="profiles",
        action="store_const",
        const=None,
        help="skip profile matching",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        result = identify(args.rom)
        profiles = load_profiles(args.profiles) if args.profiles is not None else []
        result["profile_checked"] = args.profiles is not None
        result["profile"] = match_profile(result, profiles)
    except IdentifyError as error:
        if args.json:
            print(json.dumps({"error": str(error)}, sort_keys=True), file=sys.stderr)
        else:
            print(f"identify-rom: {error}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print_human(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
