/* SPDX-License-Identifier: MIT
 * Track A (2026-11-12): the shell's time of day as text, 12- or 24-hour (sh_clock24). Freestanding (no libc), shared
 * by shell_lock.c and the host test (kern/host/tzdb-test.c).
 *   24 h:            "09:41"
 *   12 h:            "9:41"     (the lock screen's clock and the status bar, as iOS: no AM / PM)
 *   12 h with ampm:  "9:41 AM"  (lists: alarms, notifications, calls); 00:05 is "12:05 AM", 12:00 "12:00 PM"
 * b needs 9 bytes. Returns the length. */
#ifndef SH_TIMEFMT_H
#define SH_TIMEFMT_H

static inline int sh_fmt_time(char *b, int h, int m, int h24, int ampm)
{
    int n = 0;
    h = (h % 24 + 24) % 24;
    m = (m % 60 + 60) % 60;
    if (h24) {
        b[n++] = (char)('0' + h / 10);
        b[n++] = (char)('0' + h % 10);
    } else {
        int h12 = h % 12 ? h % 12 : 12;
        if (h12 >= 10) b[n++] = '1';
        b[n++] = (char)('0' + h12 % 10);
    }
    b[n++] = ':';
    b[n++] = (char)('0' + m / 10);
    b[n++] = (char)('0' + m % 10);
    if (!h24 && ampm) {
        b[n++] = ' ';
        b[n++] = h < 12 ? 'A' : 'P';
        b[n++] = 'M';
    }
    b[n] = 0;
    return n;
}

#endif
