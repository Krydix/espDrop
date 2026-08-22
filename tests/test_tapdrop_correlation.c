#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "tapdrop/correlation.h"

static espdrop_peer_t make_peer(
    uint8_t id,
    uint32_t signals,
    uint64_t first_seen,
    uint64_t last_seen,
    int16_t ble_rssi,
    int16_t awdl_rssi)
{
    espdrop_peer_t peer = {
        .id = {.bytes = {id}, .length = 1},
        .signals = signals,
        .ble_rssi = ble_rssi,
        .awdl_rssi = awdl_rssi,
        .first_seen_ms = first_seen,
        .last_seen_ms = last_seen,
        .ble_seen_ms = last_seen,
        .awdl_seen_ms = last_seen,
        .airdrop_seen_ms = last_seen,
    };
    return peer;
}

int main(void)
{
    const uint64_t tap_ms = 10000;
    const tapdrop_correlation_policy_t policy = {
        .session_window_ms = 15000,
        .pre_tap_grace_ms = 1500,
        .minimum_score = 450,
        .ambiguity_margin = 100,
    };

    espdrop_peer_t peers[] = {
        make_peer(1,
                  ESPDROP_PEER_SIGNAL_BLE | ESPDROP_PEER_SIGNAL_AWDL |
                      ESPDROP_PEER_SIGNAL_AIRDROP,
                  10100, 10200, -31, -35),
        make_peer(2,
                  ESPDROP_PEER_SIGNAL_BLE | ESPDROP_PEER_SIGNAL_AWDL |
                      ESPDROP_PEER_SIGNAL_AIRDROP,
                  1000, 10150, -72, -76),
    };
    tapdrop_correlation_result_t result =
        tapdrop_correlate(peers, 2, tap_ms, 10300, &policy);
    assert(result.status == TAPDROP_CORRELATION_MATCH);
    assert(result.peer == &peers[0]);
    assert(result.best_score - result.runner_up_score >=
           policy.ambiguity_margin);

    espdrop_peer_t twins[] = {
        make_peer(3,
                  ESPDROP_PEER_SIGNAL_BLE | ESPDROP_PEER_SIGNAL_AWDL |
                      ESPDROP_PEER_SIGNAL_AIRDROP,
                  10100, 10200, -40, -40),
        make_peer(4,
                  ESPDROP_PEER_SIGNAL_BLE | ESPDROP_PEER_SIGNAL_AWDL |
                      ESPDROP_PEER_SIGNAL_AIRDROP,
                  10100, 10200, -40, -40),
    };
    result = tapdrop_correlate(twins, 2, tap_ms, 10300, &policy);
    assert(result.status == TAPDROP_CORRELATION_AMBIGUOUS);
    assert(result.peer == NULL);

    espdrop_peer_t weak = make_peer(
        5, ESPDROP_PEER_SIGNAL_AIRDROP, 1000, 10100, -127, -127);
    result = tapdrop_correlate(&weak, 1, tap_ms, 10200, &policy);
    assert(result.status == TAPDROP_CORRELATION_NONE);

    result = tapdrop_correlate(peers, 2, tap_ms, 26000, &policy);
    assert(result.status == TAPDROP_CORRELATION_EXPIRED);

    result = tapdrop_correlate(NULL, 0, tap_ms, 10100, &policy);
    assert(result.status == TAPDROP_CORRELATION_NONE);
    assert(result.best_score == INT_MIN);

    puts("TapDrop correlation tests passed");
    return 0;
}
