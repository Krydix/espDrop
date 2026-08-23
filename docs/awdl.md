# AWDL on ESP32-S3

Status date: 2026-08-23. See the evidence labels in
[airdrop-protocol.md](airdrop-protocol.md).

## What must exist

**CONFIRMED:** AWDL is a proprietary, undocumented IEEE 802.11-based ad-hoc
link. The published reverse engineering describes synchronization, master
election, availability windows, channel sequences, service discovery, and an
IPv6 data path.

**CONFIRMED:** ESP-IDF exposes the two primitives needed for an experiment:
promiscuous reception of management/data/control frames and raw transmission
of action and non-QoS data frames through `esp_wifi_80211_tx()`.

**CONFIRMED:** ESP32-S3 is a 2.4 GHz Wi-Fi device. It cannot follow a peer onto
5 GHz social channels. Channel 6 interoperability is therefore a hard
dependency, not merely an optimization.

**REFERENCE:** archef2000/esp-owl ports major OWL state, schedule, frame,
election, RX/TX, netif, and peer-table code to ESP-IDF and claims ESP32-S3
support. Its current head is pinned in [unknowns.md](unknowns.md).

## Phase 1 gates

The AWDL milestone is deliberately split:

1. **COMPLETE:** capture Apple AWDL action frames on channel 6.
2. **COMPLETE:** parse peer address, synchronization parameters, election data,
   and channel sequence without transmitting.
3. **COMPLETE FOR MAC AND IPHONE PEERS:** emit synchronized PSF/MIF action
   frames and observe an Apple peer admit the ESP after directed data.
4. **COMPLETE FOR MAC AND IPHONE PEERS:** inject unicast AWDL/IPv6 data and
   confirm Apple-side processing.
5. **COMPLETE AT THE RAW FRAME BOUNDARY:** form and decode link-local IPv6 in
   both directions.
6. **COMPLETE:** pass an ICMPv6 Echo Request/Reply round trip with a stock
   iPhone.
7. **COMPLETE FOR A BOUNDED RUN:** attach the proven frame path to an ESP-IDF
   netif.
8. **COMPLETE FOR A BOUNDED RUN:** pass UDP multicast on port 5353 in both
   directions.
9. Run for 30 minutes without schedule drift, watchdog reset, or peer loss.

M1 is reached only at gate 6. AirDrop work starts after gate 7.

## First hardware capture

**CONFIRMED on 2026-08-23:** an ESP32-S3 on channel 6 received 703 Apple
vendor action frames in 30 seconds while an iPhone AirDrop share sheet was
open. All 703 used AWDL BSSID `00:25:00:ff:94:73`, Apple OUI `00:17:f2`,
action type 8, and action-header byte `0x10`, which encodes AWDL protocol
version 1.0 as major/minor nibbles. Both PSF subtype 0 and MIF subtype 3 were
present. Three ephemeral source addresses were observed;
the strongest sample was -18 dBm. No receive records were dropped.

The compact raw evidence is preserved in
[`lab/2026-08-23-awdl-channel6.json`](lab/2026-08-23-awdl-channel6.json).
This proves receive-only gate 1, not bidirectional AWDL or M1.

**CONFIRMED on 2026-08-23:** the S3 captured complete 491-byte and 528-byte
MIFs without their FCS. The bounded parser successfully iterated 13 and 16
TLVs and extracted synchronization, election v1/v2, and 16-slot channel
sequences. Observed synchronization used a 16 TU AW period, 110 TU action
frame period, presence mode 4, and master channel 6. The independently elected
master had distance 0; the other captured peer had distance 1. Both advertised
master and self metrics of 510.

The raw-capture summary is preserved in
[`lab/2026-08-23-awdl-mif-tlvs.json`](lab/2026-08-23-awdl-mif-tlvs.json), and
the four core TLVs from the 491-byte live MIF are a permanent host regression
fixture in [`../tests/fixtures/awdl-mif-core.hex`](../tests/fixtures/awdl-mif-core.hex).
Full service-response payloads are intentionally not committed.

## First bounded transmit experiment

**CONFIRMED on 2026-08-23:** an opt-in lab image derived channel-6 timing,
master election, and channel availability state from live MIFs, then submitted
137 independently constructed PSF/MIF action frames during a hard-limited
15-second window. ESP-IDF accepted all 137, and its transmit-completion callback
reported 137 radio successes and zero failures. The normal firmware and GitHub
Pages image leave this experiment disabled.

**NOT CONFIRMED IN THIS FIRST RUN:** neither Apple peer sent a directed AWDL
action to the ESP, and macOS did not create an IPv6 neighbor entry for the
ESP. Apple action traffic remained active throughout, but that run alone did
not prove either peer parsed or admitted the ESP. The compact evidence is
preserved in
[`lab/2026-08-23-awdl-tx-lab.json`](lab/2026-08-23-awdl-tx-lab.json).

The capture also corrected two stale OWL-era assumptions for the current Apple
frames observed here: the version TLV carries `0xa0`, and ARPA/name data uses
TLV type 16 rather than type 17. These are observations, not yet general
compatibility claims.

## First directed AWDL data result

**CONFIRMED on 2026-08-23 against this Mac:** the S3 constructed a 112-byte
non-QoS AWDL data frame containing an IPv6 Neighbor Solicitation for the live
macOS `awdl0` master. The frame used the ESP's real station MAC, its derived
link-local address `fe80::1edb:d4ff:fe42:3fa0`, hop limit 255, a source
link-layer option, and a host-tested ICMPv6 checksum.

Twenty probes were submitted while deliberately unscheduled. Three reported
successful radio completion and seventeen failed, consistent with hitting
only some peer availability windows. During that transmit window, independent
macOS polling created this previously absent entry:

    fe80::1edb:d4ff:fe42:3fa0%awdl0  1c:db:d4:42:3f:a0  awdl0 permanent R

This is the first Apple-side proof that an independently constructed ESP frame
crossed AWDL and was consumed far enough to populate Apple IPv6 neighbor
state. It completes gate 4 for the Mac peer. It does not yet prove the reverse
data path or iPhone interoperability; the ESP observed no Neighbor
Advertisement. Evidence is in
[`lab/2026-08-23-awdl-data-neighbor.json`](lab/2026-08-23-awdl-data-neighbor.json).

## First ICMPv6 Echo result

**CONFIRMED on 2026-08-23 against this Mac:** a second bounded lab interleaved
20 Neighbor Solicitations and 20 host-tested ICMPv6 Echo Requests across the
15-second window. ESP-IDF accepted all 40 data frames. Its completion callback,
classified by an AWDL sequence marker retained in the returned frame, reported
3/20 Neighbor Solicitations and 4/20 Echo Requests as radio successes. There
were no unclassified data completions. Independent macOS polling observed the
ESP's exact `awdl0` neighbor mapping in 53 samples during the same run.

**NOT CONFIRMED:** the ESP decoded zero Apple AWDL data frames, Neighbor
Advertisements, or Echo Replies despite observing 147 raw data frames. This
proves Echo transmission reached the radio-success boundary but not that macOS
accepted the Echo payload or replied. Gate 6 and M1 remain incomplete. Compact
evidence is preserved in
[`lab/2026-08-23-awdl-echo-reverse.json`](lab/2026-08-23-awdl-echo-reverse.json).

## Scheduled reverse-path result

**CONFIRMED on 2026-08-23 against this Mac:** peer channel-sequence scheduling
placed 14 probe groups into the elected master's advertised channel-6 windows
with 1–2 microseconds measured lateness. All 14 MIFs, 14 Neighbor Solicitations,
and 14 Echo Requests reported radio success. Action timestamps were mapped
from the master's observed `phy_tx` clock, and the data path used the required
Apple SNAP OUI `00:17:f2` rather than generic SNAP `00:00:00`.

The receive path now distinguishes non-QoS, QoS, and A-MSDU AWDL data and logs
only privacy-bounded candidate headers. During this run it saw 27 unrelated raw
data frames, but zero frames sourced by or addressed to the ESP and zero frames
using the AWDL BSSID. Therefore the decoder was not discarding a hidden reverse
reply: macOS emitted none.

**NOT CONFIRMED:** macOS still produced no directed reaction, Neighbor
Advertisement, or Echo Reply. Channel timing, action timestamp domain, legacy
non-QoS framing, and SNAP encapsulation are no longer the leading blockers.
The next boundary is current Apple peer admission: identify the additional MIF
TLVs or state transitions required beyond the legacy OWL-compatible subset.
Compact evidence is preserved in
[`lab/2026-08-23-awdl-scheduled-reverse.json`](lab/2026-08-23-awdl-scheduled-reverse.json).

## Verbatim channel-sequence result

**CONFIRMED on 2026-08-23 against a stock iPhone:** the admission failure was
caused by espDrop advertising a synthetic all-channel-6 sequence while using
the selected peer's real sequence only for local scheduling. Current iOS 26
interoperability research independently reports that Apple peers reject this
kind of pinned sequence. espDrop now preserves the observed encoding,
duplicate/step/fill fields, all 16 channels, and their operating classes in
both advertised sequence locations.

In the bounded hardware run, the selected iPhone advertised
`44,44,44,0,0,0,0,0,6,44,44,0,0,0,0,0`. espDrop copied it verbatim and sent
14 MIF/NS/Echo probe groups in the channel-6 windows. The iPhone then emitted
two QoS AWDL data frames directly to the ESP MAC: one IPv6 Neighbor
Advertisement and one matching ICMPv6 Echo Reply. The ESP decoded both.

This is the first confirmed bidirectional IPv6 exchange between the stock
iPhone and ESP32-S3 and completes the raw-link M1 proof criterion. Production
netif integration, bidirectional mDNS, and endurance remain separate gates.
Compact evidence is preserved in
[`lab/2026-08-23-awdl-verbatim-sequence.json`](lab/2026-08-23-awdl-verbatim-sequence.json).

## First ESP-IDF netif and mDNS result

**CONFIRMED on 2026-08-23:** the raw AWDL data path is attached to a dedicated
Ethernet-shaped ESP-IDF netif with MTU 1460 and the ESP's derived link-local
address `fe80::1edb:d4ff:fe42:3fa0`. lwIP-originated Ethernet frames are
wrapped in Apple AWDL SNAP/data headers only inside synchronized channel-6
windows; decoded peer frames are reconstructed as Ethernet and passed to
`esp_netif_receive()`.

A real IPv6 UDP socket bound to port 5353 sent six 37-byte DNS-SD PTR queries
for `_airdrop._tcp.local`. Four 197-byte multicast responses from two Apple
AWDL peers crossed the raw decoder into the netif and were delivered to that
same socket. Each response had one answer and five additional records. The
bounded run radio-completed 14/14 queued netif frames and injected 4/4 inbound
frames with zero drops or resets.

This completes gates 7 and 8 for a bounded run. It proves transport through
the ordinary socket API, not yet AirDrop receiver discovery semantics,
long-duration synchronization, or file transfer. Compact evidence is in
[`lab/2026-08-23-awdl-netif-mdns.json`](lab/2026-08-23-awdl-netif-mdns.json).

## Timing model

**CONFIRMED by the OWL research:** AWDL divides time into availability windows
and shares channel-sequence state in action frames. Election state chooses a
synchronization master.

**REFERENCE:** current OWL-derived tooling treats a 16-window sequence as
approximately 1.048 seconds. The exact unit/guard behavior used by current
iPhones must be validated from captures before it becomes a firmware constant.

The ESP backend must timestamp received frames as close to the Wi-Fi callback
as possible, enqueue only bounded metadata there, and do parsing/state updates
in a worker task. Logging and allocation do not belong in the promiscuous RX
callback.

## S3 risk register

- **PARTIALLY CONFIRMED:** current Apple devices expose AWDL action traffic on
  channel 6 at high volume. Whether channel-6 availability windows are long
  enough for reliable file transfer remains unknown.
- **CONFIRMED FOR A BOUNDED RUN:** ESP-IDF can radio-complete bounded AWDL
  management/data frames, and a stock iPhone returns directed IPv6 after the
  ESP advertises the observed peer channel sequence verbatim. Reliability and
  sustained throughput remain unknown.
- **UNKNOWN:** sustainable bidirectional throughput under BLE coexistence and
  channel switching.
- **UNKNOWN:** whether an infrastructure STA connection can coexist with AWDL.
  It is out of scope for the first proof and should remain disabled.
- **UNKNOWN:** current esp-owl stability on ESP-IDF 5.4.1; its saved sdkconfig
  identifies 5.2.1 and also contains stale C6 configuration.

## Licensing boundary

OWL and OpenDrop are GPL-3.0. The esp-owl repository has mixed per-file notices
but no repository-level license at the pinned head. The MIT core therefore
defines an AWDL backend interface but does not copy that source into release
firmware. Options are:

1. obtain a clear license for esp-owl changes;
2. keep a separately distributed GPL backend with complete notices; or
3. implement the ESP adapter independently from the papers and measured
   packet fixtures.

## References

- [One Billion Apples' Secret Sauce](https://arxiv.org/abs/1808.03156)
- [OWL](https://github.com/seemoo-lab/owl)
- [esp-owl](https://github.com/archef2000/esp-owl)
- [ESP-IDF ESP32-S3 Wi-Fi driver](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-guides/wifi.html)
