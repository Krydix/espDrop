#include "espdrop/awdl_data.h"

#include <string.h>

#define IEEE80211_HEADER_BYTES 24U
#define IEEE80211_QOS_BYTES 2U
#define AMSDU_SUBFRAME_HEADER_BYTES 14U
#define LLC_HEADER_BYTES 8U
#define AWDL_DATA_HEADER_BYTES 8U
#define IPV6_HEADER_BYTES 40U
#define ICMPV6_NS_BYTES 32U
#define ICMPV6_ECHO_BYTES 16U
#define ETHERTYPE_IPV6 0x86ddU

static const uint8_t awdl_bssid[6] = {
    0x00, 0x25, 0x00, 0xff, 0x94, 0x73,
};
static const uint8_t llc_header[LLC_HEADER_BYTES] = {
    0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00,
};

static uint16_t read_be16(const uint8_t *value)
{
    return (uint16_t)((uint16_t)value[0] << 8U) | value[1];
}

static uint32_t read_be32(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24U) |
           ((uint32_t)value[1] << 16U) |
           ((uint32_t)value[2] << 8U) |
           value[3];
}

static uint16_t read_le16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8U);
}

static void put_be16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void put_le16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t length)
{
    while (length >= 2U) {
        sum += read_be16(data);
        data += 2;
        length -= 2U;
    }
    if (length != 0U) {
        sum += (uint16_t)data[0] << 8U;
    }
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xffffU) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

static uint16_t icmpv6_checksum(
    const uint8_t source[16],
    const uint8_t destination[16],
    const uint8_t *icmp,
    uint16_t length)
{
    uint32_t sum = 0U;
    sum = checksum_add(sum, source, 16U);
    sum = checksum_add(sum, destination, 16U);
    const uint8_t pseudo_tail[8] = {
        0U, 0U, (uint8_t)(length >> 8U), (uint8_t)length,
        0U, 0U, 0U, 58U,
    };
    sum = checksum_add(sum, pseudo_tail, sizeof(pseudo_tail));
    sum = checksum_add(sum, icmp, length);
    return checksum_finish(sum);
}

void espdrop_awdl_link_local_from_mac(
    const uint8_t mac[6],
    uint8_t address[16])
{
    if (mac == NULL || address == NULL) {
        return;
    }
    memset(address, 0, 16U);
    address[0] = 0xfeU;
    address[1] = 0x80U;
    address[8] = mac[0] ^ 0x02U;
    address[9] = mac[1];
    address[10] = mac[2];
    address[11] = 0xffU;
    address[12] = 0xfeU;
    address[13] = mac[3];
    address[14] = mac[4];
    address[15] = mac[5];
}

bool espdrop_awdl_build_neighbor_solicitation(
    uint8_t *frame,
    size_t capacity,
    size_t *length,
    const uint8_t self_mac[6],
    const uint8_t target_mac[6],
    uint16_t ieee80211_sequence,
    uint16_t awdl_sequence)
{
    if (frame == NULL || length == NULL || self_mac == NULL ||
        target_mac == NULL || capacity < ESPDROP_AWDL_NS_FRAME_BYTES) {
        return false;
    }

    memset(frame, 0, ESPDROP_AWDL_NS_FRAME_BYTES);
    frame[0] = 0x08U;
    memcpy(frame + 4, target_mac, 6U);
    memcpy(frame + 10, self_mac, 6U);
    memcpy(frame + 16, awdl_bssid, sizeof(awdl_bssid));
    put_le16(frame + 22,
             (uint16_t)((ieee80211_sequence & 0x0fffU) << 4U));

    uint8_t *cursor = frame + IEEE80211_HEADER_BYTES;
    memcpy(cursor, llc_header, sizeof(llc_header));
    cursor += LLC_HEADER_BYTES;
    cursor[0] = 0x03U;
    cursor[1] = 0x04U;
    put_le16(cursor + 2, awdl_sequence);
    put_be16(cursor + 6, ETHERTYPE_IPV6);
    cursor += AWDL_DATA_HEADER_BYTES;

    uint8_t source_ip[16];
    uint8_t target_ip[16];
    espdrop_awdl_link_local_from_mac(self_mac, source_ip);
    espdrop_awdl_link_local_from_mac(target_mac, target_ip);
    cursor[0] = 0x60U;
    put_be16(cursor + 4, ICMPV6_NS_BYTES);
    cursor[6] = 58U;
    cursor[7] = 255U;
    memcpy(cursor + 8, source_ip, sizeof(source_ip));
    memcpy(cursor + 24, target_ip, sizeof(target_ip));
    cursor += IPV6_HEADER_BYTES;

    cursor[0] = 135U;
    cursor[1] = 0U;
    memcpy(cursor + 8, target_ip, sizeof(target_ip));
    cursor[24] = 1U;
    cursor[25] = 1U;
    memcpy(cursor + 26, self_mac, 6U);
    put_be16(cursor + 2,
             icmpv6_checksum(source_ip, target_ip, cursor, ICMPV6_NS_BYTES));

    *length = ESPDROP_AWDL_NS_FRAME_BYTES;
    return true;
}

bool espdrop_awdl_build_echo_request(
    uint8_t *frame,
    size_t capacity,
    size_t *length,
    const uint8_t self_mac[6],
    const uint8_t target_mac[6],
    uint16_t ieee80211_sequence,
    uint16_t awdl_sequence,
    uint16_t identifier,
    uint16_t echo_sequence)
{
    if (frame == NULL || length == NULL || self_mac == NULL ||
        target_mac == NULL || capacity < ESPDROP_AWDL_ECHO_FRAME_BYTES) {
        return false;
    }

    memset(frame, 0, ESPDROP_AWDL_ECHO_FRAME_BYTES);
    frame[0] = 0x08U;
    memcpy(frame + 4, target_mac, 6U);
    memcpy(frame + 10, self_mac, 6U);
    memcpy(frame + 16, awdl_bssid, sizeof(awdl_bssid));
    put_le16(frame + 22,
             (uint16_t)((ieee80211_sequence & 0x0fffU) << 4U));

    uint8_t *cursor = frame + IEEE80211_HEADER_BYTES;
    memcpy(cursor, llc_header, sizeof(llc_header));
    cursor += LLC_HEADER_BYTES;
    cursor[0] = 0x03U;
    cursor[1] = 0x04U;
    put_le16(cursor + 2, awdl_sequence);
    put_be16(cursor + 6, ETHERTYPE_IPV6);
    cursor += AWDL_DATA_HEADER_BYTES;

    uint8_t source_ip[16];
    uint8_t target_ip[16];
    espdrop_awdl_link_local_from_mac(self_mac, source_ip);
    espdrop_awdl_link_local_from_mac(target_mac, target_ip);
    cursor[0] = 0x60U;
    put_be16(cursor + 4, ICMPV6_ECHO_BYTES);
    cursor[6] = 58U;
    cursor[7] = 64U;
    memcpy(cursor + 8, source_ip, sizeof(source_ip));
    memcpy(cursor + 24, target_ip, sizeof(target_ip));
    cursor += IPV6_HEADER_BYTES;

    cursor[0] = 128U;
    cursor[1] = 0U;
    put_be16(cursor + 4, identifier);
    put_be16(cursor + 6, echo_sequence);
    memcpy(cursor + 8, "espDrop!", 8U);
    put_be16(cursor + 2,
             icmpv6_checksum(source_ip, target_ip, cursor,
                             ICMPV6_ECHO_BYTES));

    *length = ESPDROP_AWDL_ECHO_FRAME_BYTES;
    return true;
}

bool espdrop_awdl_build_ethernet_frame(
    uint8_t *frame,
    size_t capacity,
    size_t *length,
    const uint8_t *ethernet,
    size_t ethernet_length,
    const uint8_t self_mac[6],
    uint16_t ieee80211_sequence,
    uint16_t awdl_sequence)
{
    if (frame == NULL || length == NULL || ethernet == NULL ||
        self_mac == NULL || ethernet_length < ESPDROP_ETHERNET_HEADER_BYTES) {
        return false;
    }
    const size_t payload_length =
        ethernet_length - ESPDROP_ETHERNET_HEADER_BYTES;
    const size_t frame_length = ESPDROP_AWDL_DATA_FRAME_OVERHEAD +
                                payload_length;
    if (capacity < frame_length || frame_length > UINT16_MAX) {
        return false;
    }

    memset(frame, 0, frame_length);
    frame[0] = 0x08U;
    memcpy(frame + 4, ethernet, 6U);
    memcpy(frame + 10, self_mac, 6U);
    memcpy(frame + 16, awdl_bssid, sizeof(awdl_bssid));
    put_le16(frame + 22,
             (uint16_t)((ieee80211_sequence & 0x0fffU) << 4U));

    uint8_t *cursor = frame + IEEE80211_HEADER_BYTES;
    memcpy(cursor, llc_header, sizeof(llc_header));
    cursor += LLC_HEADER_BYTES;
    cursor[0] = 0x03U;
    cursor[1] = 0x04U;
    put_le16(cursor + 2, awdl_sequence);
    memcpy(cursor + 6, ethernet + 12, 2U);
    cursor += AWDL_DATA_HEADER_BYTES;
    memcpy(cursor, ethernet + ESPDROP_ETHERNET_HEADER_BYTES, payload_length);

    *length = frame_length;
    return true;
}

static espdrop_awdl_data_decode_result_t decode_msdu(
    const uint8_t *msdu,
    size_t length,
    espdrop_awdl_data_t *data)
{
    if (length < LLC_HEADER_BYTES + AWDL_DATA_HEADER_BYTES) {
        return ESPDROP_AWDL_DATA_DECODE_TOO_SHORT;
    }
    if (memcmp(msdu, llc_header, sizeof(llc_header)) != 0) {
        return ESPDROP_AWDL_DATA_DECODE_LLC;
    }
    const uint8_t *awdl = msdu + LLC_HEADER_BYTES;
    if (awdl[0] != 0x03U || awdl[1] != 0x04U ||
        awdl[4] != 0U || awdl[5] != 0U) {
        return ESPDROP_AWDL_DATA_DECODE_HEADER;
    }
    data->sequence = read_le16(awdl + 2);
    data->ethertype = read_be16(awdl + 6);
    data->payload = awdl + AWDL_DATA_HEADER_BYTES;
    data->payload_length = length - LLC_HEADER_BYTES - AWDL_DATA_HEADER_BYTES;
    return ESPDROP_AWDL_DATA_DECODE_OK;
}

espdrop_awdl_data_decode_result_t espdrop_awdl_decode_data_ex(
    const uint8_t *frame,
    size_t length,
    espdrop_awdl_data_t *data)
{
    if (frame == NULL || data == NULL) {
        return ESPDROP_AWDL_DATA_DECODE_INVALID_ARGUMENT;
    }
    if (length < IEEE80211_HEADER_BYTES) {
        return ESPDROP_AWDL_DATA_DECODE_TOO_SHORT;
    }
    if ((frame[0] & 0x0cU) != 0x08U) {
        return ESPDROP_AWDL_DATA_DECODE_NOT_DATA;
    }
    if ((frame[1] & 0x03U) != 0U) {
        return ESPDROP_AWDL_DATA_DECODE_DISTRIBUTION_SYSTEM;
    }
    if (memcmp(frame + 16, awdl_bssid, sizeof(awdl_bssid)) != 0) {
        return ESPDROP_AWDL_DATA_DECODE_BSSID;
    }
    const uint8_t subtype = frame[0] & 0xf0U;
    if (subtype != 0x00U && subtype != 0x80U) {
        return ESPDROP_AWDL_DATA_DECODE_SUBTYPE;
    }

    memset(data, 0, sizeof(*data));
    data->qos = subtype == 0x80U;
    size_t offset = IEEE80211_HEADER_BYTES;
    if (data->qos) {
        if (length < offset + IEEE80211_QOS_BYTES) {
            return ESPDROP_AWDL_DATA_DECODE_QOS_TOO_SHORT;
        }
        data->amsdu = (frame[offset] & 0x80U) != 0U;
        offset += IEEE80211_QOS_BYTES;
    }

    if (data->amsdu) {
        if (length < offset + AMSDU_SUBFRAME_HEADER_BYTES) {
            return ESPDROP_AWDL_DATA_DECODE_AMSDU_TOO_SHORT;
        }
        const uint16_t msdu_length = read_be16(frame + offset + 12);
        if ((size_t)msdu_length >
            length - offset - AMSDU_SUBFRAME_HEADER_BYTES) {
            return ESPDROP_AWDL_DATA_DECODE_AMSDU_LENGTH;
        }
        memcpy(data->destination, frame + offset, 6U);
        memcpy(data->source, frame + offset + 6, 6U);
        return decode_msdu(frame + offset + AMSDU_SUBFRAME_HEADER_BYTES,
                           msdu_length, data);
    }

    memcpy(data->destination, frame + 4, 6U);
    memcpy(data->source, frame + 10, 6U);
    return decode_msdu(frame + offset, length - offset, data);
}

bool espdrop_awdl_decode_data(
    const uint8_t *frame,
    size_t length,
    espdrop_awdl_data_t *data)
{
    return espdrop_awdl_decode_data_ex(frame, length, data) ==
           ESPDROP_AWDL_DATA_DECODE_OK;
}

size_t espdrop_awdl_decode_data_frames(
    const uint8_t *frame,
    size_t length,
    espdrop_awdl_data_t *data,
    size_t capacity,
    espdrop_awdl_data_decode_result_t *result)
{
    if (result == NULL) {
        return 0U;
    }
    *result = ESPDROP_AWDL_DATA_DECODE_INVALID_ARGUMENT;
    if (frame == NULL || data == NULL || capacity == 0U) {
        return 0U;
    }

    espdrop_awdl_data_t first;
    *result = espdrop_awdl_decode_data_ex(frame, length, &first);
    if (*result != ESPDROP_AWDL_DATA_DECODE_OK) {
        return 0U;
    }
    if (!first.amsdu) {
        data[0] = first;
        return 1U;
    }

    size_t cursor = IEEE80211_HEADER_BYTES + IEEE80211_QOS_BYTES;
    size_t count = 0U;
    while (cursor < length) {
        if (length - cursor < AMSDU_SUBFRAME_HEADER_BYTES) {
            *result = ESPDROP_AWDL_DATA_DECODE_AMSDU_TOO_SHORT;
            return 0U;
        }
        if (count >= capacity) {
            *result = ESPDROP_AWDL_DATA_DECODE_AMSDU_CAPACITY;
            return 0U;
        }
        const uint16_t msdu_length = read_be16(frame + cursor + 12U);
        if ((size_t)msdu_length >
            length - cursor - AMSDU_SUBFRAME_HEADER_BYTES) {
            *result = ESPDROP_AWDL_DATA_DECODE_AMSDU_LENGTH;
            return 0U;
        }

        espdrop_awdl_data_t *item = &data[count];
        memset(item, 0, sizeof(*item));
        item->qos = true;
        item->amsdu = true;
        memcpy(item->destination, frame + cursor, 6U);
        memcpy(item->source, frame + cursor + 6U, 6U);
        *result = decode_msdu(
            frame + cursor + AMSDU_SUBFRAME_HEADER_BYTES,
            msdu_length, item);
        if (*result != ESPDROP_AWDL_DATA_DECODE_OK) {
            return 0U;
        }
        ++count;
        cursor += AMSDU_SUBFRAME_HEADER_BYTES + msdu_length;
        if (cursor < length) {
            const size_t padding =
                (4U - ((AMSDU_SUBFRAME_HEADER_BYTES + msdu_length) % 4U)) %
                4U;
            if (padding > length - cursor) {
                *result = ESPDROP_AWDL_DATA_DECODE_AMSDU_TOO_SHORT;
                return 0U;
            }
            cursor += padding;
        }
    }
    *result = ESPDROP_AWDL_DATA_DECODE_OK;
    return count;
}

bool espdrop_awdl_decode_ipv6(
    const espdrop_awdl_data_t *data,
    espdrop_awdl_ipv6_t *ipv6)
{
    if (data == NULL || ipv6 == NULL || data->ethertype != ETHERTYPE_IPV6 ||
        data->payload == NULL || data->payload_length < IPV6_HEADER_BYTES ||
        (data->payload[0] >> 4U) != 6U) {
        return false;
    }
    const uint16_t payload_length = read_be16(data->payload + 4);
    if ((size_t)payload_length > data->payload_length - IPV6_HEADER_BYTES) {
        return false;
    }

    memset(ipv6, 0, sizeof(*ipv6));
    ipv6->next_header = data->payload[6];
    ipv6->hop_limit = data->payload[7];
    memcpy(ipv6->source, data->payload + 8, 16U);
    memcpy(ipv6->destination, data->payload + 24, 16U);
    if (ipv6->next_header == 58U && payload_length >= 4U) {
        const uint8_t *icmp = data->payload + IPV6_HEADER_BYTES;
        ipv6->icmp_type = icmp[0];
        ipv6->icmp_code = icmp[1];
        ipv6->icmp_payload = icmp + 4;
        ipv6->icmp_payload_length = payload_length - 4U;
    }
    return true;
}

bool espdrop_awdl_decode_tcp(
    const espdrop_awdl_data_t *data,
    espdrop_awdl_tcp_t *tcp)
{
    espdrop_awdl_ipv6_t ipv6;
    if (tcp == NULL || !espdrop_awdl_decode_ipv6(data, &ipv6) ||
        ipv6.next_header != 6U) {
        return false;
    }
    const uint16_t ipv6_payload_length = read_be16(data->payload + 4U);
    if (ipv6_payload_length < 20U) {
        return false;
    }
    const uint8_t *segment = data->payload + IPV6_HEADER_BYTES;
    const uint8_t header_length = (uint8_t)((segment[12] >> 4U) * 4U);
    if (header_length < 20U || header_length > ipv6_payload_length) {
        return false;
    }

    memset(tcp, 0, sizeof(*tcp));
    tcp->source_port = read_be16(segment);
    tcp->destination_port = read_be16(segment + 2U);
    tcp->sequence = read_be32(segment + 4U);
    tcp->acknowledgment = read_be32(segment + 8U);
    tcp->header_length = header_length;
    tcp->flags = segment[13];
    tcp->window = read_be16(segment + 14U);
    tcp->payload_length = (uint16_t)(ipv6_payload_length - header_length);
    uint32_t checksum = 0U;
    checksum = checksum_add(checksum, ipv6.source, sizeof(ipv6.source));
    checksum = checksum_add(checksum, ipv6.destination,
                            sizeof(ipv6.destination));
    const uint8_t pseudo_tail[8] = {
        (uint8_t)((uint32_t)ipv6_payload_length >> 24U),
        (uint8_t)((uint32_t)ipv6_payload_length >> 16U),
        (uint8_t)((uint32_t)ipv6_payload_length >> 8U),
        (uint8_t)ipv6_payload_length,
        0U, 0U, 0U, 6U,
    };
    checksum = checksum_add(checksum, pseudo_tail, sizeof(pseudo_tail));
    checksum = checksum_add(checksum, segment, ipv6_payload_length);
    tcp->checksum_valid = checksum_finish(checksum) == 0U;
    return true;
}

bool espdrop_awdl_data_to_ethernet(
    const espdrop_awdl_data_t *data,
    uint8_t *ethernet,
    size_t capacity,
    size_t *length)
{
    if (data == NULL || ethernet == NULL || length == NULL ||
        data->payload == NULL ||
        data->payload_length > SIZE_MAX - ESPDROP_ETHERNET_HEADER_BYTES) {
        return false;
    }
    const size_t ethernet_length =
        ESPDROP_ETHERNET_HEADER_BYTES + data->payload_length;
    if (capacity < ethernet_length) {
        return false;
    }
    memcpy(ethernet, data->destination, 6U);
    memcpy(ethernet + 6, data->source, 6U);
    put_be16(ethernet + 12, data->ethertype);
    memcpy(ethernet + ESPDROP_ETHERNET_HEADER_BYTES, data->payload,
           data->payload_length);
    *length = ethernet_length;
    return true;
}
