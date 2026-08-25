#include "espdrop/ble_wake.h"

#include <string.h>

size_t espdrop_ble_airdrop_manufacturer_data(
    uint8_t *output,
    size_t capacity,
    const espdrop_ble_airdrop_hashes_t *hashes)
{
    if (output == NULL || capacity < ESPDROP_BLE_AIRDROP_MANUFACTURER_BYTES) {
        return 0U;
    }

    /* Apple company ID 0x004c, Continuity AirDrop type 0x05, body length 18.
     * The eight-byte zero prefix and version byte match the published
     * Continuity record used by the proven Everyone-mode wake profile. */
    static const uint8_t prefix[] = {
        0x4c, 0x00, 0x05, 0x12,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01,
    };
    memcpy(output, prefix, sizeof(prefix));

    uint8_t *short_hashes = output + sizeof(prefix);
    if (hashes == NULL) {
        memset(short_hashes, 0, 8U);
    } else {
        memcpy(short_hashes, hashes->apple_id, 2U);
        memcpy(short_hashes + 2U, hashes->phone, 2U);
        memcpy(short_hashes + 4U, hashes->email, 2U);
        memcpy(short_hashes + 6U, hashes->email2, 2U);
    }
    output[ESPDROP_BLE_AIRDROP_MANUFACTURER_BYTES - 1U] = 0x00;
    return ESPDROP_BLE_AIRDROP_MANUFACTURER_BYTES;
}
