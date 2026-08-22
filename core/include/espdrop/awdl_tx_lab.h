#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "espdrop/awdl_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t espdrop_awdl_tx_lab_init(const char *name);

void espdrop_awdl_tx_lab_observe_mif(
    const uint8_t source[6],
    const espdrop_awdl_mif_t *mif,
    uint64_t received_at_us);

void espdrop_awdl_tx_lab_note_directed_peer(const uint8_t source[6]);

#ifdef __cplusplus
}
#endif
