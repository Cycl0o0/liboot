#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Cycl0o0
"""ROM-free tests for tools/identify-rom.py."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "identify-rom.py"
MIN_ROM_SIZE = 0x1060
MAX_ROM_SIZE = 256 * 1024 * 1024
STREAMING_ROM_SIZE = 1024 * 1024 + MIN_ROM_SIZE


def synthetic_rom() -> bytes:
    data = bytearray(
        (index * 37 + 11) & 0xFF for index in range(STREAMING_ROM_SIZE)
    )
    data[0:4] = b"\x80\x37\x12\x40"
    data[0x20:0x34] = b"LIBOOT SYNTHETIC".ljust(20, b" ")
    data[0x3B:0x3F] = b"NZLE"
    data[0x3F] = 2
    return bytes(data)


def as_v64(canonical: bytes) -> bytes:
    converted = bytearray(len(canonical))
    converted[0::2] = canonical[1::2]
    converted[1::2] = canonical[0::2]
    return bytes(converted)


def as_n64(canonical: bytes) -> bytes:
    converted = bytearray(len(canonical))
    converted[0::4] = canonical[3::4]
    converted[1::4] = canonical[2::4]
    converted[2::4] = canonical[1::4]
    converted[3::4] = canonical[0::4]
    return bytes(converted)


class RomIdentityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="liboot-rom-identity-")
        self.directory = Path(self.temporary.name)
        self.canonical = synthetic_rom()
        self.digest = hashlib.sha256(self.canonical).hexdigest()
        self.md5_digest = hashlib.md5(self.canonical).hexdigest()  # nosec B324

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_tool(self, *arguments: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOL), *(str(argument) for argument in arguments)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def write(self, name: str, contents: bytes) -> Path:
        path = self.directory / name
        path.write_bytes(contents)
        return path

    def test_all_orders_have_one_canonical_identity(self) -> None:
        variants = (
            ("fixture.z64", self.canonical, "z64", "big-endian"),
            ("fixture.v64", as_v64(self.canonical), "v64", "byte-swapped (16-bit)"),
            (
                "fixture.n64",
                as_n64(self.canonical),
                "n64",
                "little-endian (32-bit word-swapped)",
            ),
        )
        for name, contents, expected_format, expected_order in variants:
            with self.subTest(name=name):
                completed = self.run_tool(
                    "--json", "--no-profiles", self.write(name, contents)
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                result = json.loads(completed.stdout)
                self.assertEqual(result["canonical_sha256"], self.digest)
                self.assertEqual(result["canonical_md5"], self.md5_digest)
                self.assertEqual(result["size_bytes"], len(self.canonical))
                self.assertEqual(result["format"], expected_format)
                self.assertEqual(result["byte_order"], expected_order)
                self.assertEqual(result["title"], "LIBOOT SYNTHETIC")
                self.assertEqual(result["game_code"], "NZLE")
                self.assertEqual(result["region_code"], "E")
                self.assertEqual(result["region"], "North America")
                self.assertEqual(result["revision"], 2)
                self.assertFalse(result["profile_checked"])
                self.assertIsNone(result["profile"])

    def test_human_output_reports_metadata(self) -> None:
        completed = self.run_tool(self.write("fixture.z64", self.canonical))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn(f"Canonical SHA-256: {self.digest}", completed.stdout)
        self.assertIn(f"Canonical MD5: {self.md5_digest}", completed.stdout)
        self.assertIn("Title: LIBOOT SYNTHETIC", completed.stdout)
        self.assertIn("Game code: NZLE", completed.stdout)
        self.assertIn("Region: North America (E)", completed.stdout)
        self.assertIn("Revision: 2", completed.stdout)
        self.assertIn("Profile: no match", completed.stdout)

    def test_rejects_short_oversize_and_bad_magic(self) -> None:
        short = self.write(
            "short.z64", b"\x80\x37\x12\x40" + bytes(MIN_ROM_SIZE - 5)
        )
        completed = self.run_tool(short)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("smaller", completed.stderr)
        self.assertEqual(completed.stdout, "")

        oversize = self.directory / "oversize.z64"
        with oversize.open("wb") as stream:
            stream.write(b"\x80\x37\x12\x40")
            stream.truncate(MAX_ROM_SIZE + 1)
        completed = self.run_tool("--json", oversize)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("larger", json.loads(completed.stderr)["error"])
        self.assertEqual(completed.stdout, "")

        bad = bytearray(self.canonical)
        bad[0:4] = b"NOPE"
        bad[0x20:0x34] = b"PRIVATE-MARKER".ljust(20, b" ")
        completed = self.run_tool(self.write("bad.z64", bytes(bad)))
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("unrecognized", completed.stderr)
        self.assertNotIn("PRIVATE-MARKER", completed.stderr)
        self.assertEqual(completed.stdout, "")

        completed = self.run_tool(self.write("wrong.rom", self.canonical))
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("unsupported ROM extension", completed.stderr)

        misaligned = as_v64(self.canonical) + b"\x00"
        completed = self.run_tool(self.write("misaligned.v64", misaligned))
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("not aligned", completed.stderr)

    def test_profile_match_uses_canonical_digest(self) -> None:
        profile_path = self.directory / "profiles.json"
        profile_path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "profiles": [
                        {
                            "id": "synthetic-rev-2",
                            "label": "Synthetic revision 2",
                            "canonical_sha256": self.digest,
                            "size_bytes": len(self.canonical),
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        completed = self.run_tool(
            "--json",
            "--profiles",
            profile_path,
            self.write("fixture.n64", as_n64(self.canonical)),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertTrue(result["profile_checked"])
        self.assertEqual(
            result["profile"],
            {"id": "synthetic-rev-2", "label": "Synthetic revision 2"},
        )

        profile_path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "profiles": [
                        {
                            "id": "synthetic-md5",
                            "label": "Synthetic MD5 profile",
                            "canonical_md5": self.md5_digest,
                            "size_bytes": len(self.canonical),
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        completed = self.run_tool(
            "--json",
            "--profiles",
            profile_path,
            self.write("fixture.v64", as_v64(self.canonical)),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["profile"]["id"], "synthetic-md5")

    def test_malformed_profile_is_safe_error(self) -> None:
        profile_path = self.directory / "profiles.json"
        profile_path.write_text('{"profiles": ["PRIVATE-PROFILE-DATA"]', encoding="utf-8")
        completed = self.run_tool(
            "--json",
            "--profiles",
            profile_path,
            self.write("fixture.z64", self.canonical),
        )
        self.assertNotEqual(completed.returncode, 0)
        error = json.loads(completed.stderr)["error"]
        self.assertIn("not valid JSON", error)
        self.assertNotIn("PRIVATE-PROFILE-DATA", error)
        self.assertEqual(completed.stdout, "")

        profile_path.write_text(
            json.dumps({"schema_version": 999, "profiles": []}), encoding="utf-8"
        )
        completed = self.run_tool(
            "--json",
            "--profiles",
            profile_path,
            self.write("fixture-2.z64", self.canonical),
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("unsupported profile schema_version", completed.stderr)
        self.assertNotIn(str(profile_path), completed.stderr)

        nesting = max(10000, sys.getrecursionlimit() * 20)
        profile_path.write_text(
            '{"schema_version": 1, "profiles": '
            + "[" * nesting
            + "]" * nesting
            + "}",
            encoding="utf-8",
        )
        completed = self.run_tool(
            "--json",
            "--profiles",
            profile_path,
            self.write("fixture-3.z64", self.canonical),
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("nesting is too deep", completed.stderr)
        self.assertNotIn("Traceback", completed.stderr)

        profile_path.write_text(
            '{"schema_version": 1, "schema_version": 999, "profiles": []}',
            encoding="utf-8",
        )
        completed = self.run_tool(
            "--json",
            "--profiles",
            profile_path,
            self.write("fixture-4.z64", self.canonical),
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("duplicate object key", completed.stderr)
        self.assertNotIn("Traceback", completed.stderr)

        profile_path.write_text(
            '{"schema_version": ' + "9" * 10000 + ', "profiles": []}',
            encoding="utf-8",
        )
        completed = self.run_tool(
            "--json",
            "--profiles",
            profile_path,
            self.write("fixture-5.z64", self.canonical),
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("invalid JSON value", completed.stderr)
        self.assertNotIn("Traceback", completed.stderr)

        for index, constant in enumerate(("NaN", "Infinity", "-Infinity"), 6):
            with self.subTest(constant=constant):
                profile_path.write_text(
                    '{"schema_version": 1, "profiles": [' + constant + "]}",
                    encoding="utf-8",
                )
                completed = self.run_tool(
                    "--json",
                    "--profiles",
                    profile_path,
                    self.write(f"fixture-{index}.z64", self.canonical),
                )
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn("non-standard JSON number", completed.stderr)
                self.assertNotIn("Traceback", completed.stderr)


if __name__ == "__main__":
    unittest.main()
