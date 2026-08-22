#include "tapdrop/tapdrop.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "tapdrop";
static tapdrop_config_t active_config;
static tapdrop_session_t session;
static portMUX_TYPE session_lock = portMUX_INITIALIZER_UNLOCKED;
static bool initialized;

static uint64_t monotonic_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void IRAM_ATTR field_isr(void *argument)
{
    (void)argument;
    const uint64_t tap_ms = monotonic_ms();
    portENTER_CRITICAL_ISR(&session_lock);
    ++session.generation;
    session.tap_ms = tap_ms;
    session.expires_ms = tap_ms + active_config.session_window_ms;
    session.active = true;
    portEXIT_CRITICAL_ISR(&session_lock);
}

esp_err_t tapdrop_init(const tapdrop_config_t *config)
{
    if (config == NULL || config->active_level < 0 ||
        config->active_level > 1 || config->session_window_ms == 0 ||
        config->minimum_score < 0 || config->ambiguity_margin < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    active_config = *config;
    session = (tapdrop_session_t){0};

    if (config->field_gpio >= 0) {
        const gpio_config_t pin_config = {
            .pin_bit_mask = 1ULL << (unsigned)config->field_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = config->active_level == 0,
            .pull_down_en = config->active_level == 1,
            .intr_type = config->active_level == 1
                             ? GPIO_INTR_POSEDGE
                             : GPIO_INTR_NEGEDGE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&pin_config), TAG,
                            "configure NFC field GPIO");
        esp_err_t result = gpio_install_isr_service(0);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            return result;
        }
        ESP_RETURN_ON_ERROR(
            gpio_isr_handler_add((gpio_num_t)config->field_gpio, field_isr, NULL),
            TAG, "install NFC field ISR");
        ESP_LOGI(TAG, "NFC field detection armed on GPIO %d",
                 config->field_gpio);
    } else {
        ESP_LOGI(TAG, "NFC field GPIO disabled; software tap events remain available");
    }

    initialized = true;
    return ESP_OK;
}

esp_err_t tapdrop_signal_tap(uint64_t timestamp_ms)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timestamp_ms == 0) {
        timestamp_ms = monotonic_ms();
    }
    portENTER_CRITICAL(&session_lock);
    ++session.generation;
    session.tap_ms = timestamp_ms;
    session.expires_ms = timestamp_ms + active_config.session_window_ms;
    session.active = true;
    portEXIT_CRITICAL(&session_lock);
    ESP_LOGI(TAG, "tap session %lu opened for %lu ms",
             (unsigned long)session.generation,
             (unsigned long)active_config.session_window_ms);
    return ESP_OK;
}

tapdrop_session_t tapdrop_current_session(void)
{
    tapdrop_session_t snapshot;
    portENTER_CRITICAL(&session_lock);
    snapshot = session;
    portEXIT_CRITICAL(&session_lock);
    if (snapshot.active && monotonic_ms() > snapshot.expires_ms) {
        snapshot.active = false;
    }
    return snapshot;
}

tapdrop_correlation_result_t tapdrop_select_peer(
    const espdrop_peer_table_t *table,
    uint64_t now_ms)
{
    const tapdrop_session_t snapshot = tapdrop_current_session();
    if (!snapshot.active || table == NULL) {
        return (tapdrop_correlation_result_t){
            .status = snapshot.tap_ms != 0
                          ? TAPDROP_CORRELATION_EXPIRED
                          : TAPDROP_CORRELATION_NONE,
        };
    }
    const tapdrop_correlation_policy_t policy = {
        .session_window_ms = active_config.session_window_ms,
        .pre_tap_grace_ms = 1500,
        .minimum_score = active_config.minimum_score,
        .ambiguity_margin = active_config.ambiguity_margin,
    };
    return tapdrop_correlate(table->peers, table->count, snapshot.tap_ms,
                             now_ms, &policy);
}
