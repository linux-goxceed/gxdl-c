#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <gxdl/gxdl.h>

int main(void) {
    gxdl_options options;
    size_t i;
    gxdl_options_init(&options);
    assert(options.baud == 115200U);
    assert(strcmp(gxdl_version(), "1.0.0") == 0);
    for (i = 0; i < gxdl_model_count(); ++i)
        assert(gxdl_model_name(i) != NULL);
    puts("shared public API test: OK");
    return 0;
}
