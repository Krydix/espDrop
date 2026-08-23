#!/usr/bin/env python3
"""Trigger espDrop's one-shot GitHub Pages OTA maintenance boot."""

from __future__ import annotations

import argparse
import sys
import time

import serial

from serial_port import open_without_reset


ACK = b"ESPDROP-OTA-ARMED\n"


def trigger_update(device: str, *, url: str | None, timeout: float) -> None:
    command = "ESPDROP OTA"
    if url is not None:
        if len(url.encode("ascii")) >= 256 or not url.startswith(("http://", "https://")):
            raise ValueError("OTA URL must be an HTTP(S) URL shorter than 256 bytes")
        command += f" {url}"

    with open_without_reset(device, timeout=0.2) as port:
        time.sleep(2.0)
        port.reset_input_buffer()
        port.write(command.encode("ascii") + b"\n")
        port.flush()
        deadline = time.monotonic() + timeout
        received = bytearray()
        while time.monotonic() < deadline:
            received.extend(port.read(512))
            if ACK in received:
                print("espDrop OTA armed; device is restarting into maintenance mode")
                return
            if b"ESPDROP-OTA-NOT-PROVISIONED\n" in received:
                raise SystemExit("maintenance Wi-Fi is not provisioned")
            if b"ESPDROP-OTA-INVALID-URL\n" in received:
                raise SystemExit("firmware rejected the OTA URL")
        raise SystemExit("OTA trigger timed out; ensure normal espDrop firmware is running")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--url")
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()
    trigger_update(args.port, url=args.url, timeout=args.timeout)


if __name__ == "__main__":
    try:
        main()
    except serial.SerialException as error:
        print(f"serial error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
