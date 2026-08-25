# Unknowns and experiment ledger

Status date: 2026-08-24.

This file is the source of truth for claims that espDrop has not yet
demonstrated. A milestone moves to confirmed only with a capture/log artifact
and a reproducible test procedure.

## Pinned research inputs

| Project | Commit inspected | Status |
| --- | --- | --- |
| archef2000/esp-owl | `653deac69fceecc441129cbfa89d141d236d019f` | ESP32-S3 reference; no repository-level license found |
| seemoo-lab/owl | `da255a70f221784c836d943dd3f243bc798f223b` | GPL-3.0-or-later in inspected source notices; AWDL reference |
| seemoo-lab/opendrop | `11fe7ba7861093b302bc0637e8cb10adf2d29337` | GPL-3.0-or-later in inspected source notices; AirDrop reference |
| opendrop-rs/opendrop-rs | `dccc798e244363eb92d35e3c52e9a913188dda91` | Secondary AirDrop implementation inspected; OWL remains authoritative for peer lifecycle |
| jedbillyb/airdrop-mt7921 | `d7c86192e3b79c520fde5965ddc24a1ad8cd1066` | GPL-3.0 current iOS 26 observations/patches |

These hashes are research provenance, not dependencies in release firmware.

## Current AWDL peer model

**CORRECTED AND HARDWARE-CONFIRMED on 2026-08-23:** OWL does not gate a peer
behind Neighbor Solicitation/Echo responses or top-master approval. Its peer
validity predicate is received MIF plus nonzero Version-TLV version and device
class; it immediately derives the RFC 4291 link-local address from the source
MAC and installs that neighbor mapping. espDrop now does the same.

A bounded direct-peer run created two mappings with zero failures, sent 19/19
queued netif frames, injected 6/6 received IPv6 frames, obtained six mDNS
response packets, and retained one complete AirDrop endpoint at
`fe80::a4ed:54ff:fe02:5b4e`, port 8770. The exact-endpoint TCP attempt timed out
with error 116. Therefore peer "admission" is no longer an open gate; TCP over
the already-proven direct AWDL/IPv6 path is. Evidence is in
[`lab/2026-08-23-awdl-owl-direct-peer.json`](lab/2026-08-23-awdl-owl-direct-peer.json).

## Hardware identity

- **CONFIRMED:** the target is an ESP32-S3 revision 0.2 with 16 MiB SPI flash,
  8 MiB embedded octal PSRAM, and base MAC `1c:db:d4:42:3f:a0`. The ROM USB
  Serial/JTAG interface enumerates as vendor 0x303a, product 0x1001, serial
  `1C:DB:D4:42:3F:A0`, currently `/dev/cu.usbmodem1101` (the device suffix may
  change after re-enumeration).
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
- [x] Validate and map a stock iPhone peer using the OWL predicate; its MIF
  carried nonzero version/device-class state and the derived link-local/MAC
  entry was installed without NDP.
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
- [x] Test whether ESP-IDF 5.4's exported private station BSSID/RX-policy
  transitions enable ACK-capable promiscuous reception. Both the normal
  pre-authentication policy and its authenticated transition remained stable,
  but 64 directed TCP SYN submissions across two bounded Mac-target runs
  produced zero frames addressed to the ESP and no TCP connection. These
  wrappers are not the missing active-monitor switch. The lab hook remains
  version-pinned and default-off. Evidence is in
  [`lab/2026-08-24-esp32s3-active-rx-policy.json`](lab/2026-08-24-esp32s3-active-rx-policy.json).
- [x] mDNS multicast in both directions; an ESP-IDF UDP/5353 socket sent six
  `_airdrop._tcp.local` PTR queries and received four DNS responses from two
  Apple AWDL peers through the custom netif.
- [x] Map and communicate with an AirDrop endpoint independently of its
  election-tree distance. The current lab targets an OWL-valid AirDrop MIF
  directly; master state is used for synchronization rather than permission.
- [x] Maintain dynamic election state from all observed MIF peers. The
  OWL-derived bounded model selects an immediate synchronization parent,
  propagates the top-master state, rejects loops/over-height paths, refreshes
  known peers from every decoded action, and drives transmitted MIF state.
  Hardware confirmed live parent/master transitions and fully scheduled radio
  transmission. The fixed-channel lab extends peer expiry from two to five
  seconds because the S3 cannot follow peers into their advertised 5 GHz
  windows.
- [ ] Repeat the direct-peer hardware run while the selected AirDrop
  advertiser reports nonzero election distance. No special admission
  handshake is expected by the OWL model. Earlier negative controls each
  achieved
  14/14 radio-successful NS and Echo probes with zero directed replies. The
  first followed the distance-zero master's sequence while addressing the
  iPhone. The second followed the iPhone's own sequence and corrected the
  transmitted election-v2 `sync_master` from the observed TLV. Neither model
  was sufficient. Dynamic election/synchronization-tree state is now
  implemented and hardware-tested, but the intended distance-one address
  rotated away before the valid post-election run. A fresh session candidate
  advertised itself at distance zero and still returned no admission evidence.
  A later same-boot selector eliminated the rebuild/ephemeral-MAC race and
  locked a protocol-confirmed distance-one `_airdrop._tcp` receiver. Its
  14/14 action, 14/14 NS, and 14/14 Echo radio completions again produced zero
  directed replies. A phase-aware follow-up then intersected the elected
  synchronization schedule with that target's independently derived schedule;
  all 11 retained windows were marked copresent and all 42 probe frames again
  radio-completed without a directed reply. Target copresence is therefore not
  sufficient under the old experimental gate. Those results are historical,
  not evidence of an AWDL membership requirement. Evidence is in
  [`lab/2026-08-23-awdl-auto-target.json`](lab/2026-08-23-awdl-auto-target.json)
  and
  [`lab/2026-08-23-awdl-peer-copresence.json`](lab/2026-08-23-awdl-peer-copresence.json).
- [ ] Record loss, schedule drift, heap high-water mark, and watchdog status.

## M2/M3 — AirDrop

- [ ] Capture target iPhone OS/build and discoverability mode.
- [x] Implement and host-test a compression-aware bounded PTR/SRV/TXT/AAAA
  decoder. A real ESP socket also decoded Apple PTR traffic into the live
  instance `9df4fc4f18c2._airdrop._tcp.local` without crashing after moving
  parser scratch storage out of the mDNS task stack.
- [x] Model a complete DNS-SD endpoint on the ephemeral AWDL peer. A complete
  tuple is now matched by advertised IPv6 address, stores the service instance
  and advertised port, and is eligible for automatic selection only when
  exactly one fresh complete endpoint exists. Peer-table writers from the
  probe and mDNS tasks are serialized.
- [x] Identify AirDrop receiver capability inside AWDL MIF service-response
  TLVs. The bounded parser recognizes the AWDL DNS compression code for
  `_airdrop._tcp.local`, distinguishes `_asquic`, records PTR/SRV/TXT counts,
  and rejects malformed/truncated records. A passive S3 run classified the
  Mac's nine records with zero errors. The peer table only selects a receiver
  when exactly one fresh protocol-confirmed candidate exists.
- [x] Select a live ephemeral AirDrop lab target without rebuilding firmware.
  With no explicit target, the bounded image observes fresh valid
  `_airdrop._tcp` MIF candidates for one second, requires at least -70 dBm and
  an 8 dB lead over the runner-up, then locks the selected ephemeral peer in
  RAM for one session. Arrival order no longer decides the recipient, and an
  unclear field remains explicitly ambiguous. The original same-boot proof is in
  [`lab/2026-08-23-awdl-owl-direct-peer.json`](lab/2026-08-23-awdl-owl-direct-peer.json).
- [x] Repeatably reconstruct a complete PTR/SRV/TXT/AAAA receiver in one
  retained hardware artifact. The direct-peer run reconstructed
  `9df4fc4f18c2._airdrop._tcp.local`, host
  `308235b7-037e-431f-bce3-1fb6ef624237.local`, address
  `fe80::a4ed:54ff:fe02:5b4e`, and advertised port 8770. The successful
  artifact is
  [`lab/2026-08-23-awdl-owl-direct-peer.json`](lab/2026-08-23-awdl-owl-direct-peer.json).
- [x] Establish TCP to a dynamically discovered AirDrop endpoint. In the
  retained 30-second run, espDrop reconstructed receiver
  `ae0ae0a8304e._airdrop._tcp.local` at
  `fe80::50f4:36ff:feb8:fdf5` port 8770, emitted one checksum-valid SYN, saw
  four SYN-ACK observations, and completed the lwIP socket connection with
  error 0. All six TCP transmissions radio-completed. The earlier timeout is
  retained as a receiver-unavailable negative control; a raw macOS `nc` probe
  also timed out immediately before this successful ESP run and is therefore
  not a valid AirDrop readiness gate. Evidence is in
  [`lab/2026-08-23-awdl-tcp-connect.json`](lab/2026-08-23-awdl-tcp-connect.json).
- [x] Confirm a minimum working TLS version, cipher, certificate profile, and
  AWDL scoping.
  The bounded TLS 1.2 client is implemented behind
  `CONFIG_ESPDROP_AIRDROP_TLS_LAB`. It presents an explicitly public lab-only
  RSA self-signed certificate, accepts the receiver's self-signed certificate
  to mirror OpenDrop's Everyone-mode behavior, and records the negotiated
  version/cipher plus bounded certificate metadata. Normal firmware excludes
  the credential and the probe. A pinned stock-iPhone run completed scoped
  TCP and TLS 1.2 with `TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384`, received a
  1,390-byte peer certificate, and returned verification flags zero under the
  explicit no-PKI-verification lab policy. The first six-second control timed
  out after the receiver's handshake flight arrived; allowing 12 seconds for
  AWDL-window-delayed TLS flights completed successfully. Evidence is in
  [`lab/2026-08-24-airdrop-tls-connect.json`](lab/2026-08-24-airdrop-tls-connect.json).
  Contacts Only identity validation and the full set of accepted modern TLS
  profiles remain separate unknowns.
- [ ] Capture `/Discover`, `/Ask`, and `/Upload` for each direction.
  ESP-to-iPhone `/Discover`, `/Ask`, and `/Upload` are now captured and
  accepted; the opposite direction remains open. The successful attended send
  is in
  [`lab/2026-08-24-airdrop-upload.json`](lab/2026-08-24-airdrop-upload.json).
- [x] Confirm TransferID continuity and connection topology for sender
  profiles. One small espDrop transfer reused the accepted TLS connection and
  landed successfully. The clean anonymous iPhone-to-Mac control instead
  waited for acceptance and then opened a distinct Upload connection 162 ms
  later. In the reverse anonymous Mac-to-iPhone control, the Upload request
  object joined the Ask object's existing HTTP/1, TCP, and TLS flow as stream
  2. espDrop's sender follows this direction-specific same-socket topology
  while preserving the exact accepted TransferID. Evidence is in
  [`lab/2026-08-24-iphone-macos-anonymous-receive.json`](lab/2026-08-24-iphone-macos-anonymous-receive.json)
  and
  [`lab/2026-08-24-macos-iphone-anonymous-send.json`](lab/2026-08-24-macos-iphone-anonymous-send.json).
- [ ] Confirm cpio versus dvzip by file type.
  One zlib-compressed dvzip block containing an ODC cpio JPEG archive is now
  hardware-proven from both contiguous and two-pass streaming senders. Three
  stored-dvzip controls hit sender TLS timeouts before the corrected IPv6 MTU
  and PSRAM queue path. A later attended relay sent a 53,359-byte JPEG in one
  61,440-byte stored block and received Upload HTTP 200, proving stored dvzip
  against the stock iPhone. Relay sizing selects compressed output only when
  it fits one 128 KiB dvzip block and otherwise emits bounded stored blocks.
  Bare cpio and other file types remain untested.
- [ ] Prove bounded streaming receive to microSD.
- [x] Prove one embedded JPEG send to a stock iPhone. The 445-byte lab fixture
  arrived as `hello.jpg` after explicit acceptance, with no retry.
- [ ] Prove one JPEG send from microSD.
  The rewindable source boundary is hardware-proven using an embedded fixture,
  a fixed 2 KiB source workspace, and two zlib passes. A concrete microSD
  adapter still needs the selected board's SD wiring and a live file.

## M4/M5 — TapDrop

- [ ] Select NFC controller and record field-detect polarity/pulse behavior.
- [ ] Choose final field-detect GPIO after board identification.
- [ ] Capture at least 100 single-phone tap sessions.
- [ ] Capture crowded-room/two-phone negative cases.
- [ ] Determine if BLE and AWDL expose a safe ephemeral link key.
- [ ] Calibrate score threshold and ambiguity margin from held-out sessions.
- [ ] Define a UI confirmation path for every ambiguous result.

## macOS lab capability

> Historical note: the admission-gated narrative below records experiments
> that led to the OWL source audit. The current model is the direct MIF peer
> mapping described at the top of this ledger.

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

The staged-admission follow-up narrowed that boundary without resolving it.
The MIF differential found that current Apple peers use a longer Data Path TLV
and additional fields, but those fields occurred on both sides of the earlier
success/failure comparison while espDrop's existing short form had already
been accepted by a top master. Copying them is therefore not evidence-backed.
The new bounded profile can require a distinct AirDrop child and top master,
freeze that pair, admit only the exact master first, and remain passive if the
topology is not internally consistent. One pre-freeze run followed a rotating
master set and received no directed response; a 130-second post-freeze control
never saw a qualifying pair and transmitted nothing. The next run did obtain a
stable pair: all 14 MIF/Neighbor Solicitation/Echo bursts to frozen top master
`a6:ed:54:02:5b:4e` radio-completed, but it returned no directed admission
evidence and the gate never touched AirDrop child `52:f4:36:b8:fd:f5`.
Top-master-first sequencing alone is therefore rejected. **UNKNOWN:** which
AWDL membership/Data Path semantics make an Apple master recognize a new child,
and whether the ESP32-S3's single-band channel coverage can satisfy them.

**UNKNOWN:** whether the connected iPhone was in an AirDrop discoverable state
during the initial three-second mDNS browse; no receiver service was observed.
