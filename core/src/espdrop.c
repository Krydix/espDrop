#include "espdrop/espdrop.h"

#include <stdbool.h>

#include "esp_log.h"

static const char *TAG = "espdrop";
static espdrop_peer_table_t peer_table;
static espdrop_config_t active_config;
static bool initialized;
static espdrop_receive_handler_t receive_handler;
static void *receive_context;

esp_err_t espdrop_init(const espdrop_config_t *config)
{
    if (config == NULL || config->device_name == NULL ||
        config->device_name[0] == '\0' || config->max_transfer_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    active_config = *config;
    espdrop_peer_table_init(&peer_table);
    initialized = true;

    ESP_LOGI(TAG, "device=%s, max transfer=%llu bytes, accept mode=%d",
             active_config.device_name,
             (unsigned long long)active_config.max_transfer_bytes,
             (int)active_config.accept_mode);
    return ESP_OK;
}

const char *espdrop_version(void)
{
    return "0.1.0";
}

const char *espdrop_awdl_backend_name(void)
{
    return "channel-6-probe";
}

espdrop_peer_table_t *espdrop_peers(void)
{
    return initialized ? &peer_table : NULL;
}

esp_err_t espdrop_discover(uint32_t timeout_ms, size_t *peer_count)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0 || peer_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *peer_count = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espdrop_send_file(const espdrop_peer_t *peer, const char *path)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (peer == NULL || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espdrop_on_receive(
    espdrop_receive_handler_t handler,
    void *context)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    receive_handler = handler;
    receive_context = context;
    (void)receive_handler;
    (void)receive_context;
    return ESP_OK;
}
