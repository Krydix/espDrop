#include "maintenance_serial.h"

#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "improv_serial_codec.h"
#include "ota_update.h"
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
static const uint8_t ota_command[] = "ESPDROP OTA\n";
static bool allow_provisioning;

static esp_err_t serial_write(const void *data, size_t length)
{
    const int written = usb_serial_jtag_write_bytes(
        data, length, pdMS_TO_TICKS(1000));
    return written == (int)length ? ESP_OK : ESP_FAIL;
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

static void arm_ota_and_restart(void)
{
    const esp_err_t result = ota_update_request_github();
    const char *response = result == ESP_OK
                               ? "ESPDROP-OTA-ARMED\n"
                               : "ESPDROP-OTA-NOT-PROVISIONED\n";
    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_write(response, strlen(response)));
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

static void serial_task(void *context)
{
    (void)context;
    improv_serial_parser_t parser = {0};
    improv_serial_packet_t packet;
    size_t ota_match = 0U;
    uint8_t input[64];
    while (true) {
        const int received = usb_serial_jtag_read_bytes(
            input, sizeof(input), pdMS_TO_TICKS(100));
        for (int index = 0; index < received; ++index) {
            const uint8_t byte = input[index];
            const improv_serial_parse_result_t parsed =
                improv_serial_parser_feed(&parser, byte, &packet);
            if (parsed == IMPROV_SERIAL_PARSE_PACKET) {
                handle_rpc(&packet);
            } else if (parsed == IMPROV_SERIAL_PARSE_INVALID) {
                send_error(IMPROV_ERROR_INVALID_RPC);
            }

            if (byte == ota_command[ota_match]) {
                ++ota_match;
                if (ota_match == sizeof(ota_command) - 1U) {
                    ota_match = 0U;
                    arm_ota_and_restart();
                }
            } else {
                ota_match = byte == ota_command[0] ? 1U : 0U;
            }
        }
    }
}

esp_err_t maintenance_serial_start(bool provisioning_mode)
{
    allow_provisioning = provisioning_mode;
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .tx_buffer_size = 1024,
            .rx_buffer_size = 1024,
        };
        ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&config), TAG,
                            "install USB serial driver");
        usb_serial_jtag_vfs_use_driver();
    }
    if (xTaskCreate(serial_task, "maintenance_serial", 6144, NULL, 5, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB maintenance control ready%s",
             provisioning_mode ? "; Improv Wi-Fi setup enabled" : "");
    return ESP_OK;
}
