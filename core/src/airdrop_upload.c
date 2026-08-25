#include "espdrop/airdrop_upload.h"

#include <stdio.h>
#include <string.h>

#define ODC_FILE_SIZE_MAX UINT64_C(077777777777)

static bool is_upper_hex(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'F');
}

bool espdrop_airdrop_transfer_id_valid(const char *value)
{
    if (value == NULL || strlen(value) != 36U) {
        return false;
    }
    for (size_t index = 0U; index < 36U; ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') {
                return false;
            }
        } else if (!is_upper_hex(value[index])) {
            return false;
        }
    }
    return true;
}

static bool pseudonym_valid(const char *value)
{
    if (value == NULL || strlen(value) != 28U ||
        memcmp(value, "pseud:", 6U) != 0) {
        return false;
    }
    for (size_t index = 6U; index < 28U; ++index) {
        const char byte = value[index];
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '-' || byte == '_')) {
            return false;
        }
    }
    return true;
}

static bool push_token_valid(const char *value)
{
    if (value == NULL || strlen(value) != 64U) {
        return false;
    }
    for (size_t index = 0U; index < 64U; ++index) {
        if (!is_upper_hex(value[index])) {
            return false;
        }
    }
    return true;
}

bool espdrop_airdrop_upload_identity_valid(
    const espdrop_airdrop_upload_identity_t *identity)
{
    return identity != NULL &&
           espdrop_airdrop_transfer_id_valid(identity->transfer_id) &&
           pseudonym_valid(identity->sender_pseudonym) &&
           push_token_valid(identity->sender_push_token);
}

void espdrop_airdrop_format_sender_pseudonym(
    char output[ESPDROP_AIRDROP_SENDER_PSEUDONYM_BYTES],
    const uint8_t random_bytes[16])
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    memcpy(output, "pseud:", 6U);
    size_t input = 0U;
    size_t encoded = 6U;
    while (input + 3U <= 16U) {
        const uint32_t value = (uint32_t)random_bytes[input] << 16U |
                               (uint32_t)random_bytes[input + 1U] << 8U |
                               random_bytes[input + 2U];
        output[encoded++] = alphabet[(value >> 18U) & 0x3fU];
        output[encoded++] = alphabet[(value >> 12U) & 0x3fU];
        output[encoded++] = alphabet[(value >> 6U) & 0x3fU];
        output[encoded++] = alphabet[value & 0x3fU];
        input += 3U;
    }
    const uint32_t tail = (uint32_t)random_bytes[input] << 16U;
    output[encoded++] = alphabet[(tail >> 18U) & 0x3fU];
    output[encoded++] = alphabet[(tail >> 12U) & 0x3fU];
    output[encoded] = '\0';
}

void espdrop_airdrop_format_sender_push_token(
    char output[ESPDROP_AIRDROP_SENDER_PUSH_TOKEN_BYTES],
    const uint8_t random_bytes[32])
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t index = 0U; index < 32U; ++index) {
        output[index * 2U] = hex[random_bytes[index] >> 4U];
        output[index * 2U + 1U] = hex[random_bytes[index] & 0x0fU];
    }
    output[64] = '\0';
}

static bool archive_path_valid(const char *path)
{
    if (path == NULL || path[0] != '.' || path[1] != '/') {
        return false;
    }
    const size_t length = strlen(path);
    if (length < 3U || length > 255U || strchr(path + 2U, '/') != NULL ||
        strchr(path, '\\') != NULL) {
        return false;
    }
    for (size_t index = 2U; index < length; ++index) {
        const unsigned char byte = (unsigned char)path[index];
        if (byte < 0x20U || byte > 0x7eU) {
            return false;
        }
    }
    return strcmp(path + 2U, ".") != 0 && strcmp(path + 2U, "..") != 0;
}

static bool write_odc_header(
    uint8_t output[ESPDROP_AIRDROP_ODC_HEADER_BYTES],
    unsigned inode,
    unsigned mode,
    uint32_t mtime,
    size_t name_bytes,
    size_t file_bytes)
{
    if (name_bytes == 0U || name_bytes > UINT32_C(0777777)) {
        return false;
    }
#if SIZE_MAX > UINT32_MAX
    if ((uint64_t)file_bytes > ODC_FILE_SIZE_MAX) {
        return false;
    }
#endif
    char header[ESPDROP_AIRDROP_ODC_HEADER_BYTES + 1U];
    const int length = snprintf(
        header, sizeof(header),
        "070707%06o%06o%06o%06o%06o%06o%06o%011lo%06lo%011llo",
        0U, inode, mode, 0U, 0U, 1U, 0U, (unsigned long)mtime,
        (unsigned long)name_bytes, (unsigned long long)file_bytes);
    if (length != (int)ESPDROP_AIRDROP_ODC_HEADER_BYTES) {
        return false;
    }
    memcpy(output, header, ESPDROP_AIRDROP_ODC_HEADER_BYTES);
    return true;
}

size_t espdrop_airdrop_build_odc_archive(
    uint8_t *output,
    size_t capacity,
    const char *archive_path,
    const uint8_t *file_data,
    size_t file_bytes,
    uint32_t mtime)
{
    static const char trailer[] = "TRAILER!!!";
    if (output == NULL || !archive_path_valid(archive_path) ||
        (file_bytes > 0U && file_data == NULL)) {
        return 0U;
    }
    const size_t path_bytes = strlen(archive_path) + 1U;
    const size_t trailer_bytes = sizeof(trailer);
    const size_t fixed_bytes = ESPDROP_AIRDROP_ODC_HEADER_BYTES * 2U +
                               path_bytes + trailer_bytes;
    if (file_bytes > SIZE_MAX - fixed_bytes) {
        return 0U;
    }
    const size_t unpadded = fixed_bytes + file_bytes;
    if (unpadded > SIZE_MAX - (ESPDROP_AIRDROP_ODC_BLOCK_BYTES - 1U)) {
        return 0U;
    }
    const size_t archive_bytes =
        ((unpadded + ESPDROP_AIRDROP_ODC_BLOCK_BYTES - 1U) /
         ESPDROP_AIRDROP_ODC_BLOCK_BYTES) *
        ESPDROP_AIRDROP_ODC_BLOCK_BYTES;
    if (archive_bytes > capacity) {
        return 0U;
    }
    size_t offset = 0U;
    if (!write_odc_header(output + offset, 1U, 0100644U, mtime,
                          path_bytes, file_bytes)) {
        return 0U;
    }
    offset += ESPDROP_AIRDROP_ODC_HEADER_BYTES;
    memcpy(output + offset, archive_path, path_bytes);
    offset += path_bytes;
    if (file_bytes > 0U) {
        memcpy(output + offset, file_data, file_bytes);
        offset += file_bytes;
    }
    if (!write_odc_header(output + offset, 0U, 0U, 0U, trailer_bytes, 0U)) {
        return 0U;
    }
    offset += ESPDROP_AIRDROP_ODC_HEADER_BYTES;
    memcpy(output + offset, trailer, trailer_bytes);
    offset += trailer_bytes;
    memset(output + offset, 0, archive_bytes - offset);
    return archive_bytes;
}

bool espdrop_airdrop_plan_stored_dvzip(
    espdrop_airdrop_stream_plan_t *plan,
    const char *archive_path,
    size_t file_bytes)
{
    static const char trailer[] = "TRAILER!!!";
    if (plan == NULL || !archive_path_valid(archive_path)) {
        return false;
    }
#if SIZE_MAX > UINT32_MAX
    if ((uint64_t)file_bytes > ODC_FILE_SIZE_MAX) {
        return false;
    }
#endif
    const size_t path_bytes = strlen(archive_path) + 1U;
    const size_t fixed_bytes = ESPDROP_AIRDROP_ODC_HEADER_BYTES * 2U +
                               path_bytes + sizeof(trailer);
    if (file_bytes > SIZE_MAX - fixed_bytes) {
        return false;
    }
    const size_t unpadded = fixed_bytes + file_bytes;
    if (unpadded > SIZE_MAX - (ESPDROP_AIRDROP_ODC_BLOCK_BYTES - 1U)) {
        return false;
    }
    const size_t archive_bytes =
        ((unpadded + ESPDROP_AIRDROP_ODC_BLOCK_BYTES - 1U) /
         ESPDROP_AIRDROP_ODC_BLOCK_BYTES) *
        ESPDROP_AIRDROP_ODC_BLOCK_BYTES;
    const size_t blocks =
        (archive_bytes + ESPDROP_AIRDROP_DVZIP_STREAM_BLOCK_BYTES - 1U) /
        ESPDROP_AIRDROP_DVZIP_STREAM_BLOCK_BYTES;
    if (blocks > (SIZE_MAX - archive_bytes) /
                     ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES) {
        return false;
    }
    *plan = (espdrop_airdrop_stream_plan_t){
        .file_bytes = file_bytes,
        .archive_bytes = archive_bytes,
        .dvzip_blocks = blocks,
        .payload_bytes = archive_bytes +
            blocks * ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES,
    };
    return true;
}

bool espdrop_airdrop_should_use_single_zlib_block(
    size_t archive_bytes,
    size_t compressed_bytes)
{
    return archive_bytes > 0U && compressed_bytes > 0U &&
           compressed_bytes < archive_bytes &&
           compressed_bytes <= ESPDROP_AIRDROP_DVZIP_STREAM_BLOCK_BYTES;
}

typedef struct {
    espdrop_airdrop_stream_write_t write;
    void *context;
    size_t archive_remaining;
    size_t block_remaining;
    espdrop_airdrop_stream_result_t *result;
} stored_dvzip_writer_t;

static bool write_stored_archive_bytes(
    stored_dvzip_writer_t *writer,
    const uint8_t *data,
    size_t data_bytes)
{
    while (data_bytes > 0U) {
        if (writer->archive_remaining == 0U) {
            return false;
        }
        if (writer->block_remaining == 0U) {
            writer->block_remaining =
                writer->archive_remaining <
                        ESPDROP_AIRDROP_DVZIP_STREAM_BLOCK_BYTES
                    ? writer->archive_remaining
                    : ESPDROP_AIRDROP_DVZIP_STREAM_BLOCK_BYTES;
            uint8_t header[ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES];
            if (!espdrop_airdrop_build_dvzip_block_header(
                    header, (uint32_t)writer->block_remaining, true) ||
                !writer->write(writer->context, header, sizeof(header))) {
                return false;
            }
            ++writer->result->dvzip_blocks;
            writer->result->payload_bytes += sizeof(header);
        }
        size_t portion = data_bytes;
        if (portion > writer->block_remaining) {
            portion = writer->block_remaining;
        }
        if (portion > writer->archive_remaining ||
            !writer->write(writer->context, data, portion)) {
            return false;
        }
        data += portion;
        data_bytes -= portion;
        writer->block_remaining -= portion;
        writer->archive_remaining -= portion;
        writer->result->archive_bytes += portion;
        writer->result->payload_bytes += portion;
    }
    return true;
}

static uint32_t crc32_update(
    uint32_t crc,
    const uint8_t *data,
    size_t data_bytes)
{
    for (size_t index = 0U; index < data_bytes; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc;
}

typedef struct {
    espdrop_airdrop_stream_write_t write;
    void *context;
    size_t remaining;
    size_t emitted;
} odc_writer_t;

static bool write_odc_bytes(
    odc_writer_t *writer,
    const uint8_t *data,
    size_t data_bytes)
{
    if (data_bytes > writer->remaining ||
        !writer->write(writer->context, data, data_bytes)) {
        return false;
    }
    writer->remaining -= data_bytes;
    writer->emitted += data_bytes;
    return true;
}

espdrop_airdrop_stream_status_t espdrop_airdrop_stream_odc(
    const espdrop_airdrop_source_t *source,
    const char *archive_path,
    uint32_t mtime,
    uint8_t *workspace,
    size_t workspace_bytes,
    espdrop_airdrop_stream_write_t write,
    void *write_context,
    espdrop_airdrop_stream_result_t *result)
{
    if (source == NULL || source->read == NULL || workspace == NULL ||
        workspace_bytes == 0U || write == NULL || result == NULL) {
        return ESPDROP_AIRDROP_STREAM_INVALID;
    }
    memset(result, 0, sizeof(*result));
    espdrop_airdrop_stream_plan_t plan;
    if (!espdrop_airdrop_plan_stored_dvzip(
            &plan, archive_path, source->size_bytes)) {
        return ESPDROP_AIRDROP_STREAM_SIZE;
    }

    odc_writer_t writer = {
        .write = write,
        .context = write_context,
        .remaining = plan.archive_bytes,
    };
    uint8_t header[ESPDROP_AIRDROP_ODC_HEADER_BYTES];
    const size_t path_bytes = strlen(archive_path) + 1U;
    if (!write_odc_header(header, 1U, 0100644U, mtime, path_bytes,
                          source->size_bytes) ||
        !write_odc_bytes(&writer, header, sizeof(header)) ||
        !write_odc_bytes(
            &writer, (const uint8_t *)archive_path, path_bytes)) {
        return ESPDROP_AIRDROP_STREAM_SINK;
    }

    size_t remaining = source->size_bytes;
    uint32_t crc = UINT32_MAX;
    while (remaining > 0U) {
        const size_t requested =
            remaining < workspace_bytes ? remaining : workspace_bytes;
        size_t buffered = 0U;
        while (buffered < requested) {
            size_t bytes_read = 0U;
            if (!source->read(
                    source->context, workspace + buffered,
                    requested - buffered, &bytes_read) ||
                bytes_read > requested - buffered) {
                return ESPDROP_AIRDROP_STREAM_SOURCE;
            }
            if (bytes_read == 0U) {
                return ESPDROP_AIRDROP_STREAM_TRUNCATED;
            }
            buffered += bytes_read;
        }
        crc = crc32_update(crc, workspace, buffered);
        remaining -= buffered;
        result->source_bytes += buffered;
        result->source_crc32 = crc ^ UINT32_MAX;
        if (buffered > result->workspace_high_water) {
            result->workspace_high_water = buffered;
        }
        if (!write_odc_bytes(&writer, workspace, buffered)) {
            return ESPDROP_AIRDROP_STREAM_SINK;
        }
    }

    static const char trailer[] = "TRAILER!!!";
    if (!write_odc_header(header, 0U, 0U, 0U, sizeof(trailer), 0U) ||
        !write_odc_bytes(&writer, header, sizeof(header)) ||
        !write_odc_bytes(
            &writer, (const uint8_t *)trailer, sizeof(trailer))) {
        return ESPDROP_AIRDROP_STREAM_SINK;
    }
    /* The archive tail can be almost 10 KiB. Reuse the caller's workspace so
     * a relay does not turn that padding into hundreds of tiny TLS records. */
    memset(workspace, 0, workspace_bytes);
    while (writer.remaining > 0U) {
        const size_t bytes = writer.remaining < workspace_bytes
                                 ? writer.remaining
                                 : workspace_bytes;
        if (!write_odc_bytes(&writer, workspace, bytes)) {
            return ESPDROP_AIRDROP_STREAM_SINK;
        }
    }
    result->archive_bytes = writer.emitted;
    result->payload_bytes = writer.emitted;
    if (result->source_bytes != plan.file_bytes ||
        result->archive_bytes != plan.archive_bytes) {
        return ESPDROP_AIRDROP_STREAM_SIZE;
    }
    return ESPDROP_AIRDROP_STREAM_OK;
}

static bool write_stored_archive_sink(
    void *context,
    const uint8_t *data,
    size_t data_bytes)
{
    return write_stored_archive_bytes(context, data, data_bytes);
}

espdrop_airdrop_stream_status_t espdrop_airdrop_stream_stored_dvzip(
    const espdrop_airdrop_source_t *source,
    const char *archive_path,
    uint32_t mtime,
    uint8_t *workspace,
    size_t workspace_bytes,
    espdrop_airdrop_stream_write_t write,
    void *write_context,
    espdrop_airdrop_stream_result_t *result)
{
    if (source == NULL || source->read == NULL || workspace == NULL ||
        workspace_bytes == 0U || write == NULL || result == NULL) {
        return ESPDROP_AIRDROP_STREAM_INVALID;
    }
    memset(result, 0, sizeof(*result));
    espdrop_airdrop_stream_plan_t plan;
    if (!espdrop_airdrop_plan_stored_dvzip(
            &plan, archive_path, source->size_bytes)) {
        return ESPDROP_AIRDROP_STREAM_SIZE;
    }
    stored_dvzip_writer_t writer = {
        .write = write,
        .context = write_context,
        .archive_remaining = plan.archive_bytes,
        .result = result,
    };
    espdrop_airdrop_stream_result_t archive;
    const espdrop_airdrop_stream_status_t status = espdrop_airdrop_stream_odc(
        source, archive_path, mtime, workspace, workspace_bytes,
        write_stored_archive_sink, &writer, &archive);
    result->source_bytes = archive.source_bytes;
    result->workspace_high_water = archive.workspace_high_water;
    result->source_crc32 = archive.source_crc32;
    if (status != ESPDROP_AIRDROP_STREAM_OK) {
        return status;
    }
    if (writer.archive_remaining != 0U || writer.block_remaining != 0U ||
        result->archive_bytes != plan.archive_bytes ||
        result->dvzip_blocks != plan.dvzip_blocks ||
        result->payload_bytes != plan.payload_bytes) {
        return ESPDROP_AIRDROP_STREAM_SIZE;
    }
    return ESPDROP_AIRDROP_STREAM_OK;
}

size_t espdrop_airdrop_build_upload_head(
    uint8_t *output,
    size_t capacity,
    const espdrop_airdrop_upload_identity_t *identity,
    size_t total_bytes)
{
    if (output == NULL || capacity == 0U || total_bytes == 0U ||
        !espdrop_airdrop_upload_identity_valid(identity)) {
        return 0U;
    }
    const int length = snprintf(
        (char *)output, capacity,
        "POST /Upload HTTP/1.1\r\n"
        "User-Agent: AirDrop/1.0\r\n"
        "TotalBytes: %zu\r\n"
        "Content-Type: application/x-dvzip\r\n"
        "SenderPseudonym: %s\r\n"
        "SenderPushToken: %s\r\n"
        "TransferID: %s\r\n"
        "Connection: keep-alive\r\n"
        "Transfer-Encoding: chunked\r\n\r\n",
        total_bytes, identity->sender_pseudonym,
        identity->sender_push_token, identity->transfer_id);
    return length < 0 || (size_t)length >= capacity ? 0U : (size_t)length;
}

size_t espdrop_airdrop_build_chunk_prefix(
    uint8_t *output,
    size_t capacity,
    size_t payload_bytes)
{
    if (output == NULL || capacity == 0U || payload_bytes == 0U) {
        return 0U;
    }
    const int length = snprintf((char *)output, capacity, "%zx\r\n",
                                payload_bytes);
    return length < 0 || (size_t)length >= capacity ? 0U : (size_t)length;
}

size_t espdrop_airdrop_build_chunk_terminator(
    uint8_t *output,
    size_t capacity)
{
    static const uint8_t terminator[] = {'0', '\r', '\n', '\r', '\n'};
    if (output == NULL || capacity < sizeof(terminator)) {
        return 0U;
    }
    memcpy(output, terminator, sizeof(terminator));
    return sizeof(terminator);
}

bool espdrop_airdrop_build_dvzip_block_header(
    uint8_t output[ESPDROP_AIRDROP_DVZIP_BLOCK_HEADER_BYTES],
    uint32_t payload_bytes,
    bool stored)
{
    if (output == NULL || payload_bytes == 0U ||
        payload_bytes > UINT32_C(0x7fffffff)) {
        return false;
    }
    const uint32_t encoded = payload_bytes |
        (stored ? UINT32_C(0x80000000) : UINT32_C(0));
    output[0] = (uint8_t)(encoded >> 24U);
    output[1] = (uint8_t)(encoded >> 16U);
    output[2] = (uint8_t)(encoded >> 8U);
    output[3] = (uint8_t)encoded;
    return true;
}
