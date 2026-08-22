#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPDROP_AWDL_ACTION_PSF = 0,
    ESPDROP_AWDL_ACTION_MIF = 3,
} espdrop_awdl_action_subtype_t;

typedef struct {
    uint8_t source[6];
    uint8_t version;
    espdrop_awdl_action_subtype_t subtype;
    uint32_t phy_tx;
    uint32_t target_tx;
    const uint8_t *tlv_data;
    size_t tlv_length;
} espdrop_awdl_action_t;

bool espdrop_awdl_decode_action(
    const uint8_t *frame,
    size_t length,
    espdrop_awdl_action_t *action);

#ifdef __cplusplus
}
#endif
