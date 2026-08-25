#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "espdrop/awdl_frame.h"
#include "espdrop/awdl_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t espdrop_awdl_tx_lab_init(const char *name);

bool espdrop_awdl_tx_lab_netif_ready(void);

bool espdrop_awdl_tx_lab_wants_mif(const uint8_t source[6]);

bool espdrop_awdl_tx_lab_target(uint8_t output[6]);

typedef enum {
    ESPDROP_AWDL_TARGET_NONE = 0,
    ESPDROP_AWDL_TARGET_AUTO,
    ESPDROP_AWDL_TARGET_MANUAL,
} espdrop_awdl_target_mode_t;

/* Serial control may configure the target before or after the radio backend
 * starts. Manual selection is intentionally temporary and never identifies a
 * person; it is only the current AWDL session address. */
esp_err_t espdrop_awdl_tx_lab_set_target(const uint8_t target[6]);
esp_err_t espdrop_awdl_tx_lab_set_target_mode(
    espdrop_awdl_target_mode_t mode);
espdrop_awdl_target_mode_t espdrop_awdl_tx_lab_target_mode(void);

/* Arm one bounded sender generation after the host has installed its file and
 * target. A completed or failed generation can be armed again without a
 * device reboot. */
esp_err_t espdrop_awdl_tx_lab_request_run(void);

/* True once the selected unicast target and our current synchronization
 * parent are in the same live AWDL election tree. A distance-zero receiver is
 * used directly; a distance-one receiver is reached through its top master. */
bool espdrop_awdl_tx_lab_target_is_sync_master(void);

bool espdrop_awdl_tx_lab_consider_airdrop_endpoints(
    uint64_t observed_at_us);

void espdrop_awdl_tx_lab_observe_mif(
    const espdrop_awdl_action_t *action,
    const espdrop_awdl_mif_t *mif,
    bool advertises_airdrop_tcp,
    uint64_t received_at_us);

void espdrop_awdl_tx_lab_note_peer_seen(
    const uint8_t source[6],
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
