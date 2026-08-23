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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espdrop/awdl_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_AWDL_ELECTION_MAX_PEERS 16U
#define ESPDROP_AWDL_ELECTION_TREE_MAX_HEIGHT 10U
#define ESPDROP_AWDL_ELECTION_METRIC_INITIAL 60U
#define ESPDROP_AWDL_ELECTION_COUNTER_INITIAL 0U
#define ESPDROP_AWDL_ELECTION_PEER_TIMEOUT_US 2000000ULL

typedef struct {
    uint8_t self[6];
    uint8_t master[6];
    uint8_t sync_master[6];
    uint32_t distance_to_master;
    uint32_t master_metric;
    uint32_t self_metric;
    uint32_t master_counter;
    uint32_t self_counter;
} espdrop_awdl_election_state_t;

typedef struct {
    bool valid;
    uint8_t address[6];
    uint8_t master[6];
    uint8_t sync_master[6];
    uint64_t last_seen_us;
    uint32_t distance_to_master;
    uint32_t master_metric;
    uint32_t self_metric;
    uint32_t master_counter;
    uint32_t self_counter;
} espdrop_awdl_election_peer_t;

typedef struct {
    espdrop_awdl_election_state_t state;
    espdrop_awdl_election_peer_t peers[ESPDROP_AWDL_ELECTION_MAX_PEERS];
    size_t peer_count;
    uint64_t peer_timeout_us;
} espdrop_awdl_election_t;

bool espdrop_awdl_election_init(
    espdrop_awdl_election_t *election,
    const uint8_t self[6]);

bool espdrop_awdl_election_set_peer_timeout(
    espdrop_awdl_election_t *election,
    uint64_t timeout_us);

bool espdrop_awdl_election_observe(
    espdrop_awdl_election_t *election,
    const uint8_t source[6],
    const espdrop_awdl_election_v2_t *peer,
    uint64_t observed_at_us,
    bool *changed);

bool espdrop_awdl_election_touch(
    espdrop_awdl_election_t *election,
    const uint8_t source[6],
    uint64_t observed_at_us);

size_t espdrop_awdl_election_expire(
    espdrop_awdl_election_t *election,
    uint64_t now_us,
    bool *changed);

const espdrop_awdl_election_state_t *espdrop_awdl_election_state(
    const espdrop_awdl_election_t *election);

const espdrop_awdl_election_peer_t *espdrop_awdl_election_sync_peer(
    const espdrop_awdl_election_t *election);

#ifdef __cplusplus
}
#endif
