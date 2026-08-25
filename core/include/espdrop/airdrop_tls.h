#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espdrop/airdrop_upload.h"

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

typedef struct {
    bool attempted;
    bool response_complete;
    int error;
    unsigned http_status;
    size_t request_bytes;
    size_t response_bytes;
    size_t body_bytes;
    bool binary_plist;
    bool receiver_computer_name_key;
    bool chunked;
    char content_type[48];
    char content_encoding[24];
} espdrop_airdrop_discover_result_t;

typedef struct {
    bool attempted;
    bool response_complete;
    int error;
    unsigned http_status;
    size_t body_bytes;
    size_t request_bytes;
    size_t response_bytes;
    bool binary_plist;
    bool chunked;
    bool receiver_computer_name_key;
    bool ids_session_id_key;
    bool receiver_pseudonym_key;
    bool receiver_push_token_key;
    char transfer_id[37];
} espdrop_airdrop_ask_result_t;

typedef struct {
    bool attempted;
    bool response_complete;
    bool transfer_id_continuity;
    int error;
    int stream_status;
    unsigned http_status;
    size_t request_bytes;
    size_t payload_bytes;
    size_t archive_bytes;
    size_t compressed_bytes;
    size_t file_bytes;
    size_t dvzip_blocks;
    size_t workspace_high_water;
    size_t response_bytes;
    size_t body_bytes;
    uint32_t source_crc32;
    bool stored_blocks;
    char transfer_id[37];
} espdrop_airdrop_upload_result_t;

/* Perform one bounded TLS client handshake on an already-connected socket.
 * The lab profile mirrors OpenDrop's Everyone-mode behavior: present a
 * self-signed client certificate and accept the receiver's self-signed
 * certificate. This is transport research, not production identity policy. */
bool espdrop_airdrop_tls_probe(
    int socket_fd,
    const char *server_name,
    uint32_t timeout_ms,
    espdrop_airdrop_tls_result_t *result);

/* Establish TLS and, on that same connection, issue exactly one minimum
 * Everyone-mode POST /Discover request. No /Ask or /Upload is sent. */
bool espdrop_airdrop_tls_discover_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t discover_timeout_ms,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_discover_result_t *discover_result);

/* Attended lab only: establish a fresh sender TLS connection, issue one /Ask
 * for hello.jpg, and wait for the receiver's decision. Discovery is a prior
 * operation in the OpenDrop send flow and is deliberately not sent on this
 * connection. Never sends /Upload. */
bool espdrop_airdrop_tls_ask_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t ask_timeout_ms,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_ask_result_t *ask_result);

/* Relay form of the attended /Ask probe. Uses the supplied file metadata but
 * never uploads file data on this connection. */
bool espdrop_airdrop_tls_ask_stream_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t ask_timeout_ms,
    const espdrop_airdrop_outgoing_file_t *file,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_ask_result_t *ask_result);

/* Directional research helper. Establish TLS on a fresh, already-connected
 * socket and upload only after a prior /Ask returned HTTP 200. A clean
 * iPhone-to-Mac Everyone capture used this topology; the reverse Mac-to-iPhone
 * sender flow instead reused the Ask's underlying TCP/TLS connection. */
bool espdrop_airdrop_tls_upload_stream_probe(
    int socket_fd,
    const char *server_name,
    uint32_t handshake_timeout_ms,
    uint32_t upload_timeout_ms,
    const espdrop_airdrop_ask_result_t *accepted_ask,
    const espdrop_airdrop_outgoing_file_t *file,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_upload_result_t *upload_result);

/* Attended lab only: issue one /Ask and, only after HTTP 200 consent, send one
 * bounded hello.jpg /Upload on the same TLS connection with the exact accepted
 * TransferID. No retry or second transfer is attempted. */
bool espdrop_airdrop_tls_ask_upload_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t ask_timeout_ms,
    uint32_t upload_timeout_ms,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_ask_result_t *ask_result,
    espdrop_airdrop_upload_result_t *upload_result);

/* Native-Mac-shaped relay probe. A source with rewind uses a constant-memory
 * zlib sizing pass: a beneficial result fitting one 128 KiB dvzip block is
 * compressed, otherwise bounded stored blocks are used. A non-seekable source
 * also uses stored dvzip. /Ask and /Upload are sequential HTTP/1 requests on
 * one TLS connection, matching the observed anonymous Mac-to-iPhone flow. */
bool espdrop_airdrop_tls_ask_upload_stream_probe(
    int socket_fd,
    const char *server_name,
    uint16_t server_port,
    uint32_t handshake_timeout_ms,
    uint32_t ask_timeout_ms,
    uint32_t upload_timeout_ms,
    const espdrop_airdrop_outgoing_file_t *file,
    espdrop_airdrop_tls_result_t *tls_result,
    espdrop_airdrop_ask_result_t *ask_result,
    espdrop_airdrop_upload_result_t *upload_result);

#ifdef __cplusplus
}
#endif
