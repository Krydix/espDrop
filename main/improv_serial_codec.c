#include "improv_serial_codec.h"

#include <stdbool.h>
#include <string.h>

static const uint8_t header[] = {'I', 'M', 'P', 'R', 'O', 'V'};

void improv_serial_parser_reset(improv_serial_parser_t *parser)
{
    if (parser != NULL) {
        parser->length = 0;
        parser->expected_length = 0;
    }
}

static void restart_with_byte(improv_serial_parser_t *parser, uint8_t byte)
{
    improv_serial_parser_reset(parser);
    if (byte == header[0]) {
        parser->bytes[0] = byte;
        parser->length = 1;
    }
}

improv_serial_parse_result_t improv_serial_parser_feed(
    improv_serial_parser_t *parser, uint8_t byte, improv_serial_packet_t *packet)
{
    if (parser == NULL || packet == NULL) {
        return IMPROV_SERIAL_PARSE_INVALID;
    }
    if (parser->length == 0) {
        restart_with_byte(parser, byte);
        return IMPROV_SERIAL_PARSE_NONE;
    }
    if (parser->length < sizeof(header)) {
        if (byte != header[parser->length]) {
            restart_with_byte(parser, byte);
            return IMPROV_SERIAL_PARSE_NONE;
        }
        parser->bytes[parser->length++] = byte;
        return IMPROV_SERIAL_PARSE_NONE;
    }
    if (parser->length >= sizeof(parser->bytes)) {
        restart_with_byte(parser, byte);
        return IMPROV_SERIAL_PARSE_INVALID;
    }
    parser->bytes[parser->length++] = byte;
    if (parser->length == 9U) {
        parser->expected_length =
            IMPROV_SERIAL_PACKET_OVERHEAD + parser->bytes[8];
    }
    if (parser->expected_length == 0U ||
        parser->length < parser->expected_length) {
        return IMPROV_SERIAL_PARSE_NONE;
    }

    uint8_t checksum = 0;
    for (size_t index = 0; index + 1U < parser->expected_length; ++index) {
        checksum = (uint8_t)(checksum + parser->bytes[index]);
    }
    const bool valid = parser->bytes[6] == IMPROV_SERIAL_VERSION &&
                       checksum == parser->bytes[parser->expected_length - 1U];
    if (valid) {
        packet->type = parser->bytes[7];
        packet->length = parser->bytes[8];
        if (packet->length > 0U) {
            memcpy(packet->data, &parser->bytes[9], packet->length);
        }
    }
    improv_serial_parser_reset(parser);
    return valid ? IMPROV_SERIAL_PARSE_PACKET : IMPROV_SERIAL_PARSE_INVALID;
}

size_t improv_serial_encode_packet(uint8_t type, const uint8_t *data,
                                   size_t data_length, uint8_t *output,
                                   size_t capacity)
{
    if (output == NULL || data_length > IMPROV_SERIAL_MAX_DATA ||
        (data_length > 0U && data == NULL)) {
        return 0;
    }
    const size_t packet_length = IMPROV_SERIAL_PACKET_OVERHEAD + data_length;
    if (capacity < packet_length) {
        return 0;
    }
    memcpy(output, header, sizeof(header));
    output[6] = IMPROV_SERIAL_VERSION;
    output[7] = type;
    output[8] = (uint8_t)data_length;
    if (data_length > 0U) {
        memcpy(&output[9], data, data_length);
    }
    uint8_t checksum = 0;
    for (size_t index = 0; index + 1U < packet_length; ++index) {
        checksum = (uint8_t)(checksum + output[index]);
    }
    output[packet_length - 1U] = checksum;
    return packet_length;
}
