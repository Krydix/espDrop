#include "espdrop/awdl_active_rx_lab.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_wifi_types_generic.h"

static const char *TAG = "espdrop_active_rx";

static const uint8_t AWDL_BSSID[6] = {
    0x00, 0x25, 0x00, 0xff, 0x94, 0x73,
};

/*
 * These functions are exported by ESP-IDF 5.4's precompiled ESP32-S3 Wi-Fi
 * libraries but deliberately have no public header. Reverse tracing the
 * driver's connection state machine shows this exact sequence immediately
 * before an ordinary station authenticates with an AP:
 *
 *   ic_set_bssid(WIFI_IF_STA, bssid);
 *   wifi_set_rx_policy(5);
 *   wifi_set_rx_policy(7);
 *
 * Profile 5 installs the pre-authentication unicast/BSSID receive policy. We
 * Profile 7 is the driver's subsequent authenticated-station transition. We
 * call the semantic driver wrapper, not private HAL functions or registers.
 */
extern void ic_set_bssid(uint8_t interface, const uint8_t *bssid);
extern bool wifi_set_rx_policy(uint8_t profile);

esp_err_t espdrop_awdl_active_rx_lab_enable(void)
{
#if ESP_IDF_VERSION_MAJOR == 5 && ESP_IDF_VERSION_MINOR == 4
    ic_set_bssid((uint8_t)WIFI_IF_STA, AWDL_BSSID);
    if (!wifi_set_rx_policy(5)) {
        ESP_LOGE(TAG, "Wi-Fi blob rejected station pre-auth RX profile");
        return ESP_FAIL;
    }
    if (!wifi_set_rx_policy(7)) {
        ESP_LOGE(TAG, "Wi-Fi blob rejected station authenticated RX profile");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG,
             "LAB ONLY: station authenticated RX policy bound to AWDL BSSID");
    return ESP_OK;
#else
    ESP_LOGE(TAG,
             "active-RX lab is pinned to ESP-IDF 5.4; refusing blob calls on "
             "IDF %d.%d",
             ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
