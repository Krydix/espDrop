#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espdrop/awdl_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_AWDL_MAX_CHANNELS 16

typedef enum {
    ESPDROP_AWDL_PARSE_OK = 0,
    ESPDROP_AWDL_PARSE_END = 1,
    ESPDROP_AWDL_PARSE_INVALID_ARGUMENT = -1,
    ESPDROP_AWDL_PARSE_TRUNCATED = -2,
    ESPDROP_AWDL_PARSE_UNSUPPORTED = -3,
    ESPDROP_AWDL_PARSE_INVALID_VALUE = -4,
} espdrop_awdl_parse_result_t;

typedef enum {
    ESPDROP_AWDL_TLV_SYNC_PARAMETERS = 4,
    ESPDROP_AWDL_TLV_ELECTION_V1 = 5,
    ESPDROP_AWDL_TLV_CHANNEL_SEQUENCE = 18,
    ESPDROP_AWDL_TLV_VERSION = 21,
    ESPDROP_AWDL_TLV_ELECTION_V2 = 24,
} espdrop_awdl_tlv_type_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
} espdrop_awdl_tlv_iterator_t;

typedef struct {
    uint8_t type;
    const uint8_t *value;
    uint16_t length;
    size_t frame_offset;
} espdrop_awdl_tlv_view_t;

typedef struct {
    uint8_t count;
    uint8_t encoding;
    uint8_t duplicate_count;
    uint8_t step_count;
    uint16_t fill_channel;
    uint8_t channels[ESPDROP_AWDL_MAX_CHANNELS];
    uint8_t operating_classes[ESPDROP_AWDL_MAX_CHANNELS];
} espdrop_awdl_channel_sequence_t;

typedef struct {
    uint8_t next_aw_channel;
    uint16_t tx_down_counter;
    uint8_t master_channel;
    uint8_t guard_time;
    uint16_t aw_period_tu;
    uint16_t action_frame_period_tu;
    uint16_t flags;
    uint16_t extended_aw_length_tu;
    uint16_t common_aw_length_tu;
    uint16_t remaining_aw_length_tu;
    uint8_t minimum_extension;
    uint8_t maximum_multicast_extension;
    uint8_t maximum_unicast_extension;
    uint8_t maximum_action_frame_extension;
    uint8_t master[6];
    uint8_t presence_mode;
    uint16_t next_aw_sequence;
    uint16_t ap_alignment;
    bool has_embedded_channel_sequence;
    espdrop_awdl_channel_sequence_t embedded_channel_sequence;
} espdrop_awdl_sync_parameters_t;

typedef struct {
    uint8_t flags;
    uint16_t identifier;
    uint8_t distance_to_master;
    uint8_t master[6];
    uint32_t master_metric;
    uint32_t self_metric;
} espdrop_awdl_election_v1_t;

typedef struct {
    uint8_t master[6];
    uint8_t sync_master[6];
    uint32_t master_counter;
    uint32_t distance_to_master;
    uint32_t master_metric;
    uint32_t self_metric;
    uint32_t self_counter;
} espdrop_awdl_election_v2_t;

typedef struct {
    uint8_t version;
    uint8_t device_class;
} espdrop_awdl_version_t;

typedef struct {
    uint16_t tlv_count;
    bool has_sync;
    bool has_channel_sequence;
    bool has_election_v1;
    bool has_election_v2;
    bool has_version;
    espdrop_awdl_sync_parameters_t sync;
    espdrop_awdl_channel_sequence_t channel_sequence;
    espdrop_awdl_election_v1_t election_v1;
    espdrop_awdl_election_v2_t election_v2;
    espdrop_awdl_version_t version;
} espdrop_awdl_mif_t;

void espdrop_awdl_tlv_iterator_init(
    espdrop_awdl_tlv_iterator_t *iterator,
    const uint8_t *data,
    size_t length);

espdrop_awdl_parse_result_t espdrop_awdl_tlv_next(
    espdrop_awdl_tlv_iterator_t *iterator,
    espdrop_awdl_tlv_view_t *view);

espdrop_awdl_parse_result_t espdrop_awdl_parse_channel_sequence(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_channel_sequence_t *sequence);

espdrop_awdl_parse_result_t espdrop_awdl_parse_sync_parameters(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_sync_parameters_t *parameters);

espdrop_awdl_parse_result_t espdrop_awdl_parse_election_v1(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_election_v1_t *election);

espdrop_awdl_parse_result_t espdrop_awdl_parse_election_v2(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_election_v2_t *election);

espdrop_awdl_parse_result_t espdrop_awdl_parse_version(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_version_t *version);

/* OWL src/peers.c considers a MIF sender a valid AWDL peer once the MIF also
 * supplies non-zero version and device-class values. This is a peer-table
 * predicate, not a network admission handshake. */
bool espdrop_awdl_mif_peer_valid(const espdrop_awdl_mif_t *mif);

espdrop_awdl_parse_result_t espdrop_awdl_parse_mif(
    const espdrop_awdl_action_t *action,
    espdrop_awdl_mif_t *mif);

#ifdef __cplusplus
}
#endif
