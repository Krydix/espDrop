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
- [x] Reproduce peer admission against the stock iPhone; after espDrop echoed
  its 16-slot channel sequence verbatim, the iPhone returned a Neighbor
  Advertisement and matching Echo Reply directly to the ESP MAC.
- [ ] Maintain channel-6 synchronization for 30 minutes.
- [x] Attach the proven raw link-local IPv6 path through an ESP-IDF netif; a
  bounded run assigned the correct link-local address, transmitted 14/14
  queued frames, and injected 4/4 decoded peer frames without drops.
- [x] Radio-complete ICMPv6 Echo Requests ESP → Mac; 4/20 succeeded in the
  first classified bounded run and 14/14 succeeded after channel-window
  scheduling, but no reply was observed in those pinned-sequence runs.
- [x] ICMPv6 ESP → iPhone; a bounded run received the matching type-129 Echo
  Reply after advertising the iPhone's observed sequence verbatim.
- [ ] ICMPv6 iPhone → ESP.
- [x] mDNS multicast in both directions; an ESP-IDF UDP/5353 socket sent six
  `_airdrop._tcp.local` PTR queries and received four DNS responses from two
  Apple AWDL peers through the custom netif.
- [ ] Record loss, schedule drift, heap high-water mark, and watchdog status.

## M2/M3 — AirDrop

- [ ] Capture target iPhone OS/build and discoverability mode.
- [x] Implement and host-test a compression-aware bounded PTR/SRV/TXT/AAAA
  decoder. A real ESP socket also decoded Apple PTR traffic into the live
  instance `9df4fc4f18c2._airdrop._tcp.local` without crashing after moving
  parser scratch storage out of the mDNS task stack.
- [ ] Repeatably reconstruct a complete PTR/SRV/TXT/AAAA receiver in one
  retained hardware artifact. One live run reconstructed the Mac endpoint
  (`fe80::e833:2cff:fe82:f57f`, port 8770), but the capture script then lost
  the artifact to a fixed variable-shadowing bug; the clean repeat contained
  PTR records only.
- [ ] Establish TCP to the discovered AirDrop endpoint. A scoped TCP attempt
  to the confirmed Mac address/port was emitted during the bounded lab but
  ended with `EHOSTUNREACH`; no AWDL neighbor admission occurred in that run.
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
so the reverse decoder was not hiding a reply. A following run found the
admission boundary: advertising the peer's channel sequence verbatim produced
one directed Neighbor Advertisement and one matching Echo Reply from the stock
iPhone. The next bounded run attached that path to an ESP-IDF netif: a real
UDP/5353 socket sent six AirDrop PTR queries and received four DNS responses
from two Apple peers. Raw bidirectional IPv6 and socket-level mDNS are now
proven. The next slice added bounded DNS-SD record interpretation, an explicit
transmit-readiness gate, retryable full-size MIF capture for targeted peers,
and a scoped TCP diagnostic. The Mac's MIF was 1,059 bytes (larger than the old
768-byte capture ceiling), and real DNS parsing initially exposed and then
fixed a task-stack overflow. A post-fix 80-second run injected nine AWDL IPv6
packets, decoded the iPhone's AirDrop PTR, and remained stable. Complete
receiver reconstruction and TCP still depend on making AWDL peer admission
repeatable rather than merely radio-completing the scheduled frames.

The admission-gated follow-up no longer treats a successful radio callback as
proof that an Apple peer accepted espDrop. It first sends synchronized raw
Neighbor Solicitation/Echo probes, and it releases queued lwIP traffic only
after the selected peer returns a directed Neighbor Advertisement or matching
Echo Reply. Two 15-second negative controls exercised the gate: one targeted
this Mac (`ea:33:2c:82:f5:7f`) and one targeted the currently visible iPhone
peer (`a6:ed:54:02:5b:4e`). Each scheduled 14 channel-6 windows and the driver
accepted all 14 NS plus all 14 Echo probes; the retained iPhone summary also
confirmed 14/14 radio completions for both frame types. Neither peer returned
admission evidence. Consequently the gate submitted zero netif frames and made
zero TCP or mDNS attempts. This makes the remaining boundary precise:
repeatable AWDL peer admission/topology comes before AirDrop TCP, and AirDrop
discoverability alone does not provide it.

**UNKNOWN:** whether the connected iPhone was in an AirDrop discoverable state
during the initial three-second mDNS browse; no receiver service was observed.
