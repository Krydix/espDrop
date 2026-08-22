#include "espdrop/awdl_tlv.h"

#include <string.h>

#define AWDL_SYNC_FIXED_BYTES 33U
#define AWDL_CHANNEL_HEADER_BYTES 6U
#define AWDL_ELECTION_V1_BYTES 21U
#define AWDL_ELECTION_V2_BYTES 40U

static uint16_t read_le16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8U);
}

static uint32_t read_le32(const uint8_t *value)
{
    return (uint32_t)value[0] |
           ((uint32_t)value[1] << 8U) |
           ((uint32_t)value[2] << 16U) |
           ((uint32_t)value[3] << 24U);
}

void espdrop_awdl_tlv_iterator_init(
    espdrop_awdl_tlv_iterator_t *iterator,
    const uint8_t *data,
    size_t length)
{
    if (iterator == NULL) {
        return;
    }
    iterator->data = data;
    iterator->length = data == NULL ? 0U : length;
    iterator->offset = 0;
}

espdrop_awdl_parse_result_t espdrop_awdl_tlv_next(
    espdrop_awdl_tlv_iterator_t *iterator,
    espdrop_awdl_tlv_view_t *view)
{
    if (iterator == NULL || view == NULL ||
        (iterator->data == NULL && iterator->length != 0U) ||
        iterator->offset > iterator->length) {
        return ESPDROP_AWDL_PARSE_INVALID_ARGUMENT;
    }
    if (iterator->offset == iterator->length) {
        return ESPDROP_AWDL_PARSE_END;
    }
    const size_t remaining = iterator->length - iterator->offset;
    if (remaining < 3U) {
        return ESPDROP_AWDL_PARSE_TRUNCATED;
    }

    const uint8_t *header = iterator->data + iterator->offset;
    const uint16_t value_length = read_le16(header + 1);
    if ((size_t)value_length > remaining - 3U) {
        return ESPDROP_AWDL_PARSE_TRUNCATED;
    }

    view->type = header[0];
    view->length = value_length;
    view->value = header + 3;
    view->frame_offset = iterator->offset;
    iterator->offset += 3U + value_length;
    return ESPDROP_AWDL_PARSE_OK;
}

static size_t channel_encoding_size(uint8_t encoding)
{
    if (encoding == 0U) {
        return 1U;
    }
    if (encoding == 1U || encoding == 3U) {
        return 2U;
    }
    return 0U;
}

espdrop_awdl_parse_result_t espdrop_awdl_parse_channel_sequence(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_channel_sequence_t *sequence)
{
    if (value == NULL || sequence == NULL) {
        return ESPDROP_AWDL_PARSE_INVALID_ARGUMENT;
    }
    if (length < AWDL_CHANNEL_HEADER_BYTES) {
        return ESPDROP_AWDL_PARSE_TRUNCATED;
    }

    const size_t channel_count = (size_t)value[0] + 1U;
    const size_t encoded_size = channel_encoding_size(value[1]);
    if (channel_count == 0U || channel_count > ESPDROP_AWDL_MAX_CHANNELS) {
        return ESPDROP_AWDL_PARSE_INVALID_VALUE;
    }
    if (encoded_size == 0U) {
        return ESPDROP_AWDL_PARSE_UNSUPPORTED;
    }
    if (channel_count > (SIZE_MAX - AWDL_CHANNEL_HEADER_BYTES) / encoded_size ||
        length < AWDL_CHANNEL_HEADER_BYTES + channel_count * encoded_size) {
        return ESPDROP_AWDL_PARSE_TRUNCATED;
    }

    memset(sequence, 0, sizeof(*sequence));
    sequence->count = (uint8_t)channel_count;
    sequence->encoding = value[1];
    sequence->duplicate_count = value[2];
    sequence->step_count = value[3];
    sequence->fill_channel = read_le16(value + 4);
    for (size_t index = 0; index < channel_count; ++index) {
        const uint8_t *encoded = value + AWDL_CHANNEL_HEADER_BYTES +
                                 index * encoded_size;
        sequence->channels[index] = encoded[0];
        if (sequence->encoding == 3U) {
            sequence->operating_classes[index] = encoded[1];
        }
    }
    return ESPDROP_AWDL_PARSE_OK;
}

espdrop_awdl_parse_result_t espdrop_awdl_parse_sync_parameters(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_sync_parameters_t *parameters)
{
    if (value == NULL || parameters == NULL) {
        return ESPDROP_AWDL_PARSE_INVALID_ARGUMENT;
    }
    if (length < AWDL_SYNC_FIXED_BYTES) {
        return ESPDROP_AWDL_PARSE_TRUNCATED;
    }

    memset(parameters, 0, sizeof(*parameters));
    parameters->next_aw_channel = value[0];
    parameters->tx_down_counter = read_le16(value + 1);
    parameters->master_channel = value[3];
    parameters->guard_time = value[4];
    parameters->aw_period_tu = read_le16(value + 5);
    parameters->action_frame_period_tu = read_le16(value + 7);
    parameters->flags = read_le16(value + 9);
    parameters->extended_aw_length_tu = read_le16(value + 11);
    parameters->common_aw_length_tu = read_le16(value + 13);
    parameters->remaining_aw_length_tu = read_le16(value + 15);
    parameters->minimum_extension = value[17];
    parameters->maximum_multicast_extension = value[18];
    parameters->maximum_unicast_extension = value[19];
    parameters->maximum_action_frame_extension = value[20];
    memcpy(parameters->master, value + 21, sizeof(parameters->master));
    parameters->presence_mode = value[27];
    parameters->next_aw_sequence = read_le16(value + 29);
    parameters->ap_alignment = read_le16(value + 31);

    if (length > AWDL_SYNC_FIXED_BYTES) {
        const espdrop_awdl_parse_result_t result =
            espdrop_awdl_parse_channel_sequence(
                value + AWDL_SYNC_FIXED_BYTES,
                length - AWDL_SYNC_FIXED_BYTES,
                &parameters->embedded_channel_sequence);
        if (result != ESPDROP_AWDL_PARSE_OK) {
            return result;
        }
        parameters->has_embedded_channel_sequence = true;
    }
    return ESPDROP_AWDL_PARSE_OK;
}

espdrop_awdl_parse_result_t espdrop_awdl_parse_election_v1(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_election_v1_t *election)
{
    if (value == NULL || election == NULL) {
        return ESPDROP_AWDL_PARSE_INVALID_ARGUMENT;
    }
    if (length < AWDL_ELECTION_V1_BYTES) {
        return ESPDROP_AWDL_PARSE_TRUNCATED;
    }
    memset(election, 0, sizeof(*election));
    election->flags = value[0];
    election->identifier = read_le16(value + 1);
    election->distance_to_master = value[3];
    memcpy(election->master, value + 5, sizeof(election->master));
    election->master_metric = read_le32(value + 11);
    election->self_metric = read_le32(value + 15);
    return ESPDROP_AWDL_PARSE_OK;
}

espdrop_awdl_parse_result_t espdrop_awdl_parse_election_v2(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_election_v2_t *election)
{
    if (value == NULL || election == NULL) {
        return ESPDROP_AWDL_PARSE_INVALID_ARGUMENT;
    }
    if (length < AWDL_ELECTION_V2_BYTES) {
        return ESPDROP_AWDL_PARSE_TRUNCATED;
    }
    memset(election, 0, sizeof(*election));
    memcpy(election->master, value, sizeof(election->master));
    memcpy(election->sync_master, value + 6, sizeof(election->sync_master));
    election->master_counter = read_le32(value + 12);
    election->distance_to_master = read_le32(value + 16);
    election->master_metric = read_le32(value + 20);
    election->self_metric = read_le32(value + 24);
    election->self_counter = read_le32(value + 36);
    return ESPDROP_AWDL_PARSE_OK;
}

espdrop_awdl_parse_result_t espdrop_awdl_parse_mif(
    const espdrop_awdl_action_t *action,
    espdrop_awdl_mif_t *mif)
{
    if (action == NULL || mif == NULL || action->tlv_data == NULL ||
        action->subtype != ESPDROP_AWDL_ACTION_MIF) {
        return ESPDROP_AWDL_PARSE_INVALID_ARGUMENT;
    }
    memset(mif, 0, sizeof(*mif));

    espdrop_awdl_tlv_iterator_t iterator;
    espdrop_awdl_tlv_iterator_init(
        &iterator, action->tlv_data, action->tlv_length);
    while (true) {
        espdrop_awdl_tlv_view_t view;
        const espdrop_awdl_parse_result_t next =
            espdrop_awdl_tlv_next(&iterator, &view);
        if (next == ESPDROP_AWDL_PARSE_END) {
            return ESPDROP_AWDL_PARSE_OK;
        }
        if (next != ESPDROP_AWDL_PARSE_OK) {
            return next;
        }
        ++mif->tlv_count;

        espdrop_awdl_parse_result_t parsed = ESPDROP_AWDL_PARSE_OK;
        switch (view.type) {
            case ESPDROP_AWDL_TLV_SYNC_PARAMETERS:
                parsed = espdrop_awdl_parse_sync_parameters(
                    view.value, view.length, &mif->sync);
                mif->has_sync = parsed == ESPDROP_AWDL_PARSE_OK;
                break;
            case ESPDROP_AWDL_TLV_CHANNEL_SEQUENCE:
                parsed = espdrop_awdl_parse_channel_sequence(
                    view.value, view.length, &mif->channel_sequence);
                mif->has_channel_sequence =
                    parsed == ESPDROP_AWDL_PARSE_OK;
                break;
            case ESPDROP_AWDL_TLV_ELECTION_V1:
                parsed = espdrop_awdl_parse_election_v1(
                    view.value, view.length, &mif->election_v1);
                mif->has_election_v1 = parsed == ESPDROP_AWDL_PARSE_OK;
                break;
            case ESPDROP_AWDL_TLV_ELECTION_V2:
                parsed = espdrop_awdl_parse_election_v2(
                    view.value, view.length, &mif->election_v2);
                mif->has_election_v2 = parsed == ESPDROP_AWDL_PARSE_OK;
                break;
            default:
                break;
        }
        if (parsed != ESPDROP_AWDL_PARSE_OK) {
            return parsed;
        }
    }
}
