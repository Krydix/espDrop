#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "espdrop/peer_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_name;
    espdrop_accept_mode_t accept_mode;
    uint64_t max_transfer_bytes;
} espdrop_config_t;

typedef void (*espdrop_receive_handler_t)(
    const char *path,
    const char *mime_type,
    uint64_t size,
    void *context);

esp_err_t espdrop_init(const espdrop_config_t *config);
const char *espdrop_version(void);
const char *espdrop_awdl_backend_name(void);
espdrop_peer_table_t *espdrop_peers(void);
esp_err_t espdrop_discover(uint32_t timeout_ms, size_t *peer_count);
esp_err_t espdrop_send_file(const espdrop_peer_t *peer, const char *path);
esp_err_t espdrop_on_receive(
    espdrop_receive_handler_t handler,
    void *context);

#ifdef __cplusplus
}
#endif
