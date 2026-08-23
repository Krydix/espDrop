#include "espdrop/airdrop_ask.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/airdrop_upload.h"

#define ASK_OBJECT_COUNT 31U

typedef struct {
    uint8_t *output;
    size_t capacity;
    size_t length;
    bool failed;
} plist_writer_t;

static void write_bytes(plist_writer_t *writer, const void *value, size_t bytes)
{
    if (writer->failed || value == NULL || bytes > writer->capacity - writer->length) {
        writer->failed = true;
        return;
    }
    memcpy(writer->output + writer->length, value, bytes);
    writer->length += bytes;
}

static void write_byte(plist_writer_t *writer, uint8_t value)
{
    write_bytes(writer, &value, 1U);
}

static void write_be(plist_writer_t *writer, uint64_t value, size_t bytes)
{
    uint8_t encoded[8];
    for (size_t index = 0U; index < bytes; ++index) {
        encoded[bytes - index - 1U] = (uint8_t)(value >> (index * 8U));
    }
    write_bytes(writer, encoded, bytes);
}

static bool printable_ascii(const char *value, size_t minimum, size_t maximum)
{
    if (value == NULL) {
        return false;
    }
    const size_t length = strlen(value);
    if (length < minimum || length > maximum) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char byte = (unsigned char)value[index];
        if (byte < 0x20U || byte > 0x7eU) {
            return false;
        }
    }
    return true;
}

static bool sender_id_valid(const char *value)
{
    if (value == NULL || strlen(value) != 12U) {
        return false;
    }
    for (size_t index = 0U; index < 12U; ++index) {
        const char byte = value[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool file_name_valid(const char *value)
{
    if (!printable_ascii(value, 1U, 253U) || strcmp(value, ".") == 0 ||
        strcmp(value, "..") == 0) {
        return false;
    }
    return strchr(value, '/') == NULL && strchr(value, '\\') == NULL;
}

static bool ask_file_valid(const espdrop_airdrop_ask_file_t *file)
{
    return file != NULL &&
           printable_ascii(file->sender_computer_name, 1U, 127U) &&
           printable_ascii(file->sender_model_name, 1U, 127U) &&
           sender_id_valid(file->sender_id) &&
           espdrop_airdrop_transfer_id_valid(file->transfer_id) &&
           file_name_valid(file->file_name) &&
           printable_ascii(file->file_type, 1U, 127U);
}

static void begin_object(
    plist_writer_t *writer,
    size_t offsets[ASK_OBJECT_COUNT],
    size_t index)
{
    if (index >= ASK_OBJECT_COUNT) {
        writer->failed = true;
        return;
    }
    offsets[index] = writer->length;
}

static void write_ascii(plist_writer_t *writer, const char *value)
{
    const size_t length = strlen(value);
    if (length < 15U) {
        write_byte(writer, (uint8_t)(0x50U | length));
    } else {
        write_byte(writer, 0x5fU);
        write_byte(writer, 0x10U);
        write_byte(writer, (uint8_t)length);
    }
    write_bytes(writer, value, length);
}

static void write_refs(
    plist_writer_t *writer,
    const uint8_t *references,
    size_t count)
{
    write_bytes(writer, references, count);
}

static void write_trailer(
    plist_writer_t *writer,
    uint8_t offset_size,
    size_t offset_table)
{
    static const uint8_t reserved[6] = {0};
    write_bytes(writer, reserved, sizeof(reserved));
    write_byte(writer, offset_size);
    write_byte(writer, 1U);
    write_be(writer, ASK_OBJECT_COUNT, 8U);
    write_be(writer, 0U, 8U);
    write_be(writer, offset_table, 8U);
}

size_t espdrop_airdrop_build_ask_body(
    uint8_t *output,
    size_t capacity,
    const espdrop_airdrop_ask_file_t *file)
{
    if (output == NULL || capacity < 64U || !ask_file_valid(file)) {
        return 0U;
    }
    char bom_path[256];
    const int bom_length = snprintf(bom_path, sizeof(bom_path), "./%s",
                                    file->file_name);
    if (bom_length < 0 || (size_t)bom_length >= sizeof(bom_path)) {
        return 0U;
    }

    plist_writer_t writer = {
        .output = output,
        .capacity = capacity,
    };
    size_t offsets[ASK_OBJECT_COUNT] = {0};
    write_bytes(&writer, "bplist00", 8U);

    begin_object(&writer, offsets, 0U);
    write_byte(&writer, 0xd8U);
    static const uint8_t root_keys[] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const uint8_t root_values[] = {9, 10, 11, 12, 13, 14, 15, 16};
    write_refs(&writer, root_keys, sizeof(root_keys));
    write_refs(&writer, root_values, sizeof(root_values));

    static const char *const top_keys[] = {
        "BundleID", "ConvertMediaFormats", "Files", "SenderComputerName",
        "SenderID", "SenderModelName", "TransferID", "TransferType",
    };
    for (size_t index = 0U; index < 8U; ++index) {
        begin_object(&writer, offsets, index + 1U);
        write_ascii(&writer, top_keys[index]);
    }

    begin_object(&writer, offsets, 9U);
    write_ascii(&writer, "com.apple.finder");
    begin_object(&writer, offsets, 10U);
    write_byte(&writer, 0x08U);
    begin_object(&writer, offsets, 11U);
    write_byte(&writer, 0xa1U);
    write_byte(&writer, 17U);
    begin_object(&writer, offsets, 12U);
    write_ascii(&writer, file->sender_computer_name);
    begin_object(&writer, offsets, 13U);
    write_ascii(&writer, file->sender_id);
    begin_object(&writer, offsets, 14U);
    write_ascii(&writer, file->sender_model_name);
    begin_object(&writer, offsets, 15U);
    write_byte(&writer, 0xd1U);
    write_byte(&writer, 18U);
    write_byte(&writer, 19U);
    begin_object(&writer, offsets, 16U);
    write_byte(&writer, 0xd1U);
    write_byte(&writer, 20U);
    write_byte(&writer, 21U);

    begin_object(&writer, offsets, 17U);
    write_byte(&writer, 0xd5U);
    static const uint8_t file_keys[] = {22, 23, 24, 25, 26};
    static const uint8_t file_values[] = {27, 28, 10, 29, 30};
    write_refs(&writer, file_keys, sizeof(file_keys));
    write_refs(&writer, file_values, sizeof(file_values));
    begin_object(&writer, offsets, 18U);
    write_ascii(&writer, "id");
    begin_object(&writer, offsets, 19U);
    write_ascii(&writer, file->transfer_id);
    begin_object(&writer, offsets, 20U);
    write_ascii(&writer, "files");
    begin_object(&writer, offsets, 21U);
    write_byte(&writer, 0xd0U);

    static const char *const entry_keys[] = {
        "ConvertMediaFormats", "FileBomPath", "FileIsDirectory", "FileName",
        "FileType",
    };
    for (size_t index = 0U; index < 5U; ++index) {
        begin_object(&writer, offsets, index + 22U);
        write_ascii(&writer, entry_keys[index]);
    }
    begin_object(&writer, offsets, 27U);
    write_byte(&writer, 0x10U);
    write_byte(&writer, 0U);
    begin_object(&writer, offsets, 28U);
    write_ascii(&writer, bom_path);
    begin_object(&writer, offsets, 29U);
    write_ascii(&writer, file->file_name);
    begin_object(&writer, offsets, 30U);
    write_ascii(&writer, file->file_type);

    if (writer.failed) {
        return 0U;
    }
    const size_t offset_table = writer.length;
    const uint8_t offset_size = offset_table <= UINT8_MAX ? 1U : 2U;
    for (size_t index = 0U; index < ASK_OBJECT_COUNT; ++index) {
        write_be(&writer, offsets[index], offset_size);
    }
    write_trailer(&writer, offset_size, offset_table);
    return writer.failed ? 0U : writer.length;
}

size_t espdrop_airdrop_build_ask_request(
    uint8_t *output,
    size_t capacity,
    const char *host,
    uint16_t port,
    const uint8_t *body,
    size_t body_bytes)
{
    if (output == NULL || host == NULL || host[0] == '\0' || port == 0U ||
        body == NULL || body_bytes == 0U) {
        return 0U;
    }
    const int header_length = snprintf(
        (char *)output, capacity,
        "POST /Ask HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Connection: keep-alive\r\n"
        "Accept: */*\r\n"
        "User-Agent: AirDrop/1.0\r\n"
        "Accept-Language: en-us\r\n"
        "Accept-Encoding: br, gzip, deflate\r\n"
        "Content-Length: %zu\r\n\r\n",
        host, (unsigned)port, body_bytes);
    if (header_length < 0 || (size_t)header_length >= capacity ||
        body_bytes > capacity - (size_t)header_length) {
        return 0U;
    }
    memcpy(output + (size_t)header_length, body, body_bytes);
    return (size_t)header_length + body_bytes;
}

void espdrop_airdrop_format_transfer_id(
    char output[37],
    const uint8_t random_bytes[16])
{
    uint8_t uuid[16];
    memcpy(uuid, random_bytes, sizeof(uuid));
    uuid[6] = (uint8_t)((uuid[6] & 0x0fU) | 0x40U);
    uuid[8] = (uint8_t)((uuid[8] & 0x3fU) | 0x80U);
    (void)snprintf(
        output, 37U,
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
        "%02X%02X%02X%02X%02X%02X",
        uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
        uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
        uuid[14], uuid[15]);
}

void espdrop_airdrop_format_sender_id(
    char output[ESPDROP_AIRDROP_SENDER_ID_BYTES],
    const uint8_t random_bytes[6])
{
    (void)snprintf(output, ESPDROP_AIRDROP_SENDER_ID_BYTES,
                   "%02x%02x%02x%02x%02x%02x", random_bytes[0],
                   random_bytes[1], random_bytes[2], random_bytes[3],
                   random_bytes[4], random_bytes[5]);
}
