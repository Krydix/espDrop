#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the bounded anonymous AirDrop receiver research endpoint. The current
 * slice serves only TLS POST /Discover; it never accepts or stores a file. */
esp_err_t espdrop_airdrop_receiver_oracle_start(uint16_t port);

#ifdef __cplusplus
}
#endif
