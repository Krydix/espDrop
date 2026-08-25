#pragma once

#include "espdrop/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPDROP_TABLE_OK = 0,
    ESPDROP_TABLE_INVALID_ARGUMENT = -1,
    ESPDROP_TABLE_FULL = -2,
    ESPDROP_TABLE_NOT_FOUND = -3,
    ESPDROP_TABLE_AMBIGUOUS = -4,
} espdrop_table_result_t;

typedef struct {
    espdrop_peer_t peers[ESPDROP_MAX_PEERS];
    size_t count;
} espdrop_peer_table_t;

void espdrop_peer_table_init(espdrop_peer_table_t *table);

espdrop_table_result_t espdrop_peer_table_observe(
    espdrop_peer_table_t *table,
    const espdrop_peer_observation_t *observation,
    espdrop_peer_t **peer);

size_t espdrop_peer_table_expire(
    espdrop_peer_table_t *table,
    uint64_t now_ms,
    uint64_t max_age_ms);

const espdrop_peer_t *espdrop_peer_table_find(
    const espdrop_peer_table_t *table,
    const espdrop_peer_id_t *id);

espdrop_table_result_t espdrop_peer_table_select_unique_airdrop(
    const espdrop_peer_table_t *table,
    uint64_t now_ms,
    uint64_t max_age_ms,
    const espdrop_peer_t **peer);

espdrop_table_result_t espdrop_peer_table_apply_airdrop_endpoint(
    espdrop_peer_table_t *table,
    const uint8_t ipv6[ESPDROP_IPV6_BYTES],
    uint16_t port,
    const char *service_id,
    uint64_t seen_ms,
    espdrop_peer_t **peer);

espdrop_table_result_t espdrop_peer_table_select_unique_airdrop_endpoint(
    const espdrop_peer_table_t *table,
    uint64_t now_ms,
    uint64_t max_age_ms,
    const espdrop_peer_t **peer);

/* Select a live AirDrop/AWDL peer only when proximity is decisive. The best
 * candidate must meet min_rssi and exceed the runner-up by min_margin_db.
 * require_complete_endpoint delays selection until DNS-SD supplied the port
 * and IPv6 tuple; false permits MIF-level selection needed to start AWDL. */
espdrop_table_result_t espdrop_peer_table_select_airdrop_proximity(
    const espdrop_peer_table_t *table,
    uint64_t now_ms,
    uint64_t max_age_ms,
    int16_t min_rssi,
    int16_t min_margin_db,
    bool require_complete_endpoint,
    const espdrop_peer_t **peer);

bool espdrop_peer_id_equal(
    const espdrop_peer_id_t *left,
    const espdrop_peer_id_t *right);

#ifdef __cplusplus
}
#endif
