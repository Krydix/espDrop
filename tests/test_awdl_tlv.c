#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/awdl_tlv.h"

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

static size_t append_tlv(
    uint8_t *output,
    uint8_t type,
    const uint8_t *value,
    uint16_t length)
{
    output[0] = type;
    put_le16(output + 1, length);
    memcpy(output + 3, value, length);
    return (size_t)length + 3U;
}

static void make_channel_sequence(uint8_t value[38])
{
    memset(value, 0, 38);
    value[0] = 15;
    value[1] = 3;
    value[2] = 0;
    value[3] = 3;
    put_le16(value + 4, 0xffff);
    for (size_t index = 0; index < 16; ++index) {
        value[6 + index * 2] = index == 8 ? 6 : 149;
        value[7 + index * 2] = index == 8 ? 0x51 : 0x80;
    }
}

static size_t load_hex_fixture(const char *path, uint8_t *output, size_t capacity)
{
    FILE *fixture = fopen(path, "r");
    assert(fixture != NULL);
    size_t length = 0;
    int high_nibble = -1;
    int character;
    while ((character = fgetc(fixture)) != EOF) {
        if (isspace((unsigned char)character)) {
            continue;
        }
        assert(isxdigit((unsigned char)character));
        const int value = character >= '0' && character <= '9'
                              ? character - '0'
                              : (tolower((unsigned char)character) - 'a' + 10);
        if (high_nibble < 0) {
            high_nibble = value;
        } else {
            assert(length < capacity);
            output[length++] = (uint8_t)((high_nibble << 4) | value);
            high_nibble = -1;
        }
    }
    assert(high_nibble < 0);
    assert(fclose(fixture) == 0);
    return length;
}

int main(void)
{
    uint8_t tlvs[256] = {0};
    size_t length = 0;

    uint8_t sync[71] = {0};
    sync[0] = 6;
    put_le16(sync + 1, 31);
    sync[3] = 149;
    put_le16(sync + 5, 16);
    put_le16(sync + 7, 110);
    put_le16(sync + 11, 16);
    put_le16(sync + 13, 16);
    const uint8_t master[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(sync + 21, master, sizeof(master));
    sync[27] = 4;
    put_le16(sync + 29, 0x1234);
    put_le16(sync + 31, 0x5678);
    make_channel_sequence(sync + 33);
    length += append_tlv(tlvs + length, ESPDROP_AWDL_TLV_SYNC_PARAMETERS,
                         sync, sizeof(sync));

    uint8_t election[40] = {0};
    memcpy(election, master, sizeof(master));
    memcpy(election + 6, master, sizeof(master));
    put_le32(election + 12, 1000);
    put_le32(election + 16, 1);
    put_le32(election + 20, 60);
    put_le32(election + 24, 55);
    put_le32(election + 36, 1001);
    length += append_tlv(tlvs + length, ESPDROP_AWDL_TLV_ELECTION_V2,
                         election, sizeof(election));

    uint8_t standalone_sequence[38];
    make_channel_sequence(standalone_sequence);
    length += append_tlv(tlvs + length, ESPDROP_AWDL_TLV_CHANNEL_SEQUENCE,
                         standalone_sequence, sizeof(standalone_sequence));

    const espdrop_awdl_action_t action = {
        .version = 0x10,
        .subtype = ESPDROP_AWDL_ACTION_MIF,
        .tlv_data = tlvs,
        .tlv_length = length,
    };
    espdrop_awdl_mif_t mif;
    assert(espdrop_awdl_parse_mif(&action, &mif) == ESPDROP_AWDL_PARSE_OK);
    assert(mif.tlv_count == 3);
    assert(mif.has_sync);
    assert(mif.sync.aw_period_tu == 16);
    assert(mif.sync.next_aw_sequence == 0x1234);
    assert(mif.sync.has_embedded_channel_sequence);
    assert(mif.sync.embedded_channel_sequence.count == 16);
    assert(mif.sync.embedded_channel_sequence.channels[8] == 6);
    assert(mif.has_election_v2);
    assert(mif.election_v2.master_counter == 1000);
    assert(mif.election_v2.self_counter == 1001);
    assert(mif.has_channel_sequence);
    assert(mif.channel_sequence.channels[0] == 149);

    espdrop_awdl_tlv_iterator_t iterator;
    espdrop_awdl_tlv_view_t view;
    espdrop_awdl_tlv_iterator_init(&iterator, tlvs, 2);
    assert(espdrop_awdl_tlv_next(&iterator, &view) ==
           ESPDROP_AWDL_PARSE_TRUNCATED);

    uint8_t invalid_sequence[6] = {16, 3, 0, 3, 0xff, 0xff};
    espdrop_awdl_channel_sequence_t parsed_sequence;
    assert(espdrop_awdl_parse_channel_sequence(
               invalid_sequence, sizeof(invalid_sequence), &parsed_sequence) ==
           ESPDROP_AWDL_PARSE_INVALID_VALUE);

    uint8_t captured_frame[256];
    const size_t captured_length = load_hex_fixture(
        "tests/fixtures/awdl-mif-core.hex", captured_frame,
        sizeof(captured_frame));
    espdrop_awdl_action_t captured_action;
    assert(captured_length == 227);
    assert(espdrop_awdl_decode_action(captured_frame, captured_length,
                                      &captured_action));
    assert(captured_action.version == 0x10);
    assert(captured_action.subtype == ESPDROP_AWDL_ACTION_MIF);
    assert(espdrop_awdl_parse_mif(&captured_action, &mif) ==
           ESPDROP_AWDL_PARSE_OK);
    assert(mif.tlv_count == 4);
    assert(mif.has_sync && mif.sync.aw_period_tu == 16);
    assert(mif.sync.action_frame_period_tu == 110);
    assert(mif.sync.presence_mode == 4);
    assert(mif.sync.tx_down_counter == 35);
    assert(mif.sync.next_aw_sequence == 64289);
    assert(mif.has_election_v1 && mif.has_election_v2);
    assert(mif.election_v2.distance_to_master == 1);
    assert(mif.election_v2.master_metric == 510);
    assert(mif.election_v2.self_metric == 510);
    assert(mif.election_v2.master_counter == 741);
    assert(mif.election_v2.self_counter == 398);
    assert(mif.has_channel_sequence);
    assert(mif.channel_sequence.count == 16);
    assert(mif.channel_sequence.channels[0] == 44);
    assert(mif.channel_sequence.channels[8] == 6);

    puts("AWDL TLV tests passed");
    return 0;
}
