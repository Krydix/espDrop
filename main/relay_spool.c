#include "relay_spool.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"

#define RELAY_SPOOL_PARTITION_LABEL "storage"
#define RELAY_SPOOL_DATA_OFFSET 4096U
#define RELAY_SPOOL_HEADER_VERSION 1U

typedef struct {
    uint8_t magic[8];
    uint32_t version;
    uint32_t size_bytes;
    uint32_t payload_crc32;
    char file_name[RELAY_SPOOL_FILE_NAME_MAX + 1U];
    char file_type[RELAY_SPOOL_FILE_TYPE_MAX + 1U];
    uint32_t header_crc32;
} relay_spool_header_t;

typedef struct {
    const esp_partition_t *partition;
    relay_spool_header_t header;
    size_t cursor;
    size_t write_cursor;
    uint32_t write_crc32;
    bool writing;
    bool ready;
} relay_spool_state_t;

static const char *TAG = "relay_spool";
static const uint8_t spool_magic[8] = {'E', 'S', 'P', 'D', 'R', 'L', 'Y', '1'};
static relay_spool_state_t state;

_Static_assert(sizeof(relay_spool_header_t) <= RELAY_SPOOL_DATA_OFFSET,
               "relay spool header must fit in its flash sector");

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc;
}

static uint32_t crc32_bytes(const void *data, size_t length)
{
    return crc32_update(UINT32_MAX, data, length) ^ UINT32_MAX;
}

static uint32_t header_crc32(const relay_spool_header_t *header)
{
    return crc32_bytes(header, offsetof(relay_spool_header_t, header_crc32));
}

static size_t sector_ceil(size_t bytes)
{
    return (bytes + RELAY_SPOOL_DATA_OFFSET - 1U) &
           ~(RELAY_SPOOL_DATA_OFFSET - 1U);
}

static bool valid_header(const relay_spool_header_t *header)
{
    return memcmp(header->magic, spool_magic, sizeof(spool_magic)) == 0 &&
           header->version == RELAY_SPOOL_HEADER_VERSION &&
           header->size_bytes > 0U &&
           header->size_bytes <= relay_spool_capacity() &&
           header->file_name[0] != '\0' &&
           memchr(header->file_name, '\0', sizeof(header->file_name)) != NULL &&
           header->file_type[0] != '\0' &&
           memchr(header->file_type, '\0', sizeof(header->file_type)) != NULL &&
           header->header_crc32 == header_crc32(header);
}

static bool source_read(void *context, uint8_t *buffer,
                        size_t capacity, size_t *bytes_read)
{
    relay_spool_state_t *spool = context;
    if (spool == NULL || buffer == NULL || bytes_read == NULL ||
        !spool->ready || spool->writing) {
        return false;
    }
    const size_t remaining = spool->header.size_bytes - spool->cursor;
    const size_t count = remaining < capacity ? remaining : capacity;
    if (count == 0U) {
        *bytes_read = 0U;
        return true;
    }
    const esp_err_t result = esp_partition_read(
        spool->partition, RELAY_SPOOL_DATA_OFFSET + spool->cursor,
        buffer, count);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "read relay payload: %s", esp_err_to_name(result));
        return false;
    }
    spool->cursor += count;
    *bytes_read = count;
    return true;
}

static bool source_rewind(void *context)
{
    relay_spool_state_t *spool = context;
    if (spool == NULL || !spool->ready || spool->writing) {
        return false;
    }
    spool->cursor = 0U;
    return true;
}

esp_err_t relay_spool_init(void)
{
    memset(&state, 0, sizeof(state));
    state.partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        RELAY_SPOOL_PARTITION_LABEL);
    if (state.partition == NULL || state.partition->size <= RELAY_SPOOL_DATA_OFFSET) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(esp_partition_read(
                            state.partition, 0U, &state.header,
                            sizeof(state.header)),
                        TAG, "read relay header");
    state.ready = valid_header(&state.header);
    if (state.ready) {
        ESP_LOGI(TAG, "relay file ready: %s (%lu bytes, crc32=%08lx)",
                 state.header.file_name, (unsigned long)state.header.size_bytes,
                 (unsigned long)state.header.payload_crc32);
    } else {
        memset(&state.header, 0, sizeof(state.header));
        ESP_LOGI(TAG, "relay spool empty; capacity=%u bytes",
                 (unsigned)relay_spool_capacity());
    }
    return ESP_OK;
}

size_t relay_spool_capacity(void)
{
    return state.partition != NULL && state.partition->size > RELAY_SPOOL_DATA_OFFSET
               ? state.partition->size - RELAY_SPOOL_DATA_OFFSET
               : 0U;
}

bool relay_spool_is_ready(void)
{
    return state.ready && !state.writing;
}

esp_err_t relay_spool_begin(size_t size_bytes, uint32_t expected_crc32,
                            const char *file_name, const char *file_type)
{
    if (state.partition == NULL || size_bytes == 0U ||
        size_bytes > relay_spool_capacity() || file_name == NULL ||
        file_type == NULL || file_name[0] == '\0' || file_type[0] == '\0' ||
        strlen(file_name) > RELAY_SPOOL_FILE_NAME_MAX ||
        strlen(file_type) > RELAY_SPOOL_FILE_TYPE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state.writing) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t erase_bytes = sector_ceil(RELAY_SPOOL_DATA_OFFSET + size_bytes);
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(state.partition, 0U, erase_bytes),
                        TAG, "erase relay spool");
    memset(&state.header, 0, sizeof(state.header));
    memcpy(state.header.magic, spool_magic, sizeof(spool_magic));
    state.header.version = RELAY_SPOOL_HEADER_VERSION;
    state.header.size_bytes = (uint32_t)size_bytes;
    state.header.payload_crc32 = expected_crc32;
    memcpy(state.header.file_name, file_name, strlen(file_name) + 1U);
    memcpy(state.header.file_type, file_type, strlen(file_type) + 1U);
    state.cursor = 0U;
    state.write_cursor = 0U;
    state.write_crc32 = UINT32_MAX;
    state.ready = false;
    state.writing = true;
    return ESP_OK;
}

esp_err_t relay_spool_write(const void *data, size_t size_bytes)
{
    if (!state.writing || data == NULL || size_bytes == 0U ||
        state.write_cursor + size_bytes > state.header.size_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(
        esp_partition_write(state.partition,
                            RELAY_SPOOL_DATA_OFFSET + state.write_cursor,
                            data, size_bytes),
        TAG, "write relay payload");
    state.write_crc32 = crc32_update(state.write_crc32, data, size_bytes);
    state.write_cursor += size_bytes;
    return ESP_OK;
}

esp_err_t relay_spool_finish(void)
{
    if (!state.writing || state.write_cursor != state.header.size_bytes) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t actual_crc32 = state.write_crc32 ^ UINT32_MAX;
    if (actual_crc32 != state.header.payload_crc32) {
        relay_spool_abort();
        return ESP_ERR_INVALID_CRC;
    }
    state.header.header_crc32 = header_crc32(&state.header);
    ESP_RETURN_ON_ERROR(esp_partition_write(state.partition, 0U, &state.header,
                                             sizeof(state.header)),
                        TAG, "commit relay header");
    state.writing = false;
    state.ready = true;
    state.cursor = 0U;
    ESP_LOGI(TAG, "stored relay file: %s (%u bytes)", state.header.file_name,
             (unsigned)state.header.size_bytes);
    return ESP_OK;
}

void relay_spool_abort(void)
{
    state.writing = false;
    state.ready = false;
    state.cursor = 0U;
    state.write_cursor = 0U;
    memset(&state.header, 0, sizeof(state.header));
}

esp_err_t relay_spool_outgoing_file(espdrop_airdrop_outgoing_file_t *file)
{
    if (file == NULL || !relay_spool_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    *file = (espdrop_airdrop_outgoing_file_t){
        .file_name = state.header.file_name,
        .file_type = state.header.file_type,
        .source = {
            .context = &state,
            .size_bytes = state.header.size_bytes,
            .read = source_read,
            .rewind = source_rewind,
        },
    };
    return ESP_OK;
}
