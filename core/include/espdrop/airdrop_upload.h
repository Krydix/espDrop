#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_AIRDROP_TRANSFER_ID_BYTES 37U
#define ESPDROP_AIRDROP_SENDER_PSEUDONYM_BYTES 29U
#define ESPDROP_AIRDROP_SENDER_PUSH_TOKEN_BYTES 65U
#define ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES 4U
#define ESPDROP_AIRDROP_DVZIP_STREAM_BLOCK_BYTES 131072U
#define ESPDROP_AIRDROP_ODC_HEADER_BYTES 76U
#define ESPDROP_AIRDROP_ODC_BLOCK_BYTES 10240U

typedef struct {
    char transfer_id[ESPDROP_AIRDROP_TRANSFER_ID_BYTES];
    char sender_pseudonym[ESPDROP_AIRDROP_SENDER_PSEUDONYM_BYTES];
    char sender_push_token[ESPDROP_AIRDROP_SENDER_PUSH_TOKEN_BYTES];
} espdrop_airdrop_upload_identity_t;

typedef bool (*espdrop_airdrop_source_read_t)(
    void *context,
    uint8_t *output,
    size_t capacity,
    size_t *bytes_read);
typedef bool (*espdrop_airdrop_source_rewind_t)(void *context);

typedef bool (*espdrop_airdrop_stream_write_t)(
    void *context,
    const uint8_t *data,
    size_t data_bytes);

/* size_bytes is authoritative and must remain stable for the synchronous
 * send. read may return short chunks, but zero bytes before size_bytes is a
 * truncation. A seekable/spooled source supplies rewind so the sender can do
 * a constant-memory compression sizing pass followed by the upload pass. */
typedef struct {
    void *context;
    size_t size_bytes;
    espdrop_airdrop_source_read_t read;
    espdrop_airdrop_source_rewind_t rewind;
} espdrop_airdrop_source_t;

/* Synchronous one-file relay descriptor. file_name is a leaf name presented
 * to the receiver, file_type is an Apple UTI such as public.jpeg, and source
 * remains owned by the caller until the send returns. */
typedef struct {
    const char *file_name;
    const char *file_type;
    uint32_t mtime;
    espdrop_airdrop_source_t source;
} espdrop_airdrop_outgoing_file_t;

typedef struct {
    size_t file_bytes;
    size_t archive_bytes;
    size_t dvzip_blocks;
    size_t payload_bytes;
} espdrop_airdrop_stream_plan_t;

typedef struct {
    size_t source_bytes;
    size_t archive_bytes;
    size_t dvzip_blocks;
    size_t payload_bytes;
    size_t workspace_high_water;
    uint32_t source_crc32;
} espdrop_airdrop_stream_result_t;

typedef enum {
    ESPDROP_AIRDROP_STREAM_OK = 0,
    ESPDROP_AIRDROP_STREAM_INVALID = -1,
    ESPDROP_AIRDROP_STREAM_SIZE = -2,
    ESPDROP_AIRDROP_STREAM_SOURCE = -3,
    ESPDROP_AIRDROP_STREAM_TRUNCATED = -4,
    ESPDROP_AIRDROP_STREAM_SINK = -5,
    ESPDROP_AIRDROP_STREAM_REWIND = -6,
    ESPDROP_AIRDROP_STREAM_COMPRESS = -7,
} espdrop_airdrop_stream_status_t;

bool espdrop_airdrop_transfer_id_valid(const char *value);

/* Validate the observed iOS 26 Everyone-mode field shapes. This proves only
 * syntax; espDrop does not claim that the pseudonym or push token is an Apple
 * identity. */
bool espdrop_airdrop_upload_identity_valid(
    const espdrop_airdrop_upload_identity_t *identity);

/* Format anonymous Everyone-mode sender fields with the shapes captured from
 * iOS 26: URL-safe unpadded base64 over 16 random bytes, and upper-case hex
 * over 32 random bytes. These are ephemeral syntax, not Apple identities. */
void espdrop_airdrop_format_sender_pseudonym(
    char output[ESPDROP_AIRDROP_SENDER_PSEUDONYM_BYTES],
    const uint8_t random_bytes[16]);
void espdrop_airdrop_format_sender_push_token(
    char output[ESPDROP_AIRDROP_SENDER_PUSH_TOKEN_BYTES],
    const uint8_t random_bytes[32]);

/* Build one deterministic old-ASCII (odc) cpio archive and pad it to
 * libarchive's 10 KiB output block. This contiguous helper is retained for
 * fixtures and wire-format comparison; relay senders use the stream API. */
size_t espdrop_airdrop_build_odc_archive(
    uint8_t *output,
    size_t capacity,
    const char *archive_path,
    const uint8_t *file_data,
    size_t file_bytes,
    uint32_t mtime);

/* Compute exact ODC and stored-dvzip sizes without consuming the source. */
bool espdrop_airdrop_plan_stored_dvzip(
    espdrop_airdrop_stream_plan_t *plan,
    const char *archive_path,
    size_t file_bytes);

/* Stream one padded ODC cpio archive. workspace is the only source-data
 * buffer; no complete file or archive is retained. */
espdrop_airdrop_stream_status_t espdrop_airdrop_stream_odc(
    const espdrop_airdrop_source_t *source,
    const char *archive_path,
    uint32_t mtime,
    uint8_t *workspace,
    size_t workspace_bytes,
    espdrop_airdrop_stream_write_t write,
    void *write_context,
    espdrop_airdrop_stream_result_t *result);

/* Wrap the ODC stream in stored dvzip blocks. This makes TotalBytes
 * computable for a non-seekable source without a compression pre-pass. */
espdrop_airdrop_stream_status_t espdrop_airdrop_stream_stored_dvzip(
    const espdrop_airdrop_source_t *source,
    const char *archive_path,
    uint32_t mtime,
    uint8_t *workspace,
    size_t workspace_bytes,
    espdrop_airdrop_stream_write_t write,
    void *write_context,
    espdrop_airdrop_stream_result_t *result);

/* Build the exact minimal /Upload head observed in the successful iOS 26
 * interoperability run. Deliberately emits neither Host nor Accept-Encoding.
 * The caller streams chunked dvzip bytes immediately after this head. */
size_t espdrop_airdrop_build_upload_head(
    uint8_t *output,
    size_t capacity,
    const espdrop_airdrop_upload_identity_t *identity,
    size_t total_bytes);

/* Build one HTTP chunk prefix (hex byte count followed by CRLF) or the final
 * zero-length terminator. Payload bytes and the trailing CRLF are streamed by
 * the caller, so a complete non-final chunk is prefix + payload + "\r\n". */
size_t espdrop_airdrop_build_chunk_prefix(
    uint8_t *output,
    size_t capacity,
    size_t payload_bytes);
size_t espdrop_airdrop_build_chunk_terminator(
    uint8_t *output,
    size_t capacity);

/* Encode a dvzip block header. Bit 31 marks a stored block; the remaining
 * bits are the block's payload size. The payload is streamed separately. */
bool espdrop_airdrop_build_dvzip_block_header(
    uint8_t output[ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES],
    uint32_t payload_bytes,
    bool stored);

#ifdef __cplusplus
}
#endif
