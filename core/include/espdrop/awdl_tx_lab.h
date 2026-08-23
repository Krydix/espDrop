#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "espdrop/awdl_frame.h"
#include "espdrop/awdl_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t espdrop_awdl_tx_lab_init(const char *name);

void espdrop_awdl_tx_lab_observe_mif(
    const espdrop_awdl_action_t *action,
    const espdrop_awdl_mif_t *mif,
    uint64_t received_at_us);

void espdrop_awdl_tx_lab_note_directed_peer(const uint8_t source[6]);

void espdrop_awdl_tx_lab_note_neighbor_advertisement(
    const uint8_t source[6]);

void espdrop_awdl_tx_lab_note_echo_reply(
    const uint8_t source[6],
    uint16_t identifier,
    uint16_t sequence);

#ifdef __cplusplus
}
#endif
