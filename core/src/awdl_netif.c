#include "espdrop/awdl_netif.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/mld6.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

#define AWDL_NETIF_QUEUE_DEPTH 8U
#define AWDL_NETIF_MAX_ETHERNET_BYTES 1474U
#define AWDL_NETIF_RAW_FRAME_BYTES 1500U
#define AWDL_NETIF_MTU 1460U
#define AWDL_MDNS_PORT 5353U
#define AWDL_MDNS_QUERY_ATTEMPTS 6U

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
static awdl_ethernet_frame_t driver_tx_scratch;
static awdl_ethernet_frame_t radio_tx_scratch;
static uint8_t radio_raw_scratch[AWDL_NETIF_RAW_FRAME_BYTES];
static uint16_t ieee80211_sequence = 0x700U;
static uint16_t awdl_sequence;
#endif
static espdrop_awdl_netif_stats_t stats;
static bool initialized;

static esp_err_t driver_transmit(void *handle, void *buffer, size_t length)
{
    (void)handle;
    ++stats.tx_observed;
    if (buffer == NULL || length < ESPDROP_ETHERNET_HEADER_BYTES ||
        length > AWDL_NETIF_MAX_ETHERNET_BYTES) {
        ++stats.tx_dropped;
        return ESP_ERR_INVALID_SIZE;
    }
#if CONFIG_ESPDROP_AWDL_TX_LAB
    driver_tx_scratch.length = (uint16_t)length;
    memcpy(driver_tx_scratch.bytes, buffer, length);
    if (tx_queue == NULL ||
        xQueueSend(tx_queue, &driver_tx_scratch, 0) != pdTRUE) {
        ++stats.tx_dropped;
        return ESP_ERR_NO_MEM;
    }
    ++stats.tx_enqueued;
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

static bool receive_mdns_packet(int socket_fd)
{
    static uint8_t packet[768];
    const ssize_t received = recvfrom(socket_fd, packet, sizeof(packet), 0,
                                      NULL, NULL);
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
    return true;
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

    static const uint8_t query[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x08, '_', 'a', 'i', 'r', 'd', 'r', 'o', 'p',
        0x04, '_', 't', 'c', 'p',
        0x05, 'l', 'o', 'c', 'a', 'l', 0x00,
        /* PTR with the QU bit requests a unicast response to this socket. */
        0x00, 0x0c, 0x80, 0x01,
    };
    struct sockaddr_in6 destination = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(AWDL_MDNS_PORT),
        .sin6_scope_id = (uint32_t)interface_index,
    };
    (void)inet6_aton("ff02::fb", &destination.sin6_addr);

    for (unsigned attempt = 1; attempt <= AWDL_MDNS_QUERY_ATTEMPTS;
         ++attempt) {
        const ssize_t sent = sendto(socket_fd, query, sizeof(query), 0,
                                    (struct sockaddr *)&destination,
                                    sizeof(destination));
        if (sent == (ssize_t)sizeof(query)) {
            ++stats.mdns_queries;
        }
        ESP_LOGI(TAG, "AWDL-MDNS-QUERY attempt=%u bytes=%d error=%d",
                 attempt, (int)sent, sent < 0 ? errno : 0);

        for (unsigned receive_attempt = 0; receive_attempt < 5U;
             ++receive_attempt) {
            (void)receive_mdns_packet(socket_fd);
        }
    }
    /* Keep the joined socket alive beyond the bounded transmit window. Apple
     * peers periodically emit their service cache even without a fresh query. */
    for (unsigned receive_attempt = 0; receive_attempt < 60U;
         ++receive_attempt) {
        (void)receive_mdns_packet(socket_fd);
    }
    ESP_LOGW(TAG,
             "AWDL-MDNS-SUMMARY queries=%lu packets=%lu responses=%lu",
             (unsigned long)stats.mdns_queries,
             (unsigned long)stats.mdns_packets,
             (unsigned long)stats.mdns_responses);
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
    rx_queue = xQueueCreate(AWDL_NETIF_QUEUE_DEPTH,
                            sizeof(awdl_ethernet_frame_t));
#if CONFIG_ESPDROP_AWDL_TX_LAB
    tx_queue = xQueueCreate(AWDL_NETIF_QUEUE_DEPTH,
                            sizeof(awdl_ethernet_frame_t));
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
    ESP_RETURN_ON_ERROR(esp_netif_create_ip6_linklocal(awdl_netif), TAG,
                        "create AWDL link-local address");
    if (xTaskCreate(rx_task, "awdl_netif_rx", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_ESPDROP_AWDL_MDNS_LAB
    if (xTaskCreate(mdns_task, "awdl_mdns", 5120, NULL, 5, NULL) != pdPASS) {
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

bool espdrop_awdl_netif_receive(const espdrop_awdl_data_t *data)
{
    if (!initialized || data == NULL || rx_queue == NULL) {
        return false;
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
        size_t raw_length = 0U;
        const uint16_t marked_sequence =
            (uint16_t)(ESPDROP_AWDL_NETIF_SEQUENCE_MARKER |
                       (awdl_sequence++ & 0x7fffU));
        if (!espdrop_awdl_build_ethernet_frame(
                radio_raw_scratch, sizeof(radio_raw_scratch), &raw_length,
                radio_tx_scratch.bytes, radio_tx_scratch.length,
                station_mac, ieee80211_sequence++,
                marked_sequence)) {
            ++stats.tx_errors;
            continue;
        }
        ++stats.tx_submitted;
        const esp_err_t result =
            esp_wifi_80211_tx(WIFI_IF_STA, radio_raw_scratch,
                              (int)raw_length, false);
        if (result == ESP_OK) {
            ++stats.tx_accepted;
        } else {
            ++stats.tx_errors;
        }
        ++flushed;
        ESP_LOGI(TAG,
                 "AWDL-NETIF-TX bytes=%u ethertype=0x%02x%02x driver=%s count=%lu",
                 (unsigned)raw_length, radio_tx_scratch.bytes[12],
                 radio_tx_scratch.bytes[13],
                 esp_err_to_name(result), (unsigned long)stats.tx_submitted);
    }
    return flushed;
#else
    (void)maximum_frames;
    return 0U;
#endif
}

void espdrop_awdl_netif_note_tx_done(bool success)
{
    if (success) {
        ++stats.tx_radio_success;
    } else {
        ++stats.tx_radio_failed;
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
