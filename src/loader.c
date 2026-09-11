#include "gxdl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool gx_is_uart_ipl_stub(const uint8_t *data, size_t size) {
    if (!data || size < 0x2020U || memcmp(data, "toob", 4) != 0)
        return false;
    if (size == 0x2020U)
        return true;
    return memcmp(data + 0x2020, "GXAI", 4) == 0;
}

size_t gx_parse_target_catalog(const uint8_t *data, size_t size, uint16_t *ids,
                               size_t max_ids) {
    size_t count, i;
    if (!data || size < 0x20U || memcmp(data + 0x0C, "GXMT", 4) != 0)
        return 0;
    if (data[0x10] != 1U || data[0x11] > 6U)
        return 0;
    count = data[0x11];
    if (0x12U + count * 2U > 0x20U)
        return 0;
    if (ids && max_ids < count)
        count = max_ids;
    if (!ids)
        return data[0x11];
    for (i = 0; i < count; ++i)
        ids[i] = gx_read_le16(data + 0x12 + i * 2U);
    return count;
}

bool gx_parse_gxid(const uint8_t *data, size_t size, char *family,
                   size_t family_size, char *name, size_t name_size) {
    static const char prefix[] = "GXID family=";
    static const char name_tag[] = " name=";
    size_t i;
    if (!data || !family || !name || family_size < 2U || name_size < 2U)
        return false;
    for (i = 0; i + sizeof(prefix) - 1U < size; ++i) {
        size_t pos, n, name_pos, nlen;
        if (memcmp(data + i, prefix, sizeof(prefix) - 1U) != 0)
            continue;
        pos = i + sizeof(prefix) - 1U;
        n = 0;
        while (pos + n < size) {
            uint8_t c = data[pos + n];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
                break;
            ++n;
        }
        if (n == 0U || n >= family_size)
            continue;
        name_pos = pos + n;
        if (name_pos + sizeof(name_tag) - 1U > size ||
            memcmp(data + name_pos, name_tag, sizeof(name_tag) - 1U) != 0)
            continue;
        name_pos += sizeof(name_tag) - 1U;
        nlen = 0;
        while (name_pos + nlen < size) {
            uint8_t c = data[name_pos + nlen];
            if (c <= ' ' || c == 0x7f)
                break;
            ++nlen;
        }
        if (nlen == 0U || nlen >= name_size)
            continue;
        memcpy(family, data + pos, n);
        family[n] = '\0';
        memcpy(name, data + name_pos, nlen);
        name[nlen] = '\0';
        return true;
    }
    return false;
}

bool gx_family_trains_ddr(const char *family) {
    return family && (strcmp(family, "gemini") == 0 || strcmp(family, "cygnus") == 0);
}

const char *gx_bootcode_filename_for_family(const char *family) {
    if (!family)
        return NULL;
    if (strcmp(family, "gemini") == 0)
        return "gx6702-bootcode.bin";
    if (strcmp(family, "cygnus") == 0)
        return "gx6706-bootcode.bin";
    return NULL;
}

bool gx_wrap_gxbc(const uint8_t *payload, size_t payload_size, uint8_t **out,
                  size_t *out_size) {
    uint32_t sum = 0;
    size_t i;
    uint8_t *result;
    if (!payload || !out || !out_size || payload_size > UINT32_MAX - 16U)
        return false;
    result = malloc(16U + payload_size);
    if (!result)
        return false;
    for (i = 0; i < payload_size; ++i)
        sum += payload[i];
    gx_write_le32(result, GX_GXBC_MAGIC);
    gx_write_le32(result + 4, (uint32_t)payload_size);
    gx_write_le32(result + 8, GX_GXBC_ENTRY);
    gx_write_le32(result + 12, sum);
    memcpy(result + 16, payload, payload_size);
    *out = result;
    *out_size = 16U + payload_size;
    return true;
}

void gx_loader_print_info(const gx_loader *loader) {
    uint16_t extras[6];
    size_t n, i;
    if (!loader)
        return;
    fprintf(stderr, "[+] Loaded boot image: %s (%zu bytes)\n",
            loader->description ? loader->description : "(unnamed)", loader->size);
    fprintf(stderr, "    Version: 0x%04x, Chip: 0x%04x, Baud: %u\n",
            loader->version, loader->chip, loader->baud);
    n = gx_parse_target_catalog(loader->data, loader->size, extras, 6);
    if (n == 0)
        return;
    fprintf(stderr, "    Header catalog extra IDs: ");
    for (i = 0; i < n; ++i)
        fprintf(stderr, "%s0x%04X", i ? ", " : "", extras[i]);
    fprintf(stderr, "\n    UART Stage 1 is still sent once (offset 6 / 8 KiB stub layout)\n");
}

bool gx_loader_validate(gx_loader *loader) {
    size_t required = 0x2020U;
    if (!loader || !loader->data) {
        fprintf(stderr, "[!] Boot image is empty\n");
        return false;
    }
    if (loader->size >= 8U) {
        switch (gx_read_le16(loader->data + 6)) {
        case 0x6612: required = 0x4000U; break;
        case 0x6616:
        case 0x3211:
        case 0x6701:
        case 0x6705: required = 0x2020U; break;
        default: required = 0x1020U; break;
        }
    }
    if (loader->size < required) {
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
