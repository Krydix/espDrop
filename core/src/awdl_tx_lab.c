#include "espdrop/awdl_tx_lab.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "espdrop/awdl_tx.h"
#include "espdrop/awdl_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_ESPDROP_AWDL_TX_LAB
#include "esp_private/wifi.h"
#endif

#define AWDL_TX_LAB_WINDOW_MS 15000U
#define AWDL_TX_LAB_START_DELAY_MS 1500U
#define AWDL_TX_LAB_PROBE_LIMIT 14U
#define AWDL_TX_LAB_CHANNEL_WINDOW_GUARD_US 4000U
#define AWDL_TX_LAB_AWDL_SEQUENCE_OFFSET 34U
#define AWDL_TX_LAB_ECHO_SEQUENCE_MARKER 0x4000U
#define AWDL_TX_LAB_ECHO_IDENTIFIER 0xed01U

#if CONFIG_ESPDROP_AWDL_TX_LAB
static const char *TAG = "awdl_tx_lab";
static SemaphoreHandle_t state_lock;
static espdrop_awdl_tx_state_t tx_state;
static uint8_t station_mac[6];
static char device_name[ESPDROP_AWDL_TX_NAME_BYTES];
static bool has_state;
static bool task_started;
static uint32_t directed_reactions;
static uint32_t neighbor_advertisements;
static uint32_t echo_replies;
static volatile uint32_t action_radio_completed;
static volatile uint32_t action_radio_success;
static volatile uint32_t action_radio_failed;
static volatile uint32_t data_radio_completed;
static volatile uint32_t data_radio_success;
static volatile uint32_t data_radio_failed;
static volatile uint32_t ns_radio_completed;
static volatile uint32_t ns_radio_success;
static volatile uint32_t ns_radio_failed;
static volatile uint32_t echo_radio_completed;
static volatile uint32_t echo_radio_success;
static volatile uint32_t echo_radio_failed;
static volatile uint32_t unknown_data_radio_completed;

static void wait_until_us(uint64_t target_us)
{
    while ((uint64_t)esp_timer_get_time() < target_us) {
        const uint64_t remaining_us =
            target_us - (uint64_t)esp_timer_get_time();
        if (remaining_us > 3000U) {
            vTaskDelay(pdMS_TO_TICKS((remaining_us - 1500U) / 1000U));
        } else {
            esp_rom_delay_us((uint32_t)(remaining_us > 250U
                                            ? 250U : remaining_us));
        }
    }
}

static void lab_tx_done(
    uint8_t interface_index,
    uint8_t *data,
    uint16_t *data_length,
    bool success)
{
    (void)interface_index;
    const bool data_frame = data != NULL && (data[0] & 0x0cU) == 0x08U;
    volatile uint32_t *completed = data_frame
                                       ? &data_radio_completed
                                       : &action_radio_completed;
    volatile uint32_t *succeeded = data_frame
                                       ? &data_radio_success
                                       : &action_radio_success;
    volatile uint32_t *failed = data_frame
                                    ? &data_radio_failed
                                    : &action_radio_failed;
    ++*completed;
    if (success) {
        ++*succeeded;
    } else {
        ++*failed;
    }

    if (!data_frame) {
        return;
    }
    if (data_length == NULL ||
        *data_length <= AWDL_TX_LAB_AWDL_SEQUENCE_OFFSET + 1U) {
        ++unknown_data_radio_completed;
        return;
    }

    volatile uint32_t *typed_completed = NULL;
    volatile uint32_t *typed_succeeded = NULL;
    volatile uint32_t *typed_failed = NULL;
    const uint16_t awdl_sequence =
        (uint16_t)data[AWDL_TX_LAB_AWDL_SEQUENCE_OFFSET] |
        ((uint16_t)data[AWDL_TX_LAB_AWDL_SEQUENCE_OFFSET + 1U] << 8U);
    if ((awdl_sequence & AWDL_TX_LAB_ECHO_SEQUENCE_MARKER) != 0U) {
        typed_completed = &echo_radio_completed;
        typed_succeeded = &echo_radio_success;
        typed_failed = &echo_radio_failed;
    } else {
        typed_completed = &ns_radio_completed;
        typed_succeeded = &ns_radio_success;
        typed_failed = &ns_radio_failed;
    }

    ++*typed_completed;
    if (success) {
        ++*typed_succeeded;
    } else {
        ++*typed_failed;
    }
}

static void lab_tx_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(AWDL_TX_LAB_START_DELAY_MS));

    uint32_t attempted = 0U;
    uint32_t accepted = 0U;
    uint32_t errors = 0U;
    uint32_t data_attempted = 0U;
    uint32_t data_accepted = 0U;
    uint32_t data_errors = 0U;
    uint32_t echo_attempted = 0U;
    uint32_t echo_accepted = 0U;
    uint32_t echo_errors = 0U;
    uint16_t sequence = 0U;
    const int64_t started_us = esp_timer_get_time();
    const uint64_t deadline_us =
        (uint64_t)started_us + AWDL_TX_LAB_WINDOW_MS * 1000ULL;
    ESP_LOGW(TAG,
             "TX-LAB-START duration_ms=%u probe_limit=%u channel=6 "
             "scope=scheduled-AWDL-actions-neighbor-solicitation-and-echo",
             AWDL_TX_LAB_WINDOW_MS, AWDL_TX_LAB_PROBE_LIMIT);

    while ((uint64_t)esp_timer_get_time() < deadline_us &&
           data_attempted < AWDL_TX_LAB_PROBE_LIMIT) {
        espdrop_awdl_tx_state_t snapshot;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        snapshot = tx_state;
        xSemaphoreGive(state_lock);

        uint64_t scheduled_us = 0U;
        const uint64_t before_wait_us = (uint64_t)esp_timer_get_time();
        if (!espdrop_awdl_next_channel_window_us(
                &snapshot, 6U, before_wait_us,
                AWDL_TX_LAB_CHANNEL_WINDOW_GUARD_US, &scheduled_us)) {
            ESP_LOGE(TAG, "TX-LAB-SCHEDULE unavailable");
            ++errors;
            break;
        }
        if (scheduled_us >= deadline_us) {
            break;
        }
        wait_until_us(scheduled_us);
        const uint64_t actual_us = (uint64_t)esp_timer_get_time();
        ESP_LOGI(TAG,
                 "TX-LAB-WINDOW number=%lu scheduled=%llu actual=%llu "
                 "lateness_us=%llu",
                 (unsigned long)(data_attempted + 1U), scheduled_us,
                 actual_us, actual_us - scheduled_us);

        uint8_t frame[ESPDROP_AWDL_TX_FRAME_CAPACITY];
        size_t length = 0U;
        const espdrop_awdl_build_result_t built =
            espdrop_awdl_build_action(frame, sizeof(frame), &length,
                                      &snapshot, ESPDROP_AWDL_ACTION_MIF,
                                      actual_us,
                                      sequence++);
        ++attempted;
        if (built != ESPDROP_AWDL_BUILD_OK) {
            ++errors;
            ESP_LOGE(TAG, "TX-LAB-BUILD result=%d", built);
        } else {
            const esp_err_t result = esp_wifi_80211_tx(
                WIFI_IF_STA, frame, (int)length, false);
            if (result == ESP_OK) {
                ++accepted;
            } else {
                ++errors;
                ESP_LOGE(TAG, "TX-LAB-SEND result=%s", esp_err_to_name(result));
            }
            ESP_LOGI(TAG,
                     "TX-LAB-FRAME number=%lu subtype=%u bytes=%u "
                     "driver=%s",
                     (unsigned long)attempted,
                     (unsigned)ESPDROP_AWDL_ACTION_MIF,
                     (unsigned)length, esp_err_to_name(result));
        }

        vTaskDelay(pdMS_TO_TICKS(2U));
        uint8_t data_frame[ESPDROP_AWDL_NS_FRAME_BYTES];
        size_t data_length = 0U;
        ++data_attempted;
        if (!espdrop_awdl_build_neighbor_solicitation(
                data_frame, sizeof(data_frame), &data_length,
                snapshot.self, snapshot.master, sequence++,
                (uint16_t)data_attempted)) {
            ++data_errors;
        } else {
            const esp_err_t data_result = esp_wifi_80211_tx(
                WIFI_IF_STA, data_frame, (int)data_length, false);
            if (data_result == ESP_OK) {
                ++data_accepted;
            } else {
                ++data_errors;
            }
            ESP_LOGI(TAG,
                     "TX-LAB-NS number=%lu bytes=%u target="
                     "%02x:%02x:%02x:%02x:%02x:%02x driver=%s",
                     (unsigned long)data_attempted, (unsigned)data_length,
                     snapshot.master[0], snapshot.master[1],
                     snapshot.master[2], snapshot.master[3],
                     snapshot.master[4], snapshot.master[5],
                     esp_err_to_name(data_result));
        }

        vTaskDelay(pdMS_TO_TICKS(4U));
        uint8_t echo_frame[ESPDROP_AWDL_ECHO_FRAME_BYTES];
        size_t echo_length = 0U;
        ++echo_attempted;
        if (!espdrop_awdl_build_echo_request(
                echo_frame, sizeof(echo_frame), &echo_length,
                snapshot.self, snapshot.master, sequence++,
                (uint16_t)(echo_attempted +
                           AWDL_TX_LAB_ECHO_SEQUENCE_MARKER),
                AWDL_TX_LAB_ECHO_IDENTIFIER,
                (uint16_t)echo_attempted)) {
            ++echo_errors;
        } else {
            const esp_err_t echo_result = esp_wifi_80211_tx(
                WIFI_IF_STA, echo_frame, (int)echo_length, false);
            if (echo_result == ESP_OK) {
                ++echo_accepted;
            } else {
                ++echo_errors;
            }
            ESP_LOGI(TAG,
                     "TX-LAB-ECHO number=%lu bytes=%u target="
                     "%02x:%02x:%02x:%02x:%02x:%02x driver=%s",
                     (unsigned long)echo_attempted, (unsigned)echo_length,
                     snapshot.master[0], snapshot.master[1],
                     snapshot.master[2], snapshot.master[3],
                     snapshot.master[4], snapshot.master[5],
                     esp_err_to_name(echo_result));
        }

        vTaskDelay(pdMS_TO_TICKS(20U));
    }

    vTaskDelay(pdMS_TO_TICKS(100U));
    ESP_LOGW(TAG,
             "TX-LAB-SUMMARY action_attempted=%lu action_accepted=%lu "
             "action_errors=%lu action_radio_completed=%lu "
             "action_radio_success=%lu action_radio_failed=%lu "
             "data_attempted=%lu data_accepted=%lu data_errors=%lu "
             "data_radio_completed=%lu data_radio_success=%lu "
             "data_radio_failed=%lu ns_radio_completed=%lu "
             "ns_radio_success=%lu ns_radio_failed=%lu echo_attempted=%lu "
             "echo_accepted=%lu echo_errors=%lu directed_reactions=%lu "
             "echo_radio_completed=%lu echo_radio_success=%lu "
             "echo_radio_failed=%lu unknown_data_radio_completed=%lu "
             "neighbor_advertisements=%lu echo_replies=%lu",
             (unsigned long)attempted, (unsigned long)accepted,
             (unsigned long)errors,
             (unsigned long)action_radio_completed,
             (unsigned long)action_radio_success,
             (unsigned long)action_radio_failed,
             (unsigned long)data_attempted,
             (unsigned long)data_accepted,
             (unsigned long)data_errors,
             (unsigned long)data_radio_completed,
             (unsigned long)data_radio_success,
             (unsigned long)data_radio_failed,
             (unsigned long)ns_radio_completed,
             (unsigned long)ns_radio_success,
             (unsigned long)ns_radio_failed,
             (unsigned long)echo_attempted,
             (unsigned long)echo_accepted,
             (unsigned long)echo_errors,
             (unsigned long)directed_reactions,
             (unsigned long)echo_radio_completed,
             (unsigned long)echo_radio_success,
             (unsigned long)echo_radio_failed,
             (unsigned long)unknown_data_radio_completed,
             (unsigned long)neighbor_advertisements,
             (unsigned long)echo_replies);
    vTaskDelete(NULL);
}
#endif

esp_err_t espdrop_awdl_tx_lab_init(const char *name)
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    state_lock = xSemaphoreCreateMutex();
    if (state_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, station_mac), TAG,
                        "read station MAC");
    ESP_RETURN_ON_ERROR(esp_wifi_set_tx_done_cb(lab_tx_done), TAG,
                        "register transmit completion callback");
    (void)strncpy(device_name, name, sizeof(device_name) - 1U);
    ESP_LOGW(TAG,
             "lab transmit profile armed; waits for a valid MIF, then sends "
             "for at most 15 seconds");
#else
    (void)name;
#endif
    return ESP_OK;
}

void espdrop_awdl_tx_lab_observe_mif(
    const espdrop_awdl_action_t *action,
    const espdrop_awdl_mif_t *mif,
    uint64_t received_at_us)
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
    if (state_lock == NULL || action == NULL || mif == NULL) {
        return;
    }
    espdrop_awdl_tx_state_t candidate;
    if (!espdrop_awdl_tx_state_from_mif(
            &candidate, station_mac, action->source, device_name,
            action->phy_tx, mif,
            received_at_us)) {
        return;
    }

    bool start_task = false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (!has_state || candidate.distance_to_master <
                          tx_state.distance_to_master ||
        (candidate.distance_to_master == tx_state.distance_to_master &&
         memcmp(candidate.sync_master, tx_state.sync_master,
                sizeof(candidate.sync_master)) == 0)) {
        tx_state = candidate;
        has_state = true;
    }
    if (!task_started) {
        task_started = true;
        start_task = true;
    }
    xSemaphoreGive(state_lock);

    if (start_task && xTaskCreate(lab_tx_task, "awdl_tx_lab", 5120, NULL,
                                  6, NULL) != pdPASS) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        task_started = false;
        xSemaphoreGive(state_lock);
        ESP_LOGE(TAG, "could not create lab transmit task");
    }
#else
    (void)action;
    (void)mif;
    (void)received_at_us;
#endif
}

void espdrop_awdl_tx_lab_note_directed_peer(const uint8_t source[6])
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
    ++directed_reactions;
    ESP_LOGI(TAG,
             "TX-LAB-REACTION directed AWDL action from "
             "%02x:%02x:%02x:%02x:%02x:%02x count=%lu",
             source[0], source[1], source[2], source[3], source[4], source[5],
             (unsigned long)directed_reactions);
#else
    (void)source;
#endif
}

void espdrop_awdl_tx_lab_note_neighbor_advertisement(
    const uint8_t source[6])
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
    ++neighbor_advertisements;
    ESP_LOGI(TAG,
             "TX-LAB-NA IPv6 Neighbor Advertisement from "
             "%02x:%02x:%02x:%02x:%02x:%02x count=%lu",
             source[0], source[1], source[2], source[3], source[4], source[5],
             (unsigned long)neighbor_advertisements);
#else
    (void)source;
#endif
}

void espdrop_awdl_tx_lab_note_echo_reply(
    const uint8_t source[6],
    uint16_t identifier,
    uint16_t sequence)
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
    if (identifier != AWDL_TX_LAB_ECHO_IDENTIFIER) {
        return;
    }
    ++echo_replies;
    ESP_LOGI(TAG,
             "TX-LAB-ECHO-REPLY from "
             "%02x:%02x:%02x:%02x:%02x:%02x id=%u sequence=%u count=%lu",
             source[0], source[1], source[2], source[3], source[4], source[5],
             identifier, sequence, (unsigned long)echo_replies);
#else
    (void)source;
    (void)identifier;
    (void)sequence;
#endif
}
