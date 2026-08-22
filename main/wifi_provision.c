#include "wifi_provision.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "nvs.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_RETRY_LIMIT 8U

static const char *TAG = "espdrop_wifi";
static EventGroupHandle_t event_group;
static unsigned retry_count;
static bool started;
static char current_ip[16];

static void event_handler(void *context, esp_event_base_t base, int32_t event_id,
                          void *event_data)
{
    (void)context;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT);
        current_ip[0] = '\0';
        if (retry_count++ < WIFI_RETRY_LIMIT) {
            (void)esp_wifi_connect();
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        snprintf(current_ip, sizeof(current_ip), IPSTR,
                 IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "maintenance Wi-Fi connected at %s", current_ip);
    }
}

esp_err_t wifi_provision_is_configured(bool *configured)
{
    if (configured == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *configured = false;
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("espdrop", NVS_READONLY, &nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }
    uint8_t value = 0;
    result = nvs_get_u8(nvs, "wifi_ready", &value);
    nvs_close(nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result == ESP_OK) {
        *configured = value == 1U;
    }
    return result;
}

esp_err_t wifi_provision_start(void)
{
    if (started) {
        return ESP_OK;
    }
    event_group = xEventGroupCreate();
    if (event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "initialize netif");
    esp_err_t result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }
    const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&config), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "enable credential persistence");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL),
                        TAG, "register Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL),
                        TAG, "register IP events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
    started = true;

    wifi_config_t stored = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK &&
        stored.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "using stored maintenance Wi-Fi credentials");
        return esp_wifi_connect();
    }
    ESP_LOGI(TAG, "waiting for Wi-Fi credentials over Improv Serial");
    return ESP_OK;
}

esp_err_t wifi_provision_connect(const char *ssid, const char *password)
{
    if (!started || ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32U ||
        (password != NULL && strlen(password) > 63U)) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s",
             password != NULL ? password : "");
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    retry_count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                        "store credentials");
    esp_err_t result = esp_wifi_connect();
    return result == ESP_ERR_WIFI_CONN ? ESP_OK : result;
}

esp_err_t wifi_provision_wait_connected(unsigned timeout_ms)
{
    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_provision_mark_configured(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("espdrop", NVS_READWRITE, &nvs), TAG,
                        "open settings");
    esp_err_t result = nvs_set_u8(nvs, "wifi_ready", 1U);
    if (result == ESP_OK) {
        result = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return result;
}

bool wifi_provision_connected(void)
{
    return started &&
           (xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0U;
}

void wifi_provision_ip(char *destination, size_t capacity)
{
    if (destination != NULL && capacity > 0U) {
        snprintf(destination, capacity, "%s", current_ip);
    }
}
