/*
 * espDrop AWDL transmit scheduling and frame construction
 *
 * The phase-aware common-channel gate is adapted from OWL src/schedule.c at
 * commit da255a70f221784c836d943dd3f243bc798f223b.
 * Copyright (C) 2018 The Open Wireless Link Project
 * Copyright (C) 2018 Milan Stute
 * Copyright (C) 2026 Krydix and espDrop contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "espdrop/awdl_tx.h"

#include <string.h>

#define IEEE80211_HEADER_BYTES 24U
#define AWDL_ACTION_HEADER_BYTES 16U
#define AWDL_SYNC_VALUE_BYTES 73U
#define AWDL_CHANNEL_VALUE_BYTES 41U
#define AWDL_ELECTION_V1_VALUE_BYTES 21U
#define AWDL_ELECTION_V2_VALUE_BYTES 40U
#define AWDL_SERVICE_VALUE_BYTES 11U
#define AWDL_HT_VALUE_BYTES 9U
#define AWDL_DATA_PATH_VALUE_BYTES 15U
#define AWDL_VERSION_VALUE_BYTES 2U
#define AWDL_TU_US 1024ULL

static const uint8_t broadcast[6] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t awdl_bssid[6] = {
    0x00, 0x25, 0x00, 0xff, 0x94, 0x73,
};

static void put_le16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void put_le32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static bool mac_is_zero(const uint8_t address[6])
{
    static const uint8_t zero[6];
    return memcmp(address, zero, sizeof(zero)) == 0;
}

static size_t bounded_string_length(const char *value, size_t maximum)
{
    size_t length = 0U;
    while (length < maximum && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool state_is_valid(const espdrop_awdl_tx_state_t *state);

static bool make_channel_sequence(
    uint8_t *value,
    size_t padding,
    const espdrop_awdl_tx_state_t *state)
{
    memset(value, 0, 38U + padding);
    if (state == NULL || state->peer_channel_count != ESPDROP_AWDL_MAX_CHANNELS ||
        state->peer_channel_encoding != 3U) {
        return false;
    }
    value[0] = (uint8_t)(state->peer_channel_count - 1U);
    value[1] = state->peer_channel_encoding;
    value[2] = state->peer_channel_duplicate_count;
    value[3] = state->peer_channel_step_count;
    put_le16(value + 4, state->peer_channel_fill);
    for (size_t index = 0; index < state->peer_channel_count; ++index) {
        value[6U + index * 2U] = state->peer_channels[index];
        value[7U + index * 2U] = state->peer_operating_classes[index];
    }
    return true;
}

static uint8_t *append_tlv(uint8_t *output, uint8_t type, uint16_t length)
{
    output[0] = type;
    put_le16(output + 1, length);
    memset(output + 3, 0, length);
    return output + 3;
}

bool espdrop_awdl_tx_state_from_mif(
    espdrop_awdl_tx_state_t *state,
    const uint8_t self[6],
    const uint8_t source[6],
    const char *name,
    uint32_t peer_phy_tx,
    const espdrop_awdl_mif_t *mif,
    uint64_t observation_us)
{
    if (state == NULL || self == NULL || source == NULL || name == NULL ||
        mif == NULL || !mif->has_sync || !mif->has_election_v2 ||
        mif->sync.aw_period_tu == 0U || mif->sync.presence_mode == 0U ||
        mif->sync.presence_mode > ESPDROP_AWDL_MAX_CHANNELS ||
        mac_is_zero(self) || mac_is_zero(mif->election_v2.master) ||
        mac_is_zero(mif->election_v2.sync_master)) {
        return false;
    }

    const uint32_t eaw_period_tu =
        (uint32_t)mif->sync.aw_period_tu * mif->sync.presence_mode;
    if (eaw_period_tu == 0U) {
        return false;
    }
    const uint32_t down_tu = mif->sync.tx_down_counter % eaw_period_tu;
    const uint32_t elapsed_in_eaw_tu =
        down_tu == 0U ? 0U : eaw_period_tu - down_tu;
    const uint64_t elapsed_us = (uint64_t)elapsed_in_eaw_tu * AWDL_TU_US;

    memset(state, 0, sizeof(*state));
    memcpy(state->self, self, sizeof(state->self));
    memcpy(state->master, mif->election_v2.master, sizeof(state->master));
    memcpy(state->sync_master, source, sizeof(state->sync_master));
    (void)strncpy(state->name, name, sizeof(state->name) - 1U);
    state->sync_reference_us = observation_us >= elapsed_us
                                   ? observation_us - elapsed_us : 0U;
    state->peer_time_observed_us = observation_us;
    state->peer_time_reference = peer_phy_tx;
    state->aw_sequence_base = (uint16_t)(
        mif->sync.next_aw_sequence &
        (uint16_t)~(mif->sync.presence_mode - 1U));
    state->aw_period_tu = mif->sync.aw_period_tu;
    state->action_frame_period_tu =
        mif->sync.action_frame_period_tu == 0U
            ? 110U : mif->sync.action_frame_period_tu;
    state->presence_mode = mif->sync.presence_mode;
    state->channel = mif->sync.master_channel == 0U
                         ? 6U : mif->sync.master_channel;
    const espdrop_awdl_channel_sequence_t *peer_sequence = NULL;
    if (mif->has_channel_sequence) {
        peer_sequence = &mif->channel_sequence;
    } else if (mif->sync.has_embedded_channel_sequence) {
        peer_sequence = &mif->sync.embedded_channel_sequence;
    }
    if (peer_sequence != NULL) {
        state->peer_channel_count = peer_sequence->count;
        state->peer_channel_encoding = peer_sequence->encoding;
        state->peer_channel_duplicate_count =
            peer_sequence->duplicate_count;
        state->peer_channel_step_count = peer_sequence->step_count;
        state->peer_channel_fill = peer_sequence->fill_channel;
        memcpy(state->peer_channels, peer_sequence->channels,
               sizeof(state->peer_channels));
        memcpy(state->peer_operating_classes,
               peer_sequence->operating_classes,
               sizeof(state->peer_operating_classes));
    }
    state->distance_to_master =
        mif->election_v2.distance_to_master + 1U;
    state->master_metric = mif->election_v2.master_metric;
    state->self_metric = ESPDROP_AWDL_ELECTION_METRIC_INITIAL;
    state->master_counter = mif->election_v2.master_counter;
    state->self_counter = ESPDROP_AWDL_ELECTION_COUNTER_INITIAL;
    return true;
}

bool espdrop_awdl_tx_state_apply_election(
    espdrop_awdl_tx_state_t *state,
    const espdrop_awdl_election_state_t *election)
{
    if (state == NULL || election == NULL ||
        memcmp(state->self, election->self, sizeof(state->self)) != 0 ||
        mac_is_zero(election->master) ||
        mac_is_zero(election->sync_master) ||
        election->distance_to_master >
            ESPDROP_AWDL_ELECTION_TREE_MAX_HEIGHT) {
        return false;
    }
    memcpy(state->master, election->master, sizeof(state->master));
    memcpy(state->sync_master, election->sync_master,
           sizeof(state->sync_master));
    state->distance_to_master = election->distance_to_master;
    state->master_metric = election->master_metric;
    state->self_metric = election->self_metric;
    state->master_counter = election->master_counter;
    state->self_counter = election->self_counter;
    return true;
}

bool espdrop_awdl_next_channel_window_us(
    const espdrop_awdl_tx_state_t *state,
    uint8_t channel,
    uint64_t now_us,
    uint32_t guard_us,
    uint64_t *scheduled_us)
{
    if (!state_is_valid(state) || scheduled_us == NULL ||
        channel == 0U || state->peer_channel_count == 0U ||
        state->peer_channel_count > ESPDROP_AWDL_MAX_CHANNELS ||
        now_us < state->sync_reference_us) {
        return false;
    }
    const uint64_t eaw_us = (uint64_t)state->aw_period_tu *
                            state->presence_mode * AWDL_TU_US;
    if (eaw_us == 0U || guard_us >= eaw_us) {
        return false;
    }

    const uint64_t elapsed_eaws =
        (now_us - state->sync_reference_us) / eaw_us;
    const uint64_t current_start =
        state->sync_reference_us + elapsed_eaws * eaw_us;
    const uint64_t sequence_base =
        state->aw_sequence_base / state->presence_mode;
    for (uint64_t offset = 0U;
         offset <= (uint64_t)state->peer_channel_count;
         ++offset) {
        const size_t index = (size_t)(
            (sequence_base + elapsed_eaws + offset) %
            state->peer_channel_count);
        if (state->peer_channels[index] != channel) {
            continue;
        }
        const uint64_t candidate =
            current_start + offset * eaw_us + guard_us;
        if (candidate >= now_us) {
            *scheduled_us = candidate;
            return true;
        }
    }
    return false;
}

typedef struct {
    uint64_t start_us;
    uint64_t end_us;
    uint8_t channel;
} awdl_channel_window_t;

static bool channel_window_at(
    const espdrop_awdl_tx_state_t *state,
    uint64_t at_us,
    awdl_channel_window_t *window)
{
    if (!state_is_valid(state) || window == NULL ||
        at_us < state->sync_reference_us) {
        return false;
    }
    const uint64_t eaw_us = (uint64_t)state->aw_period_tu *
                            state->presence_mode * AWDL_TU_US;
    if (eaw_us == 0U) {
        return false;
    }
    const uint64_t elapsed_eaws =
        (at_us - state->sync_reference_us) / eaw_us;
    if (elapsed_eaws >
        (UINT64_MAX - state->sync_reference_us) / eaw_us) {
        return false;
    }
    window->start_us = state->sync_reference_us + elapsed_eaws * eaw_us;
    if (window->start_us > UINT64_MAX - eaw_us) {
        return false;
    }
    window->end_us = window->start_us + eaw_us;
    const uint64_t sequence_base =
        state->aw_sequence_base / state->presence_mode;
    const size_t index = (size_t)(
        (sequence_base + elapsed_eaws) % state->peer_channel_count);
    window->channel = state->peer_channels[index];
    return true;
}

bool espdrop_awdl_next_common_channel_window_us(
    const espdrop_awdl_tx_state_t *local,
    const espdrop_awdl_tx_state_t *peer,
    uint8_t channel,
    uint64_t now_us,
    uint32_t guard_us,
    uint64_t *scheduled_us)
{
    if (!state_is_valid(local) || !state_is_valid(peer) ||
        scheduled_us == NULL || channel == 0U ||
        now_us < local->sync_reference_us ||
        now_us < peer->sync_reference_us) {
        return false;
    }

    const uint64_t local_eaw_us = (uint64_t)local->aw_period_tu *
                                  local->presence_mode * AWDL_TU_US;
    const uint64_t peer_eaw_us = (uint64_t)peer->aw_period_tu *
                                 peer->presence_mode * AWDL_TU_US;
    if (local_eaw_us != peer_eaw_us ||
        (uint64_t)guard_us * 2U >= local_eaw_us ||
        (uint64_t)guard_us * 2U >= peer_eaw_us) {
        return false;
    }

    /* Each sequence repeats after at most 16 EAWs. Walking both sets of
     * boundaries for two complete sequences covers every relative slot/phase
     * combination without an unbounded search. */
    const size_t boundary_limit =
        2U * (local->peer_channel_count + peer->peer_channel_count) + 2U;
    uint64_t cursor_us = now_us;
    for (size_t boundary = 0U; boundary < boundary_limit; ++boundary) {
        awdl_channel_window_t local_window;
        awdl_channel_window_t peer_window;
        if (!channel_window_at(local, cursor_us, &local_window) ||
            !channel_window_at(peer, cursor_us, &peer_window)) {
            return false;
        }

        uint64_t safe_start_us = local_window.start_us + guard_us;
        const uint64_t peer_safe_start = peer_window.start_us + guard_us;
        if (safe_start_us < peer_safe_start) {
            safe_start_us = peer_safe_start;
        }
        const uint64_t local_safe_end = local_window.end_us - guard_us;
        const uint64_t peer_safe_end = peer_window.end_us - guard_us;
        const uint64_t safe_end_us = local_safe_end < peer_safe_end
                                         ? local_safe_end : peer_safe_end;
        if (local_window.channel == channel &&
            peer_window.channel == channel &&
            safe_start_us >= now_us &&
            safe_start_us <= safe_end_us) {
            *scheduled_us = safe_start_us;
            return true;
        }

        const uint64_t next_boundary_us =
            local_window.end_us < peer_window.end_us
                ? local_window.end_us : peer_window.end_us;
        if (next_boundary_us <= cursor_us) {
            return false;
        }
        cursor_us = next_boundary_us;
    }
    return false;
}

static bool state_is_valid(const espdrop_awdl_tx_state_t *state)
{
    return state != NULL && !mac_is_zero(state->self) &&
           !mac_is_zero(state->master) &&
           !mac_is_zero(state->sync_master) && state->aw_period_tu != 0U &&
           state->presence_mode != 0U &&
           state->presence_mode <= ESPDROP_AWDL_MAX_CHANNELS &&
           state->peer_channel_count == ESPDROP_AWDL_MAX_CHANNELS &&
           state->peer_channel_encoding == 3U &&
           state->channel >= 1U && state->channel <= 13U;
}

espdrop_awdl_build_result_t espdrop_awdl_build_action(
    uint8_t *frame,
    size_t capacity,
    size_t *length,
    const espdrop_awdl_tx_state_t *state,
    espdrop_awdl_action_subtype_t subtype,
    uint64_t now_us,
    uint16_t sequence_number)
{
    if (frame == NULL || length == NULL || state == NULL ||
        (subtype != ESPDROP_AWDL_ACTION_PSF &&
         subtype != ESPDROP_AWDL_ACTION_MIF)) {
        return ESPDROP_AWDL_BUILD_INVALID_ARGUMENT;
    }
    if (!state_is_valid(state) || now_us < state->sync_reference_us) {
        return ESPDROP_AWDL_BUILD_INVALID_STATE;
    }

    const size_t name_length =
        bounded_string_length(state->name, sizeof(state->name));
    const size_t arpa_value_length = 4U + name_length;
    const size_t required = IEEE80211_HEADER_BYTES + AWDL_ACTION_HEADER_BYTES +
        (3U + AWDL_SYNC_VALUE_BYTES) +
        (3U + AWDL_ELECTION_V1_VALUE_BYTES) +
        (3U + AWDL_CHANNEL_VALUE_BYTES) +
        (3U + AWDL_ELECTION_V2_VALUE_BYTES) +
        (3U + AWDL_SERVICE_VALUE_BYTES) +
        (subtype == ESPDROP_AWDL_ACTION_MIF
             ? (3U + AWDL_HT_VALUE_BYTES) + (3U + arpa_value_length) : 0U) +
        (3U + AWDL_DATA_PATH_VALUE_BYTES) +
        (3U + AWDL_VERSION_VALUE_BYTES);
    if (capacity < required || required > ESPDROP_AWDL_TX_FRAME_CAPACITY) {
        return ESPDROP_AWDL_BUILD_NO_SPACE;
    }

    memset(frame, 0, required);
    frame[0] = 0xd0U;
    memcpy(frame + 4, broadcast, sizeof(broadcast));
    memcpy(frame + 10, state->self, sizeof(state->self));
    memcpy(frame + 16, awdl_bssid, sizeof(awdl_bssid));
    put_le16(frame + 22, (uint16_t)((sequence_number & 0x0fffU) << 4U));

    uint8_t *action = frame + IEEE80211_HEADER_BYTES;
    action[0] = 127U;
    action[1] = 0x00U;
    action[2] = 0x17U;
    action[3] = 0xf2U;
    action[4] = 8U;
    action[5] = 0x10U;
    action[6] = (uint8_t)subtype;
    action[7] = 0U;
    const uint32_t peer_now = state->peer_time_reference +
        (uint32_t)(now_us - state->peer_time_observed_us);
    put_le32(action + 8, peer_now);
    put_le32(action + 12, peer_now);

    const uint64_t elapsed_tu =
        (now_us - state->sync_reference_us) / AWDL_TU_US;
    const uint32_t eaw_period_tu =
        (uint32_t)state->aw_period_tu * state->presence_mode;
    const uint32_t phase_tu = (uint32_t)(elapsed_tu % eaw_period_tu);
    const uint16_t tx_down_tu = (uint16_t)(eaw_period_tu - phase_tu);
    const uint32_t elapsed_eaws = (uint32_t)(elapsed_tu / eaw_period_tu);
    const uint16_t current_aw = (uint16_t)(
        state->aw_sequence_base + elapsed_eaws * state->presence_mode +
        phase_tu / state->aw_period_tu);

    uint8_t *cursor = action + AWDL_ACTION_HEADER_BYTES;
    uint8_t *value = append_tlv(cursor, 4U, AWDL_SYNC_VALUE_BYTES);
    value[0] = state->channel;
    put_le16(value + 1, tx_down_tu);
    value[3] = state->channel;
    value[4] = 0U;
    put_le16(value + 5, state->aw_period_tu);
    put_le16(value + 7, state->action_frame_period_tu);
    put_le16(value + 9, 0x1800U);
    put_le16(value + 11, state->aw_period_tu);
    put_le16(value + 13, state->aw_period_tu);
    const uint32_t elapsed_current_eaw = eaw_period_tu - tx_down_tu;
    put_le16(value + 15,
             elapsed_current_eaw < state->aw_period_tu
                 ? (uint16_t)(state->aw_period_tu - elapsed_current_eaw) : 0U);
    value[17] = (uint8_t)(state->presence_mode - 1U);
    value[18] = value[17];
    value[19] = value[17];
    value[20] = value[17];
    memcpy(value + 21, state->master, sizeof(state->master));
    value[27] = state->presence_mode;
    put_le16(value + 29, current_aw);
    put_le16(value + 31, current_aw);
    if (!make_channel_sequence(value + 33, 2U, state)) {
        return ESPDROP_AWDL_BUILD_INVALID_STATE;
    }
    cursor += 3U + AWDL_SYNC_VALUE_BYTES;

    value = append_tlv(cursor, 5U, AWDL_ELECTION_V1_VALUE_BYTES);
    value[3] = (uint8_t)(state->distance_to_master > UINT8_MAX
                             ? UINT8_MAX : state->distance_to_master);
    memcpy(value + 5, state->master, sizeof(state->master));
    put_le32(value + 11, state->master_metric);
    put_le32(value + 15, state->self_metric);
    cursor += 3U + AWDL_ELECTION_V1_VALUE_BYTES;

    value = append_tlv(cursor, 18U, AWDL_CHANNEL_VALUE_BYTES);
    if (!make_channel_sequence(value, 3U, state)) {
        return ESPDROP_AWDL_BUILD_INVALID_STATE;
    }
    cursor += 3U + AWDL_CHANNEL_VALUE_BYTES;

    value = append_tlv(cursor, 24U, AWDL_ELECTION_V2_VALUE_BYTES);
    memcpy(value, state->master, sizeof(state->master));
    memcpy(value + 6, state->sync_master, sizeof(state->sync_master));
    put_le32(value + 12, state->master_counter);
    put_le32(value + 16, state->distance_to_master);
    put_le32(value + 20, state->master_metric);
    put_le32(value + 24, state->self_metric);
    put_le32(value + 36, state->self_counter);
    cursor += 3U + AWDL_ELECTION_V2_VALUE_BYTES;

    value = append_tlv(cursor, 6U, AWDL_SERVICE_VALUE_BYTES);
    cursor += 3U + AWDL_SERVICE_VALUE_BYTES;

    if (subtype == ESPDROP_AWDL_ACTION_MIF) {
        value = append_tlv(cursor, 7U, AWDL_HT_VALUE_BYTES);
        value[2] = 0x6fU;
        value[4] = 0x1fU;
        value[5] = 0xffU;
        value[6] = 0xffU;
        cursor += 3U + AWDL_HT_VALUE_BYTES;

        value = append_tlv(cursor, 16U, (uint16_t)arpa_value_length);
        value[0] = 3U;
        value[1] = (uint8_t)name_length;
        memcpy(value + 2, state->name, name_length);
        value[2U + name_length] = 0xc0U;
        value[3U + name_length] = 0x0cU;
        cursor += 3U + arpa_value_length;
    }

    value = append_tlv(cursor, 12U, AWDL_DATA_PATH_VALUE_BYTES);
    put_le16(value, 0x8f24U);
    value[2] = 'X';
    value[3] = '0';
    value[4] = 0U;
    put_le16(value + 5, 0x0001U);
    memcpy(value + 7, state->self, sizeof(state->self));
    put_le16(value + 13, 0U);
    cursor += 3U + AWDL_DATA_PATH_VALUE_BYTES;

    value = append_tlv(cursor, 21U, AWDL_VERSION_VALUE_BYTES);
    value[0] = 0xa0U;
    value[1] = 1U;
    cursor += 3U + AWDL_VERSION_VALUE_BYTES;

    *length = (size_t)(cursor - frame);
    return *length == required ? ESPDROP_AWDL_BUILD_OK
                               : ESPDROP_AWDL_BUILD_INVALID_STATE;
}
