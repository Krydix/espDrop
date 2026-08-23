#include "espdrop/peer_table.h"

#include <string.h>

static bool peer_id_valid(const espdrop_peer_id_t *id)
{
    return id != NULL && id->length > 0 &&
           id->length <= ESPDROP_PEER_ID_MAX_BYTES;
}

bool espdrop_peer_id_equal(
    const espdrop_peer_id_t *left,
    const espdrop_peer_id_t *right)
{
    return peer_id_valid(left) && peer_id_valid(right) &&
           left->length == right->length &&
           memcmp(left->bytes, right->bytes, left->length) == 0;
}

void espdrop_peer_table_init(espdrop_peer_table_t *table)
{
    if (table != NULL) {
        memset(table, 0, sizeof(*table));
    }
}

static espdrop_peer_t *find_mutable(
    espdrop_peer_table_t *table,
    const espdrop_peer_id_t *id)
{
    for (size_t index = 0; index < table->count; ++index) {
        if (espdrop_peer_id_equal(&table->peers[index].id, id)) {
            return &table->peers[index];
        }
    }
    return NULL;
}

const espdrop_peer_t *espdrop_peer_table_find(
    const espdrop_peer_table_t *table,
    const espdrop_peer_id_t *id)
{
    if (table == NULL || !peer_id_valid(id)) {
        return NULL;
    }
    for (size_t index = 0; index < table->count; ++index) {
        if (espdrop_peer_id_equal(&table->peers[index].id, id)) {
            return &table->peers[index];
        }
    }
    return NULL;
}

espdrop_table_result_t espdrop_peer_table_select_unique_airdrop(
    const espdrop_peer_table_t *table,
    uint64_t now_ms,
    uint64_t max_age_ms,
    const espdrop_peer_t **peer)
{
    if (table == NULL || peer == NULL) {
        return ESPDROP_TABLE_INVALID_ARGUMENT;
    }
    *peer = NULL;
    for (size_t index = 0U; index < table->count; ++index) {
        const espdrop_peer_t *candidate = &table->peers[index];
        if ((candidate->signals & ESPDROP_PEER_SIGNAL_AIRDROP) == 0U) {
            continue;
        }
        const bool fresh = candidate->airdrop_seen_ms <= now_ms &&
            now_ms - candidate->airdrop_seen_ms <= max_age_ms;
        if (!fresh) {
            continue;
        }
        if (*peer != NULL) {
            *peer = NULL;
            return ESPDROP_TABLE_AMBIGUOUS;
        }
        *peer = candidate;
    }
    return *peer == NULL ? ESPDROP_TABLE_NOT_FOUND : ESPDROP_TABLE_OK;
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (source == NULL || capacity == 0) {
        return;
    }
    size_t length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

espdrop_table_result_t espdrop_peer_table_observe(
    espdrop_peer_table_t *table,
    const espdrop_peer_observation_t *observation,
    espdrop_peer_t **peer)
{
    if (table == NULL || observation == NULL ||
        !peer_id_valid(&observation->id) ||
        observation->signals == ESPDROP_PEER_SIGNAL_NONE) {
        return ESPDROP_TABLE_INVALID_ARGUMENT;
    }

    espdrop_peer_t *target = find_mutable(table, &observation->id);
    if (target == NULL) {
        if (table->count >= ESPDROP_MAX_PEERS) {
            return ESPDROP_TABLE_FULL;
        }
        target = &table->peers[table->count++];
        memset(target, 0, sizeof(*target));
        target->id = observation->id;
        target->first_seen_ms = observation->seen_ms;
        target->ble_rssi = -127;
        target->awdl_rssi = -127;
    }

    target->signals |= observation->signals;
    if (observation->seen_ms > target->last_seen_ms) {
        target->last_seen_ms = observation->seen_ms;
    }
    if ((observation->signals & ESPDROP_PEER_SIGNAL_BLE) != 0U) {
        target->ble_seen_ms = observation->seen_ms;
        target->ble_rssi = observation->rssi;
    }
    if ((observation->signals & ESPDROP_PEER_SIGNAL_AWDL) != 0U) {
        target->awdl_seen_ms = observation->seen_ms;
        target->awdl_rssi = observation->rssi;
        if (observation->awdl_mac != NULL) {
            memcpy(target->awdl_mac, observation->awdl_mac,
                   sizeof(target->awdl_mac));
        }
        if (observation->ipv6 != NULL) {
            memcpy(target->ipv6, observation->ipv6, sizeof(target->ipv6));
        }
    }
    if ((observation->signals & ESPDROP_PEER_SIGNAL_AIRDROP) != 0U) {
        target->airdrop_seen_ms = observation->seen_ms;
        copy_text(target->service_id, sizeof(target->service_id),
                  observation->service_id);
        copy_text(target->display_name, sizeof(target->display_name),
                  observation->display_name);
    }

    if (peer != NULL) {
        *peer = target;
    }
    return ESPDROP_TABLE_OK;
}

size_t espdrop_peer_table_expire(
    espdrop_peer_table_t *table,
    uint64_t now_ms,
    uint64_t max_age_ms)
{
    if (table == NULL) {
        return 0;
    }

    size_t write_index = 0;
    const size_t old_count = table->count;
    for (size_t read_index = 0; read_index < old_count; ++read_index) {
        const espdrop_peer_t *candidate = &table->peers[read_index];
        const bool future_timestamp = candidate->last_seen_ms > now_ms;
        const bool fresh = future_timestamp ||
                           now_ms - candidate->last_seen_ms <= max_age_ms;
        if (fresh) {
            if (write_index != read_index) {
                table->peers[write_index] = table->peers[read_index];
            }
            ++write_index;
        }
    }
    if (write_index < old_count) {
        memset(&table->peers[write_index], 0,
               (old_count - write_index) * sizeof(table->peers[0]));
    }
    table->count = write_index;
    return old_count - write_index;
}
