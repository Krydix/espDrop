#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/airdrop_http.h"

int main(void)
{
    uint8_t request[512];
    const size_t request_length = espdrop_airdrop_build_discover_request(
        request, sizeof(request), "iphone.local", 8770U);
    assert(request_length > ESPDROP_AIRDROP_DISCOVER_BODY_BYTES);
    assert(memcmp(request, "POST /Discover HTTP/1.1\r\n", 25U) == 0);
    assert(strstr((char *)request, "User-Agent: AirDrop/1.0\r\n") != NULL);
    assert(strstr((char *)request, "Content-Length: 42\r\n") != NULL);
    assert(memcmp(request + request_length - 42U, "bplist00", 8U) == 0);
    assert(espdrop_airdrop_build_discover_request(
               request, 64U, "iphone.local", 8770U) == 0U);

    static const char body[] = "bplist00xxxxReceiverComputerNameyyyy";
    char response[256];
    const int header_length = snprintf(
        response, sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        (unsigned)(sizeof(body) - 1U));
    assert(header_length > 0);
    memcpy(response + header_length, body, sizeof(body) - 1U);
    const size_t response_length =
        (size_t)header_length + sizeof(body) - 1U;
    espdrop_airdrop_http_result_t parsed;
    assert(espdrop_airdrop_parse_discover_response(
               (const uint8_t *)response, response_length - 1U, false,
               &parsed) == ESPDROP_AIRDROP_HTTP_INCOMPLETE);
    assert(espdrop_airdrop_parse_discover_response(
               (const uint8_t *)response, response_length, false,
               &parsed) == ESPDROP_AIRDROP_HTTP_COMPLETE);
    assert(parsed.status_code == 200U);
    assert(parsed.body_bytes == sizeof(body) - 1U);
    assert(parsed.binary_plist);
    assert(parsed.receiver_computer_name_key);
    assert(strcmp(parsed.content_type, "application/octet-stream") == 0);

    static const char chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "8\r\nbplist00\r\n"
        "b\r\nxxxxReceive\r\n"
        "d\r\nrComputerName\r\n"
        "4\r\nyyyy\r\n"
        "0\r\n\r\n";
    assert(espdrop_airdrop_parse_discover_response(
               (const uint8_t *)chunked, sizeof(chunked) - 2U, false,
               &parsed) == ESPDROP_AIRDROP_HTTP_INCOMPLETE);
    assert(espdrop_airdrop_parse_discover_response(
               (const uint8_t *)chunked, sizeof(chunked) - 1U, false,
               &parsed) == ESPDROP_AIRDROP_HTTP_COMPLETE);
    assert(parsed.status_code == 200U);
    assert(parsed.chunked);
    assert(parsed.body_bytes == 36U);
    assert(parsed.binary_plist);
    assert(parsed.receiver_computer_name_key);

    static const char ask_chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "14\r\nbplist00IDSSessionID\r\n"
        "12\r\nReceiverPseudonymR\r\n"
        "10\r\neceiverPushToken\r\n"
        "0\r\n\r\n";
    assert(espdrop_airdrop_parse_ask_response(
               (const uint8_t *)ask_chunked, sizeof(ask_chunked) - 1U,
               false, &parsed) == ESPDROP_AIRDROP_HTTP_COMPLETE);
    assert(parsed.status_code == 200U);
    assert(parsed.binary_plist);
    assert(parsed.ids_session_id_key);
    assert(parsed.receiver_pseudonym_key);
    assert(parsed.receiver_push_token_key);

    static const char upload_response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    assert(espdrop_airdrop_parse_upload_response(
               (const uint8_t *)upload_response,
               sizeof(upload_response) - 1U, false,
               &parsed) == ESPDROP_AIRDROP_HTTP_COMPLETE);
    assert(parsed.status_code == 200U);
    assert(parsed.body_bytes == 0U);

    static const char invalid[] = "NOPE\r\nContent-Length: 0\r\n\r\n";
    assert(espdrop_airdrop_parse_discover_response(
               (const uint8_t *)invalid, sizeof(invalid) - 1U, true,
               &parsed) == ESPDROP_AIRDROP_HTTP_INVALID);

    puts("AirDrop HTTP tests passed");
    return 0;
}
