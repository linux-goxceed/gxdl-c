#define _POSIX_C_SOURCE 200809L
#include "gxdl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

extern bool gx_internal_wait_marker(gx_context *, gx_buffer *, const char *, int, bool);
extern bool gx_internal_append_read(gx_context *, gx_buffer *, int);

#define TFTP_RRQ 1
#define TFTP_WRQ 2
#define TFTP_DATA 3
#define TFTP_ACK 4
#define TFTP_ERROR 5
#define TFTP_OACK 6
#define TFTP_BLOCK 1024U
#define TFTP_MAX_PAYLOAD 8192U

typedef enum {
    PHASE_WAIT_REQUEST = 0,
    PHASE_RECV,
    PHASE_OACK,
    PHASE_SEND,
    PHASE_DONE,
    PHASE_FAIL
} tftp_phase;

typedef struct {
    int fd;
    bool sending;
    bool verbose;
    tftp_phase phase;
    struct sockaddr_in peer;
    socklen_t peer_len;
    unsigned advertised;
    unsigned blksize;
    uint16_t expected;
    uint16_t block;
    unsigned retries;
    unsigned oack_tries;
    size_t expected_size;
    size_t transferred;
    FILE *file;
    const uint8_t *send_data;
    size_t send_size;
    size_t send_offset;
    uint8_t oack[128];
    size_t oack_len;
    char error[160];
} gx_tftp;

bool gx_detect_local_ip(char *buf, size_t size) {
    int fd;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (!buf || size < 8U)
        return false;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    if (inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) != 0 ||
        !inet_ntop(AF_INET, &addr.sin_addr, buf, (socklen_t)size)) {
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

bool gx_next_ipv4(const char *ip, char *buf, size_t size) {
    struct in_addr addr;
    uint32_t value;
    if (!ip || !buf || size < 8U || inet_pton(AF_INET, ip, &addr) != 1)
        return false;
    value = ntohl(addr.s_addr) + 1U;
    addr.s_addr = htonl(value);
    return inet_ntop(AF_INET, &addr, buf, (socklen_t)size) != NULL;
}

static void tftp_fail(gx_tftp *tftp, const char *message) {
    if (tftp->phase != PHASE_FAIL && tftp->phase != PHASE_DONE)
        snprintf(tftp->error, sizeof(tftp->error), "%s", message);
    tftp->phase = PHASE_FAIL;
}

static int tftp_bind(const char *bind_ip, unsigned int port) {
    int fd;
    int reuse = 1;
    struct sockaddr_in addr;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[!] TFTP socket: %s\n", strerror(errno));
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        fprintf(stderr, "[!] TFTP SO_REUSEADDR: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "[!] Invalid TFTP bind address: %s\n", bind_ip);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[!] TFTP bind %s:%u: %s\n", bind_ip, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void tftp_send_error(gx_tftp *tftp, const struct sockaddr_in *addr,
                            socklen_t addr_len, uint16_t code, const char *message) {
    uint8_t packet[128];
    size_t length = strlen(message);
    if (length > sizeof(packet) - 5U)
        length = sizeof(packet) - 5U;
    gx_write_be16(packet, TFTP_ERROR);
    gx_write_be16(packet + 2, code);
    memcpy(packet + 4, message, length);
    packet[4 + length] = 0;
    (void)sendto(tftp->fd, packet, 5U + length, 0, (const struct sockaddr *)addr,
                 addr_len);
}

static void tftp_ack(gx_tftp *tftp, uint16_t block) {
    uint8_t packet[4];
    gx_write_be16(packet, TFTP_ACK);
    gx_write_be16(packet + 2, block);
    (void)sendto(tftp->fd, packet, 4, 0, (const struct sockaddr *)&tftp->peer,
                 tftp->peer_len);
}

static unsigned parse_blksize(const uint8_t *data, size_t size) {
    size_t i = 2;
    unsigned zeros = 0;
    unsigned blksize = TFTP_BLOCK;
    while (i < size) {
        const uint8_t *start = data + i;
        const uint8_t *zero = memchr(start, 0, size - i);
        size_t token_len;
        if (!zero)
            break;
        token_len = (size_t)(zero - start);
        i = (size_t)(zero - data) + 1U;
        ++zeros;
        if (zeros >= 3U && zeros % 2U == 1U && token_len == 7U &&
            strncasecmp((const char *)start, "blksize", 7) == 0 && i < size) {
            blksize = (unsigned)strtoul((const char *)(data + i), NULL, 10);
            if (blksize < 8U || blksize > TFTP_MAX_PAYLOAD)
                blksize = TFTP_BLOCK;
            break;
        }
    }
    return blksize;
}

static void build_oack(gx_tftp *tftp, unsigned blksize) {
    static const char key[] = "blksize";
    char value[16];
    int n = snprintf(value, sizeof(value), "%u", blksize);
    gx_write_be16(tftp->oack, TFTP_OACK);
    memcpy(tftp->oack + 2, key, sizeof(key));
    memcpy(tftp->oack + 2 + sizeof(key), value, (size_t)n + 1U);
    tftp->oack_len = 2U + sizeof(key) + (size_t)n + 1U;
}

static size_t current_chunk(const gx_tftp *tftp) {
    size_t remaining = tftp->send_size - tftp->send_offset;
    return remaining > tftp->blksize ? tftp->blksize : remaining;
}

static void send_data_block(gx_tftp *tftp) {
    uint8_t packet[4 + TFTP_MAX_PAYLOAD];
    size_t chunk = current_chunk(tftp);
    gx_write_be16(packet, TFTP_DATA);
    gx_write_be16(packet + 2, tftp->block);
    memcpy(packet + 4, tftp->send_data + tftp->send_offset, chunk);
    (void)sendto(tftp->fd, packet, 4U + chunk, 0, (const struct sockaddr *)&tftp->peer,
                 tftp->peer_len);
}

static void begin_send_data(gx_tftp *tftp) {
    tftp->phase = PHASE_SEND;
    tftp->block = 1;
    tftp->send_offset = 0;
    tftp->retries = 0;
    send_data_block(tftp);
}

static void handle_wrq(gx_tftp *tftp, const uint8_t *data, size_t size,
                       const struct sockaddr_in *addr, socklen_t addr_len) {
    tftp->peer = *addr;
    tftp->peer_len = addr_len;
    tftp->advertised = parse_blksize(data, size);
    tftp->blksize = TFTP_BLOCK;
    tftp->expected = 1;
    tftp->retries = 0;
    tftp->transferred = 0;
    if (tftp->file) {
        rewind(tftp->file);
        if (ftruncate(fileno(tftp->file), 0) != 0) {
            tftp_fail(tftp, "cannot truncate TFTP output");
            return;
        }
    }
    tftp_ack(tftp, 0);
    tftp->phase = PHASE_RECV;
}

static void handle_rrq(gx_tftp *tftp, const uint8_t *data, size_t size,
                       const struct sockaddr_in *addr, socklen_t addr_len) {
    tftp->peer = *addr;
    tftp->peer_len = addr_len;
    tftp->blksize = parse_blksize(data, size);
    tftp->oack_tries = 0;
    tftp->retries = 0;
    build_oack(tftp, tftp->blksize);
    (void)sendto(tftp->fd, tftp->oack, tftp->oack_len, 0,
                 (const struct sockaddr *)&tftp->peer, tftp->peer_len);
    tftp->phase = PHASE_OACK;
}

static void handle_data(gx_tftp *tftp, const uint8_t *data, size_t size) {
    uint16_t block;
    const uint8_t *chunk;
    size_t chunk_len;
    if (size < 4U)
        return;
    block = gx_read_be16(data + 2);
    chunk = data + 4;
    chunk_len = size - 4U;
    if (tftp->expected == 1U && chunk_len > tftp->blksize)
        tftp->blksize = (unsigned)chunk_len;
    if (block == tftp->expected) {
        if (tftp->file && fwrite(chunk, 1, chunk_len, tftp->file) != chunk_len) {
            tftp_fail(tftp, "TFTP write failed");
            return;
        }
        tftp->transferred += chunk_len;
        tftp_ack(tftp, block);
        tftp->retries = 0;
        if (chunk_len < tftp->blksize ||
            (tftp->expected_size && tftp->transferred >= tftp->expected_size)) {
            tftp->phase = PHASE_DONE;
            return;
        }
        tftp->expected = (uint16_t)((tftp->expected + 1U) & 0xFFFFU);
    } else if (block == (uint16_t)((tftp->expected - 1U) & 0xFFFFU)) {
        tftp_ack(tftp, block);
    }
}

static void handle_ack(gx_tftp *tftp, const uint8_t *data, size_t size) {
    uint16_t block;
    size_t chunk;
    if (size < 4U)
        return;
    block = gx_read_be16(data + 2);
    if (tftp->phase == PHASE_OACK) {
        if (block == 0)
            begin_send_data(tftp);
        return;
    }
    if (tftp->phase != PHASE_SEND || block != tftp->block)
        return;
    chunk = current_chunk(tftp);
    tftp->send_offset += chunk;
    tftp->transferred = tftp->send_offset;
    tftp->retries = 0;
    if (chunk < tftp->blksize || tftp->send_offset >= tftp->send_size) {
        tftp->phase = PHASE_DONE;
        return;
    }
    tftp->block = (uint16_t)((tftp->block + 1U) & 0xFFFFU);
    send_data_block(tftp);
}

static void tftp_on_packet(gx_tftp *tftp, const uint8_t *data, size_t size,
                           const struct sockaddr_in *addr, socklen_t addr_len) {
    uint16_t opcode;
    if (size < 2U)
        return;
    opcode = gx_read_be16(data);
    if (tftp->verbose)
        fprintf(stderr, "[*] TFTP opcode %u from %s:%u\n", opcode,
                inet_ntoa(addr->sin_addr), (unsigned)ntohs(addr->sin_port));
    if (tftp->phase == PHASE_WAIT_REQUEST) {
        if (!tftp->sending && opcode == TFTP_WRQ) {
            handle_wrq(tftp, data, size, addr, addr_len);
            return;
        }
        if (tftp->sending && opcode == TFTP_RRQ) {
            handle_rrq(tftp, data, size, addr, addr_len);
            return;
        }
        tftp_send_error(tftp, addr, addr_len, 4, "Unexpected TFTP opcode");
        return;
    }
    if (opcode == TFTP_WRQ && !tftp->sending) {
        handle_wrq(tftp, data, size, addr, addr_len);
        return;
    }
    if (opcode == TFTP_DATA && tftp->phase == PHASE_RECV) {
        handle_data(tftp, data, size);
        return;
    }
    if (opcode == TFTP_ACK && (tftp->phase == PHASE_OACK || tftp->phase == PHASE_SEND)) {
        handle_ack(tftp, data, size);
        return;
    }
}

static bool tftp_readable(gx_tftp *tftp) {
    uint8_t packet[4 + TFTP_MAX_PAYLOAD];
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    ssize_t n = recvfrom(tftp->fd, packet, sizeof(packet), MSG_DONTWAIT,
                         (struct sockaddr *)&addr, &addr_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        tftp_fail(tftp, strerror(errno));
        return false;
    }
    tftp_on_packet(tftp, packet, (size_t)n, &addr, addr_len);
    return tftp->phase != PHASE_FAIL;
}

static void tftp_timeout(gx_tftp *tftp) {
    if (tftp->phase == PHASE_RECV) {
        tftp->retries += 1U;
        if (tftp->retries > 8U) {
            if (tftp->transferred > 0U && tftp->blksize &&
                tftp->transferred % tftp->blksize == 0U) {
                tftp->phase = PHASE_DONE;
                return;
            }
            tftp_fail(tftp, "timed out waiting for TFTP DATA");
            return;
        }
        tftp_ack(tftp, tftp->expected > 1U ? (uint16_t)(tftp->expected - 1U) : 0);
        return;
    }
    if (tftp->phase == PHASE_OACK) {
        tftp->oack_tries += 1U;
        if (tftp->oack_tries >= 9U) {
            begin_send_data(tftp);
            return;
        }
        (void)sendto(tftp->fd, tftp->oack, tftp->oack_len, 0,
                     (const struct sockaddr *)&tftp->peer, tftp->peer_len);
        return;
    }
    if (tftp->phase == PHASE_SEND) {
        tftp->retries += 1U;
        if (tftp->retries > 8U) {
            if (tftp->send_offset + current_chunk(tftp) >= tftp->send_size) {
                tftp->phase = PHASE_DONE;
                return;
            }
            tftp_fail(tftp, "timed out waiting for TFTP ACK");
            return;
        }
        send_data_block(tftp);
    }
}

static bool print_serial_tail(gx_buffer *buffer) {
    ssize_t prompt = gx_buffer_find(buffer, "boot>", 5);
    if (prompt > 0) {
        size_t length = (size_t)prompt;
        while (length && (buffer->data[length - 1U] == '\r' ||
                          buffer->data[length - 1U] == '\n'))
            --length;
        if (length) {
            fwrite(buffer->data, 1, length, stdout);
            fputc('\n', stdout);
        }
        return true;
    }
    return gx_buffer_find(buffer, "boot>", 5) == 0;
}

static bool resolve_net_ips(gx_context *ctx, char *pcip, size_t pcip_size,
                            char *stbip, size_t stbip_size, unsigned int *port) {
    struct in_addr addr;
    *port = ctx->tftp_port ? ctx->tftp_port : GX_TFTP_PORT;
    if (ctx->pcip) {
        if (inet_pton(AF_INET, ctx->pcip, &addr) != 1) {
            fprintf(stderr, "[!] Invalid --pcip address: %s\n", ctx->pcip);
            return false;
        }
        snprintf(pcip, pcip_size, "%s", ctx->pcip);
    } else if (!gx_detect_local_ip(pcip, pcip_size)) {
        fprintf(stderr, "[!] Cannot auto-detect host IP; pass -p/--pcip\n");
        return false;
    }
    if (ctx->stbip) {
        if (inet_pton(AF_INET, ctx->stbip, &addr) != 1) {
            fprintf(stderr, "[!] Invalid --stbip address: %s\n", ctx->stbip);
            return false;
        }
        snprintf(stbip, stbip_size, "%s", ctx->stbip);
    } else if (!gx_next_ipv4(pcip, stbip, stbip_size)) {
        fprintf(stderr, "[!] Cannot derive board IP from %s; pass -s/--stbip\n", pcip);
        return false;
    }
    return true;
}

static bool net_configure(gx_context *ctx, const char *stbip, unsigned int port) {
    char command[96];
    gx_buffer response;
    bool ok;
    snprintf(command, sizeof(command), "config ip %s", stbip);
    if (!gx_command_begin(ctx, command, &response))
        return false;
    ok = gx_internal_wait_marker(ctx, &response, "boot>", 30000, false);
    print_serial_tail(&response);
    gx_buffer_free(&response);
    if (!ok) {
        fprintf(stderr, "[!] Timeout waiting for response to: %s\n", command);
        return false;
    }
    snprintf(command, sizeof(command), "config tftpport %u", port);
    if (!gx_command_begin(ctx, command, &response))
        return false;
    ok = gx_internal_wait_marker(ctx, &response, "boot>", 10000, false);
    print_serial_tail(&response);
    gx_buffer_free(&response);
    if (!ok) {
        fprintf(stderr, "[!] Timeout waiting for response to: %s\n", command);
        return false;
    }
    return true;
}

static bool run_tftp_with_serial(gx_context *ctx, gx_tftp *tftp, const char *command,
                                 int timeout_ms) {
    gx_buffer serial;
    int64_t deadline = gx_now_ms() + timeout_ms;
    int64_t next_tick = gx_now_ms() + 1000;
    bool got_prompt = false;
    if (!gx_command_begin(ctx, command, &serial))
        return false;
    if (gx_buffer_find(&serial, "boot>", 5) >= 0)
        got_prompt = true;
    while (gx_now_ms() < deadline) {
        struct pollfd fds[2];
        int wait;
        int64_t now = gx_now_ms();
        fds[0].fd = ctx->serial.fd;
        fds[0].events = POLLIN;
        fds[1].fd = tftp->fd;
        fds[1].events = POLLIN;
        wait = (int)(deadline - now);
        if (wait > 100)
            wait = 100;
        if (wait < 0)
            wait = 0;
        if (poll(fds, 2, wait) < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "[!] poll: %s\n", strerror(errno));
            gx_buffer_free(&serial);
            return false;
        }
        if (fds[0].revents & POLLIN) {
            if (!gx_internal_append_read(ctx, &serial, 50) && errno != ETIMEDOUT) {
                gx_buffer_free(&serial);
                return false;
            }
            if (gx_buffer_find(&serial, "boot>", 5) >= 0)
                got_prompt = true;
        }
        if (fds[1].revents & POLLIN)
            tftp_readable(tftp);
        now = gx_now_ms();
        if (now >= next_tick) {
            tftp_timeout(tftp);
            next_tick = now + 1000;
        }
        if (tftp->phase == PHASE_FAIL) {
            fprintf(stderr, "[!] TFTP failed: %s\n", tftp->error);
            print_serial_tail(&serial);
            gx_buffer_free(&serial);
            return false;
        }
        if (got_prompt && (tftp->phase == PHASE_DONE || tftp->phase == PHASE_FAIL))
            break;
        if (tftp->phase == PHASE_DONE && got_prompt)
            break;
    }
    print_serial_tail(&serial);
    gx_buffer_free(&serial);
    if (tftp->phase != PHASE_DONE) {
        fprintf(stderr, "[!] TFTP %s\n", tftp->error[0] ? tftp->error : "timeout");
        return false;
    }
    return true;
}

static bool init_tftp(gx_tftp *tftp, gx_context *ctx, const char *pcip,
                      unsigned int port, bool sending) {
    memset(tftp, 0, sizeof(*tftp));
    tftp->fd = tftp_bind(pcip, port);
    tftp->sending = sending;
    tftp->verbose = ctx->verbose;
    tftp->phase = PHASE_WAIT_REQUEST;
    tftp->peer_len = sizeof(tftp->peer);
    tftp->blksize = TFTP_BLOCK;
    return tftp->fd >= 0;
}

static void close_tftp(gx_tftp *tftp) {
    if (tftp->file)
        fclose(tftp->file);
    if (tftp->fd >= 0)
        close(tftp->fd);
    tftp->file = NULL;
    tftp->fd = -1;
}

bool gx_net_dump(gx_context *ctx, const char *target, const char *size_token,
                 size_t size, const char *output) {
    char pcip[64], stbip[64], command[256];
    unsigned int port;
    gx_tftp tftp;
    int timeout;
    bool ok = false;
    if (!resolve_net_ips(ctx, pcip, sizeof(pcip), stbip, sizeof(stbip), &port))
        return false;
    fprintf(stderr, "[*] TFTP netdump: board %s -> host %s:%u\n", stbip, pcip, port);
    if (!init_tftp(&tftp, ctx, pcip, port, false))
        return false;
    tftp.file = fopen(output, "wb");
    if (!tftp.file) {
        fprintf(stderr, "[!] Cannot create %s: %s\n", output, strerror(errno));
        close_tftp(&tftp);
        return false;
    }
    tftp.expected_size = size;
    if (!net_configure(ctx, stbip, port))
        goto done;
    snprintf(command, sizeof(command), "netdump %s %s %s %s", target, pcip,
             gx_path_basename(output), size_token);
    timeout = (int)(size / 50000U + 20U);
    if (timeout < 30)
        timeout = 30;
    if (timeout > 3600)
        timeout = 3600;
    fprintf(stderr, "[*] %s\n", command);
    if (!run_tftp_with_serial(ctx, &tftp, command, timeout * 1000))
        goto done;
    if (tftp.transferred == 0) {
        fprintf(stderr, "[!] TFTP receive failed: empty file\n");
        goto done;
    }
    fprintf(stderr, "[+] Wrote %zu bytes to %s\n", tftp.transferred, output);
    ok = true;
done:
    close_tftp(&tftp);
    return ok;
}

bool gx_net_download(gx_context *ctx, const char *target, const char *input_file) {
    char pcip[64], stbip[64], command[320];
    unsigned int port;
    gx_tftp tftp;
    uint8_t *data = NULL;
    size_t size = 0;
    int timeout;
    bool ok = false;
    if (!gx_read_file(input_file, &data, &size))
        return false;
    if (!resolve_net_ips(ctx, pcip, sizeof(pcip), stbip, sizeof(stbip), &port)) {
        free(data);
        return false;
    }
    fprintf(stderr, "[*] TFTP netdown: host %s:%u -> board %s\n", pcip, port, stbip);
    if (!init_tftp(&tftp, ctx, pcip, port, true)) {
        free(data);
        return false;
    }
    tftp.send_data = data;
    tftp.send_size = size;
    if (!net_configure(ctx, stbip, port))
        goto done;
    snprintf(command, sizeof(command), "partition download %s %s \"%s\" %zu", target,
             pcip, gx_path_basename(input_file), size);
    timeout = (int)(size / 30000U + 30U);
    if (timeout < 60)
        timeout = 60;
    if (timeout > 3600)
        timeout = 3600;
    fprintf(stderr, "[*] %s\n", command);
    if (!run_tftp_with_serial(ctx, &tftp, command, timeout * 1000))
        goto done;
    fprintf(stderr, "[+] Sent %zu bytes from %s\n", size, input_file);
    ok = true;
done:
    close_tftp(&tftp);
    free(data);
    return ok;
}
