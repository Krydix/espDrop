#include "maintenance_serial.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "improv_serial_codec.h"
#include "ota_update.h"
#include "relay_spool.h"
#include "relay_stream.h"
#include "espdrop/airdrop_outgoing.h"
#include "espdrop/awdl_netif.h"
#include "espdrop/awdl_tx_lab.h"
#include "espdrop/ble_wake.h"
#include "espdrop/espdrop.h"
#include "wifi_provision.h"

#define IMPROV_TYPE_CURRENT_STATE 0x01
#define IMPROV_TYPE_ERROR_STATE 0x02
#define IMPROV_TYPE_RPC_COMMAND 0x03
#define IMPROV_TYPE_RPC_RESULT 0x04

#define IMPROV_STATE_READY 0x02
#define IMPROV_STATE_PROVISIONING 0x03
#define IMPROV_STATE_PROVISIONED 0x04

#define IMPROV_ERROR_NONE 0x00
#define IMPROV_ERROR_INVALID_RPC 0x01
#define IMPROV_ERROR_UNKNOWN_RPC 0x02
#define IMPROV_ERROR_UNABLE_TO_CONNECT 0x03

#define IMPROV_RPC_WIFI_SETTINGS 0x01
#define IMPROV_RPC_CURRENT_STATE 0x02
#define IMPROV_RPC_DEVICE_INFO 0x03

#define PROVISION_TIMEOUT_MS 30000U

static const char *TAG = "espdrop_serial";
static const char ota_command[] = "ESPDROP OTA";
static const char relay_command[] = "ESPDROP RELAY BEGIN ";
static const char stream_begin_command[] = "ESPDROP STREAM BEGIN ";
static const char stream_data_command[] = "ESPDROP STREAM DATA ";
static bool allow_provisioning;
static SemaphoreHandle_t serial_write_lock;
static volatile bool application_ready;
typedef struct {
    uint8_t awdl_mac[6];
    int16_t awdl_rssi;
    uint32_t signals;
    bool peer_valid;
    bool advertises_airdrop;
    uint8_t device_class;
    uint32_t distance_to_master;
    bool endpoint_complete;
    uint16_t port;
    uint64_t last_seen_ms;
    uint64_t airdrop_seen_ms;
    char service_id[ESPDROP_SERVICE_ID_MAX_BYTES];
} serial_peer_record_t;
static serial_peer_record_t *serial_peer_snapshot;
/* Keep the framed USB payload outside maintenance_serial's task stack. The
 * parser already owns about 1 KiB of input plus command/provisioning state;
 * a 4 KiB local frame overflowed its 6 KiB stack on the first live chunk.
 * Both work areas are explicitly allocated from PSRAM because Wi-Fi, NimBLE,
 * lwIP, and esp-netif need the S3's scarce internal DRAM concurrently. */
static uint8_t *stream_chunk;

static esp_err_t allocate_serial_work_areas(void)
{
    if (serial_peer_snapshot == NULL) {
        serial_peer_snapshot = heap_caps_calloc(
            ESPDROP_MAX_PEERS, sizeof(*serial_peer_snapshot),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (serial_peer_snapshot == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (stream_chunk == NULL) {
        stream_chunk = heap_caps_malloc(
            RELAY_STREAM_CHUNK_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (stream_chunk == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t serial_write(const void *data, size_t length)
{
    if (data == NULL || length == 0U || serial_write_lock == NULL ||
        xSemaphoreTake(serial_write_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_FAIL;
    }
    const uint8_t *bytes = data;
    size_t offset = 0U;
    const TickType_t started = xTaskGetTickCount();
    while (offset < length &&
           xTaskGetTickCount() - started < pdMS_TO_TICKS(3000)) {
        const int written = usb_serial_jtag_write_bytes(
            bytes + offset, length - offset, pdMS_TO_TICKS(250));
        if (written > 0) {
            offset += (size_t)written;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    xSemaphoreGive(serial_write_lock);
    return offset == length ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_packet(uint8_t type, const uint8_t *data, size_t length)
{
    uint8_t packet[IMPROV_SERIAL_MAX_PACKET_SIZE];
    const size_t packet_length = improv_serial_encode_packet(
        type, data, length, packet, sizeof(packet));
    if (packet_length == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    return serial_write(packet, packet_length);
}

static void send_state(uint8_t state)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        write_packet(IMPROV_TYPE_CURRENT_STATE, &state, 1));
}

static void send_error(uint8_t error)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        write_packet(IMPROV_TYPE_ERROR_STATE, &error, 1));
}

static esp_err_t send_rpc_result(uint8_t command, const char *const *strings,
                                 size_t string_count)
{
    uint8_t data[IMPROV_SERIAL_MAX_DATA];
    size_t position = 2U;
    data[0] = command;
    for (size_t index = 0; index < string_count; ++index) {
        const size_t length = strings[index] != NULL
                                  ? strlen(strings[index]) : 0U;
        if (length > UINT8_MAX || position + 1U + length > sizeof(data)) {
            return ESP_ERR_INVALID_SIZE;
        }
        data[position++] = (uint8_t)length;
        if (length > 0U) {
            memcpy(&data[position], strings[index], length);
            position += length;
        }
    }
    data[1] = (uint8_t)(position - 2U);
    return write_packet(IMPROV_TYPE_RPC_RESULT, data, position);
}

static bool decode_wifi_settings(const uint8_t *data, size_t length,
                                 char *ssid, size_t ssid_capacity,
                                 char *password, size_t password_capacity)
{
    if (data == NULL || length < 2U) {
        return false;
    }
    size_t position = 0U;
    const size_t ssid_length = data[position++];
    if (ssid_length == 0U || ssid_length >= ssid_capacity ||
        position + ssid_length + 1U > length) {
        return false;
    }
    memcpy(ssid, &data[position], ssid_length);
    ssid[ssid_length] = '\0';
    position += ssid_length;
    const size_t password_length = data[position++];
    if (password_length >= password_capacity ||
        position + password_length != length) {
        return false;
    }
    memcpy(password, &data[position], password_length);
    password[password_length] = '\0';
    return true;
}

static void handle_wifi_settings(const uint8_t *data, size_t length)
{
    if (!allow_provisioning) {
        send_error(IMPROV_ERROR_UNKNOWN_RPC);
        return;
    }
    char ssid[33];
    char password[64];
    if (!decode_wifi_settings(data, length, ssid, sizeof(ssid), password,
                              sizeof(password))) {
        send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }
    send_state(IMPROV_STATE_PROVISIONING);
    esp_err_t result = wifi_provision_connect(ssid, password);
    memset(password, 0, sizeof(password));
    if (result == ESP_OK) {
        result = wifi_provision_wait_connected(PROVISION_TIMEOUT_MS);
    }
    if (result == ESP_OK) {
        result = wifi_provision_mark_configured();
    }
    if (result != ESP_OK) {
        send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
        send_state(IMPROV_STATE_READY);
        ESP_LOGE(TAG, "Wi-Fi provisioning failed: %s", esp_err_to_name(result));
        return;
    }

    send_error(IMPROV_ERROR_NONE);
    send_state(IMPROV_STATE_PROVISIONED);
    const char *urls[] = {"https://krydix.github.io/espDrop/"};
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        send_rpc_result(IMPROV_RPC_WIFI_SETTINGS, urls, 1));
    ESP_LOGI(TAG, "maintenance Wi-Fi provisioned; restarting into AWDL mode");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void handle_rpc(const improv_serial_packet_t *packet)
{
    if (packet->type != IMPROV_TYPE_RPC_COMMAND || packet->length < 2U ||
        packet->data[1] != packet->length - 2U) {
        send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }
    const uint8_t command = packet->data[0];
    const uint8_t *data = &packet->data[2];
    const size_t length = packet->data[1];
    send_error(IMPROV_ERROR_NONE);
    if (command == IMPROV_RPC_WIFI_SETTINGS) {
        handle_wifi_settings(data, length);
    } else if (command == IMPROV_RPC_CURRENT_STATE && length == 0U) {
        bool configured = false;
        if (wifi_provision_is_configured(&configured) != ESP_OK) {
            send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
            return;
        }
        send_state(configured ? IMPROV_STATE_PROVISIONED : IMPROV_STATE_READY);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            send_rpc_result(command, NULL, 0));
    } else if (command == IMPROV_RPC_DEVICE_INFO && length == 0U) {
        const esp_app_desc_t *description = esp_app_get_description();
        const char *strings[] = {
            "espDrop",
            description->version,
            CONFIG_IDF_TARGET "/" CONFIG_IDF_TARGET,
            "espDrop maintenance",
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_rpc_result(command, strings, 4));
    } else {
        send_error(IMPROV_ERROR_UNKNOWN_RPC);
    }
}

static void arm_ota_and_restart(const char *url)
{
    const esp_err_t result = url != NULL
                                 ? ota_update_request_url(url)
                                 : ota_update_request_github();
    const char *response = result == ESP_OK
                               ? "ESPDROP-OTA-ARMED\n"
                               : result == ESP_ERR_INVALID_STATE
                                     ? "ESPDROP-OTA-NOT-PROVISIONED\n"
                                     : "ESPDROP-OTA-INVALID-URL\n";
    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(response, strlen(response)));
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool decode_hex_name(const char *encoded, char *decoded,
                            size_t capacity)
{
    const size_t length = encoded != NULL ? strlen(encoded) : 0U;
    if (length == 0U || (length & 1U) != 0U || length / 2U >= capacity) {
        return false;
    }
    for (size_t index = 0U; index < length; index += 2U) {
        const int high = hex_nibble(encoded[index]);
        const int low = hex_nibble(encoded[index + 1U]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) {
            return false;
        }
        decoded[index / 2U] = (char)((high << 4) | low);
    }
    decoded[length / 2U] = '\0';
    return true;
}

static bool valid_file_type(const char *file_type)
{
    if (file_type == NULL || file_type[0] == '\0' ||
        strlen(file_type) > RELAY_SPOOL_FILE_TYPE_MAX) {
        return false;
    }
    for (const char *cursor = file_type; *cursor != '\0'; ++cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '.' ||
              *cursor == '-')) {
            return false;
        }
    }
    return true;
}

static bool parse_mac(const char *text, uint8_t output[6])
{
    if (text == NULL || output == NULL) {
        return false;
    }
    uint8_t parsed[6] = {0};
    size_t nibble = 0U;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == ':' || *cursor == '-') {
            continue;
        }
        const int value = hex_nibble(*cursor);
        if (value < 0 || nibble >= 12U) {
            return false;
        }
        if ((nibble & 1U) == 0U) {
            parsed[nibble / 2U] = (uint8_t)(value << 4);
        } else {
            parsed[nibble / 2U] |= (uint8_t)value;
        }
        ++nibble;
    }
    static const uint8_t zero[6];
    if (nibble != 12U || memcmp(parsed, zero, sizeof(zero)) == 0) {
        return false;
    }
    memcpy(output, parsed, sizeof(parsed));
    return true;
}

static const char *target_mode_name(espdrop_awdl_target_mode_t mode)
{
    switch (mode) {
    case ESPDROP_AWDL_TARGET_AUTO:
        return "auto";
    case ESPDROP_AWDL_TARGET_MANUAL:
        return "manual";
    default:
        return "none";
    }
}

static void send_target_status(void)
{
    uint8_t target[6] = {0};
    const bool selected = espdrop_awdl_tx_lab_target(target);
    char response[128];
    const int length = selected
        ? snprintf(response, sizeof(response),
                   "ESPDROP-TARGET mode=%s peer="
                   "%02x:%02x:%02x:%02x:%02x:%02x\n",
                   target_mode_name(espdrop_awdl_tx_lab_target_mode()),
                   target[0], target[1], target[2], target[3], target[4],
                   target[5])
        : snprintf(response, sizeof(response),
                   "ESPDROP-TARGET mode=%s peer=-\n",
                   target_mode_name(espdrop_awdl_tx_lab_target_mode()));
    if (length > 0 && (size_t)length < sizeof(response)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            serial_write(response, (size_t)length));
    }
}

static bool handle_target_command(const char *line)
{
    static const char prefix[] = "ESPDROP TARGET ";
    if (strncmp(line, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    const char *argument = line + sizeof(prefix) - 1U;
    esp_err_t result;
    if (strcmp(argument, "STATUS") == 0) {
        send_target_status();
        return true;
    }
    if (strcmp(argument, "AUTO") == 0) {
        result = espdrop_awdl_tx_lab_set_target_mode(
            ESPDROP_AWDL_TARGET_AUTO);
    } else if (strcmp(argument, "NONE") == 0) {
        result = espdrop_awdl_tx_lab_set_target_mode(
            ESPDROP_AWDL_TARGET_NONE);
    } else {
        uint8_t target[6];
        if (!parse_mac(argument, target)) {
            result = ESP_ERR_INVALID_ARG;
        } else {
            result = espdrop_awdl_tx_lab_set_target(target);
        }
    }
    if (result != ESP_OK) {
        static const char invalid[] = "ESPDROP-TARGET-INVALID\n";
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            serial_write(invalid, sizeof(invalid) - 1U));
    } else {
        send_target_status();
    }
    return true;
}

static bool peer_is_visible_awdl_candidate(const espdrop_peer_t *peer)
{
    return peer != NULL && peer->awdl.mif_seen;
}

static void send_peer_list(void)
{
    espdrop_peer_table_t *table = espdrop_peers();
    if (table == NULL || !espdrop_lock_peers()) {
        static const char unavailable[] =
            "ESPDROP-PEERS-BEGIN count=0\nESPDROP-PEERS-END\n";
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            serial_write(unavailable, sizeof(unavailable) - 1U));
        return;
    }
    size_t count = 0U;
    for (size_t index = 0U; index < table->count; ++index) {
        if (peer_is_visible_awdl_candidate(&table->peers[index])) {
            const espdrop_peer_t *peer = &table->peers[index];
            serial_peer_record_t *record = &serial_peer_snapshot[count++];
            memcpy(record->awdl_mac, peer->awdl_mac,
                   sizeof(record->awdl_mac));
            record->awdl_rssi = peer->awdl_rssi;
            record->signals = peer->signals;
            record->peer_valid = peer->awdl.peer_valid;
            record->advertises_airdrop = peer->awdl.advertises_airdrop;
            record->device_class = peer->awdl.device_class;
            record->distance_to_master = peer->awdl.distance_to_master;
            record->endpoint_complete = peer->airdrop_endpoint_complete;
            record->port = peer->airdrop_port;
            record->last_seen_ms = peer->last_seen_ms;
            record->airdrop_seen_ms = peer->airdrop_seen_ms;
            memcpy(record->service_id, peer->service_id,
                   sizeof(record->service_id));
            record->service_id[sizeof(record->service_id) - 1U] = '\0';
        }
    }
    espdrop_unlock_peers();
    char response[768];
    int length = snprintf(response, sizeof(response),
                          "ESPDROP-PEERS-BEGIN count=%u\n",
                          (unsigned)count);
    if (length > 0 && (size_t)length < sizeof(response)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            serial_write(response, (size_t)length));
    }
    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    for (size_t index = 0U; index < count; ++index) {
        const serial_peer_record_t *peer = &serial_peer_snapshot[index];
        const uint64_t age_ms = peer->last_seen_ms <= now_ms
            ? now_ms - peer->last_seen_ms : 0U;
        const uint64_t airdrop_age_ms =
            peer->airdrop_seen_ms != 0U && peer->airdrop_seen_ms <= now_ms
                ? now_ms - peer->airdrop_seen_ms
                : UINT64_MAX;
        const char *instance = peer->service_id[0] != '\0'
            ? peer->service_id : "-";
        length = snprintf(
            response, sizeof(response),
            "ESPDROP-PEER mac=%02x:%02x:%02x:%02x:%02x:%02x "
            "rssi=%d signals=%lu valid=%u airdrop=%u class=%u distance=%lu "
            "endpoint=%u port=%u age_ms=%llu airdrop_age_ms=%llu "
            "instance=%s\n",
            peer->awdl_mac[0], peer->awdl_mac[1], peer->awdl_mac[2],
            peer->awdl_mac[3], peer->awdl_mac[4], peer->awdl_mac[5],
            peer->awdl_rssi, (unsigned long)peer->signals,
            peer->peer_valid ? 1U : 0U,
            peer->advertises_airdrop ? 1U : 0U,
            peer->device_class, (unsigned long)peer->distance_to_master,
            peer->endpoint_complete ? 1U : 0U,
            peer->port, (unsigned long long)age_ms,
            (unsigned long long)airdrop_age_ms, instance);
        if (length > 0 && (size_t)length < sizeof(response)) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                serial_write(response, (size_t)length));
        }
    }
    static const char end[] = "ESPDROP-PEERS-END\n";
    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(end, sizeof(end) - 1U));
}

static bool start_relay_upload(char *line)
{
    const size_t prefix_length = sizeof(relay_command) - 1U;
    if (strncmp(line, relay_command, prefix_length) != 0) {
        return false;
    }
    char *save = NULL;
    char *size_text = strtok_r(line + prefix_length, " ", &save);
    char *crc_text = strtok_r(NULL, " ", &save);
    char *name_hex = strtok_r(NULL, " ", &save);
    char *file_type = strtok_r(NULL, " ", &save);
    char *extra = strtok_r(NULL, " ", &save);
    char *size_end = NULL;
    char *crc_end = NULL;
    const unsigned long size_value =
        size_text != NULL ? strtoul(size_text, &size_end, 10) : 0UL;
    const unsigned long crc_value =
        crc_text != NULL ? strtoul(crc_text, &crc_end, 16) : 0UL;
    char file_name[RELAY_SPOOL_FILE_NAME_MAX + 1U];
    if (size_text == NULL || crc_text == NULL || name_hex == NULL ||
        file_type == NULL || extra != NULL || *size_end != '\0' ||
        *crc_end != '\0' || size_value == 0UL ||
        size_value > relay_spool_capacity() || crc_value > UINT32_MAX ||
        !decode_hex_name(name_hex, file_name, sizeof(file_name)) ||
        !valid_file_type(file_type)) {
        static const char invalid[] = "ESPDROP-RELAY-INVALID\n";
        ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(invalid, sizeof(invalid) - 1U));
        return false;
    }
    if (espdrop_airdrop_outgoing_clear() != ESP_OK) {
        static const char busy[] = "ESPDROP-RELAY-BUSY\n";
        ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(busy, sizeof(busy) - 1U));
        return false;
    }
    const esp_err_t result = relay_spool_begin(
        (size_t)size_value, (uint32_t)crc_value, file_name, file_type);
    if (result != ESP_OK) {
        static const char failed[] = "ESPDROP-RELAY-FAILED\n";
        ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(failed, sizeof(failed) - 1U));
        ESP_LOGE(TAG, "cannot begin relay upload: %s", esp_err_to_name(result));
        return false;
    }
    char response[80];
    const int length = snprintf(response, sizeof(response),
                                "ESPDROP-RELAY-READY capacity=%u\n",
                                (unsigned)relay_spool_capacity());
    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(response, (size_t)length));
    return true;
}

static void finish_relay_upload(void)
{
    esp_err_t result = relay_spool_finish();
    espdrop_airdrop_outgoing_file_t outgoing;
    if (result == ESP_OK) {
        result = relay_spool_outgoing_file(&outgoing);
    }
    if (result == ESP_OK) {
        result = espdrop_airdrop_outgoing_set(&outgoing);
    }
    if (result == ESP_OK) {
        result = espdrop_awdl_tx_lab_request_run();
    }
#if CONFIG_ESPDROP_BLE_WAKE_LAB
    if (result == ESP_OK) {
        result = espdrop_ble_wake_start(
            CONFIG_ESPDROP_BLE_WAKE_DURATION_MS);
    }
#endif
    const char *response = result == ESP_OK
                               ? "ESPDROP-RELAY-STORED\n"
                               : result == ESP_ERR_INVALID_CRC
                                     ? "ESPDROP-RELAY-CRC-ERROR\n"
                                     : "ESPDROP-RELAY-FAILED\n";
    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(response, strlen(response)));
    if (result != ESP_OK) {
        relay_spool_abort();
        ESP_LOGE(TAG, "relay upload failed: %s", esp_err_to_name(result));
    }
}

static bool start_stream_upload(char *line)
{
    const size_t prefix_length = sizeof(stream_begin_command) - 1U;
    if (strncmp(line, stream_begin_command, prefix_length) != 0) {
        return false;
    }
    char *save = NULL;
    char *file_size_text = strtok_r(line + prefix_length, " ", &save);
    char *file_crc_text = strtok_r(NULL, " ", &save);
    char *payload_size_text = strtok_r(NULL, " ", &save);
    char *payload_crc_text = strtok_r(NULL, " ", &save);
    char *archive_size_text = strtok_r(NULL, " ", &save);
    char *block_count_text = strtok_r(NULL, " ", &save);
    char *name_hex = strtok_r(NULL, " ", &save);
    char *file_type = strtok_r(NULL, " ", &save);
    char *extra = strtok_r(NULL, " ", &save);
    char *ends[6] = {NULL};
    const unsigned long file_size = file_size_text != NULL
                                        ? strtoul(file_size_text, &ends[0], 10)
                                        : 0UL;
    const unsigned long file_crc = file_crc_text != NULL
                                       ? strtoul(file_crc_text, &ends[1], 16)
                                       : 0UL;
    const unsigned long payload_size = payload_size_text != NULL
                                           ? strtoul(payload_size_text, &ends[2], 10)
                                           : 0UL;
    const unsigned long payload_crc = payload_crc_text != NULL
                                          ? strtoul(payload_crc_text, &ends[3], 16)
                                          : 0UL;
    const unsigned long archive_size = archive_size_text != NULL
                                           ? strtoul(archive_size_text, &ends[4], 10)
                                           : 0UL;
    const unsigned long block_count = block_count_text != NULL
                                          ? strtoul(block_count_text, &ends[5], 10)
                                          : 0UL;
    char file_name[RELAY_SPOOL_FILE_NAME_MAX + 1U];
    bool parsed_numbers = file_size_text != NULL && file_crc_text != NULL &&
                          payload_size_text != NULL &&
                          payload_crc_text != NULL &&
                          archive_size_text != NULL &&
                          block_count_text != NULL;
    for (size_t index = 0U; parsed_numbers && index < 6U; ++index) {
        parsed_numbers = ends[index] != NULL && *ends[index] == '\0';
    }
    const bool shape_valid =
        archive_size > 0UL && block_count > 0UL &&
        block_count <= ULONG_MAX / 4UL &&
        archive_size <= ULONG_MAX - block_count * 4UL &&
        archive_size <=
            ULONG_MAX - (ESPDROP_AIRDROP_DVZIP_STREAM_BLOCK_BYTES - 1UL) &&
        payload_size == archive_size + block_count * 4UL &&
        block_count ==
            (archive_size + ESPDROP_AIRDROP_DVZIP_STREAM_BLOCK_BYTES - 1UL) /
                ESPDROP_AIRDROP_DVZIP_STREAM_BLOCK_BYTES;
    if (!parsed_numbers || name_hex == NULL || file_type == NULL ||
        extra != NULL || file_size == 0UL ||
        file_size > CONFIG_ESPDROP_MAX_TRANSFER_BYTES ||
        file_crc > UINT32_MAX || payload_size == 0UL ||
        payload_crc > UINT32_MAX || !shape_valid ||
        !decode_hex_name(name_hex, file_name, sizeof(file_name)) ||
        !valid_file_type(file_type)) {
        static const char invalid[] = "ESPDROP-STREAM-INVALID\n";
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            serial_write(invalid, sizeof(invalid) - 1U));
        return false;
    }
    if (espdrop_airdrop_outgoing_clear() != ESP_OK) {
        static const char busy[] = "ESPDROP-STREAM-BUSY\n";
        ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(busy, sizeof(busy) - 1U));
        return false;
    }
    espdrop_airdrop_outgoing_file_t outgoing;
    esp_err_t result = relay_stream_begin(
        (size_t)file_size, (uint32_t)file_crc, (size_t)payload_size,
        (uint32_t)payload_crc, (size_t)archive_size, (size_t)block_count,
        file_name, file_type, &outgoing);
    if (result == ESP_OK) {
        result = espdrop_airdrop_outgoing_set(&outgoing);
    }
    if (result == ESP_OK) {
        result = espdrop_awdl_tx_lab_request_run();
    }
#if CONFIG_ESPDROP_BLE_WAKE_LAB
    if (result == ESP_OK) {
        result = espdrop_ble_wake_start(
            CONFIG_ESPDROP_BLE_WAKE_DURATION_MS);
    }
#endif
    if (result != ESP_OK) {
        relay_stream_abort();
        static const char failed[] = "ESPDROP-STREAM-FAILED\n";
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            serial_write(failed, sizeof(failed) - 1U));
        ESP_LOGE(TAG, "cannot arm relay stream: %s", esp_err_to_name(result));
        return false;
    }
    char response[128];
    const int length = snprintf(
        response, sizeof(response),
        "ESPDROP-STREAM-ARMED file_bytes=%lu payload_bytes=%lu chunk=%u\n",
        file_size, payload_size, (unsigned)RELAY_STREAM_CHUNK_MAX);
    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(response, (size_t)length));
    return true;
}

static void cancel_stream_upload(void)
{
    relay_stream_abort();
    const esp_err_t result = espdrop_airdrop_outgoing_clear();
    const char *response = result == ESP_OK
        ? "ESPDROP-STREAM-CANCELLED\n"
        : "ESPDROP-STREAM-CANCEL-PENDING\n";
    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(response, strlen(response)));
}

static void send_transport_stats(void)
{
    const espdrop_awdl_netif_stats_t value = espdrop_awdl_netif_stats();
    char response[768];
    const int length = snprintf(
        response, sizeof(response),
        "ESPDROP-STATS rx_enqueued=%lu rx_injected=%lu rx_dropped=%lu "
        "rx_filtered=%lu tx_enqueued=%lu tx_dropped=%lu "
        "tx_bad_size=%lu tx_q_full=%lu tx_drop_len=%lu tx_q_high=%lu "
        "tx_submitted=%lu tx_redundant=%lu "
        "tx_radio_success=%lu tx_radio_failed=%lu "
        "tcp_tx=%lu tcp_tx_bytes=%lu tcp_tx_seq=%lu tcp_tx_ack=%lu "
        "tcp_tx_win=%u tcp_tx_len=%u "
        "tcp_rx=%lu tcp_rx_bytes=%lu tcp_rx_seq=%lu tcp_rx_ack=%lu "
        "tcp_rx_win=%u tcp_rx_len=%u tcp_syn_ack=%lu tcp_rst=%lu "
        "tcp_fin=%lu tcp_zero_win=%lu "
        "tcp_connected=%lu tls_connected=%lu "
        "ask_accepted=%lu upload_accepted=%lu\n",
        (unsigned long)value.rx_enqueued,
        (unsigned long)value.rx_injected,
        (unsigned long)value.rx_dropped,
        (unsigned long)value.rx_filtered,
        (unsigned long)value.tx_enqueued,
        (unsigned long)value.tx_dropped,
        (unsigned long)value.tx_invalid_size,
        (unsigned long)value.tx_queue_full,
        (unsigned long)value.tx_last_dropped_length,
        (unsigned long)value.tx_queue_high_water,
        (unsigned long)value.tx_submitted,
        (unsigned long)value.tx_redundant_submitted,
        (unsigned long)value.tx_radio_success,
        (unsigned long)value.tx_radio_failed,
        (unsigned long)value.tcp_tx_segments,
        (unsigned long)value.tcp_tx_payload_bytes,
        (unsigned long)value.tcp_tx_last_sequence,
        (unsigned long)value.tcp_tx_last_acknowledgment,
        (unsigned)value.tcp_tx_last_window,
        (unsigned)value.tcp_tx_last_payload_length,
        (unsigned long)value.tcp_rx_segments,
        (unsigned long)value.tcp_rx_payload_bytes,
        (unsigned long)value.tcp_rx_last_sequence,
        (unsigned long)value.tcp_rx_last_acknowledgment,
        (unsigned)value.tcp_rx_last_window,
        (unsigned)value.tcp_rx_last_payload_length,
        (unsigned long)value.tcp_rx_syn_ack,
        (unsigned long)value.tcp_rx_rst,
        (unsigned long)value.tcp_rx_fin,
        (unsigned long)value.tcp_rx_zero_window,
        (unsigned long)value.airdrop_tcp_connected,
        (unsigned long)value.airdrop_tls_connected,
        (unsigned long)value.airdrop_ask_accepted,
        (unsigned long)value.airdrop_upload_accepted);
    if (length > 0 && (size_t)length < sizeof(response)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            serial_write(response, (size_t)length));
    }
}

static void send_stream_result(void)
{
    const espdrop_airdrop_outgoing_result_t result =
        espdrop_airdrop_outgoing_result();
    const char *state_name = "none";
    switch (result.state) {
    case ESPDROP_AIRDROP_OUTGOING_RESULT_PENDING:
        state_name = "pending";
        break;
    case ESPDROP_AIRDROP_OUTGOING_RESULT_SUCCESS:
        state_name = "success";
        break;
    case ESPDROP_AIRDROP_OUTGOING_RESULT_FAILED:
        state_name = "failed";
        break;
    case ESPDROP_AIRDROP_OUTGOING_RESULT_NONE:
    default:
        break;
    }
    const char *stage_name = "none";
    switch (result.stage) {
    case ESPDROP_AIRDROP_OUTGOING_STAGE_TLS:
        stage_name = "tls";
        break;
    case ESPDROP_AIRDROP_OUTGOING_STAGE_ASK:
        stage_name = "ask";
        break;
    case ESPDROP_AIRDROP_OUTGOING_STAGE_UPLOAD:
        stage_name = "upload";
        break;
    case ESPDROP_AIRDROP_OUTGOING_STAGE_NONE:
    default:
        break;
    }
    char response[224];
    const int length = snprintf(
        response, sizeof(response),
        "ESPDROP-STREAM-RESULT state=%s stage=%s error=%d status=%u "
        "request_bytes=%u payload_bytes=%u\n",
        state_name, stage_name, result.error, result.http_status,
        (unsigned)result.request_bytes, (unsigned)result.payload_bytes);
    if (length > 0 && (size_t)length < sizeof(response)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            serial_write(response, (size_t)length));
    }
}

static bool start_stream_chunk(char *line, uint32_t *sequence,
                               size_t *data_bytes, uint32_t *crc32)
{
    const size_t prefix_length = sizeof(stream_data_command) - 1U;
    if (strncmp(line, stream_data_command, prefix_length) != 0) {
        return false;
    }
    char *save = NULL;
    char *sequence_text = strtok_r(line + prefix_length, " ", &save);
    char *size_text = strtok_r(NULL, " ", &save);
    char *crc_text = strtok_r(NULL, " ", &save);
    char *extra = strtok_r(NULL, " ", &save);
    char *sequence_end = NULL;
    char *size_end = NULL;
    char *crc_end = NULL;
    const unsigned long sequence_value = sequence_text != NULL
                                             ? strtoul(sequence_text, &sequence_end, 10)
                                             : ULONG_MAX;
    const unsigned long size_value = size_text != NULL
                                         ? strtoul(size_text, &size_end, 10)
                                         : 0UL;
    const unsigned long crc_value = crc_text != NULL
                                        ? strtoul(crc_text, &crc_end, 16)
                                        : ULONG_MAX;
    if (sequence_text == NULL || size_text == NULL || crc_text == NULL ||
        extra != NULL || *sequence_end != '\0' || *size_end != '\0' ||
        *crc_end != '\0' || sequence_value > UINT32_MAX ||
        crc_value > UINT32_MAX ||
        relay_stream_validate_chunk((uint32_t)sequence_value,
                                    (size_t)size_value) != ESP_OK) {
        static const char invalid[] = "ESPDROP-STREAM-DATA-INVALID\n";
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            serial_write(invalid, sizeof(invalid) - 1U));
        return false;
    }
    *sequence = (uint32_t)sequence_value;
    *data_bytes = (size_t)size_value;
    *crc32 = (uint32_t)crc_value;
    char response[80];
    const int length = snprintf(response, sizeof(response),
                                "ESPDROP-STREAM-DATA-READY seq=%lu\n",
                                sequence_value);
    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(response, (size_t)length));
    return true;
}

static void serial_task(void *context)
{
    (void)context;
    improv_serial_parser_t parser = {0};
    improv_serial_packet_t packet;
    char command_line[512];
    size_t command_length = 0U;
    bool relay_receiving = false;
    size_t relay_remaining = 0U;
    TickType_t relay_last_receive = 0U;
    bool stream_chunk_receiving = false;
    size_t stream_chunk_bytes = 0U;
    size_t stream_chunk_received = 0U;
    uint32_t stream_chunk_sequence = 0U;
    uint32_t stream_chunk_crc32 = 0U;
    TickType_t stream_last_receive = 0U;
    uint8_t input[1024];
    while (true) {
        const int received = usb_serial_jtag_read_bytes(
            input, sizeof(input), pdMS_TO_TICKS(100));
        int index = 0;
        while (index < received) {
            if (stream_chunk_receiving) {
                const size_t available = (size_t)(received - index);
                const size_t remaining =
                    stream_chunk_bytes - stream_chunk_received;
                const size_t count = available < remaining
                                         ? available
                                         : remaining;
                memcpy(stream_chunk + stream_chunk_received, &input[index],
                       count);
                index += (int)count;
                stream_chunk_received += count;
                stream_last_receive = xTaskGetTickCount();
                if (stream_chunk_received == stream_chunk_bytes) {
                    const esp_err_t result = relay_stream_push_chunk(
                        stream_chunk_sequence, stream_chunk,
                        stream_chunk_bytes, stream_chunk_crc32);
                    char response[96];
                    const int length = result == ESP_OK
                        ? snprintf(response, sizeof(response),
                                   "ESPDROP-STREAM-ACK seq=%lu bytes=%u\n",
                                   (unsigned long)stream_chunk_sequence,
                                   (unsigned)stream_chunk_bytes)
                        : snprintf(response, sizeof(response),
                                   "ESPDROP-STREAM-CHUNK-ERROR seq=%lu error=%s\n",
                                   (unsigned long)stream_chunk_sequence,
                                   esp_err_to_name(result));
                    ESP_ERROR_CHECK_WITHOUT_ABORT(
                        serial_write(response, (size_t)length));
                    if (result != ESP_OK) {
                        relay_stream_abort();
                    }
                    stream_chunk_receiving = false;
                    stream_chunk_bytes = 0U;
                    stream_chunk_received = 0U;
                }
                continue;
            }
            if (relay_receiving) {
                const size_t available = (size_t)(received - index);
                const size_t count = available < relay_remaining
                                         ? available : relay_remaining;
                const esp_err_t result = relay_spool_write(&input[index], count);
                if (result != ESP_OK) {
                    relay_spool_abort();
                    relay_receiving = false;
                    relay_remaining = 0U;
                    static const char failed[] = "ESPDROP-RELAY-FAILED\n";
                    ESP_ERROR_CHECK_WITHOUT_ABORT(
                        serial_write(failed, sizeof(failed) - 1U));
                    ESP_LOGE(TAG, "relay flash write failed: %s",
                             esp_err_to_name(result));
                    break;
                }
                index += (int)count;
                relay_remaining -= count;
                relay_last_receive = xTaskGetTickCount();
                if (relay_remaining == 0U) {
                    relay_receiving = false;
                    finish_relay_upload();
                }
                continue;
            }
            const uint8_t byte = input[index];
            ++index;
            const improv_serial_parse_result_t parsed =
                improv_serial_parser_feed(&parser, byte, &packet);
            if (parsed == IMPROV_SERIAL_PARSE_PACKET) {
                handle_rpc(&packet);
            } else if (parsed == IMPROV_SERIAL_PARSE_INVALID) {
                send_error(IMPROV_ERROR_INVALID_RPC);
            }

            if (command_length == 0U) {
                if (byte == 'E') {
                    command_line[command_length++] = (char)byte;
                }
                continue;
            }
            if (byte == '\n') {
                command_line[command_length] = '\0';
                const size_t ota_command_length = sizeof(ota_command) - 1U;
                if (strcmp(command_line, "ESPDROP PING") == 0) {
                    char pong[96];
                    const int pong_length = snprintf(
                        pong, sizeof(pong),
                        "ESPDROP-PONG protocol=1 uptime_ms=%llu ready=%u\n",
                        (unsigned long long)(esp_timer_get_time() / 1000),
                        application_ready ? 1U : 0U);
                    if (pong_length > 0 &&
                        (size_t)pong_length < sizeof(pong)) {
                        ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(
                            pong, (size_t)pong_length));
                    }
                } else if (strcmp(command_line, "ESPDROP PEERS") == 0) {
                    send_peer_list();
                } else if (strcmp(command_line, "ESPDROP STATS") == 0) {
                    send_transport_stats();
                } else if (strcmp(command_line,
                                  "ESPDROP STREAM RESULT") == 0) {
                    send_stream_result();
                } else if (strcmp(command_line, "ESPDROP WAKE") == 0) {
#if CONFIG_ESPDROP_BLE_WAKE_LAB
                    const int result = espdrop_ble_wake_start(
                        CONFIG_ESPDROP_BLE_WAKE_DURATION_MS);
                    const char *response = result == ESP_OK
                        ? "ESPDROP-WAKE-ARMED\n"
                        : "ESPDROP-WAKE-FAILED\n";
#else
                    const char *response = "ESPDROP-WAKE-UNAVAILABLE\n";
#endif
                    ESP_ERROR_CHECK_WITHOUT_ABORT(
                        serial_write(response, strlen(response)));
                } else if (handle_target_command(command_line)) {
                    /* Response emitted by the target control handler. */
                } else if (strcmp(command_line, "ESPDROP RESTART") == 0) {
                    static const char restarting[] = "ESPDROP-RESTARTING\n";
                    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(
                        restarting, sizeof(restarting) - 1U));
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                } else if (strcmp(command_line,
                                  "ESPDROP STREAM CANCEL") == 0) {
                    cancel_stream_upload();
                } else if (strcmp(command_line, ota_command) == 0) {
                    arm_ota_and_restart(NULL);
                } else if (command_length > ota_command_length + 1U &&
                           memcmp(command_line, ota_command,
                                  ota_command_length) == 0 &&
                           command_line[ota_command_length] == ' ') {
                    arm_ota_and_restart(
                        &command_line[ota_command_length + 1U]);
                } else if (start_relay_upload(command_line)) {
                    relay_receiving = true;
                    relay_remaining = relay_spool_capacity();
                    /* begin stored the exact declared length; obtain it from
                     * the outgoing descriptor only after completion. Parse
                     * it again here before strtok-modified fields disappear. */
                    char *end = NULL;
                    relay_remaining = strtoul(
                        command_line + sizeof(relay_command) - 1U, &end, 10);
                    relay_last_receive = xTaskGetTickCount();
                } else if (start_stream_upload(command_line)) {
                    /* The source blocks until /Ask is accepted, then emits
                     * ESPDROP-STREAM-GO. Host payload bytes are not accepted
                     * before that marker. */
                } else if (start_stream_chunk(
                               command_line, &stream_chunk_sequence,
                               &stream_chunk_bytes, &stream_chunk_crc32)) {
                    stream_chunk_receiving = true;
                    stream_chunk_received = 0U;
                    stream_last_receive = xTaskGetTickCount();
                }
                command_length = 0U;
                continue;
            }
            if (byte < 0x20U || byte > 0x7eU ||
                command_length + 1U >= sizeof(command_line)) {
                command_length = 0U;
                continue;
            }
            command_line[command_length++] = (char)byte;
            static const char command_prefix[] = "ESPDROP ";
            if (command_length <= sizeof(command_prefix) - 1U &&
                command_line[command_length - 1U] !=
                    command_prefix[command_length - 1U]) {
                command_length = byte == 'E' ? 1U : 0U;
                if (command_length == 1U) {
                    command_line[0] = (char)byte;
                }
            }
        }
        if (relay_receiving && received == 0 &&
            xTaskGetTickCount() - relay_last_receive > pdMS_TO_TICKS(15000)) {
            relay_spool_abort();
            relay_receiving = false;
            relay_remaining = 0U;
            static const char timeout[] = "ESPDROP-RELAY-TIMEOUT\n";
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                serial_write(timeout, sizeof(timeout) - 1U));
        }
        if (stream_chunk_receiving && received == 0 &&
            xTaskGetTickCount() - stream_last_receive >
                pdMS_TO_TICKS(15000)) {
            relay_stream_abort();
            stream_chunk_receiving = false;
            stream_chunk_bytes = 0U;
            stream_chunk_received = 0U;
            static const char timeout[] = "ESPDROP-STREAM-TIMEOUT\n";
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                serial_write(timeout, sizeof(timeout) - 1U));
        }
    }
}

esp_err_t maintenance_serial_start(bool provisioning_mode)
{
    allow_provisioning = provisioning_mode;
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .tx_buffer_size = 2048,
            .rx_buffer_size = RELAY_STREAM_CHUNK_MAX,
        };
        ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&config), TAG,
                            "install USB serial driver");
        usb_serial_jtag_vfs_use_driver();
    }
    if (serial_write_lock == NULL) {
        serial_write_lock = xSemaphoreCreateMutex();
        if (serial_write_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_RETURN_ON_ERROR(allocate_serial_work_areas(), TAG,
                        "allocate serial PSRAM work areas");
    ESP_RETURN_ON_ERROR(relay_stream_init(serial_write), TAG,
                        "initialize host relay stream");
    if (xTaskCreate(serial_task, "maintenance_serial", 6144, NULL, 5, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB maintenance control ready%s",
             provisioning_mode ? "; Improv Wi-Fi setup enabled" : "");
    return ESP_OK;
}

void maintenance_serial_set_application_ready(bool ready)
{
    application_ready = ready;
}
