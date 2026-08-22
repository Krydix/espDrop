#!/usr/bin/env python3
"""Trigger espDrop's one-shot GitHub Pages OTA maintenance boot."""

from __future__ import annotations

import argparse
import sys
import time

import serial


COMMAND = b"ESPDROP OTA\n"
ACK = b"ESPDROP-OTA-ARMED\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()

    with serial.Serial(args.port, 115200, timeout=0.2, exclusive=True) as port:
        port.dtr = False
        port.rts = False
        time.sleep(2.0)
        port.reset_input_buffer()
        port.write(COMMAND)
        port.flush()
        deadline = time.monotonic() + args.timeout
        received = bytearray()
        while time.monotonic() < deadline:
            received.extend(port.read(512))
            if ACK in received:
                print("espDrop OTA armed; device is restarting into maintenance mode")
                return
            if b"ESPDROP-OTA-NOT-PROVISIONED\n" in received:
                raise SystemExit("maintenance Wi-Fi is not provisioned")
        raise SystemExit("OTA trigger timed out; ensure normal espDrop firmware is running")


if __name__ == "__main__":
    try:
        main()
    except serial.SerialException as error:
        print(f"serial error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
