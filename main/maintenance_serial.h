#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t maintenance_serial_start(bool provisioning_mode);
void maintenance_serial_set_application_ready(bool ready);
