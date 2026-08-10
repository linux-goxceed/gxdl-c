#define _POSIX_C_SOURCE 200809L
#include "gxdl.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint16_t gx_read_le16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

uint32_t gx_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void gx_write_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

void gx_write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

void gx_write_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

uint32_t gx_checksum(const uint8_t *data, size_t size) {
    static const uint8_t key[] = {0x12, 0x34, 0x56, 0x78};
    uint32_t sum = 0;
    size_t i;
    for (i = 0; i < size; ++i)
        sum += (uint32_t)(key[i & 3U] ^ data[i]);
    return sum;
}

int64_t gx_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void gx_sleep_ms(unsigned int ms) {
    struct timespec req = {(time_t)(ms / 1000U), (long)(ms % 1000U) * 1000000L};
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {}
}

bool gx_parse_u64(const char *text, uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;
    if (!text || !*text || text[0] == '-')
        return false;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0')
        return false;
    *value = (uint64_t)parsed;
    return true;
}

void gx_buffer_init(gx_buffer *buffer) {
    memset(buffer, 0, sizeof(*buffer));
}

void gx_buffer_free(gx_buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

bool gx_buffer_append(gx_buffer *buffer, const void *data, size_t size) {
    size_t need;
    uint8_t *next;
    if (size == 0)
        return true;
    if (SIZE_MAX - buffer->len < size)
        return false;
    need = buffer->len + size;
    if (need > buffer->cap) {
        size_t cap = buffer->cap ? buffer->cap : 256;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        next = realloc(buffer->data, cap);
        if (!next)
            return false;
        buffer->data = next;
        buffer->cap = cap;
    }
    memcpy(buffer->data + buffer->len, data, size);
    buffer->len += size;
    return true;
}

void gx_buffer_consume(gx_buffer *buffer, size_t size) {
    if (size >= buffer->len) {
        buffer->len = 0;
        return;
    }
    memmove(buffer->data, buffer->data + size, buffer->len - size);
    buffer->len -= size;
}

ssize_t gx_buffer_find(const gx_buffer *buffer, const void *needle, size_t size) {
    size_t i;
    if (size == 0)
        return 0;
    if (size > buffer->len)
        return -1;
    for (i = 0; i <= buffer->len - size; ++i) {
        if (memcmp(buffer->data + i, needle, size) == 0)
            return (ssize_t)i;
    }
    return -1;
}
