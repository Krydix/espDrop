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

from serial_port import open_without_reset


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

MIF_SERVICES = re.compile(
    r"MIF-SERVICES src=(?P<source>[0-9a-f:]+) "
    r"result=(?P<result>-?\d+) records=(?P<records>\d+) "
    r"malformed=(?P<malformed>\d+) ptr=(?P<ptr>\d+) "
    r"srv=(?P<srv>\d+) txt=(?P<txt>\d+) "
    r"airdrop=(?P<airdrop>\d+) tcp=(?P<tcp>\d+) "
    r"udp=(?P<udp>\d+) asquic=(?P<asquic>\d+)",
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
    r"lateness_us=(?P<lateness_us>\d+)"
    r"(?: copresence=(?P<copresence>\d+) "
    r"target=(?P<target>[0-9a-f:]+)"
    r"(?: stage=(?P<stage>anchor|target) "
    r"probe=(?P<probe>[0-9a-f:]+))?)?",
    re.IGNORECASE,
)

TX_SUMMARY = re.compile(r"TX-LAB-SUMMARY (?P<details>.*)", re.IGNORECASE)

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
    r"tcp_src=(?P<tcp_source_port>\d+) "
    r"tcp_dst=(?P<tcp_destination_port>\d+) "
    r"tcp_flags=(?P<tcp_flags>0x[0-9a-f]+) "
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

TX_ADMITTED = re.compile(
    r"TX-LAB-ADMITTED peer=(?P<peer>[0-9a-f:]+) "
    r"evidence=(?P<evidence>[a-z-]+)",
    re.IGNORECASE,
)

TX_ANCHOR_ADMITTED = re.compile(
    r"TX-LAB-ANCHOR-ADMITTED peer=(?P<peer>[0-9a-f:]+) "
    r"evidence=(?P<evidence>[a-z-]+)",
    re.IGNORECASE,
)

TX_AUTO_TARGET = re.compile(
    r"TX-LAB-AUTO-TARGET peer=(?P<peer>[0-9a-f:]+) "
    r"service=(?P<service>[^ ]+) distance=(?P<distance>\d+)",
    re.IGNORECASE,
)

TX_TOPOLOGY_WAIT = re.compile(
    r"TX-LAB-WAIT peer=(?P<peer>[0-9a-f:]+) "
    r"observed_distance=(?P<observed_distance>\d+) "
    r"count=(?P<count>\d+)",
    re.IGNORECASE,
)

TX_TOPOLOGY = re.compile(
    r"TX-LAB-TOPOLOGY peer=(?P<peer>[0-9a-f:]+) "
    r"observed_distance=(?P<observed_distance>\d+) "
    r"result=(?P<result>[a-z-]+)",
    re.IGNORECASE,
)

TX_ELECTION = re.compile(
    r"TX-LAB-ELECTION self=(?P<self>[0-9a-f:]+) "
    r"sync=(?P<sync>[0-9a-f:]+) master=(?P<master>[0-9a-f:]+) "
    r"distance=(?P<distance>\d+) metric=(?P<metric>\d+) "
    r"counter=(?P<counter>\d+) peers=(?P<peers>\d+)",
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
    r"next=(?P<next_header>\d+) tcp_src=(?P<tcp_source_port>\d+) "
    r"tcp_dst=(?P<tcp_destination_port>\d+) "
    r"tcp_flags=(?P<tcp_flags>0x[0-9a-f]+) "
    r"tcp_seq=(?P<tcp_sequence>\d+) "
    r"tcp_checksum=(?P<tcp_checksum>\d+) "
    r"driver=(?P<driver>[A-Z0-9_]+) count=(?P<count>\d+)",
    re.IGNORECASE,
)

MDNS_QUERY = re.compile(
    r"AWDL-MDNS-QUERY attempt=(?P<attempt>\d+) "
    r"bytes=(?P<bytes>-?\d+) error=(?P<error>\d+)",
    re.IGNORECASE,
)

MDNS_RESOLVE = re.compile(
    r"AWDL-MDNS-RESOLVE round=(?P<round>\d+) "
    r"name=(?P<name>[^ ]+) type=(?P<type>\d+) "
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
    r"packets=(?P<packets>\d+) responses=(?P<responses>\d+) "
    r"services=(?P<services>\d+) complete=(?P<complete>\d+) "
    r"tcp_attempts=(?P<tcp_attempts>\d+) "
    r"tcp_connected=(?P<tcp_connected>\d+)",
    re.IGNORECASE,
)

MDNS_SERVICE = re.compile(
    r"AWDL-AIRDROP-SERVICE instance=(?P<instance>[^ ]+) "
    r"target=(?P<target>[^ ]+) ipv6=(?P<ipv6>[^ ]+) "
    r"port=(?P<port>\d+) ptr=(?P<ptr>\d+) srv=(?P<srv>\d+) "
    r"txt_record=(?P<txt_record>\d+) aaaa=(?P<aaaa>\d+) "
    r"complete=(?P<complete>\d+) txt=(?P<txt>.*)",
    re.IGNORECASE,
)

MDNS_ENDPOINT = re.compile(
    r"AWDL-AIRDROP-ENDPOINT instance=(?P<instance>[^ ]+) "
    r"ipv6=(?P<ipv6>[^ ]+) port=(?P<port>\d+) "
    r"peer=(?P<peer>[0-9a-f:]+) complete=(?P<complete>\d+)",
    re.IGNORECASE,
)

AIRDROP_TCP = re.compile(
    r"AWDL-AIRDROP-TCP instance=(?P<instance>[^ ]+) "
    r"target=(?P<target>[^ ]+) ipv6=(?P<ipv6>[^ ]+) "
    r"port=(?P<port>\d+) result=(?P<result>[^ ]+) "
    r"error=(?P<error>\d+)",
    re.IGNORECASE,
)

AIRDROP_TLS = re.compile(
    r"AWDL-AIRDROP-TLS instance=(?P<instance>[^ ]+) "
    r"result=(?P<result>[^ ]+) error=(?P<error>-?\d+) "
    r"version=(?P<version>[^ ]+) cipher=(?P<cipher>[^ ]+) "
    r"verify=(?P<verify>0x[0-9a-f]+) "
    r"peer_cert=(?P<peer_cert>\d+) "
    r"peer_cert_bytes=(?P<peer_cert_bytes>\d+)",
    re.IGNORECASE,
)

AIRDROP_DISCOVER = re.compile(
    r"AWDL-AIRDROP-DISCOVER instance=(?P<instance>[^ ]+) "
    r"attempted=(?P<attempted>\d+) result=(?P<result>[^ ]+) "
    r"error=(?P<error>-?\d+) status=(?P<status>\d+) "
    r"request_bytes=(?P<request_bytes>\d+) "
    r"response_bytes=(?P<response_bytes>\d+) "
    r"body_bytes=(?P<body_bytes>\d+) bplist=(?P<bplist>\d+) "
    r"receiver_name=(?P<receiver_name>\d+) "
    r"chunked=(?P<chunked>\d+) type=(?P<content_type>[^ ]+) "
    r"encoding=(?P<content_encoding>[^ ]+)",
    re.IGNORECASE,
)

AIRDROP_ASK = re.compile(
    r"AWDL-AIRDROP-ASK instance=(?P<instance>[^ ]+) "
    r"attempted=(?P<attempted>\d+) result=(?P<result>[^ ]+) "
    r"error=(?P<error>-?\d+) status=(?P<status>\d+) "
    r"request_bytes=(?P<request_bytes>\d+) "
    r"response_bytes=(?P<response_bytes>\d+) "
    r"body_bytes=(?P<body_bytes>\d+) bplist=(?P<bplist>\d+) "
    r"receiver_name=(?P<receiver_name>\d+) "
    r"ids_session=(?P<ids_session>\d+) "
    r"receiver_pseudonym=(?P<receiver_pseudonym>\d+) "
    r"receiver_push_token=(?P<receiver_push_token>\d+) "
    r"chunked=(?P<chunked>\d+) transfer_id=(?P<transfer_id>[^ ]+) "
    r"upload=(?P<upload>[^ ]+)",
    re.IGNORECASE,
)

AIRDROP_UPLOAD = re.compile(
    r"AWDL-AIRDROP-UPLOAD instance=(?P<instance>[^ ]+) "
    r"attempted=(?P<attempted>\d+) result=(?P<result>[^ ]+) "
    r"error=(?P<error>-?\d+) status=(?P<status>\d+) "
    r"request_bytes=(?P<request_bytes>\d+) "
    r"payload_bytes=(?P<payload_bytes>\d+) "
    r"archive_bytes=(?P<archive_bytes>\d+) "
    r"compressed_bytes=(?P<compressed_bytes>\d+) "
    r"file_bytes=(?P<file_bytes>\d+) "
    r"dvzip_blocks=(?P<dvzip_blocks>\d+) "
    r"stored=(?P<stored>\d+) workspace=(?P<workspace>\d+) "
    r"crc32=(?P<crc32>[0-9a-f]+) "
    r"stream_status=(?P<stream_status>-?\d+) "
    r"response_bytes=(?P<response_bytes>\d+) "
    r"body_bytes=(?P<body_bytes>\d+) "
    r"transfer_id=(?P<transfer_id>[^ ]+) "
    r"continuity=(?P<continuity>\d+) retry=(?P<retry>[^ ]+)",
    re.IGNORECASE,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--reset",
        action="store_true",
        help="hard-reset into the application after opening the serial port",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.seconds <= 0:
        raise SystemExit("--seconds must be positive")

    started = datetime.now(timezone.utc)
    frames = []
    diagnostics = []
    raw_mifs = {}
    mif_results = []
    mif_services = []
    tx_frames = []
    tx_windows = []
    tx_summary = None
    tx_admission = None
    tx_anchor_admissions = []
    tx_auto_target = None
    tx_topology_waits = []
    tx_topology = None
    tx_elections = []
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
    mdns_resolution_queries = []
    mdns_packets = []
    mdns_summary = None
    mdns_services = []
    mdns_endpoints = []
    airdrop_tcp = []
    airdrop_tls = []
    airdrop_discover = []
    airdrop_ask = []
    airdrop_upload = []
    boot_lines = []
    with open_without_reset(args.port, timeout=0.25) as connection:
        if args.reset:
            # Match ESP-IDF monitor's normal-boot reset sequence.  Put IO0 and
            # EN in their idle states first, then pulse EN without selecting
            # the ROM bootloader.  Keeping the port open captures the bounded
            # lab's first line instead of racing the native USB console.
            connection.reset_input_buffer()
            connection.rts = False
            connection.dtr = False
            time.sleep(0.05)
            connection.rts = True
            time.sleep(0.10)
            connection.rts = False
            time.sleep(0.25)
        deadline = time.monotonic() + args.seconds
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
            services_match = MIF_SERVICES.search(line)
            if services_match:
                service = services_match.groupdict()
                for key in (
                    "result", "records", "malformed", "ptr", "srv",
                    "txt", "airdrop", "tcp", "udp", "asquic",
                ):
                    service[key] = int(service[key])
                mif_services.append(service)
            tx_frame_match = TX_FRAME.search(line)
            if tx_frame_match:
                record = tx_frame_match.groupdict()
                for key in ("number", "subtype", "bytes"):
                    record[key] = int(record[key])
                tx_frames.append(record)
            tx_window_match = TX_WINDOW.search(line)
            if tx_window_match:
                window = tx_window_match.groupdict()
                for key in (
                    "number", "scheduled", "actual", "lateness_us",
                    "copresence",
                ):
                    if window[key] is not None:
                        window[key] = int(window[key])
                    else:
                        window.pop(key)
                if window["target"] is None:
                    window.pop("target")
                if window["stage"] is None:
                    window.pop("stage")
                if window["probe"] is None:
                    window.pop("probe")
                tx_windows.append(window)
            tx_summary_match = TX_SUMMARY.search(line)
            if tx_summary_match:
                tx_summary = {
                    key: int(value)
                    for key, value in re.findall(
                        r"([a-z0-9_]+)=(\d+)",
                        tx_summary_match.group("details"),
                        re.IGNORECASE,
                    )
                }
            tx_reaction_match = TX_REACTION.search(line)
            if tx_reaction_match:
                reaction = tx_reaction_match.groupdict()
                reaction["count"] = int(reaction["count"])
                tx_reactions.append(reaction)
            tx_admitted_match = TX_ADMITTED.search(line)
            if tx_admitted_match:
                tx_admission = tx_admitted_match.groupdict()
            tx_anchor_admitted_match = TX_ANCHOR_ADMITTED.search(line)
            if tx_anchor_admitted_match:
                tx_anchor_admissions.append(
                    tx_anchor_admitted_match.groupdict()
                )
            tx_auto_target_match = TX_AUTO_TARGET.search(line)
            if tx_auto_target_match:
                tx_auto_target = tx_auto_target_match.groupdict()
                tx_auto_target["distance"] = int(
                    tx_auto_target["distance"]
                )
            tx_topology_wait_match = TX_TOPOLOGY_WAIT.search(line)
            if tx_topology_wait_match:
                topology_wait = tx_topology_wait_match.groupdict()
                topology_wait["observed_distance"] = int(
                    topology_wait["observed_distance"]
                )
                topology_wait["count"] = int(topology_wait["count"])
                tx_topology_waits.append(topology_wait)
            tx_topology_match = TX_TOPOLOGY.search(line)
            if tx_topology_match:
                tx_topology = tx_topology_match.groupdict()
                tx_topology["observed_distance"] = int(
                    tx_topology["observed_distance"]
                )
            tx_election_match = TX_ELECTION.search(line)
            if tx_election_match:
                election = tx_election_match.groupdict()
                for key in ("distance", "metric", "counter", "peers"):
                    election[key] = int(election[key])
                tx_elections.append(election)
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
                    "icmp_type", "tcp_source_port",
                    "tcp_destination_port", "directed",
                ):
                    record[key] = int(record[key])
                record["tcp_flags"] = int(record["tcp_flags"], 16)
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
                for key in (
                    "bytes", "next_header", "tcp_source_port",
                    "tcp_destination_port", "tcp_sequence", "tcp_checksum",
                    "count",
                ):
                    record[key] = int(record[key])
                record["tcp_flags"] = int(record["tcp_flags"], 16)
                netif_tx.append(record)
            mdns_query_match = MDNS_QUERY.search(line)
            if mdns_query_match:
                mdns_queries.append({
                    key: int(value)
                    for key, value in mdns_query_match.groupdict().items()
                })
            mdns_resolve_match = MDNS_RESOLVE.search(line)
            if mdns_resolve_match:
                resolution_query = mdns_resolve_match.groupdict()
                for key in ("round", "type", "bytes", "error"):
                    resolution_query[key] = int(resolution_query[key])
                mdns_resolution_queries.append(resolution_query)
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
            mdns_service_match = MDNS_SERVICE.search(line)
            if mdns_service_match:
                service = mdns_service_match.groupdict()
                for key in ("port", "ptr", "srv", "txt_record", "aaaa",
                            "complete"):
                    service[key] = int(service[key])
                mdns_services.append(service)
            mdns_endpoint_match = MDNS_ENDPOINT.search(line)
            if mdns_endpoint_match:
                endpoint = mdns_endpoint_match.groupdict()
                endpoint["port"] = int(endpoint["port"])
                endpoint["complete"] = int(endpoint["complete"])
                mdns_endpoints.append(endpoint)
            tcp_match = AIRDROP_TCP.search(line)
            if tcp_match:
                tcp_probe = tcp_match.groupdict()
                tcp_probe["port"] = int(tcp_probe["port"])
                tcp_probe["error"] = int(tcp_probe["error"])
                airdrop_tcp.append(tcp_probe)
            tls_match = AIRDROP_TLS.search(line)
            if tls_match:
                tls_probe = tls_match.groupdict()
                tls_probe["error"] = int(tls_probe["error"])
                tls_probe["verify"] = int(tls_probe["verify"], 16)
                tls_probe["peer_cert"] = int(tls_probe["peer_cert"])
                tls_probe["peer_cert_bytes"] = int(
                    tls_probe["peer_cert_bytes"]
                )
                airdrop_tls.append(tls_probe)
            discover_match = AIRDROP_DISCOVER.search(line)
            if discover_match:
                discover = discover_match.groupdict()
                for key in (
                    "attempted", "error", "status", "request_bytes",
                    "response_bytes", "body_bytes", "bplist",
                    "receiver_name", "chunked",
                ):
                    discover[key] = int(discover[key])
                airdrop_discover.append(discover)
            ask_match = AIRDROP_ASK.search(line)
            if ask_match:
                ask = ask_match.groupdict()
                for key in (
                    "attempted", "error", "status", "request_bytes",
                    "response_bytes", "body_bytes", "bplist",
                    "receiver_name", "ids_session", "receiver_pseudonym",
                    "receiver_push_token", "chunked",
                ):
                    ask[key] = int(ask[key])
                airdrop_ask.append(ask)
            upload_match = AIRDROP_UPLOAD.search(line)
            if upload_match:
                upload = upload_match.groupdict()
                for key in (
                    "attempted", "error", "status", "request_bytes",
                    "payload_bytes", "archive_bytes", "compressed_bytes",
                    "file_bytes", "response_bytes", "body_bytes",
                    "dvzip_blocks", "stored", "workspace", "stream_status",
                    "continuity",
                ):
                    upload[key] = int(upload[key])
                upload["crc32"] = int(upload["crc32"], 16)
                airdrop_upload.append(upload)

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
        "resetAtStart": args.reset,
        "framesLogged": len(frames),
        "uniqueSources": unique_sources,
        "mifLogged": sum(item["subtype"].upper() == "MIF" for item in frames),
        "psfLogged": sum(item["subtype"].upper() == "PSF" for item in frames),
        "frames": frames,
        "diagnostics": diagnostics,
        "finalDiagnostics": diagnostics[-1] if diagnostics else None,
        "rawMifCaptures": sorted(raw_captures, key=lambda item: item["source"]),
        "mifResults": mif_results,
        "mifServices": mif_services,
        "txLab": {
            "scheduledWindows": tx_windows,
            "sampledFrames": tx_frames,
            "summary": tx_summary,
            "admission": tx_admission,
            "anchorAdmissions": tx_anchor_admissions,
            "autoTarget": tx_auto_target,
            "topologyWaits": tx_topology_waits,
            "topology": tx_topology,
            "elections": tx_elections,
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
            "mdnsResolutionQueries": mdns_resolution_queries,
            "mdnsPackets": mdns_packets,
            "mdnsSummary": mdns_summary,
            "airdropServices": mdns_services,
            "airdropEndpoints": mdns_endpoints,
            "airdropTcp": airdrop_tcp,
            "airdropTls": airdrop_tls,
            "airdropDiscover": airdrop_discover,
            "airdropAsk": airdrop_ask,
            "airdropUpload": airdrop_upload,
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
