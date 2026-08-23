#include "espdrop/awdl_probe.h"
#include "espdrop/awdl_frame.h"
#include "espdrop/awdl_data.h"
#include "espdrop/awdl_netif.h"
#include "espdrop/awdl_tlv.h"
#include "espdrop/awdl_tx_lab.h"
#include "espdrop/espdrop.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    uint8_t source[6];
    uint8_t destination[6];
    uint8_t bssid[6];
    uint8_t oui[3];
    uint8_t category;
    uint8_t action_type;
    uint8_t version;
    uint8_t subtype;
    int8_t rssi;
    uint8_t channel;
    uint32_t timestamp_us;
    uint64_t received_at_us;
    bool sampled_action;
    bool decoded_awdl;
    bool directed_to_self;
} awdl_probe_record_t;

#define AWDL_CAPTURE_BYTES 1536U
#define AWDL_CAPTURE_PEERS 8U
#define AWDL_DATA_CAPTURE_BYTES 96U
#define AWDL_DATA_CAPTURE_LIMIT 8U

typedef struct {
    uint8_t source[6];
    uint16_t frame_length;
    uint16_t captured_length;
    uint64_t received_at_us;
    uint8_t frame[AWDL_CAPTURE_BYTES];
} awdl_mif_capture_t;

typedef struct {
    uint8_t source[6];
    uint8_t destination[6];
    int8_t rssi;
    uint8_t channel;
    uint32_t timestamp_us;
    uint16_t frame_length;
    uint16_t awdl_sequence;
    uint16_t ethertype;
    uint8_t next_header;
    uint8_t hop_limit;
    uint8_t icmp_type;
    uint16_t icmp_identifier;
    uint16_t icmp_sequence;
    bool qos;
    bool amsdu;
    bool ipv6;
    bool directed_to_self;
} awdl_data_record_t;

typedef struct {
    uint8_t source[6];
    uint8_t destination[6];
    uint8_t bssid[6];
    uint16_t frame_control;
    uint16_t frame_length;
    uint16_t captured_length;
    uint8_t decode_result;
    uint8_t frame[AWDL_DATA_CAPTURE_BYTES];
} awdl_data_capture_t;

static const char *TAG = "awdl_probe";
static QueueHandle_t record_queue;
static QueueHandle_t capture_queue;
static QueueHandle_t data_queue;
static QueueHandle_t data_capture_queue;
static espdrop_awdl_probe_stats_t stats;
static bool started;
static volatile uint32_t management_frames;
static volatile uint32_t action_candidates;
static volatile uint32_t awdl_bssid_matches;
static volatile uint32_t vendor_action_frames;
static volatile uint32_t apple_oui_matches;
static volatile uint32_t awdl_header_matches;
static volatile uint32_t decoded_frames;
static volatile uint32_t data_frames;
static volatile uint32_t data_frames_from_self;
static volatile uint32_t data_frames_to_self;
static volatile uint32_t data_frames_awdl_bssid;
static volatile uint32_t sampled_data_candidates;
static volatile uint32_t decoded_data_frames;
static volatile uint32_t ipv6_data_frames;
static volatile uint32_t neighbor_advertisements;
static volatile uint32_t echo_replies;
static uint8_t sampled_mif_sources[AWDL_CAPTURE_PEERS][6];
static size_t sampled_mif_source_count;
static uint8_t detailed_mif_sources[AWDL_CAPTURE_PEERS][6];
static size_t detailed_mif_source_count;
static uint8_t station_mac[6];

static const uint8_t diagnostic_apple_oui[3] = {0x00, 0x17, 0xf2};
static const uint8_t diagnostic_awdl_bssid[6] = {
    0x00, 0x25, 0x00, 0xff, 0x94, 0x73,
};

static uint16_t read_be16(const uint8_t *value)
{
    return (uint16_t)((uint16_t)value[0] << 8U) | value[1];
}

static void promiscuous_data_rx(const wifi_promiscuous_pkt_t *packet)
{
    ++data_frames;
    const uint8_t *frame = packet->payload;
    const size_t received_length = packet->rx_ctrl.sig_len;
    const size_t length = received_length >= 4U
                              ? received_length - 4U : received_length;
    const bool has_header = length >= 24U;
    const bool from_self =
        has_header && memcmp(frame + 10, station_mac, 6U) == 0;
    const bool to_self =
        has_header && memcmp(frame + 4, station_mac, 6U) == 0;
    const bool awdl_bssid =
        has_header && memcmp(frame + 16, diagnostic_awdl_bssid, 6U) == 0;
    if (from_self) {
        ++data_frames_from_self;
    }
    if (to_self) {
        ++data_frames_to_self;
    }
    if (awdl_bssid) {
        ++data_frames_awdl_bssid;
    }

    espdrop_awdl_data_t data;
    const espdrop_awdl_data_decode_result_t decode_result =
        espdrop_awdl_decode_data_ex(frame, length, &data);
    if (decode_result != ESPDROP_AWDL_DATA_DECODE_OK) {
        if (!from_self && (to_self || awdl_bssid) &&
            sampled_data_candidates < AWDL_DATA_CAPTURE_LIMIT &&
            data_capture_queue != NULL) {
            awdl_data_capture_t capture = {
                .frame_control = (uint16_t)frame[0] |
                                 ((uint16_t)frame[1] << 8U),
                .frame_length = (uint16_t)(length > UINT16_MAX
                                               ? UINT16_MAX : length),
                .captured_length = (uint16_t)(
                    length > AWDL_DATA_CAPTURE_BYTES
                        ? AWDL_DATA_CAPTURE_BYTES : length),
                .decode_result = (uint8_t)decode_result,
            };
            memcpy(capture.destination, frame + 4, 6U);
            memcpy(capture.source, frame + 10, 6U);
            memcpy(capture.bssid, frame + 16, 6U);
            memcpy(capture.frame, frame, capture.captured_length);
            ++sampled_data_candidates;
            if (xQueueSend(data_capture_queue, &capture, 0) != pdTRUE) {
                ++stats.dropped_records;
            }
        }
        return;
    }
    if (memcmp(data.source, station_mac, sizeof(station_mac)) == 0) {
        return;
    }

    ++decoded_data_frames;
    (void)espdrop_awdl_netif_receive(&data);
    awdl_data_record_t record = {
        .rssi = packet->rx_ctrl.rssi,
        .channel = packet->rx_ctrl.channel,
        .timestamp_us = packet->rx_ctrl.timestamp,
        .frame_length = (uint16_t)(length > UINT16_MAX
                                       ? UINT16_MAX : length),
        .awdl_sequence = data.sequence,
        .ethertype = data.ethertype,
        .qos = data.qos,
        .amsdu = data.amsdu,
    };
    memcpy(record.source, data.source, sizeof(record.source));
    memcpy(record.destination, data.destination, sizeof(record.destination));
    record.directed_to_self =
        memcmp(record.destination, station_mac, sizeof(station_mac)) == 0;

    espdrop_awdl_ipv6_t ipv6;
    if (espdrop_awdl_decode_ipv6(&data, &ipv6)) {
        record.ipv6 = true;
        record.next_header = ipv6.next_header;
        record.hop_limit = ipv6.hop_limit;
        record.icmp_type = ipv6.icmp_type;
        ++ipv6_data_frames;
        if (record.directed_to_self && ipv6.next_header == 58U &&
            ipv6.icmp_type == 136U) {
            ++neighbor_advertisements;
        } else if (record.directed_to_self && ipv6.next_header == 58U &&
                   ipv6.icmp_type == 129U &&
                   ipv6.icmp_payload_length >= 4U) {
            record.icmp_identifier = read_be16(ipv6.icmp_payload);
            record.icmp_sequence = read_be16(ipv6.icmp_payload + 2U);
            ++echo_replies;
        }
    }
    if (xQueueSend(data_queue, &record, 0) != pdTRUE) {
        ++stats.dropped_records;
    }
}

static bool should_capture_mif(const uint8_t source[6])
{
    for (size_t index = 0; index < sampled_mif_source_count; ++index) {
        if (memcmp(sampled_mif_sources[index], source, 6) == 0) {
            return espdrop_awdl_tx_lab_wants_mif(source);
        }
    }
    if (sampled_mif_source_count >= AWDL_CAPTURE_PEERS) {
        return false;
    }
    memcpy(sampled_mif_sources[sampled_mif_source_count], source, 6);
    ++sampled_mif_source_count;
    return true;
}

static void promiscuous_rx(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (record_queue == NULL || data_queue == NULL) {
        return;
    }
    const wifi_promiscuous_pkt_t *packet =
        (const wifi_promiscuous_pkt_t *)buffer;
    if (type == WIFI_PKT_DATA) {
        promiscuous_data_rx(packet);
        return;
    }
    if (type != WIFI_PKT_MGMT) {
        return;
    }
    ++management_frames;
    const uint8_t *frame = packet->payload;
    const size_t received_length = packet->rx_ctrl.sig_len;
    const size_t length = received_length >= 4U
                              ? received_length - 4U
                              : received_length;
    if (length < 24U || (frame[0] & 0xfcU) != 0xd0U) {
        return;
    }

    const uint32_t candidate_number = ++action_candidates;
    awdl_probe_record_t record = {
        .rssi = packet->rx_ctrl.rssi,
        .channel = packet->rx_ctrl.channel,
        .timestamp_us = packet->rx_ctrl.timestamp,
        .received_at_us = (uint64_t)esp_timer_get_time(),
        .sampled_action = candidate_number <= 10U,
    };
    memcpy(record.source, frame + 10, sizeof(record.source));
    memcpy(record.destination, frame + 4, sizeof(record.destination));
    memcpy(record.bssid, frame + 16, sizeof(record.bssid));
    record.directed_to_self =
        memcmp(record.destination, station_mac, sizeof(station_mac)) == 0;

    if (memcmp(record.bssid, diagnostic_awdl_bssid,
               sizeof(diagnostic_awdl_bssid)) == 0) {
        ++awdl_bssid_matches;
    }
    if (length >= 31U) {
        const uint8_t *body = frame + 24;
        record.category = body[0];
        memcpy(record.oui, body + 1, sizeof(record.oui));
        record.action_type = body[4];
        record.version = body[5];
        record.subtype = body[6];
        if (body[0] == 127U) {
            ++vendor_action_frames;
        }
        if (memcmp(body + 1, diagnostic_apple_oui,
                   sizeof(diagnostic_apple_oui)) == 0) {
            ++apple_oui_matches;
        }
        if (body[0] == 127U &&
            memcmp(body + 1, diagnostic_apple_oui,
                   sizeof(diagnostic_apple_oui)) == 0 &&
            body[4] == 8U && body[5] == 0x10U) {
            ++awdl_header_matches;
        }
    }

    espdrop_awdl_action_t action;
    if (espdrop_awdl_decode_action(frame, length, &action)) {
        if (memcmp(action.source, station_mac, sizeof(station_mac)) == 0) {
            return;
        }
        record.decoded_awdl = true;
        record.subtype = (uint8_t)action.subtype;
        ++decoded_frames;
        if (action.subtype == ESPDROP_AWDL_ACTION_MIF &&
            capture_queue != NULL && should_capture_mif(action.source)) {
            awdl_mif_capture_t capture = {
                .frame_length = (uint16_t)(length > UINT16_MAX
                                               ? UINT16_MAX : length),
                .captured_length = (uint16_t)(length > AWDL_CAPTURE_BYTES
                                                  ? AWDL_CAPTURE_BYTES
                                                  : length),
                .received_at_us = (uint64_t)esp_timer_get_time(),
            };
            memcpy(capture.source, action.source, sizeof(capture.source));
            memcpy(capture.frame, frame, capture.captured_length);
            if (xQueueSend(capture_queue, &capture, 0) != pdTRUE) {
                ++stats.dropped_records;
            }
        }
    }

    if (!record.sampled_action && !record.decoded_awdl) {
        return;
    }
    if (xQueueSend(record_queue, &record, 0) != pdTRUE) {
        ++stats.dropped_records;
    }
}

static espdrop_peer_t *observe_awdl_peer(const awdl_probe_record_t *record)
{
    espdrop_peer_table_t *table = espdrop_peers();
    if (table == NULL) {
        return NULL;
    }
    espdrop_peer_observation_t observation = {
        .id = {.length = 6},
        .signals = ESPDROP_PEER_SIGNAL_AWDL,
        .rssi = record->rssi,
        .seen_ms = record->timestamp_us / 1000U,
        .awdl_mac = record->source,
    };
    memcpy(observation.id.bytes, record->source, 6);
    espdrop_peer_t *peer = NULL;
    if (espdrop_peer_table_observe(table, &observation, &peer) !=
        ESPDROP_TABLE_OK) {
        return NULL;
    }
    return peer;
}

static void apply_mif_to_peer(
    const uint8_t source[6],
    const espdrop_awdl_action_t *action,
    const espdrop_awdl_mif_t *mif)
{
    espdrop_peer_table_t *table = espdrop_peers();
    if (table == NULL) {
        return;
    }
    espdrop_peer_id_t id = {.length = 6};
    memcpy(id.bytes, source, 6);
    espdrop_peer_t *peer = NULL;
    for (size_t index = 0; index < table->count; ++index) {
        if (espdrop_peer_id_equal(&table->peers[index].id, &id)) {
            peer = &table->peers[index];
            break;
        }
    }
    if (peer == NULL) {
        return;
    }

    peer->awdl.mif_seen = true;
    peer->awdl.protocol_version = action->version;
    if (mif->has_sync) {
        memcpy(peer->awdl.master, mif->sync.master, 6);
        peer->awdl.master_channel = mif->sync.master_channel;
        peer->awdl.presence_mode = mif->sync.presence_mode;
        peer->awdl.aw_period_tu = mif->sync.aw_period_tu;
        peer->awdl.tx_down_counter_tu = mif->sync.tx_down_counter;
        peer->awdl.next_aw_sequence = mif->sync.next_aw_sequence;
    }
    if (mif->has_election_v2) {
        memcpy(peer->awdl.master, mif->election_v2.master, 6);
        memcpy(peer->awdl.sync_master, mif->election_v2.sync_master, 6);
        peer->awdl.distance_to_master = mif->election_v2.distance_to_master;
        peer->awdl.master_metric = mif->election_v2.master_metric;
        peer->awdl.self_metric = mif->election_v2.self_metric;
        peer->awdl.master_counter = mif->election_v2.master_counter;
        peer->awdl.self_counter = mif->election_v2.self_counter;
    } else if (mif->has_election_v1) {
        memcpy(peer->awdl.master, mif->election_v1.master, 6);
        peer->awdl.distance_to_master = mif->election_v1.distance_to_master;
        peer->awdl.master_metric = mif->election_v1.master_metric;
        peer->awdl.self_metric = mif->election_v1.self_metric;
    }

    const espdrop_awdl_channel_sequence_t *sequence = NULL;
    if (mif->has_channel_sequence) {
        sequence = &mif->channel_sequence;
    } else if (mif->has_sync &&
               mif->sync.has_embedded_channel_sequence) {
        sequence = &mif->sync.embedded_channel_sequence;
    }
    if (sequence != NULL) {
        peer->awdl.channel_count = sequence->count;
        memcpy(peer->awdl.channels, sequence->channels,
               sizeof(peer->awdl.channels));
        memcpy(peer->awdl.operating_classes, sequence->operating_classes,
               sizeof(peer->awdl.operating_classes));
    }
}

static void log_channel_sequence(
    const uint8_t source[6],
    const espdrop_awdl_channel_sequence_t *sequence)
{
    char channels[64];
    size_t used = 0;
    channels[0] = '\0';
    for (size_t index = 0; index < sequence->count; ++index) {
        const int written = snprintf(
            channels + used, sizeof(channels) - used,
            index == 0 ? "%u" : ",%u", sequence->channels[index]);
        if (written < 0 || (size_t)written >= sizeof(channels) - used) {
            break;
        }
        used += (size_t)written;
    }
    ESP_LOGI(TAG,
             "MIF-CHANSEQ src=%02x:%02x:%02x:%02x:%02x:%02x "
             "count=%u encoding=%u duplicate=%u step=%u fill=%u channels=%s",
             source[0], source[1], source[2], source[3], source[4], source[5],
             sequence->count, sequence->encoding, sequence->duplicate_count,
             sequence->step_count, sequence->fill_channel, channels);
}

static void log_raw_capture(const awdl_mif_capture_t *capture)
{
    for (size_t offset = 0; offset < capture->captured_length; offset += 32U) {
        const size_t chunk_length =
            capture->captured_length - offset > 32U
                ? 32U : capture->captured_length - offset;
        char hex[65];
        for (size_t index = 0; index < chunk_length; ++index) {
            (void)snprintf(hex + index * 2U, sizeof(hex) - index * 2U,
                           "%02x", capture->frame[offset + index]);
        }
        ESP_LOGI(TAG,
                 "MIF-RAW src=%02x:%02x:%02x:%02x:%02x:%02x "
                 "offset=%u frame=%u captured=%u data=%s",
                 capture->source[0], capture->source[1], capture->source[2],
                 capture->source[3], capture->source[4], capture->source[5],
                 (unsigned)offset, (unsigned)capture->frame_length,
                 (unsigned)capture->captured_length, hex);
    }
}

static bool should_log_mif_detail(const uint8_t source[6])
{
    for (size_t index = 0U; index < detailed_mif_source_count; ++index) {
        if (memcmp(detailed_mif_sources[index], source, 6U) == 0) {
            return false;
        }
    }
    if (detailed_mif_source_count >= AWDL_CAPTURE_PEERS) {
        return false;
    }
    memcpy(detailed_mif_sources[detailed_mif_source_count], source, 6U);
    ++detailed_mif_source_count;
    return true;
}

static void process_mif_capture(const awdl_mif_capture_t *capture)
{
    espdrop_awdl_action_t action;
    espdrop_awdl_mif_t mif;
    espdrop_awdl_parse_result_t result = ESPDROP_AWDL_PARSE_TRUNCATED;
    if (espdrop_awdl_decode_action(capture->frame,
                                   capture->captured_length, &action)) {
        result = espdrop_awdl_parse_mif(&action, &mif);
    }
    const bool detailed = should_log_mif_detail(capture->source);
    if (detailed || result != ESPDROP_AWDL_PARSE_OK) {
        ESP_LOGI(TAG,
                 "MIF-PARSE src=%02x:%02x:%02x:%02x:%02x:%02x result=%d "
                 "tlvs=%u sync=%u election1=%u election2=%u chanseq=%u",
                 capture->source[0], capture->source[1], capture->source[2],
                 capture->source[3], capture->source[4], capture->source[5],
                 result,
                 result == ESPDROP_AWDL_PARSE_OK ? mif.tlv_count : 0U,
                 result == ESPDROP_AWDL_PARSE_OK && mif.has_sync,
                 result == ESPDROP_AWDL_PARSE_OK && mif.has_election_v1,
                 result == ESPDROP_AWDL_PARSE_OK && mif.has_election_v2,
                 result == ESPDROP_AWDL_PARSE_OK &&
                     mif.has_channel_sequence);
    }
    if (result != ESPDROP_AWDL_PARSE_OK) {
        log_raw_capture(capture);
        return;
    }
    espdrop_awdl_tx_lab_observe_mif(&action, &mif,
                                    capture->received_at_us);
    apply_mif_to_peer(capture->source, &action, &mif);
    if (!detailed) {
        return;
    }
    if (mif.has_sync) {
        ESP_LOGI(TAG,
                 "MIF-SYNC src=%02x:%02x:%02x:%02x:%02x:%02x "
                 "aw_period=%u af_period=%u presence=%u tx_down=%u "
                 "next_aw=%u master_channel=%u master=%02x:%02x:%02x:%02x:%02x:%02x",
                 capture->source[0], capture->source[1], capture->source[2],
                 capture->source[3], capture->source[4], capture->source[5],
                 mif.sync.aw_period_tu, mif.sync.action_frame_period_tu,
                 mif.sync.presence_mode, mif.sync.tx_down_counter,
                 mif.sync.next_aw_sequence, mif.sync.master_channel,
                 mif.sync.master[0], mif.sync.master[1], mif.sync.master[2],
                 mif.sync.master[3], mif.sync.master[4], mif.sync.master[5]);
    }
    if (mif.has_election_v2) {
        ESP_LOGI(TAG,
                 "MIF-ELECTION src=%02x:%02x:%02x:%02x:%02x:%02x "
                 "version=2 distance=%lu master_metric=%lu self_metric=%lu "
                 "master_counter=%lu self_counter=%lu",
                 capture->source[0], capture->source[1], capture->source[2],
                 capture->source[3], capture->source[4], capture->source[5],
                 (unsigned long)mif.election_v2.distance_to_master,
                 (unsigned long)mif.election_v2.master_metric,
                 (unsigned long)mif.election_v2.self_metric,
                 (unsigned long)mif.election_v2.master_counter,
                 (unsigned long)mif.election_v2.self_counter);
    }
    if (mif.has_channel_sequence) {
        log_channel_sequence(capture->source, &mif.channel_sequence);
    } else if (mif.has_sync && mif.sync.has_embedded_channel_sequence) {
        log_channel_sequence(capture->source,
                             &mif.sync.embedded_channel_sequence);
    }
    log_raw_capture(capture);
}

static void log_raw_data_capture(const awdl_data_capture_t *capture)
{
    char hex[AWDL_DATA_CAPTURE_BYTES * 2U + 1U];
    for (size_t index = 0; index < capture->captured_length; ++index) {
        (void)snprintf(hex + index * 2U, sizeof(hex) - index * 2U,
                       "%02x", capture->frame[index]);
    }
    hex[capture->captured_length * 2U] = '\0';
    ESP_LOGI(TAG,
             "DATA-RAW result=%u fc=0x%04x "
             "src=%02x:%02x:%02x:%02x:%02x:%02x "
             "dst=%02x:%02x:%02x:%02x:%02x:%02x "
             "bssid=%02x:%02x:%02x:%02x:%02x:%02x "
             "frame=%u captured=%u data=%s",
             capture->decode_result, capture->frame_control,
             capture->source[0], capture->source[1], capture->source[2],
             capture->source[3], capture->source[4], capture->source[5],
             capture->destination[0], capture->destination[1],
             capture->destination[2], capture->destination[3],
             capture->destination[4], capture->destination[5],
             capture->bssid[0], capture->bssid[1], capture->bssid[2],
             capture->bssid[3], capture->bssid[4], capture->bssid[5],
             capture->frame_length, capture->captured_length, hex);
}

static void probe_log_task(void *argument)
{
    (void)argument;
    awdl_probe_record_t record;
    TickType_t last_diagnostic = xTaskGetTickCount();
    while (true) {
        if (xQueueReceive(record_queue, &record, pdMS_TO_TICKS(1000)) ==
            pdTRUE) {
            if (record.sampled_action) {
                ESP_LOGI(TAG,
                         "ACTION src=%02x:%02x:%02x:%02x:%02x:%02x "
                         "bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d "
                         "channel=%u cat=%u oui=%02x:%02x:%02x type=%u "
                         "version=%u.%u(0x%02x) subtype=%u decoded=%u",
                         record.source[0], record.source[1], record.source[2],
                         record.source[3], record.source[4], record.source[5],
                         record.bssid[0], record.bssid[1], record.bssid[2],
                         record.bssid[3], record.bssid[4], record.bssid[5],
                         record.rssi, record.channel, record.category,
                         record.oui[0], record.oui[1], record.oui[2],
                         record.action_type, record.version >> 4U,
                         record.version & 0x0fU, record.version, record.subtype,
                         record.decoded_awdl ? 1U : 0U);
            }
            if (record.decoded_awdl) {
                espdrop_awdl_tx_lab_note_peer_seen(
                    record.source, record.received_at_us);
                if (record.directed_to_self) {
                    espdrop_awdl_tx_lab_note_directed_peer(record.source);
                }
                (void)observe_awdl_peer(&record);
                ++stats.action_frames;
                if (record.subtype == ESPDROP_AWDL_ACTION_MIF) {
                    ++stats.master_indication_frames;
                } else {
                    ++stats.periodic_sync_frames;
                }
                if (stats.action_frames <= 10U ||
                    stats.action_frames % 50U == 0U) {
                    ESP_LOGI(TAG,
                             "AWDL %s src=%02x:%02x:%02x:%02x:%02x:%02x "
                             "rssi=%d channel=%u ts=%lu count=%lu",
                             record.subtype == ESPDROP_AWDL_ACTION_MIF
                                 ? "MIF" : "PSF",
                             record.source[0], record.source[1],
                             record.source[2], record.source[3],
                             record.source[4], record.source[5], record.rssi,
                             record.channel, (unsigned long)record.timestamp_us,
                             (unsigned long)stats.action_frames);
                }
            }
        }

        awdl_mif_capture_t capture;
        while (capture_queue != NULL &&
               xQueueReceive(capture_queue, &capture, 0) == pdTRUE) {
            process_mif_capture(&capture);
        }

        awdl_data_record_t data_record;
        while (data_queue != NULL &&
               xQueueReceive(data_queue, &data_record, 0) == pdTRUE) {
            ESP_LOGI(TAG,
                     "AWDL-DATA src=%02x:%02x:%02x:%02x:%02x:%02x "
                     "dst=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d "
                     "channel=%u bytes=%u seq=%u qos=%u amsdu=%u "
                     "ethertype=0x%04x ipv6=%u next=%u hop=%u icmp=%u "
                     "directed=%u",
                     data_record.source[0], data_record.source[1],
                     data_record.source[2], data_record.source[3],
                     data_record.source[4], data_record.source[5],
                     data_record.destination[0], data_record.destination[1],
                     data_record.destination[2], data_record.destination[3],
                     data_record.destination[4], data_record.destination[5],
                     data_record.rssi, data_record.channel,
                     data_record.frame_length, data_record.awdl_sequence,
                     data_record.qos ? 1U : 0U,
                     data_record.amsdu ? 1U : 0U,
                     data_record.ethertype, data_record.ipv6 ? 1U : 0U,
                     data_record.next_header, data_record.hop_limit,
                     data_record.icmp_type,
                     data_record.directed_to_self ? 1U : 0U);
            if (data_record.directed_to_self && data_record.ipv6 &&
                data_record.next_header == 58U &&
                data_record.icmp_type == 136U) {
                espdrop_awdl_tx_lab_note_neighbor_advertisement(
                    data_record.source);
            } else if (data_record.directed_to_self && data_record.ipv6 &&
                       data_record.next_header == 58U &&
                       data_record.icmp_type == 129U) {
                espdrop_awdl_tx_lab_note_echo_reply(
                    data_record.source, data_record.icmp_identifier,
                    data_record.icmp_sequence);
            }
        }

        awdl_data_capture_t data_capture;
        while (data_capture_queue != NULL &&
               xQueueReceive(data_capture_queue, &data_capture, 0) == pdTRUE) {
            log_raw_data_capture(&data_capture);
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_diagnostic >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG,
                     "AWDL-DIAG mgmt=%lu action=%lu bssid=%lu vendor=%lu "
                     "apple=%lu header=%lu decoded=%lu dropped=%lu",
                     (unsigned long)management_frames,
                     (unsigned long)action_candidates,
                     (unsigned long)awdl_bssid_matches,
                     (unsigned long)vendor_action_frames,
                     (unsigned long)apple_oui_matches,
                     (unsigned long)awdl_header_matches,
                     (unsigned long)decoded_frames,
                     (unsigned long)stats.dropped_records);
            ESP_LOGI(TAG,
                     "AWDL-DATA-DIAG raw=%lu self_src=%lu self_dst=%lu "
                     "awdl_bssid=%lu sampled=%lu decoded=%lu ipv6=%lu "
                     "na=%lu echo_reply=%lu",
                     (unsigned long)data_frames,
                     (unsigned long)data_frames_from_self,
                     (unsigned long)data_frames_to_self,
                     (unsigned long)data_frames_awdl_bssid,
                     (unsigned long)sampled_data_candidates,
                     (unsigned long)decoded_data_frames,
                     (unsigned long)ipv6_data_frames,
                     (unsigned long)neighbor_advertisements,
                     (unsigned long)echo_replies);
            last_diagnostic = now;
        }
    }
}

esp_err_t espdrop_awdl_probe_start(uint8_t channel)
{
    if (started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel < 1 || channel > 13) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "initialize netif");
    esp_err_t result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }

    const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "set Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, station_mac), TAG,
                        "read station MAC");
    if (CONFIG_ESPDROP_AWDL_NETIF) {
        ESP_RETURN_ON_ERROR(espdrop_awdl_netif_init(station_mac), TAG,
                            "initialize AWDL esp-netif");
    }
    ESP_RETURN_ON_ERROR(
        espdrop_awdl_tx_lab_init(CONFIG_ESPDROP_DEVICE_NAME), TAG,
        "initialize bounded transmit lab");

    const wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filter), TAG,
                        "set promiscuous filter");

    record_queue = xQueueCreate(32, sizeof(awdl_probe_record_t));
    if (record_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    capture_queue = xQueueCreate(4, sizeof(awdl_mif_capture_t));
    if (capture_queue == NULL) {
        vQueueDelete(record_queue);
        record_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    data_queue = xQueueCreate(16, sizeof(awdl_data_record_t));
    if (data_queue == NULL) {
        vQueueDelete(record_queue);
        vQueueDelete(capture_queue);
        record_queue = NULL;
        capture_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    data_capture_queue = xQueueCreate(8, sizeof(awdl_data_capture_t));
    if (data_capture_queue == NULL) {
        vQueueDelete(record_queue);
        vQueueDelete(capture_queue);
        vQueueDelete(data_queue);
        record_queue = NULL;
        capture_queue = NULL;
        data_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(probe_log_task, "awdl_probe_log", 6144, NULL, 5, NULL) !=
        pdPASS) {
        vQueueDelete(record_queue);
        vQueueDelete(capture_queue);
        vQueueDelete(data_queue);
        vQueueDelete(data_capture_queue);
        record_queue = NULL;
        capture_queue = NULL;
        data_queue = NULL;
        data_capture_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(promiscuous_rx), TAG,
                        "register promiscuous callback");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE),
                        TAG, "select channel");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG,
                        "enable promiscuous mode");
    started = true;
    ESP_LOGI(TAG, "listening for AWDL action frames on channel %u", channel);
    return ESP_OK;
}

espdrop_awdl_probe_stats_t espdrop_awdl_probe_stats(void)
{
    return stats;
}
