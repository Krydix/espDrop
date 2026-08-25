#include "espdrop/ble_wake.h"

#include <limits.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "espdrop_ble_wake";
static uint8_t manufacturer_data[ESPDROP_BLE_AIRDROP_MANUFACTURER_BYTES];
static uint32_t advertisement_duration_ms;
static bool host_started;
static bool advertising;

static int start_advertising(void);

static int gap_event(struct ble_gap_event *event, void *context)
{
    (void)context;
    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        advertising = false;
        ESP_LOGI(TAG, "AirDrop wake advertisement complete reason=%d",
                 event->adv_complete.reason);
    }
    return 0;
}

static void on_sync(void)
{
    (void)start_advertising();
}

static int start_advertising(void)
{
    uint8_t own_address_type = 0;
    int result = ble_hs_id_infer_auto(0, &own_address_type);
    if (result != 0) {
        ESP_LOGE(TAG, "cannot infer BLE address type rc=%d", result);
        return ESP_FAIL;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.mfg_data = manufacturer_data;
    fields.mfg_data_len = sizeof(manufacturer_data);
    result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(TAG, "cannot set AirDrop wake advertisement rc=%d", result);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.conn_mode = BLE_GAP_CONN_MODE_NON;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(own_address_type, NULL,
                               (int32_t)advertisement_duration_ms,
                               &parameters, gap_event, NULL);
    if (result != 0) {
        ESP_LOGE(TAG, "cannot start AirDrop wake advertisement rc=%d", result);
        return ESP_FAIL;
    }

    advertising = true;
    ESP_LOGW(TAG,
             "AirDrop Everyone-mode BLE wake armed for %lu ms; "
             "manufacturer=4c000512... version=1 hashes=zero",
             (unsigned long)advertisement_duration_ms);
    return ESP_OK;
}

static void host_task(void *context)
{
    (void)context;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

int espdrop_ble_wake_start(uint32_t duration_ms)
{
    if (duration_ms == 0U || duration_ms > INT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (espdrop_ble_airdrop_manufacturer_data(
            manufacturer_data, sizeof(manufacturer_data), NULL) !=
        sizeof(manufacturer_data)) {
        return ESP_FAIL;
    }

    advertisement_duration_ms = duration_ms;
    if (host_started) {
        if (advertising) {
            const int stop_result = ble_gap_adv_stop();
            if (stop_result != 0 && stop_result != BLE_HS_EALREADY) {
                ESP_LOGE(TAG, "cannot restart AirDrop wake rc=%d",
                         stop_result);
                return ESP_FAIL;
            }
            advertising = false;
            /* Let NimBLE deliver the completion event for the old instance
             * before installing the new bounded advertisement. */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return start_advertising();
    }
    const esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s",
                 esp_err_to_name(result));
        return result;
    }

    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    host_started = true;
    return ESP_OK;
}

int espdrop_ble_wake_stop(void)
{
    if (!host_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!advertising) {
        return ESP_OK;
    }

    const int result = ble_gap_adv_stop();
    if (result != 0 && result != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "cannot stop AirDrop wake advertisement rc=%d", result);
        return ESP_FAIL;
    }
    advertising = false;
    ESP_LOGI(TAG, "AirDrop wake advertisement stopped before AWDL transfer");
    return ESP_OK;
}

bool espdrop_ble_wake_active(void)
{
    return advertising;
}
