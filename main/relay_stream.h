#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "espdrop/airdrop_upload.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_STREAM_CHUNK_MAX 4096U

typedef esp_err_t (*relay_stream_notify_t)(const void *data, size_t length);

esp_err_t relay_stream_init(relay_stream_notify_t notify);

esp_err_t relay_stream_begin(
    size_t file_bytes,
    uint32_t file_crc32,
    size_t payload_bytes,
    uint32_t payload_crc32,
    size_t archive_bytes,
    size_t dvzip_blocks,
    const char *file_name,
    const char *file_type,
    espdrop_airdrop_outgoing_file_t *outgoing);

esp_err_t relay_stream_push_chunk(
    uint32_t sequence,
    const uint8_t *data,
    size_t data_bytes,
    uint32_t expected_crc32);

esp_err_t relay_stream_validate_chunk(uint32_t sequence, size_t data_bytes);

void relay_stream_abort(void);
bool relay_stream_is_armed(void);

#ifdef __cplusplus
}
#endif
