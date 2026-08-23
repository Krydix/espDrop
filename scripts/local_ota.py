#!/usr/bin/env python3
"""Serve a local firmware image and request it over espDrop maintenance Wi-Fi."""

from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import threading
import time

import serial
from serial.tools import list_ports

from provision_wifi import packet, parse_packets, RPC_COMMAND
from serial_port import open_without_reset
from trigger_ota import trigger_update


RPC_RESULT = 0x04
RPC_DEVICE_INFO = 0x03


def normalized_serial(value: str | None) -> str:
    return (value or "").replace(":", "").replace("-", "").upper()


def verify_target(device: str, expected_serial: str) -> str:
    matches = [port for port in list_ports.comports() if port.device == device]
    if len(matches) != 1:
        raise SystemExit(f"target {device} is not an attached serial device")
    port = matches[0]
    actual = normalized_serial(port.serial_number)
    expected = normalized_serial(expected_serial)
    if port.vid != 0x303A or actual != expected:
        raise SystemExit(
            f"refusing target {device}: expected Espressif serial {expected_serial}, "
            f"found VID={port.vid!r} serial={port.serial_number!r}"
        )
    return port.serial_number or expected_serial


def routed_ipv4() -> str:
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))
        return str(probe.getsockname()[0])
    finally:
        probe.close()


def decode_strings(data: bytes) -> list[str]:
    if len(data) < 2 or data[0] != RPC_DEVICE_INFO or data[1] != len(data) - 2:
        return []
    strings = []
    position = 2
    while position < len(data):
        length = data[position]
        position += 1
        if position + length > len(data):
            return []
        strings.append(data[position:position + length].decode("utf-8", "replace"))
        position += length
    return strings


def wait_for_device_info(device: str, timeout: float = 20.0) -> list[str]:
    deadline = time.monotonic() + timeout
    request = packet(RPC_COMMAND, bytes((RPC_DEVICE_INFO, 0)))
    while time.monotonic() < deadline:
        try:
            with open_without_reset(device, timeout=0.2) as connection:
                connection.reset_input_buffer()
                connection.write(request)
                connection.flush()
                received = bytearray()
                attempt_deadline = min(deadline, time.monotonic() + 2.0)
                while time.monotonic() < attempt_deadline:
                    received.extend(connection.read(512))
                    for packet_type, data in parse_packets(received):
                        if packet_type == RPC_RESULT:
                            strings = decode_strings(data)
                            if strings:
                                return strings
        except serial.SerialException:
            pass
        time.sleep(0.5)
    raise SystemExit("firmware was delivered but the device did not return to normal mode")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--expected-serial", required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--host")
    parser.add_argument("--listen-port", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=50.0)
    args = parser.parse_args()

    firmware = args.firmware.resolve()
    if not firmware.is_file():
        raise SystemExit(f"firmware not found: {firmware}")
    serial_number = verify_target(args.port, args.expected_serial)
    host = args.host or routed_ipv4()
    served = threading.Event()

    class FirmwareHandler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - stdlib handler API
            if self.path != "/espdrop.bin":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(firmware.stat().st_size))
            self.end_headers()
            with firmware.open("rb") as source:
                while chunk := source.read(64 * 1024):
                    self.wfile.write(chunk)
            served.set()

        def log_message(self, message: str, *values: object) -> None:
            print(f"local OTA HTTP: {message % values}")

    server = ThreadingHTTPServer((host, args.listen_port), FirmwareHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://{host}:{server.server_port}/espdrop.bin"
    print(
        f"verified target {args.port}; Espressif serial/MAC={serial_number}; "
        f"serving {firmware.name} at {url}"
    )
    try:
        trigger_update(args.port, url=url, timeout=8.0)
        if not served.wait(args.timeout):
            raise SystemExit("device did not fetch the local firmware before timeout")
        print("local firmware delivered; waiting for validation and restart")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2.0)

    info = wait_for_device_info(args.port)
    version = info[1] if len(info) > 1 else "unknown"
    print(f"local OTA complete; {info[0]} {version} is back in normal mode")


if __name__ == "__main__":
    main()
