/*
 * Automatic detection of serial modems, part of mmumblephone.
 *
 * A Millennium terminal can't be probed directly (it only ever calls the
 * Manager), but the modem it calls through speaks Hayes AT commands, so that
 * is what is detected here.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mm_detect.h"
#include "mm_serial.h"

/* Rates tried after the preferred one. */
static const int common_bauds[] = { 19200, 9600, 38400, 57600, 115200, 2400, 1200 };

/*
 * Send an AT command and collect the reply into resp (NUL terminated.)
 *
 * Returns once the modem says OK or ERROR, or when a read times out
 * (about 1 second without data.)  Returns 1 if the modem responded to the command.
 */
static int at_exchange(int fd, const char *command, char *resp, size_t resp_len) {
    char    cmd[48];
    size_t  len = 0;
    ssize_t n;

    platform_flush_serial(fd);
    snprintf(cmd, sizeof(cmd), "%s\r", command);
    if (platform_write_serial(fd, cmd, strlen(cmd)) <= 0) {
        resp[0] = '\0';
        return 0;
    }

    resp[0] = '\0';
    while ((len < resp_len - 1) && ((n = platform_read_serial(fd, &resp[len], resp_len - 1 - len)) > 0)) {
        len += (size_t)n;
        resp[len] = '\0';
        if ((strstr(resp, "OK") != NULL) || (strstr(resp, "ERROR") != NULL)) {
            return 1;
        }
    }

    return 0;
}

/* Copy the first line of an ATI reply that is neither the echoed command nor a result code. */
static void extract_ident(const char *resp, char *ident, size_t ident_len) {
    const char *p = resp;

    ident[0] = '\0';
    while (*p != '\0') {
        char   line[MM_DETECT_IDENT_LEN];
        size_t n = 0;

        while ((*p == '\r') || (*p == '\n')) {
            p++;
        }
        while ((*p != '\0') && (*p != '\r') && (*p != '\n') && (n < sizeof(line) - 1)) {
            line[n++] = *p++;
        }
        while ((*p != '\0') && (*p != '\r') && (*p != '\n')) {
            p++;    /* Discard the rest of an overlong line. */
        }
        line[n] = '\0';

        if ((n == 0) || (strncmp(line, "AT", 2) == 0) || (strcmp(line, "OK") == 0) || (strcmp(line, "ERROR") == 0)) {
            continue;
        }
        snprintf(ident, ident_len, "%s", line);
        return;
    }
}

int mm_detect_probe_port(const char *path, const int *bauds, int num_bauds, mm_detected_modem_t *modem) {
    char resp[256];
    int  fd = platform_open_serial(path);
    int  found = 0;

    if (fd < 0) {
        return -1;
    }

    for (int i = 0; (i < num_bauds) && !found; i++) {
        /* ATQ0V1E1 turns on echo and verbose result codes, in case the modem was left in another mode. */
        static const char probe[] = "ATQ0V1E1";

        if (platform_init_serial(fd, bauds[i]) != 0) {
            continue;
        }

        /* A modem that has been idle may swallow the first command, so ask the first rate twice. */
        found = at_exchange(fd, probe, resp, sizeof(resp));
        if (!found && (i == 0)) {
            found = at_exchange(fd, probe, resp, sizeof(resp));
        }

        if (found) {
            memset(modem, 0, sizeof(*modem));
            snprintf(modem->path, sizeof(modem->path), "%s", path);
            modem->baudrate = bauds[i];

            /* Not every modem implements ATI3, so the identification is best-effort. */
            if (at_exchange(fd, "ATI3", resp, sizeof(resp))) {
                extract_ident(resp, modem->ident, sizeof(modem->ident));
            }
        }
    }

    platform_close_serial(fd);

    return found;
}

/* Build the list of baud rates to try: the preferred one first, then the common ones. */
static int build_baud_list(int preferred_baud, int *bauds) {
    int count = 0;

    if (preferred_baud > 0) {
        bauds[count++] = preferred_baud;
    }

    for (size_t i = 0; (i < sizeof(common_bauds) / sizeof(common_bauds[0])) && (count < MM_DETECT_MAX_BAUDS); i++) {
        int duplicate = 0;

        for (int j = 0; j < count; j++) {
            if (bauds[j] == common_bauds[i]) {
                duplicate = 1;
            }
        }
        if (!duplicate) {
            bauds[count++] = common_bauds[i];
        }
    }

    return count;
}

int mm_detect_modems(mm_detected_modem_t *modems, int max_modems, int preferred_baud, FILE *report) {
    static char ports[MM_DETECT_MAX_PORTS][MM_DETECT_PATH_LEN];
    int         bauds[MM_DETECT_MAX_BAUDS];
    int         num_bauds = build_baud_list(preferred_baud, bauds);
    int         num_ports = platform_list_serial_ports(&ports[0][0], MM_DETECT_PATH_LEN, MM_DETECT_MAX_PORTS);
    int         found = 0;

    if (report != NULL) {
        fprintf(report, "Scanning %d serial port%s for modems...\n", num_ports, (num_ports == 1) ? "" : "s");
    }

    for (int i = 0; (i < num_ports) && (found < max_modems); i++) {
        int status = mm_detect_probe_port(ports[i], bauds, num_bauds, &modems[found]);

        if (status > 0) {
            if (report != NULL) {
                fprintf(report, "  %-20s modem found at %d bps%s%s\n", ports[i], modems[found].baudrate,
                        (modems[found].ident[0] != '\0') ? ": " : "", modems[found].ident);
            }
            found++;
        } else if (report != NULL) {
            fprintf(report, "  %-20s %s\n", ports[i], (status == 0) ? "no modem answered" : "can't open, skipped");
        }
        if (report != NULL) {
            fflush(report);
        }
    }

    return found;
}

int mm_detect_report(int preferred_baud) {
    mm_detected_modem_t modems[MM_DETECT_MAX_PORTS];
    int                 found = mm_detect_modems(modems, MM_DETECT_MAX_PORTS, preferred_baud, stdout);

    if (found == 0) {
        printf("No modems found.  Check that the modem is plugged in and powered on, that you can\n"
               "open the serial port (on Linux, you may need to be in the 'dialout' group), and that\n"
               "nothing else is using it.  To use the SIP virtual modem instead, see -S.\n");
    } else {
        printf("\nFound %d modem%s.  To use the first one:\n  mm_manager -m -f %s -b %d -n <NCC number>\n"
               "or let mm_manager choose it for you:\n  mm_manager -m -f auto -n <NCC number>\n",
               found, (found == 1) ? "" : "s", modems[0].path, modems[0].baudrate);
    }

    return found;
}
