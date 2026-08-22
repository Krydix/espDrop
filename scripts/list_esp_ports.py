#!/usr/bin/env python3
"""List Espressif serial ports and distinguish ROM/JTAG from app USB mode."""

from serial.tools import list_ports


def main() -> None:
    found = False
    for port in sorted(list_ports.comports(), key=lambda item: item.device):
        if port.vid != 0x303A:
            continue
        found = True
        if port.pid == 0x1001:
            mode = "ROM/USB Serial-JTAG (flashable)"
        elif port.pid == 0x4001:
            mode = "application USB device (enter BOOT mode to flash)"
        else:
            mode = f"Espressif USB PID 0x{port.pid:04x}"
        print(
            f"{port.device}: {mode}; "
            f"serial={port.serial_number or 'unknown'}"
        )
    if not found:
        print("No Espressif USB serial ports found.")


if __name__ == "__main__":
    main()
