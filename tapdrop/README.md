# TapDrop

TapDrop is the optional NFC tap-to-peer extension. It records a short-lived
physical-intent session and ranks ephemeral AirDrop peers using timing,
appearance transitions, BLE/AWDL co-observation, and RSSI.

A score never bypasses ambiguity handling. If the top two peers fall within
the configured margin, the result is ambiguous and the application must ask
for physical or UI confirmation.
