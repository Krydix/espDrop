#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/awdl_service.h"

static void put_le16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static size_t append_record(
    uint8_t *output,
    const uint8_t *name,
    size_t name_length,
    uint8_t record_type)
{
    const uint8_t null_target[2] = {0xc0U, 0x00U};
    const uint16_t value_length =
        (uint16_t)(2U + name_length + 1U + 2U + 2U + sizeof(null_target));
    output[0] = 2U;
    put_le16(output + 1U, value_length);
    put_le16(output + 3U, (uint16_t)(name_length + 1U));
    memcpy(output + 5U, name, name_length);
    output[5U + name_length] = record_type;
    put_le16(output + 6U + name_length, sizeof(null_target));
    put_le16(output + 8U + name_length, 0U);
    memcpy(output + 10U + name_length, null_target, sizeof(null_target));
    return 3U + value_length;
}

int main(void)
{
    uint8_t tlvs[128];
    size_t length = 0U;
    const uint8_t airdrop_name[2] = {0xc0U, 0x07U};
    length += append_record(tlvs + length, airdrop_name,
                            sizeof(airdrop_name), 12U);
    const uint8_t instance_name[] = {
        4U, 'd', 'e', 'm', 'o', 0xc0U, 0x07U,
    };
    length += append_record(tlvs + length, instance_name,
                            sizeof(instance_name), 33U);
    const uint8_t asquic_name[] = {
        7U, '_', 'a', 's', 'q', 'u', 'i', 'c', 0xc0U, 0x0bU,
    };
    length += append_record(tlvs + length, asquic_name,
                            sizeof(asquic_name), 12U);

    espdrop_awdl_service_profile_t profile;
    assert(espdrop_awdl_scan_service_responses(
               tlvs, length, &profile) == ESPDROP_AWDL_PARSE_OK);
    assert(profile.record_count == 3U);
    assert(profile.malformed_record_count == 0U);
    assert(profile.ptr_count == 2U);
    assert(profile.srv_count == 1U);
    assert(profile.txt_count == 0U);
    assert(profile.has_airdrop);
    assert(profile.has_airdrop_tcp);
    assert(!profile.has_airdrop_udp);
    assert(profile.has_asquic);

    const uint8_t expanded_airdrop[] = {
        8U, '_', 'a', 'i', 'r', 'd', 'r', 'o', 'p',
        4U, '_', 't', 'c', 'p',
        5U, 'l', 'o', 'c', 'a', 'l', 0U,
    };
    length = append_record(tlvs, expanded_airdrop,
                           sizeof(expanded_airdrop), 16U);
    assert(espdrop_awdl_scan_service_responses(
               tlvs, length, &profile) == ESPDROP_AWDL_PARSE_OK);
    assert(profile.record_count == 1U);
    assert(profile.has_airdrop);
    assert(profile.has_airdrop_tcp);
    assert(!profile.has_airdrop_udp);
    assert(!profile.has_asquic);

    uint8_t malformed[] = {
        2U, 8U, 0U,
        3U, 0U, 0xc0U, 0x07U, 12U, 99U, 0U, 0U,
    };
    assert(espdrop_awdl_scan_service_responses(
               malformed, sizeof(malformed), &profile) ==
           ESPDROP_AWDL_PARSE_INVALID_VALUE);
    assert(profile.record_count == 0U);
    assert(profile.malformed_record_count == 1U);
    assert(!profile.has_airdrop);

    const uint8_t truncated_tlv[] = {2U, 9U, 0U, 0U};
    assert(espdrop_awdl_scan_service_responses(
               truncated_tlv, sizeof(truncated_tlv), &profile) ==
           ESPDROP_AWDL_PARSE_TRUNCATED);
    assert(espdrop_awdl_scan_service_responses(
               NULL, 1U, &profile) == ESPDROP_AWDL_PARSE_INVALID_ARGUMENT);
    assert(espdrop_awdl_scan_service_responses(
               NULL, 0U, &profile) == ESPDROP_AWDL_PARSE_OK);

    puts("AWDL service-response tests passed");
    return 0;
}
