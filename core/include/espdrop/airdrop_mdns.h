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
