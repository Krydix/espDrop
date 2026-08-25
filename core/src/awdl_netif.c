#include "espdrop/awdl_netif.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "espdrop/airdrop_mdns.h"
#include "espdrop/airdrop_outgoing.h"
#include "espdrop/airdrop_tls.h"
#include "espdrop/ble_wake.h"
#include "espdrop/espdrop.h"
#include "espdrop/awdl_tx_lab.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/mld6.h"
#include "lwip/netif.h"
#include "lwip/priv/nd6_priv.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

#define AWDL_NETIF_RX_QUEUE_DEPTH 8U
#define AWDL_NETIF_TX_QUEUE_DEPTH 64U
#define AWDL_NETIF_MAX_ETHERNET_BYTES 1474U
#define AWDL_NETIF_RAW_FRAME_BYTES 1500U
#define AWDL_NETIF_MTU 1460U
#define AWDL_NETIF_TCP_PAYLOAD_COPIES 2U
#define AWDL_MDNS_PORT 5353U
#define AWDL_MDNS_QUERY_ATTEMPTS 6U
#define AWDL_MDNS_RESOLVE_BUDGET 1U
#if CONFIG_ESPDROP_AIRDROP_TLS_LAB
#define AWDL_MDNS_TASK_STACK_BYTES 12288U
#else
#define AWDL_MDNS_TASK_STACK_BYTES 5120U
#endif

typedef struct {
    esp_netif_driver_base_t base;
} awdl_driver_t;

typedef struct {
    uint16_t length;
    uint8_t bytes[AWDL_NETIF_MAX_ETHERNET_BYTES];
} awdl_ethernet_frame_t;

static const char *TAG = "awdl_netif";
static awdl_driver_t driver;
static esp_netif_t *awdl_netif;
static QueueHandle_t rx_queue;
static uint8_t station_mac[6];
/*
 * These frames are intentionally static. Each belongs to one serialized
 * execution context (tcpip, Wi-Fi RX, or the lab TX task), and keeping the
 * 1.5 KiB records off those task stacks avoids needlessly tight headroom.
 */
static awdl_ethernet_frame_t rx_task_scratch;
static awdl_ethernet_frame_t promiscuous_rx_scratch;
#if CONFIG_ESPDROP_AWDL_TX_LAB
static QueueHandle_t tx_queue;
static StaticQueue_t tx_queue_control;
static uint8_t *tx_queue_storage;
static awdl_ethernet_frame_t driver_tx_scratch;
static awdl_ethernet_frame_t radio_tx_scratch;
static uint8_t radio_raw_scratch[AWDL_NETIF_RAW_FRAME_BYTES];
static uint16_t ieee80211_sequence = 0x700U;
static uint16_t awdl_sequence;
#endif
static espdrop_awdl_netif_stats_t stats;
static bool initialized;

typedef struct {
    struct netif *netif;
    ip6_addr_t address;
    uint8_t mac[6];
    err_t result;
    bool created;
} awdl_neighbor_mapping_t;

static void add_awdl_neighbor(void *argument)
{
    awdl_neighbor_mapping_t *mapping = argument;
    int selected = -1;
    for (int index = 0; index < LWIP_ND6_NUM_NEIGHBORS; ++index) {
        if (neighbor_cache[index].state != ND6_NO_ENTRY &&
            ip6_addr_eq(&neighbor_cache[index].next_hop_address,
                        &mapping->address)) {
            selected = index;
            break;
        }
        if (selected < 0 &&
            neighbor_cache[index].state == ND6_NO_ENTRY) {
            selected = index;
            mapping->created = true;
        }
    }
    if (selected < 0) {
        mapping->result = ERR_MEM;
        return;
    }

    struct nd6_neighbor_cache_entry *entry = &neighbor_cache[selected];
    if (entry->q != NULL) {
        /* This path is called as soon as the MIF arrives, before socket traffic
         * is started. Refuse to replace a queued NDP entry rather than leak or
         * reorder lwIP-owned pbufs if that invariant is ever violated. */
        mapping->result = ERR_INPROGRESS;
        return;
    }
    ip6_addr_copy(entry->next_hop_address, mapping->address);
    entry->netif = mapping->netif;
    memset(entry->lladdr, 0, sizeof(entry->lladdr));
    memcpy(entry->lladdr, mapping->mac, sizeof(mapping->mac));
    entry->isrouter = 0U;
    entry->state = ND6_REACHABLE;
    entry->counter.reachable_time = reachable_time;
    mapping->result = ERR_OK;
}

static esp_err_t driver_transmit(void *handle, void *buffer, size_t length)
{
    (void)handle;
    ++stats.tx_observed;
    if (buffer == NULL || length < ESPDROP_ETHERNET_HEADER_BYTES ||
        length > AWDL_NETIF_MAX_ETHERNET_BYTES) {
        ++stats.tx_dropped;
        ++stats.tx_invalid_size;
        stats.tx_last_dropped_length = length;
        return ESP_ERR_INVALID_SIZE;
    }
#if CONFIG_ESPDROP_AWDL_TX_LAB
    driver_tx_scratch.length = (uint16_t)length;
    memcpy(driver_tx_scratch.bytes, buffer, length);
    if (tx_queue == NULL ||
        xQueueSend(tx_queue, &driver_tx_scratch, 0) != pdTRUE) {
        ++stats.tx_dropped;
        ++stats.tx_queue_full;
        stats.tx_last_dropped_length = length;
        return ESP_ERR_NO_MEM;
    }
    ++stats.tx_enqueued;
    const UBaseType_t queued = uxQueueMessagesWaiting(tx_queue);
    if (queued > stats.tx_queue_high_water) {
        stats.tx_queue_high_water = queued;
    }
#else
    ++stats.tx_suppressed;
#endif
    return ESP_OK;
}

static void driver_free_rx(void *handle, void *buffer)
{
    (void)handle;
    free(buffer);
}

static esp_err_t driver_post_attach(
    esp_netif_t *netif,
    esp_netif_iodriver_handle handle)
{
    awdl_driver_t *attached = handle;
    attached->base.netif = netif;
    esp_netif_action_start(netif, NULL, 0, NULL);
    esp_netif_action_connected(netif, NULL, 0, NULL);
    return ESP_OK;
}

static void rx_task(void *argument)
{
    (void)argument;
    while (true) {
        if (xQueueReceive(rx_queue, &rx_task_scratch,
                          portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint8_t *owned = malloc(rx_task_scratch.length);
        if (owned == NULL) {
            ++stats.rx_dropped;
            continue;
        }
        memcpy(owned, rx_task_scratch.bytes, rx_task_scratch.length);
        const esp_err_t result =
            esp_netif_receive(awdl_netif, owned, rx_task_scratch.length, NULL);
        if (result == ESP_OK) {
            ++stats.rx_injected;
        } else {
            ++stats.rx_dropped;
        }
    }
}

#if CONFIG_ESPDROP_AWDL_MDNS_LAB
typedef struct {
    struct netif *netif;
    ip6_addr_t group;
    err_t result;
} awdl_mld_join_t;

static void join_mld_group(void *argument)
{
    awdl_mld_join_t *join = argument;
    join->result = mld6_joingroup_netif(join->netif, &join->group);
}

static espdrop_airdrop_mdns_result_t mdns_cache;
static espdrop_airdrop_mdns_result_t mdns_parse_scratch;
static bool airdrop_mif_probe_started;
static bool airdrop_mdns_probe_started;
static uint64_t airdrop_probe_requested_ms;

void espdrop_awdl_netif_request_airdrop_probe(void)
{
    airdrop_mif_probe_started = false;
    airdrop_mdns_probe_started = false;
    airdrop_probe_requested_ms =
        (uint64_t)esp_timer_get_time() / 1000U;
}

#if !CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_LAB
static void probe_airdrop_tcp(int interface_index);
#endif

static void log_service(const espdrop_airdrop_service_t *service)
{
    char address[INET6_ADDRSTRLEN] = "-";
    if (service->has_ipv6) {
        ip6_addr_t ipv6;
        memcpy(&ipv6, service->ipv6, sizeof(service->ipv6));
        (void)inet6_ntoa_r(ipv6, address, sizeof(address));
    }
    ESP_LOGI(TAG,
             "AWDL-AIRDROP-SERVICE instance=%s target=%s ipv6=%s port=%u "
             "ptr=%u srv=%u txt_record=%u aaaa=%u complete=%u txt=%s",
             service->instance,
             service->has_srv ? service->target : "-", address,
             service->port, service->has_ptr, service->has_srv,
             service->has_txt, service->has_ipv6,
             espdrop_airdrop_service_complete(service),
             service->has_txt && service->txt[0] != '\0' ? service->txt : "-");
}

static bool publish_complete_service(
    const espdrop_airdrop_service_t *service)
{
    const bool first_publish = !service->endpoint_published;
    espdrop_peer_table_t *table = espdrop_peers();
    if (table == NULL || !espdrop_lock_peers()) {
        return false;
    }
    espdrop_peer_t *peer = NULL;
    uint8_t peer_mac[6] = {0};
    const espdrop_table_result_t result =
        espdrop_peer_table_apply_airdrop_endpoint(
            table, service->ipv6, service->port, service->instance,
            (uint64_t)esp_timer_get_time() / 1000U, &peer);
    if (result == ESPDROP_TABLE_OK && peer != NULL) {
        memcpy(peer_mac, peer->awdl_mac, sizeof(peer_mac));
    }
    espdrop_unlock_peers();
    if (result != ESPDROP_TABLE_OK || peer == NULL) {
        return false;
    }
    char address[INET6_ADDRSTRLEN] = "-";
    ip6_addr_t ipv6;
    memcpy(&ipv6, service->ipv6, sizeof(service->ipv6));
    (void)inet6_ntoa_r(ipv6, address, sizeof(address));
    if (first_publish) {
        ESP_LOGW(TAG,
                 "AWDL-AIRDROP-ENDPOINT instance=%s ipv6=%s port=%u "
                 "peer=%02x:%02x:%02x:%02x:%02x:%02x complete=1",
                 service->instance, address, service->port,
                 peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3],
                 peer_mac[4], peer_mac[5]);
    }
    return true;
}

static bool receive_mdns_packet(
    int socket_fd,
    const struct sockaddr_in6 *announcement_destination,
    const uint8_t *announcement,
    size_t announcement_bytes)
{
    static uint8_t packet[768];
    struct sockaddr_in6 source = {0};
    socklen_t source_length = sizeof(source);
    const ssize_t received = recvfrom(socket_fd, packet, sizeof(packet), 0,
                                      (struct sockaddr *)&source,
                                      &source_length);
    if (received < 12) {
        return false;
    }
    const uint16_t flags =
        (uint16_t)((uint16_t)packet[2] << 8U) | packet[3];
    const uint16_t questions =
        (uint16_t)((uint16_t)packet[4] << 8U) | packet[5];
    const uint16_t answers =
        (uint16_t)((uint16_t)packet[6] << 8U) | packet[7];
    const uint16_t authority =
        (uint16_t)((uint16_t)packet[8] << 8U) | packet[9];
    const uint16_t additional =
        (uint16_t)((uint16_t)packet[10] << 8U) | packet[11];
    ++stats.mdns_packets;
    if ((flags & 0x8000U) != 0U) {
        ++stats.mdns_responses;
    }
    ESP_LOGI(TAG,
             "AWDL-MDNS-RX bytes=%d response=%u qd=%u an=%u ns=%u ar=%u count=%lu",
             (int)received, (flags & 0x8000U) != 0U, questions, answers,
             authority, additional, (unsigned long)stats.mdns_packets);

#if CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_LAB
    const espdrop_mdns_query_response_t response_mode =
        espdrop_airdrop_mdns_query_response(packet, (size_t)received);
    if (announcement_destination != NULL && announcement != NULL &&
        announcement_bytes != 0U &&
        response_mode != ESPDROP_MDNS_QUERY_RESPONSE_NONE) {
        struct sockaddr_in6 response_destination =
            *announcement_destination;
        if (response_mode == ESPDROP_MDNS_QUERY_RESPONSE_UNICAST) {
            response_destination = source;
            if (response_destination.sin6_scope_id == 0U) {
                response_destination.sin6_scope_id =
                    announcement_destination->sin6_scope_id;
            }
        }
        ++stats.airdrop_receiver_queries;
        const ssize_t sent = sendto(
            socket_fd, announcement, announcement_bytes, 0,
            (const struct sockaddr *)&response_destination,
            sizeof(response_destination));
        if (sent == (ssize_t)announcement_bytes) {
            ++stats.airdrop_receiver_announcements;
        }
        ESP_LOGW(TAG,
                 "AWDL-AIRDROP-RECEIVER query=%lu announcement=%lu "
                 "mode=%s bytes=%d error=%d",
                 (unsigned long)stats.airdrop_receiver_queries,
                 (unsigned long)stats.airdrop_receiver_announcements,
                 response_mode == ESPDROP_MDNS_QUERY_RESPONSE_UNICAST
                     ? "unicast" : "multicast",
                 (int)sent, sent < 0 ? errno : 0);
    }
#else
    (void)announcement_destination;
    (void)announcement;
    (void)announcement_bytes;
#endif

    if (!espdrop_airdrop_mdns_parse(packet, (size_t)received,
                                    &mdns_parse_scratch)) {
        ESP_LOGW(TAG, "AWDL-MDNS-PARSE bytes=%d result=invalid", (int)received);
        return true;
    }
    espdrop_airdrop_mdns_merge(&mdns_cache, &mdns_parse_scratch);
    stats.mdns_services = (uint32_t)mdns_cache.service_count;
    stats.mdns_complete_services = 0U;
    for (size_t index = 0U; index < mdns_cache.service_count; ++index) {
        espdrop_airdrop_service_t *service = &mdns_cache.services[index];
        if (espdrop_airdrop_service_complete(service)) {
            ++stats.mdns_complete_services;
            /* Re-apply complete endpoints to refresh their observation time.
             * Selection deliberately waits for a proximity settle window, so
             * a one-shot publication can occur too early to choose anything. */
            if (publish_complete_service(service)) {
                service->endpoint_published = true;
            }
        }
        log_service(service);
    }
    if (stats.mdns_complete_services != 0U) {
        (void)espdrop_awdl_tx_lab_consider_airdrop_endpoints(
            (uint64_t)esp_timer_get_time());
    }
    return true;
}

#if !CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_LAB
static int connect_airdrop_tcp_service(
    int interface_index,
    const espdrop_airdrop_service_t *service,
    unsigned attempt)
{
    ++stats.airdrop_tcp_attempts;
    const int socket_fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG,
                 "AWDL-AIRDROP-TCP instance=%s attempt=%u "
                 "result=socket-error error=%d",
                 service->instance, attempt, errno);
        return -1;
    }
    const struct timeval timeout = {.tv_sec = 6, .tv_usec = 0};
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                     &timeout, sizeof(timeout));
    struct sockaddr_in6 destination = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(service->port),
        .sin6_scope_id = (uint32_t)interface_index,
    };
    memcpy(&destination.sin6_addr, service->ipv6,
           sizeof(destination.sin6_addr));
    (void)fcntl(socket_fd, F_SETFL,
                fcntl(socket_fd, F_GETFL, 0) | O_NONBLOCK);
    int result = connect(socket_fd, (struct sockaddr *)&destination,
                         sizeof(destination));
    int connection_error = result == 0 ? 0 : errno;
    if (result != 0 && connection_error == EINPROGRESS) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_fd, &write_set);
        /* The closed-share-sheet iOS 26 wake profile has answered from 2 to
         * 32.5 seconds after the first SYN on hardware. Keep this bounded,
         * but retain the PCB long enough to admit that observed late reply. */
        struct timeval connect_timeout = {.tv_sec = 45, .tv_usec = 0};
        const int selected = select(socket_fd + 1, NULL, &write_set, NULL,
                                    &connect_timeout);
        if (selected > 0) {
            socklen_t error_length = sizeof(connection_error);
            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR,
                           &connection_error, &error_length) != 0) {
                connection_error = errno;
            }
            result = connection_error == 0 ? 0 : -1;
        } else {
            connection_error = selected == 0 ? ETIMEDOUT : errno;
            result = -1;
        }
    }
    if (result == 0) {
        ++stats.airdrop_tcp_connected;
    }
    char address[INET6_ADDRSTRLEN] = "-";
    ip6_addr_t ipv6;
    memcpy(&ipv6, service->ipv6, sizeof(service->ipv6));
    (void)inet6_ntoa_r(ipv6, address, sizeof(address));
    ESP_LOGW(TAG,
             "AWDL-AIRDROP-TCP instance=%s attempt=%u target=%s ipv6=%s "
             "port=%u result=%s error=%d",
             service->instance, attempt, service->target, address,
             service->port, result == 0 ? "connected" : "failed",
             connection_error);
    if (result != 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static void probe_airdrop_tcp_service(
    int interface_index,
    const espdrop_airdrop_service_t *service)
{
#if CONFIG_ESPDROP_BLE_WAKE_LAB
    /* A fresh receiver publication is the wake-complete boundary. Release
     * NimBLE before opening TCP so Wi-Fi owns the coexistence radio for AWDL
     * data, TLS, and upload. */
    if (espdrop_ble_wake_active()) {
        const int wake_stop_result = espdrop_ble_wake_stop();
        if (wake_stop_result != ESP_OK) {
            ESP_LOGW(TAG, "AirDrop BLE wake stop failed: %s",
                     esp_err_to_name(wake_stop_result));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50U));
        }
    }
    /* lwIP caps SYN retransmissions at 12. Retain bounded fresh PCBs for a
     * receiver that takes several AWDL windows to admit the sender. */
    const unsigned maximum_attempts = 4U;
#else
    const unsigned maximum_attempts = 1U;
#endif
    int socket_fd = -1;
    for (unsigned attempt = 1U;
         attempt <= maximum_attempts && socket_fd < 0; ++attempt) {
        socket_fd = connect_airdrop_tcp_service(
            interface_index, service, attempt);
#if CONFIG_ESPDROP_BLE_WAKE_LAB
        if (socket_fd < 0 && attempt < maximum_attempts &&
            espdrop_ble_wake_active()) {
            vTaskDelay(pdMS_TO_TICKS(250U));
        }
#endif
    }
#if CONFIG_ESPDROP_AIRDROP_TLS_LAB
    if (socket_fd >= 0) {
        ++stats.airdrop_tls_attempts;
        espdrop_airdrop_tls_result_t tls;
        espdrop_airdrop_discover_result_t discover;
#if CONFIG_ESPDROP_AIRDROP_ASK_LAB
        espdrop_airdrop_ask_result_t ask;
        memset(&discover, 0, sizeof(discover));
#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
        espdrop_airdrop_upload_result_t upload;
        memset(&upload, 0, sizeof(upload));
#endif
#endif
        /* A TLS flight can span several AWDL availability windows. The
         * receiver's first handshake response has been observed near the
         * former six-second bound, so leave enough time for our certificate
         * flight and the receiver's Finished message. */
        const bool tls_connected =
#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
            ({
                espdrop_airdrop_outgoing_file_t outgoing;
                const bool has_outgoing =
                    espdrop_airdrop_outgoing_acquire(&outgoing);
                bool sent;
                if (has_outgoing) {
                    /* The anonymous native Mac sender creates a second
                     * Network.framework request object for Upload, but that
                     * object joins the accepted Ask's HTTP/1, TCP, and TLS
                     * flow. Mirror the wire behavior directly: sequential
                     * requests on this socket with one TransferID. */
                    sent = espdrop_airdrop_tls_ask_upload_stream_probe(
                        socket_fd, service->target, service->port, 12000U,
                        30000U, 120000U, &outgoing, &tls, &ask, &upload);
                } else {
                    sent = espdrop_airdrop_tls_ask_upload_probe(
                        socket_fd, service->target, service->port, 12000U,
                        30000U, 30000U, &tls, &ask, &upload);
                }
                if (has_outgoing) {
                    espdrop_airdrop_outgoing_result_t completion = {
                        .state = ESPDROP_AIRDROP_OUTGOING_RESULT_FAILED,
                    };
                    if (!tls.connected) {
                        completion.stage = ESPDROP_AIRDROP_OUTGOING_STAGE_TLS;
                        completion.error = tls.error;
                    } else if (!ask.response_complete ||
                               ask.http_status != 200U) {
                        completion.stage = ESPDROP_AIRDROP_OUTGOING_STAGE_ASK;
                        completion.error = ask.error;
                        completion.http_status = ask.http_status;
                        completion.request_bytes = ask.request_bytes;
                    } else {
                        completion.stage =
                            ESPDROP_AIRDROP_OUTGOING_STAGE_UPLOAD;
                        completion.error = upload.error;
                        completion.http_status = upload.http_status;
                        completion.request_bytes = upload.request_bytes;
                        completion.payload_bytes = upload.payload_bytes;
                        if (upload.response_complete &&
                            upload.http_status == 200U) {
                            completion.state =
                                ESPDROP_AIRDROP_OUTGOING_RESULT_SUCCESS;
                        }
                    }
                    espdrop_airdrop_outgoing_complete(&completion);
                    espdrop_airdrop_outgoing_release();
                }
                sent;
            });
#elif CONFIG_ESPDROP_AIRDROP_ASK_LAB
            espdrop_airdrop_tls_ask_probe(
                socket_fd, service->target, service->port, 12000U, 30000U,
                &tls, &ask);
#else
            espdrop_airdrop_tls_discover_probe(
            socket_fd, service->target, service->port, 12000U, 8000U,
            &tls, &discover);
#endif
        if (tls_connected) {
            ++stats.airdrop_tls_connected;
        }
        ESP_LOGW(TAG,
                 "AWDL-AIRDROP-TLS instance=%s result=%s error=%d "
                 "version=%s cipher=%s verify=0x%08lx peer_cert=%u "
                 "peer_cert_bytes=%u",
                 service->instance,
                 tls_connected ? "connected" : "failed", tls.error,
                 tls.version, tls.ciphersuite,
                 (unsigned long)tls.verify_flags,
                 tls.peer_certificate_present ? 1U : 0U,
                 (unsigned)tls.peer_certificate_bytes);
        if (discover.attempted) {
            ++stats.airdrop_discover_attempts;
        }
        if (discover.response_complete) {
            ++stats.airdrop_discover_responses;
        }
        const bool discover_accepted =
            discover.response_complete && discover.http_status == 200U;
        if (discover_accepted) {
            ++stats.airdrop_discover_accepted;
        }
        if (discover.attempted) {
            ESP_LOGW(TAG,
                     "AWDL-AIRDROP-DISCOVER instance=%s attempted=1 "
                     "result=%s error=%d status=%u request_bytes=%u "
                     "response_bytes=%u body_bytes=%u bplist=%u "
                     "receiver_name=%u chunked=%u type=%s encoding=%s",
                     service->instance,
                     discover_accepted ? "accepted" :
                         (discover.response_complete ? "rejected" : "failed"),
                     discover.error, discover.http_status,
                     (unsigned)discover.request_bytes,
                     (unsigned)discover.response_bytes,
                     (unsigned)discover.body_bytes,
                     discover.binary_plist ? 1U : 0U,
                     discover.receiver_computer_name_key ? 1U : 0U,
                     discover.chunked ? 1U : 0U,
                     discover.content_type, discover.content_encoding);
        }
#if CONFIG_ESPDROP_AIRDROP_ASK_LAB
        if (ask.attempted) {
            ++stats.airdrop_ask_attempts;
        }
        if (ask.response_complete) {
            ++stats.airdrop_ask_responses;
        }
        const bool ask_accepted =
            ask.response_complete && ask.http_status == 200U;
        if (ask_accepted) {
            ++stats.airdrop_ask_accepted;
        }
#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
        static const char ask_upload_policy[] = "enabled-on-accept";
#else
        static const char ask_upload_policy[] = "disabled";
#endif
        ESP_LOGW(TAG,
                 "AWDL-AIRDROP-ASK instance=%s attempted=%u result=%s "
                 "error=%d status=%u request_bytes=%u response_bytes=%u "
                 "body_bytes=%u bplist=%u receiver_name=%u ids_session=%u "
                 "receiver_pseudonym=%u receiver_push_token=%u chunked=%u "
                 "transfer_id=%s upload=%s",
                 service->instance, ask.attempted ? 1U : 0U,
                 ask_accepted ? "accepted" :
                     (ask.response_complete ? "rejected" : "failed"),
                 ask.error, ask.http_status, (unsigned)ask.request_bytes,
                 (unsigned)ask.response_bytes, (unsigned)ask.body_bytes,
                 ask.binary_plist ? 1U : 0U,
                 ask.receiver_computer_name_key ? 1U : 0U,
                 ask.ids_session_id_key ? 1U : 0U,
                 ask.receiver_pseudonym_key ? 1U : 0U,
                 ask.receiver_push_token_key ? 1U : 0U,
                 ask.chunked ? 1U : 0U, ask.transfer_id,
                 ask_upload_policy);
#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
        if (upload.attempted) {
            ++stats.airdrop_upload_attempts;
        }
        if (upload.response_complete) {
            ++stats.airdrop_upload_responses;
        }
        const bool upload_accepted =
            upload.response_complete && upload.http_status == 200U;
        if (upload_accepted) {
            ++stats.airdrop_upload_accepted;
        }
        ESP_LOGW(TAG,
                 "AWDL-AIRDROP-UPLOAD instance=%s attempted=%u result=%s "
                 "error=%d status=%u request_bytes=%u payload_bytes=%u "
                 "archive_bytes=%u compressed_bytes=%u file_bytes=%u "
                 "dvzip_blocks=%u stored=%u workspace=%u crc32=%08lx "
                 "stream_status=%d "
                 "response_bytes=%u body_bytes=%u transfer_id=%s "
                 "continuity=%u connection=%s retry=disabled",
                 service->instance, upload.attempted ? 1U : 0U,
                 upload_accepted ? "accepted" :
                     (upload.response_complete ? "rejected" : "failed"),
                 upload.error, upload.http_status,
                 (unsigned)upload.request_bytes,
                 (unsigned)upload.payload_bytes,
                 (unsigned)upload.archive_bytes,
                 (unsigned)upload.compressed_bytes,
                 (unsigned)upload.file_bytes,
                 (unsigned)upload.dvzip_blocks,
                 upload.stored_blocks ? 1U : 0U,
                 (unsigned)upload.workspace_high_water,
                 (unsigned long)upload.source_crc32,
                 upload.stream_status,
                 (unsigned)upload.response_bytes,
                 (unsigned)upload.body_bytes, upload.transfer_id,
                 upload.transfer_id_continuity ? 1U : 0U,
                 "ask-socket");
#endif
#endif
    }
#endif
    if (socket_fd >= 0) {
        close(socket_fd);
    }
}

static void probe_airdrop_tcp(int interface_index)
{
#if CONFIG_ESPDROP_AIRDROP_UPLOAD_LAB
    /* In serial-controlled relay mode, discovery and target selection may run
     * before a host has armed a file. Never fall back to the built-in lab
     * fixture or present a prompt the host did not request. */
    if (!espdrop_airdrop_outgoing_ready()) {
        return;
    }
#endif
    uint8_t configured_target[6];
    const bool has_configured_target =
        espdrop_awdl_tx_lab_target(configured_target);
#if CONFIG_ESPDROP_AWDL_LAB_AUTO_TARGET_AIRDROP
    /* A multicast service cache can become complete before proximity
     * selection settles. Do not open a connection to an arbitrary cached
     * endpoint; wait until the selector installs a concrete AWDL target. */
    if (!has_configured_target) {
        return;
    }
#endif
    uint8_t configured_address[16];
    char configured_instance[ESPDROP_SERVICE_ID_MAX_BYTES] = {0};
    if (has_configured_target) {
        espdrop_awdl_link_local_from_mac(configured_target,
                                         configured_address);
        /* A stream generation starts BLE wake and invalidates any cached
         * endpoint. Do not stop BLE or consume TCP retries until this exact
         * target advertises AirDrop again after that generation began. */
        bool fresh_endpoint = false;
        espdrop_peer_id_t id = {.length = 6U};
        memcpy(id.bytes, configured_target, sizeof(configured_target));
        espdrop_peer_table_t *table = espdrop_peers();
        if (table != NULL && espdrop_lock_peers()) {
            const espdrop_peer_t *peer =
                espdrop_peer_table_find(table, &id);
            fresh_endpoint =
                peer != NULL && peer->airdrop_endpoint_complete &&
                peer->airdrop_port != 0U &&
                peer->airdrop_seen_ms >= airdrop_probe_requested_ms;
            if (fresh_endpoint) {
                (void)strncpy(configured_instance, peer->service_id,
                              sizeof(configured_instance) - 1U);
            }
            espdrop_unlock_peers();
        }
        if (!fresh_endpoint) {
            return;
        }
        /* A receiver can publish its service before election convergence.
         * Starting connect() at that point burns lwIP's bounded SYN/PCB
         * retries while our transmitter has no common availability window.
         * Wait until the selected peer and our sender are in the same live
         * synchronization tree; the queued SYN can then leave in the next
         * strictly common channel window. */
        if (!espdrop_awdl_tx_lab_target_is_sync_master()) {
            return;
        }
    }
    const espdrop_airdrop_service_t *matched_mdns = NULL;
    for (size_t index = 0U; index < mdns_cache.service_count; ++index) {
        const espdrop_airdrop_service_t *service = &mdns_cache.services[index];
        if (espdrop_airdrop_service_complete(service) &&
            (!has_configured_target ||
             memcmp(service->ipv6, configured_address,
                    sizeof(configured_address)) == 0) &&
            (configured_instance[0] == '\0' ||
             strncmp(service->instance, configured_instance,
                     strlen(configured_instance)) == 0)) {
            matched_mdns = service;
            break;
        }
    }
    if (matched_mdns != NULL) {
        if (!airdrop_mdns_probe_started) {
            airdrop_mdns_probe_started = true;
            probe_airdrop_tcp_service(interface_index, matched_mdns);
        }
        return;
    }
    if (!has_configured_target || airdrop_mif_probe_started) {
        return;
    }

    /* Full Apple MIFs commonly include their PTR/SRV/TXT records. When mDNS
     * does not answer, connect using that SRV port and the originating AWDL
     * peer's link-local address. */
    espdrop_airdrop_service_t mif_service = {0};
    espdrop_peer_id_t id = {.length = 6U};
    memcpy(id.bytes, configured_target, sizeof(configured_target));
    espdrop_peer_table_t *table = espdrop_peers();
    if (table == NULL || !espdrop_lock_peers()) {
        return;
    }
    const espdrop_peer_t *peer = espdrop_peer_table_find(table, &id);
    if (peer != NULL && peer->airdrop_endpoint_complete &&
        peer->airdrop_port != 0U) {
        (void)strncpy(mif_service.instance, peer->service_id,
                      sizeof(mif_service.instance) - 1U);
        (void)strncpy(mif_service.target, "mif-awdl-peer.local",
                      sizeof(mif_service.target) - 1U);
        memcpy(mif_service.ipv6, peer->ipv6, sizeof(mif_service.ipv6));
        mif_service.port = peer->airdrop_port;
        mif_service.has_ptr = true;
        mif_service.has_srv = true;
        mif_service.has_txt = true;
        mif_service.has_ipv6 = true;
    }
    espdrop_unlock_peers();
    if (espdrop_airdrop_service_complete(&mif_service)) {
        airdrop_mif_probe_started = true;
        ESP_LOGW(TAG,
                 "AWDL-AIRDROP-ENDPOINT instance=%s port=%u "
                 "evidence=mif-service-response",
                 mif_service.instance, mif_service.port);
        probe_airdrop_tcp_service(interface_index, &mif_service);
    }
}
#endif

static ssize_t send_mdns_question(
    int socket_fd,
    const struct sockaddr_in6 *destination,
    const char *name,
    uint16_t type,
    bool unicast_response)
{
    uint8_t query[ESPDROP_MDNS_NAME_BYTES + 20U];
    size_t query_length = 0U;
    if (!espdrop_mdns_build_query(query, sizeof(query), &query_length,
                                  name, type, unicast_response)) {
        errno = EINVAL;
        return -1;
    }
    const ssize_t sent = sendto(socket_fd, query, query_length, 0,
                                (const struct sockaddr *)destination,
                                sizeof(*destination));
    if (sent == (ssize_t)query_length) {
        ++stats.mdns_queries;
    }
    return sent;
}

static void send_resolution_questions(
    int socket_fd,
    const struct sockaddr_in6 *destination,
    unsigned round)
{
    unsigned remaining = AWDL_MDNS_RESOLVE_BUDGET;
    for (size_t index = 0U;
         index < mdns_cache.service_count && remaining != 0U; ++index) {
        const espdrop_airdrop_service_t *service = &mdns_cache.services[index];
        const struct {
            bool missing;
            const char *name;
            uint16_t type;
        } questions[] = {
            {!service->has_srv, service->instance, ESPDROP_MDNS_TYPE_SRV},
            {!service->has_txt, service->instance, ESPDROP_MDNS_TYPE_TXT},
            {service->has_srv && !service->has_ipv6,
             service->target, ESPDROP_MDNS_TYPE_AAAA},
        };
        for (size_t question = 0U;
             question < sizeof(questions) / sizeof(questions[0]) &&
             remaining != 0U; ++question) {
            if (!questions[question].missing ||
                questions[question].name[0] == '\0') {
                continue;
            }
            const ssize_t sent = send_mdns_question(
                socket_fd, destination, questions[question].name,
                questions[question].type, round == 1U);
            ESP_LOGI(TAG,
                     "AWDL-MDNS-RESOLVE round=%u name=%s type=%u "
                     "bytes=%d error=%d",
                     round, questions[question].name,
                     questions[question].type, (int)sent,
                     sent < 0 ? errno : 0);
            --remaining;
        }
    }
}

static void mdns_task(void *argument)
{
    (void)argument;
    esp_ip6_addr_t link_local;
    for (unsigned attempt = 0; attempt < 20U; ++attempt) {
        if (esp_netif_get_ip6_linklocal(awdl_netif, &link_local) == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
    if (esp_netif_get_ip6_linklocal(awdl_netif, &link_local) != ESP_OK) {
        ESP_LOGE(TAG, "AWDL-MDNS link-local address unavailable");
        vTaskDelete(NULL);
        return;
    }

    const int interface_index = esp_netif_get_netif_impl_index(awdl_netif);
    ESP_LOGI(TAG, "AWDL-NETIF ready if=%d ipv6=" IPV6STR " mtu=%u up=%u",
             interface_index, IPV62STR(link_local), AWDL_NETIF_MTU,
             esp_netif_is_netif_up(awdl_netif) ? 1U : 0U);

    const int socket_fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "AWDL-MDNS socket error=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    const int enabled = 1;
    (void)setsockopt(socket_fd, IPPROTO_IPV6, IPV6_V6ONLY,
                     &enabled, sizeof(enabled));
    const uint8_t loopback = 0;
    (void)setsockopt(socket_fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                     &loopback, sizeof(loopback));
    const uint8_t hops = 255;
    (void)setsockopt(socket_fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                     &hops, sizeof(hops));
    const uint8_t multicast_interface = (uint8_t)interface_index;
    (void)setsockopt(socket_fd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                     &multicast_interface, sizeof(multicast_interface));
    const struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout));

    struct sockaddr_in6 bind_address = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(AWDL_MDNS_PORT),
        .sin6_addr = IN6ADDR_ANY_INIT,
    };
    if (bind(socket_fd, (struct sockaddr *)&bind_address,
             sizeof(bind_address)) != 0) {
        ESP_LOGE(TAG, "AWDL-MDNS bind error=%d", errno);
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    struct ipv6_mreq membership = {
        .ipv6mr_interface = (unsigned int)interface_index,
    };
    (void)inet6_aton("ff02::fb", &membership.ipv6mr_multiaddr);
    bool joined_multicast = false;
    for (unsigned attempt = 1U; attempt <= 4U; ++attempt) {
        if (setsockopt(socket_fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                       &membership, sizeof(membership)) == 0) {
            joined_multicast = true;
            break;
        }
        ESP_LOGW(TAG,
                 "AWDL-MDNS membership attempt=%u error=%d (%s)",
                 attempt, errno, strerror(errno));
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
    if (!joined_multicast) {
        /*
         * The socket wrapper collapses every MLD failure into
         * EADDRNOTAVAIL. Ask MLD directly on the tcpip thread so the lab can
         * distinguish an index-translation failure from a group-pool error.
         */
        awdl_mld_join_t direct_join = {
            .netif = esp_netif_get_netif_impl(awdl_netif),
            .result = ERR_IF,
        };
        (void)ip6addr_aton("ff02::fb", &direct_join.group);
        const err_t callback_result =
            tcpip_callback_wait(join_mld_group, &direct_join);
        ESP_LOGI(TAG,
                 "AWDL-MDNS direct-membership callback=%d mld=%d",
                 callback_result, direct_join.result);
        joined_multicast = callback_result == ERR_OK &&
                           direct_join.result == ERR_OK;
    }
    ESP_LOGI(TAG, "AWDL-MDNS membership=%s",
             joined_multicast ? "joined" : "unicast-response-only");

    struct sockaddr_in6 destination = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(AWDL_MDNS_PORT),
        .sin6_scope_id = (uint32_t)interface_index,
    };
    (void)inet6_aton("ff02::fb", &destination.sin6_addr);
    ESP_LOGI(TAG, "AWDL-MDNS destination=multicast ipv6=ff02::fb port=%u",
             AWDL_MDNS_PORT);

    static uint8_t receiver_announcement[512];
    size_t receiver_announcement_bytes = 0U;
#if CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_LAB
    char service_id[13];
    (void)snprintf(service_id, sizeof(service_id),
                   "%02x%02x%02x%02x%02x%02x",
                   station_mac[0], station_mac[1], station_mac[2],
                   station_mac[3], station_mac[4], station_mac[5]);
    if (!espdrop_airdrop_mdns_build_announcement(
            receiver_announcement, sizeof(receiver_announcement),
            &receiver_announcement_bytes, service_id, "espdrop",
            CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_PORT,
            ESPDROP_AIRDROP_RECEIVER_FLAGS_ANONYMOUS,
            (const uint8_t *)link_local.addr)) {
        ESP_LOGE(TAG, "could not build AirDrop receiver announcement");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG,
             "anonymous AirDrop receiver oracle prepared instance=%s "
             "port=%u flags=%u identity=none",
             service_id, CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_PORT,
             ESPDROP_AIRDROP_RECEIVER_FLAGS_ANONYMOUS);
#endif

    /* Do not enqueue DNS traffic until the selected peer has supplied a valid
     * schedule and the bounded transmitter has drained at least one netif
     * frame. A fixed delay races peer discovery and can fill lwIP's queue. */
    unsigned ready_waits = 0U;
    while (!espdrop_awdl_tx_lab_netif_ready() && ready_waits < 360U) {
        vTaskDelay(pdMS_TO_TICKS(250U));
        ++ready_waits;
    }
    if (!espdrop_awdl_tx_lab_netif_ready()) {
        ESP_LOGE(TAG, "AWDL-MDNS transmit window unavailable");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "AWDL-MDNS transmit window ready wait_ms=%u",
             ready_waits * 250U);

#if CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_LAB
    const ssize_t announced = sendto(
        socket_fd, receiver_announcement, receiver_announcement_bytes, 0,
        (const struct sockaddr *)&destination, sizeof(destination));
    if (announced == (ssize_t)receiver_announcement_bytes) {
        ++stats.airdrop_receiver_announcements;
    }
    ESP_LOGW(TAG,
             "AWDL-AIRDROP-RECEIVER initial-announcement bytes=%d error=%d",
             (int)announced, announced < 0 ? errno : 0);
#endif

    for (unsigned attempt = 1; attempt <= AWDL_MDNS_QUERY_ATTEMPTS;
         ++attempt) {
        const ssize_t sent = send_mdns_question(
            socket_fd, &destination, "_airdrop._tcp.local",
            ESPDROP_MDNS_TYPE_PTR, attempt == 1U);
        ESP_LOGI(TAG, "AWDL-MDNS-QUERY attempt=%u bytes=%d error=%d",
                 attempt, (int)sent, sent < 0 ? errno : 0);

        for (unsigned receive_attempt = 0; receive_attempt < 4U;
             ++receive_attempt) {
            (void)receive_mdns_packet(
                socket_fd, &destination, receiver_announcement,
                receiver_announcement_bytes);
        }
        send_resolution_questions(socket_fd, &destination, attempt);
        for (unsigned receive_attempt = 0; receive_attempt < 4U;
             ++receive_attempt) {
            (void)receive_mdns_packet(
                socket_fd, &destination, receiver_announcement,
                receiver_announcement_bytes);
        }
#if !CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_LAB
        probe_airdrop_tcp(interface_index);
#endif
    }
    /* Keep the joined socket alive beyond the bounded transmit window. Apple
     * peers periodically emit their service cache even without a fresh query. */
    for (;;) {
        (void)receive_mdns_packet(
            socket_fd, &destination, receiver_announcement,
            receiver_announcement_bytes);
#if !CONFIG_ESPDROP_AIRDROP_RECEIVER_ORACLE_LAB
        probe_airdrop_tcp(interface_index);
#endif
    }
    ESP_LOGW(TAG,
             "AWDL-MDNS-SUMMARY queries=%lu packets=%lu responses=%lu "
             "services=%lu complete=%lu tcp_attempts=%lu tcp_connected=%lu",
             (unsigned long)stats.mdns_queries,
             (unsigned long)stats.mdns_packets,
             (unsigned long)stats.mdns_responses,
             (unsigned long)stats.mdns_services,
             (unsigned long)stats.mdns_complete_services,
             (unsigned long)stats.airdrop_tcp_attempts,
             (unsigned long)stats.airdrop_tcp_connected);
    close(socket_fd);
    vTaskDelete(NULL);
}
#endif

esp_err_t espdrop_awdl_netif_init(const uint8_t self_mac[6])
{
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (self_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(station_mac, self_mac, sizeof(station_mac));
    rx_queue = xQueueCreate(AWDL_NETIF_RX_QUEUE_DEPTH,
                            sizeof(awdl_ethernet_frame_t));
#if CONFIG_ESPDROP_AWDL_TX_LAB
    /* Sustained TLS output can enqueue an entire advertised TCP window before
     * the next common AWDL channel. Keep those frames in PSRAM; the radio
     * path copies one frame into internal memory immediately before TX. */
    tx_queue_storage = heap_caps_malloc(
        AWDL_NETIF_TX_QUEUE_DEPTH * sizeof(awdl_ethernet_frame_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tx_queue_storage != NULL) {
        tx_queue = xQueueCreateStatic(
            AWDL_NETIF_TX_QUEUE_DEPTH, sizeof(awdl_ethernet_frame_t),
            tx_queue_storage, &tx_queue_control);
    }
    if (rx_queue == NULL || tx_queue == NULL) {
#else
    if (rx_queue == NULL) {
#endif
        return ESP_ERR_NO_MEM;
    }

    driver.base.post_attach = driver_post_attach;
    esp_netif_inherent_config_t base = {
        .flags = ESP_NETIF_FLAG_AUTOUP | ESP_NETIF_FLAG_MLDV6_REPORT,
        .if_key = "AWDL_DEF",
        .if_desc = "awdl",
        .route_prio = 5,
    };
    memcpy(base.mac, self_mac, sizeof(base.mac));
    const esp_netif_driver_ifconfig_t driver_config = {
        .handle = &driver,
        .transmit = driver_transmit,
        .driver_free_rx_buffer = driver_free_rx,
    };
    const esp_netif_config_t config = {
        .base = &base,
        .driver = &driver_config,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    awdl_netif = esp_netif_new(&config);
    if (awdl_netif == NULL) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_netif_attach(awdl_netif, &driver), TAG,
                        "attach AWDL netif");
    /* The Ethernet stack installs its placeholder 02:00:00:00:00:00 MAC
     * while attaching. Replace it before deriving the IPv6 link-local. */
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(awdl_netif, station_mac), TAG,
                        "set AWDL netif MAC");
    struct netif *lwip_netif = esp_netif_get_netif_impl(awdl_netif);
    if (lwip_netif == NULL) {
        return ESP_FAIL;
    }
    lwip_netif->mtu = AWDL_NETIF_MTU;
#if LWIP_IPV6 && LWIP_ND6_ALLOW_RA_UPDATES
    /* lwIP keeps a distinct IPv6 MTU when RA updates are enabled. The
     * Ethernet attach path seeds it to 1500, so changing only `mtu` leaves
     * TCP with a 1440-byte MSS and produces 1540-byte raw AWDL frames that
     * esp_wifi_80211_tx cannot accept. */
    lwip_netif->mtu6 = AWDL_NETIF_MTU;
#endif
    ESP_RETURN_ON_ERROR(esp_netif_create_ip6_linklocal(awdl_netif), TAG,
                        "create AWDL link-local address");
    /* Drain capture ownership before lower-priority logging and TLS work.
     * tcpip remains responsible for protocol processing after this short
     * handoff. */
    if (xTaskCreate(rx_task, "awdl_netif_rx", 4096, NULL, 19, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_ESPDROP_AWDL_MDNS_LAB
    if (xTaskCreate(mdns_task, "awdl_mdns", AWDL_MDNS_TASK_STACK_BYTES,
                    NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
    initialized = true;
#if CONFIG_ESPDROP_AWDL_TX_LAB
    ESP_LOGI(TAG, "AWDL esp-netif attached; radio TX=bounded-lab");
#else
    ESP_LOGI(TAG, "AWDL esp-netif attached; radio TX=suppressed");
#endif
    return ESP_OK;
}

bool espdrop_awdl_netif_add_peer(const uint8_t peer_mac[6])
{
    if (!initialized || awdl_netif == NULL || peer_mac == NULL) {
        return false;
    }
    struct netif *lwip_netif = esp_netif_get_netif_impl(awdl_netif);
    if (lwip_netif == NULL) {
        ++stats.peer_mapping_failures;
        return false;
    }

    uint8_t address_bytes[16];
    espdrop_awdl_link_local_from_mac(peer_mac, address_bytes);
    awdl_neighbor_mapping_t mapping = {
        .netif = lwip_netif,
        .result = ERR_IF,
    };
    memcpy(&mapping.address, address_bytes, sizeof(address_bytes));
    ip6_addr_assign_zone(&mapping.address, IP6_UNICAST, lwip_netif);
    memcpy(mapping.mac, peer_mac, sizeof(mapping.mac));
    const err_t callback = tcpip_callback_wait(add_awdl_neighbor, &mapping);
    if (callback != ERR_OK || mapping.result != ERR_OK) {
        ++stats.peer_mapping_failures;
        ESP_LOGW(TAG,
                 "AWDL-PEER-MAP peer=%02x:%02x:%02x:%02x:%02x:%02x "
                 "result=failed callback=%d error=%d",
                 peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3],
                 peer_mac[4], peer_mac[5], callback, mapping.result);
        return false;
    }
    if (mapping.created) {
        ++stats.peer_mappings;
        ESP_LOGI(TAG,
                 "AWDL-PEER-MAP peer=%02x:%02x:%02x:%02x:%02x:%02x "
                 "result=created source=mif-rfc4291",
                 peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3],
                 peer_mac[4], peer_mac[5]);
    }
    return true;
}

bool espdrop_awdl_netif_receive(const espdrop_awdl_data_t *data)
{
    if (!initialized || data == NULL || rx_queue == NULL) {
        return false;
    }
    const bool directed_to_self =
        memcmp(data->destination, station_mac, sizeof(station_mac)) == 0;
    const bool multicast = (data->destination[0] & 0x01U) != 0U;
    if (!directed_to_self && !multicast) {
        /* Promiscuous mode sees the entire AWDL cluster. Feeding unrelated
         * unicast traffic into the bounded RX queue can evict the selected
         * receiver's TCP ACK and freeze a sustained upload. */
        ++stats.rx_filtered;
        return false;
    }
    espdrop_awdl_tcp_t tcp;
    if (espdrop_awdl_decode_tcp(data, &tcp)) {
        ++stats.tcp_rx_segments;
        stats.tcp_rx_payload_bytes += tcp.payload_length;
        stats.tcp_rx_last_sequence = tcp.sequence;
        stats.tcp_rx_last_acknowledgment = tcp.acknowledgment;
        stats.tcp_rx_last_window = tcp.window;
        stats.tcp_rx_last_payload_length = tcp.payload_length;
        if ((tcp.flags & 0x12U) == 0x12U) {
            ++stats.tcp_rx_syn_ack;
        }
        if ((tcp.flags & 0x04U) != 0U) {
            ++stats.tcp_rx_rst;
        }
        if ((tcp.flags & 0x01U) != 0U) {
            ++stats.tcp_rx_fin;
        }
        if ((tcp.flags & 0x10U) != 0U && tcp.window == 0U) {
            ++stats.tcp_rx_zero_window;
        }
    }
    size_t length = 0U;
    if (!espdrop_awdl_data_to_ethernet(
            data, promiscuous_rx_scratch.bytes,
            sizeof(promiscuous_rx_scratch.bytes), &length)) {
        ++stats.rx_dropped;
        return false;
    }
    promiscuous_rx_scratch.length = (uint16_t)length;
    if (xQueueSend(rx_queue, &promiscuous_rx_scratch, 0) != pdTRUE) {
        ++stats.rx_dropped;
        return false;
    }
    ++stats.rx_enqueued;
    return true;
}

size_t espdrop_awdl_netif_flush(size_t maximum_frames)
{
#if CONFIG_ESPDROP_AWDL_TX_LAB
    if (!initialized || tx_queue == NULL) {
        return 0U;
    }
    size_t flushed = 0U;
    while (flushed < maximum_frames &&
           xQueueReceive(tx_queue, &radio_tx_scratch, 0) == pdTRUE) {
        espdrop_awdl_data_t ethernet_data = {
            .ethertype = radio_tx_scratch.length >= 14U
                             ? (uint16_t)((uint16_t)radio_tx_scratch.bytes[12]
                                          << 8U) |
                                   radio_tx_scratch.bytes[13]
                             : 0U,
            .payload = radio_tx_scratch.length >= 14U
                           ? radio_tx_scratch.bytes + 14U : NULL,
            .payload_length = radio_tx_scratch.length >= 14U
                                  ? radio_tx_scratch.length - 14U : 0U,
        };
        espdrop_awdl_ipv6_t ipv6 = {0};
        espdrop_awdl_tcp_t tcp = {0};
        const bool is_ipv6 = espdrop_awdl_decode_ipv6(&ethernet_data, &ipv6);
        const bool is_tcp = espdrop_awdl_decode_tcp(&ethernet_data, &tcp);
        if (is_tcp) {
            ++stats.tcp_tx_segments;
            stats.tcp_tx_payload_bytes += tcp.payload_length;
            stats.tcp_tx_last_sequence = tcp.sequence;
            stats.tcp_tx_last_acknowledgment = tcp.acknowledgment;
            stats.tcp_tx_last_window = tcp.window;
            stats.tcp_tx_last_payload_length = tcp.payload_length;
            if ((tcp.flags & 0x02U) != 0U &&
                (tcp.flags & 0x10U) == 0U) {
                ++stats.tcp_tx_syn;
            }
        }
        const unsigned copies = is_tcp && tcp.payload_length != 0U
                                    ? AWDL_NETIF_TCP_PAYLOAD_COPIES
                                    : 1U;
        unsigned accepted_copies = 0U;
        esp_err_t last_result = ESP_FAIL;
        size_t raw_length = 0U;
        for (unsigned copy = 0U; copy < copies; ++copy) {
            const uint16_t marked_sequence =
                (uint16_t)(ESPDROP_AWDL_NETIF_SEQUENCE_MARKER |
                           (awdl_sequence++ & 0x7fffU));
            if (!espdrop_awdl_build_ethernet_frame(
                    radio_raw_scratch, sizeof(radio_raw_scratch), &raw_length,
                    radio_tx_scratch.bytes, radio_tx_scratch.length,
                    station_mac, ieee80211_sequence++, marked_sequence)) {
                ++stats.tx_errors;
                last_result = ESP_ERR_INVALID_SIZE;
                continue;
            }
            ++stats.tx_submitted;
            if (copy != 0U) {
                ++stats.tx_redundant_submitted;
            }
            last_result = esp_wifi_80211_tx(
                WIFI_IF_STA, radio_raw_scratch, (int)raw_length, false);
            if (last_result == ESP_OK) {
                ++stats.tx_accepted;
                ++accepted_copies;
            } else {
                ++stats.tx_errors;
            }
        }
        ++flushed;
        ESP_LOGI(TAG,
                 "AWDL-NETIF-TX bytes=%u ethertype=0x%02x%02x next=%u "
                 "tcp_src=%u tcp_dst=%u tcp_flags=0x%02x tcp_seq=%lu "
                 "tcp_checksum=%u copies=%u accepted=%u driver=%s count=%lu",
                 (unsigned)raw_length, radio_tx_scratch.bytes[12],
                 radio_tx_scratch.bytes[13], is_ipv6 ? ipv6.next_header : 0U,
                 tcp.source_port, tcp.destination_port, tcp.flags,
                 (unsigned long)tcp.sequence,
                 is_tcp && tcp.checksum_valid ? 1U : 0U,
                 copies, accepted_copies, esp_err_to_name(last_result),
                 (unsigned long)stats.tx_submitted);
    }
    return flushed;
#else
    (void)maximum_frames;
    return 0U;
#endif
}

void espdrop_awdl_netif_note_tx_done(
    bool success,
    const uint8_t *frame,
    size_t length)
{
    if (success) {
        ++stats.tx_radio_success;
    } else {
        ++stats.tx_radio_failed;
    }
    espdrop_awdl_data_t data;
    espdrop_awdl_tcp_t tcp;
    const bool decoded_tcp =
        espdrop_awdl_decode_data(frame, length, &data) &&
        espdrop_awdl_decode_tcp(&data, &tcp);
    /* ESP-IDF's TX callback may return a driver-normalized header. Keep a
     * bounded fallback for espDrop's fixed non-QoS AWDL layout: IPv6 begins
     * at byte 40 and its Next Header byte is at 46. */
    bool normalized_layout_tcp = false;
    if (frame != NULL) {
        const size_t scan_limit = length < 64U ? length : 64U;
        for (size_t offset = 24U; offset + 9U < scan_limit; ++offset) {
            if (frame[offset] == 0x86U && frame[offset + 1U] == 0xddU &&
                (frame[offset + 2U] >> 4U) == 6U &&
                frame[offset + 8U] == 6U) {
                normalized_layout_tcp = true;
                break;
            }
        }
    }
    if (decoded_tcp || normalized_layout_tcp) {
        if (success) {
            ++stats.tcp_tx_radio_success;
        } else {
            ++stats.tcp_tx_radio_failed;
        }
    }
}

espdrop_awdl_netif_stats_t espdrop_awdl_netif_stats(void)
{
    return stats;
}

esp_netif_t *espdrop_awdl_netif_handle(void)
{
    return awdl_netif;
}
