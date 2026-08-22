# BLE discovery

Status date: 2026-08-22. See the evidence labels in
[airdrop-protocol.md](airdrop-protocol.md).

## Known role

**CONFIRMED:** When a user opens AirDrop sharing, the sender emits a BLE signal
containing an AirDrop short identity hash. Nearby awake Apple devices with
AirDrop enabled detect it and may respond over peer-to-peer Wi-Fi.

**CONFIRMED:** In Contacts Only mode a receiver compares the short hash with
hashes derived from its Contacts and does not respond if no match exists.
Everyone mode removes that match requirement.

**REFERENCE:** OpenDrop documents missing BLE triggering as a reason an Apple
receiver may not appear even when configured for Everyone.

## espDrop responsibilities

The BLE layer has two independent jobs:

1. Observe Apple continuity advertisements and record timestamp, RSSI, and
   ephemeral payload fingerprint.
2. Emit the minimum AirDrop wake/discovery advertisement necessary to make a
   stock receiver bring up/respond over AWDL.

The payload fingerprint is retained only inside a bounded peer/session table.
It is not a persistent identifier and is not stored after the correlation
window.

## Capture plan

For every NFC experiment, record on the ESP monotonic clock:

- field rising/falling edge;
- each relevant BLE advertisement timestamp, RSSI, address type, and raw
  manufacturer data;
- each AWDL peer appearance/update;
- each AirDrop mDNS service appearance;
- the eventual selected service or ambiguity result.

A paired Mac capture should record the same BLE/AWDL interval where possible.
Raw logs must redact contact-derived hash material before publication.

## Unknowns

- **UNKNOWN:** exact current iOS 26 advertisement bytes required to wake an
  Everyone-mode receiver.
- **UNKNOWN:** whether opening the AirDrop share sheet, an NFC field event, or
  both cause a reliably measurable BLE appearance transition.
- **UNKNOWN:** a stable and privacy-safe BLE-to-AWDL correlation field on
  current iOS.
- **UNKNOWN:** RSSI calibration across the selected antenna, enclosure, and
  NFC placement.
- **UNKNOWN:** Wi-Fi/BLE coexistence impact on AWDL schedule adherence.

No production matching decision may be based on BLE RSSI alone.

## References

- [Apple Platform Security: AirDrop security](https://support.apple.com/guide/security/airdrop-security-sec2261183f4/web)
- [USENIX Security 2019 protocol flow](https://www.usenix.org/system/files/sec19-stute.pdf)
- [OpenDrop limitations](https://github.com/seemoo-lab/opendrop#current-limitationstodos)
