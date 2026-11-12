/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: a Bluetooth HID keyboard in boot protocol (bt_hid.c). Pure C: the HIDP frames from the keyboard's
 * interrupt channel in, key down / up events with characters out. No SDP: a boot-protocol keyboard needs no report
 * descriptor (the host sets boot protocol and the reports have a fixed layout).
 *
 * Sources: Bluetooth HID Profile 1.1 (HIDP header = transaction type << 4 | parameter: HANDSHAKE 0x0, HID_CONTROL 0x1,
 * SET_PROTOCOL 0x7 (parameter 0 = boot), DATA 0xA (parameter 1 input, 2 output); PSMs 0x11 control, 0x13 interrupt;
 * boot reports carry report ID 1); USB HID 1.11 appendix B.1 (the boot keyboard report: modifiers, reserved, six
 * keycodes; 0x01 in every slot = ErrorRollOver) and B.2 (the LED output report: bit 0 Num Lock, bit 1 Caps Lock);
 * HID Usage Tables 1.12 chapter 10 (Keyboard/Keypad page 0x07). Characters: the US layout. */
#ifndef SBOS_BT_HID_H
#define SBOS_BT_HID_H
#include <stdint.h>

#define HID_PSM_CTRL 0x0011
#define HID_PSM_INTR 0x0013
#define HIDP_HANDSHAKE 0x00                 /* | result: 0 successful, 3 unsupported request, ... */
#define HIDP_CONTROL 0x10                   /* | 5: VIRTUAL_CABLE_UNPLUG */
#define HIDP_VC_UNPLUG 0x15
#define HIDP_SET_PROTOCOL_BOOT 0x70
#define HIDP_DATA_INPUT 0xA1
#define HIDP_DATA_OUTPUT 0xA2

/* usages without a character (the callback's usage tells them apart) */
#define HID_KEY_ENTER 0x28
#define HID_KEY_ESC 0x29
#define HID_KEY_BACKSPACE 0x2A
#define HID_KEY_TAB 0x2B
#define HID_KEY_CAPS 0x39
#define HID_KEY_DELETE 0x4C
#define HID_KEY_RIGHT 0x4F
#define HID_KEY_LEFT 0x50
#define HID_KEY_DOWN 0x51
#define HID_KEY_UP 0x52
#define HID_MOD_LCTRL 0x01
#define HID_MOD_LSHIFT 0x02
#define HID_MOD_LALT 0x04
#define HID_MOD_LGUI 0x08                   /* the right ones are << 4 */

struct bthid_kbd {
    uint8_t mods, keys[6];
    uint8_t leds;                           /* the LED output report's byte (bit 1: Caps Lock) */
    int caps, leds_changed;
    void (*cb)(int usage, int down, uint32_t unicode);
    unsigned reports, ignored, rollover, malformed, downs;
};

void bthid_init(struct bthid_kbd *k);       /* keeps k->cb */
/* every held key up (the callback told), as when the keyboard goes away */
void bthid_release(struct bthid_kbd *k);
/* a frame from the interrupt channel: 1 a keyboard report taken, 0 another kind (ignored), -1 malformed */
int bthid_frame(struct bthid_kbd *k, const uint8_t *p, unsigned n);
/* the character for a usage: Enter '\n', Backspace 0x08, Tab '\t', Esc 0x1B, Delete 0x7F; 0 for none (arrows, F-keys,
 * modifiers) and while Ctrl or GUI is held */
uint32_t bthid_char(uint8_t usage, uint8_t mods, int caps);
#endif
