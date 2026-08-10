#include "gxdl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool gx_loader_validate(gx_loader *loader) {
    if (loader->size < 0x2020U) {
        fprintf(stderr, "[!] Boot image is too small: %zu bytes\n", loader->size);
        return false;
    }
    if (memcmp(loader->data, "toob", 4) != 0) {
        fprintf(stderr, "[!] Invalid boot image magic: %02x%02x%02x%02x\n",
                loader->data[0], loader->data[1], loader->data[2], loader->data[3]);
        return false;
    }
    loader->version = gx_read_le16(loader->data + 4);
    loader->chip = gx_read_le16(loader->data + 6);
    loader->baud = gx_read_le32(loader->data + 8);
    return true;
}

bool gx_loader_from_file(const char *path, gx_loader *loader) {
    FILE *file;
    long length;
    uint8_t *data;
    memset(loader, 0, sizeof(*loader));
    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[!] Cannot open boot image %s: %s\n", path, strerror(errno));
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[!] Cannot determine boot image size: %s\n", strerror(errno));
        fclose(file);
        return false;
    }
    if ((unsigned long)length > SIZE_MAX) {
        fprintf(stderr, "[!] Boot image is too large\n");
        fclose(file);
        return false;
    }
    data = malloc((size_t)length ? (size_t)length : 1U);
    if (!data) {
        fprintf(stderr, "[!] Out of memory loading boot image\n");
        fclose(file);
        return false;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "[!] Failed to read boot image %s\n", path);
        free(data);
        fclose(file);
        return false;
    }
    fclose(file);
    loader->data = data;
    loader->owned_data = data;
    loader->size = (size_t)length;
    loader->description = path;
    if (!gx_loader_validate(loader)) {
        gx_loader_release(loader);
        return false;
    }
    return true;
}

bool gx_loader_from_model(const char *name, gx_loader *loader) {
    const gx_embedded_loader *entry = gx_embedded_loader_find(name);
    memset(loader, 0, sizeof(*loader));
    if (!entry) {
        fprintf(stderr, "[!] Embedded loader not found: %s\n", name);
        if (gx_embedded_loader_count() == 0)
            fprintf(stderr, "[!] This binary was built with EMBED_LOADERS=0\n");
        else
            fprintf(stderr, "[!] Use --list-loaders to see available model names\n");
        return false;
    }
    loader->data = entry->data;
    loader->size = entry->size;
    loader->description = entry->name;
    return gx_loader_validate(loader);
}

void gx_loader_release(gx_loader *loader) {
    free(loader->owned_data);
    memset(loader, 0, sizeof(*loader));
}
