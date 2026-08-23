# Unknowns and experiment ledger

Status date: 2026-08-23.

This file is the source of truth for claims that espDrop has not yet
demonstrated. A milestone moves to confirmed only with a capture/log artifact
and a reproducible test procedure.

## Pinned research inputs

| Project | Commit inspected | Status |
| --- | --- | --- |
| archef2000/esp-owl | `653deac69fceecc441129cbfa89d141d236d019f` | ESP32-S3 reference; no repository-level license found |
| seemoo-lab/owl | `da255a70f221784c836d943dd3f243bc798f223b` | GPL-3.0-or-later in inspected source notices; AWDL reference |
| seemoo-lab/opendrop | `11fe7ba7861093b302bc0637e8cb10adf2d29337` | GPL-3.0-or-later in inspected source notices; AirDrop reference |
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
- [x] Reproduce peer admission deterministically from the observed AWDL
  election topology. While the intended iPhone peer advertised distance 1,
  a distance-zero-only lab stayed passive for two 60-second runs. Retargeting
  the same bounded profile to the distance-zero master immediately produced
  one directed Neighbor Advertisement, two matching Echo Replies, 20/20
  radio-successful lwIP frames, and 25 socket-level mDNS packets during the
  retained capture.
- [x] Maintain dynamic election state from all observed MIF peers. The
  OWL-derived bounded model selects an immediate synchronization parent,
  propagates the top-master state, rejects loops/over-height paths, refreshes
  known peers from every decoded action, and drives transmitted MIF state.
  Hardware confirmed live parent/master transitions and fully scheduled radio
  transmission. The fixed-channel lab extends peer expiry from two to five
  seconds because the S3 cannot follow peers into their advertised 5 GHz
  windows.
- [ ] Admit a distance-one iPhone peer. Two bounded controls each achieved
  14/14 radio-successful NS and Echo probes with zero directed replies. The
  first followed the distance-zero master's sequence while addressing the
  iPhone. The second followed the iPhone's own sequence and corrected the
  transmitted election-v2 `sync_master` from the observed TLV. Neither model
  was sufficient. Dynamic election/synchronization-tree state is now
  implemented and hardware-tested, but the intended distance-one address
  rotated away before the valid post-election run. A fresh session candidate
  advertised itself at distance zero and still returned no admission evidence.
  Distance-one admission therefore remains open, and the missing behavior is
  likely elsewhere in peer admission or the still-minimal advertised MIF.
- [ ] Record loss, schedule drift, heap high-water mark, and watchdog status.

## M2/M3 — AirDrop

- [ ] Capture target iPhone OS/build and discoverability mode.
- [x] Implement and host-test a compression-aware bounded PTR/SRV/TXT/AAAA
  decoder. A real ESP socket also decoded Apple PTR traffic into the live
  instance `9df4fc4f18c2._airdrop._tcp.local` without crashing after moving
  parser scratch storage out of the mDNS task stack.
- [x] Identify AirDrop receiver capability inside AWDL MIF service-response
  TLVs. The bounded parser recognizes the AWDL DNS compression code for
  `_airdrop._tcp.local`, distinguishes `_asquic`, records PTR/SRV/TXT counts,
  and rejects malformed/truncated records. A passive S3 run classified the
  Mac's nine records with zero errors. The peer table only selects a receiver
  when exactly one fresh protocol-confirmed candidate exists.
- [ ] Repeatably reconstruct a complete PTR/SRV/TXT/AAAA receiver in one
  retained hardware artifact. One live run reconstructed the Mac endpoint
  (`fe80::e833:2cff:fe82:f57f`, port 8770), but the capture script then lost
  the artifact to a fixed variable-shadowing bug; the clean repeat contained
  PTR records only.
- [ ] Establish TCP to the discovered AirDrop endpoint. A scoped TCP attempt
  to the confirmed Mac address/port was emitted during the bounded lab but
  ended with `EHOSTUNREACH`; no AWDL neighbor admission occurred in that run.
  A later distance-zero master control passed admission and emitted a TCP SYN
  to its derived link-local address using the guessed port 8770, but timed out
  with `ETIMEDOUT` (116). Three PTR instances were observed afterward, so the
  next test must resolve each instance's SRV/TXT/AAAA records and use the
  advertised endpoint rather than the election master plus a fixed port.
  Explicit bounded SRV/TXT/AAAA follow-up queries and exact target-address
  filtering are implemented and host-tested, but distance-one admission did
  not open the netif gate, so they have not yet transmitted on hardware.
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

The distance-zero follow-up isolated that topology boundary. With the iPhone
on the Photos share/AirDrop screen, peer `a6:ed:54:02:5b:4e` remained at
election distance 1 for the full capture and the ESP transmitted nothing. Its
MIF named `52:f4:36:b8:fd:f5` as master in 50 of 55 observations. The same
distance-zero-only firmware retargeted to that master qualified immediately:
the selected peer returned one Neighbor Advertisement and two matching Echo
Replies, the admission gate opened, and all 20 submitted lwIP frames completed
successfully at the radio. The socket received 25 mDNS packets (11 responses)
and found three AirDrop PTR instances. A diagnostic TCP attempt to the master
at a guessed port 8770 timed out, which is expected evidence against treating
the AWDL election master as the AirDrop receiver endpoint. Peer admission is
now repeatable when aligned to election topology; complete DNS-SD endpoint
resolution is the next boundary.

Two subsequent distance-one controls rejected a tempting but incorrect
interpretation of that result. First, espDrop followed the distance-zero
master's schedule while directing admission probes to the distance-one iPhone.
Then it followed the iPhone's own channel sequence and corrected its MIF to
copy `sync_master` from the received election-v2 TLV rather than assuming the
MIF source was the master. Both variants radio-completed all 14 Neighbor
Solicitations and all 14 Echo Requests, but the iPhone returned no Neighbor
Advertisement or Echo Reply. Thus distance zero is an AWDL election role, not
a proximity measurement, and simply accepting distance one is insufficient.
At that point, maintaining the election and synchronization tree like a real
peer became the next research slice; the current minimal 9-TLV bounded
advertisement was not enough to join a non-master iPhone reliably.

The dynamic-election slice implemented that missing tree state from OWL's
published election model and exercised it against rotating live peers. The ESP
correctly distinguished a distance-one synchronization parent from its
distance-zero top master in the first control, then selected a newly appearing
distance-zero session candidate directly in the retained run. All 42 retained
radio transmissions succeeded, but no directed response opened admission.
The intended distance-one AirDrop address had rotated before its targeted run,
so that run's 28 failed unicast radio completions are an absence control, not
evidence against the election model. This leaves the richer MIF/service state
and multi-channel limitations as the next admission research boundary.

**UNKNOWN:** whether the connected iPhone was in an AirDrop discoverable state
during the initial three-second mDNS browse; no receiver service was observed.
