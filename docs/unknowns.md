# Unknowns and experiment ledger

Status date: 2026-08-23.

This file is the source of truth for claims that espDrop has not yet
demonstrated. A milestone moves to confirmed only with a capture/log artifact
and a reproducible test procedure.

## Pinned research inputs

| Project | Commit inspected | Status |
| --- | --- | --- |
| archef2000/esp-owl | `653deac69fceecc441129cbfa89d141d236d019f` | ESP32-S3 reference; no repository-level license found |
| seemoo-lab/owl | `da255a70f221784c836d943dd3f243bc798f223b` | GPL-3.0 AWDL reference |
| seemoo-lab/opendrop | `11fe7ba7861093b302bc0637e8cb10adf2d29337` | GPL-3.0 AirDrop reference |
| jedbillyb/airdrop-mt7921 | `d7c86192e3b79c520fde5965ddc24a1ad8cd1066` | GPL-3.0 current iOS 26 observations/patches |

These hashes are research provenance, not dependencies in release firmware.

## Hardware identity

- **CONFIRMED:** the target is an ESP32-S3 revision 0.2 with 16 MiB SPI flash,
  8 MiB embedded octal PSRAM, and base MAC `1c:db:d4:42:3f:a0`. The ROM USB
  Serial/JTAG interface enumerates as vendor 0x303a, product 0x1001, serial
  `1C:DB:D4:42:3F:A0`, currently `/dev/cu.usbmodem101`.
- **CONFIRMED:** a manual BOOT/reset entry was required for the first flash.
  Once espDrop booted, its USB Serial/JTAG console remained flashable without
  entering BOOT manually.
- **UNKNOWN:** the board product/module name and GPIO mapping.
- **CONFIRMED:** `/dev/cu.usbserial-0001` is an ESP32-D0WDQ6, not the target
  S3. It must never be selected for espDrop flashing.
- **CONFIRMED:** `/dev/cu.usbmodem007NTCZFT7872` is an LG monitor-control
  interface, not an ESP.

## M1 — AWDL

- [x] Receive a current iPhone AWDL synchronization frame on S3; the
  2026-08-23 capture observed 703 Apple AWDL action frames in 30 seconds on
  channel 6, including PSF and MIF frames using header byte `0x10` (AWDL 1.0).
- [x] Parse captured synchronization/election/channel-sequence TLVs; two full
  hardware MIFs parsed successfully with 13/16 TLVs and no dropped records.
- [x] Radio-complete an independently built synchronized PSF/MIF frame; the
  bounded lab sent 137/137 successfully with no Wi-Fi driver failures.
- [x] Observe an Apple peer act upon ESP traffic; after a valid AWDL/IPv6
  Neighbor Solicitation, macOS installed the ESP's exact link-local/MAC pair
  as a previously absent `awdl0` neighbor.
- [ ] Reproduce peer admission against the stock iPhone rather than the Mac.
- [ ] Maintain channel-6 synchronization for 30 minutes.
- [ ] Attach link-local IPv6 through an ESP-IDF netif.
- [x] Radio-complete ICMPv6 Echo Requests ESP → Mac; 4/20 succeeded in the
  first classified bounded run and 14/14 succeeded after channel-window
  scheduling, but no reply was observed.
- [ ] ICMPv6 ESP → iPhone.
- [ ] ICMPv6 iPhone → ESP.
- [ ] mDNS multicast in both directions.
- [ ] Record loss, schedule drift, heap high-water mark, and watchdog status.

## M2/M3 — AirDrop

- [ ] Capture target iPhone OS/build and discoverability mode.
- [ ] Confirm current mDNS PTR/SRV/TXT/AAAA behavior.
- [ ] Confirm TLS versions, cipher, certificate requirements, and scoping.
- [ ] Capture `/Discover`, `/Ask`, and `/Upload` for each direction.
- [ ] Confirm TransferID and connection-reuse requirements.
- [ ] Confirm cpio versus dvzip by file type.
- [ ] Prove bounded streaming receive to microSD.
- [ ] Prove one JPEG send from microSD.

## M4/M5 — TapDrop

- [ ] Select NFC controller and record field-detect polarity/pulse behavior.
- [ ] Choose final field-detect GPIO after board identification.
- [ ] Capture at least 100 single-phone tap sessions.
- [ ] Capture crowded-room/two-phone negative cases.
- [ ] Determine if BLE and AWDL expose a safe ephemeral link key.
- [ ] Calibrate score threshold and ambiguity margin from held-out sessions.
- [ ] Define a UI confirmation path for every ambiguous result.

## macOS lab capability

**CONFIRMED:** this Mac has an active `awdl0` interface with link-local IPv6,
and Apple's `sharingd`/AirDrop services are loaded. That makes it useful for
packet capture and for exercising host-side protocol code.

**CONFIRMED:** during the first transmit lab, this Mac's `awdl0` MAC was the
elected master seen by the ESP. Its IPv6 neighbor table contained the Mac and
iPhone AWDL addresses afterward, but not the ESP address.

**CONFIRMED:** during the subsequent directed-data lab, macOS added
`fe80::1edb:d4ff:fe42:3fa0%awdl0` mapped to the ESP MAC. Three of twenty raw
unicast frames reported radio success. The entry was temporary and disappeared
after the ESP lab session reset.

**CONFIRMED:** channel-window scheduling improved the bounded lab to 14/14
Neighbor Solicitations and 14/14 Echo Requests radio-completed with 1–2
microseconds measured lateness. The transmit and receive paths now use Apple's
SNAP OUI `00:17:f2`; the receive decoder covers non-QoS, QoS, and A-MSDU forms.
The ESP observed no data frame addressed to itself or carrying the AWDL BSSID,
so the reverse decoder was not hiding a reply. Reverse IPv6 remains unproven;
current Apple peer admission is the next investigation boundary.

**UNKNOWN:** whether the connected iPhone was in an AirDrop discoverable state
during the initial three-second mDNS browse; no receiver service was observed.
