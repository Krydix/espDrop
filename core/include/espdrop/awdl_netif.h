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
    uint32_t rx_filtered;
    uint32_t tx_observed;
    uint32_t tx_enqueued;
    uint32_t tx_suppressed;
    uint32_t tx_dropped;
    uint32_t tx_invalid_size;
    uint32_t tx_queue_full;
    uint32_t tx_last_dropped_length;
    uint32_t tx_queue_high_water;
    uint32_t tx_submitted;
    uint32_t tx_redundant_submitted;
    uint32_t tx_accepted;
    uint32_t tx_errors;
    uint32_t tx_radio_success;
    uint32_t tx_radio_failed;
    uint32_t peer_mappings;
    uint32_t peer_mapping_failures;
    uint32_t mdns_queries;
    uint32_t mdns_packets;
    uint32_t mdns_responses;
    uint32_t mdns_services;
    uint32_t mdns_complete_services;
    uint32_t airdrop_receiver_queries;
    uint32_t airdrop_receiver_announcements;
    uint32_t airdrop_tcp_attempts;
    uint32_t airdrop_tcp_connected;
    uint32_t airdrop_tls_attempts;
    uint32_t airdrop_tls_connected;
    uint32_t airdrop_discover_attempts;
    uint32_t airdrop_discover_responses;
    uint32_t airdrop_discover_accepted;
    uint32_t airdrop_ask_attempts;
    uint32_t airdrop_ask_responses;
    uint32_t airdrop_ask_accepted;
    uint32_t airdrop_upload_attempts;
    uint32_t airdrop_upload_responses;
    uint32_t airdrop_upload_accepted;
    uint32_t tcp_tx_segments;
    uint32_t tcp_tx_syn;
    uint32_t tcp_tx_payload_bytes;
    uint32_t tcp_tx_last_sequence;
    uint32_t tcp_tx_last_acknowledgment;
    uint16_t tcp_tx_last_window;
    uint16_t tcp_tx_last_payload_length;
    uint32_t tcp_tx_radio_success;
    uint32_t tcp_tx_radio_failed;
    uint32_t tcp_rx_segments;
    uint32_t tcp_rx_syn_ack;
    uint32_t tcp_rx_rst;
    uint32_t tcp_rx_fin;
    uint32_t tcp_rx_zero_window;
    uint32_t tcp_rx_payload_bytes;
    uint32_t tcp_rx_last_sequence;
    uint32_t tcp_rx_last_acknowledgment;
    uint16_t tcp_rx_last_window;
    uint16_t tcp_rx_last_payload_length;
} espdrop_awdl_netif_stats_t;

esp_err_t espdrop_awdl_netif_init(const uint8_t self_mac[6]);

/* Seed lwIP's IPv6 neighbor cache from an AWDL MIF source address, matching
 * OWL's neighbor_add_rfc4291 behavior and avoiding an NDP handshake. */
bool espdrop_awdl_netif_add_peer(const uint8_t peer_mac[6]);

bool espdrop_awdl_netif_receive(const espdrop_awdl_data_t *data);

size_t espdrop_awdl_netif_flush(size_t maximum_frames);

void espdrop_awdl_netif_note_tx_done(
    bool success,
    const uint8_t *frame,
    size_t length);

espdrop_awdl_netif_stats_t espdrop_awdl_netif_stats(void);

/* Permit the persistent discovery task to make one fresh AirDrop connection
 * attempt for a newly armed host transfer. */
void espdrop_awdl_netif_request_airdrop_probe(void);

esp_netif_t *espdrop_awdl_netif_handle(void);

#ifdef __cplusplus
}
#endif
