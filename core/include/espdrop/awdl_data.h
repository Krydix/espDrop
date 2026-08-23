#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_AWDL_NS_FRAME_BYTES 112U
#define ESPDROP_AWDL_ECHO_FRAME_BYTES 96U
#define ESPDROP_AWDL_DATA_FRAME_OVERHEAD 40U
#define ESPDROP_ETHERNET_HEADER_BYTES 14U

typedef struct {
    uint8_t destination[6];
    uint8_t source[6];
    uint16_t sequence;
    uint16_t ethertype;
    bool qos;
    bool amsdu;
    const uint8_t *payload;
    size_t payload_length;
} espdrop_awdl_data_t;

typedef struct {
    uint8_t source[16];
    uint8_t destination[16];
    uint8_t next_header;
    uint8_t hop_limit;
    uint8_t icmp_type;
    uint8_t icmp_code;
    const uint8_t *icmp_payload;
    size_t icmp_payload_length;
} espdrop_awdl_ipv6_t;

typedef enum {
    ESPDROP_AWDL_DATA_DECODE_OK = 0,
    ESPDROP_AWDL_DATA_DECODE_INVALID_ARGUMENT,
    ESPDROP_AWDL_DATA_DECODE_TOO_SHORT,
    ESPDROP_AWDL_DATA_DECODE_NOT_DATA,
    ESPDROP_AWDL_DATA_DECODE_DISTRIBUTION_SYSTEM,
    ESPDROP_AWDL_DATA_DECODE_BSSID,
    ESPDROP_AWDL_DATA_DECODE_SUBTYPE,
    ESPDROP_AWDL_DATA_DECODE_QOS_TOO_SHORT,
    ESPDROP_AWDL_DATA_DECODE_AMSDU_TOO_SHORT,
    ESPDROP_AWDL_DATA_DECODE_AMSDU_LENGTH,
    ESPDROP_AWDL_DATA_DECODE_LLC,
    ESPDROP_AWDL_DATA_DECODE_HEADER,
} espdrop_awdl_data_decode_result_t;

void espdrop_awdl_link_local_from_mac(
    const uint8_t mac[6],
    uint8_t address[16]);

bool espdrop_awdl_build_neighbor_solicitation(
    uint8_t *frame,
    size_t capacity,
    size_t *length,
    const uint8_t self_mac[6],
    const uint8_t target_mac[6],
    uint16_t ieee80211_sequence,
    uint16_t awdl_sequence);

bool espdrop_awdl_build_echo_request(
    uint8_t *frame,
    size_t capacity,
    size_t *length,
    const uint8_t self_mac[6],
    const uint8_t target_mac[6],
    uint16_t ieee80211_sequence,
    uint16_t awdl_sequence,
    uint16_t identifier,
    uint16_t echo_sequence);

bool espdrop_awdl_build_ethernet_frame(
    uint8_t *frame,
    size_t capacity,
    size_t *length,
    const uint8_t *ethernet,
    size_t ethernet_length,
    const uint8_t self_mac[6],
    uint16_t ieee80211_sequence,
    uint16_t awdl_sequence);

bool espdrop_awdl_decode_data(
    const uint8_t *frame,
    size_t length,
    espdrop_awdl_data_t *data);

espdrop_awdl_data_decode_result_t espdrop_awdl_decode_data_ex(
    const uint8_t *frame,
    size_t length,
    espdrop_awdl_data_t *data);

bool espdrop_awdl_decode_ipv6(
    const espdrop_awdl_data_t *data,
    espdrop_awdl_ipv6_t *ipv6);

bool espdrop_awdl_data_to_ethernet(
    const espdrop_awdl_data_t *data,
    uint8_t *ethernet,
    size_t capacity,
    size_t *length);

#ifdef __cplusplus
}
#endif
