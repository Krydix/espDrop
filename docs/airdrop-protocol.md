# AirDrop protocol baseline

Status date: 2026-08-22.

Labels used throughout the research notes:

- **CONFIRMED**: documented by Apple, a primary research publication, or
  reproduced in espDrop.
- **REFERENCE**: implemented or captured by an external open-source project,
  but not yet reproduced by espDrop.
- **UNKNOWN**: must be measured; code must not depend on it as fact.

## Confirmed transport model

**CONFIRMED:** Apple documents BLE for proximity signalling, direct
peer-to-peer Wi-Fi for data, and TLS encryption. Infrastructure Wi-Fi and
Internet access are not required for an Everyone-mode local transfer.

**CONFIRMED:** The Open Wireless Link research identified that peer-to-peer
Wi-Fi transport as AWDL. OpenDrop uses an AWDL-capable interface on macOS or
the OWL reimplementation on Linux.

**CONFIRMED:** The reverse-engineered AirDrop flow has three layers:

1. BLE advertises AirDrop intent and, in Contacts Only mode, short contact
   hashes.
2. AWDL synchronization plus Bonjour discovery finds an
   `_airdrop._tcp.local.` receiver.
3. HTTPS requests perform discovery, consent, and payload transfer.

## HTTPS exchange

The stable research baseline is:

| Request | Purpose | Typical content |
| --- | --- | --- |
| `POST /Discover` | authenticate/classify a candidate receiver | sender identity and optional validation record |
| `POST /Ask` | present transfer intent | sender metadata, file metadata, thumbnail, transfer identity |
| `POST /Upload` | carry accepted payload | archive/container stream |

**REFERENCE:** OpenDrop represents request and response metadata as binary
property lists and advertises a receiver TXT `flags` value.

**REFERENCE (iOS 26 capture, not yet reproduced here):** the
airdrop-mt7921 project reports chunked `/Discover` and `/Ask` bodies,
`application/x-dvzip` photo uploads, and a requirement to declare a
`TransferID` in `/Ask` and reuse it in `/Upload`. Its reported dvzip
body is a sequence of four-byte big-endian length/flag headers and stored or
zlib-compressed blocks containing an ODC cpio archive.

The iOS 26 observations are inputs to a compatibility fixture, not universal
protocol truths. espDrop must retain raw captures and gate behavior by observed
peer behavior rather than a hard-coded OS version.

## Identity and discoverability

**CONFIRMED:** Contacts Only uses iCloud-derived identities, short hashes in
BLE discovery, long hashes, certificates, and the receiver's Contacts data.
Apple documents a 2048-bit RSA AirDrop identity stored on signed-in devices.

**CONFIRMED:** Everyone mode responds without a Contacts short-hash match.
Unverified peers are presented separately in Apple's UI.

**REFERENCE:** OpenDrop can use Apple credentials extracted from a Mac for
Contacts Only, but its own documentation warns that its authentication and
connection-state validation are incomplete.

For the first embedded milestones, espDrop targets explicit **Everyone for 10
Minutes** lab sessions and still requires user consent. Contacts Only is not an
initial success criterion.

## Implementation rule

No AirDrop endpoint is considered implemented until its exact request headers,
body framing, plist keys, response, TLS behavior, and payload hash are captured
against the test iPhone. A `200` from `/Ask` is not evidence that
`/Upload` will be accepted.

## Primary references

- [Apple Platform Security: AirDrop security](https://support.apple.com/guide/security/airdrop-security-sec2261183f4/web)
- [USENIX Security 2019 AirDrop/AWDL paper](https://www.usenix.org/system/files/sec19-stute.pdf)
- [OpenDrop](https://github.com/seemoo-lab/opendrop)
- [airdrop-mt7921 iOS 26 compatibility work](https://github.com/jedbillyb/airdrop-mt7921)
