#!/usr/bin/env python3
"""Provision espDrop's maintenance Wi-Fi using Improv Serial over USB."""

from __future__ import annotations

import argparse
import getpass
import sys
import time

import serial

from serial_port import open_without_reset


HEADER = b"IMPROV"
RPC_COMMAND = 0x03
RPC_WIFI_SETTINGS = 0x01
STATE = 0x01
STATE_PROVISIONED = 0x04


def packet(packet_type: int, data: bytes) -> bytes:
    body = HEADER + bytes((1, packet_type, len(data))) + data
    return body + bytes((sum(body) & 0xFF,))


def parse_packets(buffer: bytearray):
    while True:
        start = buffer.find(HEADER)
        if start < 0:
            del buffer[:-5]
            return
        if start:
            del buffer[:start]
        if len(buffer) < 10:
            return
        length = 10 + buffer[8]
        if len(buffer) < length:
            return
        candidate = bytes(buffer[:length])
        del buffer[:length]
        if candidate[6] == 1 and (sum(candidate[:-1]) & 0xFF) == candidate[-1]:
            yield candidate[7], candidate[9:-1]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--ssid")
    parser.add_argument("--timeout", type=float, default=45.0)
    args = parser.parse_args()

    ssid = args.ssid or input("Maintenance Wi-Fi SSID: ").strip()
    password = getpass.getpass("Wi-Fi password (input hidden): ")
    ssid_bytes = ssid.encode("utf-8")
    password_bytes = password.encode("utf-8")
    if not 0 < len(ssid_bytes) <= 32 or len(password_bytes) > 63:
        raise SystemExit("SSID must be 1-32 bytes and password at most 63 bytes")
    rpc_data = bytes((RPC_WIFI_SETTINGS, 2 + len(ssid_bytes) + len(password_bytes)))
    rpc_data += bytes((len(ssid_bytes),)) + ssid_bytes
    rpc_data += bytes((len(password_bytes),)) + password_bytes
    password = ""
    password_bytes = b""

    with open_without_reset(args.port, timeout=0.2) as port:
        time.sleep(2.0)
        port.reset_input_buffer()
        port.write(packet(RPC_COMMAND, rpc_data))
        port.flush()
        deadline = time.monotonic() + args.timeout
        received = bytearray()
        while time.monotonic() < deadline:
            received.extend(port.read(512))
            for packet_type, data in parse_packets(received):
                if packet_type == STATE and data == bytes((STATE_PROVISIONED,)):
                    print("espDrop maintenance Wi-Fi provisioned; device is restarting")
                    return
        raise SystemExit("provisioning timed out; check SSID/password and retry")


if __name__ == "__main__":
    try:
        main()
    except serial.SerialException as error:
        print(f"serial error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
