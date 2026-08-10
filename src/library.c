#define _POSIX_C_SOURCE 200809L
#include "gxdl/gxdl.h"
#include "gxdl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gxdl_session {
    gx_context context;
    char *device;
    char error[160];
};

static char *copy_string(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy)
        memcpy(copy, value, length);
    return copy;
}

static int failed(gxdl_session *session, const char *message) {
    if (session)
        (void)snprintf(session->error, sizeof(session->error), "%s", message);
    return -1;
}

void gxdl_options_init(gxdl_options *options) {
    if (!options)
        return;
    memset(options, 0, sizeof(*options));
    options->baud = 115200U;
}

gxdl_session *gxdl_open(const gxdl_options *options) {
    gxdl_session *session;
    unsigned int baud;
    if (!options || !options->device || options->device[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    session = calloc(1, sizeof(*session));
    if (!session)
        return NULL;
    session->context.serial.fd = -1;
    session->device = copy_string(options->device);
    if (!session->device) {
        free(session);
        return NULL;
    }
    baud = options->baud ? options->baud : 115200U;
    session->context.verbose = options->verbose;
    session->context.assume_yes = options->assume_yes;
    session->context.reset_dtr = options->reset_dtr;
    session->context.reset_rts = options->reset_rts;
    if (gx_serial_open(&session->context.serial, session->device, baud,
                       options->verbose) != 0) {
        int saved_errno = errno;
        free(session->device);
        free(session);
        errno = saved_errno;
        return NULL;
    }
    return session;
}

void gxdl_close(gxdl_session *session) {
    if (!session)
        return;
    gx_serial_close(&session->context.serial);
    free(session->device);
    free(session);
}

int gxdl_boot_file(gxdl_session *session, const char *path) {
    gx_loader loader;
    bool result;
    if (!session || !path) {
        errno = EINVAL;
        return -1;
    }
    if (!gx_loader_from_file(path, &loader))
        return failed(session, "failed to load external boot image");
    result = gx_boot(&session->context, &loader, true);
    gx_loader_release(&loader);
    return result ? 0 : failed(session, "bootloader upload failed");
}

int gxdl_boot_model(gxdl_session *session, const char *model) {
    gx_loader loader;
    bool result;
    if (!session || !model) {
        errno = EINVAL;
        return -1;
    }
    if (!gx_loader_from_model(model, &loader))
        return failed(session, "embedded loader was not found or is invalid");
    result = gx_boot(&session->context, &loader, true);
    gx_loader_release(&loader);
    return result ? 0 : failed(session, "bootloader upload failed");
}

int gxdl_wait_for_prompt(gxdl_session *session, unsigned int timeout_ms) {
    if (!session) {
        errno = EINVAL;
        return -1;
    }
    if (timeout_ms == 0U)
        timeout_ms = 5000U;
    if (timeout_ms > (unsigned int)INT32_MAX) {
        errno = EINVAL;
        return failed(session, "prompt timeout is too large");
    }
    return gx_wait_prompt(&session->context, (int)timeout_ms)
        ? 0 : failed(session, "boot prompt was not received");
}

int gxdl_run(gxdl_session *session, const char *command) {
    if (!session || !command || command[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    return gx_run_command(&session->context, command)
        ? 0 : failed(session, "downloader command failed");
}

int gxdl_run_config_file(gxdl_session *session, const char *path) {
    if (!session || !path) {
        errno = EINVAL;
        return -1;
    }
    return gx_run_config(&session->context, path)
        ? 0 : failed(session, "config execution failed");
}

const char *gxdl_last_error(const gxdl_session *session) {
    if (!session)
        return "invalid libgxdl session";
    return session->error[0] ? session->error : "no error";
}

size_t gxdl_model_count(void) {
    return gx_embedded_loader_count();
}

const char *gxdl_model_name(size_t index) {
    const gx_embedded_loader *loader = gx_embedded_loader_at(index);
    return loader ? loader->name : NULL;
}

int gxdl_compare(const char *left, const char *right) {
    if (!left || !right) {
        errno = EINVAL;
        return -1;
    }
    return gx_compare_files(left, right) ? 0 : 1;
}

const char *gxdl_version(void) {
    return "1.0.0";
}
