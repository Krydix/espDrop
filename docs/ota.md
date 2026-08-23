# OTA maintenance design

Status date: 2026-08-23.

espDrop uses ESPresso's dual-slot, rollback-protected GitHub Pages update
pattern, adapted so infrastructure Wi-Fi never remains active during normal
AWDL operation.

## Partition migration

The 16 MiB target layout contains `ota_0` and `ota_1`, each 3 MiB, plus an OTA
selection sector, NVS, PHY data, coredump storage, and approximately 9.8 MiB of
application storage. A board running the earlier factory-only map needs one
complete USB flash. Application-only updates cannot change a partition table.

## Modes

`provisioning`
: Entered when no successful maintenance Wi-Fi setup has been recorded. The
  AWDL probe does not start. Credentials arrive over the physical USB link via
  Improv Serial, are tested, persisted by ESP-IDF, and never logged.

`normal`
: Starts the AWDL research stack and a small USB maintenance-command parser.
  Infrastructure Wi-Fi is not joined. Sending the exact `ESPDROP OTA` command
  records a one-shot request and restarts.

`OTA maintenance`
: Consumes the request before networking, joins the saved network, validates
  time for TLS when required, and fetches the requested target app image. It either
  installs a different version or returns to normal mode after a bounded
  failure. Consuming the request first prevents a bad network from causing a
  reboot loop.

## Trust and recovery

- HTTPS certificate verification uses ESP-IDF's common CA bundle.
- Local HTTP is accepted only when its one-shot URL arrives through the physical
  USB maintenance command; it is intended for the same-LAN development loop.
- The downloaded ESP app descriptor must name the `espdrop` project.
- ESP-IDF validates the image and target before selecting the inactive slot.
- Bootloader rollback remains armed until the new app initializes espDrop,
  TapDrop, the AWDL probe, and USB maintenance control.
- The OTA trigger requires the physical USB data connection.
- An interrupted download leaves the currently selected app untouched.
- A full Web Serial installation remains the recovery path.

This path is deliberately separate from file reception. AirDropped content can
never arm or install firmware.

## Hardware proof

**CONFIRMED on 2026-08-23:** after one complete USB migration, the target was
provisioned over Improv Serial and booted normally without joining the saved
infrastructure network. A physical USB OTA command then entered one-shot
maintenance mode, joined the saved network, downloaded the certificate-verified
GitHub Pages image, moved from `ota_0` at `0x20000` to `ota_1` at `0x320000`,
and booted version `0.1.0-e5a941a`. A subsequent restart remained on `ota_1`,
confirming the healthy-start path canceled pending rollback.

The same target, USB serial/MAC `1C:DB:D4:42:3F:A0`, also accepted a one-shot
local URL, joined its saved LAN, fetched the 895,264-byte app image directly
from the development Mac at `10.100.14.50`, installed it into the inactive OTA
slot, and returned to normal AWDL mode. The local command verifies the selected
USB device against an explicitly supplied serial/MAC before serving or arming
the update.
