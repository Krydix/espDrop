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
    assert(memcmp(frame + 24,
                  "\xaa\xaa\x03\x00\x17\xf2\x08\x00", 8U) == 0);
    assert(frame[32] == 0x03U && frame[33] == 0x04U);
    assert(frame[34] == 0x67U && frame[35] == 0x45U);
    assert(frame[38] == 0x86U && frame[39] == 0xddU);

    espdrop_awdl_data_t data;
    assert(espdrop_awdl_decode_data(frame, length, &data));
    assert(!data.qos && !data.amsdu);
    assert(data.sequence == 0x4567U);
    assert(data.ethertype == 0x86ddU);
    assert(data.payload_length == 72U);
    assert(espdrop_awdl_decode_data_ex(frame, length, &data) ==
           ESPDROP_AWDL_DATA_DECODE_OK);

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
    assert(espdrop_awdl_decode_data_ex(frame, sizeof(frame), &data) ==
           ESPDROP_AWDL_DATA_DECODE_BSSID);
    frame[16] ^= 1U;

    uint8_t qos_frame[128] = {0};
    memcpy(qos_frame, frame, 24U);
    qos_frame[0] = 0x88U;
    memcpy(qos_frame + 26U, frame + 24U, length - 24U);
    assert(espdrop_awdl_decode_data(qos_frame, length + 2U, &data));
    assert(data.qos && !data.amsdu);
    assert(data.sequence == 0x4567U);

    uint8_t amsdu_frame[160] = {0};
    memcpy(amsdu_frame, frame, 24U);
    amsdu_frame[0] = 0x88U;
    amsdu_frame[24] = 0x80U;
    memcpy(amsdu_frame + 26U, target, 6U);
    memcpy(amsdu_frame + 32U, self, 6U);
    const size_t msdu_length = length - 24U;
    amsdu_frame[38] = (uint8_t)(msdu_length >> 8U);
    amsdu_frame[39] = (uint8_t)msdu_length;
    memcpy(amsdu_frame + 40U, frame + 24U, msdu_length);
    const size_t amsdu_length = 40U + msdu_length;
    assert(espdrop_awdl_decode_data(amsdu_frame, amsdu_length, &data));
    assert(data.qos && data.amsdu);
    assert(memcmp(data.destination, target, 6U) == 0);
    assert(memcmp(data.source, self, 6U) == 0);
    assert(data.sequence == 0x4567U);
    amsdu_frame[38] = 0xffU;
    amsdu_frame[39] = 0xffU;
    assert(espdrop_awdl_decode_data_ex(
               amsdu_frame, amsdu_length, &data) ==
           ESPDROP_AWDL_DATA_DECODE_AMSDU_LENGTH);

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

    uint8_t ethernet[ESPDROP_ETHERNET_HEADER_BYTES + 40U] = {0};
    const uint8_t mdns_multicast[6] = {0x33, 0x33, 0, 0, 0, 0xfb};
    memcpy(ethernet, mdns_multicast, 6U);
    memcpy(ethernet + 6, self, 6U);
    ethernet[12] = 0x86U;
    ethernet[13] = 0xddU;
    ethernet[14] = 0x60U;
    ethernet[20] = 17U;
    ethernet[21] = 255U;

    uint8_t generic[128];
    assert(espdrop_awdl_build_ethernet_frame(
        generic, sizeof(generic), &length, ethernet, sizeof(ethernet), self,
        0x234U, 0x8abcU));
    assert(length == ESPDROP_AWDL_DATA_FRAME_OVERHEAD + 40U);
    assert(memcmp(generic + 4, mdns_multicast, 6U) == 0);
    assert(memcmp(generic + 10, self, 6U) == 0);
    assert(generic[22] == 0x40U && generic[23] == 0x23U);
    assert(espdrop_awdl_decode_data(generic, length, &data));
    assert(data.sequence == 0x8abcU);
    assert(data.ethertype == 0x86ddU);
    assert(data.payload_length == 40U);

    uint8_t decoded_ethernet[sizeof(ethernet)];
    size_t decoded_ethernet_length = 0U;
    assert(espdrop_awdl_data_to_ethernet(
        &data, decoded_ethernet, sizeof(decoded_ethernet),
        &decoded_ethernet_length));
    assert(decoded_ethernet_length == sizeof(ethernet));
    assert(memcmp(decoded_ethernet, ethernet, sizeof(ethernet)) == 0);
    assert(!espdrop_awdl_build_ethernet_frame(
        generic, sizeof(generic), &length, ethernet,
        ESPDROP_ETHERNET_HEADER_BYTES - 1U, self, 0, 0));
    assert(!espdrop_awdl_data_to_ethernet(
        &data, decoded_ethernet, sizeof(decoded_ethernet) - 1U,
        &decoded_ethernet_length));

    uint8_t tcp_ethernet[ESPDROP_ETHERNET_HEADER_BYTES + 60U] = {0};
    memcpy(tcp_ethernet, target, 6U);
    memcpy(tcp_ethernet + 6U, self, 6U);
    tcp_ethernet[12] = 0x86U;
    tcp_ethernet[13] = 0xddU;
    tcp_ethernet[14] = 0x60U;
    tcp_ethernet[18] = 0U;
    tcp_ethernet[19] = 20U;
    tcp_ethernet[20] = 6U;
    tcp_ethernet[21] = 64U;
    memcpy(tcp_ethernet + 22U, expected_self_ip, 16U);
    memcpy(tcp_ethernet + 38U, expected_target_ip, 16U);
    tcp_ethernet[54] = 0xc3U;
    tcp_ethernet[55] = 0x50U;
    tcp_ethernet[56] = 0x22U;
    tcp_ethernet[57] = 0x42U;
    tcp_ethernet[58] = 0x01U;
    tcp_ethernet[59] = 0x02U;
    tcp_ethernet[60] = 0x03U;
    tcp_ethernet[61] = 0x04U;
    tcp_ethernet[62] = 0x05U;
    tcp_ethernet[63] = 0x06U;
    tcp_ethernet[64] = 0x07U;
    tcp_ethernet[65] = 0x08U;
    tcp_ethernet[66] = 0x50U;
    tcp_ethernet[67] = 0x12U;
    tcp_ethernet[68] = 0x10U;
    tcp_ethernet[69] = 0x00U;
    uint32_t tcp_checksum = 0U;
    tcp_checksum = add_words(tcp_checksum, tcp_ethernet + 22U, 16U);
    tcp_checksum = add_words(tcp_checksum, tcp_ethernet + 38U, 16U);
    const uint8_t tcp_pseudo_tail[8] = {0, 0, 0, 20, 0, 0, 0, 6};
    tcp_checksum = add_words(tcp_checksum, tcp_pseudo_tail,
                             sizeof(tcp_pseudo_tail));
    tcp_checksum = add_words(tcp_checksum, tcp_ethernet + 54U, 20U);
    const uint16_t tcp_checksum_value = (uint16_t)~fold(tcp_checksum);
    tcp_ethernet[70] = (uint8_t)(tcp_checksum_value >> 8U);
    tcp_ethernet[71] = (uint8_t)tcp_checksum_value;
    assert(espdrop_awdl_build_ethernet_frame(
        generic, sizeof(generic), &length, tcp_ethernet,
        sizeof(tcp_ethernet), self, 0x235U, 0x8abdU));
    assert(espdrop_awdl_decode_data(generic, length, &data));
    espdrop_awdl_tcp_t tcp;
    assert(espdrop_awdl_decode_tcp(&data, &tcp));
    assert(tcp.source_port == 50000U);
    assert(tcp.destination_port == 8770U);
    assert(tcp.sequence == 0x01020304U);
    assert(tcp.acknowledgment == 0x05060708U);
    assert(tcp.header_length == 20U);
    assert(tcp.flags == 0x12U);
    assert(tcp.window == 4096U);
    assert(tcp.payload_length == 0U);
    assert(tcp.checksum_valid);
    generic[46] = 17U;
    assert(!espdrop_awdl_decode_tcp(&data, &tcp));

    puts("AWDL data tests passed");
    return 0;
}
