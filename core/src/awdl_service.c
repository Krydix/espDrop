#include "espdrop/awdl_service.h"

#include <string.h>

#define AWDL_SERVICE_RESPONSE_TLV 2U
#define AWDL_DNS_PTR 12U
#define AWDL_DNS_TXT 16U
#define AWDL_DNS_SRV 33U
#define AWDL_DNS_AIRDROP_TCP 0xc007U
#define AWDL_DNS_AIRDROP_UDP 0xc008U
#define AWDL_DNS_AIRDROP_LABEL 0xc009U
#define AWDL_DNS_TCP_LOCAL 0xc00aU
#define AWDL_DNS_UDP_LOCAL 0xc00bU

typedef struct {
    bool airdrop;
    bool airdrop_tcp;
    bool airdrop_udp;
    bool asquic;
} awdl_service_name_t;

static uint16_t read_le16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8U);
}

static bool label_equal(
    const uint8_t *label,
    size_t length,
    const char *expected)
{
    const size_t expected_length = strlen(expected);
    if (length != expected_length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        uint8_t actual = label[index];
        uint8_t wanted = (uint8_t)expected[index];
        if (actual >= 'A' && actual <= 'Z') {
            actual = (uint8_t)(actual + ('a' - 'A'));
        }
        if (wanted >= 'A' && wanted <= 'Z') {
            wanted = (uint8_t)(wanted + ('a' - 'A'));
        }
        if (actual != wanted) {
            return false;
        }
    }
    return true;
}

static bool scan_service_name(
    const uint8_t *name,
    size_t length,
    awdl_service_name_t *service)
{
    size_t offset = 0U;
    bool saw_airdrop_label = false;
    bool saw_asquic_label = false;
    bool saw_tcp_label = false;
    bool saw_udp_label = false;
    bool saw_local_label = false;
    while (offset < length) {
        const uint8_t component_length = name[offset];
        if ((component_length & 0xc0U) != 0U) {
            if ((component_length & 0xc0U) != 0xc0U ||
                length - offset < 2U || offset + 2U != length) {
                return false;
            }
            const uint16_t compressed =
                ((uint16_t)component_length << 8U) | name[offset + 1U];
            if (compressed == AWDL_DNS_AIRDROP_TCP) {
                service->airdrop = true;
                service->airdrop_tcp = true;
            } else if (compressed == AWDL_DNS_AIRDROP_UDP) {
                service->airdrop = true;
                service->airdrop_udp = true;
            } else if (compressed == AWDL_DNS_AIRDROP_LABEL) {
                service->airdrop = true;
            } else if (compressed == AWDL_DNS_TCP_LOCAL &&
                       saw_airdrop_label) {
                service->airdrop = true;
                service->airdrop_tcp = true;
            } else if (compressed == AWDL_DNS_UDP_LOCAL) {
                if (saw_airdrop_label) {
                    service->airdrop = true;
                    service->airdrop_udp = true;
                }
                if (saw_asquic_label) {
                    service->asquic = true;
                }
            }
            return true;
        }
        if (component_length == 0U) {
            if (offset + 1U != length) {
                return false;
            }
            service->airdrop_tcp =
                saw_airdrop_label && saw_tcp_label && saw_local_label;
            service->airdrop_udp =
                saw_airdrop_label && saw_udp_label && saw_local_label;
            service->airdrop =
                service->airdrop_tcp || service->airdrop_udp;
            service->asquic =
                saw_asquic_label && saw_udp_label && saw_local_label;
            return true;
        }
        if (component_length > 63U ||
            (size_t)component_length > length - offset - 1U) {
            return false;
        }
        const uint8_t *label = name + offset + 1U;
        saw_airdrop_label = saw_airdrop_label ||
            label_equal(label, component_length, "_airdrop");
        saw_asquic_label = saw_asquic_label ||
            label_equal(label, component_length, "_asquic");
        saw_tcp_label = saw_tcp_label ||
            label_equal(label, component_length, "_tcp");
        saw_udp_label = saw_udp_label ||
            label_equal(label, component_length, "_udp");
        saw_local_label = saw_local_label ||
            label_equal(label, component_length, "local");
        offset += 1U + component_length;
    }
    return false;
}

static bool scan_service_record(
    const uint8_t *value,
    size_t length,
    espdrop_awdl_service_profile_t *profile)
{
    if (length < 8U) {
        return false;
    }
    const uint16_t name_and_type_length = read_le16(value);
    if (name_and_type_length < 2U ||
        (size_t)name_and_type_length > length - 6U) {
        return false;
    }
    const size_t name_length = (size_t)name_and_type_length - 1U;
    const size_t type_offset = 2U + name_length;
    const size_t data_length_offset = type_offset + 1U;
    const uint16_t data_length = read_le16(value + data_length_offset);
    const size_t required = data_length_offset + 4U + data_length;
    if (required != length) {
        return false;
    }

    awdl_service_name_t service = {0};
    if (!scan_service_name(value + 2U, name_length, &service)) {
        return false;
    }
    ++profile->record_count;
    const uint8_t record_type = value[type_offset];
    if (record_type == AWDL_DNS_PTR) {
        ++profile->ptr_count;
    } else if (record_type == AWDL_DNS_TXT) {
        ++profile->txt_count;
    } else if (record_type == AWDL_DNS_SRV) {
        ++profile->srv_count;
    }
    profile->has_airdrop = profile->has_airdrop || service.airdrop;
    profile->has_airdrop_tcp =
        profile->has_airdrop_tcp || service.airdrop_tcp;
    profile->has_airdrop_udp =
        profile->has_airdrop_udp || service.airdrop_udp;
    profile->has_asquic = profile->has_asquic || service.asquic;
    return true;
}

espdrop_awdl_parse_result_t espdrop_awdl_scan_service_responses(
    const uint8_t *tlv_data,
    size_t tlv_length,
    espdrop_awdl_service_profile_t *profile)
{
    if (profile == NULL || (tlv_data == NULL && tlv_length != 0U)) {
        return ESPDROP_AWDL_PARSE_INVALID_ARGUMENT;
    }
    memset(profile, 0, sizeof(*profile));
    espdrop_awdl_tlv_iterator_t iterator;
    espdrop_awdl_tlv_iterator_init(&iterator, tlv_data, tlv_length);
    while (true) {
        espdrop_awdl_tlv_view_t view;
        const espdrop_awdl_parse_result_t result =
            espdrop_awdl_tlv_next(&iterator, &view);
        if (result == ESPDROP_AWDL_PARSE_END) {
            return profile->malformed_record_count == 0U
                       ? ESPDROP_AWDL_PARSE_OK
                       : ESPDROP_AWDL_PARSE_INVALID_VALUE;
        }
        if (result != ESPDROP_AWDL_PARSE_OK) {
            return result;
        }
        if (view.type == AWDL_SERVICE_RESPONSE_TLV &&
            !scan_service_record(view.value, view.length, profile)) {
            ++profile->malformed_record_count;
        }
    }
}
