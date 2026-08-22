#include "ota_update.h"

#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi_provision.h"

#ifndef ESPDROP_OTA_URL
#define ESPDROP_OTA_URL                                                        \
    "https://krydix.github.io/espDrop/firmware/" CONFIG_IDF_TARGET            \
    "/espdrop.bin"
#endif

#define OTA_CONNECT_TIMEOUT_MS 30000U
#define OTA_CLOCK_MINIMUM 1704067200

static const char *TAG = "espdrop_ota";

const char *ota_update_github_url(void)
{
    return ESPDROP_OTA_URL;
}

esp_err_t ota_update_init(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL) {
        ESP_LOGE(TAG, "no inactive OTA slot; a full USB migration is required");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "inactive OTA slot %s has %lu bytes", next->label,
             (unsigned long)next->size);
    return ESP_OK;
}

esp_err_t ota_update_is_pending(bool *pending)
{
    if (pending == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pending = false;
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("espdrop", NVS_READONLY, &nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }
    uint8_t value = 0;
    result = nvs_get_u8(nvs, "ota_pending", &value);
    nvs_close(nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result == ESP_OK) {
        *pending = value == 1U;
    }
    return result;
}

static esp_err_t set_pending(uint8_t value)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("espdrop", NVS_READWRITE, &nvs), TAG,
                        "open OTA settings");
    esp_err_t result = nvs_set_u8(nvs, "ota_pending", value);
    if (result == ESP_OK) {
        result = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return result;
}

esp_err_t ota_update_request_github(void)
{
    bool configured = false;
    ESP_RETURN_ON_ERROR(wifi_provision_is_configured(&configured), TAG,
                        "read provisioning state");
    if (!configured) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(set_pending(1U), TAG, "arm OTA boot");
    ESP_LOGW(TAG, "GitHub OTA armed for next boot: %s", ESPDROP_OTA_URL);
    return ESP_OK;
}

static esp_err_t synchronize_clock(void)
{
    if (time(NULL) >= OTA_CLOCK_MINIMUM) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "synchronizing clock for certificate validation");
    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t result = esp_netif_sntp_init(&config);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    for (unsigned attempt = 0; attempt < 8U &&
                               time(NULL) < OTA_CLOCK_MINIMUM;
         ++attempt) {
        (void)esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
    }
    return time(NULL) >= OTA_CLOCK_MINIMUM ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t validate_description(const esp_app_desc_t *description)
{
    if (description == NULL ||
        strcmp(description->project_name, "espdrop") != 0) {
        ESP_LOGE(TAG, "refusing firmware for project '%s'",
                 description != NULL ? description->project_name : "");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t ota_update_apply_pending(void)
{
    bool pending = false;
    ESP_RETURN_ON_ERROR(ota_update_is_pending(&pending), TAG,
                        "read OTA request");
    if (!pending) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Consume the request before networking so a failed update cannot create a
     * reboot loop. A new request can always be sent over the physical USB link. */
    ESP_RETURN_ON_ERROR(set_pending(0U), TAG, "consume OTA request");
    ESP_LOGW(TAG, "OTA maintenance boot; normal AWDL radio is paused");
    ESP_RETURN_ON_ERROR(wifi_provision_start(), TAG,
                        "start maintenance Wi-Fi");
    ESP_RETURN_ON_ERROR(
        wifi_provision_wait_connected(OTA_CONNECT_TIMEOUT_MS), TAG,
        "connect maintenance Wi-Fi");
    ESP_RETURN_ON_ERROR(synchronize_clock(), TAG, "synchronize clock");

    esp_http_client_config_t http_config = {
        .url = ESPDROP_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
    esp_https_ota_handle_t handle = NULL;
    ESP_LOGI(TAG, "checking %s", ESPDROP_OTA_URL);
    esp_err_t result = esp_https_ota_begin(&ota_config, &handle);
    if (result != ESP_OK) {
        return result;
    }

    esp_app_desc_t next = {0};
    result = esp_https_ota_get_img_desc(handle, &next);
    if (result == ESP_OK) {
        result = validate_description(&next);
    }
    if (result != ESP_OK) {
        esp_https_ota_abort(handle);
        return result;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (strcmp(next.version, running->version) == 0) {
        ESP_LOGI(TAG, "already running published version %s", running->version);
        esp_https_ota_abort(handle);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "installing %s over %s", next.version, running->version);

    do {
        result = esp_https_ota_perform(handle);
        const int written = esp_https_ota_get_image_len_read(handle);
        if (written > 0 && ((unsigned)written % (256U * 1024U)) < 4096U) {
            ESP_LOGI(TAG, "downloaded %d bytes", written);
        }
    } while (result == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (result != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    }
    result = esp_https_ota_finish(handle);
    if (result == ESP_OK) {
        ESP_LOGW(TAG, "firmware %s installed; restarting into verification",
                 next.version);
    }
    return result;
}

esp_err_t ota_update_confirm_running(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t result = esp_ota_get_state_partition(running, &state);
    if (result == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_RETURN_ON_ERROR(esp_ota_mark_app_valid_cancel_rollback(), TAG,
                            "confirm OTA image");
        ESP_LOGI(TAG, "startup checks passed; OTA rollback cancelled");
    } else if (result != ESP_OK && result != ESP_ERR_NOT_SUPPORTED) {
        return result;
    }
#endif
    return ESP_OK;
}
