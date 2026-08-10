#ifndef LIBGXDL_GXDL_H
#define LIBGXDL_GXDL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GXDL_VERSION_MAJOR 1
#define GXDL_VERSION_MINOR 0
#define GXDL_VERSION_PATCH 0

/** Opaque connection to a GX bootloader serial port. */
typedef struct gxdl_session gxdl_session;

/** Options used by gxdl_open(). Initialize with gxdl_options_init(). */
typedef struct {
    const char *device;
    unsigned int baud;
    bool verbose;
    bool assume_yes;
    bool reset_dtr;
    bool reset_rts;
} gxdl_options;

/** Fill options with defaults (115200 baud, all flags disabled). */
void gxdl_options_init(gxdl_options *options);

/**
 * Open and configure a serial port. Returns NULL on failure and sets errno.
 * The device string is copied and may be released by the caller afterwards.
 */
gxdl_session *gxdl_open(const gxdl_options *options);

/** Close the serial port and release a session. NULL is accepted. */
void gxdl_close(gxdl_session *session);

/** Upload an external .boot image and wait for the downloader prompt. Returns 0 on success. */
int gxdl_boot_file(gxdl_session *session, const char *path);

/** Upload an embedded loader by filename stem and wait for the prompt. Returns 0 on success. */
int gxdl_boot_model(gxdl_session *session, const char *model);

/** Wait until the connected downloader presents its boot> prompt. Returns 0 on success. */
int gxdl_wait_for_prompt(gxdl_session *session, unsigned int timeout_ms);

/** Execute one command using the same syntax accepted by gxdl-c -c. Returns 0 on success. */
int gxdl_run(gxdl_session *session, const char *command);

/** Execute a load_conf_down-style command file in the current session. Returns 0 on success. */
int gxdl_run_config_file(gxdl_session *session, const char *path);

/** Return a short description of the most recent failed library operation. */
const char *gxdl_last_error(const gxdl_session *session);

/** Enumerate loaders compiled into this libgxdl build. */
size_t gxdl_model_count(void);
const char *gxdl_model_name(size_t index);

/** Host comparison: 0 means identical, 1 means different, and -1 means invalid arguments. */
int gxdl_compare(const char *left, const char *right);

/** Compile-time libgxdl version string. */
const char *gxdl_version(void);

#ifdef __cplusplus
}
#endif

#endif
