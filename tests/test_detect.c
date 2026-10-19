/*
 * Test for serial modem detection.
 *
 * Pseudo-terminals stand in for serial ports.  One has a fake Hayes modem on
 * its master side, one is silent, and one only echoes (as a loopback or a
 * wrongly connected device would.)
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include "src/mm_detect.h"

typedef struct fake_port {
    int         master_fd;
    char        slave_path[MM_DETECT_PATH_LEN];
    int         echo_only;      /* Echo what is received, never answer. */
    int         respond;        /* Answer AT commands. */
    volatile int stop;
    pthread_t   thread;
} fake_port_t;

static int failures;

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } else { printf("ok:   %s\n", msg); } } while (0)

static void *modem_thread(void *arg) {
    fake_port_t *port = (fake_port_t *)arg;
    char         line[64];
    size_t       len = 0;

    while (!port->stop) {
        struct pollfd pfd = { port->master_fd, POLLIN, 0 };
        char          c;

        if ((poll(&pfd, 1, 50) <= 0) || (read(port->master_fd, &c, 1) != 1)) {
            continue;
        }

        if (port->echo_only) {
            if (write(port->master_fd, &c, 1) != 1) break;
            continue;
        }
        if (!port->respond) {
            continue;
        }

        if (c != '\r') {
            if (len < sizeof(line) - 1) line[len++] = c;
            continue;
        }
        line[len] = '\0';
        len = 0;

        if (strcmp(line, "ATI3") == 0) {
            static const char rsp[] = "\r\nFake Modem 1200\r\n\r\nOK\r\n";
            if (write(port->master_fd, rsp, sizeof(rsp) - 1) < 0) break;
        } else if (strncmp(line, "AT", 2) == 0) {
            static const char rsp[] = "\r\nOK\r\n";
            if (write(port->master_fd, rsp, sizeof(rsp) - 1) < 0) break;
        }
    }
    return NULL;
}

static int start_port(fake_port_t *port, int respond, int echo_only) {
    char *name;

    memset(port, 0, sizeof(*port));
    port->master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if ((port->master_fd < 0) || (grantpt(port->master_fd) != 0) || (unlockpt(port->master_fd) != 0) ||
        ((name = ptsname(port->master_fd)) == NULL)) {
        return -1;
    }
    snprintf(port->slave_path, sizeof(port->slave_path), "%s", name);
    port->respond   = respond;
    port->echo_only = echo_only;

    return pthread_create(&port->thread, NULL, modem_thread, port);
}

static void stop_port(fake_port_t *port) {
    port->stop = 1;
    pthread_join(port->thread, NULL);
    close(port->master_fd);
}

int main(void) {
    fake_port_t         modem, silent, echo;
    mm_detected_modem_t found;
    const int           bauds[] = { 9600, 19200 };
    const int           one_baud[] = { 19200 };

    /* The modem side has to see the slave open before it answers, so keep a slave fd open on each. */
    if (start_port(&modem, 1, 0) || start_port(&silent, 0, 0) || start_port(&echo, 0, 1)) {
        printf("SKIP: pseudo-terminals are not available.\n");
        return 0;
    }

    memset(&found, 0, sizeof(found));
    CHECK(mm_detect_probe_port(modem.slave_path, bauds, 2, &found) == 1, "modem is detected");
    CHECK(strcmp(found.path, modem.slave_path) == 0, "detected path is the port probed");
    CHECK(found.baudrate == 9600, "first baud rate that works is reported");
    CHECK(strcmp(found.ident, "Fake Modem 1200") == 0, "identification string is read from ATI3");

    CHECK(mm_detect_probe_port(silent.slave_path, one_baud, 1, &found) == 0, "silent port is not a modem");
    CHECK(mm_detect_probe_port(echo.slave_path, one_baud, 1, &found) == 0, "echo-only port is not a modem");
    CHECK(mm_detect_probe_port("/dev/mm_no_such_port", one_baud, 1, &found) == -1, "missing port reports an open error");

    stop_port(&modem);
    stop_port(&silent);
    stop_port(&echo);

    if (failures) {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
    printf("All detection tests passed.\n");
    return 0;
}
