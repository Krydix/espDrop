# Send flow: ESP32-S3 to iPhone

Target: send one JPEG to a stock iPhone in an explicit Everyone-mode lab
session.

## State machine

| State | Required evidence before advancing |
| --- | --- |
| IDLE | file exists, size/type policy passes |
| DISCOVERING | BLE wake sent; AWDL schedule active |
| PEER_FOUND | IPv6 plus `_airdrop._tcp` SRV/TXT resolved |
| IDENTIFIED | `/Discover` succeeded or peer flags indicate it is not used |
| ASKING | binary plist includes file metadata and one session TransferID |
| ACCEPTED | receiver returned success; acceptance timeout not expired |
| UPLOADING | same TransferID used; bytes stream from storage |
| COMPLETE | HTTP success and exact sent byte count recorded |
| FAILED | sockets closed, session secrets cleared, bounded diagnostic retained |

## Baseline request flow

1. Advertise/scan over BLE to wake receivers.
2. Synchronize on AWDL and browse `_airdrop._tcp.local.`.
3. Resolve the selected service to link-local IPv6 and port.
4. Establish TLS in the AWDL interface scope.
5. If required by receiver flags, send `POST /Discover`.
6. Create one random, short-lived transfer identifier.
7. Send `POST /Ask` with sender fields, file metadata, icon/thumbnail, that
   TransferID, and transfer type.
8. Wait for the iPhone user to accept.
9. Send `POST /Upload` with the exact same TransferID.
10. Treat only an explicit HTTP success as complete.

## First payload profile

- one file: `/sd/outbox/hello.jpg`;
- filename normalized and bounded;
- MIME `image/jpeg`;
- no URL transfer and no multi-file transfer;
- stream from microSD; never buffer the JPEG in PSRAM;
- retain archive framing buffers only;
- 15-second discovery/tap session; transfer timeout is separate and
  size-dependent.

## Current compatibility hypotheses

**IMPLEMENTED AS AN EXPLICIT LAB PROFILE:** the first TLS client uses mbedTLS
on the same scoped IPv6 socket that completed the AirDrop TCP proof. Following
OpenDrop's sender behavior, it presents a self-signed client certificate and
does not treat the receiver's self-signed certificate as a public-PKI identity.
It records protocol version, ciphersuite, verification flags, and bounded peer
certificate metadata. The normal firmware contains neither the lab private key
nor the active probe. Hardware negotiation is not yet confirmed.

**REFERENCE (iOS 26 capture):** matching the TransferID between `/Ask` and
`/Upload` was necessary for upload acceptance. The observed Apple-like upload
used chunked dvzip on a reused connection.

**UNKNOWN:** whether the target iPhone requires dvzip for JPEG received from a
non-Apple sender, accepts cpio, or negotiates this via metadata/flags.

**UNKNOWN:** the exact minimum TLS certificate and identity behavior for an
unauthenticated peer under current Everyone mode.

These are captured as test dimensions. They are not silently guessed at
runtime.

## Failure policy

- No target or a low score: keep discovering until timeout.
- Multiple plausible targets: ask for UI/physical confirmation.
- TLS or plist rejection: preserve a redacted diagnostic; do not retry with
  weaker validation automatically.
- User decline: close immediately and destroy the transfer session.
- Mid-upload failure: no blind replay, because it could create duplicates.

## Reference

- [OpenDrop client flow](https://github.com/seemoo-lab/opendrop/blob/master/opendrop/client.py)
- [airdrop-mt7921 iOS 26 send notes](https://github.com/jedbillyb/airdrop-mt7921/tree/main/patches)
