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

TX_WINDOW = re.compile(
    r"TX-LAB-WINDOW number=(?P<number>\d+) "
    r"scheduled=(?P<scheduled>\d+) actual=(?P<actual>\d+) "
    r"lateness_us=(?P<lateness_us>\d+)",
    re.IGNORECASE,
)

TX_SUMMARY = re.compile(
    r"TX-LAB-SUMMARY action_attempted=(?P<action_attempted>\d+) "
    r"action_accepted=(?P<action_accepted>\d+) "
    r"action_errors=(?P<action_errors>\d+) "
    r"action_radio_completed=(?P<action_radio_completed>\d+) "
    r"action_radio_success=(?P<action_radio_success>\d+) "
    r"action_radio_failed=(?P<action_radio_failed>\d+) "
    r"data_attempted=(?P<data_attempted>\d+) "
    r"data_accepted=(?P<data_accepted>\d+) "
    r"data_errors=(?P<data_errors>\d+) "
    r"data_radio_completed=(?P<data_radio_completed>\d+) "
    r"data_radio_success=(?P<data_radio_success>\d+) "
    r"data_radio_failed=(?P<data_radio_failed>\d+) "
    r"ns_radio_completed=(?P<ns_radio_completed>\d+) "
    r"ns_radio_success=(?P<ns_radio_success>\d+) "
    r"ns_radio_failed=(?P<ns_radio_failed>\d+) "
    r"echo_attempted=(?P<echo_attempted>\d+) "
    r"echo_accepted=(?P<echo_accepted>\d+) "
    r"echo_errors=(?P<echo_errors>\d+) "
    r"directed_reactions=(?P<directed_reactions>\d+) "
    r"echo_radio_completed=(?P<echo_radio_completed>\d+) "
    r"echo_radio_success=(?P<echo_radio_success>\d+) "
    r"echo_radio_failed=(?P<echo_radio_failed>\d+) "
    r"unknown_data_radio_completed=(?P<unknown_data_radio_completed>\d+) "
    r"neighbor_advertisements=(?P<neighbor_advertisements>\d+) "
    r"echo_replies=(?P<echo_replies>\d+) "
    r"netif_tx_observed=(?P<netif_tx_observed>\d+) "
    r"netif_tx_enqueued=(?P<netif_tx_enqueued>\d+) "
    r"netif_tx_submitted=(?P<netif_tx_submitted>\d+) "
    r"netif_tx_accepted=(?P<netif_tx_accepted>\d+) "
    r"netif_tx_radio_success=(?P<netif_tx_radio_success>\d+) "
    r"netif_tx_radio_failed=(?P<netif_tx_radio_failed>\d+) "
    r"netif_rx_enqueued=(?P<netif_rx_enqueued>\d+) "
    r"netif_rx_injected=(?P<netif_rx_injected>\d+) "
    r"netif_rx_dropped=(?P<netif_rx_dropped>\d+) "
    r"mdns_queries=(?P<mdns_queries>\d+) "
    r"mdns_packets=(?P<mdns_packets>\d+) "
    r"mdns_responses=(?P<mdns_responses>\d+)",
    re.IGNORECASE,
)

TX_NS = re.compile(
    r"TX-LAB-NS number=(?P<number>\d+) bytes=(?P<bytes>\d+) "
    r"target=(?P<target>[0-9a-f:]+) driver=(?P<driver>[A-Z0-9_]+)",
    re.IGNORECASE,
)

TX_NA = re.compile(
    r"TX-LAB-NA IPv6 Neighbor Advertisement from "
    r"(?P<source>[0-9a-f:]+) count=(?P<count>\d+)",
    re.IGNORECASE,
)

TX_ECHO = re.compile(
    r"TX-LAB-ECHO number=(?P<number>\d+) bytes=(?P<bytes>\d+) "
    r"target=(?P<target>[0-9a-f:]+) driver=(?P<driver>[A-Z0-9_]+)",
    re.IGNORECASE,
)

TX_ECHO_REPLY = re.compile(
    r"TX-LAB-ECHO-REPLY from (?P<source>[0-9a-f:]+) "
    r"id=(?P<identifier>\d+) sequence=(?P<sequence>\d+) "
    r"count=(?P<count>\d+)",
    re.IGNORECASE,
)

AWDL_DATA = re.compile(
    r"AWDL-DATA src=(?P<source>[0-9a-f:]+) "
    r"dst=(?P<destination>[0-9a-f:]+) rssi=(?P<rssi>-?\d+) "
    r"channel=(?P<channel>\d+) bytes=(?P<bytes>\d+) "
    r"seq=(?P<sequence>\d+) qos=(?P<qos>\d+) "
    r"amsdu=(?P<amsdu>\d+) ethertype=(?P<ethertype>0x[0-9a-f]+) "
    r"ipv6=(?P<ipv6>\d+) next=(?P<next_header>\d+) "
    r"hop=(?P<hop_limit>\d+) icmp=(?P<icmp_type>\d+) "
    r"directed=(?P<directed>\d+)",
    re.IGNORECASE,
)

AWDL_DATA_DIAGNOSTIC = re.compile(
    r"AWDL-DATA-DIAG raw=(?P<raw>\d+) "
    r"self_src=(?P<self_source>\d+) self_dst=(?P<self_destination>\d+) "
    r"awdl_bssid=(?P<awdl_bssid>\d+) sampled=(?P<sampled>\d+) "
    r"decoded=(?P<decoded>\d+) ipv6=(?P<ipv6>\d+) "
    r"na=(?P<neighbor_advertisements>\d+) "
    r"echo_reply=(?P<echo_replies>\d+)",
    re.IGNORECASE,
)

RAW_DATA = re.compile(
    r"DATA-RAW result=(?P<result>\d+) fc=(?P<frame_control>0x[0-9a-f]+) "
    r"src=(?P<source>[0-9a-f:]+) dst=(?P<destination>[0-9a-f:]+) "
    r"bssid=(?P<bssid>[0-9a-f:]+) frame=(?P<frame_length>\d+) "
    r"captured=(?P<captured_length>\d+) data=(?P<data>[0-9a-f]+)",
    re.IGNORECASE,
)

TX_REACTION = re.compile(
    r"TX-LAB-REACTION directed AWDL action from "
    r"(?P<source>[0-9a-f:]+) count=(?P<count>\d+)",
    re.IGNORECASE,
)

NETIF_READY = re.compile(
    r"AWDL-NETIF ready if=(?P<interface>\d+) "
    r"ipv6=(?P<ipv6>[^ ]+) mtu=(?P<mtu>\d+)",
    re.IGNORECASE,
)

NETIF_TX = re.compile(
    r"AWDL-NETIF-TX bytes=(?P<bytes>\d+) "
    r"ethertype=(?P<ethertype>0x[0-9a-f]+) "
    r"driver=(?P<driver>[A-Z0-9_]+) count=(?P<count>\d+)",
    re.IGNORECASE,
)

MDNS_QUERY = re.compile(
    r"AWDL-MDNS-QUERY attempt=(?P<attempt>\d+) "
    r"bytes=(?P<bytes>-?\d+) error=(?P<error>\d+)",
    re.IGNORECASE,
)

MDNS_RX = re.compile(
    r"AWDL-MDNS-RX bytes=(?P<bytes>\d+) response=(?P<response>\d+) "
    r"qd=(?P<questions>\d+) an=(?P<answers>\d+) "
    r"ns=(?P<authority>\d+) ar=(?P<additional>\d+) "
    r"count=(?P<count>\d+)",
    re.IGNORECASE,
)

MDNS_SUMMARY = re.compile(
    r"AWDL-MDNS-SUMMARY queries=(?P<queries>\d+) "
    r"packets=(?P<packets>\d+) responses=(?P<responses>\d+)",
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
    tx_windows = []
    tx_summary = None
    tx_reactions = []
    tx_neighbor_solicitations = []
    tx_neighbor_advertisements = []
    tx_echo_requests = []
    tx_echo_replies = []
    awdl_data_frames = []
    awdl_data_diagnostics = []
    raw_data_captures = []
    netif_ready = None
    netif_tx = []
    mdns_queries = []
    mdns_packets = []
    mdns_summary = None
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
            tx_window_match = TX_WINDOW.search(line)
            if tx_window_match:
                tx_windows.append({
                    key: int(value)
                    for key, value in tx_window_match.groupdict().items()
                })
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
            tx_ns_match = TX_NS.search(line)
            if tx_ns_match:
                solicitation = tx_ns_match.groupdict()
                solicitation["number"] = int(solicitation["number"])
                solicitation["bytes"] = int(solicitation["bytes"])
                tx_neighbor_solicitations.append(solicitation)
            tx_na_match = TX_NA.search(line)
            if tx_na_match:
                advertisement = tx_na_match.groupdict()
                advertisement["count"] = int(advertisement["count"])
                tx_neighbor_advertisements.append(advertisement)
            tx_echo_match = TX_ECHO.search(line)
            if tx_echo_match:
                echo = tx_echo_match.groupdict()
                echo["number"] = int(echo["number"])
                echo["bytes"] = int(echo["bytes"])
                tx_echo_requests.append(echo)
            tx_echo_reply_match = TX_ECHO_REPLY.search(line)
            if tx_echo_reply_match:
                reply = tx_echo_reply_match.groupdict()
                for key in ("identifier", "sequence", "count"):
                    reply[key] = int(reply[key])
                tx_echo_replies.append(reply)
            data_match = AWDL_DATA.search(line)
            if data_match:
                record = data_match.groupdict()
                for key in (
                    "rssi", "channel", "bytes", "sequence", "qos",
                    "amsdu", "ipv6", "next_header", "hop_limit",
                    "icmp_type", "directed",
                ):
                    record[key] = int(record[key])
                awdl_data_frames.append(record)
            data_diagnostic_match = AWDL_DATA_DIAGNOSTIC.search(line)
            if data_diagnostic_match:
                awdl_data_diagnostics.append({
                    key: int(value)
                    for key, value in
                    data_diagnostic_match.groupdict().items()
                })
            raw_data_match = RAW_DATA.search(line)
            if raw_data_match:
                capture = raw_data_match.groupdict()
                for key in ("result", "frame_length", "captured_length"):
                    capture[key] = int(capture[key])
                capture["frame_control"] = int(
                    capture["frame_control"], 16
                )
                raw_data_captures.append(capture)
            netif_ready_match = NETIF_READY.search(line)
            if netif_ready_match:
                netif_ready = netif_ready_match.groupdict()
                netif_ready["interface"] = int(netif_ready["interface"])
                netif_ready["mtu"] = int(netif_ready["mtu"])
            netif_tx_match = NETIF_TX.search(line)
            if netif_tx_match:
                record = netif_tx_match.groupdict()
                for key in ("bytes", "count"):
                    record[key] = int(record[key])
                netif_tx.append(record)
            mdns_query_match = MDNS_QUERY.search(line)
            if mdns_query_match:
                mdns_queries.append({
                    key: int(value)
                    for key, value in mdns_query_match.groupdict().items()
                })
            mdns_rx_match = MDNS_RX.search(line)
            if mdns_rx_match:
                mdns_packets.append({
                    key: int(value)
                    for key, value in mdns_rx_match.groupdict().items()
                })
            mdns_summary_match = MDNS_SUMMARY.search(line)
            if mdns_summary_match:
                mdns_summary = {
                    key: int(value)
                    for key, value in mdns_summary_match.groupdict().items()
                }

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
            "scheduledWindows": tx_windows,
            "sampledFrames": tx_frames,
            "summary": tx_summary,
            "reactions": tx_reactions,
            "neighborSolicitations": tx_neighbor_solicitations,
            "neighborAdvertisements": tx_neighbor_advertisements,
            "echoRequests": tx_echo_requests,
            "echoReplies": tx_echo_replies,
        },
        "awdlData": {
            "frames": awdl_data_frames,
            "candidateCaptures": raw_data_captures,
            "diagnostics": awdl_data_diagnostics,
            "finalDiagnostics": (
                awdl_data_diagnostics[-1]
                if awdl_data_diagnostics else None
            ),
        },
        "awdlNetif": {
            "ready": netif_ready,
            "transmittedFrames": netif_tx,
            "mdnsQueries": mdns_queries,
            "mdnsPackets": mdns_packets,
            "mdnsSummary": mdns_summary,
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
