#include "espdrop/awdl_tx_lab.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "espdrop/awdl_data.h"
#include "espdrop/awdl_election.h"
#include "espdrop/awdl_netif.h"
#include "espdrop/awdl_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef CONFIG_ESPDROP_AWDL_LAB_AUTO_TARGET_AIRDROP
#define CONFIG_ESPDROP_AWDL_LAB_AUTO_TARGET_AIRDROP 0
#endif
#if CONFIG_ESPDROP_AWDL_TX_LAB
#include "esp_private/wifi.h"
#endif

#if CONFIG_ESPDROP_AIRDROP_TLS_LAB
#define AWDL_TX_LAB_WINDOW_MS 25000U
#define AWDL_TX_LAB_PROBE_LIMIT 24U
#else
#define AWDL_TX_LAB_WINDOW_MS 15000U
#define AWDL_TX_LAB_PROBE_LIMIT 14U
#endif
#if CONFIG_ESPDROP_AWDL_MDNS_LAB && \
    !CONFIG_ESPDROP_AWDL_LAB_AUTO_TARGET_AIRDROP
#define AWDL_TX_LAB_START_DELAY_MS 8000U
#else
#define AWDL_TX_LAB_START_DELAY_MS 1500U
#endif
#define AWDL_TX_LAB_CHANNEL_WINDOW_GUARD_US 4000U
#define AWDL_TX_LAB_AWDL_SEQUENCE_OFFSET 34U
#define AWDL_TX_LAB_ELECTION_PEERS ESPDROP_AWDL_ELECTION_MAX_PEERS
#define AWDL_TX_LAB_FIXED_CHANNEL_PEER_TIMEOUT_US 5000000ULL

#if CONFIG_ESPDROP_AWDL_TX_LAB
static const char *TAG = "awdl_tx_lab";
static SemaphoreHandle_t state_lock;
static espdrop_awdl_tx_state_t tx_state;
static espdrop_awdl_tx_state_t target_state;
static espdrop_awdl_election_t election;
typedef struct {
    bool valid;
    uint8_t source[6];
    uint32_t peer_phy_tx;
    uint64_t received_at_us;
    espdrop_awdl_mif_t mif;
} awdl_lab_peer_mif_t;
static awdl_lab_peer_mif_t peer_mifs[AWDL_TX_LAB_ELECTION_PEERS];
static uint8_t station_mac[6];
static uint8_t target_mac[6];
static bool has_target_mac;
static uint8_t schedule_mac[6];
static bool has_schedule_mac;
static char device_name[ESPDROP_AWDL_TX_NAME_BYTES];
static bool has_state;
static bool has_target_state;
static bool task_started;
static volatile bool netif_ready;
static uint32_t directed_reactions;
static uint32_t neighbor_advertisements;
static uint32_t echo_replies;
static volatile uint32_t action_radio_completed;
static volatile uint32_t action_radio_success;
static volatile uint32_t action_radio_failed;
static volatile uint32_t data_radio_completed;
static volatile uint32_t data_radio_success;
static volatile uint32_t data_radio_failed;
static volatile uint32_t unknown_data_radio_completed;

static awdl_lab_peer_mif_t *find_peer_mif(const uint8_t source[6])
{
    for (size_t index = 0U; index < AWDL_TX_LAB_ELECTION_PEERS; ++index) {
        if (peer_mifs[index].valid &&
            memcmp(peer_mifs[index].source, source, 6U) == 0) {
            return &peer_mifs[index];
        }
    }
    return NULL;
}

static awdl_lab_peer_mif_t *store_peer_mif(
    const espdrop_awdl_action_t *action,
    const espdrop_awdl_mif_t *mif,
    uint64_t received_at_us)
{
    awdl_lab_peer_mif_t *entry = find_peer_mif(action->source);
    awdl_lab_peer_mif_t *oldest = &peer_mifs[0];
    if (entry == NULL) {
        for (size_t index = 0U; index < AWDL_TX_LAB_ELECTION_PEERS; ++index) {
            if (!peer_mifs[index].valid) {
                entry = &peer_mifs[index];
                break;
            }
            if (peer_mifs[index].received_at_us < oldest->received_at_us) {
                oldest = &peer_mifs[index];
            }
        }
    }
    if (entry == NULL) {
        entry = oldest;
    }
    entry->valid = true;
    memcpy(entry->source, action->source, sizeof(entry->source));
    entry->peer_phy_tx = action->phy_tx;
    entry->received_at_us = received_at_us;
    entry->mif = *mif;
    return entry;
}

static bool dynamic_election_candidate(
    const espdrop_awdl_action_t *action,
    const espdrop_awdl_mif_t *mif,
    uint64_t received_at_us,
    espdrop_awdl_tx_state_t *candidate)
{
#if CONFIG_ESPDROP_AWDL_LAB_DYNAMIC_ELECTION
    if (candidate == NULL) {
        return false;
    }
    if (!mif->has_election_v2 ||
        store_peer_mif(action, mif, received_at_us) == NULL) {
        return false;
    }
    bool changed = false;
    if (!espdrop_awdl_election_observe(
            &election, action->source, &mif->election_v2,
            received_at_us, &changed)) {
        return false;
    }
    const espdrop_awdl_election_state_t *elected =
        espdrop_awdl_election_state(&election);
    if (changed) {
        ESP_LOGW(TAG,
                 "TX-LAB-ELECTION self=%02x:%02x:%02x:%02x:%02x:%02x "
                 "sync=%02x:%02x:%02x:%02x:%02x:%02x "
                 "master=%02x:%02x:%02x:%02x:%02x:%02x "
                 "distance=%lu metric=%lu counter=%lu peers=%u",
                 elected->self[0], elected->self[1], elected->self[2],
                 elected->self[3], elected->self[4], elected->self[5],
                 elected->sync_master[0], elected->sync_master[1],
                 elected->sync_master[2], elected->sync_master[3],
                 elected->sync_master[4], elected->sync_master[5],
                 elected->master[0], elected->master[1], elected->master[2],
                 elected->master[3], elected->master[4], elected->master[5],
                 (unsigned long)elected->distance_to_master,
                 (unsigned long)elected->master_metric,
                 (unsigned long)elected->master_counter,
                 (unsigned)election.peer_count);
    }
    const awdl_lab_peer_mif_t *sync_mif =
        find_peer_mif(elected->sync_master);
    if (sync_mif == NULL ||
        !espdrop_awdl_tx_state_from_mif(
            candidate, station_mac, sync_mif->source, device_name,
            sync_mif->peer_phy_tx, &sync_mif->mif,
            sync_mif->received_at_us)) {
        return false;
    }
    if (!espdrop_awdl_tx_state_apply_election(candidate, elected)) {
        return false;
    }
    return true;
#else
    (void)action;
    (void)mif;
    (void)received_at_us;
    (void)candidate;
    return false;
#endif
}

static bool parse_mac(const char *text, uint8_t output[6])
{
    unsigned octets[6];
    char trailing;
    if (text == NULL || text[0] == '\0' ||
        sscanf(text, "%x:%x:%x:%x:%x:%x%c",
               &octets[0], &octets[1], &octets[2], &octets[3],
               &octets[4], &octets[5], &trailing) != 6) {
        return false;
    }
    for (size_t index = 0U; index < 6U; ++index) {
        if (octets[index] > 0xffU) {
            return false;
        }
        output[index] = (uint8_t)octets[index];
    }
    return true;
}

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

    const uint16_t awdl_sequence =
        (uint16_t)data[AWDL_TX_LAB_AWDL_SEQUENCE_OFFSET] |
        ((uint16_t)data[AWDL_TX_LAB_AWDL_SEQUENCE_OFFSET + 1U] << 8U);
    if ((awdl_sequence & ESPDROP_AWDL_NETIF_SEQUENCE_MARKER) != 0U) {
        espdrop_awdl_netif_note_tx_done(success, data, *data_length);
        return;
    }
    ++unknown_data_radio_completed;
}

static void lab_tx_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(AWDL_TX_LAB_START_DELAY_MS));

    uint32_t attempted = 0U;
    uint32_t accepted = 0U;
    uint32_t errors = 0U;
    uint32_t windows = 0U;
    uint16_t sequence = 0U;
    const int64_t started_us = esp_timer_get_time();
    const uint64_t deadline_us =
        (uint64_t)started_us + AWDL_TX_LAB_WINDOW_MS * 1000ULL;
    ESP_LOGW(TAG,
             "TX-LAB-START duration_ms=%u probe_limit=%u channel=6 scope=%s",
             AWDL_TX_LAB_WINDOW_MS, AWDL_TX_LAB_PROBE_LIMIT,
#if CONFIG_ESPDROP_AWDL_MDNS_LAB
             "owl-direct-peer-mif-and-scheduled-netif-frames"
#else
             "owl-direct-peer-mif"
#endif
    );

    while ((uint64_t)esp_timer_get_time() < deadline_us &&
           windows < AWDL_TX_LAB_PROBE_LIMIT) {
        espdrop_awdl_tx_state_t snapshot;
        espdrop_awdl_tx_state_t target_snapshot;
        uint8_t selected_target[6] = {0};
        bool target_snapshot_valid = false;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        snapshot = tx_state;
        if (has_target_mac && has_target_state) {
            target_snapshot = target_state;
            memcpy(selected_target, target_mac, sizeof(selected_target));
            target_snapshot_valid = true;
        } else {
            memcpy(selected_target, snapshot.sync_master,
                   sizeof(selected_target));
        }
        xSemaphoreGive(state_lock);

        uint64_t scheduled_us = 0U;
        const uint64_t before_wait_us = (uint64_t)esp_timer_get_time();
        const bool scheduled = target_snapshot_valid
            ? espdrop_awdl_next_common_channel_window_us(
                  &snapshot, &target_snapshot, 6U, before_wait_us,
                  AWDL_TX_LAB_CHANNEL_WINDOW_GUARD_US, &scheduled_us)
            : espdrop_awdl_next_channel_window_us(
                  &snapshot, 6U, before_wait_us,
                  AWDL_TX_LAB_CHANNEL_WINDOW_GUARD_US, &scheduled_us);
        if (!scheduled) {
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
                 "lateness_us=%llu copresence=%u target="
                 "%02x:%02x:%02x:%02x:%02x:%02x path=direct-peer",
                 (unsigned long)(windows + 1U), scheduled_us,
                 actual_us, actual_us - scheduled_us,
                 target_snapshot_valid ? 1U : 0U,
                 selected_target[0], selected_target[1],
                 selected_target[2], selected_target[3],
                 selected_target[4], selected_target[5]);
        ++windows;

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

        if (netif_ready) {
            vTaskDelay(pdMS_TO_TICKS(2U));
            const size_t netif_flushed = espdrop_awdl_netif_flush(2U);
            if (netif_flushed != 0U) {
                ESP_LOGI(TAG, "TX-LAB-NETIF flushed=%u",
                         (unsigned)netif_flushed);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }

    vTaskDelay(pdMS_TO_TICKS(100U));
    const espdrop_awdl_netif_stats_t netif = espdrop_awdl_netif_stats();
    ESP_LOGW(TAG,
             "TX-LAB-SUMMARY action_attempted=%lu action_accepted=%lu "
             "action_errors=%lu action_radio_completed=%lu "
             "action_radio_success=%lu action_radio_failed=%lu "
             "data_radio_completed=%lu data_radio_success=%lu "
             "data_radio_failed=%lu directed_reactions=%lu "
             "unknown_data_radio_completed=%lu neighbor_advertisements=%lu "
             "echo_replies=%lu peer_ready=%u "
             "netif_tx_observed=%lu netif_tx_enqueued=%lu "
             "netif_tx_submitted=%lu netif_tx_accepted=%lu "
             "netif_tx_radio_success=%lu netif_tx_radio_failed=%lu "
             "netif_rx_enqueued=%lu netif_rx_injected=%lu "
             "netif_rx_dropped=%lu mdns_queries=%lu mdns_packets=%lu "
             "mdns_responses=%lu mdns_services=%lu "
             "mdns_complete_services=%lu airdrop_tcp_attempts=%lu "
             "airdrop_tcp_connected=%lu airdrop_tls_attempts=%lu "
             "airdrop_tls_connected=%lu airdrop_discover_attempts=%lu "
             "airdrop_discover_responses=%lu "
             "airdrop_discover_accepted=%lu peer_mappings=%lu "
             "peer_mapping_failures=%lu tcp_tx_segments=%lu "
             "tcp_tx_syn=%lu tcp_tx_radio_success=%lu "
             "tcp_tx_radio_failed=%lu tcp_rx_segments=%lu "
             "tcp_rx_syn_ack=%lu tcp_rx_rst=%lu",
             (unsigned long)attempted, (unsigned long)accepted,
             (unsigned long)errors,
             (unsigned long)action_radio_completed,
             (unsigned long)action_radio_success,
             (unsigned long)action_radio_failed,
             (unsigned long)data_radio_completed,
             (unsigned long)data_radio_success,
             (unsigned long)data_radio_failed,
             (unsigned long)directed_reactions,
             (unsigned long)unknown_data_radio_completed,
             (unsigned long)neighbor_advertisements,
             (unsigned long)echo_replies,
             netif_ready ? 1U : 0U,
             (unsigned long)netif.tx_observed,
             (unsigned long)netif.tx_enqueued,
             (unsigned long)netif.tx_submitted,
             (unsigned long)netif.tx_accepted,
             (unsigned long)netif.tx_radio_success,
             (unsigned long)netif.tx_radio_failed,
             (unsigned long)netif.rx_enqueued,
             (unsigned long)netif.rx_injected,
             (unsigned long)netif.rx_dropped,
             (unsigned long)netif.mdns_queries,
             (unsigned long)netif.mdns_packets,
             (unsigned long)netif.mdns_responses,
             (unsigned long)netif.mdns_services,
             (unsigned long)netif.mdns_complete_services,
             (unsigned long)netif.airdrop_tcp_attempts,
             (unsigned long)netif.airdrop_tcp_connected,
             (unsigned long)netif.airdrop_tls_attempts,
             (unsigned long)netif.airdrop_tls_connected,
             (unsigned long)netif.airdrop_discover_attempts,
             (unsigned long)netif.airdrop_discover_responses,
             (unsigned long)netif.airdrop_discover_accepted,
             (unsigned long)netif.peer_mappings,
             (unsigned long)netif.peer_mapping_failures,
             (unsigned long)netif.tcp_tx_segments,
             (unsigned long)netif.tcp_tx_syn,
             (unsigned long)netif.tcp_tx_radio_success,
             (unsigned long)netif.tcp_tx_radio_failed,
             (unsigned long)netif.tcp_rx_segments,
             (unsigned long)netif.tcp_rx_syn_ack,
             (unsigned long)netif.tcp_rx_rst);
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
#if CONFIG_ESPDROP_AWDL_LAB_DYNAMIC_ELECTION
    if (!espdrop_awdl_election_init(&election, station_mac)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!espdrop_awdl_election_set_peer_timeout(
            &election, AWDL_TX_LAB_FIXED_CHANNEL_PEER_TIMEOUT_US)) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    has_target_mac = parse_mac(CONFIG_ESPDROP_AWDL_LAB_TARGET_MAC,
                               target_mac);
    has_schedule_mac = parse_mac(CONFIG_ESPDROP_AWDL_LAB_SCHEDULE_MAC,
                                 schedule_mac);
    if (!has_schedule_mac && has_target_mac) {
        memcpy(schedule_mac, target_mac, sizeof(schedule_mac));
        has_schedule_mac = true;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_tx_done_cb(lab_tx_done), TAG,
                        "register transmit completion callback");
    (void)strncpy(device_name, name, sizeof(device_name) - 1U);
    ESP_LOGW(TAG,
             "lab transmit profile armed; waits for a valid MIF, then sends "
             "for at most %u seconds",
             (unsigned)(AWDL_TX_LAB_WINDOW_MS / 1000U));
    if (has_target_mac) {
        ESP_LOGW(TAG,
                 "lab target=%02x:%02x:%02x:%02x:%02x:%02x",
                 target_mac[0], target_mac[1], target_mac[2], target_mac[3],
                 target_mac[4], target_mac[5]);
    }
    if (!has_target_mac &&
        CONFIG_ESPDROP_AWDL_LAB_AUTO_TARGET_AIRDROP) {
        ESP_LOGW(TAG,
                 "lab auto-target armed; waiting for a complete live "
                 "_airdrop._tcp MIF");
    }
    if (has_schedule_mac && !CONFIG_ESPDROP_AWDL_LAB_DYNAMIC_ELECTION) {
        ESP_LOGW(TAG,
                 "lab schedule-source=%02x:%02x:%02x:%02x:%02x:%02x",
                 schedule_mac[0], schedule_mac[1], schedule_mac[2],
                 schedule_mac[3], schedule_mac[4], schedule_mac[5]);
    }
#if CONFIG_ESPDROP_AWDL_LAB_DYNAMIC_ELECTION
    ESP_LOGW(TAG,
             "dynamic election enabled; tracking all fresh MIF peers "
             "with fixed-channel timeout=%llu us",
             AWDL_TX_LAB_FIXED_CHANNEL_PEER_TIMEOUT_US);
#endif
    ESP_LOGW(TAG,
             "OWL direct-peer mode; MIF version/device-class seeds the "
             "RFC4291 neighbor mapping without NDP admission");
#else
    (void)name;
#endif
    return ESP_OK;
}

void espdrop_awdl_tx_lab_observe_mif(
    const espdrop_awdl_action_t *action,
    const espdrop_awdl_mif_t *mif,
    bool advertises_airdrop_tcp,
    uint64_t received_at_us)
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
    if (state_lock == NULL || action == NULL || mif == NULL) {
        return;
    }
#if !CONFIG_ESPDROP_AWDL_LAB_DYNAMIC_ELECTION
    if (has_schedule_mac &&
        memcmp(action->source, schedule_mac, sizeof(schedule_mac)) != 0) {
        return;
    }
#endif
    espdrop_awdl_tx_state_t candidate;
    espdrop_awdl_tx_state_t observed_peer;
    const bool has_observed_peer = espdrop_awdl_tx_state_from_mif(
        &observed_peer, station_mac, action->source, device_name,
        action->phy_tx, mif, received_at_us);
#if CONFIG_ESPDROP_AWDL_LAB_DYNAMIC_ELECTION
    const bool has_candidate = dynamic_election_candidate(
        action, mif, received_at_us, &candidate);
#else
    const bool has_candidate = espdrop_awdl_tx_state_from_mif(
        &candidate, station_mac, action->source, device_name,
        action->phy_tx, mif, received_at_us);
#endif

    bool start_task = false;
    bool auto_targeted = false;
    bool peer_became_ready = false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
#if CONFIG_ESPDROP_AWDL_LAB_DYNAMIC_ELECTION
    if (has_candidate) {
        tx_state = candidate;
        has_state = true;
    }
#else
    if (has_candidate &&
        (!has_state || candidate.distance_to_master <
                           tx_state.distance_to_master ||
         (candidate.distance_to_master == tx_state.distance_to_master &&
          memcmp(candidate.sync_master, tx_state.sync_master,
                 sizeof(candidate.sync_master)) == 0))) {
        tx_state = candidate;
        has_state = true;
    }
#endif
    if (!has_target_mac &&
        CONFIG_ESPDROP_AWDL_LAB_AUTO_TARGET_AIRDROP &&
        advertises_airdrop_tcp && espdrop_awdl_mif_peer_valid(mif)) {
        memcpy(target_mac, action->source, sizeof(target_mac));
        has_target_mac = true;
        auto_targeted = true;
    }
    if (has_target_mac && has_observed_peer &&
        memcmp(action->source, target_mac, sizeof(target_mac)) == 0) {
        target_state = observed_peer;
        has_target_state = true;
        if (espdrop_awdl_mif_peer_valid(mif) && !netif_ready) {
            netif_ready = true;
            peer_became_ready = true;
        }
    }
    const bool target_is_live =
        !has_target_mac ||
        memcmp(action->source, target_mac, sizeof(target_mac)) == 0;
    if (!task_started && has_state && target_is_live && netif_ready &&
        (!CONFIG_ESPDROP_AWDL_LAB_AUTO_TARGET_AIRDROP || has_target_mac) &&
        (!has_target_mac || has_target_state)) {
        task_started = true;
        start_task = true;
    }
    xSemaphoreGive(state_lock);

    if (auto_targeted) {
        ESP_LOGW(TAG,
                 "TX-LAB-AUTO-TARGET peer="
                 "%02x:%02x:%02x:%02x:%02x:%02x service=_airdrop._tcp "
                 "distance=%lu",
                 action->source[0], action->source[1], action->source[2],
                 action->source[3], action->source[4], action->source[5],
                 (unsigned long)(mif->has_election_v2
                     ? mif->election_v2.distance_to_master : UINT32_MAX));
        if (!has_observed_peer) {
            ESP_LOGW(TAG,
                     "TX-LAB-TARGET-WAIT peer="
                     "%02x:%02x:%02x:%02x:%02x:%02x "
                     "reason=incomplete-synchronization-state",
                     action->source[0], action->source[1], action->source[2],
                     action->source[3], action->source[4], action->source[5]);
        }
    }

    if (peer_became_ready) {
        ESP_LOGW(TAG,
                 "TX-LAB-PEER-READY peer="
                 "%02x:%02x:%02x:%02x:%02x:%02x "
                 "evidence=mif-version-device-class ipv6=derived-rfc4291",
                 action->source[0], action->source[1], action->source[2],
                 action->source[3], action->source[4], action->source[5]);
    }

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
    (void)advertises_airdrop_tcp;
    (void)received_at_us;
#endif
}

bool espdrop_awdl_tx_lab_netif_ready(void)
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
    return netif_ready;
#else
    return false;
#endif
}

void espdrop_awdl_tx_lab_note_peer_seen(
    const uint8_t source[6],
    uint64_t received_at_us)
{
#if CONFIG_ESPDROP_AWDL_TX_LAB && \
    CONFIG_ESPDROP_AWDL_LAB_DYNAMIC_ELECTION
    (void)espdrop_awdl_election_touch(&election, source, received_at_us);
#else
    (void)source;
    (void)received_at_us;
#endif
}

bool espdrop_awdl_tx_lab_wants_mif(const uint8_t source[6])
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
#if CONFIG_ESPDROP_AWDL_LAB_DYNAMIC_ELECTION
    return source != NULL;
#else
    return source != NULL && has_schedule_mac &&
           memcmp(source, schedule_mac, sizeof(schedule_mac)) == 0;
#endif
#else
    (void)source;
    return false;
#endif
}

bool espdrop_awdl_tx_lab_target(uint8_t output[6])
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
    if (output == NULL || !has_target_mac) {
        return false;
    }
    memcpy(output, target_mac, sizeof(target_mac));
    return true;
#else
    (void)output;
    return false;
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
