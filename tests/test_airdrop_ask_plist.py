#!/usr/bin/env python3
import plistlib
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_airdrop_ask_plist.py FIXTURE")
    with Path(sys.argv[1]).open("rb") as fixture:
        value = plistlib.load(fixture)
    assert value == {
        "BundleID": "com.apple.finder",
        "ConvertMediaFormats": False,
        "Files": [
            {
                "ConvertMediaFormats": 0,
                "FileBomPath": "./hello.jpg",
                "FileIsDirectory": False,
                "FileName": "hello.jpg",
                "FileType": "public.jpeg",
            }
        ],
        "SenderComputerName": "espDrop",
        "SenderID": "1cdbd4423fa0",
        "SenderModelName": "ESP32-S3",
        "TransferID": {"id": "00112233-4455-4677-8899-AABBCCDDEEFF"},
        "TransferType": {"files": {}},
    }
    print("AirDrop Ask plist tests passed")


if __name__ == "__main__":
    main()
