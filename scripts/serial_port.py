"""Serial-port helpers shared by espDrop's USB maintenance tools."""

from __future__ import annotations

import serial


def open_without_reset(device: str, *, timeout: float) -> serial.Serial:
    """Open an ESP USB serial port without pulsing its reset control lines."""
    connection = serial.Serial(
        port=None,
        baudrate=115200,
        timeout=timeout,
        exclusive=True,
    )
    connection.port = device
    # macOS opens the ESP32-S3 USB Serial/JTAG endpoint with both lines
    # asserted.  Keeping that state avoids the DTR/RTS edge sequence which
    # the chip interprets as USB_UART_CHIP_RESET.
    connection.dtr = True
    connection.rts = True
    try:
        connection.open()
    except BaseException:
        connection.close()
        raise
    return connection
