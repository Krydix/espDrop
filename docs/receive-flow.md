# Receive flow: iPhone to ESP32-S3

Target: accept one JPEG, PNG, TXT, or PDF from a stock iPhone and stream it to
microSD only after explicit local consent.

## State machine

| State | Receiver behavior |
| --- | --- |
| OFFLINE | no AirDrop service advertised |
| AVAILABLE | AWDL active; HTTPS listener and mDNS service active |
| DISCOVERED | validate bounded `/Discover` plist; answer identity metadata |
| ASK_PENDING | parse bounded `/Ask`; display sender/file summary |
| ACCEPTED | reserve storage and bind accepted TransferID to this connection/session |
| RECEIVING | stream upload container to a generated temporary name |
| VERIFYING | validate container paths, byte count, type, and storage result |
| COMPLETE | atomically promote allowed file into inbox |
| FAILED | remove temporary data and clear session |

## Input rules

- Maximum plist/header/body-prefix sizes are hard limits.
- Both Content-Length and chunked request framing are parsed incrementally.
- Exactly one active receive session is allowed in public-device mode.
- `/Upload` must correspond to an accepted, unexpired `/Ask`.
- Archive paths are never trusted: reject absolute paths, `..`, links,
  devices, and duplicate normalized paths.
- The displayed filename is metadata only. Storage uses a generated name.
- MIME, extension, and magic bytes must agree with an allowed profile.
- Files are never executed, rendered with an unsafe parser, or treated as
  firmware.

## Current format baseline

**REFERENCE:** OpenDrop historically receives
`application/x-cpio` and extracts an archive.

**REFERENCE (iOS 26 capture):** a recent iPhone sent photo uploads as
`application/x-dvzip`, a block wrapper around ODC cpio. This must be
implemented as a bounded streaming decoder before espDrop claims iOS 26 photo
receive compatibility.

**UNKNOWN:** container choice for PNG, PDF, TXT, multiple photos, and Files-app
documents on the target phone.

## Consent

Development firmware may expose a compile-time permissive mode, but published
firmware defaults to confirmation. A `200` response to `/Ask` is consent
and therefore cannot be sent before the local decision.

## Storage transaction

1. Check free space against advertised size plus safety reserve.
2. Open a generated file in `/sd/tmp`.
3. Stream and hash while enforcing the byte limit.
4. Decode the archive through a path-safe extractor.
5. Verify allowed type and final size.
6. Rename atomically into `/sd/inbox`.
7. Delete raw/container temporary files unless diagnostic retention was
   explicitly enabled.

## Reference

- [OpenDrop server flow](https://github.com/seemoo-lab/opendrop/blob/master/opendrop/server.py)
- [airdrop-mt7921 iOS 26 receive notes](https://github.com/jedbillyb/airdrop-mt7921/tree/main/patches)
