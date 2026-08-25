#include "relay_stream.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#include "relay_spool.h"

#define RELAY_STREAM_BUFFER_BYTES (RELAY_STREAM_CHUNK_MAX * 4U)
#define RELAY_STREAM_WAIT_MS 30000U

typedef struct {
    StreamBufferHandle_t buffer;
    relay_stream_notify_t notify;
    portMUX_TYPE lock;
    char file_name[RELAY_SPOOL_FILE_NAME_MAX + 1U];
    char file_type[RELAY_SPOOL_FILE_TYPE_MAX + 1U];
    size_t file_bytes;
    size_t payload_bytes;
    size_t archive_bytes;
    size_t input_bytes;
    size_t output_bytes;
    size_t dvzip_blocks;
    uint32_t file_crc32;
    uint32_t payload_crc32;
    uint32_t input_crc32;
    uint32_t next_sequence;
    bool armed;
    bool go_sent;
    bool failed;
} relay_stream_state_t;

static const char *TAG = "relay_stream";
static relay_stream_state_t state = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

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

static void notify_text(const char *value)
{
    if (state.notify != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(state.notify(value, strlen(value)));
    }
}

static bool prepared_source_read(
    void *context,
    uint8_t *output,
    size_t capacity,
    size_t *bytes_read)
{
    relay_stream_state_t *stream = context;
    if (stream == NULL || output == NULL || capacity == 0U ||
        bytes_read == NULL) {
        return false;
    }

    bool send_go = false;
    portENTER_CRITICAL(&stream->lock);
    if (stream->armed && !stream->failed && !stream->go_sent) {
        stream->go_sent = true;
        send_go = true;
    }
    const bool readable = stream->armed && !stream->failed;
    const size_t remaining = readable
                                 ? stream->payload_bytes - stream->output_bytes
                                 : 0U;
    portEXIT_CRITICAL(&stream->lock);
    if (!readable) {
        return false;
    }
    if (send_go) {
        char response[96];
        const int length = snprintf(
            response, sizeof(response),
            "ESPDROP-STREAM-GO payload_bytes=%u chunk=%u\n",
            (unsigned)stream->payload_bytes,
            (unsigned)RELAY_STREAM_CHUNK_MAX);
        if (length <= 0 || (size_t)length >= sizeof(response) ||
            stream->notify(response, (size_t)length) != ESP_OK) {
            portENTER_CRITICAL(&stream->lock);
            stream->failed = true;
            portEXIT_CRITICAL(&stream->lock);
            return false;
        }
    }
    if (remaining == 0U) {
        *bytes_read = 0U;
        return true;
    }

    const size_t request = remaining < capacity ? remaining : capacity;
    const size_t received = xStreamBufferReceive(
        stream->buffer, output, request, pdMS_TO_TICKS(RELAY_STREAM_WAIT_MS));
    if (received == 0U) {
        portENTER_CRITICAL(&stream->lock);
        stream->failed = true;
        portEXIT_CRITICAL(&stream->lock);
        notify_text("ESPDROP-STREAM-SOURCE-TIMEOUT\n");
        return false;
    }

    bool complete = false;
    portENTER_CRITICAL(&stream->lock);
    stream->output_bytes += received;
    complete = stream->output_bytes == stream->payload_bytes;
    portEXIT_CRITICAL(&stream->lock);
    *bytes_read = received;
    if (complete) {
        notify_text("ESPDROP-STREAM-SOURCE-COMPLETE\n");
    }
    return true;
}

esp_err_t relay_stream_init(relay_stream_notify_t notify)
{
    if (notify == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state.buffer == NULL) {
        state.buffer = xStreamBufferCreate(RELAY_STREAM_BUFFER_BYTES, 1U);
        if (state.buffer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    state.notify = notify;
    return ESP_OK;
}

esp_err_t relay_stream_begin(
    size_t file_bytes,
    uint32_t file_crc32,
    size_t payload_bytes,
    uint32_t payload_crc32,
    size_t archive_bytes,
    size_t dvzip_blocks,
    const char *file_name,
    const char *file_type,
    espdrop_airdrop_outgoing_file_t *outgoing)
{
    if (state.buffer == NULL || state.notify == NULL || file_bytes == 0U ||
        payload_bytes == 0U || archive_bytes == 0U || dvzip_blocks == 0U ||
        file_name == NULL || file_type == NULL || outgoing == NULL ||
        file_name[0] == '\0' || file_type[0] == '\0' ||
        strlen(file_name) > RELAY_SPOOL_FILE_NAME_MAX ||
        strlen(file_type) > RELAY_SPOOL_FILE_TYPE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    xStreamBufferReset(state.buffer);
    portENTER_CRITICAL(&state.lock);
    memset(state.file_name, 0, sizeof(state.file_name));
    memset(state.file_type, 0, sizeof(state.file_type));
    memcpy(state.file_name, file_name, strlen(file_name) + 1U);
    memcpy(state.file_type, file_type, strlen(file_type) + 1U);
    state.file_bytes = file_bytes;
    state.payload_bytes = payload_bytes;
    state.archive_bytes = archive_bytes;
    state.input_bytes = 0U;
    state.output_bytes = 0U;
    state.dvzip_blocks = dvzip_blocks;
    state.file_crc32 = file_crc32;
    state.payload_crc32 = payload_crc32;
    state.input_crc32 = UINT32_MAX;
    state.next_sequence = 0U;
    state.go_sent = false;
    state.failed = false;
    state.armed = true;
    portEXIT_CRITICAL(&state.lock);

    *outgoing = (espdrop_airdrop_outgoing_file_t){
        .file_name = state.file_name,
        .file_type = state.file_type,
        .source = {
            .size_bytes = state.file_bytes,
        },
        .prepared_payload = {
            .source = {
                .context = &state,
                .size_bytes = state.payload_bytes,
                .read = prepared_source_read,
            },
            .archive_bytes = state.archive_bytes,
            .dvzip_blocks = state.dvzip_blocks,
            .file_crc32 = state.file_crc32,
            .stored_blocks = true,
        },
    };
    ESP_LOGI(TAG,
             "host stream armed: %s file=%u archive=%u payload=%u blocks=%u",
             state.file_name, (unsigned)state.file_bytes,
             (unsigned)state.archive_bytes, (unsigned)state.payload_bytes,
             (unsigned)state.dvzip_blocks);
    return ESP_OK;
}

esp_err_t relay_stream_push_chunk(
    uint32_t sequence,
    const uint8_t *data,
    size_t data_bytes,
    uint32_t expected_crc32)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(relay_stream_validate_chunk(sequence, data_bytes), TAG,
                        "validate host stream chunk");
    portENTER_CRITICAL(&state.lock);
    const bool valid = state.armed && state.go_sent && !state.failed &&
                       sequence == state.next_sequence;
    portEXIT_CRITICAL(&state.lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t chunk_crc32 =
        crc32_update(UINT32_MAX, data, data_bytes) ^ UINT32_MAX;
    if (chunk_crc32 != expected_crc32) {
        return ESP_ERR_INVALID_CRC;
    }

    portENTER_CRITICAL(&state.lock);
    const uint32_t next_crc = crc32_update(state.input_crc32, data, data_bytes);
    const bool final = state.input_bytes + data_bytes == state.payload_bytes;
    const bool total_crc_valid =
        !final || (next_crc ^ UINT32_MAX) == state.payload_crc32;
    if (!total_crc_valid) {
        state.failed = true;
    }
    portEXIT_CRITICAL(&state.lock);
    if (!total_crc_valid) {
        return ESP_ERR_INVALID_CRC;
    }

    const size_t sent = xStreamBufferSend(
        state.buffer, data, data_bytes, pdMS_TO_TICKS(RELAY_STREAM_WAIT_MS));
    if (sent != data_bytes) {
        portENTER_CRITICAL(&state.lock);
        state.failed = true;
        portEXIT_CRITICAL(&state.lock);
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&state.lock);
    state.input_crc32 = next_crc;
    state.input_bytes += data_bytes;
    ++state.next_sequence;
    portEXIT_CRITICAL(&state.lock);
    return ESP_OK;
}

esp_err_t relay_stream_validate_chunk(uint32_t sequence, size_t data_bytes)
{
    if (data_bytes == 0U || data_bytes > RELAY_STREAM_CHUNK_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    portENTER_CRITICAL(&state.lock);
    const bool valid = state.armed && state.go_sent && !state.failed &&
                       sequence == state.next_sequence &&
                       state.input_bytes <= state.payload_bytes &&
                       data_bytes <= state.payload_bytes - state.input_bytes;
    portEXIT_CRITICAL(&state.lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void relay_stream_abort(void)
{
    portENTER_CRITICAL(&state.lock);
    state.armed = false;
    state.failed = true;
    portEXIT_CRITICAL(&state.lock);
}

bool relay_stream_is_armed(void)
{
    portENTER_CRITICAL(&state.lock);
    const bool armed = state.armed && !state.failed;
    portEXIT_CRITICAL(&state.lock);
    return armed;
}
