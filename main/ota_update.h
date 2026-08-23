#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define ESPDROP_OTA_URL_MAX 256U

const char *ota_update_github_url(void);
esp_err_t ota_update_init(void);
esp_err_t ota_update_is_pending(bool *pending);
esp_err_t ota_update_request_github(void);
esp_err_t ota_update_request_url(const char *url);
esp_err_t ota_update_apply_pending(void);
esp_err_t ota_update_confirm_running(void);
