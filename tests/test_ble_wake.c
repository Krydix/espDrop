#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/ble_wake.h"

int main(void)
{
    static const uint8_t everyone_expected[] = {
        0x4c, 0x00, 0x05, 0x12,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00,
    };
    _Static_assert(sizeof(everyone_expected) ==
                       ESPDROP_BLE_AIRDROP_MANUFACTURER_BYTES,
                   "AirDrop manufacturer record length changed");

    uint8_t output[ESPDROP_BLE_AIRDROP_MANUFACTURER_BYTES];
    assert(espdrop_ble_airdrop_manufacturer_data(
               output, sizeof(output), NULL) == sizeof(output));
    assert(memcmp(output, everyone_expected, sizeof(output)) == 0);

    const espdrop_ble_airdrop_hashes_t hashes = {
        .apple_id = {0x11, 0x22},
        .phone = {0x33, 0x44},
        .email = {0x55, 0x66},
        .email2 = {0x77, 0x88},
    };
    assert(espdrop_ble_airdrop_manufacturer_data(
               output, sizeof(output), &hashes) == sizeof(output));
    assert(memcmp(output + 13U,
                  "\x11\x22\x33\x44\x55\x66\x77\x88", 8U) == 0);
    assert(output[21] == 0x00);
    assert(espdrop_ble_airdrop_manufacturer_data(
               output, sizeof(output) - 1U, NULL) == 0U);
    assert(espdrop_ble_airdrop_manufacturer_data(
               NULL, sizeof(output), NULL) == 0U);

    puts("AirDrop BLE wake payload tests passed");
    return 0;
}
