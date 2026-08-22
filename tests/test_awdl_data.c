#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/awdl_data.h"

static uint16_t read_be16(const uint8_t *value)
{
    return (uint16_t)((uint16_t)value[0] << 8U) | value[1];
}

static uint32_t add_words(uint32_t sum, const uint8_t *data, size_t length)
{
    while (length >= 2U) {
        sum += read_be16(data);
        data += 2;
        length -= 2U;
    }
    return sum;
}

static uint16_t fold(uint32_t sum)
{
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xffffU) + (sum >> 16U);
    }
    return (uint16_t)sum;
}

int main(void)
{
    const uint8_t self[6] = {0x1c, 0xdb, 0xd4, 0x42, 0x3f, 0xa0};
    const uint8_t target[6] = {0xea, 0x33, 0x2c, 0x82, 0xf5, 0x7f};
    const uint8_t expected_self_ip[16] = {
        0xfe, 0x80, 0, 0, 0, 0, 0, 0,
        0x1e, 0xdb, 0xd4, 0xff, 0xfe, 0x42, 0x3f, 0xa0,
    };
    const uint8_t expected_target_ip[16] = {
        0xfe, 0x80, 0, 0, 0, 0, 0, 0,
        0xe8, 0x33, 0x2c, 0xff, 0xfe, 0x82, 0xf5, 0x7f,
    };

    uint8_t address[16];
    espdrop_awdl_link_local_from_mac(self, address);
    assert(memcmp(address, expected_self_ip, sizeof(address)) == 0);

    uint8_t frame[ESPDROP_AWDL_NS_FRAME_BYTES];
    size_t length = 0U;
    assert(espdrop_awdl_build_neighbor_solicitation(
        frame, sizeof(frame), &length, self, target, 0x123U, 0x4567U));
    assert(length == ESPDROP_AWDL_NS_FRAME_BYTES);
    assert(frame[0] == 0x08U);
    assert(memcmp(frame + 4, target, 6U) == 0);
    assert(memcmp(frame + 10, self, 6U) == 0);
    assert(frame[22] == 0x30U && frame[23] == 0x12U);
    assert(memcmp(frame + 24, "\xaa\xaa\x03\0\0\0\x08\0", 8U) == 0);
    assert(frame[32] == 0x03U && frame[33] == 0x04U);
    assert(frame[34] == 0x67U && frame[35] == 0x45U);
    assert(frame[38] == 0x86U && frame[39] == 0xddU);

    espdrop_awdl_data_t data;
    assert(espdrop_awdl_decode_data(frame, length, &data));
    assert(!data.qos && !data.amsdu);
    assert(data.sequence == 0x4567U);
    assert(data.ethertype == 0x86ddU);
    assert(data.payload_length == 72U);

    espdrop_awdl_ipv6_t ipv6;
    assert(espdrop_awdl_decode_ipv6(&data, &ipv6));
    assert(memcmp(ipv6.source, expected_self_ip, 16U) == 0);
    assert(memcmp(ipv6.destination, expected_target_ip, 16U) == 0);
    assert(ipv6.next_header == 58U);
    assert(ipv6.hop_limit == 255U);
    assert(ipv6.icmp_type == 135U && ipv6.icmp_code == 0U);
    assert(ipv6.icmp_payload_length == 28U);

    const uint8_t *ip = data.payload;
    const uint8_t *icmp = ip + 40;
    uint32_t checksum = 0U;
    checksum = add_words(checksum, ip + 8, 16U);
    checksum = add_words(checksum, ip + 24, 16U);
    const uint8_t pseudo_tail[8] = {0, 0, 0, 32, 0, 0, 0, 58};
    checksum = add_words(checksum, pseudo_tail, sizeof(pseudo_tail));
    checksum = add_words(checksum, icmp, 32U);
    assert(fold(checksum) == 0xffffU);

    assert(!espdrop_awdl_build_neighbor_solicitation(
        frame, sizeof(frame) - 1U, &length, self, target, 0, 0));
    frame[16] ^= 1U;
    assert(!espdrop_awdl_decode_data(frame, sizeof(frame), &data));

    uint8_t echo[ESPDROP_AWDL_ECHO_FRAME_BYTES];
    assert(espdrop_awdl_build_echo_request(
        echo, sizeof(echo), &length, self, target, 0x321U, 0x7654U,
        0xed01U, 7U));
    assert(length == ESPDROP_AWDL_ECHO_FRAME_BYTES);
    assert(echo[22] == 0x10U && echo[23] == 0x32U);
    assert(espdrop_awdl_decode_data(echo, length, &data));
    assert(data.sequence == 0x7654U);
    assert(espdrop_awdl_decode_ipv6(&data, &ipv6));
    assert(ipv6.icmp_type == 128U && ipv6.icmp_code == 0U);
    assert(ipv6.hop_limit == 64U);
    assert(ipv6.icmp_payload_length == 12U);
    assert(read_be16(ipv6.icmp_payload) == 0xed01U);
    assert(read_be16(ipv6.icmp_payload + 2) == 7U);
    assert(memcmp(ipv6.icmp_payload + 4, "espDrop!", 8U) == 0);

    ip = data.payload;
    icmp = ip + 40;
    checksum = 0U;
    checksum = add_words(checksum, ip + 8, 16U);
    checksum = add_words(checksum, ip + 24, 16U);
    const uint8_t echo_pseudo_tail[8] = {0, 0, 0, 16, 0, 0, 0, 58};
    checksum = add_words(checksum, echo_pseudo_tail,
                         sizeof(echo_pseudo_tail));
    checksum = add_words(checksum, icmp, 16U);
    assert(fold(checksum) == 0xffffU);
    assert(!espdrop_awdl_build_echo_request(
        echo, sizeof(echo) - 1U, &length, self, target, 0, 0, 0, 0));

    puts("AWDL data tests passed");
    return 0;
}
