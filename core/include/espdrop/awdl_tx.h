#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espdrop/awdl_election.h"
#include "espdrop/awdl_frame.h"
#include "espdrop/awdl_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_AWDL_TX_FRAME_CAPACITY 384U
#define ESPDROP_AWDL_TX_NAME_BYTES 32U

typedef enum {
    ESPDROP_AWDL_BUILD_OK = 0,
    ESPDROP_AWDL_BUILD_INVALID_ARGUMENT = -1,
    ESPDROP_AWDL_BUILD_INVALID_STATE = -2,
    ESPDROP_AWDL_BUILD_NO_SPACE = -3,
} espdrop_awdl_build_result_t;

typedef struct {
    uint8_t self[6];
    uint8_t master[6];
    uint8_t sync_master[6];
    char name[ESPDROP_AWDL_TX_NAME_BYTES];
    uint64_t sync_reference_us;
    uint64_t peer_time_observed_us;
    uint32_t peer_time_reference;
    uint16_t aw_sequence_base;
    uint16_t aw_period_tu;
    uint16_t action_frame_period_tu;
    uint8_t presence_mode;
    uint8_t channel;
    uint8_t peer_channel_count;
    uint8_t peer_channel_encoding;
    uint8_t peer_channel_duplicate_count;
    uint8_t peer_channel_step_count;
    uint16_t peer_channel_fill;
    uint8_t peer_channels[ESPDROP_AWDL_MAX_CHANNELS];
    uint8_t peer_operating_classes[ESPDROP_AWDL_MAX_CHANNELS];
    uint32_t distance_to_master;
    uint32_t master_metric;
    uint32_t self_metric;
    uint32_t master_counter;
    uint32_t self_counter;
    bool advertise_airdrop_tcp;
    uint16_t airdrop_port;
} espdrop_awdl_tx_state_t;

bool espdrop_awdl_tx_state_from_mif(
    espdrop_awdl_tx_state_t *state,
    const uint8_t self[6],
    const uint8_t source[6],
    const char *name,
    uint32_t peer_phy_tx,
    const espdrop_awdl_mif_t *mif,
    uint64_t observation_us);

bool espdrop_awdl_tx_state_apply_election(
    espdrop_awdl_tx_state_t *state,
    const espdrop_awdl_election_state_t *election);

bool espdrop_awdl_next_channel_window_us(
    const espdrop_awdl_tx_state_t *state,
    uint8_t channel,
    uint64_t now_us,
    uint32_t guard_us,
    uint64_t *scheduled_us);

bool espdrop_awdl_next_common_channel_window_us(
    const espdrop_awdl_tx_state_t *local,
    const espdrop_awdl_tx_state_t *peer,
    uint8_t channel,
    uint64_t now_us,
    uint32_t guard_us,
    uint64_t *scheduled_us);

espdrop_awdl_build_result_t espdrop_awdl_build_action(
    uint8_t *frame,
    size_t capacity,
    size_t *length,
    const espdrop_awdl_tx_state_t *state,
    espdrop_awdl_action_subtype_t subtype,
    uint64_t now_us,
    uint16_t sequence_number);

#ifdef __cplusplus
}
#endif
