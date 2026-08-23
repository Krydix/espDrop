#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/peer_table.h"

static espdrop_peer_id_t peer_id(uint8_t value)
{
    espdrop_peer_id_t id = {.length = 1};
    id.bytes[0] = value;
    return id;
}

int main(void)
{
    espdrop_peer_table_t table;
    espdrop_peer_table_init(&table);
    assert(table.count == 0);

    const espdrop_peer_observation_t ble = {
        .id = peer_id(1),
        .signals = ESPDROP_PEER_SIGNAL_BLE,
        .rssi = -37,
        .seen_ms = 1000,
    };
    espdrop_peer_t *peer = NULL;
    assert(espdrop_peer_table_observe(&table, &ble, &peer) == ESPDROP_TABLE_OK);
    assert(table.count == 1);
    assert(peer != NULL);
    assert(peer->ble_rssi == -37);
    assert(peer->first_seen_ms == 1000);

    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t ipv6[16] = {
        0xfe, 0x80, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1,
    };
    const espdrop_peer_observation_t awdl_and_airdrop = {
        .id = peer_id(1),
        .signals = ESPDROP_PEER_SIGNAL_AWDL | ESPDROP_PEER_SIGNAL_AIRDROP,
        .rssi = -42,
        .seen_ms = 1200,
        .awdl_mac = mac,
        .ipv6 = ipv6,
        .service_id = "ephemeral-service",
        .display_name = "Test iPhone",
    };
    assert(espdrop_peer_table_observe(&table, &awdl_and_airdrop, &peer) ==
           ESPDROP_TABLE_OK);
    assert(table.count == 1);
    assert(peer->signals == (ESPDROP_PEER_SIGNAL_BLE |
                             ESPDROP_PEER_SIGNAL_AWDL |
                             ESPDROP_PEER_SIGNAL_AIRDROP));
    assert(peer->last_seen_ms == 1200);
    assert(memcmp(peer->awdl_mac, mac, sizeof(mac)) == 0);
    assert(strcmp(peer->display_name, "Test iPhone") == 0);
    const espdrop_peer_t *selected = NULL;
    assert(espdrop_peer_table_select_unique_airdrop(
               &table, 1300U, 500U, &selected) == ESPDROP_TABLE_OK);
    assert(selected == peer);
    assert(espdrop_peer_table_select_unique_airdrop(
               &table, 2000U, 500U, &selected) == ESPDROP_TABLE_NOT_FOUND);
    assert(selected == NULL);
    assert(espdrop_peer_table_select_unique_airdrop(
               &table, 1000U, 500U, &selected) == ESPDROP_TABLE_NOT_FOUND);
    assert(selected == NULL);

    const espdrop_peer_observation_t other = {
        .id = peer_id(2),
        .signals = ESPDROP_PEER_SIGNAL_BLE,
        .rssi = -80,
        .seen_ms = 100,
    };
    assert(espdrop_peer_table_observe(&table, &other, NULL) == ESPDROP_TABLE_OK);
    assert(table.count == 2);
    assert(espdrop_peer_table_select_unique_airdrop(
               &table, 1300U, 500U, &selected) == ESPDROP_TABLE_OK);

    const espdrop_peer_observation_t second_airdrop = {
        .id = peer_id(3),
        .signals = ESPDROP_PEER_SIGNAL_AIRDROP,
        .rssi = -80,
        .seen_ms = 1250U,
        .service_id = "second-service",
    };
    assert(espdrop_peer_table_observe(
               &table, &second_airdrop, NULL) == ESPDROP_TABLE_OK);
    assert(espdrop_peer_table_select_unique_airdrop(
               &table, 1300U, 500U, &selected) ==
           ESPDROP_TABLE_AMBIGUOUS);
    assert(selected == NULL);
    assert(espdrop_peer_table_expire(&table, 1500, 500) == 1);
    assert(table.count == 2);
    assert(espdrop_peer_table_find(&table, &ble.id) != NULL);

    const espdrop_peer_observation_t invalid = {0};
    assert(espdrop_peer_table_observe(&table, &invalid, NULL) ==
           ESPDROP_TABLE_INVALID_ARGUMENT);
    assert(espdrop_peer_table_select_unique_airdrop(
               NULL, 0U, 0U, &selected) ==
           ESPDROP_TABLE_INVALID_ARGUMENT);

    puts("peer table tests passed");
    return 0;
}
