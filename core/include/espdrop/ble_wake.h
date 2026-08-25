#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_BLE_AIRDROP_MANUFACTURER_BYTES 22U

typedef struct {
    uint8_t apple_id[2];
    uint8_t phone[2];
    uint8_t email[2];
    uint8_t email2[2];
} espdrop_ble_airdrop_hashes_t;

/* Build the Apple manufacturer data carried by the legacy BLE advertisement.
 * Passing NULL hashes emits the Everyone-mode wake profile with four zero
 * short hashes. The returned bytes start with Apple's little-endian company
 * ID; the BLE stack supplies the surrounding manufacturer-data AD header. */
size_t espdrop_ble_airdrop_manufacturer_data(
    uint8_t *output,
    size_t capacity,
    const espdrop_ble_airdrop_hashes_t *hashes);

/* Start one non-connectable, bounded AirDrop wake advertisement. This is a
 * lab boundary until the current iPhone compatibility profile is captured. */
int espdrop_ble_wake_start(uint32_t duration_ms);
int espdrop_ble_wake_stop(void);
bool espdrop_ble_wake_active(void);

#ifdef __cplusplus
}
#endif
