#define _POSIX_C_SOURCE 200809L
#include "gxdl.h"
#include "gxdl/gxdl.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_endian_and_checksum(void) {
    uint8_t bytes[4];
    static const uint8_t payload[] = {0x00, 0x01, 0x02, 0x03, 0xff};
    gx_write_le16(bytes, 0xabcd);
    assert(bytes[0] == 0xcd && bytes[1] == 0xab);
    assert(gx_read_le16(bytes) == 0xabcd);
    gx_write_le32(bytes, 0x12345678U);
    assert(gx_read_le32(bytes) == 0x12345678U);
    gx_write_be32(bytes, 0x12345678U);
    assert(memcmp(bytes, "\x12\x34\x56\x78", 4) == 0);
    assert(gx_checksum(payload, sizeof(payload)) ==
           ((0x12U ^ 0x00U) + (0x34U ^ 0x01U) + (0x56U ^ 0x02U) +
            (0x78U ^ 0x03U) + (0x12U ^ 0xffU)));
}

static void test_buffer(void) {
    gx_buffer buffer;
    gx_buffer_init(&buffer);
    assert(gx_buffer_append(&buffer, "abc", 3));
    assert(gx_buffer_append(&buffer, "def", 3));
    assert(gx_buffer_find(&buffer, "cde", 3) == 2);
    gx_buffer_consume(&buffer, 2);
    assert(buffer.len == 4 && memcmp(buffer.data, "cdef", 4) == 0);
    gx_buffer_consume(&buffer, 100);
    assert(buffer.len == 0);
    gx_buffer_free(&buffer);
}

static void test_packets(void) {
    gx_loader loader;
    uint8_t *source = malloc(0x2020);
    uint8_t packet[GX_STAGE1_PACKET_SIZE];
    uint8_t metadata[8];
    uint8_t *stage2 = NULL;
    size_t stage2_size = 0;
    uint32_t sum = 0;
    size_t i;
    assert(source);
    for (i = 0; i < 0x2020; ++i)
        source[i] = (uint8_t)(i * 17U + 3U);
    memcpy(source, "toob", 4);
    gx_write_le16(source + 4, 1);
    gx_write_le16(source + 6, 0x6701);
    gx_write_le32(source + 8, 115200);
    memset(&loader, 0, sizeof(loader));
    loader.data = source;
    loader.size = 0x2020;
    assert(gx_loader_validate(&loader));
    assert(loader.version == 1 && loader.chip == 0x6701 && loader.baud == 115200);
    assert(gx_build_stage1(&loader, packet));
    assert(memcmp(packet, "\x59\x00\x08\x00\x00", 5) == 0);
    assert(memcmp(packet + 5, source + 0x20, GX_STAGE1_PAYLOAD_SIZE) == 0);
    assert(memcmp(packet + sizeof(packet) - 4, "boot", 4) == 0);
    assert(gx_build_stage2(&loader, &stage2, &stage2_size, metadata));
    assert(stage2_size == loader.size);
    assert(memcmp(stage2, "toob", 4) == 0);
    assert(memcmp(stage2 + 4, source + 0x20, loader.size - 0x20) == 0);
    for (i = loader.size - 28; i < loader.size; ++i)
        assert(stage2[i] == 0);
    for (i = 0; i < stage2_size; ++i)
        sum += stage2[i];
    assert(gx_read_le16(metadata) == (uint16_t)sum);
    assert(gx_read_le16(metadata + 2) == 0x00c2);
    assert(gx_read_le32(metadata + 4) == loader.size);
    free(stage2);
    free(source);
}

static void test_parsing_and_compare(void) {
    uint64_t value;
    char left[] = "/tmp/gxdl-left-XXXXXX";
    char right[] = "/tmp/gxdl-right-XXXXXX";
    int lfd, rfd;
    assert(gx_parse_u64("12345", &value) && value == 12345);
    assert(!gx_parse_u64("0x10", &value));
    assert(!gx_parse_u64("-1", &value));
    assert(!gx_parse_u64("12x", &value));
    lfd = mkstemp(left);
    rfd = mkstemp(right);
    assert(lfd >= 0 && rfd >= 0);
    assert(write(lfd, "same", 4) == 4);
    assert(write(rfd, "same", 4) == 4);
    close(lfd);
    close(rfd);
    assert(gx_compare_files(left, right));
    rfd = open(right, O_WRONLY | O_TRUNC);
    assert(rfd >= 0 && write(rfd, "diff", 4) == 4);
    close(rfd);
    assert(!gx_compare_files(left, right));
    unlink(left);
    unlink(right);
}

static void test_public_api(void) {
    gxdl_options options;
    gxdl_options_init(&options);
    assert(options.device == NULL);
    assert(options.baud == 115200U);
    assert(!options.verbose && !options.assume_yes);
    assert(strcmp(gxdl_version(), "1.0.0") == 0);
    assert(gxdl_model_count() == 0);
    assert(gxdl_model_name(0) == NULL);
    errno = 0;
    assert(gxdl_open(NULL) == NULL && errno == EINVAL);
}

int main(void) {
    test_endian_and_checksum();
    test_buffer();
    test_packets();
    test_parsing_and_compare();
    test_public_api();
    assert(gx_embedded_loader_count() == 0);
    puts("unit tests: OK");
    return 0;
}
