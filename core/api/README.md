# Application API

Applications include `espdrop/espdrop.h`. Protocol implementation details
stay behind this boundary. Discovery and send calls currently return
`ESP_ERR_NOT_SUPPORTED`; this is intentional fail-closed behavior until an
AWDL netif exists. Receive handlers may be registered now and will become
active with the receiver backend.

The attended sender lab also exposes the lower-level
`espdrop_airdrop_outgoing_file_t` relay contract. A producer supplies a stable
byte count plus synchronous short-read and optional rewind callbacks. A
seekable source is zlib-counted, rewound, and streamed after `/Ask` acceptance;
the source bytes are never retained as a complete file. The path uses a 2 KiB
source workspace, fixed compressor state in PSRAM, and a fixed 16 KiB network
staging buffer. This transport primitive is implemented and hardware-tested;
it is not yet promoted to `espdrop_send_file()` because public peer selection,
job ownership, and cancellation are still unfinished.
