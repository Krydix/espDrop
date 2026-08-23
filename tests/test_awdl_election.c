#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/awdl_election.h"

static espdrop_awdl_election_v2_t peer_state(
    const uint8_t master[6],
    const uint8_t sync_master[6],
    uint32_t distance,
    uint32_t counter,
    uint32_t metric)
{
    espdrop_awdl_election_v2_t state = {
        .master_counter = counter,
        .distance_to_master = distance,
        .master_metric = metric,
        .self_metric = metric,
        .self_counter = counter,
    };
    memcpy(state.master, master, 6U);
    memcpy(state.sync_master, sync_master, 6U);
    return state;
}

int main(void)
{
    const uint8_t self[6] = {0x1c, 0xdb, 0xd4, 0x42, 0x3f, 0xa0};
    const uint8_t master[6] = {0x52, 0xf4, 0x36, 0xb8, 0xfd, 0xf5};
    const uint8_t iphone[6] = {0xa6, 0xed, 0x54, 0x02, 0x5b, 0x4e};
    const uint8_t lower_peer[6] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};

    espdrop_awdl_election_t election;
    assert(espdrop_awdl_election_init(&election, self));
    const espdrop_awdl_election_state_t *state =
        espdrop_awdl_election_state(&election);
    assert(memcmp(state->master, self, 6U) == 0);
    assert(memcmp(state->sync_master, self, 6U) == 0);
    assert(state->distance_to_master == 0U);
    assert(state->self_metric == ESPDROP_AWDL_ELECTION_METRIC_INITIAL);
    assert(!espdrop_awdl_election_set_peer_timeout(NULL, 100U));
    assert(!espdrop_awdl_election_set_peer_timeout(&election, 0U));

    bool changed = false;
    espdrop_awdl_election_v2_t iphone_state =
        peer_state(master, master, 1U, 741U, 510U);
    assert(espdrop_awdl_election_observe(
        &election, iphone, &iphone_state, 1000000ULL, &changed));
    assert(changed);
    state = espdrop_awdl_election_state(&election);
    assert(memcmp(state->master, master, 6U) == 0);
    assert(memcmp(state->sync_master, iphone, 6U) == 0);
    assert(state->distance_to_master == 2U);
    assert(state->master_counter == 741U);
    assert(state->master_metric == 510U);
    assert(state->self_counter == ESPDROP_AWDL_ELECTION_COUNTER_INITIAL);
    assert(state->self_metric == ESPDROP_AWDL_ELECTION_METRIC_INITIAL);
    assert(espdrop_awdl_election_sync_peer(&election) != NULL);
    assert(memcmp(espdrop_awdl_election_sync_peer(&election)->address,
                  iphone, 6U) == 0);

    espdrop_awdl_election_v2_t master_state =
        peer_state(master, master, 0U, 741U, 510U);
    assert(espdrop_awdl_election_observe(
        &election, master, &master_state, 1100000ULL, &changed));
    assert(changed);
    state = espdrop_awdl_election_state(&election);
    assert(memcmp(state->sync_master, master, 6U) == 0);
    assert(state->distance_to_master == 1U);

    espdrop_awdl_election_v2_t lower_state =
        peer_state(lower_peer, lower_peer, 0U, 740U, 999U);
    assert(espdrop_awdl_election_observe(
        &election, lower_peer, &lower_state, 1200000ULL, &changed));
    assert(!changed);
    assert(memcmp(espdrop_awdl_election_state(&election)->sync_master,
                  master, 6U) == 0);

    const uint8_t higher_tie_peer[6] = {
        0xf0, 0x11, 0x12, 0x13, 0x14, 0x15,
    };
    espdrop_awdl_election_v2_t tie_state =
        peer_state(master, master, 0U, 741U, 510U);
    assert(espdrop_awdl_election_observe(
        &election, higher_tie_peer, &tie_state, 1250000ULL, &changed));
    assert(changed);
    assert(memcmp(espdrop_awdl_election_state(&election)->sync_master,
                  higher_tie_peer, 6U) == 0);

    espdrop_awdl_election_v2_t cycle_state =
        peer_state(master, self, 0U, 900U, 900U);
    assert(espdrop_awdl_election_observe(
        &election, iphone, &cycle_state, 1300000ULL, &changed));
    assert(!changed);
    assert(memcmp(espdrop_awdl_election_state(&election)->sync_master,
                  higher_tie_peer, 6U) == 0);

    assert(espdrop_awdl_election_touch(
        &election, higher_tie_peer, 1300150ULL));
    assert(espdrop_awdl_election_set_peer_timeout(&election, 100U));
    assert(espdrop_awdl_election_expire(
               &election, 1300201ULL, &changed) == 3U);
    assert(!changed);
    assert(memcmp(espdrop_awdl_election_state(&election)->sync_master,
                  higher_tie_peer, 6U) == 0);
    assert(espdrop_awdl_election_expire(
               &election, 1300300ULL, &changed) == 1U);
    assert(changed);
    state = espdrop_awdl_election_state(&election);
    assert(memcmp(state->master, self, 6U) == 0);
    assert(memcmp(state->sync_master, self, 6U) == 0);
    assert(state->distance_to_master == 0U);
    assert(election.peer_count == 0U);

    puts("AWDL election tests passed");
    return 0;
}
