#define _POSIX_C_SOURCE 200809L
#include "gxdl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "progressbar.h"

typedef struct {
    progressbar *bar;
    size_t total;
    unsigned int percent;
    bool tty;
    const char *label;
} gx_progress;

static void progress_start(gx_progress *p, const char *label, size_t total) {
    memset(p, 0, sizeof(*p));
    p->total = total;
    p->label = label;
    p->tty = isatty(STDERR_FILENO) && getenv("TERM") != NULL;
    if (p->tty)
        p->bar = progressbar_new(label, total ? (unsigned long)total : 1UL);
    else
        fprintf(stderr, "[*] %s: 0%%\n", label);
}

static void progress_update(gx_progress *p, size_t value) {
    unsigned int percent = p->total
        ? (unsigned int)((100.0L * (long double)value) / (long double)p->total)
        : 100U;
    if (percent == p->percent && value != p->total)
        return;
    p->percent = percent;
    if (p->bar)
        progressbar_update(p->bar, (unsigned long)value);
    else if (percent == 100U || percent % 10U == 0U)
        fprintf(stderr, "[*] %s: %u%%\n", p->label, percent);
}

static void progress_finish(gx_progress *p) {
    if (p->bar) {
        progressbar_update(p->bar, (unsigned long)(p->total ? p->total : 1U));
        progressbar_finish(p->bar);
    } else if (p->percent != 100U) {
        fprintf(stderr, "[*] %s: 100%%\n", p->label);
    }
}

static void progress_cancel(gx_progress *p) {
    if (p->bar) {
        progressbar_free(p->bar);
        fputc('\n', stderr);
        p->bar = NULL;
    }
}

static bool append_read(gx_context *ctx, gx_buffer *buffer, int timeout_ms) {
    uint8_t block[4096];
    ssize_t n = gx_serial_read(&ctx->serial, block, sizeof(block), timeout_ms);
    if (n < 0) {
        fprintf(stderr, "[!] Serial read failed: %s\n", strerror(errno));
        return false;
    }
    if (n == 0) {
        errno = ETIMEDOUT;
        return false;
    }
    if (!gx_buffer_append(buffer, block, (size_t)n)) {
        errno = ENOMEM;
        return false;
    }
    return true;
}

static bool wait_marker(gx_context *ctx, gx_buffer *buffer, const char *marker,
                        int timeout_ms, bool consume) {
    size_t marker_len = strlen(marker);
    int64_t deadline = gx_now_ms() + timeout_ms;
    for (;;) {
        ssize_t at = gx_buffer_find(buffer, marker, marker_len);
        if (at >= 0) {
            if (consume)
                gx_buffer_consume(buffer, (size_t)at + marker_len);
            return true;
        }
        if (gx_now_ms() >= deadline)
            return false;
        if (!append_read(ctx, buffer, (int)(deadline - gx_now_ms())) &&
            errno != ETIMEDOUT)
            return false;
    }
}

static size_t stage1_transfer_size(uint16_t chip) {
    if (chip == 0x6612U) return 0x4000U;
    if (chip == 0x6616U || chip == 0x3211U || chip == 0x6701U || chip == 0x6705U)
        return 0x2000U;
    return 0x1000U;
}

bool gx_build_stage1(const gx_loader *loader, uint8_t **packet, size_t *size) {
    size_t transfer_size, payload_size;
    uint8_t *result;
    if (!loader || !packet || !size || loader->size < 8U)
        return false;
    transfer_size = stage1_transfer_size(loader->chip);
    payload_size = loader->chip == 0x6612U ? transfer_size - 0x20U : transfer_size - 4U;
    if (loader->size < 0x20U + payload_size)
        return false;
    result = malloc(5U + payload_size + 4U);
    if (!result)
        return false;
    result[0] = 0x59;
    gx_write_le16(result + 1, (uint16_t)(transfer_size >> 2));
    gx_write_le16(result + 3, 0);
    memcpy(result + 5, loader->data + 0x20, payload_size);
    memcpy(result + 5 + payload_size, "boot", 4);
    *packet = result;
    *size = 5U + payload_size + 4U;
    return true;
}

bool gx_build_stage2(const gx_loader *loader, uint8_t **data, size_t *size,
                     uint8_t metadata[8]) {
    uint32_t sum = 0;
    size_t i;
    uint8_t *content;
    if (!loader || loader->size < 0x20U || loader->size > UINT32_MAX)
        return false;
    content = calloc(1, loader->size);
    if (!content)
        return false;
    memcpy(content, loader->data, 4);
    memcpy(content + 4, loader->data + 0x20, loader->size - 0x20);
    for (i = 0; i < loader->size; ++i)
        sum += content[i];
    gx_write_le32(metadata, sum);
    gx_write_le32(metadata + 4, (uint32_t)loader->size);
    *data = content;
    *size = loader->size;
    return true;
}

static bool wait_handshake(gx_context *ctx) {
    uint8_t recent[8] = {0};
    size_t count = 0;
    int64_t deadline = gx_now_ms() + 30000;
    fprintf(stderr, "[*] Waiting for device handshake; power-cycle or reset it now\n");
    while (gx_now_ms() < deadline) {
        uint8_t byte;
        ssize_t n = gx_serial_read(&ctx->serial, &byte, 1, 50);
        if (n < 0) {
            fprintf(stderr, "[!] Handshake read failed: %s\n", strerror(errno));
            return false;
        }
        if (n == 0)
            continue;
        if (count < sizeof(recent))
            recent[count++] = byte;
        else {
            memmove(recent, recent + 1, sizeof(recent) - 1);
            recent[sizeof(recent) - 1U] = byte;
        }
        if (byte == 0x58U && count >= 3U) {
            size_t end = count - 1U;
            bool valid = recent[end - 2U] == 0x00U || recent[end - 2U] == 0xb0U ||
                         recent[end - 2U] == 0xb8U;
            if (!valid && count >= 4U)
                valid = recent[end - 3U] == 0x00U || recent[end - 3U] == 0xb0U ||
                        recent[end - 3U] == 0xb8U;
            if (valid) {
                if (ctx->verbose)
                    fprintf(stderr, "[*] Handshake detected\n");
                return true;
            }
        }
    }
    fprintf(stderr, "[!] Timeout waiting for device handshake\n");
    return false;
}

static bool ascii_equal(uint8_t value, char expected) {
    return value == (uint8_t)expected || value == (uint8_t)(expected - 'A' + 'a');
}

static bool runget_has_contiguous(const gx_buffer *buffer) {
    size_t i;
    for (i = 0; i + 6U <= buffer->len; ++i) {
        if (ascii_equal(buffer->data[i], 'R') && ascii_equal(buffer->data[i + 1U], 'U') &&
            ascii_equal(buffer->data[i + 2U], 'N') && ascii_equal(buffer->data[i + 3U], 'G') &&
            ascii_equal(buffer->data[i + 4U], 'E') && ascii_equal(buffer->data[i + 5U], 'T'))
            return true;
    }
    return false;
}

static bool is_alnum_byte(uint8_t value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9');
}

static bool runget_has_token(const gx_buffer *buffer, const char token[4]) {
    size_t i;
    size_t length = strlen(token);
    for (i = 0; i + length <= buffer->len; ++i) {
        size_t j;
        bool match = true;
        if ((i > 0U && is_alnum_byte(buffer->data[i - 1U])) ||
            (i + length < buffer->len && is_alnum_byte(buffer->data[i + length])))
            continue;
        for (j = 0; j < length; ++j) {
            if (!ascii_equal(buffer->data[i + j], token[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static bool runget_has_short_sequence(const gx_buffer *buffer) {
    size_t i, position = 0;
    static const char pattern[] = "RUNGET";
    for (i = 0; i < buffer->len && position < sizeof(pattern) - 1U; ++i) {
        if (ascii_equal(buffer->data[i], pattern[position])) {
            ++position;
            continue;
        }
        if (position > 0U && is_alnum_byte(buffer->data[i]))
            position = 0;
        else if (position > 0U && i > 0U) {
            size_t gap = 0;
            size_t k = i;
            while (k > 0U && !is_alnum_byte(buffer->data[k - 1U]) && gap <= 4U) {
                --k;
                ++gap;
            }
            if (gap > 4U) position = 0;
        }
    }
    return position == sizeof(pattern) - 1U;
}

static bool runget_has_ordered(const gx_buffer *buffer, size_t max_gap) {
    static const char pattern[] = "RUNGET";
    size_t start;
    for (start = 0; start < buffer->len; ++start) {
        size_t position = 0, last = start, i;
        if (!ascii_equal(buffer->data[start], pattern[0]))
            continue;
        position = 1;
        for (i = start + 1U; i < buffer->len && position < sizeof(pattern) - 1U; ++i) {
            if (!ascii_equal(buffer->data[i], pattern[position]))
                continue;
            if (i - last > max_gap)
                break;
            last = i;
            ++position;
        }
        if (position == sizeof(pattern) - 1U)
            return true;
    }
    return false;
}

static bool wait_runget(gx_context *ctx) {
    gx_buffer buffer;
    int64_t deadline = gx_now_ms() + 10000;
    int64_t run_seen = 0;
    gx_buffer_init(&buffer);
    while (gx_now_ms() < deadline) {
        if (runget_has_contiguous(&buffer)) {
            fprintf(stderr, "[*] Detected RUNGET\n");
            gx_buffer_free(&buffer);
            return true;
        }
        if (runget_has_token(&buffer, "RUN") && run_seen == 0) {
            fprintf(stderr, "[*] Received RUN\n");
            run_seen = gx_now_ms();
        }
        if (run_seen && runget_has_token(&buffer, "GET")) {
            fprintf(stderr, "[*] Received GET\n");
            gx_buffer_free(&buffer);
            return true;
        }
        if (runget_has_short_sequence(&buffer) || runget_has_ordered(&buffer, 40U)) {
            fprintf(stderr, "[*] Detected tolerant RUNGET variant\n");
            gx_buffer_free(&buffer);
            return true;
        }
        if (run_seen && gx_now_ms() - run_seen >= 1000) {
            fprintf(stderr, "[*] Proceeding after RUN without explicit GET\n");
            gx_buffer_free(&buffer);
            return true;
        }
        if (!append_read(ctx, &buffer, 50) && errno != ETIMEDOUT) {
            gx_buffer_free(&buffer);
            return false;
        }
    }
    fprintf(stderr, "[!] Timeout waiting for RUNGET response\n");
    gx_buffer_free(&buffer);
    return false;
}

static void read_boot_output(gx_context *ctx, int timeout_ms) {
    gx_buffer buffer;
    int64_t deadline = gx_now_ms() + timeout_ms;
    gx_buffer_init(&buffer);
    while (gx_now_ms() < deadline) {
        uint8_t block[512];
        ssize_t n = gx_serial_read(&ctx->serial, block, sizeof(block), 100);
        if (n < 0)
            break;
        if (n > 0) {
            fwrite(block, 1, (size_t)n, stdout);
            fflush(stdout);
            if (!gx_buffer_append(&buffer, block, (size_t)n))
                break;
            if (gx_buffer_find(&buffer, "boot>", 5) >= 0)
                break;
            if (buffer.len > 32)
                gx_buffer_consume(&buffer, buffer.len - 32);
        }
    }
    gx_buffer_free(&buffer);
}

bool gx_boot(gx_context *ctx, const gx_loader *loader, bool read_output) {
    uint8_t *stage1 = NULL;
    size_t stage1_size = 0;
    size_t stage1_payload_size;
    uint8_t metadata[8];
    uint8_t *stage2 = NULL;
    size_t stage2_size = 0, sent = 0;
    gx_progress progress;
    if (!gx_build_stage1(loader, &stage1, &stage1_size) ||
        !gx_build_stage2(loader, &stage2, &stage2_size, metadata)) {
        fprintf(stderr, "[!] Could not construct boot packets\n");
        free(stage1);
        return false;
    }
    stage1_payload_size = stage1_size - 9U;
    if (gx_serial_pulse_resets(&ctx->serial, ctx->reset_dtr, ctx->reset_rts) != 0 &&
        ctx->verbose)
        fprintf(stderr, "[*] Reset flush failed; continuing\n");
    (void)gx_serial_flush(&ctx->serial);
    if (!wait_handshake(ctx))
        goto fail;
    if (gx_serial_flush(&ctx->serial) != 0 ||
        gx_serial_write_all(&ctx->serial, stage1, 5U, 5000) != 0 ||
        gx_serial_write_all(&ctx->serial, stage1 + 5U, stage1_payload_size, 5000) != 0 ||
        gx_serial_write_all(&ctx->serial, stage1 + 5U + stage1_payload_size, 4U, 5000) != 0 ||
        gx_serial_drain(&ctx->serial) != 0) {
        fprintf(stderr, "[!] Stage 1 write failed: %s\n", strerror(errno));
        goto fail;
    }
    if (!wait_runget(ctx))
        goto fail;
    gx_sleep_ms(50);
    if (gx_serial_write_all(&ctx->serial, metadata, 4, 5000) != 0 ||
        gx_serial_write_all(&ctx->serial, metadata + 4, 4, 5000) != 0) {
        fprintf(stderr, "[!] Stage 2 metadata write failed: %s\n", strerror(errno));
        goto fail;
    }
    progress_start(&progress, "Uploading loader", stage2_size);
    while (sent < stage2_size) {
        size_t chunk = stage2_size - sent;
        if (chunk > GX_STAGE2_CHUNK) chunk = GX_STAGE2_CHUNK;
        if (gx_serial_write_all(&ctx->serial, stage2 + sent, chunk, 5000) != 0) {
            fprintf(stderr, "[!] Stage 2 write failed at %zu bytes: %s\n",
                    sent, strerror(errno));
            progress_cancel(&progress);
            goto fail;
        }
        sent += chunk;
        progress_update(&progress, sent);
    }
    if (gx_serial_drain(&ctx->serial) != 0) {
        fprintf(stderr, "[!] Failed to drain loader data: %s\n", strerror(errno));
        progress_cancel(&progress);
        goto fail;
    }
    progress_finish(&progress);
    free(stage1);
    free(stage2);
    fprintf(stderr, "[+] Boot upload complete\n");
    if (read_output)
        read_boot_output(ctx, 15000);
    return true;
fail:
    free(stage1);
    free(stage2);
    return false;
}

bool gx_wait_prompt(gx_context *ctx, int timeout_ms) {
    gx_buffer buffer;
    int64_t started = gx_now_ms();
    int64_t deadline = started + timeout_ms;
    bool poked = false;
    gx_buffer_init(&buffer);
    while (gx_now_ms() < deadline) {
        if (gx_buffer_find(&buffer, "boot>", 5) >= 0) {
            gx_buffer_free(&buffer);
            return true;
        }
        if (!poked && gx_now_ms() - started >= 200) {
            (void)gx_serial_write_all(&ctx->serial, "\n", 1, 1000);
            (void)gx_serial_drain(&ctx->serial);
            poked = true;
        }
        if (!append_read(ctx, &buffer, 50) && errno != ETIMEDOUT)
            break;
        if (buffer.len > 4096)
            gx_buffer_consume(&buffer, buffer.len - 64);
    }
    gx_buffer_free(&buffer);
    fprintf(stderr, "[!] Not at boot> prompt\n");
    return false;
}

/* Shared with commands.c through deliberately small internal hooks. */
bool gx_internal_wait_marker(gx_context *ctx, gx_buffer *buffer,
                             const char *marker, int timeout_ms, bool consume) {
    return wait_marker(ctx, buffer, marker, timeout_ms, consume);
}

bool gx_internal_append_read(gx_context *ctx, gx_buffer *buffer, int timeout_ms) {
    return append_read(ctx, buffer, timeout_ms);
}

void gx_internal_progress_start(void *progress, const char *label, size_t total) {
    progress_start(progress, label, total);
}
void gx_internal_progress_update(void *progress, size_t value) {
    progress_update(progress, value);
}
void gx_internal_progress_finish(void *progress) { progress_finish(progress); }
void gx_internal_progress_cancel(void *progress) { progress_cancel(progress); }
size_t gx_internal_progress_size(void) { return sizeof(gx_progress); }
