#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/airdrop_upload.h"

static const espdrop_airdrop_upload_identity_t identity = {
    .transfer_id = "664D3979-F245-4E9E-9EAC-80453E255E31",
    .sender_pseudonym = "pseud:RDUDcHgUEfGhmdJSNF0FmQ",
    .sender_push_token =
        "1D0DD878B679E3AA7C9EC13EC596983FA9CF05E3AAEA25F978B5B14D4AB50493",
};

int main(void)
{
    assert(espdrop_airdrop_upload_identity_valid(&identity));
    espdrop_airdrop_upload_identity_t invalid = identity;
    invalid.transfer_id[0] = 'a';
    assert(!espdrop_airdrop_upload_identity_valid(&invalid));
    invalid = identity;
    invalid.sender_pseudonym[27] = '=';
    assert(!espdrop_airdrop_upload_identity_valid(&invalid));
    invalid = identity;
    invalid.sender_push_token[63] = 'z';
    assert(!espdrop_airdrop_upload_identity_valid(&invalid));

    uint8_t head[512];
    const size_t head_bytes = espdrop_airdrop_build_upload_head(
        head, sizeof(head), &identity, 1303171U);
    assert(head_bytes > 0U);
    head[head_bytes] = '\0';
    static const char expected[] =
        "POST /Upload HTTP/1.1\r\n"
        "User-Agent: AirDrop/1.0\r\n"
        "TotalBytes: 1303171\r\n"
        "Content-Type: application/x-dvzip\r\n"
        "SenderPseudonym: pseud:RDUDcHgUEfGhmdJSNF0FmQ\r\n"
        "SenderPushToken: "
        "1D0DD878B679E3AA7C9EC13EC596983FA9CF05E3AAEA25F978B5B14D4AB50493\r\n"
        "TransferID: 664D3979-F245-4E9E-9EAC-80453E255E31\r\n"
        "Connection: keep-alive\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    assert(head_bytes == sizeof(expected) - 1U);
    assert(memcmp(head, expected, sizeof(expected) - 1U) == 0);
    assert(strstr((const char *)head, "Host:") == NULL);
    assert(strstr((const char *)head, "Accept-Encoding:") == NULL);
    assert(espdrop_airdrop_build_upload_head(
               head, 32U, &identity, 1U) == 0U);
    assert(espdrop_airdrop_build_upload_head(
               head, sizeof(head), &identity, 0U) == 0U);

    uint8_t framing[32];
    size_t framing_bytes = espdrop_airdrop_build_chunk_prefix(
        framing, sizeof(framing), 131072U);
    assert(framing_bytes == 7U);
    assert(memcmp(framing, "20000\r\n", 7U) == 0);
    assert(espdrop_airdrop_build_chunk_prefix(
               framing, sizeof(framing), 0U) == 0U);
    framing_bytes = espdrop_airdrop_build_chunk_terminator(
        framing, sizeof(framing));
    assert(framing_bytes == 5U);
    assert(memcmp(framing, "0\r\n\r\n", 5U) == 0);

    uint8_t dvzip[ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES];
    assert(espdrop_airdrop_build_dvzip_block_header(
        dvzip, 131072U, true));
    static const uint8_t expected_stored[] = {0x80, 0x02, 0x00, 0x00};
    assert(memcmp(dvzip, expected_stored, sizeof(dvzip)) == 0);
    assert(espdrop_airdrop_build_dvzip_block_header(
        dvzip, 0x1234U, false));
    static const uint8_t expected_zlib[] = {0x00, 0x00, 0x12, 0x34};
    assert(memcmp(dvzip, expected_zlib, sizeof(dvzip)) == 0);
    assert(!espdrop_airdrop_build_dvzip_block_header(dvzip, 0U, true));
    assert(!espdrop_airdrop_build_dvzip_block_header(
        dvzip, UINT32_C(0x80000000), true));

    puts("AirDrop upload tests passed");
    return 0;
}
