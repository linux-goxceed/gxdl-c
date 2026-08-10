#include "gxdl.h"

#include <string.h>

size_t gx_embedded_loader_count(void) { return 0; }

const gx_embedded_loader *gx_embedded_loader_at(size_t index) {
    (void)index;
    return NULL;
}

const gx_embedded_loader *gx_embedded_loader_find(const char *name) {
    (void)name;
    return NULL;
}
