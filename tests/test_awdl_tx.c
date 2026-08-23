#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/awdl_tx.h"

static uint16_t read_le16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8U);
}

int main(void)
{
    const uint8_t self[6] = {0x1c, 0xdb, 0xd4, 0x42, 0x3f, 0xa0};
    const uint8_t source[6] = {0xea, 0x33, 0x2c, 0x82, 0xf5, 0x7f};
    espdrop_awdl_mif_t observed = {
        .has_sync = true,
        .has_election_v2 = true,
        .has_channel_sequence = true,
        .sync = {
            .tx_down_counter = 35,
            .master_channel = 6,
            .aw_period_tu = 16,
            .action_frame_period_tu = 110,
            .presence_mode = 4,
            .next_aw_sequence = 64289,
        },
        .election_v2 = {
            .master_counter = 741,
            .distance_to_master = 0,
            .master_metric = 510,
            .self_metric = 510,
            .self_counter = 741,
        },
        .channel_sequence = {
            .count = 16,
            .encoding = 3,
            .step_count = 3,
            .fill_channel = 0xffff,
        },
    };
    for (size_t index = 0; index < 16U; ++index) {
        observed.channel_sequence.channels[index] = 44U;
        observed.channel_sequence.operating_classes[index] = 0x80U;
    }
    observed.channel_sequence.channels[8] = 6U;
    observed.channel_sequence.operating_classes[8] = 0x51U;
    memcpy(observed.election_v2.master, source, sizeof(source));
    memcpy(observed.election_v2.sync_master, source, sizeof(source));

    espdrop_awdl_tx_state_t state;
    assert(espdrop_awdl_tx_state_from_mif(
        &state, self, source, "espDrop", 0x12345678U, &observed,
        1000000ULL));
    assert(memcmp(state.self, self, sizeof(self)) == 0);
    assert(memcmp(state.master, source, sizeof(source)) == 0);
    assert(state.distance_to_master == 1);
    assert(state.master_metric == 510);
    assert(state.self_metric == ESPDROP_AWDL_ELECTION_METRIC_INITIAL);
    assert(state.channel == 6);
    assert(state.peer_channel_count == 16U);
    assert(state.peer_channel_encoding == 3U);
    assert(state.peer_channels[8] == 6U);
    assert(state.peer_operating_classes[8] == 0x51U);
    assert(state.aw_sequence_base == 64288);
    assert(state.sync_reference_us == 1000000ULL - 29ULL * 1024ULL);
    assert(state.peer_time_observed_us == 1000000ULL);
    assert(state.peer_time_reference == 0x12345678U);

    uint64_t scheduled_us = 0U;
    assert(espdrop_awdl_next_channel_window_us(
        &state, 6U, state.sync_reference_us + 1000U, 3000U,
        &scheduled_us));
    assert(scheduled_us == state.sync_reference_us + 3000U);
    assert(espdrop_awdl_next_channel_window_us(
        &state, 6U, state.sync_reference_us + 4000U, 3000U,
        &scheduled_us));
    assert(scheduled_us == state.sync_reference_us +
                               16ULL * 16ULL * 4ULL * 1024ULL + 3000ULL);

    const uint64_t eaw_us = 16ULL * 4ULL * 1024ULL;
    espdrop_awdl_tx_state_t phase_local = state;
    phase_local.aw_sequence_base = 0U;
    memset(phase_local.peer_channels, 0,
           sizeof(phase_local.peer_channels));
    phase_local.peer_channels[8] = 6U;
    espdrop_awdl_tx_state_t phase_peer = phase_local;
    memset(phase_peer.peer_channels, 0,
           sizeof(phase_peer.peer_channels));
    phase_peer.peer_channels[9] = 6U;
    phase_peer.sync_reference_us = phase_local.sync_reference_us - eaw_us;
    assert(espdrop_awdl_next_common_channel_window_us(
        &phase_local, &phase_peer, 6U,
        phase_local.sync_reference_us + 7ULL * eaw_us, 3000U,
        &scheduled_us));
    assert(scheduled_us ==
           phase_local.sync_reference_us + 8ULL * eaw_us + 3000ULL);
    assert(espdrop_awdl_next_common_channel_window_us(
        &phase_local, &phase_peer, 6U, scheduled_us + 1U, 3000U,
        &scheduled_us));
    assert(scheduled_us ==
           phase_local.sync_reference_us + 24ULL * eaw_us + 3000ULL);

    phase_peer.sync_reference_us = phase_local.sync_reference_us;
    assert(!espdrop_awdl_next_common_channel_window_us(
        &phase_local, &phase_peer, 6U,
        phase_local.sync_reference_us + 7ULL * eaw_us, 3000U,
        &scheduled_us));
    assert(!espdrop_awdl_next_common_channel_window_us(
        &phase_local, &phase_peer, 6U,
        phase_local.sync_reference_us + 7ULL * eaw_us, 33000U,
        &scheduled_us));
    phase_peer.presence_mode = 2U;
    assert(!espdrop_awdl_next_common_channel_window_us(
        &phase_local, &phase_peer, 6U,
        phase_local.sync_reference_us + 7ULL * eaw_us, 3000U,
        &scheduled_us));

    uint8_t frame[ESPDROP_AWDL_TX_FRAME_CAPACITY];
    size_t length = 0;
    assert(espdrop_awdl_build_action(
               frame, sizeof(frame), &length, &state,
               ESPDROP_AWDL_ACTION_MIF, 1000000ULL, 7) ==
           ESPDROP_AWDL_BUILD_OK);
    assert(length < sizeof(frame));
    assert(frame[0] == 0xd0);
    assert(memcmp(frame + 4, "\xff\xff\xff\xff\xff\xff", 6) == 0);
    assert(memcmp(frame + 10, self, sizeof(self)) == 0);
    assert(read_le16(frame + 22) == (7U << 4U));

    espdrop_awdl_action_t action;
    assert(espdrop_awdl_decode_action(frame, length, &action));
    assert(action.subtype == ESPDROP_AWDL_ACTION_MIF);
    assert(action.phy_tx == 0x12345678U);
    assert(action.target_tx == 0x12345678U);

    espdrop_awdl_mif_t parsed;
    assert(espdrop_awdl_parse_mif(&action, &parsed) ==
           ESPDROP_AWDL_PARSE_OK);
    assert(parsed.tlv_count == 9);
    assert(parsed.has_sync);
    assert(parsed.sync.tx_down_counter == 35);
    assert(parsed.sync.next_aw_sequence == 64289);
    assert(parsed.sync.master_channel == 6);
    assert(parsed.sync.has_embedded_channel_sequence);
    assert(parsed.sync.embedded_channel_sequence.channels[0] == 44);
    assert(parsed.sync.embedded_channel_sequence.operating_classes[0] ==
           0x80U);
    assert(parsed.has_election_v1);
    assert(parsed.has_election_v2);
    assert(parsed.election_v2.distance_to_master == 1);
    assert(parsed.election_v2.self_metric ==
           ESPDROP_AWDL_ELECTION_METRIC_INITIAL);
    assert(parsed.has_channel_sequence);
    assert(parsed.channel_sequence.channels[8] == 6);
    assert(parsed.channel_sequence.operating_classes[8] == 0x51U);
    assert(parsed.channel_sequence.channels[15] == 44);

    bool found_current_arpa = false;
    bool found_current_version = false;
    espdrop_awdl_tlv_iterator_t iterator;
    espdrop_awdl_tlv_iterator_init(&iterator, action.tlv_data,
                                   action.tlv_length);
    while (true) {
        espdrop_awdl_tlv_view_t view;
        const espdrop_awdl_parse_result_t result =
            espdrop_awdl_tlv_next(&iterator, &view);
        if (result == ESPDROP_AWDL_PARSE_END) {
            break;
        }
        assert(result == ESPDROP_AWDL_PARSE_OK);
        if (view.type == 16U) {
            assert(view.length == 11U);
            assert(view.value[0] == 3U);
            assert(view.value[1] == 7U);
            assert(memcmp(view.value + 2, "espDrop", 7) == 0);
            found_current_arpa = true;
        }
        if (view.type == 21U) {
            assert(view.length == 2U);
            assert(view.value[0] == 0xa0U);
            assert(view.value[1] == 1U);
            found_current_version = true;
        }
    }
    assert(found_current_arpa);
    assert(found_current_version);

    size_t psf_length = 0;
    assert(espdrop_awdl_build_action(
               frame, sizeof(frame), &psf_length, &state,
               ESPDROP_AWDL_ACTION_PSF, 1010000ULL, 8) ==
           ESPDROP_AWDL_BUILD_OK);
    assert(psf_length < length);
    assert(espdrop_awdl_decode_action(frame, psf_length, &action));
    assert(action.subtype == ESPDROP_AWDL_ACTION_PSF);
    assert(action.phy_tx == 0x12347d88U);
    assert(action.target_tx == 0x12347d88U);
    assert(espdrop_awdl_build_action(
               frame, 32, &length, &state, ESPDROP_AWDL_ACTION_PSF,
               1010000ULL, 9) == ESPDROP_AWDL_BUILD_NO_SPACE);

    const uint8_t distance_one_peer[6] = {
        0xa6, 0xed, 0x54, 0x02, 0x5b, 0x4e,
    };
    const uint8_t distance_zero_master[6] = {
        0x52, 0xf4, 0x36, 0xb8, 0xfd, 0xf5,
    };
    observed.election_v2.distance_to_master = 1U;
    memcpy(observed.election_v2.master, distance_zero_master,
           sizeof(distance_zero_master));
    memcpy(observed.election_v2.sync_master, distance_zero_master,
           sizeof(distance_zero_master));
    assert(espdrop_awdl_tx_state_from_mif(
        &state, self, distance_one_peer, "espDrop", 0x12345678U,
        &observed, 1000000ULL));
    assert(memcmp(state.master, distance_zero_master,
                  sizeof(distance_zero_master)) == 0);
    assert(memcmp(state.sync_master, distance_one_peer,
                  sizeof(distance_one_peer)) == 0);
    assert(state.distance_to_master == 2U);

    espdrop_awdl_election_state_t elected = {
        .distance_to_master = 1U,
        .master_metric = 510U,
        .self_metric = ESPDROP_AWDL_ELECTION_METRIC_INITIAL,
        .master_counter = 741U,
        .self_counter = ESPDROP_AWDL_ELECTION_COUNTER_INITIAL,
    };
    memcpy(elected.self, self, sizeof(self));
    memcpy(elected.master, distance_zero_master,
           sizeof(distance_zero_master));
    memcpy(elected.sync_master, distance_zero_master,
           sizeof(distance_zero_master));
    assert(espdrop_awdl_tx_state_apply_election(&state, &elected));
    assert(memcmp(state.sync_master, distance_zero_master,
                  sizeof(distance_zero_master)) == 0);
    assert(state.distance_to_master == 1U);
    assert(espdrop_awdl_build_action(
               frame, sizeof(frame), &length, &state,
               ESPDROP_AWDL_ACTION_MIF, 1000000ULL, 10U) ==
           ESPDROP_AWDL_BUILD_OK);
    assert(espdrop_awdl_decode_action(frame, length, &action));
    assert(espdrop_awdl_parse_mif(&action, &parsed) ==
           ESPDROP_AWDL_PARSE_OK);
    assert(memcmp(parsed.election_v2.sync_master, distance_zero_master,
                  sizeof(distance_zero_master)) == 0);
    assert(parsed.election_v2.distance_to_master == 1U);
    assert(parsed.election_v2.self_metric ==
           ESPDROP_AWDL_ELECTION_METRIC_INITIAL);

    puts("AWDL TX tests passed");
    return 0;
}
