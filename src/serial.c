#define _POSIX_C_SOURCE 200809L
#include "gxdl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static bool baud_constant(unsigned int baud, speed_t *speed) {
#define GX_BAUD(value) case value: *speed = B##value; return true
    switch (baud) {
    GX_BAUD(50); GX_BAUD(75); GX_BAUD(110); GX_BAUD(134); GX_BAUD(150);
    GX_BAUD(200); GX_BAUD(300); GX_BAUD(600); GX_BAUD(1200); GX_BAUD(1800);
    GX_BAUD(2400); GX_BAUD(4800); GX_BAUD(9600); GX_BAUD(19200);
    GX_BAUD(38400); GX_BAUD(57600); GX_BAUD(115200); GX_BAUD(230400);
#ifdef B460800
    GX_BAUD(460800);
#endif
#ifdef B500000
    GX_BAUD(500000);
#endif
#ifdef B576000
    GX_BAUD(576000);
#endif
#ifdef B921600
    GX_BAUD(921600);
#endif
#ifdef B1000000
    GX_BAUD(1000000);
#endif
#ifdef B1500000
    GX_BAUD(1500000);
#endif
#ifdef B2000000
    GX_BAUD(2000000);
#endif
#ifdef B3000000
    GX_BAUD(3000000);
#endif
#ifdef B4000000
    GX_BAUD(4000000);
#endif
    default: return false;
    }
#undef GX_BAUD
}

int gx_serial_open(gx_serial *serial, const char *device, unsigned int baud,
                   bool verbose) {
    struct termios tty;
    speed_t speed;
    memset(serial, 0, sizeof(*serial));
    serial->fd = -1;
    serial->device = device;
    serial->baud = baud;
    serial->verbose = verbose;
    if (!baud_constant(baud, &speed)) {
        fprintf(stderr, "[!] Unsupported baud rate: %u\n", baud);
        return -1;
    }
    serial->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial->fd < 0) {
        fprintf(stderr, "[!] Cannot open %s: %s\n", device, strerror(errno));
        return -1;
    }
    if (tcgetattr(serial->fd, &tty) != 0) {
        fprintf(stderr, "[!] tcgetattr(%s): %s\n", device, strerror(errno));
        gx_serial_close(serial);
        return -1;
    }
    memset(&tty, 0, sizeof(tty));
    tty.c_iflag = INPCK;
    tty.c_oflag = 0;
    tty.c_cflag = CS8 | CREAD | HUPCL | CLOCAL;
    tty.c_lflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    if (tcsetattr(serial->fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "[!] tcsetattr(%s): %s\n", device, strerror(errno));
        gx_serial_close(serial);
        return -1;
    }
    (void)gx_serial_drain(serial);
    (void)gx_serial_flush(serial);
    (void)gx_serial_set_line(serial, TIOCM_RTS, false);
    (void)gx_serial_set_line(serial, TIOCM_DTR, false);
    if (verbose)
        fprintf(stderr, "[*] Opened %s at %u baud\n", device, baud);
    return 0;
}

void gx_serial_close(gx_serial *serial) {
    if (serial && serial->fd >= 0) {
        close(serial->fd);
        serial->fd = -1;
    }
}

static int wait_fd(int fd, short events, int64_t deadline) {
    struct pollfd pfd = {fd, events, 0};
    for (;;) {
        int64_t remaining = deadline - gx_now_ms();
        int rc;
        if (remaining <= 0)
            return 0;
        rc = poll(&pfd, 1, remaining > INT32_MAX ? INT32_MAX : (int)remaining);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            return rc;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
        if (pfd.revents & events)
            return 1;
    }
}

int gx_serial_write_all(gx_serial *serial, const void *data, size_t size,
                        int timeout_ms) {
    const uint8_t *p = data;
    int64_t deadline = gx_now_ms() + timeout_ms;
    while (size > 0) {
        ssize_t n;
        int ready = wait_fd(serial->fd, POLLOUT, deadline);
        if (ready <= 0) {
            if (ready == 0) errno = ETIMEDOUT;
            return -1;
        }
        n = write(serial->fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n < 0)
            return -1;
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += (size_t)n;
        size -= (size_t)n;
    }
    return 0;
}

ssize_t gx_serial_read(gx_serial *serial, void *data, size_t size,
                       int timeout_ms) {
    int ready;
    ssize_t n;
    if (size == 0)
        return 0;
    ready = wait_fd(serial->fd, POLLIN, gx_now_ms() + timeout_ms);
    if (ready == 0)
        return 0;
    if (ready < 0)
        return -1;
    do {
        n = read(serial->fd, data, size);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return n;
}

int gx_serial_drain(gx_serial *serial) {
    int rc;
    do rc = tcdrain(serial->fd); while (rc != 0 && errno == EINTR);
    return rc;
}

int gx_serial_flush(gx_serial *serial) {
    return tcflush(serial->fd, TCIOFLUSH);
}

int gx_serial_set_line(gx_serial *serial, int line, bool asserted) {
    int value = line;
    if (ioctl(serial->fd, asserted ? TIOCMBIS : TIOCMBIC, &value) != 0) {
        if (serial->verbose)
            fprintf(stderr, "[*] Modem line ioctl unavailable: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int gx_serial_pulse_resets(gx_serial *serial, bool dtr, bool rts) {
    if (!dtr && !rts)
        return 0;
    if (serial->verbose)
        fprintf(stderr, "[*] Pulsing reset line%s%s\n", dtr ? " DTR" : "",
                rts ? " RTS" : "");
    if (dtr) {
        (void)gx_serial_set_line(serial, TIOCM_DTR, true);
        gx_sleep_ms(100);
        (void)gx_serial_set_line(serial, TIOCM_DTR, false);
    }
    if (rts) {
        (void)gx_serial_set_line(serial, TIOCM_RTS, true);
        gx_sleep_ms(100);
        (void)gx_serial_set_line(serial, TIOCM_RTS, false);
    }
    gx_sleep_ms(200);
    return gx_serial_flush(serial);
}
