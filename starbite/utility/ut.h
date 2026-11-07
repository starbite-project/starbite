/* SPDX-License-Identifier: GPL-2.0-only
 * Shared between the Utility OS's main.c and its tool pages (tools.c). */
#pragma once
struct ui_ev;

/* a page that must stay visible (a running timer, Measure): no screen saver and no idle return */
extern int ut_awake;
extern unsigned ut_saver_wakes;         /* counts screen-saver wakes (the touch in progress was dropped) */
void ut_service(void);          /* USB, the shell, the watchdog, the idle return: every loop turn */
void ut_status_bar(void);
void ut_measure(void);
void ut_everyday(void);
void ut_pad(void);
void ut_files(void);
struct ub;
int ut_now(int64_t *t);                 /* UTC epoch from the RTC (clock.c), 0 if it did not read */
void ut_clock_status(struct ub *u);     /* "14:41 EET   87%" for the status bar */
void ut_clock_card(char *big, unsigned big_n, char *small, unsigned small_n);
void ut_world(void);                    /* the world clock page */
int ut_ufs_allowed(const char *name);   /* main.c's allow-list: drives and the hex view */
struct exfat;
struct exfat *ut_card(void);           /* the card's exFAT (tools.c), 0 if not readable */

/* text with the Nordic letters (text.c): å ä ö, the euro sign drawn over the font's base letter */
int ut_text(int font, int x, int y, const char *s, uint32_t c);
int ut_text_w(int font, const char *s);
int ut_text_c(int font, int cx, int y, const char *s, uint32_t c);
/* screens */
void ut_remote(void);                   /* remote.c: keyboard, trackpad, gamepad, media, presenter */
void ut_kbd(void);                      /* kbd.c */
void ut_gamepad(void);                  /* gamepad.c */
void ut_sensors(void);                  /* sensors.c */
void ut_qr(void);                       /* qrpage.c */
int ut_back_event(const struct ui_ev *e);
