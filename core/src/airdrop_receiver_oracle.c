#include "espdrop/airdrop_receiver_oracle.h"

#include "sdkconfig.h"

#if CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_LAB

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"

extern const unsigned char airdrop_lab_certificate_start[]
    asm("_binary_airdrop_lab_certificate_pem_start");
extern const unsigned char airdrop_lab_certificate_end[]
    asm("_binary_airdrop_lab_certificate_pem_end");
extern const unsigned char airdrop_lab_private_key_start[]
    asm("_binary_airdrop_lab_private_key_pem_start");
extern const unsigned char airdrop_lab_private_key_end[]
    asm("_binary_airdrop_lab_private_key_pem_end");

#define RECEIVER_TASK_STACK_BYTES 16384U
#define RECEIVER_REQUEST_BYTES 16384U
#define RECEIVER_HEADER_LIMIT_BYTES 4096U
#define RECEIVER_HANDSHAKE_TIMEOUT_MS 30000U
#define RECEIVER_REQUEST_TIMEOUT_MS 15000U
#define RECEIVER_MAX_CONNECTIONS 8U

static const char *TAG = "airdrop_receiver";
static uint16_t receiver_port;
static bool receiver_started;
static uint8_t receiver_request[RECEIVER_REQUEST_BYTES];

/* plistlib FMT_BINARY encoding of:
 * { ReceiverMediaCapabilities: b'{"Version": 1}',
 *   ReceiverComputerName: "espDrop",
 *   ReceiverModelName: "ESP32-S3" }
 * No ReceiverRecordData is present, forcing the anonymous Everyone path. */
static const uint8_t discover_body[] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd3, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x5f, 0x10, 0x19, 0x52, 0x65, 0x63, 0x65, 0x69, 0x76,
    0x65, 0x72, 0x4d, 0x65, 0x64, 0x69, 0x61, 0x43, 0x61, 0x70, 0x61, 0x62,
    0x69, 0x6c, 0x69, 0x74, 0x69, 0x65, 0x73, 0x5f, 0x10, 0x14, 0x52, 0x65,
    0x63, 0x65, 0x69, 0x76, 0x65, 0x72, 0x43, 0x6f, 0x6d, 0x70, 0x75, 0x74,
    0x65, 0x72, 0x4e, 0x61, 0x6d, 0x65, 0x5f, 0x10, 0x11, 0x52, 0x65, 0x63,
    0x65, 0x69, 0x76, 0x65, 0x72, 0x4d, 0x6f, 0x64, 0x65, 0x6c, 0x4e, 0x61,
    0x6d, 0x65, 0x4e, 0x7b, 0x22, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e,
    0x22, 0x3a, 0x20, 0x31, 0x7d, 0x57, 0x65, 0x73, 0x70, 0x44, 0x72, 0x6f,
    0x70, 0x58, 0x45, 0x53, 0x50, 0x33, 0x32, 0x2d, 0x53, 0x33, 0x08, 0x0f,
    0x2b, 0x42, 0x56, 0x65, 0x6d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x76,
};

static size_t find_header_end(const uint8_t *bytes, size_t length)
{
    for (size_t index = 0U; index + 3U < length; ++index) {
        if (memcmp(bytes + index, "\r\n\r\n", 4U) == 0) {
            return index + 4U;
        }
    }
    return 0U;
}

static bool ascii_starts_with_case(
    const uint8_t *bytes,
    size_t length,
    const char *prefix)
{
    const size_t prefix_length = strlen(prefix);
    if (length < prefix_length) {
        return false;
    }
    for (size_t index = 0U; index < prefix_length; ++index) {
        if (tolower((unsigned char)bytes[index]) !=
            tolower((unsigned char)prefix[index])) {
            return false;
        }
    }
    return true;
}

static bool parse_content_length(
    const uint8_t *headers,
    size_t header_bytes,
    size_t *content_length)
{
    *content_length = 0U;
    size_t offset = 0U;
    while (offset + 1U < header_bytes) {
        size_t end = offset;
        while (end + 1U < header_bytes &&
               !(headers[end] == '\r' && headers[end + 1U] == '\n')) {
            ++end;
        }
        if (ascii_starts_with_case(headers + offset, end - offset,
                                   "Content-Length:")) {
            size_t value = offset + strlen("Content-Length:");
            while (value < end && (headers[value] == ' ' ||
                                    headers[value] == '\t')) {
                ++value;
            }
            if (value == end) {
                return false;
            }
            size_t parsed = 0U;
            for (; value < end; ++value) {
                if (headers[value] < '0' || headers[value] > '9' ||
                    parsed > (SIZE_MAX - (headers[value] - '0')) / 10U) {
                    return false;
                }
                parsed = parsed * 10U + (size_t)(headers[value] - '0');
            }
            *content_length = parsed;
            return true;
        }
        offset = end + 2U;
    }
    return true;
}

static bool tls_write_all(
    mbedtls_ssl_context *ssl,
    const uint8_t *bytes,
    size_t length,
    int64_t deadline_us,
    int *status)
{
    size_t written = 0U;
    while (written < length && esp_timer_get_time() < deadline_us) {
        *status = mbedtls_ssl_write(ssl, bytes + written, length - written);
        if (*status > 0) {
            written += (size_t)*status;
        } else if (*status == MBEDTLS_ERR_SSL_WANT_READ ||
                   *status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10U));
        } else {
            return false;
        }
    }
    if (written != length) {
        *status = MBEDTLS_ERR_SSL_TIMEOUT;
        return false;
    }
    return true;
}

static int serve_discover(mbedtls_ssl_context *ssl, size_t *request_bytes)
{
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)RECEIVER_REQUEST_TIMEOUT_MS * 1000;
    size_t received = 0U;
    size_t header_bytes = 0U;
    size_t body_bytes = 0U;
    int status = 0;
    while (esp_timer_get_time() < deadline_us &&
           received < sizeof(receiver_request)) {
        status = mbedtls_ssl_read(
            ssl, receiver_request + received,
            sizeof(receiver_request) - received);
        if (status > 0) {
            received += (size_t)status;
            if (header_bytes == 0U) {
                header_bytes = find_header_end(receiver_request, received);
                if (header_bytes > RECEIVER_HEADER_LIMIT_BYTES ||
                    (header_bytes != 0U &&
                     !parse_content_length(receiver_request, header_bytes,
                                           &body_bytes))) {
                    return -1;
                }
                if (header_bytes != 0U &&
                    body_bytes > sizeof(receiver_request) - header_bytes) {
                    return -1;
                }
            }
            if (header_bytes != 0U &&
                received >= header_bytes + body_bytes) {
                break;
            }
        } else if (status == MBEDTLS_ERR_SSL_WANT_READ ||
                   status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10U));
        } else {
            return status;
        }
    }
    *request_bytes = received;
    if (header_bytes == 0U || received < header_bytes + body_bytes) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    static const char discover_line[] = "POST /Discover HTTP/1.";
    const bool discover =
        received >= sizeof(discover_line) - 1U &&
        memcmp(receiver_request, discover_line,
               sizeof(discover_line) - 1U) == 0;
    if (!discover) {
        static const uint8_t not_found[] =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)tls_write_all(ssl, not_found, sizeof(not_found) - 1U,
                            deadline_us, &status);
        return -2;
    }

    uint8_t response_head[160];
    const int head_bytes = snprintf(
        (char *)response_head, sizeof(response_head),
        "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Connection: close\r\n\r\n",
        (unsigned)sizeof(discover_body));
    if (head_bytes <= 0 || (size_t)head_bytes >= sizeof(response_head) ||
        !tls_write_all(ssl, response_head, (size_t)head_bytes,
                       deadline_us, &status) ||
        !tls_write_all(ssl, discover_body, sizeof(discover_body),
                       deadline_us, &status)) {
        return status;
    }
    return 0;
}

static void handle_connection(
    int client_fd,
    const struct sockaddr_in6 *source,
    unsigned connection)
{
    (void)fcntl(client_fd, F_SETFL,
                fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    mbedtls_net_context network = {.fd = client_fd};
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&random);
    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&private_key);
    mbedtls_ssl_config_init(&config);
    mbedtls_ssl_init(&ssl);

    int status = 0;
    size_t request_bytes = 0U;
    static const unsigned char personalization[] =
        "espdrop-airdrop-receiver-oracle";
    status = mbedtls_ctr_drbg_seed(
        &random, mbedtls_entropy_func, &entropy,
        personalization, sizeof(personalization) - 1U);
    if (status == 0) {
        status = mbedtls_x509_crt_parse(
            &certificate, airdrop_lab_certificate_start,
            (size_t)(airdrop_lab_certificate_end -
                     airdrop_lab_certificate_start));
    }
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    if (status == 0) {
        status = mbedtls_pk_parse_key(
            &private_key, airdrop_lab_private_key_start,
            (size_t)(airdrop_lab_private_key_end -
                     airdrop_lab_private_key_start),
            NULL, 0U, mbedtls_ctr_drbg_random, &random);
    }
#else
    if (status == 0) {
        status = mbedtls_pk_parse_key(
            &private_key, airdrop_lab_private_key_start,
            (size_t)(airdrop_lab_private_key_end -
                     airdrop_lab_private_key_start), NULL, 0U);
    }
#endif
    if (status == 0) {
        status = mbedtls_ssl_config_defaults(
            &config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (status == 0) {
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
        status = mbedtls_ssl_conf_own_cert(
            &config, &certificate, &private_key);
    }
    if (status == 0) {
        status = mbedtls_ssl_setup(&ssl, &config);
    }
    if (status == 0) {
        mbedtls_ssl_set_bio(&ssl, &network, mbedtls_net_send,
                            mbedtls_net_recv, NULL);
        const int64_t deadline_us = esp_timer_get_time() +
            (int64_t)RECEIVER_HANDSHAKE_TIMEOUT_MS * 1000;
        do {
            status = mbedtls_ssl_handshake(&ssl);
            if (status == MBEDTLS_ERR_SSL_WANT_READ ||
                status == MBEDTLS_ERR_SSL_WANT_WRITE) {
                vTaskDelay(pdMS_TO_TICKS(10U));
            }
        } while ((status == MBEDTLS_ERR_SSL_WANT_READ ||
                  status == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                 esp_timer_get_time() < deadline_us);
        if ((status == MBEDTLS_ERR_SSL_WANT_READ ||
             status == MBEDTLS_ERR_SSL_WANT_WRITE) &&
            esp_timer_get_time() >= deadline_us) {
            status = MBEDTLS_ERR_SSL_TIMEOUT;
        }
    }
    const bool tls_connected = status == 0;
    if (tls_connected) {
        status = serve_discover(&ssl, &request_bytes);
        (void)mbedtls_ssl_close_notify(&ssl);
    }
    char address[INET6_ADDRSTRLEN] = "-";
    (void)inet6_ntoa_r(source->sin6_addr, address, sizeof(address));
    ESP_LOGW(TAG,
             "AIRDROP-RECEIVER connection=%u source=%s tls=%u "
             "request_bytes=%u discover=%u status=%d",
             connection, address, tls_connected ? 1U : 0U,
             (unsigned)request_bytes,
             tls_connected && status == 0 ? 1U : 0U, status);

    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_pk_free(&private_key);
    mbedtls_x509_crt_free(&certificate);
    mbedtls_ctr_drbg_free(&random);
    mbedtls_entropy_free(&entropy);
}

static void receiver_task(void *argument)
{
    (void)argument;
    const int listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        ESP_LOGE(TAG, "receiver socket failed error=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    const int enabled = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     &enabled, sizeof(enabled));
    (void)setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY,
                     &enabled, sizeof(enabled));
    struct sockaddr_in6 address = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(receiver_port),
        .sin6_addr = IN6ADDR_ANY_INIT,
    };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 2) != 0) {
        ESP_LOGE(TAG, "receiver listen failed port=%u error=%d",
                 receiver_port, errno);
        close(listener);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG,
             "anonymous TLS /Discover listener ready port=%u identity=none",
             receiver_port);
    for (unsigned connection = 1U;
         connection <= RECEIVER_MAX_CONNECTIONS; ++connection) {
        struct sockaddr_in6 source = {0};
        socklen_t source_bytes = sizeof(source);
        const int client = accept(listener, (struct sockaddr *)&source,
                                  &source_bytes);
        if (client < 0) {
            ESP_LOGW(TAG, "receiver accept failed error=%d", errno);
            --connection;
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }
        handle_connection(client, &source, connection);
        close(client);
    }
    ESP_LOGW(TAG, "receiver connection budget exhausted count=%u",
             RECEIVER_MAX_CONNECTIONS);
    close(listener);
    vTaskDelete(NULL);
}

esp_err_t espdrop_airdrop_receiver_oracle_start(uint16_t port)
{
    if (port == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (receiver_started) {
        return ESP_ERR_INVALID_STATE;
    }
    receiver_port = port;
    if (xTaskCreate(receiver_task, "airdrop_receiver",
                    RECEIVER_TASK_STACK_BYTES, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    receiver_started = true;
    return ESP_OK;
}

#else

esp_err_t espdrop_airdrop_receiver_oracle_start(uint16_t port)
{
    (void)port;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
