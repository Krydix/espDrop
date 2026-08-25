#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_AIRDROP_ASK_BODY_MAX_BYTES 1536U
#define ESPDROP_AIRDROP_ASK_REQUEST_MAX_BYTES 2048U
#define ESPDROP_AIRDROP_SENDER_ID_BYTES 13U

typedef struct {
    const char *sender_computer_name;
    const char *sender_model_name;
    const char *sender_id;
    const char *transfer_id;
    const char *file_name;
    const char *file_type;
    size_t file_size;
} espdrop_airdrop_ask_file_t;

/* Build the minimum one-file binary plist used by the first sender lab. The
 * schema includes TransferID={id=UUID}, TransferType={files={}}, and the
 * native FileSize metadata so a later upload can be bound to the exact
 * accepted session. Strings are deliberately restricted to bounded printable
 * ASCII in this first compatibility profile. */
size_t espdrop_airdrop_build_ask_body(
    uint8_t *output,
    size_t capacity,
    const espdrop_airdrop_ask_file_t *file);

/* Wrap a caller-supplied binary plist in OpenDrop-compatible /Ask headers. */
size_t espdrop_airdrop_build_ask_request(
    uint8_t *output,
    size_t capacity,
    const char *host,
    uint16_t port,
    const uint8_t *body,
    size_t body_bytes);

/* Format random bytes as an upper-case RFC 4122 version-4 TransferID and a
 * lower-case 12-hex-character ephemeral SenderID. */
void espdrop_airdrop_format_transfer_id(
    char output[37],
    const uint8_t random_bytes[16]);
void espdrop_airdrop_format_sender_id(
    char output[ESPDROP_AIRDROP_SENDER_ID_BYTES],
    const uint8_t random_bytes[6]);

#ifdef __cplusplus
}
#endif
