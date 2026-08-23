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

typedef struct {
    char transfer_id[ESPDROP_AIRDROP_TRANSFER_ID_BYTES];
    char sender_pseudonym[ESPDROP_AIRDROP_SENDER_PSEUDONYM_BYTES];
    char sender_push_token[ESPDROP_AIRDROP_SENDER_PUSH_TOKEN_BYTES];
} espdrop_airdrop_upload_identity_t;

/* Validate the observed iOS 26 Everyone-mode field shapes. This proves only
 * syntax; espDrop does not claim that the pseudonym or push token is an Apple
 * identity. */
bool espdrop_airdrop_upload_identity_valid(
    const espdrop_airdrop_upload_identity_t *identity);

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
