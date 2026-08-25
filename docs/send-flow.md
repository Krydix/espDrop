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

## USB target control

The relay profile exposes a line-oriented serial control plane so the host
application—not an embedded proximity heuristic—owns receiver selection:

```text
ESPDROP PING
ESPDROP PEERS
ESPDROP TARGET 52:f4:36:b8:fd:f5
ESPDROP TARGET AUTO
ESPDROP TARGET NONE
ESPDROP TARGET STATUS
ESPDROP RESTART
```

Peer enumeration is framed by `ESPDROP-PEERS-BEGIN` and
`ESPDROP-PEERS-END`. Each `ESPDROP-PEER` record contains the temporary AWDL
MAC, RSSI, signal mask, endpoint completeness, port, observation age, and
AirDrop service instance. These are session identifiers, not persistent user
identifiers.

The ESP will not open an outgoing AirDrop TCP connection until both a target
and a real outgoing file are registered. The Rust CLI mirrors the control
plane with `peers`, `target`, `restart`, and `send --target ... --restart`, so
the same API can back a later Windows/macOS/Linux receiver picker.

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

**CONFIRMED BY AN ANONYMOUS NATIVE SENDER CONTROL:** a MacBook Air and newly
configured iPhone signed into different Apple accounts completed a Finder photo
transfer through Everyone mode. The native sender skipped `/Hello`, included
preview data in `/Ask`, waited for the accepted Ask response, and then started
`/Upload`. Although Network.framework named the request objects `C81` and
`C82`, both used the same flow UUID, source port, HTTP connection, and TLS
session; Upload joined that connection as HTTP/1 stream 2. The anonymous iPhone
prompt did not render the included preview, so visible preview UI is not a
protocol-validity test. The sender selected CPIO with adaptive compression and
disabled compression for the already-compressed photo. Evidence is in
[`lab/2026-08-24-macos-iphone-anonymous-send.json`](lab/2026-08-24-macos-iphone-anonymous-send.json).

**HARDWARE-PROVEN BEHIND AN ATTENDED LAB FLAG:** the core validates the observed
upper-case UUID, URL-safe pseudonym, and upper-case push-token shapes; builds
the exact minimal `/Upload` header order without `Host` or `Accept-Encoding`;
and uploads only after `/Ask` returns 200. The first successful live run used
one contiguous 10 KiB ODC archive and one zlib-compressed dvzip block.

**IMPLEMENTED AND HARDWARE-PROVEN FOR THE SMALL COMPRESSED PROFILE:** the sender accepts a declared-size source
with short-read and optional rewind callbacks. For a seekable/spooled source,
it streams ODC through zlib once to count the exact payload, rewinds, and then
streams the same source into `/Upload`. The successful hardware run used a
fixed 2 KiB source workspace, produced the same 469-byte zlib stream as the
old contiguous implementation, received HTTP 200, and delivered `hello.jpg`.
Neither the complete file nor archive is retained. A fixed 16 KiB network
staging buffer coalesces producer writes, while mbedTLS submissions stay below
the lwIP send window. Evidence is in
[`lab/2026-08-24-airdrop-streaming-upload.json`](lab/2026-08-24-airdrop-streaming-upload.json).

The relay now enforces dvzip's 128 KiB block ceiling. A compressed archive is
used only when it fits one block and saves bytes; larger or incompressible
payloads use bounded stored blocks after the sizing pass. This prevents the old
large-JPEG path from putting an approximately 2 MB zlib stream into one dvzip
block and follows the native sender's observed decision to disable compression
for JPEG data. This adaptive selection is now hardware-proven with a
53,359-byte JPEG sent as a 61,440-byte ODC archive in one stored dvzip block.

The stored-dvzip fallback remains host-tested for truly non-seekable sources,
including short reads, a 180,000-byte two-block transfer, truncation, source
failure, sink failure, CRC accounting, and independent Python archive
reconstruction. Three earlier live stored controls stalled without an HTTP
response after 3,024, 5,109, and 4,438 request bytes respectively; each hit the
sender's TLS timeout before the native topology correction. The now-confirmed
same-socket and adaptive stored-block profile needs a new attended control.
The sender never blindly retries a transfer.

**IMPLEMENTED AND HARDWARE-PROVEN:** the
portable Rust CLI can now prepare the complete logical ODC cpio/stored-dvzip
stream without materializing it in memory. Before arming the transfer it
declares the original file length and CRC, padded archive length, dvzip block
count, and exact final payload length and CRC. `/Ask` advertises the original
length. Only after acceptance does `/Upload` request the prepared source, which
emits `ESPDROP-STREAM-GO`; the CLI then sends CRC-protected 4 KiB frames under
one-frame acknowledgements into a 16 KiB FreeRTOS stream buffer. This makes the
host the seekable packaging boundary and the ESP a bounded USB-to-AWDL relay.
The radio queue is backed by PSRAM, and both lwIP's general MTU and its separate
IPv6 MTU are set to 1460 so injected raw frames remain within the ESP32-S3
driver's 1500-byte limit. An attended stock-iPhone run streamed all 61,444
payload bytes with zero size or queue drops, returned Upload HTTP 200, and
delivered the real JPEG. The ESP records a terminal TLS/Ask/Upload result; the
CLI polls that record and exits successfully only after the final Upload
response. Evidence is in
[`lab/2026-08-25-airdrop-host-relay.json`](lab/2026-08-25-airdrop-host-relay.json).

**CONFIRMED:** the target iPhone accepts both zlib-compressed and stored dvzip
containing ODC cpio. The compressed profile is proven by the contiguous and
two-pass streaming senders; the stored profile is proven by the host relay.
Bare cpio, other file types, and other receiver versions remain unknown.

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
