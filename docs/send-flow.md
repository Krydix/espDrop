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

**CONFIRMED IN AN EXPLICIT LAB PROFILE:** the first TLS client uses mbedTLS
on the same scoped IPv6 socket that completed the AirDrop TCP proof. Following
OpenDrop's sender behavior, it presents a self-signed client certificate and
does not treat the receiver's self-signed certificate as a public-PKI identity.
It records protocol version, ciphersuite, verification flags, and bounded peer
certificate metadata. A stock iPhone completed TLS 1.2 with
`TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384` and returned a 1,390-byte certificate.
The normal firmware contains neither the lab private key nor the active probe.
The discovery lab path then sent OpenDrop's minimum empty binary-plist
`/Discover`; the iPhone returned `HTTP 200` with a chunked response. The
attended sender path separately opened a fresh TLS connection, making `/Ask`
its first HTTP request as OpenDrop does. Its bounded one-file binary plist
carried `hello.jpg`, `public.jpeg`, a UUID TransferID, and a files transfer
type. The stock iPhone displayed its native AirDrop prompt and, after explicit
acceptance, returned `HTTP 200` with a 359-byte chunked binary plist containing
the receiver-name, IDS-session, pseudonym, and push-token keys. The subsequent
attended run reused that connection and exact TransferID for one Apple-shaped,
chunked dvzip `/Upload`. The iPhone returned `HTTP 200`, and the user confirmed
that `hello.jpg` arrived. Evidence is in
[`lab/2026-08-24-airdrop-upload.json`](lab/2026-08-24-airdrop-upload.json).

**CONFIRMED FOR THE TEST IPHONE:** the successful upload matched the TransferID
between `/Ask` and `/Upload`, used chunked dvzip, and reused the accepted TLS
connection. This reproduces the iOS 26 reference profile; it does not yet prove
which of those properties other receiver versions require.

**IMPLEMENTED AND HARDWARE-PROVEN BEHIND AN ATTENDED LAB FLAG:** the core validates the
observed upper-case UUID, URL-safe pseudonym, and upper-case push-token shapes;
builds the exact minimal `/Upload` header order without `Host` or
`Accept-Encoding`; builds a bounded ODC cpio archive and HTTP chunk/dvzip
framing; compresses it with the ESP32-S3 ROM miniz implementation; and uploads
only after `/Ask` returns 200. Unit tests independently parse the archive and
round-trip its zlib/dvzip payload. The first live fixture is deliberately tiny
and contiguous; microSD streaming and the public sender state machine remain.

**CONFIRMED:** the target iPhone accepts dvzip for a JPEG received from this
non-Apple sender. Whether it also accepts bare cpio, and which containers other
file types or receiver versions require, remains unknown.

**UNKNOWN:** which alternate TLS/certificate profiles current Everyone mode
accepts, and the Contacts Only identity requirements. One minimum working
Everyone-mode profile is now confirmed; it is not yet a compatibility matrix.

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
