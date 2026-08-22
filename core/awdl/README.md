# AWDL boundary

The embedded AWDL backend will live here. The initial backend is a receive-only
channel-6 probe: it recognizes AWDL PSF/MIF vendor action frames and logs their
ephemeral source, RSSI, channel, and radio timestamp. It does not parse TLVs,
synchronize, transmit, or attach a netif.

Phase 1 replaces the probe only after bidirectional IPv6 packets have been
captured and reproduced on the ESP32-S3.
