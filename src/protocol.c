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

bool gx_build_stage1(const gx_loader *loader,
                     uint8_t packet[GX_STAGE1_PACKET_SIZE]) {
    if (!loader || loader->size < 0x201cU)
        return false;
    packet[0] = 0x59;
    packet[1] = 0x00;
    packet[2] = 0x08;
    packet[3] = 0x00;
    packet[4] = 0x00;
    memcpy(packet + 5, loader->data + 0x20, GX_STAGE1_PAYLOAD_SIZE);
    memcpy(packet + 5 + GX_STAGE1_PAYLOAD_SIZE, "boot", 4);
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
    gx_write_le16(metadata, (uint16_t)sum);
    gx_write_le16(metadata + 2, 0x00c2);
    gx_write_le32(metadata + 4, (uint32_t)loader->size);
    *data = content;
    *size = loader->size;
    return true;
}

static bool wait_handshake(gx_context *ctx) {
    uint8_t recent[4] = {0};
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
            recent[3] = byte;
        }
        if ((count >= 3 && memcmp(recent + count - 3, "\xb0\xb0\x58", 3) == 0) ||
            (count >= 4 && (memcmp(recent, "\xb8\xb0\xff\x58", 4) == 0 ||
                            memcmp(recent, "\x00\xb0\xb0\x58", 4) == 0 ||
                            memcmp(recent, "\xb0\x30\xff\x58", 4) == 0))) {
            if (ctx->verbose)
                fprintf(stderr, "[*] Handshake detected\n");
            return true;
        }
    }
    fprintf(stderr, "[!] Timeout waiting for device handshake\n");
    return false;
}

static bool wait_runget(gx_context *ctx) {
    gx_buffer buffer;
    int64_t deadline = gx_now_ms() + 10000;
    int64_t run_seen = 0;
    gx_buffer_init(&buffer);
    while (gx_now_ms() < deadline) {
        if (gx_buffer_find(&buffer, "RUN", 3) >= 0 && run_seen == 0) {
            fprintf(stderr, "[*] Received RUN\n");
            run_seen = gx_now_ms();
        }
        if (run_seen && gx_buffer_find(&buffer, "GET", 3) >= 0) {
            fprintf(stderr, "[*] Received GET\n");
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
    uint8_t stage1[GX_STAGE1_PACKET_SIZE];
    uint8_t metadata[8];
    uint8_t *stage2 = NULL;
    size_t stage2_size = 0, sent = 0;
    gx_progress progress;
    if (!gx_build_stage1(loader, stage1) ||
        !gx_build_stage2(loader, &stage2, &stage2_size, metadata)) {
        fprintf(stderr, "[!] Could not construct boot packets\n");
        return false;
    }
    if (gx_serial_pulse_resets(&ctx->serial, ctx->reset_dtr, ctx->reset_rts) != 0 &&
        ctx->verbose)
        fprintf(stderr, "[*] Reset flush failed; continuing\n");
    (void)gx_serial_flush(&ctx->serial);
    if (!wait_handshake(ctx))
        goto fail;
    if (gx_serial_flush(&ctx->serial) != 0 ||
        gx_serial_write_all(&ctx->serial, stage1, 5, 5000) != 0 ||
        gx_serial_write_all(&ctx->serial, stage1 + 5,
                            GX_STAGE1_PAYLOAD_SIZE, 5000) != 0 ||
        gx_serial_write_all(&ctx->serial,
                            stage1 + 5 + GX_STAGE1_PAYLOAD_SIZE, 4, 5000) != 0 ||
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
    free(stage2);
    fprintf(stderr, "[+] Boot upload complete\n");
    if (read_output)
        read_boot_output(ctx, 15000);
    return true;
fail:
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
