# gxdl-c

`gxdl-c` is a Linux C11 utility for booting and servicing NationalChip GX
devices through their UART BootROM/downloader protocol. It is command-compatible
with [`libre-gxdl`](https://github.com/matu6968/libre-gxdl) implementation and can either read a
loader from disk or carry the loaders inside the executable.

The protocol has primarily been tested by the reference project on GX6702
hardware. Support for other chips and boards is inherently experimental.

## Build

Required build tools and libraries:

- a C11 compiler and GNU Make;
- Python 3 for the pseudo-terminal integration tests only.

Build an external-loader-only binary:

```sh
make
```

Build one binary containing every `lib/loaders/*.boot` file:

```sh
make clean
make EMBED_LOADERS=1
./gxdl-c --list-loaders
```

The embedded sources are generated under `build/` using the vendored `bin2c`
tool. They are not copied into the source tree. `CC`, `CPPFLAGS`, `CFLAGS`,
`LDFLAGS`, `LDLIBS`, and `BUILD_DIR` may be overridden in the usual Make style.
The progress bar uses `TIOCGWINSZ` and has no termcap or ncurses dependency.

For cross compilation, set `CC` to the target compiler. Build-time tools still
use the native `HOSTCC`, which is important when embedding loaders:

```sh
make clean
make CC=csky-linux-gcc
make clean
make CC=csky-linux-gcc HOSTCC=cc EMBED_LOADERS=1
```

Object files are separated by compiler name under `build/`. The resulting
target binary cannot be exercised by `make test` on the build host; copy it to
the target Linux system or run it through a suitable emulator.

The normal cross build is dynamically linked and requires the matching target
libc and program interpreter. When the toolchain supplies static libc archives,
a more self-contained binary can be produced with:

```sh
make CC=csky-linux-gcc LDFLAGS=-static
```

Run the automated tests with:

```sh
make test
make clean && make EMBED_LOADERS=1 test
make clean && make SANITIZE=1 test
```

## Loader selection

Transfer mode `s` (the default) requires exactly one loader source:

```sh
# External loader file
./gxdl-c -b lib/loaders/gemini-6702H5-sflash-24M.boot -d /dev/ttyUSB0

# Embedded loader; use the exact filename without .boot
./gxdl-c -m gemini-6702H5-sflash-24M -d /dev/ttyUSB0
```

As in the vendor utility, `-b` replaces `-m`; supplying both is an error. Model
names are case-sensitive. `-m` is available only in a binary built with
`EMBED_LOADERS=1`.

When the device is already at a `boot>` prompt, `nns` skips loader upload and
therefore does not require `-b` or `-m`:

```sh
./gxdl-c -t nns -d /dev/ttyUSB0 -c "flash badinfo"
```

## Commands

Pass one downloader command as the quoted argument to `-c`:

```text
serialdump <partition|address> <size> <host-file>
serialdown <partition|address> <host-file>
usbdump <partition|address> <size> <device-side-file>
usbdown <partition|address> <device-side-file>

gx_otp tread <address> <length>
gx_otp read <address> <length> <host-file>
gx_otp write <address> <host-file>
gx_otp twrite <address> <hex-digits>

sflash_otp status
sflash_otp getregion
sflash_otp read <address> <length> <host-file>
sflash_otp write <address> <host-file>
sflash_otp erase

flash badinfo
flash erase [nospread] <partition|address> [length]
flash eraseall

compare <host-file> <host-file>
load_conf_down <config-file> <transport> [transport-path]
```

Config files contain one command per line. Empty lines and lines beginning with
`#` are ignored, and execution stops on the first invalid or failed command.
Arguments follow the same whitespace-separated syntax as `-c`; paths containing
spaces are not supported by the reference command format.

Examples:

```sh
# Read the BOOT partition over UART
./gxdl-c -m gemini-6702H5-sflash-24M -d /dev/ttyUSB0 \
  -c "serialdump BOOT 65536 boot-backup.bin"

# Write a logo over UART
./gxdl-c -b loader.boot -d /dev/ttyUSB0 \
  -c "serialdown LOGO logo.bin"

# Read OTP without uploading a loader
./gxdl-c -t nns -d /dev/ttyUSB0 -c "gx_otp tread 0 32"
```

## Safety

Writing flash or OTP data can permanently brick a device. `flash erase` and
`flash eraseall` require interactive confirmation; `-y` bypasses those prompts
for compatibility with scripted use. Other write commands retain the reference
tool's warning-only behavior. OTP writes may be irreversible.

Back up the complete flash before writing it, verify board and loader identity,
and keep power stable throughout erase/write operations.

## Serial behavior and troubleshooting

The port is configured as raw 8N1 with `INPCK`, no hardware/software flow
control, and the vendor-compatible flush/drain sequence. The default baud rate
is 115200. Use `--baud` for another supported termios rate.

- Start the tool first, then power-cycle the target while it waits for the
  handshake.
- Use `--reset-dtr` and/or `--reset-rts` only when those modem lines are wired
  to the board's reset circuit.
- Check TX/RX crossing and common ground. `--loopback-test` can verify a serial
  adapter by physically shorting its TX and RX pins.
- A Stage 1 timeout usually indicates the wrong loader, missed BootROM window,
  wiring trouble, or incorrect voltage levels.
- `nns` works only when the target is already presenting `boot>`.
- Run with `-v` to display protocol milestones and checksum values.

## Manual hardware smoke test

1. Short TX to RX on the disconnected adapter and run
   `./gxdl-c -d /dev/ttyUSB0 --loopback-test`.
2. Connect the target, boot it with the board-specific loader, and confirm that
   its partition/device information reaches `boot>`.
3. Perform a small read such as `serialdump BOOT 1024 smoke.bin` and verify the
   output size.
4. Test erase/write/OTP commands only on recoverable hardware with a known-good
   full backup.

The reverse-engineered packet details and known handshake variants are in
`reference/PROTOCOL.md`.

## Using libgxdl

The default build also creates `libgxdl.a` and `libgxdl.so`. The installed
public interface is [include/gxdl/gxdl.h](include/gxdl/gxdl.h); transport and
protocol implementation headers remain private.

Build the supplied example with:

```sh
make example
build/cc/examples/basic /dev/ttyUSB0 loader.boot "flash badinfo"
```

A minimal client looks like this:

```c
#include <stdio.h>
#include <gxdl/gxdl.h>

int main(void) {
    gxdl_options options;
    gxdl_session *session;

    gxdl_options_init(&options);
    options.device = "/dev/ttyUSB0";

    session = gxdl_open(&options);
    if (session == NULL)
        return 1;

    if (gxdl_boot_file(session, "loader.boot") != 0 ||
        gxdl_run(session, "flash badinfo") != 0) {
        fprintf(stderr, "%s\n", gxdl_last_error(session));
        gxdl_close(session);
        return 1;
    }

    gxdl_close(session);
    return 0;
}
```

Applications can upload external or embedded loaders, wait for a prompt,
execute individual commands or config files, enumerate embedded model names,
and use the host-side comparison helper. Each `gxdl_session` owns one serial
connection. Protocol status and command responses currently use the process's
standard streams; erase confirmations read standard input unless
`gxdl_options.assume_yes` is enabled.

Install the executable, libraries, public header, and `libgxdl.pc` metadata:

```sh
make install PREFIX=/usr/local
cc app.c $(pkg-config --cflags --libs libgxdl) -o app
```

For staged packaging, add `DESTDIR=/path/to/package-root`. Use `make uninstall`
with the same `PREFIX` and `DESTDIR` to remove the installed files.
