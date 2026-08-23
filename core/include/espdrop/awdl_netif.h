#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "espdrop/awdl_data.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_AWDL_NETIF_SEQUENCE_MARKER 0x8000U

typedef struct {
    uint32_t rx_enqueued;
    uint32_t rx_injected;
    uint32_t rx_dropped;
    uint32_t tx_observed;
    uint32_t tx_enqueued;
    uint32_t tx_suppressed;
    uint32_t tx_dropped;
    uint32_t tx_submitted;
    uint32_t tx_accepted;
    uint32_t tx_errors;
    uint32_t tx_radio_success;
    uint32_t tx_radio_failed;
    uint32_t mdns_queries;
    uint32_t mdns_packets;
    uint32_t mdns_responses;
    uint32_t mdns_services;
    uint32_t mdns_complete_services;
    uint32_t airdrop_tcp_attempts;
    uint32_t airdrop_tcp_connected;
} espdrop_awdl_netif_stats_t;

esp_err_t espdrop_awdl_netif_init(const uint8_t self_mac[6]);

bool espdrop_awdl_netif_receive(const espdrop_awdl_data_t *data);

size_t espdrop_awdl_netif_flush(size_t maximum_frames);

void espdrop_awdl_netif_note_tx_done(bool success);

espdrop_awdl_netif_stats_t espdrop_awdl_netif_stats(void);

esp_netif_t *espdrop_awdl_netif_handle(void);

#ifdef __cplusplus
}
#endif
