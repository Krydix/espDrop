/*
 * espDrop AWDL election state
 *
 * Adapted from OWL src/election.c and src/election.h at commit
 * da255a70f221784c836d943dd3f243bc798f223b.
 * Copyright (C) 2018 The Open Wireless Link Project
 * Copyright (C) 2018 Milan Stute
 * Copyright (C) 2026 Krydix and espDrop contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "espdrop/awdl_election.h"

#include <string.h>

static bool mac_is_zero(const uint8_t address[6])
{
    static const uint8_t zero[6];
    return memcmp(address, zero, sizeof(zero)) == 0;
}

static bool state_equal(
    const espdrop_awdl_election_state_t *left,
    const espdrop_awdl_election_state_t *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

static void reset_to_self(espdrop_awdl_election_state_t *state)
{
    memcpy(state->master, state->self, sizeof(state->master));
    memcpy(state->sync_master, state->self, sizeof(state->sync_master));
    state->distance_to_master = 0U;
    state->master_metric = state->self_metric;
    state->master_counter = state->self_counter;
}

static int compare_u32(uint32_t left, uint32_t right)
{
    return left < right ? -1 : left > right ? 1 : 0;
}

static int compare_peer_to_candidate(
    const espdrop_awdl_election_peer_t *peer,
    const espdrop_awdl_election_state_t *state,
    const espdrop_awdl_election_peer_t *selected)
{
    const uint32_t candidate_counter = selected == NULL
                                           ? state->master_counter
                                           : selected->master_counter;
    const uint32_t candidate_metric = selected == NULL
                                          ? state->master_metric
                                          : selected->master_metric;
    const uint32_t candidate_distance = selected == NULL
                                            ? state->distance_to_master
                                            : selected->distance_to_master;
    const uint8_t *candidate_address = selected == NULL
                                           ? state->self : selected->address;
    int comparison = compare_u32(peer->master_counter, candidate_counter);
    if (comparison == 0) {
        comparison = compare_u32(peer->master_metric, candidate_metric);
    }
    if (comparison != 0) {
        return comparison;
    }
    if (peer->distance_to_master < candidate_distance) {
        return 1;
    }
    if (peer->distance_to_master > candidate_distance) {
        return -1;
    }
    return memcmp(peer->address, candidate_address, 6U);
}

static bool run_election(espdrop_awdl_election_t *election)
{
    const espdrop_awdl_election_state_t previous = election->state;
    reset_to_self(&election->state);
    const espdrop_awdl_election_peer_t *selected = NULL;

    for (size_t index = 0U; index < election->peer_count; ++index) {
        const espdrop_awdl_election_peer_t *peer = &election->peers[index];
        if (!peer->valid ||
            peer->distance_to_master >=
                ESPDROP_AWDL_ELECTION_TREE_MAX_HEIGHT ||
            memcmp(peer->sync_master, election->state.self,
                   sizeof(peer->sync_master)) == 0) {
            continue;
        }
        if (compare_peer_to_candidate(peer, &election->state,
                                      selected) <= 0) {
            continue;
        }
        selected = peer;
    }

    if (selected != NULL) {
        memcpy(election->state.master, selected->master,
               sizeof(election->state.master));
        memcpy(election->state.sync_master, selected->address,
               sizeof(election->state.sync_master));
        election->state.distance_to_master =
            selected->distance_to_master + 1U;
        election->state.master_metric = selected->master_metric;
        election->state.master_counter = selected->master_counter;
    }
    return !state_equal(&previous, &election->state);
}

bool espdrop_awdl_election_init(
    espdrop_awdl_election_t *election,
    const uint8_t self[6])
{
    if (election == NULL || self == NULL || mac_is_zero(self)) {
        return false;
    }
    memset(election, 0, sizeof(*election));
    memcpy(election->state.self, self, sizeof(election->state.self));
    election->state.self_metric = ESPDROP_AWDL_ELECTION_METRIC_INITIAL;
    election->state.self_counter = ESPDROP_AWDL_ELECTION_COUNTER_INITIAL;
    election->peer_timeout_us = ESPDROP_AWDL_ELECTION_PEER_TIMEOUT_US;
    reset_to_self(&election->state);
    return true;
}

bool espdrop_awdl_election_set_peer_timeout(
    espdrop_awdl_election_t *election,
    uint64_t timeout_us)
{
    if (election == NULL || timeout_us == 0U) {
        return false;
    }
    election->peer_timeout_us = timeout_us;
    return true;
}

static espdrop_awdl_election_peer_t *find_peer(
    espdrop_awdl_election_t *election,
    const uint8_t address[6])
{
    for (size_t index = 0U; index < election->peer_count; ++index) {
        if (memcmp(election->peers[index].address, address, 6U) == 0) {
            return &election->peers[index];
        }
    }
    return NULL;
}

bool espdrop_awdl_election_touch(
    espdrop_awdl_election_t *election,
    const uint8_t source[6],
    uint64_t observed_at_us)
{
    if (election == NULL || source == NULL) {
        return false;
    }
    espdrop_awdl_election_peer_t *peer = find_peer(election, source);
    if (peer == NULL || !peer->valid) {
        return false;
    }
    peer->last_seen_us = observed_at_us;
    return true;
}

size_t espdrop_awdl_election_expire(
    espdrop_awdl_election_t *election,
    uint64_t now_us,
    bool *changed)
{
    if (changed != NULL) {
        *changed = false;
    }
    if (election == NULL) {
        return 0U;
    }
    size_t write_index = 0U;
    const size_t previous_count = election->peer_count;
    for (size_t read_index = 0U; read_index < previous_count; ++read_index) {
        const espdrop_awdl_election_peer_t *peer =
            &election->peers[read_index];
        const bool clock_wrapped = peer->last_seen_us > now_us;
        const bool fresh = clock_wrapped ||
            now_us - peer->last_seen_us <= election->peer_timeout_us;
        if (!fresh) {
            continue;
        }
        if (write_index != read_index) {
            election->peers[write_index] = *peer;
        }
        ++write_index;
    }
    if (write_index < previous_count) {
        memset(&election->peers[write_index], 0,
               (previous_count - write_index) * sizeof(election->peers[0]));
    }
    election->peer_count = write_index;
    const bool state_changed = run_election(election);
    if (changed != NULL) {
        *changed = state_changed;
    }
    return previous_count - write_index;
}

bool espdrop_awdl_election_observe(
    espdrop_awdl_election_t *election,
    const uint8_t source[6],
    const espdrop_awdl_election_v2_t *peer_state,
    uint64_t observed_at_us,
    bool *changed)
{
    if (changed != NULL) {
        *changed = false;
    }
    if (election == NULL || source == NULL || peer_state == NULL ||
        mac_is_zero(source) || mac_is_zero(peer_state->master) ||
        mac_is_zero(peer_state->sync_master) ||
        memcmp(source, election->state.self, 6U) == 0) {
        return false;
    }

    bool expired_changed = false;
    (void)espdrop_awdl_election_expire(election, observed_at_us,
                                       &expired_changed);
    espdrop_awdl_election_peer_t *peer = find_peer(election, source);
    if (peer == NULL) {
        if (election->peer_count >= ESPDROP_AWDL_ELECTION_MAX_PEERS) {
            return false;
        }
        peer = &election->peers[election->peer_count++];
        memset(peer, 0, sizeof(*peer));
        memcpy(peer->address, source, sizeof(peer->address));
    }
    peer->valid = true;
    memcpy(peer->master, peer_state->master, sizeof(peer->master));
    memcpy(peer->sync_master, peer_state->sync_master,
           sizeof(peer->sync_master));
    peer->last_seen_us = observed_at_us;
    peer->distance_to_master = peer_state->distance_to_master;
    peer->master_metric = peer_state->master_metric;
    peer->self_metric = peer_state->self_metric;
    peer->master_counter = peer_state->master_counter;
    peer->self_counter = peer_state->self_counter;
    const bool observed_changed = run_election(election);
    if (changed != NULL) {
        *changed = expired_changed || observed_changed;
    }
    return true;
}

const espdrop_awdl_election_state_t *espdrop_awdl_election_state(
    const espdrop_awdl_election_t *election)
{
    return election == NULL ? NULL : &election->state;
}

const espdrop_awdl_election_peer_t *espdrop_awdl_election_sync_peer(
    const espdrop_awdl_election_t *election)
{
    if (election == NULL ||
        memcmp(election->state.sync_master, election->state.self, 6U) == 0) {
        return NULL;
    }
    for (size_t index = 0U; index < election->peer_count; ++index) {
        if (election->peers[index].valid &&
            memcmp(election->peers[index].address,
                   election->state.sync_master, 6U) == 0) {
            return &election->peers[index];
        }
    }
    return NULL;
}
