#include "espdrop/airdrop_mdns.h"

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
