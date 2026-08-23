#include "espdrop/airdrop_http.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const uint8_t discover_body[ESPDROP_AIRDROP_DISCOVER_BODY_BYTES] = {
    0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd0, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
};

static unsigned char ascii_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + 32U)
                                        : value;
}

static bool ascii_equal(const uint8_t *value, size_t length, const char *text)
{
    const size_t text_length = strlen(text);
    if (length != text_length) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (ascii_lower(value[index]) !=
            ascii_lower((unsigned char)text[index])) {
            return false;
        }
    }
    return true;
}

static bool contains_bytes(
    const uint8_t *value,
    size_t length,
    const char *needle)
{
    const size_t needle_length = strlen(needle);
    if (needle_length == 0U || needle_length > length) {
        return false;
    }
    for (size_t offset = 0U; offset + needle_length <= length; ++offset) {
        if (memcmp(value + offset, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static void copy_header_value(
    char *output,
    size_t capacity,
    const uint8_t *value,
    size_t length)
{
    if (capacity == 0U) {
        return;
    }
    if (length >= capacity) {
        length = capacity - 1U;
    }
    memcpy(output, value, length);
    output[length] = '\0';
}

static bool parse_decimal(const uint8_t *value, size_t length, size_t *parsed)
{
    if (length == 0U || parsed == NULL) {
        return false;
    }
    size_t number = 0U;
    for (size_t index = 0U; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        const unsigned digit = (unsigned)(value[index] - '0');
        if (number > (SIZE_MAX - digit) / 10U) {
            return false;
        }
        number = number * 10U + digit;
    }
    *parsed = number;
    return true;
}

static int hex_digit(uint8_t value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = ascii_lower(value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static espdrop_airdrop_http_parse_t parse_chunked_body(
    const uint8_t *response,
    size_t length,
    size_t offset,
    bool end_of_stream,
    espdrop_airdrop_http_result_t *result)
{
    static const char receiver_key[] = "ReceiverComputerName";
    uint8_t prefix[8] = {0};
    size_t prefix_bytes = 0U;
    size_t key_match = 0U;
    size_t decoded = 0U;
    while (offset < length) {
        size_t line_end = offset;
        while (line_end + 1U < length &&
               !(response[line_end] == '\r' &&
                 response[line_end + 1U] == '\n')) {
            ++line_end;
        }
        if (line_end + 1U >= length) {
            return end_of_stream ? ESPDROP_AIRDROP_HTTP_INVALID
                                 : ESPDROP_AIRDROP_HTTP_INCOMPLETE;
        }
        size_t digits_end = offset;
        while (digits_end < line_end && response[digits_end] != ';') {
            ++digits_end;
        }
        if (digits_end == offset) {
            return ESPDROP_AIRDROP_HTTP_INVALID;
        }
        size_t chunk_bytes = 0U;
        for (size_t index = offset; index < digits_end; ++index) {
            const int digit = hex_digit(response[index]);
            if (digit < 0 || chunk_bytes > (SIZE_MAX - (size_t)digit) / 16U) {
                return ESPDROP_AIRDROP_HTTP_INVALID;
            }
            chunk_bytes = chunk_bytes * 16U + (size_t)digit;
        }
        offset = line_end + 2U;
        if (chunk_bytes == 0U) {
            if (offset + 2U <= length && response[offset] == '\r' &&
                response[offset + 1U] == '\n') {
                result->body_bytes = decoded;
                result->binary_plist =
                    prefix_bytes == sizeof(prefix) &&
                    memcmp(prefix, "bplist00", sizeof(prefix)) == 0;
                result->receiver_computer_name_key =
                    key_match == sizeof(receiver_key) - 1U;
                return ESPDROP_AIRDROP_HTTP_COMPLETE;
            }
            for (size_t index = offset; index + 3U < length; ++index) {
                if (memcmp(response + index, "\r\n\r\n", 4U) == 0) {
                    result->body_bytes = decoded;
                    result->binary_plist =
                        prefix_bytes == sizeof(prefix) &&
                        memcmp(prefix, "bplist00", sizeof(prefix)) == 0;
                    result->receiver_computer_name_key =
                        key_match == sizeof(receiver_key) - 1U;
                    return ESPDROP_AIRDROP_HTTP_COMPLETE;
                }
            }
            return end_of_stream ? ESPDROP_AIRDROP_HTTP_INVALID
                                 : ESPDROP_AIRDROP_HTTP_INCOMPLETE;
        }
        if (chunk_bytes > length - offset ||
            chunk_bytes + 2U > length - offset) {
            return end_of_stream ? ESPDROP_AIRDROP_HTTP_INVALID
                                 : ESPDROP_AIRDROP_HTTP_INCOMPLETE;
        }
        for (size_t index = 0U; index < chunk_bytes; ++index) {
            const uint8_t byte = response[offset + index];
            if (prefix_bytes < sizeof(prefix)) {
                prefix[prefix_bytes++] = byte;
            }
            if (key_match < sizeof(receiver_key) - 1U) {
                if (byte == (uint8_t)receiver_key[key_match]) {
                    ++key_match;
                } else {
                    key_match = byte == (uint8_t)receiver_key[0] ? 1U : 0U;
                }
            }
        }
        decoded += chunk_bytes;
        offset += chunk_bytes;
        if (response[offset] != '\r' || response[offset + 1U] != '\n') {
            return ESPDROP_AIRDROP_HTTP_INVALID;
        }
        offset += 2U;
    }
    return end_of_stream ? ESPDROP_AIRDROP_HTTP_INVALID
                         : ESPDROP_AIRDROP_HTTP_INCOMPLETE;
}

size_t espdrop_airdrop_build_discover_request(
    uint8_t *output,
    size_t capacity,
    const char *host,
    uint16_t port)
{
    if (output == NULL || host == NULL || host[0] == '\0' || port == 0U) {
        return 0U;
    }
    const int header_length = snprintf(
        (char *)output, capacity,
        "POST /Discover HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Connection: keep-alive\r\n"
        "Accept: */*\r\n"
        "User-Agent: AirDrop/1.0\r\n"
        "Accept-Language: en-us\r\n"
        "Accept-Encoding: br, gzip, deflate\r\n"
        "Content-Length: %u\r\n\r\n",
        host, (unsigned)port,
        (unsigned)ESPDROP_AIRDROP_DISCOVER_BODY_BYTES);
    if (header_length < 0 || (size_t)header_length >= capacity ||
        (size_t)header_length + sizeof(discover_body) > capacity) {
        return 0U;
    }
    memcpy(output + (size_t)header_length, discover_body,
           sizeof(discover_body));
    return (size_t)header_length + sizeof(discover_body);
}

espdrop_airdrop_http_parse_t espdrop_airdrop_parse_discover_response(
    const uint8_t *response,
    size_t length,
    bool end_of_stream,
    espdrop_airdrop_http_result_t *result)
{
    if (response == NULL || result == NULL) {
        return ESPDROP_AIRDROP_HTTP_INVALID;
    }
    memset(result, 0, sizeof(*result));
    size_t header_bytes = 0U;
    for (size_t index = 0U; index + 3U < length; ++index) {
        if (memcmp(response + index, "\r\n\r\n", 4U) == 0) {
            header_bytes = index + 4U;
            break;
        }
    }
    if (header_bytes == 0U) {
        return end_of_stream ? ESPDROP_AIRDROP_HTTP_INVALID
                             : ESPDROP_AIRDROP_HTTP_INCOMPLETE;
    }
    const uint8_t *line_end = NULL;
    for (size_t index = 0U; index + 1U < header_bytes; ++index) {
        if (response[index] == '\r' && response[index + 1U] == '\n') {
            line_end = response + index;
            break;
        }
    }
    if (line_end == NULL || (size_t)(line_end - response) < 12U ||
        memcmp(response, "HTTP/1.", 7U) != 0 || response[8] != ' ' ||
        response[9] < '0' || response[9] > '9' ||
        response[10] < '0' || response[10] > '9' ||
        response[11] < '0' || response[11] > '9') {
        return ESPDROP_AIRDROP_HTTP_INVALID;
    }
    result->status_code =
        (unsigned)(response[9] - '0') * 100U +
        (unsigned)(response[10] - '0') * 10U +
        (unsigned)(response[11] - '0');
    result->header_bytes = header_bytes;

    size_t offset = (size_t)(line_end - response) + 2U;
    while (offset + 2U < header_bytes) {
        size_t end = offset;
        while (end + 1U < header_bytes &&
               !(response[end] == '\r' && response[end + 1U] == '\n')) {
            ++end;
        }
        if (end == offset) {
            break;
        }
        size_t colon = offset;
        while (colon < end && response[colon] != ':') {
            ++colon;
        }
        if (colon == end) {
            return ESPDROP_AIRDROP_HTTP_INVALID;
        }
        size_t value = colon + 1U;
        while (value < end &&
               (response[value] == ' ' || response[value] == '\t')) {
            ++value;
        }
        size_t value_end = end;
        while (value_end > value &&
               (response[value_end - 1U] == ' ' ||
                response[value_end - 1U] == '\t')) {
            --value_end;
        }
        const size_t value_length = value_end - value;
        if (ascii_equal(response + offset, colon - offset,
                        "Content-Length")) {
            if (!parse_decimal(response + value, value_length,
                               &result->content_length)) {
                return ESPDROP_AIRDROP_HTTP_INVALID;
            }
            result->has_content_length = true;
        } else if (ascii_equal(response + offset, colon - offset,
                               "Content-Type")) {
            copy_header_value(result->content_type,
                              sizeof(result->content_type),
                              response + value, value_length);
        } else if (ascii_equal(response + offset, colon - offset,
                               "Content-Encoding")) {
            copy_header_value(result->content_encoding,
                              sizeof(result->content_encoding),
                              response + value, value_length);
        } else if (ascii_equal(response + offset, colon - offset,
                               "Transfer-Encoding") &&
                   ascii_equal(response + value, value_length, "chunked")) {
            result->chunked = true;
        }
        offset = end + 2U;
    }

    const size_t available_body = length - header_bytes;
    if (result->has_content_length) {
        if (available_body < result->content_length) {
            return end_of_stream ? ESPDROP_AIRDROP_HTTP_INVALID
                                 : ESPDROP_AIRDROP_HTTP_INCOMPLETE;
        }
        result->body_bytes = result->content_length;
    } else if (result->chunked) {
        return parse_chunked_body(response, length, header_bytes,
                                  end_of_stream, result);
    } else {
        if (!end_of_stream) {
            return ESPDROP_AIRDROP_HTTP_INCOMPLETE;
        }
        result->body_bytes = available_body;
    }
    const uint8_t *body = response + header_bytes;
    result->binary_plist =
        result->body_bytes >= 8U && memcmp(body, "bplist00", 8U) == 0;
    result->receiver_computer_name_key = contains_bytes(
        body, result->body_bytes, "ReceiverComputerName");
    return ESPDROP_AIRDROP_HTTP_COMPLETE;
}
