#!/usr/bin/env python3
"""Independently validate the deterministic odc/dvzip upload fixture."""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


HEADER_BYTES = 76


def read_entry(archive: bytes, offset: int) -> tuple[str, bytes, int]:
    header = archive[offset : offset + HEADER_BYTES]
    assert len(header) == HEADER_BYTES
    assert header[:6] == b"070707"
    assert all(byte in b"01234567" for byte in header[6:])
    name_bytes = int(header[59:65], 8)
    file_bytes = int(header[65:76], 8)
    offset += HEADER_BYTES
    raw_name = archive[offset : offset + name_bytes]
    assert len(raw_name) == name_bytes and raw_name[-1:] == b"\0"
    name = raw_name[:-1].decode("ascii")
    offset += name_bytes
    data = archive[offset : offset + file_bytes]
    assert len(data) == file_bytes
    return name, data, offset + file_bytes


def main() -> None:
    archive = Path(sys.argv[1]).read_bytes()
    assert len(archive) == 10_240
    name, jpeg, offset = read_entry(archive, 0)
    assert name == "./hello.jpg"
    assert jpeg == bytes(
        [
            0xFF,
            0xD8,
            0xFF,
            0xE0,
            0x00,
            0x10,
            0x4A,
            0x46,
            0x49,
            0x46,
            0x00,
            0x01,
            0xFF,
            0xD9,
        ]
    )
    trailer, trailer_data, offset = read_entry(archive, offset)
    assert trailer == "TRAILER!!!" and trailer_data == b""
    assert archive[offset:] == bytes(len(archive) - offset)

    compressed = zlib.compress(archive)
    dvzip = struct.pack(">I", len(compressed)) + compressed
    block_bytes = struct.unpack(">I", dvzip[:4])[0]
    assert block_bytes == len(dvzip) - 4
    assert zlib.decompress(dvzip[4:]) == archive
    print("AirDrop upload archive tests passed")


if __name__ == "__main__":
    main()
