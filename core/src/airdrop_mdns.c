#include "espdrop/airdrop_mdns.h"

#include <stdio.h>
#include <string.h>

#define DNS_HEADER_BYTES 12U
#define DNS_POINTER_MASK 0xc0U
#define DNS_POINTER_VALUE 0xc0U
#define DNS_MAX_RECORDS 64U
#define DNS_MAX_POINTERS 16U
#define DNS_TYPE_AAAA ESPDROP_MDNS_TYPE_AAAA
#define DNS_TYPE_PTR ESPDROP_MDNS_TYPE_PTR
#define DNS_TYPE_SRV ESPDROP_MDNS_TYPE_SRV
#define DNS_TYPE_TXT ESPDROP_MDNS_TYPE_TXT
#define DNS_CLASS_IN 0x0001U
#define DNS_CLASS_CACHE_FLUSH 0x8001U
#define DNS_ANNOUNCEMENT_TTL 120U
#define DNS_AIRDROP_TYPE_NAME_BYTES 21U
#define DNS_LOCAL_SUFFIX_BYTES 7U

typedef struct {
    uint8_t *packet;
    size_t capacity;
    size_t position;
    bool failed;
} dns_writer_t;

static void dns_write_u8(dns_writer_t *writer, uint8_t value)
{
    if (writer->failed || writer->position >= writer->capacity) {
        writer->failed = true;
        return;
    }
    writer->packet[writer->position++] = value;
}

static void dns_write_u16(dns_writer_t *writer, uint16_t value)
{
    dns_write_u8(writer, (uint8_t)(value >> 8U));
    dns_write_u8(writer, (uint8_t)value);
}

static void dns_write_u32(dns_writer_t *writer, uint32_t value)
{
    dns_write_u16(writer, (uint16_t)(value >> 16U));
    dns_write_u16(writer, (uint16_t)value);
}

static void dns_write_bytes(
    dns_writer_t *writer,
    const uint8_t *bytes,
    size_t length)
{
    if (writer->failed || length > writer->capacity - writer->position) {
        writer->failed = true;
        return;
    }
    memcpy(writer->packet + writer->position, bytes, length);
    writer->position += length;
}

static bool dns_label_valid(const char *label)
{
    if (label == NULL) {
        return false;
    }
    const size_t length = strlen(label);
    if (length == 0U || length > 63U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char byte = (unsigned char)label[index];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= 'A' && byte <= 'Z') ||
              (byte >= '0' && byte <= '9') || byte == '-')) {
            return false;
        }
    }
    return true;
}

static void dns_write_label(dns_writer_t *writer, const char *label)
{
    const size_t length = strlen(label);
    dns_write_u8(writer, (uint8_t)length);
    dns_write_bytes(writer, (const uint8_t *)label, length);
}

static void dns_write_airdrop_type(dns_writer_t *writer)
{
    dns_write_label(writer, "_airdrop");
    dns_write_label(writer, "_tcp");
    dns_write_label(writer, "local");
    dns_write_u8(writer, 0U);
}

static void dns_write_instance(dns_writer_t *writer, const char *service_id)
{
    dns_write_label(writer, service_id);
    dns_write_airdrop_type(writer);
}

static void dns_write_host(dns_writer_t *writer, const char *host_name)
{
    dns_write_label(writer, host_name);
    dns_write_label(writer, "local");
    dns_write_u8(writer, 0U);
}

static void dns_write_record_head(
    dns_writer_t *writer,
    uint16_t type,
    uint16_t dns_class,
    uint16_t data_length)
{
    dns_write_u16(writer, type);
    dns_write_u16(writer, dns_class);
    dns_write_u32(writer, DNS_ANNOUNCEMENT_TTL);
    dns_write_u16(writer, data_length);
}

bool espdrop_mdns_build_query(
    uint8_t *packet,
    size_t capacity,
    size_t *length,
    const char *name,
    uint16_t type,
    bool unicast_response)
{
    if (packet == NULL || length == NULL || name == NULL || name[0] == '\0' ||
        capacity < DNS_HEADER_BYTES + 6U) {
        return false;
    }
    memset(packet, 0, DNS_HEADER_BYTES);
    packet[5] = 1U;
    size_t position = DNS_HEADER_BYTES;
    const char *label = name;
    while (*label != '\0') {
        const char *dot = strchr(label, '.');
        const size_t label_length = dot != NULL
                                        ? (size_t)(dot - label)
                                        : strlen(label);
        if (label_length == 0U || label_length > 63U ||
            position + 1U + label_length + 5U > capacity) {
            return false;
        }
        packet[position++] = (uint8_t)label_length;
        memcpy(&packet[position], label, label_length);
        position += label_length;
        if (dot == NULL) {
            break;
        }
        label = dot + 1;
    }
    packet[position++] = 0U;
    packet[position++] = (uint8_t)(type >> 8U);
    packet[position++] = (uint8_t)type;
    const uint16_t dns_class = unicast_response ? 0x8001U : 0x0001U;
    packet[position++] = (uint8_t)(dns_class >> 8U);
    packet[position++] = (uint8_t)dns_class;
    *length = position;
    return true;
}

bool espdrop_airdrop_mdns_build_announcement(
    uint8_t *packet,
    size_t capacity,
    size_t *length,
    const char *service_id,
    const char *host_name,
    uint16_t port,
    uint32_t flags,
    const uint8_t ipv6[16])
{
    if (packet == NULL || length == NULL || ipv6 == NULL || port == 0U ||
        flags == 0U || !dns_label_valid(service_id) ||
        !dns_label_valid(host_name) || capacity < DNS_HEADER_BYTES) {
        return false;
    }
    char txt[32];
    const int txt_length = snprintf(txt, sizeof(txt), "flags=%lu",
                                    (unsigned long)flags);
    if (txt_length <= 0 || (size_t)txt_length > UINT8_MAX ||
        (size_t)txt_length >= sizeof(txt)) {
        return false;
    }

    dns_writer_t writer = {
        .packet = packet,
        .capacity = capacity,
    };
    dns_write_u16(&writer, 0U);
    dns_write_u16(&writer, 0x8400U);
    dns_write_u16(&writer, 0U);
    dns_write_u16(&writer, 1U);
    dns_write_u16(&writer, 0U);
    dns_write_u16(&writer, 3U);

    const uint16_t instance_bytes = (uint16_t)(
        1U + strlen(service_id) + DNS_AIRDROP_TYPE_NAME_BYTES);
    const uint16_t host_bytes = (uint16_t)(
        1U + strlen(host_name) + DNS_LOCAL_SUFFIX_BYTES);

    dns_write_airdrop_type(&writer);
    dns_write_record_head(&writer, DNS_TYPE_PTR, DNS_CLASS_IN,
                          instance_bytes);
    dns_write_instance(&writer, service_id);

    dns_write_instance(&writer, service_id);
    dns_write_record_head(&writer, DNS_TYPE_SRV, DNS_CLASS_CACHE_FLUSH,
                          (uint16_t)(6U + host_bytes));
    dns_write_u16(&writer, 0U);
    dns_write_u16(&writer, 0U);
    dns_write_u16(&writer, port);
    dns_write_host(&writer, host_name);

    dns_write_instance(&writer, service_id);
    dns_write_record_head(&writer, DNS_TYPE_TXT, DNS_CLASS_CACHE_FLUSH,
                          (uint16_t)(1U + (size_t)txt_length));
    dns_write_u8(&writer, (uint8_t)txt_length);
    dns_write_bytes(&writer, (const uint8_t *)txt, (size_t)txt_length);

    dns_write_host(&writer, host_name);
    dns_write_record_head(&writer, DNS_TYPE_AAAA, DNS_CLASS_CACHE_FLUSH, 16U);
    dns_write_bytes(&writer, ipv6, 16U);

    if (writer.failed) {
        *length = 0U;
        return false;
    }
    *length = writer.position;
    return true;
}

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8U) | bytes[1];
}

static bool is_airdrop_instance(const char *name)
{
    static const char suffix[] = "._airdrop._tcp.local";
    const size_t name_length = strlen(name);
    const size_t suffix_length = sizeof(suffix) - 1U;
    return name_length > suffix_length &&
           strcmp(name + name_length - suffix_length, suffix) == 0;
}

static bool decode_name(
    const uint8_t *packet,
    size_t length,
    size_t offset,
    char *output,
    size_t capacity,
    size_t *consumed)
{
    if (packet == NULL || output == NULL || capacity == 0U ||
        offset >= length) {
        return false;
    }
    size_t position = offset;
    size_t output_length = 0U;
    size_t encoded_length = 0U;
    unsigned pointers = 0U;
    bool jumped = false;
    while (position < length) {
        const uint8_t label_length = packet[position];
        if ((label_length & DNS_POINTER_MASK) == DNS_POINTER_VALUE) {
            if (position + 1U >= length || ++pointers > DNS_MAX_POINTERS) {
                return false;
            }
            const size_t target =
                ((size_t)(label_length & 0x3fU) << 8U) | packet[position + 1U];
            if (target >= length) {
                return false;
            }
            if (!jumped) {
                encoded_length += 2U;
            }
            position = target;
            jumped = true;
            continue;
        }
        if ((label_length & DNS_POINTER_MASK) != 0U || label_length > 63U) {
            return false;
        }
        ++position;
        if (!jumped) {
            ++encoded_length;
        }
        if (label_length == 0U) {
            output[output_length] = '\0';
            if (consumed != NULL) {
                *consumed = encoded_length;
            }
            return true;
        }
        if (position + label_length > length ||
            output_length + (output_length == 0U ? 0U : 1U) +
                    label_length >= capacity) {
            return false;
        }
        if (output_length != 0U) {
            output[output_length++] = '.';
        }
        memcpy(&output[output_length], &packet[position], label_length);
        output_length += label_length;
        position += label_length;
        if (!jumped) {
            encoded_length += label_length;
        }
    }
    return false;
}

espdrop_mdns_query_response_t espdrop_airdrop_mdns_query_response(
    const uint8_t *packet,
    size_t length)
{
    if (packet == NULL || length < DNS_HEADER_BYTES ||
        (read_u16(packet + 2U) & 0x8000U) != 0U) {
        return ESPDROP_MDNS_QUERY_RESPONSE_NONE;
    }
    const uint16_t questions = read_u16(packet + 4U);
    if (questions == 0U || questions > DNS_MAX_RECORDS) {
        return ESPDROP_MDNS_QUERY_RESPONSE_NONE;
    }
    bool matched = false;
    bool unicast = false;
    size_t offset = DNS_HEADER_BYTES;
    for (uint16_t question = 0U; question < questions; ++question) {
        char name[ESPDROP_MDNS_NAME_BYTES];
        size_t consumed = 0U;
        if (!decode_name(packet, length, offset, name, sizeof(name),
                         &consumed) || offset + consumed + 4U > length) {
            return ESPDROP_MDNS_QUERY_RESPONSE_NONE;
        }
        offset += consumed;
        const uint16_t type = read_u16(packet + offset);
        const uint16_t dns_class = read_u16(packet + offset + 2U);
        offset += 4U;
        const bool question_matches =
            (strcmp(name, "_airdrop._tcp.local") == 0 &&
             (type == DNS_TYPE_PTR || type == 255U)) ||
            (is_airdrop_instance(name) &&
             (type == DNS_TYPE_SRV || type == DNS_TYPE_TXT ||
              type == 255U));
        if (question_matches) {
            matched = true;
            unicast = unicast || (dns_class & 0x8000U) != 0U;
        }
    }
    if (!matched) {
        return ESPDROP_MDNS_QUERY_RESPONSE_NONE;
    }
    return unicast ? ESPDROP_MDNS_QUERY_RESPONSE_UNICAST
                   : ESPDROP_MDNS_QUERY_RESPONSE_MULTICAST;
}

bool espdrop_airdrop_mdns_query_requests_service(
    const uint8_t *packet,
    size_t length)
{
    return espdrop_airdrop_mdns_query_response(packet, length) !=
           ESPDROP_MDNS_QUERY_RESPONSE_NONE;
}

static espdrop_airdrop_service_t *find_service(
    espdrop_airdrop_mdns_result_t *result,
    const char *instance,
    bool create)
{
    for (size_t index = 0U; index < result->service_count; ++index) {
        if (strcmp(result->services[index].instance, instance) == 0) {
            return &result->services[index];
        }
    }
    if (!create || result->service_count >= ESPDROP_MDNS_MAX_SERVICES) {
        return NULL;
    }
    espdrop_airdrop_service_t *service =
        &result->services[result->service_count++];
    memset(service, 0, sizeof(*service));
    memcpy(service->instance, instance, strlen(instance) + 1U);
    return service;
}

static bool parse_txt(
    const uint8_t *data,
    size_t length,
    char *output,
    size_t capacity)
{
    size_t input = 0U;
    size_t written = 0U;
    while (input < length) {
        const size_t item_length = data[input++];
        if (input + item_length > length) {
            return false;
        }
        const size_t separator = written == 0U ? 0U : 1U;
        if (written + separator + item_length >= capacity) {
            return false;
        }
        if (separator != 0U) {
            output[written++] = ';';
        }
        for (size_t index = 0U; index < item_length; ++index) {
            const uint8_t byte = data[input + index];
            output[written++] = byte >= 0x20U && byte <= 0x7eU
                                    ? (char)byte : '?';
        }
        input += item_length;
    }
    output[written] = '\0';
    return true;
}

bool espdrop_airdrop_mdns_parse(
    const uint8_t *packet,
    size_t length,
    espdrop_airdrop_mdns_result_t *result)
{
    if (packet == NULL || result == NULL || length < DNS_HEADER_BYTES) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    result->response = (read_u16(&packet[2]) & 0x8000U) != 0U;
    result->questions = read_u16(&packet[4]);
    result->answers = read_u16(&packet[6]);
    result->authority = read_u16(&packet[8]);
    result->additional = read_u16(&packet[10]);
    const size_t record_count = (size_t)result->answers + result->authority +
                                result->additional;
    if (record_count > DNS_MAX_RECORDS || result->questions > DNS_MAX_RECORDS) {
        return false;
    }

    size_t offset = DNS_HEADER_BYTES;
    for (uint16_t question = 0U; question < result->questions; ++question) {
        char ignored[ESPDROP_MDNS_NAME_BYTES];
        size_t consumed = 0U;
        if (!decode_name(packet, length, offset, ignored, sizeof(ignored),
                         &consumed) || offset + consumed + 4U > length) {
            return false;
        }
        offset += consumed + 4U;
    }

    for (size_t record = 0U; record < record_count; ++record) {
        char owner[ESPDROP_MDNS_NAME_BYTES];
        size_t owner_bytes = 0U;
        if (!decode_name(packet, length, offset, owner, sizeof(owner),
                         &owner_bytes) || offset + owner_bytes + 10U > length) {
            return false;
        }
        offset += owner_bytes;
        const uint16_t type = read_u16(&packet[offset]);
        const uint16_t data_length = read_u16(&packet[offset + 8U]);
        offset += 10U;
        if (offset + data_length > length) {
            return false;
        }

        if (type == DNS_TYPE_PTR &&
            strcmp(owner, "_airdrop._tcp.local") == 0) {
            char instance[ESPDROP_MDNS_NAME_BYTES];
            size_t consumed = 0U;
            if (!decode_name(packet, length, offset, instance,
                             sizeof(instance), &consumed) ||
                consumed > data_length) {
                return false;
            }
            espdrop_airdrop_service_t *service =
                find_service(result, instance, true);
            if (service != NULL) {
                service->has_ptr = true;
            }
        } else if (type == DNS_TYPE_SRV && data_length >= 7U &&
                   is_airdrop_instance(owner)) {
            char target[ESPDROP_MDNS_NAME_BYTES];
            size_t consumed = 0U;
            if (!decode_name(packet, length, offset + 6U, target,
                             sizeof(target), &consumed) ||
                consumed + 6U > data_length) {
                return false;
            }
            espdrop_airdrop_service_t *service =
                find_service(result, owner, true);
            if (service != NULL) {
                service->port = read_u16(&packet[offset + 4U]);
                memcpy(service->target, target, strlen(target) + 1U);
                service->has_srv = true;
            }
        } else if (type == DNS_TYPE_TXT && is_airdrop_instance(owner)) {
            espdrop_airdrop_service_t *service =
                find_service(result, owner, true);
            if (service != NULL) {
                service->has_txt = parse_txt(
                    &packet[offset], data_length, service->txt,
                    sizeof(service->txt));
                if (!service->has_txt) {
                    return false;
                }
            }
        } else if (type == DNS_TYPE_AAAA && data_length == 16U &&
                   result->host_count < ESPDROP_MDNS_MAX_HOSTS) {
            espdrop_mdns_host_t *host =
                &result->hosts[result->host_count++];
            memcpy(host->name, owner, strlen(owner) + 1U);
            memcpy(host->address, &packet[offset], sizeof(host->address));
        }
        offset += data_length;
    }

    for (size_t service_index = 0U;
         service_index < result->service_count; ++service_index) {
        espdrop_airdrop_service_t *service = &result->services[service_index];
        for (size_t host_index = 0U; host_index < result->host_count;
             ++host_index) {
            if (strcmp(service->target, result->hosts[host_index].name) == 0) {
                memcpy(service->ipv6, result->hosts[host_index].address,
                       sizeof(service->ipv6));
                service->has_ipv6 = true;
                break;
            }
        }
    }
    return true;
}

void espdrop_airdrop_mdns_merge(
    espdrop_airdrop_mdns_result_t *destination,
    const espdrop_airdrop_mdns_result_t *source)
{
    if (destination == NULL || source == NULL) {
        return;
    }
    for (size_t index = 0U; index < source->host_count; ++index) {
        const espdrop_mdns_host_t *incoming = &source->hosts[index];
        espdrop_mdns_host_t *target = NULL;
        for (size_t existing = 0U; existing < destination->host_count;
             ++existing) {
            if (strcmp(destination->hosts[existing].name, incoming->name) == 0) {
                target = &destination->hosts[existing];
                break;
            }
        }
        if (target == NULL && destination->host_count < ESPDROP_MDNS_MAX_HOSTS) {
            target = &destination->hosts[destination->host_count++];
        }
        if (target != NULL) {
            *target = *incoming;
        }
    }
    for (size_t index = 0U; index < source->service_count; ++index) {
        const espdrop_airdrop_service_t *incoming = &source->services[index];
        espdrop_airdrop_service_t *target =
            find_service(destination, incoming->instance, true);
        if (target == NULL) {
            continue;
        }
        if (incoming->has_ptr) {
            target->has_ptr = true;
        }
        if (incoming->has_srv) {
            target->has_srv = true;
            target->port = incoming->port;
            memcpy(target->target, incoming->target,
                   strlen(incoming->target) + 1U);
        }
        if (incoming->has_txt) {
            target->has_txt = true;
            memcpy(target->txt, incoming->txt, strlen(incoming->txt) + 1U);
        }
        if (incoming->has_ipv6) {
            target->has_ipv6 = true;
            memcpy(target->ipv6, incoming->ipv6, sizeof(target->ipv6));
        }
    }
    for (size_t service_index = 0U;
         service_index < destination->service_count; ++service_index) {
        espdrop_airdrop_service_t *service =
            &destination->services[service_index];
        if (!service->has_srv || service->has_ipv6) {
            continue;
        }
        for (size_t host_index = 0U; host_index < destination->host_count;
             ++host_index) {
            if (strcmp(service->target,
                       destination->hosts[host_index].name) == 0) {
                memcpy(service->ipv6,
                       destination->hosts[host_index].address,
                       sizeof(service->ipv6));
                service->has_ipv6 = true;
                break;
            }
        }
    }
}

bool espdrop_airdrop_service_complete(
    const espdrop_airdrop_service_t *service)
{
    return service != NULL && service->has_ptr && service->has_srv &&
           service->has_txt && service->has_ipv6 && service->port != 0U;
}
