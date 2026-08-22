#include "espdrop/awdl_frame.h"

#include <stdbool.h>
#include <string.h>

#define IEEE80211_HEADER_BYTES 24U
#define AWDL_ACTION_HEADER_BYTES 16U
#define AWDL_ACTION_CATEGORY 127U
#define AWDL_ACTION_TYPE 8U
#define AWDL_ACTION_VERSION_1_0 0x10U

static const uint8_t apple_oui[3] = {0x00, 0x17, 0xf2};
static const uint8_t awdl_bssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};

static uint32_t read_le32(const uint8_t *value)
{
    return (uint32_t)value[0] |
           ((uint32_t)value[1] << 8U) |
           ((uint32_t)value[2] << 16U) |
           ((uint32_t)value[3] << 24U);
}

bool espdrop_awdl_decode_action(
    const uint8_t *frame,
    size_t length,
    espdrop_awdl_action_t *action)
{
    if (frame == NULL || action == NULL ||
        length < IEEE80211_HEADER_BYTES + AWDL_ACTION_HEADER_BYTES ||
        frame[0] != 0xd0 ||
        memcmp(frame + 16, awdl_bssid, sizeof(awdl_bssid)) != 0) {
        return false;
    }

    const uint8_t *body = frame + IEEE80211_HEADER_BYTES;
    if (body[0] != AWDL_ACTION_CATEGORY ||
        memcmp(body + 1, apple_oui, sizeof(apple_oui)) != 0 ||
        body[4] != AWDL_ACTION_TYPE ||
        body[5] != AWDL_ACTION_VERSION_1_0 ||
        (body[6] != ESPDROP_AWDL_ACTION_PSF &&
         body[6] != ESPDROP_AWDL_ACTION_MIF)) {
        return false;
    }

    memcpy(action->source, frame + 10, sizeof(action->source));
    action->version = body[5];
    action->subtype = (espdrop_awdl_action_subtype_t)body[6];
    action->phy_tx = read_le32(body + 8);
    action->target_tx = read_le32(body + 12);
    action->tlv_data = body + AWDL_ACTION_HEADER_BYTES;
    action->tlv_length = length - IEEE80211_HEADER_BYTES -
                         AWDL_ACTION_HEADER_BYTES;
    return true;
}
