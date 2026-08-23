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
#define ESPDROP_AIRDROP_ODC_HEADER_BYTES 76U
#define ESPDROP_AIRDROP_ODC_BLOCK_BYTES 10240U

typedef struct {
    char transfer_id[ESPDROP_AIRDROP_TRANSFER_ID_BYTES];
    char sender_pseudonym[ESPDROP_AIRDROP_SENDER_PSEUDONYM_BYTES];
    char sender_push_token[ESPDROP_AIRDROP_SENDER_PUSH_TOKEN_BYTES];
} espdrop_airdrop_upload_identity_t;

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
 * libarchive's 10 KiB output block. The archive path is a single normalized
 * "./name" entry and the source bytes can come from flash, PSRAM, or storage.
 * This bounded helper is the first lab payload; the production path will
 * replace the contiguous source with a storage stream. */
size_t espdrop_airdrop_build_odc_archive(
    uint8_t *output,
    size_t capacity,
    const char *archive_path,
    const uint8_t *file_data,
    size_t file_bytes,
    uint32_t mtime);

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
