#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_PEER_ID_MAX_BYTES 16
#define ESPDROP_PEER_NAME_MAX_BYTES 64
#define ESPDROP_SERVICE_ID_MAX_BYTES 64
#define ESPDROP_IPV6_BYTES 16
#define ESPDROP_MAX_PEERS 16
#define ESPDROP_AWDL_CHANNEL_SEQUENCE_MAX 16

typedef enum {
    ESPDROP_ACCEPT_CONFIRM = 0,
    ESPDROP_ACCEPT_PERMISSIVE_DEVELOPMENT,
    ESPDROP_ACCEPT_REJECT,
} espdrop_accept_mode_t;

typedef enum {
    ESPDROP_PEER_SIGNAL_NONE = 0,
    ESPDROP_PEER_SIGNAL_BLE = 1U << 0,
    ESPDROP_PEER_SIGNAL_AWDL = 1U << 1,
    ESPDROP_PEER_SIGNAL_AIRDROP = 1U << 2,
} espdrop_peer_signal_t;

typedef struct {
    uint8_t bytes[ESPDROP_PEER_ID_MAX_BYTES];
    uint8_t length;
} espdrop_peer_id_t;

typedef struct {
    bool mif_seen;
    uint8_t protocol_version;
    uint8_t master[6];
    uint8_t sync_master[6];
    uint8_t master_channel;
    uint8_t presence_mode;
    uint16_t aw_period_tu;
    uint16_t tx_down_counter_tu;
    uint16_t next_aw_sequence;
    uint32_t distance_to_master;
    uint32_t master_metric;
    uint32_t self_metric;
    uint32_t master_counter;
    uint32_t self_counter;
    uint8_t channel_count;
    uint8_t channels[ESPDROP_AWDL_CHANNEL_SEQUENCE_MAX];
    uint8_t operating_classes[ESPDROP_AWDL_CHANNEL_SEQUENCE_MAX];
    uint16_t service_record_count;
    uint16_t malformed_service_record_count;
    bool advertises_airdrop;
    bool advertises_asquic;
} espdrop_awdl_peer_state_t;

typedef struct {
    espdrop_peer_id_t id;
    uint32_t signals;
    int16_t ble_rssi;
    int16_t awdl_rssi;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint64_t ble_seen_ms;
    uint64_t awdl_seen_ms;
    uint64_t airdrop_seen_ms;
    uint8_t awdl_mac[6];
    uint8_t ipv6[ESPDROP_IPV6_BYTES];
    espdrop_awdl_peer_state_t awdl;
    char service_id[ESPDROP_SERVICE_ID_MAX_BYTES];
    char display_name[ESPDROP_PEER_NAME_MAX_BYTES];
} espdrop_peer_t;

typedef struct {
    espdrop_peer_id_t id;
    uint32_t signals;
    int16_t rssi;
    uint64_t seen_ms;
    const uint8_t *awdl_mac;
    const uint8_t *ipv6;
    const char *service_id;
    const char *display_name;
} espdrop_peer_observation_t;

#ifdef __cplusplus
}
#endif
