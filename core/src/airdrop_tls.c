#include "espdrop/airdrop_tls.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_ESPDROP_AIRDROP_TLS_LAB

#include "esp_timer.h"
#include "espdrop/airdrop_ask.h"
#include "espdrop/airdrop_http.h"
#include "espdrop/airdrop_upload.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"

#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
#include "esp_heap_caps.h"
#include "miniz.h"
#endif

extern const unsigned char airdrop_lab_certificate_start[]
    asm("_binary_airdrop_lab_certificate_pem_start");
extern const unsigned char airdrop_lab_certificate_end[]
    asm("_binary_airdrop_lab_certificate_pem_end");
extern const unsigned char airdrop_lab_private_key_start[]
    asm("_binary_airdrop_lab_private_key_pem_start");
extern const unsigned char airdrop_lab_private_key_end[]
    asm("_binary_airdrop_lab_private_key_pem_end");

#define AIRDROP_DISCOVER_REQUEST_BYTES 512U
#define AIRDROP_DISCOVER_RESPONSE_BYTES 4096U
#define AIRDROP_DISCOVER_ERROR_BUILD -1
#define AIRDROP_DISCOVER_ERROR_PARSE -2
#define AIRDROP_DISCOVER_ERROR_OVERFLOW -3
#define AIRDROP_ASK_ERROR_BUILD -11
#define AIRDROP_ASK_ERROR_RANDOM -12
#define AIRDROP_ASK_ERROR_PARSE -13
#define AIRDROP_ASK_ERROR_OVERFLOW -14
#define AIRDROP_ASK_BODY_BYTES ESPDROP_AIRDROP_ASK_BODY_MAX_BYTES
#define AIRDROP_ASK_REQUEST_BYTES ESPDROP_AIRDROP_ASK_REQUEST_MAX_BYTES
#define AIRDROP_ASK_RESPONSE_BYTES 4096U
#define AIRDROP_UPLOAD_ERROR_RANDOM -21
#define AIRDROP_UPLOAD_ERROR_ARCHIVE -22
#define AIRDROP_UPLOAD_ERROR_COMPRESS -23
#define AIRDROP_UPLOAD_ERROR_BUILD -24
#define AIRDROP_UPLOAD_ERROR_PARSE -25
#define AIRDROP_UPLOAD_ERROR_OVERFLOW -26
#define AIRDROP_UPLOAD_HEAD_BYTES 512U
#define AIRDROP_UPLOAD_RESPONSE_BYTES 1024U
#define AIRDROP_UPLOAD_COMPRESSED_BYTES 12288U

static uint8_t discover_request[AIRDROP_DISCOVER_REQUEST_BYTES];
static uint8_t discover_response[AIRDROP_DISCOVER_RESPONSE_BYTES];
static uint8_t ask_body[AIRDROP_ASK_BODY_BYTES];
static uint8_t ask_request[AIRDROP_ASK_REQUEST_BYTES];
static uint8_t ask_response[AIRDROP_ASK_RESPONSE_BYTES];

#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
static uint8_t upload_archive[ESPDROP_AIRDROP_ODC_BLOCK_BYTES];
static uint8_t upload_compressed[AIRDROP_UPLOAD_COMPRESSED_BYTES];
static uint8_t upload_head[AIRDROP_UPLOAD_HEAD_BYTES];
static uint8_t upload_response[AIRDROP_UPLOAD_RESPONSE_BYTES];
static const uint8_t upload_jpeg[] = {
#include "airdrop_lab_jpeg.inc"
};
#endif

static void copy_negotiated_text(
    char *destination,
    size_t capacity,
    const char *source)
{
    if (capacity == 0U) {
        return;
    }
    if (source == NULL) {
        source = "-";
    }
    (void)snprintf(destination, capacity, "%s", source);
}

static void copy_discover_http_result(
    espdrop_airdrop_discover_result_t *destination,
    const espdrop_airdrop_http_result_t *source)
{
    destination->http_status = source->status_code;
    destination->body_bytes = source->body_bytes;
    destination->binary_plist = source->binary_plist;
    destination->receiver_computer_name_key =
        source->receiver_computer_name_key;
    destination->chunked = source->chunked;
    copy_negotiated_text(destination->content_type,
                         sizeof(destination->content_type),
                         source->content_type[0] != '\0'
                             ? source->content_type : "-");
    copy_negotiated_text(destination->content_encoding,
                         sizeof(destination->content_encoding),
                         source->content_encoding[0] != '\0'
                             ? source->content_encoding : "-");
}

static void perform_discover(
    mbedtls_ssl_context *ssl,
    const char *server_name,
    uint16_t server_port,
    uint32_t timeout_ms,
    espdrop_airdrop_discover_result_t *result)
{
    memset(result, 0, sizeof(*result));
    copy_negotiated_text(result->content_type,
                         sizeof(result->content_type), "-");
    copy_negotiated_text(result->content_encoding,
                         sizeof(result->content_encoding), "-");
    result->request_bytes = espdrop_airdrop_build_discover_request(
        discover_request, sizeof(discover_request), server_name, server_port);
    if (result->request_bytes == 0U) {
        result->error = AIRDROP_DISCOVER_ERROR_BUILD;
        return;
    }
    result->attempted = true;
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t written = 0U;
    while (written < result->request_bytes &&
           esp_timer_get_time() < deadline_us) {
        const int status = mbedtls_ssl_write(
            ssl, discover_request + written,
            result->request_bytes - written);
        if (status > 0) {
            written += (size_t)status;
        } else if (status == MBEDTLS_ERR_SSL_WANT_READ ||
                   status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10U));
        } else {
            result->error = status;
            return;
        }
    }
    if (written != result->request_bytes) {
        result->error = MBEDTLS_ERR_SSL_TIMEOUT;
        return;
    }

    size_t received = 0U;
    espdrop_airdrop_http_result_t parsed;
    while (esp_timer_get_time() < deadline_us) {
        if (received == sizeof(discover_response)) {
            result->response_bytes = received;
            result->error = AIRDROP_DISCOVER_ERROR_OVERFLOW;
            return;
        }
        const int status = mbedtls_ssl_read(
            ssl, discover_response + received,
            sizeof(discover_response) - received);
        bool end_of_stream = false;
        if (status > 0) {
            received += (size_t)status;
        } else if (status == MBEDTLS_ERR_SSL_WANT_READ ||
                   status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10U));
            continue;
        } else if (status == 0 ||
                   status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            end_of_stream = true;
        } else {
            result->response_bytes = received;
            result->error = status;
            return;
        }
        const espdrop_airdrop_http_parse_t parse =
            espdrop_airdrop_parse_discover_response(
                discover_response, received, end_of_stream, &parsed);
        if (parse == ESPDROP_AIRDROP_HTTP_COMPLETE) {
            result->response_complete = true;
            result->response_bytes = received;
            copy_discover_http_result(result, &parsed);
            return;
        }
        if (parse == ESPDROP_AIRDROP_HTTP_INVALID || end_of_stream) {
            result->response_bytes = received;
            copy_discover_http_result(result, &parsed);
            result->error = AIRDROP_DISCOVER_ERROR_PARSE;
            return;
        }
    }
    result->response_bytes = received;
    const espdrop_airdrop_http_parse_t parse =
        espdrop_airdrop_parse_discover_response(
            discover_response, received, false, &parsed);
    if (parse != ESPDROP_AIRDROP_HTTP_INVALID) {
        copy_discover_http_result(result, &parsed);
    }
    result->error = MBEDTLS_ERR_SSL_TIMEOUT;
}

static void copy_ask_http_result(
    espdrop_airdrop_ask_result_t *destination,
    const espdrop_airdrop_http_result_t *source)
{
    destination->http_status = source->status_code;
    destination->body_bytes = source->body_bytes;
    destination->binary_plist = source->binary_plist;
    destination->chunked = source->chunked;
    destination->receiver_computer_name_key =
        source->receiver_computer_name_key;
    destination->ids_session_id_key = source->ids_session_id_key;
    destination->receiver_pseudonym_key = source->receiver_pseudonym_key;
    destination->receiver_push_token_key = source->receiver_push_token_key;
}

static bool write_tls_request(
    mbedtls_ssl_context *ssl,
    const uint8_t *request,
    size_t request_bytes,
    int64_t deadline_us,
    int *error)
{
    size_t written = 0U;
    while (written < request_bytes && esp_timer_get_time() < deadline_us) {
        const int status = mbedtls_ssl_write(
            ssl, request + written, request_bytes - written);
        if (status > 0) {
            written += (size_t)status;
        } else if (status == MBEDTLS_ERR_SSL_WANT_READ ||
                   status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10U));
        } else {
            *error = status;
            return false;
        }
    }
    if (written != request_bytes) {
        *error = MBEDTLS_ERR_SSL_TIMEOUT;
        return false;
    }
    return true;
}

static void perform_ask(
    mbedtls_ssl_context *ssl,
    mbedtls_ctr_drbg_context *random,
    const char *server_name,
    uint16_t server_port,
    uint32_t timeout_ms,
    espdrop_airdrop_ask_result_t *result)
{
    memset(result, 0, sizeof(*result));
    uint8_t transfer_random[16];
    uint8_t sender_random[6];
    if (mbedtls_ctr_drbg_random(random, transfer_random,
                               sizeof(transfer_random)) != 0 ||
        mbedtls_ctr_drbg_random(random, sender_random,
                               sizeof(sender_random)) != 0) {
        result->error = AIRDROP_ASK_ERROR_RANDOM;
        return;
    }
    char sender_id[ESPDROP_AIRDROP_SENDER_ID_BYTES];
    espdrop_airdrop_format_transfer_id(result->transfer_id, transfer_random);
    espdrop_airdrop_format_sender_id(sender_id, sender_random);
    const espdrop_airdrop_ask_file_t file = {
        .sender_computer_name = "espDrop",
        .sender_model_name = "ESP32-S3",
        .sender_id = sender_id,
        .transfer_id = result->transfer_id,
        .file_name = "hello.jpg",
        .file_type = "public.jpeg",
    };
    const size_t body_bytes = espdrop_airdrop_build_ask_body(
        ask_body, sizeof(ask_body), &file);
    result->request_bytes = espdrop_airdrop_build_ask_request(
        ask_request, sizeof(ask_request), server_name, server_port,
        ask_body, body_bytes);
    if (body_bytes == 0U || result->request_bytes == 0U) {
        result->error = AIRDROP_ASK_ERROR_BUILD;
        return;
    }
    result->attempted = true;
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    if (!write_tls_request(ssl, ask_request, result->request_bytes,
                           deadline_us, &result->error)) {
        return;
    }

    size_t received = 0U;
    espdrop_airdrop_http_result_t parsed;
    while (esp_timer_get_time() < deadline_us) {
        if (received == sizeof(ask_response)) {
            result->response_bytes = received;
            result->error = AIRDROP_ASK_ERROR_OVERFLOW;
            return;
        }
        const int status = mbedtls_ssl_read(
            ssl, ask_response + received, sizeof(ask_response) - received);
        bool end_of_stream = false;
        if (status > 0) {
            received += (size_t)status;
        } else if (status == MBEDTLS_ERR_SSL_WANT_READ ||
                   status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10U));
            continue;
        } else if (status == 0 ||
                   status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            end_of_stream = true;
        } else {
            result->response_bytes = received;
            result->error = status;
            return;
        }
        const espdrop_airdrop_http_parse_t parse =
            espdrop_airdrop_parse_ask_response(
                ask_response, received, end_of_stream, &parsed);
        if (parse == ESPDROP_AIRDROP_HTTP_COMPLETE) {
            result->response_complete = true;
            result->response_bytes = received;
            copy_ask_http_result(result, &parsed);
            return;
        }
        if (parse == ESPDROP_AIRDROP_HTTP_INVALID || end_of_stream) {
            result->response_bytes = received;
            copy_ask_http_result(result, &parsed);
            result->error = AIRDROP_ASK_ERROR_PARSE;
            return;
        }
    }
    result->response_bytes = received;
    const espdrop_airdrop_http_parse_t parse =
        espdrop_airdrop_parse_ask_response(
            ask_response, received, false, &parsed);
    if (parse != ESPDROP_AIRDROP_HTTP_INVALID) {
        copy_ask_http_result(result, &parsed);
    }
    result->error = MBEDTLS_ERR_SSL_TIMEOUT;
}

#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
static void copy_upload_http_result(
    espdrop_airdrop_upload_result_t *destination,
    const espdrop_airdrop_http_result_t *source)
{
    destination->http_status = source->status_code;
    destination->body_bytes = source->body_bytes;
}

static bool write_upload_part(
    mbedtls_ssl_context *ssl,
    const uint8_t *value,
    size_t bytes,
    int64_t deadline_us,
    espdrop_airdrop_upload_result_t *result)
{
    if (!write_tls_request(ssl, value, bytes, deadline_us, &result->error)) {
        return false;
    }
    result->request_bytes += bytes;
    return true;
}

static void perform_upload(
    mbedtls_ssl_context *ssl,
    mbedtls_ctr_drbg_context *random,
    const espdrop_airdrop_ask_result_t *ask,
    uint32_t timeout_ms,
    espdrop_airdrop_upload_result_t *result)
{
    memset(result, 0, sizeof(*result));
    copy_negotiated_text(result->transfer_id, sizeof(result->transfer_id),
                         ask->transfer_id);
    result->file_bytes = sizeof(upload_jpeg);
    result->archive_bytes = espdrop_airdrop_build_odc_archive(
        upload_archive, sizeof(upload_archive), "./hello.jpg", upload_jpeg,
        sizeof(upload_jpeg), 0U);
    if (result->archive_bytes == 0U) {
        result->error = AIRDROP_UPLOAD_ERROR_ARCHIVE;
        return;
    }

    tdefl_compressor *compressor = heap_caps_malloc(
        sizeof(*compressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (compressor == NULL) {
        result->error = AIRDROP_UPLOAD_ERROR_COMPRESS;
        return;
    }
    const int flags = TDEFL_DEFAULT_MAX_PROBES | TDEFL_WRITE_ZLIB_HEADER;
    tdefl_status compression = tdefl_init(compressor, NULL, NULL, flags);
    size_t input_bytes = result->archive_bytes;
    size_t output_bytes = sizeof(upload_compressed);
    if (compression == TDEFL_STATUS_OKAY) {
        compression = tdefl_compress(
            compressor, upload_archive, &input_bytes, upload_compressed,
            &output_bytes, TDEFL_FINISH);
    }
    heap_caps_free(compressor);
    if (compression != TDEFL_STATUS_DONE ||
        input_bytes != result->archive_bytes || output_bytes == 0U ||
        output_bytes > UINT32_MAX) {
        result->error = AIRDROP_UPLOAD_ERROR_COMPRESS;
        return;
    }
    result->compressed_bytes = output_bytes;
    result->payload_bytes =
        ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES + output_bytes;

    uint8_t pseudonym_random[16];
    uint8_t token_random[32];
    if (mbedtls_ctr_drbg_random(random, pseudonym_random,
                               sizeof(pseudonym_random)) != 0 ||
        mbedtls_ctr_drbg_random(random, token_random,
                               sizeof(token_random)) != 0) {
        result->error = AIRDROP_UPLOAD_ERROR_RANDOM;
        return;
    }
    espdrop_airdrop_upload_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    copy_negotiated_text(identity.transfer_id, sizeof(identity.transfer_id),
                         ask->transfer_id);
    espdrop_airdrop_format_sender_pseudonym(identity.sender_pseudonym,
                                            pseudonym_random);
    espdrop_airdrop_format_sender_push_token(identity.sender_push_token,
                                             token_random);
    result->transfer_id_continuity =
        strcmp(identity.transfer_id, ask->transfer_id) == 0;

    const size_t head_bytes = espdrop_airdrop_build_upload_head(
        upload_head, sizeof(upload_head), &identity, result->payload_bytes);
    uint8_t chunk_prefix[32];
    const size_t prefix_bytes = espdrop_airdrop_build_chunk_prefix(
        chunk_prefix, sizeof(chunk_prefix), result->payload_bytes);
    uint8_t block_header[ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES];
    uint8_t terminator[8];
    const size_t terminator_bytes = espdrop_airdrop_build_chunk_terminator(
        terminator, sizeof(terminator));
    if (head_bytes == 0U || prefix_bytes == 0U || terminator_bytes == 0U ||
        !espdrop_airdrop_build_dvzip_block_header(
            block_header, (uint32_t)output_bytes, false) ||
        !result->transfer_id_continuity) {
        result->error = AIRDROP_UPLOAD_ERROR_BUILD;
        return;
    }

    result->attempted = true;
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    static const uint8_t chunk_end[] = {'\r', '\n'};
    if (!write_upload_part(ssl, upload_head, head_bytes, deadline_us, result) ||
        !write_upload_part(ssl, chunk_prefix, prefix_bytes, deadline_us,
                           result) ||
        !write_upload_part(ssl, block_header, sizeof(block_header), deadline_us,
                           result) ||
        !write_upload_part(ssl, upload_compressed, output_bytes, deadline_us,
                           result) ||
        !write_upload_part(ssl, chunk_end, sizeof(chunk_end), deadline_us,
                           result) ||
        !write_upload_part(ssl, terminator, terminator_bytes, deadline_us,
                           result)) {
        return;
    }

    size_t received = 0U;
    espdrop_airdrop_http_result_t parsed;
    while (esp_timer_get_time() < deadline_us) {
        if (received == sizeof(upload_response)) {
            result->response_bytes = received;
            result->error = AIRDROP_UPLOAD_ERROR_OVERFLOW;
            return;
        }
        const int status = mbedtls_ssl_read(
            ssl, upload_response + received,
            sizeof(upload_response) - received);
        bool end_of_stream = false;
        if (status > 0) {
            received += (size_t)status;
        } else if (status == MBEDTLS_ERR_SSL_WANT_READ ||
                   status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10U));
            continue;
        } else if (status == 0 ||
                   status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            end_of_stream = true;
        } else {
            result->response_bytes = received;
            result->error = status;
            return;
        }
        const espdrop_airdrop_http_parse_t parse =
            espdrop_airdrop_parse_upload_response(
                upload_response, received, end_of_stream, &parsed);
        if (parse == ESPDROP_AIRDROP_HTTP_COMPLETE) {
            result->response_complete = true;
            result->response_bytes = received;
            copy_upload_http_result(result, &parsed);
            return;
        }
        if (parse == ESPDROP_AIRDROP_HTTP_INVALID || end_of_stream) {
            result->response_bytes = received;
            copy_upload_http_result(result, &parsed);
            result->error = AIRDROP_UPLOAD_ERROR_PARSE;
            return;
        }
    }
    result->response_bytes = received;
    const espdrop_airdrop_http_parse_t parse =
        espdrop_airdrop_parse_upload_response(
            upload_response, received, false, &parsed);
    if (parse != ESPDROP_AIRDROP_HTTP_INVALID) {
        copy_upload_http_result(result, &parsed);
    }
    result->error = MBEDTLS_ERR_SSL_TIMEOUT;
}
#endif

static bool run_tls(
    int socket_fd,
    const char *server_name,
    uint32_t timeout_ms,
    espdrop_airdrop_tls_result_t *result,
    uint16_t discover_port,
    uint32_t discover_timeout_ms,
    espdrop_airdrop_discover_result_t *discover_result,
    uint32_t ask_timeout_ms,
    espdrop_airdrop_ask_result_t *ask_result,
    uint32_t upload_timeout_ms,
    espdrop_airdrop_upload_result_t *upload_result)
{
    if (socket_fd < 0 || timeout_ms == 0U || result == NULL) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    copy_negotiated_text(result->version, sizeof(result->version), "-");
    copy_negotiated_text(result->ciphersuite, sizeof(result->ciphersuite),
                         "-");

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    mbedtls_net_context network = {.fd = socket_fd};
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&random);
    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&private_key);
    mbedtls_ssl_config_init(&config);
    mbedtls_ssl_init(&ssl);

    int status = 0;
    static const unsigned char personalization[] =
        "espdrop-airdrop-tls-lab";
    status = mbedtls_ctr_drbg_seed(
        &random, mbedtls_entropy_func, &entropy,
        personalization, sizeof(personalization) - 1U);
    if (status != 0) {
        goto done;
    }
    status = mbedtls_x509_crt_parse(
        &certificate, airdrop_lab_certificate_start,
        (size_t)(airdrop_lab_certificate_end -
                 airdrop_lab_certificate_start));
    if (status != 0) {
        goto done;
    }
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    status = mbedtls_pk_parse_key(
        &private_key, airdrop_lab_private_key_start,
        (size_t)(airdrop_lab_private_key_end -
                 airdrop_lab_private_key_start),
        NULL, 0U, mbedtls_ctr_drbg_random, &random);
#else
    status = mbedtls_pk_parse_key(
        &private_key, airdrop_lab_private_key_start,
        (size_t)(airdrop_lab_private_key_end -
                 airdrop_lab_private_key_start),
        NULL, 0U);
#endif
    if (status != 0) {
        goto done;
    }
    status = mbedtls_ssl_config_defaults(
        &config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (status != 0) {
        goto done;
    }
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
    /* OpenDrop documents that AirDrop accepts self-signed certificates in
     * this mode. Verification policy is intentionally deferred until the
     * authenticated/Contacts Only protocol is implemented. */
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
    status = mbedtls_ssl_conf_own_cert(
        &config, &certificate, &private_key);
    if (status != 0) {
        goto done;
    }
    status = mbedtls_ssl_setup(&ssl, &config);
    if (status != 0) {
        goto done;
    }
    if (server_name != NULL && server_name[0] != '\0') {
        status = mbedtls_ssl_set_hostname(&ssl, server_name);
        if (status != 0) {
            goto done;
        }
    }
    mbedtls_ssl_set_bio(&ssl, &network, mbedtls_net_send,
                        mbedtls_net_recv, NULL);

    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000;
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

    result->verify_flags = mbedtls_ssl_get_verify_result(&ssl);
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl);
    result->peer_certificate_present = peer != NULL;
    if (peer != NULL) {
        result->peer_certificate_bytes = peer->raw.len;
    }
    if (status == 0) {
        result->connected = true;
        copy_negotiated_text(result->version, sizeof(result->version),
                             mbedtls_ssl_get_version(&ssl));
        copy_negotiated_text(result->ciphersuite,
                             sizeof(result->ciphersuite),
                             mbedtls_ssl_get_ciphersuite(&ssl));
        if (discover_result != NULL) {
            perform_discover(&ssl, server_name, discover_port,
                             discover_timeout_ms, discover_result);
        }
        if (ask_result != NULL &&
            (discover_result == NULL ||
             (discover_result->response_complete &&
              discover_result->http_status == 200U))) {
            perform_ask(&ssl, &random, server_name, discover_port,
                        ask_timeout_ms, ask_result);
#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
            if (upload_result != NULL && ask_result->response_complete &&
                ask_result->http_status == 200U) {
                perform_upload(&ssl, &random, ask_result, upload_timeout_ms,
                               upload_result);
            }
#else
            (void)upload_timeout_ms;
            (void)upload_result;
#endif
        }
    }

done:
    result->error = status;
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_pk_free(&private_key);
    mbedtls_x509_crt_free(&certificate);
    mbedtls_ctr_drbg_free(&random);
    mbedtls_entropy_free(&entropy);
    return result->connected;
}

bool espdrop_airdrop_tls_probe(
    int socket_fd,
    const char *server_name,
    uint32_t timeout_ms,
    espdrop_airdrop_tls_result_t *result)
{
    return run_tls(socket_fd, server_name, timeout_ms, result, 0U, 0U, NULL,
                   0U, NULL, 0U, NULL);
}

bool espdrop_airdrop_tls_discover_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t discover_timeout_ms,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_discover_result_t *discover_result)
{
    if (server_port == 0U || discover_timeout_ms == 0U ||
        discover_result == NULL) {
        return false;
    }
    memset(discover_result, 0, sizeof(*discover_result));
    return run_tls(socket_fd, server_name, handshake_timeout_ms, tls_result,
                   server_port, discover_timeout_ms, discover_result, 0U,
                   NULL, 0U, NULL);
}

bool espdrop_airdrop_tls_ask_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t ask_timeout_ms,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_ask_result_t *ask_result)
{
    if (server_port == 0U || ask_timeout_ms == 0U || ask_result == NULL) {
        return false;
    }
    memset(ask_result, 0, sizeof(*ask_result));
    return run_tls(socket_fd, server_name, handshake_timeout_ms, tls_result,
                   server_port, 0U, NULL, ask_timeout_ms, ask_result, 0U,
                   NULL);
}

bool espdrop_airdrop_tls_ask_upload_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t ask_timeout_ms,
    uint32_t upload_timeout_ms,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_ask_result_t *ask_result,
    espdrop_airdrop_upload_result_t *upload_result)
{
#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
    if (server_port == 0U || ask_timeout_ms == 0U ||
        upload_timeout_ms == 0U || ask_result == NULL ||
        upload_result == NULL) {
        return false;
    }
    memset(ask_result, 0, sizeof(*ask_result));
    memset(upload_result, 0, sizeof(*upload_result));
    return run_tls(socket_fd, server_name, handshake_timeout_ms, tls_result,
                   server_port, 0U, NULL, ask_timeout_ms, ask_result,
                   upload_timeout_ms, upload_result);
#else
    (void)socket_fd;
    (void)server_name;
    (void)server_port;
    (void)handshake_timeout_ms;
    (void)ask_timeout_ms;
    (void)upload_timeout_ms;
    if (tls_result != NULL) {
        memset(tls_result, 0, sizeof(*tls_result));
        tls_result->error = AIRDROP_UPLOAD_ERROR_BUILD;
    }
    if (ask_result != NULL) {
        memset(ask_result, 0, sizeof(*ask_result));
        ask_result->error = AIRDROP_UPLOAD_ERROR_BUILD;
    }
    if (upload_result != NULL) {
        memset(upload_result, 0, sizeof(*upload_result));
        upload_result->error = AIRDROP_UPLOAD_ERROR_BUILD;
    }
    return false;
#endif
}

#else

bool espdrop_airdrop_tls_probe(
    int socket_fd,
    const char *server_name,
    uint32_t timeout_ms,
    espdrop_airdrop_tls_result_t *result)
{
    (void)socket_fd;
    (void)server_name;
    (void)timeout_ms;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->error = -1;
    }
    return false;
}

bool espdrop_airdrop_tls_discover_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t discover_timeout_ms,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_discover_result_t *discover_result)
{
    (void)socket_fd;
    (void)server_name;
    (void)server_port;
    (void)handshake_timeout_ms;
    (void)discover_timeout_ms;
    if (tls_result != NULL) {
        memset(tls_result, 0, sizeof(*tls_result));
        tls_result->error = -1;
    }
    if (discover_result != NULL) {
        memset(discover_result, 0, sizeof(*discover_result));
        discover_result->error = -1;
    }
    return false;
}

bool espdrop_airdrop_tls_ask_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t ask_timeout_ms,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_ask_result_t *ask_result)
{
    (void)socket_fd;
    (void)server_name;
    (void)server_port;
    (void)handshake_timeout_ms;
    (void)ask_timeout_ms;
    if (tls_result != NULL) {
        memset(tls_result, 0, sizeof(*tls_result));
        tls_result->error = -1;
    }
    if (ask_result != NULL) {
        memset(ask_result, 0, sizeof(*ask_result));
        ask_result->error = -1;
    }
    return false;
}

bool espdrop_airdrop_tls_ask_upload_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t ask_timeout_ms,
    uint32_t upload_timeout_ms,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_ask_result_t *ask_result,
    espdrop_airdrop_upload_result_t *upload_result)
{
    (void)socket_fd;
    (void)server_name;
    (void)server_port;
    (void)handshake_timeout_ms;
    (void)ask_timeout_ms;
    (void)upload_timeout_ms;
    if (tls_result != NULL) {
        memset(tls_result, 0, sizeof(*tls_result));
        tls_result->error = -1;
    }
    if (ask_result != NULL) {
        memset(ask_result, 0, sizeof(*ask_result));
        ask_result->error = -1;
    }
    if (upload_result != NULL) {
        memset(upload_result, 0, sizeof(*upload_result));
        upload_result->error = -1;
    }
    return false;
}

#endif
