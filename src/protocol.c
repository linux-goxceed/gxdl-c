#define _POSIX_C_SOURCE 200809L
#include "gxdl.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    uint16_t chip;
    bool stub;
    if (!loader || !packet || !size || loader->size < 8U || !loader->data)
        return false;
    stub = gx_is_uart_ipl_stub(loader->data, loader->size);
    chip = loader->chip;
    if (!stub && loader->has_chip_override)
        chip = loader->chip_override;
    if (stub)
        chip = 0x6701U;
    transfer_size = stage1_transfer_size(chip);
    payload_size = chip == 0x6612U ? transfer_size - 0x20U : transfer_size - 4U;
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

bool gx_build_payload_stage2(const uint8_t *payload, size_t payload_size,
                             uint8_t **data, size_t *size, uint8_t metadata[8]) {
    uint32_t sum = 0;
    size_t i;
    uint8_t *content;
    if (!payload || !data || !size || !metadata || payload_size == 0 ||
        payload_size > UINT32_MAX)
        return false;
    content = malloc(payload_size);
    if (!content)
        return false;
    memcpy(content, payload, payload_size);
    for (i = 0; i < payload_size; ++i)
        sum += content[i];
    gx_write_le32(metadata, sum);
    gx_write_le32(metadata + 4, (uint32_t)payload_size);
    *data = content;
    *size = payload_size;
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

static void record_gxid(gx_context *ctx, const gx_buffer *buffer) {
    ctx->has_gxid = gx_parse_gxid(buffer->data, buffer->len, ctx->gxid_family,
                                  sizeof(ctx->gxid_family), ctx->gxid_name,
                                  sizeof(ctx->gxid_name));
    if (ctx->has_gxid)
        fprintf(stderr, "[+] GXID family=%s name=%s\n", ctx->gxid_family,
                ctx->gxid_name);
}

static bool wait_runget(gx_context *ctx) {
    gx_buffer buffer;
    int64_t deadline = gx_now_ms() + 10000;
    int64_t run_seen = 0;
    gx_buffer_init(&buffer);
    ctx->has_gxid = false;
    ctx->gxid_family[0] = '\0';
    ctx->gxid_name[0] = '\0';
    while (gx_now_ms() < deadline) {
        if (gx_parse_gxid(buffer.data, buffer.len, ctx->gxid_family,
                          sizeof(ctx->gxid_family), ctx->gxid_name,
                          sizeof(ctx->gxid_name))) {
            ctx->has_gxid = true;
            if (!gx_family_trains_ddr(ctx->gxid_family)) {
                fprintf(stderr, "[+] Detection-only GXID; no DDR training (untested)\n");
                record_gxid(ctx, &buffer);
                gx_buffer_free(&buffer);
                return true;
            }
        }
        if (runget_has_contiguous(&buffer)) {
            fprintf(stderr, "[*] Detected RUNGET\n");
            record_gxid(ctx, &buffer);
            gx_buffer_free(&buffer);
            return true;
        }
        if (runget_has_token(&buffer, "RUN") && run_seen == 0) {
            fprintf(stderr, "[*] Received RUN\n");
            run_seen = gx_now_ms();
        }
        if (run_seen && runget_has_token(&buffer, "GET")) {
            fprintf(stderr, "[*] Received GET\n");
            record_gxid(ctx, &buffer);
            gx_buffer_free(&buffer);
            return true;
        }
        if (runget_has_short_sequence(&buffer) || runget_has_ordered(&buffer, 40U)) {
            fprintf(stderr, "[*] Detected tolerant RUNGET variant\n");
            record_gxid(ctx, &buffer);
            gx_buffer_free(&buffer);
            return true;
        }
        if (run_seen && gx_now_ms() - run_seen >= 1000) {
            fprintf(stderr, "[*] Proceeding after RUN without explicit GET\n");
            record_gxid(ctx, &buffer);
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

static bool path_is_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool join_dir_file(char *out, size_t out_size, const char *dir, const char *name) {
    int needed;
    if (!out || out_size == 0 || !dir || !name)
        return false;
    needed = snprintf(out, out_size, "%s/%s", dir, name);
    return needed >= 0 && (size_t)needed < out_size;
}

static bool resolve_bootcode(const gx_context *ctx, const char *family, char *out,
                             size_t out_size) {
    const char *name = gx_bootcode_filename_for_family(family);
    if (ctx->bootcode_path) {
        if (path_is_file(ctx->bootcode_path)) {
            int needed = snprintf(out, out_size, "%s", ctx->bootcode_path);
            return needed >= 0 && (size_t)needed < out_size;
        }
        return false;
    }
    if (!name)
        return false;
    if (ctx->bootcode_dir && join_dir_file(out, out_size, ctx->bootcode_dir, name) &&
        path_is_file(out))
        return true;
    if (ctx->boot_file) {
        char copy[PATH_MAX];
        int needed = snprintf(copy, sizeof(copy), "%s", ctx->boot_file);
        if (needed >= 0 && (size_t)needed < sizeof(copy)) {
            const char *dir = dirname(copy);
            if (join_dir_file(out, out_size, dir, name) && path_is_file(out))
                return true;
        }
    }
    {
        int needed = snprintf(out, out_size, "%s", name);
        if (needed < 0 || (size_t)needed >= out_size)
            return false;
    }
    return path_is_file(out);
}

static bool send_stage2_bytes(gx_context *ctx, const uint8_t *metadata,
                              const uint8_t *data, size_t size) {
    size_t sent = 0;
    gx_progress progress;
    gx_sleep_ms(50);
    if (gx_serial_write_all(&ctx->serial, metadata, 4, 5000) != 0 ||
        gx_serial_write_all(&ctx->serial, metadata + 4, 4, 5000) != 0) {
        fprintf(stderr, "[!] Stage 2 metadata write failed: %s\n", strerror(errno));
        return false;
    }
    progress_start(&progress, "Uploading loader", size);
    while (sent < size) {
        size_t chunk = size - sent;
        if (chunk > GX_STAGE2_CHUNK)
            chunk = GX_STAGE2_CHUNK;
        if (gx_serial_write_all(&ctx->serial, data + sent, chunk, 5000) != 0) {
            fprintf(stderr, "[!] Stage 2 write failed at %zu bytes: %s\n",
                    sent, strerror(errno));
            progress_cancel(&progress);
            return false;
        }
        sent += chunk;
        progress_update(&progress, sent);
    }
    if (gx_serial_drain(&ctx->serial) != 0) {
        fprintf(stderr, "[!] Failed to drain loader data: %s\n", strerror(errno));
        progress_cancel(&progress);
        return false;
    }
    progress_finish(&progress);
    return true;
}

bool gx_boot(gx_context *ctx, const gx_loader *loader, bool read_output) {
    gx_loader stage_loader;
    uint8_t *stage1 = NULL;
    size_t stage1_size = 0;
    size_t stage1_payload_size;
    uint8_t metadata[8];
    uint8_t *stage2 = NULL;
    size_t stage2_size = 0;
    char bootcode_path[PATH_MAX];
    bool skip_stage2 = false;

    if (!ctx || !loader)
        return false;
    stage_loader = *loader;
    if (ctx->has_chip_override) {
        stage_loader.has_chip_override = true;
        stage_loader.chip_override = ctx->chip_override;
    }
    if (!gx_build_stage1(&stage_loader, &stage1, &stage1_size)) {
        fprintf(stderr, "[!] Could not construct boot packets\n");
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

    if (ctx->has_gxid && !gx_family_trains_ddr(ctx->gxid_family)) {
        fprintf(stderr, "[+] No Stage 2: this family has no open DDR init\n");
        skip_stage2 = true;
    } else if (ctx->has_gxid) {
        if (!resolve_bootcode(ctx, ctx->gxid_family, bootcode_path, sizeof(bootcode_path))) {
            const char *name = gx_bootcode_filename_for_family(ctx->gxid_family);
            const char *hint = strcmp(ctx->gxid_family, "cygnus") == 0
                                   ? "make SOC=gx6706 bootcode"
                                   : "make bootcode";
            fprintf(stderr, "[!] GXID received; matching bootcode is not present\n");
            fprintf(stderr, "[!] Need %s (%s) or --bootcode <file>\n",
                    name ? name : "a family bootcode file", hint);
            fprintf(stderr, "[!] Not sending the UART stub as Stage 2 (that causes EBUNDLE)\n");
            goto fail;
        } else {
            uint8_t *raw = NULL, *wrapped = NULL;
            size_t raw_size = 0, wrapped_size = 0;
            fprintf(stderr, "[+] Sending GXBC from %s\n", bootcode_path);
            if (!gx_read_file(bootcode_path, &raw, &raw_size) ||
                !gx_wrap_gxbc(raw, raw_size, &wrapped, &wrapped_size) ||
                !gx_build_payload_stage2(wrapped, wrapped_size, &stage2, &stage2_size,
                                         metadata)) {
                fprintf(stderr, "[!] Could not construct GXBC Stage 2\n");
                free(raw);
                free(wrapped);
                goto fail;
            }
            free(raw);
            free(wrapped);
            if (!send_stage2_bytes(ctx, metadata, stage2, stage2_size))
                goto fail;
        }
    } else if (!gx_build_stage2(loader, &stage2, &stage2_size, metadata) ||
               !send_stage2_bytes(ctx, metadata, stage2, stage2_size)) {
        fprintf(stderr, "[!] Could not construct boot packets\n");
        goto fail;
    }

    free(stage1);
    free(stage2);
    if (skip_stage2)
        return true;
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

bool gx_command_begin(gx_context *ctx, const char *command, gx_buffer *extra) {
    size_t len = strlen(command);
    gx_buffer_init(extra);
    if (!gx_wait_prompt(ctx, 2000))
        return false;
    if (ctx->verbose)
        fprintf(stderr, "[*] Sending command: %s\n", command);
    if (gx_serial_write_all(&ctx->serial, command, len, 5000) != 0 ||
        gx_serial_write_all(&ctx->serial, "\n", 1, 1000) != 0 ||
        gx_serial_drain(&ctx->serial) != 0) {
        fprintf(stderr, "[!] Command write failed: %s\n", strerror(errno));
        return false;
    }
    if (!wait_marker(ctx, extra, command, 5000, true)) {
        fprintf(stderr, "[!] Command was not echoed: %s\n", command);
        gx_buffer_free(extra);
        return false;
    }
    while (extra->len && (extra->data[0] == '\r' || extra->data[0] == '\n'))
        gx_buffer_consume(extra, 1);
    return true;
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
