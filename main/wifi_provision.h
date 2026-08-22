#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t wifi_provision_is_configured(bool *configured);
esp_err_t wifi_provision_start(void);
esp_err_t wifi_provision_connect(const char *ssid, const char *password);
esp_err_t wifi_provision_wait_connected(unsigned timeout_ms);
esp_err_t wifi_provision_mark_configured(void);
bool wifi_provision_connected(void);
void wifi_provision_ip(char *destination, size_t capacity);
