#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espdrop/airdrop_upload.h"

static const espdrop_airdrop_upload_identity_t identity = {
    .transfer_id = "664D3979-F245-4E9E-9EAC-80453E255E31",
    .sender_pseudonym = "pseud:RDUDcHgUEfGhmdJSNF0FmQ",
    .sender_push_token =
        "1D0DD878B679E3AA7C9EC13EC596983FA9CF05E3AAEA25F978B5B14D4AB50493",
};

static const uint8_t lab_jpeg[] = {
#include "airdrop_lab_jpeg.inc"
};

static bool contains_bytes(
    const uint8_t *value,
    size_t value_bytes,
    const void *needle,
    size_t needle_bytes)
{
    for (size_t offset = 0U; offset + needle_bytes <= value_bytes; ++offset) {
        if (memcmp(value + offset, needle, needle_bytes) == 0) {
            return true;
        }
    }
    return false;
}

typedef struct {
    const uint8_t *data;
    size_t data_bytes;
    size_t offset;
    size_t max_chunk;
    bool fail;
} memory_source_t;

static bool read_memory_source(
    void *context,
    uint8_t *output,
    size_t capacity,
    size_t *bytes_read)
{
    memory_source_t *source = context;
    if (source->fail) {
        return false;
    }
    size_t available = source->data_bytes - source->offset;
    size_t bytes = available < capacity ? available : capacity;
    if (source->max_chunk > 0U && bytes > source->max_chunk) {
        bytes = source->max_chunk;
    }
    if (bytes > 0U) {
        memcpy(output, source->data + source->offset, bytes);
        source->offset += bytes;
    }
    *bytes_read = bytes;
    return true;
}

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t bytes;
    size_t fail_after;
} memory_sink_t;

static bool write_memory_sink(
    void *context,
    const uint8_t *data,
    size_t data_bytes)
{
    memory_sink_t *sink = context;
    if (data_bytes > sink->capacity - sink->bytes ||
        (sink->fail_after > 0U &&
         sink->bytes + data_bytes > sink->fail_after)) {
        return false;
    }
    memcpy(sink->data + sink->bytes, data, data_bytes);
    sink->bytes += data_bytes;
    return true;
}

static size_t decode_stored_dvzip(
    const uint8_t *payload,
    size_t payload_bytes,
    uint8_t *archive,
    size_t archive_capacity,
    size_t *block_count)
{
    size_t input = 0U;
    size_t output = 0U;
    *block_count = 0U;
    while (input < payload_bytes) {
        assert(payload_bytes - input >= 4U);
        const uint32_t header = (uint32_t)payload[input] << 24U |
                                (uint32_t)payload[input + 1U] << 16U |
                                (uint32_t)payload[input + 2U] << 8U |
                                payload[input + 3U];
        input += 4U;
        assert((header & UINT32_C(0x80000000)) != 0U);
        const size_t block_bytes = header & UINT32_C(0x7fffffff);
        assert(block_bytes > 0U && block_bytes <= payload_bytes - input);
        assert(block_bytes <= archive_capacity - output);
        memcpy(archive + output, payload + input, block_bytes);
        input += block_bytes;
        output += block_bytes;
        ++*block_count;
    }
    return output;
}

int main(int argc, char **argv)
{
    static const uint8_t jpeg_sof0[] = {
        0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x20, 0x00, 0x20,
    };
    assert(sizeof(lab_jpeg) == 445U);
    assert(lab_jpeg[0] == 0xffU && lab_jpeg[1] == 0xd8U);
    assert(lab_jpeg[sizeof(lab_jpeg) - 2U] == 0xffU &&
           lab_jpeg[sizeof(lab_jpeg) - 1U] == 0xd9U);
    assert(contains_bytes(
        lab_jpeg, sizeof(lab_jpeg), jpeg_sof0, sizeof(jpeg_sof0)));

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

    uint8_t pseudonym_random[16];
    uint8_t token_random[32];
    for (size_t index = 0U; index < sizeof(pseudonym_random); ++index) {
        pseudonym_random[index] = (uint8_t)index;
    }
    for (size_t index = 0U; index < sizeof(token_random); ++index) {
        token_random[index] = (uint8_t)index;
    }
    char pseudonym[ESPDROP_AIRDROP_SENDER_PSEUDONYM_BYTES];
    char token[ESPDROP_AIRDROP_SENDER_PUSH_TOKEN_BYTES];
    espdrop_airdrop_format_sender_pseudonym(pseudonym, pseudonym_random);
    espdrop_airdrop_format_sender_push_token(token, token_random);
    assert(strcmp(pseudonym, "pseud:AAECAwQFBgcICQoLDA0ODw") == 0);
    assert(strcmp(token,
                  "000102030405060708090A0B0C0D0E0F"
                  "101112131415161718191A1B1C1D1E1F") == 0);

    static const uint8_t jpeg[] = {
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46,
        0x49, 0x46, 0x00, 0x01, 0xff, 0xd9,
    };
    uint8_t archive[ESPDROP_AIRDROP_ODC_BLOCK_BYTES];
    const size_t archive_bytes = espdrop_airdrop_build_odc_archive(
        archive, sizeof(archive), "./hello.jpg", jpeg, sizeof(jpeg), 0U);
    assert(archive_bytes == ESPDROP_AIRDROP_ODC_BLOCK_BYTES);
    assert(memcmp(archive, "070707", 6U) == 0);
    assert(contains_bytes(
        archive, archive_bytes, "./hello.jpg\0", 12U));
    assert(contains_bytes(
        archive, archive_bytes, "TRAILER!!!\0", 11U));
    assert(espdrop_airdrop_build_odc_archive(
               archive, sizeof(archive), "../hello.jpg", jpeg,
               sizeof(jpeg), 0U) == 0U);
    assert(espdrop_airdrop_build_odc_archive(
               archive, 128U, "./hello.jpg", jpeg,
               sizeof(jpeg), 0U) == 0U);

    espdrop_airdrop_stream_plan_t plan;
    assert(espdrop_airdrop_plan_stored_dvzip(
        &plan, "./hello.jpg", sizeof(jpeg)));
    assert(plan.file_bytes == sizeof(jpeg));
    assert(plan.archive_bytes == ESPDROP_AIRDROP_ODC_BLOCK_BYTES);
    assert(plan.dvzip_blocks == 1U);
    assert(plan.payload_bytes == ESPDROP_AIRDROP_ODC_BLOCK_BYTES + 4U);
    uint8_t streamed[ESPDROP_AIRDROP_ODC_BLOCK_BYTES + 4U];
    uint8_t workspace[7];
    memory_source_t small_source = {
        .data = jpeg,
        .data_bytes = sizeof(jpeg),
        .max_chunk = 3U,
    };
    const espdrop_airdrop_source_t source = {
        .context = &small_source,
        .size_bytes = sizeof(jpeg),
        .read = read_memory_source,
    };
    memory_sink_t small_sink = {
        .data = streamed,
        .capacity = sizeof(streamed),
    };
    espdrop_airdrop_stream_result_t stream_result;
    assert(espdrop_airdrop_stream_stored_dvzip(
               &source, "./hello.jpg", 0U, workspace, sizeof(workspace),
               write_memory_sink, &small_sink, &stream_result) ==
           ESPDROP_AIRDROP_STREAM_OK);
    assert(stream_result.source_bytes == sizeof(jpeg));
    assert(stream_result.archive_bytes == plan.archive_bytes);
    assert(stream_result.dvzip_blocks == plan.dvzip_blocks);
    assert(stream_result.payload_bytes == plan.payload_bytes);
    assert(stream_result.workspace_high_water == sizeof(workspace));
    assert(stream_result.source_crc32 == UINT32_C(0xadb75530));
    assert(streamed[0] == 0x80U && streamed[1] == 0x00U &&
           streamed[2] == 0x28U && streamed[3] == 0x00U);
    assert(memcmp(streamed + 4U, archive, sizeof(archive)) == 0);

    const size_t large_file_bytes = 180000U;
    uint8_t *large_file = malloc(large_file_bytes);
    assert(large_file != NULL);
    for (size_t index = 0U; index < large_file_bytes; ++index) {
        large_file[index] = (uint8_t)(index * 37U + 11U);
    }
    assert(espdrop_airdrop_plan_stored_dvzip(
        &plan, "./relay.bin", large_file_bytes));
    assert(plan.dvzip_blocks == 2U);
    uint8_t *large_payload = malloc(plan.payload_bytes);
    uint8_t *large_archive = malloc(plan.archive_bytes);
    assert(large_payload != NULL && large_archive != NULL);
    memory_source_t large_source = {
        .data = large_file,
        .data_bytes = large_file_bytes,
        .max_chunk = 31U,
    };
    espdrop_airdrop_source_t large_descriptor = {
        .context = &large_source,
        .size_bytes = large_file_bytes,
        .read = read_memory_source,
    };
    memory_sink_t large_sink = {
        .data = large_payload,
        .capacity = plan.payload_bytes,
    };
    assert(espdrop_airdrop_stream_stored_dvzip(
               &large_descriptor, "./relay.bin", 123U, workspace,
               sizeof(workspace), write_memory_sink, &large_sink,
               &stream_result) == ESPDROP_AIRDROP_STREAM_OK);
    assert(large_sink.bytes == plan.payload_bytes);
    size_t decoded_blocks = 0U;
    assert(decode_stored_dvzip(
               large_payload, large_sink.bytes, large_archive,
               plan.archive_bytes, &decoded_blocks) == plan.archive_bytes);
    assert(decoded_blocks == 2U);
    assert(memcmp(large_archive, "070707", 6U) == 0);
    assert(memcmp(large_archive + ESPDROP_AIRDROP_ODC_HEADER_BYTES,
                  "./relay.bin\0", 12U) == 0);
    assert(memcmp(
        large_archive + ESPDROP_AIRDROP_ODC_HEADER_BYTES + 12U,
        large_file, large_file_bytes) == 0);

    memory_source_t truncated_source = {
        .data = jpeg,
        .data_bytes = sizeof(jpeg),
    };
    espdrop_airdrop_source_t truncated_descriptor = {
        .context = &truncated_source,
        .size_bytes = sizeof(jpeg) + 1U,
        .read = read_memory_source,
    };
    memory_sink_t discard_sink = {
        .data = large_payload,
        .capacity = plan.payload_bytes,
    };
    assert(espdrop_airdrop_stream_stored_dvzip(
               &truncated_descriptor, "./bad.bin", 0U, workspace,
               sizeof(workspace), write_memory_sink, &discard_sink,
               &stream_result) == ESPDROP_AIRDROP_STREAM_TRUNCATED);
    memory_source_t failed_source = {.fail = true};
    espdrop_airdrop_source_t failed_descriptor = {
        .context = &failed_source,
        .size_bytes = 1U,
        .read = read_memory_source,
    };
    assert(espdrop_airdrop_stream_stored_dvzip(
               &failed_descriptor, "./bad.bin", 0U, workspace,
               sizeof(workspace), write_memory_sink, &discard_sink,
               &stream_result) == ESPDROP_AIRDROP_STREAM_SOURCE);
    small_source.offset = 0U;
    small_sink.bytes = 0U;
    small_sink.fail_after = 100U;
    assert(espdrop_airdrop_stream_stored_dvzip(
               &source, "./hello.jpg", 0U, workspace, sizeof(workspace),
               write_memory_sink, &small_sink, &stream_result) ==
           ESPDROP_AIRDROP_STREAM_SINK);
    free(large_archive);
    free(large_payload);
    free(large_file);

    if (argc >= 2) {
        FILE *fixture = fopen(argv[1], "wb");
        assert(fixture != NULL);
        assert(fwrite(archive, 1U, archive_bytes, fixture) == archive_bytes);
        assert(fclose(fixture) == 0);
    }
    if (argc >= 3) {
        FILE *fixture = fopen(argv[2], "wb");
        assert(fixture != NULL);
        assert(fwrite(streamed, 1U, sizeof(streamed), fixture) ==
               sizeof(streamed));
        assert(fclose(fixture) == 0);
    }

    puts("AirDrop upload tests passed");
    return 0;
}
