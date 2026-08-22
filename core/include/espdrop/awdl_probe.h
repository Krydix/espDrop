#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t action_frames;
    uint32_t master_indication_frames;
    uint32_t periodic_sync_frames;
    uint32_t dropped_records;
} espdrop_awdl_probe_stats_t;

esp_err_t espdrop_awdl_probe_start(uint8_t channel);
espdrop_awdl_probe_stats_t espdrop_awdl_probe_stats(void);

#ifdef __cplusplus
}
#endif
