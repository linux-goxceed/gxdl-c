#define _POSIX_C_SOURCE 200809L
#include "gxdl.h"
#include "argparse.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t active_fd = -1;

static void stop_handler(int signal_number) {
    int fd = (int)active_fd;
    (void)signal_number;
    active_fd = -1;
    if (fd >= 0)
        close(fd);
}

static void install_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
}

static void list_loaders(void) {
    size_t count = gx_embedded_loader_count();
    size_t i;
    if (count == 0) {
        puts("No embedded loaders. This binary was built with EMBED_LOADERS=0.");
        return;
    }
    printf("Embedded loaders (%zu):\n", count);
    for (i = 0; i < count; ++i) {
        const gx_embedded_loader *loader = gx_embedded_loader_at(i);
        if (loader)
            printf("  %s\n", loader->name);
    }
}

static bool loopback_test(const char *device, unsigned int baud, bool verbose) {
    static const uint8_t test[] = "LOOPBACK_TEST_12345";
    uint8_t response[sizeof(test) - 1];
    gx_serial serial;
    size_t received = 0;
    fprintf(stderr, "[*] Serial loopback test: short TX to RX first\n");
    if (gx_serial_open(&serial, device, baud, verbose) != 0)
        return false;
    active_fd = serial.fd;
    if (gx_serial_write_all(&serial, test, sizeof(test) - 1, 2000) != 0 ||
        gx_serial_drain(&serial) != 0) {
        fprintf(stderr, "[!] Loopback write failed: %s\n", strerror(errno));
        goto fail;
    }
    while (received < sizeof(response)) {
        ssize_t n = gx_serial_read(&serial, response + received,
                                   sizeof(response) - received, 1000);
        if (n <= 0)
            break;
        received += (size_t)n;
    }
    if (received != sizeof(response) || memcmp(test, response, sizeof(response)) != 0) {
        fprintf(stderr, "[!] Loopback FAILED: received %zu/%zu matching bytes\n",
                received, sizeof(response));
        goto fail;
    }
    fprintf(stderr, "[+] Loopback OK: %zu bytes\n", received);
    active_fd = -1;
    gx_serial_close(&serial);
    return true;
fail:
    active_fd = -1;
    gx_serial_close(&serial);
    return false;
}

int main(int argc, const char **argv) {
    const bool no_arguments = argc == 1;
    const char *boot_path = NULL;
    const char *model = NULL;
    const char *device = NULL;
    const char *command = NULL;
    const char *transfer_mode = "s";
    int baud = 115200;
    int assume_yes = 0, verbose = 0, reset_dtr = 0, reset_rts = 0;
    int loopback = 0, show_loaders = 0;
    gx_loader loader;
    gx_context ctx;
    bool success = false;
    static const char *const usages[] = {
        "gxdl-c [options]",
        NULL,
    };
    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_GROUP("Loader selection"),
        OPT_STRING('b', "boot", &boot_path, "external .boot image", NULL, 0, 0),
        OPT_STRING('m', "model", &model, "embedded loader name (without .boot)", NULL, 0, 0),
        OPT_BOOLEAN(0, "list-loaders", &show_loaders, "list loaders compiled into this binary", NULL, 0, 0),
        OPT_GROUP("Connection"),
        OPT_STRING('d', "device", &device, "serial device, for example /dev/ttyUSB0", NULL, 0, 0),
        OPT_INTEGER(0, "baud", &baud, "serial baud rate (default: 115200)", NULL, 0, 0),
        OPT_BOOLEAN(0, "reset-dtr", &reset_dtr, "pulse DTR before boot upload", NULL, 0, 0),
        OPT_BOOLEAN(0, "reset-rts", &reset_rts, "pulse RTS before boot upload", NULL, 0, 0),
        OPT_BOOLEAN(0, "loopback-test", &loopback, "test a TX-to-RX serial loopback", NULL, 0, 0),
        OPT_GROUP("Operation"),
        OPT_STRING('c', "command", &command, "bootloader command to execute", NULL, 0, 0),
        OPT_STRING('t', "transfer-mode", &transfer_mode, "s (upload) or nns (existing prompt)", NULL, 0, 0),
        OPT_BOOLEAN('y', "yes", &assume_yes, "skip flash erase confirmations", NULL, 0, 0),
        OPT_BOOLEAN('v', "verbose", &verbose, "show protocol details", NULL, 0, 0),
        OPT_END(),
    };
    struct argparse parser;

    argparse_init(&parser, options, usages, 0);
    argparse_describe(&parser,
        "NationalChip GX serial bootloader and flash utility.",
        "Loader selection:\n"
        "  -b FILE loads an external .boot image and replaces -m.\n"
        "  -m NAME selects an embedded loader by filename without .boot.\n"
        "  Use --list-loaders to print the models compiled into this binary.\n"
        "\n"
        "Transfer modes:\n"
        "  s   upload the selected .boot image before executing a command\n"
        "  nns use a device that is already waiting at the boot> prompt\n"
        "\n"
        "Commands:\n"
        "  serialdown <partition|address> <host-file>\n"
        "  serialdump <partition|address> <length> <host-file>\n"
        "  usbdown <partition|address> <device-side-file>\n"
        "  usbdump <partition|address> <length> <device-side-file>\n"
        "  flash erase [nospread] <partition|address> [length]\n"
        "  flash badinfo\n"
        "  flash eraseall\n"
        "  load_conf_down <config-file> <transport> [transport-path]\n"
        "  gx_otp read <address> <length> <host-file>\n"
        "  gx_otp tread <address> <length>\n"
        "  gx_otp write <address> <host-file>\n"
        "  gx_otp twrite <address> <hex-digits>\n"
        "  sflash_otp status|getregion|erase\n"
        "  sflash_otp read <address> <length> <host-file>\n"
        "  sflash_otp write <address> <host-file>\n"
        "  compare <src-file> <dst-file>\n"
        "\n"
        "Examples:\n"
        "  gxdl-c -m gemini-6702H5-sflash-24M -d /dev/ttyUSB0\n"
        "  gxdl-c -b loader.boot -d /dev/ttyUSB0 -c \"serialdump BOOT 65536 boot.bin\"\n"
        "  gxdl-c -t nns -d /dev/ttyUSB0 -c \"flash badinfo\"\n"
        "\n"
        "WARNING: flash and OTP writes can permanently brick a device.\n");
    if (no_arguments) {
        argparse_usage(&parser);
        return 0;
    }
    argc = argparse_parse(&parser, argc, argv);
    if (argc != 0) {
        fprintf(stderr, "[!] Unexpected positional argument: %s\n", argv[0]);
        argparse_usage(&parser);
        return 2;
    }
    if (show_loaders) {
        list_loaders();
        return 0;
    }
    if (baud <= 0) {
        fprintf(stderr, "[!] Baud rate must be positive\n");
        return 2;
    }
    if (!device) {
        fprintf(stderr, "[!] -d/--device is required\n");
        return 2;
    }
    if (boot_path && model) {
        fprintf(stderr, "[!] -b/--boot and -m/--model are mutually exclusive\n");
        return 2;
    }
    if (strcmp(transfer_mode, "s") != 0 && strcmp(transfer_mode, "nns") != 0) {
        fprintf(stderr, "[!] Transfer mode must be 's' or 'nns'\n");
        return 2;
    }
    install_handlers();
    if (loopback)
        return loopback_test(device, (unsigned int)baud, verbose != 0) ? 0 : 1;
    if (strcmp(transfer_mode, "s") == 0 && !boot_path && !model) {
        fprintf(stderr, "[!] Transfer mode s requires -b/--boot or -m/--model\n");
        return 2;
    }
    memset(&loader, 0, sizeof(loader));
    if (strcmp(transfer_mode, "s") == 0) {
        if (boot_path) {
            if (!gx_loader_from_file(boot_path, &loader))
                return 1;
        } else if (!gx_loader_from_model(model, &loader)) {
            return 1;
        }
        fprintf(stderr, "[+] Loaded boot image: %s (%zu bytes)\n",
                loader.description, loader.size);
        fprintf(stderr, "    Version: 0x%04x, Chip: 0x%04x, Baud: %u\n",
                loader.version, loader.chip, loader.baud);
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.serial.fd = -1;
    ctx.verbose = verbose != 0;
    ctx.assume_yes = assume_yes != 0;
    ctx.reset_dtr = reset_dtr != 0;
    ctx.reset_rts = reset_rts != 0;
    if (gx_serial_open(&ctx.serial, device, (unsigned int)baud, ctx.verbose) != 0)
        goto done;
    active_fd = ctx.serial.fd;
    if (strcmp(transfer_mode, "s") == 0) {
        if (!gx_boot(&ctx, &loader, true))
            goto done;
    } else if (!command && !gx_wait_prompt(&ctx, 2000)) {
        fprintf(stderr, "[!] Cannot use nns mode without an active boot> prompt\n");
        goto done;
    }
    success = command ? gx_run_command(&ctx, command) : true;
done:
    active_fd = -1;
    gx_serial_close(&ctx.serial);
    gx_loader_release(&loader);
    return success ? 0 : 1;
}
