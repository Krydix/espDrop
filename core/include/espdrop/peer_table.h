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

bool espdrop_peer_id_equal(
    const espdrop_peer_id_t *left,
    const espdrop_peer_id_t *right);

#ifdef __cplusplus
}
#endif
