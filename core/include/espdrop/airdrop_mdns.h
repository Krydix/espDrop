#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPDROP_MDNS_NAME_BYTES 128U
#define ESPDROP_MDNS_TXT_BYTES 256U
#define ESPDROP_MDNS_MAX_SERVICES 4U
#define ESPDROP_MDNS_MAX_HOSTS 4U
#define ESPDROP_MDNS_TYPE_PTR 12U
#define ESPDROP_MDNS_TYPE_TXT 16U
#define ESPDROP_MDNS_TYPE_AAAA 28U
#define ESPDROP_MDNS_TYPE_SRV 33U
#define ESPDROP_AIRDROP_RECEIVER_FLAGS_ANONYMOUS 136U

typedef struct {
    char name[ESPDROP_MDNS_NAME_BYTES];
    uint8_t address[16];
} espdrop_mdns_host_t;

typedef struct {
    char instance[ESPDROP_MDNS_NAME_BYTES];
    char target[ESPDROP_MDNS_NAME_BYTES];
    char txt[ESPDROP_MDNS_TXT_BYTES];
    uint8_t ipv6[16];
    uint16_t port;
    bool has_ptr;
    bool has_srv;
    bool has_txt;
    bool has_ipv6;
    bool endpoint_published;
} espdrop_airdrop_service_t;

typedef struct {
    espdrop_airdrop_service_t services[ESPDROP_MDNS_MAX_SERVICES];
    size_t service_count;
    espdrop_mdns_host_t hosts[ESPDROP_MDNS_MAX_HOSTS];
    size_t host_count;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
    bool response;
} espdrop_airdrop_mdns_result_t;

typedef enum {
    ESPDROP_MDNS_QUERY_RESPONSE_NONE = 0,
    ESPDROP_MDNS_QUERY_RESPONSE_MULTICAST,
    ESPDROP_MDNS_QUERY_RESPONSE_UNICAST,
} espdrop_mdns_query_response_t;

bool espdrop_mdns_build_query(
    uint8_t *packet,
    size_t capacity,
    size_t *length,
    const char *name,
    uint16_t type,
    bool unicast_response);

/* Build a complete identity-free receiver announcement: one AirDrop PTR
 * answer plus its SRV, TXT, and IPv6 address records. */
bool espdrop_airdrop_mdns_build_announcement(
    uint8_t *packet,
    size_t capacity,
    size_t *length,
    const char *service_id,
    const char *host_name,
    uint16_t port,
    uint32_t flags,
    const uint8_t ipv6[16]);

bool espdrop_airdrop_mdns_query_requests_service(
    const uint8_t *packet,
    size_t length);

/* Return how an AirDrop DNS-SD query asks to be answered. mDNS questions set
 * the high class bit (QU) when the first response must be sent directly back
 * to the querier instead of to ff02::fb. */
espdrop_mdns_query_response_t espdrop_airdrop_mdns_query_response(
    const uint8_t *packet,
    size_t length);

bool espdrop_airdrop_mdns_parse(
    const uint8_t *packet,
    size_t length,
    espdrop_airdrop_mdns_result_t *result);

void espdrop_airdrop_mdns_merge(
    espdrop_airdrop_mdns_result_t *destination,
    const espdrop_airdrop_mdns_result_t *source);

bool espdrop_airdrop_service_complete(
    const espdrop_airdrop_service_t *service);

#ifdef __cplusplus
}
#endif
