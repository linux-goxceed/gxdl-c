#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <gxdl/gxdl.h>

int main(int argc, char **argv) {
    gxdl_options options;
    gxdl_session *session;
    int result;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s SERIAL_DEVICE BOOT_FILE [COMMAND]\n", argv[0]);
        fprintf(stderr, "Example: %s /dev/ttyUSB0 loader.boot \"flash badinfo\"\n",
                argv[0]);
        return 2;
    }

    gxdl_options_init(&options);
    options.device = argv[1];
    options.verbose = true;

    session = gxdl_open(&options);
    if (!session) {
        fprintf(stderr, "Unable to open %s: %s\n", options.device, strerror(errno));
        return 1;
    }

    result = gxdl_boot_file(session, argv[2]);
    if (result == 0 && argc == 4)
        result = gxdl_run(session, argv[3]);

    if (result != 0)
        fprintf(stderr, "libgxdl error: %s\n", gxdl_last_error(session));

    gxdl_close(session);
    return result == 0 ? 0 : 1;
}
