#ifndef GXDL_H
#define GXDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define GX_TRANSFER_CHUNK 1024U
#define GX_STAGE2_CHUNK 2048U

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} gx_buffer;

typedef struct {
    int fd;
    const char *device;
    unsigned int baud;
    bool verbose;
} gx_serial;

typedef struct {
    const uint8_t *data;
    size_t size;
    uint16_t version;
    uint16_t chip;
    uint32_t baud;
    uint8_t *owned_data;
    const char *description;
} gx_loader;

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t size;
} gx_embedded_loader;

typedef struct {
    gx_serial serial;
    bool verbose;
    bool assume_yes;
    bool reset_dtr;
    bool reset_rts;
} gx_context;

uint16_t gx_read_le16(const uint8_t *p);
uint32_t gx_read_le32(const uint8_t *p);
void gx_write_le16(uint8_t *p, uint16_t value);
void gx_write_le32(uint8_t *p, uint32_t value);
void gx_write_be32(uint8_t *p, uint32_t value);
uint32_t gx_checksum(const uint8_t *data, size_t size);
int64_t gx_now_ms(void);
void gx_sleep_ms(unsigned int ms);
bool gx_parse_u64(const char *text, uint64_t *value);

void gx_buffer_init(gx_buffer *buffer);
void gx_buffer_free(gx_buffer *buffer);
bool gx_buffer_append(gx_buffer *buffer, const void *data, size_t size);
void gx_buffer_consume(gx_buffer *buffer, size_t size);
ssize_t gx_buffer_find(const gx_buffer *buffer, const void *needle, size_t size);

int gx_serial_open(gx_serial *serial, const char *device, unsigned int baud,
                   bool verbose);
void gx_serial_close(gx_serial *serial);
int gx_serial_write_all(gx_serial *serial, const void *data, size_t size,
                        int timeout_ms);
ssize_t gx_serial_read(gx_serial *serial, void *data, size_t size,
                       int timeout_ms);
int gx_serial_drain(gx_serial *serial);
int gx_serial_flush(gx_serial *serial);
int gx_serial_set_line(gx_serial *serial, int line, bool asserted);
int gx_serial_pulse_resets(gx_serial *serial, bool dtr, bool rts);

bool gx_loader_from_file(const char *path, gx_loader *loader);
bool gx_loader_from_model(const char *name, gx_loader *loader);
bool gx_loader_validate(gx_loader *loader);
void gx_loader_release(gx_loader *loader);
size_t gx_embedded_loader_count(void);
const gx_embedded_loader *gx_embedded_loader_at(size_t index);
const gx_embedded_loader *gx_embedded_loader_find(const char *name);

bool gx_build_stage1(const gx_loader *loader, uint8_t **packet, size_t *size);
bool gx_build_stage2(const gx_loader *loader, uint8_t **data, size_t *size,
                     uint8_t metadata[8]);
bool gx_boot(gx_context *ctx, const gx_loader *loader, bool read_output);
bool gx_wait_prompt(gx_context *ctx, int timeout_ms);
bool gx_run_command(gx_context *ctx, const char *command_line);
bool gx_run_config(gx_context *ctx, const char *path);
bool gx_compare_files(const char *left, const char *right);

#endif
