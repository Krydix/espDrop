# AirDrop protocol baseline

Status date: 2026-08-24.

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

**REFERENCE (iOS 26 capture):** the airdrop-mt7921 project reports
`application/x-dvzip` photo uploads and a requirement to declare a
`TransferID` in `/Ask` and reuse it in `/Upload`. Its reported dvzip
body is a sequence of four-byte big-endian length/flag headers and stored or
zlib-compressed blocks containing an ODC cpio archive.

The iOS 26 observations are inputs to a compatibility fixture, not universal
protocol truths. espDrop must retain raw captures and gate behavior by observed
peer behavior rather than a hard-coded OS version.

**CONFIRMED on the espDrop S3 test target:** a minimum Everyone-mode binary
plist `POST /Discover`, modelled on OpenDrop's sender, received `HTTP 200` from
the stock iPhone over the already-proven AWDL/TLS path. The response used
chunked framing. The first run established HTTP acceptance but preceded the
chunk decoder, so its response plist fields remain unclaimed; subsequent runs
missed the receiver's TCP availability window. Compact evidence is in
[`lab/2026-08-24-airdrop-discover.json`](lab/2026-08-24-airdrop-discover.json).

**CONFIRMED on the espDrop S3 test target:** DNS-SD discovery followed by a
fresh sender TLS connection whose first request was a bounded binary-plist
`POST /Ask`. A stock iPhone displayed the native AirDrop prompt for
`hello.jpg`; after the user explicitly accepted, it returned `HTTP 200` with a
complete chunked binary-plist response. The response contained the receiver
computer-name, IDS-session, pseudonym, and push-token keys. The negative
control—appending `/Ask` after `/Discover` on one connection—timed out without
a prompt. This matches OpenDrop's separation between its earlier find flow and
the new client connection used by send. No `/Upload` was armed. Compact
evidence is in
[`lab/2026-08-24-airdrop-ask.json`](lab/2026-08-24-airdrop-ask.json).

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

## AWDL-embedded receiver advertisements

**CONFIRMED on 2026-08-23:** current Apple MIFs can carry compact DNS-style
service-response TLVs in addition to ordinary mDNS over IPv6. Record owner
names use an AWDL compression dictionary; `0xc007` represents
`_airdrop._tcp.local`. The retained iPhone capture contained three such AirDrop
records, while a different temporary peer contained only `_asquic._udp.local`.
Treating every newly appearing or strong AWDL peer as an AirDrop receiver is
therefore incorrect.

espDrop now parses the bounded record envelope and owner name, recognizes
compressed and regular AirDrop labels, stores the result on the ephemeral peer,
and exposes a unique-fresh-receiver selection rule. Zero candidates returns
not-found; multiple candidates return ambiguous and require further
correlation or confirmation. A passive S3 run decoded the known Mac's nine
records as three PTR, three SRV, and three TXT records with zero malformed
records and correctly marked it as an AirDrop TCP receiver.

The wire layout and compression constants are cross-checked against the
[Wireshark AWDL dissector](https://gitlab.com/wireshark/wireshark/-/blob/4520e9eb867c2e6969308e3aa4d7304b2bbda157/epan/dissectors/packet-awdl.c).
Compact local evidence is in
[`lab/2026-08-23-awdl-service-identification.json`](lab/2026-08-23-awdl-service-identification.json).

For transport use, an AirDrop receiver is stricter than a service hint. The
core now distinguishes an AirDrop-advertising peer from a complete endpoint:
the latter requires a converged PTR/SRV/TXT/AAAA tuple and stores the service
instance, scoped IPv6 address, and advertised TCP port on one ephemeral peer.
Only a unique, fresh, complete endpoint can be selected automatically.

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
