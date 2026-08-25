#include "espdrop/airdrop_outgoing.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE registry_lock = portMUX_INITIALIZER_UNLOCKED;
static espdrop_airdrop_outgoing_file_t registered_file;
static espdrop_airdrop_outgoing_result_t transfer_result;
static bool registered;
static bool acquired;

static bool valid_file(const espdrop_airdrop_outgoing_file_t *file)
{
    const bool raw_source = file != NULL && file->source.read != NULL;
    const bool prepared_source =
        file != NULL && file->prepared_payload.source.read != NULL &&
        file->prepared_payload.source.size_bytes > 0U &&
        file->prepared_payload.archive_bytes > 0U &&
        file->prepared_payload.dvzip_blocks > 0U;
    return file != NULL && file->file_name != NULL &&
           file->file_name[0] != '\0' && file->file_type != NULL &&
           file->file_type[0] != '\0' && file->source.size_bytes > 0U &&
           (raw_source || prepared_source);
}

esp_err_t espdrop_airdrop_outgoing_set(
    const espdrop_airdrop_outgoing_file_t *file)
{
    if (!valid_file(file)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&registry_lock);
    if (acquired) {
        portEXIT_CRITICAL(&registry_lock);
        return ESP_ERR_INVALID_STATE;
    }
    registered_file = *file;
    transfer_result = (espdrop_airdrop_outgoing_result_t){
        .state = ESPDROP_AIRDROP_OUTGOING_RESULT_PENDING,
    };
    registered = true;
    portEXIT_CRITICAL(&registry_lock);
    return ESP_OK;
}

esp_err_t espdrop_airdrop_outgoing_clear(void)
{
    portENTER_CRITICAL(&registry_lock);
    if (acquired) {
        portEXIT_CRITICAL(&registry_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&registered_file, 0, sizeof(registered_file));
    memset(&transfer_result, 0, sizeof(transfer_result));
    registered = false;
    portEXIT_CRITICAL(&registry_lock);
    return ESP_OK;
}

bool espdrop_airdrop_outgoing_ready(void)
{
    portENTER_CRITICAL(&registry_lock);
    const bool ready = registered && !acquired;
    portEXIT_CRITICAL(&registry_lock);
    return ready;
}

bool espdrop_airdrop_outgoing_acquire(
    espdrop_airdrop_outgoing_file_t *file)
{
    if (file == NULL) {
        return false;
    }
    portENTER_CRITICAL(&registry_lock);
    const bool available = registered && !acquired;
    if (available) {
        *file = registered_file;
        acquired = true;
    }
    portEXIT_CRITICAL(&registry_lock);
    return available;
}

void espdrop_airdrop_outgoing_release(void)
{
    portENTER_CRITICAL(&registry_lock);
    acquired = false;
    portEXIT_CRITICAL(&registry_lock);
}

void espdrop_airdrop_outgoing_complete(
    const espdrop_airdrop_outgoing_result_t *result)
{
    if (result == NULL ||
        (result->state != ESPDROP_AIRDROP_OUTGOING_RESULT_SUCCESS &&
         result->state != ESPDROP_AIRDROP_OUTGOING_RESULT_FAILED)) {
        return;
    }
    portENTER_CRITICAL(&registry_lock);
    if (registered && acquired &&
        transfer_result.state == ESPDROP_AIRDROP_OUTGOING_RESULT_PENDING) {
        transfer_result = *result;
    }
    portEXIT_CRITICAL(&registry_lock);
}

espdrop_airdrop_outgoing_result_t espdrop_airdrop_outgoing_result(void)
{
    portENTER_CRITICAL(&registry_lock);
    const espdrop_airdrop_outgoing_result_t result = transfer_result;
    portEXIT_CRITICAL(&registry_lock);
    return result;
}
