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
3. **PARTIAL:** emit synchronized PSF/MIF action frames; Apple peer admission
   is not yet demonstrated.
4. Inject one unicast AWDL data frame and confirm it over the air.
5. Attach an ESP-IDF netif with a link-local IPv6 address.
6. Pass ICMPv6 echo in both directions.
7. Pass UDP multicast on port 5353 in both directions.
8. Run for 30 minutes without schedule drift, watchdog reset, or peer loss.

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

**NOT CONFIRMED:** neither Apple peer sent a directed AWDL action to the ESP,
and macOS did not create an IPv6 neighbor entry for the ESP. Apple action
traffic remained active throughout, but that alone does not prove either peer
parsed or admitted the ESP. Gate 3 therefore remains partial. The compact
evidence is preserved in
[`lab/2026-08-23-awdl-tx-lab.json`](lab/2026-08-23-awdl-tx-lab.json).

The capture also corrected two stale OWL-era assumptions for the current Apple
frames observed here: the version TLV carries `0xa0`, and ARPA/name data uses
TLV type 16 rather than type 17. These are observations, not yet general
compatibility claims.

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
- **PARTIALLY CONFIRMED:** ESP-IDF can radio-complete bounded AWDL management
  action frames without driver error. Current Apple parsing/admission and raw
  AWDL data-frame handling remain unknown.
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
