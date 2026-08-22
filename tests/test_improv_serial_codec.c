#include "improv_serial_codec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_encode_and_parse(void)
{
    const uint8_t rpc[] = {0x02, 0x00};
    uint8_t encoded[IMPROV_SERIAL_MAX_PACKET_SIZE];
    const size_t length = improv_serial_encode_packet(
        0x03, rpc, sizeof(rpc), encoded, sizeof(encoded));
    assert(length == 12U);
    assert(memcmp(encoded, "IMPROV", 6) == 0);

    improv_serial_parser_t parser = {0};
    improv_serial_packet_t packet = {0};
    for (size_t index = 0; index < length; ++index) {
        const improv_serial_parse_result_t expected =
            index + 1U == length ? IMPROV_SERIAL_PARSE_PACKET
                                 : IMPROV_SERIAL_PARSE_NONE;
        assert(improv_serial_parser_feed(&parser, encoded[index], &packet) ==
               expected);
    }
    assert(packet.type == 0x03);
    assert(packet.length == sizeof(rpc));
    assert(memcmp(packet.data, rpc, sizeof(rpc)) == 0);
}

static void test_checksum_rejection_and_recovery(void)
{
    const uint8_t state[] = {0x02};
    uint8_t encoded[IMPROV_SERIAL_MAX_PACKET_SIZE];
    const size_t length = improv_serial_encode_packet(
        0x01, state, sizeof(state), encoded, sizeof(encoded));
    ++encoded[length - 1U];
    improv_serial_parser_t parser = {0};
    improv_serial_packet_t packet = {0};
    for (size_t index = 0; index < length; ++index) {
        const improv_serial_parse_result_t expected =
            index + 1U == length ? IMPROV_SERIAL_PARSE_INVALID
                                 : IMPROV_SERIAL_PARSE_NONE;
        assert(improv_serial_parser_feed(&parser, encoded[index], &packet) ==
               expected);
    }
    assert(parser.length == 0U);
}

int main(void)
{
    test_encode_and_parse();
    test_checksum_rejection_and_recovery();
    puts("Improv Serial codec tests passed");
    return 0;
}
