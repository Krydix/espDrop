# espDrop

**AirDrop for physical devices.**

espDrop is an open embedded AirDrop/AWDL stack, initially for ESP32-S3. Its
signature extension is TapDrop: physical NFC intent correlated with a
short-lived AirDrop peer session.

    espDrop
    ├── core       AirDrop/AWDL stack and application API
    └── TapDrop    NFC tap-to-peer extension

The target interaction is:

    tap iPhone → native AirDrop prompt → receive the device's file

No app, account, Internet connection, infrastructure Wi-Fi, QR upload page, or
cloud service.

> **Research status:** the ESP32-S3 has exchanged bidirectional IPv6 with a
> stock iPhone over AWDL and completed AirDrop DNS-SD discovery through an
> ESP-IDF socket. It reconstructed a live receiver's exact IPv6 address and
> advertised TCP port, completed TCP, and negotiated a certificate-bearing
> TLS 1.2 transport using a lab-only self-signed client certificate. It then
> sent a bounded binary-plist `POST /Ask` on a fresh sender connection. The
> stock iPhone displayed its native AirDrop prompt; after explicit acceptance,
> espDrop reused that connection and TransferID for a chunked dvzip
> `POST /Upload`. The iPhone returned HTTP 200 and received `hello.jpg`. See
> [the unknowns ledger](docs/unknowns.md) for evidence-backed status.

## What exists now

- ESP-IDF 5.4.1 firmware for ESP32-S3
- receive-only detection of AWDL MIF/PSF vendor action frames on channel 6
- host-tested PSF/MIF construction plus a separate 15-second transmit lab
  profile (disabled in normal and web-flasher builds)
- OWL-compatible peer validity from MIF plus Version TLV, with RFC 4291
  link-local/MAC neighbor mapping inserted directly into lwIP (no NDP gate)
- a hardware-proven raw IPv6 round trip with a stock iPhone: its directed
  Neighbor Advertisement and matching Echo Reply were decoded on the S3
- a hardware-proven TLS 1.2 session to a stock iPhone's AirDrop endpoint,
  isolated behind an explicit bounded lab profile
- a hardware-proven minimum AirDrop `/Discover` request accepted with HTTP
  200, plus bounded Content-Length/chunked response parsing
- a hardware-proven one-file `/Ask` request that triggered the stock iPhone's
  native prompt and received HTTP 200 after explicit acceptance
- a hardware-proven attended `/Upload`: ODC cpio, zlib-compressed dvzip,
  Apple-order headers, same accepted TransferID and TLS connection, HTTP 200,
  and a JPEG received by the stock iPhone; disabled in normal firmware
- verbatim preservation of a peer's modern 16-slot AWDL channel sequence
- bounded ephemeral BLE/AWDL/AirDrop peer model
- TapDrop scoring based on timing, appearance, cross-layer observation, and
  RSSI
- mandatory ambiguity result when two targets are too close to distinguish
- NFC field-detect GPIO/session boundary (disabled until a GPIO is selected)
- host tests on macOS and Linux
- target-specific firmware artifacts and a GitHub Pages Web Serial installer
- dual-slot OTA with bootloader rollback, certificate-verified GitHub Pages
  downloads, and one-shot USB-triggered maintenance mode
- protocol notes that distinguish confirmed, referenced, and unknown behavior

## Repository layout

    core/
      api/            stable application-facing boundary
      awdl/           raw frames, synchronization, channels
      ble/            discovery observations and wake behavior
      network/        IPv6 and mDNS over AWDL
      airdrop/        TLS, metadata, sender, receiver
    tapdrop/
      include/        tap session and correlation API
      src/            GPIO field detection and scoring
    main/             ESP32-S3 research firmware
    docs/             protocol baseline and experiment ledger
    tests/            host-side deterministic tests
    web-installer/    GitHub Pages source

## Local build and flash

Install ESP-IDF 5.4, then:

    make test
    make build
    make ports
    make flash PORT=/dev/cu.usbmodemXXXX
    make monitor PORT=/dev/cu.usbmodemXXXX

The Makefile follows the same local workflow as ESPresso and defaults
`IDF_PATH` to `~/esp/esp-idf`.

If `make ports` reports “application USB device,” hold **BOOT**, tap
**RESET**, then release **BOOT**. The port should reappear as
“ROM/USB Serial-JTAG (flashable).” Always verify the detected chip before
writing. A classic ESP32 is not a supported espDrop target.

The firmware logs:

- chip revision and actual flash size;
- free heap;
- configured TapDrop/NFC state;
- every first AWDL frame, then periodic counts with subtype, ephemeral source,
  channel, RSSI, and radio timestamp.

To generate the same browser installer used by CI:

    make web-installer

The active direct-peer experiment is intentionally separate:

    make lab-awdl-tx-test PORT=/dev/cu.usbmodemXXXX DURATION=25

It waits for an OWL-valid live MIF, transmits for at most 15 seconds, installs
the peer's derived link-local/MAC mapping, and records action/data radio
completion plus socket-level mDNS/TCP results. This is a research target, not
a general-purpose firmware image.

Serve `build/web-installer` over localhost or HTTPS. GitHub Pages deployment
is defined in [pages.yml](.github/workflows/pages.yml).

## Firmware updates

The OTA design follows ESPresso's provisioning and release pattern while
keeping espDrop's normal radio path local-first. Infrastructure Wi-Fi is used
only during provisioning or a one-shot maintenance boot; normal operation
returns to the AWDL channel-6 probe.

The first OTA-capable installation must be a complete USB flash because it
replaces the old factory-only partition map with two 3 MiB application slots:

    make flash PORT=/dev/cu.usbmodemXXXX

Keep USB connected and provision maintenance Wi-Fi. The password prompt is
hidden and credentials are stored by ESP-IDF in NVS; they are not added to the
repository or command history:

    make provision-wifi PORT=/dev/cu.usbmodemXXXX

After CI publishes a newer `main` build to GitHub Pages, trigger an update:

    make ota-trigger PORT=/dev/cu.usbmodemXXXX

For the development loop, build locally and serve the app image directly from
the Mac over the provisioned maintenance network. The required serial/MAC guard
prevents an adjacent serial device from being selected accidentally:

    make ota-local PORT=/dev/cu.usbmodemXXXX ESP_SERIAL=AA:BB:CC:DD:EE:FF

The physical USB command writes a one-shot maintenance flag and restarts. The
device connects using its saved credentials, synchronizes its clock, downloads
`firmware/esp32s3/espdrop.bin` over certificate-verified HTTPS, rejects images
whose project name is not `espdrop`, writes the inactive slot, and restarts.
The bootloader rolls back unless the new build reaches a healthy normal-mode
startup. See [OTA maintenance design](docs/ota.md).

## First lab procedure

1. Flash and monitor the ESP32-S3.
2. Confirm the log says it is listening on channel 6.
3. On an iPhone, set AirDrop to **Everyone for 10 Minutes**.
4. Open the AirDrop share sheet near the ESP antenna.
5. Capture a reproducible probe artifact:

       make test-hardware-awdl PORT=/dev/cu.usbmodemXXXX DURATION=60

6. Save the iPhone model/OS build, ESP log, time window, and result.

This validates receive visibility only. It is not M1; M1 requires
bidirectional IPv6 over AWDL.

## Research map

- [AirDrop baseline](docs/airdrop-protocol.md)
- [AWDL plan and S3 risks](docs/awdl.md)
- [BLE discovery](docs/ble-discovery.md)
- [Send flow](docs/send-flow.md)
- [Receive flow](docs/receive-flow.md)
- [OTA maintenance design](docs/ota.md)
- [Unknowns and milestone evidence](docs/unknowns.md)
- [Licensing and contribution policy](docs/licensing.md)

The public implementation baseline is Apple’s security documentation and the
Open Wireless Link research. OpenDrop, OWL, esp-owl, and the current iOS 26
interoperability work are pinned as research inputs in the ledger.

## Security and privacy defaults

- no automatic incoming-content execution;
- no automatic firmware flashing from AirDrop;
- generated storage names and bounded streaming;
- one public-device transfer session at a time;
- explicit acceptance by default;
- 15-second TapDrop correlation sessions;
- no persistent phone identifier;
- no probabilistic auto-send when the target is ambiguous.

## License

espDrop is free software licensed under the
[GNU General Public License, version 3 or later](LICENSE)
(`GPL-3.0-or-later`). Commercial use is allowed. Distributors must provide
recipients the corresponding source and the same freedoms required by the
GPL. See [licensing and contribution policy](docs/licensing.md) and
[NOTICE](NOTICE) for provenance and the status of earlier MIT snapshots.
