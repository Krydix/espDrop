#pragma once

#include <stddef.h>
#include <stdint.h>

#include "espdrop/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t session_window_ms;
    uint32_t pre_tap_grace_ms;
    int minimum_score;
    int ambiguity_margin;
} tapdrop_correlation_policy_t;

typedef enum {
    TAPDROP_CORRELATION_NONE = 0,
    TAPDROP_CORRELATION_MATCH,
    TAPDROP_CORRELATION_AMBIGUOUS,
    TAPDROP_CORRELATION_EXPIRED,
} tapdrop_correlation_status_t;

typedef struct {
    tapdrop_correlation_status_t status;
    const espdrop_peer_t *peer;
    int best_score;
    int runner_up_score;
    size_t candidates;
} tapdrop_correlation_result_t;

tapdrop_correlation_result_t tapdrop_correlate(
    const espdrop_peer_t *peers,
    size_t peer_count,
    uint64_t tap_ms,
    uint64_t now_ms,
    const tapdrop_correlation_policy_t *policy);

#ifdef __cplusplus
}
#endif
