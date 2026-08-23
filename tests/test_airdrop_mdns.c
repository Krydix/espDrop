#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "espdrop/airdrop_mdns.h"

static void put_u16(uint8_t *packet, size_t *position, uint16_t value)
{
    packet[(*position)++] = (uint8_t)(value >> 8U);
    packet[(*position)++] = (uint8_t)value;
}

static void put_u32(uint8_t *packet, size_t *position, uint32_t value)
{
    packet[(*position)++] = (uint8_t)(value >> 24U);
    packet[(*position)++] = (uint8_t)(value >> 16U);
    packet[(*position)++] = (uint8_t)(value >> 8U);
    packet[(*position)++] = (uint8_t)value;
}

static void put_name(uint8_t *packet, size_t *position, const char *name)
{
    const char *label = name;
    while (*label != '\0') {
        const char *dot = strchr(label, '.');
        const size_t length = dot != NULL ? (size_t)(dot - label) : strlen(label);
        packet[(*position)++] = (uint8_t)length;
        memcpy(&packet[*position], label, length);
        *position += length;
        if (dot == NULL) {
            break;
        }
        label = dot + 1;
    }
    packet[(*position)++] = 0U;
}

static void put_record_header(
    uint8_t *packet,
    size_t *position,
    uint16_t type,
    uint16_t data_length)
{
    put_u16(packet, position, type);
    put_u16(packet, position, 0x8001U);
    put_u32(packet, position, 120U);
    put_u16(packet, position, data_length);
}

static size_t fixture(uint8_t *packet)
{
    memset(packet, 0, 768U);
    packet[2] = 0x84U;
    packet[7] = 1U;
    packet[11] = 3U;
    size_t position = 12U;

    put_name(packet, &position, "_airdrop._tcp.local");
    put_record_header(packet, &position, 12U, 6U);
    packet[position++] = 3U;
    memcpy(&packet[position], "abc", 3U);
    position += 3U;
    packet[position++] = 0xc0U;
    packet[position++] = 0x0cU;

    packet[position++] = 3U;
    memcpy(&packet[position], "abc", 3U);
    position += 3U;
    packet[position++] = 0xc0U;
    packet[position++] = 0x0cU;
    put_record_header(packet, &position, 33U, 20U);
    put_u16(packet, &position, 0U);
    put_u16(packet, &position, 0U);
    put_u16(packet, &position, 8770U);
    put_name(packet, &position, "iphone.local");

    packet[position++] = 3U;
    memcpy(&packet[position], "abc", 3U);
    position += 3U;
    packet[position++] = 0xc0U;
    packet[position++] = 0x0cU;
    static const uint8_t txt[] = {
        7U, 'f', 'l', 'a', 'g', 's', '=', '1',
        10U, 'm', 'o', 'd', 'e', 'l', '=', 'P', 'h', 'o', 'n',
    };
    put_record_header(packet, &position, 16U, sizeof(txt));
    memcpy(&packet[position], txt, sizeof(txt));
    position += sizeof(txt);

    put_name(packet, &position, "iphone.local");
    put_record_header(packet, &position, 28U, 16U);
    static const uint8_t ipv6[16] = {
        0xfe, 0x80, 0, 0, 0, 0, 0, 0,
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    };
    memcpy(&packet[position], ipv6, sizeof(ipv6));
    return position + sizeof(ipv6);
}

int main(void)
{
    uint8_t packet[768];
    const size_t length = fixture(packet);
    espdrop_airdrop_mdns_result_t result;
    assert(espdrop_airdrop_mdns_parse(packet, length, &result));
    assert(result.response);
    assert(result.answers == 1U);
    assert(result.additional == 3U);
    assert(result.service_count == 1U);
    assert(result.host_count == 1U);
    const espdrop_airdrop_service_t *service = &result.services[0];
    assert(strcmp(service->instance, "abc._airdrop._tcp.local") == 0);
    assert(strcmp(service->target, "iphone.local") == 0);
    assert(strcmp(service->txt, "flags=1;model=Phon") == 0);
    assert(service->port == 8770U);
    assert(service->ipv6[15] == 0xf0U);
    assert(espdrop_airdrop_service_complete(service));

    espdrop_airdrop_mdns_result_t cache = {0};
    espdrop_airdrop_mdns_merge(&cache, &result);
    assert(cache.service_count == 1U);
    assert(espdrop_airdrop_service_complete(&cache.services[0]));

    espdrop_airdrop_mdns_result_t service_packet = result;
    service_packet.host_count = 0U;
    service_packet.services[0].has_ipv6 = false;
    memset(service_packet.services[0].ipv6, 0,
           sizeof(service_packet.services[0].ipv6));
    espdrop_airdrop_mdns_result_t address_packet = {0};
    address_packet.host_count = 1U;
    address_packet.hosts[0] = result.hosts[0];
    espdrop_airdrop_mdns_result_t split_cache = {0};
    espdrop_airdrop_mdns_merge(&split_cache, &service_packet);
    assert(!espdrop_airdrop_service_complete(&split_cache.services[0]));
    espdrop_airdrop_mdns_merge(&split_cache, &address_packet);
    assert(espdrop_airdrop_service_complete(&split_cache.services[0]));

    assert(!espdrop_airdrop_mdns_parse(packet, length - 1U, &result));
    packet[12] = 0xc0U;
    packet[13] = 0x0cU;
    assert(!espdrop_airdrop_mdns_parse(packet, length, &result));

    puts("AirDrop mDNS tests passed");
    return 0;
}
