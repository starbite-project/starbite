/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS USB HID: hid.c (keyboard, mouse, media keys). */
#pragma once
#include <stdint.h>

/* modifier bits (HID keyboard report byte 1): left ctrl, shift, alt (option), GUI (command) */
#define HID_CTRL  0x01U
#define HID_SHIFT 0x02U
#define HID_ALT   0x04U
#define HID_GUI   0x08U
/* mouse buttons */
#define HID_LEFT  0x01U
#define HID_RIGHT 0x02U
/* consumer usages (HUT 1.12 section 15) */
#define HID_VOL_UP   0x00e9U
#define HID_VOL_DOWN 0x00eaU
#define HID_MUTE     0x00e2U
#define HID_PLAY     0x00cdU
#define HID_NEXT     0x00b5U
#define HID_PREV     0x00b6U

struct hid_stats { uint64_t reports, keys, clicks, dropped; };
extern struct hid_stats hid_stats;

void hid_key(uint8_t mod, uint8_t key);   /* press and release */
void hid_text(const char *s);             /* letters, digits, space, newline, . , - / (US layout) */
void hid_move(int dx, int dy);            /* accumulated, sent in steps */
void hid_wheel(int n);
void hid_click(uint8_t btn);
void hid_buttons(uint8_t b);              /* held buttons (drag) */
void hid_media(uint16_t usage);           /* press and release */
void hid_service(void);                   /* each loop turn */
void hid_reset(void);
