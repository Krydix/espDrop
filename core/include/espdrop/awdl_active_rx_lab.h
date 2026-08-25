#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Apply the ESP-IDF 5.4 station pre-authentication receive policy to the
 * AWDL BSSID while promiscuous capture remains enabled.
 *
 * This is a research-only probe of undocumented Wi-Fi blob entry points. It
 * must never be enabled in normal or web-installer firmware.
 */
esp_err_t espdrop_awdl_active_rx_lab_enable(void);

#ifdef __cplusplus
}
#endif
