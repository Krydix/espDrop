#include "espdrop/airdrop_tls.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_ESPDROP_AIRDROP_TLS_LAB

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

bool espdrop_airdrop_tls_probe(
    int socket_fd,
    const char *server_name,
    uint32_t timeout_ms,
    espdrop_airdrop_tls_result_t *result)
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

#endif
