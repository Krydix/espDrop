#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espdrop/awdl_tlv.h"
#include "espdrop/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t record_count;
    uint16_t malformed_record_count;
    uint16_t ptr_count;
    uint16_t txt_count;
    uint16_t srv_count;
    bool has_airdrop;
    bool has_airdrop_tcp;
    bool has_airdrop_udp;
    bool has_asquic;
    bool has_airdrop_endpoint;
    uint16_t airdrop_port;
    char airdrop_service_id[ESPDROP_SERVICE_ID_MAX_BYTES];
} espdrop_awdl_service_profile_t;

espdrop_awdl_parse_result_t espdrop_awdl_scan_service_responses(
    const uint8_t *tlv_data,
    size_t tlv_length,
    espdrop_awdl_service_profile_t *profile);

#ifdef __cplusplus
}
#endif
