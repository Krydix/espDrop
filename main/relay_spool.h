#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "espdrop/airdrop_upload.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_SPOOL_FILE_NAME_MAX 127U
#define RELAY_SPOOL_FILE_TYPE_MAX 63U

esp_err_t relay_spool_init(void);
size_t relay_spool_capacity(void);
bool relay_spool_is_ready(void);

esp_err_t relay_spool_begin(size_t size_bytes, uint32_t expected_crc32,
                            const char *file_name, const char *file_type);
esp_err_t relay_spool_write(const void *data, size_t size_bytes);
esp_err_t relay_spool_finish(void);
void relay_spool_abort(void);

esp_err_t relay_spool_outgoing_file(
    espdrop_airdrop_outgoing_file_t *file);

#ifdef __cplusplus
}
#endif
