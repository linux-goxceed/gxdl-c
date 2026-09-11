#define _POSIX_C_SOURCE 200809L
#include "gxdl.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern bool gx_internal_wait_marker(gx_context *, gx_buffer *, const char *, int, bool);
extern bool gx_internal_append_read(gx_context *, gx_buffer *, int);
extern void gx_internal_progress_start(void *, const char *, size_t);
extern void gx_internal_progress_update(void *, size_t);
extern void gx_internal_progress_finish(void *);
extern void gx_internal_progress_cancel(void *);
extern size_t gx_internal_progress_size(void);

typedef struct {
    char *storage;
    char *argv[32];
    size_t argc;
} words;

static bool split_words(const char *line, words *out) {
    char *save = NULL;
    char *token;
    memset(out, 0, sizeof(*out));
    out->storage = strdup(line ? line : "");
    if (!out->storage)
        return false;
    token = strtok_r(out->storage, " \t\r\n", &save);
    while (token) {
        if (out->argc == sizeof(out->argv) / sizeof(out->argv[0])) {
            fprintf(stderr, "[!] Too many command arguments\n");
            free(out->storage);
            out->storage = NULL;
            return false;
        }
        out->argv[out->argc++] = token;
        token = strtok_r(NULL, " \t\r\n", &save);
    }
    return true;
}

static void free_words(words *args) {
    free(args->storage);
    memset(args, 0, sizeof(*args));
}

static bool parse_size(const char *text, size_t *size) {
    uint64_t value;
    if (!gx_parse_u64(text, &value) || value > SIZE_MAX) {
        fprintf(stderr, "[!] Invalid size or address: %s\n", text);
        return false;
    }
    *size = (size_t)value;
    return true;
}

static bool command_begin(gx_context *ctx, const char *command, gx_buffer *extra) {
    return gx_command_begin(ctx, command, extra);
}

static bool text_command(gx_context *ctx, const char *command, int timeout_ms) {
    gx_buffer response;
    ssize_t prompt;
    if (!command_begin(ctx, command, &response))
        return false;
    if (!gx_internal_wait_marker(ctx, &response, "boot>", timeout_ms, false)) {
        fprintf(stderr, "[!] Timeout waiting for response to: %s\n", command);
        gx_buffer_free(&response);
        return false;
    }
    prompt = gx_buffer_find(&response, "boot>", 5);
    if (prompt > 0) {
        size_t length = (size_t)prompt;
        while (length && (response.data[length - 1] == '\r' ||
                          response.data[length - 1] == '\n'))
            --length;
        if (length) {
            fwrite(response.data, 1, length, stdout);
            fputc('\n', stdout);
        }
    }
    gx_buffer_free(&response);
    return true;
}

static bool confirm(gx_context *ctx, const char *warning) {
    char answer[16];
    size_t length;
    if (ctx->assume_yes)
        return true;
    fprintf(stderr, "[!] %s\n[!] Continue? (y/N) ", warning);
    fflush(stderr);
    if (!fgets(answer, sizeof(answer), stdin)) {
        fprintf(stderr, "\n[!] Aborted\n");
        return false;
    }
    length = strcspn(answer, "\r\n");
    answer[length] = '\0';
    if (strcasecmp(answer, "y") == 0 || strcasecmp(answer, "yes") == 0)
        return true;
    fprintf(stderr, "[!] Aborted\n");
    return false;
}

static bool binary_read(gx_context *ctx, const char *command, size_t size,
                        const char *output) {
    gx_buffer input;
    uint8_t *data = NULL;
    size_t received = 0;
    void *progress = NULL;
    bool ok = false;
    FILE *file;
    bool file_ok;
    if (!command_begin(ctx, command, &input))
        return false;
    if (!gx_internal_wait_marker(ctx, &input, "~sta~", 10000, true)) {
        fprintf(stderr, "[!] Timeout waiting for ~sta~\n");
        goto done;
    }
    data = malloc(size ? size : 1U);
    progress = calloc(1, gx_internal_progress_size());
    if (!data || !progress) {
        fprintf(stderr, "[!] Out of memory receiving %zu bytes\n", size);
        goto done;
    }
    gx_internal_progress_start(progress, "Receiving", size);
    if (input.len) {
        size_t take = input.len < size ? input.len : size;
        memcpy(data, input.data, take);
        received = take;
        gx_buffer_consume(&input, take);
        gx_internal_progress_update(progress, received);
    }
    while (received < size) {
        ssize_t n = gx_serial_read(&ctx->serial, data + received, size - received, 15000);
        if (n <= 0) {
            fprintf(stderr, "[!] Data timeout at %zu/%zu bytes\n", received, size);
            goto finish_progress;
        }
        received += (size_t)n;
        gx_internal_progress_update(progress, received);
    }
    gx_internal_progress_finish(progress);
    free(progress);
    progress = NULL;
    if (!gx_internal_wait_marker(ctx, &input, "~crc~", 5000, true)) {
        fprintf(stderr, "[!] Missing ~crc~ marker\n");
        goto done;
    }
    while (input.len < 4) {
        if (!gx_internal_append_read(ctx, &input, 5000)) {
            fprintf(stderr, "[!] Missing device CRC value\n");
            goto done;
        }
    }
    if (ctx->verbose)
        fprintf(stderr, "[*] Device CRC: 0x%08" PRIx32 "\n",
                gx_read_le32(input.data));
    gx_buffer_consume(&input, 4);
    if (!gx_internal_wait_marker(ctx, &input, "~fin~", 5000, true)) {
        fprintf(stderr, "[!] Missing ~fin~ marker\n");
        goto done;
    }
    file = fopen(output, "wb");
    if (!file) {
        fprintf(stderr, "[!] Cannot create %s: %s\n", output, strerror(errno));
        goto done;
    }
    file_ok = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0)
        file_ok = false;
    if (!file_ok) {
        fprintf(stderr, "[!] Failed writing %s\n", output);
        goto done;
    }
    fprintf(stderr, "[+] Wrote %zu bytes to %s\n", size, output);
    ok = true;
    goto done;
finish_progress:
    gx_internal_progress_cancel(progress);
done:
    free(progress);
    free(data);
    gx_buffer_free(&input);
    return ok;
}

static bool wait_write_completion(gx_context *ctx, gx_buffer *buffer,
                                  bool flash_write) {
    int timeout = flash_write ? 120000 : 60000;
    int64_t deadline = gx_now_ms() + timeout;
    bool finished = gx_buffer_find(buffer, "~fin~", 5) >= 0;
    while (gx_now_ms() < deadline) {
        if (gx_buffer_find(buffer, "err:", 4) >= 0 ||
            gx_buffer_find(buffer, "ERR:", 4) >= 0) {
            fprintf(stderr, "[!] Device reported an error\n");
            return false;
        }
        if (gx_buffer_find(buffer, "~fin~", 5) >= 0)
            finished = true;
        if (!flash_write && finished)
            return true;
        if (gx_buffer_find(buffer, "Partition Version", 17) >= 0 ||
            (finished && gx_buffer_find(buffer, "boot>", 5) >= 0))
            return true;
        if (!gx_internal_append_read(ctx, buffer, 100) && errno != ETIMEDOUT)
            return false;
        if (buffer->len > 8192)
            gx_buffer_consume(buffer, buffer->len - 4096);
    }
    fprintf(stderr, "[!] Timeout waiting for write completion\n");
    return false;
}

static bool binary_write(gx_context *ctx, const char *command_prefix,
                         const char *input_file, bool flash_write) {
    uint8_t *data = NULL;
    size_t size = 0, sent = 0;
    uint32_t checksum;
    uint8_t checksum_bytes[4];
    char *command = NULL;
    gx_buffer response;
    void *progress = NULL;
    bool ok = false;
    if (!gx_read_file(input_file, &data, &size))
        return false;
    {
        int needed = snprintf(NULL, 0, "%s %zu", command_prefix, size);
        if (needed < 0) {
            free(data);
            return false;
        }
        command = malloc((size_t)needed + 1U);
        if (!command) {
            free(data);
            return false;
        }
        (void)snprintf(command, (size_t)needed + 1U, "%s %zu", command_prefix, size);
    }
    if (!command) {
        free(data);
        return false;
    }
    fprintf(stderr, "[!] WARNING: writing %zu bytes from %s\n", size, input_file);
    if (!command_begin(ctx, command, &response))
        goto done;
    if (!gx_internal_wait_marker(ctx, &response, "~sta~", 10000, true)) {
        fprintf(stderr, "[!] Timeout waiting for ~sta~\n");
        goto response_done;
    }
    checksum = gx_checksum(data, size);
    progress = calloc(1, gx_internal_progress_size());
    if (!progress)
        goto response_done;
    gx_internal_progress_start(progress, "Sending", size);
    while (sent < size) {
        size_t chunk = size - sent;
        if (chunk > GX_TRANSFER_CHUNK) chunk = GX_TRANSFER_CHUNK;
        if (gx_serial_write_all(&ctx->serial, data + sent, chunk, 10000) != 0) {
            fprintf(stderr, "[!] Data write failed at %zu bytes: %s\n",
                    sent, strerror(errno));
            goto finish_progress;
        }
        sent += chunk;
        gx_internal_progress_update(progress, sent);
    }
    if (gx_serial_drain(&ctx->serial) != 0)
        goto finish_progress;
    gx_internal_progress_finish(progress);
    free(progress);
    progress = NULL;
    if (!gx_internal_wait_marker(ctx, &response, "~crc~", 10000, true)) {
        fprintf(stderr, "[!] Timeout waiting for ~crc~\n");
        goto response_done;
    }
    gx_write_be32(checksum_bytes, checksum);
    if (gx_serial_write_all(&ctx->serial, checksum_bytes, 4, 5000) != 0 ||
        gx_serial_drain(&ctx->serial) != 0) {
        fprintf(stderr, "[!] Checksum write failed: %s\n", strerror(errno));
        goto response_done;
    }
    if (ctx->verbose)
        fprintf(stderr, "[*] Sent checksum 0x%08" PRIx32 "\n", checksum);
    ok = wait_write_completion(ctx, &response, flash_write);
    if (ok)
        fprintf(stderr, "[+] Write completed successfully\n");
    goto response_done;
finish_progress:
    gx_internal_progress_cancel(progress);
response_done:
    free(progress);
    gx_buffer_free(&response);
done:
    free(command);
    free(data);
    return ok;
}

bool gx_compare_files(const char *left, const char *right) {
    FILE *a = fopen(left, "rb");
    FILE *b = NULL;
    uint8_t abuf[65536], bbuf[65536];
    uint64_t offset = 0;
    bool same = false;
    if (!a) {
        fprintf(stderr, "[!] Cannot open %s: %s\n", left, strerror(errno));
        return false;
    }
    b = fopen(right, "rb");
    if (!b) {
        fprintf(stderr, "[!] Cannot open %s: %s\n", right, strerror(errno));
        fclose(a);
        return false;
    }
    for (;;) {
        size_t an = fread(abuf, 1, sizeof(abuf), a);
        size_t bn = fread(bbuf, 1, sizeof(bbuf), b);
        size_t n = an < bn ? an : bn;
        size_t i;
        for (i = 0; i < n; ++i) {
            if (abuf[i] != bbuf[i]) {
                fprintf(stderr, "[!] Files differ at offset 0x%" PRIx64
                        ": 0x%02x != 0x%02x\n", offset + i, abuf[i], bbuf[i]);
                goto done;
            }
        }
        if (an != bn) {
            fprintf(stderr, "[!] Files differ in size near offset 0x%" PRIx64 "\n",
                    offset + n);
            goto done;
        }
        offset += an;
        if (an == 0) {
            if (ferror(a) || ferror(b)) {
                fprintf(stderr, "[!] File comparison read failed\n");
                goto done;
            }
            same = true;
            break;
        }
    }
    fprintf(stderr, "[+] Files are identical (%" PRIu64 " bytes)\n", offset);
done:
    fclose(a);
    fclose(b);
    return same;
}

static bool join_command(char *line, size_t line_size, const words *args, size_t start) {
    size_t i, pos = 0;
    line[0] = '\0';
    for (i = start; i < args->argc; ++i) {
        int needed = snprintf(line + pos, line_size - pos, "%s%s",
                              i > start ? " " : "", args->argv[i]);
        if (needed < 0 || (size_t)needed >= line_size - pos)
            return false;
        pos += (size_t)needed;
    }
    return true;
}

static bool dispatch(gx_context *ctx, const words *args, bool from_config) {
    const char *cmd;
    char line[1024];
    size_t size, address;
    if (args->argc == 0)
        return false;
    cmd = args->argv[0];
    if (strcmp(cmd, "serialdump") == 0) {
        if (args->argc != 4 || !parse_size(args->argv[2], &size)) {
            fprintf(stderr, "[!] Usage: serialdump <partition|addr> <size> <file>\n");
            return false;
        }
        snprintf(line, sizeof(line), "serialdump %s %s", args->argv[1], args->argv[2]);
        return binary_read(ctx, line, size, args->argv[3]);
    }
    if (strcmp(cmd, "serialdown") == 0) {
        if (args->argc != 3) {
            fprintf(stderr, "[!] Usage: serialdown <partition|addr> <file>\n");
            return false;
        }
        snprintf(line, sizeof(line), "serialdown %s", args->argv[1]);
        return binary_write(ctx, line, args->argv[2], true);
    }
    if (strcmp(cmd, "usbdump") == 0) {
        if (args->argc != 4 || !parse_size(args->argv[2], &size)) {
            fprintf(stderr, "[!] Usage: usbdump <partition|addr> <size> <file>\n");
            return false;
        }
        snprintf(line, sizeof(line), "usbdump %s %s %s", args->argv[1], args->argv[2],
                 args->argv[3]);
        return text_command(ctx, line, 120000);
    }
    if (strcmp(cmd, "usbdown") == 0) {
        if (args->argc != 3) {
            fprintf(stderr, "[!] Usage: usbdown <partition|addr> <file>\n");
            return false;
        }
        fprintf(stderr, "[!] WARNING: USB download erases and writes flash\n");
        snprintf(line, sizeof(line), "usbdown %s %s", args->argv[1], args->argv[2]);
        return text_command(ctx, line, 300000);
    }
    if (strcmp(cmd, "gx_otp") == 0) {
        if (args->argc < 2) goto gx_usage;
        if (strcmp(args->argv[1], "read") == 0) {
            if (args->argc != 5 || !parse_size(args->argv[2], &address) ||
                !parse_size(args->argv[3], &size)) goto gx_usage;
            snprintf(line, sizeof(line), "gx_otp read %s %s", args->argv[2], args->argv[3]);
            return binary_read(ctx, line, size, args->argv[4]);
        }
        if (strcmp(args->argv[1], "tread") == 0) {
            if (args->argc != 4 || !parse_size(args->argv[2], &address) ||
                !parse_size(args->argv[3], &size)) goto gx_usage;
            snprintf(line, sizeof(line), "gx_otp tread %s %s", args->argv[2], args->argv[3]);
            return text_command(ctx, line, 5000);
        }
        if (strcmp(args->argv[1], "write") == 0) {
            if (args->argc != 4 || !parse_size(args->argv[2], &address)) goto gx_usage;
            snprintf(line, sizeof(line), "gx_otp write %s", args->argv[2]);
            return binary_write(ctx, line, args->argv[3], false);
        }
        if (strcmp(args->argv[1], "twrite") == 0) {
            if (args->argc != 4 || !parse_size(args->argv[2], &address)) goto gx_usage;
            fprintf(stderr, "[!] WARNING: GX OTP writes may be irreversible\n");
            snprintf(line, sizeof(line), "gx_otp twrite %s %s", args->argv[2], args->argv[3]);
            return text_command(ctx, line, 30000);
        }
gx_usage:
        fprintf(stderr, "[!] Usage: gx_otp <read|tread|write|twrite> ...\n");
        return false;
    }
    if (strcmp(cmd, "sflash_otp") == 0) {
        if (args->argc < 2) goto sflash_usage;
        if (strcmp(args->argv[1], "status") == 0 && args->argc == 2)
            return text_command(ctx, "sflash_otp status", 5000);
        if (strcmp(args->argv[1], "getregion") == 0 && args->argc == 2)
            return text_command(ctx, "sflash_otp getregion", 5000);
        if (strcmp(args->argv[1], "read") == 0) {
            if (args->argc != 5 || !parse_size(args->argv[2], &address) ||
                !parse_size(args->argv[3], &size)) goto sflash_usage;
            snprintf(line, sizeof(line), "sflash_otp read %s %s", args->argv[2],
                     args->argv[3]);
            return binary_read(ctx, line, size, args->argv[4]);
        }
        if (strcmp(args->argv[1], "write") == 0) {
            if (args->argc != 4 || !parse_size(args->argv[2], &address)) goto sflash_usage;
            snprintf(line, sizeof(line), "sflash_otp write %s", args->argv[2]);
            return binary_write(ctx, line, args->argv[3], false);
        }
        if (strcmp(args->argv[1], "erase") == 0 && args->argc == 2) {
            fprintf(stderr, "[!] WARNING: SPI flash OTP erase may be irreversible\n");
            return text_command(ctx, "sflash_otp erase", 30000);
        }
        if (strcmp(args->argv[1], "lock") == 0 && args->argc == 2) {
            if (!confirm(ctx, "This will lock SPI Flash OTP."))
                return false;
            return text_command(ctx, "sflash_otp lock", 30000);
        }
        if (strcmp(args->argv[1], "setregion") == 0 && args->argc == 3) {
            if (!parse_size(args->argv[2], &size))
                goto sflash_usage;
            if (!confirm(ctx, "This will set SPI Flash OTP region selection."))
                return false;
            snprintf(line, sizeof(line), "sflash_otp setregion %s", args->argv[2]);
            return text_command(ctx, line, 5000);
        }
sflash_usage:
        fprintf(stderr, "[!] Usage: sflash_otp <status|getregion|read|write|erase|lock|setregion> ...\n");
        return false;
    }
    if (strcmp(cmd, "flash") == 0) {
        if (args->argc == 2 && strcmp(args->argv[1], "badinfo") == 0)
            return text_command(ctx, "flash badinfo", 10000);
        if (args->argc == 2 && strcmp(args->argv[1], "eraseall") == 0) {
            if (!confirm(ctx, "This will erase the entire serial flash and may brick the device."))
                return false;
            return text_command(ctx, "flash eraseall", 300000);
        }
        if (args->argc >= 3 && strcmp(args->argv[1], "erase") == 0) {
            size_t i = 2;
            bool nospread = strcmp(args->argv[i], "nospread") == 0;
            if (nospread) ++i;
            if (i >= args->argc || args->argc > i + 2) goto flash_usage;
            if (!confirm(ctx, "This will erase flash data."))
                return false;
            if (args->argc == i + 2) {
                if (!parse_size(args->argv[i + 1], &size)) return false;
                snprintf(line, sizeof(line), "flash erase %s%s %s",
                         nospread ? "nospread " : "", args->argv[i], args->argv[i + 1]);
            } else {
                snprintf(line, sizeof(line), "flash erase %s%s",
                         nospread ? "nospread " : "", args->argv[i]);
            }
            return text_command(ctx, line, 120000);
        }
        if (args->argc >= 2 && strcmp(args->argv[1], "scrub") == 0) {
            if (args->argc == 3 && strcmp(args->argv[2], "all") == 0) {
                if (!confirm(ctx, "This will scrub NAND and can discard factory bad-block markers."))
                    return false;
                return text_command(ctx, "flash scrub all", 300000);
            }
            if (args->argc != 4 || !parse_size(args->argv[2], &address) ||
                !parse_size(args->argv[3], &size)) {
                fprintf(stderr, "[!] Usage: flash scrub <flash addr> <length>\n");
                fprintf(stderr, "[!]        flash scrub all\n");
                return false;
            }
            if (!confirm(ctx, "This will scrub NAND and can discard factory bad-block markers."))
                return false;
            snprintf(line, sizeof(line), "flash scrub %s %s", args->argv[2], args->argv[3]);
            return text_command(ctx, line, 120000);
        }
        if (args->argc == 4 && strcmp(args->argv[1], "mark") == 0 &&
            strcmp(args->argv[2], "bad") == 0) {
            if (!parse_size(args->argv[3], &address))
                goto flash_usage;
            if (!confirm(ctx, "This will mark a NAND block as bad."))
                return false;
            snprintf(line, sizeof(line), "flash mark bad %s", args->argv[3]);
            return text_command(ctx, line, 30000);
        }
flash_usage:
        fprintf(stderr, "[!] Usage: flash <erase [nospread] target [length]|badinfo|eraseall|scrub|mark bad>\n");
        return false;
    }
    if (strcmp(cmd, "netdump") == 0) {
        if (args->argc != 4 || !parse_size(args->argv[2], &size)) {
            fprintf(stderr, "[!] Usage: netdump <partition|addr> <size> <file>\n");
            return false;
        }
        return gx_net_dump(ctx, args->argv[1], args->argv[2], size, args->argv[3]);
    }
    if (strcmp(cmd, "netdown") == 0) {
        if (args->argc != 3) {
            fprintf(stderr, "[!] Usage: netdown <partition|addr> <file>\n");
            return false;
        }
        if (!confirm(ctx, "netdown writes flash over TFTP and can brick the device."))
            return false;
        return gx_net_download(ctx, args->argv[1], args->argv[2]);
    }
    if (strcmp(cmd, "net") == 0) {
        if (!join_command(line, sizeof(line), args, 0)) {
            fprintf(stderr, "[!] net command is too long\n");
            return false;
        }
        return text_command(ctx, line, 30000);
    }
    if (strcmp(cmd, "compare") == 0) {
        if (args->argc != 3) {
            fprintf(stderr, "[!] Usage: compare <src_file> <dst_file>\n");
            return false;
        }
        return gx_compare_files(args->argv[1], args->argv[2]);
    }
    if (strcmp(cmd, "load_conf_down") == 0 && !from_config) {
        if (args->argc < 3 || args->argc > 4) {
            fprintf(stderr, "[!] Usage: load_conf_down <config_file> <transport> [path]\n");
            return false;
        }
        fprintf(stderr, "[*] Loading config via %s: %s\n", args->argv[2], args->argv[1]);
        return gx_run_config(ctx, args->argv[1]);
    }
    fprintf(stderr, "[!] Unknown or unsupported command: %s\n", cmd);
    fprintf(stderr, "[*] Available commands: serialdump, serialdown, usbdump, usbdown,\n");
    fprintf(stderr, "    gx_otp, sflash_otp, flash, compare, load_conf_down, netdump, netdown, net\n");
    return false;
}

bool gx_run_command(gx_context *ctx, const char *command_line) {
    words args;
    bool result;
    if (!split_words(command_line, &args) || args.argc == 0) {
        fprintf(stderr, "[!] Empty command\n");
        return false;
    }
    result = dispatch(ctx, &args, false);
    free_words(&args);
    return result;
}

bool gx_run_config(gx_context *ctx, const char *path) {
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    size_t count = 0;
    bool result = false;
    if (!file) {
        fprintf(stderr, "[!] Cannot open config %s: %s\n", path, strerror(errno));
        return false;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char *start = line;
        words args;
        while (*start && isspace((unsigned char)*start)) ++start;
        if (!*start || *start == '#')
            continue;
        while (length > 0 && isspace((unsigned char)line[length - 1]))
            line[--length] = '\0';
        if (!split_words(start, &args) || args.argc == 0)
            goto done;
        if (!dispatch(ctx, &args, true)) {
            fprintf(stderr, "[!] Config failed on command: %s", start);
            free_words(&args);
            goto done;
        }
        free_words(&args);
        ++count;
    }
    if (ferror(file)) {
        fprintf(stderr, "[!] Error reading config %s\n", path);
        goto done;
    }
    if (count == 0) {
        fprintf(stderr, "[!] Config contained no runnable commands\n");
        goto done;
    }
    fprintf(stderr, "[+] Executed %zu commands from %s\n", count, path);
    result = true;
done:
    free(line);
    fclose(file);
    return result;
}
