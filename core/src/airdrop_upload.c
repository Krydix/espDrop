#include "espdrop/airdrop_upload.h"

#include <stdio.h>
#include <string.h>

static bool is_upper_hex(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'F');
}

static bool transfer_id_valid(const char *value)
{
    if (value == NULL || strlen(value) != 36U) {
        return false;
    }
    for (size_t index = 0U; index < 36U; ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') {
                return false;
            }
        } else if (!is_upper_hex(value[index])) {
            return false;
        }
    }
    return true;
}

static bool pseudonym_valid(const char *value)
{
    if (value == NULL || strlen(value) != 28U ||
        memcmp(value, "pseud:", 6U) != 0) {
        return false;
    }
    for (size_t index = 6U; index < 28U; ++index) {
        const char byte = value[index];
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '-' || byte == '_')) {
            return false;
        }
    }
    return true;
}

static bool push_token_valid(const char *value)
{
    if (value == NULL || strlen(value) != 64U) {
        return false;
    }
    for (size_t index = 0U; index < 64U; ++index) {
        if (!is_upper_hex(value[index])) {
            return false;
        }
    }
    return true;
}

bool espdrop_airdrop_upload_identity_valid(
    const espdrop_airdrop_upload_identity_t *identity)
{
    return identity != NULL && transfer_id_valid(identity->transfer_id) &&
           pseudonym_valid(identity->sender_pseudonym) &&
           push_token_valid(identity->sender_push_token);
}

size_t espdrop_airdrop_build_upload_head(
    uint8_t *output,
    size_t capacity,
    const espdrop_airdrop_upload_identity_t *identity,
    size_t total_bytes)
{
    if (output == NULL || capacity == 0U || total_bytes == 0U ||
        !espdrop_airdrop_upload_identity_valid(identity)) {
        return 0U;
    }
    const int length = snprintf(
        (char *)output, capacity,
        "POST /Upload HTTP/1.1\r\n"
        "User-Agent: AirDrop/1.0\r\n"
        "TotalBytes: %zu\r\n"
        "Content-Type: application/x-dvzip\r\n"
        "SenderPseudonym: %s\r\n"
        "SenderPushToken: %s\r\n"
        "TransferID: %s\r\n"
        "Connection: keep-alive\r\n"
        "Transfer-Encoding: chunked\r\n\r\n",
        total_bytes, identity->sender_pseudonym,
        identity->sender_push_token, identity->transfer_id);
    return length < 0 || (size_t)length >= capacity ? 0U : (size_t)length;
}

size_t espdrop_airdrop_build_chunk_prefix(
    uint8_t *output,
    size_t capacity,
    size_t payload_bytes)
{
    if (output == NULL || capacity == 0U || payload_bytes == 0U) {
        return 0U;
    }
    const int length = snprintf((char *)output, capacity, "%zx\r\n",
                                payload_bytes);
    return length < 0 || (size_t)length >= capacity ? 0U : (size_t)length;
}

size_t espdrop_airdrop_build_chunk_terminator(
    uint8_t *output,
    size_t capacity)
{
    static const uint8_t terminator[] = {'0', '\r', '\n', '\r', '\n'};
    if (output == NULL || capacity < sizeof(terminator)) {
        return 0U;
    }
    memcpy(output, terminator, sizeof(terminator));
    return sizeof(terminator);
}

bool espdrop_airdrop_build_dvzip_block_header(
    uint8_t output[ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES],
    uint32_t payload_bytes,
    bool stored)
{
    if (output == NULL || payload_bytes == 0U ||
        payload_bytes > UINT32_C(0x7fffffff)) {
        return false;
    }
    const uint32_t encoded = payload_bytes |
        (stored ? UINT32_C(0x80000000) : UINT32_C(0));
    output[0] = (uint8_t)(encoded >> 24U);
    output[1] = (uint8_t)(encoded >> 16U);
    output[2] = (uint8_t)(encoded >> 8U);
    output[3] = (uint8_t)encoded;
    return true;
}
