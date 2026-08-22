#!/usr/bin/env python3
"""Capture espDrop AWDL probe logs and write a machine-readable lab artifact."""

from __future__ import annotations

import argparse
import json
import re
import time
from datetime import datetime, timezone
from pathlib import Path

import serial


FRAME = re.compile(
    r"AWDL (?P<subtype>MIF|PSF) "
    r"src=(?P<source>[0-9a-f:]+) "
    r"rssi=(?P<rssi>-?\d+) channel=(?P<channel>\d+) "
    r"ts=(?P<timestamp>\d+) count=(?P<count>\d+)",
    re.IGNORECASE,
)

DIAGNOSTIC = re.compile(
    r"AWDL-DIAG mgmt=(?P<management>\d+) action=(?P<action>\d+) "
    r"bssid=(?P<bssid>\d+) vendor=(?P<vendor>\d+) "
    r"apple=(?P<apple>\d+) header=(?P<header>\d+) "
    r"decoded=(?P<decoded>\d+) dropped=(?P<dropped>\d+)",
    re.IGNORECASE,
)

RAW_MIF = re.compile(
    r"MIF-RAW src=(?P<source>[0-9a-f:]+) offset=(?P<offset>\d+) "
    r"frame=(?P<frame>\d+) captured=(?P<captured>\d+) "
    r"data=(?P<data>[0-9a-f]+)",
    re.IGNORECASE,
)

MIF_RESULT = re.compile(
    r"MIF-(?P<kind>PARSE|SYNC|ELECTION|CHANSEQ) "
    r"src=(?P<source>[0-9a-f:]+) (?P<details>.*)",
    re.IGNORECASE,
)

TX_FRAME = re.compile(
    r"TX-LAB-FRAME number=(?P<number>\d+) subtype=(?P<subtype>\d+) "
    r"bytes=(?P<bytes>\d+) driver=(?P<driver>[A-Z0-9_]+)",
    re.IGNORECASE,
)

TX_SUMMARY = re.compile(
    r"TX-LAB-SUMMARY attempted=(?P<attempted>\d+) "
    r"accepted=(?P<accepted>\d+) errors=(?P<errors>\d+) "
    r"radio_completed=(?P<radio_completed>\d+) "
    r"radio_success=(?P<radio_success>\d+) "
    r"radio_failed=(?P<radio_failed>\d+) "
    r"directed_reactions=(?P<directed_reactions>\d+)",
    re.IGNORECASE,
)

TX_REACTION = re.compile(
    r"TX-LAB-REACTION directed AWDL action from "
    r"(?P<source>[0-9a-f:]+) count=(?P<count>\d+)",
    re.IGNORECASE,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.seconds <= 0:
        raise SystemExit("--seconds must be positive")

    started = datetime.now(timezone.utc)
    deadline = time.monotonic() + args.seconds
    frames = []
    diagnostics = []
    raw_mifs = {}
    mif_results = []
    tx_frames = []
    tx_summary = None
    tx_reactions = []
    boot_lines = []
    with serial.Serial(args.port, 115200, timeout=0.25) as connection:
        connection.dtr = False
        connection.rts = False
        while time.monotonic() < deadline:
            raw = connection.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            print(line)
            if len(boot_lines) < 500:
                boot_lines.append(line)
            match = FRAME.search(line)
            if match:
                record = match.groupdict()
                for key in ("rssi", "channel", "timestamp", "count"):
                    record[key] = int(record[key])
                frames.append(record)
            diagnostic_match = DIAGNOSTIC.search(line)
            if diagnostic_match:
                diagnostics.append(
                    {key: int(value)
                     for key, value in diagnostic_match.groupdict().items()}
                )
            raw_match = RAW_MIF.search(line)
            if raw_match:
                raw = raw_match.groupdict()
                source = raw["source"].lower()
                capture = raw_mifs.setdefault(
                    source,
                    {
                        "source": source,
                        "frameLength": int(raw["frame"]),
                        "capturedLength": int(raw["captured"]),
                        "chunks": {},
                    },
                )
                capture["chunks"][int(raw["offset"])] = raw["data"].lower()
            result_match = MIF_RESULT.search(line)
            if result_match:
                mif_results.append(result_match.groupdict())
            tx_frame_match = TX_FRAME.search(line)
            if tx_frame_match:
                record = tx_frame_match.groupdict()
                for key in ("number", "subtype", "bytes"):
                    record[key] = int(record[key])
                tx_frames.append(record)
            tx_summary_match = TX_SUMMARY.search(line)
            if tx_summary_match:
                tx_summary = {
                    key: int(value)
                    for key, value in tx_summary_match.groupdict().items()
                }
            tx_reaction_match = TX_REACTION.search(line)
            if tx_reaction_match:
                reaction = tx_reaction_match.groupdict()
                reaction["count"] = int(reaction["count"])
                tx_reactions.append(reaction)

    unique_sources = sorted({item["source"] for item in frames})
    raw_captures = []
    for capture in raw_mifs.values():
        ordered = sorted(capture.pop("chunks").items())
        expected_offset = 0
        hex_parts = []
        complete = True
        for offset, data in ordered:
            if offset != expected_offset or len(data) % 2:
                complete = False
            expected_offset = offset + len(data) // 2
            hex_parts.append(data)
        capture["hex"] = "".join(hex_parts)
        capture["complete"] = (
            complete and expected_offset == capture["capturedLength"]
        )
        raw_captures.append(capture)
    artifact = {
        "schema": 1,
        "startedAt": started.isoformat(timespec="seconds"),
        "durationSeconds": args.seconds,
        "port": args.port,
        "framesLogged": len(frames),
        "uniqueSources": unique_sources,
        "mifLogged": sum(item["subtype"].upper() == "MIF" for item in frames),
        "psfLogged": sum(item["subtype"].upper() == "PSF" for item in frames),
        "frames": frames,
        "diagnostics": diagnostics,
        "finalDiagnostics": diagnostics[-1] if diagnostics else None,
        "rawMifCaptures": sorted(raw_captures, key=lambda item: item["source"]),
        "mifResults": mif_results,
        "txLab": {
            "sampledFrames": tx_frames,
            "summary": tx_summary,
            "reactions": tx_reactions,
        },
        "bootLogPrefix": boot_lines,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(artifact, indent=2) + "\n", encoding="utf-8")
    print(
        f"captured {len(frames)} logged AWDL records from "
        f"{len(unique_sources)} source(s); wrote {args.output}"
    )


if __name__ == "__main__":
    main()
