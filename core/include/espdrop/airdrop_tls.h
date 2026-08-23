#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_AIRDROP_TLS_VERSION_BYTES 16U
#define ESPDROP_AIRDROP_TLS_CIPHER_BYTES 64U

typedef struct {
    bool connected;
    bool peer_certificate_present;
    int error;
    uint32_t verify_flags;
    size_t peer_certificate_bytes;
    char version[ESPDROP_AIRDROP_TLS_VERSION_BYTES];
    char ciphersuite[ESPDROP_AIRDROP_TLS_CIPHER_BYTES];
} espdrop_airdrop_tls_result_t;

/* Perform one bounded TLS client handshake on an already-connected socket.
 * The lab profile mirrors OpenDrop's Everyone-mode behavior: present a
 * self-signed client certificate and accept the receiver's self-signed
 * certificate. This is transport research, not production identity policy. */
bool espdrop_airdrop_tls_probe(
    int socket_fd,
    const char *server_name,
    uint32_t timeout_ms,
    espdrop_airdrop_tls_result_t *result);

#ifdef __cplusplus
}
#endif
