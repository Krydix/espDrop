#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "espdrop/espdrop.h"
#include "espdrop/awdl_probe.h"
#include "tapdrop/tapdrop.h"

static const char *TAG = "espdrop_app";

static esp_err_t turn_off_ws2812(gpio_num_t gpio)
{
    rmt_channel_handle_t channel = NULL;
    rmt_encoder_handle_t encoder = NULL;
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    esp_err_t result = rmt_new_tx_channel(&channel_config, &channel);
    if (result != ESP_OK) {
        return result;
    }

    const rmt_copy_encoder_config_t encoder_config = {};
    result = rmt_new_copy_encoder(&encoder_config, &encoder);
    if (result == ESP_OK) {
        result = rmt_enable(channel);
    }

    rmt_symbol_word_t off[24];
    for (size_t bit = 0; bit < 24; ++bit) {
        off[bit] = (rmt_symbol_word_t){
            .level0 = 1,
            .duration0 = 3,
            .level1 = 0,
            .duration1 = 9,
        };
    }
    if (result == ESP_OK) {
        const rmt_transmit_config_t transmit_config = {.loop_count = 0};
        result = rmt_transmit(channel, encoder, off, sizeof(off), &transmit_config);
    }
    if (result == ESP_OK) {
        result = rmt_tx_wait_all_done(channel, 100);
    }

    if (channel != NULL) {
        (void)rmt_disable(channel);
    }
    if (encoder != NULL) {
        (void)rmt_del_encoder(encoder);
    }
    if (channel != NULL) {
        (void)rmt_del_channel(channel);
    }
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, 0);
    return result;
}

static void turn_off_devkit_rgb(void)
{
    /* DevKitC-1 revisions use either GPIO 38 or GPIO 48 for the one-pixel LED. */
    ESP_ERROR_CHECK(turn_off_ws2812(GPIO_NUM_38));
    ESP_ERROR_CHECK(turn_off_ws2812(GPIO_NUM_48));
    ESP_LOGI(TAG, "onboard RGB LED off (GPIO 38/48 compatibility)");
}

static void log_board_info(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0;

    esp_chip_info(&chip);
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));
    ESP_LOGI(TAG, "ESP32-S3 revision %d, %d cores, %" PRIu32 " MiB flash",
             chip.revision, chip.cores, flash_size / (1024U * 1024U));
    ESP_LOGI(TAG, "free heap=%" PRIu32 " bytes, minimum=%" PRIu32 " bytes",
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "PSRAM total=%u bytes, free=%u bytes",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    turn_off_devkit_rgb();
    ESP_LOGI(TAG, "espDrop %s", espdrop_version());
    log_board_info();

    const espdrop_config_t config = {
        .device_name = CONFIG_ESPDROP_DEVICE_NAME,
        .accept_mode = ESPDROP_ACCEPT_CONFIRM,
        .max_transfer_bytes = CONFIG_ESPDROP_MAX_TRANSFER_BYTES,
    };
    ESP_ERROR_CHECK(espdrop_init(&config));

    const tapdrop_config_t tap_config = {
        .field_gpio = CONFIG_TAPDROP_NFC_FIELD_GPIO,
        .active_level = CONFIG_TAPDROP_NFC_ACTIVE_LEVEL,
        .session_window_ms = CONFIG_TAPDROP_SESSION_WINDOW_MS,
        .minimum_score = CONFIG_TAPDROP_MINIMUM_SCORE,
        .ambiguity_margin = CONFIG_TAPDROP_AMBIGUITY_MARGIN,
    };
    ESP_ERROR_CHECK(tapdrop_init(&tap_config));

    ESP_LOGI(TAG, "core ready; AWDL backend=%s, NFC field GPIO=%d",
             espdrop_awdl_backend_name(), tap_config.field_gpio);
    if (CONFIG_ESPDROP_AWDL_PROBE) {
        ESP_ERROR_CHECK(espdrop_awdl_probe_start(CONFIG_ESPDROP_AWDL_PROBE_CHANNEL));
    }
    ESP_LOGW(TAG, "protocol transport is research-stage; no AirDrop transfer is armed");
}
