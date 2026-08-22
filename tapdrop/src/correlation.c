#include "tapdrop/correlation.h"

#include <limits.h>
#include <stdbool.h>

static int clamp(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static bool within_window(
    uint64_t timestamp_ms,
    uint64_t tap_ms,
    uint32_t before_ms,
    uint32_t after_ms)
{
    if (timestamp_ms >= tap_ms) {
        return timestamp_ms - tap_ms <= after_ms;
    }
    return tap_ms - timestamp_ms <= before_ms;
}

static int recency_score(uint64_t last_seen_ms, uint64_t tap_ms, uint64_t now_ms)
{
    uint64_t reference_ms = last_seen_ms > tap_ms ? last_seen_ms : tap_ms;
    if (reference_ms > now_ms) {
        return 0;
    }
    uint64_t age_ms = now_ms - reference_ms;
    if (age_ms >= 5000U) {
        return 0;
    }
    return 200 - (int)((age_ms * 200U) / 5000U);
}

static int rssi_score(int16_t rssi, int maximum)
{
    if (rssi <= -100) {
        return 0;
    }
    int normalized = clamp((int)rssi, -100, -20) + 100;
    return (normalized * maximum) / 80;
}

static int score_peer(
    const espdrop_peer_t *peer,
    uint64_t tap_ms,
    uint64_t now_ms,
    const tapdrop_correlation_policy_t *policy)
{
    if ((peer->signals & ESPDROP_PEER_SIGNAL_AIRDROP) == 0U ||
        !within_window(peer->last_seen_ms, tap_ms, policy->pre_tap_grace_ms,
                       policy->session_window_ms)) {
        return INT_MIN;
    }

    int score = 0;
    if (peer->first_seen_ms >= tap_ms &&
        peer->first_seen_ms - tap_ms <= policy->session_window_ms) {
        score += 320;
    } else if (within_window(peer->first_seen_ms, tap_ms,
                             policy->pre_tap_grace_ms, 0)) {
        score += 80;
    }

    score += recency_score(peer->last_seen_ms, tap_ms, now_ms);

    const bool ble = (peer->signals & ESPDROP_PEER_SIGNAL_BLE) != 0U;
    const bool awdl = (peer->signals & ESPDROP_PEER_SIGNAL_AWDL) != 0U;
    if (ble && awdl) {
        score += 180;
    } else if (awdl) {
        score += 100;
    } else if (ble) {
        score += 80;
    }

    if (ble && within_window(peer->ble_seen_ms, tap_ms,
                             policy->pre_tap_grace_ms,
                             policy->session_window_ms)) {
        score += rssi_score(peer->ble_rssi, 120);
    }
    if (awdl && within_window(peer->awdl_seen_ms, tap_ms,
                              policy->pre_tap_grace_ms,
                              policy->session_window_ms)) {
        score += rssi_score(peer->awdl_rssi, 80);
    }
    return score;
}

tapdrop_correlation_result_t tapdrop_correlate(
    const espdrop_peer_t *peers,
    size_t peer_count,
    uint64_t tap_ms,
    uint64_t now_ms,
    const tapdrop_correlation_policy_t *policy)
{
    tapdrop_correlation_result_t result = {
        .status = TAPDROP_CORRELATION_NONE,
        .best_score = INT_MIN,
        .runner_up_score = INT_MIN,
    };

    if (policy == NULL || tap_ms == 0 ||
        (peers == NULL && peer_count != 0)) {
        return result;
    }
    if (now_ms >= tap_ms &&
        now_ms - tap_ms > policy->session_window_ms) {
        result.status = TAPDROP_CORRELATION_EXPIRED;
        return result;
    }

    for (size_t index = 0; index < peer_count; ++index) {
        const int score = score_peer(&peers[index], tap_ms, now_ms, policy);
        if (score == INT_MIN) {
            continue;
        }
        ++result.candidates;
        if (score > result.best_score) {
            result.runner_up_score = result.best_score;
            result.best_score = score;
            result.peer = &peers[index];
        } else if (score > result.runner_up_score) {
            result.runner_up_score = score;
        }
    }

    if (result.peer == NULL || result.best_score < policy->minimum_score) {
        result.status = TAPDROP_CORRELATION_NONE;
        return result;
    }
    if (result.runner_up_score != INT_MIN &&
        result.best_score - result.runner_up_score < policy->ambiguity_margin) {
        result.status = TAPDROP_CORRELATION_AMBIGUOUS;
        result.peer = NULL;
        return result;
    }
    result.status = TAPDROP_CORRELATION_MATCH;
    return result;
}
