#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/airdrop_ask.h"
#include "espdrop/airdrop_upload.h"

static const espdrop_airdrop_ask_file_t ask = {
    .sender_computer_name = "espDrop",
    .sender_model_name = "ESP32-S3",
    .sender_id = "1cdbd4423fa0",
    .transfer_id = "00112233-4455-4677-8899-AABBCCDDEEFF",
    .file_name = "hello.jpg",
    .file_type = "public.jpeg",
};

int main(int argc, char **argv)
{
    assert(argc == 2);
    uint8_t body[ESPDROP_AIRDROP_ASK_BODY_MAX_BYTES];
    const size_t body_bytes = espdrop_airdrop_build_ask_body(
        body, sizeof(body), &ask);
    assert(body_bytes > 8U);
    assert(memcmp(body, "bplist00", 8U) == 0);
    assert(memmem(body, body_bytes, ask.transfer_id,
                  strlen(ask.transfer_id)) != NULL);
    assert(memmem(body, body_bytes, "TransferType", 12U) != NULL);
    assert(memmem(body, body_bytes, "FileBomPath", 11U) != NULL);

    espdrop_airdrop_ask_file_t invalid = ask;
    invalid.file_name = "../hello.jpg";
    assert(espdrop_airdrop_build_ask_body(
               body, sizeof(body), &invalid) == 0U);
    invalid = ask;
    invalid.sender_id = "1CDBD4423FA0";
    assert(espdrop_airdrop_build_ask_body(
               body, sizeof(body), &invalid) == 0U);
    invalid = ask;
    invalid.transfer_id = "00112233-4455-4677-8899-aABBCCDDEEFF";
    assert(espdrop_airdrop_build_ask_body(
               body, sizeof(body), &invalid) == 0U);
    assert(espdrop_airdrop_build_ask_body(body, 64U, &ask) == 0U);

    assert(espdrop_airdrop_build_ask_body(
               body, sizeof(body), &ask) == body_bytes);

    uint8_t request[ESPDROP_AIRDROP_ASK_REQUEST_MAX_BYTES];
    const size_t request_bytes = espdrop_airdrop_build_ask_request(
        request, sizeof(request), "iphone.local", 8770U, body, body_bytes);
    assert(request_bytes > body_bytes);
    request[request_bytes] = '\0';
    assert(memcmp(request, "POST /Ask HTTP/1.1\r\n", 20U) == 0);
    assert(strstr((const char *)request,
                  "Content-Type: application/octet-stream\r\n") != NULL);
    assert(strstr((const char *)request,
                  "User-Agent: AirDrop/1.0\r\n") != NULL);
    char content_length[48];
    const int length = snprintf(content_length, sizeof(content_length),
                                "Content-Length: %zu\r\n", body_bytes);
    assert(length > 0);
    assert(strstr((const char *)request, content_length) != NULL);
    assert(memcmp(request + request_bytes - body_bytes, body, body_bytes) == 0);
    assert(espdrop_airdrop_build_ask_request(
               request, 64U, "iphone.local", 8770U, body, body_bytes) == 0U);

    const uint8_t uuid_random[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x08, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    char transfer_id[ESPDROP_AIRDROP_TRANSFER_ID_BYTES];
    espdrop_airdrop_format_transfer_id(transfer_id, uuid_random);
    assert(strcmp(transfer_id,
                  "00112233-4455-4677-8899-AABBCCDDEEFF") == 0);
    assert(espdrop_airdrop_transfer_id_valid(transfer_id));

    espdrop_airdrop_ask_file_t linked_ask = ask;
    linked_ask.transfer_id = transfer_id;
    const size_t linked_body_bytes = espdrop_airdrop_build_ask_body(
        body, sizeof(body), &linked_ask);
    assert(linked_body_bytes > 0U);
    assert(memmem(body, linked_body_bytes, transfer_id,
                  strlen(transfer_id)) != NULL);
    const espdrop_airdrop_upload_identity_t upload_identity = {
        .transfer_id = "00112233-4455-4677-8899-AABBCCDDEEFF",
        .sender_pseudonym = "pseud:RDUDcHgUEfGhmdJSNF0FmQ",
        .sender_push_token =
            "1D0DD878B679E3AA7C9EC13EC596983FA9CF05E3AAEA25F978B5B14D4AB50493",
    };
    const size_t upload_head_bytes = espdrop_airdrop_build_upload_head(
        request, sizeof(request), &upload_identity, 128U);
    assert(upload_head_bytes > 0U);
    request[upload_head_bytes] = '\0';
    assert(strstr((const char *)request, transfer_id) != NULL);

    const uint8_t sender_random[6] = {0x1c, 0xdb, 0xd4, 0x42, 0x3f, 0xa0};
    char sender_id[ESPDROP_AIRDROP_SENDER_ID_BYTES];
    espdrop_airdrop_format_sender_id(sender_id, sender_random);
    assert(strcmp(sender_id, "1cdbd4423fa0") == 0);

    FILE *fixture = fopen(argv[1], "wb");
    assert(fixture != NULL);
    assert(fwrite(body, 1U, body_bytes, fixture) == body_bytes);
    assert(fclose(fixture) == 0);
    puts("AirDrop Ask C tests passed");
    return 0;
}
