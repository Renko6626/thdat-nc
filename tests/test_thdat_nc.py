#!/usr/bin/env python3

import os
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LINUX_BINARY = Path(os.environ.get("THDAT_NC", ROOT / "build/thdat-nc"))
WINDOWS_BINARY = ROOT / "bin/windows-x86_64/thdat-nc.exe"
MASK64 = (1 << 64) - 1
GOLDEN64 = 0x9E3779B97F4A7C15
COMPRESSED_PAYLOAD = bytes.fromhex(
    "28b52ffd205d350100f84e4320636f6d7072657373656420666978747572652070"
    "61796c6f61642e0a01004f48ca09"
)


def mix64(value: int) -> int:
    value ^= value >> 30
    value = value * 0xBF58476D1CE4E5B9 & MASK64
    value ^= value >> 27
    value = value * 0x94D049BB133111EB & MASK64
    return (value ^ (value >> 31)) & MASK64


def xor_key(seed: int) -> bytes:
    state = ((seed * 0x9E3779B1 + 1) ^ (seed << 32)) & MASK64
    return struct.pack(
        "<QQ",
        mix64((state + GOLDEN64) & MASK64),
        mix64((state + 2 * GOLDEN64) & MASK64),
    )


def crypt(data: bytes, seed: int) -> bytes:
    key = xor_key(seed)
    return bytes(value ^ key[index & 15] for index, value in enumerate(data))


def write_fixture(path: Path) -> dict[str, bytes]:
    entries = [
        ("plain.txt", 0, 0x10203040, b"plain fixture payload\n", b"plain fixture payload\n"),
        (
            "compressed.bin",
            1,
            0x50607080,
            b"NC compressed fixture payload.\n" * 3,
            COMPRESSED_PAYLOAD,
        ),
    ]
    index_size = sum(32 + len(name.encode("utf-8")) for name, *_ in entries)
    offset = 8 + index_size
    records = []
    payloads = []
    expected = {}
    for name, flags, checksum, unpacked, stored in entries:
        encoded_name = name.encode("utf-8")
        records.append(
            struct.pack(
                "<HIQQQH",
                flags,
                checksum,
                len(unpacked),
                len(stored),
                offset,
                len(encoded_name),
            )
            + encoded_name
        )
        payloads.append(crypt(stored, checksum))
        expected[name] = unpacked
        offset += len(stored)
    directory = b"".join(records)
    directory_seed = zlib.crc32(path.stem.encode("ascii")) & 0xFFFFFFFF
    path.write_bytes(b"PKGL" + struct.pack("<I", len(directory)) + crypt(directory, directory_seed) + b"".join(payloads))
    return expected


def read_directory_records(path: Path) -> tuple[bytes, list[dict[str, int | str]]]:
    archive = path.read_bytes()
    if archive[:4] != b"PKGL":
        raise AssertionError("created archive has no PKGL magic")
    index_size = struct.unpack_from("<I", archive, 4)[0]
    encrypted = archive[8 : 8 + index_size]
    seed = zlib.crc32(path.stem.encode("ascii")) & 0xFFFFFFFF
    directory = crypt(encrypted, seed)
    records = []
    cursor = 0
    while cursor < len(directory):
        flags, checksum, size, stored_size, offset, name_size = struct.unpack_from(
            "<HIQQQH", directory, cursor
        )
        cursor += 32
        name = directory[cursor : cursor + name_size].decode("utf-8")
        cursor += name_size
        records.append(
            {
                "name": name,
                "flags": flags,
                "checksum": checksum,
                "size": size,
                "stored_size": stored_size,
                "offset": offset,
            }
        )
    return archive, records


def run_tool(*arguments: str) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run([str(LINUX_BINARY), *arguments], text=True, capture_output=True)
    except FileNotFoundError:
        raise AssertionError(f"missing test binary: {LINUX_BINARY}") from None


class ThdatNcCliTests(unittest.TestCase):
    def test_help_documents_all_modes(self):
        result = run_tool("-h")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("-c ARCHIVE INPUT-DIRECTORY", result.stdout)
        self.assertIn("-l", result.stdout)
        self.assertIn("-x", result.stdout)
        self.assertIn("-C", result.stdout)

    def test_lists_and_extracts_owned_fixture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "sample.dat"
            output = root / "output"
            expected = write_fixture(archive)

            listing = run_tool("-l", str(archive))
            extraction = run_tool("-x", "-C", str(output), str(archive))

            self.assertEqual(listing.returncode, 0, listing.stderr)
            self.assertEqual(
                listing.stdout.splitlines(),
                ["Name\tSize\tStored", "plain.txt\t22\t22", "compressed.bin\t93\t47"],
            )
            self.assertEqual(extraction.returncode, 0, extraction.stderr)
            self.assertEqual(
                {path.name: path.read_bytes() for path in output.iterdir()},
                expected,
            )

    def test_create_roundtrips_with_canonical_layout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            (source / "nested").mkdir(parents=True)
            expected = {
                "nested/compressible.bin": b"A" * 4096,
                "plain.bin": bytes(range(256)),
            }
            for name, payload in expected.items():
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(payload)
            archive = root / "packed.dat"
            output = root / "output"

            creation = run_tool("-c", str(archive), str(source))
            extraction = run_tool("-x", "-C", str(output), str(archive))

            self.assertEqual(creation.returncode, 0, creation.stderr)
            self.assertEqual(extraction.returncode, 0, extraction.stderr)
            self.assertEqual(
                {
                    path.relative_to(output).as_posix(): path.read_bytes()
                    for path in output.rglob("*")
                    if path.is_file()
                },
                expected,
            )
            archive_bytes, records = read_directory_records(archive)
            self.assertEqual([record["name"] for record in records], list(expected))
            self.assertEqual([record["flags"] for record in records], [1, 0])
            self.assertEqual(
                [record["checksum"] for record in records],
                [0xE95B5568, 0x9F1DC070],
            )
            self.assertTrue(all(record["offset"] % 16 == 0 for record in records))
            first_offset = int(records[0]["offset"])
            index_end = 8 + struct.unpack_from("<I", archive_bytes, 4)[0]
            self.assertEqual(set(archive_bytes[index_end:first_offset]), {0})
            first_end = first_offset + int(records[0]["stored_size"])
            second_offset = int(records[1]["offset"])
            self.assertEqual(set(archive_bytes[first_end:second_offset]), {0})
            second_end = second_offset + int(records[1]["stored_size"])
            self.assertEqual(archive_bytes[second_end:], b"\0")

    def test_create_is_reproducible_for_the_same_archive_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            (source / "content.bin").write_bytes(b"repeatable" * 100)
            first = root / "one" / "same.dat"
            second = root / "two" / "same.dat"
            first.parent.mkdir()
            second.parent.mkdir()

            first_result = run_tool("-c", str(first), str(source))
            second_result = run_tool("-c", str(second), str(source))

            self.assertEqual(first_result.returncode, 0, first_result.stderr)
            self.assertEqual(second_result.returncode, 0, second_result.stderr)
            self.assertEqual(first.read_bytes(), second.read_bytes())


class ThdatNcWindowsArtifactTests(unittest.TestCase):
    def test_windows_release_is_static_x64_pe(self):
        self.assertTrue(WINDOWS_BINARY.is_file(), f"missing Windows binary: {WINDOWS_BINARY}")
        description = subprocess.run(
            ["file", str(WINDOWS_BINARY)], text=True, capture_output=True, check=True
        ).stdout.casefold()
        imports = subprocess.run(
            ["objdump", "-p", str(WINDOWS_BINARY)], text=True, capture_output=True, check=True
        ).stdout.casefold()

        self.assertIn("pe32+ executable", description)
        self.assertIn("x86-64", description)
        self.assertNotIn("zstd.dll", imports)
        self.assertNotIn("libzstd", imports)


if __name__ == "__main__":
    unittest.main()
