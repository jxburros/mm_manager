/*
 * Automatic detection of serial modems, part of mmumblephone.
 *
 * Finds the serial ports on this machine and probes each one with Hayes AT
 * commands, so a modem (or USB modem / serial-attached ATA) that a Millennium
 * payphone will call can be located without knowing its device name or baud rate.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#ifndef MM_DETECT_H_
#define MM_DETECT_H_

#include <stdio.h>

#define MM_DETECT_MAX_PORTS     (64)
#define MM_DETECT_PATH_LEN      (128)
#define MM_DETECT_IDENT_LEN     (64)
#define MM_DETECT_MAX_BAUDS     (8)

typedef struct mm_detected_modem {
    char path[MM_DETECT_PATH_LEN];      /* Device name, to be passed to -f */
    int  baudrate;                      /* Rate at which the modem answered */
    char ident[MM_DETECT_IDENT_LEN];    /* Identification string from ATI3, if any */
} mm_detected_modem_t;

/*
 * Probe one serial port with AT commands, trying each baud rate in bauds[] in turn.
 *
 * Returns 1 and fills *modem if a modem answered, 0 if the port is there but nothing
 * answered, or -1 if the port could not be opened.
 */
int mm_detect_probe_port(const char *path, const int *bauds, int num_bauds, mm_detected_modem_t *modem);

/*
 * Probe every serial port on this machine.  preferred_baud is tried first, followed
 * by the other common rates.  If report is non-NULL, progress is printed to it.
 *
 * Returns the number of modems found, stored in modems[] (at most max_modems.)
 */
int mm_detect_modems(mm_detected_modem_t *modems, int max_modems, int preferred_baud, FILE *report);

/*
 * The "-D" command: scan, list what was found, and say how to use it.  Returns the
 * number of modems found.
 */
int mm_detect_report(int preferred_baud);

#endif /* MM_DETECT_H_ */
