#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "espdrop/airdrop_upload.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPDROP_AIRDROP_OUTGOING_RESULT_NONE = 0,
    ESPDROP_AIRDROP_OUTGOING_RESULT_PENDING,
    ESPDROP_AIRDROP_OUTGOING_RESULT_SUCCESS,
    ESPDROP_AIRDROP_OUTGOING_RESULT_FAILED,
} espdrop_airdrop_outgoing_result_state_t;

typedef enum {
    ESPDROP_AIRDROP_OUTGOING_STAGE_NONE = 0,
    ESPDROP_AIRDROP_OUTGOING_STAGE_TLS,
    ESPDROP_AIRDROP_OUTGOING_STAGE_ASK,
    ESPDROP_AIRDROP_OUTGOING_STAGE_UPLOAD,
} espdrop_airdrop_outgoing_stage_t;

typedef struct {
    espdrop_airdrop_outgoing_result_state_t state;
    espdrop_airdrop_outgoing_stage_t stage;
    int error;
    uint16_t http_status;
    size_t request_bytes;
    size_t payload_bytes;
} espdrop_airdrop_outgoing_result_t;

/* Register one file for the attended sender. It may provide a rewindable raw
 * source or a non-seekable host-prepared dvzip payload. The registry owns a
 * copy of the descriptor, while source contexts remain caller-owned. */
esp_err_t espdrop_airdrop_outgoing_set(
    const espdrop_airdrop_outgoing_file_t *file);

/* Invalidate the registered file. Fails while a sender holds it. */
esp_err_t espdrop_airdrop_outgoing_clear(void);

/* Non-owning readiness check used by the serial-controlled sender gate. */
bool espdrop_airdrop_outgoing_ready(void);

/* A sender must pair every successful acquire with release. */
bool espdrop_airdrop_outgoing_acquire(
    espdrop_airdrop_outgoing_file_t *file);
void espdrop_airdrop_outgoing_release(void);

/* Publish and query the terminal result for a host-armed transfer. Registering
 * a new file resets the result to PENDING; clearing it resets to NONE. */
void espdrop_airdrop_outgoing_complete(
    const espdrop_airdrop_outgoing_result_t *result);
espdrop_airdrop_outgoing_result_t espdrop_airdrop_outgoing_result(void);

#ifdef __cplusplus
}
#endif
