# espDrop core

This ESP-IDF component owns the transport-independent public API and the
AirDrop/AWDL stack boundaries:

    core/
    ├── awdl/       raw frames, synchronization, channels, peer table
    ├── ble/        AirDrop discovery advertisements and observations
    ├── network/    IPv6 and Bonjour/mDNS over the AWDL netif
    ├── airdrop/    discovery, TLS, metadata, sender, receiver
    └── api/        stable application-facing send/receive/discover API

Only the peer model and lifecycle API are implemented in the first scaffold.
Unimplemented transport calls must fail closed; they must never pretend that a
transfer or peer correlation succeeded.
