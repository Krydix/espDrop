# Native macOS AirDrop research

The Mac can be used as a local reference implementation in addition to the
published OpenDrop and OWL code. Prefer wire captures and unified logs for
behavior, then use binary metadata or narrow disassembly to answer specific
unresolved questions. Do not copy Apple implementation code into espDrop.

Run the reproducible, read-only inventory with:

```text
./scripts/inspect_macos_airdrop.sh
```

## 2026-08-24 inventory

Host: macOS 26.6.2 (25G83), arm64.

Confirmed from `/usr/libexec/sharingd` metadata and strings:

- the daemon has the `com.apple.wifi.awdl` entitlement;
- current native code names `application/x-dvzip`, `EnableDVZip`,
  `TotalBytes`, `FileIcon`, and `SmallFileIcon`;
- imported Sharing framework constants include
  `kSFOperationFileIconKey`, `kSFOperationSmallFileIconKey`, and
  `kSFOperationTotalBytesKey`;
- retained Swift source-path metadata names `SDAirDropMessage.Ask.swift`,
  `SDAirDropSendCompressionAdapter.swift`, and
  `SDAirDropReceiveCompressionAdapter.swift`;
- native types include `SDAirDropBLEController`,
  `SDAirDropNearFieldService`, and `SDAirDropNearFieldActiveTapMonitor`.

The Sharing framework imports expose more precise current data-model fields:

- `SFAirDropSend.Request.itemPreviewData`;
- `SFAirDropSend.ItemMetadata.fileSize`, `fileName`, `fileType`,
  `fileSubType`, `fileBOMPath`, `fileIsDirectory`, and `previewImageData`;
- `SFAirDropReceive.AskRequest.previewImage` and `smallPreviewImage`.

These fields establish a preview-data path but do not guarantee that the
receiver UI renders it. The anonymous native Mac-to-iPhone control logged
preview data being added to `/Ask`, while the user observed a text-only consent
prompt. espDrop currently omits `FileIcon`; that remains a metadata difference,
but the prompt alone cannot diagnose it. The exact current macOS plist shape
and size limits remain to be captured before implementation.

Swift reflection metadata in the native daemon confirms the current legacy
wire keys used by its Ask model. Each file can carry `FileName`, `FileType`,
`FileSubType`, `FileSize`, `FileBomPath`, `FileIsDirectory`, and
`ShouldConvertMediaFormats`. Top-level Ask data includes `Items`, `Files`,
`ItemsDescription`, `FileIcon`, `SmallFileIcon`, `CustomPayload`, and sender
identity fields. espDrop currently omits `FileSize`, `FileIcon`, and
`SmallFileIcon`.

The native state model separately names Hello, Discover, Ask, Upload,
Exchange, IdentityShare, and Error request/response variants. Network metrics
record start/sent/finished phases for Hello and Ask, followed by Upload start
and finish. Therefore `/Ask` remains the consent boundary, while the exact
relationship between current Hello and legacy `/Discover` still needs a live
capture.

The Upload request model contains `totalBytes`, `contentType`, streamed `data`,
and `uploadComplete`. Static disassembly also confirms that the current native
HTTP path appends `TotalBytes` as a header. This makes completion signaling and
stream scheduling a high-priority comparison for the iPhone stall.

The exported native preview helper accepts optional image data, decodes it,
resizes the image through `SFResizedCGImage` with a bound of `0x100000`, and
encodes the resized image back to data. The exact output format and the
separate small-icon dimensions remain unknown.

## Successful native image control

On 2026-08-24 a native Finder transfer sent the same 53,359-byte
`catalina-coast.jpg` relay fixture to the test iPhone. The Mac and iPhone were
logged into the same Apple account. The user confirmed that the transfer
completed; the capture does not establish whether acceptance was manual or
same-account automatic. Filtered `sharingd` logs establish this sequence:

```text
HELLO request
  -> HELLO response
  -> add file metadata and preview data
  -> ASK request
  -> start UPLOAD on a second derived connection
  -> compress and send data while consent is pending
  -> UPLOAD response
  -> ASK response
  -> success
```

This transfer was reported as protocol 1, `legacy=false`, over AWDL. Network
logs show QUIC on `awdl0` with MSS 1452. The sender created separate Ask and
Upload connections from its established connection and began Upload 63
microseconds after Ask, roughly five seconds before the user-accept response.
The native zipper explicitly selected PKZip, not DVZip. The sender also logged
that it added preview data to Ask. Full retained facts are in
`docs/lab/2026-08-24-macos-airdrop-oracle-transfer.json`.

This is explicitly **not** an anonymous Everyone-mode oracle. Same-account
identity may have selected the protocol-1 application-service endpoint,
identity auth tags, pipelining, and PKZip. These observations explain several
differences from espDrop, but they are not permission to copy the modern path
blindly. espDrop reaches the iPhone's legacy `_airdrop._tcp` TLS/TCP endpoint,
where it has already proven the traditional consent-then-Upload sequence with
a small file. The later anonymous Mac-to-iPhone control below confirms that
the legacy protocol-0 path retains consent-then-Upload, one underlying TLS
connection, and CPIO with adaptive compression.

## Anonymous iPhone-to-Mac control

On 2026-08-24 a newly configured iPhone, not signed into the MacBook Air's
Apple account, sent one photo to the Mac through Everyone mode. The user
explicitly accepted the transfer and confirmed completion. This is the clean
anonymous control that the earlier same-account transfer could not provide.

The receiver trace shows `/Ask` at 06:11:20.399, acceptance at 06:11:25.434,
and a new incoming `/Upload` TCP/TLS flow at 06:11:25.596. Upload therefore
started 162 ms after consent and used a connection distinct from `/Ask`; it
was not pipelined while consent was pending. The receiver explicitly logged
that no `/Hello` request was present. It decoded CPIO with adaptive
compression, returned the Upload response after decompression, and recorded a
successful roughly-20-MB transfer.

The Ask connection negotiated TLS 1.3 with `TLS_AES_128_GCM_SHA256`, X25519,
and RSA-PSS-SHA256, without ALPN or resumption. The Mac requested an optional
client certificate and its external certificate verification completed. The
receiver still treated the transfer as an Everyone interaction; TLS
credentials and contact/account authorization are separate concepts.

This establishes the iPhone-sender-to-Mac-receiver topology, but the reverse
direction is different. A subsequent anonymous Mac-to-iPhone control reused
the Ask connection for Upload. The fresh-connection implementation remains a
directional research profile rather than espDrop's default sender behavior.
Full retained facts for this direction are in
[`lab/2026-08-24-iphone-macos-anonymous-receive.json`](lab/2026-08-24-iphone-macos-anonymous-receive.json).

## Anonymous Mac-to-iPhone sender control

The same new iPhone then received a Finder photo from the MacBook Air through
Everyone mode. The two devices were signed into different Apple accounts. The
native Mac added preview data to `/Ask`, but the user observed no preview in
the anonymous iPhone consent prompt. Preview transport and consent-UI rendering
are therefore separate facts; a text-only prompt is not evidence that the
sender omitted or malformed file metadata.

The sender skipped `/Hello`, waited for `/Ask` HTTP 200, extracted IDS session,
pseudonym, and push-token fields, then began `/Upload`. Network.framework
created request objects `C81` and `C82`, but `C82` explicitly joined `C81`'s
HTTP messaging protocol. Both reported flow UUID
`5C4A0754-624C-41EB-BA64-1179E4F162DB` and local AWDL port 49543. Ask was
`i29:c1:s1`; Upload was `i29:c1:s2`; the same BoringSSL session remained in
use. Thus the wire topology is sequential HTTP/1 requests on one TCP/TLS
connection, not a reconnect.

The successful 1,344,332-byte photo used CPIO with adaptive compression. The
native compressor detected incompressible photo data and switched compression
off. espDrop's sender now mirrors the confirmed same-socket topology; the next
payload investigation is its adaptive DVZip/CPIO block behavior. Full retained
facts are in
[`lab/2026-08-24-macos-iphone-anonymous-send.json`](lab/2026-08-24-macos-iphone-anonymous-send.json).

OpenDrop commit `11fe7ba7861093b302bc0637e8cb10adf2d29337` supplies the
historical identity-free receiver baseline: a random 12-hex service ID, a
self-signed TLS certificate, no Apple validation record, and decimal TXT
`flags=136` (mixed-types plus `/Discover`). espDrop now has a separate
receiver-oracle build that emits a complete PTR/SRV/TXT/AAAA announcement with
that profile. Its announcement round-trips through the bounded DNS-SD parser,
and normal/sender firmware leaves the profile disabled. Native macOS
discoverability and the subsequent TLS request are not yet hardware-confirmed.

The near-field strings are especially relevant to TapDrop. They refer to a
tap event containing public-key data, a transaction ID, an endpoint UUID, and
comparison with Bonjour endpoint identifiers/contact IDs. This is evidence
that Apple's own near-field flow performs protocol-level tap-to-peer
correlation. It does **not** yet prove that a passive or dynamic third-party
NFC tag can produce the required Apple near-field transaction.

The imported `SFAirDropClient.BoopAtADistance` types make the apparent
correlation tuple more concrete: server info contains `applicationLabel`,
`publicKeyData`, and `bonjourUUID`; tap events expose an identifier, remote
server info, device metadata, and optional contact/account context. These are
API/type names only. Their serialization and availability to third-party NFC
hardware remain unknown.

## Research order

1. Advertise the identity-free espDrop receiver and confirm that native macOS
   lists it as an AirDrop destination.
2. Terminate native TLS on espDrop and retain bounded plaintext `/Discover`,
   `/Ask`, and `/Upload` evidence.
3. Capture native plaintext at the controlled espDrop receiver and compare it
   with the anonymous iPhone-to-Mac sequence and same-account control.
4. Use symbol inspection or narrow disassembly only for facts the controlled
   receiver cannot expose.
5. Record every conclusion as confirmed, inferred, or unknown, including the
   macOS/iOS build used.

Static strings prove that a concept exists in the binary, not its wire value
or runtime behavior. Dynamic captures remain the source of truth.

## Native transfer capture

Run a bounded native transfer capture with:

```text
./scripts/capture_macos_airdrop_oracle.sh 60
```

During the window, send exactly one known image from the Mac and accept it on
the iPhone. The helper retains filtered `sharingd` logs, AirDrop Bonjour
events, the active AWDL interface state, and—when BPF permission permits—an
AWDL packet capture. TLS packet captures prove sequencing, packet sizes, and
retransmissions but do not expose encrypted plist or DVZip bodies. Exact
plaintext values still require a controlled receiver that terminates the
native sender's TLS connection.
