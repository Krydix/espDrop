#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "espdrop/peer_table.h"
#include "tapdrop/correlation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int field_gpio;
    int active_level;
    uint32_t session_window_ms;
    int minimum_score;
    int ambiguity_margin;
} tapdrop_config_t;

typedef struct {
    uint32_t generation;
    uint64_t tap_ms;
    uint64_t expires_ms;
    bool active;
} tapdrop_session_t;

esp_err_t tapdrop_init(const tapdrop_config_t *config);
esp_err_t tapdrop_signal_tap(uint64_t timestamp_ms);
tapdrop_session_t tapdrop_current_session(void);
tapdrop_correlation_result_t tapdrop_select_peer(
    const espdrop_peer_table_t *table,
    uint64_t now_ms);

#ifdef __cplusplus
}
#endif
