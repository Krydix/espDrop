#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/awdl_frame.h"

static void make_action(uint8_t frame[40], uint8_t version, uint8_t subtype)
{
    memset(frame, 0, 40);
    frame[0] = 0xd0;
    frame[1] = 0x00;
    const uint8_t source[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    const uint8_t bssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
    memcpy(frame + 10, source, sizeof(source));
    memcpy(frame + 16, bssid, sizeof(bssid));
    frame[24] = 127;
    frame[25] = 0x00;
    frame[26] = 0x17;
    frame[27] = 0xf2;
    frame[28] = 8;
    frame[29] = version;
    frame[30] = subtype;
}

int main(void)
{
    uint8_t frame[40];
    espdrop_awdl_action_t action;
    make_action(frame, 0x10, ESPDROP_AWDL_ACTION_MIF);
    assert(espdrop_awdl_decode_action(frame, sizeof(frame), &action));
    assert(action.version == 0x10);
    assert(action.subtype == ESPDROP_AWDL_ACTION_MIF);
    assert(action.source[0] == 0x02);
    assert(action.source[5] == 0xee);

    frame[1] = 0x08;
    assert(espdrop_awdl_decode_action(frame, sizeof(frame), &action));

    make_action(frame, 0x10, ESPDROP_AWDL_ACTION_PSF);
    assert(espdrop_awdl_decode_action(frame, sizeof(frame), &action));
    assert(action.version == 0x10);
    assert(action.subtype == ESPDROP_AWDL_ACTION_PSF);

    frame[27] ^= 1;
    assert(!espdrop_awdl_decode_action(frame, sizeof(frame), &action));
    make_action(frame, 0x10, ESPDROP_AWDL_ACTION_MIF);
    frame[16] ^= 1;
    assert(!espdrop_awdl_decode_action(frame, sizeof(frame), &action));
    make_action(frame, 0x01, ESPDROP_AWDL_ACTION_MIF);
    assert(!espdrop_awdl_decode_action(frame, sizeof(frame), &action));
    assert(!espdrop_awdl_decode_action(frame, 39, &action));
    assert(!espdrop_awdl_decode_action(NULL, 40, &action));

    puts("AWDL frame tests passed");
    return 0;
}
