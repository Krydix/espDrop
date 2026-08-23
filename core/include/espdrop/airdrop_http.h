#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_AIRDROP_DISCOVER_BODY_BYTES 42U
#define ESPDROP_AIRDROP_HTTP_CONTENT_TYPE_BYTES 48U
#define ESPDROP_AIRDROP_HTTP_CONTENT_ENCODING_BYTES 24U

typedef enum {
    ESPDROP_AIRDROP_HTTP_INVALID = -1,
    ESPDROP_AIRDROP_HTTP_INCOMPLETE = 0,
    ESPDROP_AIRDROP_HTTP_COMPLETE = 1,
} espdrop_airdrop_http_parse_t;

typedef struct {
    unsigned status_code;
    size_t header_bytes;
    size_t body_bytes;
    size_t content_length;
    bool has_content_length;
    bool chunked;
    bool binary_plist;
    bool receiver_computer_name_key;
    bool ids_session_id_key;
    bool receiver_pseudonym_key;
    bool receiver_push_token_key;
    char content_type[ESPDROP_AIRDROP_HTTP_CONTENT_TYPE_BYTES];
    char content_encoding[ESPDROP_AIRDROP_HTTP_CONTENT_ENCODING_BYTES];
} espdrop_airdrop_http_result_t;

/* Build OpenDrop's minimum Everyone-mode /Discover request: an empty binary
 * plist and no SenderRecordData. The returned byte count includes the body. */
size_t espdrop_airdrop_build_discover_request(
    uint8_t *output,
    size_t capacity,
    const char *host,
    uint16_t port);

/* Parse one bounded HTTP/1.x response accumulated in memory. Responses with
 * Content-Length become complete as soon as that body is present. A response
 * without a length is complete only when end_of_stream is true. */
espdrop_airdrop_http_parse_t espdrop_airdrop_parse_discover_response(
    const uint8_t *response,
    size_t length,
    bool end_of_stream,
    espdrop_airdrop_http_result_t *result);

espdrop_airdrop_http_parse_t espdrop_airdrop_parse_ask_response(
    const uint8_t *response,
    size_t length,
    bool end_of_stream,
    espdrop_airdrop_http_result_t *result);

#ifdef __cplusplus
}
#endif
