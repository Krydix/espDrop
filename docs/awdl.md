# AWDL on ESP32-S3

Status date: 2026-08-24. See the evidence labels in
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

## OWL peer lifecycle correction

**CONFIRMED FROM OWL SOURCE AND ON HARDWARE on 2026-08-23:** AWDL does not
define the speculative Neighbor Solicitation/Echo or top-master "admission"
handshake used by espDrop's earlier lab profiles. In OWL, a peer becomes valid
after a received MIF plus nonzero Version-TLV version and device class. OWL
then derives the peer's RFC 4291 link-local IPv6 address from its source MAC
and installs that exact IPv6/MAC neighbor mapping. Master election supplies
synchronization state; it does not authorize communication with child peers.

espDrop now follows that model. Every OWL-valid MIF refreshes a bounded lwIP
neighbor-cache entry directly, without sending NDP first. The bounded lab then
targets the AirDrop-advertising MIF peer itself and uses its common channel
windows. The previous staged-admission code and configuration have been
removed. Admission-oriented experiment sections below are retained only as a
historical record of the path to this correction and must not be read as the
current protocol model.

The first direct-peer hardware run created two mappings with zero failures,
radio-completed 14/14 MIF actions and 19/19 queued netif frames, injected six
inbound AWDL IPv6 frames with zero drops, received six mDNS response packets,
and reconstructed one complete receiver:

    9df4fc4f18c2._airdrop._tcp.local
    308235b7-037e-431f-bce3-1fb6ef624237.local
    fe80::a4ed:54ff:fe02:5b4e%awdl0 port 8770

No Neighbor Advertisement or Echo Reply occurred or was required. A TCP
attempt to that exact advertised endpoint timed out with error 116, so TCP
delivery/response is the next boundary. Compact evidence is in
[`lab/2026-08-23-awdl-owl-direct-peer.json`](lab/2026-08-23-awdl-owl-direct-peer.json).

The TCP diagnostic follow-up classified five checksum-valid SYNs from lwIP to
that exact address and port. Each was submitted in a guarded common window;
the raw receive path saw no SYN-ACK, RST, or other TCP segment. Crucially, a
native `nc` attempt from this Mac's active `awdl0` also timed out to the same
endpoint while the neighbor entry was reachable. The retained run is therefore
a receiver-unavailable negative control, not proof of an ESP TCP framing bug.
Evidence is in
[`lab/2026-08-23-awdl-tcp-syn-boundary.json`](lab/2026-08-23-awdl-tcp-syn-boundary.json).

**TCP CONFIRMED ON HARDWARE on 2026-08-23:** a later 30-second run dynamically
selected AirDrop peer `52:f4:36:b8:fd:f5`, reconstructed
`ae0ae0a8304e._airdrop._tcp.local` at
`fe80::50f4:36ff:feb8:fdf5` port 8770, and completed the socket connection.
lwIP emitted one checksum-valid SYN; the receive path classified four SYN-ACK
observations and no reset; and all six TCP segments transmitted by the ESP
reported radio success. No NDP admission exchange was required.

The Mac's raw `nc` probe to that advertised receiver had timed out immediately
before the successful ESP run. DNS-SD resolution with the native
`IncludeAWDL` flag did prove that the service was live, but raw TCP from `nc`
is not a reliable AirDrop readiness gate. This result closes the AWDL TCP
boundary. Compact evidence is in
[`lab/2026-08-23-awdl-tcp-connect.json`](lab/2026-08-23-awdl-tcp-connect.json).

**TLS CONFIRMED ON HARDWARE on 2026-08-24:** the same scoped AirDrop endpoint
completed TLS 1.2 with the ESP's explicit lab-only self-signed client
certificate. The negotiated suite was
`TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384`; the iPhone supplied a 1,390-byte peer
certificate. A six-second control timed out because handshake flights were
delayed across AWDL availability windows, while the otherwise identical
12-second probe completed. AirDrop HTTP requests remain separate work. Compact
evidence is in
[`lab/2026-08-24-airdrop-tls-connect.json`](lab/2026-08-24-airdrop-tls-connect.json).

## ESP32-S3 active-receive policy experiment

**NEGATIVE RESULT on 2026-08-24:** ESP-IDF 5.4 has no public active-monitor or
automatic-ACK API. Its ESP32-S3 Wi-Fi blobs do export semantic station BSSID
and RX-policy wrappers. Local call-graph tracing identified the exact
pre-authentication and authenticated transitions used by the normal station
connection state machine, so espDrop tested those transitions behind a
version-pinned, default-off lab flag without copying blob code or writing raw
registers.

Both bounded runs remained stable and continued to receive AWDL action and
multicast IPv6 traffic. The pre-authentication run submitted 22 directed TCP
SYNs and the authenticated run submitted 42, but both observed zero frames
addressed to the ESP, zero Neighbor Advertisements, zero Echo Replies, and no
TCP connection. Therefore these exported policies do not turn promiscuous mode
into an ACK-capable active-monitor path for the tested Mac peer. This does not
invalidate the already proven iPhone bidirectional/TCP runs and does not rule
out unexported firmware mechanisms. Evidence and exact bounds are in
[`lab/2026-08-24-esp32s3-active-rx-policy.json`](lab/2026-08-24-esp32s3-active-rx-policy.json).

## Phase 1 gates

The AWDL milestone is deliberately split:

1. **COMPLETE:** capture Apple AWDL action frames on channel 6.
2. **COMPLETE:** parse peer address, synchronization parameters, election data,
   and channel sequence without transmitting.
3. **COMPLETE FOR MAC AND IPHONE PEERS:** emit synchronized PSF/MIF action
   frames and validate peers from their MIF/Version state.
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

## Dynamic election result

**CONFIRMED IN SOFTWARE AND A BOUNDED HARDWARE RUN on 2026-08-23:** espDrop
now maintains a bounded AWDL election table derived from OWL's published GPL
implementation. It compares top-master counter and metric, distance, and MAC
tie-breaks; rejects cycles and over-height trees; expires peers; and keeps the
immediate synchronization parent distinct from the elected top master. The
transmitted MIF state is regenerated from that election instead of assuming
that one configured peer is always the master.

The retained live run tracked a newly appearing session candidate as the
distance-zero top master and this Mac as its distance-one child. espDrop chose
the candidate directly, followed its channel-6 windows, and radio-completed
14/14 MIFs, 14/14 Neighbor Solicitations, and 14/14 Echo Requests. The peer
returned no directed action or IPv6 response, so this is election and transmit
proof—not a new admission proof.

The run also exposed an ESP32-S3-specific constraint: OWL's two-second peer
timeout assumes a radio capable of following the advertised channel sequence.
This S3 remains on channel 6 and cannot observe 5 GHz windows, which caused
brief false peer expiry. Every decoded PSF/MIF now refreshes peer liveness, and
the fixed-channel lab uses a five-second timeout while the reusable election
core retains the two-second default. The compact evidence is in
[`lab/2026-08-23-awdl-dynamic-election.json`](lab/2026-08-23-awdl-dynamic-election.json).

## Service-profile result

**CONFIRMED PASSIVELY on 2026-08-23:** a MIF peer is not necessarily an AirDrop
receiver. The previous fresh-session target advertised three `_asquic` records
and no AirDrop owner, whereas the known Mac advertised nine embedded records
including compressed `_airdrop._tcp.local`. The new bounded decoder classified
the Mac as AirDrop TCP with three PTR, three SRV, three TXT, and zero malformed
records. The normal firmware remained receive-only throughout.

The MIF audit also rules out indiscriminate Apple capability mirroring. The
iPhone from the earlier successful IPv6 exchange advertised additional data
path, HT/VHT-container, version, and unknown capability state, but it admitted
espDrop while espDrop transmitted the same minimal nine-TLV MIF used later.
Some captured fields describe hardware features the ESP32-S3 does not possess.
They remain research observations rather than bytes to impersonate.

AWDL-embedded AirDrop service evidence is now carried into the peer table, and
the table can return exactly one fresh receiver or reject the selection as
not-found/ambiguous. Evidence is in
[`lab/2026-08-23-awdl-service-identification.json`](lab/2026-08-23-awdl-service-identification.json).

## DNS-SD endpoint publication

The DNS-SD cache now publishes a receiver only after PTR, SRV, TXT, and AAAA
state has converged. The advertised IPv6 address must match an already
observed ephemeral AWDL peer; the service instance, IPv6 address, and
advertised port are then stored together. A separate selector returns a peer
only when exactly one fresh complete endpoint exists. The probe and mDNS tasks
serialize peer-table writes.

Resolution is deliberately paced at one missing question per round so the
eight-frame AWDL transmit queue is not flooded. The first multicast question
sets the unicast-response bit and retransmissions clear it, following
[RFC 6762 section 5.4](https://www.rfc-editor.org/rfc/rfc6762.html#section-5.4).
The PTR to SRV/TXT/AAAA follow-up behavior follows
[RFC 6763 section 12](https://www.rfc-editor.org/rfc/rfc6763.html#section-12).

**CONFIRMED ON HARDWARE:** after replacing that unsupported gate with OWL's
direct peer mapping, the bounded lab sent five queries by its summary, received
six response packets, and retained the complete receiver endpoint shown above.
The older negative controls remain in
[`lab/2026-08-23-airdrop-endpoint-resolution.json`](lab/2026-08-23-airdrop-endpoint-resolution.json);
the successful direct-peer result is in
[`lab/2026-08-23-awdl-owl-direct-peer.json`](lab/2026-08-23-awdl-owl-direct-peer.json).

## Same-boot AirDrop target selection

AirDrop receiver MAC addresses are ephemeral enough that copying one into a
lab configuration, rebuilding, and rebooting can lose the intended peer. The
bounded lab now observes fresh valid MIFs advertising `_airdrop._tcp` for one
second, then binds a peer in RAM only if it is at least -70 dBm and exceeds the
runner-up by 8 dB. Arrival order cannot select the recipient; a weak or crowded
field remains ambiguous. An explicit configured target still takes precedence
and waits for its own live MIF. The capture artifact records the automatic
selection event.

**CONFIRMED IN A BOUNDED HARDWARE RUN on 2026-08-23:** same-boot selection
locked protocol-confirmed AirDrop peer `06:e9:49:af:57:78` at election
distance one. All 14 action frames, 14 Neighbor Solicitations, and 14 Echo
Requests radio-completed. The peer returned no directed reaction, so the
admission gate emitted zero mDNS queries and retained no endpoint.

The lab defaults now additionally require an automatically selected AirDrop
peer to advertise election distance zero. Two subsequent captures, including
one reopening of the native macOS AirDrop receiver view during the same boot,
saw no peer satisfying both conditions. No target was selected, no transmit
task started, and no mDNS query was sent. This is a valid passive safety
control, not a failed transmission. The complete compact evidence is in
[`lab/2026-08-23-awdl-auto-target.json`](lab/2026-08-23-awdl-auto-target.json).

Same-boot selection removes the stale-identity race. The later direct-peer work
removed the historical admission gate; current selection uses protocol
validity plus a conservative proximity margin and then proceeds to endpoint
resolution.

## Phase-aware target copresence

OWL's unicast schedule check does not rely only on the elected synchronization
parent. It phase-corrects both peers' schedules and requires the sender and
destination to advertise the requested channel at the same time. espDrop now
applies that rule before every targeted lab probe. The implementation walks a
bounded set of schedule boundaries, applies a guard at both ends of the
intersection, and emits at most one probe burst for a selected guarded
interval. The algorithm is adapted from OWL `src/schedule.c` at commit
`da255a70f221784c836d943dd3f243bc798f223b` under the project's compatible
GPL-3.0-or-later license.

The selected target's timing state is derived from its own live MIF and retained
separately from the synchronization/election state used to construct outbound
MIFs. AirDrop target observation is also independent of whether that particular
MIF can immediately supply the current election candidate; transmission still
requires both valid states. A separate
`sdkconfig.awdl-distance-one-lab.defaults` profile makes this deliberately
broader experiment explicit, while the existing generic lab defaults retain
their distance-zero target qualifier.

**CONFIRMED IN A BOUNDED HARDWARE RUN on 2026-08-23:** espDrop selected peer
`1e:25:d2:3e:bb:2b`, which had advertised `_airdrop._tcp` in the immediately
preceding same-session capture. Eleven retained window records all report
target copresence, with 2–130 microseconds scheduling lateness; the summary
confirms 14/14 MIF, 14/14 Neighbor Solicitation, and 14/14 Echo radio
completions. The peer returned no directed response, admission remained closed,
and zero mDNS queries were submitted. The run therefore proves execution of
the target-specific gate but rejects target copresence alone as the missing
distance-one admission primitive.

That capture also exposed repeated use of one live overlap interval. The
scheduler was corrected to select only a future guarded interval and covered
with an aligned-phase host regression. A 45-second post-correction hardware
control saw `_asquic` but no `_airdrop._tcp` target and remained fully passive,
so the correction still needs a live-AirDrop hardware repeat. Compact evidence
is preserved in
[`lab/2026-08-23-awdl-peer-copresence.json`](lab/2026-08-23-awdl-peer-copresence.json).

## Staged top-master admission (superseded experiment)

> This experiment tested a protocol gate that OWL does not implement. It is
> retained for provenance; the direct MIF peer model above replaces it.

The retained distance-zero success and distance-one failures differ most
clearly in election topology: the successful peer was the top master, whereas
the desired AirDrop receiver was often a child. A transmitted-MIF differential
did not justify copying newer Apple-only fields. Current Apple MIFs used a
47-byte Data Path TLV and additional type-17/type-33 state, but both successful
and unsuccessful Apple peers used that form, and the earlier top-master
admission accepted espDrop's existing 15-byte form. Those fields are therefore
documented observations, not a demonstrated missing requirement.

The bounded lab now supports a narrower experiment: select the elected top
master first, direct MIF/Neighbor Solicitation/Echo probes to it, and progress
to a separate AirDrop endpoint only after that exact master returns a directed
Neighbor Advertisement or matching Echo Reply. A stale or different master
cannot unlock the endpoint. When the endpoint is itself top master, the helper
correctly takes the direct path. The distance-one profile can additionally
wait for an advertiser whose own election distance is nonzero and for which a
distinct elected master's MIF is present. The qualified target/anchor pair is
then frozen for that one bounded session so election churn cannot silently
change the question being tested.

**PARTIALLY CONFIRMED IN BOUNDED HARDWARE RUNS on 2026-08-23:** a direct-master
control used the target stage for all 14 windows. A later rotating-topology run
used four windows against one anchor, two against another, and eight directly
against the endpoint after the live election changed; none returned admission
evidence. That run exposed the confounder and motivated session freezing. A
130-second post-fix control observed only an AirDrop endpoint that remained top
master, so it selected no child target, scheduled no windows, and transmitted
nothing. A subsequent 180-second run produced the clean condition: AirDrop
child `52:f4:36:b8:fd:f5` at reported distance one and frozen top master
`a6:ed:54:02:5b:4e`. All 14 guarded windows remained in the anchor stage with
1–2 microseconds lateness, and all 14 MIFs, Neighbor Solicitations, and Echo
Requests radio-completed. The master returned no Neighbor Advertisement or
Echo Reply, so the gate correctly sent nothing to the child and emitted zero
mDNS queries. This rejects top-master-first sequencing alone as the missing
admission primitive. Evidence is preserved in
[`lab/2026-08-23-awdl-staged-admission.json`](lab/2026-08-23-awdl-staged-admission.json).

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

espDrop is licensed under `GPL-3.0-or-later`, matching the “version 3 or any
later version” notices in the inspected OWL and OpenDrop source. That makes it
possible to adapt their GPL implementation work while keeping attribution,
copyright notices, modification history, and corresponding source intact.
Compatibility is not permission to erase provenance: every adapted file must
identify its upstream project and retain the upstream notice.

The esp-owl repository has mixed per-file notices but no repository-level
license at the pinned head. Code from it must therefore be reviewed file by
file before reuse. A file without a clear compatible grant remains research
input only. See [licensing and contribution policy](licensing.md).

## References

- [One Billion Apples' Secret Sauce](https://arxiv.org/abs/1808.03156)
- [OWL](https://github.com/seemoo-lab/owl)
- [esp-owl](https://github.com/archef2000/esp-owl)
- [ESP-IDF ESP32-S3 Wi-Fi driver](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-guides/wifi.html)
